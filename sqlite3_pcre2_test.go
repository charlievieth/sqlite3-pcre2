// WARN: must be ran with: `go test --tags "libsqlite3 darwin"`

package pcre2

import (
	"bytes"
	"database/sql"
	"database/sql/driver"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"testing"

	"github.com/mattn/go-sqlite3"
)

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

const CreateStringsTableStmt = `
CREATE TABLE IF NOT EXISTS strings_table (
    id    INTEGER PRIMARY KEY,
    value TEXT
);`

var TableNames = []string{
	"strings_table",
}

func fileExists(name string) bool {
	fi, err := os.Stat(name)
	return err == nil && fi.Mode().IsRegular()
}

// TODO: try to use the same DB connection so that we
// can test the thread safety of the library.
func InitDatabase(t testing.TB) *sql.DB {
	// WARN: _mutex=full
	// &mode=memory
	if fileExists("test.db") {
		if err := os.Remove("test.db"); err != nil {
			t.Fatal(err)
		}
	}

	const filename = ":memory:"
	// const filename = "test.db"
	const opts = "?cache=shared&_mutex=no"
	// db, err := sql.Open("sqlite3_with_regexp", "file:test.db?cache=shared")
	db, err := sql.Open(DriverName, "file:"+filename+opts)
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Ping(); err != nil {
		t.Fatal(err)
	}

	for _, name := range TableNames {
		db.Exec(fmt.Sprintf("TRUNCATE TABLE IF EXISTS %s;", name))
	}

	for _, stmt := range []string{
		CreateStringsTableStmt,
	} {
		if _, err := db.Exec(stmt); err != nil {
			t.Fatal(err)
		}
	}

	t.Cleanup(func() {
		db.Close()
	})
	return db
}

