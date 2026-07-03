/* reconcile.c - 2O9 declarative reconciler / diff engine
 *
 * Computes the diff between desired state (2O9.nix evaluation result)
 * and the current generation. Uses cJSON for proper JSON parsing.
 *
 * Transaction plan:
 *   repo_install:  in desired.packages but not in current generation
 *   aur_install:   in desired.aur.packages but not in current generation
 *   pkg_remove:    in current generation but not in desired (either list)
 *   svc_enable:    services with enable=true in desired but not yet active
 *   svc_disable:   services that were enabled but not in desired
 *
 * Part of 2O9.  Pure C, no C++ dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reconcile.h"
#include "cJSON.h"
#include "../aur/build.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

pkg_name_t *pkg_name_create(const char *name)
{
    pkg_name_t *p = calloc(1, sizeof(*p));
    if (p) p->name = strdup(name);
    return p;
}

void pkg_name_list_free(pkg_name_t *head)
{
    while (head) {
        pkg_name_t *next = head->next;
        free(head->name);
        free(head);
        head = next;
    }
}

svc_entry_t *svc_entry_create(const char *name, int enable)
{
    svc_entry_t *s = calloc(1, sizeof(*s));
    if (s) {
        s->name = strdup(name);
        s->enable = enable;
    }
    return s;
}

void svc_entry_list_free(svc_entry_t *head)
{
    while (head) {
        svc_entry_t *next = head->next;
        free(head->name);
        free(head);
        head = next;
    }
}

int pkg_name_list_contains(pkg_name_t *head, const char *name)
{
    for (pkg_name_t *p = head; p; p = p->next) {
        if (strcmp(p->name, name) == 0) return 1;
    }
    return 0;
}

/* Append a package name to a linked list, return tail */
static pkg_name_t **pkg_name_append(pkg_name_t **tail, const char *name)
{
    pkg_name_t *n = pkg_name_create(name);
    if (!n) return tail;
    *tail = n;
    return &n->next;
}

/* Append a service entry to a linked list, return tail */
static svc_entry_t **svc_entry_append(svc_entry_t **tail,
                                       const char *name, int enable)
{
    svc_entry_t *s = svc_entry_create(name, enable);
    if (!s) return tail;
    *tail = s;
    return &s->next;
}

/* ── Read current generation's package list from manifest ──────────── */

