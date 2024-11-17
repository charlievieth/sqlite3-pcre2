// vim: ts=4 sw=4

// Prevent Go from trying to load this file

//go:build never
// +build never

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>

#include "hedley.h"

// Size of the compiled pcre2 code cache.
#ifndef CACHE_SIZE
#define CACHE_SIZE 16
#endif

// Invalid patterns larger than this size will be truncated.
#ifndef MAX_DISPLAYED_PATTERN_LENGTH
#define MAX_DISPLAYED_PATTERN_LENGTH 256
#endif

// Require CACHE_SIZE to be reasonable (large values will make it slow
// unless changed to use a hash map).
HEDLEY_STATIC_ASSERT(1 <= CACHE_SIZE && CACHE_SIZE <= 1024, "invalid CACHE_SIZE");

// Start size of the pcre2 JIT stack.
//
// Must be greater than zero otherwise pcre2_jit_stack_create returns a null
// JIT stack, which defeats the purpose of having a max JIT stack size.
#ifndef JIT_STACK_START_SIZE
#define JIT_STACK_START_SIZE (32 * 1024LU)
#endif
HEDLEY_STATIC_ASSERT(JIT_STACK_START_SIZE > 0,
	"JIT_STACK_START_SIZE must be greater than zero");

// Maximum size of the pcre2 JIT stack.
#ifndef JIT_STACK_MAX_SIZE
#define JIT_STACK_MAX_SIZE (512 * 1024LU)
#endif
HEDLEY_STATIC_ASSERT(JIT_STACK_START_SIZE <= JIT_STACK_MAX_SIZE,
	"JIT_STACK_MAX_SIZE must be larger than JIT_STACK_START_SIZE");

#define noinline HEDLEY_NEVER_INLINE

#ifndef unlikely
#define unlikely(x) HEDLEY_UNLIKELY(x)
#endif

#ifndef likely
#define likely(x) HEDLEY_LIKELY(x)
#endif

// Define __counted_by if it does not exist.
// https://clang.llvm.org/docs/BoundsSafety.html
#ifndef __counted_by
#define __counted_by(N)
#endif

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API
#endif

// Wrapper around sqlite3_free to aid static analysis.
static void re_free(void *block) {
	sqlite3_free(block);
}

// Wrapper around sqlite3_malloc64 with function attributes
// to aid static analysis (particularly for GCC).
#if HEDLEY_GCC_VERSION_CHECK(12,0,0)
    __attribute__ ((malloc, malloc(re_free, 1)))
#else
    HEDLEY_MALLOC
#endif
static void *re_malloc(size_t size) {
	return sqlite3_malloc64(size);
}

// malloc wrapper for pcre2
static void *re_pcre2_malloc(size_t size, void *data) {
	(void)data;
	return re_malloc(size);
}

// free wrapper for pcre2
static void re_pcre2_free(void *block, void *data) {
	(void)data;
	re_free(block);
}

// Forward declarations
typedef struct cache_entry cache_entry;
typedef struct cache_list cache_list;

struct cache_entry {
	cache_entry *next;
	cache_entry *prev;

	// The cache_list this element belongs to. We store this
	// here since it simplifies passing an entry to aux data.
	cache_list  *cache;
	uint32_t    ref_count; // Number of aux data references to this entry.
	uint32_t    pattern_len;
	char        *pattern __counted_by(pattern_len);
	pcre2_code  *code;
	bool        jit_compiled; // TODO: pack into top-bit of ref_count
};

static void cache_entry_free(cache_entry *c) {
	if (c->pattern) {
		re_free(c->pattern);
	}
	if (c->code) {
		pcre2_code_free(c->code);
	}
	// Zero the entry while preserving the intrusive list.
	memset(&c->ref_count, 0, sizeof(cache_entry) - offsetof(cache_entry, ref_count));
}

static inline bool cache_entry_match(const cache_entry *e, const char *ptrn, size_t plen) {
	return e->pattern_len == plen && e->pattern[0] == ptrn[0] &&
		memcmp(e->pattern, ptrn, plen) == 0;
}

