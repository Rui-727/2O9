/* pam_nss.h - declarative PAM and NSS configuration (Phase 4)
 *
 * Applies the `security.pam` and `security.nss` blocks from a 2O9 manifest.
 * PAM service configs land in /etc/pam.d/<service>; NSS sources land in
 * /etc/nsswitch.conf. Existing files are backed up to <path>.bak.2O9 (single
 * most-recent backup) before being overwritten.
 *
 * PAM services can be declared either as raw config text or as a structured
 * attrset that 2O9 renders. NSS databases are lists of sources; a list
 * element can be a string (a source name) or a sublist whose first element
 * is the source name and the rest are action specifiers.
 *
 * See docs/PHASE4_PAM_NSS.md for the config schema and examples.
 */

#ifndef TWO9_PAM_NSS_H
#define TWO9_PAM_NSS_H

/* Apply PAM and NSS declarations from the manifest JSON.
 *
 * - Parses security.pam. For each service, writes /etc/pam.d/<service>
 *   either verbatim (string form) or rendered (structured form).
 * - Parses security.nss. Writes /etc/nsswitch.conf with one line per
 *   declared database.
 * - Backs up each existing file to <path>.bak.2O9 before overwriting
 *   (single most-recent backup).
 * - If prev_manifest_json is non-NULL, warns about PAM services that
 *   were declared previously but are gone from the current manifest.
 *   Files for removed services are NOT deleted; the user handles that
 *   manually.
 *
 * manifest_json:      the evaluated 2O9.nix JSON.
 * prev_manifest_json: the previous generation's manifest, or NULL.
 * dry_run:            if 1, print what would happen but do not execute.
 *
 * Returns 0 on success, non-zero on error. Requires root (getuid() == 0);
 * returns 1 with a diagnostic otherwise. If security.pam is missing or
 * empty, the PAM step is skipped. If security.nss is missing or empty,
 * the NSS step is skipped. */
int pam_nss_apply(const char *manifest_json, const char *prev_manifest_json,
                  int dry_run);

#endif /* TWO9_PAM_NSS_H */
