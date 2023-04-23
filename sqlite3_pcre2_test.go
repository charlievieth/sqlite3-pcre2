// WARN: must be ran with: `go test --tags "libsqlite3 darwin"`

package pcre2

import (
	"bytes"
	"compress/gzip"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"text/tabwriter"
	"time"
)

func TestMain(m *testing.M) {
	// TODO: also test: SearchWorkingDirectory(search)

	exe, err := os.Executable()
	if err != nil {
		panic(err)
	}
	dir := filepath.Dir(exe)
	wd, err := os.Getwd()
	if err != nil {
		panic(err)
	}
	for _, ext := range LibExts {
		name := LibraryName + ext
		src := filepath.Join(wd, name)
		if fileExists(src) {
			dst := filepath.Join(dir, name)
			os.Symlink(src, dst)
		}
	}
	os.Exit(m.Run())
}

// var libraryPath string

// func init() {
// 	pwd, err := os.Getwd()
// 	if err != nil {
// 		panic(err)
// 	}
// 	libraryPath = filepath.Join(pwd, "../pcre2.dylib")
// 	if _, err := os.Stat(libraryPath); err != nil {
// 		panic(err)
// 	}
// 	sql.Register("sqlite3_with_regexp",
// 		&sqlite3.SQLiteDriver{
// 			Extensions: []string{libraryPath},
// 		},
// 	)
// }

// WARN: this must match MAX_DISPLAYED_PATTERN_LENGTH in pcre2.c
const maxDisplayedPatternLength = 256

const CreateStringsTableStmt = `
CREATE TABLE IF NOT EXISTS strings_table (
    id    INTEGER PRIMARY KEY,
    value TEXT
);`

const CreateBlobsTableStmt = `
CREATE TABLE IF NOT EXISTS blobs_table (
    id    INTEGER PRIMARY KEY,
    value BLOB
);`

var TableNames = []string{
	"strings_table",
	"blobs_table",
}

var TestTables = map[string]string{
	"strings_table": `
CREATE TABLE IF NOT EXISTS strings_table (
    id    INTEGER PRIMARY KEY,
    value TEXT
);`,

	"blobs_table": `
CREATE TABLE IF NOT EXISTS blobs_table (
    id    INTEGER PRIMARY KEY,
    value BLOB
);`,
}

func fileExists(name string) bool {
	fi, err := os.Stat(name)
	return err == nil && fi.Mode().IsRegular()
}

// TODO: try to use the same DB connection so that we
// can test the thread safety of the library.
func InitDatabase(t testing.TB) (*sql.DB, func()) {
	const filename = ":memory:"
	const opts = "?cache=shared&_mutex=no"
	// db, err := sql.Open("sqlite3_with_regexp", "file:test.db?cache=shared")
	db, err := sql.Open(DriverName, "file:"+filename+opts)
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Ping(); err != nil {
		t.Fatal(err)
	}

	for name, schema := range TestTables {
		if _, err := db.Exec(fmt.Sprintf("DROP TABLE IF EXISTS %s;", name)); err != nil {
			t.Fatal(err)
		}
		if _, err := db.Exec(schema); err != nil {
			t.Fatal(err)
		}
	}

	return db, func() { db.Close() }
}

func InsertIntoTable(t testing.TB, db *sql.DB, tableName string, values ...any) {
	if _, err := db.Exec("DELETE FROM " + tableName + ";"); err != nil {
		t.Fatal(err)
	}
	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if t.Failed() {
			tx.Rollback()
		}
	}()
	stmt, err := tx.Prepare("INSERT INTO " + tableName + " (value) VALUES (?);")
	if err != nil {
		t.Fatal(err)
	}
	for _, v := range values {
		if _, err := stmt.Exec(v); err != nil {
			t.Fatal(err)
		}
	}
	if err := stmt.Close(); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
}

func InsertIntoStringsTable(t testing.TB, db *sql.DB, values ...any) {
	InsertIntoTable(t, db, "strings_table", values...)
}

func InsertIntoBlobsTable(t testing.TB, db *sql.DB, values ...any) {
	InsertIntoTable(t, db, "blobs_table", values...)
}

