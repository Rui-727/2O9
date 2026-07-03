/* chroot.c - chroot AUR builds via makechrootpkg
 *
 * Implements: mkarchroot -> arch-nspawn -> makechrootpkg.
 * All three require the `devtools` package on Arch.
 *
 * The chroot layout matches devtools' convention:
 *   <chroot_dir>/root       - the base root (created by mkarchroot)
 *   <chroot_dir>/<user>     - per-user copy-on-write layer
 *
 * Built packages land in <chroot_dir>/<user>/src/dest/ after makechrootpkg,
 * but makechrootpkg also copies them back to the host clone_dir by default.
 * We check both locations.
 *
 * Modeled on paru's src/chroot.rs.
 */

/* _GNU_SOURCE is provided by the Makefile (-D_GNU_SOURCE). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <spawn.h>
#include <fcntl.h>
#include <errno.h>

extern char **environ;

#include "chroot.h"

#define DEFAULT_CHROOT_DIR "/var/lib/2O9/chroot"

/* ── Helpers ──────────────────────────────────────────────────────── */

static const char *effective_chroot_dir(const char *chroot_dir)
{
        return (chroot_dir && *chroot_dir) ? chroot_dir : DEFAULT_CHROOT_DIR;
}

/* Run a command via posix_spawnp. Returns 0 on success, -1 on failure. */
static int run_argv(char **argv)
{
        pid_t pid;
        int ret = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
        if (ret != 0) return -1;

        int status;
        waitpid(pid, &status, 0);

        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Check if a binary exists and is executable on $PATH. */
static int bin_available(const char *name)
{
        char *path = getenv("PATH");
        if (!path) return 0;

        char *path_copy = strdup(path);
        if (!path_copy) return 0;

        char *save = NULL;
        char *tok = strtok_r(path_copy, ":", &save);
        while (tok) {
                char full[PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s", tok, name);
                if (access(full, X_OK) == 0) {
                        free(path_copy);
                        return 1;
                }
                tok = strtok_r(NULL, ":", &save);
        }
        free(path_copy);
        return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

int chroot_create(const char *chroot_dir, const char *makepkg_conf)
{
        if (!bin_available("mkarchroot")) {
                fprintf(stderr,
                        "  error: mkarchroot not found - install the 'devtools' package\n");
                return -1;
        }

        const char *dir = effective_chroot_dir(chroot_dir);
        char root_dir[PATH_MAX];
        snprintf(root_dir, sizeof(root_dir), "%s/root", dir);

        struct stat st;
        if (stat(root_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                /* Idempotent: chroot already exists */
                return 0;
        }

        fprintf(stderr, "  creating chroot at %s...\n", dir);

        /* sudo mkarchroot [-C <makepkg.conf>] [-M <makepkg.conf>] \
         *                  <root_dir> base-devel */
        int argc_max = 16;
        char **argv = calloc(argc_max, sizeof(char *));
        int i = 0;
        argv[i++] = "sudo";
        argv[i++] = "mkarchroot";
        if (makepkg_conf) {
                argv[i++] = "-C";
                argv[i++] = (char *)makepkg_conf;
                argv[i++] = "-M";
                argv[i++] = (char *)makepkg_conf;
        }
        argv[i++] = root_dir;
        argv[i++] = "base-devel";
        argv[i++] = NULL;

        int rc = run_argv(argv);
        free(argv);

        if (rc < 0) {
                fprintf(stderr, "  error: mkarchroot failed\n");
                return -1;
        }

        return 0;
}

int chroot_run_as_root(const char *chroot_dir, char **argv)
{
        if (!bin_available("arch-nspawn")) {
                fprintf(stderr,
                        "  error: arch-nspawn not found - install the 'devtools' package\n");
                return -1;
        }

        if (!argv || !argv[0]) return -1;

        const char *dir = effective_chroot_dir(chroot_dir);
        char root_dir[PATH_MAX];
        snprintf(root_dir, sizeof(root_dir), "%s/root", dir);

        /* Count user argv */
        int user_argc = 0;
        while (argv[user_argc]) user_argc++;

        /* sudo arch-nspawn <root_dir> <argv...> */
        char **full = calloc(user_argc + 4, sizeof(char *));
        int i = 0;
        full[i++] = "sudo";
        full[i++] = "arch-nspawn";
        full[i++] = root_dir;
        for (int j = 0; j < user_argc; j++)
                full[i++] = argv[j];
        full[i] = NULL;

        int rc = run_argv(full);
        free(full);
        return rc;
}

/* Find the .pkg.tar.* file produced by makechrootpkg.
 * Checks clone_dir first (makechrootpkg copies there by default),
 * then falls back to <chroot_dir>/<user>/src/dest/.
 * Returns a strdup'd path or NULL. */
static char *find_built_pkg_in(const char *dir)
{
        char cmd[PATH_MAX * 2];
        snprintf(cmd, sizeof(cmd),
                 "find '%s' -maxdepth 1 \\( -name '*.pkg.tar.zst' "
                 "-o -name '*.pkg.tar.xz' -o -name '*.pkg.tar.gz' \\) "
                 "2>/dev/null | head -1",
                 dir);

        FILE *p = popen(cmd, "r");
        if (!p) return NULL;

        char result[PATH_MAX];
        if (!fgets(result, sizeof(result), p)) {
                pclose(p);
                return NULL;
        }
        pclose(p);

        char *nl = strchr(result, '\n');
        if (nl) *nl = '\0';

        if (access(result, R_OK) != 0)
                return NULL;

        return strdup(result);
}

/* Parse .PKGINFO out of a built package to extract pkgname and pkgver.
 * Returns 0 on success, -1 on failure. Either out param may be NULL. */
static int parse_pkginfo_from_pkg(const char *pkg_path,
                                  char **out_name, char **out_version)
{
        if (out_name)     *out_name = NULL;
        if (out_version)  *out_version = NULL;

        char tmpdir[PATH_MAX];
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/2O9-chroot-pkginfo-%d", getpid());
        if (mkdir(tmpdir, 0755) < 0 && errno != EEXIST) return -1;

        char *ex_argv[] = {
                "bsdtar", "-xf", (char *)pkg_path,
                "-C", tmpdir, ".PKGINFO", NULL
        };
        if (run_argv(ex_argv) < 0) {
                char rm[PATH_MAX + 32];
                snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
                system(rm);
                return -1;
        }

        char info_path[PATH_MAX];
        snprintf(info_path, sizeof(info_path), "%s/.PKGINFO", tmpdir);

        FILE *f = fopen(info_path, "r");
        if (!f) {
                char rm[PATH_MAX + 32];
                snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
                system(rm);
                return -1;
        }

        char line[4096];
        while (fgets(line, sizeof(line), f)) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';

                if (strncmp(line, "pkgname = ", 10) == 0 && out_name)
                        *out_name = strdup(line + 10);
                else if (strncmp(line, "pkgver = ", 9) == 0 && out_version)
                        *out_version = strdup(line + 9);
        }
        fclose(f);

        char rm[PATH_MAX + 32];
        snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
        system(rm);

        return 0;
}

char *chroot_build(const char *chroot_dir, const char *clone_dir,
                   const char *makepkg_conf, int no_confirm,
                   char **out_name, char **out_version)
{
        if (out_name)     *out_name = NULL;
        if (out_version)  *out_version = NULL;

        if (!bin_available("makechrootpkg")) {
                fprintf(stderr,
                        "  error: makechrootpkg not found - install the 'devtools' package\n");
                return NULL;
        }
        if (!clone_dir) return NULL;

        const char *dir = effective_chroot_dir(chroot_dir);

        /* Ensure the chroot root exists (idempotent). */
        if (chroot_create(dir, makepkg_conf) < 0)
                return NULL;

        /* sudo makechrootpkg -r <chroot_dir> -d <clone_dir>:/src \
         *      -- -feA [--noconfirm] --noprepare --holdver */
        char bind_arg[PATH_MAX * 2];
        snprintf(bind_arg, sizeof(bind_arg), "%s:/src", clone_dir);

        int argc_max = 32;
        char **argv = calloc(argc_max, sizeof(char *));
        int i = 0;
        argv[i++] = "sudo";
        argv[i++] = "makechrootpkg";
        argv[i++] = "-r";
        argv[i++] = (char *)dir;
        argv[i++] = "-d";
        argv[i++] = bind_arg;
        argv[i++] = "--";
        argv[i++] = "-feA";
        argv[i++] = "--noprepare";
        argv[i++] = "--holdver";
        if (no_confirm)
                argv[i++] = "--noconfirm";
        argv[i] = NULL;

        fprintf(stderr, "  building in chroot: %s\n", clone_dir);
        int rc = run_argv(argv);
        free(argv);

        if (rc < 0) {
                fprintf(stderr, "  error: makechrootpkg failed\n");
                return NULL;
        }

        /* Find the built package. makechrootpkg copies the result back
         * to the host clone_dir by default; fall back to the chroot's
         * dest dir if not found there. */
        char *pkg_path = find_built_pkg_in(clone_dir);
        if (!pkg_path) {
                const char *user = getenv("SUDO_USER");
                if (!user) user = getenv("USER");
                if (!user) user = "nobody";

                char dest_dir[PATH_MAX];
                snprintf(dest_dir, sizeof(dest_dir), "%s/%s/src/dest", dir, user);
                pkg_path = find_built_pkg_in(dest_dir);
        }
        if (!pkg_path) {
                fprintf(stderr,
                        "  error: built package not found after makechrootpkg\n");
                return NULL;
        }

        /* Extract name and version from .PKGINFO */
        if (out_name || out_version)
                parse_pkginfo_from_pkg(pkg_path, out_name, out_version);

        return pkg_path;
}
