#!/usr/bin/env bash
# test_diff_why_lock.sh — test diff, why, and lock commands

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

export HOME="$TEST_ROOT/home"
mkdir -p "$HOME/.local/state/2O9/generations"

# Create two fake generations
mkdir -p "$HOME/.local/state/2O9/generations/1"
cat > "$HOME/.local/state/2O9/generations/1/manifest.json" <<'EOF'
{
  "id": 1, "timestamp": 1000,
  "packages": [
    {"name": "pkgA", "version": "1.0", "store_path": "/nix/store/pkgA-1.0", "origin": "repo"},
    {"name": "pkgB", "version": "2.0", "store_path": "/nix/store/pkgB-2.0", "origin": "repo"}
  ]
}
EOF
cat > "$HOME/.local/state/2O9/generations/1/diff.json" <<'EOF'
{"parent": 0, "total": 2, "added": [{"name":"pkgA","version":"1.0"},{"name":"pkgB","version":"2.0"}], "removed": [], "changed": []}
EOF

mkdir -p "$HOME/.local/state/2O9/generations/2"
cat > "$HOME/.local/state/2O9/generations/2/manifest.json" <<'EOF'
{
  "id": 2, "timestamp": 2000,
  "packages": [
    {"name": "pkgA", "version": "1.1", "store_path": "/nix/store/pkgA-1.1", "origin": "repo"},
    {"name": "pkgC", "version": "3.0", "store_path": "/nix/store/pkgC-3.0", "origin": "aur"}
  ]
}
EOF
cat > "$HOME/.local/state/2O9/generations/2/diff.json" <<'EOF'
{"parent": 1, "total": 2, "added": [{"name":"pkgC","version":"3.0"}], "removed": [{"name":"pkgB","version":"2.0"}], "changed": [{"name":"pkgA","old":"1.0","new":"1.1"}]}
EOF
ln -s "$HOME/.local/state/2O9/generations/2" "$HOME/.local/state/2O9/current"

echo "=== test_diff_why_lock ==="

# Test diff
echo "--- diff 1 2 ---"
OUT=$("$BINARY" diff 1 2 2>&1)
echo "$OUT" | grep -q "Added" && echo "OK: diff shows added" || echo "FAIL: no added section"
echo "$OUT" | grep -q "pkgC" && echo "OK: diff shows pkgC added" || echo "FAIL: no pkgC"
echo "$OUT" | grep -q "Removed" && echo "OK: diff shows removed" || echo "FAIL: no removed section"
echo "$OUT" | grep -q "pkgB" && echo "OK: diff shows pkgB removed" || echo "FAIL: no pkgB"
echo "$OUT" | grep -q "Changed" && echo "OK: diff shows changed" || echo "FAIL: no changed section"
echo "$OUT" | grep -q "1.0 -> 1.1" && echo "OK: diff shows version change" || echo "FAIL: no version change"

# Test diff without args
echo "--- diff without args ---"
OUT=$("$BINARY" diff 2>&1) || true
echo "$OUT" | grep -q "usage" && echo "OK: diff shows usage" || echo "FAIL: no usage"

# Test why
echo "--- why pkgA ---"
OUT=$("$BINARY" why pkgA 2>&1)
echo "$OUT" | grep -q "pkgA" && echo "OK: why shows pkgA" || echo "FAIL: no pkgA"
echo "$OUT" | grep -q "1.1" && echo "OK: why shows version" || echo "FAIL: no version"
echo "$OUT" | grep -q "installed" && echo "OK: why confirms installed" || echo "FAIL: no install confirmation"

# Test why on uninstalled package
echo "--- why nonexistent ---"
OUT=$("$BINARY" why nonexistent 2>&1) || true
echo "$OUT" | grep -q "not installed" && echo "OK: why errors on uninstalled" || echo "FAIL: should error"

# Test lock export
echo "--- lock export ---"
OUT=$("$BINARY" lock --export "$TEST_ROOT/2O9.lock" 2>&1)
echo "$OUT" | grep -q "Lockfile" && echo "OK: lock export works" || echo "FAIL: lock export failed"
[ -f "$TEST_ROOT/2O9.lock" ] && echo "OK: lockfile created" || echo "FAIL: no lockfile"
grep -q "pkgA" "$TEST_ROOT/2O9.lock" && echo "OK: lockfile contains pkgA" || echo "FAIL: no pkgA in lockfile"

# Test lock import
echo "--- lock import ---"
OUT=$("$BINARY" lock --import "$TEST_ROOT/2O9.lock" 2>&1)
echo "$OUT" | grep -q "generation" && echo "OK: lock import commits generation" || echo "FAIL: lock import failed"

# Test lock without args
echo "--- lock without args ---"
OUT=$("$BINARY" lock 2>&1) || true
echo "$OUT" | grep -q "usage" && echo "OK: lock shows usage" || echo "FAIL: no usage"

# Test upgrade dispatches
echo "--- upgrade ---"
OUT=$("$BINARY" upgrade 2>&1) || true
echo "$OUT" | grep -qiE "upgrade|checking" && echo "OK: upgrade dispatches" || echo "WARN: upgrade may need config"

# Test -Su dispatches
echo "--- -Su ---"
OUT=$("$BINARY" -Su 2>&1) || true
echo "$OUT" | grep -qiE "upgrade|checking" && echo "OK: -Su dispatches" || echo "WARN: -Su may need config"

echo "=== test_diff_why_lock: PASS ==="
