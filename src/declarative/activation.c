/* activation.c — 2O9 post-extract activation phase (skeleton)
 *
 * Implements the 9-step activation phase from DESIGN.md §7.
 * Only step 7 (services enable/disable) is fully wired; the others
 * are stubs with TODOs for Phase 5 polish.
 *
 * Why stubs instead of full implementations:
 *   - Steps 1, 9 require knowing which services "changed" in this
 *     generation — needs richer reconciliation than we have today.
 *   - Steps 2-5 require walking the store paths of the new generation
 *     to find systemd unit files, tmpfiles.d configs, sysusers.d
 *     configs, etc. The store adapter doesn't currently expose a
 *     "list all files matching pattern X in this generation" API.
 *   - Step 8 (caches) is a known list of binaries to invoke but
 *     should be skipped silently if the binary isn't installed.
 *
 * Each stub logs what it would do, so 209 apply output shows the
 * intended activation sequence even before the implementation lands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "activation.h"
#include "reconcile.h"

/* ── Helper: run a subprocess, return exit status, log on failure ── */

static int run_cmd(const char *argv[], const char *why)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* child */
        execvp(argv[0], (char *const *)argv);
        perror(argv[0]);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        /* Binary not found — log and continue (idempotent: missing
         * helper tools are not fatal, same as pacman's approach). */
        fprintf(stderr, "activation: %s not installed (skipped: %s)\n",
                argv[0], why);
        return 0;
    }
    fprintf(stderr, "activation: %s failed (exit %d): %s\n",
            argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1, why);
    return -1;
}

/* ── Public: services enable/disable ─────────────────────────────── */

int activation_services_apply(reconcile_txn_t *txn)
{
    if (!txn) return 0;

    /* Disable services first so daemon-reload picks up the removal */
    svc_entry_t *s = txn->svc_disable;
    while (s) {
        if (s->name) {
            const char *argv[] = {"systemctl", "disable", s->name, NULL};
            fprintf(stderr, "activation: systemctl disable %s\n", s->name);
            run_cmd(argv, "disable service");
        }
        s = s->next;
    }

    /* Enable services */
    s = txn->svc_enable;
    while (s) {
        if (s->name) {
            const char *argv[] = {"systemctl", "enable", s->name, NULL};
            fprintf(stderr, "activation: systemctl enable %s\n", s->name);
            run_cmd(argv, "enable service");
        }
        s = s->next;
    }

    return 0;
}

/* ── Stubbed steps — Phase 5 implementations ────────────────────── */

int activation_stop_affected_services(reconcile_txn_t *txn)
{
    /* TODO: walk txn->services_on/off (or a new txn->services_changed
     * field) and stop them before symlinks change, so the new
     * generation's binaries can be picked up cleanly on restart.
     *
     * For now: do nothing. The user reboots after `209 apply` per
     * DESIGN.md §7 "Generation switch — reboot required". */
    (void)txn;
    return 0;
}

int activation_populate_etc_symlinks(void)
{
    /* TODO: walk all store paths in the new generation, find any
     * files under etc/ or usr/lib/systemd/system/, and symlink them
     * into /etc/ (or /usr/lib/systemd/system/) at their canonical
     * paths. The symlink farm already does this for binaries in
     * ~/.local/bin — extend it to handle /etc/ entries. */
    fprintf(stderr, "activation: (stub) populate /etc symlinks\n");
    return 0;
}

int activation_apply_sysusers(void)
{
    /* TODO: collect sysusers.d config files from the new generation's
     * store paths and run `systemd-sysusers <files>` to create any
     * users/groups declared by packages. Idempotent. */
    const char *argv[] = {"systemd-sysusers", NULL};
    return run_cmd(argv, "apply sysusers.d configs");
}

int activation_apply_tmpfiles(void)
{
    /* TODO: collect tmpfiles.d config files from the new generation's
     * store paths and run `systemd-tmpfiles --create <files>` to
     * create declared runtime directories and files. Idempotent. */
    const char *argv[] = {"systemd-tmpfiles", "--create", NULL};
    return run_cmd(argv, "apply tmpfiles.d configs");
}

int activation_update_users_groups(void)
{
    /* TODO: scan package metadata for declared users/groups (some
     * Arch packages create users in .install scripts via useradd;
     * we extract the intent into package metadata at scan time).
     *
     * For now: rely on systemd-sysusers (step 3) to handle the
     * common case. */
    fprintf(stderr, "activation: (stub) update users and groups\n");
    return 0;
}

int activation_daemon_reload(void)
{
    const char *argv[] = {"systemctl", "daemon-reload", NULL};
    return run_cmd(argv, "reload systemd after unit files changed");
}

int activation_rebuild_caches(void)
{
    /* Each of these is silently skipped if the binary isn't installed. */
    const char *icon_argv[] = {"gtk-update-icon-cache", "-f", "/usr/share/icons/hicolor", NULL};
    const char *desktop_argv[] = {"update-desktop-database", "-q", NULL};
    const char *font_argv[] = {"fc-cache", "-f", NULL};

    run_cmd(icon_argv, "rebuild icon cache");
    run_cmd(desktop_argv, "rebuild desktop database");
    run_cmd(font_argv, "rebuild font cache");
    return 0;
}

int activation_start_changed_services(reconcile_txn_t *txn)
{
    /* TODO: after daemon-reload + enable, restart any service whose
     * unit file or binaries changed in this generation. Until
     * "what changed" is tracked, we rely on the user rebooting. */
    (void)txn;
    fprintf(stderr, "activation: (stub) start/restart changed services (reboot recommended)\n");
    return 0;
}

/* ── Full activation sequence ────────────────────────────────────── */

int activation_run(reconcile_txn_t *txn)
{
    fprintf(stderr, "=== activation phase ===\n");

    /* 1. Stop affected services */
    activation_stop_affected_services(txn);

    /* 2. Populate /etc symlinks */
    activation_populate_etc_symlinks();

    /* 3. Apply sysusers */
    activation_apply_sysusers();

    /* 4. Apply tmpfiles */
    activation_apply_tmpfiles();

    /* 5. Update users/groups */
    activation_update_users_groups();

    /* 6. daemon-reload */
    activation_daemon_reload();

    /* 7. Enable/disable services */
    activation_services_apply(txn);

    /* 8. Rebuild caches */
    activation_rebuild_caches();

    /* 9. Start/restart changed services */
    activation_start_changed_services(txn);

    fprintf(stderr, "=== activation phase complete ===\n");
    return 0;
}
