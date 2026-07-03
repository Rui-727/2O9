# test/

Integration tests for 2O9. Unit tests for individual modules live
next to the code they test:

- `lib/2O9/nix/test_nix_eval.c` - Nix evaluator (49 tests)
- `lib/2O9/nix/test_nix_eval_edge.c` - Nix evaluator edge cases (17 tests)
- `lib/2O9/nix/test_nix_lexer.c` - Nix lexer
- `src/aur/test_aur_rpc.c` - AUR RPC client
- `src/store/test_nar.c` - NAR serialisation (27 tests)
- `src/store/test_db.c` - SQLite store DB / refs graph (25 tests)
- `src/store/test_signing.c` - Ed25519 signing + base64 (23 tests)
- `src/store/test_narinfo.c` - narinfo parse / serialize (51 tests)
- `src/store/test_keygen.c` - `209 keygen` round-trip helper (used by test_cache_keygen_roundtrip.sh)

The scripts in this directory exercise multiple components together -
`209 apply` end-to-end, generation rollback, the Trakker sandbox, etc.

## Running

```sh
make test              # everything: unit + integration
make test-nix-eval     # just the evaluator unit tests
make test-nix-eval-edge  # just the evaluator edge cases
make test-nix-lexer    # just the lexer
make test-aur-rpc      # just the AUR RPC client (needs network)
make test-nar          # NAR serialisation
make test-db           # SQLite refs graph
make test-signing      # Ed25519 + base64
make test-narinfo      # narinfo parse / serialize
make test-keygen       # `209 keygen` round-trip helper
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
| `test_apply_empty_config.sh` | `209 apply` with `{ packages = []; }` - empty config evaluates, commits a generation with empty manifest, no crash |
| `test_apply_self_reference_cycle.sh` | `2O9.nix` with self-referential conditional - fixed-point recursion terminates (converging case succeeds; pathological toggle fails with clear error, no hang) |
| `test_rollback.sh` | Creates two fake generations, rolls back between them, verifies the `current` symlink points to the right one |
| `test_rollback_to_missing.sh` | `209 999 rollback` where gen 999 doesn't exist - clear error, non-zero exit, no crash, `current` symlink unchanged |
| `test_nix_eval_e2e.sh` | Runs `209 apply` with a 2O9.nix that uses self-reference + conditional + let bindings; verifies evaluation succeeds |
| `test_nix_eval_edge_cases.sh` | Drives `test-nix-eval-edge` through 17 Nix edge cases: empty list/attrset, nested let, curried lambda, inherit, with shadowing, builtins.map, string interpolation |
| `test_trakker.sh` | Runs `echo` in the sandbox with `--no-net`, runs `touch` with `--no-write` and verifies the write is blocked |
| `test_bundle.sh` | Bundle / import round-trip on a fake generation |
| `test_cache.sh` | `209 cache` dispatch + `--help` shows `--keep` / `--dry-run` |
| `test_cache_keygen_roundtrip.sh` | `209 keygen` output parses, pub+sec base64-decode to 32 bytes, sign + verify round-trip via `test-keygen` helper, `--name`/`--out` flags work |
| `test_diff_why_lock.sh` | `209 diff`, `209 why`, `209 lock --export` / `--import`, `209 upgrade`, `209 -Su` |
| `test_debag.sh` | `209 debag static-scan` on `/bin/ls`, run-under-debag, fast mode, error paths |
| `test_script_analysis.sh` | `209 debag` help mentions install scripts, static-scan on `/bin/sh` detects exec syscalls |
| `test_pacman_flags.sh` | `-S` `-Q` `-Qs` `-Qi` `-Qm` `-R` `-Ss` `-Si` `-Sy` dispatch and produce expected output |
| `test_gc_closure.sh` | Store DB closure logic (via `test-db`) preserves transitive deps; `209 gc` itself requires root (skipped as non-root) |
| `test_optimise_dedup.sh` | `209 optimise` hardlinks identical files; requires root (skipped as non-root) |
| `test_install_sig_required_missing.sh` | `SigLevel = "Required"` install with missing `.sig` is refused (not silently proceeding); requires root (skipped as non-root, but signing crypto verified via `test-signing`) |

## Edge case tests

These tests focus on corner cases rather than the happy path. They
live alongside the happy-path tests above and run as part of `make test`.

**Nix evaluator edge cases** (`test/test_nix_eval_edge_cases.sh`):
Empty containers, nested let, with shadowing, builtins.map, string
interpolation with builtins.toString, missing-attribute error path.
Driven by `lib/2O9/nix/test_nix_eval_edge.c`.

**Apply edge cases**:
- `test_apply_empty_config.sh` - empty package list, no crash.
- `test_apply_self_reference_cycle.sh` - fixed-point recursion caps
  at 100 iterations and either converges or fails with a clear
  "did not converge" error. Verified under a 30s timeout so a
  regression that hangs the evaluator fails the test fast.

**Rollback edge cases**:
- `test_rollback_to_missing.sh` - rolling back to a nonexistent
  generation produces a clear error, exits non-zero, doesn't crash,
  and leaves the `current` symlink unchanged.

**Signing edge cases** (in `src/store/test_signing.c`):
Tampered fingerprint, tampered signature, wrong public key, empty
input to base64, 32-byte input, 64-byte input, 100-byte non-aligned
input, raw-hex fingerprint prefixing.

**narinfo edge cases** (in `src/store/test_narinfo.c`):
Minimal narinfo (optional fields NULL), full narinfo, round-trip
parse+serialize, multiple References, multiple Sig: lines, malformed
(missing StorePath) returns NULL, empty References: doesn't crash.

**NAR edge cases** (in `src/store/test_nar.c`):
Empty dir, single regular file, executable file, symlink, nested
dirs - each verified against a hardcoded expected hash computed
independently from the documented wire format. Plus: sort-order
determinism (same contents, different creation order = same hash),
round-trip extract(dump(tree)) preserves contents/modes/symlink
targets, and a 1.5 MB file isn't truncated at the 64 KiB chunk size.

**SQLite refs graph edge cases** (in `src/store/test_db.c`):
Idempotent re-registration, transitive closure, cycles don't
infinite-loop, dead-paths with empty closure returns all paths,
unregister cascades to refs.

**GC / optimise / install-sig** (require root):
- `test_gc_closure.sh` - `209 gc` doesn't delete transitive deps.
  As non-root: closure logic verified via `test-db`; the `209 gc`
  run is skipped with a clear message.
- `test_optimise_dedup.sh` - `209 optimise` hardlinks identical
  files. As non-root: skipped (the dedup code writes to
  `/nix/store/.links/`).
- `test_install_sig_required_missing.sh` - `SigLevel = "Required"`
  install with missing `.sig` is refused. As non-root: the install
  is blocked at the privilege check; the signing crypto is verified
  via `test-signing`.

Tests are designed to run in a sandbox - they create their own temp
directories and clean up after themselves. Some warnings are expected
(no real package source, no root for systemctl, etc.); the tests use
`OK`/`WARN`/`SKIP` prefixes so you can tell what matters. Tests that
need root print `SKIP` and exit 0 when run as a non-root user, so
`make test` stays green in CI.
