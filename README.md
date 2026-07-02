# 2O9

2O9 (the binary is `209`) is a package manager for Arch Linux that puts
files in `/nix/store/` instead of `/`. It takes three tools that
normally don't talk to each other — pacman, paru, and Nix — and makes
them one:

1. **pacman's engine**, libalpm, copied into the tree and modified
   directly as **lib2O9**. Dependency resolution, repo sync, database
   parsing, hooks — all the parts that make pacman good at its job.
   We point the solver at the generation DB instead of
   `/var/lib/pacman/local/`, and the install backend dispatches to the
   store adapter. See `lib/2O9/alpm/MODIFICATIONS.md` for the gory
   details.
2. **paru's AUR workflow**, rewritten in C. AUR RPC queries, PKGBUILD
   clone and review, recursive dependency resolution, makepkg
   orchestration.
3. **A real `/nix/store`**, with predictable paths
   (`/nix/store/<name>-<version>/` — no content hash) and atomic
   generations. The whole thing is driven by a Nix-syntax config file
   (`2O9.nix`) that declares what your system should look like. The
   Nix evaluator is **written from scratch in C** as part of lib2O9 —
   not a vendored copy of the C++ nix source. It handles the function
   form (`{ config, ... }: ...`) with fixed-point recursion for
   self-reference, and `import` for splitting configs across files.

Plus **Trakker** — a ptrace sandbox for running untrusted commands with
syscall tracing, network blocking, and write redirection. The trace
comes out as JSON.

Rollback is a symlink swap plus a reboot. There's no boot-time rollback
machinery, no daemon, no service manager. You switch generations, reboot,
and systemd starts exactly what's enabled. Simple and correct.

## The package repo is always Arch Linux

2O9 is an Arch Linux package manager. The repository is always an Arch
Linux mirror — configured in `2O9.nix` under `pacman.repos`. No custom
repos, no Nix binary caches for packages, no alternative sources. We
just put the files in `/nix/store/` instead of `/`.

## Naming

| | Form | Where |
|---|---|---|
| Binary / command | `209` (numeric) | what you type |
| Project / branding | `2O9` (letter O) | docs, repo name, on-disk paths (`/etc/2O9/`, `/var/lib/2O9/`) |
| Merged internal library | `lib2O9` | static `lib2O9.a` — modified libalpm + own C Nix evaluator |

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
| `209` | The CLI binary — SOV dispatch, store adapter, generation DB, AUR pipeline, Trakker, Nix evaluator |
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
├── Makefile                    # build system — builds 209 + 3 test binaries
├── DESIGN.md                   # full architecture doc (950 lines)
├── LICENSE                     # GPL-2.0-only
├── README.md                   # this file
├── lib/2O9/                    # lib2O9 — own C Nix evaluator + (future) modified libalpm
│   ├── nix/                    #   Nix evaluator, written from scratch in C
│   │   ├── nix_lexer.c         #   416 LOC — tokenizer
│   │   ├── nix_parser.c        # ~1,060 LOC — recursive-descent parser → AST
│   │   ├── nix_eval.c          # ~2,410 LOC — evaluator + 19 builtins + AST clone + JSON emit
│   │   ├── nix_eval.h          #   public API
│   │   ├── README.md           #   evaluator design notes
│   │   ├── test_nix_lexer.c    #   lexer unit tests
│   │   └── test_nix_eval.c     #   evaluator unit tests (49 tests)
│   ├── alpm/                   #   modified libalpm (Phase 1 — all 3 mods applied + built)
│   │   ├── MODIFICATIONS.md    #   log of 3 applied modifications
│   │   ├── two9_init.c         #   2O9 programmatic config entrypoint (MOD #3)
│   │   └── two9_init.h
│   └── common/                 #   shared utils + vendored cJSON
│       ├── cJSON.c, cJSON.h   #   vendored JSON library (moved from src/aur/)
│       ├── ini.c, ini.h
│       └── util-common.c, util-common.h
├── src/
│   ├── cli/main.c              # 209 binary — SOV command dispatch
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
├── test/                       # integration test suite (skeleton — see test/README.md)
├── docs/                       # extended documentation (skeleton — see docs/README.md)
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
209 sync                    # download repo databases
sudo 209 apply              # make the system match your config
```

Day-to-day use:

```sh
# Install a package temporarily (not in the config — next apply removes it)
209 neovim install

# Build something from the AUR
209 yt-dlp aur build

# Search installed packages, fall back to AUR if no local match
209 ffmpeg search

# Read the news
209 news

# List generations and roll back
209 generations
209 3 rollback              # go back to generation 3
209 3 pin                   # protect it from garbage collection

# Run an untrusted binary in the sandbox
209 weird-binary trakker --no-net --redirect-writes /tmp/trakker
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

