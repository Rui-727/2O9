/* build.h - AUR build pipeline
 *
 * Rewrites paru's install.rs + exec.rs in C.
 * Orchestrates: clone PKGBUILD → review → makepkg → install.
 */

#ifndef TWO9_BUILD_H
#define TWO9_BUILD_H

#include "aur_rpc.h"

/* Build configuration (from 2O9.nix + extra.nix + CLI flags).
 *
 * Phase 1 additions: chroot AUR builds, PGP key import, MFlags pass-through.
 *
 * use_chroot (tri-state, applied by aur_build after consulting two9_config_t):
 *   0  = unset/defer to two9_config_t.use_chroot (default 1 = on, like paru)
 *   1  = force chroot on
 *   -1 = force chroot off (--no-chroot)
 *
 * The CLI (src/cli/main.c) is expected to:
 *   - call two9_config_load() at startup
 *   - apply two9_config.mflags / git_flags / chroot_dir to this struct
 *   - set use_chroot = -1 when --no-chroot is passed
 *   - set use_chroot = 1 when --chroot is passed
 *   - leave use_chroot = 0 (defer) otherwise
 *
 * Until main.c is updated to do this, aur_build() will consult
 * two9_config_t itself as a fallback.
 */
typedef struct build_config {
        char *build_dir;       /* where to clone and build */
        char *makepkg_conf;    /* path to makepkg.conf (or NULL for default) */
        char *pacman_conf;     /* path to pacman.conf (or NULL for default) */
        char *cflags;          /* custom CFLAGS from 2O9.nix */
        char *cxxflags;        /* custom CXXFLAGS from 2O9.nix */
        char *ldflags;         /* custom LDFLAGS from 2O9.nix */
        int no_confirm;        /* skip confirmation prompts */
        int skip_review;       /* skip PKGBUILD review */
        int chroot;            /* legacy field, set by main.c (0 = off) */
        int sign;              /* sign built packages */
        char *gpg_key;         /* GPG key for signing */

        /* Phase 1: chroot, PGP, MFlags pass-through */
        int use_chroot;        /* tri-state, see comment above */
        char *chroot_dir;      /* NULL = /var/lib/2O9/chroot (from two9_config) */
        char **mflags;         /* NULL-terminated extra makepkg flags;
                                * NULL = use default "-feA" */
        char **git_flags;      /* extra git clone flags (NULL-terminated) */
} build_config_t;

/* Build result */
typedef struct build_result {
        char *pkg_name;
        char *pkg_version;
        char *pkg_path;        /* path to .pkg.tar.zst */
        int success;
        char *error_msg;
        struct build_result *next;
} build_result_t;

/* Clone a PKGBUILD from AUR into build_dir.
 * Uses git clone https://aur.archlinux.org/<pkg>.git */
int aur_clone(const char *pkg_name, const char *build_dir);

/* Show PKGBUILD diff for review */
int aur_review(const char *pkg_name, const char *build_dir);

/* Build a package with makepkg.
 * Steps: makepkg --verifysource → makepkg -feA
 * Returns the path to the built .pkg.tar.zst */
build_result_t *aur_build(const char *pkg_name, const char *build_dir,
                          const build_config_t *config);

/* Install a built package via lib209 (store add → symlink farm) */
int aur_install(const char *pkg_path);

/* Free a build result */
void build_result_free(build_result_t *r);

#endif /* TWO9_BUILD_H */
