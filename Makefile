# 2O9 Makefile
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
PREFIX ?= /usr
VERSION := $(shell git describe --abbrev=4 --dirty 2>/dev/null || echo 0.1.0)

# Phase 2: sqlite3 detection (store DB / refs graph for GC).
HAVE_SQLITE3 := $(shell pkg-config --exists sqlite3 2>/dev/null && echo yes)
ifeq ($(HAVE_SQLITE3),yes)
SQLITE_DEFS   := -DHAVE_SQLITE3
SQLITE_LIBS   := $(shell pkg-config --libs sqlite3)
SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3)
else
$(warning libsqlite3 not found via pkg-config; building without store DB - GC falls back to set-based)
SQLITE_DEFS   :=
SQLITE_LIBS   :=
SQLITE_CFLAGS :=
endif


# 2O9-specific defines - paths the CLI binary uses
DEFS = -D_GNU_SOURCE \
       -DPACKAGE='"2O9"' \
       -DPACKAGE_VERSION='"$(VERSION)"' \
       -DSTORE_ROOT='"/nix/store"' \
       -DCONFIG_PATH='"/nix/config/2O9.nix"' \
       -DDBPATH='"/var/lib/2O9/"' \
       -DCACHEDIR='"/var/cache/2O9/pkg/"' \
       -DPROFILE_DIR='"/nix/var/nix/profiles/per-user/2O9-system"' \
       -DUSER_PROFILE_DIR='"/.local/state/2O9/profile"' \
       -DBIN_DIR='"/.local/bin"' \
       -DLIB_DIR='"/.local/lib"' \
       $(SQLITE_DEFS) \
       $(SODIUM_DEFS) \
       $(CAPSTONE_DEFS)

INCS = -Isrc -Isrc/store -Isrc/declarative -Isrc/aur -Isrc/trakker -Isrc/debag \
       -Ilib/2O9/nix -Ilib/2O9/alpm -Ilib/2O9/common \
       $(LIB2O9_DEPS_INCS) $(SQLITE_CFLAGS) $(SODIUM_CFLAGS) $(CAPSTONE_CFLAGS)
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


# Phase 3: Ed25519 signing for binary-cache narinfo signatures.
# Prefer libsodium (smaller, cleaner API); fall back to OpenSSL 1.1+
# Ed25519 via EVP_DigestSign/DigestVerify (-lcrypto is already linked).
# pkg-config check; if no .pc file but sodium.h is on the local deps
# prefix include path, accept that too.
HAVE_SODIUM_PC := $(shell pkg-config --exists libsodium 2>/dev/null && echo yes)
HAVE_SODIUM_HDR := $(shell test -f $(LIB2O9_DEPS_PREFIX)/usr/include/sodium.h && echo yes)
ifeq ($(HAVE_SODIUM_PC),yes)
SODIUM_DEFS := -DHAVE_SODIUM
SODIUM_LIBS := $(shell pkg-config --libs libsodium)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium)
else ifeq ($(HAVE_SODIUM_HDR),yes)
SODIUM_DEFS := -DHAVE_SODIUM
SODIUM_LIBS := -lsodium
SODIUM_CFLAGS :=
else
$(warning libsodium not found; falling back to OpenSSL Ed25519 for signing)
SODIUM_DEFS :=
SODIUM_LIBS :=
SODIUM_CFLAGS :=
endif

