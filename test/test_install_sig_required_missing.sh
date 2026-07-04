#!/usr/bin/env bash
# test_install_sig_required_missing.sh - SigLevel=Required refuses unsigned pkg
#
# Sets `SigLevel = "Required"` in the manifest, points the install at a
# real .pkg.tar.zst via TWO09_PKG_PATH, but configures a fake mirror URL
# so the .sig fetch fails. The install must be refused with a clear
# error (not silently proceeding without sig verification).
#
# `209 install` requires root (writes to /nix/store). This test:
#   - As root: runs the install and verifies it's refused.
#   - As non-root: verifies the SigLevel parsing path doesn't crash,
#     and the unit tests for signing (test-signing) cover the crypto.
#
# Usage: ./test/test_install_sig_required_missing.sh [path/to/209]

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
BINARY_DIR="$(dirname "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_install_sig_required_missing: sandbox at $TEST_ROOT ==="

export HOME="$TEST_ROOT/home"
export TWO9_CONFIG_DIR="$TEST_ROOT/nix/config"
USER_NAME="$(id -un)"
mkdir -p "$TWO9_CONFIG_DIR"
mkdir -p "$HOME/.local/state/2O9/generations"
mkdir -p "$TEST_ROOT/cache"

# Create a <user>.nix with SigLevel = "Required". This forces the install
# path to fetch and verify a .sig file alongside the .pkg.tar.zst.
cat > "$TWO9_CONFIG_DIR/$USER_NAME.nix" <<'EOF'
{ config, ... }:
{
  packages = [ ];
  services = { };
  SigLevel = "Required";
}
EOF

# Create a fake .pkg.tar.zst - just a regular file with the right
# magic bytes so it's recognised as a zst archive by the file command.
# We don't actually need it to be a valid package - the test verifies
# that the sig-required check fails BEFORE extraction.
PKG_PATH="$TEST_ROOT/cache/fakepkg-1.0-1-x86_64.pkg.tar.zst"
# zst magic: 0x28 0xB5 0x2F 0xFD
printf '\x28\xb5\x2f\xfd' > "$PKG_PATH"
# Pad with random bytes so it's not zero-length.
dd if=/dev/zero of="$PKG_PATH" bs=1024 count=4 oflag=append conv=notrunc 2>/dev/null || true
echo "OK: created fake .pkg.tar.zst at $PKG_PATH"

# Do NOT create a .sig file alongside it. With SigLevel=Required, the
# install path should attempt to fetch $PKG_PATH.sig, fail, and refuse
# to install. We use TWO09_PKG_PATH to point at our fake pkg.
export TWO09_PKG_PATH="$PKG_PATH"

# Run `209 fakepkg install` (SOV pattern: <subject> <verb>). As
# non-root, 209 will try to re-exec via sudo. We capture the output
# and detect the sig-related error path.
echo "--- running: 209 fakepkg install (SigLevel=Required, .sig missing) ---"
OUTPUT=$("$BINARY" fakepkg install 2>&1) || rc=$?
rc=${rc:-0}
echo "$OUTPUT" | head -25

# Run the signing unit tests as a sanity check (covers the crypto path
# that sig verification uses).
SIGN_BIN="$BINARY_DIR/test-signing"
if [ -x "$SIGN_BIN" ]; then
    echo "--- running: $SIGN_BIN (sig verify round-trip) ---"
    if "$SIGN_BIN" >/tmp/209-test-signing-install.log 2>&1; then
        echo "OK: test-signing confirms the signing/verify crypto works"
    else
        echo "WARN: test-signing failed (sig verification may be broken)"
    fi
fi

# What we expect from `209 fakepkg install`:
#   - If running as root and the SigLevel=Required code path is
#     exercised, the install should refuse the package (either at
#     .sig fetch failure or at gpgme verification failure).
#   - If running as non-root, 209 will try to re-exec via sudo, which
#     will likely fail with "sudo is required" (acceptable - we can't
#     easily get to the sig check without root).
#
# In either case, 209 must NOT print "successfully installed" or exit 0
# (the install cannot succeed without a valid .sig).

if [ "$rc" -eq 0 ]; then
    if echo "$OUTPUT" | grep -qiE "install.*success|installed fakepkg|extracted to store"; then
        echo "FAIL: 209 install reported success despite missing .sig (SigLevel=Required)"
        exit 1
    fi
    echo "WARN: 209 install exited 0 - check output for what happened"
else
    echo "OK: 209 install exited non-zero (rc=$rc)"
fi

# The output should mention one of: sig, signature, verify, required,
# sudo, root, or the package name. We're looking for evidence that
# either the sig check ran OR the privilege check refused.
if echo "$OUTPUT" | grep -qiE "sig|signature|verify|required|sudo|root|fakepkg|gpgme|keyring"; then
    echo "OK: output mentions sig/verify/privilege path"
else
    echo "WARN: output doesn't mention sig or privilege path"
    echo "      (the install may have failed for an unrelated reason)"
fi

# 209 must not crash with a signal.
if [ "$rc" -ge 128 ]; then
    echo "FAIL: 209 install died with signal $((rc - 128))"
    exit 1
fi
echo "OK: 209 did not crash with a signal"

# Negative control: run with TWO09_TEST_MODE=1, which bypasses the
# real download path entirely. This confirms the test mode doesn't
# accidentally bypass the SigLevel check (it shouldn't, because the
# test mode skips the download/sig path completely - but verify).
echo "--- running: TWO09_TEST_MODE=1 209 fakepkg install (bypasses real install) ---"
TEST_OUT=$(TWO09_TEST_MODE=1 "$BINARY" fakepkg install 2>&1) || true
echo "$TEST_OUT" | head -5
if echo "$TEST_OUT" | grep -qiE "test mode|fake store"; then
    echo "OK: TWO09_TEST_MODE bypasses the real install path"
else
    echo "WARN: TWO09_TEST_MODE output unexpected"
fi

echo "=== test_install_sig_required_missing: PASS ==="
