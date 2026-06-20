# 2O9

**2O9** (stylized name; the binary is **`209`**) is a unified package manager
that combines four things into one tool:

1. **pacman's engine** — libalpm, modified in-tree into **lib209**: dependency
   resolution, repository sync, database parsing, and hooks. The solver reads
   from the generation DB instead of `/var/lib/pacman/local/`; the install
   backend dispatches to the store adapter.
2. **paru's AUR workflow** — rewritten in C: AUR RPC, PKGBUILD review, recursive
   AUR dependency resolution, and `makepkg` orchestration.
3. **A real `/nix/store`** — content-addressed storage with atomic generations,
   driven by a **declarative** (Nix-syntax) configuration describing desired
   system and per-user state.
4. **Trakker** — execution sandbox and trace recorder: ptrace-based syscall
   tracing, network/write blocking, file-write redirection, and JSON trace
   output. Run any command sandboxed: `209 <cmd> trakker --no-net`.

Rollback is just a symlink swap — there is **no boot-time rollback machinery**.
Activation repoints a generation symlink; that's it.

## Naming

| | Form | Where |
|---|---|---|
| Binary / command | `209` (numeric) | what you type |
| Project / branding | `2O9` (letter O) | docs, repo name, on-disk paths (`/etc/2O9/`, `/var/lib/2O9/`) |
| Modified libalpm | `lib209` | internal static library, never published separately |

## Status

Design phase. See [`DESIGN.md`](./DESIGN.md) for the full architecture, the
lib209 integration strategy, and the phased roadmap.

## License

GPL-2.0-only. See [`LICENSE`](./LICENSE).
