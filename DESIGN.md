# 2O9 — Design Document

**Working title:** 2O9 (stylized; the binary is `209`)
**License:** GPL-2.0-only (inherited from pacman, the engine we link against)
**Scope of this document:** architecture and integration strategy for combining
pacman's libalpm, a C rewrite of paru's AUR workflow, and a real `/nix/store`
with declarative configuration — into a single tool.

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

## 3. The core problem

pacman and Nix have **opposite** filesystem models. This is the single hardest
problem and the design is built around solving it.

| | pacman | Nix |
|---|---|---|
| Install target | `/` (root overlay) | `/nix/store/<hash>-name` |
| Bookkeeping | `/var/lib/pacman/local/*` (files at canonical paths) | store paths + symlink profiles |
| Multiple versions coexist | No (conflict = error) | Yes (distinguished by hash) |
| Rollback | Manual / snapshots | Atomic generation switch |

If files go to `/nix/store`, pacman's local-db bookkeeping breaks — it expects
`/usr/bin/firefox`, not `/nix/store/abc-firefox/bin/firefox`. If we lie to libalpm
and keep extracting to `/`, we lose everything the store buys us.

### The resolution: a store adapter behind libalpm

We treat libalpm as the **solver + metadata + hooks engine** and intercept its
**install backend**. Instead of extracting a `.pkg.tar.zst` to `/`:

