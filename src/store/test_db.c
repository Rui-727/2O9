/* test_db.c - unit tests for the SQLite store DB refs graph
 *
 * Exercises store_db_register_path / add_ref / closure / dead_paths /
 * unregister_path on a temp DB. Each test opens a fresh DB to keep
 * state isolated. Skips cleanly if HAVE_SQLITE3 is undefined at build
 * time (the symbols return stub failures; the test reports SKIP).
 *
 * Build: see Makefile target `test-db`.
 * Run: ./test-db
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "db.h"

static int test_count = 0;
static int pass_count = 0;
static int skip_count = 0;

#define OK(msg) do { printf("PASS: %s\n", msg); pass_count++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)
#define SKIP(msg) do { printf("SKIP: %s\n", msg); skip_count++; } while (0)

#define CHECK(cond, name) do { \
        test_count++; \
        if (cond) OK(name); \
        else FAIL(name); \
} while (0)

/* Make a fresh empty DB dir under /tmp. Caller frees path + rmtrees it. */
static char *fresh_db_dir(const char *tag)
{
        char tmpl[128];
        snprintf(tmpl, sizeof(tmpl), "/tmp/209-test-db-%s-XXXXXX", tag);
        char *d = strdup(tmpl);
        if (!d) return NULL;
        if (mkdtemp(d) == NULL) { free(d); return NULL; }
        return d;
}

static void rmtree_recursive(const char *path)
{
        DIR *d = opendir(path);
        if (!d) { rmdir(path); return; }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                        continue;
                char child[1024];
                snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                struct stat st;
                if (lstat(child, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) rmtree_recursive(child);
                        else unlink(child);
                }
        }
        closedir(d);
        rmdir(path);
}

/* Helper: count entries in a NULL-terminated list. */
static size_t list_len(char **list)
{
        if (!list) return 0;
        size_t n = 0;
        while (list[n]) n++;
        return n;
}

static int list_contains(char **list, const char *s)
{
        if (!list) return 0;
        for (size_t i = 0; list[i]; i++)
                if (strcmp(list[i], s) == 0) return 1;
        return 0;
}

