# Modifications to vendored pacman

This is pacman v6.0.0, copied into the 2O9 tree and edited in place.
Every change is marked `/* 2O9: <reason> */` so you can grep for what
we touched and why.

## The three modifications

1. **Install backend** - dispatch to the store adapter instead of
   libalpm's builtin extractor
2. **Installed-set query** - the solver reads from the generation DB,
   not `/var/lib/pacman/local/`
3. **Config entrypoint** - lib2O9 is configured programmatically from
   a manifest, never from `pacman.conf`

## Change log

### Applied: Modification 1 - Install backend dispatch

**Files**: `lib/2O9/alpm/handle.h`, `lib/2O9/alpm/package.h`,
`lib/2O9/alpm/package.c`, `lib/2O9/alpm/add.c`

A function pointer `install_backend` is added to `alpm_handle_t` in
`handle.h`:

```c
/* 2O9: install backend dispatch - replaces direct extraction */
char *(*install_backend)(alpm_handle_t *handle, alpm_pkg_t *pkg,
                         const char *pkgfile_path);
```

In `add.c`'s `commit_single_pkg()`, before the per-file extraction loop,
the dispatch is checked. When `install_backend` is non-NULL, libalpm:

1. Calls `handle->install_backend(handle, newpkg, pkgfile)` - the backend
   extracts the .pkg.tar.zst into `/nix/store/<name>-<version>/` and
   returns the store path as a malloc'd string.
2. Attaches the store path to the package via the new
   `alpm_pkg_t.two9_store_path` field (in `package.h`).
3. Jumps to `backend_done:`, skipping libalpm's local-DB write and
   `.install` scriptlet execution (those are 2O9's responsibility - 
   see `src/declarative/activation.c`).

When `install_backend` is NULL, pacman's default extraction is preserved.
This keeps the tree buildable and the modifications auditable.

`_alpm_pkg_free()` in `package.c` was updated to free `two9_store_path`.

**Marking**: Every changed line in `add.c`, `handle.h`, `package.h`, and
`package.c` has a `/* 2O9: ... */` comment.

### Applied: Modification 2 - Installed-set query from generation DB

**Files**: `lib/2O9/alpm/handle.h`, `lib/2O9/alpm/be_local.c`

A function pointer `installed_set_loader` is added to `alpm_handle_t`:

```c
/* 2O9: installed-set source - replaces /var/lib/pacman/local/ read */
int (*installed_set_loader)(alpm_handle_t *handle, alpm_db_t *db);
```

In `be_local.c`'s `local_db_populate()`, before opening
`/var/lib/pacman/local/`, the dispatch is checked. When
`installed_set_loader` is non-NULL, libalpm calls it instead. The
callback is responsible for populating `db->pkgcache` with
`alpm_pkg_t` entries built from the 2O9 generation DB
(`/var/lib/2O9/generations/<N>/manifest.json`).

The callback builds the same in-memory `alpm_pkg_t` structures libalpm
expects - name, version, depends, conflicts, provides, files, etc. - 
just sourced from a different place. No changes to the solver itself.

When `installed_set_loader` is NULL, pacman's default `/var/lib/pacman/local/`
read is preserved.

**Marking**: The dispatch block in `be_local.c` has a `/* 2O9: ... */` comment.

### Applied: Modification 3 - Programmatic config entrypoint

**Files**: `lib/2O9/alpm/two9_init.c`, `lib/2O9/alpm/two9_init.h` (new)

A new file `two9_init.c` provides the 2O9-specific entrypoint:

```c
alpm_handle_t *two9_alpm_init_from_manifest(const char *manifest_json);
void two9_alpm_register_backends(alpm_handle_t *handle,
                                 char *(*install_backend)(...),
                                 int (*installed_set_loader)(...));
```

`two9_alpm_init_from_manifest()` calls `alpm_initialize("/", "/var/lib/2O9", &err)`,
then programmatically sets cache dirs, architectures, parallel downloads,
and registers sync DBs from the manifest's `pacman.repos` block. It never
reads `/etc/pacman.conf` - the manifest (output of evaluating `2O9.nix`)
is the single source of truth.

`two9_alpm_register_backends()` wires the install_backend and
installed_set_loader callbacks onto an existing handle, activating 2O9
mode. After this call, modifications #1 and #2 take effect.

The manifest JSON is parsed with cJSON, which is already vendored at
`src/aur/cJSON.{c,h}`. cJSON has no aur/ coupling - it's a generic JSON
library that just happens to live in that directory. We include it from
there rather than duplicating JSON parsing logic.

**Marking**: The new file is wholly 2O9 code; no `/* 2O9: */` markers
needed in vendored source for this modification.

## What "no fork" still means

- We are **not** publishing a competing pacman binary or maintaining pacman as a
  separate project. lib2O9 is an internal build artifact of 2O9, consumed only
  by 2O9.
- The vendored pacman source advances upstream independently; we re-pull with
  `git subtree pull` and resolve conflicts in our modified tree. Conflicts are
  contained to the modification targets above - `add.c` (install backend),
  `be_local.c` (installed-set query), and `handle.h`/`package.h` (struct fields).
  `two9_init.c` is wholly ours and never conflicts.

## Build status

The lib2O9 modifications are applied to the source tree but not yet built
into the `209` binary. Building lib2O9 requires:

- `libarchive-dev` (for .pkg.tar.zst extraction in `add.c`, `be_package.c`)
- `openssl-dev` (for signature verification in `signing.c`)
- Optional: `libgpgme-dev` (for GPG signature verification)
- Optional: `libcurl-dev` (already linked for AUR; libalpm uses it for downloads)

The Makefile's `lib2O9.a` target will be added once these deps are confirmed
available in the build environment. Until then, the `209` binary operates
independently of libalpm - using the store adapter and generation DB
directly. See `src/cli/main.c` comments.

To build lib2O9 manually (when deps are available):

```sh
cd lib/2O9/alpm/
cc -c -I. -DHAVE_LIBCURL -DHAVE_LIBARCHIVE -D_GNU_SOURCE *.c
ar rcs lib2O9.a *.o
```

Then link `lib2O9.a` into the `209` binary by adding it to `LIBS` in the
top-level Makefile.
