/* snapshot.h - content-addressed snapshots of declared paths
 *
 * A snapshot is a NAR-hashed copy of a path declared in 2O9.nix under
 * the `snapshots` attrset. Storage reuses /nix/store/<base32>-snap-<name>/
 * so snapshots benefit from the same content addressing, dedup, and GC
 * as packages. A SQLite DB at <db_root>/snapshots.sqlite records the
 * snapshot history per path with parent links for the undo chain.
 *
 * The DB schema mirrors src/store/db.c's pattern (WAL mode, busy
 * timeout, IF NOT EXISTS schema init). If HAVE_SQLITE3 is undefined at
 * build time, this file compiles to stubs that always return failure;
 * the 209 binary still builds and runs without snapshot support.
 *
 * Auto-scheduling is wired by cmd_apply (see snapshot_install_timer):
 * each path with `auto != "manual"` gets a systemd timer + service.
 * The service runs `209 snapshot take <path>` as the appropriate user
 * (root for system paths, the user for user paths).
 */
#ifndef TWO9_SNAPSHOT_H
#define TWO9_SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct snapshot_db snapshot_db_t;

/* A single snapshot row from the DB. Used by snapshot_list() and
 * snapshot_get(). Caller frees strings with snapshot_record_free(). */
typedef struct snapshot_record {
    int64_t  id;
    char    *path;          /* absolute original path */
    char    *store_path;    /* /nix/store/<base32>-snap-<name>/ */
    char    *nar_hash;      /* 64-char lowercase hex */
    int64_t  parent_id;     /* 0 if no parent */
    char    *message;       /* may be NULL */
    int64_t  taken_at;      /* unix timestamp */
    char    *scope;         /* "system" or username */
} snapshot_record_t;

/* A single diff entry from snapshot_diff(). */
typedef enum {
    SNAP_DIFF_ADDED,        /* in id2 only */
    SNAP_DIFF_REMOVED,      /* in id1 only */
    SNAP_DIFF_MODIFIED,     /* in both, content differs */
} snapshot_diff_kind_t;

typedef struct snapshot_diff {
    snapshot_diff_kind_t kind;
    char    *rel_path;      /* path relative to snapshot root */
    char    *hash_a;        /* NAR hash of file in id1, NULL if added */
    char    *hash_b;        /* NAR hash of file in id2, NULL if removed */
} snapshot_diff_t;

/* ── DB open/close ──────────────────────────────────────────────────
 * db_root is the generation DB root (system: /var/lib/2O9,
 * user: ~/.local/state/2O9). The snapshot DB lives at
 * <db_root>/snapshots.sqlite. Returns NULL on failure. */
snapshot_db_t *snapshot_db_open(const char *db_root);
void           snapshot_db_close(snapshot_db_t *db);

/* ── Path sanitisation ─────────────────────────────────────────────
 * Turn an absolute path into a flat token for use in store path
 * basenames and systemd unit names. `/` becomes `-`, leading `-`
 * stripped. `/var/lib/pg` -> `var-lib-pg`. Writes a NUL-terminated
 * string to `out`. Returns 0 on success, -1 if `path` is NULL, empty,
 * or the result would not fit in `outsz`. */
int sanitize_path_for_name(const char *path, char *out, size_t outsz);

/* ── Take a snapshot ───────────────────────────────────────────────
 * NAR-hash the directory at `path`, copy it into
 * /nix/store/<base32>-snap-<sanitized-name>/ (idempotent: if the
 * content-addressed path already exists, reuse it), then insert a row
 * in the snapshot DB with parent_id = most recent snapshot of this
 * path. `scope` is "system" or the username; recorded for display.
 * `message` is an optional human note (may be NULL).
 *
 * Returns the new snapshot id (>0) on success, -1 on error (errno set
 * or a message printed to stderr). */
int64_t snapshot_take(snapshot_db_t *db, const char *path,
                      const char *scope, const char *message);

