#!/usr/bin/env bash
# test_share_subs.sh - test 209 share / get / subs dispatch
#
# Verifies the new commands dispatch correctly without needing root or
# a populated /nix/store. The actual NAR-hash-and-push path needs root
# to write to /nix/store; this test only checks argument parsing, help
# output, and the non-interactive subs fallback.

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_share_subs ==="

# Point config at an empty dir so two9_config_load finds nothing.
export TWO9_CONFIG_DIR="$TEST_ROOT/config"
mkdir -p "$TWO9_CONFIG_DIR"

echo "--- 209 share --help ---"
OUT=$("$BINARY" share --help 2>&1)
echo "$OUT" | grep -q "share a file or folder" && echo "OK: share --help shows description" || {
    echo "FAIL: share --help missing description"
    exit 1
}
echo "$OUT" | grep -q "nar://" && echo "OK: share --help mentions nar://" || {
    echo "FAIL: share --help missing nar://"
    exit 1
}

echo "--- 209 share (no args) ---"
OUT=$("$BINARY" share 2>&1) || rc=$?
rc=${rc:-0}
if [ "$rc" -ne 1 ]; then
    echo "FAIL: 209 share with no args should exit 1 (got $rc)"
    exit 1
fi
echo "$OUT" | grep -q "usage:" && echo "OK: share with no args prints usage" || {
    echo "FAIL: share with no args missing usage"
    exit 1
}

echo "--- 209 share relative-path (must be absolute) ---"
OUT=$("$BINARY" share relative-path 2>&1) || rc=$?
rc=${rc:-0}
if [ "$rc" -ne 1 ]; then
    echo "FAIL: 209 share relative-path should exit 1 (got $rc)"
    exit 1
fi
echo "$OUT" | grep -q "must be absolute" && echo "OK: share rejects relative path" || {
    echo "FAIL: share did not reject relative path"
    exit 1
}

echo "--- 209 share ls (empty) ---"
OUT=$("$BINARY" share ls 2>&1)
echo "$OUT" | grep -q "no local shares" && echo "OK: share ls shows empty message" || {
    echo "FAIL: share ls did not show empty message"
    exit 1
}

echo "--- 209 get --help ---"
OUT=$("$BINARY" get --help 2>&1)
echo "$OUT" | grep -q "fetch a share by URI" && echo "OK: get --help shows description" || {
    echo "FAIL: get --help missing description"
    exit 1
}
echo "$OUT" | grep -q "nar://<hash>" && echo "OK: get --help mentions nar://" || {
    echo "FAIL: get --help missing nar://"
    exit 1
}

echo "--- 209 get (no args) ---"
OUT=$("$BINARY" get 2>&1) || rc=$?
rc=${rc:-0}
if [ "$rc" -ne 1 ]; then
    echo "FAIL: 209 get with no args should exit 1 (got $rc)"
    exit 1
fi
echo "$OUT" | grep -q "usage:" && echo "OK: get with no args prints usage" || {
    echo "FAIL: get with no args missing usage"
    exit 1
}

echo "--- 209 subs --help ---"
OUT=$("$BINARY" subs --help 2>&1)
echo "$OUT" | grep -q "interactive sub picker" && echo "OK: subs --help shows description" || {
    echo "FAIL: subs --help missing description"
    exit 1
}
echo "$OUT" | grep -q "subs add" && echo "OK: subs --help mentions add" || {
    echo "FAIL: subs --help missing add"
    exit 1
}

echo "--- 209 subs (no config) ---"
OUT=$("$BINARY" subs 2>&1)
echo "$OUT" | grep -q "No subs configured" && echo "OK: subs with no config shows message" || {
    echo "FAIL: subs with no config did not show message"
    exit 1
}

echo "--- 209 subs <name> (nonexistent) ---"
OUT=$("$BINARY" subs nonexistent 2>&1) || rc=$?
rc=${rc:-0}
if [ "$rc" -ne 1 ]; then
    echo "FAIL: 209 subs nonexistent should exit 1 (got $rc)"
    exit 1
fi
echo "$OUT" | grep -q "no subs configured" && echo "OK: subs nonexistent prints error" || {
    echo "FAIL: subs nonexistent missing error"
    exit 1
}

echo "=== test_share_subs: PASS ==="
