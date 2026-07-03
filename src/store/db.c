/* db.c - SQLite store DB implementation
 *
 * See db.h for the schema rationale. The DB is a flat file at
 * <db_root>/store.sqlite - one per user (or one system-wide at
 * /var/lib/2O9/store.sqlite).
 *
 * Concurrency: sqlite3 handles its own locking (WAL mode + busy_timeout
 * set in open). 2O9 also takes a generation DB flock for higher-level
 * install/apply atomicity, so the SQLite layer only needs to be safe
 * against concurrent readers + a single writer.
 *
 * If HAVE_SQLITE3 is undefined at compile time, this file compiles to
 * stub implementations that always return failure. cmd_gc detects this
 * and falls back to the Phase 0/1 set-based algorithm. The build still
 * succeeds without libsqlite3, just with the refs graph disabled. */

#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

#ifdef HAVE_SQLITE3
#include <sqlite3.h>
#endif

#ifndef HAVE_SQLITE3

/* ── Stub implementations (sqlite3 unavailable at build time) ────────
 * Every function returns failure. cmd_gc falls back to set-based GC,
 * register_target_in_store_db becomes a no-op. The 209 binary still
 * builds and runs - just without Phase 2's refs graph. */

store_db_t *store_db_open(const char *db_root)
{
        (void)db_root;
        errno = ENOSYS;
        return NULL;
}

void store_db_close(store_db_t *db) { (void)db; }

int64_t store_db_register_path(store_db_t *db, const char *path,
                                const char *nar_hash, int64_t nar_size,
                                const char *deriver)
{
        (void)db; (void)path; (void)nar_hash; (void)nar_size; (void)deriver;
        errno = ENOSYS;
        return -1;
}

int store_db_add_ref(store_db_t *db, const char *referrer_path,
                     const char *reference_path)
{
        (void)db; (void)referrer_path; (void)reference_path;
        errno = ENOSYS;
        return -1;
}

int store_db_unregister_path(store_db_t *db, const char *path)
{
        (void)db; (void)path;
        errno = ENOSYS;
        return -1;
}

int store_db_has_path(store_db_t *db, const char *path)
{
        (void)db; (void)path;
        return -1;
}

char *store_db_find_by_name(store_db_t *db, const char *pkg_name)
{
        (void)db; (void)pkg_name;
        return NULL;
}

char **store_db_closure(store_db_t *db, char **roots, size_t n_roots)
{
        (void)db; (void)roots; (void)n_roots;
        return NULL;
}

char **store_db_get_refs(store_db_t *db, const char *path)
{
        (void)db; (void)path;
        return NULL;
}

char **store_db_dead_paths(store_db_t *db, char **live_closure, size_t n_live)
{
        (void)db; (void)live_closure; (void)n_live;
        return NULL;
}

char **store_db_all_paths(store_db_t *db)
{
        (void)db;
        return NULL;
}

#else /* HAVE_SQLITE3 */

/* ── Real SQLite implementation ───────────────────────────────────── */

struct store_db {
        sqlite3 *db;
        char *path;
};

/* SQL executed once on first open. IF NOT EXISTS makes it idempotent. */
static const char *SCHEMA_SQL =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS valid_paths ("
        "    id INTEGER PRIMARY KEY,"
        "    path TEXT UNIQUE NOT NULL,"
        "    hash TEXT NOT NULL,"
        "    nar_size INTEGER NOT NULL,"
        "    deriver TEXT,"
        "    sigs TEXT,"
        "    ca TEXT,"
        "    registrationTime INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS refs ("
        "    referrer INTEGER NOT NULL,"
        "    reference INTEGER NOT NULL,"
        "    PRIMARY KEY (referrer, reference),"
        "    FOREIGN KEY (referrer) REFERENCES valid_paths(id) ON DELETE CASCADE,"
        "    FOREIGN KEY (reference) REFERENCES valid_paths(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_refs_referrer ON refs(referrer);"
        "CREATE INDEX IF NOT EXISTS idx_refs_reference ON refs(reference);";

static int exec_simple(sqlite3 *db, const char *sql)
{
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
                if (err) {
                        fprintf(stderr, "209: store_db: %s\n", err);
                        sqlite3_free(err);
                }
                return -1;
        }
        return 0;
}

