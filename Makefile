# 2O9 Makefile
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
PREFIX ?= /usr
VERSION := $(shell git describe --abbrev=4 --dirty 2>/dev/null || echo 0.0.1)

DEFS = -D_GNU_SOURCE		-DPACKAGE='"2O9"'		-DPACKAGE_VERSION='"$(VERSION)"'		-DSTORE_ROOT='"/nix/store"'		-DCONFIG_PATH='"/etc/2O9/2O9.nix"'		-DDBPATH='"/var/lib/2O9/"'		-DCACHEDIR='"/var/cache/2O9/pkg/"'		-DPROFILE_DIR='"/nix/var/nix/profiles/per-user/2O9-system"'		-DUSER_PROFILE_DIR='"/.local/state/2O9/profile"'		-DBIN_DIR='"/.local/bin"'		-DLIB_DIR='"/.local/lib"'

INCS = -Isrc -Isrc/store -Isrc/declarative -Isrc/aur -Ilib/2O9/nix
LIBS = -lcurl

CLI_SRC   = src/cli/main.c
STORE_SRC = src/store/store.c src/store/symlinks.c
DECL_SRC  = src/declarative/gen.c src/declarative/reconcile.c
AUR_SRC   = src/aur/aur_rpc.c src/aur/cJSON.c src/aur/aur_build.c src/aur/aur_resolve.c
NIX_SRC   = lib/2O9/nix/nix_eval.c lib/2O9/nix/nix_lexer.c lib/2O9/nix/nix_parser.c

SRC = $(CLI_SRC) $(STORE_SRC) $(DECL_SRC) $(AUR_SRC) $(NIX_SRC)
OBJ = $(SRC:.c=.o)

all: 209 test-aur-rpc test-nix-lexer test-nix-eval

209: $(OBJ)
		$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

test-aur-rpc: src/aur/test_aur_rpc.o src/aur/aur_rpc.o src/aur/cJSON.o
		$(CC) $(CFLAGS) -o $@ $^ -lcurl

test-nix-lexer: lib/2O9/nix/test_nix_lexer.o lib/2O9/nix/nix_lexer.o lib/2O9/nix/nix_eval.o lib/2O9/nix/nix_parser.o
		$(CC) $(CFLAGS) -o $@ $^

test-nix-eval: lib/2O9/nix/test_nix_eval.o lib/2O9/nix/nix_lexer.o lib/2O9/nix/nix_eval.o lib/2O9/nix/nix_parser.o
		$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
		$(CC) $(CFLAGS) $(DEFS) $(INCS) -c -o $@ $<

clean:
		rm -f 209 test-aur-rpc test-nix-lexer test-nix-eval $(OBJ) src/aur/test_aur_rpc.o lib/2O9/nix/test_nix_lexer.o lib/2O9/nix/test_nix_eval.o

install: 209
		install -d $(DESTDIR)$(PREFIX)/bin
		install -m 755 209 $(DESTDIR)$(PREFIX)/bin/209

.PHONY: all clean install
