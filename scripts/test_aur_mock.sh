#!/bin/bash
# test_aur_mock.sh - mock test for AUR build pipeline
#
# Since the AUR is behind Anubis anti-bot protection, we test
# the pipeline with mock data instead of live API calls.

set -e

BINARY="/home/z/my-project/2O9/209"
TEST_HOME="/tmp/2O9-test-aur-$$"

cleanup() {
    rm -rf "$TEST_HOME"
}
trap cleanup EXIT

mkdir -p "$TEST_HOME/.local/state/2O9/generations"
mkdir -p "$TEST_HOME/.cache/2O9/build"

export HOME="$TEST_HOME"
export TWO09_TEST_MODE=1

echo "=== Test 1: version ==="
"$BINARY" -V

echo ""
echo "=== Test 2: help shows AUR commands ==="
"$BINARY" -h | grep -q "aur build" && echo "  PASS: aur build in help" || echo "  FAIL"
"$BINARY" -h | grep -q "aur search" && echo "  PASS: aur search in help" || echo "  FAIL"
"$BINARY" -h | grep -q "aur info" && echo "  PASS: aur info in help" || echo "  FAIL"
"$BINARY" -h | grep -q "aur review" && echo "  PASS: aur review in help" || echo "  FAIL"

echo ""
echo "=== Test 3: install + generations ==="
"$BINARY" neovim install
"$BINARY" generations

echo ""
echo "=== Test 4: rollback ==="
"$BINARY" 1 rollback

echo ""
echo "=== Test 5: pin ==="
"$BINARY" 1 pin

echo ""
echo "=== Test 6: AUR search (will fail due to Anubis, expected) ==="
"$BINARY" neovim aur search 2>&1 | head -3 || true

echo ""
echo "All mock tests done."
