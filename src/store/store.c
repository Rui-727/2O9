/* store.c — 2O9 store adapter implementation
 *
 * Phase 1: implements both backends (nix-store subprocess and direct
 * libarchive extraction). The nix-store backend is what production uses;
 * the direct backend is for development/testing without nix installed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

extern char **environ;

#include "store.h"

/* ── nix-store --add backend ─────────────────────────────────────── */

static int spawn_nix_store_add(const char *path, char **store_path_out)
{
        int pipefd[2];
        if (pipe(pipefd) < 0) {
                return -1;
        }

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

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                return -1;
        }

        if (n <= 0) {
                return -1;
        }

        /* Trim trailing whitespace */
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
                buf[--n] = '\0';
        }

        *store_path_out = strdup(buf);
        return 0;
}

/* ── Direct extraction backend (libarchive) ──────────────────────── */
/* This will be implemented when libarchive-dev is available.
 * For now, we provide a stub that returns an error. */

static int direct_extract(const char *pkg_path, char **store_path_out)
{
        /* TODO: implement with libarchive
         * 1. Open .pkg.tar.zst with archive_read_open_filename()
         * 2. Read .PKGINFO to get pkgname + pkgver
         * 3. Create /nix/store/<name>-<version>/ (no hash)
         * 4. Extract all files into that directory
         * 5. Return the store path
         */
        (void)pkg_path;
        (void)store_path_out;
        return -1;
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
                                result.error_msg = strdup("nix-store not found — install nix first");
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
                        result.error_msg = strdup("direct extraction not yet implemented "
                                                  "(needs libarchive-dev)");
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

/* ── Manifest ────────────────────────────────────────────────────── */

store_manifest_t *store_manifest_create(const char *store_path,
                                        const char *pkg_name,
                                        const char *pkg_version)
{
        store_manifest_t *m = calloc(1, sizeof(*m));
        if (!m) return NULL;

        m->store_path = strdup(store_path);
        m->pkg_name = strdup(pkg_name);
        m->pkg_version = strdup(pkg_version);

        /* TODO: walk the store directory and build the entry list.
         * For now, create an empty manifest. Phase 1 MVP will populate
         * this by scanning the store path after extraction. */
        m->entries = NULL;
        m->entry_count = 0;

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
