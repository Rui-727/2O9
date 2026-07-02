# 2O9 — Design Document

**Working title:** 2O9 (stylized; the binary is `209`)
**License:** GPL-2.0-only (inherited from pacman, the engine we link against)
**Scope:** how to combine pacman's libalpm, a C rewrite of paru's AUR
workflow, and a real `/nix/store` with declarative configuration into
one tool.

---

## 1. What 2O9 is

2O9 is one binary that does three jobs today done by three separate tools:

| Job | Today | In 2O9 |
|---|---|---|
| Resolve & install binary packages | `pacman` (libalpm, C) | **lib2O9** (libalpm, modified in-tree) |
| Build & install from the AUR | `paru` (Rust) | **rewritten in C** |
| Declarative, reproducible system state | Nix / NixOS | `/nix/store` + Nix-syntax config |

Rollback is **just a symlink swap + reboot**. There is no boot-time rollback
machinery, no init integration, no special activation service. Picking a
generation = repointing a profile symlink. After that, the user reboots.
That is the entire rollback story.

> **Command vs name.** The binary you type is `209` (numeric). The project name
> **2O9** (with the letter O) is the stylized form, used for branding, docs, and
> on-disk paths: `/etc/2O9/`, `/var/lib/2O9/`, `~/.config/2O9/`.

---

## 2. Locked decisions

These were settled up front and constrain the design:

1. **Single language: C.** The whole 2O9 codebase is C. paru's Rust logic is
   ported to C; the declarative engine is new C. No Rust, no Go.
2. **Full `/nix/store` — no hash in paths.** Not "Nix-inspired", not "Nix language
   only". The real store, with atomic generations. Store paths use the form
   `/nix/store/<name>-<version>/` — **no content hash**. Predictable paths mean
   you always know where a package lives; the tradeoff is no dedup by identical
   content (same bytes from two builds share a path anyway since version is the
   same). The `nix` toolchain (C++) is orchestrated as a **subprocess**, never
   linked, because linking `libstore`/`libexpr` would pull a C++ ABI into an
   otherwise pure-C build and break the "all C" constraint.
3. **No fork of pacman — but the source lives in our tree and we edit it.**
   pacman's source is **copied into our git tree** (`git subtree add` from
   upstream) under `subprojects/pacman/` and **modified directly** there. The
   result is **lib2O9** — our modified build of libalpm, statically linked into
   the `209` binary. We do not publish a competing pacman; we ship 2O9 only.
4. **GPL-2.0-only.** Inherited from pacman. The paru C port is original C code,
   so it inherits the project license cleanly.
5. **Symlink-only activation. Reboot required after generation switch.** No
   boot-time rollback. A generation is a symlink. After switching generations
   (`209 <n> rollback` or `209 apply`), the user **must reboot** for the new
   system state to take full effect. This is the explicit tradeoff of not having
   boot-time rollback machinery: simpler code, but manual reboot.
6. **Two config scopes: global and per-user. Global wins on conflict.** Both are
   first-class, but when `2O9.nix` and `home.nix` conflict on the same setting,
   `/etc/2O9/2O9.nix` takes precedence. The sysadmin's declaration is authoritative.
7. **One config format: Nix.** Everything lives in `2O9.nix`. No `config.toml`,
   no `pacman.conf`, no `paru.conf`. One file, one format, one source of truth.
8. **Own C Nix evaluator — full Nix semantics.** The Nix expression evaluator
   is written from scratch in C as part of lib2O9. No vendored C++ nix source.
   The evaluator supports the function form (`{ config, pkgs, ... }: { ... }`)
   with fixed-point recursion so `config.services.sshd.enable` can be referenced
   from within the config itself. It also supports `import`/`include` so configs
   can be split across multiple files (`packages.nix`, `services.nix`) that are
   packed into one big evaluation unit. It does not need to be a full Nix
   interpreter for all of nixpkgs — just enough to evaluate `2O9.nix` with
   self-references and imports.
9. **Lockfile for concurrency.** `/var/lib/2O9/lock` is an exclusive lock held
   during any mutating operation (`apply`, `install`, `remove`, `rollback`).
   Prevents concurrent writes to the generation DB and store. Simple `flock()` —
   no lock daemon, no stale-lock recovery beyond `rm` (same as pacman).
10. **Services via systemctl.** 2O9 does not manage services itself. Enabling a
    service in `2O9.nix` translates to `systemctl enable <service>`. Disabling
    translates to `systemctl disable <service>`. No 2O9 service manager, no
    home-manager-style service activation. Delegation to systemd.

---

## 3. The core idea — `pacman.conf` becomes `2O9.nix`

Everything starts from one observation: `pacman.conf` is already a declarative
document — it lists what repos exist, what options are set, what packages to
ignore. It just happens to live in an ini-like format that drifts and can't be
versioned cleanly.

2O9 takes that and makes it real — and puts **everything** in one Nix file:

```
pacman.conf + paru.conf + package lists + services  ──►  2O9.nix
                                                          │
                                                          ├── pacman options
                                                          ├── repo sources
                                                          ├── package list (binary + AUR)
                                                          ├── AUR build config
                                                          └── services
```

