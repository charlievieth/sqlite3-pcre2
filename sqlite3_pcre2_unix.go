//go:build unix && !darwin

package pcre2

// LibExts are the library file extensions searched for (LibraryName + ext).
var LibExts = []string{".so"}