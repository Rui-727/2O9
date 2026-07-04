/* snapshot.c - content-addressed snapshots of declared paths
 *
 * See snapshot.h for the design rationale. The DB layer mirrors
 * src/store/db.c: WAL mode, 30s busy timeout, IF NOT EXISTS schema
 * init, opaque handle. The filesystem layer reuses nar.c for hashing
 * and store path derivation, and shells out to cp -a for the actual
 * tree copy (a recursive C implementation would duplicate cp's
 * metadata preservation logic for no gain).
 *
 * If HAVE_SQLITE3 is undefined at build time, every function compiles
 * to a stub that returns failure. The 209 binary still builds, just
 * without snapshot support. cmd_snapshot in main.c prints a clear
 * "rebuilt without sqlite" message when the open fails.
 */

#include "snapshot.h"
#include "nar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>

#include <openssl/sha.h>

#include "cJSON.h"

#ifdef HAVE_SQLITE3
#include <sqlite3.h>
#endif

#ifndef HAVE_SQLITE3

/* ── Stub implementations (sqlite3 unavailable at build time) ─────── */

snapshot_db_t *snapshot_db_open(const char *db_root)
{
    (void)db_root;
    errno = ENOSYS;
    return NULL;
}
void snapshot_db_close(snapshot_db_t *db) { (void)db; }

int sanitize_path_for_name(const char *path, char *out, size_t outsz)
{
    if (!path || !out || outsz == 0) { errno = EINVAL; return -1; }
    if (*path != '/') { errno = EINVAL; return -1; }
    /* Sanitisation does not need sqlite; implement it here. */
    size_t off = 0;
    for (const char *p = path; *p && off + 1 < outsz; p++) {
        out[off++] = (*p == '/') ? '-' : *p;
    }
    /* Strip leading dashes (one or more from leading slashes). */
    size_t start = 0;
    while (start < off && out[start] == '-') start++;
    if (start > 0) memmove(out, out + start, off - start + 1);
    if (off == start) { errno = EINVAL; return -1; }
    return 0;
}

int64_t snapshot_take(snapshot_db_t *db, const char *path,
                      const char *scope, const char *message)
{
    (void)db; (void)path; (void)scope; (void)message;
    errno = ENOSYS; return -1;
}
int snapshot_list(snapshot_db_t *db, const char *path_filter,
                  snapshot_record_t **out, size_t *count_out)
{
    (void)db; (void)path_filter;
    if (out) *out = NULL;
    if (count_out) *count_out = 0;
    errno = ENOSYS; return -1;
}
int snapshot_get(snapshot_db_t *db, int64_t id, snapshot_record_t *out)
{
    (void)db; (void)id; (void)out;
    errno = ENOSYS; return -1;
}
int snapshot_restore(snapshot_db_t *db, int64_t id)
{
    (void)db; (void)id;
    errno = ENOSYS; return -1;
}
int snapshot_diff(snapshot_db_t *db, int64_t id1, int64_t id2,
                  snapshot_diff_t **out, size_t *count_out)
{
    (void)db; (void)id1; (void)id2;
    if (out) *out = NULL;
    if (count_out) *count_out = 0;
    errno = ENOSYS; return -1;
}
int snapshot_remove(snapshot_db_t *db, int64_t id)
{
    (void)db; (void)id;
    errno = ENOSYS; return -1;
}
int snapshot_prune(snapshot_db_t *db, const char *path, int keep)
{
    (void)db; (void)path; (void)keep;
    errno = ENOSYS; return -1;
}

#else /* HAVE_SQLITE3 */

/* ── Real SQLite implementation ──────────────────────────────────── */

struct snapshot_db {
    sqlite3 *db;
    char    *path;
};

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS snapshots ("
    "    id INTEGER PRIMARY KEY,"
    "    path TEXT NOT NULL,"
    "    store_path TEXT NOT NULL,"
    "    nar_hash TEXT NOT NULL,"
    "    parent_id INTEGER,"
    "    message TEXT,"
    "    taken_at INTEGER NOT NULL,"
    "    scope TEXT NOT NULL,"
    "    FOREIGN KEY (parent_id) REFERENCES snapshots(id)"
    "        ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_snapshots_path ON snapshots(path);"
    "CREATE INDEX IF NOT EXISTS idx_snapshots_parent ON snapshots(parent_id);";

static int exec_simple(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "209: snapshot_db: %s\n", err);
            sqlite3_free(err);
        }
        return -1;
    }
    return 0;
}

