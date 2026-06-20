/* symlinks.c — 2O9 symlink farm builder implementation
 *
 * Walks a generation's package manifest, creates symlinks from
 * store paths to user-visible locations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>

#include "symlinks.h"
#include "gen.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static int mkdirs(const char *path)
{
        char tmp[PATH_MAX];
        char *p = NULL;
        size_t len;

        snprintf(tmp, sizeof(tmp), "%s", path);
        len = strlen(tmp);
        if (tmp[len - 1] == '/')
                tmp[len - 1] = '\0';

        for (p = tmp + 1; *p; p++) {
                if (*p == '/') {
                        *p = '\0';
                        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                                return -1;
                        *p = '/';
                }
        }
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;

        return 0;
}

static char *get_home_dir(void)
{
        const char *home = getenv("HOME");
        if (home) return strdup(home);

        struct passwd *pw = getpwuid(getuid());
        if (pw) return strdup(pw->pw_dir);

        return strdup("/root");
}

/* ── Symlink primitives ──────────────────────────────────────────── */

int symlink_create(const char *target, const char *link_path)
{
        /* Create parent directory */
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", link_path);
        char *slash = strrchr(parent, '/');
        if (slash) {
                *slash = '\0';
                if (mkdirs(parent) < 0)
                        return -1;
        }

        /* Remove existing file/symlink at link_path */
        unlink(link_path);

        /* Create symlink */
        if (symlink(target, link_path) < 0)
                return -1;

        return 0;
}

int symlink_remove(const char *link_path)
{
        /* Only remove if it's a symlink — don't nuke real files */
        struct stat st;
        if (lstat(link_path, &st) < 0)
                return 0;  /* already gone */
        if (!S_ISLNK(st.st_mode))
                return 0;  /* not a symlink, leave it alone */

        return unlink(link_path);
}

/* ── Symlink farm for one package ────────────────────────────────── */

static int symlink_package(const char *store_path, const char *pkg_name)
{
        char *home = get_home_dir();
        if (!home) return -1;

        /* TODO: walk the store directory and create symlinks.
         * For Phase 1 MVP, we create a few well-known symlinks
         * based on common directory conventions in .pkg.tar.zst:
         *
         *   store_path/bin/      → ~/.local/bin/<name>
         *   store_path/lib/      → ~/.local/lib/<name>
         *   store_path/etc/      → /etc/<name>          (needs root)
         *   store_path/usr/bin/  → ~/.local/bin/<name>
         *
         * The full implementation will scan the store directory
         * and create a symlink for each file, following the
         * classification rules from DESIGN.md §7.
         */

        /* For now, create the directories to prove the mechanism works */
        char bin_dir[PATH_MAX];
        snprintf(bin_dir, sizeof(bin_dir), "%s/.local/bin", home);
        mkdirs(bin_dir);

        char lib_dir[PATH_MAX];
        snprintf(lib_dir, sizeof(lib_dir), "%s/.local/lib", home);
        mkdirs(lib_dir);

        free(home);
        (void)store_path;
        (void)pkg_name;
        return 0;
}

/* ── Build symlink farm for a generation ─────────────────────────── */

int symlink_farm_build(gen_db_t *db, gen_t *gen, gen_t *prev_gen)
{
        if (!gen) return -1;

        /* If we have a previous generation, tear down its symlinks first.
         * This is the simple approach — remove all, then rebuild.
         * A smarter diff-based approach would only touch changed files,
         * but that's an optimization for later. */
        if (prev_gen) {
                symlink_farm_teardown(prev_gen);
        }

        /* Build symlinks for each package in the new generation */
        gen_pkg_t *pkg = gen->packages;
        while (pkg) {
                if (pkg->store_path) {
                        symlink_package(pkg->store_path, pkg->name);
                }
                pkg = pkg->next;
        }

        (void)db;
        return 0;
}

/* ── Teardown ────────────────────────────────────────────────────── */

int symlink_farm_teardown(gen_t *gen)
{
        if (!gen) return -1;

        /* TODO: walk the manifest and remove each symlink.
         * For now this is a no-op — the build function recreates
         * everything anyway, and stale symlinks pointing to the
         * store are harmless (the store path just won't exist
         * after GC). */
        return 0;
}
