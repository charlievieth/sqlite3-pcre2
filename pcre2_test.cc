// Prevent Go from trying to load this file:

//go:build never
// +build never

// This is a very simple test suite for the pcre2 extension and exists mostly
// to run the address sanitizer to find leaks, which we can't do with the Go
// test suite.

#include <sqlite3.h>

#include <fstream>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <cassert>

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

sqlite3 *init_test_database() {
	char *errmsg = NULL;
	#define _(code) assert_sqlite3(code)

	const int opts = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
		SQLITE_OPEN_URI | SQLITE_OPEN_EXRESCODE;

	sqlite3 *db;
	_(sqlite3_open_v2("file:./test_c.sqlite3", &db, opts, NULL));

	_(sqlite3_enable_load_extension(db, 1));
	_(sqlite3_load_extension(db, "./sqlite3_pcre2", "sqlite3_sqlitepcre_init", &errmsg));

	#undef _
	return db;
}

struct RegexTest {
	std::string pattern;
	std::string subject;
	bool        match;
};

static const RegexTest regexTests[] = {
	{"", "", true},
	{"^abc", "abc", true},
	{"^abc", "cba", false},
	{"日本語+", "日本語語", true},
	{"日本語+", "日本語a", true},
	{"日本語+", "日本a語", false},

	{"", "", true},
	{"^abcdefg", "abcdefg", true},
	{"a+", "baaab", true},
	{"abcd..", "abcdef", true},
	{"a", "a", true},
	{"x", "y", false},
	{"b", "abc", true},
	{".", "a", true},
	{".*", "abcdef", true},
	{"^", "abcde", true},
	{"$", "abcde", true},
	{"^abcd$", "abcd", true},
	{"^abcd$", "abcde", false},
	{"a+", "baaab", true},
	{"a*", "baaab", true},
	{"[a-z]+", "abcd", true},
	{"[^a-z]+", "ab1234cd", true},
	{"[a\\-\\]z]+", "az]-bcz", true},
	{"[^\n]+", "abcd\n", true},
	{"[日本語]+", "日本語日本語", true},
	{"日本語+", "日本語", true},
	{"日本語+", "日本語語語語", true},
	{"()", "", true},
	{"(a)", "a", true},
	{"(.)(.)", "日a", true},
	{"(.*)", "", true},
	{"(.*)", "abcd", true},
	{"(..)(..)", "abcd", true},
	{"(([^xyz]*)(d))", "abcd", true},
	{"((a|b|c)*(d))", "abcd", true},
	{"(((a|b|c)*)(d))", "abcd", true},
	{"\a\f\n\r\t\v", "\a\f\n\r\t\v", true},
	{"[\a\f\n\r\t\v]+", "\a\f\n\r\t\v", true},

	{"a*(|(b))c*", "aacc", true},
	{"(.*).*", "ab", true},
	{"[.]", ".", true},
	{"/$", "/abc/", true},
	{"/$", "/abc", false},

	// multiple matches
	{".", "abc", true},
	{"(.)", "abc", true},
	{".(.)", "abcd", true},
	{"ab*", "abbaab", true},
	{"a(b*)", "abbaab", true},

	// fixed bugs
	{"ab$", "cab", true},
	{"axxb$", "axxcb", false},
	{"data", "daXY data", true},
	{"da(.)a$", "daXY data", true},
	{"zx+", "zzx", true},
	{"ab$", "abcab", true},
	{"(aa)*$", "a", true},
	{"(?:.|(?:.a))", "", false},
	{"(?:A(?:A|a))", "Aa", true},
	{"(?:A|(?:A|a))", "a", true},
	{"(a){0}", "", true},
	{"(?-s)(?:(?:^).)", "\n", false},
	{"(?s)(?:(?:^).)", "\n", true},
	{"(?:(?:^).)", "\n", false},
	{"\\b", "x", true},
	{"\\b", "xx", true},
	{"\\b", "x y", true},
	{"\\b", "xx yy", true},
	{"\\B", "x", false},
	{"\\B", "xx", true},
	{"\\B", "x y", false},
	{"\\B", "xx yy", true},
	{"(|a)*", "aa", true},

	// long set of matches (longer than startSize)
	{".", "qwertyuiopasdfghjklzxcvbnm1234567890", true},

	// Empty matches
	{"", "", true},
	{"^$", "", true},
	{"^", "", true},
	{"$", "", true},
	{"a", "", false},
	{" ", "", false},

	// Unicode fun
	{"🙈.*🙉.*🙊", "😈 🙈 🙉 🙊 😈", true},
	{"🙈.*🙉[^a]+🙊", "😈 🙈 🙉 a 🙊 😈", false},
	{"🙈.+🙉.+🙊", "😈 🙈 🙉 🙊 😈", true},
	{"🙈.+🙉.+🙊", "🙈🙉🙊", false},
	{"🙈\\s+🙉\\s+🙊", " 🙈 🙉 🙊 ", true},
};

static int exec_callback(void *data, int n, char **results, char **columns) {
	(void)columns;
	bool *match = static_cast<bool*>(data);
	*match = !!(n == 1 && results && std::strcmp(results[0], "1") == 0);
	return SQLITE_OK;
}

static std::string format_regex_query(std::string pattern, std::string subject, bool caseless = false) {
	if (caseless) {
		return "SELECT IREGEXP('" + pattern + "', '" + subject + "');";
	}
	return "SELECT REGEXP('" + pattern + "', '" + subject + "');";
}

constexpr const char * bool_to_string(bool b){
	return b ? "true" : "false";
}

static bool test_regex(sqlite3 *db, bool caseless) {
	bool match = false;
	bool passed = true;
	char *errmsg = NULL;
	for (auto test : regexTests) {
		std::string query = format_regex_query(test.pattern, test.subject, caseless);
		const char *q = query.c_str();

		int ret = sqlite3_exec(db, q, exec_callback, &match, &errmsg);
		if (ret != SQLITE_OK) {
			std::printf("Error: %s: %d: %s\n", q, ret, errmsg);
			passed = false;
			continue;
		}
		if (match != test.match) {
			std::printf("Error: %s = %s want: %s\n", q, bool_to_string(match),
				bool_to_string(test.match));
			passed = false;
		}
	}
	return passed;
}

int main(int argc, char const *argv[]) {
	(void)argc;
	(void)argv;

	sqlite3 *db = init_test_database();

	bool failed = false;
	if (!test_regex(db, false)) {
		std::cout << "FAIL: regexp" << std::endl;
		failed = true;
	}
	if (!test_regex(db, true)) {
		std::cout << "FAIL: iregexp" << std::endl;
		failed = true;
	}
	assert(sqlite3_close_v2(db) == SQLITE_OK);

	if (failed) {
		return EXIT_FAILURE;
	}
	std::cout << "PASS" << std::endl;
	return EXIT_SUCCESS;
}
