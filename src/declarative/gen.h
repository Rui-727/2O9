/* gen.h — 2O9 generation database
 *
 * A generation is a snapshot of the system's package set. Each time
 * you install, remove, or apply, a new generation is committed.
 * Rolling back = repointing the profile symlink to an earlier generation.
 *
 * Storage layout (from DESIGN.md §7):
 *
 *   /var/lib/2O9/
 *   ├── generations/
 *   │   ├── 1/manifest.json      ← first generation
 *   │   ├── 2/manifest.json      ← second generation
 *   │   └── ...
 *   ├── current → generations/42  ← symlink to current generation
 *   ├── lock                      ← flock() lockfile (concurrency safety)
 *   └── pinned                    ← file listing pinned generation numbers
 *
 *   Per-user:
 *   ~/.local/state/2O9/
 *   ├── generations/
 *   │   └── ...
 *   └── current → generations/3
 *
 * This is file-based, not sqlite. Simple, auditable, easy to debug.
 * (sqlite is optional per DESIGN.md §9 dependency table.)
 */

#ifndef TWO9_GEN_H
#define TWO9_GEN_H

#include <stddef.h>
#include <time.h>

/* A single package entry in a generation */
typedef struct gen_pkg {
        char *name;
        char *version;
        char *store_path;    /* /nix/store/<name>-<version> (no hash) */
        char *origin;        /* "repo", "aur", "imperative" */
        struct gen_pkg *next;
} gen_pkg_t;

/* A generation snapshot */
typedef struct gen {
        int id;              /* 1, 2, 3, ... */
        time_t timestamp;
        char *manifest_path; /* path to manifest.json on disk */
        gen_pkg_t *packages;
        size_t pkg_count;
        int is_pinned;       /* 1 = protected from GC */
} gen_t;

/* Generation DB handle */
typedef struct gen_db {
        char *root;          /* /var/lib/2O9 or ~/.local/state/2O9 */
        int scope;           /* 0 = system, 1 = user */
        int lock_fd;         /* fd for /var/lib/2O9/lock (flock), -1 if unlocked */
} gen_db_t;

/* Open a generation DB. root = base directory (e.g. /var/lib/2O9) */
gen_db_t *gen_db_open(const char *root);

/* Close and free */
void gen_db_close(gen_db_t *db);

/* Get current generation number. Returns 0 if none. */
int gen_db_current(gen_db_t *db);

/* Get a generation by number. Returns NULL if not found. */
gen_t *gen_db_get(gen_db_t *db, int id);

/* Commit a new generation with the given package set.
 * Returns the new generation ID, or -1 on error. */
int gen_db_commit(gen_db_t *db, gen_pkg_t *packages);

/* Roll back to a specific generation.
 * Repoints the 'current' symlink. Returns 0 on success. */
int gen_db_rollback(gen_db_t *db, int target_id);

/* List all generations (newest first). Returns array, sets *count.
 * Caller must free with gen_list_free(). */
gen_t **gen_db_list(gen_db_t *db, size_t *count);

/* Pin/unpin a generation (protect from GC) */
int gen_db_pin(gen_db_t *db, int id);
int gen_db_unpin(gen_db_t *db, int id);

/* Lock/unlock the DB for mutating operations.
 * gen_db_lock() takes an exclusive, non-blocking flock on
 * <root>/lock. Returns 0 on success, -1 if already locked.
 * The lock is released by gen_db_unlock() or gen_db_close().
 * Call gen_db_lock() before commit, rollback, pin, or gc. */
int gen_db_lock(gen_db_t *db);
void gen_db_unlock(gen_db_t *db);

/* Free a single generation */
void gen_free(gen_t *g);

/* Free an array of generations */
void gen_list_free(gen_t **list, size_t count);

/* Free a package linked list */
void gen_pkg_list_free(gen_pkg_t *p);

/* Create a gen_pkg */
gen_pkg_t *gen_pkg_create(const char *name, const char *version,
                          const char *store_path, const char *origin);

#endif /* TWO9_GEN_H */