There is **no `pacman.conf`, no `paru.conf`, no `config.toml`**. Everything is
declared in `2O9.nix`. The package repository is **always an Arch Linux mirror**
— the Nix file just declares *which* mirrors, same as `pacman.conf` does today.
There are no custom repos, no Nix binary caches for packages, no alternative
package sources. 2O9 is an Arch Linux package manager that puts files in
`/nix/store/`.

On `209 apply`, the engine evaluates `2O9.nix` and feeds the `pacman` block
**directly into lib2O9's in-memory API** (`alpm_option_set`,
`alpm_db_register_sync`, etc.). lib2O9 is never pointed at a config file; it is
configured programmatically from the manifest, so the declaration is the single
source of truth with nothing to drift out of sync.

### Two problems to solve

Putting packages in `/nix/store` instead of `/` creates two sub-problems, not
one. Both are solved by modifying libalpm into lib2O9.

**Problem 1: Making files visible.** A package in `/nix/store/firefox-120/`
has its binary at `/nix/store/firefox-120/bin/firefox`, not somewhere on
`$PATH`. The answer is a **symlink farm** — but the symlinks go into each
user's `~/.local/`, not into `/usr/bin/` or any system path. Store paths
have **no hash** — just `<name>-<version>` — so paths are predictable.

```
/nix/store/firefox-120/bin/firefox
/nix/store/firefox-120/etc/firefox/...
/nix/store/neovim-0.9/bin/nvim
/nix/store/neovim-0.9/etc/nvim/...
          │
          ▼  symlink farm (generation #42, for user alice)
~/.local/bin/firefox   →  /nix/store/firefox-120/bin/firefox
~/.local/bin/nvim      →  /nix/store/neovim-0.9/bin/nvim
/etc/firefox           →  /nix/store/firefox-120/etc/firefox
/etc/nvim              →  /nix/store/neovim-0.9/etc/nvim
```

The user's shell includes `~/.local/bin` in `$PATH`. That's it. No system-wide
`/usr/bin` symlinks. Every user gets their own symlink farm in their own
`~/.local/`. Global packages from `2O9.nix` and per-user packages from
`home.nix` all land in the same place — the user's `~/.local/`.

`/etc` files are symlinked at their real paths — `/etc/foo` → store. Programs
expect `/etc/foo` to be at `/etc/foo`, and that's where they find it. Only
binaries and libraries are symlinked into `~/.local/`.

**Problem 2: Making the solver work.** libalpm's solver reads
`/var/lib/pacman/local/` to know what's installed. It expects files at canonical
paths like `/usr/bin/firefox`. But in 2O9, nothing goes to `/usr/bin/` —
it goes to `~/.local/bin/`. The solver still breaks, because it expects the
old filesystem model. We fix it in lib2O9.

We solve this in lib2O9: the "what's installed" query is rewritten to read from
the generation DB instead of `/var/lib/pacman/local/`. The solver sees a
coherent installed set drawn from the current generation — which packages are in
it, what versions, what deps. It doesn't care where the files physically live;
it only needs the metadata. The generation DB provides that metadata, and the
solver works as before.

Both problems are internal to lib2O9. No external hack, no shim layer, no
compatibility bridge. We modify the library to match our model.

---

## 4. Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Unified CLI  (C)                                         │
│  <subject> <verb> — SOV order                            │
│  nginx install | firefox remove | aur search | apply     │
└──────────────┬───────────────────────────────────────────┘
               │
      ┌────────┴─────────┐
      ▼                  ▼
┌──────────────┐   ┌──────────────────────┐
│ Declarative  │   │  AUR Helper          │
│ Engine (C)   │   │  (C, paru port)      │
│              │   │                      │
│ eval 2O9.nix │   │ RPC, clone PKGBUILD, │
│ → manifest → │   │ review diff, resolve │
│ reconcile →  │   │ deps, run makepkg    │
│ transaction  │   │                      │
└──────┬───────┘   └──────────┬───────────┘
       │                      │
       └──────────┬───────────┘
                  ▼
┌──────────────────────────────────────────────────────────┐
│  lib2O9 (libalpm, modified in-tree)                      │
│  dep resolution · repo sync · db parse · hooks           │
│  reads installed set from generation DB, not /var/lib    │
│  install backend dispatches to store adapter              │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌──────────────────────────────────────────────────────────┐
│  Store Adapter (C)                                       │
│  pkg → /nix/store/<name>-<version> → symlink farm → gen  │
│  (extracts .pkg.tar.zst directly into the store)         │
└──────────────────────────────────────────────────────────┘
        │ subprocess / JSON (for nix-store operations)
        ▼
   real /nix/store + nix daemon  (external C++ dependency)
