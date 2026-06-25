/* activation.h — 2O9 post-extract activation phase
 *
 * Replaces pacman's .install scripts with an idempotent activation
 * phase that runs after packages are extracted into the store but
 * before the new generation is committed.
 *
 * From DESIGN.md §7 "Install scripts — the activation model":
 *
 *   1. Stop affected services
 *   2. Populate /etc symlinks (unit files, configs from the store)
 *   3. Apply sysusers    (systemd-sysusers)
 *   4. Apply tmpfiles    (systemd-tmpfiles --create)
 *   5. Create/update users and groups
 *   6. daemon-reload     (systemctl daemon-reload)
 *   7. Enable/disable services (per 2O9.nix services block)
 *   8. Rebuild caches    (icon cache, desktop db, font cache)
 *   9. Start/restart services that changed in this generation
 *
 * All steps are idempotent — safe to run on every 209 apply.
 *
 * Status: SKELETON. Step 7 (services enable/disable) is wired into
 * cmd_apply via activation_services_apply(); the other 8 steps are
 * stubbed with TODOs and will be filled in as Phase 5 polish work.
 */

#ifndef TWO9_ACTIVATION_H
#define TWO9_ACTIVATION_H

#include "reconcile.h"

/* Run the full activation phase for a transaction.
 *
 * Called by cmd_apply() after the new generation's packages are in
 * the store and the symlink farm is built, but before the generation
 * is committed.
 *
 * Returns 0 on success, -1 on failure (aborts the apply).
 */
int activation_run(reconcile_txn_t *txn);

/* Apply just the services step (enable/disable per manifest).
 * This is the only step currently wired up; extracted as a separate
 * function so cmd_apply can call it directly even before the full
 * activation_run() is complete. */
int activation_services_apply(reconcile_txn_t *txn);

/* Individual phase steps — each idempotent, each logs failures but
 * continues (a failed icon-cache rebuild shouldn't abort the whole
 * apply). These are stubs; implementations land in Phase 5. */
int activation_stop_affected_services(reconcile_txn_t *txn);
int activation_populate_etc_symlinks(void);
int activation_apply_sysusers(void);
int activation_apply_tmpfiles(void);
int activation_update_users_groups(void);
int activation_daemon_reload(void);
int activation_rebuild_caches(void);
int activation_start_changed_services(reconcile_txn_t *txn);

#endif /* TWO9_ACTIVATION_H */