snapshot_db_t *snapshot_db_open(const char *db_root)
{
    if (!db_root) { errno = EINVAL; return NULL; }
    mkdir(db_root, 0755);

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/snapshots.sqlite", db_root)
        >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return NULL;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 30000);

    if (exec_simple(db, SCHEMA_SQL) < 0) {
        sqlite3_close(db);
        return NULL;
    }

    snapshot_db_t *sdb = calloc(1, sizeof(*sdb));
    if (!sdb) { sqlite3_close(db); return NULL; }
    sdb->db = db;
    sdb->path = strdup(path);
    return sdb;
}

void snapshot_db_close(snapshot_db_t *db)
{
    if (!db) return;
    if (db->db) sqlite3_close(db->db);
    free(db->path);
    free(db);
}

/* ── Path sanitisation ───────────────────────────────────────────── */

int sanitize_path_for_name(const char *path, char *out, size_t outsz)
{
    if (!path || !out || outsz == 0) { errno = EINVAL; return -1; }
    if (*path != '/') { errno = EINVAL; return -1; }

    size_t off = 0;
    for (const char *p = path; *p && off + 1 < outsz; p++) {
        /* Replace '/' with '-'. Other path-unsafe chars get replaced
         * too so the result is a valid systemd unit-name component. */
        char c = *p;
        if (c == '/' || c == '\0' || c == ' ' || c == '\t' || c == '\n')
            c = '-';
        out[off++] = c;
    }
    out[off] = '\0';

    /* Strip leading dashes (from leading slashes). */
    size_t start = 0;
    while (start < off && out[start] == '-') start++;
    if (start > 0) memmove(out, out + start, off - start + 1);
    if (out[0] == '\0') { errno = EINVAL; return -1; }
    return 0;
}

/* Return the store root. The hash fingerprint always uses the literal
 * "/nix/store" so content addresses match across machines, but the
 * actual filesystem location can be overridden via TWO9_STORE_ROOT
 * for testing. Mirrors the TWO9_CONFIG_DIR override pattern. */
static const char *snapshot_store_root(void)
{
    const char *env = getenv("TWO9_STORE_ROOT");
    return (env && *env) ? env : "/nix/store";
}

/* ── Store path derivation for snapshots ───────────────────────────
 *
 * Same fingerprint formula as compute_store_path() in nar.c but with
 * no version component: "output:out:sha256:<hash>:/nix/store:snap-<name>".
 * The fingerprint always uses the literal "/nix/store" so content
 * addresses match across machines. The actual filesystem location
 * (returned in the result string) honours TWO9_STORE_ROOT for testing.
 * Result: <store_root>/<base32>-snap-<sanitized-name>. Caller frees. */
static char *compute_snapshot_store_path(const char *nar_hash_hex,
                                         const char *sanitized_name)
{
    if (!nar_hash_hex || !sanitized_name) { errno = EINVAL; return NULL; }

    char fp[PATH_MAX];
    if (snprintf(fp, sizeof(fp), "output:out:sha256:%s:/nix/store:snap-%s",
                 nar_hash_hex, sanitized_name) >= (int)sizeof(fp)) {
        errno = ENAMETOOLONG; return NULL;
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)fp, strlen(fp), digest);

    static const char base32[] = "0123456789abcdfghijklmnpqrsvwxyz";
    char hash32[33];
    for (int g = 0; g < 4; g++) {
        unsigned long long n = 0;
        for (int b = 0; b < 5; b++)
            n |= ((unsigned long long)digest[g * 5 + b]) << (8 * b);
        for (int c = 0; c < 8; c++) {
            hash32[g * 8 + c] = base32[n & 0x1f];
            n >>= 5;
        }
    }
    hash32[32] = '\0';

    const char *store_root = snapshot_store_root();
    char *out = malloc(PATH_MAX);
    if (!out) return NULL;
    if (snprintf(out, PATH_MAX, "%s/%s-snap-%s",
                 store_root, hash32, sanitized_name) >= PATH_MAX) {
        free(out); errno = ENAMETOOLONG; return NULL;
    }
    return out;
}

