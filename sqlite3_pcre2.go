// Package pcre2 registers a sqlite3 PCRE2 extension for use with
// github.com/mattn/go-sqlite3.
//
// TODO: note that this adds the REGEX and non-standard IREGEX functions.
package pcre2

import (
	"database/sql"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"github.com/mattn/go-sqlite3"
)

const (
	// Name of the sqlite3_pcre2 database driver. To use this library with
	// go-sqlite3 open the database with:
	//
	// 	sql.Open(pcre2.DriverName, "opts...").
	//
	DriverName = "sqlite3_with_pcre2"

	// Name of the sqlite3_pcre2 library without file extension(s).
	LibraryName = "sqlite3_pcre2"

	// Environment variable key to set a custom path to the sqlite3_pcre2
	// library, which can be the path to the library itself or the directory
	// containing it.
	EnvKey = "SQLITE3_PCRE2_LIBRARY"
)

// ErrLibraryNotFound is returned if the sqlite3_pcre2 library could not be
// found.
var ErrLibraryNotFound = errors.New("pcre2: could not find sqlite3_pcre2 shared library")

var executable = func() string {
	s, _ := os.Executable()
	return filepath.Dir(s)
}()

// BaseSearchPaths are the first file system paths searched for the
// sqlite3_pcre2 shared library and are prepended to DefaultSearchPaths.
// Relative paths are joined with the current working directory.
// Empty strings are ignored.
//
// The search order is:
//  1. "SQLITE3_PCRE2_LIBRARY" environment variable, which can be the path to
//     the shared library or the directory containing it.
//  2. Directory containing the executable that started the current process.
//     If the executable is a symlink the directory containing the symlink is
//     also searched.
//  3. Current working directory (WARN: this can be exploited).
//  4. System specific directories ("/usr/local/lib").
//
// System specific search paths are added via the DefaultSearchPaths variable.
// The DefaultSearchPaths variable should be modified to add custom search
// paths.
var BaseSearchPaths = []string{

	// TODO: if "SQLITE3_PCRE2_LIBRARY" is set or the path is explicitly set
	// then we should only consider those and error otherwise.

	// The "SQLITE3_PCRE2_LIBRARY" environment variable which can be a
	// directory or the path to the shared library.
	os.Getenv(EnvKey),

	// Directory containing the exectuable that started the process (the
	// process may be symlinked into this directory).
	executable,

	// Directory containing the exectuable that started the process after
	// resolving symlinks (or an empty string if there are no symlinks).
	func() string {
		s, _ := filepath.EvalSymlinks(executable)
		return s
	}(),

	// Current working directory.
	".",
}

var (
	libraryPath string
	loadErr     error
	searchPaths []string
)

// WARN: should not load here
// func LibraryPath() (string, error) {
// 	LoadLibrary()
// 	return libraryPath, loadErr
// }

// WARN: implement
//
// TODO: prevent from being called mutliple times?
func SetLibraryPath(path string) {
	libraryPath = path
}

// TODO: Note that paths can be directories or files.
// TODO: Support expanding env vars?
//
// TODO: prevent from being called mutliple times?
func SetSearchPaths(paths ...string) {
	searchPaths = append(searchPaths[:0], paths...)
}

func isFile(path string) bool {
	fi, err := os.Stat(path)
	return err == nil && fi.Mode().IsRegular()
}

func addPath(paths []string, path string) []string {
	for _, s := range paths {
		if s == path {
			return paths
		}
	}
	return append(paths, path)
}

func findLibraries(paths []string) ([]string, error) {
	names := make([]string, 1, len(LibExts)+1)
	names[0] = LibraryName
	for _, ext := range LibExts {
		names = append(names, LibraryName+ext)
	}

	var candidates []string
	for _, path := range paths {
		if path == "" {
			continue
		}
		var err error
		path, err = filepath.Abs(path)
		if err != nil {
			continue
		}
		fi, err := os.Stat(path)
		if err != nil {
			continue
		}
		switch {
		case fi.Mode().IsRegular():
			candidates = addPath(candidates, path)
		case fi.IsDir():
			for _, lib := range names {
				name := filepath.Join(path, lib)
				if isFile(name) {
					candidates = addPath(candidates, name)
				}
			}
		}
	}
	if len(candidates) == 0 {
		return nil, ErrLibraryNotFound
	}
	return candidates, nil
}

var libraryCache sync.Map

func cacheKey(paths, exts []string) string {
	var w strings.Builder
	n := len(paths) + len(exts) + 1
	for _, s := range paths {
		n += len(s)
	}
	for _, s := range exts {
		n += len(s)
	}
	w.Grow(n)
	for _, s := range paths {
		w.WriteString(s)
		w.WriteByte(0)
	}
	w.WriteByte(0)
	for _, s := range exts {
		w.WriteString(s)
		w.WriteByte(0)
	}
	return w.String()
}

func findLibrariesCached(paths []string) ([]string, error) {
	key := cacheKey(paths, LibExts)
	if v, ok := libraryCache.Load(key); ok {
		return v.([]string), nil
	}
	libs, err := findLibraries(DefaultSearchPaths)
	if err != nil {
		return nil, err
	}
	libraryCache.Store(key, libs)
	return libs, nil
}

// TODO: use this to preemptively load the library and set the path to it so
// that searches can be skipped in the ConnectHook.
func LoadLibrary() (string, error) {
	panic("IMPLEMENT")
	// loadOnce.Do(func() {
	// 	libraryPath, loadErr = findLibrary()
	// })
	// return libraryPath, loadErr
}

func sqlite3ConnectHook(conn *sqlite3.SQLiteConn) error {
	libs, err := findLibrariesCached(DefaultSearchPaths)
	if err != nil {
		return ErrLibraryNotFound
	}
	for _, lib := range libs {
		err := conn.LoadExtension(lib, "sqlite3_sqlitepcre_init")
		if err == nil {
			return nil
		}
	}
	return ErrLibraryNotFound
}

func init() {
	sql.Register(DriverName, &sqlite3.SQLiteDriver{
		ConnectHook: sqlite3ConnectHook,
	})
}
