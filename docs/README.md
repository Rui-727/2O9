# docs/

Long-form documentation that doesn't fit in the README or DESIGN.md.

## What's here

| File | What it covers |
|---|---|
| [`TUTORIAL.md`](./TUTORIAL.md) | Linear walkthrough: install, first config, first apply, first rollback, first AUR build, first GC, first substituter |
| [`COOKBOOK.md`](./COOKBOOK.md) | 20 self-contained recipes for common tasks (install, search, build, roll back, cache, sandbox) |
| [`USE_CASES.md`](./USE_CASES.md) | Longer writeups of how 2O9 fits real workflows (developer workstation, fleet, AUR power user, air-gap, CI, untrusted review) |
| [`MIGRATION.md`](./MIGRATION.md) | Coming from pacman, paru, Nix, or NixOS: command mappings and conceptual diffs |
| [`MANPAGE.md`](./MANPAGE.md) | The `209` man page: every command, option, and file path |
| [`CONFIG.md`](./CONFIG.md) | `2O9.nix` and `extra.nix` schema reference, both Nix, with examples |

## Where to look for other things

- [`README.md`](../README.md): overview, quick start, build, status
- [`DESIGN.md`](../DESIGN.md): full architecture, the why behind everything
- [`lib/2O9/nix/README.md`](../lib/2O9/nix/README.md): Nix evaluator design and supported features
- [`lib/2O9/alpm/MODIFICATIONS.md`](../lib/2O9/alpm/MODIFICATIONS.md): what we changed in vendored libalpm and why
- [`PKGBUILD`](../PKGBUILD): self-hosting packaging for 2O9 itself
