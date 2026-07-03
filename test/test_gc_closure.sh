#!/usr/bin/env bash
# test_gc_closure.sh - `209 gc` respects transitive deps
#
# Verifies that `209 gc` does NOT delete a store path that's a transitive
# dependency of a generation's root, even if no generation explicitly
# references that dep.
#
# Setup:
#   - Create a generation manifest with package A -> store path SP_A.
#   - In the store DB (store.sqlite), register SP_A and SP_B with a ref
#     SP_A -> SP_B (SP_A depends on SP_B).
#   - Run `209 gc`. Both SP_A and SP_B must survive (SP_B is reachable
#     via the closure of SP_A).
#   - Then remove A from the generation. Run `209 gc` again. Both SP_A
#     and SP_B must be reaped (neither is reachable).
#
# `209 gc` requires root (writes to /nix/store). This test:
#   - As root: runs the full scenario.
#   - As non-root: skips the actual GC, but still verifies that the
#     store DB closure logic (tested in test-db) correctly preserves
#     transitive deps. Prints SKIP for the GC-specific assertions.
#
# The DB population uses python3 (always available alongside the build)
# since the sqlite3 CLI isn't installed in the sandbox.
#
# Usage: ./test/test_gc_closure.sh [path/to/209]

set -euo pipefail

BINARY="${1:-./209}"
BINARY="$(realpath "$BINARY")"
BINARY_DIR="$(dirname "$BINARY")"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

echo "=== test_gc_closure: sandbox at $TEST_ROOT ==="

export HOME="$TEST_ROOT/home"
mkdir -p "$HOME/.local/state/2O9/generations/1"

# Create a generation manifest with package A -> SP_A.
SP_A="/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-pkgA-1.0"
SP_B="/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-pkgB-1.0"
cat > "$HOME/.local/state/2O9/generations/1/manifest.json" <<EOF
{
  "id": 1,
  "timestamp": 1000000,
  "packages": [
    {"name": "pkgA", "version": "1.0", "store_path": "$SP_A", "origin": "repo"}
  ]
}
EOF
ln -s "$HOME/.local/state/2O9/generations/1" "$HOME/.local/state/2O9/current"

# Populate the store DB (store.sqlite) with SP_A, SP_B, and a ref
# SP_A -> SP_B. Schema matches db.c SCHEMA_SQL.
DB_PATH="$HOME/.local/state/2O9/store.sqlite"
python3 - "$DB_PATH" "$SP_A" "$SP_B" <<'PYEOF'
import sqlite3, sys
db_path, sp_a, sp_b = sys.argv[1], sys.argv[2], sys.argv[3]
conn = sqlite3.connect(db_path)
conn.executescript("""
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS valid_paths (
    id INTEGER PRIMARY KEY,
    path TEXT UNIQUE NOT NULL,
    hash TEXT NOT NULL,
    nar_size INTEGER NOT NULL,
    deriver TEXT,
    sigs TEXT,
    ca TEXT,
    registrationTime INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS refs (
    referrer INTEGER NOT NULL,
    reference INTEGER NOT NULL,
    PRIMARY KEY (referrer, reference),
    FOREIGN KEY (referrer) REFERENCES valid_paths(id) ON DELETE CASCADE,
    FOREIGN KEY (reference) REFERENCES valid_paths(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_refs_referrer ON refs(referrer);
CREATE INDEX IF NOT EXISTS idx_refs_reference ON refs(reference);
""")
import time
now = int(time.time())
conn.execute("INSERT INTO valid_paths (path, hash, nar_size, deriver, sigs, ca, registrationTime) VALUES (?, ?, 100, NULL, NULL, NULL, ?)",
             (sp_a, "sha256:aaaa", now))
conn.execute("INSERT INTO valid_paths (path, hash, nar_size, deriver, sigs, ca, registrationTime) VALUES (?, ?, 100, NULL, NULL, NULL, ?)",
             (sp_b, "sha256:bbbb", now))
# Look up ids and add ref SP_A -> SP_B
cur = conn.execute("SELECT id FROM valid_paths WHERE path=?", (sp_a,))
a_id = cur.fetchone()[0]
cur = conn.execute("SELECT id FROM valid_paths WHERE path=?", (sp_b,))
b_id = cur.fetchone()[0]
conn.execute("INSERT OR IGNORE INTO refs (referrer, reference) VALUES (?, ?)", (a_id, b_id))
conn.commit()
conn.close()
print("OK: store DB populated (SP_A=%s, SP_B=%s, ref SP_A->SP_B)" % (sp_a, sp_b))
PYEOF

