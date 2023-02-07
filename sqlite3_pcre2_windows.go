//go:build windows

package pcre2

// LibExts are the library file extensions searched for (LibraryName + ext).
var LibExts = []string{".dll"}

// DefaultSearchPaths are the default search paths for the sqlite3_pcre2
// library.
//
// FIXME: figure out what the search paths should be on Windows
var DefaultSearchPaths = BaseSearchPaths
