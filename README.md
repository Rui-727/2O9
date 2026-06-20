# 2O9

**2O9** (stylized name; the binary is **`209`**) is a unified package manager
for Arch Linux that puts files in `/nix/store/`. It combines three things
into one tool:

1. **pacman's engine** — libalpm, modified in-tree as part of **lib2O9**:
   dependency resolution, repository sync, database parsing, and hooks. The
   solver reads from the generation DB instead of `/var/lib/pacman/local/`;
   the install backend dispatches to the store adapter.
2. **paru's AUR workflow** — rewritten in C: AUR RPC queries, PKGBUILD clone
   and review, recursive AUR dependency resolution, and `makepkg` orchestration.
3. **A real `/nix/store`** — predictable store paths (`/nix/store/<name>-<version>/`,
   no content hash) with atomic generations, driven by a **declarative** Nix-syntax
   configuration (`2O9.nix`) describing desired system and per-user state. The
   Nix evaluator is **written from scratch in C** as part of lib2O9 — not a vendored
   copy of the C++ nix source. It supports the function form (`{ config, ... }: ...`)
   with fixed-point recursion for self-reference, and `import` for splitting configs
   across multiple files.

Plus **Trakker** — a ptrace-based execution sandbox: syscall tracing, network
and write blocking, file-write redirection, JSON trace output.
`209 <cmd> trakker --no-net`.

Rollback is just a symlink swap + reboot. No boot-time rollback machinery, no
daemon. Activation repoints a generation symlink; the user reboots for the new
system state to take full effect. Services are managed via `systemctl enable`.

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
make                # builds 209 and test-aur-rpc
make install        # installs 209 to /usr/bin
make install PREFIX=/usr/local  # custom prefix
```

Requirements: a C compiler, make, libcurl-dev. To build lib2O9 you also need
libarchive-dev and openssl-dev. The Nix evaluator is our own C code — no
C++ deps needed.

## Repo structure

```
2O9/
├── Makefile              # build system
├── lib/2O9/              # lib2O9 — one static library
│   ├── alpm/             #   modified libalpm (from pacman)
│   ├── nix/              #   own C Nix evaluator (from scratch)
│   └── common/           #   shared utils
├── src/
│   ├── cli/              # 209 binary (SOV command dispatch)
│   ├── aur/              # AUR helper (paru rewritten in C)
│   ├── declarative/      # generation DB, reconcile engine
│   ├── store/            # store adapter (nix-store subprocess)
│   └── trakker/          # sandbox + trace recorder
├── DESIGN.md             # full architecture doc
└── LICENSE               # GPL-2.0-only
```

## Status

Phase 0–3 in progress. The 209 binary handles install, rollback, generations,
pinning, and the full AUR pipeline (search, info, clone, review, resolve
deps, makepkg, install to store). AUR RPC client works. Dependency
resolver classifies deps into repo vs AUR. Build optimization via
CFLAGS/CXXFLAGS/LDFLAGS env vars. **Phase 3 — Declarative Engine** has its
core Nix evaluator working: the evaluator parses Nix expressions, produces
JSON manifests, supports import resolution and fixed-point recursion for
config self-reference. `209 apply` evaluates `2O9.nix` and commits
generations. 19 builtins are registered. The parser still needs binary
operator precedence levels and lambda formals parsing for full 2O9.nix
support. See [`DESIGN.md`](./DESIGN.md) for the phased roadmap.

## License

GPL-2.0-only. See [`LICENSE`](./LICENSE).
