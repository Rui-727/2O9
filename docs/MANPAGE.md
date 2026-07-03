# 209(1) - 2O9 package manager

## SYNOPSIS

`209` [options] `<subject>` `<verb>`
`209` [options] `<command>`

## DESCRIPTION

**2O9** is a unified package manager for Arch Linux that puts files in
`/nix/store/`. It combines three things into one tool:

1. **pacman's engine** - libalpm, modified in-tree as part of lib2O9
2. **paru's AUR workflow** - rewritten in C
3. **A real `/nix/store`** - with atomic generations, driven by a
   declarative Nix-syntax configuration (`2O9.nix`)

Plus **Trakker** - a ptrace-based execution sandbox.

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
: Upgrade all packages. Not yet implemented - use `209 apply`.

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
: List foreign (AUR) packages - installed packages with `origin: "aur"`.

### 2O9 commands

`209 apply`
: Evaluate `2O9.nix`, reconcile against the current generation, commit
  a new generation if the manifest changed.

`209 generations`
: List all generations. The current one is marked with `← current`.
  Shows a change summary (`+N -N ~N`) per generation from the diff.

`209 sync`
: Download repo databases. Same as `-Sy`. When a `2O9.nix` config exists,
  uses lib2O9 (`alpm_db_update`). Otherwise falls back to direct libcurl
  download of default Arch mirrors.

`209 gc`
: Garbage-collect store paths not referenced by any generation.

`209 news`
: Fetch and display the latest Arch Linux news from the RSS feed.

`209 init` [`--system`]
: Create a starter `2O9.nix` in `~/.config/2O9/` (or `/etc/2O9/` with
  `--system`). Refuses to overwrite an existing file.

### SOV patterns (Subject-Object-Verb)

`209 <pkg> install`
: Install a package temporarily. It lands in the current generation
  but is not declared in `2O9.nix` - the next `209 apply` will flag
  it for removal unless you add it to the config first.

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
  runs makepkg, installs the result into `/nix/store/`.

`209 <term> aur search`
: Search the AUR.

`209 <pkg> aur info`
: Show AUR package info.

`209 <pkg> aur review`
: Clone the PKGBUILD and show a diff for review before building.

`209 trakker` [`--no-net`] [`--no-write`] [`--redirect-writes` `<dir>`] [`--allow-net` `port=<port>`] [`--`] `<cmd>` `[args...]`
: Run `<cmd>` inside the Trakker sandbox. The command is resolved via
  `$PATH`, so bare names work (`209 trakker ls -la`). Records file I/O,
  network connections, and process activity as a JSON trace. Use `--` to
  separate trakker flags from the command's own flags.

  Examples:
 - `209 trakker ls -la`
 - `209 trakker --no-net -- curl https://example.com`
 - `209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f`

`209 <n> rollback`
: Roll back to generation #n. Repoints the current-generation symlink
  and rebuilds the symlink farm. A reboot is recommended for the full
  system state to take effect.

`209 <n> pin`
: Pin generation #n so garbage collection won't reap its store paths.

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

Configuration lives in `2O9.nix` - a Nix-syntax file. Two scopes:

- **User:** `~/.config/2O9/home.nix`
- **System:** `/etc/2O9/2O9.nix`

Both are evaluated and merged per `DESIGN.md` §7: global wins on
conflict, packages concatenate. See [`docs/CONFIG.md`](./CONFIG.md)
for the full schema reference.

## FILES

`/etc/2O9/2O9.nix`
: System-wide declarative config.

`~/.config/2O9/home.nix`
: Per-user declarative config (overlaid on the system config).

`/var/lib/2O9/`
: System generation DB - one subdirectory per generation, each
  containing a `manifest.json`.

`~/.local/state/2O9/`
: Per-user generation DB (same layout as the system DB).

`/nix/store/`
: The store. Package files live here as `/nix/store/<name>-<version>/`.

`~/.local/bin/`, `~/.local/lib/`
: Symlink farm targets. The shell's `$PATH` should include
`~/.local/bin`.

## ENVIRONMENT

`HOME`
: Determines the user config and state directories.

`LIB2O9_DEPS_PREFIX`
: Build-time only. Points the Makefile at a user-local install of
  libarchive-dev / libgpgme-dev headers when system-wide install
  isn't available.

## SEE ALSO

- [`README.md`](../README.md) - overview and quick start
- [`DESIGN.md`](../DESIGN.md) - full architecture (950 lines)
- [`docs/CONFIG.md`](./CONFIG.md) - `2O9.nix` schema reference
- [`lib/2O9/nix/README.md`](../lib/2O9/nix/README.md) - Nix evaluator
- [`lib/2O9/alpm/MODIFICATIONS.md`](../lib/2O9/alpm/MODIFICATIONS.md) - libalpm mods

## AUTHOR

Rui-727 <https://github.com/Rui-727>

## LICENSE

GPL-2.0-only. Inherited from pacman, the engine we link against.