typedef struct {
	uint64_t evacuations;
	uint64_t hits;
	uint64_t misses;
	uint64_t regexes_compiled;
} cache_list_stats;

// cache_list is a doubly linked list of compiled pcre2 codes
struct cache_list {
	cache_entry           root;
	int                   len;
	// Shared pcre2 data structures.
	pcre2_general_context *general_context;
	pcre2_compile_context *compile_context;
	pcre2_jit_stack       *jit_stack;
	pcre2_match_context   *context;
	pcre2_match_data      *match_data; // oveccount == 1
	cache_list_stats      stats;
};

static int cache_list_size(const cache_list *l) {
	return l->len;
}

static void cache_list_insert(cache_list *l, cache_entry *e, cache_entry *at) {
	e->prev = at;
	e->next = at->next;
	e->prev->next = e;
	e->prev->prev = e;
	l->len++;
}

static void cache_list_push_front(cache_list *l, cache_entry *e) {
	cache_list_insert(l, e, &l->root);
}

static inline void cache_list_move(cache_entry *e, cache_entry *at) {
	if (e == at) {
		return;
	}
	e->prev->next = e->next;
	e->next->prev = e->prev;

	e->prev = at;
	e->next = at->next;
	e->prev->next = e;
	e->next->prev = e;
}

static inline void cache_list_remove(cache_list *l, cache_entry *e) {
	e->prev->next = e->next;
	e->next->prev = e->prev;
	e->next = NULL;
	e->prev = NULL;
	l->len--;
}

static cache_entry *cache_list_back(cache_list *l) {
	return l->root.prev;
}

static inline void cache_list_move_front(cache_list *l, cache_entry *e) {
	if (e->next != NULL) {
		cache_list_move(e, &l->root);
		return;
	}
	// Entry not already part of the list.
	cache_list_push_front(l, e);
	if (l->len > CACHE_SIZE) {
		cache_entry *back = cache_list_back(l);
		cache_list_remove(l, e);
		if (back->ref_count == 0) {
			// Free the entry if nothing is using it. If it is in
			// use then it will be put back into the list when the
			// statement using it is closed.
			cache_entry_free(back);
		}
	}
}

static cache_list *cache_list_init(void) {
	cache_list *list = re_malloc(sizeof(cache_list));
	if (!list) {
		return NULL;
	}
	memset(list, 0, sizeof(cache_list));

	// Create a general context that uses sqlite3's memory allocator instead of
	// the system default. This simplifies the tracking of memory used.
	//
	// clang-format off
	list->general_context = pcre2_general_context_create(
		re_pcre2_malloc,
		re_pcre2_free,
		NULL
	);
	if (!list->general_context) {
		goto error;
	}

	// clang-format off
	list->compile_context = pcre2_compile_context_create(
		list->general_context
	);
	if (!list->compile_context) {
		goto error;
	}

	// Initialize the linked list.
	list->root.next = &list->root;
	list->root.prev = &list->root;
	return list;

error:
	if (list->general_context) {
		pcre2_general_context_free(list->general_context);
	}
	if (list->compile_context) {
		pcre2_compile_context_free(list->compile_context);
	}
	re_free(list);
	return NULL;
}

static pcre2_jit_stack *cache_list_jit_stack_callback(void* p) {
	return ((cache_list *)p)->jit_stack;
}

static int cache_list_init_jit_stack(cache_list *cache) {
	// clang-format off
	cache->jit_stack = pcre2_jit_stack_create(
		JIT_STACK_START_SIZE,
		JIT_STACK_MAX_SIZE,
		cache->general_context
	);
	if (cache->jit_stack == NULL) {
		return 1;
	}

	cache->context = pcre2_match_context_create(cache->general_context);
	if (cache->context == NULL) {
		return 1;
	}
	pcre2_jit_stack_assign(cache->context, cache_list_jit_stack_callback, cache);

	// Use oveccount == 1 since we don't care about capture groups.
	cache->match_data = pcre2_match_data_create(1, cache->general_context);
	if (cache->match_data == NULL) {
		return 1;
	}
	return 0;
}