static void free_list(char **list)
{
        if (!list) return;
        for (size_t i = 0; list[i]; i++) free(list[i]);
        free(list);
}

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_register_and_has(void)
{
        char *dir = fresh_db_dir("reg");
        if (!dir) { CHECK(0, "register: mkdtemp"); return; }

        store_db_t *db = store_db_open(dir);
        if (!db) {
                SKIP("register: store_db_open returned NULL (sqlite unavailable?)");
                rmtree_recursive(dir); free(dir); return;
        }

        int64_t id = store_db_register_path(db, "/nix/store/aaa-foo-1.0",
                                            "sha256:deadbeef", 100, NULL);
        CHECK(id >= 1, "register: returns row id >= 1");

        int has = store_db_has_path(db, "/nix/store/aaa-foo-1.0");
        CHECK(has == 1, "register: has_path returns 1 for registered path");

        int has2 = store_db_has_path(db, "/nix/store/zzz-not-there-2.0");
        CHECK(has2 == 0, "register: has_path returns 0 for unknown path");

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_register_idempotent(void)
{
        char *dir = fresh_db_dir("idem");
        if (!dir) { CHECK(0, "idem: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("idem: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        int64_t id1 = store_db_register_path(db, "/nix/store/aaa-foo-1.0",
                                             "sha256:hash1", 100, NULL);
        int64_t id2 = store_db_register_path(db, "/nix/store/aaa-foo-1.0",
                                             "sha256:hash2", 200, NULL);
        CHECK(id1 == id2, "idem: re-register returns same row id");

        char **all = store_db_all_paths(db);
        size_t n = list_len(all);
        CHECK(n == 1, "idem: all_paths returns exactly 1 entry (no dup)");
        free_list(all);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_closure_one_level(void)
{
        char *dir = fresh_db_dir("closure1");
        if (!dir) { CHECK(0, "closure1: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("closure1: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/A", "sha256:a", 1, NULL);
        store_db_register_path(db, "/nix/store/B", "sha256:b", 1, NULL);
        int r = store_db_add_ref(db, "/nix/store/A", "/nix/store/B");
        CHECK(r == 0, "closure1: add_ref A->B returns 0");

        char *roots[] = { "/nix/store/A" };
        char **clos = store_db_closure(db, roots, 1);
        CHECK(clos != NULL, "closure1: closure returns non-NULL");
        CHECK(list_len(clos) == 2, "closure1: closure has 2 entries (A + B)");
        CHECK(list_contains(clos, "/nix/store/A"), "closure1: closure contains A");
        CHECK(list_contains(clos, "/nix/store/B"), "closure1: closure contains B");
        free_list(clos);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_closure_transitive(void)
{
        char *dir = fresh_db_dir("closure2");
        if (!dir) { CHECK(0, "closure2: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("closure2: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/A", "sha256:a", 1, NULL);
        store_db_register_path(db, "/nix/store/B", "sha256:b", 1, NULL);
        store_db_register_path(db, "/nix/store/C", "sha256:c", 1, NULL);
        store_db_add_ref(db, "/nix/store/A", "/nix/store/B");
        store_db_add_ref(db, "/nix/store/B", "/nix/store/C");

        char *roots[] = { "/nix/store/A" };
        char **clos = store_db_closure(db, roots, 1);
        CHECK(list_len(clos) == 3, "closure2: closure has 3 entries (A + B + C)");
        CHECK(list_contains(clos, "/nix/store/C"), "closure2: closure contains transitive C");
        free_list(clos);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_closure_cycle(void)
{
        char *dir = fresh_db_dir("cycle");
        if (!dir) { CHECK(0, "cycle: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("cycle: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/A", "sha256:a", 1, NULL);
        store_db_register_path(db, "/nix/store/B", "sha256:b", 1, NULL);
        store_db_add_ref(db, "/nix/store/A", "/nix/store/B");
        store_db_add_ref(db, "/nix/store/B", "/nix/store/A");

        char *roots[] = { "/nix/store/A" };
        char **clos = store_db_closure(db, roots, 1);
        /* Must terminate (not infinite-loop) and return exactly A + B. */
        CHECK(clos != NULL, "cycle: closure terminates with non-NULL result");
        CHECK(list_len(clos) == 2, "cycle: closure has exactly 2 entries (no dup from cycle)");
        free_list(clos);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_dead_paths(void)
{
        char *dir = fresh_db_dir("dead");
        if (!dir) { CHECK(0, "dead: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("dead: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/A", "sha256:a", 1, NULL);
        store_db_register_path(db, "/nix/store/B", "sha256:b", 1, NULL);
        store_db_register_path(db, "/nix/store/C", "sha256:c", 1, NULL);

        /* Live closure = [A, B]; C is dead. */
        char *live[] = { "/nix/store/A", "/nix/store/B" };
        char **dead = store_db_dead_paths(db, live, 2);
        CHECK(dead != NULL, "dead: dead_paths returns non-NULL");
        CHECK(list_len(dead) == 1, "dead: exactly 1 dead path (C)");
        CHECK(list_contains(dead, "/nix/store/C"), "dead: dead path is C");
        free_list(dead);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_dead_paths_empty_closure(void)
{
        /* No live closure -> all paths are dead. */
        char *dir = fresh_db_dir("deadall");
        if (!dir) { CHECK(0, "deadall: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("deadall: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/A", "sha256:a", 1, NULL);
        store_db_register_path(db, "/nix/store/B", "sha256:b", 1, NULL);

        char **dead = store_db_dead_paths(db, NULL, 0);
        CHECK(dead != NULL, "deadall: dead_paths(NULL, 0) returns non-NULL");
        CHECK(list_len(dead) == 2, "deadall: all 2 paths are dead with empty closure");
        free_list(dead);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_unregister_cascades(void)
{
        char *dir = fresh_db_dir("unreg");
        if (!dir) { CHECK(0, "unreg: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("unreg: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/A", "sha256:a", 1, NULL);
        store_db_register_path(db, "/nix/store/B", "sha256:b", 1, NULL);
        store_db_add_ref(db, "/nix/store/A", "/nix/store/B");

        /* Verify ref exists. */
        char **refs = store_db_get_refs(db, "/nix/store/A");
        CHECK(refs != NULL && list_len(refs) == 1, "unreg: A has 1 ref (B) before unregister");
        free_list(refs);

        /* Unregister A; both A's row AND its refs should be removed. */
        int r = store_db_unregister_path(db, "/nix/store/A");
        CHECK(r == 0, "unreg: unregister_path returns 0");
        CHECK(store_db_has_path(db, "/nix/store/A") == 0, "unreg: A is gone after unregister");

        /* B should still be there (we only removed A; B is a separate path). */
        CHECK(store_db_has_path(db, "/nix/store/B") == 1, "unreg: B remains after A unregister");

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

static void test_get_refs_empty(void)
{
        char *dir = fresh_db_dir("getrefs");
        if (!dir) { CHECK(0, "getrefs: mkdtemp"); return; }
        store_db_t *db = store_db_open(dir);
        if (!db) { SKIP("getrefs: store_db_open returned NULL"); rmtree_recursive(dir); free(dir); return; }

        store_db_register_path(db, "/nix/store/lonely", "sha256:l", 1, NULL);
        char **refs = store_db_get_refs(db, "/nix/store/lonely");
        /* Should return a non-NULL list with zero entries (NULL terminator
         * at index 0). The spec says "known to have no refs" vs NULL
         * "unknown". */
        CHECK(refs != NULL, "getrefs: returns non-NULL list for path with no refs");
        CHECK(list_len(refs) == 0, "getrefs: list has 0 entries");
        free_list(refs);

        store_db_close(db);
        rmtree_recursive(dir); free(dir);
}

int main(void)
{
        printf("=== Store DB (refs graph) tests ===\n\n");

        test_register_and_has();
        test_register_idempotent();
        test_closure_one_level();
        test_closure_transitive();
        test_closure_cycle();
        test_dead_paths();
        test_dead_paths_empty_closure();
        test_unregister_cascades();
        test_get_refs_empty();

        printf("\n=== Results: %d/%d passed, %d skipped ===\n",
               pass_count, test_count, skip_count);
        /* Skips are acceptable (build without sqlite). Failures are not. */
        return (pass_count + skip_count) == test_count ? 0 : 1;
}