Six components, layered top-down. The unified CLI dispatches into either the
declarative engine or the AUR helper; both produce transactions that flow
through lib2O9 (the modified libalpm + the Nix evaluator) into the store
adapter, which writes to `/nix/store` and updates the symlink farm. Trakker
sits beside the CLI as an execution sandbox invoked on demand. The full
component diagram is in [`DESIGN.md`](./DESIGN.md) §4.

1. **Unified CLI** — entrypoint. SOV dispatch: `209 <subject> <verb>`.
2. **Declarative Engine** — turns `2O9.nix` into a transaction: evaluate,
   reconcile against the current generation, produce an install/remove plan.
3. **AUR Helper** — paru ported to C. RPC, PKGBUILD clone, review diff,
   recursive dep resolution, makepkg orchestration, into the store.
4. **Trakker** — ptrace-based execution sandbox and trace recorder.
5. **lib2O9** — modified libalpm (solver reads from generation DB, install
   backend dispatches to store adapter) plus our own C Nix evaluator. Built
   into one static library.
6. **Store Adapter** — puts packages in `/nix/store/`, then symlinks them
   into `~/.local/bin`, `~/.local/lib`, and (for config files) `/etc/`.

## Configuration

There is no `config.toml`, no `paru.conf`, no `pacman.conf`. One file format
for everything — Nix. Two scopes:

| Scope | Config file | Profile symlink | Generation DB |
|---|---|---|---|
| **Global (system)** | `/etc/2O9/2O9.nix` | `/nix/var/nix/profiles/per-user/2O9-system` | `/var/lib/2O9` |
| **Per-user** | `~/.config/2O9/home.nix` | `~/.local/state/2O9/profile` | `~/.local/state/2O9` |

Merge order, lowest to highest precedence:

1. Built-in defaults (compiled into `209`)
2. `~/.config/2O9/home.nix`
3. `/etc/2O9/2O9.nix`
4. CLI flags

`~/.local/bin` is in `$PATH`. That is the visibility mechanism — binaries from
the store are symlinked there. Libraries go to `~/.local/lib/`. Config files
stay at their real paths: `/etc/` is `/etc/`, never `~/.local/etc/`.

Services are managed with `systemctl enable`/`disable` — there is no 2O9
service manager. After `209 apply` changes which systemd units are visible in
the store, the user reboots (or manually runs `systemctl daemon-reload` and
starts/stops the relevant units) for the new state to take full effect. This
is simple and correct: systemd starts exactly what is enabled.

## Commands

The CLI uses **Subject-Object-Verb** order. `209 nginx install` reads as
"nginx — install." The thing comes first, the action comes last. You already
know what you're talking about before you say what to do with it. Subject-first
is intent; verb-first is ceremony.

| Command | Meaning | Origin |
|---|---|---|
| `209 <pkg> install` | Install a package temporarily (not declared in `2O9.nix`) | pacman |
| `209 <pkg> remove` | Remove a package — new generation without it, rebuild symlink farm | pacman |
| `209 <pkg> info` | Show package info | pacman |
| `209 <term> search` | Search repos | pacman |
| `209 <pkg> aur build` | Build from AUR | paru |
| `209 <term> aur search` | Search AUR | paru |
| `209 <pkg> aur review` | Review PKGBUILD diff | paru |
| `209 <subject> trakker [flags]` | Run command in sandbox, record trace | new |
| `209 apply` | Apply declarative config (`2O9.nix`) | new |
| `209 <n> rollback` | Roll back to generation #n | new |
| `209 <n> pin` | Pin a generation (protect from GC) | new |
| `209 generations` | List generations | new |
| `209 gc` | Garbage-collect unreferenced store paths | new |
| `209 sync` | Sync repo databases | pacman |
| `209 news` | Show Arch Linux news | paru |

**Special subjects:** `apply`, `generations`, `sync`, `news`, `gc` are
zero-argument commands — they have no subject, only a verb. They operate on
the system as a whole, not on a named thing.

**Multi-subject:** `209 nginx firefox install` installs both. The verb comes
last, applied to everything before it.

## Trakker — execution sandbox

Trakker runs a command inside 2O9's sandbox, records everything it does, and
optionally restricts what it is allowed to do. It uses ptrace to intercept
syscalls. Recorded events: file I/O (read, write, create, delete), network
connections, process forks/execs/exits, and mmap summaries. Output is a JSON
trace log.

Restriction flags:

| Flag | Effect |
|---|---|
| `--no-net` | Block all network access |
| `--no-write` | Block all file writes |
| `--redirect-writes <dir>` | Redirect writes into `<dir>` instead of their real paths |
| `--allow-net port=443` | Allow only the listed port(s) (repeatable) |

```
209 untrusted-thing trakker --no-net --redirect-writes /tmp/trakker --allow-net port=443
```

## Status

What works, what doesn't, what's left.

