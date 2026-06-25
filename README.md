# 2O9

**2O9** (stylized project name; the binary is **`209`**) is a unified package
manager for Arch Linux that puts files in `/nix/store/`. It combines three
things into one tool:

1. **pacman's engine** — libalpm, copied into the tree and modified directly
   as part of **lib2O9**: dependency resolution, repo sync, database parsing,
   and hooks. The plan is for the solver to read the installed set from the
   generation DB instead of `/var/lib/pacman/local/`, and the install backend
   to dispatch to the store adapter (see `lib/2O9/alpm/MODIFICATIONS.md`).
2. **paru's AUR workflow** — rewritten in C: AUR RPC queries, PKGBUILD clone
   and review, recursive AUR dependency resolution, and `makepkg` orchestration.
3. **A real `/nix/store`** — predictable store paths (`/nix/store/<name>-<version>/`,
   no content hash) with atomic generations, driven by a declarative Nix-syntax
   configuration (`2O9.nix`) describing desired system and per-user state. The
   Nix evaluator is **written from scratch in C** as part of lib2O9 — not a
   vendored copy of the C++ nix source. It supports the function form
   (`{ config, ... }: ...`) with fixed-point recursion for self-reference, and
   `import` for splitting configs across multiple files.

Plus **Trakker** — a ptrace-based execution sandbox: syscall tracing, network
and write blocking, file-write redirection, JSON trace output. Invoke as
`209 <pkg> trakker --no-net`.

Rollback is a symlink swap + reboot. No boot-time rollback machinery, no
daemon. Activation repoints a generation symlink; the user reboots for the new
system state to take full effect. Services are managed via `systemctl enable` —
2O9 has no service manager of its own.

## The package repo is always Arch Linux

2O9 is an Arch Linux package manager. The repository is always an Arch Linux
mirror — configured in `2O9.nix` under `pacman.repos`. No custom repos, no
Nix binary caches for packages, no alternative sources. We just put the files
in `/nix/store/` instead of `/`.

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

**Real dependencies today:** a C compiler, `make`, and `libcurl-dev`. Only the
AUR helper links `libcurl` (`-lcurl` is the only link flag in the Makefile).
cJSON is vendored in `src/aur/cJSON.{c,h}` — no external JSON dependency.
lib2O9 (the alpm stub plus the Nix evaluator) is pure C with no extra deps.