```

Six components:

1. **Unified CLI** — entrypoint, dispatch.
2. **Declarative Engine** — turns `2O9.nix` into a transaction.
3. **AUR Helper** — builds packages that aren't in binary repos, with build
   optimization (see §5.2).
4. **Trakker** — execution sandbox and trace recorder (see §7).
5. **lib2O9** (libalpm, modified in-tree) — solver reads from generation DB,
   install backend dispatches to store adapter.
6. **Store Adapter** — puts packages in the store, then symlinks them into view.

---

## 5. Component designs

### 5.1 Store Adapter (C) — symlink farm manager

The adapter has one job: take a package, put it in the store, then make its
files visible at their conventional paths.

- **`stage_and_register(pkg, files)`**
  Extract `.pkg.tar.zst` to a staging dir → compute tree hash →
  Move staging dir to `/nix/store/<name>-<version>/` — the store path has **no
  hash**, just name and version. Idempotent: if the path already exists, skip.

- **Symlink farm builder**
  For each store path in a generation, create symlinks from conventional
  locations (`~/.local/bin/`, `~/.local/lib/`, `~/.local/share/`) into the
  store. **Not `/etc/`** — `/etc/` is `/etc/`. Config files are symlinked at
  their real system paths. Only binaries and libraries go to `~/.local/`.
  A **generation** = an ordered set of store paths + the symlink-farm manifest
  (the union view). `commit_generation()` atomically swaps the active profile
  symlink via `rename(2)` — that's the rollback primitive.

- **Mapping table** (under `/var/lib/2O9`, format TBD between SQLite and a plain
  directory of files): `pkgname-version → store-path → generation-id`.
  Generation IDs are **integers**, starting at 1, incrementing by 1.

- **Rollback** = `209 <n> rollback` — repoint the profile symlink to generation
  `n`. Instant, because files never move. `209 generations` lists them:

  ```
  #1  2024-03-15 10:30   3 packages   +firefox +neovim +curl
  #2  2024-03-16 14:22   5 packages   +htop +btop  -old-editor  ← current
  ```

  Each generation shows what changed relative to the one before it: `+pkg`
  for additions, `-pkg` for removals. `209 generations` is a log of every
  mutation, not just a list of snapshots.

- **Garbage collection.** Store paths that belong to no generation are eligible
  for GC. `209 gc` runs `nix-collect-garbage`. By default, only the current
  generation's store paths are kept alive; old generations' paths are GC'd
  unless pinned. A generation can be pinned with `209 <n> pin` to prevent its
  store paths from being collected.

- **Nix interop** is exclusively subprocess: `nix-store --add-fixed`,
  `nix-store --realise`, `nix-store --query`, `nix profile`, `nix-collect-garbage`.
  In/out is JSON or line-oriented stdout. We never `dlopen` or link Nix's C++.

- **Running processes.** A running process whose binary is in the store is fine —
  the file is open via file descriptor, so even if the symlink changes, the old
  binary stays readable until the process exits. Store paths are only GC'd when
  no generation references them, so a running process from a recent generation
  is safe. A process from a very old, unpinned, GC'd generation will break — but
  that's the same as deleting a running binary on any system. Don't do that.

### 5.2 AUR Helper — paru ported to C, with build optimization

Paru's responsibilities, mapped onto C libraries — plus compiler optimization
that actually works via the environment:

| paru (Rust) concern | C implementation in 2O9 |
|---|---|
| AUR RPC (`/rpc?v=5`) | **libcurl** + **cJSON** |
| Clone / checkout PKGBUILDs | **libgit2** |
| File-based PKGBUILD review & diff | custom TTY renderer + a diff library |
| Recursive AUR dependency resolution | port of paru's resolver over lib2O9 |
| `makepkg` orchestration | `fork`/`exec` makepkg, capture `.pkg.tar.*` |
| clean-after, news, mflags | direct ports from `paru.conf` semantics |
| config | `2O9.nix` (see §7) |

The resulting `.pkg.tar.*` is handed to the **store adapter** (not `pacman -U`),
so AUR packages land in the store alongside binary ones.

#### Build optimization

2O9 lets you set compiler flags and parallel jobs for AUR builds. These work by
injecting environment variables into `makepkg` — the same mechanism Arch's own
`makepkg.conf` uses. No PKGBUILD patching, no USE flags, no feature toggles.
Just the knobs that actually work from the outside:

```nix
# In 2O9.nix:
aur = {
  packages = [ "google-chrome" ];

  # Build optimization — injected into makepkg's environment
  build = {
    # Optimization profiles: native | safe | custom
    profile = "native";
    # "native" expands to:
    #   CFLAGS    = "-march=native -O3 -pipe";
    #   CXXFLAGS  = "-march=native -O3 -pipe";
    #   LUSTFLAGS = "-C target-cpu=native";
    # "safe" expands to:
    #   CFLAGS    = "-O2 -pipe";
    #   CXXFLAGS  = "-O2 -pipe";
    # Or set them explicitly:
    # CFLAGS    = "-march=native -O3 -pipe -fno-plt";
    # CXXFLAGS  = "-march=native -O3 -pipe -fno-plt";
    # LDFLAGS   = "-Wl,-O1 -Wl,--as-needed";
    # RUSTFLAGS = "-C target-cpu=native";

    # Per-package overrides
    ffmpeg = {
      CFLAGS = "-march=native -O3 -pipe -fno-plt";
    };

    # Parallel build jobs (overrides makepkg.conf MAKEFLAGS)
    jobs = "auto";  # auto = nproc, or an integer like 8
  };
};
```

These are the same knobs you'd set in `/etc/makepkg.conf` today — 2O9 just
moves them into the Nix file and writes a temporary overlay before invoking
`makepkg`. Packages that respect `CFLAGS`, `CXXFLAGS`, `MAKEFLAGS`, etc. pick
up the optimization automatically. Packages that hardcode their own flags don't
— that's on them, same as today.

### 5.3 Declarative Engine (C)

The user writes `2O9.nix` (global) and/or `home.nix` (user scope). This is a
real Nix file — the language, the evaluator, the store, all of it.

The engine evaluates the file and produces a JSON manifest. The Nix expression
evaluator is **written from scratch in C** as part of lib2O9 — under
`lib/2O9/nix/`. It is not a vendored copy of the C++ nix source; it is our own
implementation that understands the Nix language subset 2O9 needs. It builds as
C code within `lib2O9.a`, so the declarative engine calls it directly — no `nix
eval` subprocess, no C++ dependency for config parsing. The store still uses
`nix-store` as a subprocess for store operations, but evaluation is pure C and
entirely ours.

**The config is a function, not a plain attrset.** The `{ config, pkgs, ... }:`
form is required — this is what makes it Nix. The `config` argument enables
self-reference: one part of the config can read another part. The evaluator
implements fixed-point recursion to resolve this — `config` is the result of
evaluating the function itself, resolved lazily. Example:

```nix
{ config, ... }:
{
  services.sshd.enable = true;
  # Self-reference: install openssh-git only if sshd is enabled
  aur.packages = if config.services.sshd.enable
                 then [ "openssh-git" ]
                 else [];
}
```

**Import/include is supported.** Configs can be split across multiple files for
organization. During evaluation, `import`/`include` expressions are resolved by
reading the referenced file and evaluating it in the current scope. The
evaluator packs everything into one evaluation unit. Example:

```nix
# /etc/2O9/2O9.nix
{ config, ... }:
let
  packages = import ./packages.nix;
  services = import ./services.nix;