/* ── List snapshots ────────────────────────────────────────────────
 * With path_filter == NULL, list all snapshots across all paths.
 * With a non-NULL path_filter, list only snapshots whose `path` column
 * equals path_filter (exact match). Results are ordered by id
 * ascending. Returns 0 on success and writes a malloc'd array to
 * *out (caller frees with snapshot_record_list_free). Returns -1 on
 * error. *out is set to NULL and *count_out to 0 if no rows. */
int snapshot_list(snapshot_db_t *db, const char *path_filter,
                  snapshot_record_t **out, size_t *count_out);

/* ── Look up a single snapshot by id ───────────────────────────────
 * Returns 0 on success and fills *out (caller frees strings with
 * snapshot_record_free). Returns -1 if not found or on error. */
int snapshot_get(snapshot_db_t *db, int64_t id, snapshot_record_t *out);

/* ── Restore a snapshot ────────────────────────────────────────────
 * Before restoring, takes a snapshot of the path's current state (so
 * you can undo). Then replaces the path's contents with the snapshot's
 * store path contents. The original path is removed first; if the
 * snapshot is a directory, the path is recreated as a directory and
 * populated. Returns 0 on success, -1 on error. */
int snapshot_restore(snapshot_db_t *db, int64_t id);

/* ── Diff two snapshots ────────────────────────────────────────────
 * Walks both store paths recursively and reports per-file diffs.
 * id1 is the "from" snapshot, id2 is the "to" snapshot. Added =
 * in id2 only, removed = in id1 only, modified = in both with
 * different NAR hashes. Returns 0 on success and writes a malloc'd
 * array to *out (caller frees with snapshot_diff_list_free). Returns
 * -1 on error. */
int snapshot_diff(snapshot_db_t *db, int64_t id1, int64_t id2,
                  snapshot_diff_t **out, size_t *count_out);

/* ── Remove a snapshot from the DB ─────────────────────────────────
 * Deletes the snapshot row. The store path is left alone and will be
 * reaped by `209 gc` if nothing else references it. Children of this
 * snapshot (rows with parent_id == id) have their parent_id cleared
 * (set to NULL) rather than cascading. Returns 0 on success, -1 on
 * error. */
int snapshot_remove(snapshot_db_t *db, int64_t id);

/* ── Apply keep policy ─────────────────────────────────────────────
 * For the given path, keep only the most recent `keep` snapshots.
 * Older snapshots are removed via snapshot_remove(). keep == 0 means
 * keep all. Returns 0 on success, -1 on error. */
int snapshot_prune(snapshot_db_t *db, const char *path, int keep);

/* ── Managed-path check ────────────────────────────────────────────
 * Returns 1 if `path` appears as a key in the `snapshots` attrset of
 * `manifest_json` (the evaluated 2O9.nix output). Returns 0 if not.
 * Returns -1 on error (NULL input, JSON parse failure). */
int path_is_managed(const char *path, const char *manifest_json);

/* ── systemd timer install/remove ──────────────────────────────────
 * snapshot_install_timer: writes
 *   /etc/systemd/system/2O9-snap-<sanitized>.service
 *   /etc/systemd/system/2O9-snap-<sanitized>.timer
 * and runs `systemctl daemon-reload && systemctl enable --now <timer>`.
 * The service ExecStart is `/usr/bin/209 snapshot take <path>`. If
 * `user` is non-NULL and not "root", the service file has `User=<user>`.
 * `schedule` is one of "hourly", "daily", "weekly" (passed through to
 * systemd's OnCalendar=). Returns 0 on success, -1 on error.
 *
 * snapshot_remove_timer: runs `systemctl disable --now <timer>`,
 * unlinks both unit files, runs `systemctl daemon-reload`. Returns 0
 * on success, -1 on error (missing unit files are not an error). */
int snapshot_install_timer(const char *path, const char *schedule,
                           const char *user);
int snapshot_remove_timer(const char *path);

/* ── Free helpers ────────────────────────────────────────────────── */
void snapshot_record_free(snapshot_record_t *r);
void snapshot_record_list_free(snapshot_record_t *arr, size_t count);
void snapshot_diff_list_free(snapshot_diff_t *arr, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* TWO9_SNAPSHOT_H */
