# 2O9 Nix Evaluator

This is our own C implementation of the Nix expression language - not a
vendored copy of the C++ nix source. It implements just enough of the
language to evaluate `2O9.nix` config files: the function form with
self-reference, imports, attrsets, lists, lambdas, the usual builtins.

## What we implement

The features `2O9.nix` actually needs:

- **Attribute sets** - `{ key = value; ... }` and recursive attrsets `rec { ... }`
- **Lists** - `[ elem1 elem2 ... ]`
- **Let-bindings** - `let ... in ...`
- **String interpolation** - `"hello ${name}"`
- **Function application** - `f x`, `f { ... }`
- **Lambda functions** - `x: body`, `{ ... }: body`
- **Function form with config** - `{ config, pkgs, ... }: { ... }` - the
  standard NixOS pattern. The `config` argument enables self-reference via
  fixed-point recursion.
- **Fixed-point recursion** - `config` is the result of evaluating the function
  itself. The evaluator resolves this lazily so `config.services.sshd.enable`
  can be read from within the same config.
- **Import/include** - `import ./packages.nix` reads and evaluates another
  Nix file. All imports are packed into one evaluation unit. Supports
  relative paths (relative to the importing file).
- **With-expressions** - `with pkgs; [ firefox neovim ]`
- **Conditional expressions** - `if cond then a else b`
- **Comments** - `# line` and `/* block */`
- **Primitive operations** - string concatenation, list concatenation, attribute
  selection (`a.b`), `assert`, `throw`

## What we do NOT implement

These Nix features are not needed for 2O9's config evaluation:

- **Derivations** - 2O9 has its own build model (Arch .pkg.tar.zst)
- **Fetchers** - no `fetchTarball`, `fetchGit`, `fetchUrl` (no network during eval)
- **Flakes** - 2O9 has its own package model
- **Full Nixpkgs** - we don't evaluate nixpkgs; we evaluate a small config schema
- **Nix store integration** - evaluation is pure; store operations are separate

Note: `builtins.pathExists` and `builtins.readFile` ARE supported (needed for
import resolution). Only network-based builtins are excluded.

## Architecture

```
┌─────────────┐     ┌──────────┐     ┌─────────────┐     ┌──────────┐
│ Source text  │────►│  Lexer   │────►│  Parser     │────►│ Evaluator│
│ (2O9.nix)   │     │ (tokens) │     │ (AST)       │     │ (values) │
└─────────────┘     └──────────┘     └─────────────┘     └──────────┘
                                                                │
                                                                ▼
                                                        ┌──────────────┐
                                                        │ JSON manifest│
                                                        │ (output)     │
                                                        └──────────────┘
```

### Lexer (`nix_lexer.c`)
Tokenizes Nix source into: identifiers, strings (with interpolation), integers,
operators (`+`, `++`, `.`, `:`), keywords (`let`, `in`, `if`, `then`, `else`,
`with`, `rec`, `assert`), and delimiters (`{`, `}`, `[`, `]`, `(`, `)`, `;`).

### Parser (`nix_parser.c`)
Recursive-descent parser producing an AST. Node types:
- `NIX_NODE_ATTR_SET` - attribute set literal
- `NIX_NODE_LIST` - list literal
- `NIX_NODE_STRING` - string (possibly with interpolation parts)
- `NIX_NODE_IDENT` - identifier reference
- `NIX_NODE_SELECT` - attribute selection (`a.b.c`)
- `NIX_NODE_APPLY` - function application
- `NIX_NODE_LAMBDA` - lambda (`x: body` or `{ ... }: body`)
- `NIX_NODE_LET` - let-expression
- `NIX_NODE_IF` - conditional
- `NIX_NODE_WITH` - with-expression
- `NIX_NODE_BINOP` - binary operation (+, ++, etc.)

### Evaluator (`nix_eval.c`)
Walks the AST, evaluates expressions in an environment (symbol table of
name→value bindings). Values are:

- `NIX_VAL_ATTR_SET` - attribute set (ordered map)
- `NIX_VAL_LIST` - list (array)
- `NIX_VAL_STRING` - string
- `NIX_VAL_INT` - integer
- `NIX_VAL_BOOL` - boolean
- `NIX_VAL_NULL` - null
- `NIX_VAL_LAMBDA` - closure (env + AST node)
- `NIX_VAL_PATH` - path literal

The evaluator produces a `nix_value_t *` root value from evaluating `2O9.nix`.
A separate `nix_to_json()` function walks the value tree and emits a JSON
manifest that the declarative engine consumes.

### 2O9-specific primops (`nix_primops.c`)
Custom builtins for 2O9's config schema:

- `builtins.fromJSON` - parse a JSON string into a Nix value
- `builtins.toJSON` - serialize a Nix value to JSON string
- `builtins.map` - list map
- `builtins.filter` - list filter
- `builtins.length` - list length
- `builtins.head` / `builtins.tail` - list head/tail
- `builtins.attrNames` / `builtins.attrValues` - attrset introspection
- `builtins.hasAttr` / `builtins.getAttr` - attrset access
- `builtins.trace` - debug tracing (to stderr)

More primops can be added as needed.

## Build

These files compile as part of `lib2O9.a` - pure C, no C++ deps.

## Status

Phase 3 - core evaluator implemented. The evaluator handles:
- Attribute sets (plain & recursive), lists, strings with interpolation
- Let-bindings, if/then/else, with, assert, select
- Lambda functions (ident param and formal params `{ a, b ? default, ... }: body`)
- Function application (including curried application: `(x: y: x + y) 3 4`)
- Import resolution with base directory
- Fixed-point recursion for `{ config, ... }: { ... }` pattern
- 19 builtins (map, filter, length, head, tail, attrNames, attrValues, etc.)
- `builtins.<name>` dot-notation select for all builtins
- `inherit ident;` and `inherit (src) ident1 ident2;` forms
- JSON serialization of values

The parser is feature-complete for the 2O9.nix subset. All 9 binary
operator precedence levels are implemented (implication → or → and →
equality → comparison → concat → add → mul → unary), with correct
left- and right-associativity.

The test suite (`test_nix_eval.c`) has 49 passing tests covering all
of the above.
