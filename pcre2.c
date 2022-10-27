// vim: ts=4 sw=4

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// WARN: dev only
#include <stdio.h>

// TODO: make sure we need all of these
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <assert.h>

#ifndef CACHE_SIZE
#define CACHE_SIZE 16
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

typedef struct cache_entry cache_entry;
struct cache_entry {
	cache_entry *next;
	cache_entry *prev;

	char *pattern;
	size_t pattern_len; // WARN: currently broken

	pcre2_code *code;
	pcre2_match_data *match_data;
	pcre2_match_context *context;
	pcre2_jit_stack *jit_stack;
};

static void free_cache_entry(cache_entry *c) {
	if (c->pattern)
		free(c->pattern);
	if (c->code)
		pcre2_code_free(c->code);
	if (c->match_data)
		pcre2_match_data_free(c->match_data);
	if (c->context)
		pcre2_match_context_free(c->context);
	if (c->jit_stack)
		pcre2_jit_stack_free(c->jit_stack);

	c->pattern = NULL;
	c->pattern_len = 0;
	c->code = NULL;
	c->match_data = NULL;
	c->context = NULL;
	c->jit_stack = NULL;

	// TODO: only do this when debugging
	// *c = (cache_entry){ 0 };
}

// reset_cache_entry resets cache_entry c but preserves the jit_stack and context.
static void reset_cache_entry(cache_entry *c) {
	if (c->pattern)
		free(c->pattern);
	if (c->code)
		pcre2_code_free(c->code);
	if (c->match_data)
		pcre2_match_data_free(c->match_data);
	// if (c->context) pcre2_match_context_free(c->context);
	// if (c->jit_stack) pcre2_jit_stack_free(c->jit_stack);

	c->pattern = NULL;
	c->pattern_len = 0;
	c->code = NULL;
	c->match_data = NULL;
}

// TODO: use a linked list
//
typedef struct cache_list {
	cache_entry root;
	cache_entry *data;
	int len;
} cache_list;

