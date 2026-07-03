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
#include <signal.h>
#include <stdint.h>

extern char **environ;

#include "store.h"
#include "nar.h"

/* ── Atomic extraction helpers ─────────────────────────────────────
 * direct_extract() below extracts to /nix/store/.tmp/<name>-<ver>.<pid>/
 * then renames to /nix/store/<name>-<ver>/ so a Ctrl-C mid-extract never
 * leaves a half-populated final store path. The .tmp/ subdir is hidden
 * from cmd_gc (which skips d_name[0] == '.') and from store_manifest_create
 * (which walks a specific store_path, not /nix/store itself). */

static volatile sig_atomic_t extract_got_signal = 0;
static char extract_temp_path[PATH_MAX];

static void extract_sig_handler(int sig)
{
        (void)sig;
        extract_got_signal = 1;
}

/* Recursively remove a directory tree. Best-effort: ignores errors on
 * individual entries so a stuck file doesn't leak the parent dir. */
static int rmtree(const char *path)
{
        DIR *d = opendir(path);
        if (!d) {
                /* Not a dir or doesn't exist - try unlink */
                return unlink(path);
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                        continue;
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                struct stat st;
                if (lstat(child, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                                rmtree(child);
                        } else {
                                unlink(child);
                        }
                }
        }
        closedir(d);
        return rmdir(path);
}

/* Ensure /nix/store/.tmp exists with 0700 perms. Idempotent. */
static void ensure_store_tmp(void)
{
        mkdir("/nix", 0755);
        mkdir("/nix/store", 0755);
        /* 0700: only owner can see/enter temp dirs (each contains a partial
         * store path being extracted, possibly with sensitive files). */
        mkdir("/nix/store/.tmp", 0700);
}

/* ── Helpers ─────────────────────────────────────────────────────── */

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

