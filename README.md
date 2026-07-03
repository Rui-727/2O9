# 2O9

Its either 2O9 or Pacman. your choice! its not my rights to judge people.

2O9 is a package manager for Arch Linux that puts files in `/nix/store/`
instead of `/`. The binary is `209`. It takes three tools that normally
don't talk to each other (pacman, paru, and Nix) and makes them one:

1. **pacman's engine** (libalpm), copied into the tree and modified
   directly as **lib2O9**. All the good parts: dependency resolution,
   repo sync, database parsing, hooks. The solver reads from the
   generation DB instead of `/var/lib/pacman/local/`, and the install
   backend dispatches to the store adapter. See
   `lib/2O9/alpm/MODIFICATIONS.md` for what we changed.
2. **paru's AUR workflow**, rewritten in C. AUR RPC, PKGBUILD clone
   and review, recursive dependency resolution, makepkg orchestration.
3. **A real `/nix/store`** with predictable paths
   (`/nix/store/<name>-<version>/`, no content hash) and atomic
   generations. Everything is driven by a Nix-syntax config file
   (`2O9.nix`) that declares what your system should look like. The
   Nix evaluator is **written from scratch in C** as part of lib2O9,
   not a vendored copy of the C++ nix source. It does the function
   form (`{ config, ... }: ...`) with fixed-point recursion for
   self-reference, plus `import` for splitting configs across files.

There's also **Trakker**, a ptrace sandbox for running untrusted
commands with syscall tracing, network blocking, and write redirection.
And **Debag**, a hybrid sandbox that uses seccomp for the fast path and
ptrace only for syscalls that need argument inspection, so the target
runs at about 90% native speed instead of 20%.

Rollback is a symlink swap plus a reboot. No boot-time rollback
machinery, no daemon, no service manager. You switch generations,
reboot, and systemd starts whatever's enabled. That's it.

## The package repo is always Arch Linux

The repository is always an Arch Linux mirror, configured in `2O9.nix`
under `pacman.repos`. No custom repos, no Nix binary caches for
packages, no alternative sources. We just put the files in
`/nix/store/` instead of `/`.

## Naming

| | Form | Where |
|---|---|---|
| Binary / command | `209` (numeric) | what you type |
| Project / branding | `2O9` (letter O) | docs, repo name, on-disk paths (`/etc/2O9/`, `/var/lib/2O9/`) |
| Merged internal library | `lib2O9` | static `lib2O9.a` - modified libalpm + own C Nix evaluator |

## Build

```sh
make                            # builds 209 + 3 test binaries
make install                    # installs 209 to /usr/bin
make install PREFIX=/usr/local  # custom prefix
make clean                      # remove build artifacts
```

`make` builds four targets:

| Target | What it is |
|---|---|
| `209` | The CLI binary - SOV dispatch, store adapter, generation DB, AUR pipeline, Trakker, Nix evaluator |
| `test-aur-rpc` | AUR RPC client unit tests (libcurl + cJSON) |
| `test-nix-lexer` | Nix lexer unit tests |
| `test-nix-eval` | Nix evaluator unit tests |

**Real dependencies today:** a C compiler, `make`, `libcurl-dev`,
`libarchive-dev`, `openssl-dev`, `libgpgme-dev`, and `libxml2-dev`.
lib2O9 (the modified libalpm + own C Nix evaluator + cJSON) is built
into `lib2O9.a` and linked into the `209` binary. The Makefile's
`LIB2O9_DEPS_PREFIX` variable lets you point at a user-local install
of the dev headers (e.g. `~/local/`) when system-wide install isn't
available.

**Running tests:**
```sh
make test           # unit tests (nix-eval, nix-lexer, aur-rpc) + integration tests
make test-nix-eval  # just the Nix evaluator unit tests (49 tests)
```

## Repo structure

