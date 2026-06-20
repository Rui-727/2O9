# 2O9 — Design Document

**Working title:** 2O9 (stylized; the binary is `209`)
**License:** GPL-2.0-only (inherited from pacman, the engine we link against)
**Scope of this document:** architecture for combining pacman's libalpm, a C rewrite
of paru's AUR workflow, and a real `/nix/store` with declarative configuration —
into a single tool.

---

## 1. What 2O9 is

2O9 is one binary that does three jobs today done by three separate tools:

| Job | Today | In 2O9 |
|---|---|---|
| Resolve & install binary packages | `pacman` (libalpm, C) | libalpm **linked as a dependency** |
| Build & install from the AUR | `paru` (Rust) | **rewritten in C** |
| Declarative, reproducible system state | Nix / NixOS | `/nix/store` + Nix-syntax config |

Rollback is **just a symlink swap**. There is no boot-time rollback machinery,
no init integration, no special activation service. Picking a generation = repointing
a profile symlink. That is the entire rollback story.

> **Command vs name.** The binary you type is `209` (numeric). The project name
> **2O9** (with the letter O) is the stylized form, used for branding, docs, and
> on-disk paths: `/etc/2O9/`, `/var/lib/2O9/`, `~/.config/2O9/`.

---

## 2. Locked decisions

These were settled up front and constrain the design:

1. **Single language: C.** The whole 2O9 codebase is C. paru's Rust logic is
   ported to C; the declarative engine is new C. No Rust, no Go.
2. **Full `/nix/store`.** Not "Nix-inspired", not "Nix language only". The real
   store, with content-addressed paths and atomic generations. 2O9's process is
   pure C; the `nix` toolchain (C++) is orchestrated as a **subprocess**, never
   linked, because linking `libstore`/`libexpr` would pull a C++ ABI into an
   otherwise pure-C build and break the "all C" constraint.
3. **No fork of pacman — but the source lives in our tree and we edit it.**
   pacman's source is **copied into our git tree** (`git subtree add` from
   upstream) under `subprojects/pacman/` and **modified directly** there. It is
   not an external dependency we build against untouched, and it is not a
   divergent upstream fork we publish — we ship 2O9 only. (See §6 for the seam.)
4. **GPL-2.0-only.** Inherited from pacman. The paru C port is original C code,
   so it inherits the project license cleanly.
5. **Symlink-only activation.** No boot rollback. A generation is a symlink.
6. **Two config scopes: global and per-user.** Both are first-class.

---

## 3. The core idea — `pacman.conf` becomes `2O9.nix`

Everything starts from one observation: `pacman.conf` is already a declarative
document — it lists what repos exist, what options are set, what packages to
ignore. It just happens to live in an ini-like format that drifts and can't be
versioned cleanly.

2O9 takes that and makes it real:

```
pacman.conf  ──►  2O9.nix  (declarative, versionable, evaluable)
                    │
                    ├── pacman options  (SigLevel, ParallelDownloads, IgnorePkg, repos…)
                    ├── package list    (binary + AUR)
                    ├── AUR config      (build flags, optimizations — see §5.2)
                    └── services        (what's enabled)
```

There is **no `pacman.conf` file of any kind**. The `[options]` and `[repo]`
sections are Nix attributes, and on `209 apply` the declarative engine feeds
them **directly into libalpm's in-memory API** (`alpm_option_set`,
`alpm_db_register_sync`, etc.). libalpm is never pointed at a config file; it
is configured programmatically from the manifest, so the declaration is the
single source of truth with nothing to drift out of sync.

### The store problem: `/nix/store/<hash>/bin`

When a package lands in `/nix/store/<hash>-pkgname/`, its files are at paths
like `/nix/store/<hash>-firefox/bin/firefox`, not `/usr/bin/firefox`. The only
hard problem is making those files appear where the system expects them.

The answer is straightforward: **symlink farm**. Each generation is a set of
union symlinks that map `/nix/store/<hash>/bin/*` → `/usr/bin/*`,
`/nix/store/<hash>/lib/*` → `/usr/lib/*`, and so on. The profile symlink points
at a generation; swapping that symlink is rollback.

```
/nix/store/x3f-firefox-120/bin/firefox
/nix/store/x3f-firefox-120/lib/firefox/...
/nix/store/a91-neovim-0.9/bin/nvim
          │
          ▼  symlink farm (generation #42)
/usr/bin/firefox  →  /nix/store/x3f-firefox-120/bin/firefox
/usr/bin/nvim     →  /nix/store/a91-neovim-0.9/bin/nvim
/usr/lib/firefox  →  /nix/store/x3f-firefox-120/lib/firefox
```

