# Modifications to vendored pacman

This is pacman v6.0.0, copied into the 2O9 tree and edited in place.
Every change is marked `/* 2O9: <reason> */` so you can grep for what
we touched and why.

## The three modifications

1. **Install backend**: dispatch to the store adapter instead of
   libalpm's builtin extractor
2. **Installed-set query**: the solver reads from the generation DB,
   not `/var/lib/pacman/local/`
3. **Config entrypoint**: lib2O9 is configured programmatically from
   a manifest, never from `pacman.conf`

## Change log

### Applied: Modification 1, Install backend dispatch

**Files**: `lib/2O9/alpm/handle.h`, `lib/2O9/alpm/package.h`,
`lib/2O9/alpm/package.c`, `lib/2O9/alpm/add.c`

A function pointer `install_backend` is added to `alpm_handle_t` in
`handle.h`:

```c
/* 2O9: install backend dispatch, replaces direct extraction */
char *(*install_backend)(alpm_handle_t *handle, alpm_pkg_t *pkg,
                         const char *pkgfile_path);
```

In `add.c`'s `commit_single_pkg()`, before the per-file extraction
loop, the dispatch is checked. When `install_backend` is non-NULL,
libalpm:

1. Calls `handle->install_backend(handle, newpkg, pkgfile)`. The
   backend extracts the `.pkg.tar.zst` into
   `/nix/store/<base32-hash>-<name>-<version>/` and returns the store
   path as a malloc'd string.
2. Attaches the store path to the package via the new
   `alpm_pkg_t.two9_store_path` field (in `package.h`).
3. Jumps to `backend_done:`, skipping libalpm's local-DB write and
   `.install` scriptlet execution. Those are 2O9's responsibility
   (see `src/declarative/activation.c`).

When `install_backend` is NULL, pacman's default extraction is
preserved. This keeps the tree buildable and the modifications
auditable.

`_alpm_pkg_free()` in `package.c` was updated to free
`two9_store_path`.

**Marking**: Every changed line in `add.c`, `handle.h`, `package.h`,
and `package.c` has a `/* 2O9: ... */` comment.

### Applied: Modification 2, Installed-set query from generation DB

**Files**: `lib/2O9/alpm/handle.h`, `lib/2O9/alpm/be_local.c`

A function pointer `installed_set_loader` is added to `alpm_handle_t`:

```c
/* 2O9: installed-set source, replaces /var/lib/pacman/local/ read */
int (*installed_set_loader)(alpm_handle_t *handle, alpm_db_t *db);
```

In `be_local.c`'s `local_db_populate()`, before opening
`/var/lib/pacman/local/`, the dispatch is checked. When
`installed_set_loader` is non-NULL, libalpm calls it instead. The
callback is responsible for populating `db->pkgcache` with
`alpm_pkg_t` entries built from the 2O9 generation DB
(`/var/lib/2O9/generations/<N>/manifest.json`).

The callback builds the same in-memory `alpm_pkg_t` structures libalpm
expects (name, version, depends, conflicts, provides, files, etc.),
just sourced from a different place. No changes to the solver itself.

When `installed_set_loader` is NULL, pacman's default
`/var/lib/pacman/local/` read is preserved.

**Marking**: The dispatch block in `be_local.c` has a
`/* 2O9: ... */` comment.

### Applied: Modification 3, Programmatic config entrypoint

**Files**: `lib/2O9/alpm/two9_init.c`, `lib/2O9/alpm/two9_init.h` (new)

A new file `two9_init.c` provides the 2O9-specific entrypoint:

```c
alpm_handle_t *two9_alpm_init_from_manifest(const char *manifest_json);
void two9_alpm_register_backends(alpm_handle_t *handle,
                                 char *(*install_backend)(...),
                                 int (*installed_set_loader)(...));
```

`two9_alpm_init_from_manifest()` calls
`alpm_initialize("/", "/var/lib/2O9", &err)`, then programmatically
sets cache dirs, architectures, parallel downloads, SigLevel, and
registers sync DBs from the manifest's `pacman.repos` block. It never
reads `/etc/pacman.conf`. The manifest (output of evaluating
`2O9.nix`) is the single source of truth.

`SigLevel` parsing mirrors pacman's `parseSigLevel` from
`lib/libalpm/handle.c`. Tokens: `Package`, `PackageOptional`,
`PackageRequired`, `Database`, `DatabaseOptional`, `DatabaseRequired`,
`Required`, `Optional`, `Never`, `Default`, etc. Leading `~` unsets a
bit. The parsed level is applied via
`alpm_option_set_default_siglevel` +
`alpm_option_set_local_file_siglevel` +
`alpm_option_set_remote_file_siglevel`, AND passed as the third arg
to `alpm_register_syncdb` (was hardcoded to 0 before Phase 0).

