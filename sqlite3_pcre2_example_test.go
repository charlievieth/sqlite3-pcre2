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
