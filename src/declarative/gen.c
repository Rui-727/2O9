/* gen.c — 2O9 generation database implementation
 *
 * File-based generation tracking. Each generation is a directory
 * containing a manifest.json. The current generation is a symlink.
 *
 * This is intentionally simple. No sqlite, no complex queries.
 * You want to know what changed between gen 41 and 42? diff the manifests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

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

static int dir_exists(const char *path)
{
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ── Package list helpers ────────────────────────────────────────── */

gen_pkg_t *gen_pkg_create(const char *name, const char *version,
                          const char *store_path, const char *origin)
{
        gen_pkg_t *p = calloc(1, sizeof(*p));
        if (!p) return NULL;
        p->name = strdup(name);
        p->version = strdup(version);
        p->store_path = store_path ? strdup(store_path) : NULL;
        p->origin = strdup(origin);
        return p;
}

void gen_pkg_list_free(gen_pkg_t *p)
{
        while (p) {
                gen_pkg_t *next = p->next;
                free(p->name);
                free(p->version);
                free(p->store_path);
                free(p->origin);
                free(p);
                p = next;
        }
}

/* ── DB open/close ───────────────────────────────────────────────── */

gen_db_t *gen_db_open(const char *root)
{
        gen_db_t *db = calloc(1, sizeof(*db));
        if (!db) return NULL;

        db->root = strdup(root);
        db->scope = 0;
        db->lock_fd = -1;

        /* Ensure directory structure exists */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/generations", root);
        if (mkdirs(path) < 0) {
                free(db->root);
                free(db);
                return NULL;
        }

        return db;
}

void gen_db_close(gen_db_t *db)
{
        if (!db) return;
        if (db->lock_fd >= 0) {
                close(db->lock_fd);
                db->lock_fd = -1;
        }
        free(db->root);
        free(db);
}

/* ── Current generation ──────────────────────────────────────────── */

int gen_db_current(gen_db_t *db)
{
        char link[PATH_MAX];
        char target[PATH_MAX];

        snprintf(link, sizeof(link), "%s/current", db->root);

        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n < 0)
                return 0;  /* no current generation */
        target[n] = '\0';

        /* Extract the generation number from the target path.
         * Target is like ".../generations/42" */
        char *slash = strrchr(target, '/');
        if (!slash)
                return 0;

        return atoi(slash + 1);
}

/* ── Write manifest ──────────────────────────────────────────────── */

static int write_manifest(gen_db_t *db, int id, gen_pkg_t *packages)
{
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/generations/%d", db->root, id);

        if (mkdirs(dir) < 0)
                return -1;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/manifest.json", dir);

        FILE *f = fopen(path, "w");
        if (!f)
                return -1;

        fprintf(f, "{\n");
        fprintf(f, "  \"id\": %d,\n", id);
        fprintf(f, "  \"timestamp\": %ld,\n", (long)time(NULL));
        fprintf(f, "  \"packages\": [\n");

        gen_pkg_t *p = packages;
        int first = 1;
        while (p) {
                if (!first) fprintf(f, ",\n");
                first = 0;
                fprintf(f, "    {\"name\": \"%s\", \"version\": \"%s\"", p->name, p->version);
                if (p->store_path)
                        fprintf(f, ", \"store_path\": \"%s\"", p->store_path);
                fprintf(f, ", \"origin\": \"%s\"}", p->origin);
                p = p->next;
        }

        fprintf(f, "\n  ]\n");
        fprintf(f, "}\n");
        fclose(f);
        return 0;
}

/* ── Lockfile ────────────────────────────────────────────────────── */

int gen_db_lock(gen_db_t *db)
{
        if (!db) return -1;

        char lock_path[PATH_MAX];
        snprintf(lock_path, sizeof(lock_path), "%s/lock", db->root);

        int fd = open(lock_path, O_RDWR | O_CREAT, 0644);
        if (fd < 0)
                return -1;

        /* Non-blocking lock. If another 2O9 process holds it, fail fast. */
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
                close(fd);
                return -1;  /* already locked */
        }

        db->lock_fd = fd;
        return 0;
}

void gen_db_unlock(gen_db_t *db)
{
        if (!db || db->lock_fd < 0) return;
        flock(db->lock_fd, LOCK_UN);
        close(db->lock_fd);
        db->lock_fd = -1;
}