static int direct_extract(const char *pkg_path, char **store_path_out,
                           const char *known_name, const char *known_ver,
                           char **nar_hash_out, int64_t *nar_size_out)
{
        char pkg_name[256] = {0};
        char pkg_ver[128] = {0};

        *nar_hash_out = NULL;
        *nar_size_out = 0;

        /* If name+version provided, use them directly (skip .PKGINFO) */
        if (known_name && known_ver) {
                strncpy(pkg_name, known_name, sizeof(pkg_name) - 1);
                strncpy(pkg_ver, known_ver, sizeof(pkg_ver) - 1);
        } else {
                /* Read .PKGINFO to get pkgname + pkgver */
                if (read_pkginfo(pkg_path, pkg_name, sizeof(pkg_name),
                                 pkg_ver, sizeof(pkg_ver)) < 0) {
                        return -1;
                }
        }

        /* Phase 2: legacy pre-check on the un-hashed /nix/store/<name>-<ver>/
         * path. If a Phase 0/1 install left this here with a .PKGINFO marker,
         * treat it as an idempotent skip and return that path - the spec
         * says new installs use hashed paths but old ones still work for
         * reads, and re-extracting would just duplicate the data. The
         * returned store_path goes into the generation DB verbatim, so
         * future operations on this generation keep using the legacy path
         * until the user upgrades. */
        char legacy_path[PATH_MAX];
        snprintf(legacy_path, sizeof(legacy_path), "/nix/store/%s-%s",
                 pkg_name, pkg_ver);
        struct stat legacy_st;
        if (stat(legacy_path, &legacy_st) == 0 && S_ISDIR(legacy_st.st_mode)) {
                char marker[PATH_MAX];
                snprintf(marker, sizeof(marker), "%s/.PKGINFO", legacy_path);
                struct stat ms;
                if (stat(marker, &ms) == 0) {
                        /* Legacy complete extraction - idempotent skip.
                         * We do not compute the NAR hash here because we
                         * didn't extract anything; nar_hash_out stays NULL
                         * and the caller skips DB registration. */
                        const char *debug = getenv("TWO09_DEBUG");
                        if (debug)
                                fprintf(stderr, "  [debug] legacy store path "
                                        "exists, skipping: %s\n", legacy_path);
                        *store_path_out = strdup(legacy_path);
                        return 0;
                }
        }

        /* Step 3: Ensure /nix/store/.tmp/ exists, then build temp path.
         * The temp dir is /nix/store/.tmp/<name>-<version>.<pid>/ - the
         * pid suffix avoids collisions between concurrent 209 installs. */
        ensure_store_tmp();
        char temp_path[PATH_MAX];
        snprintf(temp_path, sizeof(temp_path), "/nix/store/.tmp/%s-%s.%d",
                 pkg_name, pkg_ver, (int)getpid());

        /* Remove stale temp dir from a previous crashed run with the
         * same pid (shouldn't happen, but cheap to handle). */
        rmtree(temp_path);

        if (mkdir(temp_path, 0755) < 0 && errno != EEXIST)
                return -1;

        /* Step 4: Install SIGINT/SIGTERM handlers so a Ctrl-C during the
         * system("tar ...") call cleans up the temp dir. The handler just
         * sets a flag (async-signal-safe); cleanup happens after system()
         * returns, then we re-raise the signal so the parent shell sees
         * the right exit code. */
        extract_got_signal = 0;
        strncpy(extract_temp_path, temp_path, sizeof(extract_temp_path) - 1);
        extract_temp_path[sizeof(extract_temp_path) - 1] = '\0';

        struct sigaction sa, old_int, old_term;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = extract_sig_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, &old_int);
        sigaction(SIGTERM, &sa, &old_term);

        /* Step 5: Extract the archive into the temp dir.
         * Use system() so PATH is inherited properly under sudo. Try
         * multiple methods: plain tar, tar with --use-compress-program,
         * then a zstd|tar pipeline. */
        const char *debug = getenv("TWO09_DEBUG");
        char cmd[PATH_MAX * 3];
        int ret = -1;

        snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s'", pkg_path, temp_path);
        if (debug) fprintf(stderr, "  [debug] extracting: %s\n", cmd);
        ret = system(cmd);

        if (ret != 0 && !extract_got_signal) {
                if (debug) fprintf(stderr, "  [debug] tar failed (rc=%d), trying zstd...\n", ret);
                snprintf(cmd, sizeof(cmd),
                         "tar --use-compress-program=zstd -xf '%s' -C '%s'",
                         pkg_path, temp_path);
                ret = system(cmd);
        }

        if (ret != 0 && !extract_got_signal) {
                if (debug) fprintf(stderr, "  [debug] zstd tar failed (rc=%d), trying pipeline...\n", ret);
                snprintf(cmd, sizeof(cmd),
                         "zstd -d -c '%s' | tar xf - -C '%s'",
                         pkg_path, temp_path);
                ret = system(cmd);
        }

        /* Restore signal handlers regardless of outcome. */
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        extract_temp_path[0] = '\0';

        /* If interrupted, clean up the temp dir and re-raise so the
         * process exits with the right signal disposition. */
        if (extract_got_signal) {
                rmtree(temp_path);
                /* Restore default for whichever signal we got and re-raise.
                 * Both old_int/old_term were captured before installation,
                 * so either is the default disposition. Use SIGINT as the
                 * common case. */
                raise(SIGINT);
                return -1;  /* not reached */
        }

        if (ret != 0) {
                if (debug) fprintf(stderr, "  [debug] all extraction methods failed\n");
                rmtree(temp_path);
                return -1;
        }

        /* Step 6 (Phase 2): Compute NAR hash of the extracted tree.
         * The hash is what makes the store path content-addressed. If
         * hashing fails (e.g. FIFO/socket in the tree, scandir failure),
         * fall back to the legacy un-hashed path so we still ship the
         * package - the caller just doesn't get a NAR hash to register. */
        char nar_hash_hex[65];
        size_t nar_size = 0;
        int hash_ok = (nar_hash_directory(temp_path, nar_hash_hex, &nar_size) == 0);
        if (debug && hash_ok)
                fprintf(stderr, "  [debug] nar hash: %s (%zu bytes)\n",
                        nar_hash_hex, nar_size);
        else if (!hash_ok)
                fprintf(stderr, "  [warn] nar_hash_directory failed (%s); "
                        "installing without content address\n",
                        strerror(errno));

        /* Step 7: Compute the final content-addressed store path. */
        char final_path[PATH_MAX];
        int have_hashed = 0;
        if (hash_ok) {
                char *hashed = compute_store_path(nar_hash_hex, pkg_name, pkg_ver);
                if (hashed) {
                        snprintf(final_path, sizeof(final_path), "%s", hashed);
                        free(hashed);
                        have_hashed = 1;
                }
        }
        if (!have_hashed) {
                snprintf(final_path, sizeof(final_path), "/nix/store/%s-%s",
                         pkg_name, pkg_ver);
        }

        /* Step 8: Idempotency on the final path. If a previous run already
         * installed the same content (same hash), drop our temp and return
         * the existing path. Empty/stale dirs are removed; non-empty dirs
         * without .PKGINFO are left alone (could be user-modified). */
        struct stat st;
        if (stat(final_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                char marker[PATH_MAX];
                snprintf(marker, sizeof(marker), "%s/.PKGINFO", final_path);
                struct stat ms;
                if (stat(marker, &ms) == 0) {
                        rmtree(temp_path);
                        *store_path_out = strdup(final_path);
                        if (have_hashed) {
                                *nar_hash_out = strdup(nar_hash_hex);
                                *nar_size_out = (int64_t)nar_size;
                        }
                        return 0;
                }
                DIR *d = opendir(final_path);
                int is_empty = 1;
                if (d) {
                        struct dirent *e;
                        while ((e = readdir(d)) != NULL) {
                                if (strcmp(e->d_name, ".") == 0 ||
                                    strcmp(e->d_name, "..") == 0)
                                        continue;
                                is_empty = 0;
                                break;
                        }
                        closedir(d);
                }
                if (is_empty) {
                        rmdir(final_path);
                } else {
                        if (debug)
                                fprintf(stderr, "  [debug] store path exists "
                                        "without .PKGINFO, leaving as-is: %s\n",
                                        final_path);
                        rmtree(temp_path);
                        *store_path_out = strdup(final_path);
                        return 0;
                }
        }

        /* Step 9: Atomic rename to the final store path. rename() is
         * atomic on the same filesystem, and /nix/store/.tmp is on the
         * same filesystem as /nix/store by design. */
        if (rename(temp_path, final_path) != 0) {
                if (debug) fprintf(stderr, "  [debug] rename %s -> %s failed: %s\n",
                                   temp_path, final_path, strerror(errno));
                rmtree(temp_path);
                return -1;
        }

        *store_path_out = strdup(final_path);
        if (have_hashed) {
                *nar_hash_out = strdup(nar_hash_hex);
                *nar_size_out = (int64_t)nar_size;
        }
        return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

store_add_result_t store_add(const char *pkg_path, store_backend_t backend)
{
        return store_add_named(pkg_path, NULL, NULL, backend);
}

store_add_result_t store_add_named(const char *pkg_path, const char *pkg_name,
                                    const char *pkg_version, store_backend_t backend)
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
                rc = direct_extract(pkg_path, &store_path, pkg_name, pkg_version,
                                    &result.nar_hash, &result.nar_size);
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
        free(r->nar_hash);
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