/* ── Recursive tree copy ───────────────────────────────────────────
 *
 * Shells out to `cp -a` so we get correct metadata (perms, mtimes,
 * symlink targets, xattrs where supported). A pure-C implementation
 * would duplicate cp's logic for no real gain. */
static int copy_tree(const char *src, const char *dst_parent)
{
    char cmd[PATH_MAX * 2 + 32];
    int n = snprintf(cmd, sizeof(cmd), "cp -a -- '%s' '%s'", src, dst_parent);
    if (n < 0 || n >= (int)sizeof(cmd)) { errno = ENAMETOOLONG; return -1; }
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "209: snapshot: cp -a %s -> %s failed (rc=%d)\n",
                src, dst_parent, rc);
        return -1;
    }
    return 0;
}

/* Recursively remove a directory tree. */
static int rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) rmtree(child);
            else                     unlink(child);
        }
    }
    closedir(d);
    return rmdir(path);
}

/* ── snapshot_take ───────────────────────────────────────────────── */

int64_t snapshot_take(snapshot_db_t *db, const char *path,
                      const char *scope, const char *message)
{
    if (!db || !path || !scope) { errno = EINVAL; return -1; }

    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "209: snapshot: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "209: snapshot: %s: not a directory\n", path);
        return -1;
    }

    /* Sanitise path for store path basename. */
    char sanitized[PATH_MAX];
    if (sanitize_path_for_name(path, sanitized, sizeof(sanitized)) < 0) {
        fprintf(stderr, "209: snapshot: cannot sanitize %s\n", path);
        return -1;
    }

    /* Ensure <store_root>/.tmp exists. */
    const char *store_root = snapshot_store_root();
    /* Best-effort: create parent dirs if they don't exist. mkdir -p
     * style. Ignore EEXIST. */
    {
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "%s", store_root);
        for (char *p = buf + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(buf, 0755);
                *p = '/';
            }
        }
        mkdir(buf, 0755);
    }
    {
        char tmp_base[PATH_MAX];
        snprintf(tmp_base, sizeof(tmp_base), "%s/.tmp", store_root);
        mkdir(tmp_base, 0700);
    }

    /* Stage the copy in a temp dir, then hash it, then rename to the
     * content-addressed store path. The pid suffix avoids collisions
     * with concurrent snapshot_take() calls on the same path. */
    char tmpdir[PATH_MAX];
    if (snprintf(tmpdir, sizeof(tmpdir),
                 "%s/.tmp/snap-%s-%d", store_root, sanitized, (int)getpid())
        >= (int)sizeof(tmpdir)) {
        errno = ENAMETOOLONG; return -1;
    }
    rmtree(tmpdir);
    if (mkdir(tmpdir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "209: snapshot: mkdir %s: %s\n",
                tmpdir, strerror(errno));
        return -1;
    }

    /* Copy the source path INTO tmpdir. cp -a places the source dir
     * as a child of dst_parent, so tmpdir/<basename> ends up holding
     * the tree. We hash that subtree so the snapshot NAR matches what
     * nar_hash_directory would produce on the original. */
    if (copy_tree(path, tmpdir) < 0) {
        rmtree(tmpdir);
        return -1;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char hashable[PATH_MAX];
    if (snprintf(hashable, sizeof(hashable), "%s/%s", tmpdir, base)
        >= (int)sizeof(hashable)) {
        errno = ENAMETOOLONG; rmtree(tmpdir); return -1;
    }

    char nar_hash[65];
    size_t nar_size = 0;
    if (nar_hash_directory(hashable, nar_hash, &nar_size) < 0) {
        fprintf(stderr, "209: snapshot: NAR hash failed for %s\n", hashable);
        rmtree(tmpdir);
        return -1;
    }

    char *store_path = compute_snapshot_store_path(nar_hash, sanitized);
    if (!store_path) { rmtree(tmpdir); return -1; }

    /* If the content-addressed path already exists (same content was
     * snapshotted before), reuse it and discard the temp copy. */
    struct stat sp_st;
    if (stat(store_path, &sp_st) == 0) {
        rmtree(tmpdir);
    } else {
        /* Rename the staged hashable subdir into place. We need to
         * move tmpdir/<base> to /nix/store/<hash>-snap-<sanitized>.
         * rename() across dirs on the same filesystem is atomic. */
        if (rename(hashable, store_path) < 0) {
            fprintf(stderr, "209: snapshot: rename %s -> %s: %s\n",
                    hashable, store_path, strerror(errno));
            rmtree(tmpdir);
            free(store_path);
            return -1;
        }
        rmtree(tmpdir);
    }

    /* Find the most recent snapshot of this path to use as parent. */
    int64_t parent_id = 0;
    {
        static const char *SQL =
            "SELECT id FROM snapshots WHERE path = ?1 "
            "ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                parent_id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    /* Insert the new snapshot row. */
    static const char *INS =
        "INSERT INTO snapshots "
        "  (path, store_path, nar_hash, parent_id, message, taken_at, scope) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->db, INS, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "209: snapshot: prepare insert: %s\n",
                sqlite3_errmsg(db->db));
        free(store_path);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, store_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, nar_hash, -1, SQLITE_TRANSIENT);
    if (parent_id > 0)
        sqlite3_bind_int64(stmt, 4, parent_id);
    else
        sqlite3_bind_null(stmt, 4);
    if (message)
        sqlite3_bind_text(stmt, 5, message, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 7, scope, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_int64 new_id = sqlite3_last_insert_rowid(db->db);
    sqlite3_finalize(stmt);
    free(store_path);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "209: snapshot: insert step: %s\n",
                sqlite3_errmsg(db->db));
        return -1;
    }
    return new_id;
}