static void cache_list_insert(cache_list *l, cache_entry *e, cache_entry *at) {
	// WARN: need to fixup the current next/current entry
	(void)l;
	e->prev = at;
	e->next = at->next;
	e->prev->next = e;
	e->prev->prev = e;
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

// TODO: rename to 'cache_list_move_front'
static inline void cache_list_push_front(cache_list *l, cache_entry *e) {
	// WARN: make sure we don't keep growing the list with
	// the same set of entires (check length).
	cache_list_move(e, &l->root);
}

static inline void cache_list_push_back(cache_list *l, cache_entry *e) {
	if (l->root.prev == e) {
		return;
	}
	cache_list_move(e, l->root.prev);
}

static cache_list *init_cache_list(int cap) {
	cache_list *list = calloc(1, sizeof(cache_list));
	if (!list) {
		return NULL;
	}
	cache_entry *data = calloc(cap, sizeof(cache_entry));
	if (!data) {
		free(list);
		return NULL;
	}
	list->data = data;
	list->len = 0;

	// don't think we need this
	list->root.next = &list->root;
	list->root.prev = &list->root;

	for (int i = 0; i < cap; i++) {
		// TODO: is correct and is there a better way?
		cache_list_insert(list, &data[i], &list->root);
	}
	return list;
}

// static inline void cache_list_free(cache_list *l, cache_entry *e) {
// 	if (e->pattern) {
// 	}
// 	if (l->root.prev == e) {
// 		return;
// 	}
// 	cache_list_move(e, l->root.prev);
// }

static cache_entry *cache_list_find(cache_list *l, const char *ptrn, size_t plen) {
	for (cache_entry *e = l->root.next; e != &l->root; e = e->next) {
		if (e->pattern == NULL) {
			break; // WARN: this should work
		}
		if (e->pattern_len == plen && memcmp(e->pattern, ptrn, plen) == 0) {
			cache_list_push_front(l, e);
			return e;
		}
	}
	return NULL;
}

static cache_entry *cache_list_next(cache_list *l) {
	// WARN: make len work
	// if (l->len < CACHE_SIZE) {
	for (cache_entry *e = l->root.next; e != &l->root; e = e->next) {
		if (e->pattern == NULL) {
			cache_list_push_front(l, e);
			// l->len++;
			return e; // WARN: this should work
		}
	}
	// }
	cache_entry *e = l->root.prev;
	assert(e != &l->root); // TODO: remove
	// free_cache_entry(e);
	reset_cache_entry(e);
	cache_list_push_front(l, e);
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

// WARN: this doesn't work for escaped text like '\A' which is a anchor.
static bool starts_with_upper(const char *s) {
	if (!s) {
		return false;
	}
	int ch;
	while ((ch = *s++) != '\0') {
		if (isalpha(ch)) {
			return isupper(ch);
		}
	}
	return false;
}

static void handle_regexp_compile_error(sqlite3_context *ctx, const char *pattern,
										int errcode, size_t offset) {
	const int bufsz = 128;
	char buf[bufsz];

	int ret = pcre2_get_error_message(errcode, (PCRE2_UCHAR*)&buf[0], bufsz);
	if (ret == PCRE2_ERROR_BADDATA) {
		strcpy(&buf[0], "<invalid>");
	}

	char *msg = sqlite3_mprintf("%s: %s (offset %llu)", pattern, &buf[0], (uint64_t)offset);
	if (msg) {
		sqlite3_result_error(ctx, msg, -1);
		sqlite3_free(msg);
	} else {
		sqlite3_result_error(ctx, "unable to format pcre2 error message", -1);
	}
}

static void handle_regexp_match_error(sqlite3_context *ctx, int errcode,
									  const char *pattern, const char *subject) {
	const int bufsz = 128;
	char buf[bufsz];

	int ret = pcre2_get_error_message(errcode, (PCRE2_UCHAR*)&buf[0], bufsz);
	if (ret == PCRE2_ERROR_BADDATA) {
		strcpy(&buf[0], "<invalid>");
	}

	char *msg = sqlite3_mprintf("pcre2_match(\"%s\", \"%s\") = %d - %s",
							    pattern, subject, errcode, &buf[0]);
	if (msg) {
		sqlite3_result_error(ctx, msg, -1);
		sqlite3_free(msg);
	} else {
		sqlite3_result_error(ctx, "unable to format pcre2_match error message", -1);
	}
}

static void regexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	assert(argc == 2);
	if (argc != 2) {
		sqlite3_result_error(ctx, "invalid number of arguments", -1);
		return;
	}

	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		// WARN: allow NULL here ???
		return;
	}
	if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
		return;
	}

	const char *pattern = (const char *)sqlite3_value_text(argv[0]);
	if (!pattern) {
		sqlite3_result_error(ctx, "empty regex pattern", -1);
		return;
	}
	// TODO: handle zero length patterns?
	size_t pattern_len = sqlite3_value_bytes(argv[0]);

	const char *subject = (const char *)sqlite3_value_text(argv[1]);
	if (!subject) {
		sqlite3_result_error(ctx, "empty search subject", -1);
		return;
	}

	cache_list *cache = sqlite3_user_data(ctx);
	assert(cache);
	if (unlikely(cache == NULL)) {
		sqlite3_result_error(ctx, "internal error: missing regex cache", -1);
		return;
	}

	cache_entry *ent = cache_list_find(cache, pattern, pattern_len);
	if (ent == NULL) {
		uint32_t options = PCRE2_MULTILINE;
		if (has_non_ascii(pattern)) {
			options |= PCRE2_UTF;
		}
		// if (!case_sensitive(re)) {
		// 	options |= PCRE2_CASELESS;
		// }

		int errCode;
		size_t errPos;
		pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, pattern_len,
								  options, &errCode, &errPos, NULL);
		assert(code); // WARN
		if (unlikely(code == NULL)) {
			handle_regexp_compile_error(ctx, pattern, errCode, errPos);
			return;
		}
		// WARN: ignoring error
		pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);

		uint32_t ovecsize = 0;
		int rc = pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &ovecsize);
		if (unlikely(rc != 0)) {
			pcre2_code_free(code);
			sqlite3_result_error(ctx, "pcre2_pattern_info", -1);
			return;
		}

		pcre2_match_data *match_data = pcre2_match_data_create(ovecsize, NULL);
		if (unlikely(match_data == NULL)) {
			pcre2_code_free(code);
			sqlite3_result_error_nomem(ctx);
			return;
		}

		// TODO: combine find and next
		ent = cache_list_next(cache);
		assert(ent);
		ent->code = code;
		ent->match_data = match_data;

		// TODO: do we really want to reuse the jit_stack ???
		if (ent->jit_stack == NULL) {
			ent->jit_stack = pcre2_jit_stack_create(32*1024, 512*1024, NULL);
			ent->context = pcre2_match_context_create(NULL);
			if (unlikely(ent->jit_stack == NULL || ent->context == NULL)) {
				free_cache_entry(ent);
				cache_list_push_back(cache, ent);
				sqlite3_result_error_nomem(ctx);
				return;
			}
			pcre2_jit_stack_assign(ent->context, NULL, ent->jit_stack);
		}

		ent->pattern_len = pattern_len;
		ent->pattern = malloc(pattern_len + 1);
		if (unlikely(ent->pattern == NULL)) {
			free_cache_entry(ent);
			cache_list_push_back(cache, ent);
			sqlite3_result_error_nomem(ctx);
			return;
		}
		memcpy(ent->pattern, pattern, pattern_len + 1);
	}
	assert(ent);

	// TODO: handle zero length subjects?
	size_t subject_len = sqlite3_value_bytes(argv[1]);

	int rc = pcre2_match(ent->code, (const PCRE2_SPTR)subject,
						 subject_len, 0, 0 /* options */,
					     ent->match_data, ent->context);
	if (rc < 0 && rc != -1) {
		handle_regexp_match_error(ctx, rc, pattern, subject);
		return;
	}

	sqlite3_result_int(ctx, rc >= 0);
	return;
}

// TODO: rename folder to pcre2
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_pcre_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {

	(void)pzErrMsg;

	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	cache_list *cache = init_cache_list(CACHE_SIZE);
	if (!cache) {
		return SQLITE_NOMEM;
	}

	// TODO: add desctructor function
	const int opts = SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC;
	rc = sqlite3_create_function(db, "regexp", 2, opts, cache, regexp, NULL, NULL);

	/* Insert here calls to
	**     sqlite3_create_function_v2(),
	**     sqlite3_create_collation_v2(),
	**     sqlite3_create_module_v2(), and/or
	**     sqlite3_vfs_register()
	** to register the new features that your extension adds.
	*/
	return rc;
}