static void cache_list_free(cache_list *list) {
	if (!list) {
		return;
	}
	if (list->general_context) {
		pcre2_jit_free_unused_memory(list->general_context);
		pcre2_general_context_free(list->general_context);
	}
	if (list->compile_context) {
		pcre2_compile_context_free(list->compile_context);
	}
	if (list->jit_stack) {
		pcre2_jit_stack_free(list->jit_stack);
	}
	if (list->context) {
		pcre2_match_context_free(list->context);
	}
	if (list->match_data) {
		pcre2_match_data_free(list->match_data);
	}
	for (cache_entry *e = list->root.next; e != &list->root; ) {
		cache_entry *next = e->next;
		cache_entry_free(e);
		re_free(e);
		e = next;
	}
#ifndef NDEBUG
	// Zero when debugging to detect "use after free" errors
	memset(list, 0, sizeof(cache_list));
#endif
	re_free(list);
}

// sqlite3_cache_list_destroy is the destructor
// used be sqlite3_create_function_v2.
static void sqlite3_cache_list_destroy(void *p) {
	cache_list *list = (cache_list *)p;
	cache_list_free(list);
}

// cache_list_find returns the cache entry that has a compiled regex with pattern
// ptrn, or NULL if no entry was found.
static cache_entry *cache_list_find(cache_list *l, const char *ptrn, uint32_t plen) {
	for (cache_entry *e = l->root.next; e != &l->root; e = e->next) {
		if (cache_entry_match(e, ptrn, plen)) {
			cache_list_move_front(l, e);
			l->stats.hits++;
			return e;
		}
	}
	l->stats.misses++;
	return NULL;
}

HEDLEY_PRINTF_FORMAT(3, 4)
static noinline void handle_pcre2_error(sqlite3_context *ctx, int errcode,
                                         const char *format, ...) {
	enum { ERRBUFSIZ = 256 }; // taken from pcre2grep
	char buf[ERRBUFSIZ];
	int rc = pcre2_get_error_message(errcode, (PCRE2_UCHAR8 *)&buf[0], ERRBUFSIZ);
	if (rc == PCRE2_ERROR_BADDATA) {
		sqlite3_snprintf(sizeof(buf), &buf[0], "invalid error code: %d", errcode);
	}

	va_list args;
	va_start(args, format);
	char *msg = sqlite3_vmprintf(format, args);
	va_end(args);
	if (!msg) {
		sqlite3_result_error_nomem(ctx);
		return;
	}

	char *err = sqlite3_mprintf("regexp: %s: %s", msg, &buf[0]);
	if (!err) {
		re_free(msg);
		sqlite3_result_error_nomem(ctx);
		return;
	}

	sqlite3_result_error(ctx, err, -1);

	re_free(msg);
	re_free(err);
}

static noinline void handle_pcre2_compilation_error(
	sqlite3_context *ctx,
	int errcode,
    const char *pattern,
    uint32_t pattern_len,
    size_t errpos
) {
	#define max_size MAX_DISPLAYED_PATTERN_LENGTH
	static const char *format = "error compiling pattern '%s' at offset %llu";

	if (0 < max_size && pattern_len <= max_size) {
		handle_pcre2_error(ctx, errcode, format, pattern, errpos);
	} else {
		// Truncate large patterns
		int64_t omitted = pattern_len - max_size;
		char *msg = sqlite3_mprintf("%.*s... omitting %lld bytes ...%.*s",
		                       max_size/2, pattern,
		                       omitted,
		                       max_size/2, &pattern[pattern_len - (max_size/2)]);
		if (!msg) {
			sqlite3_result_error_nomem(ctx);
			return;
		}
		handle_pcre2_error(ctx, errcode, format, msg, errpos);
		re_free(msg);
	}
	#undef max_size
}

