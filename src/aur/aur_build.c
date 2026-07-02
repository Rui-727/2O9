/* aur_build.c — AUR build pipeline implementation
 *
 * Implements: clone PKGBUILD → review → makepkg → install to store.
 * Uses git subprocess for clone/fetch (same approach as paru).
 * Uses fork/exec for makepkg with CFLAGS injection.
 *
 * Part of Phase 2: paru → C port.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>

extern char **environ;

#include "build.h"
#include "aur_rpc.h"

#define AUR_GIT_URL "https://aur.archlinux.org"

/* ── Helpers ──────────────────────────────────────────────────────── */

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

/* Run a command and capture its exit code. argv is NULL-terminated.
 * If stdout_path is non-NULL, redirect stdout there.
 * Returns 0 on success, -1 on failure. */
static int run_cmd(char **argv, const char *stdout_path)
{
        pid_t pid;

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);

        if (stdout_path) {
                int fd = open(stdout_path,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                        posix_spawn_file_actions_destroy(&actions);
                        return -1;
                }
                posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
                posix_spawn_file_actions_addclose(&actions, fd);
        }

        int ret = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);

        if (ret != 0) return -1;

        int status;
        waitpid(pid, &status, 0);

        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Run a command and pipe its stdout to the caller (for review).
 * Currently unused — will be needed for interactive review. */
static int run_cmd_pipe(char **argv, FILE **out_fp)
{
        int pipefd[2];
        if (pipe(pipefd) < 0) return -1;

        pid_t pid;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        int ret = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);

        if (ret != 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                return -1;
        }

        close(pipefd[1]);
        *out_fp = fdopen(pipefd[0], "r");
        if (!*out_fp) {
                close(pipefd[0]);
                return -1;
        }

        return 0;
}

/* ── Clone ────────────────────────────────────────────────────────── */

