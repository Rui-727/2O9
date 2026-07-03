#!/usr/bin/env bash
# test_cache_keygen_roundtrip.sh - verify `209 keygen` output works
#
# Runs `209 keygen`, parses the `<name>:<pub_b64>:<sec_b64>` line,
# verifies the public and secret keys each base64-decode to exactly
# 32 bytes, then invokes ./test-keygen <pub_b64> <sec_b64> which does
# an Ed25519 sign + verify round-trip and a tamper-rejection check.
#
# This test runs without root (keygen is a pure-crypto operation).
#
# Usage: ./test/test_cache_keygen_roundtrip.sh [path/to/209]

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
BINARY_DIR="$(dirname "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_cache_keygen_roundtrip: sandbox at $TEST_ROOT ==="

# `209 keygen` doesn't need HOME but we set it to keep any code that
# reads HOME (logging, etc.) predictable.
export HOME="$TEST_ROOT/home"
mkdir -p "$HOME"

# Run `209 keygen`. It writes the keypair to stdout and an info
# message to stderr.
echo "--- running: 209 keygen ---"
STDOUT=$("$BINARY" keygen 2>"$TEST_ROOT/stderr") || rc=$?
rc=${rc:-0}
if [ "$rc" -ne 0 ]; then
    echo "FAIL: 209 keygen exited $rc"
    cat "$TEST_ROOT/stderr"
    exit 1
fi
echo "OK: 209 keygen exited 0"

# Parse the keypair line. Format: "<name>:<pub_b64>:<sec_b64>\n"
NAME=$(echo "$STDOUT" | head -1 | cut -d: -f1)
PUB_B64=$(echo "$STDOUT" | head -1 | cut -d: -f2)
SEC_B64=$(echo "$STDOUT" | head -1 | cut -d: -f3)

if [ -z "$NAME" ] || [ -z "$PUB_B64" ] || [ -z "$SEC_B64" ]; then
    echo "FAIL: could not parse keygen output"
    echo "  stdout: $STDOUT"
    exit 1
fi
echo "OK: parsed keypair (name=$NAME)"

# Verify the public key base64-decodes to exactly 32 bytes.
PUB_BYTES=$(echo -n "$PUB_B64" | base64 -d 2>/dev/null | wc -c)
if [ "$PUB_BYTES" -eq 32 ]; then
    echo "OK: public key decodes to 32 bytes"
else
    echo "FAIL: public key decodes to $PUB_BYTES bytes (expected 32)"
    exit 1
fi

# Verify the secret key base64-decodes to exactly 32 bytes.
SEC_BYTES=$(echo -n "$SEC_B64" | base64 -d 2>/dev/null | wc -c)
if [ "$SEC_BYTES" -eq 32 ]; then
    echo "OK: secret key decodes to 32 bytes"
else
    echo "FAIL: secret key decodes to $SEC_BYTES bytes (expected 32)"
    exit 1
fi

# Default key name should be "209-local-1" (per cmd_keygen).
if [ "$NAME" = "209-local-1" ]; then
    echo "OK: default key name is '209-local-1'"
else
    echo "WARN: key name is '$NAME' (expected '209-local-1')"
fi

# Run the C round-trip helper.
KEYGEN_BIN="$BINARY_DIR/test-keygen"
if [ ! -x "$KEYGEN_BIN" ]; then
    echo "FAIL: $KEYGEN_BIN not found (run 'make test-keygen')"
    exit 1
fi

echo "--- running: $KEYGEN_BIN <pub> <sec> ---"
RT_OUTPUT=$("$KEYGEN_BIN" "$PUB_B64" "$SEC_B64" 2>&1) || rc=$?
rc=${rc:-0}
echo "$RT_OUTPUT"

if [ "$rc" -ne 0 ]; then
    echo "FAIL: test-keygen round-trip exited $rc"
    exit 1
fi

# Verify the round-trip helper explicitly confirmed each step.
echo "$RT_OUTPUT" | grep -q "pub decodes to 32 bytes" && echo "OK: helper confirmed pub decode" || {
    echo "FAIL: helper did not confirm pub decode"
    exit 1
}
echo "$RT_OUTPUT" | grep -q "sec decodes to 32 bytes" && echo "OK: helper confirmed sec decode" || {
    echo "FAIL: helper did not confirm sec decode"
    exit 1
}
echo "$RT_OUTPUT" | grep -q "signing_verify(pub_b64) returns 1" && echo "OK: helper confirmed verify round-trip" || {
    echo "FAIL: helper did not confirm verify round-trip"
    exit 1
}
echo "$RT_OUTPUT" | grep -q "tampered fingerprint rejected" && echo "OK: helper confirmed tamper rejection" || {
    echo "FAIL: helper did not confirm tamper rejection"
    exit 1
}

# Also test --name and --out flags.
echo "--- testing: 209 keygen --name custom-1 --out <file> ---"
OUT_FILE="$TEST_ROOT/key.txt"
"$BINARY" keygen --name custom-1 --out "$OUT_FILE" 2>"$TEST_ROOT/stderr2" || rc=$?
rc=${rc:-0}
if [ "$rc" -ne 0 ]; then
    echo "FAIL: 209 keygen --out exited $rc"
    cat "$TEST_ROOT/stderr2"
    exit 1
fi
if [ ! -f "$OUT_FILE" ]; then
    echo "FAIL: --out file not created"
    exit 1
fi
LINE=$(head -1 "$OUT_FILE")
CUSTOM_NAME=$(echo "$LINE" | cut -d: -f1)
if [ "$CUSTOM_NAME" = "custom-1" ]; then
    echo "OK: --name custom-1 reflected in output file"
else
    echo "FAIL: --name not respected (got '$CUSTOM_NAME')"
    exit 1
fi

echo "=== test_cache_keygen_roundtrip: PASS ==="
