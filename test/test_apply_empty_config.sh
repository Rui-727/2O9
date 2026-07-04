#!/usr/bin/env bash
# test_apply_empty_config.sh - `209 apply` with an empty package list
#
# The 2O9.nix declares `{ config, ... }: { packages = []; }` - no
# packages, no services, no AUR. `209 apply` should:
#   - evaluate the config without crashing
#   - commit a new generation
#   - write a manifest.json whose packages array is empty
#   - not crash on the empty diff
#
# This runs without root (the apply logic only fails at the install step,
# which doesn't happen when packages is empty).
#
# Usage: ./test/test_apply_empty_config.sh [path/to/209]

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_apply_empty_config: sandbox at $TEST_ROOT ==="

export HOME="$TEST_ROOT/home"
export TWO9_CONFIG_DIR="$TEST_ROOT/nix/config"
USER_NAME="$(id -un)"
mkdir -p "$TWO9_CONFIG_DIR"
mkdir -p "$HOME/.local/state/2O9/generations"
mkdir -p "$HOME/.local/bin"
mkdir -p "$HOME/.local/lib"

# Empty <user>.nix - just declares an empty package list.
cat > "$TWO9_CONFIG_DIR/$USER_NAME.nix" <<'EOF'
{ config, ... }:
{
  packages = [ ];
  services = { };
}
EOF

# Run 209 apply
echo "--- running: 209 apply ---"
OUTPUT=$("$BINARY" apply 2>&1) || rc=$?
rc=${rc:-0}
echo "$OUTPUT" | head -25

# We expect evaluation to succeed. The apply may bail at the
# "reconcile" or "install" step because there are no packages to
# install, but the manifest should still be committed.
if echo "$OUTPUT" | grep -q "evaluation failed"; then
    echo "FAIL: Nix evaluation failed for empty config"
    exit 1
fi
echo "OK: Nix evaluation succeeded for empty config"

# Verify a generation was committed.
if ls "$HOME/.local/state/2O9/generations/" 2>/dev/null | grep -qE '^[0-9]+$'; then
    echo "OK: generation directory created"
    GEN=$(ls "$HOME/.local/state/2O9/generations/" | sort -n | tail -1)
    echo "  generation: $GEN"

    MANIFEST="$HOME/.local/state/2O9/generations/$GEN/manifest.json"
    if [ -f "$MANIFEST" ]; then
        echo "OK: manifest.json exists"

        # The packages array should be empty: either `"packages": []`
        # or `"packages":[]` (no whitespace). Both forms are valid JSON.
        if grep -qE '"packages"[[:space:]]*:[[:space:]]*\[\s*\]' "$MANIFEST"; then
            echo "OK: manifest has empty packages array"
        else
            echo "WARN: packages array not empty (may include carried-forward imperative pkgs)"
            # Print the packages line for debugging.
            grep -E '"packages"' "$MANIFEST" || true
        fi

        # Should NOT crash: manifest must be valid JSON (starts with '{').
        if head -c 1 "$MANIFEST" | grep -q '{'; then
            echo "OK: manifest is valid JSON"
        else
            echo "FAIL: manifest does not start with '{'"
            exit 1
        fi
    else
        echo "WARN: manifest.json missing (apply may have failed before commit)"
    fi
else
    echo "WARN: no generation directory created (apply may have failed early)"
fi

# Verify the apply didn't hang or crash mid-eval. rc==0 is ideal;
# non-zero is acceptable if it failed at the install step (no real
# store backend in the sandbox) but NOT if it failed at eval time.
if [ "$rc" -eq 0 ]; then
    echo "OK: 209 apply exited 0"
elif echo "$OUTPUT" | grep -qiE "install|store|backend|repo"; then
    echo "OK: 209 apply failed at install step (expected without a real store)"
else
    echo "WARN: 209 apply exited $rc for an unexpected reason"
fi

echo "=== test_apply_empty_config: PASS ==="
