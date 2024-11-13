// Prevent Go from trying to load this file:

//go:build never
// +build never

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static void check_sqlite3_response_impl(int code, int line) {
	const char *err = sqlite3_errstr(code);
	if (!err) {
		err = "invalid code";
	}
	fprintf(stderr, "#%d: error (%d): %s\n", line, code, err);
	assert(0);
	abort();
}

#define assert_sqlite3(code)                                           \
	do {                                                               \
		if (code != SQLITE_OK) {                                       \
			if (errmsg) {                                              \
				fprintf(stderr, "#%d: error: %s\n", __LINE__, errmsg); \
			}                                                          \
			check_sqlite3_response_impl(code, __LINE__);               \
		}                                                              \
	} while (0)

static const char *create_tables_stmt = ""
"CREATE TABLE IF NOT EXISTS strings_table ("
    "id    INTEGER PRIMARY KEY,"
    "value TEXT"
");";

sqlite3 *init_test_database() {
	char *errmsg = NULL;
	#define _(code) assert_sqlite3(code)

	const int opts = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
		SQLITE_OPEN_URI | SQLITE_OPEN_EXRESCODE;

	sqlite3 *db;
	_(sqlite3_open_v2("file:./test_c.sqlite3", &db, opts, NULL));

	_(sqlite3_enable_load_extension(db, 1));
	_(sqlite3_load_extension(db, "sqlite3_pcre2.dylib", "sqlite3_sqlitepcre_init", &errmsg));

	// "SELECT REGEXP('%s', '%s');"

	// TODO: use "SELECT REGEXP(pattern, subject)"
	_(sqlite3_exec(db, create_tables_stmt, NULL, NULL, &errmsg));
	_(sqlite3_exec(db, "DELETE FROM strings_table;", NULL, NULL, &errmsg));
	return db;
}

static int exec_callback(void *data, int n, char **results, char **columns) {
	(void)data;
	printf("n: %d\n", n);
	printf("result: %s\n", results[0]);
	printf("column: %s\n", columns[0]);
	return SQLITE_WARNING;
}

int main(int argc, char const *argv[]) {
	(void)argc;
	(void)argv;

	char *errmsg = NULL;
	#define _(code) assert_sqlite3(code)

	sqlite3 *db = init_test_database();
	_(sqlite3_exec(db, "SELECT REGEXP('abc', 'abc');", exec_callback, NULL, NULL));

	_(sqlite3_close_v2(db));

	printf("PASS");
	return 0;
}