# Capstone (optional) - powers disassembly in the debag static-db and
# step-over instruction-length decoding in the dynamic-db REPL. Without
# it, those features fall back to a stub or a mini manual decoder. Prefer
# pkg-config (system install); fall back to the local-deps prefix used
# for libarchive-dev/gpgme-dev when extracted from .deb packages.
HAVE_CAPSTONE_PC := $(shell pkg-config --exists capstone 2>/dev/null && echo yes)
HAVE_CAPSTONE_HDR := $(shell test -f $(LIB2O9_DEPS_PREFIX)/usr/include/capstone/capstone.h && echo yes)
ifeq ($(HAVE_CAPSTONE_PC),yes)
CAPSTONE_DEFS := -DHAVE_CAPSTONE
CAPSTONE_LIBS := $(shell pkg-config --libs capstone)
CAPSTONE_CFLAGS := $(shell pkg-config --cflags capstone)
else ifeq ($(HAVE_CAPSTONE_HDR),yes)
CAPSTONE_DEFS := -DHAVE_CAPSTONE
CAPSTONE_LIBS := -lcapstone
CAPSTONE_CFLAGS := -I$(LIB2O9_DEPS_PREFIX)/usr/include/capstone
else
$(warning libcapstone not found; debag static-db pd/pdd will print a hint, dynamic-db dso uses mini decoder)
CAPSTONE_DEFS :=
CAPSTONE_LIBS :=
CAPSTONE_CFLAGS :=
endif
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
CLI_SRC   = src/cli/main.c src/cli/subs_ui.c
STORE_SRC = src/store/store.c src/store/symlinks.c src/store/nar.c src/store/optimise.c \
            src/store/narinfo.c src/store/binary-cache.c src/store/signing.c src/store/share.c
ifeq ($(HAVE_SQLITE3),yes)
STORE_SRC += src/store/db.c src/store/snapshot.c
endif
DECL_SRC  = src/declarative/gen.c src/declarative/reconcile.c src/declarative/reconcile_execute.c src/declarative/activation.c src/declarative/gen_index.c src/declarative/users.c src/declarative/fstab.c src/declarative/bootloader.c
AUR_SRC   = src/aur/aur_rpc.c src/aur/aur_build.c src/aur/aur_resolve.c \
            src/aur/chroot.c src/aur/pgp.c src/aur/config.c
TRAK_SRC  = src/trakker/trakker.c src/debag/debag.c src/debag/static_analysis.c src/debag/seccomp_filter.c src/debag/script_analysis.c src/debag/dynamic_db.c src/debag/static_db.c
NIX_SRC   = lib/2O9/nix/nix_eval.c lib/2O9/nix/nix_lexer.c lib/2O9/nix/nix_parser.c

SRC = $(CLI_SRC) $(STORE_SRC) $(DECL_SRC) $(AUR_SRC) $(TRAK_SRC) $(NIX_SRC)
OBJ = $(SRC:.c=.o)

# Link libs: libcurl for AUR + libalpm downloads, libarchive for .pkg.tar.zst,
# libgpgme + assuan + gpg-error for signature verification, openssl for crypto,
# plus libarchive's transitive deps (zlib, lzma, bz2, lz4, zstd, nettle, gmp).
LIB2O9_LIBS = -lcurl -larchive -lgpgme -lassuan -lgpg-error -lcrypto -lsqlite3 \
              -lz -llzma -lbz2 -llz4 -lzstd -lnettle -lhogweed -lgmp \
              -lxml2 -lacl -lm -lseccomp -ldl \
              $(SODIUM_LIBS) \
              $(CAPSTONE_LIBS) \
              $(LIB2O9_DEPS_LIBS)

all: 209 test-aur-rpc test-nix-lexer test-nix-eval test-nix-eval-edge \
     test-nar test-db test-signing test-narinfo test-keygen

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

test-nix-eval-edge: lib/2O9/nix/test_nix_eval_edge.o lib/2O9/nix/nix_lexer.o lib/2O9/nix/nix_eval.o lib/2O9/nix/nix_parser.o
	$(CC) $(CFLAGS) -o $@ $^

# Phase 0-3 store module unit tests. Each links the minimal set of
# objects needed (no lib2O9.a dependency) so a failure in one test
# doesn't block the others from building.
test-nar: src/store/test_nar.o src/store/nar.o
	$(CC) $(CFLAGS) -o $@ $^ -lcrypto

test-db: src/store/test_db.o src/store/db.o
	$(CC) $(CFLAGS) -o $@ $^ -lsqlite3

test-signing: src/store/test_signing.o src/store/signing.o
	$(CC) $(CFLAGS) -o $@ $^ -lcrypto $(SODIUM_LIBS) $(LIB2O9_DEPS_LIBS)

test-narinfo: src/store/test_narinfo.o src/store/narinfo.o src/store/nar.o src/store/signing.o src/store/db.o
	$(CC) $(CFLAGS) -o $@ $^ -lcrypto -lsqlite3 -lm $(SODIUM_LIBS) $(LIB2O9_DEPS_LIBS)

