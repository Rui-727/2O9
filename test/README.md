# test/

Integration tests for 2O9. Unit tests for individual modules live
next to the code they test:

- `lib/2O9/nix/test_nix_eval.c` — Nix evaluator (49 tests)
- `lib/2O9/nix/test_nix_lexer.c` — Nix lexer
- `src/aur/test_aur_rpc.c` — AUR RPC client

The scripts in this directory exercise multiple components together —
`209 apply` end-to-end, generation rollback, the Trakker sandbox, etc.

## Running

```sh
make test              # everything: unit + integration
make test-nix-eval     # just the evaluator unit tests
make test-nix-lexer    # just the lexer
make test-aur-rpc      # just the AUR RPC client
```

Or run a single integration test directly:

```sh
./test/test_apply.sh ./209
./test/test_rollback.sh ./209
./test/test_trakker.sh ./209
./test/test_nix_eval_e2e.sh ./209
```

## What each test does

| Test | What it checks |
|---|---|
| `test_apply.sh` | Creates a fake 2O9.nix + store path, runs `209 apply`, verifies a generation is committed with the right packages in the manifest |
| `test_rollback.sh` | Creates two fake generations, rolls back between them, verifies the `current` symlink points to the right one |
| `test_nix_eval_e2e.sh` | Runs `209 apply` with a 2O9.nix that uses self-reference + conditional + let bindings; verifies evaluation succeeds |
| `test_trakker.sh` | Runs `echo` in the sandbox with `--no-net`, runs `touch` with `--no-write` and verifies the write is blocked |

Tests are designed to run in a sandbox — they create their own temp
directories and clean up after themselves. Some warnings are expected
(no real package source, no root for systemctl, etc.); the tests use
`OK`/`WARN` prefixes so you can tell what matters.