in
{
  packages = packages;
  services = services;
  # Self-reference still works across imports
  aur.packages = if config.services.sshd.enable
                 then [ "openssh-git" ]
                 else [];
}
```

```nix
# /etc/2O9/packages.nix
[ "firefox" "neovim" "curl" ]
```

```nix
# /etc/2O9/services.nix
{ sshd.enable = true; }
```

What the user writes (`/etc/2O9/2O9.nix`):

```nix
{ config, ... }:

{
  packages = [ "firefox" "neovim" ];
  aur = {
    packages = [ "google-chrome" ];
    build = {
      profile = "native";
      jobs = "auto";
    };
  };
  pacman = {
    options = {
      SigLevel = "Required DatabaseOptional";
      ParallelDownloads = 5;
      IgnorePkg = [ "linux" ];
    };
    repos = {
      core      = { server = "https://mirror.../core/os/$arch"; };
      extra     = { server = "https://mirror.../extra/os/$arch"; };
      multilib  = { server = "https://mirror.../multilib/os/$arch"; };
    };
  };
  services = {
    sshd = { enable = true; };
  };
}
```

What the engine produces (JSON manifest):

```json
{
  "packages": ["firefox", "neovim"],
  "aur": { "packages": ["google-chrome"], "build": { "profile": "native", "jobs": "auto" } },
  "pacman": {
    "options": { "SigLevel": "Required DatabaseOptional", ... },
    "repos": { "core": { "server": "..." }, ... }
  },
  "services": { "sshd": { "enable": true } }
}
```

The `pacman` block **is** `2O9.nix` — there is no `pacman.conf`. On `209 apply`
the engine feeds the `pacman` block **directly into lib2O9's in-memory API**.
The declaration is the single source of truth.

The **reconciler** diffs manifest ↔ current generation DB and produces a
transaction:

```
{ install: [...], remove: [...], aur_build: [...],
  pacman_options_changed: bool, services_on: [...], services_off: [...] }
```

The transaction is executed through lib2O9 + the AUR helper + the store adapter.
On success → `commit_generation()`. On failure → abort; the profile is untouched,
so the system stays consistent.

### 5.4 Unified CLI — SOV order

The CLI uses **Subject-Object-Verb** order. This is a deliberate design choice,
not an aesthetic quirk.

`209 nginx install` reads as "nginx — install." The thing comes first, the action
comes last. You already know what you're talking about before you say what to do
with it. `209 install nginx` puts the verb up front and makes you wait for the
object — action-first is ceremony; subject-first is intent.

pacman's `src/pacman/` frontend is the base for the CLI. The **binary is `209`**
(numeric); the project/branding name is 2O9. Commands:

| Command | Meaning | Origin |
|---|---|---|
| `209 <pkg> install` | Install a package temporarily (not in `2O9.nix`) | pacman |
| `209 <pkg> remove` | Remove a package (new generation without it, rebuild symlink farm) | pacman |
| `209 <pkg> info` | Show package info | pacman |
| `209 <term> search` | Search repos | pacman |
| `209 <pkg> aur build` | Build from AUR | paru |
| `209 <term> aur search` | Search AUR | paru |
| `209 <pkg> aur review` | Review PKGBUILD diff | paru |
| `209 <subject> trakker [flags]` | Run command in sandbox, record trace | new |
| `209 apply` | Apply declarative config | new |
| `209 <n> rollback` | Roll back to generation #n | new |
| `209 <n> pin` | Pin a generation (protect from GC) | new |
| `209 generations` | List generations | new |
| `209 gc` | Garbage-collect unreferenced store paths | new |
| `209 sync` | Sync repo databases | pacman |
| `209 news` | Show Arch news | paru |

**Special subjects:** `apply`, `generations`, `sync`, `news`, `gc` are
zero-argument commands — they have no subject, only a verb. These are the
exceptions that prove the rule: they operate on the system as a whole, not on a
named thing.

**Multi-subject:** `209 nginx firefox install` installs both. The verb comes last,
applied to everything before it.

Backwards-compatible flags are preserved so existing muscle memory and scripts
keep working. Scope (global vs user) is selected by flag / config (§7).

---

## 6. lib2O9 — libalpm + nix evaluator, merged in-tree

We don't fork pacman upstream and we don't build against it untouched. We do
something in between: **copy the source into our tree and modify it directly**.
The result is **lib2O9** — a single static library that merges our modified
libalpm with our modified Nix evaluator, statically linked into the `209` binary.

Both components live under `lib/2O9/` — `alpm/` for the modified libalpm,
`nix/` for our own C Nix evaluator. They're built into one `lib2O9.a`. There
is no separate lib209, no separate nix-eval library. One library, one build
step, one thing to link.

**What we do:**
- pacman's source is pulled into the repo once via `git subtree add` from
  `https://gitlab.archlinux.org/pacman/pacman`, landing under
  `lib/2O9/alpm/`. From that point it's **our copy**, committed and version-
  controlled alongside 2O9's own code.
