# vim: ts=4 sw=4

MAJVER=  0
MINVER=  0
RELVER=  1
PREREL=
VERSION= $(MAJVER).$(MINVER).$(RELVER)$(PREREL)

##############################################################################
#
# Change the installation path as needed. This automatically adjusts
# the paths in src/luaconf.h, too. Note: PREFIX must be an absolute path!
#
export PREFIX= /usr/local
export MULTILIB= lib
##############################################################################

DEFAULT_CC = gcc
CC= $(DEFAULT_CC)

RM= rm -f
MKDIR= mkdir -p
RMDIR= rm -rf
SYMLINK= ln -sf
INSTALL_X= install -m 0755
INSTALL_F= install -m 0644
UNINSTALL= $(RM)
INSTALL=install

ifeq (,$(findstring Windows,$(OS)))
  HOST_SYS:= $(shell uname -s)
else
  HOST_SYS= Windows
endif
TARGET_SYS?= $(HOST_SYS)

##############################################################################
# Compiler flags
##############################################################################

CFLAGS=-O2 -g -std=c11 -fPIC

##############################################################################
# pcre2
##############################################################################

CFLAGS += $(shell pkg-config --cflags libpcre2-8)
LIBS += $(shell pkg-config --libs libpcre2-8)

##############################################################################
# sqlite3
##############################################################################

LIBS += -lsqlite3
ifeq ($(TARGET_SYS),Darwin)
	# Try to use the Homebrew installed sqlite3
	ifneq ("$(wildcard /opt/homebrew/opt/sqlite3/lib)","")
		CFLAGS += -I/opt/homebrew/opt/sqlite3/lib/include
		LIBS += -L/opt/homebrew/opt/sqlite3/lib
	else
	ifneq ("$(wildcard /usr/local/opt/sqlite3/lib)","")
		CFLAGS += -I/usr/local/opt/sqlite3/lib/include
		LIBS += -L/usr/local/opt/sqlite3/lib
	else
	endif
	endif
endif

##############################################################################
# Warning options.
##############################################################################

CFLAGS += -Wall -Wextra -Wpedantic -pedantic-errors -Wshadow
CFLAGS += -Wswitch-enum -Wcast-qual -Wpointer-arith
CFLAGS += -Wstrict-overflow=5 -Wcast-align
CFLAGS += -Wno-gnu-zero-variadic-macro-arguments
CFLAGS += -Wunused-macros
CFLAGS += -Wnull-dereference

##############################################################################
# System specific options.
##############################################################################

ifeq ($(TARGET_SYS),Linux)
	INSTALL_DEP = pcre2.so
	CC_FLAGS = -shared
	LIBS += -Wl,-z,defs
else
ifeq ($(TARGET_SYS),Darwin)
	INSTALL_DEP = pcre2.dylib
	CC_FLAGS = -dynamiclib
endif
endif

##############################################################################
# Make targets.
##############################################################################

.PHONY: build clean dist install

$(INSTALL_DEP): pcre2.c
	${CC} ${CC_FLAGS} -o $@ ${CFLAGS} pcre2.c ${LIBS}

build: $(INSTALL_DEP)

clean:
	@$(RM) *.so *.dylib *.plist sqlite3-pcre2-*.tar.gz
	@$(RMDIR) *.dylib.dSYM sqlite3-pcre2-*

install: CFLAGS+=-DNDEBUG
install: $(INSTALL_DEP)
	${INSTALL} -pD -m755 ${INSTALL_DEP} ${DESTDIR}${PREFIX}/lib/sqlite3/${INSTALL_DEP}

dist: clean
	$(MKDIR) sqlite3-pcre2-${VERSION}
	cp pcre2.c Makefile readme.txt sqlite3-pcre2-${VERSION}
	tar -czf sqlite3-pcre2-${VERSION}.tar.gz sqlite3-pcre2-${VERSION}