That's it. No grand filesystem model clash — just symlinks from store paths to
their conventional locations. The pacman local DB is kept for metadata (deps,
version, files-list) so tooling stays sane, but "installed paths" now point into
the store.

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
│ nix eval →   │   │ RPC, clone PKGBUILD, │
│ manifest →   │   │ review diff, resolve │
│ reconcile →  │   │ deps, run makepkg    │
│ transaction  │   │                      │
└──────┬───────┘   └──────────┬───────────┘
       │                      │
       └──────────┬───────────┘
                  ▼
┌──────────────────────────────────────────────────────────┐
│  libalpm (pacman C backend) — copied in & modified     │
│  dep resolution · repo sync · db parse · hooks           │
│  INTERCEPT: install backend swapped to store emission    │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌──────────────────────────────────────────────────────────┐
│  Store Adapter (C)                                       │
│  pkg → /nix/store/<hash> → symlink farm → generation     │
│  (orchestrates the `nix` binary via subprocess / JSON)   │
└──────────────────────────────────────────────────────────┘
        │ subprocess / JSON
        ▼
   real /nix/store + nix daemon  (external C++ dependency)
```

Five components:

1. **Unified CLI** — entrypoint, dispatch.
2. **Declarative Engine** — turns `2O9.nix` into a transaction.
3. **AUR Helper** — builds packages that aren't in binary repos, with
   Gentoo-style build optimization (see §5.2).
4. **libalpm** (copied-in, modified pacman tree) — solver + metadata + hooks.
5. **Store Adapter** — puts packages in the store, then symlinks them into view.

---

## 5. Component designs

### 5.1 Store Adapter (C) — symlink farm manager

The adapter has one job: take a package, put it in the store, then make its
files visible at their conventional paths.

- **`stage_and_register(pkg, files)`**
  Extract `.pkg.tar.zst` to a staging dir → compute tree hash →
  `nix-store --add-fixed --recursive sha256 <staging>` → returns the store path.
  Idempotent: if the path already exists in the store, skip (dedup by content).

- **Symlink farm builder**
  For each store path in a generation, create symlinks from conventional
  locations (`/usr/bin/`, `/usr/lib/`, `/usr/share/`, etc.) into the store.
  A **generation** = an ordered set of store paths + the symlink-farm manifest
  (the union view). `commit_generation()` atomically swaps the active profile
  symlink via `rename(2)` — that's the rollback primitive.

- **Mapping table** (under `/var/lib/2O9`, format TBD between SQLite and a plain
  directory of files): `pkgname-version → store-path → generation-id`.

- **Rollback** = pick a prior generation id, repoint the profile symlink. Instant,
  because files never move. There is no "uninstall then reinstall"; the symlink
  just points elsewhere.

- **Nix interop** is exclusively subprocess: `nix-store --add-fixed`,
  `nix-store --realise`, `nix-store --query`, `nix profile`, `nix-collect-garbage`.
  In/out is JSON or line-oriented stdout. We never `dlopen` or link Nix's C++.

### 5.2 AUR Helper — paru ported to C, with Gentoo-style build optimization

Paru's responsibilities, mapped onto C libraries — plus a build-optimization
layer inspired by Gentoo's `make.conf`:

| paru (Rust) concern | C implementation in 2O9 |
|---|---|
| AUR RPC (`/rpc?v=5`) | **libcurl** + **cJSON** |
| Clone / checkout PKGBUILDs | **libgit2** |
| File-based PKGBUILD review & diff | custom TTY renderer + a diff library |
| Recursive AUR dependency resolution | port of paru's resolver over libalpm |
| `makepkg` orchestration | `fork`/`exec` makepkg, capture `.pkg.tar.*` |
| clean-after, news, mflags | direct ports from `paru.conf` semantics |
| config | unified `2O9.conf` (see §7) replacing `paru.conf` |

The resulting `.pkg.tar.*` is handed to the **store adapter** (not `pacman -U`),
so AUR packages land in the store alongside binary ones.

#### Build optimization (Gentoo-style)

2O9's AUR config exposes the same knobs Gentoo users know from `make.conf`,
so AUR packages are built with the user's chosen compiler flags and feature
toggles instead of the upstream PKGBUILD defaults:

```nix
# In 2O9.nix, under aur:
aur = {
  # Per-package USE-style flags — enable/disable optional features
  # before building. These are injected into the PKGBUILD's
  # environment as environment variables or build flags.
  flags = {
    # Global defaults (applied to every AUR build unless overridden)
    "*" = {
      MAKEOPTS = "-j$(nproc)";
      # Strip debug symbols for smaller packages
      OPTIONS = [ "strip" "!debug" ];
    };
    # Per-package overrides
    "ffmpeg" = {
      ENABLE = [ "nvenc" "vaapi" "vulkan" ];
      DISABLE = [ "alsa" ];
      CFLAGS = "-march=native -O3 -pipe";
    };
    "neovim" = {
      ENABLE = [ "lua" "python" ];
    };
  };

  # Compiler optimization profiles — select one or define your own
  # These expand into CFLAGS / CXXFLAGS / LDFLAGS for makepkg
  optimize = {
    profile = "native";   # native | safe | aggressive | custom
    # Or explicit:
    # CFLAGS    = "-march=native -O3 -pipe -fno-plt";
    # CXXFLAGS  = "-march=native -O3 -pipe -fno-plt";
    # LDFLAGS   = "-Wl,-O1 -Wl,--as-needed";
    # RUSTFLAGS = "-C target-cpu=native";
  };

  # Parallel build jobs (overrides makepkg.conf MAKEFLAGS)
  jobs = "auto";  # auto = nproc, or an integer
};
```

The mapping from Gentoo concepts:

| Gentoo (`make.conf`) | 2O9 (`2O9.nix` → `aur.flags`) | Effect |
|---|---|---|
| `USE="nvenc vaapi -alsa"` | `ENABLE = [ "nvenc" "vaapi" ]; DISABLE = [ "alsa" ]` | Feature toggles injected into PKGBUILD environment |
| `CFLAGS="-march=native -O3"` | `optimize.CFLAGS = "..."` or `optimize.profile = "native"` | Compiler flags for all C/C++ AUR builds |
| `MAKEOPTS="-j8"` | `jobs = 8` or `flags."*".MAKEOPTS` | Parallel build jobs |
| `FEATURES="strip"` | `OPTIONS = [ "strip" ]` | Build-time features |

The build optimization layer works by **patching the environment** that
`makepkg` sees — it writes a temporary `makepkg.conf` overlay with the user's
flags before invoking `makepkg`. The PKGBUILD itself is not modified; flags
are injected via the environment, matching how Arch's own `makepkg.conf`
works. Packages that respect standard build variables (`CFLAGS`, `CMAKEFLAGS`,
etc.) pick up the optimization automatically.

### 5.3 Declarative Engine (C)

The user writes `2O9.nix` (global) and/or `home.nix` (user scope). We **do not
ship a Nix interpreter**. We shell out to `nix eval --json --impure ./2O9.nix`,
which yields a JSON **manifest**:

```json
{
  "packages": ["firefox", "neovim"],
  "aur": ["google-chrome"],
  "pacman": {
    "options": { "SigLevel": "Required DatabaseOptional",
                 "ParallelDownloads": 5,
                 "IgnorePkg": ["linux"] },
    "repos": {
      "core":    { "server": "https://mirror.../core/os/$arch" },
      "extra":   { "server": "https://mirror.../extra/os/$arch" },
      "multilib":{ "server": "https://mirror.../multilib/os/$arch" }
    }
  },
  "services": { "sshd": { "enable": true } }
}
```

The `pacman` block **is** `2O9.nix` — there is no `pacman.conf`. `[options]`
and `[repo]` sections are Nix attributes, and on `209 apply` the declarative
engine feeds them **directly into libalpm's in-memory API**. The declaration is
the single source of truth with nothing to drift out of sync.

The **reconciler** diffs manifest ↔ current generation DB and produces a
transaction:

```
{ install: [...], remove: [...], aur_build: [...],
  pacman_options_changed: bool, services_on: [...], services_off: [...] }