func InsertIntoStringsTable(t testing.TB, db *sql.DB, values ...any) {
	if _, err := db.Exec("DELETE FROM strings_table;"); err != nil {
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
	stmt, err := tx.Prepare("INSERT INTO strings_table (value) VALUES (?);")
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

// Test in parallel using the same connection.
//
// TODO: clean this up
func TestPCRE2_XXX(t *testing.T) {

	// Open db conn

	sqlDriver := &sqlite3.SQLiteDriver{
		Extensions: []string{libraryPath},
	}
	// WARN: full MUTEX
	// v, err := sqlDriver.Open("file::memory:?cache=shared&_mutex=no")

	v, err := sqlDriver.Open("file::memory:?cache=shared&_mutex=full")
	if err != nil {
		t.Fatal(err)
	}
	conn := v.(*sqlite3.SQLiteConn)

	for _, name := range TableNames {
		conn.Exec(fmt.Sprintf("DELETE FROM %s;", name), nil)
	}

	for _, stmt := range []string{
		CreateStringsTableStmt,
	} {
		if _, err := conn.Exec(stmt, nil); err != nil {
			t.Fatal(err)
		}
	}

	// Insert values

	insertStmt, err := conn.Prepare("INSERT INTO strings_table (value) VALUES (?);")
	if err != nil {
		t.Fatal(err)
	}

	for ch := 'A'; ch <= 'Z'; ch++ {
		val := strings.Repeat(fmt.Sprintf("%c", ch), 4)
		for i := 0; i < 4; i++ {
			_, err := insertStmt.Exec([]driver.Value{val})
			if err != nil {
				t.Fatal(err)
			}
		}
	}

	if err := insertStmt.Close(); err != nil {
		t.Fatal(err)
	}

	const queryFmt = "SELECT COUNT(*) FROM strings_table WHERE value REGEXP '.*%c.*';"

	queryRow := func(conn *sqlite3.SQLiteConn, query string) (int64, error) {
		rows, err := conn.Query(query, nil)
		if err != nil {
			return 0, err
		}
		dest := []driver.Value{int64(0)}
		if err := rows.Next(dest); err != nil {
			return 0, err
		}
		if err := rows.Next(nil); err != io.EOF {
			return 0, err
		}
		if err := rows.Close(); err != nil {
			return 0, err
		}
		return dest[0].(int64), nil
	}

	t.Run("Serial", func(t *testing.T) {
		for ch := 'A'; ch <= 'Z'; ch++ {
			query := fmt.Sprintf(queryFmt, ch)
			n, err := queryRow(conn, query)
			if err != nil {
				t.Fatal(err)
			}
			if n != 4 {
				t.Fatalf("%c: got: %d; want: %d", ch, n, 4)
			}
		}
	})

	t.Run("Parallel", func(t *testing.T) {
		iterations := 500
		if testing.Short() {
			iterations = 100
		}
		var wg sync.WaitGroup
		start := make(chan struct{})
		for ch := 'A'; ch <= 'Z'; ch++ {
			wg.Add(1)
			go func(ch rune) {
				defer wg.Done()
				query := fmt.Sprintf(queryFmt, ch)
				<-start // start all the queries at once
				for i := 0; i < iterations; i++ {
					n, err := queryRow(conn, query)
					if err != nil {
						t.Error(err)
						return
					}
					if n != 4 {
						t.Errorf("%c: got: %d; want: %d", ch, n, 4)
						return
					}
				}
			}(ch)
		}
		close(start)
		wg.Wait()
	})
}

func TestPCRE2(t *testing.T) {
	db := InitDatabase(t)

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
	db := InitDatabase(t)

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
	db := InitDatabase(t)

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
	db := InitDatabase(t)

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
	db := InitDatabase(t)

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

type MatchTest struct {
	pattern, subject string
	match            bool
}

func (m *MatchTest) Query() string {
	return "SELECT REGEXP('" + m.pattern + "', '" + m.subject + "');"
}

func TestUnicode(t *testing.T) {
	db := InitDatabase(t)

	tests := []MatchTest{
		{`🙈.*🙉.*🙊`, `😈 🙈 🙉 🙊 😈`, true},
		{`🙈.*🙉[^a]+🙊`, `😈 🙈 🙉 a 🙊 😈`, false},
		{`🙈.+🙉.+🙊`, `😈 🙈 🙉 🙊 😈`, true},
		{`🙈.+🙉.+🙊`, `🙈🙉🙊`, false},
		{`🙈\s+🙉\s+🙊`, ` 🙈 🙉 🙊 `, true},
	}
	for _, test := range tests {
		var match bool
		if err := db.QueryRow(test.Query()).Scan(&match); err != nil {
			t.Fatal(err)
		}
		if match != test.match {
			t.Fatalf("`%s` = %t; want: %t", test.Query(), match, test.match)
		}
	}
}

func TestBinaryBlob(t *testing.T) {
	db := InitDatabase(t)

	var runes []rune
	for i := rune(0); i <= ' '; i++ {
		runes = append(runes, i)
	}
	InsertIntoStringsTable(t, db,
		string(runes[:len(runes)-1]),
		string(runes),
	)

	var rows int64
	err := db.QueryRow("SELECT COUNT(*) FROM strings_table WHERE value REGEXP ?;", "[[:print:]]+").Scan(&rows)
	if err != nil {
		t.Fatal(err)
	}
	if rows != 1 {
		t.Fatalf("rows got: %d want: %d", rows, 1)
	}
}

func TestHugePattern(t *testing.T) {
	t.Skip("FIXME: don't print massive patterns")

	db := InitDatabase(t)

	// SELECT REGEXP('x', 'x');

	// Another option is to use `b*` repeated 9,000 times (this is too big for the JIT)
	// https://github.com/PCRE2Project/pcre2/blob/b73b3347673358795e86a99acad56d81c2e4fecd/testdata/testoutput17#L16

	for i := 0; i < 5; i++ {
		n := 9_000 * (i + 1)
		t.Run(fmt.Sprintf("%d", n), func(t *testing.T) {
			pattern := strings.Repeat("b*", n)
			subject := strings.Repeat("b", n/2)
			var match bool
			err := db.QueryRow(fmt.Sprintf("SELECT REGEXP('%s', '%s');", pattern, subject)).Scan(&match)
			if err != nil {
				t.Fatal(err)
			}
			if !match {
				t.Error("expected match to be true")
			}
		})
	}

	// pattern := strings.Repeat("b*", 9_000)
	// subject := strings.Repeat("b", 9_000/2)
	// var match bool
	// err := db.QueryRow(fmt.Sprintf("SELECT REGEXP('%s', '%s');", pattern, subject)).Scan(&match)
	// if err != nil {
	// 	t.Fatal(err)
	// }
	// if !match {
	// 	t.Error("expected match to be true")
	// }
}

// (?:[a-z0-9!#$%&'*+/=?^_`{|}~-]+(?:\.[a-z0-9!#$%&'*+/=?^_`{|}~-]+)*|"(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21\x23-\x5b\x5d-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])*")@(?:(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z0-9](?:[a-z0-9-]*[a-z0-9])?|\[(?:(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9]))\.){3}(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9])|[a-z0-9-]*[a-z0-9]:(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21-\x5a\x53-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])+)\])

func TestComplexRegexp(t *testing.T) {
	// https://stackoverflow.com/questions/201323/how-can-i-validate-an-email-address-using-a-regular-expression
	const emailRe = `(?:[a-z0-9!#$%&'*+/=?^_` + "`" + `{|}~-]+(?:\.[a-z0-9!#$%&'*+/=?^_` + "`" + `{|}~-]+)*|"(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21\x23-\x5b\x5d-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])*")@(?:(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z0-9](?:[a-z0-9-]*[a-z0-9])?|\[(?:(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9]))\.){3}(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9])|[a-z0-9-]*[a-z0-9]:(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21-\x5a\x53-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])+)\])`
	db := InitDatabase(t)

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

// WARN: bad benchmark
func BenchmarkPCRE2(b *testing.B) {
	db := InitDatabase(b)
	b.Cleanup(func() { db.Close() })

	for ch := 'A'; ch <= 'Z'; ch++ {
		val := strings.Repeat(fmt.Sprintf("%c", ch), 4)
		for i := 0; i < 4; i++ {
			_, err := db.Exec("INSERT INTO strings_table (value) VALUES (?);", val)
			if err != nil {
				b.Fatal(err)
			}
		}
	}

	const query = "SELECT COUNT(*) FROM strings_table WHERE value REGEXP 'OO';"
	for i := 0; i < b.N; i++ {
		var n int64
		if err := db.QueryRow(query).Scan(&n); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkLIKE2(b *testing.B) {
	db := InitDatabase(b)
	b.Cleanup(func() { db.Close() })

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
