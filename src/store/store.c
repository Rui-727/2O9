/* store.c - 2O9 store adapter implementation
 *
 * Phase 1: implements both backends (nix-store subprocess and direct
 * tar extraction). The nix-store backend is what production uses when
 * Nix is installed; the direct backend extracts .pkg.tar.zst into
 * /nix/store/<name>-<version>/ using tar as a subprocess.
 *
 * Direct extraction flow:
 *   1. Read .PKGINFO from the archive to get pkgname + pkgver
 *   2. Create /nix/store/<name>-<version>/
 *   3. Extract the archive contents into that directory
 *   4. Return the store path
 *
 * This matches DESIGN.md §5.1:
 *   "stage_and_register(pkg, files): Extract .pkg.tar.zst to a staging dir
 *    → Move staging dir to /nix/store/<name>-<version>/ - the store path
 *    has no hash, just name and version. Idempotent: if the path already
 *    exists, skip."
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>

extern char **environ;

#include "store.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static int mkdirs(const char *path)
{
        char tmp[PATH_MAX];
        char *p = NULL;
        size_t len;

        snprintf(tmp, sizeof(tmp), "%s", path);
        len = strlen(tmp);
        if (len > 0 && tmp[len - 1] == '/')
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

/* Read .PKGINFO from a .pkg.tar.zst to extract pkgname and pkgver.
 * Uses system("tar ... > tempfile") to avoid PATH issues with posix_spawnp
 * under sudo. */
static int read_pkginfo(const char *pkg_path, char *name_out, size_t name_sz,
                        char *ver_out, size_t ver_sz)
{
        /* Use a temp file to capture .PKGINFO output */
        char tmpfile[] = "/tmp/2O9-pkginfo-XXXXXX";
        int tmpfd = mkstemp(tmpfile);
        if (tmpfd < 0) return -1;

        char cmd[PATH_MAX * 2];
        snprintf(cmd, sizeof(cmd),
                 "tar -xf '%s' --to-stdout .PKGINFO > '%s' 2>/dev/null",
                 pkg_path, tmpfile);
        int ret = system(cmd);

        if (ret != 0) {
                /* Try with ./ prefix */
                snprintf(cmd, sizeof(cmd),
                         "tar -xf '%s' --to-stdout ./.PKGINFO > '%s' 2>/dev/null",
                         pkg_path, tmpfile);
                ret = system(cmd);
        }

        if (ret != 0) {
                close(tmpfd);
                unlink(tmpfile);
                return -1;
        }

        /* Read the temp file */
        lseek(tmpfd, 0, SEEK_SET);
        char buf[8192] = {0};
        ssize_t total = read(tmpfd, buf, sizeof(buf) - 1);
        close(tmpfd);
        unlink(tmpfile);

        if (total <= 0) return -1;

        buf[total] = '\0';

        /* Parse pkgname and pkgver from .PKGINFO */
        name_out[0] = '\0';
        ver_out[0] = '\0';

        char *line = strtok(buf, "\n");
        while (line) {
                /* Skip leading whitespace */
                while (*line && isspace((unsigned char)*line)) line++;

                if (strncmp(line, "pkgname", 7) == 0 && line[7] == '=') {
                        char *val = line + 8;
                        while (*val && isspace((unsigned char)*val)) val++;
                        strncpy(name_out, val, name_sz - 1);
                        name_out[name_sz - 1] = '\0';
                        /* Trim trailing whitespace */
                        size_t l = strlen(name_out);
                        while (l > 0 && isspace((unsigned char)name_out[l-1]))
                                name_out[--l] = '\0';
                } else if (strncmp(line, "pkgver", 6) == 0 && line[6] == '=') {
                        char *val = line + 7;
                        while (*val && isspace((unsigned char)*val)) val++;
                        strncpy(ver_out, val, ver_sz - 1);
                        ver_out[ver_sz - 1] = '\0';
                        size_t l = strlen(ver_out);
                        while (l > 0 && isspace((unsigned char)ver_out[l-1]))
                                ver_out[--l] = '\0';
                }
                line = strtok(NULL, "\n");
        }

        if (name_out[0] == '\0' || ver_out[0] == '\0')
                return -1;

        return 0;
}