**Future dependencies (not currently linked):** `DESIGN.md` anticipates
`libarchive-dev` and `openssl-dev` for the eventual real libalpm build
(package extraction and signature verification). The Makefile does not link
them today because the 209 binary operates independently of libalpm — see
[Status](#status). When the libalpm modifications land in Phase 1, those deps
will be added.

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
│   ├── alpm/                   #   modified libalpm (planned — currently stub)
│   │   └── MODIFICATIONS.md    #   log of 3 planned modifications (all "Planned", none applied)
│   └── common/                 #   shared utils (ini.c, util-common.c)
├── src/
│   ├── cli/main.c              # 209 binary — SOV command dispatch
│   ├── aur/                    # AUR helper (paru ported to C)
│   │   ├── aur_rpc.c           # AUR RPC client (libcurl)
│   │   ├── aur_build.c         # PKGBUILD clone + makepkg orchestration
│   │   ├── aur_resolve.c       # recursive AUR dependency resolver
│   │   ├── cJSON.c/h           # vendored JSON library
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

```sh
# Imperative install — pulls a pkg.tar.zst into the store, commits a new generation
TWO09_PKG_PATH=/var/cache/pacman/pkg/neovim-0.10.0-1-x86_64.pkg.tar.zst \
  209 neovim install

# End-to-end pipeline test without nix-store (uses a fake store path)
TWO09_TEST_MODE=1 209 sl install

# Build from the AUR (resolves deps, clones PKGBUILD, runs makepkg, adds to store)
209 yt-dlp aur build

# Search the AUR, inspect a package, read its PKGBUILD diff before building
209 ffmpeg aur search
209 ffmpeg aur info
209 ffmpeg aur review

# Apply declarative config — evaluate 2O9.nix, reconcile against the current
# generation, commit a new generation if the manifest changed
sudo 209 apply

# List generations, roll back to one, pin it so GC won't reap its store paths
209 generations
209 3 rollback
209 3 pin

# Run an untrusted binary in the sandbox with no network and redirected writes
209 weird-binary trakker --no-net --redirect-writes /tmp/trakker
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

Honest accounting of what works versus what is planned.

- **Phase 0 — Foundation: DONE.** Repo, Makefile, build works, four targets
  compile and link.
- **Phase 1 — Store adapter MVP: PARTIAL → MODIFICATIONS APPLIED.** The
  store adapter (`src/store/`) and symlink farm (`src/store/symlinks.c`)
  are implemented, as is the file-based generation DB
  (`src/declarative/gen.c`). **All three lib2O9 modifications to vendored
  libalpm are now applied** to the source tree (see
  [`lib/2O9/alpm/MODIFICATIONS.md`](./lib/2O9/alpm/MODIFICATIONS.md)):
  1. **Install backend dispatch** — `alpm_handle_t.install_backend`
     function pointer checked in `add.c`'s `commit_single_pkg()`; when
     set, libalpm hands the .pkg.tar.* to the 2O9 store adapter instead
     of extracting to `handle->root`.
  2. **Installed-set query** — `alpm_handle_t.installed_set_loader`
     checked in `be_local.c`'s `local_db_populate()`; when set, libalpm
     calls it instead of reading `/var/lib/pacman/local/`.
  3. **Config entrypoint** — new `lib/2O9/alpm/two9_init.c` provides
     `two9_alpm_init_from_manifest()` which configures an
     `alpm_handle_t` programmatically from a 2O9 manifest JSON, never
     reading `/etc/pacman.conf`.

  The modifications are marked with `/* 2O9: ... */` comments per the
  design spec. **lib2O9 is not yet built into the 209 binary** —
  building it requires libarchive-dev, openssl-dev, and optionally
  libgpgme-dev. The `209` binary continues to operate independently of
  libalpm until those deps are added to the Makefile and `lib2O9.a` is
  linked. The modifications are real, auditable C code changes ready to
  be exercised once lib2O9 is built.
- **Phase 2 — paru → C port: DONE.** AUR RPC client (`src/aur/aur_rpc.c`,
  libcurl), PKGBUILD clone + makepkg orchestration (`aur_build.c`), recursive
  AUR dependency resolver (`aur_resolve.c`). `test-aur-rpc` works against the
  mock server in `scripts/test_aur_mock.sh`.
- **Phase 3 — Declarative engine: DONE.** The Nix evaluator is implemented
  (lexer + parser + evaluator, ~3.4k LOC across `lib/2O9/nix/`, 19 builtins,
  fixed-point recursion for `{ config, ... }`, `import`/`include` support,
  curried lambda application, formal parameters with defaults, all 9 binary
  operator precedence levels, `inherit (src) ident;` form, dot-notation
  select for `builtins.*`). 49/49 evaluator tests pass. The generation DB
  and reconciler are implemented. `209 apply` evaluates `2O9.nix` and
  commits generations. **User-scope `home.nix` is now wired in**:
  `cmd_apply` evaluates both `~/.config/2O9/home.nix` and
  `/etc/2O9/2O9.nix`, merges them per DESIGN.md §7 (global wins on
  conflict, packages concatenate), then reconciles. See
  [`lib/2O9/nix/README.md`](./lib/2O9/nix/README.md) for details.
- **Phase 4 — Trakker: DONE.** `src/trakker/trakker.c` implements the
  ptrace-based sandbox with the `trakker_policy_t` struct, event recording
  (file, network, process, memory), and all four restriction flags.
- **Phase 5 — Polish: IN PROGRESS.** The 9-step activation phase is
  fully implemented (`src/declarative/activation.{c,h}`) and wired into
  `cmd_apply` — runs after the symlink farm, before the final report.
  All 9 steps invoke real `systemctl`/`systemd-sysusers`/
  `systemd-tmpfiles`/cache-rebuild commands (or are intentional no-ops
  where the symlink farm covers them). `209 news` fetches the Arch
  Linux RSS feed. `209 <pkg> info` and `209 <term> search` work
  against the local generation DB and fall back to AUR. `209 sync`
  downloads repo .db files via libcurl to `/var/cache/2O9/pkg/`.
  `test/` and `docs/` directories exist with planning docs. Remaining
  Phase 5 work: link lib2O9 into the 209 binary (needs libarchive-dev
  + openssl-dev), packaging, full integration testing.

## Roadmap

Phased roadmap with risk-first ordering is in [`DESIGN.md`](./DESIGN.md) §10.
The remaining major work is **Phase 1** (libalpm modifications — make-or-break
for the "full Nix store on pacman" premise) and **Phase 5** (polish,
packaging, docs). Phases 0, 2, and 4 are done; Phase 3 is largely done modulo
parser gaps.

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