`two9_alpm_register_backends()` wires the `install_backend` and
`installed_set_loader` callbacks onto an existing handle, activating
2O9 mode. After this call, modifications #1 and #2 take effect.

The manifest JSON is parsed with cJSON, which is vendored at
`lib/2O9/common/cJSON.{c,h}`.

**Marking**: The new file is wholly 2O9 code. No `/* 2O9: */` markers
needed in vendored source for this modification.

### Applied: Transaction wiring in `cmd_install`

**Files**: `src/cli/main.c` (the 2O9 CLI, not vendored libalpm)

The CLI now calls `alpm_trans_init(handle, ALPM_TRANS_FLAG_NOLOCK)`
plus `alpm_add_pkg(handle, pkg)` plus `alpm_trans_prepare(handle,
&data)` before doing any download. This revives the libalpm solver
(dependency resolution, file-conflict detection, cycle detection,
architecture check) without calling `alpm_trans_commit`.

Why not `alpm_trans_commit`? The commit callback contract is wired to
libalpm's own extraction path (it expects to extract to `/usr/bin/`
etc.). 2O9 extracts to `/nix/store/<hash>-<name>-<version>/` instead.
Bridging that requires overriding `_alpm_sync_commit` /
`commit_single_pkg`, which is a later-phase task. For now, we walk
`alpm_trans_get_add()` ourselves (topologically sorted), download each
target, and call `store_add_named()`.

The `ALPM_TRANS_FLAG_NOLOCK` flag is used because 2O9 takes its own
`flock()` on the generation DB, and `alpm_trans_commit` would assert
that `NOLOCK` isn't set. We sidestep that by not calling commit.

**Marking**: Documented in a multi-line comment in `cmd_install`.

### Applied: Package signature verification

**Files**: `src/cli/main.c`, `lib/2O9/alpm/two9_init.c`

When the manifest's `SigLevel` includes `ALPM_SIG_PACKAGE`, the CLI
fetches `<filename>.sig` alongside `<filename>` from the mirror. If
the siglevel requires signatures and the `.sig` fetch fails, the
install is refused.

Verification: `alpm_pkg_load(handle, cache_path, 0, pkg_siglevel,
&loaded)` reads the `.sig` file and calls
`_alpm_check_pgp_helper()` (in `lib/2O9/alpm/signing.c`) which calls
gpgme against the trusted keyring. On failure, both `.pkg` and `.sig`
are unlinked and the transaction aborts.

For DB sync (`209 sync`): no extra code needed. `alpm_db_update()`
verifies `.db.sig` automatically now that `alpm_register_syncdb` got
the real siglevel.

**Marking**: Documented in `cmd_install` and `two9_init.c`.

## What "no fork" still means

- We are **not** publishing a competing pacman binary or maintaining
  pacman as a separate project. lib2O9 is an internal build artifact
  of 2O9, consumed only by 2O9.
- The vendored pacman source advances upstream independently. We
  re-pull with `git subtree pull` and resolve conflicts in our
  modified tree. Conflicts are contained to the modification targets
  above: `add.c` (install backend), `be_local.c` (installed-set
  query), `handle.h` and `package.h` (struct fields).
  `two9_init.c` is wholly ours and never conflicts.

## Build status

lib2O9 is built and linked. The `209` binary is fully functional
against the modified libalpm. Building lib2O9 requires:

- `libarchive-dev` (for `.pkg.tar.zst` extraction in `add.c`,
  `be_package.c`)
- `openssl-dev` (for SHA-256, signature verification in `signing.c`,
  Ed25519 fallback in `src/store/signing.c`)
- `libgpgme-dev` (for GPG signature verification in `signing.c`)
- `libcurl-dev` (already linked for AUR; libalpm uses it for downloads)
- `libsqlite3-dev` (for `src/store/db.c`, the refs graph)
- Optional: `libsodium-dev` (preferred for Ed25519 in
  `src/store/signing.c`; falls back to OpenSSL if not installed)

The Makefile's `lib2O9.a` target produces the static library from all
of `lib/2O9/alpm/*.c` plus `lib/2O9/common/*.c`. The `209` binary
links it alongside the 2O9-specific sources in `src/`.

To build lib2O9 manually (when deps are available):

```sh
cd lib/2O9/alpm/
cc -c -I. -DHAVE_LIBCURL -DHAVE_LIBARCHIVE -DHAVE_LIBGPGME -DHAVE_LIBSSL \
   -D_GNU_SOURCE *.c
ar rcs lib2O9.a *.o
```

Then link `lib2O9.a` into the `209` binary via `LIBS` in the
top-level Makefile (already wired).
