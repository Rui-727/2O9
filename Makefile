# 2O9 — Makefile
# Build: make
# Install: make install PREFIX=/usr
# Clean: make clean

CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
PREFIX ?= /usr

# ── Version ────────────────────────────────────────────────────────
VERSION := $(shell git describe --abbrev=4 --dirty 2>/dev/null || echo 0.0.1)

# ── Defines ────────────────────────────────────────────────────────
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

# ── Includes ───────────────────────────────────────────────────────
INCS = -Isrc -Isrc/store -Isrc/declarative -Isrc/aur

# ── External libs ──────────────────────────────────────────────────
LIBS = -lcurl

# ── Sources ────────────────────────────────────────────────────────
CLI_SRC   = src/cli/main.c
STORE_SRC = src/store/store.c src/store/symlinks.c
DECL_SRC  = src/declarative/gen.c
AUR_SRC   = src/aur/aur_rpc.c src/aur/cJSON.c

SRC = $(CLI_SRC) $(STORE_SRC) $(DECL_SRC) $(AUR_SRC)

# ── Objects ────────────────────────────────────────────────────────
OBJ = $(SRC:.c=.o)

# ── Targets ────────────────────────────────────────────────────────
all: 209 test-aur-rpc

209: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

test-aur-rpc: src/aur/test_aur_rpc.o src/aur/aur_rpc.o src/aur/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -lcurl

%.o: %.c
	$(CC) $(CFLAGS) $(DEFS) $(INCS) -c -o $@ $<

clean:
	rm -f 209 test-aur-rpc $(OBJ) src/aur/test_aur_rpc.o

install: 209
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 209 $(DESTDIR)$(PREFIX)/bin/209

.PHONY: all clean install
