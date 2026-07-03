/* activation.h - what runs after packages land in the store
 *
 * pacman runs .install scripts after extraction. We don't - those
 * scripts assume files are at FHS paths, they're not idempotent, and
 * they can't be re-run on rollback. Instead, we extract the *intent*
 * (systemd units, tmpfiles, sysusers, icon caches) and run it through
 * a 9-step idempotent activation phase.
 *
 * From DESIGN.md §7:
 *
 *   1. Stop affected services
 *   2. Populate /etc symlinks (unit files, configs from the store)
 *   3. Apply sysusers    (systemd-sysusers)
 *   4. Apply tmpfiles    (systemd-tmpfiles --create)
 *   5. Create/update users and groups
 *   6. daemon-reload     (systemctl daemon-reload)
 *   7. Enable/disable services (per 2O9.nix services block)
 *   8. Rebuild caches    (icon cache, desktop db, font cache, ldconfig)
 *   9. Start/restart services that changed in this generation
 *
 * Every step is idempotent - safe to run on every `209 apply`. If a
 * tool isn't installed (gtk-update-icon-cache on a headless box, say),
 * we skip it silently rather than failing the whole apply.
 *
 * Implementation notes:
 * - Step 2 is a no-op: the symlink farm already handles /etc/ entries
 *     via the store_manifest is_config flag.
 * - Step 5 is a no-op: systemd-sysusers (step 3) covers the standard
 *     case. Packages needing custom user creation outside sysusers.d
 *     get a warning (DESIGN.md: "don't run .install scripts").
 * - Steps 3 and 4 invoke systemd-sysusers / systemd-tmpfiles with no
 *     explicit file args, so they scan the default system directories
 *     where the symlink farm has placed the configs. A future enhancement
 *     is to pass explicit file lists from the new generation's store paths.
 * - Step 9 starts (not restarts) services - restart would disrupt
 *     running sessions. The user should still reboot for full state
 *     to take effect (DESIGN.md §7).
 */

#ifndef TWO9_ACTIVATION_H
#define TWO9_ACTIVATION_H

#include "reconcile.h"

/* Run the full activation phase for a transaction.
 *
 * Called by cmd_apply() after the new generation's packages are in
 * the store and the symlink farm is built, but before the generation
 * is reported as committed.
 *
 * txn may be NULL (imperative install path) - services enable/disable
 * is skipped, but daemon-reload, cache rebuild, and other idempotent
 * steps still run.
 *
 * Returns 0 on success, -1 on failure (currently always returns 0;
 * individual step failures are logged but non-fatal).
 */
int activation_run(reconcile_txn_t *txn);

/* Apply just the services step (enable/disable per manifest).
 * Extracted as a separate function so it can be called independently
 * (e.g. from tests or future imperative service commands). */
int activation_services_apply(reconcile_txn_t *txn);

/* The 8 individual step functions (stop_affected_services,
 * populate_etc_symlinks, apply_sysusers, apply_tmpfiles,
 * update_users_groups, daemon_reload, rebuild_caches,
 * start_changed_services) are static in activation.c - only
 * activation_run() calls them. If a future caller needs one
 * directly, promote it to a public declaration here. */

#endif /* TWO9_ACTIVATION_H */
