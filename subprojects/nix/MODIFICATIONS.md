# 2O9 Modifications to vendored Nix evaluator

This is the Nix expression evaluator (from NixOS/nix), copied into the
2O9 tree and modified directly. Every change is marked with
`/* 2O9: <reason> */` in the source.

## What we kept

- `src/libutil/` — Foundation library (hash, strings, error, logging)
- `src/libstore/` — StorePath, Derivation, Store API (required by libexpr)
- `src/libexpr/` — The evaluator: parser, lexer, EvalState, primops, values
- `src/libfetchers/` — Partial (attrs, fetchers, cache, tarball)
- `nix-meson-build-support/` — Shared meson build infrastructure

## What we deleted

- `src/nix/` — CLI (2O9 has its own CLI)
- `src/libcmd/` — CLI command framework
- `src/libmain/` — CLI main entry
- `src/libflake/` — Flake system (2O9 has its own package model)
- `src/*-c/` — C API wrappers (not needed for C++ integration)
- `src/*-tests/` — Test suites
- `tests/`, `doc/`, `contrib/`, `scripts/`, `packaging/` — all removed

## Modification targets

1. **Two9Store**: Create a custom `Store` subclass that:
   - Returns 2O9's store directory (`/nix/store`)
   - `writeDerivation()` → records derivation data in 2O9's manifest format
   - `isValidPath()` → asks 2O9's generation DB
   - Stubs out network/GC/daemon operations

2. **EvalState construction**: Inject `Two9Store` instead of a real Nix store.
   The evaluator works unmodified — it just talks to our store.

3. **prim_derivationStrict**: Produce derivation data without requiring
   `store->writeDerivation()` to write `.drv` files. Output to 2O9's
   manifest JSON instead.

4. **Remove unnecessary primops**: fetchClosure, fetchMercurial — 2O9
   doesn't need them. Stub or delete.

## Change log

No modifications applied yet. Phase 3 will produce the first changes.
