/* chroot.h - AUR chroot builds via makechrootpkg
 *
 * Rewrites paru's src/chroot.rs in C.
 *
 * Builds AUR packages inside an isolated chroot so PKGBUILDs don't
 * run with the user's full filesystem access. Any PKGBUILD has the
 * same permissions as the build user; without a chroot it can read
 * ~/.ssh, ~/.gnupg, browser cookies, etc.
 *
 * Requires the `devtools` package on Arch (provides mkarchroot,
 * arch-nspawn, makechrootpkg). If missing, calls fail with a clear
 * error telling the user to install devtools.
 *
 * Chroot layout (devtools convention):
 *   <chroot_dir>/root       - base root created by mkarchroot
 *   <chroot_dir>/<user>     - per-user copy-on-write build layer
 *
 * Default chroot_dir = /var/lib/2O9/chroot.
 */
#ifndef TWO9_CHROOT_H
#define TWO9_CHROOT_H

/* Create the chroot root if it doesn't exist. Idempotent.
 *
 * Runs: sudo mkarchroot -C <makepkg.conf> -M <makepkg.conf> \
 *                          <chroot_dir>/root base-devel
 *
 * chroot_dir: NULL or "" -> /var/lib/2O9/chroot
 * makepkg_conf: NULL -> use system makepkg.conf defaults
 *
 * Returns 0 on success (or already exists), -1 on failure. */
int chroot_create(const char *chroot_dir, const char *makepkg_conf);

/* Run a command in the chroot as root (via arch-nspawn).
 * argv is NULL-terminated.
 * Returns 0 on success, -1 on failure. */
int chroot_run_as_root(const char *chroot_dir, char **argv);

/* Build a PKGBUILD inside the chroot via makechrootpkg.
 *
 * Runs: sudo makechrootpkg -r <chroot_dir> -d <clone_dir>:/src \
 *                          -- -feA [--noconfirm] --noprepare --holdver
 *
 * clone_dir: PKGBUILD directory on the host (bind-mounted at /src)
 * makepkg_conf: only used for chroot_create() if the chroot needs
 *               initialization; not passed to makechrootpkg itself.
 * no_confirm: if true, pass --noconfirm to makepkg inside the chroot.
 *
 * On success, returns a strdup'd path to the built .pkg.tar.* (caller
 * frees). out_name and out_version (if non-NULL) are filled from
 * .PKGINFO; caller frees them on success.
 * Returns NULL on failure. */
char *chroot_build(const char *chroot_dir, const char *clone_dir,
                   const char *makepkg_conf, int no_confirm,
                   char **out_name, char **out_version);

#endif /* TWO9_CHROOT_H */