func TestPCRE2(t *testing.T) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	var vals []any // string
	for ch := 'A'; ch <= 'Z'; ch++ {
		val := strings.Repeat(fmt.Sprintf("%c", ch), 4)
		for i := 0; i < 4; i++ {
			vals = append(vals, val)
		}
	}
	InsertIntoStringsTable(t, db, vals...)

	const queryFmt = "SELECT COUNT(*) FROM strings_table WHERE value REGEXP '^%c';"
	t.Run("Serial", func(t *testing.T) {
		for ch := 'A'; ch <= 'Z'; ch++ {
			query := fmt.Sprintf(queryFmt, ch)
			var n int64
			if err := db.QueryRow(query).Scan(&n); err != nil {
				t.Fatal(err)
			}
			if n != 4 {
				t.Errorf("%c: got: %d; want: %d", ch, n, 4)
			}
		}
	})

	t.Run("Parallel", func(t *testing.T) {
		var wg sync.WaitGroup
		for ch := 'A'; ch <= 'Z'; ch++ {
			wg.Add(1)
			go func(ch rune) {
				defer wg.Done()
				query := fmt.Sprintf(queryFmt, ch)
				for i := 0; i < 20; i++ {
					var n int64
					if err := db.QueryRow(query).Scan(&n); err != nil {
						t.Error(err)
						return
					}
					if n != 4 {
						t.Errorf("%c: got: %d; want: %d", ch, n, 4)
					}
				}
			}(ch)
		}
		wg.Wait()
	})
}

func TestInvalidRegex(t *testing.T) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	InsertIntoStringsTable(t, db, "a", "b", "c")

	var rows int64
	err := db.QueryRow("SELECT COUNT(*) FROM strings_table WHERE value REGEXP '[a'").Scan(&rows)
	if err == nil {
		t.Fatal("expected error")
	}
	const exp = "regexp: error compiling pattern '[a' at offset 2: missing terminating ] for character class"
	if err.Error() != exp {
		t.Fatalf("error got: %q want: %q", err.Error(), exp)
	}
}

func TestEmptyRegex(t *testing.T) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	InsertIntoStringsTable(t, db, "a", "b", "c")

	var rows int64
	err := db.QueryRow("SELECT COUNT(*) FROM strings_table WHERE value REGEXP ''").Scan(&rows)
	if err != nil {
		t.Fatal(err)
	}
	// Empty regex matches everything
	if rows != 3 {
		t.Fatalf("rows got %d want: %d", rows, 3)
	}
}

func TestNullValues(t *testing.T) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	InsertIntoStringsTable(t, db, "a", nil, "c", nil)

	var rows int64
	err := db.QueryRow("SELECT COUNT(*) FROM strings_table WHERE value REGEXP '^[ac]'").Scan(&rows)
	if err != nil {
		t.Fatal(err)
	}
	if rows != 2 {
		t.Fatalf("rows got %d want: %d", rows, 2)
	}
}

func TestNumericValues(t *testing.T) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	InsertIntoStringsTable(t, db, "1", 1, 1.234, "2", 2, 2.3456)

	var rows int64
	err := db.QueryRow("SELECT COUNT(*) FROM strings_table WHERE value REGEXP '^1'").Scan(&rows)
	if err != nil {
		t.Fatal(err)
	}
	if rows != 3 {
		t.Fatalf("rows got %d want: %d", rows, 3)
	}
}

// TODO: combine match tests
type MatchTest struct {
	pattern, subject string
	match            bool
}

func (m MatchTest) Query() string {
	return fmt.Sprintf("SELECT REGEXP('%s', '%s');",
		strings.ReplaceAll(m.pattern, `'`, `''`),
		strings.ReplaceAll(m.subject, `'`, `''`))
}

func (m MatchTest) IRegexp() string {
	return fmt.Sprintf("SELECT IREGEXP('%s', '%s');",
		strings.ReplaceAll(m.pattern, `'`, `''`),
		strings.ReplaceAll(m.subject, `'`, `''`))
}

