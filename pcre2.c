// vim: ts=4 sw=4

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// TODO: make sure we need all of these
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hedley.h"

#ifndef CACHE_SIZE
#define CACHE_SIZE 16
#endif

// Require CACHE_SIZE to be reasonable (large values will make it slow
// unless changed to use a hash map).
static_assert(1 <= CACHE_SIZE && CACHE_SIZE <= 1024, "invalid CACHE_SIZE");

#ifndef JIT_STACK_START_SIZE
#define JIT_STACK_START_SIZE (32 * 1024)
#endif
#ifndef JIT_STACK_MAX_SIZE
#define JIT_STACK_MAX_SIZE (512 * 1024)
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

typedef struct cache_entry cache_entry;
struct cache_entry {
	cache_entry *next;
	cache_entry *prev;

	char *pattern;
	size_t pattern_len;

	pcre2_code *code;
	pcre2_match_data *match_data;
	pcre2_match_context *context;
	pcre2_jit_stack *jit_stack;
};

// cache_entry_reset resets cache_entry c but preserves the jit_stack and
// context.
static void cache_entry_reset(cache_entry *c) {
	if (c->pattern) {
		sqlite3_free(c->pattern);
	}
	if (c->code) {
		pcre2_code_free(c->code);
	}
	if (c->match_data) {
		pcre2_match_data_free(c->match_data);
	}
	c->pattern = NULL;
	c->pattern_len = 0;
	c->code = NULL;
	c->match_data = NULL;
}

static void cache_entry_free(cache_entry *c) {
	cache_entry_reset(c);
	if (c->context) {
		pcre2_match_context_free(c->context);
	}
	if (c->jit_stack) {
		pcre2_jit_stack_free(c->jit_stack);
	}
	c->context = NULL;
	c->jit_stack = NULL;
}

static inline bool cache_entry_match(const cache_entry *e, const char *ptrn,
                                     size_t plen) {
	return !!(e->pattern_len == plen && e->pattern[0] == ptrn[0] &&
	          memcmp(e->pattern, ptrn, plen) == 0);
}

// TODO: make thread-safe
// cache_list is a doubly linked list of compiled pcre2 codes
typedef struct cache_list {
	cache_entry root;
	pcre2_general_context *general_context;
	pcre2_compile_context *compile_context;
	cache_entry data[CACHE_SIZE];
} cache_list;

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
	if (l->root.prev == e) {
		return;
	}
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

static void cache_list_free(cache_list *list) {
	if (!list) {
		return;
	}
	if (list->compile_context) {
		pcre2_compile_context_free(list->compile_context);
	}
	if (list->general_context) {
		pcre2_general_context_free(list->general_context);
	}
	for (int i = 0; i < CACHE_SIZE; i++) {
		if (list->data[i].pattern) {
			cache_entry_free(&list->data[i]);
		}
	}
	sqlite3_free(list);
}

static cache_entry *cache_list_find(cache_list *l, const char *ptrn,
                                    size_t plen) {
	for (cache_entry *e = l->root.next; e != &l->root; e = e->next) {
		if (e->pattern == NULL) {
			break;
		}
		if (cache_entry_match(e, ptrn, plen)) {
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
        cache_entry_reset(e);
    }
    cache_list_move_front(l, e);
    return e;
}

static inline bool non_ascii(int ch) {
	return !isascii(ch) || ch == '\033';
}

static bool has_non_ascii(const char *s) {
	if (!s) {
		return false;
	}
	int ch;
	while ((ch = *s++) != '\0') {
		if (non_ascii(ch)) {
			return true;
		}
	}
	return false;
}

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

static cache_entry *regexp_compile(sqlite3_context *ctx, cache_list *cache,
	                               const char *pattern, size_t pattern_len) {

	uint32_t options = PCRE2_MULTILINE;
	if (has_non_ascii(pattern)) {
		options |= (PCRE2_UTF | PCRE2_MATCH_INVALID_UTF);
	}

	int errcode;
	size_t errpos;
	pcre2_match_data *match_data = NULL;
	cache_entry *ent = NULL;

	// clang-format off
	pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, pattern_len, options,
	                                 &errcode, &errpos, cache->compile_context);
	if (unlikely(code == NULL)) {
		if (errcode == PCRE2_ERROR_NOMEMORY) {
			goto err_nomem;
		}
		// Pretty print an error message
		handle_pcre2_error(ctx, errcode, "error compiling pattern '%s' at offset %llu",
		                   pattern, (uint64_t)errpos);
		return NULL;
	}
	// ignore error
	pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);

	// use oveccount == 1 since we don't care about capture groups
	match_data = pcre2_match_data_create(1, cache->general_context);
	if (unlikely(match_data == NULL)) {
		goto err_nomem;
	}

	// TODO: combine find and next
	ent = cache_list_next(cache);
	ent->code = code;
	ent->match_data = match_data;

	// TODO: use a thread local variable and callback for the JIT stack.
	if (ent->jit_stack == NULL) {
		// clang-format off
		ent->jit_stack = pcre2_jit_stack_create(
			JIT_STACK_START_SIZE,
			JIT_STACK_MAX_SIZE,
			cache->general_context
		);
		ent->context = pcre2_match_context_create(cache->general_context);
		if (unlikely(ent->jit_stack == NULL || ent->context == NULL)) {
			goto err_nomem;
		}
		pcre2_jit_stack_assign(ent->context, NULL, ent->jit_stack);
	}

	ent->pattern = sqlite3_malloc64(pattern_len + 1);
	if (unlikely(ent->pattern == NULL)) {
		goto err_nomem;
	}
	memcpy(ent->pattern, pattern, pattern_len + 1);
	ent->pattern_len = pattern_len;

	return ent;

