[![Tests](https://github.com/charlievieth/sqlite3-pcre2/actions/workflows/test.yml/badge.svg)](https://github.com/charlievieth/sqlite3-pcre2/actions/workflows/test.yml)
[![GoDoc](https://img.shields.io/badge/godoc-reference-blue.svg)](https://pkg.go.dev/github.com/charlievieth/sqlite3-pcre2@master)

# sqlite3-pcre2

sqlite3-pcre2 is a sqlite3 [extension](https://www.sqlite.org/loadext.html) that
adds the PCRE2 match functions: REGEXP and IREGEXP (case-insensitive). A small
LRU cache is used to store frequently used regexes (the size is controlled by
the CACHE_SIZE macro).

It also comes with a Go library that will automatically register the extension
with [github.com/mattn/go-sqlite3](https://github.com/mattn/go-sqlite3).

## Installing / Building

The build system here is still a work in progress and installation is manual
(the Makefile is a bit of a mess). The `make build` or `make release` targets
will create a library which can be loaded into sqlite3 and it is suggested to
either colocate the library with you application or to place it in a well known
location like: "/lib", "/usr/lib", or "/usr/local/lib" (the Go library will
search all of these when trying to load the extension).

## Examples

```sh
$ cat testdata/example.sql | sqlite3
# 1|foo
# 2|bar
# 3|baz
# 1|foo
# 2|bar
# 3|baz
```

The following [example](./testdata/example.sql) illustrates how to use
the REGEXP and IREGEXP functions and the *_INFO functions to query cache
stats (you can run this with `cat testdata/example.sql | sqlite3`:
```sql
-- sqlite3 will deduce the library extension based on the current OS
.load sqlite3_pcre2

CREATE TABLE strings (
    id    INTEGER PRIMARY KEY,
    value TEXT
);

INSERT INTO strings VALUES (1, 'foo');
INSERT INTO strings VALUES (2, 'bar');
INSERT INTO strings VALUES (3, 'baz');

SELECT * FROM strings WHERE REGEXP('^(foo|bar|baz)$', value);
-- 1|foo
-- 2|bar
-- 3|baz

SELECT * FROM strings WHERE IREGEXP('^(FOO|BAR|BAZ)$', value);
-- 1|foo
-- 2|bar
-- 3|baz

-- You can also use this syntax for the REGEXP function,
-- but not IREGEXP:
SELECT * FROM strings WHERE value REGEXP 'foo';
-- 1|foo

SELECT REGEXP_INFO('cache_in_use');
-- 2
SELECT IREGEXP_INFO('cache_in_use');
-- 1
```

## Go Library

A Go library [pcre2](https://pkg.go.dev/github.com/charlievieth/sqlite3-pcre2@master)
is included in this repo that will automatically register the sqlite3-pcre2
library with [github.com/mattn/go-sqlite3](https://github.com/mattn/go-sqlite3).
Below is an example of how to use the library.

```go
package pcre2_test

import (
	"database/sql"
	"fmt"

	// The pcre2 package can also be imported for side-effect ("_") if
	// the library search settings do not need to be changed.
	pcre2 "github.com/charlievieth/sqlite3-pcre2"
)

func Example() {
	// This example requires the sqlite3_pcre2 shared library to exist in
	// the current working directory or in a well known library path such
	// as "/lib", "/usr/lib", or "/usr/local/lib". The shared library can
	// be create by running `make build`.
	//
	// Search the current working directory for the "sqlite3_pcre2" shared library.
	pcre2.SearchWorkingDirectory(true)

	// We need to use the pcre2.DriverName to open the database connection
	// since includes the sqlite3 connection hook.
	db, err := sql.Open(pcre2.DriverName, ":memory:")
	if err != nil {
		panic(err)
	}
	defer db.Close()

	var ok bool
	if err := db.QueryRow(`SELECT REGEXP('^a$', 'a');`).Scan(&ok); err != nil {
		panic(err)
	}
	if !ok {
		panic(fmt.Sprintf("got: %t want: %t", ok, true))
	}
}
```

## Testing

There are two test targets: `test` which runs the Go tests that are
comprehensive; and `test_pcre2` which uses a small and hacky C++ test
suite. In the future the C++ test suite will be expanded to be more
comprehensive since it makes checking for memory leaks simpler.
