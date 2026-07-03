# 2O9 Makefile
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
PREFIX ?= /usr
VERSION := $(shell git describe --abbrev=4 --dirty 2>/dev/null || echo 0.0.1)

# 2O9-specific defines - paths the CLI binary uses
DEFS = -D_GNU_SOURCE \
       -DPACKAGE='"2O9"' \
       -DPACKAGE_VERSION='"$(VERSION)"' \
       -DSTORE_ROOT='"/nix/store"' \
       -DCONFIG_PATH='"/etc/2O9/2O9.nix"' \
       -DDBPATH='"/var/lib/2O9/"' \
       -DCACHEDIR='"/var/cache/2O9/pkg/"' \
       -DPROFILE_DIR='"/nix/var/nix/profiles/per-user/2O9-system"' \
       -DUSER_PROFILE_DIR='"/.local/state/2O9/profile"' \
       -DBIN_DIR='"/.local/bin"' \
       -DLIB_DIR='"/.local/lib"'

INCS = -Isrc -Isrc/store -Isrc/declarative -Isrc/aur -Isrc/trakker -Isrc/debag \
       -Ilib/2O9/nix -Ilib/2O9/alpm -Ilib/2O9/common \
       $(LIB2O9_DEPS_INCS)
LIBS = -lcurl -lseccomp

# ── lib2O9 build (vendored pacman + own Nix evaluator + 2O9 init) ──
#
# lib2O9 is the merged static library: modified libalpm + own C Nix
# evaluator + two9_init.c bridge. Built into lib2O9.a and linked into
# the 209 binary.
#
# Build deps:
#  - libarchive-dev (for .pkg.tar.zst extraction in add.c, be_package.c)
#  - openssl-dev    (for signature verification in signing.c)
#  - libgpgme-dev   (optional, for GPG signature verification)
#  - libcurl-dev    (already linked for AUR; libalpm uses it for downloads)
#
# The HAVE_* defines match what pacman's configure script would set.
# FSSTATSTYPE is normally detected by autoconf; on Linux it's struct statvfs.
# SYSHOOKDIR/SCRIPTLET_SHELL/LDCONFIG are install-path constants.

# User-local install location for libarchive-dev/gpgme-dev headers,
# extracted from .deb files when system-wide install isn't available.
# Set LIB2O9_DEPS_PREFIX to your local prefix if needed (e.g. ~/local).
LIB2O9_DEPS_PREFIX ?= $(HOME)/local
comma := ,
LIB2O9_DEPS_INCS := $(if $(wildcard $(LIB2O9_DEPS_PREFIX)/usr/include),-I$(LIB2O9_DEPS_PREFIX)/usr/include -I$(LIB2O9_DEPS_PREFIX)/usr/include/x86_64-linux-gnu -I$(LIB2O9_DEPS_PREFIX)/usr/include/libxml2,)
LIB2O9_DEPS_LIBS := $(if $(wildcard $(LIB2O9_DEPS_PREFIX)/usr/lib),-L$(LIB2O9_DEPS_PREFIX)/usr/lib/x86_64-linux-gnu -Wl$(comma)-rpath$(comma)$(LIB2O9_DEPS_PREFIX)/usr/lib/x86_64-linux-gnu,)

ALPM_DEFS = -DHAVE_LIBCURL -DHAVE_LIBARCHIVE -DHAVE_LIBGPGME -DHAVE_LIBSSL \
            -DHAVE_STRNLEN \
            -DHAVE_SYS_STATVFS_H -DHAVE_SYS_MOUNT_H -DHAVE_SYS_TYPES_H \
            -DFSSTATSTYPE='struct statvfs' \
            -DSYSHOOKDIR='"/usr/lib/systemd/hooks"' \
            -DSCRIPTLET_SHELL='"/bin/sh"' \
            -DLDCONFIG='"/sbin/ldconfig"' \
            -DLOCALEDIR='"/usr/share/locale"' \
            -DLIB_VERSION='"13.0.0"' \
            -D_FILE_OFFSET_BITS=64

ALPM_CFLAGS = $(CFLAGS) $(ALPM_DEFS) $(DEFS) \
              -Ilib/2O9/alpm -Ilib/2O9/common -Isrc/aur \
              $(LIB2O9_DEPS_INCS) \
              -Wno-unused-parameter -Wno-format-truncation -Wno-comment \
              -Wno-address-of-packed-member -Wno-multichar -Wno-switch \
              -Wno-calloc-transposed-args