```
2O9/
├── Makefile                    # build system - builds 209 + 3 test binaries
├── DESIGN.md                   # full architecture doc (950 lines)
├── LICENSE                     # GPL-2.0-only
├── README.md                   # this file
├── lib/2O9/                    # lib2O9 - own C Nix evaluator + (future) modified libalpm
│   ├── nix/                    #   Nix evaluator, written from scratch in C
│   │   ├── nix_lexer.c         #   416 LOC - tokenizer
│   │   ├── nix_parser.c        # ~1,060 LOC - recursive-descent parser → AST
│   │   ├── nix_eval.c          # ~2,410 LOC - evaluator + 19 builtins + AST clone + JSON emit
│   │   ├── nix_eval.h          #   public API
│   │   ├── README.md           #   evaluator design notes
│   │   ├── test_nix_lexer.c    #   lexer unit tests
│   │   └── test_nix_eval.c     #   evaluator unit tests (49 tests)
│   ├── alpm/                   #   modified libalpm (Phase 1 - all 3 mods applied + built)
│   │   ├── MODIFICATIONS.md    #   log of 3 applied modifications
│   │   ├── two9_init.c         #   2O9 programmatic config entrypoint (MOD #3)
│   │   └── two9_init.h
│   └── common/                 #   shared utils + vendored cJSON
│       ├── cJSON.c, cJSON.h   #   vendored JSON library (moved from src/aur/)
│       ├── ini.c, ini.h
│       └── util-common.c, util-common.h
├── src/
│   ├── cli/main.c              # 209 binary - SOV command dispatch
│   ├── aur/                    # AUR helper (paru ported to C)
│   │   ├── aur_rpc.c           # AUR RPC client (libcurl)
│   │   ├── aur_build.c         # PKGBUILD clone + makepkg orchestration
│   │   ├── aur_resolve.c       # recursive AUR dependency resolver
│   │   ├── build.h, resolver.h, aur_rpc.h
│   │   └── test_aur_rpc.c      # RPC unit tests
│   ├── declarative/            # generation DB + reconcile engine + activation
│   │   ├── gen.c, gen.h        # generation DB (file-based, /var/lib/2O9/generations/N/manifest.json)
│   │   ├── reconcile.c, reconcile.h  # diff manifest ↔ current gen → transaction
│   │   └── activation.c, activation.h  # 9-step post-extract activation phase (Phase 5 skeleton)
│   ├── store/                  # store adapter + symlink farm
│   │   ├── store.c, store.h    # pkg.tar.zst → /nix/store/<name>-<version>/
│   │   └── symlinks.c, symlinks.h  # ~/.local/bin / ~/.local/lib / /etc symlink farm
│   └── trakker/                # ptrace-based execution sandbox
│       ├── trakker.c, trakker.h  # policy + event recording + JSON trace output
├── scripts/
│   └── test_aur_mock.sh        # mock AUR RPC server for tests
├── test/                       # integration test suite (skeleton - see test/README.md)
├── docs/                       # extended documentation (skeleton - see docs/README.md)
├── test-nix-lexer              # built test binary (gitignored)
└── test-nix-eval               # built test binary (gitignored)
```

## Quick start

First, create a config:

```sh
209 init                    # creates ~/.config/2O9/home.nix
```

Edit the config to list what you want, then apply it:

```sh
209 -Sy                     # download repo databases (same as: 209 sync)
sudo 209 apply              # make the system match your config
```

Day-to-day use - pacman flags work too:

```sh
209 -S neovim               # install (same as: 209 neovim install)
209 -R htop                 # remove (same as: 209 htop remove)
209 -Q                      # list all installed packages
209 -Qs vim                 # search installed packages
209 -Qi neovim              # show package info
209 -Ql neovim              # list files in a package
209 -Qm                     # list foreign (AUR) packages
209 -Ss ffmpeg              # search repos
```

Build from the AUR:

```sh
209 yt-dlp aur build        # build from AUR (makepkg + store add)
209 ffmpeg aur search       # search AUR
209 ffmpeg aur review       # review PKGBUILD diff before building
```

Generations and rollback:

```sh
209 generations             # list all generations
209 3 rollback              # go back to generation 3
209 3 pin                   # protect it from GC
```

Trakker (sandbox - command resolved via $PATH):

```sh
209 trakker ls -la
209 trakker --no-net -- curl https://example.com
209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f
```

Testing the pipeline without a real /nix/store:

```sh
# End-to-end test with a fake store path
TWO09_TEST_MODE=1 209 sl install

# Or feed it a real .pkg.tar.zst
TWO09_PKG_PATH=/var/cache/pacman/pkg/neovim-0.10.0-1-x86_64.pkg.tar.zst \
  209 neovim install
```

## Architecture

Six pieces, layered. The CLI dispatches into either the declarative
engine or the AUR helper; both produce transactions that flow through
lib2O9 (the modified libalpm + Nix evaluator) into the store adapter,
which writes to `/nix/store` and rebuilds the symlink farm. Trakker
sits beside all of it as an on-demand sandbox. The full diagram is in
[`DESIGN.md`](./DESIGN.md) §4.

1. **CLI** - the entrypoint. SOV dispatch (`209 <subject> <verb>`) for
   package ops, leading-form for trakker (`209 trakker ls -la`).
2. **Declarative Engine** - turns `2O9.nix` into a transaction:
   evaluate, reconcile against the current generation, produce an
   install/remove plan.
3. **AUR Helper** - paru ported to C. RPC, PKGBUILD clone, review
   diff, recursive dep resolution, makepkg, into the store.
4. **Trakker** - ptrace sandbox. Records what a command does, blocks
   what you tell it to.
