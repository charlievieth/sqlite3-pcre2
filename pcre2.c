// vim: ts=4 sw=4

// Prevent Go from trying to load this file

//go:build never
// +build never

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// TODO: make sure we need all of these
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <ctype.h>
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
#ifndef JIT_STACK_START_SIZE
#define JIT_STACK_START_SIZE (0)
// #define JIT_STACK_START_SIZE (32 * 1024LU)
#endif

// Maximum size of the pcre2 JIT stack.
#ifndef JIT_STACK_MAX_SIZE
#define JIT_STACK_MAX_SIZE (512 * 1024LU)
#endif

#define noinline HEDLEY_NEVER_INLINE

#ifndef unlikely
#define unlikely(x) HEDLEY_UNLIKELY(x)
#endif

#ifndef likely
#define likely(x) HEDLEY_LIKELY(x)
#endif

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API
#endif

// GCC (-fanalyzer) only:
//
// void sqlite3_free(void*);
// __attribute__ ((malloc, malloc(sqlite3_free, 1)))
// void *sqlite3_malloc64(sqlite3_uint64);

// malloc wrapper for pcre2
HEDLEY_MALLOC
static void *re_malloc(size_t size, void *data) {
	(void)data;
	return sqlite3_malloc64(size);
}

// free wrapper for pcre2
static void re_free(void *block, void *data) {
	(void)data;
	sqlite3_free(block);
}

// Forward declarations
typedef struct cache_entry cache_entry;
typedef struct cache_list cache_list;

struct cache_entry {
	cache_entry *next;
	cache_entry *prev;
	bool        caseless;
	uint32_t    pattern_len;
	char        *pattern;
	pcre2_code  *code;
};

static void cache_entry_free(cache_entry *c) {
	if (c->pattern) {
		sqlite3_free(c->pattern);
	}
	if (c->code) {
		pcre2_code_free(c->code);
	}
	c->caseless = false;
	c->pattern_len = 0;
	c->pattern = NULL;
	c->code = NULL;
}

static inline bool cache_entry_match(const cache_entry *e, bool caseless,
                                     const char *ptrn, size_t plen) {
	return !!(e->caseless == caseless && e->pattern_len == plen &&
	          e->pattern[0] == ptrn[0] && memcmp(e->pattern, ptrn, plen) == 0);
}

// TODO: make thread-safe
// cache_list is a doubly linked list of compiled pcre2 codes
struct cache_list {
	cache_entry           root;

	// Shared pcre2 data structures.
	pcre2_general_context *general_context;
	pcre2_compile_context *compile_context;
	pcre2_jit_stack       *jit_stack;
	pcre2_match_context   *context;
	pcre2_match_data      *match_data; // oveccount == 1
	int                   ref_count;

	cache_entry           data[CACHE_SIZE];
};