- The Nix evaluator is **written from scratch in C** under `lib/2O9/nix/`.
  Not a vendored copy of the C++ nix source — our own implementation of the
  Nix language subset that 2O9 needs.
- Both build into a single static `lib2O9.a`.
- We **edit the vendored source directly** — no separate patch series overlaid at
  build time. The modifications are real commits in 2O9's history, visible to
  `git log` and `git blame`.
- Every modification is marked with a `/* 2O9: <reason> */` comment so the
  touched spots are greppable and auditable.

**Why copy-and-edit, not pristine + patches:**
- No patch-application step at build — what's in the tree is exactly what builds.
- Reviewers see the final code, not a patch to imagine applying.
- The downside is real: upstream changes can't be a clean `git am`. We re-pull
  with `git subtree pull` and **resolve conflicts in the modified tree**. To make
  this tractable we keep the modified surface **small and isolated** (see target
  list below) and record each touch in `lib/2O9/alpm/MODIFICATIONS.md`.

**What "no fork" still means:**
- We are **not** publishing a competing pacman binary or maintaining pacman as a
  separate project. lib2O9 is an internal build artifact of 2O9, consumed only
  by 2O9.

**The modification targets (refined in Phase 1, recorded in MODIFICATIONS.md):**

1. **Install backend**: dispatch to the store adapter instead of libalpm's
   builtin extractor, so files land in `/nix/store`.
2. **Installed-set query**: the solver reads "what's installed" from the
   generation DB, not from `/var/lib/pacman/local/`. This is the modification
   that makes the store model work without lying to the solver — it sees a
   coherent installed set, just sourced from a different place.
3. **Config entrypoint**: lib2O9 is configured programmatically from the
   manifest, never from `pacman.conf`. The `alpm_option_set` /
   `alpm_db_register_sync` API already supports this; we just remove the
   config-file path entirely.

---

## 7. Configuration: one file, one format

### Everything in `2O9.nix`

There is no `config.toml`, no `paru.conf`, no `pacman.conf`. One file format
for everything:

| Scope | Config file | Profile symlink | Generation DB |
|---|---|---|---|
| **Global (system)** | `/etc/2O9/2O9.nix` | `/nix/var/nix/profiles/per-user/2O9-system` | `/var/lib/2O9` |
| **Per-user** | `~/.config/2O9/home.nix` | `~/.local/state/2O9/profile` (user-owned Nix profile) | `~/.local/state/2O9` |

### How profiles work

`~/.local/bin` is in `$PATH`. That's the visibility mechanism. Binaries are
symlinked there. Libraries to `~/.local/lib/`. Config files stay at their real
paths — `/etc/` is `/etc/`.

The global `2O9.nix` applies to **all users**. When `209 apply` runs, it
evaluates the Nix file and produces one manifest for the whole system. Then it
builds the symlink farm:

```
/etc/2O9/2O9.nix
        │
        ▼  209 apply (one manifest for everyone)
        │
        ├── binaries:    ~/.local/bin/*  →  store    (per user, no root needed)
        ├── libraries:   ~/.local/lib/*  →  store    (per user, no root needed)
        ├── config:      /etc/*          →  store    (system, root needed)
        └── system bins: /usr/bin/*      →  store    (system, root needed)
```

No daemon. When a new generation is committed, the symlink farm is written
directly. Each user's `~/.local/` gets their symlinks. System paths (`/etc/`,
`/usr/bin/`) get system symlinks (root needed). The shell already has
`~/.local/bin` in `$PATH` (set via `/etc/profile.d/2O9.sh` on install), so
new binaries are visible in the next shell session. If you want them
immediately: `source /etc/profile.d/2O9.sh`.

A user's `home.nix` overlays on top of the global config — packages and
settings in `home.nix` are added to that user's `~/.local/` only, without
affecting anyone else. The merge order (below) determines what wins.