ALPM_SRC = $(wildcard lib/2O9/alpm/*.c) $(wildcard lib/2O9/common/*.c)
ALPM_OBJ = $(patsubst lib/2O9/%.c,lib/2O9/%.o,$(ALPM_SRC))

# ── 209 binary source ──
CLI_SRC   = src/cli/main.c
STORE_SRC = src/store/store.c src/store/symlinks.c
DECL_SRC  = src/declarative/gen.c src/declarative/reconcile.c src/declarative/activation.c src/declarative/gen_index.c
AUR_SRC   = src/aur/aur_rpc.c src/aur/aur_build.c src/aur/aur_resolve.c
TRAK_SRC  = src/trakker/trakker.c src/debag/debag.c src/debag/static_analysis.c src/debag/seccomp_filter.c src/debag/script_analysis.c
NIX_SRC   = lib/2O9/nix/nix_eval.c lib/2O9/nix/nix_lexer.c lib/2O9/nix/nix_parser.c

SRC = $(CLI_SRC) $(STORE_SRC) $(DECL_SRC) $(AUR_SRC) $(TRAK_SRC) $(NIX_SRC)
OBJ = $(SRC:.c=.o)

# Link libs: libcurl for AUR + libalpm downloads, libarchive for .pkg.tar.zst,
# libgpgme + assuan + gpg-error for signature verification, openssl for crypto,
# plus libarchive's transitive deps (zlib, lzma, bz2, lz4, zstd, nettle, gmp).
LIB2O9_LIBS = -lcurl -larchive -lgpgme -lassuan -lgpg-error -lcrypto \
              -lz -llzma -lbz2 -llz4 -lzstd -lnettle -lhogweed -lgmp \
              -lxml2 -lacl -lm -lseccomp \
              $(LIB2O9_DEPS_LIBS)

all: 209 test-aur-rpc test-nix-lexer test-nix-eval

# lib2O9.a - the merged static library (modified libalpm + Nix evaluator + 2O9 init)
lib2O9.a: $(ALPM_OBJ)
	$(AR) rcs $@ $^

# 209 binary - links lib2O9.a + the 209-specific source
209: $(OBJ) lib2O9.a
	$(CC) $(CFLAGS) -o $@ $(OBJ) lib2O9.a $(LIB2O9_LIBS)

test-aur-rpc: src/aur/test_aur_rpc.o src/aur/aur_rpc.o lib/2O9/common/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -lcurl

test-nix-lexer: lib/2O9/nix/test_nix_lexer.o lib/2O9/nix/nix_lexer.o lib/2O9/nix/nix_eval.o lib/2O9/nix/nix_parser.o
	$(CC) $(CFLAGS) -o $@ $^

test-nix-eval: lib/2O9/nix/test_nix_eval.o lib/2O9/nix/nix_lexer.o lib/2O9/nix/nix_eval.o lib/2O9/nix/nix_parser.o
	$(CC) $(CFLAGS) -o $@ $^

# Pattern rule for 209-specific objects (uses 209 INCS)
%.o: %.c
	$(CC) $(CFLAGS) $(DEFS) $(INCS) -c -o $@ $<

# Pattern rule for lib2O9 objects (uses ALPM_CFLAGS with HAVE_* defines)
# Covers both lib/2O9/alpm/ and lib/2O9/common/ subdirs.
lib/2O9/%.o: lib/2O9/%.c
	$(CC) $(ALPM_CFLAGS) -c -o $@ $<

clean:
	rm -f 209 test-aur-rpc test-nix-lexer test-nix-eval lib2O9.a \
              $(OBJ) $(ALPM_OBJ) \
              src/aur/test_aur_rpc.o lib/2O9/nix/test_nix_lexer.o lib/2O9/nix/test_nix_eval.o

install: 209
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 209 $(DESTDIR)$(PREFIX)/bin/209

# Run unit + integration tests
test: test-nix-eval test-nix-lexer test-aur-rpc
	@echo "=== running integration tests ==="
	@for t in test/test_*.sh; do \
                echo "--- $$t ---"; \
                ./$$t ./209 || exit 1; \
        done
	@echo "=== all tests passed ==="

.PHONY: all clean install test