store_db_t *store_db_open(const char *db_root)
{
        if (!db_root) { errno = EINVAL; return NULL; }

        /* Ensure the directory exists (it usually does, since the
         * generation DB lives there too, but be defensive). */
        mkdir(db_root, 0755);

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/store.sqlite", db_root);

        sqlite3 *db = NULL;
        if (sqlite3_open(path, &db) != SQLITE_OK) {
                if (db) sqlite3_close(db);
                return NULL;
        }

        /* Set a 30s busy timeout so concurrent 2O9 processes don't fail
         * on transient SQLite locks. */
        sqlite3_busy_timeout(db, 30000);

        if (exec_simple(db, SCHEMA_SQL) < 0) {
                sqlite3_close(db);
                return NULL;
        }

        store_db_t *sdb = calloc(1, sizeof(*sdb));
        if (!sdb) { sqlite3_close(db); return NULL; }
        sdb->db = db;
        sdb->path = strdup(path);
        return sdb;
}

void store_db_close(store_db_t *db)
{
        if (!db) return;
        if (db->db) sqlite3_close(db->db);
        free(db->path);
        free(db);
}

/* ── Path registration ────────────────────────────────────────────── */

int64_t store_db_register_path(store_db_t *db, const char *path,
                                const char *nar_hash, int64_t nar_size,
                                const char *deriver)
{
        if (!db || !path || !nar_hash) { errno = EINVAL; return -1; }

        /* Upsert: INSERT ... ON CONFLICT(path) DO UPDATE. The hash/size
         * are updated in case the same path was re-extracted with
         * different content (shouldn't happen for content-addressed
         * paths, but cheap to handle). */
        static const char *SQL =
                "INSERT INTO valid_paths (path, hash, nar_size, deriver, "
                "  sigs, ca, registrationTime) "
                "VALUES (?1, ?2, ?3, ?4, NULL, NULL, ?5) "
                "ON CONFLICT(path) DO UPDATE SET "
                "  hash=excluded.hash, nar_size=excluded.nar_size, "
                "  deriver=excluded.deriver, "
                "  registrationTime=excluded.registrationTime "
                "RETURNING id;";

        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK) {
                fprintf(stderr, "209: store_db: prepare register: %s\n",
                        sqlite3_errmsg(db->db));
                return -1;
        }
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, nar_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, nar_size);
        if (deriver)
                sqlite3_bind_text(stmt, 4, deriver, -1, SQLITE_TRANSIENT);
        else
                sqlite3_bind_null(stmt, 4);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));

        int64_t id = -1;
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
                id = sqlite3_column_int64(stmt, 0);
        } else {
                fprintf(stderr, "209: store_db: register step: %s\n",
                        sqlite3_errmsg(db->db));
        }
        sqlite3_finalize(stmt);
        return id;
}

/* Helper: look up the row id for a path. Returns -1 if not found or error. */
static int64_t path_to_id(store_db_t *db, const char *path)
{
        if (!db || !path) return -1;
        static const char *SQL = "SELECT id FROM valid_paths WHERE path = ?1;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
                return -1;
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        int64_t id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
                id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
}

int store_db_add_ref(store_db_t *db, const char *referrer_path,
                     const char *reference_path)
{
        if (!db || !referrer_path || !reference_path) { errno = EINVAL; return -1; }

        int64_t referrer_id = path_to_id(db, referrer_path);
        int64_t reference_id = path_to_id(db, reference_path);
        if (referrer_id < 0 || reference_id < 0) {
                fprintf(stderr, "209: store_db: add_ref: both paths must be "
                        "registered first (referrer=%s id=%lld, reference=%s id=%lld)\n",
                        referrer_path, (long long)referrer_id,
                        reference_path, (long long)reference_id);
                return -1;
        }

        static const char *SQL =
                "INSERT OR IGNORE INTO refs (referrer, reference) VALUES (?1, ?2);";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
                return -1;
        sqlite3_bind_int64(stmt, 1, referrer_id);
        sqlite3_bind_int64(stmt, 2, reference_id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE || rc == SQLITE_CONSTRAINT) ? 0 : -1;
}

int store_db_unregister_path(store_db_t *db, const char *path)
{
        if (!db || !path) { errno = EINVAL; return -1; }

        /* Foreign keys are ON, so deleting the valid_paths row cascades
         * to refs where this path is referrer or reference. */
        static const char *SQL = "DELETE FROM valid_paths WHERE path = ?1;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
                return -1;
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE) ? 0 : -1;
}

int store_db_has_path(store_db_t *db, const char *path)
{
        if (!db || !path) return -1;
        static const char *SQL = "SELECT 1 FROM valid_paths WHERE path = ?1 LIMIT 1;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
                return -1;
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        int found = 0;
        if (rc == SQLITE_ROW) found = 1;
        else if (rc != SQLITE_DONE) found = -1;
        sqlite3_finalize(stmt);
        return found;
}