var matchTests = []MatchTest{
	{``, ``, true},
	{`^abcdefg`, "abcdefg", true},
	{`a+`, "baaab", true},
	{"abcd..", "abcdef", true},
	{`a`, "a", true},
	{`x`, "y", false},
	{`b`, "abc", true},
	{`.`, "a", true},
	{`.*`, "abcdef", true},
	{`^`, "abcde", true},
	{`$`, "abcde", true},
	{`^abcd$`, "abcd", true},
	{`^bcd'`, "abcdef", false},
	{`^abcd$`, "abcde", false},
	{`a+`, "baaab", true},
	{`a*`, "baaab", true},
	{`[a-z]+`, "abcd", true},
	{`[^a-z]+`, "ab1234cd", true},
	{`[a\-\]z]+`, "az]-bcz", true},
	{`[^\n]+`, "abcd\n", true},
	{`[日本語]+`, "日本語日本語", true},
	{`日本語+`, "日本語", true},
	{`日本語+`, "日本語語語語", true},
	{`()`, "", true},
	{`(a)`, "a", true},
	{`(.)(.)`, "日a", true},
	{`(.*)`, "", true},
	{`(.*)`, "abcd", true},
	{`(..)(..)`, "abcd", true},
	{`(([^xyz]*)(d))`, "abcd", true},
	{`((a|b|c)*(d))`, "abcd", true},
	{`(((a|b|c)*)(d))`, "abcd", true},
	{`\a\f\n\r\t\v`, "\a\f\n\r\t\v", true},
	{`[\a\f\n\r\t\v]+`, "\a\f\n\r\t\v", true},

	{`a*(|(b))c*`, "aacc", true},
	{`(.*).*`, "ab", true},
	{`[.]`, ".", true},
	{`/$`, "/abc/", true},
	{`/$`, "/abc", false},

	// multiple matches
	{`.`, "abc", true},
	{`(.)`, "abc", true},
	{`.(.)`, "abcd", true},
	{`ab*`, "abbaab", true},
	{`a(b*)`, "abbaab", true},

	// fixed bugs
	{`ab$`, "cab", true},
	{`axxb$`, "axxcb", false},
	{`data`, "daXY data", true},
	{`da(.)a$`, "daXY data", true},
	{`zx+`, "zzx", true},
	{`ab$`, "abcab", true},
	{`(aa)*$`, "a", true},
	{`(?:.|(?:.a))`, "", false},
	{`(?:A(?:A|a))`, "Aa", true},
	{`(?:A|(?:A|a))`, "a", true},
	{`(a){0}`, "", true},
	{`(?-s)(?:(?:^).)`, "\n", false},
	{`(?s)(?:(?:^).)`, "\n", true},
	{`(?:(?:^).)`, "\n", false},
	{`\b`, "x", true},
	{`\b`, "xx", true},
	{`\b`, "x y", true},
	{`\b`, "xx yy", true},
	{`\B`, "x", false},
	{`\B`, "xx", true},
	{`\B`, "x y", false},
	{`\B`, "xx yy", true},
	{`(|a)*`, "aa", true},

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
	{`🙈.*🙉.*🙊`, `😈 🙈 🙉 🙊 😈`, true},
	{`🙈.*🙉[^a]+🙊`, `😈 🙈 🙉 a 🙊 😈`, false},
	{`🙈.+🙉.+🙊`, `😈 🙈 🙉 🙊 😈`, true},
	{`🙈.+🙉.+🙊`, `🙈🙉🙊`, false},
	{`🙈\s+🙉\s+🙊`, ` 🙈 🙉 🙊 `, true},
}

var iregexTests = []MatchTest{
	{``, ``, true},
	{`^abcdefg`, "ABCDEFG", true},
	{`a+`, "BAAAB", true},
	{"abcd..", "ABCDEF", true},
	{`a`, "A", true},
	{`x`, "Y", false},
	{`b`, "ABC", true},
	{`.`, "A", true},
	{`.*`, "ABCDEF", true},
	{`^`, "ABCDE", true},
	{`$`, "ABCDE", true},
	{`^abcd$`, "ABCD", true},
	{`^bcd'`, "ABCDEF", false},
	{`^abcd$`, "ABCDE", false},
	{`a+`, "BAAAB", true},
	{`a*`, "BAAAB", true},
	{`[a-z]+`, "ABCD", true},
	{`[^a-z]+`, "AB1234CD", true},
	{`[a\-\]z]+`, "AZ]-BCZ", true},
	{`[^\n]+`, "ABCD\n", true},
	{`(a)`, "A", true},
	{`(.*)`, "", true},
	{`(.*)`, "ABCD", true},
	{`(..)(..)`, "ABCD", true},
	{`(([^xyz]*)(d))`, "ABCD", true},
	{`((a|b|c)*(d))`, "ABCD", true},
	{`(((a|b|c)*)(d))`, "ABCD", true},
	{`a*(|(b))c*`, "AACC", true},
	{`(.*).*`, "AB", true},
	{`.`, "ABC", true},
	{`(.)`, "ABC", true},
	{`.(.)`, "ABCD", true},
	{`ab*`, "ABBAAB", true},
	{`a(b*)`, "ABBAAB", true},
	{`ab$`, "CAB", true},
	{`axxb$`, "AXXCB", false},
	{`data`, "daXY DATA", true},
	{`zx+`, "ZZX", true},
	{`ab$`, "ABCAB", true},
	{`(aa)*$`, "a", true},
	{`(|a)*`, "AA", true},
}

