/* reconcile.c - 2O9 declarative reconciler / diff engine
 *
 * Computes the diff between the desired state (from 2O9.nix evaluation)
 * and the current generation, producing a transaction plan.
 *
 * The transaction is then executed by cmd_apply. Execution now uses
 * 209's own install path (lib2O9 + store adapter) instead of shelling
 * out to pacman.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reconcile.h"
#include "cJSON.h"
#include "../aur/build.h"

/* Forward declarations from main.c - we call cmd_install which does
 * the full lib2O9 + store adapter + generation pipeline. */
extern int cmd_install(const char *pkg_name);
extern int cmd_remove(const char *pkg_name);

int reconcile_execute(reconcile_txn_t *txn)
{
    if (!txn) return -1;

    int rc = 0;

    /* Step 1: Install repo packages via 209's own install path.
     * If a package isn't found in the repo sync DBs, automatically
     * fall back to building from the AUR. */
    if (txn->repo_install_count > 0) {
        printf("  installing %zu repo packages:", txn->repo_install_count);
        for (pkg_name_t *p = txn->repo_install; p; p = p->next)
            printf(" %s", p->name);
        printf("\n");

        build_config_t config = {
            .build_dir = "/tmp/2O9-build",
            .no_confirm = 1,
            .skip_review = 1,
            .chroot = 0,
            .sign = 0,
        };

        for (pkg_name_t *p = txn->repo_install; p; p = p->next) {
            printf("    installing %s...\n", p->name);
            int ret = cmd_install(p->name);
            if (ret != 0) {
                printf("    %s not in repos, trying AUR...\n", p->name);

                /* Fall back to AUR build */
                if (aur_clone(p->name, config.build_dir) != 0) {
                    fprintf(stderr, "    failed to clone %s from AUR\n", p->name);
                    rc = -1;
                    continue;
                }

                build_result_t *result = aur_build(p->name, config.build_dir, &config);
                if (!result || !result->success) {
                    fprintf(stderr, "    failed to build %s: %s\n",
                            p->name,
                            (result && result->error_msg) ? result->error_msg
                                                          : "unknown error");
                    if (result) build_result_free(result);
                    rc = -1;
                    continue;
                }

                if (aur_install(result->pkg_path) != 0) {
                    fprintf(stderr, "    failed to install %s from AUR\n", p->name);
                    rc = -1;
                } else {
                    printf("    installed %s from AUR\n", p->name);
                }

                build_result_free(result);
            }
        }
    }

    /* Step 2: Build and install AUR packages */
    if (txn->aur_install_count > 0) {
        printf("  building %zu AUR packages:", txn->aur_install_count);
        for (pkg_name_t *p = txn->aur_install; p; p = p->next)
            printf(" %s", p->name);
        printf("\n");

        build_config_t config = {
            .build_dir = "/tmp/2O9-build",
            .no_confirm = 1,
            .skip_review = 1,
            .chroot = 0,
            .sign = 0,
        };

        for (pkg_name_t *p = txn->aur_install; p; p = p->next) {
            printf("    building %s...\n", p->name);

            if (aur_clone(p->name, config.build_dir) != 0) {
                fprintf(stderr, "    failed to clone %s\n", p->name);
                rc = -1;
                continue;
            }

            build_result_t *result = aur_build(p->name, config.build_dir, &config);
            if (!result || !result->success) {
                fprintf(stderr, "    failed to build %s: %s\n",
                        p->name,
                        (result && result->error_msg) ? result->error_msg
                                                      : "unknown error");
                if (result) build_result_free(result);
                rc = -1;
                continue;
            }

            if (aur_install(result->pkg_path) != 0) {
                fprintf(stderr, "    failed to install %s\n", p->name);
                rc = -1;
            }

            build_result_free(result);
        }
    }

    /* Step 3: Remove packages no longer in the manifest.
     * Note: cmd_remove commits its own generation. This is intentional -
     * the remove creates a new generation without the package, and then
     * cmd_apply will commit another generation with the desired state.
     * If the install failed, the remove still cleans up old packages. */
    if (txn->pkg_remove_count > 0) {
        printf("  removing %zu packages:", txn->pkg_remove_count);
        for (pkg_name_t *p = txn->pkg_remove; p; p = p->next)
            printf(" %s", p->name);
        printf("\n");

        for (pkg_name_t *p = txn->pkg_remove; p; p = p->next) {
            int ret = cmd_remove(p->name);
            if (ret != 0) {
                fprintf(stderr, "    warning: failed to remove %s\n", p->name);
            }
        }
    }

    /* Step 4: Enable/disable services */
    if (txn->svc_enable_count > 0) {
        printf("  enabling %zu services:", txn->svc_enable_count);
        for (svc_entry_t *s = txn->svc_enable; s; s = s->next)
            printf(" %s", s->name);
        printf("\n");

        for (svc_entry_t *s = txn->svc_enable; s; s = s->next) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "systemctl enable %s 2>/dev/null", s->name);
            system(cmd);
        }
    }

    if (txn->svc_disable_count > 0) {
        printf("  disabling %zu services:", txn->svc_disable_count);
        for (svc_entry_t *s = txn->svc_disable; s; s = s->next)
            printf(" %s", s->name);
        printf("\n");

        for (svc_entry_t *s = txn->svc_disable; s; s = s->next) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "systemctl disable %s 2>/dev/null", s->name);
            system(cmd);
        }
    }

    return rc;
}