// TODO: Consider only printing the pattern and omitting the subject since there
// could be some security and PIAA risks caused by including the subject in the
// error message, which will likely be logged.
static noinline void handle_pcre2_match_error(
	sqlite3_context *ctx,
	int errcode,
    const char *pattern,
    uint32_t pattern_len,
    const char *subject,
    uint32_t subject_len
) {
	#define max_size MAX_DISPLAYED_PATTERN_LENGTH
	const char *format = "error matching regex: '%s' against subject: '%s'";

	if (max_size < 0 || (pattern_len <= max_size && subject_len <= max_size)) {
		handle_pcre2_error(ctx, errcode, format, pattern, subject);
		return;
	}

	char *p = NULL;
	char *s = NULL;
	if (pattern_len > max_size) {
		int64_t omitted = pattern_len - max_size;
		p = sqlite3_mprintf("%.*s... omitting %lld bytes ...%.*s",
		                    max_size/2, pattern,
		                    omitted,
		                    max_size/2, &pattern[pattern_len - (max_size/2)]);
		if (!p) {
			sqlite3_result_error_nomem(ctx);
			return;
		}
	}
	// WARN: printing the subject could be a security/privacy risk and
	// should be optional.
	if (subject_len > max_size) {
		int64_t omitted = subject_len - max_size;
		s = sqlite3_mprintf("%.*s... omitting %lld bytes ...%.*s",
		                    max_size/2, subject,
		                    omitted,
		                    max_size/2, &subject[subject_len - (max_size/2)]);
		if (!s) {
			re_free(p);
			sqlite3_result_error_nomem(ctx);
			return;
		}
	}
	handle_pcre2_error(ctx, errcode, format, p ? p : pattern, s ? s : subject);
	if (p) {
		re_free(p);
	}
	if (s) {
		re_free(s);
	}
	#undef max_size
}

static cache_entry *regexp_compile(sqlite3_context *ctx, cache_list *cache,
                                   const char *pattern, uint32_t pattern_len,
                                   bool caseless) {

	uint32_t options = PCRE2_MULTILINE | PCRE2_UTF;
#ifdef PCRE2_MATCH_INVALID_UTF
	options |= PCRE2_MATCH_INVALID_UTF;
#endif
	if (caseless) {
		options |= PCRE2_CASELESS;
	}

	int errcode;
	size_t errpos;
	cache_entry *ent = NULL;

	// TODO: check if the pattern matches an empty string
	//	 see: pcre_comp.empty_match in grep/src/pcresearch.c
	//
	// clang-format off
	pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, pattern_len, options,
	                                 &errcode, &errpos, cache->compile_context);
	if (code == NULL) {
		// TODO: I think there are more error cases that we want to handle here.
		if (errcode == PCRE2_ERROR_NOMEMORY) {
			goto err_nomem;
		}
		handle_pcre2_compilation_error(ctx, errcode, pattern, pattern_len, errpos);
		return NULL;
	}

	int rc = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
	if (rc != SQLITE_OK && rc != PCRE2_ERROR_JIT_BADOPTION && rc != PCRE2_ERROR_NOMEMORY) {
		// PCRE2_ERROR_JIT_BADOPTION: jit not supported
		// PCRE2_ERROR_NOMEMORY:      pattern too large for jit compilation.
		pcre2_code_free(code);
		handle_pcre2_error(ctx, rc, "internal JIT error: %d", rc);
		return NULL;
	}

	ent = re_malloc(sizeof(cache_entry));
	if (ent == NULL) {
		goto err_nomem;
	}
	ent->next = NULL; // pedantically zero
	ent->prev = NULL;
	ent->cache = cache;
	ent->code = code;
	ent->jit_compiled = (rc == SQLITE_OK);

	// Initialize the shared JIT stack.
	if (unlikely(cache->jit_stack == NULL)) {
		if (cache_list_init_jit_stack(cache)) {
			goto err_nomem;
		}
	}

	ent->pattern_len = pattern_len;
	ent->pattern = re_malloc(ent->pattern_len + 1);
	if (unlikely(ent->pattern == NULL)) {
		goto err_nomem;
	}
	memcpy(ent->pattern, pattern, ent->pattern_len + 1);

	cache->stats.regexes_compiled++;
	return ent;

err_nomem:
	if (code) {
		pcre2_code_free(code);
	}
	if (ent) {
		cache_entry_free(ent);
	}
	sqlite3_result_error_nomem(ctx);
	return NULL;
}

