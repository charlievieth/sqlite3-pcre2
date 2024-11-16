# vim: ts=4 sw=4

# WARN: this Makefile is a mess and in serious need of cleanup.
# At some point, this project will be updated to use CMake (once
# I learn to stop fighting it) or meson.

MAJVER=  0
MINVER=  0
RELVER=  1
PREREL=
VERSION= $(MAJVER).$(MINVER).$(RELVER)$(PREREL)

##############################################################################
# Build options
##############################################################################

# Change the installation path as needed.
# Note: PREFIX must be an absolute path!
export PREFIX= /usr/local

PCRE2_CFLAGS =
PCRE2_LIBS =
SQLITE3_CFLAGS =
SQLITE3_LIBS =

##############################################################################
# Compiler/tool options
##############################################################################

DEFAULT_CC = cc
# DEFAULT_CC = gcc-14
DEFAULT_CXX = g++
CC = $(DEFAULT_CC)
CXX = $(DEFAULT_CXX)
GOTAGS =

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
  $(error Windows is not currently supported!)
  HOST_SYS= Windows
endif
TARGET_SYS ?= $(HOST_SYS)

##############################################################################
# Compiler flags
##############################################################################

CFLAGS = -O2 -g -std=c11
# CPPFLAGS = -O2 -g -std=c++20

##############################################################################
# pcre2
##############################################################################

# TODO: use `pcre2-config`

# ifndef PCRE2_CFLAGS
#   PCRE2_CFLAGS = $(shell pkg-config --cflags libpcre2-8)
# endif
# ifndef PCRE2_LIBS
#   PCRE2_LIBS = $(shell pkg-config --libs libpcre2-8)
# endif
CFLAGS += $(shell pkg-config --cflags libpcre2-8)
LIBS += $(shell pkg-config --libs libpcre2-8)

##############################################################################
# sqlite3
##############################################################################

LIBS += -lsqlite3

# TODO: make using the Homebrew installed sqlite3 optional
ifeq ($(TARGET_SYS),Darwin)
  ifneq ("$(wildcard /opt/homebrew/opt/sqlite/lib/pkgconfig)","")
    PKG_CONFIG_PATH = /opt/homebrew/opt/sqlite/lib/pkgconfig
  endif
  ifneq ("$(wildcard /usr/local/opt/sqlite/lib/pkgconfig)","")
    PKG_CONFIG_PATH = /usr/local/opt/sqlite/lib/pkgconfig
  endif
  export PKG_CONFIG_PATH
  CFLAGS += $(shell pkg-config --cflags sqlite3)
  LIBS += $(shell pkg-config --libs-only-L sqlite3)
  GOTAGS += -tags "libsqlite3,darwin"
endif

##############################################################################
# Warning options.
##############################################################################

CFLAGS += -Wall -Wextra -Wpedantic -pedantic-errors -Wshadow
CFLAGS += -Wswitch-enum -Wcast-qual -Wpointer-arith
CFLAGS += -Wstrict-overflow=5 -Wcast-align
# TODO: this is clang specific
# CFLAGS += -Wno-gnu-zero-variadic-macro-arguments
CFLAGS += -Wunused-macros
CFLAGS += -Wnull-dereference
CFLAGS += -Wconversion
CFLAGS += -Wno-sign-conversion
# CEV: disable if this gets too noisy
CFLAGS += -Wwrite-strings
CFLAGS += -Winline

# TODO: consider adding
# -minline-stringops-dynamically
# -minline-all-stringops

# WARN: debug (and gcc) only
# CFLAGS += -Wno-aggressive-loop-optimizations
# CFLAGS += -Wvector-operation-performance
# CFLAGS += -Wdisabled-optimization
# CFLAGS += -fanalyzer

##############################################################################
# System specific options.
##############################################################################

ifeq ($(TARGET_SYS),Darwin)
  TARGET_EXE = dylib
  CC_FLAGS = -dynamiclib -fPIC
else
  TARGET_EXE = so
  CC_FLAGS = -shared -fPIC
  LIBS += -Wl,-z,defs
endif

TARGET_DEP = sqlite3_pcre2.$(TARGET_EXE)
TEST_DEP = pcre2_test

##############################################################################
# Make targets.
##############################################################################

.PHONY: build clean dist install

$(TARGET_DEP): pcre2.c
	@${CC} ${CC_FLAGS} -o $@ ${CFLAGS} pcre2.c ${LIBS}

# WARN: split C/C++ flags
$(TEST_DEP): pcre2_test.cc
	${CXX} -o $@ ${CFLAGS} -std=c++20 ${LIBS} pcre2_test.cc

build: $(TARGET_DEP) $(TEST_DEP)

clean:
	@$(RM) *.so *.dylib *.plist sqlite3-pcre2-*.tar.gz $(TEST_DEP)
	@$(RMDIR) *.dylib.dSYM *.dSYM sqlite3-pcre2-*
	@go clean

.PHONY: address
address: CFLAGS+=-fsanitize=address
address: clean build

install: CFLAGS+=-DNDEBUG
install: build
	${INSTALL} -pD -m755 ${TARGET_DEP} ${DESTDIR}${PREFIX}/lib/sqlite3/${TARGET_DEP}

dist: clean
	$(MKDIR) sqlite3-pcre2-${VERSION}
	cp pcre2.c Makefile readme.txt sqlite3-pcre2-${VERSION}
	tar -czf sqlite3-pcre2-${VERSION}.tar.gz sqlite3-pcre2-${VERSION}

# WARN: fix build tags for Darwin
.PHONY: test
test: CFLAGS+=-DMAX_DISPLAYED_PATTERN_LENGTH=256
test: build
	SQLITE3_PCRE2_LIBRARY=$(TARGET_DEP) go test $(GOTAGS)

# TODO: reuse make targets
.PHONY: testrace
testrace: CFLAGS+=-DMAX_DISPLAYED_PATTERN_LENGTH=256
testrace: build
	@SQLITE3_PCRE2_LIBRARY=$(TARGET_DEP) go test $(GOTAGS) -race

# TODO: reuse make targets
.PHONY: shorttest
shorttest: CFLAGS+=-DMAX_DISPLAYED_PATTERN_LENGTH=256
shorttest: build
	@SQLITE3_PCRE2_LIBRARY=$(TARGET_DEP) go test $(GOTAGS)

# TODO: rename
.PHONY: test_pcre2
test_pcre2: build
	@echo ""
	@./$(TEST_DEP)

# TODO: build and run in separate steps
# pcre2_test: CFLAGS+=-fsanitize=address
# pcre2_test: address
# pcre2_test: build
# 	${CC} -o $@ ${CFLAGS} pcre2_test.c ${LIBS}
# 	@./pcre2_test

# WARN: make is this correct
compile_commands.json: pcre2.c
compile_commands.json: $(TARGET_DEP)
	bear -- $(MAKE) build

.PHONY: lint_pcre2
lint_pcre2: compile_commands.json
	clang-tidy ./pcre2.c

.PHONY: lint_pcre2_test
lint_pcre2_test: compile_commands.json
	clang-tidy ./pcre2_test.cc

.PHONY: lint
lint: lint_pcre2 lint_pcre2_test

.PHONY: all
all: build