/* ── Name lookup ────────────────────────────────────────────────────
 *
 * The store path basename is <hash>-<name>-<version> (Phase 2) or
 * <name>-<version> (Phase 0/1). We extract <name> by finding the last
 * '-' that separates name from version, then the second-to-last '-'
 * separates hash from name. For legacy paths without a hash, the name
 * is everything before the last '-'.
 *
 * For the DB query, we match paths WHERE path LIKE '/nix/store/%-<name>-%'
 * AND path NOT LIKE '/nix/store/%<name>-%-%-%' (avoid matching name as
 * a substring of another name). Simplest correct approach: filter in C
 * after pulling all paths. */
char *store_db_find_by_name(store_db_t *db, const char *pkg_name)
{
        if (!db || !pkg_name) return NULL;

        /* Pull all paths and pattern-match in C. The DB is small
         * (hundreds to low thousands of paths) so this is fine. */
        static const char *SQL = "SELECT path FROM valid_paths ORDER BY path;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
                return NULL;

        char *result = NULL;
        size_t nlen = strlen(pkg_name);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(stmt, 0);
                if (!p) continue;
                /* Skip /nix/store/ prefix */
                const char *base = strrchr(p, '/');
                base = base ? base + 1 : p;
                /* Try to match "-<pkg_name>-" as a substring. */
                size_t blen = strlen(base);
                if (blen < nlen + 2) continue;
                /* Search for "-<pkg_name>-" anywhere in base. */
                char needle[300];
                snprintf(needle, sizeof(needle), "-%s-", pkg_name);
                if (strstr(base, needle) != NULL) {
                        result = strdup(p);
                        break;
                }
                /* Also try matching "<pkg_name>-" at the start (legacy
                 * /nix/store/<name>-<version> paths). */
                if (strncmp(base, pkg_name, nlen) == 0 && base[nlen] == '-') {
                        result = strdup(p);
                        break;
                }
        }
        sqlite3_finalize(stmt);
        return result;
}

/* ── Closure computation ────────────────────────────────────────────
 *
 * Iterative BFS: start with the roots, look up each path's refs, add
 * new ones to the queue. Stops when no new paths are added. Returns
 * the full closure as a NULL-terminated list.
 *
 * We use a simple growable array + linear scan for membership. DB size
 * is small enough that this is fine. */
char **store_db_closure(store_db_t *db, char **roots, size_t n_roots)
{
        if (!db || !roots || n_roots == 0) return NULL;

        /* Growable set. */
        size_t cap = 16;
        size_t count = 0;
        char **set = malloc(cap * sizeof(char *));
        if (!set) return NULL;

        /* Helper: add a path to the set if not already present. */
        #define SET_ADD(p) do {                                  \
                int _found = 0;                                  \
                for (size_t _i = 0; _i < count; _i++) {          \
                        if (strcmp(set[_i], (p)) == 0) {         \
                                _found = 1; break;               \
                        }                                        \
                }                                                \
                if (!_found) {                                   \
                        if (count == cap) {                      \
                                cap *= 2;                        \
                                char **_ns = realloc(set,        \
                                    cap * sizeof(char *));       \
                                if (!_ns) goto cleanup;          \
                                set = _ns;                       \
                        }                                        \
                        set[count++] = strdup((p));              \
                }                                                \
        } while (0)

        /* Seed with roots. */
        for (size_t i = 0; i < n_roots; i++) {
                if (roots[i]) SET_ADD(roots[i]);
        }

        /* BFS: process each path in the set, looking up its refs.
         * We use an index `processed` to avoid re-processing paths. */
        static const char *REFS_SQL =
                "SELECT vp2.path FROM refs r "
                "JOIN valid_paths vp1 ON r.referrer = vp1.id "
                "JOIN valid_paths vp2 ON r.reference = vp2.id "
                "WHERE vp1.path = ?1;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, REFS_SQL, -1, &stmt, NULL) != SQLITE_OK)
                goto cleanup;

        size_t processed = 0;
        while (processed < count) {
                const char *cur = set[processed++];
                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, cur, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                        const char *ref = (const char *)sqlite3_column_text(stmt, 0);
                        if (ref) SET_ADD(ref);
                }
        }
        sqlite3_finalize(stmt);

        /* NULL-terminate. */
        if (count == cap) {
                char **ns = realloc(set, (cap + 1) * sizeof(char *));
                if (!ns) goto cleanup;
                set = ns;
        }
        set[count] = NULL;
        return set;