func testMatchTests(t *testing.T, fn func(MatchTest) string, tests []MatchTest) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)
	// TODO: The empty match tests report only 1 failure if we don't
	// reset the DB is that an issue here or in the sqlite3_pcre2 lib?
	reset := func(t *testing.T) {
		db.Close()
		xdb, xcleanup := InitDatabase(t)
		t.Cleanup(xcleanup)
		db = xdb
	}
	for _, test := range tests {
		var match bool
		q := fn(test)
		if err := db.QueryRow(q).Scan(&match); err != nil {
			t.Errorf("%+v: %s: %v", test, q, err)
			reset(t)
			continue
		}
		if match != test.match {
			t.Errorf("`%s` = %t; want: %t", q, match, test.match)
			reset(t)
		}
	}
}

func TestRegexpMatch(t *testing.T) {
	testMatchTests(t, (MatchTest).Query, matchTests)
}

func toUpperASCII(s string) string {
	fn := func(r rune) rune {
		if 'a' <= r && r <= 'z' {
			r -= 'a' - 'A'
		}
		return r
	}
	return strings.Map(fn, s)
}

func TestIRegexpMatch(t *testing.T) {
	testMatchTests(t, (MatchTest).IRegexp, matchTests)
	testMatchTests(t, (MatchTest).IRegexp, iregexTests)
}

func writeCols(t testing.TB, tw *tabwriter.Writer, args []string) {
	if _, err := tw.Write([]byte(strings.Join(args, "\t") + "\n")); err != nil {
		t.Fatal(err)
	}
}

func writeRow(t testing.TB, tw *tabwriter.Writer, args []any) {
	var err error
	for i, a := range args {
		if p, ok := a.(*any); ok {
			a = *p
		}
		var prefix string
		if i > 0 {
			prefix = "\t"
		}
		switch v := a.(type) {
		case int64:
			_, err = fmt.Fprintf(tw, "%s%d", prefix, v)
		case float64:
			_, err = fmt.Fprintf(tw, "%s%f", prefix, v)
		case bool:
			_, err = fmt.Fprintf(tw, "%s%t", prefix, v)
		case []byte:
			_, err = fmt.Fprintf(tw, "%s%q", prefix, v)
		case string:
			_, err = fmt.Fprintf(tw, "%s%q", prefix, v)
		case time.Time:
			_, err = fmt.Fprintf(tw, "%s%v", prefix, v)
		default:
			t.Fatalf("Invalid type: %T", a)
		}
	}
	if err == nil {
		_, err = tw.Write([]byte("\n"))
	}
	if err != nil {
		t.Fatal(err)
	}
}