// cache_aux_data_destroy is the deestructor for sqlite3_set_auxdata and ensures
// that we decrement the entry's ref_count and put it back into the cache.
static void cache_aux_data_destroy(void *p) {
	cache_entry *e = (cache_entry *)p;
	e->ref_count--;
	cache_list_move_front(e->cache, e);
}

// cache_aux_data_set is a wrapper around sqlite3_set_auxdata that
// ensures we increment the entry's ref_count.
static void cache_aux_data_set(sqlite3_context *ctx, cache_entry *e) {
	e->ref_count++;
	sqlite3_set_auxdata(ctx, 0, e, cache_aux_data_destroy);
}

static inline int regexp_match(const cache_list *cache, const cache_entry *ent,
	                           const char *subject, size_t subject_len) {
	return ent->jit_compiled
		? pcre2_jit_match(ent->code, (const PCRE2_SPTR)subject, subject_len, 0,
		                  PCRE2_NO_UTF_CHECK, cache->match_data, cache->context)
		: pcre2_match(ent->code, (const PCRE2_SPTR)subject, subject_len, 0,
		              PCRE2_NO_UTF_CHECK, cache->match_data, cache->context);
}

// regexp_execute does the actual work of matching a regex pattern against
// a sqlite3 query.
static void regexp_execute(sqlite3_context *ctx, sqlite3_value *pval,
                           sqlite3_value *sval, bool caseless) {
	// NULL values never match
	int subject_type = sqlite3_value_type(sval);
	if (subject_type == SQLITE_NULL) {
		sqlite3_result_int(ctx, 0);
		return;
	}

	int subject_len = sqlite3_value_bytes(sval);

	const char *subject = subject_type == SQLITE_BLOB
		? (const char *)sqlite3_value_blob(sval)
		: (const char *)sqlite3_value_text(sval);
	if (unlikely(subject == NULL)) {
		if (sqlite3_errcode(sqlite3_context_db_handle(ctx)) == SQLITE_NOMEM) {
			sqlite3_result_error_nomem(ctx);
		} else {
			sqlite3_result_int(ctx, 0); // NULL values never match
		}
		return;
	}

	int pattern_len;
	const char *pattern = NULL;

	cache_entry *ent = sqlite3_get_auxdata(ctx, 0);
	if (ent == NULL) {
		// No aux data: attempt to find a cached regex or compile a new one.

		// TODO: consider only allowing string patterns (sqlite3 will
		// coerce pretty much anything to a string so this could help
		// users who might've passed an INT or someting as the argument).
		if (sqlite3_value_type(pval) == SQLITE_NULL) {
			sqlite3_result_error(ctx, "regexp: NULL pattern", -1);
			return;
		}

		pattern_len = sqlite3_value_bytes(pval);

		// Empty patterns match everything.
		if (pattern_len <= 0) {
			sqlite3_result_int(ctx, 1);
			return;
		}

		pattern = (const char *)sqlite3_value_text(pval);
		if (unlikely(pattern == NULL)) {
			sqlite3_result_error_nomem(ctx);
			return;
		}

		cache_list *cache = sqlite3_user_data(ctx);
		if (unlikely(cache == NULL)) {
			sqlite3_result_error_code(ctx, SQLITE_INTERNAL);
			sqlite3_result_error(ctx, "regexp: cache not initialized", -1);
			return;
		}

		ent = cache_list_find(cache, pattern, pattern_len);
		if (ent == NULL) {
			// No cached regex: compile a new one.
			ent = regexp_compile(ctx, cache, pattern, pattern_len, caseless);
			if (ent == NULL) {
				return; // sqlite3 error already set
			}
		}

		cache_aux_data_set(ctx, ent);
	}

	int rc = regexp_match(ent->cache, ent, subject, subject_len);
	if (likely(rc >= PCRE2_ERROR_NOMATCH)) {
		sqlite3_result_int(ctx, !!(rc >= 0));
		return;
	}

	if (pattern == NULL) {
		pattern_len = sqlite3_value_bytes(pval);
		pattern = (const char *)sqlite3_value_text(pval);
	}
	handle_pcre2_match_error(ctx, rc, pattern, pattern_len, subject, subject_len);
	return;
}

