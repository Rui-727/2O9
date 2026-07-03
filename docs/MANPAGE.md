# 209(1) - 2O9 package manager

## SYNOPSIS

`209` [options] `<subject>` `<verb>`
`209` [options] `<command>`

## DESCRIPTION

**2O9** is a unified package manager for Arch Linux that puts files in
`/nix/store/`. It combines three things into one tool:

1. **pacman's engine** (libalpm), modified in-tree as part of lib2O9
2. **paru's AUR workflow**, rewritten in C
3. **A real `/nix/store`** with content-addressed paths, atomic
   generations, and a references graph, driven by a declarative
   Nix-syntax configuration (`2O9.nix`)

Plus **Trakker** (ptrace sandbox) and **Debag** (seccomp + ptrace hybrid
sandbox).

The binary is `209` (numeric). The project name is `2O9` (letter O).

## COMMANDS

### Pacman-compatible flags

2O9 supports pacman's common operation flags so muscle memory transfers.
Each maps to the equivalent 2O9 command.

`209 -S` `<pkg>` `[...]`
: Install package(s). Same as `209 <pkg> install`.

`209 -Sy`
: Refresh repo databases. Same as `209 sync`.

`209 -Su`
: Upgrade all packages. Evaluates `2O9.nix`, computes what's outdated
  in the current generation, downloads new versions from configured
  caches or the Arch mirror, extracts, commits a new generation.
  Requires root (writes to `/nix/store/`).

`209 -Ss` `<term>`
: Search repos. Same as `209 <term> search`.

`209 -Si` `<pkg>`
: Show package info. Same as `209 <pkg> info`.

`209 -R` `<pkg>` `[...]`
: Remove package(s). Same as `209 <pkg> remove`.

`209 -Q`
: List all installed packages (name + version, one per line).

`209 -Qs` `<term>`
: Search installed packages by substring.

`209 -Qi` `<pkg>`
: Show info for an installed package.

`209 -Ql` `<pkg>`
: List files in an installed package (from its store path).

`209 -Qm`
: List foreign (AUR) packages, installed packages with `origin: "aur"`.

### 2O9 commands

`209 apply`
: Evaluate `2O9.nix`, reconcile against the current generation, commit
  a new generation if the manifest changed.

`209 generations`
: List all generations. The current one is marked with `← current`.
  Shows a change summary (`+N -N ~N`) per generation from the diff.

`209 sync`
: Download repo databases. Same as `-Sy`. When a `2O9.nix` config
  exists, uses lib2O9 (`alpm_db_update`). Otherwise falls back to
  direct libcurl download of default Arch mirrors.

`209 gc` [`--optimise`]
: Garbage-collect store paths not reachable from any generation's root
  set. Walks the references graph (SQLite) to compute the closure, so
  dependencies of installed packages are preserved even if no
  generation explicitly references them. With `--optimise`, also runs
  hardlink dedup on the surviving store paths.

`209 optimise`
: Walk `/nix/store/`, SHA-256 every regular file, hardlink identical
  files into `/nix/store/.links/<sha256>`. Saves disk when multiple
  packages ship the same file (locale data, license text, etc).

`209 news`
: Fetch and display the latest Arch Linux news from the RSS feed.

`209 init` [`--system`]
: Create a starter `2O9.nix` in `~/.config/2O9/` (or `/etc/2O9/` with
  `--system`). Refuses to overwrite an existing file.

### SOV patterns (Subject-Object-Verb)

`209 <pkg> install`
: Install a package temporarily. It lands in the current generation
  but is not declared in `2O9.nix`. The next `209 apply` will flag it
  for removal unless you add it to the config first.

`209 <pkg> remove`
: Remove a package. Commits a new generation without it and rebuilds
  the symlink farm.

`209 <pkg> info`
: Show info about an installed package. Falls back to AUR info if the
  package isn't installed locally.

`209 <term> search`
: Search installed packages by substring. Falls back to AUR search if
  no local matches.

`209 <pkg> aur build`
: Build a package from the AUR. Resolves deps, clones the PKGBUILD,
  imports PGP keys if `validpgpkeys` is set, runs makepkg inside a
  chroot (on by default, use `--no-chroot` to disable), installs the
  result into `/nix/store/`.

`209 <term> aur search`
: Search the AUR.

`209 <pkg> aur info`
: Show AUR package info.

`209 <pkg> aur review`
: Clone the PKGBUILD and show a diff for review before building.

`209 trakker` [`--no-net`] [`--no-write`] [`--redirect-writes` `<dir>`] [`--allow-net` `port=<port>`] [`--`] `<cmd>` `[args...]`
: Run `<cmd>` inside the Trakker sandbox. The command is resolved via
  `$PATH`, so bare names work (`209 trakker ls -la`). Records file I/O,
  network connections, and process activity as a JSON trace. Use `--`
  to separate trakker flags from the command's own flags.

  Examples:
  - `209 trakker ls -la`
  - `209 trakker --no-net -- curl https://example.com`
  - `209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f`

`209 debag` [`--static-scan`] [`--no-net`] [`--fast-mode`] [`--`] `<cmd>` `[args...]`
: Run `<cmd>` inside the Debag hybrid sandbox. Uses seccomp-bpf for
  the fast path (most syscalls allowed directly) and ptrace only for
  syscalls that need argument inspection. About 90% native speed. With
  `--static-scan`, scans the ELF symbol table first to build a tighter
  seccomp filter.

