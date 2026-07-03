/* reconcile.h - 2O9 declarative reconciler / diff engine
 *
 * Computes the diff between the desired state (from 2O9.nix evaluation)
 * and the current generation, producing a transaction plan:
 *
 * - Packages to install from official repos (pacman)
 * - Packages to build from AUR
 * - Packages to remove (present in current but not in desired)
 * - Services to enable (systemctl enable)
 * - Services to disable (systemctl disable)
 *
 * The transaction is then executed by cmd_apply.
 *
 * Part of 2O9.  Pure C, no C++ dependencies.
 */

#ifndef TWO9_RECONCILE_H
#define TWO9_RECONCILE_H

#include <stddef.h>

/* A single package name - used in diff sets */
typedef struct pkg_name {
    char *name;
    struct pkg_name *next;
} pkg_name_t;

/* A service entry extracted from the manifest */
typedef struct svc_entry {
    char *name;          /* e.g. "sshd" */
    int  enable;         /* 1 = should be enabled, 0 = should be disabled */
    struct svc_entry *next;
} svc_entry_t;

/* A reconciliation transaction - the diff between desired and current state */
typedef struct reconcile_txn {
    /* Packages to install from official repos */
    pkg_name_t *repo_install;
    size_t      repo_install_count;

    /* Packages to build and install from AUR */
    pkg_name_t *aur_install;
    size_t      aur_install_count;

    /* Packages to remove (in current but not in desired) */
    pkg_name_t *pkg_remove;
    size_t      pkg_remove_count;

    /* Services to enable */
    svc_entry_t *svc_enable;
    size_t       svc_enable_count;

    /* Services to disable */
    svc_entry_t *svc_disable;
    size_t       svc_disable_count;

    /* All desired repo packages (for the new generation manifest) */
    pkg_name_t *all_repo_pkgs;
    size_t      all_repo_pkg_count;

    /* All desired AUR packages (for the new generation manifest) */
    pkg_name_t *all_aur_pkgs;
    size_t      all_aur_pkg_count;

    /* All desired services (for the new generation manifest) */
    svc_entry_t *all_services;
    size_t       all_service_count;
} reconcile_txn_t;

/* Reconcile the desired JSON manifest against the current generation.
 *
 * desired_json:  JSON output from the Nix evaluator (2O9.nix evaluation)
 * current_pkgs:  linked list of packages in the current generation (may be NULL)
 * db_root:       path to the generation DB root (for reading current manifest)
 *
 * Returns a newly allocated transaction. Caller must free with reconcile_free(). */
reconcile_txn_t *reconcile(const char *desired_json,
                            const char *db_root);

/* Free a transaction and all its contents */
void reconcile_free(reconcile_txn_t *txn);

/* Execute a reconciliation transaction:
 *   1. Install repo packages via pacman
 *   2. Build and install AUR packages
 *   3. Remove packages no longer in the manifest
 *   4. Enable/disable services
 *
 * Returns 0 on success, non-zero on failure. */
int reconcile_execute(reconcile_txn_t *txn);

/* Helper: create a pkg_name_t linked list entry */
pkg_name_t *pkg_name_create(const char *name);
void pkg_name_list_free(pkg_name_t *head);

/* Helper: create a svc_entry_t linked list entry */
svc_entry_t *svc_entry_create(const char *name, int enable);
void svc_entry_list_free(svc_entry_t *head);

/* Check if a package name is in a pkg_name_t list */
int pkg_name_list_contains(pkg_name_t *head, const char *name);

#endif /* TWO9_RECONCILE_H */
