/* users.h - declarative user and group management (Phase 4)
 *
 * Applies the `users` and `groups` attrsets from a 2O9 manifest to the
 * live system. Groups first, then users, then passwords, then
 * supplementary group memberships. Idempotent: every `209 apply`
 * reconciles the declared set against what is on disk.
 *
 * See docs/PHASE4_USERS.md for the config schema and examples.
 */

#ifndef TWO9_USERS_H
#define TWO9_USERS_H

/* Apply user and group declarations from the manifest JSON.
 *
 * Creates groups first, then users. Idempotent: updates existing
 * users/groups to match the declaration. Removes users/groups that
 * existed in the previous generation's manifest but not the current
 * one (with a confirmation prompt).
 *
 * manifest_json:      the evaluated 2O9.nix JSON (from nix_eval_file).
 * prev_manifest_json: the previous generation's manifest JSON, or NULL.
 * dry_run:            if 1, print what would happen but do not execute.
 *
 * Returns 0 on success, non-zero on error. Errors during individual
 * user/group operations are logged but non-fatal; the function still
 * returns non-zero if any occurred.
 *
 * Requires root (getuid() == 0). Returns non-zero with a clear message
 * if invoked as a non-root user.
 */
int users_apply(const char *manifest_json, const char *prev_manifest_json, int dry_run);

#endif /* TWO9_USERS_H */
