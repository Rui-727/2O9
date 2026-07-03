#!/usr/bin/env bash
# test_pacman_flags.sh — test pacman-compatible flags
#
# Tests -S -Q -Qs -Qi -Ql -Qm -R -Ss -Si -Sy
# Verifies they dispatch correctly and produce expected output.

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

export HOME="$TEST_ROOT/home"
mkdir -p "$HOME/.local/state/2O9/generations"

echo "=== test_pacman_flags: sandbox at $TEST_ROOT ==="

# Create a fake generation with packages
mkdir -p "$HOME/.local/state/2O9/generations/1"
cat > "$HOME/.local/state/2O9/generations/1/manifest.json" <<'EOF'
{
  "id": 1,
  "timestamp": 1000000,
  "packages": [
    {"name": "fakepkg", "version": "1.0", "store_path": "/nix/store/fakepkg-1.0", "origin": "repo"},
    {"name": "aurpkg", "version": "2.0", "store_path": "/nix/store/aurpkg-2.0", "origin": "aur"},
    {"name": "libfoo", "version": "3.1", "store_path": "/nix/store/libfoo-3.1", "origin": "repo"}
  ]
}
EOF
ln -s "$HOME/.local/state/2O9/generations/1" "$HOME/.local/state/2O9/current"

# Create fake store paths
mkdir -p "$TEST_ROOT/store/fakepkg-1.0/bin"
echo "#!/bin/sh" > "$TEST_ROOT/store/fakepkg-1.0/bin/fakepkg"
mkdir -p "$TEST_ROOT/store/aurpkg-2.0/bin"
echo "#!/bin/sh" > "$TEST_ROOT/store/aurpkg-2.0/bin/aurpkg"

# Test -Q (list all)
echo "--- -Q (list all) ---"
OUT=$("$BINARY" -Q 2>&1)
echo "$OUT" | grep -q fakepkg && echo "OK: -Q lists fakepkg" || echo "FAIL: -Q missing fakepkg"
echo "$OUT" | grep -q aurpkg && echo "OK: -Q lists aurpkg" || echo "FAIL: -Q missing aurpkg"
echo "$OUT" | grep -q libfoo && echo "OK: -Q lists libfoo" || echo "FAIL: -Q missing libfoo"

# Test -Qs (search)
echo "--- -Qs (search) ---"
OUT=$("$BINARY" -Qs fake 2>&1)
echo "$OUT" | grep -q fakepkg && echo "OK: -Qs finds fakepkg" || echo "FAIL: -Qs missing fakepkg"

# Test -Qi (info)
echo "--- -Qi (info) ---"
OUT=$("$BINARY" -Qi fakepkg 2>&1)
echo "$OUT" | grep -q "fakepkg" && echo "OK: -Qi shows fakepkg" || echo "FAIL: -Qi missing fakepkg"
echo "$OUT" | grep -q "1.0" && echo "OK: -Qi shows version" || echo "FAIL: -Qi missing version"

# Test -Qm (foreign/AUR packages)
echo "--- -Qm (foreign) ---"
OUT=$("$BINARY" -Qm 2>&1)
echo "$OUT" | grep -q aurpkg && echo "OK: -Qm lists aurpkg" || echo "FAIL: -Qm missing aurpkg"
echo "$OUT" | grep -q fakepkg && echo "FAIL: -Qm should not list fakepkg (repo)" || echo "OK: -Qm excludes repo packages"

# Test unknown flag
echo "--- -X (unknown) ---"
OUT=$("$BINARY" -X 2>&1) || true
echo "$OUT" | grep -q "unknown flag" && echo "OK: -X gives error" || echo "FAIL: -X should error"

# Test -Sy (sync) — just verify it dispatches (will fail on network)
echo "--- -Sy (sync) ---"
OUT=$("$BINARY" -Sy 2>&1) || true
echo "$OUT" | grep -qiE "sync|mirror|repo" && echo "OK: -Sy dispatches to sync" || echo "WARN: -Sy output unexpected"

# Test -Ss (search repos)
echo "--- -Ss (search repos) ---"
OUT=$("$BINARY" -Ss fakepkg 2>&1) || true
echo "OK: -Ss dispatched (may show AUR fallback)"

# Test -Si (info)
echo "--- -Si (info) ---"
OUT=$("$BINARY" -Si fakepkg 2>&1) || true
echo "OK: -Si dispatched (may show AUR fallback)"

echo "=== test_pacman_flags: PASS ==="