/* ── snapshot_list ───────────────────────────────────────────────── */

int snapshot_list(snapshot_db_t *db, const char *path_filter,
                  snapshot_record_t **out, size_t *count_out)
{
    if (!db || !out || !count_out) { errno = EINVAL; return -1; }
    *out = NULL; *count_out = 0;

    const char *SQL = path_filter
        ? "SELECT id, path, store_path, nar_hash, parent_id, message, "
          "taken_at, scope FROM snapshots WHERE path = ?1 ORDER BY id ASC;"
        : "SELECT id, path, store_path, nar_hash, parent_id, message, "
          "taken_at, scope FROM snapshots ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (path_filter)
        sqlite3_bind_text(stmt, 1, path_filter, -1, SQLITE_TRANSIENT);

    size_t cap = 16, count = 0;
    snapshot_record_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count == cap) {
            cap *= 2;
            snapshot_record_t *na = realloc(arr, cap * sizeof(*arr));
            if (!na) { sqlite3_finalize(stmt); goto fail; }
            arr = na;
        }
        snapshot_record_t *r = &arr[count++];
        r->id        = sqlite3_column_int64(stmt, 0);
        r->path      = strdup((const char *)sqlite3_column_text(stmt, 1));
        r->store_path= strdup((const char *)sqlite3_column_text(stmt, 2));
        r->nar_hash  = strdup((const char *)sqlite3_column_text(stmt, 3));
        r->parent_id = sqlite3_column_type(stmt, 4) == SQLITE_NULL
                       ? 0 : sqlite3_column_int64(stmt, 4);
        if (sqlite3_column_type(stmt, 5) == SQLITE_NULL)
            r->message = NULL;
        else
            r->message = strdup((const char *)sqlite3_column_text(stmt, 5));
        r->taken_at  = sqlite3_column_int64(stmt, 6);
        r->scope     = strdup((const char *)sqlite3_column_text(stmt, 7));
    }
    sqlite3_finalize(stmt);

    *out = arr;
    *count_out = count;
    return 0;

fail:
    snapshot_record_list_free(arr, count);
    return -1;
}

/* ── snapshot_get ────────────────────────────────────────────────── */