```

The transaction is executed through libalpm (configured in-memory from the
manifest) + the AUR helper + the store adapter.
On success → `commit_generation()`. On failure → abort; the profile is untouched,
so the system stays consistent. Because generations are symlinks, a half-applied
transaction simply never gets its generation committed.

### 5.4 Unified CLI — SOV order

The CLI uses **Subject-Object-Verb** order. This is a deliberate design choice,
not an aesthetic quirk.

`209 nginx install` reads as "nginx. install. now." — direct, declarative,
no ceremony. `209 install nginx` reads as "Excuse me, could you please install
nginx if you have a moment?" — too polite for PID 1.

pacman's `src/pacman/` frontend is the base for the CLI. The **binary is `209`**
(numeric); the project/branding name is 2O9. Commands:

| Command | Meaning | Origin |
|---|---|---|
| `209 <pkg> install` | Install a package | pacman |
| `209 <pkg> remove` | Remove a package | pacman |
| `209 <pkg> info` | Show package info | pacman |
| `209 <term> search` | Search repos | pacman |
| `209 <pkg> aur build` | Build from AUR | paru |
| `209 <term> aur search` | Search AUR | paru |
| `209 <pkg> aur review` | Review PKGBUILD diff | paru |
| `209 apply` | Apply declarative config | new |
| `209 <gen> rollback` | Roll back to generation | new |
| `209 generations` | List generations | new |
| `209 sync` | Sync repo databases | pacman |
| `209 news` | Show Arch news | paru |

**Special subjects:** `apply`, `generations`, `sync`, `news` are zero-argument
commands — they have no subject, only a verb. These are the exceptions that
prove the rule: they operate on the system as a whole, not on a named thing.

**Multi-subject:** `209 nginx firefox install` installs both. The verb comes last,
applied to everything before it.

Backwards-compatible flags are preserved so existing muscle memory and scripts
keep working. Scope (global vs user) is selected by flag / config (§7).

---

## 6. The libalpm integration seam — copied & modified in-tree

We don't fork pacman upstream and we don't build against it untouched. We do
something in between: **copy the source into our tree and modify it directly**.

**What we do:**
- pacman's source is pulled into the repo once via `git subtree add` from
  `https://gitlab.archlinux.org/pacman/pacman`, landing under
  `subprojects/pacman/`. From that point it's **our copy**, committed and version-
  controlled alongside 2O9's own code.
