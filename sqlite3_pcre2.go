package pcre2

import (
	"database/sql"
	"database/sql/driver"
	"os"
	"path/filepath"

	"github.com/mattn/go-sqlite3"
)

const DriverName = "sqlite3_with_pcre2"

// TODO: allow using a env var to override the lib path?
const EnvKey = "SQLITE3_PCRE2_LIBRARY"

var libraryPath string

func SetLibraryPath(path string) {
	panic("IMPLEMENT")
}

func SetSearchPaths(paths ...string) {
	panic("IMPLEMENT")
}

func FindLibrary() error {
	panic("IMPLEMENT")
}

// WARN: macos only
var StdSearchPaths = []string{
	"/opt/homebrew/lib",
	"/usr/local/lib",
	"/usr/lib",
}

// WARN: there needs to be a better way to do this !!!
func init() {
	// WARN: only valid for testing !!!

	// TODO: check std paths
	//
	// 	Binary Path
	// 	/opt/homebrew/lib
	// 	/usr/local/lib
	// 	/usr/lib
	//

	path, ok := os.LookupEnv(EnvKey)
	if !ok {
		var err error
		path, err = filepath.Abs("./pcre2.dylib")
		if err != nil {
			panic(err)
		}
	}
	if _, err := os.Stat(path); err != nil {
		panic(err)
	}
	libraryPath = path

	// TODO: lazily find the Library by overloading Driver.Open
	// 	Open(name string) (Conn, error)

	sql.Register(DriverName,
		&sqlite3.SQLiteDriver{
			Extensions: []string{libraryPath},
		},
	)
}

func LibraryPath() string { return libraryPath }

type Driver struct {
	*sqlite3.SQLiteDriver
}

func (d *Driver) Open(name string) (driver.Conn, error) {
	return nil, nil
}

// Driver is the interface that must be implemented by a database
// driver.
//
// Database drivers may implement DriverContext for access
// to contexts and to parse the name only once for a pool of connections,
// instead of once per connection.
// type Driver interface {
// 	// Open returns a new connection to the database.
// 	// The name is a string in a driver-specific format.
// 	//
// 	// Open may return a cached connection (one previously
// 	// closed), but doing so is unnecessary; the sql package
// 	// maintains a pool of idle connections for efficient re-use.
// 	//
// 	// The returned connection is only used by one goroutine at a
// 	// time.
// 	Open(name string) (Conn, error)
// }