int snapshot_get(snapshot_db_t *db, int64_t id, snapshot_record_t *out)
{
    if (!db || !out) { errno = EINVAL; return -1; }
    static const char *SQL =
        "SELECT id, path, store_path, nar_hash, parent_id, message, "
        "taken_at, scope FROM snapshots WHERE id = ?1 LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }

    out->id        = sqlite3_column_int64(stmt, 0);
    out->path      = strdup((const char *)sqlite3_column_text(stmt, 1));
    out->store_path= strdup((const char *)sqlite3_column_text(stmt, 2));
    out->nar_hash  = strdup((const char *)sqlite3_column_text(stmt, 3));
    out->parent_id = sqlite3_column_type(stmt, 4) == SQLITE_NULL
                     ? 0 : sqlite3_column_int64(stmt, 4);
    if (sqlite3_column_type(stmt, 5) == SQLITE_NULL)
        out->message = NULL;
    else
        out->message = strdup((const char *)sqlite3_column_text(stmt, 5));
    out->taken_at  = sqlite3_column_int64(stmt, 6);
    out->scope     = strdup((const char *)sqlite3_column_text(stmt, 7));
    sqlite3_finalize(stmt);
    return 0;
}

/* ── snapshot_restore ────────────────────────────────────────────── */

int snapshot_restore(snapshot_db_t *db, int64_t id)
{
    if (!db) { errno = EINVAL; return -1; }

    snapshot_record_t r = {0};
    if (snapshot_get(db, id, &r) < 0) {
        fprintf(stderr, "209: snapshot: id %lld not found\n",
                (long long)id);
        return -1;
    }

    /* Take a snapshot of the current state first so the user can
     * undo the restore. If the path doesn't currently exist, skip
     * (the snapshot_take() call would fail). */
    struct stat st;
    if (lstat(r.path, &st) == 0) {
        int64_t pre = snapshot_take(db, r.path, r.scope,
                                    "auto: pre-restore state");
        if (pre < 0) {
            fprintf(stderr, "209: snapshot: pre-restore snapshot failed, "
                    "aborting restore\n");
            snapshot_record_free(&r);
            return -1;
        }
        printf("  pre-restore snapshot: id %lld\n", (long long)pre);
    }

    /* Remove the existing path, then copy the snapshot's store path
     * back into place. The snapshot store path contains the original
     * directory's contents directly (file.txt is at the root of the
     * store path, not at store_path/<orig_basename>/). So we recreate
     * r.path as a copy of store_path. */
    if (lstat(r.path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) rmtree(r.path);
        else                     unlink(r.path);
    }

    /* Find parent dir of r.path. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", r.path);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        fprintf(stderr, "209: snapshot: %s: cannot find parent dir\n", r.path);
        snapshot_record_free(&r);
        return -1;
    }
    *slash = '\0';
    if (parent[0] == '\0') strcpy(parent, "/");

    /* Ensure parent exists. */
    mkdir(parent, 0755);  /* best-effort; may already exist */

    /* `cp -a <store_path> <r.path>` when r.path doesn't exist creates
     * r.path as a copy of store_path, so r.path ends up with the
     * original directory's contents. */
    char cmd[PATH_MAX * 2 + 32];
    if (snprintf(cmd, sizeof(cmd), "cp -a -- '%s' '%s'",
                 r.store_path, r.path) >= (int)sizeof(cmd)) {
        errno = ENAMETOOLONG;
        snapshot_record_free(&r);
        return -1;
    }
    if (system(cmd) != 0) {
        fprintf(stderr, "209: snapshot: restore copy %s -> %s failed\n",
                r.store_path, r.path);
        snapshot_record_free(&r);
        return -1;
    }

    printf("  restored %s from snapshot %lld\n", r.path, (long long)id);
    snapshot_record_free(&r);
    return 0;
}

/* ── Tree walk for diff ──────────────────────────────────────────── */

typedef struct file_entry {
    char *rel_path;     /* relative to root, with leading '/' */
    char *hash;          /* 64-char hex of the file's contents, or
                           "<dir>" for directories, "<link:<target>>"
                           for symlinks. NULL on hash error. */
    struct file_entry *next;
} file_entry_t;

/* Compute a stable per-file fingerprint. For regular files this is
 * the SHA-256 of the file's bytes (NOT the NAR encoding; we want a
 * per-file hash so two files with different mtimes but identical
 * content compare equal). For symlinks we hash the target string.
 * Directories get a fixed marker (they don't have content; only
 * their children matter for diffing). */