5. **lib2O9** - modified libalpm (solver reads from the generation
   DB, install backend dispatches to the store adapter) plus our own
   C Nix evaluator. One static library.
6. **Store Adapter** - puts packages in `/nix/store/`, then symlinks
   them into `~/.local/bin`, `~/.local/lib`, and `/etc/`.

## Configuration

There is no `config.toml`, no `paru.conf`, no `pacman.conf`. One file format
for everything - Nix. Two scopes:

| Scope | Config file | Profile symlink | Generation DB |
|---|---|---|---|
| **Global (system)** | `/etc/2O9/2O9.nix` | `/nix/var/nix/profiles/per-user/2O9-system` | `/var/lib/2O9` |
| **Per-user** | `~/.config/2O9/home.nix` | `~/.local/state/2O9/profile` | `~/.local/state/2O9` |

Merge order, lowest to highest precedence:

1. Built-in defaults (compiled into `209`)
2. `~/.config/2O9/home.nix`
3. `/etc/2O9/2O9.nix`
4. CLI flags

`~/.local/bin` is in `$PATH`. That is the visibility mechanism - binaries from
the store are symlinked there. Libraries go to `~/.local/lib/`. Config files
stay at their real paths: `/etc/` is `/etc/`, never `~/.local/etc/`.

Services are managed with `systemctl enable`/`disable` - there is no 2O9
service manager. After `209 apply` changes which systemd units are visible in
the store, the user reboots (or manually runs `systemctl daemon-reload` and
starts/stops the relevant units) for the new state to take full effect. This
is simple and correct: systemd starts exactly what is enabled.

## Commands

2O9 supports two CLI styles. Pacman flags (`-S`, `-R`, `-Q`) for muscle
memory, and 2O9's own Subject-Object-Verb order for the new stuff.
Both work; pick whichever feels right.

### Pacman-compatible flags

| Flag | Meaning | 2O9 equivalent |
|---|---|---|
| `209 -S <pkg>...` | Install | `209 <pkg> install` |
| `209 -Sy` | Refresh repo DBs | `209 sync` |
| `209 -Su` | Upgrade all | `209 apply` (not yet wired) |
| `209 -Ss <term>` | Search repos | `209 <term> search` |
| `209 -Si <pkg>` | Package info | `209 <pkg> info` |
| `209 -R <pkg>...` | Remove | `209 <pkg> remove` |
| `209 -Q` | List all installed | - |
| `209 -Qs <term>` | Search installed | `209 <term> search` |
| `209 -Qi <pkg>` | Installed info | `209 <pkg> info` |
| `209 -Ql <pkg>` | List files in package | - |
| `209 -Qm` | List foreign (AUR) packages | - |

### 2O9 commands

| Command | Meaning | Origin |
|---|---|---|
| `209 <pkg> install` | Install a package temporarily (not declared in `2O9.nix`) | pacman |
| `209 <pkg> remove` | Remove a package - new generation without it, rebuild symlink farm | pacman |
| `209 <pkg> info` | Show package info | pacman |
| `209 <term> search` | Search repos | pacman |
| `209 <pkg> aur build` | Build from AUR | paru |
| `209 <term> aur search` | Search AUR | paru |
| `209 <pkg> aur review` | Review PKGBUILD diff | paru |
| `209 trakker [flags] [--] <cmd> [args...]` | Run a command in the sandbox (PATH-resolved) | new |
| `209 apply` | Apply declarative config (`2O9.nix`) | new |
| `209 <n> rollback` | Roll back to generation #n | new |
| `209 <n> pin` | Pin a generation (protect from GC) | new |
| `209 generations` | List generations | new |
| `209 gc` | Garbage-collect unreferenced store paths | new |
| `209 sync` | Sync repo databases | pacman |
| `209 news` | Show Arch Linux news | paru |

**Special subjects:** `apply`, `generations`, `sync`, `news`, `gc` are
zero-argument commands - they have no subject, only a verb. They operate on
the system as a whole, not on a named thing.

**Multi-subject:** `209 nginx firefox install` installs both. The verb comes
last, applied to everything before it.

## Trakker and Debag

**Trakker** is a ptrace sandbox. It runs a command, records everything
it does, and optionally restricts it. The command is resolved via
`$PATH`, so bare names work:

```sh
209 trakker ls -la
209 trakker --no-net -- curl https://example.com
209 trakker --no-write --redirect-writes /tmp/trakker -- makepkg -f
```

Use `--` to separate trakker's flags from the command's own args.
Trakker uses ptrace to intercept syscalls and records file I/O, network
connections, process forks/execs, and mmap activity. The trace comes
out as JSON.

