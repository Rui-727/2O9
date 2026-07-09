/* fstab.h - declarative file system and swap management
 *
 * Reads fileSystems and swapDevices from the manifest JSON (the
 * evaluated 2O9.nix) and generates /etc/fstab on 209 apply. Swap files
 * declared with a size are created with dd + mkswap. The previous
 * /etc/fstab is backed up to /etc/fstab.bak.2O9 (single most-recent
 * backup) before the new file is written. When fstab changes, swapon -a
 * and mount -a run so new entries take effect immediately.
 *
 * Wired into cmd_apply by the parent agent after the activation phase;
 * this module does not modify activation.c, main.c, or the Makefile.
 */

#ifndef TWO9_FSTAB_H
#define TWO9_FSTAB_H

/* Apply file system declarations from the manifest JSON.
 *
 * - Creates swap files declared with a size (regular file paths only;
 *   block devices under /dev/ are left alone).
 * - Generates /etc/fstab from the declared fileSystems (sorted by mount
 *   point depth, shallower first) and swapDevices.
 * - Backs up the existing /etc/fstab to /etc/fstab.bak.2O9 before
 *   overwriting (single most-recent backup).
 * - Runs `swapon -a` and `mount -a` when the fstab content changed.
 * - Prints warnings for file systems present in prev_manifest_json but
 *   absent from manifest_json. Nothing is unmounted or destroyed; the
 *   user handles removal manually.
 *
 * manifest_json:      the evaluated 2O9.nix JSON.
 * prev_manifest_json: the previous generation's manifest, or NULL.
 * dry_run:            if 1, print what would happen but do not execute.
 *
 * Returns 0 on success, non-zero on error. Requires root (getuid()==0);
 * returns 1 with a diagnostic otherwise. */
int fstab_apply(const char *manifest_json, const char *prev_manifest_json,
                int dry_run);

#endif /* TWO9_FSTAB_H */