static inline cache_entry *cache_list_insert(cache_entry *e, cache_entry *at) {
	e->prev = at;
	e->next = at->next;
	e->prev->next = e;
	e->prev->prev = e;
	return e;
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

static inline void cache_list_move_front(cache_list *l, cache_entry *e) {
	cache_list_move(e, &l->root);
}

static inline void cache_list_move_back(cache_list *l, cache_entry *e) {
	cache_list_move(e, l->root.prev);
}

// cache_list_discard frees the cache_entry and places it in the back of the list.
static inline void cache_list_discard(cache_list *l, cache_entry *e) {
	cache_entry_free(e);
	cache_list_move_back(l, e);
}

static cache_list *cache_list_init(void) {
	cache_list *list = sqlite3_malloc64(sizeof(cache_list));
	if (!list) {
		return NULL;
	}
	memset(list, 0, sizeof(cache_list));

	// clang-format off
	list->general_context = pcre2_general_context_create(
		re_malloc,
		re_free,
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

	list->root.next = &list->root;
	list->root.prev = &list->root;
	for (int i = CACHE_SIZE - 1; i >= 0; i--) {
		cache_list_insert(&list->data[i], &list->root);
	}
	return list;

error:
	if (list->general_context) {
		pcre2_general_context_free(list->general_context);
	}
	if (list->compile_context) {
		pcre2_compile_context_free(list->compile_context);
	}
	sqlite3_free(list);
	return NULL;
}

static pcre2_jit_stack *cache_list_jit_stack_callback(void* p) {
	return ((cache_list *)p)->jit_stack;
}

// TODO: lazily grow the jit stack
static int cache_list_init_jit_stack(cache_list *cache) {
	// clang-format off
	cache->jit_stack = pcre2_jit_stack_create(
		JIT_STACK_START_SIZE,
		JIT_STACK_MAX_SIZE,
		cache->general_context
	);
	if (JIT_STACK_START_SIZE > 0 && JIT_STACK_MAX_SIZE > 0) {
		if (unlikely(cache->jit_stack == NULL)) {
			return 1;
		}
	}
	cache->context = pcre2_match_context_create(cache->general_context);
	if (unlikely(cache->context == NULL)) {
		return 1;
	}
	pcre2_jit_stack_assign(cache->context, cache_list_jit_stack_callback, cache);

	// use oveccount == 1 since we don't care about capture groups
	cache->match_data = pcre2_match_data_create(1, cache->general_context);
	if (unlikely(cache->match_data == NULL)) {
		return 1;
	}
	return 0;
}

// TODO: rename "list" to "cache" and make this consistent across functions.
static void cache_list_free(cache_list *list) {
	if (!list) {
		return;
	}
	if (list->general_context) {
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
	for (int i = 0; i < CACHE_SIZE; i++) {
		if (list->data[i].pattern) {
			cache_entry_free(&list->data[i]);
		}
	}
#ifndef NDEBUG
	// Zero when debugging to detect "use after free" errors
	memset(list, 0, sizeof(cache_list));
#endif
	sqlite3_free(list);
}

// TODO: rename
static void sqlite3_cache_list_destroy(void *p) {
	cache_list *list = (cache_list *)p;
	if (--list->ref_count <= 0) {
		cache_list_free(list);
	}
}

static cache_entry *cache_list_find(cache_list *l, bool caseless,
                                    const char *ptrn, uint32_t plen) {
	for (cache_entry *e = l->root.next; e != &l->root; e = e->next) {
		if (e->pattern == NULL) {
			break;
		}
		if (cache_entry_match(e, caseless, ptrn, plen)) {
			cache_list_move_front(l, e);
			return e;
		}
	}
	return NULL;
}

HEDLEY_RETURNS_NON_NULL
static cache_entry *cache_list_next(cache_list *l) {
    cache_entry *e;
    for (e = l->root.next; e != &l->root; e = e->next) {
        if (e->pattern == NULL) {
            break;
        }
    }
    if (e->pattern != NULL) {
        cache_entry_free(e);
    }
    cache_list_move_front(l, e);
    return e;
}

// static inline bool non_ascii(int ch) {
// 	return !isascii(ch) || ch == '\033';
// }

// static bool has_non_ascii(const char *s) {
// 	if (!s) {
// 		return false;
// 	}
// 	int ch;
// 	while ((ch = *s++) != '\0') {
// 		if (non_ascii(ch)) {
// 			return true;
// 		}
// 	}
// 	return false;
// }

// // static bool has_non_ascii_2(const char *s, ssize_t n) {
// static bool has_non_ascii_2(const char *s, int n) {
// 	for (ssize_t i = 0; i < n; i++) {
// 		// if (non_ascii(s[i])) {
// 		if (s[i] & 0x80) {
// 			return true;
// 		}
// 	}
// 	return false;
// }
//
// static bool has_non_ascii_3(const char *s, const int n) {
// 	int i = 0;
// 	for ( ; i < n; i++) {
// 		if (non_ascii(s[i])) {
// 			return true;
// 		}
// 	}
// 	for ( ; i < n; i++) {
// 		if (non_ascii(s[i])) {
// 			return true;
// 		}
// 	}
// 	return false;
// }

HEDLEY_PRINTF_FORMAT(3, 4)
static noinline void handle_pcre2_error(sqlite3_context *ctx, int errcode,
                                         const char *format, ...) {
	char buf[128];
	int rc = pcre2_get_error_message(errcode, (PCRE2_UCHAR *)&buf[0], sizeof(buf));
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

	const char *fmt = strcmp(msg, "regexp: ") ? "regexp: %s: %s" : "%s: %s";
	char *err = sqlite3_mprintf(fmt, msg, &buf[0]);
	if (!err) {
		sqlite3_free(msg);
		sqlite3_result_error_nomem(ctx);
		return;
	}

	sqlite3_result_error(ctx, err, -1);

	sqlite3_free(msg);
	sqlite3_free(err);
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
		sqlite3_free(msg);
	}
	#undef max_size
}

// WARN: find a way to test this
static noinline void handle_pcre2_match_error(
	sqlite3_context *ctx,
	int errcode,
    const char *pattern,
    uint32_t pattern_len,
    const char *subject,
    uint32_t subject_len
) {
	#define max_size (MAX_DISPLAYED_PATTERN_LENGTH)
	static const char *format = "error matching regex '%s' against subject '%s'";

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
			sqlite3_free(p);
			sqlite3_result_error_nomem(ctx);
			return;
		}
	}
	handle_pcre2_error(ctx, errcode, format, p ? p : pattern, s ? s : subject);
	if (p) {
		sqlite3_free(p);
	}
	if (s) {
		sqlite3_free(s);
	}
	#undef max_size
}

// WARN: remove this
// __thread pcre2_jit_stack *xjit_stack = NULL;
// __thread cache_list *_cache = NULL;

