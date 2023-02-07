//go:build unix && !darwin

package pcre2

// LibExts are the library file extensions searched for (LibraryName + ext).
var LibExts = []string{".so"}

// DefaultSearchPaths are the default search paths for the sqlite3_pcre2
// library.
var DefaultSearchPaths = append(
	BaseSearchPaths,
	"/usr/local/lib",
	"/usr/lib",
	"/lib",
)