err_nomem:
	if (code) {
		pcre2_code_free(code);
	}
	if (match_data) {
		pcre2_match_data_free(match_data);
	}
	if (ent) {
		cache_list_discard(cache, ent);
	}
	sqlite3_result_error_nomem(ctx);
	return NULL;
}

static void regexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	#define ERR(msg)  "regexp: "msg
	#define IERR(msg) "regexp: internal error: "msg

	if (unlikely(argc != 2)) {
		sqlite3_result_error(ctx, ERR("invalid number of arguments"), -1);
		return;
	}

	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		sqlite3_result_error(ctx, ERR("NULL regex pattern"), -1);
		return;
	}

	// NULL values never match
	if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
		sqlite3_result_int(ctx, 0);
		return;
	}

	size_t pattern_len = sqlite3_value_bytes(argv[0]);

	// empty patterns match everything
	if (pattern_len == 0) {
		sqlite3_result_int(ctx, 1);
		return;
	}

	const char *pattern = (const char *)sqlite3_value_text(argv[0]);
	const char *subject = (const char *)sqlite3_value_text(argv[1]);
	if (unlikely(pattern == NULL || subject == NULL)) {
		sqlite3_result_error_nomem(ctx);
		return;
	}

	cache_list *cache = sqlite3_user_data(ctx);
	if (unlikely(cache == NULL)) {
		sqlite3_result_error(ctx, IERR("missing regex cache"), -1);
		return;
	}

	// TODO: combine `cache_list_find` and `cache_list_next`
	cache_entry *ent = cache_list_find(cache, pattern, pattern_len);
	if (ent == NULL) {
		ent = regexp_compile(ctx, cache, pattern, pattern_len);
		if (unlikely(ent == NULL)) {
			return; // sqlite3 error already set
		}
	}

	// TODO: handle zero length subjects?
	size_t subject_len = sqlite3_value_bytes(argv[1]);

	int rc = pcre2_match(ent->code, (const PCRE2_SPTR)subject, subject_len,
	                     0, 0, ent->match_data, ent->context);
	if (likely(rc >= PCRE2_ERROR_NOMATCH)) {
		sqlite3_result_int(ctx, !!(rc >= 0));
	} else {
		handle_pcre2_error(ctx, rc, "error matching regex '%s' against subject '%s'",
		                   pattern, subject);
	}
	return;
}

static void sqlite3_pcre_destroy(void *p) {
	cache_list *list = (cache_list *)p;
	if (list) {
		cache_list_free(list);
	}
}

// TODO: rename folder to pcre2
API int sqlite3_pcre_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
	(void)pzErrMsg;

	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	cache_list *cache = cache_list_init();
	if (!cache) {
		return SQLITE_NOMEM;
	}

	// TODO: add desctructor function
	const int opts = SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC;
	rc = sqlite3_create_function_v2(db, "regexp", 2, opts, cache, regexp, NULL,
	                                NULL, sqlite3_pcre_destroy);
	return rc;
}
