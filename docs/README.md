# docs/ — extended 2O9 documentation

This directory holds extended documentation that doesn't fit in the
top-level `README.md` or `DESIGN.md`.

## Planned documents

| File | Topic |
|---|---|
| `MANPAGE.md` | The `209` man page — full command reference |
| `CONFIG.md` | `2O9.nix` schema reference — all config keys, types, defaults |
| `ACTIVATION.md` | How the 9-step activation phase works, how to extend it |
| `LIB2O9.md` | lib2O9 internals — modified libalpm + own C Nix evaluator |
| `TRAKKER.md` | Trakker sandbox internals — ptrace, JSON trace format |
| `CONTRIBUTING.md` | How to contribute — code style, commit conventions, PR flow |
| `RELEASE.md` | Release process — versioning, changelog, packaging |

Status: skeleton. Documents land as Phase 5 polish work. Until then,
the primary references are:

- [`README.md`](../README.md) — overview, build, commands, status
- [`DESIGN.md`](../DESIGN.md) — full architecture (950 lines)
- [`lib/2O9/nix/README.md`](../lib/2O9/nix/README.md) — Nix evaluator design
- [`lib/2O9/alpm/MODIFICATIONS.md`](../lib/2O9/alpm/MODIFICATIONS.md) — libalpm modification log