func DumpTable(t testing.TB, db *sql.DB, tableName string) {
	t.Helper()
	rows, err := db.Query("SELECT * FROM " + tableName + ";")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	cols, err := rows.Columns()
	if err != nil {
		t.Fatal(err)
	}
	var buf bytes.Buffer
	fmt.Fprintf(&buf, "## Table: %q:\n", tableName)
	tw := tabwriter.NewWriter(&buf, 4, 1, 1, ' ', 0)
	writeCols(t, tw, cols)

	args := make([]any, len(cols))
	ptrs := make([]any, len(cols))
	for i := range ptrs {
		ptrs[i] = &args[i]
	}
	for rows.Next() {
		if err := rows.Scan(ptrs...); err != nil {
			t.Fatal(err)
		}
		writeRow(t, tw, args)
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	if err := tw.Flush(); err != nil {
		t.Fatal(err)
	}
	fmt.Fprintln(&buf, "##")
	t.Log(buf.String())
}

func TestBinaryBlob(t *testing.T) {
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	var runes []rune
	for i := rune(0); i <= ' '; i++ {
		runes = append(runes, i)
	}
	InsertIntoBlobsTable(t, db,
		string(runes[:len(runes)-1]),
		string(runes),
		string([]rune{0, 'a', 0, 'a', 0}),
		123456,
		"bbbbb",
	)

	test := func(t *testing.T, pattern string, want int) {
		t.Run("", func(t *testing.T) {
			defer func() {
				if t.Failed() {
					t.Logf("Pattern: `%s`", pattern)
					DumpTable(t, db, "blobs_table")
				}
			}()
			rows, err := db.Query("SELECT value FROM blobs_table WHERE value REGEXP ?;", pattern)
			if err != nil {
				t.Fatal(err)
			}
			var vals []string
			for rows.Next() {
				var b []byte
				if err := rows.Scan(&b); err != nil {
					t.Fatal(err)
				}
				vals = append(vals, fmt.Sprintf("%q", b))
			}
			if err := rows.Err(); err != nil {
				t.Fatal(err)
			}
			if len(vals) != want {
				t.Fatalf("Pattern: `%s`: Rows: got: %d want: %d\nValues:\n%s",
					pattern, len(vals), want, strings.Join(vals, "\n"))
			}
		})
	}
	test(t, `[ ]+$`, 1)
	test(t, `a.*a`, 1)
	test(t, `a.*b`, 0)
}

func truncatePattern(pattern string) string {
	if len(pattern) <= maxDisplayedPatternLength {
		return pattern
	}
	omitted := len(pattern) - maxDisplayedPatternLength
	return fmt.Sprintf("%.*s... omitting %d bytes ...%.*s",
		maxDisplayedPatternLength/2,
		pattern,
		omitted,
		maxDisplayedPatternLength/2,
		pattern[len(pattern)-maxDisplayedPatternLength/2:],
	)
}

func TestHugePattern(t *testing.T) {
	// Test that large patterns are truncated and match the format:
	// "%.*s... omitting %d bytes ...%.*s"

	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	errMustContain := func(t *testing.T, err error, substr string) {
		t.Helper()
		if err == nil {
			t.Fatal("Expected non-nil error")
		}
		if !strings.Contains(err.Error(), substr) {
			t.Errorf("Expected error to contain %q got: %q", substr, err.Error())
		}
	}

	for i := 0; i < 5; i++ {
		n := 4500 * (i + 1)
		t.Run(fmt.Sprintf("%d", n), func(t *testing.T) {
			if n > math.MaxUint32 {
				t.Fatalf("Pattern size %d cannot exceed %d", n, math.MaxUint32)
			}
			pattern := strings.Repeat("a*b*", n)
			subject := strings.Repeat("ab", n/2)
			var match bool
			err := db.QueryRow(fmt.Sprintf("SELECT REGEXP('%s', '%s');", pattern, subject)).Scan(&match)
			switch {
			case err != nil:
				errMustContain(t, err, truncatePattern(pattern))
				errMustContain(t, err, "regular expression is too large")
			case !match:
				t.Error("expected match to be true")
			}
		})
	}
}

// (?:[a-z0-9!#$%&'*+/=?^_`{|}~-]+(?:\.[a-z0-9!#$%&'*+/=?^_`{|}~-]+)*|"(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21\x23-\x5b\x5d-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])*")@(?:(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z0-9](?:[a-z0-9-]*[a-z0-9])?|\[(?:(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9]))\.){3}(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9])|[a-z0-9-]*[a-z0-9]:(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21-\x5a\x53-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])+)\])

func TestComplexRegexp(t *testing.T) {
	// https://stackoverflow.com/questions/201323/how-can-i-validate-an-email-address-using-a-regular-expression
	const emailRe = `(?:[a-z0-9!#$%&'*+/=?^_` + "`" + `{|}~-]+(?:\.[a-z0-9!#$%&'*+/=?^_` + "`" + `{|}~-]+)*|"(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21\x23-\x5b\x5d-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])*")@(?:(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z0-9](?:[a-z0-9-]*[a-z0-9])?|\[(?:(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9]))\.){3}(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9])|[a-z0-9-]*[a-z0-9]:(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21-\x5a\x53-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])+)\])`
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)

	InsertIntoStringsTable(t, db,
		`abigail@example.com`,
		`abigail@example.com `,
		`*@example.net`,
		`"\""@foo.bar`,
		`fred&barny@example.com`,
		`---@example.com`,
		`foo-bar@example.net`,
		`"127.0.0.1"@[127.0.0.1]`,
		`Abigail <abigail@example.com>`,
		`Abigail<abigail@example.com>`,
		`Abigail<@a,@b,@c:abigail@example.com>`,
		`"This is a phrase"<abigail@example.com>`,
	)

	// Consider using this massive regex
	//
	// https://regex101.com/r/OmVWyR/1
	//
	// https://github.com/PCRE2Project/pcre2/blob/b73b3347673358795e86a99acad56d81c2e4fecd/testdata/testoutput2#L2234
	//
	// /(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\d+(?:\s|$))(\w+)\s+(\270)/I

	// Another option is to use `b*` repeated 9,000 times (this is too big for the JIT)
	// https://github.com/PCRE2Project/pcre2/blob/b73b3347673358795e86a99acad56d81c2e4fecd/testdata/testoutput17#L16

	var want int64
	if err := db.QueryRow("SELECT COUNT(*) FROM strings_table;", emailRe).Scan(&want); err != nil {
		t.Fatal(err)
	}

	var got int64
	// query := fmt.Sprintf("SELECT COUNT(*) FROM strings_table WHERE value REGEXP '%s'", strings.ReplaceAll(emailRe, `"`, `\"`))
	err := db.QueryRow("SELECT COUNT(*) FROM strings_table WHERE value REGEXP ?;", emailRe).Scan(&got)
	if err != nil {
		t.Fatal(err)
	}
	if got != want {
		// var vals []string
		rows, err := db.Query("SELECT value FROM strings_table;")
		if err != nil {
			t.Fatal(err)
		}
		var buf bytes.Buffer
		for i := 0; rows.Next(); i++ {
			var s string
			if err := rows.Scan(&s); err != nil {
				t.Fatal(err)
			}
			fmt.Fprintf(&buf, "%d: %q\n", i, s)
		}
		if err := rows.Err(); err != nil {
			t.Fatal(err)
		}
		t.Fatalf("rows got %d want: %d\nValues:\n%s", got, want, &buf)
	}
}