1. Extract to a staging directory.
2. Compute a content-addressed hash over the unpacked tree (gives reproducibility
   *by content* — note Arch pkgs are **not** Nix derivations, so we get
   content-addressing, not Nix's deep input purity; see §11).
3. Register the path in `/nix/store` via `nix-store --add-fixed`.
4. Record the mapping `pacman-pkg → store-path` in 209's **generation database**.
5. Repoint the active **profile symlink** to a generation that union-symlinks the
   constituent store paths into view.

The pacman local DB is **kept but repurposed**: it continues to hold package
metadata (deps, version, files-list) so pacman/AUR tooling stays sane, but its
"installed path" semantics now point into the store.

---

## 4. Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Unified CLI  (C)                                         │
│  install | remove | search | aur | apply | rollback |    │
│  generations | sync | news                                │
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
│  content-hash → nix-store --add → profile → generation   │
│  (orchestrates the `nix` binary via subprocess / JSON)   │
└──────────────────────────────────────────────────────────┘
        │ subprocess / JSON
        ▼
   real /nix/store + nix daemon  (external C++ dependency)
```

Five components:

1. **Unified CLI** — entrypoint, dispatch.
2. **Declarative Engine** — turns config into a transaction.
3. **AUR Helper** — builds packages that aren't in binary repos.
4. **libalpm** (copied-in, modified pacman tree) — solver + metadata + hooks.
5. **Store Adapter** — the keystone; bridges libalpm's install path to the store.

---

## 5. Component designs

### 5.1 Store Adapter (C) — the keystone

The adapter owns the boundary between libalpm's filesystem model and Nix's. It
has one job: take a package and make it appear in the store, then in a profile.

- **`stage_and_register(pkg, files)`**
  Extract `.pkg.tar.zst` to a staging dir → compute tree hash →
  `nix-store --add-fixed --recursive sha256 <staging>` → returns the store path.
  Idempotent: if the path already exists in the store, skip (dedup by content).

- **Profile manager**
  A **generation** = an ordered set of store paths + a symlink-farm manifest
  (the union view). `commit_generation()` atomically swaps the active profile
  symlink via `rename(2)` — that's the rollback primitive.

- **Mapping table** (under `/var/lib/2O9`, format TBD between SQLite and a plain
  directory of files): `pkgname-version → store-path → generation-id`. (Path uses
  the stylized name; the command you run is `209`.)

- **Rollback** = pick a prior generation id, repoint the profile symlink. Instant,
  because files never move. There is no "uninstall then reinstall"; the symlink
  just points elsewhere.

- **Nix interop** is exclusively subprocess: `nix-store --add-fixed`,
  `nix-store --realise`, `nix-store --query`, `nix profile`, `nix-collect-garbage`.
  In/out is JSON or line-oriented stdout. We never `dlopen` or link Nix's C++.

### 5.2 AUR Helper — paru ported to C

Paru's responsibilities, mapped onto C libraries:

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

The `pacman` block **is** `2O9.nix` — there is no `pacman.conf`, not even a
generated one. `[options]` and `[repo]` sections are expressed as Nix
attributes, and on `209 apply` the declarative engine feeds them **directly into
libalpm's in-memory API** (`alpm_option_set`, `alpm_db_register_sync`, etc.).
libalpm is never pointed at a config file; it is configured programmatically from
the manifest, so the declaration is the single source of truth with nothing to
drift out of sync.

The **reconciler** diffs manifest ↔ current generation DB and produces a
transaction:

```
{ install: [...], remove: [...], aur_build: [...],
  pacman_options_changed: bool, services_on: [...], services_off: [...] }
```

The transaction is executed through libalpm (configured in-memory from the
manifest — no `pacman.conf` file) + the AUR helper + the store adapter.
On success → `commit_generation()`. On failure → abort; the profile is untouched,
so the system stays consistent. Because generations are symlinks, a half-applied
transaction simply never gets its generation committed.

### 5.4 Unified CLI

pacman's `src/pacman/` frontend is the base for the CLI. The **binary is `209`**
(numeric); the project/branding name is 2O9. Layered verbs:

| Verb | Origin | Scope |
|---|---|---|
| `install` / `remove` / `sync` / `search` / `info` | pacman | both |
| `aur` (search/build/review) | paru | both |
| `apply` | new (declarative) | both |
| `rollback <gen>` | new | both |
| `generations` | new | both |
| `news` | paru | both |

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

- **`config.toml`** — *imperative tool settings*: AUR RPC URL, makepkg flags,
  review policy, clean-after, news source. Successor to `paru.conf`. **Not** the
  successor to `pacman.conf` — pacman's content is declarative state, so it lives
  in the Nix file (see below).
- **`*.nix`** — the *declarative manifest* (the file is conventionally named
  `2O9.nix`). Everything that is system **state**, not tool behavior: the package
  list, the AUR list, services, **and the full `pacman.conf` content** (`[options]`
  like `SigLevel`/`ParallelDownloads`/`IgnorePkg`, plus `[repo]` sections with
  their server URLs). Evaluated by `nix eval --json` into the manifest the
  reconciler consumes. There is **no `pacman.conf` file of any kind** — the engine
  feeds the `pacman` block directly into libalpm's in-memory API.

The split principle: **tool behavior** → `config.toml`; **what the system is** →
`2O9.nix`. `pacman.conf` is "what the system is", so it's declarative — and the
file it lives in *is* `2O9.nix`, with no config file ever written to disk.

The imperative CLI (`209 install foo`) and the declarative path (`209 apply`)
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
 5. AUR helper: chromium PKGBUILD → review → makepkg → pkg.tar.zst
 6. store adapter: ──► /nix/store/a91-chromium-120
 7. commit generation #42  (profile symlink swapped)
 8. profile now union-symlinks neovim + chromium into view
✓ done.  rollback with: 209 rollback 41
```

Each step is independently testable. Step 4 + 7 are the risky ones.

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
| **1 — Store adapter MVP** | One binary package → store; one profile; one rollback | `209 install sl` → store path; `209 rollback` restores | **HIGH** |
| **2 — paru → C port** | AUR RPC, clone, review, makepkg, into the store | `209 aur <pkg>` works end-to-end | Medium |
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
- **Solver/view coherence.** libalpm's solver must see a coherent "installed" set
  drawn from the generation DB, not `/var/lib/pacman/local`. The fake-view
  (§6) is the load-bearing piece; if it can't be made reliable, the whole approach
  is in question. This is the Phase 1 risk.
- **Scope realism.** "Full Nix store on pacman" is a multi-year, team-scale effort.
  The plan is structured so each phase produces something useful on its own.
