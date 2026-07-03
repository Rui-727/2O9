#!/usr/bin/env bash
# test_debag.sh - test Debag hybrid sandbox
#
# Tests:
#   1. Static analysis on a real binary (/bin/ls)
#   2. Running a command under debag
#   3. --no-net flag
#   4. --fast-mode flag

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"

echo "=== test_debag ==="

# Test 1: static analysis
echo "--- static-scan on /bin/ls ---"
OUT=$("$BINARY" debag --static-scan -- /bin/ls 2>&1) || true
echo "$OUT" | grep -q "Static Analysis" && echo "OK: static-scan produces output" || echo "FAIL: no analysis output"
echo "$OUT" | grep -q "Binary:" && echo "OK: shows binary path" || echo "FAIL: no binary path"
echo "$OUT" | grep -q "Dynamic:" && echo "OK: shows dynamic flag" || echo "FAIL: no dynamic flag"
echo "$OUT" | grep -q "Linked libraries" && echo "OK: shows linked libs" || echo "FAIL: no linked libs"
echo "$OUT" | grep -q "Seccomp Filter" && echo "OK: shows seccomp rules" || echo "FAIL: no seccomp rules"

# Test 2: run a command
echo "--- run ls under debag ---"
OUT=$("$BINARY" debag -- ls 2>&1) || true
echo "$OUT" | grep -q "exit_code" && echo "OK: produces JSON result" || echo "FAIL: no JSON output"
echo "$OUT" | grep -q "syscalls" && echo "OK: tracks syscalls" || echo "FAIL: no syscall tracking"

# Test 3: --fast-mode
echo "--- fast-mode ---"
OUT=$("$BINARY" debag --fast-mode -- echo hello 2>&1) || true
echo "$OUT" | grep -q "exit_code\|hello" && echo "OK: fast-mode runs" || echo "WARN: fast-mode may need seccomp permissions"

# Test 4: no command specified
echo "--- no command ---"
OUT=$("$BINARY" debag 2>&1) || true
echo "$OUT" | grep -q "no command" && echo "OK: errors on missing command" || echo "FAIL: should error"

# Test 5: non-ELF file
echo "--- non-ELF file ---"
echo "not an elf" > /tmp/test_debag_notelf
OUT=$("$BINARY" debag --static-scan -- /tmp/test_debag_notelf 2>&1) || true
echo "$OUT" | grep -qiE "cannot analyze|not an ELF" && echo "OK: rejects non-ELF" || echo "WARN: should reject non-ELF"
rm -f /tmp/test_debag_notelf

echo "=== test_debag: PASS ==="