// WARN: rename to note that this is a JIT stack failure
func TestMatchFailure(t *testing.T) {
	tests := []MatchTest{
		{"(?(R)a*(?1)|((?R))b)", "aaaabcde", false},
		{"(?(R)a*(?1)|((?R))b)", strings.Repeat("0123456789", 200), false},
		{"(?(R)a*(?1)|((?R))b)|(" + strings.Repeat("a", 1024) + ")", "aaaabcde", false},
	}
	db, cleanup := InitDatabase(t)
	t.Cleanup(cleanup)
	for _, m := range tests {
		var match bool
		err := db.QueryRow(m.Query()).Scan(&match)
		if err == nil {
			t.Fatal("expected error got: nil")
		}
		want := fmt.Sprintf("regexp: error matching regex: '%s' against subject: '%s': JIT stack limit reached",
			truncatePattern(m.pattern), truncatePattern(m.subject))
		if err.Error() != want {
			t.Fatalf("Error mismatch:\nGot:  %q\nWant: %q", err, want)
		}
	}
}

func TestLibraryNotFound(t *testing.T) {
	origPath := libraryPath
	SetLibraryPath("/tmp")
	t.Cleanup(func() {
		libraryPath = origPath
	})

	db, err := sql.Open(DriverName, ":memory:")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	for i, fn := range []func() error{
		db.Ping,
		func() error {
			_, err := db.Exec(`SELECT REGEXP('a', 'a');`)
			return err
		},
	} {
		err := fn()
		if !errors.Is(err, ErrLibraryNotFound) {
			t.Errorf("%d: got error: %v want: %v", i, err, ErrLibraryNotFound)
		}
	}
}

