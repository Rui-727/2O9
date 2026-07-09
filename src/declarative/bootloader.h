/* bootloader.h - declarative bootloader management (Phase 4)
 *
 * Applies the `boot.loader` block from a 2O9 manifest. Two backends:
 *
 *   - grub: BIOS installs (i386-pc) and UEFI installs (x86_64-efi).
 *     Writes /boot/grub/grub.cfg with one menuentry per 2O9 generation.
 *     Can run grub-mkconfig with the OS prober first, then append the
 *     2O9 entries, or write the whole config from scratch.
 *
 *   - systemd-boot: UEFI only. Runs bootctl install/update, writes
 *     /boot/loader/loader.conf, and one .conf per generation under
 *     /boot/loader/entries/.
 *
 * Each generation's entry points at:
 *   - /boot/vmlinuz-<kernel>   (kernel image, installed by the package)
 *   - /boot/initramfs-<kernel>.img (initrd, regenerated via mkinitcpio -P)
 *   - init=/nix/store/<hash>-hoshizora-<ver>/init (the 2O9 init)
 *   - 2O9_GENERATION=<N>       (so hoshizora knows which generation to activate)
 *
 * The current generation (per <db_root>/current) is the default entry.
 * Older generations stay in the menu so the user can roll back at boot.
 *
 * See docs/PHASE4_BOOTLOADER.md for the config schema and examples.
 */

#ifndef TWO9_BOOTLOADER_H
#define TWO9_BOOTLOADER_H

/* Apply bootloader declarations from the manifest JSON.
 *
 * - Parses boot.loader.grub and boot.loader.systemd-boot. Exactly one
 *   may be enabled; enabling both is an error, enabling neither prints
 *   "no bootloader configured" and returns 0.
 * - For grub: runs grub-install (BIOS or UEFI), generates /boot/grub/grub.cfg
 *   with one menuentry per generation, and regenerates the initrd via
 *   mkinitcpio -P when the declared kernel is installed.
 * - For systemd-boot: runs bootctl install/update, writes /boot/loader/loader.conf,
 *   and writes one entry file per generation under /boot/loader/entries/.
 * - Walks <db_root>/generations/ to enumerate generations. Each
 *   generation's manifest.json supplies the package list (for kernel and
 *   hoshizora detection) and the file mtime supplies the menu date.
 *
 * manifest_json:      the evaluated 2O9.nix JSON.
 * prev_manifest_json: the previous generation's manifest, or NULL. Used
 *                     to warn when the bootloader type changed between
 *                     generations (cleanup of the old bootloader's files
 *                     is manual).
 * db_root:            generation DB root (e.g. /var/lib/2O9 or
 *                     ~/.local/state/2O9). May be NULL; in that case the
 *                     bootloader is installed but no generation entries
 *                     are written.
 * dry_run:            if 1, print what would happen but do not execute.
 *
 * Returns 0 on success, non-zero on error. Requires root (getuid() == 0);
 * returns 1 with a diagnostic otherwise. */
int bootloader_apply(const char *manifest_json,
                     const char *prev_manifest_json,
                     const char *db_root, int dry_run);

#endif /* TWO9_BOOTLOADER_H */