int aur_clone(const char *pkg_name, const char *build_dir)
{
        if (!pkg_name || !build_dir) return -1;

        char clone_dir[PATH_MAX];
        snprintf(clone_dir, sizeof(clone_dir), "%s/%s", build_dir, pkg_name);

        /* If already cloned, skip */
        struct stat st;
        if (stat(clone_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                return 0;  /* already exists */
        }

        /* Ensure build_dir exists */
        if (mkdirs(build_dir) < 0)
                return -1;

        /* git clone https://aur.archlinux.org/<pkg>.git <build_dir>/<pkg> */
        char url[PATH_MAX];
        snprintf(url, sizeof(url), "%s/%s.git", AUR_GIT_URL, pkg_name);

        char *argv[] = { "git", "clone", url, clone_dir, NULL };

        fprintf(stderr, "  cloning %s...\n", url);
        int rc = run_cmd(argv, NULL);
        if (rc < 0) {
                fprintf(stderr, "  git clone failed for %s\n", pkg_name);
                return -1;
        }

        return 0;
}

/* ── Review ───────────────────────────────────────────────────────── */

int aur_review(const char *pkg_name, const char *build_dir)
{
        if (!pkg_name || !build_dir) return -1;

        char clone_dir[PATH_MAX];
        snprintf(clone_dir, sizeof(clone_dir), "%s/%s", build_dir, pkg_name);

        /* Show PKGBUILD contents */
        char pkgbuild[PATH_MAX];
        snprintf(pkgbuild, sizeof(pkgbuild), "%s/PKGBUILD", clone_dir);

        struct stat st;
        if (stat(pkgbuild, &st) != 0) {
                fprintf(stderr, "  PKGBUILD not found at %s\n", pkgbuild);
                return -1;
        }

        /* If stdout is a TTY, use bat or less for review; otherwise cat */
        char *pager = getenv("PAGER");
        if (pager && *pager) {
                char *argv[] = { pager, pkgbuild, NULL };
                return run_cmd(argv, NULL);
        }

        /* Fallback: just print PKGBUILD to stdout */
        FILE *f = fopen(pkgbuild, "r");
        if (!f) return -1;

        char line[4096];
        printf("─── PKGBUILD: %s ───\n", pkg_name);
        while (fgets(line, sizeof(line), f))
                fputs(line, stdout);
        printf("─── end PKGBUILD ───\n");

        fclose(f);
        return 0;
}

/* ── Build ────────────────────────────────────────────────────────── */

/* Parse .PKGINFO from a built package to extract name and version.
 * Returns 0 on success, -1 on failure. */
static int parse_pkginfo(const char *pkg_path,
                         char **name_out, char **version_out)
{
        /* .pkg.tar.zst is an archive — we need to extract .PKGINFO.
         * Use bsdtar (from libarchive) to extract just that file. */
        char tmpdir[PATH_MAX];
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/2O9-pkginfo-%d", getpid());
        mkdirs(tmpdir);

        char *argv[] = {
                "bsdtar", "-xf", (char *)pkg_path,
                "-C", tmpdir, ".PKGINFO", NULL
        };

        if (run_cmd(argv, NULL) < 0) {
                /* Clean up */
                char rm_cmd[PATH_MAX + 32];
                snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
                system(rm_cmd);
                return -1;
        }

        char info_path[PATH_MAX];
        snprintf(info_path, sizeof(info_path), "%s/.PKGINFO", tmpdir);

        FILE *f = fopen(info_path, "r");
        if (!f) {
                char rm_cmd[PATH_MAX + 32];
                snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
                system(rm_cmd);
                return -1;
        }

        char line[4096];
        *name_out = NULL;
        *version_out = NULL;

        while (fgets(line, sizeof(line), f)) {
                /* Trim newline */
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';

                if (strncmp(line, "pkgname = ", 10) == 0) {
                        *name_out = strdup(line + 10);
                } else if (strncmp(line, "pkgver = ", 9) == 0) {
                        *version_out = strdup(line + 9);
                }
        }
        fclose(f);

        /* Clean up tmp */
        char rm_cmd[PATH_MAX + 32];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
        system(rm_cmd);

        return (*name_out && *version_out) ? 0 : -1;
}

/* Find the .pkg.tar.* file in a directory after makepkg completes. */
static char *find_built_pkg(const char *dir)
{
        /* makepkg outputs to the directory it's run in.
         * Pattern: <name>-<ver>-<rel>-<arch>.pkg.tar.zst */
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof(cmd),
                 "find '%s' -maxdepth 1 -name '*.pkg.tar.zst' -o -name '*.pkg.tar.xz' -o -name '*.pkg.tar.gz' 2>/dev/null | head -1",
                 dir);

        FILE *p = popen(cmd, "r");
        if (!p) return NULL;

        char result[PATH_MAX];
        if (!fgets(result, sizeof(result), p)) {
                pclose(p);
                return NULL;
        }
        pclose(p);

        /* Trim newline */
        char *nl = strchr(result, '\n');
        if (nl) *nl = '\0';

        /* Verify the file exists */
        if (access(result, R_OK) != 0)
                return NULL;

        return strdup(result);
}

