#!/usr/bin/env bash
# test_optimise_dedup.sh - `209 optimise` hardlinks identical files
#
# Creates two store paths, each containing an identical 1 MB file.
# Runs `209 optimise`. Verifies:
#   - The hardlink count (stat -c '%h') on one of the files is >= 2
#     (the two originals + the .links/<sha256> entry).
#   - The disk usage of /nix/store/.links/ reflects only one copy of
#     the file (1 MB, not 2 MB).
#
# `209 optimise` requires root (writes to /nix/store/.links). This
# test:
#   - As root: runs the full scenario.
#   - As non-root: skips with a clear message. The hardlink dedup logic
#     itself is unit-testable but not currently exposed as a C test
#     (store_optimise walks /nix/store directly).
#
# Usage: ./test/test_optimise_dedup.sh [path/to/209]

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_optimise_dedup: sandbox at $TEST_ROOT ==="

# Generate a deterministic 1 MB blob both store paths will share.
BLOB="$TEST_ROOT/blob.bin"
python3 -c "
import sys
with open('$BLOB', 'wb') as f:
    # 1 MB of predictable bytes
    for i in range(1024):
        f.write(bytes((i * 7 + j) & 0xff for j in range(1024)))
"
BLOB_SIZE=$(stat -c '%s' "$BLOB")
echo "OK: created $BLOB_SIZE-byte blob"

# As non-root, `209 optimise` would just re-exec via sudo and fail
# (sudo isn't available in the sandbox). Skip the actual optimise run.
if [ "$(id -u)" -ne 0 ]; then
    echo ""
    echo "SKIP: 209 optimise requires root (we're uid=$(id -u))"
    echo "      The dedup logic itself (store_optimise in optimise.c)"
    echo "      walks /nix/store and hardlinks identical files into"
    echo "      /nix/store/.links/<sha256>. To test it for real:"
    echo "        sudo ./test/test_optimise_dedup.sh"
    echo ""
    # Sanity-check that the binary at least dispatches the optimise
    # subcommand (will fail to write but should print the right header).
    OUT=$("$BINARY" optimise 2>&1) || true
    if echo "$OUT" | grep -qiE "optimis|209"; then
        echo "OK: 209 optimise dispatches (failed on permissions, as expected)"
    else
        echo "WARN: 209 optimise output unexpected"
        echo "$OUT" | head -5
    fi
    echo ""
    echo "=== test_optimise_dedup: PASS (skipped - needs root) ==="
    exit 0
fi

# Root path: create two store paths with the identical 1 MB file.
SP_A="/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-dedupA-1.0"
SP_B="/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-dedupB-1.0"
mkdir -p "$SP_A" "$SP_B"
cp "$BLOB" "$SP_A/big.bin"
cp "$BLOB" "$SP_B/big.bin"
chmod 0644 "$SP_A/big.bin" "$SP_B/big.bin"

# Verify they're separate inodes (nlink == 1) before optimise.
NLINK_BEFORE=$(stat -c '%h' "$SP_A/big.bin")
if [ "$NLINK_BEFORE" -eq 1 ]; then
    echo "OK: files have nlink=1 before optimise (separate inodes)"
else
    echo "WARN: files have nlink=$NLINK_BEFORE before optimise (expected 1)"
fi

# Disk usage of /nix/store/.links before.
LINKS_DIR="/nix/store/.links"
mkdir -p "$LINKS_DIR"
chmod 1777 "$LINKS_DIR"
DU_BEFORE=$(du -sb "$LINKS_DIR" 2>/dev/null | cut -f1 || echo 0)
echo "  .links du before: $DU_BEFORE bytes"

# Run optimise.
echo "--- running: 209 optimise ---"
OPT_OUT=$("$BINARY" optimise 2>&1) || rc=$?
rc=${rc:-0}
echo "$OPT_OUT" | head -10

if [ "$rc" -ne 0 ]; then
    echo "FAIL: 209 optimise exited $rc"
    exit 1
fi

# After optimise, both files should be hardlinked (nlink >= 2).
NLINK_AFTER=$(stat -c '%h' "$SP_A/big.bin")
if [ "$NLINK_AFTER" -ge 2 ]; then
    echo "OK: files have nlink=$NLINK_AFTER after optimise (hardlinked)"
else
    echo "FAIL: files still have nlink=$NLINK_AFTER after optimise (expected >= 2)"
    exit 1
fi

# Verify the two store paths now share the same inode.
INODE_A=$(stat -c '%i' "$SP_A/big.bin")
INODE_B=$(stat -c '%i' "$SP_B/big.bin")
if [ "$INODE_A" = "$INODE_B" ]; then
    echo "OK: both files share the same inode ($INODE_A)"
else
    echo "FAIL: files have different inodes ($INODE_A vs $INODE_B) after optimise"
    exit 1
fi

# Verify .links has exactly one copy (1 MB, not 2 MB).
DU_AFTER=$(du -sb "$LINKS_DIR" 2>/dev/null | cut -f1 || echo 0)
echo "  .links du after:  $DU_AFTER bytes"

# The .links dir should contain exactly one file of size BLOB_SIZE,
# plus directory overhead. Total should be roughly BLOB_SIZE + 4K.
# We assert it's less than 2 * BLOB_SIZE (would be the case if both
# copies were stored separately).
if [ "$DU_AFTER" -lt $((2 * BLOB_SIZE)) ]; then
    echo "OK: .links disk usage reflects dedup (< 2 * BLOB_SIZE)"
else
    echo "FAIL: .links disk usage ($DU_AFTER) suggests no dedup happened"
    exit 1
fi

# Verify contents are unchanged (hardlink doesn't corrupt data).
HASH_A=$(sha256sum "$SP_A/big.bin" | cut -d' ' -f1)
HASH_B=$(sha256sum "$SP_B/big.bin" | cut -d' ' -f1)
HASH_ORIG=$(sha256sum "$BLOB" | cut -d' ' -f1)
if [ "$HASH_A" = "$HASH_ORIG" ] && [ "$HASH_B" = "$HASH_ORIG" ]; then
    echo "OK: file contents preserved after hardlink dedup"
else
    echo "FAIL: file contents changed after dedup"
    echo "  original: $HASH_ORIG"
    echo "  SP_A:     $HASH_A"
    echo "  SP_B:     $HASH_B"
    exit 1
fi

# Cleanup the store paths we created (don't leak test data into /nix/store).
rm -rf "$SP_A" "$SP_B"
# Remove the .links entry for this blob (it's now an orphan).
find "$LINKS_DIR" -type f -links 1 -delete 2>/dev/null || true

echo "=== test_optimise_dedup: PASS ==="
