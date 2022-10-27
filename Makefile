VERSION=0.1.1
CC=cc
INSTALL=install
HOST_SYS:= $(shell uname -s)
prefix=/usr/local

CFLAGS=-O2 -g -std=c11 -fPIC
LIBS := -lsqlite3

CFLAGS += $(shell pkg-config --cflags libpcre2-8)
# CFLAGS += $(shell pkg-config --cflags libpcre) # TODO: delete me
# LIBS += $(shell pkg-config --libs libpcre)
LIBS += $(shell pkg-config --libs libpcre2-8)

# WARN: not sure this is working
#
# ifeq ($(HOST_SYS),Darwin)
# ifneq ($(wildcard /opt/homebrew/opt/sqlite/lib/pkgconfig/.*),)
# export PKG_CONFIG_PATH := /opt/homebrew/opt/sqlite/lib/pkgconfig
# endif
# ifneq ($(wildcard /usr/local/opt/sqlite/lib/pkgconfig/.*),)
# export PKG_CONFIG_PATH := /usr/local/opt/sqlite/lib/pkgconfig
# endif
# CFLAGS += $(shell pkg-config --cflags sqlite3)
# LIBS += $(shell pkg-config --libs-only-L sqlite3)
# endif # Darwin

CFLAGS += -I/opt/homebrew/Cellar/sqlite/3.39.4/include
LIBS += -L/opt/homebrew/Cellar/sqlite/3.39.4/lib

#
# WARNINGS: https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
#
CFLAGS += -Wall -Wextra -Wpedantic -pedantic-errors -Wshadow
CFLAGS += -Wswitch-enum -Wcast-qual -Wpointer-arith
CFLAGS += -Wstrict-overflow=5 -Wcast-align
CFLAGS += -Wno-gnu-zero-variadic-macro-arguments
CFLAGS += -Wunused-macros
CFLAGS += -Wnull-dereference

# WARN WARN WARN WARN
# Memory sanitizer:
#
# CFLAGS+=-fsanitize=address

# LDFLAGS=-L/opt/homebrew/opt/sqlite/lib
# LIBS += $(shell pkg-config --libs libpcre2-8)
# LIBS += $(shell pkg-config --libs libpcre)
# WARN

# @echo "WAT"
# @echo "WAT"

# UNAME_S := $(shell uname -s)
# ifeq ($(UNAME_S),Linux)
# 	CFLAGS+=-D_GNU_SOURCE
# endif
# GO_INSTALL := GOBIN='$(abspath bin)' GOFLAGS= $(GO) install

.PHONY : install dist clean

# Linux
pcre.so : pcre.c
	${CC} -shared -o $@ ${CFLAGS} -W -Werror pcre.c ${LIBS} -Wl,-z,defs

# Darwin
pcre.dylib : pcre.c
	${CC} -dynamiclib -o $@ ${CFLAGS} pcre.c ${LIBS}

# Darwin
pcre2.dylib: pcre2.c
	${CC} -dynamiclib -o $@ ${CFLAGS} pcre2.c ${LIBS}

install : pcre.dylib
	${INSTALL} -pD -m755 pcre.so ${DESTDIR}${prefix}/lib/sqlite3/pcre.so

dist : clean
	mkdir sqlite3-pcre-${VERSION}
	cp -f pcre.c Makefile readme.txt sqlite3-pcre-${VERSION}
	tar -czf sqlite3-pcre-${VERSION}.tar.gz sqlite3-pcre-${VERSION}

clean :
	-rm -rf *.so *.dylib *.dylib.dSYM