- **Phase 0 — Foundation: DONE.** Repo, Makefile, four build targets.
  Nothing fancy, but it builds clean.

- **Phase 1 — Store adapter MVP: DONE.** The store adapter, symlink
  farm, and generation DB are all implemented. The three lib2O9
  modifications to vendored libalpm are applied to the source tree
  (see [`MODIFICATIONS.md`](./lib/2O9/alpm/MODIFICATIONS.md)):
  1. **Install backend dispatch** — `add.c` checks a function pointer
     on the handle; when set, libalpm hands the .pkg.tar.* to the 2O9
     store adapter instead of extracting to `handle->root`.
  2. **Installed-set query** — `be_local.c` checks a function pointer;
     when set, libalpm calls it instead of reading
     `/var/lib/pacman/local/`.
  3. **Config entrypoint** — `two9_init.c` builds an `alpm_handle_t`
     from a 2O9 manifest JSON. No `pacman.conf` involved.

  lib2O9 is built and linked into the `209` binary (2.4 MB static
  library, 216 alpm symbols in the binary). `209 sync` actually calls
  `alpm_db_update()` when a config exists — the real libalpm sync
  machinery, not a stub.

  Build deps: libarchive-dev, openssl-dev, libgpgme-dev, libcurl-dev.
  In a sandbox without root, you can extract these from .deb files to
  `~/local/` and the Makefile picks them up automatically via
  `LIB2O9_DEPS_PREFIX`.

- **Phase 2 — paru → C port: DONE.** AUR RPC (libcurl), PKGBUILD clone
  + makepkg orchestration, recursive dependency resolution. Works
  against the mock server in `scripts/test_aur_mock.sh`.

- **Phase 3 — Declarative engine: DONE.** The Nix evaluator handles
  everything `2O9.nix` needs: attrsets, lists, strings with
  interpolation, let-bindings, if/then/else, lambdas (including
  curried and formal-parameter forms), imports, fixed-point recursion
  for `{ config, ... }`, `inherit (src) ident;`, all 9 binary operator
  precedence levels, 19 builtins. 49/49 tests pass. `209 apply`
  evaluates `2O9.nix`, merges `home.nix` + `2O9.nix` (global wins,
  packages concatenate), reconciles, commits a generation.

- **Phase 4 — Trakker: DONE.** ptrace sandbox with file/network/process
  tracing, `--no-net`, `--no-write`, `--redirect-writes`, `--allow-net`.
  JSON trace output.

- **Phase 5 — Polish: IN PROGRESS.** The 9-step activation phase runs
  real systemctl / systemd-sysusers / systemd-tmpfiles / cache-rebuild
  commands (or no-ops where the symlink farm covers them). `209 news`
  fetches the Arch RSS feed. `209 info` and `209 search` query the
  local generation DB and fall back to AUR. `209 sync` downloads repo
  databases. `209 init` creates a starter config. `test/` has
  integration tests for apply, rollback, nix-eval, and trakker.
  `docs/` has a man page and config reference. What's left: more
  integration tests, packaging polish, maybe a shell completion.

## Roadmap

Phased roadmap with risk-first ordering is in [`DESIGN.md`](./DESIGN.md) §10.
**Phase 1 is now done** — lib2O9 is built, linked, and exercised. The
remaining work is **Phase 5 polish** (packaging, more integration tests,
full activation phase implementation). Phases 0, 1, 2, 3, and 4 are done.

## Honest risks

From [`DESIGN.md`](./DESIGN.md) §11.

- **Non-derivation reproducibility.** Arch `.pkg.tar.zst` are not Nix
  derivations — two builds of an AUR package may hash differently. We get
  content-addressing (same bytes → same path → dedup), not Nix's deep input
  purity. Real derivations are out of scope.
- **In-tree drift.** The copied pacman advances upstream; we re-pull with
  `git subtree pull` and resolve conflicts in our modified tree. Contained to
  the modification targets in `MODIFICATIONS.md`, but real maintenance.
- **Hooks & install scripts.** pacman's `.install` scripts run post-install
  actions. 2O9's approach is to not run them at all — instead extract the
  intent (systemd units, tmpfiles, sysusers) and execute it through an
  idempotent activation phase. The ~10 common patterns cover most packages;
  unusual scripts get a warning.
- **Symlink conflicts.** Two packages shipping the same path is a conflict in
  the symlink farm. The store makes it visible at generation-commit time
  rather than install time, which is better, but still needs a resolution
  strategy.
- **Services after rollback.** On rollback, symlinks change but running
  processes keep their old binaries via open FDs. 2O9 does not restart
  services after a generation switch — the user reboots. No live rollback of
  services without a reboot.
- **Scope realism.** "Full Nix store on pacman" is a multi-year, team-scale
  effort. The plan is structured so each phase produces something useful on
  its own.

## License

GPL-2.0-only. See [`LICENSE`](./LICENSE).
