//go:build !unix && !windows

package pcre2

// LibExts are the library file extensions searched for (LibraryName + ext).
var LibExts = []string{}

// DefaultSearchPaths are the default search paths for the sqlite3_pcre2
// library.
var DefaultSearchPaths = BaseSearchPaths