// regexp handles case-sensitive regexes
static void regexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	(void)argc;
	assert(argc == 2);
	regexp_execute(ctx, argv[0], argv[1], false);
}

// regexp handles case-insensitive regexes
static void iregexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	(void)argc;
	assert(argc == 2);
	regexp_execute(ctx, argv[0], argv[1], true);
}

// regexp_info provides information about the state of the regex extension.
static void regexp_info(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	// TODO: create virtual table (or similar) to access the settings
	// and statistics of this package (might be kind of a big lift,
	// that only benefits debugging).
	(void)argc;

	sqlite3_value *val = argv[0];
	if (sqlite3_value_type(val) != SQLITE_TEXT) {
		sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
		sqlite3_result_error(ctx, "regexp: argument to info must be a string", -1);
		return;
	}

	cache_list *cache = sqlite3_user_data(ctx);
	assert(cache);

	const char *query = (const char *)sqlite3_value_text(val);
	if (query == NULL) {
		sqlite3_result_error_nomem(ctx);
		return;
	}

	#define strieq(_s1, _s2) (sqlite3_stricmp((_s1), (_s2)) == 0)

	if (strieq("cache_size", query)) {
		sqlite3_result_int(ctx, CACHE_SIZE);
	} else if (strieq("jit_stack_start_size", query)) {
		sqlite3_result_int(ctx, JIT_STACK_START_SIZE);
	} else if (strieq("jit_stack_max_size", query)) {
		sqlite3_result_int(ctx, JIT_STACK_MAX_SIZE);
	} else if (strieq("max_displayed_pattern_length", query)) {
		sqlite3_result_int(ctx, MAX_DISPLAYED_PATTERN_LENGTH);
	} else if (strieq("cache_evacuations", query)) {
		sqlite3_result_int64(ctx, cache->stats.evacuations);
	} else if (strieq("cache_hits", query)) {
		sqlite3_result_int64(ctx, cache->stats.hits);
	} else if (strieq("cache_misses", query)) {
		sqlite3_result_int64(ctx, cache->stats.misses);
	} else if (strieq("cache_in_use", query)) {
		sqlite3_result_int64(ctx, cache_list_size(cache));
	} else if (strieq("regexes_compiled", query)) {
		sqlite3_result_int64(ctx, cache->stats.regexes_compiled);
	} else if (strieq("reset_stats", query)) {
		memset(&cache->stats, 0, sizeof(cache_list_stats));
		sqlite3_result_null(ctx);
	} else {
		char *err = sqlite3_mprintf("regexp: invalid query: %s", query);
		if (err) {
			sqlite3_result_error(ctx, err, -1);
			re_free(err);
		} else {
			sqlite3_result_error_nomem(ctx);
		}
	}

	#undef strieq
}

// Extension entry point.
API int sqlite3_sqlitepcre_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
	(void)pzErrMsg;

	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	cache_list *rcache = cache_list_init();
	cache_list *icache = cache_list_init();
	if (!rcache || !icache) {
		rc = SQLITE_NOMEM;
		goto err_exit;
	}

	const int opts = SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC;

	rc = sqlite3_create_function_v2(db, "regexp", 2, opts, (void*)rcache, regexp,
	                                NULL, NULL, sqlite3_cache_list_destroy);
	if (rc != SQLITE_OK) {
		goto err_exit;
	}
	rc = sqlite3_create_function_v2(db, "iregexp", 2, opts, (void*)icache, iregexp,
	                                NULL, NULL, sqlite3_cache_list_destroy);
	if (rc != SQLITE_OK) {
		goto err_exit;
	}

	// Info functions - these should really be a virtual table, but that's
	// a lot of effort for something people might never use.
	rc = sqlite3_create_function_v2(db, "regexp_info", 1, opts, (void*)rcache, regexp_info,
	                                NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		goto err_exit;
	}

	rc = sqlite3_create_function_v2(db, "iregexp_info", 1, opts, (void*)icache, regexp_info,
	                                NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		goto err_exit;
	}

err_exit:
	if (rc != SQLITE_OK) {
		if (rcache) {
			cache_list_free(rcache);
		}
		if (icache) {
			cache_list_free(icache);
		}
	}
	return rc;
}