static cache_entry *regexp_compile(sqlite3_context *ctx, cache_list *cache,
	                               const char *pattern, uint32_t pattern_len,
	                               bool ignore_case) {

	uint32_t options = PCRE2_MULTILINE | PCRE2_UTF;
#ifdef PCRE2_MATCH_INVALID_UTF
	options |= PCRE2_MATCH_INVALID_UTF;
#endif
	if (ignore_case) {
		options |= PCRE2_CASELESS;
	}

	int errcode;
	size_t errpos;
	cache_entry *ent = NULL;

	// TODO: check if the pattern matches an empty string
	//	 see: grep/src/pcresearch.c
	//
	// clang-format off
	pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, pattern_len, options,
	                                 &errcode, &errpos, cache->compile_context);
	if (unlikely(code == NULL)) {
		// TODO: I think there are more error cases that we want to handle here.
		if (errcode == PCRE2_ERROR_NOMEMORY) {
			goto err_nomem;
		}
		handle_pcre2_compilation_error(ctx, errcode, pattern, pattern_len, errpos);
		return NULL;
	}

	int rc = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
	if (unlikely(rc && rc != PCRE2_ERROR_JIT_BADOPTION && rc != PCRE2_ERROR_NOMEMORY)) {
		// PCRE2_ERROR_JIT_BADOPTION: jit not supported
		// PCRE2_ERROR_NOMEMORY:      pattern too large for jit compilation.
		pcre2_code_free(code);
		handle_pcre2_error(ctx, rc, "internal JIT error: %d", rc);
		return NULL;
	}

	// TODO: combine find and next
	ent = cache_list_next(cache);
	ent->code = code;
	ent->caseless = ignore_case;

	// TODO: use a thread local variable and callback for the JIT stack.
	if (unlikely(cache->jit_stack == NULL)) {
		if (cache_list_init_jit_stack(cache)) {
			goto err_nomem;
		}
	}

	// WARN: this currently leaks since nothing cleans
	// up the per-thread cache.
	ent->pattern = sqlite3_malloc64(pattern_len + 1);
	if (unlikely(ent->pattern == NULL)) {
		goto err_nomem;
	}
	memcpy(ent->pattern, pattern, pattern_len + 1);
	ent->caseless = ignore_case;
	ent->pattern_len = pattern_len;

	return ent;

err_nomem:
	if (code) {
		pcre2_code_free(code);
	}
	if (ent) {
		cache_list_discard(cache, ent);
	}
	sqlite3_result_error_nomem(ctx);
	return NULL;
}

// /* Return true if E is an error code for bad UTF-8, and if pcre2_match
//    could return E because PCRE lacks PCRE2_MATCH_INVALID_UTF.  */
// static bool bad_utf8_from_pcre2(int e) {
// #ifdef PCRE2_MATCH_INVALID_UTF
// 	return false;
// #else
// 	return PCRE2_ERROR_UTF8_ERR21 <= e && e <= PCRE2_ERROR_UTF8_ERR1;
// #endif
// }

// // WARN: rename
// static int jit_exec(cache_list *cache, cache_entry *ent, const char *subject, size_t subject_len) {
// 	while (true) {
// 		int rc = pcre2_match(ent->code, (const PCRE2_SPTR)subject, subject_len,
// 	                        0, 0, cache->match_data, cache->context);
// 		if (rc == PCRE2_ERROR_JIT_STACKLIMIT && cache->jit_stack == NULL) {
// 			cache_list_init_jit_stack(cache);
// 		} else {
// 			return rc;
// 		}
// 	};
// 	return 0;
// }

static inline int regexp_match(cache_list *cache, cache_entry *ent,
	                           const char *subject, size_t subject_len) {
	return pcre2_match(ent->code, (const PCRE2_SPTR)subject, subject_len,
	                   0, 0, cache->match_data, cache->context);
}