// WARN: bad benchmark
func BenchmarkPCRE2(b *testing.B) {
	db, cleanup := InitDatabase(b)
	b.Cleanup(cleanup)

	for ch := 'A'; ch <= 'Z'; ch++ {
		val := strings.Repeat(fmt.Sprintf("%c", ch), 4)
		for i := 0; i < 4; i++ {
			_, err := db.Exec("INSERT INTO strings_table (value) VALUES (?);", val)
			if err != nil {
				b.Fatal(err)
			}
		}
	}

	patterns := [4]string{
		"OO",
		"ZZ",
		"AA",
		"MM",
	}
	var stmts [4]*sql.Stmt
	b.Cleanup(func() {
		for _, stmt := range stmts {
			if stmt != nil {
				stmt.Close()
			}
		}
	})
	for i, ptrn := range patterns {
		query := fmt.Sprintf("SELECT COUNT(*) FROM strings_table WHERE value REGEXP '%s';", ptrn)
		stmt, err := db.Prepare(query)
		if err != nil {
			b.Fatal(err)
		}
		stmts[i] = stmt
	}

	for i := 0; i < b.N; i++ {
		var n int64
		stmt := stmts[i%len(stmts)]
		if err := stmt.QueryRow().Scan(&n); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkLIKE2(b *testing.B) {
	db, cleanup := InitDatabase(b)
	b.Cleanup(cleanup)

	var vals []any
	for ch := 'A'; ch <= 'Z'; ch++ {
		val := strings.Repeat(fmt.Sprintf("%c", ch), 4)
		for i := 0; i < 4; i++ {
			vals = append(vals, val)
		}
	}
	InsertIntoStringsTable(b, db, vals...)
	b.ResetTimer()

	const query = `SELECT COUNT(*) FROM strings_table WHERE value LIKE '%O%';`
	for i := 0; i < b.N; i++ {
		var n int64
		if err := db.QueryRow(query).Scan(&n); err != nil {
			b.Fatal(err)
		}
	}
}

var (
	web2    []string
	web2Any []any
)

func loadWeb2(t testing.TB) {
	if web2 != nil {
		return
	}
	f, err := os.Open("testdata/web2.gz")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	gr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(gr)
	if err != nil {
		t.Fatal(err)
	}
	words := strings.Fields(string(data))
	web2 = make([]string, 0, len(words))
	web2Any = make([]any, 0, len(words))
	for _, w := range words {
		if len(w) != 0 {
			web2 = append(web2, w)
			web2Any = append(web2Any, w)
		}
	}
}

var benchDB *sql.DB
var benchDBOnce sync.Once

func initBenchDB(b *testing.B) *sql.DB {
	benchDBOnce.Do(func() {
		if benchDB != nil {
			return
		}
		t := time.Now()
		db, _ := InitDatabase(b)

		loadWeb2(b)
		InsertIntoStringsTable(b, db, web2Any...)
		if _, err := db.Exec("VACUUM;"); err != nil {
			b.Fatal(err)
		}
		if testing.Verbose() {
			b.Logf("Bench Setup: %s", time.Since(t))
		}

		benchDB = db
	})
	b.ResetTimer()
	return benchDB
}

func BenchmarkWords(b *testing.B) {
	const query = `SELECT COUNT(*) FROM strings_table WHERE value REGEXP ?;`
	const pattern = `a.*t`
	db := initBenchDB(b)

	stmt, err := db.Prepare(query)
	if err != nil {
		b.Fatal(err)
	}
	defer stmt.Close()

	var n int64
	if err := stmt.QueryRow(pattern).Scan(&n); err != nil {
		b.Fatal(err)
	}
	if n != 56617 {
		b.Fatalf("Got %d rows; want: %d", n, 56617)
	}

	b.Run("Serial", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			var n int64
			if err := stmt.QueryRow(pattern).Scan(&n); err != nil {
				b.Fatal(err)
			}
		}
	})

	b.Run("Parallel", func(b *testing.B) {
		b.RunParallel(func(pb *testing.PB) {
			for pb.Next() {
				var n int64
				if err := stmt.QueryRow(pattern).Scan(&n); err != nil {
					b.Fatal(err)
				}
			}
		})
	})
}

// WARN: dev only
func BenchmarkFindLibrary(b *testing.B) {
	for i := 0; i < b.N; i++ {
		// cacheKey(DefaultSearchPaths, LibExts)
		// if _, err := findLibrary(DefaultSearchPaths); err != nil {
		if _, err := findLibrariesCached(SearchPaths()); err != nil {
			b.Fatal(err)
		}
	}
}