build_result_t *aur_build(const char *pkg_name, const char *build_dir,
                          const build_config_t *config)
{
        if (!pkg_name || !build_dir)
                return NULL;

        char clone_dir[PATH_MAX];
        snprintf(clone_dir, sizeof(clone_dir), "%s/%s", build_dir, pkg_name);

        struct stat st;
        if (stat(clone_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                build_result_t *r = calloc(1, sizeof(*r));
                r->pkg_name = strdup(pkg_name);
                r->success = 0;
                r->error_msg = strdup("PKGBUILD not cloned — call aur_clone first");
                return r;
        }

        /* Build the makepkg command */
        int argc_max = 32;
        char **argv = calloc(argc_max, sizeof(char *));
        int i = 0;

        argv[i++] = "makepkg";

        /* Flags:
         *   -f  — force rebuild (overwrite existing package)
         *   -e  — extract source files (don't use cached)
         *   -A  — ignore arch check
         *   -s  — install missing deps with pacman (for repo deps)
         *   --noconfirm — skip prompts
         */
        argv[i++] = "-feA";

        if (config && config->no_confirm)
                argv[i++] = "--noconfirm";

        if (config && config->makepkg_conf) {
                argv[i++] = "--config";
                argv[i++] = (char *)config->makepkg_conf;
        }

        argv[i] = NULL;

        /* Build the environment with CFLAGS injection */
        char env_cflags[4096] = {0};
        char env_cxxflags[4096] = {0};
        char env_ldflags[4096] = {0};
        char env_makeflags[256] = {0};

        if (config) {
                if (config->cflags) {
                        snprintf(env_cflags, sizeof(env_cflags),
                                 "CFLAGS=%s", config->cflags);
                }
                if (config->cxxflags) {
                        snprintf(env_cxxflags, sizeof(env_cxxflags),
                                 "CXXFLAGS=%s", config->cxxflags);
                }
                if (config->ldflags) {
                        snprintf(env_ldflags, sizeof(env_ldflags),
                                 "LDFLAGS=%s", config->ldflags);
                }
        }

        /* Set MAKEFLAGS for parallel builds */
        const char *existing_makeflags = getenv("MAKEFLAGS");
        if (existing_makeflags) {
                snprintf(env_makeflags, sizeof(env_makeflags),
                         "MAKEFLAGS=%s", existing_makeflags);
        }

        /* Change to clone dir and run makepkg */
        fprintf(stderr, "  building %s...\n", pkg_name);

        pid_t pid = fork();
        if (pid < 0) {
                free(argv);
                build_result_t *r = calloc(1, sizeof(*r));
                r->pkg_name = strdup(pkg_name);
                r->success = 0;
                r->error_msg = strdup("fork failed");
                return r;
        }

        if (pid == 0) {
                /* Child */
                chdir(clone_dir);

                /* Set environment for build optimization */
                if (env_cflags[0])    putenv(env_cflags);
                if (env_cxxflags[0])  putenv(env_cxxflags);
                if (env_ldflags[0])   putenv(env_ldflags);
                if (env_makeflags[0]) putenv(env_makeflags);

                execvp(argv[0], argv);
                _exit(127);
        }

        free(argv);

        /* Parent: wait for makepkg */
        int status;
        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                build_result_t *r = calloc(1, sizeof(*r));
                r->pkg_name = strdup(pkg_name);
                r->success = 0;
                r->error_msg = strdup("makepkg failed");
                return r;
        }

        /* Find the built package */
        char *pkg_path = find_built_pkg(clone_dir);
        if (!pkg_path) {
                build_result_t *r = calloc(1, sizeof(*r));
                r->pkg_name = strdup(pkg_name);
                r->success = 0;
                r->error_msg = strdup("built package not found after makepkg");
                return r;
        }

        /* Extract name and version from the built package */
        char *real_name = NULL, *real_version = NULL;
        parse_pkginfo(pkg_path, &real_name, &real_version);

        build_result_t *r = calloc(1, sizeof(*r));
        r->pkg_name = real_name ? real_name : strdup(pkg_name);
        r->pkg_version = real_version ? real_version : strdup("unknown");
        r->pkg_path = pkg_path;
        r->success = 1;
        r->error_msg = NULL;

        fprintf(stderr, "  built: %s (%s) → %s\n",
                r->pkg_name, r->pkg_version, r->pkg_path);

        return r;
}

/* ── Install to store ─────────────────────────────────────────────── */

int aur_install(const char *pkg_path)
{
        /* This hands the .pkg.tar.zst to the store adapter.
         * The store adapter adds it to /nix/store and returns the store path.
         * Then the CLI commits a new generation and rebuilds the symlink farm.
         *
         * For now, this is a thin wrapper — the CLI does the generation
         * commit.  This function just validates and returns 0. */
        if (!pkg_path) return -1;

        if (access(pkg_path, R_OK) != 0) {
                fprintf(stderr, "  aur_install: package not found: %s\n", pkg_path);
                return -1;
        }

        return 0;
}

/* ── Free ─────────────────────────────────────────────────────────── */

void build_result_free(build_result_t *r)
{
        if (!r) return;
        free(r->pkg_name);
        free(r->pkg_version);
        free(r->pkg_path);
        free(r->error_msg);
        build_result_free(r->next);
        free(r);
}