cleanup:
        if (stmt) sqlite3_finalize(stmt);
        for (size_t i = 0; i < count; i++) free(set[i]);
        free(set);
        return NULL;

        #undef SET_ADD
}

char **store_db_dead_paths(store_db_t *db, char **live_closure, size_t n_live)
{
        if (!db) return NULL;

        static const char *ALL_SQL = "SELECT path FROM valid_paths ORDER BY path;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, ALL_SQL, -1, &stmt, NULL) != SQLITE_OK)
                return NULL;

        size_t cap = 16, count = 0;
        char **dead = malloc(cap * sizeof(char *));
        if (!dead) { sqlite3_finalize(stmt); return NULL; }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(stmt, 0);
                if (!p) continue;
                int is_live = 0;
                for (size_t i = 0; i < n_live; i++) {
                        if (live_closure[i] && strcmp(live_closure[i], p) == 0) {
                                is_live = 1;
                                break;
                        }
                }
                if (is_live) continue;
                if (count == cap) {
                        cap *= 2;
                        char **nd = realloc(dead, cap * sizeof(char *));
                        if (!nd) { sqlite3_finalize(stmt); goto fail; }
                        dead = nd;
                }
                dead[count++] = strdup(p);
        }
        sqlite3_finalize(stmt);

        if (count == cap) {
                char **nd = realloc(dead, (cap + 1) * sizeof(char *));
                if (!nd) goto fail;
                dead = nd;
        }
        dead[count] = NULL;
        return dead;

fail:
        for (size_t i = 0; i < count; i++) free(dead[i]);
        free(dead);
        return NULL;
}

char **store_db_all_paths(store_db_t *db)
{
        if (!db) return NULL;
        static const char *SQL = "SELECT path FROM valid_paths ORDER BY path;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, SQL, -1, &stmt, NULL) != SQLITE_OK)
                return NULL;

        size_t cap = 16, count = 0;
        char **all = malloc(cap * sizeof(char *));
        if (!all) { sqlite3_finalize(stmt); return NULL; }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(stmt, 0);
                if (!p) continue;
                if (count == cap) {
                        cap *= 2;
                        char **na = realloc(all, cap * sizeof(char *));
                        if (!na) { sqlite3_finalize(stmt); goto fail; }
                        all = na;
                }
                all[count++] = strdup(p);
        }
        sqlite3_finalize(stmt);

        if (count == cap) {
                char **na = realloc(all, (cap + 1) * sizeof(char *));
                if (!na) goto fail;
                all = na;
        }
        all[count] = NULL;
        return all;

fail:
        for (size_t i = 0; i < count; i++) free(all[i]);
        free(all);
        return NULL;
}

char **store_db_get_refs(store_db_t *db, const char *path)
{
        if (!db || !path) return NULL;

        static const char *REFS_SQL =
                "SELECT vp2.path FROM refs r "
                "JOIN valid_paths vp1 ON r.referrer = vp1.id "
                "JOIN valid_paths vp2 ON r.reference = vp2.id "
                "WHERE vp1.path = ?1 ORDER BY vp2.path;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->db, REFS_SQL, -1, &stmt, NULL) != SQLITE_OK)
                return NULL;

        size_t cap = 8, count = 0;
        char **refs = malloc(cap * sizeof(char *));
        if (!refs) { sqlite3_finalize(stmt); return NULL; }

        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(stmt, 0);
                if (!p) continue;
                /* Strip /nix/store/ prefix - narinfo References: uses
                 * basenames (e.g. "<hash>-<name>-<version>"). */
                const char *base = strrchr(p, '/');
                base = base ? base + 1 : p;

                if (count == cap) {
                        cap *= 2;
                        char **nr = realloc(refs, cap * sizeof(char *));
                        if (!nr) { sqlite3_finalize(stmt); goto fail; }
                        refs = nr;
                }
                refs[count++] = strdup(base);
        }
        sqlite3_finalize(stmt);

        if (count == 0) {
                /* No refs - return a valid empty list (caller may treat
                 * NULL as "unknown"; we want "known to have no refs"). */
                refs[0] = NULL;
                return refs;
        }
        if (count == cap) {
                char **nr = realloc(refs, (cap + 1) * sizeof(char *));
                if (!nr) goto fail;
                refs = nr;
        }
        refs[count] = NULL;
        return refs;

fail:
        for (size_t i = 0; i < count; i++) free(refs[i]);
        free(refs);
        return NULL;
}

#endif /* HAVE_SQLITE3 */
