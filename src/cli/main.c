/* 2O9 CLI — main entry point
 *
 * The `209` binary. SOV (Subject Object Verb) command dispatch.
 *
 * Phase 0: -V, -h
 * Phase 1: <pkg> install, <n> rollback, generations, apply, sync, gc
 *
 * From DESIGN.md §5:
 *   209 <subject> <verb>          — SOV pattern
 *   209 <command>                 — zero-argument command
 *   209 <pkg1> <pkg2> <verb>     — multi-subject
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "store/store.h"
#include "declarative/gen.h"
#include "store/symlinks.h"

#ifndef PACKAGE
#define PACKAGE "2O9"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.0.1"
#endif

#define VERSION_STR PACKAGE " " PACKAGE_VERSION

/* ── Zero-argument commands ──────────────────────────────────────── */

static int cmd_version(void)
{
        printf("%s\n", VERSION_STR);
        printf("License: GPL-2.0-only\n");
        printf("Store root: %s\n", STORE_ROOT);
        return 0;
}

static int cmd_usage(void)
{
        printf("Usage: 209 [options] <subject> <verb>\n");
        printf("       209 [options] <command>\n\n");
        printf("Unified package manager: pacman + AUR + Nix store\n\n");
        printf("Commands:\n");
        printf("  apply              Apply declarative config (2O9.nix)\n");
        printf("  generations        List generations\n");
        printf("  sync               Sync repo databases\n");
        printf("  gc                 Garbage-collect unreferenced store paths\n");
        printf("  news               Show Arch Linux news\n\n");
        printf("SOV patterns:\n");
        printf("  209 <pkg> install  Install package from repo\n");
        printf("  209 <pkg> remove   Remove package\n");
        printf("  209 <term> search  Search repos\n");
        printf("  209 <pkg> aur build    Build from AUR\n");
        printf("  209 <term> aur search  Search AUR\n");
        printf("  209 <pkg> aur review   Review PKGBUILD diff\n");
        printf("  209 <pkg> trakker [flags]  Run in sandbox\n\n");
        printf("Rollback:\n");
        printf("  209 <n> rollback   Roll back to generation #n\n");
        printf("  209 <n> pin        Pin a generation (protect from GC)\n\n");
        printf("Options:\n");
        printf("  -V, --version      Show version\n");
        printf("  -h, --help         Show this help\n");
        return 0;
}

/* ── Generations ─────────────────────────────────────────────────── */

static int cmd_generations(void)
{
        char *home = getenv("HOME");
        char db_root[PATH_MAX];

        /* Try user DB first, fall back to system */
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB at %s\n", db_root);
                return 1;
        }

        size_t count;
        gen_t **gens = gen_db_list(db, &count);

        int current = gen_db_current(db);

        if (count == 0) {
                printf("No generations yet.\n");
        } else {
                printf("  ID  Packages  Pinned\n");
                printf("  ──  ────────  ──────\n");
                for (size_t i = 0; i < count; i++) {
                        printf("  %3d  %7zu  %s%s\n",
                               gens[i]->id,
                               gens[i]->pkg_count,
                               gens[i]->is_pinned ? "yes" : "no",
                               gens[i]->id == current ? "  ← current" : "");
                }
        }

        gen_list_free(gens, count);
        gen_db_close(db);
        return 0;
}

/* ── Install ─────────────────────────────────────────────────────── */

