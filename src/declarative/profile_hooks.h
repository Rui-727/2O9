/* profile_hooks.h - 2O9 profile hooks as per-generation store paths (Phase 4)
 *
 * Replaces the imperative cache-rebuild step in the activation phase
 * (gtk-update-icon-cache, update-desktop-database, fc-cache, ldconfig)
 * with per-generation store paths. Each enabled hook runs in a temp
 * directory, 2O9 NAR-hashes the output, moves it to
 * /nix/store/<hash>-hook-<name>/, and records the path in the
 * generation's hooks.json. Rolling back the generation drops the hook
 * output. Unreferenced hook outputs are collected by the next `209 gc`.
 *
 * Wired into cmd_apply by the parent after the activation phase. This
 * module does not modify activation.c, main.c, or the Makefile.
 *
 * See docs/PHASE4_PROFILE_HOOKS.md for the config schema and examples.
 */

#ifndef TWO9_PROFILE_HOOKS_H
#define TWO9_PROFILE_HOOKS_H

/* Apply profile hooks from the manifest JSON.
 *
 * - Runs each enabled hook in a temp directory under /nix/store/.tmp/.
 * - NAR-hashes the output (nar_hash_directory from src/store/nar.c).
 * - Moves the output to /nix/store/<base32-hash>-hook-<name>/ via
 *   rename() (atomic on the same filesystem).
 * - Records hook outputs in <db_root>/generations/<gen_id>/hooks.json
 *   so rollback restores the old hook outputs.
 *
 * manifest_json: the evaluated 2O9.nix JSON.
 * db_root:       the generation DB root (e.g. /var/lib/2O9). May be
 *                NULL; in that case hooks run but nothing is recorded.
 * gen_id:        the current generation ID (for recording hook outputs).
 * dry_run:       if 1, print what would happen but do not execute.
 *
 * Returns 0 on success, non-zero on error. Individual hook failures are
 * logged and non-fatal: a hook that exits non-zero, produces empty
 * output, or whose binary is missing is skipped and the next hook runs.
 * The function returns non-zero only if manifest parsing fails, root
 * is missing, or the temp/store directory cannot be created.
 *
 * Requires root (getuid() == 0). Returns 1 with a diagnostic otherwise. */
int profile_hooks_apply(const char *manifest_json, const char *db_root,
                        int gen_id, int dry_run);

#endif /* TWO9_PROFILE_HOOKS_H */