`209 <n> rollback`
: Roll back to generation #n. Repoints the current-generation symlink
  and rebuilds the symlink farm. A reboot is recommended for the full
  system state to take effect.

`209 <n> pin`
: Pin generation #n so garbage collection won't reap its store paths.

### Binary cache commands

`209 keygen`
: Generate a new Ed25519 keypair for signing narinfos. Prints the
  public key (to put in `2O9.conf` on subscriber machines) and a
  `KeyName:PublicKey:SecretKey` triple (to put on the publishing
  machine).

`209 cache push` `<store-path>`
: Upload a store path and its full closure (computed from the refs
  graph) to all configured substituters. For each path: compute the
  NAR hash, build a narinfo, sign it with the configured Ed25519 key,
  upload both. Idempotent.

`209 cache pull` `<store-path>`
: Explicitly fetch a store path from configured substituters. Normally
  not needed: the install path consults substituters automatically
  before falling through to the Arch mirror. Useful for debugging or
  for pre-populating a machine.

### Multi-subject

`209 <pkg1> <pkg2> ... <verb>`
: Apply the verb to all subjects. For example:
  `209 vim curl install` installs both.

## OPTIONS

`-V`, `--version`
: Print version and exit.

`-h`, `--help`
: Print help and exit.

## CONFIGURATION

Two config files, different jobs.

`2O9.nix` is the declarative config. Nix syntax. Says what your system
should look like. Two scopes:

- **User:** `~/.config/2O9/home.nix`
- **System:** `/etc/2O9/2O9.nix`

Both are evaluated and merged per `DESIGN.md` section 7: global wins on
conflict, packages concatenate. See [`docs/CONFIG.md`](./CONFIG.md) for
the full schema reference.

`2O9.conf` is the imperative side config. INI syntax. Holds stuff that
doesn't belong in the declarative config: substituter URLs, signing
keys, AUR build flags, chroot settings. Lives at `~/.config/2O9/2O9.conf`
(or `/etc/2O9/2O9.conf` for system-wide). See [`docs/CONFIG.md`](./CONFIG.md)
for the schema.

## FILES

`/etc/2O9/2O9.nix`
: System-wide declarative config.

`/etc/2O9/2O9.conf`
: System-wide imperative side config (substituters, signing keys).

`~/.config/2O9/home.nix`
: Per-user declarative config (overlaid on the system config).

`~/.config/2O9/2O9.conf`
: Per-user imperative side config.

`/var/lib/2O9/`
: System generation DB. One subdirectory per generation, each
  containing a `manifest.json` and a `diff.json`.

`/var/lib/2O9/store.sqlite`
: System SQLite store DB. Tables: `valid_paths`, `refs`. Used by GC
  to compute the closure of live paths.

`~/.local/state/2O9/`
: Per-user generation DB and store DB (same layout as the system DB).

`/nix/store/`
: The store. Package files live here as
  `/nix/store/<base32-hash>-<name>-<version>/`. The hash is computed
  from the NAR serialisation of the extracted tree.

`/nix/store/.links/`
: Hardlink pool used by `209 optimise`. Files are linked here by their
  SHA-256 hash, then store paths hardlink back to the pool.

`/nix/store/.tmp/`
: Staging directory for atomic extraction. 2O9 extracts here, then
  `rename()`s to the final store path. Hidden from normal store scans.

`~/.local/bin/`, `~/.local/lib/`
: Symlink farm targets. The shell's `$PATH` should include
  `~/.local/bin`.

## ENVIRONMENT

`HOME`
: Determines the user config and state directories.

`SUDO_USER`
: When 2O9 runs as root via sudo, this is used to resolve the original
  user's home directory. Ensures that `209 apply` (running as root)
  reads the same sync DB that `209 sync` (running as the user) wrote.

`TWO09_DEBUG`
: If set, prints extra debug output during install, apply, and store
  operations.

`TWO09_TEST_MODE`
: If set, uses fake store paths and skips real downloads. Used by the
  test suite.

`TWO09_PKG_PATH`
: If set, uses the given `.pkg.tar.zst` instead of downloading. Useful
  for testing extraction against a known package.

`LIB2O9_DEPS_PREFIX`
: Build-time only. Points the Makefile at a user-local install of
  libarchive-dev / libgpgme-dev / libsqlite3-dev headers when
  system-wide install isn't available.

## SEE ALSO

- [`README.md`](../README.md) - overview and quick start
- [`DESIGN.md`](../DESIGN.md) - full architecture
- [`docs/CONFIG.md`](./CONFIG.md) - `2O9.nix` and `2O9.conf` schema reference
- [`lib/2O9/nix/README.md`](../lib/2O9/nix/README.md) - Nix evaluator
- [`lib/2O9/alpm/MODIFICATIONS.md`](../lib/2O9/alpm/MODIFICATIONS.md) - libalpm mods

## AUTHOR

Rui-727 <https://github.com/Rui-727>

## LICENSE

GPL-2.0-only. Inherited from pacman, the engine we link against.