static int cmd_install(const char *pkg_name)
{
        /* Phase 1 MVP: this is the imperative install path.
         * The package lands in the current generation temporarily.
         * Next `209 apply` will flag it for removal unless it's in 2O9.nix.
         */

        printf("209: installing %s...\n", pkg_name);

        /* Step 1: resolve package via lib209 (TODO — Phase 1 proper)
         *   For now, we assume the .pkg.tar.zst is already downloaded
         *   and we just add it to the store. */

        /* Step 2: add to store */
        char pkg_path[PATH_MAX];
        pkg_path[0] = '\0';
        const char *env_path = getenv("TWO09_PKG_PATH");
        if (env_path) {
                snprintf(pkg_path, sizeof(pkg_path), "%s", env_path);
        }

        store_add_result_t result;

        /* If TWO09_TEST_MODE is set, skip nix-store and use a fake store path.
         * This lets us test the generation/rollback/symlink pipeline without nix. */
        const char *test_mode = getenv("TWO09_TEST_MODE");
        if (test_mode) {
                char fake_store[PATH_MAX];
                snprintf(fake_store, sizeof(fake_store), "/nix/store/fake-%s-0.0.0", pkg_name);
                result.success = 0;
                result.store_path = strdup(fake_store);
                result.error_msg = NULL;
                printf("  [test mode] fake store path: %s\n", fake_store);
        } else if (pkg_path[0] == '\0') {
                /* No pkg path and no test mode — can't proceed */
                fprintf(stderr, "209: package resolution not yet implemented\n");
                fprintf(stderr, "    set TWO09_PKG_PATH=/path/to/pkg.tar.zst to test store add\n");
                fprintf(stderr, "    or set TWO09_TEST_MODE=1 for end-to-end pipeline test\n");
                return 1;
        } else {
                result = store_add(pkg_path, STORE_BACKEND_NIX_STORE);
                if (result.success != 0) {
                        fprintf(stderr, "209: store add failed: %s\n", result.error_msg);
                        store_add_result_free(&result);
                        return 1;
                }
        }

        printf("  store path: %s\n", result.store_path);

        /* Step 3: commit new generation */
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                store_add_result_free(&result);
                return 1;
        }

        /* Build package entry for the new generation.
         * TODO: merge with current generation's packages, add new one. */
        gen_pkg_t *pkg = gen_pkg_create(pkg_name, "0.0.0",
                                        result.store_path, "imperative");

        int new_id = gen_db_commit(db, pkg);
        if (new_id < 0) {
                fprintf(stderr, "209: failed to commit generation\n");
                gen_pkg_list_free(pkg);
                gen_db_close(db);
                store_add_result_free(&result);
                return 1;
        }

        printf("  generation #%d committed\n", new_id);

        /* Step 4: build symlink farm */
        gen_t *gen = gen_db_get(db, new_id);
        if (gen) {
                /* For MVP, no previous generation diff */
                symlink_farm_build(db, gen, NULL);
                gen_free(gen);
        }

        printf("  done. rollback with: 209 %d rollback\n", new_id - 1 > 0 ? new_id - 1 : 1);

        gen_pkg_list_free(pkg);
        gen_db_close(db);
        store_add_result_free(&result);
        return 0;
}

/* ── Rollback ────────────────────────────────────────────────────── */

static int cmd_rollback(int target_id)
{
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                return 1;
        }

        int current = gen_db_current(db);
        if (current == 0) {
                fprintf(stderr, "209: no generations to roll back to\n");
                gen_db_close(db);
                return 1;
        }

        printf("209: rolling back from #%d to #%d...\n", current, target_id);

        if (gen_db_rollback(db, target_id) < 0) {
                fprintf(stderr, "209: rollback failed — generation #%d not found\n", target_id);
                gen_db_close(db);
                return 1;
        }

        /* Rebuild symlink farm from the target generation */
        gen_t *gen = gen_db_get(db, target_id);
        if (gen) {
                symlink_farm_build(db, gen, NULL);
                gen_free(gen);
        }

        printf("  now at generation #%d\n", target_id);
        gen_db_close(db);
        return 0;
}

/* ── Apply (declarative) ─────────────────────────────────────────── */

static int cmd_apply(void)
{
        /* TODO: Phase 3 — evaluate 2O9.nix, reconcile with current generation,
         * build transaction, commit, rebuild symlink farm. */
        fprintf(stderr, "209: apply not yet implemented (Phase 3)\n");
        return 1;
}

/* ── Sync ────────────────────────────────────────────────────────── */