- It builds a static `libalpm.a` (and the `makepkg` / database utilities we need).
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
  list below) and record each touch in `subprojects/pacman/MODIFICATIONS.md`.

**What "no fork" still means:**
- We are **not** publishing a competing pacman binary or maintaining pacman as a
  separate project. The modified libalpm is an internal build artifact of 2O9,
  consumed only by 2O9.

**The modification targets (refined in Phase 1, recorded in MODIFICATIONS.md):**
- The package-extract path: dispatch to 2O9's store adapter instead of libalpm's
  builtin extractor, so files land in `/nix/store`.
- A "fake installed view" fed to the solver, sourced from the generation DB, so
  libalpm sees current state without anything being extracted to `/`.

---

## 7. Configuration: global and per-user

Both scopes are first-class. This is settled by decision #6.

### Paths

| Scope | Config file | Profile symlink | Generation DB |
|---|---|---|---|
| **Global (system)** | `/etc/2O9/config.toml`, `/etc/2O9/2O9.nix` | `/nix/var/nix/profiles/per-user/2O9-system` | `/var/lib/2O9` |
| **Per-user** | `~/.config/2O9/config.toml`, `~/.config/2O9/home.nix` | `~/.local/state/2O9/profile` (a user-owned Nix profile) | `~/.local/state/2O9` |

Per-user profiles use Nix's user-profile mechanism so unprivileged installs work
without root, mirroring `nix profile` semantics.

### Merge order (most-specific wins)

```
built-in defaults
  → /etc/2O9/config.toml        (global)
    → ~/.config/2O9/config.toml (user)
      → CLI flags
```

### Two file kinds per scope

- **`config.toml`** — *imperative tool settings*: AUR RPC URL, review policy,
  clean-after, news source. Successor to `paru.conf`. **Not** the successor to
  `pacman.conf` — pacman's content is declarative state, so it lives in the Nix
  file (see below).
- **`*.nix`** — the *declarative manifest* (the file is conventionally named
  `2O9.nix`). Everything that is system **state**, not tool behavior: the package
  list, the AUR list, build optimization flags, services, **and the full
  `pacman.conf` content** (`[options]` like `SigLevel`/`ParallelDownloads`/
  `IgnorePkg`, plus `[repo]` sections with their server URLs). Evaluated by
  `nix eval --json` into the manifest the reconciler consumes. There is **no
  `pacman.conf` file of any kind** — the engine feeds the `pacman` block directly
  into libalpm's in-memory API.

The split principle: **tool behavior** → `config.toml`; **what the system is** →
`2O9.nix`. `pacman.conf` is "what the system is", so it's declarative — and the
file it lives in *is* `2O9.nix`, with no config file ever written to disk.

The imperative CLI (`209 foo install`) and the declarative path (`209 apply`)
coexist: an imperative install is recorded as a layer on top of the current
generation, so the system stays declarative-by-default but ad-hoc changes don't
get lost.

---

