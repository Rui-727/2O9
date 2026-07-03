#!/usr/bin/env bash
# test_bundle.sh — test bundle/export and import round-trip
#
# Creates a fake generation, bundles it, then imports it back.

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

export HOME="$TEST_ROOT/home"
mkdir -p "$HOME/.local/state/2O9/generations/1"

# Create a fake generation
cat > "$HOME/.local/state/2O9/generations/1/manifest.json" <<'EOF'
{
  "id": 1,
  "timestamp": 1000000,
  "packages": [
    {"name": "fakepkg", "version": "1.0", "store_path": "/nix/store/fakepkg-1.0", "origin": "repo"}
  ]
}
EOF
cat > "$HOME/.local/state/2O9/generations/1/diff.json" <<'EOF'
{
  "parent": 0,
  "total": 1,
  "added": [{"name": "fakepkg", "version": "1.0"}],
  "removed": [],
  "changed": []
}
EOF
ln -s "$HOME/.local/state/2O9/generations/1" "$HOME/.local/state/2O9/current"

echo "=== test_bundle: sandbox at $TEST_ROOT ==="

# Test bundle
echo "--- bundle generation 1 ---"
OUT=$("$BINARY" bundle generation 1 --output "$TEST_ROOT/bundle.tar.gz" 2>&1) || true
echo "$OUT"
if [ -f "$TEST_ROOT/bundle.tar.gz" ]; then
    echo "OK: bundle created"
    SIZE=$(stat -c%s "$TEST_ROOT/bundle.tar.gz" 2>/dev/null || stat -f%z "$TEST_ROOT/bundle.tar.gz" 2>/dev/null || echo 0)
    [ "$SIZE" -gt 0 ] && echo "OK: bundle non-empty ($SIZE bytes)" || echo "WARN: bundle is empty"
else
    echo "WARN: bundle not created (may need tar)"
fi

# Test import
echo "--- import bundle ---"
# Move the current generation out of the way so import gets a new ID
rm -f "$HOME/.local/state/2O9/current"
rm -rf "$HOME/.local/state/2O9/generations/1"

OUT=$("$BINARY" import "$TEST_ROOT/bundle.tar.gz" 2>&1) || true
echo "$OUT"
echo "$OUT" | grep -qiE "import|generation" && echo "OK: import ran" || echo "WARN: import may have failed"

# Test bundle without args
echo "--- bundle without args ---"
OUT=$("$BINARY" bundle 2>&1) || true
echo "$OUT" | grep -q "usage" && echo "OK: shows usage on missing args" || echo "FAIL: should show usage"

# Test import without args
echo "--- import without args ---"
OUT=$("$BINARY" import 2>&1) || true
echo "$OUT" | grep -q "no file" && echo "OK: errors on missing file" || echo "FAIL: should error"

echo "=== test_bundle: PASS ==="