/* ── nix-store --add backend ─────────────────────────────────────── */

static int spawn_nix_store_add(const char *path, char **store_path_out)
{
        int pipefd[2];
        if (pipe(pipefd) < 0)
                return -1;

        pid_t pid;
        char *argv[] = { "nix-store", "--add", (char *)path, NULL };

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        int ret = posix_spawnp(&pid, "nix-store", &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);

        if (ret != 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                return -1;
        }

        close(pipefd[1]);

        char buf[4096] = {0};
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        close(pipefd[0]);

        int status;
        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                return -1;

        if (n <= 0)
                return -1;

        /* Trim trailing whitespace */
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
                buf[--n] = '\0';

        *store_path_out = strdup(buf);
        return 0;
}

/* ── Direct extraction backend (tar subprocess) ──────────────────── */

static int direct_extract(const char *pkg_path, char **store_path_out)
{
        /* Step 1: Read .PKGINFO to get pkgname + pkgver */
        char pkg_name[256] = {0};
        char pkg_ver[128] = {0};

        if (read_pkginfo(pkg_path, pkg_name, sizeof(pkg_name),
                         pkg_ver, sizeof(pkg_ver)) < 0) {
                return -1;
        }

        /* Step 2: Build the store path: /nix/store/<name>-<version> */
        char store_path[PATH_MAX];
        snprintf(store_path, sizeof(store_path), "/nix/store/%s-%s",
                 pkg_name, pkg_ver);

        /* Idempotent: if path already exists and is a directory, skip extraction */
        struct stat st;
        if (stat(store_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                *store_path_out = strdup(store_path);
                return 0;
        }

        /* Step 3: Create the store directory */
        if (mkdirs(store_path) < 0)
                return -1;

        /* Step 4: Extract the archive into the store directory.
         * Arch .pkg.tar.zst files are zstd-compressed tarballs.
         * Use system() so PATH is inherited properly (posix_spawnp
         * doesn't always find tar when running under sudo). */
        char cmd[PATH_MAX * 3];
        snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s'", pkg_path, store_path);
        int ret = system(cmd);

        if (ret != 0) {
                /* tar failed - try with explicit zstd */
                snprintf(cmd, sizeof(cmd),
                         "tar --use-compress-program=zstd -xf '%s' -C '%s'",
                         pkg_path, store_path);
                ret = system(cmd);
        }

        if (ret != 0) {
                /* Last resort: zstd pipeline */
                snprintf(cmd, sizeof(cmd),
                         "zstd -d -c '%s' | tar xf - -C '%s'",
                         pkg_path, store_path);
                ret = system(cmd);
        }

        if (ret != 0) {
                /* All methods failed - clean up */
                rmdir(store_path);
                return -1;
        }

        *store_path_out = strdup(store_path);
        return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

store_add_result_t store_add(const char *pkg_path, store_backend_t backend)
{
        store_add_result_t result = {0};
        char *store_path = NULL;
        int rc;

        switch (backend) {
        case STORE_BACKEND_NIX_STORE:
                rc = spawn_nix_store_add(pkg_path, &store_path);
                if (rc < 0) {
                        result.success = -1;
                        if (access("/usr/bin/nix-store", X_OK) != 0 &&
                            access("/nix/var/nix/profiles/default/bin/nix-store", X_OK) != 0) {
                                result.error_msg = strdup("nix-store not found - install nix first");
                        } else {
                                result.error_msg = strdup("nix-store --add failed");
                        }
                        return result;
                }
                break;
        case STORE_BACKEND_DIRECT:
                rc = direct_extract(pkg_path, &store_path);
                if (rc < 0) {
                        result.success = -1;
                        result.error_msg = strdup("extraction failed - "
                                                  "ensure tar and zstd are installed");
                        return result;
                }
                break;
        default:
                result.success = -1;
                result.error_msg = strdup("unknown store backend");
                return result;
        }

        result.success = 0;
        result.store_path = store_path;
        return result;
}

void store_add_result_free(store_add_result_t *r)
{
        if (!r) return;
        free(r->store_path);
        free(r->error_msg);
}

/* ── Manifest: walk store directory and build entry list ─────────── */

static store_entry_t *entry_new(const char *path, int is_dir,
                                const char *symlink_target, int is_config)
{
        store_entry_t *e = calloc(1, sizeof(*e));
        if (!e) return NULL;
        e->path = strdup(path);
        e->is_dir = is_dir;
        e->is_config = is_config;
        e->symlink = symlink_target ? strdup(symlink_target) : NULL;
        return e;
}

/* Recursively walk a directory, building the entry list.
 * prefix is the relative path so far (e.g. "bin" or "etc/ssh"). */
static int walk_dir(const char *base, const char *prefix,
                    store_entry_t **tail, size_t *count)
{
        char full_path[PATH_MAX];
        if (prefix[0])
                snprintf(full_path, sizeof(full_path), "%s/%s", base, prefix);
        else
                snprintf(full_path, sizeof(full_path), "%s", base);

        DIR *d = opendir(full_path);
        if (!d) return -1;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                        continue;

                char rel_path[PATH_MAX];
                if (prefix[0])
                        snprintf(rel_path, sizeof(rel_path), "%s/%s", prefix, ent->d_name);
                else
                        snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);

                char abs_path[PATH_MAX];
                snprintf(abs_path, sizeof(abs_path), "%s/%s", base, rel_path);

                struct stat st;
                if (lstat(abs_path, &st) < 0)
                        continue;

                /* Classify: files under etc/ are config files */
                int is_config = (strncmp(rel_path, "etc/", 4) == 0 ||
                                 strncmp(rel_path, "usr/share/", 10) == 0);

                if (S_ISDIR(st.st_mode)) {
                        /* Add directory entry */
                        store_entry_t *e = entry_new(rel_path, 1, NULL, is_config);
                        if (e) {
                                *tail = e;
                                tail = &e->next;
                                (*count)++;
                        }
                        /* Recurse */
                        walk_dir(base, rel_path, tail, count);
                } else if (S_ISLNK(st.st_mode)) {
                        /* Read symlink target */
                        char target[PATH_MAX];
                        ssize_t n = readlink(abs_path, target, sizeof(target) - 1);
                        if (n > 0) {
                                target[n] = '\0';
                                store_entry_t *e = entry_new(rel_path, 0, target, is_config);
                                if (e) {
                                        *tail = e;
                                        tail = &e->next;
                                        (*count)++;
                                }
                        }
                } else if (S_ISREG(st.st_mode)) {
                        /* Skip .PKGINFO, .INSTALL, .MTREE, .BUILDINFO - package metadata */
                        if (strcmp(ent->d_name, ".PKGINFO") == 0 ||
                            strcmp(ent->d_name, ".INSTALL") == 0 ||
                            strcmp(ent->d_name, ".MTREE") == 0 ||
                            strcmp(ent->d_name, ".BUILDINFO") == 0)
                                continue;

                        store_entry_t *e = entry_new(rel_path, 0, NULL, is_config);
                        if (e) {
                                *tail = e;
                                tail = &e->next;
                                (*count)++;
                        }
                }
        }

        closedir(d);
        return 0;
}

store_manifest_t *store_manifest_create(const char *store_path,
                                        const char *pkg_name,
                                        const char *pkg_version)
{
        store_manifest_t *m = calloc(1, sizeof(*m));
        if (!m) return NULL;

        m->store_path = strdup(store_path);
        m->pkg_name = strdup(pkg_name);
        m->pkg_version = strdup(pkg_version);

        /* Walk the store directory and build the entry list */
        store_entry_t *tail = NULL;
        walk_dir(store_path, "", &tail, &m->entry_count);
        m->entries = tail;

        return m;
}

void store_entry_list_free(store_entry_t *e)
{
        while (e) {
                store_entry_t *next = e->next;
                free(e->path);
                free(e->symlink);
                free(e);
                e = next;
        }
}

void store_manifest_free(store_manifest_t *m)
{
        if (!m) return;
        free(m->store_path);
        free(m->pkg_name);
        free(m->pkg_version);
        store_entry_list_free(m->entries);
        free(m);
}
