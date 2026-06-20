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
3. **A real `/nix/store`** — content-addressed storage with atomic generations,
   driven by a **declarative** Nix-syntax configuration (`2O9.nix`) describing
   desired system and per-user state. The Nix evaluator is also copied into
   lib2O9, modified to work without the full Nix daemon.

Plus **Trakker** — a ptrace-based execution sandbox: syscall tracing, network
and write blocking, file-write redirection, JSON trace output.
`209 <cmd> trakker --no-net`.

Rollback is just a symlink swap. No boot-time rollback machinery, no daemon.
Activation repoints a generation symlink; that's it.

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
| Merged internal library | `lib2O9` | static `lib2O9.a` — modified libalpm + modified Nix evaluator |

## Build

```sh
make                # builds 209 and test-aur-rpc
make install        # installs 209 to /usr/bin
make install PREFIX=/usr/local  # custom prefix
```

Requirements: a C compiler, make, libcurl-dev. To build lib2O9 you also need
libarchive-dev and openssl-dev. The Nix evaluator (C++) needs its own deps —
see `lib/2O9/nix/`.

## Repo structure

```
2O9/
├── Makefile              # build system
├── lib/2O9/              # lib2O9 — one static library
│   ├── alpm/             #   modified libalpm (from pacman)
│   ├── nix/              #   modified Nix evaluator
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

Phase 0–1 complete. The 209 binary handles install, rollback, generations,
and pinning. AUR RPC client works. lib2O9 (alpm + nix evaluator) is in tree
but not yet compiled — needs libarchive-dev for the alpm half and C++ deps
for the nix half. See [`DESIGN.md`](./DESIGN.md) for the phased roadmap.

## License

GPL-2.0-only. See [`LICENSE`](./LICENSE).