static int cmd_sync(void)
{
        /* TODO: invoke lib209's sync to refresh repo databases */
        fprintf(stderr, "209: sync not yet implemented (needs lib209)\n");
        return 1;
}

/* ── GC ──────────────────────────────────────────────────────────── */

static int cmd_gc(void)
{
        /* TODO: walk store, find paths not referenced by any generation,
         * delete them. Similar to nix-collect-garbage. */
        fprintf(stderr, "209: gc not yet implemented\n");
        return 1;
}

/* ── SOV dispatch ────────────────────────────────────────────────── */

/* Check if a string is a number (generation ID) */
static int is_number(const char *s)
{
        if (!s || !*s) return 0;
        while (*s) {
                if (*s < '0' || *s > '9') return 0;
                s++;
        }
        return 1;
}

int main(int argc, char *argv[])
{
        /* Handle options */
        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-V") == 0 ||
                    strcmp(argv[i], "--version") == 0) {
                        return cmd_version();
                }
                if (strcmp(argv[i], "-h") == 0 ||
                    strcmp(argv[i], "--help") == 0) {
                        return cmd_usage();
                }
        }

        if (argc < 2) {
                cmd_usage();
                return 1;
        }

        /* Zero-argument commands */
        if (strcmp(argv[1], "apply") == 0)       return cmd_apply();
        if (strcmp(argv[1], "generations") == 0)  return cmd_generations();
        if (strcmp(argv[1], "sync") == 0)         return cmd_sync();
        if (strcmp(argv[1], "gc") == 0)           return cmd_gc();
        if (strcmp(argv[1], "news") == 0) {
                fprintf(stderr, "209: news not yet implemented\n");
                return 1;
        }

        /* SOV pattern: need at least subject + verb */
        if (argc < 3) {
                fprintf(stderr, "209: expected <subject> <verb>\n");
                fprintf(stderr, "    try: 209 --help\n");
                return 1;
        }

        const char *subject = argv[1];
        const char *verb = argv[2];

        /* Install */
        if (strcmp(verb, "install") == 0) {
                /* Multi-subject: 209 nginx firefox install */
                for (int i = 1; i < argc - 1; i++) {
                        int rc = cmd_install(argv[i]);
                        if (rc != 0) return rc;
                }
                return 0;
        }

        /* Remove */
        if (strcmp(verb, "remove") == 0) {
                fprintf(stderr, "209: remove not yet implemented\n");
                return 1;
        }

        /* Search */
        if (strcmp(verb, "search") == 0) {
                fprintf(stderr, "209: search not yet implemented\n");
                return 1;
        }

        /* AUR */
        if (strcmp(subject, "aur") == 0 ||
            (argc >= 4 && strcmp(argv[2], "aur") == 0)) {
                fprintf(stderr, "209: AUR commands not yet implemented (Phase 2)\n");
                return 1;
        }

        /* Trakker */
        if (strcmp(verb, "trakker") == 0) {
                fprintf(stderr, "209: trakker not yet implemented (Phase 4)\n");
                return 1;
        }

        /* Rollback / pin — subject is a generation number */
        if (is_number(subject)) {
                int id = atoi(subject);
                if (strcmp(verb, "rollback") == 0)  return cmd_rollback(id);
                if (strcmp(verb, "pin") == 0) {
                        char *home = getenv("HOME");
                        char db_root[PATH_MAX];
                        if (home) {
                                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
                        } else {
                                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
                        }
                        gen_db_t *db = gen_db_open(db_root);
                        if (!db) {
                                fprintf(stderr, "209: cannot open generation DB\n");
                                return 1;
                        }
                        if (gen_db_pin(db, id) < 0) {
                                fprintf(stderr, "209: failed to pin generation #%d\n", id);
                                gen_db_close(db);
                                return 1;
                        }
                        printf("209: generation #%d pinned\n", id);
                        gen_db_close(db);
                        return 0;
                }
        }

        fprintf(stderr, "209: unknown command: %s %s\n", subject, verb);
        fprintf(stderr, "    try: 209 --help\n");
        return 1;
}