### Merge order (global wins)

When `2O9.nix` and `home.nix` conflict on the same setting, **global wins**.
The sysadmin's declaration is authoritative. This is the opposite of the
conventional "most-specific wins" pattern, and it's deliberate: on a managed
system, the sysadmin's intent overrides individual users. The user scope adds
packages and settings that don't conflict; if there's a conflict, global takes
precedence.

```
built-in defaults
  → ~/.config/2O9/home.nix (user)   ← adds user packages/settings
    → /etc/2O9/2O9.nix        (global)  ← wins on conflict
      → CLI flags                        ← wins on everything
```

### Imperative installs are temporary

`209 <pkg> install` is for temporary use — testing something, debugging,
one-off needs. The package lands in the current generation and works
immediately. But it's not in `2O9.nix`, so the next `209 apply` will
remove it.

**`2O9.nix` is the source of truth. `209 apply` makes the system match the
file.**

If you `209 nginx install` and then `209 apply`, the reconciler shows the
diff before acting:

```
$ 209 apply
 reconciling manifest ↔ generation #42...
 - nginx (not in 2O9.nix, was installed temporarily)
 this package would be removed. proceed? [y/N]
```

To keep it, add it to `2O9.nix` first. To discard it, say yes. To think
about it, say no — nothing happens.

This is intentional. Imperative installs are ephemeral. If you want
something permanent, declare it. No hidden overlay, no "layer on top of the
generation." The generation is what it is. The Nix file declares what it
should be. `209 apply` closes the gap.

### Services — systemctl enable

2O9 does not manage services. Declaring `services.sshd.enable = true` in
`2O9.nix` translates to `systemctl enable sshd` during `209 apply`. Disabling
translates to `systemctl disable <service>`. There is no 2O9 service manager,
no home-manager-style service activation, no service dependency graph. 2O9
tells systemd what to enable and gets out of the way. Services that need
restarting after a generation switch are the user's responsibility — 2O9
doesn't track running daemons.

### Install scripts — the activation model

pacman's `.install` scripts run post-install actions (systemd reload, user
creation, icon cache updates, etc.) against the live filesystem. In a store
model these don't work — files aren't at their conventional paths, and the
scripts would run before symlinks are created.

**2O9's approach: don't run `.install` scripts.** Instead, extract the *intent*
from packages and execute it through an idempotent activation phase, similar to
how NixOS handles this. The key insight from NixOS:

1. **Never run install-time scripts** — they're the wrong abstraction for a
   store-based model. They assume files are at FHS paths, they're not
   idempotent, and they can't be re-run on rollback.
2. **Scan packages for declarative intents** — systemd unit files, tmpfiles.d
   configs, sysusers.d configs, desktop entries, icon caches. These are the
   actual payloads; the `.install` script is just the delivery mechanism.
3. **Execute intents in an activation phase** — idempotent, ordered, runs on
   every `209 apply`. The activation phase runs after extraction but before
   the generation is committed.

The activation phases (in order):

1. **Stop affected services** — `systemctl stop <units that changed>`
2. **Populate `/etc` symlinks** — unit files, configs from the store
3. **Apply sysusers** — `systemd-sysusers` with configs from the store (idempotent)
4. **Apply tmpfiles** — `systemd-tmpfiles --create` with rules from the store (idempotent)
5. **Create/update users and groups** — idempotent, from package metadata
6. **daemon-reload** — `systemctl daemon-reload` to pick up new unit files
7. **Enable/disable services** — `systemctl enable`/`disable` per `2O9.nix`
8. **Rebuild caches** — icon cache, desktop database, font cache, etc.
9. **Start/restart services** — only those that changed in this generation

The ~10 patterns that cover the vast majority of Arch `.install` scripts:

