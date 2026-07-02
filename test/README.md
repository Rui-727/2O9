# test/ — integration and end-to-end tests

This directory holds 2O9's integration and end-to-end test suite. Unit
tests for individual modules live alongside the modules they test:

- `lib/2O9/nix/test_nix_lexer.c` — Nix lexer unit tests
- `lib/2O9/nix/test_nix_eval.c` — Nix evaluator unit tests (49 tests)
- `src/aur/test_aur_rpc.c` — AUR RPC client unit tests

The integration tests in this directory exercise multiple components
together — e.g. `209 apply` end-to-end, generation commit + rollback,
AUR build + store adapter + symlink farm.

## Planned tests

| File | What it tests |
|---|---|
| `test_apply.sh` | `209 apply` from a 2O9.nix fixture, verify generation committed |
| `test_rollback.sh` | Install → rollback → verify symlink farm reverted |
| `test_aur_build.sh` | `209 <pkg> aur build` against mock AUR server |
| `test_trakker.sh` | `209 <cmd> trakker --no-net` produces JSON trace |
| `test_nix_eval_e2e.sh` | `209 apply` with a real 2O9.nix fixture (self-ref, imports) |

## Running tests

```sh
make test           # run all unit + integration tests (planned)
make test-nix-eval  # run just the Nix evaluator unit tests
make test-nix-lexer # run just the Nix lexer unit tests
make test-aur-rpc   # run just the AUR RPC unit tests
```

Status: skeleton. The `make test` target is not yet wired up; integration
tests land as part of Phase 5 polish.