static inline void regexp_func(sqlite3_context *ctx, int argc, sqlite3_value **argv, bool caseless) {
	#define ERR(msg) "regexp: "msg

	assert(argc == 2);

	// TODO: only allow string patterns
	// TODO: this is unlikely
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		sqlite3_result_error(ctx, ERR("NULL regex pattern"), -1);
		return;
	}

	// NULL values never match
	if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
		sqlite3_result_int(ctx, 0);
		return;
	}

	int pattern_len = sqlite3_value_bytes(argv[0]);

	// empty patterns match everything
	if (pattern_len <= 0) {
		sqlite3_result_int(ctx, 1);
		return;
	}
	if (unlikely(pattern_len) > UINT32_MAX) {
		sqlite3_result_error(ctx, ERR("pattern size exceeds UINT32_MAX"), -1);
	}

	// TODO: do was grep does and check if the pattern matches
	// an empty string when compiling.
	//
	// TODO: handle zero length subjects?
	int subject_len = sqlite3_value_bytes(argv[1]);

	const char *pattern = (const char *)sqlite3_value_text(argv[0]);
	const char *subject = (const char *)sqlite3_value_text(argv[1]);
	if (unlikely(pattern == NULL || subject == NULL)) {
		sqlite3_result_error_nomem(ctx);
		return;
	}

	cache_list *cache = sqlite3_user_data(ctx);
	assert(cache);

	// TODO: combine `cache_list_find` and `cache_list_next`
	cache_entry *ent = cache_list_find(cache, caseless, pattern, pattern_len);
	if (ent == NULL) {
		ent = regexp_compile(ctx, cache, pattern, pattern_len, caseless);
		if (unlikely(ent == NULL)) {
			return; // sqlite3 error already set
		}
	}

	int rc = regexp_match(cache, ent, subject, subject_len);
	if (likely(rc >= PCRE2_ERROR_NOMATCH)) {
		sqlite3_result_int(ctx, !!(rc >= 0));
	} else {
		handle_pcre2_match_error(ctx, rc, pattern, pattern_len,subject, subject_len);
	}
	return;
}

static void regexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	regexp_func(ctx, argc, argv, false);
}

static void iregexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	regexp_func(ctx, argc, argv, true);
}

// TODO: create virtual table (or similar) with the compiled settings.
// Check if there is a special table type used for stats.
API int sqlite3_sqlitepcre_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
	(void)pzErrMsg;

	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	// TODO: try to use two caches

	// TODO: lazily initialize backing memory
	cache_list *cache = cache_list_init();
	if (!cache) {
		rc = SQLITE_NOMEM;
		goto exit;
	}
	assert(cache);

	// TODO: add desctructor function
	const int opts = SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC;

	cache->ref_count++;
	rc = sqlite3_create_function_v2(db, "regexp", 2, opts, (void*)cache, regexp,
	                                NULL, NULL, sqlite3_cache_list_destroy);
	if (rc != SQLITE_OK) {
		goto exit;
	}
	cache->ref_count++;
	rc = sqlite3_create_function_v2(db, "iregexp", 2, opts, (void*)cache, iregexp,
	                                NULL, NULL, sqlite3_cache_list_destroy);
exit:
	if (rc != SQLITE_OK) {
		if (cache) {
			cache->ref_count = 0;
			cache_list_free(cache);
		}
	}
	return rc;
}

// vim: ts=4 sw=4

// struct db_conn_cache {
// 	cache_list **cache; // per-thread caches
// 	int32_t len;
// 	int32_t cap;
// };

// static db_conn_cache *db_conn_cache_init() {
// 	db_conn_cache *cache = sqlite3_malloc64(sizeof(db_conn_cache));
// 	if (unlikely(!cache)) {
// 		return NULL;
// 	}
// 	cache->cache = NULL;
// 	cache->len = 0;
// 	cache->cap = 0;
// 	return cache;
// }

// static int db_conn_cache_grow_locked(db_conn_cache *cache) {
// 	// Mutex must be help
// 	if (cache->len < cache->cap && cache->cap != 0) {
// 		return 0; // success
// 	}
// 	int32_t cap;
// 	if (cache->cap == 0) {
// 		cap = 2;
// 	} else {
// 		cap = cache->cap * 2;
// 	}
// 	cache_list **data = sqlite3_realloc64(cache->cache, sizeof(cache_list*) * cap);
// 	if (unlikely(data == NULL)) {
// 		return 1; // WARN: make sure we free everything
// 	}
// 	cache->cache = data;
// 	cache->cap = cap;
// 	return 0;
// }

// static cache_list *db_conn_cache_new_list(sqlite3_context *ctx, db_conn_cache *cache) {
// 	sqlite3 *db = sqlite3_context_db_handle(ctx);
// 	assert(db);
// 	sqlite3_mutex *mu = sqlite3_db_mutex(db);
//
// 	cache_list *cl = cache_list_init();
// 	if (unlikely(cl == NULL)) {
// 		return NULL;
// 	}
//
// 	sqlite3_mutex_enter(mu);
// 	if (db_conn_cache_grow_locked(cache) == 0) {
// 		cache->cache[cache->len++] = cl;
// 	} else {
// 		cache_list_free(cl); // cleanup
// 		cl = NULL;
// 	}
// 	sqlite3_mutex_leave(mu);
// 	return cl;
// }

// static void db_conn_cache_free(db_conn_cache *cache) {
// 	if (!cache) {
// 		return;
// 	}
// 	if (cache->cache) {
// 		for (int i = 0; i < cache->len; i++) {
// 			cache_list_free(cache->cache[i]);
// 		}
// 		sqlite3_free(cache->cache);
// 		cache->cache = NULL;
// 	}
// 	sqlite3_free(cache);
// }