static char *file_fingerprint(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return NULL;

    if (S_ISDIR(st.st_mode)) return strdup("<dir>");

    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0) return NULL;
        target[n] = '\0';
        char *out = malloc(n + 8);
        if (!out) return NULL;
        snprintf(out, n + 8, "<link:%s>", target);
        return out;
    }

    if (!S_ISREG(st.st_mode)) return strdup("<special>");

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        SHA256_Update(&ctx, buf, n);
    fclose(f);
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256_Final(d, &ctx);

    char *hex = malloc(65);
    if (!hex) return NULL;
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        hex[i*2]   = H[d[i] >> 4];
        hex[i*2+1] = H[d[i] & 0x0f];
    }
    hex[64] = '\0';
    return hex;
}

static file_entry_t *walk_tree(const char *root, const char *prefix)
{
    file_entry_t *head = NULL, **tail = &head;
    DIR *d = opendir(root);
    if (!d) return NULL;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) continue;

        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", root, ent->d_name)
            >= (int)sizeof(child)) continue;

        char rel[PATH_MAX];
        if (snprintf(rel, sizeof(rel), "%s/%s", prefix, ent->d_name)
            >= (int)sizeof(rel)) continue;

        file_entry_t *fe = calloc(1, sizeof(*fe));
        if (!fe) continue;
        fe->rel_path = strdup(rel);
        fe->hash     = file_fingerprint(child);
        *tail = fe; tail = &fe->next;

        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            file_entry_t *sub = walk_tree(child, rel);
            if (sub) {
                *tail = sub;
                while (*tail) tail = &(*tail)->next;
            }
        }
    }
    closedir(d);
    return head;
}

static void file_entry_list_free(file_entry_t *head)
{
    while (head) {
        file_entry_t *next = head->next;
        free(head->rel_path);
        free(head->hash);
        free(head);
        head = next;
    }
}

static file_entry_t *find_entry(file_entry_t *head, const char *rel_path)
{
    for (file_entry_t *e = head; e; e = e->next)
        if (strcmp(e->rel_path, rel_path) == 0) return e;
    return NULL;
}

int snapshot_diff(snapshot_db_t *db, int64_t id1, int64_t id2,
                  snapshot_diff_t **out, size_t *count_out)
{
    if (!db || !out || !count_out) { errno = EINVAL; return -1; }
    *out = NULL; *count_out = 0;

    snapshot_record_t r1 = {0}, r2 = {0};
    if (snapshot_get(db, id1, &r1) < 0) {
        fprintf(stderr, "209: snapshot: id %lld not found\n",
                (long long)id1);
        return -1;
    }
    if (snapshot_get(db, id2, &r2) < 0) {
        fprintf(stderr, "209: snapshot: id %lld not found\n",
                (long long)id2);
        snapshot_record_free(&r1);
        return -1;
    }

    /* Both snapshots share the same original path (we diff snapshots
     * of the same path). The store path layout puts the original
     * directory as a child of the store path, so walk
     * <store_path>/<original-basename>/. */
    const char *walk_into_a = r1.store_path;
    const char *walk_into_b = r2.store_path;

    /* The store path contains a single child dir named after the
     * original path basename. Walk that. */
    file_entry_t *a = walk_tree(walk_into_a, "");
    file_entry_t *b = walk_tree(walk_into_b, "");

    size_t cap = 32, count = 0;
    snapshot_diff_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { file_entry_list_free(a); file_entry_list_free(b);
                snapshot_record_free(&r1); snapshot_record_free(&r2);
                return -1; }

    /* Walk A: entries in A but not in B (or different hash) = removed
     * or modified. */
    for (file_entry_t *e = a; e; e = e->next) {
        file_entry_t *m = find_entry(b, e->rel_path);
        if (!m) {
            if (count == cap) {
                cap *= 2;
                arr = realloc(arr, cap * sizeof(*arr));
            }
            arr[count].kind    = SNAP_DIFF_REMOVED;
            arr[count].rel_path= strdup(e->rel_path);
            arr[count].hash_a  = e->hash ? strdup(e->hash) : NULL;
            arr[count].hash_b  = NULL;
            count++;
        } else if (!e->hash || !m->hash ||
                   strcmp(e->hash, m->hash) != 0) {
            if (count == cap) {
                cap *= 2;
                arr = realloc(arr, cap * sizeof(*arr));
            }
            arr[count].kind    = SNAP_DIFF_MODIFIED;
            arr[count].rel_path= strdup(e->rel_path);
            arr[count].hash_a  = e->hash ? strdup(e->hash) : NULL;
            arr[count].hash_b  = m->hash ? strdup(m->hash) : NULL;
            count++;
        }
    }
    /* Walk B: entries in B but not in A = added. */
    for (file_entry_t *e = b; e; e = e->next) {
        file_entry_t *m = find_entry(a, e->rel_path);
        if (!m) {
            if (count == cap) {
                cap *= 2;
                arr = realloc(arr, cap * sizeof(*arr));
            }
            arr[count].kind    = SNAP_DIFF_ADDED;
            arr[count].rel_path= strdup(e->rel_path);
            arr[count].hash_a  = NULL;
            arr[count].hash_b  = e->hash ? strdup(e->hash) : NULL;
            count++;
        }
    }

    file_entry_list_free(a);
    file_entry_list_free(b);
    snapshot_record_free(&r1);
    snapshot_record_free(&r2);

    *out = arr;
    *count_out = count;
    return 0;
}