echo "OK: generation 1 created (pkgA -> SP_A)"
echo "OK: store.sqlite populated (SP_A -> SP_B ref)"

# Run the unit test (test-db) to confirm the closure logic preserves
# transitive deps. This runs as non-root and is the meat of the test.
TEST_DB_BIN="$BINARY_DIR/test-db"
if [ -x "$TEST_DB_BIN" ]; then
    echo "--- running: $TEST_DB_BIN (unit-test the closure logic) ---"
    DB_OUT=$("$TEST_DB_BIN" 2>&1) || rc=$?
    rc=${rc:-0}
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: test-db unit tests failed"
        echo "$DB_OUT" | tail -10
        exit 1
    fi
    if echo "$DB_OUT" | grep -q "closure2: closure contains transitive C"; then
        echo "OK: test-db confirms transitive closure works"
    else
        echo "FAIL: test-db did not confirm transitive closure"
        exit 1
    fi
    if echo "$DB_OUT" | grep -q "cycle: closure terminates"; then
        echo "OK: test-db confirms cycles don't infinite-loop"
    else
        echo "FAIL: test-db cycle test missing or failed"
        exit 1
    fi
else
    echo "FAIL: $TEST_DB_BIN not found (run 'make test-db')"
    exit 1
fi

# Now try `209 gc` for real. This requires root.
if [ "$(id -u)" -ne 0 ]; then
    echo ""
    echo "SKIP: 209 gc requires root (we're uid=$(id -u))"
    echo "      The closure logic was verified above via test-db."
    echo "      To run the full GC test: sudo ./test/test_gc_closure.sh"
    echo ""
    echo "=== test_gc_closure: PASS (closure logic verified; GC skipped - needs root) ==="
    exit 0
fi

# Root path: actually create the store paths and run gc.
mkdir -p "$SP_A" "$SP_B"
echo "fake pkgA" > "$SP_A/file"
echo "fake pkgB" > "$SP_B/file"

echo "--- running: 209 gc (gen 1 references SP_A which depends on SP_B) ---"
GC_OUT=$("$BINARY" gc 2>&1) || rc=$?
rc=${rc:-0}
echo "$GC_OUT" | head -20

# After gc, both SP_A and SP_B should survive (SP_A is a root, SP_B is
# in SP_A's closure).
if [ -d "$SP_A" ]; then
    echo "OK: SP_A survived gc (referenced by generation 1)"
else
    echo "FAIL: SP_A was deleted by gc (should have survived as a root)"
    exit 1
fi
if [ -d "$SP_B" ]; then
    echo "OK: SP_B survived gc (transitive dep of SP_A via refs graph)"
else
    echo "FAIL: SP_B was deleted by gc (should have survived as a transitive dep)"
    exit 1
fi

# Now remove pkgA from the generation. Both SP_A and SP_B become dead.
cat > "$HOME/.local/state/2O9/generations/1/manifest.json" <<'EOF'
{
  "id": 1,
  "timestamp": 1000000,
  "packages": []
}
EOF
# Move "current" to a new generation that has no packages.
mkdir -p "$HOME/.local/state/2O9/generations/2"
cp "$HOME/.local/state/2O9/generations/1/manifest.json" "$HOME/.local/state/2O9/generations/2/manifest.json" 2>/dev/null || \
    cp "$HOME/.local/state/2O9/generations/1/manifest.json" "$HOME/.local/state/2O9/generations/2/manifest.json"
rm -f "$HOME/.local/state/2O9/current"
ln -s "$HOME/.local/state/2O9/generations/2" "$HOME/.local/state/2O9/current"

echo "--- running: 209 gc (no generation references SP_A or SP_B) ---"
GC_OUT=$("$BINARY" gc 2>&1) || rc=$?
rc=${rc:-0}
echo "$GC_OUT" | head -20

# After gc, both should be reaped.
if [ ! -d "$SP_A" ]; then
    echo "OK: SP_A was reaped by gc (no longer referenced)"
else
    echo "FAIL: SP_A survived gc (should have been reaped)"
    exit 1
fi
if [ ! -d "$SP_B" ]; then
    echo "OK: SP_B was reaped by gc (transitive dep of a now-dead SP_A)"
else
    echo "FAIL: SP_B survived gc (should have been reaped as a transitive dep)"
    exit 1
fi

echo "=== test_gc_closure: PASS ==="