| pacman .install pattern | 2O9 activation equivalent |
|---|---|
| `systemctl daemon-reload` | activation phase step 6 |
| `systemctl enable foo` | activation phase step 7 (or `.wants` symlink) |
| `systemd-sysusers` | activation phase step 3 |
| `systemd-tmpfiles --create` | activation phase step 4 |
| `useradd/usergroup` | activation phase step 5 |
| `gtk-update-icon-cache` | activation phase step 8 |
| `update-desktop-database` | activation phase step 8 |
| `ldconfig` | activation phase step 8 (or skip — store doesn't need ldconfig) |
| `chmod/chown /var/lib/...` | activation phase step 4 (tmpfiles rules) |
| `systemd-tmpfiles --create` | activation phase step 4 |

Anything not matching these patterns gets a warning logged. The user can add
custom activation commands in `2O9.nix` under `system.activationScripts` if
needed (Phase 5+).

### Concurrency — lockfile

`/var/lib/2O9/lock` is an exclusive lock held during any mutating operation:
`apply`, `install`, `remove`, `rollback`, `gc`. The lock is taken with
`flock(LOCK_EX)` at the start and released when the process exits. If another
2O9 process holds the lock, the second one prints an error and exits. No lock
daemon, no stale-lock recovery beyond `rm /var/lib/2O9/lock` (same approach as
pacman's db lock). The lockfile lives alongside the generation DB because
they're mutated together.

### Generation switch — reboot required

After switching generations (via `209 <n> rollback` or `209 apply`), the user
**must reboot** for the full system state to take effect. The symlink farm is
updated immediately, so new binaries are visible in the next shell session,
but running processes keep their old binaries (via open file descriptors), and
services continue running the old code. A reboot ensures everything picks up
the new generation. This is the tradeoff of not having boot-time rollback
machinery: simpler code, no activation service, but manual reboot.

### Trakker — execution sandbox and trace recorder

`209 trakker <cmd>` runs a command inside 2O9's sandbox, recording everything
it does and optionally restricting what it's allowed to do.

**What trakker records:**
- **File I/O** — every file opened, created, modified, or deleted. Full
  before/after paths.
- **Network** — every connection opened (address, port, protocol).
- **Process** — fork/exec, signals, exit code.
- **Memory** — mmap calls, page faults (summary, not full dump).
- **Registers** — register state at syscall entry (for debugging/reversing).

**What trakker can restrict (via flags):**
- `--no-net` — block all network access. Any socket() / connect() call fails.
- `--redirect-writes /tmp/trakker` — file writes are redirected to a
  specified directory instead of their real target. The program thinks it
  wrote to `/etc/foo`; the data actually landed in
  `/tmp/trakker/etc/foo`. Reads still see the real filesystem.
- `--no-write` — block all file writes entirely.
- `--allow-net port=443` — allow only specific network destinations.

**How it works:** trakker uses `ptrace` (or `seccomp`-bpf for filtering
only) to intercept syscalls. On each syscall, it records the action and
checks it against the restriction policy. Blocked syscalls return `-EPERM`
to the traced process. Redirected writes are handled by intercepting
`open()`/`creat()`, rewriting the path, and letting the call proceed to the
redirected destination.

**Use cases:**
- `209 some-app trakker --no-net` — run an app with no network access.
- `209 pkg trakker --redirect-writes /tmp/test install` — install a package
  but catch every file it would write, without actually writing it.
- `209 unknown-bin trakker` — run an untrusted binary and see what it does
  before trusting it.
- `209 aur-pkg trakker --redirect-writes /tmp/aur-review aur build` — build
  an AUR package but capture all writes for review.

Trakker output is a JSON trace log:

```json
{
  "command": "./suspicious-app",
  "exit_code": 0,
  "duration_ms": 1234,
  "files": {
    "read": ["/etc/resolv.conf", "/usr/lib/libc.so.6"],
    "write": ["/tmp/trakker/home/user/.config/app/config.toml"],
    "create": ["/tmp/trakker/home/user/.cache/app/data.db"],
    "delete": []
  },
  "network": {
    "blocked": ["connect(142.250.80.46:443)"],
    "allowed": []
  },
  "processes": {
    "forked": [{ "pid": 1234, "command": "/usr/bin/curl" }],
    "exec": []
  }
}
```

Trakker is built into the `209` binary. It is not a separate tool. The
implementation sits in `src/trakker/` and uses Linux's `ptrace` API directly
— no external tracing dependencies.

---

## 8. Worked examples

### `209 apply` — success

```
$ 209 apply
 1. eval 2O9.nix           ──►  manifest JSON
 2. reconcile(manifest, current-gen)  ──►  { +neovim, -old-editor, +chromium(AUR) }
 3. lib2O9 resolves neovim deps       ──►  transaction plan
 4. store adapter: pkg.tar.zst ──► /nix/store/neovim-0.9.5
    symlink farm: ~/.local/bin/nvim → /nix/store/neovim-0.9.5/bin/nvim
                  /etc/nvim         → /nix/store/neovim-0.9.5/etc/nvim
 5. AUR helper: chromium PKGBUILD → review → makepkg (CFLAGS from 2O9.nix) → pkg.tar.zst
 6. store adapter: ──► /nix/store/chromium-120
    symlink farm: ~/.local/bin/chromium → /nix/store/chromium-120/bin/chromium
                  /etc/chromium         → /nix/store/chromium-120/etc/chromium
 7. commit generation #42  (profile symlink swapped)
✓ done.  rollback with: 209 41 rollback
```

### `209 apply` — would remove imperative installs

```
$ 209 apply
 reconciling manifest ↔ generation #42...
 - htop (not in 2O9.nix, was installed imperatively)
 - btop (not in 2O9.nix, was installed imperatively)
 these packages would be removed. proceed? [y/N]
```

If no → nothing happens. Add them to `2O9.nix` and apply again.
If yes → they're removed, generation #43 is committed.

### `209 apply` — failure

```
$ 209 apply
 reconciling manifest ↔ generation #42...
 +chromium (AUR)
 building chromium...
 error: makepkg failed — missing dependency libva
 transaction aborted. generation #42 unchanged.
 fix the issue and try again.
```

The generation is untouched. Fix the issue (install libva, or remove chromium
from `2O9.nix`), then `209 apply` again.

### Imperative usage

```
$ 209 neovim install        # install from repo
$ 209 chromium aur build    # build from AUR
$ 209 41 rollback           # go back to generation #41
$ 209 generations           # list all generations
$ 209 gc                    # garbage-collect unreferenced store paths
```

---

## 9. Repo & build structure

```
2O9/                              # GPL-2.0-only
├── Makefile                      # build system
├── lib/2O9/                      # lib2O9 — one static library, merged from:
│   ├── alpm/                     #   modified libalpm (from pacman, copied in & edited)
│   │   └── MODIFICATIONS.md      #   log of every 2O9 change
│   ├── nix/                      #   own C Nix evaluator (written from scratch)
│   │   └── README.md             #   evaluator design notes
│   └── common/                   #   shared utils (ini.c, util-common.c)
├── src/
│   ├── cli/                      # unified front-end (C) — builds the `209` binary
│   ├── aur/                      # paru logic, ported to C  (NEW)
│   ├── declarative/              # reconcile engine        (NEW)
│   ├── store/                    # store adapter           (NEW)
│   └── trakker/                  # sandbox + trace recorder(NEW)
├── scripts/                      # hooks, makepkg wrappers, activation
├── test/                         # unit + integration + generation-rollback tests
└── docs/
```

Build system: **make** (one Makefile, no dependencies beyond a C compiler and make).

### External build/runtime dependencies

| Dep | Used by | Notes |
|---|---|---|
| `lib2O9` (alpm copied-in & modified; nix evaluator own C) | core | static `lib2O9.a` — merged libalpm + nix evaluator |
| `libarchive` | lib2O9 | for .pkg.tar.zst extraction |
| `libcurl` | AUR helper | AUR RPC |
| `libgit2` | AUR helper | clone PKGBUILDs |
| `cJSON` | AUR helper, declarative | JSON parse/emit |
| `nix` (the tool, C++) | store adapter | **subprocess only** — never linked. Not needed for eval (our C evaluator does that) |
| `makepkg` | AUR helper | invoked as subprocess |
| `sqlite` (optional) | store adapter | generation DB (or plain files) |

---

## 10. Phased roadmap (risk-first)

Phase 1 is the make-or-break. If lib2O9 can't cleanly remap its installed-set
query and install backend, the "full Nix store on pacman" premise needs
rethinking — which is exactly why it's first.

| Phase | Goal | Exit criterion | Risk |
|---|---|---|---|
| **0 — Foundation** | Repo, meson build, copy pacman into `subprojects/`, C→`nix` subprocess spike, name/license settled | `209 -V` builds; one C program can `nix-store --add` a file | Low |
| **1 — Store adapter MVP** | One binary package → store → symlink farm; one profile; one rollback | `209 sl install` → store path + symlink; `209 1 rollback` restores | **HIGH** |
| **2 — paru → C port** | AUR RPC, clone, review, makepkg, into the store; build optimization | `209 <pkg> aur build` works end-to-end with custom CFLAGS | ~~Medium~~ **DONE** |
| **3 — Declarative engine** | own C Nix evaluator (from scratch, in lib2O9) → reconcile → transaction → generations | `209 apply` from `2O9.nix`, reproducibly | Medium |
| **4 — Trakker** | ptrace-based sandbox, syscall tracing, network/write blocking, redirect | `209 some-app trakker --no-net` runs and produces trace JSON | Medium |
| **5 — Polish** | Unified CLI, hooks, user scope, docs, packaging | distro-installable, documented | Medium |

Phase 1 alone is a useful, shippable proof-of-concept even if later phases stall.

---

## 11. Honest risks

- **Non-derivation reproducibility.** Arch `.pkg.tar.zst` are not Nix derivations,
  so two builds of an AUR package may hash differently. We get
  *content-addressing* (same bytes → same path → dedup), not Nix's *deep input
  purity*. Generating real derivations is a separate, much larger project and is
  explicitly out of scope.
- **In-tree drift.** The copied pacman advances upstream; we re-pull with
  `git subtree pull` and resolve conflicts in our modified tree. Contained to the
  modification targets in §6, but real maintenance. Keeping that surface small
  and logging every change in `MODIFICATIONS.md` is what makes it tractable.
- **Hooks & install scripts.** pacman's `.install` scripts run post-install
  actions (systemd reload, user creation, etc.). In a store model these need to
  run at generation-activation time, not at package-build time. 2O9's approach
  is to **not run `.install` scripts at all** — instead, extract the *intent*
  from packages (systemd units, tmpfiles rules, sysusers configs) and execute
  it through an idempotent activation phase (see §7, "Install scripts — the
  activation model"). The ~10 common patterns cover the vast majority of Arch
  packages. Unusual `.install` scripts get a warning; users can add custom
  activation commands in `2O9.nix`. Deferred to Phase 3 but flagged now.
- **Symlink conflicts.** Two packages shipping the same path (e.g. both install
  `~/.local/bin/foo`) is a conflict in the symlink farm, just as it is in pacman
  today. The store makes it visible earlier (at generation-commit time) rather
  than at install time, which is actually better — but it still needs a conflict
  resolution strategy.
- **Services after rollback.** When you rollback, symlinks change but running
  processes keep their old binaries (via open FDs). 2O9 does not automatically
  restart services after a generation switch — the user reboots. Services are
  managed via `systemctl enable`/`disable`, so after reboot, systemd starts
  exactly what's enabled. This is simple and correct but means there's no
  "live rollback" of services without a reboot.
- **Scope realism.** "Full Nix store on pacman" is a multi-year, team-scale effort.
  The plan is structured so each phase produces something useful on its own.