## 8. Worked example — `209 apply`

```
$ 209 apply
 1. nix eval 2O9.nix     ──►  manifest JSON
 2. reconcile(manifest, current-gen)  ──►  { +neovim, -old-editor, +chromium(AUR) }
 3. libalpm resolves neovim deps      ──►  transaction plan
 4. store adapter: pkg.tar.zst ──► /nix/store/x3f-neovim-0.9.5
    symlink: /nix/store/x3f-neovim-0.9.5/bin/nvim → /usr/bin/nvim
 5. AUR helper: chromium PKGBUILD → review → makepkg (with CFLAGS from 2O9.nix) → pkg.tar.zst
 6. store adapter: ──► /nix/store/a91-chromium-120
    symlink: /nix/store/a91-chromium-120/bin/chromium → /usr/bin/chromium
 7. commit generation #42  (profile symlink swapped)
 8. /usr/bin/nvim and /usr/bin/chromium now point into the store
✓ done.  rollback with: 209 41 rollback
```

Imperative equivalent — the same operations, one package at a time:

```
$ 209 neovim install
$ 209 chromium aur build
$ 209 41 rollback
```

Each step is independently testable. Step 4–6 are the store adapter + symlink
farm work.

---

## 9. Repo & build structure

```
2O9/                              # GPL-2.0-only
├── meson.build                   # top-level, pulls in subprojects
├── subprojects/
│   └── pacman/                   # pacman source, copied in (git subtree) & modified
│       └── MODIFICATIONS.md      # log of every 2O9 change to the vendored tree
├── src/
│   ├── cli/                      # unified front-end (C) — builds the `209` binary
│   ├── aur/                      # paru logic, ported to C  (NEW)
│   ├── declarative/              # reconcile engine        (NEW)
│   └── store/                    # store adapter           (NEW)
├── lib/2O9/                      # shared internal library (config, nix-spawn, db)
├── scripts/                      # hooks, makepkg wrappers, activation
├── test/                         # unit + integration + generation-rollback tests
└── docs/
```

Build system: **meson + ninja** (what pacman already uses — minimises friction
with the copied-in tree under `subprojects/pacman/`).

### External build/runtime dependencies

| Dep | Used by | Notes |
|---|---|---|
| `alpm` (copied-in & modified) | core | static `libalpm.a` built from `subprojects/pacman/` |
| `libcurl` | AUR helper | AUR RPC |
| `libgit2` | AUR helper | clone PKGBUILDs |
| `cJSON` | AUR helper, declarative | JSON parse/emit |
| `nix` (the tool, C++) | store adapter | **subprocess only** — never linked |
| `makepkg` | AUR helper | invoked as subprocess |
| `sqlite` (optional) | store adapter | generation DB (or plain files) |

---

## 10. Phased roadmap (risk-first)

Phase 1 is the make-or-break. If the store adapter can't cleanly remap libalpm's
install backend, the "full Nix store on pacman" premise needs rethinking — which
is exactly why it's first.

| Phase | Goal | Exit criterion | Risk |
|---|---|---|---|
| **0 — Foundation** | Repo, meson build, copy pacman into `subprojects/`, C→`nix` subprocess spike, name/license settled | `209 -V` builds; one C program can `nix-store --add` a file | Low |
| **1 — Store adapter MVP** | One binary package → store → symlink farm; one profile; one rollback | `209 sl install` → store path + symlink; `209 41 rollback` restores | **HIGH** |
| **2 — paru → C port** | AUR RPC, clone, review, makepkg, into the store; build optimization flags | `209 <pkg> aur build` works end-to-end with custom CFLAGS | Medium |
| **3 — Declarative engine** | `nix eval` manifest → reconcile → transaction → generations | `209 apply` from `2O9.nix`, reproducibly | Medium |
| **4 — Polish** | Unified CLI, hooks, user scope, docs, packaging | distro-installable, documented | Medium |

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
- **Hooks & `/etc` conffiles.** pacman runs install scripts and manages `/etc`.
  In a store model these need a strategy (NixOS-style activation scripts). Deferred
  to Phase 4 but flagged now.
- **Symlink conflicts.** Two packages shipping the same path (e.g. both install
  `/usr/bin/foo`) is a conflict in the symlink farm, just as it is in pacman
  today. The store makes it visible earlier (at generation-commit time) rather
  than at install time, which is actually better — but it still needs a conflict
  resolution strategy.
- **Scope realism.** "Full Nix store on pacman" is a multi-year, team-scale effort.
  The plan is structured so each phase produces something useful on its own.
