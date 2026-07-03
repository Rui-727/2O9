#!/usr/bin/env bash
# test_nix_eval_edge_cases.sh - run the Nix evaluator on edge cases
#
# Drives ./test-nix-eval-edge (built from lib/2O9/nix/test_nix_eval_edge.c)
# which exercises empty containers, nested let, with shadowing,
# builtins.map, string interpolation with builtins.toString, and the
# missing-attribute error path. The shell wrapper just confirms the
# binary exits 0 (all assertions passed) and prints the summary line.
#
# Usage: ./test/test_nix_eval_edge_cases.sh [path/to/209]
#   (The 209 binary itself is not used - the test drives test-nix-eval-edge
#   directly. The first arg is accepted for consistency with the other
#   test scripts and `make test`'s `./$$t ./209` invocation.)

set -euo pipefail

BINARY="${1:-./209}"
BINARY_DIR="$(dirname "$(realpath "$BINARY")")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_nix_eval_edge_cases ==="

EDGE_BIN="$BINARY_DIR/test-nix-eval-edge"
if [ ! -x "$EDGE_BIN" ]; then
    echo "FAIL: $EDGE_BIN not found (run 'make test-nix-eval-edge')"
    exit 1
fi

echo "--- running: $EDGE_BIN ---"
OUTPUT=$("$EDGE_BIN" 2>&1) || rc=$?
rc=${rc:-0}

# Print the summary line + a few key results.
echo "$OUTPUT" | tail -5

if [ "$rc" -ne 0 ]; then
    echo "FAIL: test-nix-eval-edge exited $rc"
    echo "$OUTPUT" | grep -E 'FAIL:' || true
    exit 1
fi

# Verify all the expected cases passed by grepping the output.
EXPECTED_CASES=(
    "empty list"
    "empty attrset"
    "nested let"
    "curried lambda apply"
    "inherit from attrset"
    "with shadowing"
    "builtins.map doubles"
    "string interpolation toString"
    "select missing attr returns clear error"
)

all_ok=1
for case in "${EXPECTED_CASES[@]}"; do
    if ! echo "$OUTPUT" | grep -q "PASS: $case"; then
        echo "FAIL: missing or failed case: $case"
        all_ok=0
    fi
done

if [ "$all_ok" -eq 1 ]; then
    echo "OK: all expected edge cases passed"
else
    exit 1
fi

# Sanity-check the result count line.
if echo "$OUTPUT" | grep -qE 'Results: [0-9]+/[0-9]+ passed'; then
    echo "OK: summary line present"
else
    echo "FAIL: no summary line in output"
    exit 1
fi

echo "=== test_nix_eval_edge_cases: PASS ==="
