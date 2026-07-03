/* db.h - SQLite store DB with references graph
 *
 * Phase 2: 2O9's GC was wrong because it didn't know about transitive
 * deps. A package's .PKGINFO lists `depend = foo`, `depend = bar: 1.0`,
 * etc - GC needs the full closure (foo's deps, foo's deps' deps, ...) to
 * decide what's safe to delete.
 *
 * The DB has two tables:
 *   valid_paths(id, path, hash, nar_size, deriver, sigs, ca, registrationTime)
 *   refs(referrer, reference)   -- referrer depends on reference
 *
 * The schema mirrors Nix's nix-store --query interface so future Phase 3
 * binary cache work can reuse the same data.
 *
 * DB location: <db_root>/store.sqlite where db_root is the same dir as
 * the generation DB (system: /var/lib/2O9, user: ~/.local/state/2O9).
 * This is created on first call to store_db_open().
 */
#ifndef TWO9_STORE_DB_H
#define TWO9_STORE_DB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct store_db store_db_t;

/* Open (or create) the store DB at <db_root>/store.sqlite.
 * Returns NULL on failure (sqlite3_open failed, schema init failed).
 * The caller owns the returned handle and must close it with
 * store_db_close(). */
store_db_t *store_db_open(const char *db_root);
void store_db_close(store_db_t *db);

/* Register a path with its NAR hash + size. Idempotent: if path exists,
 * update hash/size/registrationTime and return the existing row id.
 * Returns -1 on error, >=1 on success (the row id). */
int64_t store_db_register_path(store_db_t *db, const char *path,
                                const char *nar_hash, int64_t nar_size,
                                const char *deriver);

/* Add a ref: referrer depends on reference. Both paths must already be
 * registered (call store_db_register_path first). Idempotent.
 * Returns 0 on success, -1 on error. */
int store_db_add_ref(store_db_t *db, const char *referrer_path,
                     const char *reference_path);

/* Remove a path from the DB. Also removes any refs where this path is
 * referrer or reference (the latter via the ON DELETE CASCADE FK).
 * Returns 0 on success, -1 on error. */
int store_db_unregister_path(store_db_t *db, const char *path);

/* Check if a path is registered. Returns 1 if yes, 0 if no, -1 on error. */
int store_db_has_path(store_db_t *db, const char *path);

/* Look up the store path for a registered package by package name.
 * The package name is parsed out of the store path basename
 * (/nix/store/<hash>-<name>-<version> -> name is the middle component).
 * Returns a malloc'd path string (caller frees) or NULL if not found. */
char *store_db_find_by_name(store_db_t *db, const char *pkg_name);

/* Compute the closure of a set of root paths. Returns a NULL-terminated
 * list of paths (caller frees each string and the list itself). Includes
 * the roots themselves. Returns NULL on error or empty input. */
char **store_db_closure(store_db_t *db, char **roots, size_t n_roots);

/* List all valid paths NOT in the given live closure. Used by GC to
 * find dead paths. Returns a NULL-terminated list (caller frees).
 * Returns NULL on error. */
char **store_db_dead_paths(store_db_t *db, char **live_closure, size_t n_live);

/* List all paths in the DB. Returns a NULL-terminated list (caller frees).
 * Useful for the GC's "scan /nix/store/ for orphans" step. */
char **store_db_all_paths(store_db_t *db);

#ifdef __cplusplus
}
#endif

#endif /* TWO9_STORE_DB_H */
