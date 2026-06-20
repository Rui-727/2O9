# 2O9 Modifications to vendored pacman

This is pacman v6.0.0, copied into the 2O9 tree and modified directly.
Every change is marked with `/* 2O9: <reason> */` in the source.

## Modification targets

1. **Install backend**: dispatch to store adapter instead of libalpm's extractor
2. **Installed-set query**: solver reads from generation DB, not /var/lib/pacman/local/
3. **Config entrypoint**: configured programmatically, never from pacman.conf

## Change log

### Planned: Modification 1 — Install backend dispatch

**File**: `lib/libalpm/add.c` — `_alpm_extract_single_file()`

Currently, `_alpm_extract_single_file()` extracts files directly to
`handle->root + entryname` (e.g. `/usr/bin/nvim`). We replace this
with a dispatch to the 2O9 store adapter, which:

1. Extracts the .pkg.tar.zst into `/nix/store/<hash>-<name>-<ver>/`
2. Returns the store path
3. The symlink farm builder then creates `~/.local/bin/nvim → /nix/store/.../bin/nvim`

**Approach**: Add a function pointer to `alpm_handle_t`:
```c
/* 2O9: install backend dispatch — replaces direct extraction */
int (*install_backend)(alpm_handle_t *handle, alpm_pkg_t *pkg,
                       const char *store_path);
```

When this is set (non-NULL), `_alpm_extract_single_file()` skips its
normal extraction and calls the backend instead. When NULL, pacman's
default behavior is preserved — this keeps the tree buildable without
2O9 changes.

**Marking**: Every changed line in add.c gets `/* 2O9: dispatch to store adapter */`

### Planned: Modification 2 — Installed-set query from generation DB

**File**: `lib/libalpm/be_local.c` — `local_db_read()`

Currently, `local_db_read()` reads package info from
`/var/lib/pacman/local/<pkgname>-<pkgver>/desc` (and other files).
We replace this with a read from the 2O9 generation DB.

**Approach**: Override the `dbe_ops` struct for the local database.
Instead of reading from the filesystem layout pacman expects, we
provide a custom backend that:

1. Reads the current generation's manifest
2. Populates `alpm_pkg_t` from the manifest data
3. Returns the same in-memory structure libalpm expects

This way the solver sees a coherent installed set — it just comes from
a different place. No changes to the solver itself.

**Marking**: `/* 2O9: read installed-set from generation DB */`

### Planned: Modification 3 — Programmatic config entrypoint

**File**: `lib/libalpm/handle.c` + `src/pacman/conf.c`

Currently, pacman reads its config from `/etc/pacman.conf` via
`_alpm_handle_new()` + `alpm_option_set_*()`. The CLI `pacman-conf.c`
parses the file and calls the setter API.

We remove the file-reading path entirely. Instead, 2O9's own CLI
constructs the handle programmatically from the 2O9.nix manifest:

```c
alpm_handle_t *handle = alpm_initialize("/", "/var/lib/2O9", &err);
alpm_option_set_arch(handle, "x86_64");
alpm_option_add_cachedir(handle, "/var/cache/2O9/pkg");
/* ... repo registration from manifest ... */
```

The `alpm_option_set` / `alpm_db_register_sync` API already supports
this — we just remove the config-file path and make it impossible to
accidentally read pacman.conf.

**Marking**: `/* 2O9: no pacman.conf, configured programmatically */`

## Status

No modifications applied yet. Phase 1 will produce the first changes.
The 209 binary currently operates independently of lib209 — using the
store adapter and generation DB directly.
