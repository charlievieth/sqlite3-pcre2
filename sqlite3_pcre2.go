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
	"runtime"
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
	DriverName = "sqlite3_pcre2"

	// Name of the sqlite3_pcre2 library without file extension(s).
	LibraryName = "sqlite3_pcre2"

	// Environment variable key to set a custom path to the sqlite3_pcre2
	// library, which can be the path to the library itself or the directory
	// containing it.
	//
	// The path must be absolute.
	EnvKey = "SQLITE3_PCRE2_LIBRARY"
)

// ErrLibraryNotFound is returned if the sqlite3_pcre2 library could not be
// found.
var ErrLibraryNotFound = errors.New("pcre2: could not find sqlite3_pcre2 shared library")

var (
	searchWD        bool   // search current working directory
	libraryPath     string // user specified path to the sqlite3_pcre2 library
	progDir         string // directory containing the running executable
	progDirAbs      string // resolved path to progDir
	loadprogExeOnce sync.Once
)

func requireAbsPath(path string) string {
	path = filepath.Clean(path)
	if !filepath.IsAbs(path) {
		return ""
	}
	return path
}

func loadProgExe() {
	exe, _ := os.Executable()
	if exe == "" {
		return
	}
	progDir = requireAbsPath(filepath.Dir(exe))
	if progDir != "" {
		progDirAbs, _ = filepath.EvalSymlinks(progDir)
		progDirAbs = requireAbsPath(progDirAbs)
	}
}

// SearchWorkingDirectory toggles if the current working directory is searched
// for the sqlite3_pcre2 shared library and returns the previous state.
//
// Searching the current working directory is disabled by default because it
// can be exploited to load malicious code.
func SearchWorkingDirectory(search bool) (previous bool) {
	previous = searchWD
	searchWD = search
	return previous
}

// SearchPaths returns the system paths searched for the sqlite3_pcre2 shared
// library.
//
// The search order is:
//
//  1. The path set by SetLibraryPath(), if any, which can be the path to the
//     shared library or the directory containing it. If set, the seach ends
//     here (that is the below locations will not be checked).
//
//  2. "SQLITE3_PCRE2_LIBRARY" environment variable, which can be the path to
//     the shared library or the directory containing it.
//
//  3. Directory containing the executable that started the current process.
//     If the executable is a symlink the directory containing the symlink is
//     also searched.
//
//  4. System specific directories ("/usr/local/lib").
//
//  5. The current working directory, but only if SearchWorkingDirectory(true)
//     is enabled.
//
// Relative paths are joined with the current working directory. Empty strings
// are ignored.
func SearchPaths() []string {
	if libraryPath != "" {
		return filepath.SplitList(libraryPath)
	}
	loadprogExeOnce.Do(loadProgExe)
	paths := make([]string, 0, 8)
	if env := os.Getenv(EnvKey); env != "" {
		for _, p := range filepath.SplitList(env) {
			if p = requireAbsPath(p); p != "" {
				paths = append(paths, p)
			}
		}
	}
	paths = append(paths, progDir, progDirAbs)
	if runtime.GOOS == "darwin" {
		paths = append(paths, "/opt/homebrew/lib")
	}
	if runtime.GOOS != "windows" {
		paths = append(paths, "/usr/local/lib", "/usr/lib", "/lib")
	}
	if searchWD {
		paths = append(paths, ".")
	}
	a := paths[:0]
	for _, s := range paths {
		if s != "" {
			a = append(a, s)
		}
	}
	return a
}

// SetLibraryPath sets the path to the sqlite3_pcre2 shared library, which can
// be the path to the shared library or the directory containing it. Relative
// paths are allowed.
//
// If set, then *only* this path will be searched.
func SetLibraryPath(path string) {
	libraryPath = path
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

// TODO: only cache the library that was successfully loaded
func findLibrariesCached(paths []string) ([]string, error) {
	key := cacheKey(paths, LibExts)
	if v, ok := libraryCache.Load(key); ok {
		return v.([]string), nil
	}
	libs, err := findLibraries(paths)
	if err != nil {
		return nil, err
	}
	libraryCache.Store(key, libs)
	return libs, nil
}

func Sqlite3ConnectHook(conn *sqlite3.SQLiteConn) error {
	paths := SearchPaths()
	libs, err := findLibrariesCached(paths)
	if err != nil {
		return ErrLibraryNotFound
	}
	// TODO: add lib paths to the returned error, if any.
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
		ConnectHook: Sqlite3ConnectHook,
	})
}