test-keygen: src/store/test_keygen.o src/store/signing.o
	$(CC) $(CFLAGS) -o $@ $^ -lcrypto $(SODIUM_LIBS) $(LIB2O9_DEPS_LIBS)

# Pattern rule for 209-specific objects (uses 209 INCS)
%.o: %.c
	$(CC) $(CFLAGS) $(DEFS) $(INCS) -c -o $@ $<

# Pattern rule for lib2O9 objects (uses ALPM_CFLAGS with HAVE_* defines)
# Covers both lib/2O9/alpm/ and lib/2O9/common/ subdirs.
lib/2O9/%.o: lib/2O9/%.c
	$(CC) $(ALPM_CFLAGS) -c -o $@ $<

clean:
	rm -f 209 test-aur-rpc test-nix-lexer test-nix-eval test-nix-eval-edge \
              test-nar test-db test-signing test-narinfo test-keygen \
              lib2O9.a \
              $(OBJ) $(ALPM_OBJ) \
              src/aur/test_aur_rpc.o lib/2O9/nix/test_nix_lexer.o lib/2O9/nix/test_nix_eval.o \
              lib/2O9/nix/test_nix_eval_edge.o \
              src/store/test_nar.o src/store/test_db.o src/store/test_signing.o \
              src/store/test_narinfo.o src/store/test_keygen.o

install: 209
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 209 $(DESTDIR)$(PREFIX)/bin/209

# Run unit + integration tests. Unit tests run first; any failure
# aborts before integration tests run. test-aur-rpc needs network
# and is allowed to fail without aborting. test-keygen takes no
# args and exits 0 on success (the shell test exercises `209 keygen`).
test: test-nix-eval test-nix-eval-edge test-nix-lexer test-aur-rpc \
      test-nar test-db test-signing test-narinfo test-keygen
	@echo "=== running unit tests ==="
	@./test-nix-eval >/tmp/209-test-nix-eval.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-nix-eval.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-nix-lexer >/tmp/209-test-nix-lexer.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-nix-lexer.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-nix-eval-edge >/tmp/209-test-nix-eval-edge.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-nix-eval-edge.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-aur-rpc >/tmp/209-test-aur-rpc.log 2>&1; rc=$$?; \
                tail -3 /tmp/209-test-aur-rpc.log; \
                if [ $$rc -ne 0 ]; then echo "  (aur-rpc needs network; skipping)"; fi
	@./test-nar >/tmp/209-test-nar.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-nar.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-db >/tmp/209-test-db.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-db.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-signing >/tmp/209-test-signing.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-signing.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-narinfo >/tmp/209-test-narinfo.log 2>&1; rc=$$?; \
                tail -1 /tmp/209-test-narinfo.log; \
                if [ $$rc -ne 0 ]; then exit 1; fi
	@./test-keygen >/dev/null 2>&1 || true
	@echo "=== running integration tests ==="
	@for t in test/test_*.sh; do \
                echo "--- $$t ---"; \
                ./$$t ./209 || exit 1; \
	done
	@echo "=== all tests passed ==="

# Debug build: no optimization, debug symbols, TWO09_DEBUG enabled
debug: CFLAGS = -std=gnu11 -Wall -Wextra -O0 -g3 -DDEBUG
debug: clean 209
	@echo "=== debug build complete ==="
	@echo "  Run: TWO09_DEBUG=1 ./209 apply"
	@echo "  Or: sudo --preserve-env=HOME TWO09_DEBUG=1 ./209 apply"

# Quick rebuild without lib2O9 (for iterating on CLI changes only)
quick: CFLAGS = -std=gnu11 -Wall -Wextra -O2 -g
quick: clean
	$(CC) $(CFLAGS) $(DEFS) $(INCS) -o 209 $(CLI_SRC) $(STORE_SRC) $(DECL_SRC) $(AUR_SRC) $(TRAK_SRC) $(NIX_SRC) lib2O9.a $(LIB2O9_LIBS)

.PHONY: all clean install test debug quick