/* ── snapshot_remove ─────────────────────────────────────────────── */

int snapshot_remove(snapshot_db_t *db, int64_t id)
{
    if (!db) { errno = EINVAL; return -1; }
    /* Children's parent_id is set to NULL via the ON DELETE SET NULL
     * foreign key declared in the schema. The store path is left in
     * place; gc will reap it if nothing else references it. */
    static const char *SQL = "DELETE FROM snapshots WHERE id = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── snapshot_prune ──────────────────────────────────────────────── */

int snapshot_prune(snapshot_db_t *db, const char *path, int keep)
{
    if (!db || !path) { errno = EINVAL; return -1; }
    if (keep <= 0) return 0;  /* 0 = keep all */

    snapshot_record_t *list = NULL;
    size_t count = 0;
    if (snapshot_list(db, path, &list, &count) < 0) return -1;

    /* list is ordered by id ASC; keep the last `keep`. */
    if (count <= (size_t)keep) {
        snapshot_record_list_free(list, count);
        return 0;
    }
    size_t to_remove = count - (size_t)keep;
    for (size_t i = 0; i < to_remove; i++) {
        snapshot_remove(db, list[i].id);
    }
    snapshot_record_list_free(list, count);
    return 0;
}

#endif /* HAVE_SQLITE3 */

/* ── Free helpers (available in all builds) ──────────────────────── */

void snapshot_record_free(snapshot_record_t *r)
{
    if (!r) return;
    free(r->path); free(r->store_path); free(r->nar_hash);
    free(r->message); free(r->scope);
}
void snapshot_record_list_free(snapshot_record_t *arr, size_t count)
{
    if (!arr) return;
    for (size_t i = 0; i < count; i++) snapshot_record_free(&arr[i]);
    free(arr);
}
void snapshot_diff_list_free(snapshot_diff_t *arr, size_t count)
{
    if (!arr) return;
    for (size_t i = 0; i < count; i++) {
        free(arr[i].rel_path); free(arr[i].hash_a); free(arr[i].hash_b);
    }
    free(arr);
}

/* ── path_is_managed ───────────────────────────────────────────────
 *
 * Implemented without sqlite: walks the manifest JSON's snapshots
 * attrset and checks if `path` is a key. Available in both stub and
 * real builds. */
int path_is_managed(const char *path, const char *manifest_json)
{
    if (!path || !manifest_json) { errno = EINVAL; return -1; }
    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) return -1;
    int found = 0;
    cJSON *snaps = cJSON_GetObjectItem(root, "snapshots");
    if (snaps && cJSON_IsObject(snaps)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, snaps) {
            if (entry->string && strcmp(entry->string, path) == 0) {
                found = 1; break;
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

/* ── systemd timer install/remove ──────────────────────────────────
 *
 * Available in both stub and real builds (doesn't need sqlite). */

int snapshot_install_timer(const char *path, const char *schedule,
                           const char *user)
{
    if (!path || !schedule) { errno = EINVAL; return -1; }

    char sanitized[PATH_MAX];
    if (sanitize_path_for_name(path, sanitized, sizeof(sanitized)) < 0)
        return -1;

    /* Validate schedule: pass through to systemd, but only allow
     * hourly/daily/weekly to keep the manifest schema simple. */
    const char *on_calendar;
    if      (strcmp(schedule, "hourly") == 0) on_calendar = "hourly";
    else if (strcmp(schedule, "daily")  == 0) on_calendar = "daily";
    else if (strcmp(schedule, "weekly") == 0) on_calendar = "weekly";
    else {
        fprintf(stderr, "209: snapshot: invalid schedule '%s' "
                "(want hourly/daily/weekly)\n", schedule);
        errno = EINVAL; return -1;
    }

    /* Resolve the 209 binary path. The service ExecStart needs an
     * absolute path; use /proc/self/exe so the same binary that
     * installed the timer is the one that runs. */
    char bin[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", bin, sizeof(bin) - 1);
    if (n < 0) {
        strncpy(bin, "/usr/bin/209", sizeof(bin) - 1);
        bin[sizeof(bin) - 1] = '\0';
    } else {
        bin[n] = '\0';
    }

    char unit_base[PATH_MAX];
    if (snprintf(unit_base, sizeof(unit_base),
                 "2O9-snap-%s", sanitized) >= (int)sizeof(unit_base)) {
        errno = ENAMETOOLONG; return -1;
    }

    char svc_path[PATH_MAX];
    char timer_path[PATH_MAX];
    snprintf(svc_path,   sizeof(svc_path),
             "/etc/systemd/system/%s.service", unit_base);
    snprintf(timer_path, sizeof(timer_path),
             "/etc/systemd/system/%s.timer", unit_base);

    /* Write service file. */
    mkdir("/etc/systemd", 0755);
    mkdir("/etc/systemd/system", 0755);
    FILE *f = fopen(svc_path, "w");
    if (!f) {
        fprintf(stderr, "209: snapshot: cannot write %s: %s\n",
                svc_path, strerror(errno));
        return -1;
    }
    fprintf(f, "[Unit]\n");
    fprintf(f, "Description=2O9 snapshot of %s\n", path);
    fprintf(f, "\n[Service]\n");
    fprintf(f, "Type=oneshot\n");
    if (user && strcmp(user, "root") != 0)
        fprintf(f, "User=%s\n", user);
    fprintf(f, "ExecStart=%s snapshot take %s\n", bin, path);
    fclose(f);

    /* Write timer file. */
    f = fopen(timer_path, "w");
    if (!f) {
        fprintf(stderr, "209: snapshot: cannot write %s: %s\n",
                timer_path, strerror(errno));
        unlink(svc_path);
        return -1;
    }
    fprintf(f, "[Unit]\n");
    fprintf(f, "Description=2O9 snapshot timer for %s\n", path);
    fprintf(f, "\n[Timer]\n");
    fprintf(f, "OnCalendar=%s\n", on_calendar);
    fprintf(f, "Persistent=true\n");
    fprintf(f, "\n[Install]\n");
    fprintf(f, "WantedBy=timers.target\n");
    fclose(f);

    /* Reload + enable. */
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd),
             "systemctl daemon-reload 2>/dev/null; "
             "systemctl enable --now %s.timer 2>/dev/null",
             unit_base);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "209: snapshot: warning: systemctl enable --now %s.timer "
                "returned %d (is systemd running?)\n", unit_base, rc);
        /* Not fatal; unit files are written. */
    }
    return 0;
}

int snapshot_remove_timer(const char *path)
{
    if (!path) { errno = EINVAL; return -1; }

    char sanitized[PATH_MAX];
    if (sanitize_path_for_name(path, sanitized, sizeof(sanitized)) < 0)
        return -1;

    char unit_base[PATH_MAX];
    if (snprintf(unit_base, sizeof(unit_base),
                 "2O9-snap-%s", sanitized) >= (int)sizeof(unit_base)) {
        errno = ENAMETOOLONG; return -1;
    }

    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd),
             "systemctl disable --now %s.timer 2>/dev/null; "
             "true",  /* ignore exit code if unit doesn't exist */
             unit_base);
    system(cmd);

    char svc_path[PATH_MAX];
    char timer_path[PATH_MAX];
    snprintf(svc_path,   sizeof(svc_path),
             "/etc/systemd/system/%s.service", unit_base);
    snprintf(timer_path, sizeof(timer_path),
             "/etc/systemd/system/%s.timer", unit_base);
    unlink(svc_path);
    unlink(timer_path);

    system("systemctl daemon-reload 2>/dev/null; true");
    return 0;
}