**Debag** is the fast version. It uses seccomp-bpf for the fast path
(most syscalls are allowed directly in the kernel, nanosecond overhead)
and ptrace only for the dangerous ones that need argument inspection
(execve, connect, open, mount). The target runs at about 90% native
speed instead of 20% with pure ptrace. Debag also does static analysis:
it scans the ELF binary's symbol table to figure out what syscalls it
will probably use, then builds a seccomp filter from that.

```sh
209 debag --static-scan -- /bin/ls    # see what a binary does before running it
209 debag --no-net -- curl URL        # run with network blocked
209 debag --fast-mode -- ls -la       # seccomp only, fastest
```

Restriction flags (both Trakker and Debag):

| Flag | Effect |
|---|---|
| `--no-net` | Block all network access |
| `--no-write` | Block all file writes |
| `--redirect-writes <dir>` | Redirect writes into `<dir>` instead of their real paths |
| `--allow-net port=443` | Allow only the listed port(s) (repeatable) |

## Status

What works and what's left.

- **Phase 0 (Foundation): DONE.** Repo, Makefile, builds clean.

- **Phase 1 (Store adapter + lib2O9): DONE.** Store adapter, symlink
  farm, and generation DB all work. All three modifications to vendored
  libalpm are applied (see
  [`MODIFICATIONS.md`](./lib/2O9/alpm/MODIFICATIONS.md)):
  1. Install backend dispatches to the store adapter instead of
     extracting to `/`.
  2. Solver reads the installed set from the generation DB instead of
     `/var/lib/pacman/local/`.
  3. Config is programmatic from a JSON manifest, no `pacman.conf`.

  lib2O9 is built and linked (2.4 MB static library, 216 alpm symbols).
  `209 sync` calls the real `alpm_db_update()`. Build needs
  libarchive-dev, openssl-dev, libgpgme-dev, and libcurl-dev.

- **Phase 2 (AUR helper): DONE.** AUR RPC, PKGBUILD clone, review,
  recursive dep resolution, makepkg. Works with the mock server.

- **Phase 3 (Declarative engine): DONE.** The Nix evaluator handles
  everything `2O9.nix` needs: attrsets, lists, strings, let, if/else,
  lambdas (curried and formal), imports, fixed-point recursion for
  `{ config, ... }`, `inherit`, all 9 binary operator precedence
  levels, 19 builtins. 49/49 tests pass. `209 apply` evaluates
  `2O9.nix`, merges `home.nix` + `2O9.nix`, reconciles, commits.

- **Phase 4 (Trakker): DONE.** ptrace sandbox with all four restriction
  flags, JSON trace output.

- **Phase 5 (Polish): IN PROGRESS.** 9-step activation phase, `209 news`,
  `209 init`, `209 doctor`, `209 wiki`, `209 cache`, `209 fuzz`,
  `209 bundle`/`import`, `209 diff`, `209 why`, `209 lock`, `209 -Su`.
  Pacman flags (`-S`, `-R`, `-Q`, `-Qs`, `-Qi`, `-Ql`, `-Qm`). Color
  output. 10 integration tests. What's left: packaging, more tests.

## Roadmap

Phased roadmap with risk-first ordering is in [`DESIGN.md`](./DESIGN.md) §10.
**Phase 1 is now done** - lib2O9 is built, linked, and exercised. The
remaining work is **Phase 5 polish** (packaging, more integration tests,
full activation phase implementation). Phases 0, 1, 2, 3, and 4 are done.

## Honest risks

From [`DESIGN.md`](./DESIGN.md) §11.

- **Non-derivation reproducibility.** Arch `.pkg.tar.zst` are not Nix
  derivations - two builds of an AUR package may hash differently. We get
  content-addressing (same bytes → same path → dedup), not Nix's deep input
  purity. Real derivations are out of scope.
- **In-tree drift.** The copied pacman advances upstream; we re-pull with
  `git subtree pull` and resolve conflicts in our modified tree. Contained to
  the modification targets in `MODIFICATIONS.md`, but real maintenance.
- **Hooks & install scripts.** pacman's `.install` scripts run post-install
  actions. 2O9's approach is to not run them at all - instead extract the
  intent (systemd units, tmpfiles, sysusers) and execute it through an
  idempotent activation phase. The ~10 common patterns cover most packages;
  unusual scripts get a warning.
- **Symlink conflicts.** Two packages shipping the same path is a conflict in
  the symlink farm. The store makes it visible at generation-commit time
  rather than install time, which is better, but still needs a resolution
  strategy.
- **Services after rollback.** On rollback, symlinks change but running
  processes keep their old binaries via open FDs. 2O9 does not restart
  services after a generation switch - the user reboots. No live rollback of
  services without a reboot.
- **Scope realism.** "Full Nix store on pacman" is a multi-year, team-scale
  effort. The plan is structured so each phase produces something useful on
  its own.

## License

GPL-2.0-only. See [`LICENSE`](./LICENSE).