/* ── Commit generation ───────────────────────────────────────────── */

int gen_db_commit(gen_db_t *db, gen_pkg_t *packages)
{
        /* Next ID = current + 1 (or 1 if no current) */
        int next_id = gen_db_current(db) + 1;

        /* Write the manifest */
        if (write_manifest(db, next_id, packages) < 0)
                return -1;

        /* Repoint current symlink */
        char link_path[PATH_MAX];
        char target[PATH_MAX];
        snprintf(link_path, sizeof(link_path), "%s/current", db->root);
        snprintf(target, sizeof(target), "%s/generations/%d", db->root, next_id);

        /* Remove old symlink if it exists */
        unlink(link_path);

        /* Create new symlink */
        if (symlink(target, link_path) < 0)
                return -1;

        return next_id;
}

/* ── Rollback ────────────────────────────────────────────────────── */

int gen_db_rollback(gen_db_t *db, int target_id)
{
        /* Verify target generation exists */
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/generations/%d", db->root, target_id);
        if (!dir_exists(dir))
                return -1;

        /* Repoint current symlink */
        char link_path[PATH_MAX];
        char target[PATH_MAX];
        snprintf(link_path, sizeof(link_path), "%s/current", db->root);
        snprintf(target, sizeof(target), "%s/generations/%d", db->root, target_id);

        unlink(link_path);
        if (symlink(target, link_path) < 0)
                return -1;

        return 0;
}

/* ── List generations ────────────────────────────────────────────── */

gen_t **gen_db_list(gen_db_t *db, size_t *count)
{
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/generations", db->root);

        DIR *d = opendir(dir);
        if (!d) {
                *count = 0;
                return NULL;
        }

        /* Count entries first */
        size_t cap = 32;
        gen_t **list = malloc(cap * sizeof(*list));
        size_t n = 0;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;

                int id = atoi(ent->d_name);
                if (id <= 0) continue;

                gen_t *g = calloc(1, sizeof(*g));
                g->id = id;
                snprintf(dir, sizeof(dir), "%s/generations/%d/manifest.json",
                         db->root, id);
                g->manifest_path = strdup(dir);
                g->packages = NULL;  /* lazy-loaded on demand */
                g->pkg_count = 0;

                if (n >= cap) {
                        cap *= 2;
                        list = realloc(list, cap * sizeof(*list));
                }
                list[n++] = g;
        }
        closedir(d);

        /* Sort descending (newest first) */
        for (size_t i = 0; i < n; i++) {
                for (size_t j = i + 1; j < n; j++) {
                        if (list[j]->id > list[i]->id) {
                                gen_t *tmp = list[i];
                                list[i] = list[j];
                                list[j] = tmp;
                        }
                }
        }

        *count = n;
        return list;
}

/* ── Get a single generation ─────────────────────────────────────── */

gen_t *gen_db_get(gen_db_t *db, int id)
{
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/generations/%d", db->root, id);
        if (!dir_exists(path))
                return NULL;

        gen_t *g = calloc(1, sizeof(*g));
        g->id = id;
        snprintf(path, sizeof(path), "%s/generations/%d/manifest.json",
                 db->root, id);
        g->manifest_path = strdup(path);
        g->packages = NULL;
        g->pkg_count = 0;
        return g;
}

/* ── Pin/unpin ───────────────────────────────────────────────────── */

static int set_pinned(gen_db_t *db, int id, int pinned)
{
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/generations/%d/.pinned", db->root, id);

        if (pinned) {
                FILE *f = fopen(path, "w");
                if (!f) return -1;
                fprintf(f, "1\n");
                fclose(f);
                return 0;
        } else {
                return unlink(path) < 0 && errno != ENOENT ? -1 : 0;
        }
}

int gen_db_pin(gen_db_t *db, int id) { return set_pinned(db, id, 1); }
int gen_db_unpin(gen_db_t *db, int id) { return set_pinned(db, id, 0); }

/* ── Free ────────────────────────────────────────────────────────── */

void gen_free(gen_t *g)
{
        if (!g) return;
        free(g->manifest_path);
        gen_pkg_list_free(g->packages);
        free(g);
}

void gen_list_free(gen_t **list, size_t count)
{
        if (!list) return;
        for (size_t i = 0; i < count; i++)
                gen_free(list[i]);
        free(list);
}