static pkg_name_t *read_current_packages(const char *db_root)
{
    /* Read the 'current' symlink to find the generation directory */
    char current_link[4096];
    snprintf(current_link, sizeof(current_link), "%s/current", db_root);

    /* Read symlink target */
    char gen_dir[4096];
    ssize_t len = readlink(current_link, gen_dir, sizeof(gen_dir) - 1);
    if (len < 0) return NULL;  /* no current generation */
    gen_dir[len] = '\0';

    /* Build manifest path: gen_dir might be relative or absolute */
    char manifest_path[4096];
    if (gen_dir[0] == '/')
        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/manifest.json", gen_dir);
    else
        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/%s/manifest.json", db_root, gen_dir);

    FILE *f = fopen(manifest_path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    buf[nread] = '\0';
    fclose(f);

    /* Parse with cJSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return NULL;

    pkg_name_t *head = NULL;
    pkg_name_t **tail = &head;

    /* Extract packages from the "packages" array */
    cJSON *pkgs = cJSON_GetObjectItem(root, "packages");
    if (pkgs && cJSON_IsArray(pkgs)) {
        cJSON *item;
        cJSON_ArrayForEach(item, pkgs) {
            cJSON *name_obj = cJSON_GetObjectItem(item, "name");
            if (name_obj && cJSON_IsString(name_obj)) {
                tail = pkg_name_append(tail, name_obj->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    return head;
}

/* ── Extract services from the manifest JSON ──────────────────────── */

static void extract_services(cJSON *services_obj,
                              svc_entry_t ***enable_tail,
                              svc_entry_t ***disable_tail,
                              svc_entry_t ***all_tail)
{
    if (!services_obj || !cJSON_IsObject(services_obj)) return;

    cJSON *svc;
    cJSON_ArrayForEach(svc, services_obj) {
        const char *svc_name = svc->string;
        if (!svc_name) continue;

        /* svc points to each value in the services object.
         * svc->string is the key (e.g. "sshd").
         * svc itself is the value (e.g. {"enable": true} or true). */
        int should_enable = 0;

        if (cJSON_IsObject(svc)) {
            cJSON *enable = cJSON_GetObjectItem(svc, "enable");
            if (enable && cJSON_IsBool(enable)) {
                should_enable = enable->valueint;
            }
        } else if (cJSON_IsBool(svc)) {
            should_enable = svc->valueint;
        }

        /* Add to all_services */
        *all_tail = svc_entry_append(*all_tail, svc_name, should_enable);

        /* Add to enable or disable list */
        if (should_enable)
            *enable_tail = svc_entry_append(*enable_tail, svc_name, 1);
        else
            *disable_tail = svc_entry_append(*disable_tail, svc_name, 0);
    }
}

/* ── Main reconciliation ────────────────────────────────────────────── */

reconcile_txn_t *reconcile(const char *desired_json, const char *db_root)
{
    if (!desired_json) return NULL;

    reconcile_txn_t *txn = calloc(1, sizeof(*txn));
    if (!txn) return NULL;

    /* Parse the desired JSON manifest */
    cJSON *root = cJSON_Parse(desired_json);
    if (!root) {
        free(txn);
        return NULL;
    }

    /* ── Extract desired repo packages ────────────────────────────── */
    pkg_name_t *desired_repo_head = NULL;
    pkg_name_t **desired_repo_tail = &desired_repo_head;

    cJSON *packages = cJSON_GetObjectItem(root, "packages");
    if (packages && cJSON_IsArray(packages)) {
        cJSON *item;
        cJSON_ArrayForEach(item, packages) {
            if (cJSON_IsString(item)) {
                desired_repo_tail = pkg_name_append(desired_repo_tail,
                                                     item->valuestring);
                txn->all_repo_pkg_count++;
            }
        }
    }
    txn->all_repo_pkgs = desired_repo_head;

    /* ── Extract desired AUR packages ─────────────────────────────── */
    pkg_name_t *desired_aur_head = NULL;
    pkg_name_t **desired_aur_tail = &desired_aur_head;

    cJSON *aur_obj = cJSON_GetObjectItem(root, "aur");
    if (aur_obj && cJSON_IsObject(aur_obj)) {
        cJSON *aur_pkgs = cJSON_GetObjectItem(aur_obj, "packages");
        if (aur_pkgs && cJSON_IsArray(aur_pkgs)) {
            cJSON *item;
            cJSON_ArrayForEach(item, aur_pkgs) {
                if (cJSON_IsString(item)) {
                    desired_aur_tail = pkg_name_append(desired_aur_tail,
                                                        item->valuestring);
                    txn->all_aur_pkg_count++;
                }
            }
        }
    }
    txn->all_aur_pkgs = desired_aur_head;

    /* ── Extract services ─────────────────────────────────────────── */
    cJSON *services = cJSON_GetObjectItem(root, "services");
    svc_entry_t *svc_enable_head = NULL, **svc_enable_tail = &svc_enable_head;
    svc_entry_t *svc_disable_head = NULL, **svc_disable_tail = &svc_disable_head;
    svc_entry_t *all_svc_head = NULL, **all_svc_tail = &all_svc_head;

    extract_services(services, &svc_enable_tail, &svc_disable_tail, &all_svc_tail);

    /* Count services */
    for (svc_entry_t *s = svc_enable_head; s; s = s->next)
        txn->svc_enable_count++;
    for (svc_entry_t *s = svc_disable_head; s; s = s->next)
        txn->svc_disable_count++;
    for (svc_entry_t *s = all_svc_head; s; s = s->next)
        txn->all_service_count++;

    txn->svc_enable = svc_enable_head;
    txn->svc_disable = svc_disable_head;
    txn->all_services = all_svc_head;

    /* ── Diff against current generation ──────────────────────────── */
    pkg_name_t *current_pkgs = read_current_packages(db_root);

    /* Repo packages to install: in desired but not in current */
    pkg_name_t **install_tail = &txn->repo_install;
    for (pkg_name_t *p = desired_repo_head; p; p = p->next) {
        if (!pkg_name_list_contains(current_pkgs, p->name)) {
            install_tail = pkg_name_append(install_tail, p->name);
            txn->repo_install_count++;
        }
    }

    /* AUR packages to install: in desired.aur but not in current */
    pkg_name_t **aur_install_tail = &txn->aur_install;
    for (pkg_name_t *p = desired_aur_head; p; p = p->next) {
        if (!pkg_name_list_contains(current_pkgs, p->name)) {
            aur_install_tail = pkg_name_append(aur_install_tail, p->name);
            txn->aur_install_count++;
        }
    }

    /* Packages to remove: in current but not in either desired list */
    pkg_name_t **remove_tail = &txn->pkg_remove;
    for (pkg_name_t *p = current_pkgs; p; p = p->next) {
        if (!pkg_name_list_contains(desired_repo_head, p->name) &&
            !pkg_name_list_contains(desired_aur_head, p->name)) {
            remove_tail = pkg_name_append(remove_tail, p->name);
            txn->pkg_remove_count++;
        }
    }

    pkg_name_list_free(current_pkgs);
    cJSON_Delete(root);
    return txn;
}

/* ── Execute a reconciliation transaction ───────────────────────────── */

int reconcile_execute(reconcile_txn_t *txn)
{
    if (!txn) return -1;

    int rc = 0;

    /* Step 1: Install repo packages via pacman */
    if (txn->repo_install_count > 0) {
        printf("  installing %zu repo packages:", txn->repo_install_count);
        for (pkg_name_t *p = txn->repo_install; p; p = p->next)
            printf(" %s", p->name);
        printf("\n");

        /* Build pacman command */
        size_t cmd_cap = 256;
        char *cmd = calloc(cmd_cap, 1);
        snprintf(cmd, cmd_cap, "sudo pacman -S --noconfirm --needed");
        for (pkg_name_t *p = txn->repo_install; p; p = p->next) {
            size_t needed = strlen(cmd) + strlen(p->name) + 2;
            if (needed >= cmd_cap) {
                cmd_cap = needed + 64;
                cmd = realloc(cmd, cmd_cap);
            }
            strcat(cmd, " ");
            strcat(cmd, p->name);
        }

        int ret = system(cmd);
        free(cmd);
        if (ret != 0) {
            fprintf(stderr, "  warning: pacman returned %d\n", ret);
            rc = ret;
        }
    }

    /* Step 2: Build and install AUR packages */
    if (txn->aur_install_count > 0) {
        printf("  building %zu AUR packages:", txn->aur_install_count);
        for (pkg_name_t *p = txn->aur_install; p; p = p->next)
            printf(" %s", p->name);
        printf("\n");

        /* For each AUR package, clone → build → install */
        build_config_t config = {
            .build_dir = "/tmp/2O9-build",
            .no_confirm = 1,
            .skip_review = 1,
            .chroot = 0,
            .sign = 0,
        };

        for (pkg_name_t *p = txn->aur_install; p; p = p->next) {
            printf("    building %s...\n", p->name);

            /* Clone (or fetch) the PKGBUILD */
            if (aur_clone(p->name, config.build_dir) != 0) {
                fprintf(stderr, "    failed to clone %s\n", p->name);
                rc = -1;
                continue;
            }

            /* Build */
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

            /* Install */
            if (aur_install(result->pkg_path) != 0) {
                fprintf(stderr, "    failed to install %s\n", p->name);
                rc = -1;
            }

            build_result_free(result);
        }
    }

    /* Step 3: Remove packages no longer in the manifest */
    if (txn->pkg_remove_count > 0) {
        printf("  removing %zu packages:", txn->pkg_remove_count);
        for (pkg_name_t *p = txn->pkg_remove; p; p = p->next)
            printf(" %s", p->name);
        printf("\n");

        size_t cmd_cap = 256;
        char *cmd = calloc(cmd_cap, 1);
        snprintf(cmd, cmd_cap, "sudo pacman -Rns --noconfirm");
        for (pkg_name_t *p = txn->pkg_remove; p; p = p->next) {
            size_t needed = strlen(cmd) + strlen(p->name) + 2;
            if (needed >= cmd_cap) {
                cmd_cap = needed + 64;
                cmd = realloc(cmd, cmd_cap);
            }
            strcat(cmd, " ");
            strcat(cmd, p->name);
        }

        int ret = system(cmd);
        free(cmd);
        if (ret != 0) {
            fprintf(stderr, "  warning: pacman -Rns returned %d\n", ret);
            /* Don't set rc - removal failures are non-fatal */
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
            snprintf(cmd, sizeof(cmd), "sudo systemctl enable --now %s 2>/dev/null",
                     s->name);
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
            snprintf(cmd, sizeof(cmd), "sudo systemctl disable --now %s 2>/dev/null",
                     s->name);
            system(cmd);
        }
    }

    return rc;
}

/* ── Free ──────────────────────────────────────────────────────────── */

void reconcile_free(reconcile_txn_t *txn)
{
    if (!txn) return;
    pkg_name_list_free(txn->repo_install);
    pkg_name_list_free(txn->aur_install);
    pkg_name_list_free(txn->pkg_remove);
    pkg_name_list_free(txn->all_repo_pkgs);
    pkg_name_list_free(txn->all_aur_pkgs);
    svc_entry_list_free(txn->svc_enable);
    svc_entry_list_free(txn->svc_disable);
    svc_entry_list_free(txn->all_services);
    free(txn);
}
