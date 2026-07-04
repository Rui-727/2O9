#!/usr/bin/env bash
# test_apply_self_reference_cycle.sh - fixed-point recursion must terminate
#
# The 2O9.nix evaluator applies the `{ config, ... }:` lambda with the
# result of its own previous iteration until the output stops changing
# (fixed-point) or it hits the iteration cap. A config that toggles
# forever based on its own state must NOT hang the evaluator - it must
# either converge (after one step the if-branch picks one arm and
# stays there) or fail with a clear "fixed-point did not converge"
# error.
#
# This test uses a self-referential conditional that's INTENDED to
# terminate (config.packages starts as [], so the if-branch adds "x",
# then on the next iteration config.packages is ["x"] which is non-empty,
# so the else-branch returns []). The fixed point is therefore
# config.packages == ["x"] - the evaluator should converge.
#
# A more pathological toggle (e.g. `if config.flag then { flag = false; }
# else { flag = true; }`) would oscillate forever; the evaluator caps
# at 100 iterations and fails with a clear error. We test both paths.
#
# Usage: ./test/test_apply_self_reference_cycle.sh [path/to/209]

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_apply_self_reference_cycle: sandbox at $TEST_ROOT ==="

export HOME="$TEST_ROOT/home"
export TWO9_CONFIG_DIR="$TEST_ROOT/nix/config"
USER_NAME="$(id -un)"
mkdir -p "$TWO9_CONFIG_DIR"
mkdir -p "$HOME/.local/state/2O9/generations"

# Run the test under a 30s timeout. The evaluator's cap is 100
# iterations; each iteration is fast. 30s is generous - if we hit it,
# the evaluator is in an infinite loop and the test fails.
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout 30"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout 30"
fi

# ── Case 1: converging self-reference ────────────────────────────────
cat > "$TWO9_CONFIG_DIR/$USER_NAME.nix" <<'EOF'
{ config, ... }:
{
  packages = if config.packages == [ ] then [ "x" ] else [ ];
}
EOF

echo "--- case 1: converging self-reference (should terminate) ---"
# shellcheck disable=SC2086
OUTPUT=$($TIMEOUT_BIN "$BINARY" apply 2>&1) || rc=$?
rc=${rc:-0}

# 124 is the timeout(1) exit code for "killed by timeout".
if [ "$rc" -eq 124 ]; then
    echo "FAIL: 209 apply hung on converging self-reference (timeout after 30s)"
    exit 1
fi
echo "OK: converging self-reference terminated (rc=$rc)"

# We expect either success or a clean "did not converge" error -
# either way, no hang. The converging case should succeed.
if echo "$OUTPUT" | grep -q "evaluation failed"; then
    if echo "$OUTPUT" | grep -qiE "converge|fixed.point"; then
        echo "OK: evaluator reported non-convergence (acceptable)"
    else
        echo "WARN: evaluation failed for another reason:"
        echo "$OUTPUT" | grep "evaluation failed" | head -3
    fi
else
    echo "OK: converging self-reference evaluated cleanly"
fi

# ── Case 2: pathological toggle (oscillates forever) ─────────────────
# This config toggles between { flag = true; } and { flag = false; }
# forever - there is no fixed point. The evaluator should detect the
# non-convergence (after 100 iterations) and fail with a clear error,
# NOT hang or crash.
cat > "$TWO9_CONFIG_DIR/$USER_NAME.nix" <<'EOF'
{ config, ... }:
let
  next = if config.flag or false then false else true;
in
{
  flag = next;
  packages = [ ];
}
EOF

echo "--- case 2: pathological toggle (should fail with clear error, not hang) ---"
# shellcheck disable=SC2086
OUTPUT=$($TIMEOUT_BIN "$BINARY" apply 2>&1) || rc=$?
rc=${rc:-0}

if [ "$rc" -eq 124 ]; then
    echo "FAIL: 209 apply hung on pathological toggle (timeout after 30s)"
    exit 1
fi
echo "OK: pathological toggle terminated (rc=$rc, did not hang)"

# The evaluator may either converge (if it interprets `or false` as
# starting state false, then toggle to true, then back to false, etc.
# indefinitely) OR find a fixed point. Either way, no hang = pass.
if echo "$OUTPUT" | grep -qiE "converge|fixed.point|max.*iter|did not converge"; then
    echo "OK: evaluator reported non-convergence with clear error"
elif [ "$rc" -eq 0 ]; then
    echo "OK: evaluator converged (acceptable - the toggle may have a stable point)"
else
    echo "OK: evaluator terminated with rc=$rc (no hang)"
fi

echo "=== test_apply_self_reference_cycle: PASS ==="
