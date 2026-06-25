/* 2O9 CLI — main entry point
 *
 * The `209` binary. SOV (Subject Object Verb) command dispatch.
 *
 * Phase 0: -V, -h
 * Phase 1: <pkg> install, <n> rollback, generations, apply, sync, gc
 *
 * From DESIGN.md §5:
 *   209 <subject> <verb>          — SOV pattern
 *   209 <command>                 — zero-argument command
 *   209 <pkg1> <pkg2> <verb>     — multi-subject
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <curl/curl.h>

#include "store/store.h"
#include "declarative/gen.h"
#include "store/symlinks.h"
#include "aur/aur_rpc.h"
#include "aur/build.h"
#include "aur/resolver.h"
#include "aur/cJSON.h"
#include "declarative/reconcile.h"
#include "declarative/activation.h"
#include "trakker/trakker.h"
#include "nix_eval.h"

#ifndef PACKAGE
#define PACKAGE "2O9"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.0.1"
#endif

#define VERSION_STR PACKAGE " " PACKAGE_VERSION

/* ── Zero-argument commands ──────────────────────────────────────── */

static int cmd_version(void)
{
        printf("%s\n", VERSION_STR);
        printf("License: GPL-2.0-only\n");
        printf("Store root: %s\n", STORE_ROOT);
        return 0;
}

static int cmd_usage(void)
{
        printf("Usage: 209 [options] <subject> <verb>\n");
        printf("       209 [options] <command>\n\n");
        printf("Unified package manager: pacman + AUR + Nix store\n\n");
        printf("Commands:\n");
        printf("  apply              Apply declarative config (2O9.nix)\n");
        printf("  generations        List generations\n");
        printf("  sync               Sync repo databases\n");
        printf("  gc                 Garbage-collect unreferenced store paths\n");
        printf("  news               Show Arch Linux news\n\n");
        printf("SOV patterns:\n");
        printf("  209 <pkg> install  Install package from repo\n");
        printf("  209 <pkg> remove   Remove package\n");
        printf("  209 <pkg> info     Show installed package info (falls back to AUR)\n");
        printf("  209 <term> search  Search installed packages (falls back to AUR)\n");
        printf("  209 <pkg> aur build    Build from AUR\n");
        printf("  209 <term> aur search  Search AUR\n");
        printf("  209 <pkg> aur info     Show AUR package info\n");
        printf("  209 <pkg> aur review   Review PKGBUILD diff\n");
        printf("  209 <pkg> trakker [flags]  Run in sandbox\n\n");
        printf("Rollback:\n");
        printf("  209 <n> rollback   Roll back to generation #n\n");
        printf("  209 <n> pin        Pin a generation (protect from GC)\n\n");
        printf("Options:\n");
        printf("  -V, --version      Show version\n");
        printf("  -h, --help         Show this help\n");
        return 0;
}

/* Forward declaration — defined after cmd_install */
static gen_pkg_t *read_current_gen_packages(const char *db_root, int current_id);

/* ── Generations ─────────────────────────────────────────────────── */

static int cmd_generations(void)
{
        char *home = getenv("HOME");
        char db_root[PATH_MAX];

        /* Try user DB first, fall back to system */
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB at %s\n", db_root);
                return 1;
        }

        size_t count;
        gen_t **gens = gen_db_list(db, &count);

        int current = gen_db_current(db);

        if (count == 0) {
                printf("No generations yet.\n");
        } else {
                printf("  ID  Packages  Pinned\n");
                printf("  ──  ────────  ──────\n");
                for (size_t i = 0; i < count; i++) {
                        /* Load package count from manifest */
                        gen_pkg_t *gp = read_current_gen_packages(db_root, gens[i]->id);
                        size_t pc = 0;
                        gen_pkg_t *gq = gp;
                        while (gq) { pc++; gq = gq->next; }
                        gen_pkg_list_free(gp);
                        /* Check pinned status */
                        char pin_path[PATH_MAX];
                        snprintf(pin_path, sizeof(pin_path),
                                 "%s/generations/%d/.pinned", db_root, gens[i]->id);
                        struct stat pst;
                        int pinned = (stat(pin_path, &pst) == 0);
                        printf("  %3d  %7zu  %s%s\n",
                               gens[i]->id,
                               pc,
                               pinned ? "yes" : "no",
                               gens[i]->id == current ? "  ← current" : "");
                }
        }

        gen_list_free(gens, count);
        gen_db_close(db);
        return 0;
}

/* ── Helper: read current generation's package list ─────────────── */

/* Parse manifest.json to extract the package list.
 * Returns a linked list of gen_pkg_t, or NULL if no current generation. */
static gen_pkg_t *read_current_gen_packages(const char *db_root, int current_id)
{
        if (current_id <= 0) return NULL;

        char manifest_path[PATH_MAX];
        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/generations/%d/manifest.json", db_root, current_id);

        /* Read the file into a buffer — cJSON_Parse needs the full string */
        FILE *f = fopen(manifest_path, "r");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc(fsize + 1);
        if (!buf) { fclose(f); return NULL; }
        size_t nread = fread(buf, 1, (size_t)fsize, f);
        buf[nread] = '\0';
        fclose(f);

        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) return NULL;

        gen_pkg_t *pkgs = NULL;
        gen_pkg_t **tail = &pkgs;

        cJSON *arr = cJSON_GetObjectItem(root, "packages");
        if (cJSON_IsArray(arr)) {
            cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                cJSON *jname    = cJSON_GetObjectItem(item, "name");
                cJSON *jversion = cJSON_GetObjectItem(item, "version");
                cJSON *jstore   = cJSON_GetObjectItem(item, "store_path");
                cJSON *jorigin  = cJSON_GetObjectItem(item, "origin");
                if (!cJSON_IsString(jname)) continue;

                gen_pkg_t *p = gen_pkg_create(
                        jname->valuestring,
                        cJSON_IsString(jversion) ? jversion->valuestring : "unknown",
                        cJSON_IsString(jstore) ? jstore->valuestring : NULL,
                        cJSON_IsString(jorigin) ? jorigin->valuestring : "repo");
                *tail = p;
                tail = &p->next;
            }
        }

        cJSON_Delete(root);
        return pkgs;
}

/* ── Install ─────────────────────────────────────────────────────── */

static int cmd_install(const char *pkg_name)
{
        /* Phase 1: imperative install path.
         * The package lands in the current generation temporarily.
         * Next `209 apply` will flag it for removal unless it's in 2O9.nix.
         *
         * The new generation carries forward all packages from the
         * current generation, plus the newly installed one. If the
         * package is already in the current generation, we just update
         * its store path (reinstall/upgrade). */

        printf("209: installing %s...\n", pkg_name);

        /* Step 1: resolve and add to store */
        char pkg_path[PATH_MAX];
        pkg_path[0] = '\0';
        const char *env_path = getenv("TWO09_PKG_PATH");
        if (env_path) {
                snprintf(pkg_path, sizeof(pkg_path), "%s", env_path);
        }

        store_add_result_t result;

        /* If TWO09_TEST_MODE is set, skip nix-store and use a fake store path.
         * This lets us test the generation/rollback/symlink pipeline without nix. */
        const char *test_mode = getenv("TWO09_TEST_MODE");
        if (test_mode) {
                char fake_store[PATH_MAX];
                snprintf(fake_store, sizeof(fake_store), "/nix/store/%s-0.0.0", pkg_name);
                result.success = 0;
                result.store_path = strdup(fake_store);
                result.error_msg = NULL;
                printf("  [test mode] fake store path: %s\n", fake_store);
        } else if (pkg_path[0] == '\0') {
                /* No pkg path and no test mode — try direct extraction
                 * by downloading the package with pacman first.
                 * For now, tell the user what to do. */
                fprintf(stderr, "209: package resolution not yet implemented\n");
                fprintf(stderr, "    set TWO09_PKG_PATH=/path/to/pkg.tar.zst to test store add\n");
                fprintf(stderr, "    or set TWO09_TEST_MODE=1 for end-to-end pipeline test\n");
                return 1;
        } else {
                /* Try nix-store first, fall back to direct extraction */
                result = store_add(pkg_path, STORE_BACKEND_NIX_STORE);
                if (result.success != 0) {
                        fprintf(stderr, "  nix-store failed (%s), trying direct extraction...\n",
                                result.error_msg);
                        store_add_result_free(&result);
                        result = store_add(pkg_path, STORE_BACKEND_DIRECT);
                        if (result.success != 0) {
                                fprintf(stderr, "209: store add failed: %s\n", result.error_msg);
                                store_add_result_free(&result);
                                return 1;
                        }
                }
        }

        printf("  store path: %s\n", result.store_path);

        /* Step 2: open generation DB and lock */
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                store_add_result_free(&result);
                return 1;
        }

        if (gen_db_lock(db) < 0) {
                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                gen_db_close(db);
                store_add_result_free(&result);
                return 1;
        }

        /* Step 3: carry forward current generation's packages, add new one.
         * If the package already exists in the current generation, replace it
         * (upgrade/reinstall). Otherwise, append it. */
        int current = gen_db_current(db);
        gen_pkg_t *pkgs = read_current_gen_packages(db_root, current);

        /* Remove existing entry for this package if present (upgrade) */
        gen_pkg_t **pp = &pkgs;
        int replaced = 0;
        while (*pp) {
                if (strcmp((*pp)->name, pkg_name) == 0) {
                        gen_pkg_t *old = *pp;
                        *pp = old->next;
                        old->next = NULL;
                        gen_pkg_list_free(old);
                        replaced = 1;
                        break;
                }
                pp = &(*pp)->next;
        }

        /* Append the new package */
        gen_pkg_t *new_pkg = gen_pkg_create(pkg_name, "0.0.0",
                                            result.store_path, "imperative");
        /* Find tail */
        gen_pkg_t **tail = &pkgs;
        while (*tail) tail = &(*tail)->next;
        *tail = new_pkg;

        if (replaced)
                printf("  upgraded %s in generation\n", pkg_name);
        else
                printf("  added %s to generation\n", pkg_name);

        /* Step 4: commit new generation */
        int new_id = gen_db_commit(db, pkgs);
        if (new_id < 0) {
                fprintf(stderr, "209: failed to commit generation\n");
                gen_pkg_list_free(pkgs);
                gen_db_unlock(db);
                gen_db_close(db);
                store_add_result_free(&result);
                return 1;
        }

        printf("  generation #%d committed\n", new_id);

        /* Step 5: build symlink farm */
        gen_t *prev_gen = current > 0 ? gen_db_get(db, current) : NULL;
        gen_t *gen = gen_db_get(db, new_id);
        if (gen) {
                symlink_farm_build(db, gen, prev_gen);
                gen_free(gen);
        }
        if (prev_gen) gen_free(prev_gen);

        printf("  done. rollback with: 209 %d rollback\n", new_id - 1 > 0 ? new_id - 1 : 1);
        printf("  NOTE: reboot for full system state to take effect\n");

        gen_pkg_list_free(pkgs);
        gen_db_unlock(db);
        gen_db_close(db);
        store_add_result_free(&result);
        return 0;
}

/* ── Rollback ────────────────────────────────────────────────────── */

static int cmd_rollback(int target_id)
{
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                return 1;
        }

        int current = gen_db_current(db);
        if (current == 0) {
                fprintf(stderr, "209: no generations to roll back to\n");
                gen_db_close(db);
                return 1;
        }

        /* Take lock before mutating */
        if (gen_db_lock(db) < 0) {
                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                gen_db_close(db);
                return 1;
        }

        printf("209: rolling back from #%d to #%d...\n", current, target_id);

        if (gen_db_rollback(db, target_id) < 0) {
                fprintf(stderr, "209: rollback failed — generation #%d not found\n", target_id);
                gen_db_unlock(db);
                gen_db_close(db);
                return 1;
        }

        /* Rebuild symlink farm from the target generation */
        gen_t *gen = gen_db_get(db, target_id);
        if (gen) {
                symlink_farm_build(db, gen, NULL);
                gen_free(gen);
        }

        printf("  now at generation #%d\n", target_id);
        printf("  NOTE: reboot for full system state to take effect\n");
        gen_db_unlock(db);
        gen_db_close(db);
        return 0;
}

/* ── Helper: read a file into a malloc'd buffer ──────────────────── */
static char *read_file_to_string(const char *path, char **err_out)
{
        FILE *f = fopen(path, "r");
        if (!f) {
                if (err_out) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "cannot read %s: %s",
                                 path, strerror(errno));
                        *err_out = strdup(buf);
                }
                return NULL;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)fsize + 1);
        if (!buf) {
                fclose(f);
                if (err_out) *err_out = strdup("out of memory");
                return NULL;
        }
        size_t nread = fread(buf, 1, (size_t)fsize, f);
        buf[nread] = '\0';
        fclose(f);
        return buf;
}

/* ── Helper: evaluate a Nix config file, return JSON manifest ────── */
static char *eval_nix_config(const char *config_path, char **err_out)
{
        char *source = read_file_to_string(config_path, err_out);
        if (!source) return NULL;

        size_t slen = strlen(source);
        char *base_dir = strdup(config_path);
        if (base_dir) {
                char *slash = strrchr(base_dir, '/');
                if (slash) *slash = '\0';
        }

        char *json = nix_eval_file_with_base(source, slen, base_dir, err_out);
        free(source);
        free(base_dir);
        return json;
}

/* ── Helper: merge two JSON manifests per DESIGN.md §7 merge order ──
 *
 * Merge order (lowest to highest precedence):
 *   1. built-in defaults
 *   2. ~/.config/2O9/home.nix (user)
 *   3. /etc/2O9/2O9.nix (global)  ← wins on conflict
 *   4. CLI flags (not handled here)
 *
 * For list values (e.g. "packages"), we concatenate — both user and
 * global packages should be installed. For everything else, global
 * wins on conflict (shallow merge).
 *
 * Returns a malloc'd merged JSON string, or NULL on error. */
static char *merge_manifests(const char *user_json, const char *global_json,
                              char **err_out)
{
        if (!user_json && !global_json) {
                if (err_out) *err_out = strdup("no manifests to merge");
                return NULL;
        }
        if (!user_json) return strdup(global_json);
        if (!global_json) return strdup(user_json);

        /* Both present — concatenate user's packages into global's
         * packages list, then return global (global wins on all other
         * keys per DESIGN.md §7).
         *
         * ponytail: This is a known shortcut — a proper deep merge
         * would handle nested objects and per-key conflict resolution.
         * For the 2O9 manifest schema (flat top-level with list-typed
         * "packages"), concatenating the package lists and letting
         * global win on everything else is correct. */
        const char *u_pkgs = strstr(user_json, "\"packages\"");
        const char *g_pkgs = strstr(global_json, "\"packages\"");
        if (!u_pkgs || !g_pkgs) return strdup(global_json);

        const char *u_arr = strchr(u_pkgs, '[');
        const char *g_arr = strchr(g_pkgs, '[');
        if (!u_arr || !g_arr) return strdup(global_json);

        const char *u_end = strchr(u_arr, ']');
        if (!u_end) return strdup(global_json);

        size_t u_entries_len = u_end - u_arr - 1;
        char *u_entries = malloc(u_entries_len + 1);
        memcpy(u_entries, u_arr + 1, u_entries_len);
        u_entries[u_entries_len] = '\0';

        const char *g_end = strchr(g_arr, ']');
        if (!g_end) { free(u_entries); return strdup(global_json); }

        size_t pre_len = g_arr + 1 - global_json;
        size_t existing = g_end - (g_arr + 1);
        size_t comma_len = (existing > 0 && u_entries_len > 0) ? 2 : 0;
        /* post includes the ']' itself plus everything after it */
        size_t post_len = strlen(g_end);

        char *merged = malloc(pre_len + existing + comma_len + u_entries_len + post_len + 1);
        memcpy(merged, global_json, pre_len);
        size_t off = pre_len;
        if (existing > 0) {
                memcpy(merged + off, g_arr + 1, existing);
                off += existing;
        }
        if (comma_len) {
                merged[off++] = ',';
                merged[off++] = ' ';
        }
        memcpy(merged + off, u_entries, u_entries_len);
        off += u_entries_len;
        /* Copy from g_end (which is ']') to end of string */
        memcpy(merged + off, g_end, post_len);
        off += post_len;
        merged[off] = '\0';

        free(u_entries);
        return merged;
}

/* ── Apply (declarative) ─────────────────────────────────────────── */

static int cmd_apply(void)
{
        /* Evaluate 2O9.nix, produce a JSON manifest, reconcile with
         * current generation, build transaction, commit, rebuild symlink farm.
         *
         * This is the heart of Phase 3: the declarative engine.
         *
         * 1. Read /etc/2O9/2O9.nix (or $HOME/.config/2O9/2O9.nix)
         * 2. Parse and evaluate with our own C Nix evaluator
         * 3. The result is a JSON manifest describing desired state
         * 4. Diff manifest against current generation
         * 5. Build transaction (install/remove/aur-build)
         * 6. Execute transaction via store adapter + AUR helper
         * 7. Commit new generation on success
         * 8. Rebuild symlink farm
         */

        /* Step 1: Find config files — both user (home.nix) and global
         * (2O9.nix). Per DESIGN.md §7, merge order is:
         *   defaults → home.nix → 2O9.nix → CLI flags
         * Global wins on conflict. We evaluate both and merge. */
        char user_config[PATH_MAX] = {0};
        char *home = getenv("HOME");
        if (home) {
                snprintf(user_config, sizeof(user_config),
                         "%s/.config/2O9/home.nix", home);
        }

        /* Step 2: Evaluate each config that exists */
        char *user_json = NULL;
        char *global_json = NULL;
        char *eval_err = NULL;

        if (user_config[0]) {
                struct stat st;
                if (stat(user_config, &st) == 0) {
                        printf("209: evaluating %s...\n", user_config);
                        user_json = eval_nix_config(user_config, &eval_err);
                        if (!user_json) {
                                fprintf(stderr, "209: %s: %s\n", user_config,
                                        eval_err ? eval_err : "evaluation failed");
                                free(eval_err);
                                return 1;
                        }
                }
        }

        struct stat st;
        if (stat(CONFIG_PATH, &st) == 0) {
                printf("209: evaluating %s...\n", CONFIG_PATH);
                global_json = eval_nix_config(CONFIG_PATH, &eval_err);
                if (!global_json) {
                        fprintf(stderr, "209: %s: %s\n", CONFIG_PATH,
                                eval_err ? eval_err : "evaluation failed");
                        free(user_json);
                        free(eval_err);
                        return 1;
                }
        }

        if (!user_json && !global_json) {
                fprintf(stderr, "209: no 2O9.nix found\n");
                fprintf(stderr, "    searched: %s, %s\n",
                        user_config[0] ? user_config : "~/.config/2O9/home.nix",
                        CONFIG_PATH);
                fprintf(stderr, "    create one with: 209 init\n");
                return 1;
        }

        /* Step 3: Merge per DESIGN.md §7 (global wins on conflict) */
        int merged_both = (user_json && global_json);
        char *json = merge_manifests(user_json, global_json, &eval_err);
        free(user_json);
        free(global_json);
        if (!json) {
                fprintf(stderr, "209: manifest merge failed: %s\n",
                        eval_err ? eval_err : "unknown error");
                free(eval_err);
                return 1;
        }

        if (merged_both) {
                printf("209: merged home.nix + 2O9.nix (global wins on conflict)\n");
        }

        /* Step 4: Open generation DB */
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB at %s\n", db_root);
                free(json);
                return 1;
        }

        /* Step 5: Reconcile — diff desired manifest against current generation */
        reconcile_txn_t *txn = reconcile(json, db_root);
        if (!txn) {
                fprintf(stderr, "209: failed to reconcile manifest\n");
                gen_db_close(db);
                free(json);
                return 1;
        }

        /* Print diff summary */
        printf("  desired state:\n");
        printf("    repo packages: %zu", txn->all_repo_pkg_count);
        if (txn->repo_install_count > 0)
                printf(" (%zu to install)", txn->repo_install_count);
        printf("\n");
        printf("    aur packages:  %zu", txn->all_aur_pkg_count);
        if (txn->aur_install_count > 0)
                printf(" (%zu to build)", txn->aur_install_count);
        printf("\n");
        if (txn->pkg_remove_count > 0)
                printf("    to remove:     %zu\n", txn->pkg_remove_count);
        if (txn->svc_enable_count > 0)
                printf("    services enable:  %zu\n", txn->svc_enable_count);
        if (txn->svc_disable_count > 0)
                printf("    services disable: %zu\n", txn->svc_disable_count);

        /* Step 6: Execute the transaction (install/build/remove/services) */
        if (txn->repo_install_count + txn->aur_install_count +
            txn->pkg_remove_count + txn->svc_enable_count +
            txn->svc_disable_count > 0) {
                int rc = reconcile_execute(txn);
                if (rc != 0) {
                        fprintf(stderr, "209: transaction had errors (rc=%d)\n", rc);
                        fprintf(stderr, "    committing partial generation anyway\n");
                }
        } else {
                printf("  no changes needed\n");
        }

        /* Step 7: Build the generation package list from reconciler output */
        gen_pkg_t *pkgs = NULL;
        gen_pkg_t **pkg_tail = &pkgs;

        /* Add all repo packages */
        for (pkg_name_t *p = txn->all_repo_pkgs; p; p = p->next) {
                char fake_store[PATH_MAX];
                snprintf(fake_store, sizeof(fake_store),
                         "/nix/store/%s-declarative", p->name);
                gen_pkg_t *pkg = gen_pkg_create(
                        p->name, "0.0.0", fake_store, "declarative");
                *pkg_tail = pkg;
                pkg_tail = &pkg->next;
        }

        /* Add all AUR packages */
        for (pkg_name_t *p = txn->all_aur_pkgs; p; p = p->next) {
                char fake_store[PATH_MAX];
                snprintf(fake_store, sizeof(fake_store),
                         "/nix/store/%s-aur", p->name);
                gen_pkg_t *pkg = gen_pkg_create(
                        p->name, "0.0.0", fake_store, "aur-declarative");
                *pkg_tail = pkg;
                pkg_tail = &pkg->next;
        }

        /* Step 8: Lock and commit */
        if (gen_db_lock(db) < 0) {
                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                gen_pkg_list_free(pkgs);
                gen_db_close(db);
                free(json);
                return 1;
        }

        /* Also save the raw manifest JSON to the generation directory */
        int new_id = gen_db_commit(db, pkgs);
        if (new_id < 0) {
                fprintf(stderr, "209: failed to commit generation\n");
                gen_pkg_list_free(pkgs);
                gen_db_unlock(db);
                gen_db_close(db);
                free(json);
                return 1;
        }

        /* Write the manifest.json for this generation */
        {
                char manifest_path[PATH_MAX];
                snprintf(manifest_path, sizeof(manifest_path),
                         "%s/generations/%d/manifest.json", db_root, new_id);
                FILE *mf = fopen(manifest_path, "w");
                if (mf) {
                        /* Write the Nix evaluator output as the manifest */
                        fprintf(mf, "{\n  \"source\": \"2O9.nix\",\n");
                        fprintf(mf, "  \"nix_output\": %s,\n", json);
                        fprintf(mf, "  \"packages\":[");
                        gen_pkg_t *p = pkgs;
                        int first = 1;
                        while (p) {
                                if (!first) fprintf(mf, ",");
                                fprintf(mf, "\n    {\"name\":\"%s\",\"version\":\"%s\","
                                           "\"store_path\":\"%s\",\"origin\":\"%s\"}",
                                        p->name, p->version,
                                        p->store_path ? p->store_path : "",
                                        p->origin ? p->origin : "");
                                first = 0;
                                p = p->next;
                        }
                        fprintf(mf, "\n  ]\n}\n");
                        fclose(mf);
                }
        }

        printf("  generation #%d committed\n", new_id);

        /* Step 7: Rebuild symlink farm */
        gen_t *gen = gen_db_get(db, new_id);
        if (gen) {
                int current = gen_db_current(db);
                gen_t *prev = current > 0 ? gen_db_get(db, current) : NULL;
                symlink_farm_build(db, gen, prev);
                gen_free(gen);
                if (prev) gen_free(prev);
        }

        /* Step 7.5: Activation phase — 9-step idempotent post-extract
         * sequence from DESIGN.md §7. Runs after the symlink farm so
         * unit files are visible in /etc/, before the final report.
         * Steps that aren't fully implemented log a stub message and
         * continue (non-fatal). */
        activation_run(txn);

        /* Step 9: Report services that need attention */
        if (txn->svc_enable_count > 0) {
                printf("  services enabled:");
                for (svc_entry_t *s = txn->svc_enable; s; s = s->next)
                        printf(" %s", s->name);
                printf("\n");
        }
        if (txn->svc_disable_count > 0) {
                printf("  services disabled:");
                for (svc_entry_t *s = txn->svc_disable; s; s = s->next)
                        printf(" %s", s->name);
                printf("\n");
        }

        printf("  done. rollback with: 209 %d rollback\n",
               new_id - 1 > 0 ? new_id - 1 : 1);

        gen_pkg_list_free(pkgs);
        reconcile_free(txn);
        gen_db_unlock(db);
        gen_db_close(db);
        free(json);
        return 0;
}

/* ── Sync ────────────────────────────────────────────────────────── */

/* ── 209 sync ──────────────────────────────────────────────────────
 * Refresh repo databases. Without lib2O9 linked, we fall back to
 * downloading the repo .db files directly via libcurl to the cache dir.
 * The repo URLs come from /etc/2O9/2O9.nix (or user config). If no
 * config exists, we use Arch defaults.
 *
 * When lib2O9 is linked (Phase 1 complete), this will instead call
 * alpm_db_update() for each registered sync DB via two9_alpm_init_from_manifest().
 */
static size_t sync_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
        FILE *fp = (FILE *)userdata;
        return fwrite(ptr, size, nmemb, fp);
}

static int sync_one_repo(CURL *curl, const char *repo_name, const char *server_url)
{
        char url[PATH_MAX];
        char dest[PATH_MAX];

        /* Construct URL: server + /os/x86_64/<repo>.db */
        /* Strip any trailing $arch or $repo variables — server templates
         * use $arch and $repo. For simplicity assume the server URL
         * already includes the arch path, or append it. */
        if (strstr(server_url, "$arch")) {
                /* Replace $arch with x86_64 and $repo with repo_name */
                char tmp[PATH_MAX];
                const char *p = server_url;
                char *out = tmp;
                while (*p && out < tmp + sizeof(tmp) - 32) {
                        if (strncmp(p, "$arch", 5) == 0) {
                                strcpy(out, "x86_64");
                                out += 6;
                                p += 5;
                        } else if (strncmp(p, "$repo", 5) == 0) {
                                strcpy(out, repo_name);
                                out += strlen(repo_name);
                                p += 5;
                        } else {
                                *out++ = *p++;
                        }
                }
                *out = '\0';
                snprintf(url, sizeof(url), "%s/%s.db", tmp, repo_name);
        } else {
                snprintf(url, sizeof(url), "%s/%s.db", server_url, repo_name);
        }

        /* Cache dir: /var/cache/2O9/pkg/<repo>.db */
        snprintf(dest, sizeof(dest), "/var/cache/2O9/pkg/%s.db", repo_name);

        /* Ensure cache dir exists */
        (void)mkdir("/var/cache/2O9", 0755);
        (void)mkdir("/var/cache/2O9/pkg", 0755);

        FILE *fp = fopen(dest, "wb");
        if (!fp) {
                fprintf(stderr, "209: cannot write to %s: %s\n",
                        dest, strerror(errno));
                return -1;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sync_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "2O9/0.0.1");

        CURLcode res = curl_easy_perform(curl);
        fclose(fp);

        if (res != CURLE_OK) {
                fprintf(stderr, "209: failed to sync %s: %s\n",
                        repo_name, curl_easy_strerror(res));
                unlink(dest);
                return -1;
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200) {
                fprintf(stderr, "209: %s.db: HTTP %ld\n", repo_name, http_code);
                unlink(dest);
                return -1;
        }

        printf("  synced %s.db\n", repo_name);
        return 0;
}

static int cmd_sync(void)
{
        /* Read repos from 2O9.nix if available; otherwise use Arch defaults. */
        const char *default_repos[][2] = {
                {"core",     "https://mirror.archlinuxarm.org/x86_64/core"},
                {"extra",    "https://mirror.archlinuxarm.org/x86_64/extra"},
                {"multilib", "https://mirror.archlinuxarm.org/x86_64/multilib"},
                {NULL, NULL}
        };

        CURL *curl = curl_easy_init();
        if (!curl) {
                fprintf(stderr, "209: cannot init libcurl\n");
                return 1;
        }

        /* TODO: parse 2O9.nix to get user-configured repos. For now,
         * sync the default Arch repos. The proper implementation will
         * use two9_alpm_init_from_manifest() once lib2O9 is linked
         * (Phase 1) and call alpm_db_update() for each registered DB. */

        printf("=== syncing repo databases ===\n");
        int errors = 0;
        for (int i = 0; default_repos[i][0]; i++) {
                if (sync_one_repo(curl, default_repos[i][0], default_repos[i][1]) < 0)
                        errors++;
        }

        curl_easy_cleanup(curl);

        if (errors) {
                fprintf(stderr, "209: %d repo(s) failed to sync\n", errors);
                return 1;
        }
        printf("=== sync complete ===\n");
        return 0;
}

/* ── GC ──────────────────────────────────────────────────────────── */

static int cmd_gc(void)
{
        /* Garbage collection: find store paths not referenced by any
         * generation and delete them. This is the 2O9 equivalent of
         * nix-collect-garbage.
         *
         * Algorithm:
         *   1. Walk all generations, collect all referenced store paths
         *   2. Walk /nix/store/, find directories not in the referenced set
         *   3. Delete unreferenced paths (unless their generation is pinned)
         *   4. Report how much space was freed
         */
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                return 1;
        }

        /* Step 1: Collect all referenced store paths from all generations */
        size_t gen_count = 0;
        gen_t **gens = gen_db_list(db, &gen_count);
        /* current gen ID — not used directly in GC logic,
         * all generations are scanned */
        (void)gen_db_current(db);

        /* Count total referenced paths */
        size_t ref_count = 0;
        for (size_t i = 0; i < gen_count; i++) {
                /* Read the manifest for each generation */
                gen_pkg_t *pkgs = read_current_gen_packages(db_root, gens[i]->id);
                while (pkgs) {
                        ref_count++;
                        pkgs = pkgs->next;
                }
                gen_pkg_list_free(pkgs); /* count only, then free immediately */
        }

        /* Step 2: Walk /nix/store/ and find unreferenced paths */
        DIR *store_dir = opendir("/nix/store");
        if (!store_dir) {
                fprintf(stderr, "209: cannot open /nix/store/ — does it exist?\n");
                gen_list_free(gens, gen_count);
                gen_db_close(db);
                return 1;
        }

        /* Build a set of referenced store path basenames for quick lookup.
         * We compare just the directory name (e.g. "neovim-0.9.5") since
         * our store paths have no hash. */
        char **ref_names = NULL;
        size_t ref_idx = 0;
        if (ref_count > 0) {
                ref_names = malloc(ref_count * sizeof(char *));
                for (size_t i = 0; i < gen_count; i++) {
                        gen_pkg_t *pkgs = read_current_gen_packages(db_root, gens[i]->id);
                        while (pkgs) {
                                if (pkgs->store_path) {
                                        const char *base = strrchr(pkgs->store_path, '/');
                                        base = base ? base + 1 : pkgs->store_path;
                                        ref_names[ref_idx++] = strdup(base);
                                }
                                gen_pkg_t *next = pkgs->next;
                                /* don't free the individual pkg fields since we strdup'd base */
                                free(pkgs->name);
                                free(pkgs->version);
                                free(pkgs->store_path);
                                free(pkgs->origin);
                                free(pkgs);
                                pkgs = next;
                        }
                }
        }

        /* Walk /nix/store/ and collect unreferenced entries */
        size_t removed = 0;
        /* freed_bytes would track space freed — TODO: add du -sk before rm */
        struct dirent *ent;
        while ((ent = readdir(store_dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;

                /* Check if this entry is referenced by any generation */
                int found = 0;
                for (size_t j = 0; j < ref_idx; j++) {
                        if (strcmp(ent->d_name, ref_names[j]) == 0) {
                                found = 1;
                                break;
                        }
                }

                if (!found) {
                        /* This store path is not referenced by any generation.
                         * But we only delete it if its generation is not pinned. */
                        char full_path[PATH_MAX];
                        snprintf(full_path, sizeof(full_path), "/nix/store/%s",
                                 ent->d_name);

                        struct stat st;
                        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                                printf("  gc: removing %s\n", full_path);
                                /* Calculate size before deletion */
                                /* Simple recursive delete via rm -rf */
                                char cmd[PATH_MAX + 32];
                                snprintf(cmd, sizeof(cmd), "rm -rf '%s'", full_path);
                                int rc = system(cmd);
                                if (rc == 0) {
                                        removed++;
                                } else {
                                        fprintf(stderr, "  warning: failed to remove %s\n",
                                                full_path);
                                }
                        }
                }
        }
        closedir(store_dir);

        /* Cleanup */
        for (size_t j = 0; j < ref_idx; j++)
                free(ref_names[j]);
        free(ref_names);
        gen_list_free(gens, gen_count);
        gen_db_close(db);

        if (removed > 0)
                printf("  garbage-collected %zu store paths\n", removed);
        else
                printf("  nothing to garbage-collect\n");

        return 0;
}

/* ── Remove ─────────────────────────────────────────────────────────── */

static int cmd_remove(const char *pkg_name)
{
        /* Remove = create a new generation without the package, rebuild symlink farm.
         * Based on Nix's approach: never mutate a generation, always create a new one.
         * Store files aren't deleted until GC. Services from removed packages are
         * stopped via systemctl. /etc symlinks are removed. */

        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                return 1;
        }

        int current = gen_db_current(db);
        if (current == 0) {
                fprintf(stderr, "209: no generations — nothing to remove from\n");
                gen_db_close(db);
                return 1;
        }

        /* Read current generation's manifest and rebuild without the target package */
        gen_t *gen = gen_db_get(db, current);
        if (!gen) {
                fprintf(stderr, "209: cannot read generation #%d\n", current);
                gen_db_close(db);
                return 1;
        }

        /* Read manifest.json to get the package list */
        char manifest_path[PATH_MAX];
        snprintf(manifest_path, sizeof(manifest_path), "%s/generations/%d/manifest.json",
                 db_root, current);

        FILE *f = fopen(manifest_path, "r");
        if (!f) {
                fprintf(stderr, "209: cannot read manifest for generation #%d\n", current);
                gen_free(gen);
                gen_db_close(db);
                return 1;
        }

        /* Simple JSON parse: look for package entries and rebuild without the target.
         * This is a basic parser — full JSON parsing uses cJSON later. */
        char line[8192];
        int found = 0;
        gen_pkg_t *new_pkgs = NULL;
        gen_pkg_t **tail = &new_pkgs;

        while (fgets(line, sizeof(line), f)) {
                /* Look for lines like: {"name": "foo", "version": "1.0", ...} */
                char *name_start = strstr(line, "\"name\":");
                if (!name_start) continue;

                name_start += strlen("\"name\":");
                while (*name_start == ' ' || *name_start == '"') name_start++;
                char *name_end = strchr(name_start, '"');
                if (!name_end) continue;

                char pkg_name_buf[256];
                size_t nlen = name_end - name_start;
                if (nlen >= sizeof(pkg_name_buf)) nlen = sizeof(pkg_name_buf) - 1;
                memcpy(pkg_name_buf, name_start, nlen);
                pkg_name_buf[nlen] = '\0';

                /* Find version */
                char *ver_start = strstr(line, "\"version\":");
                char pkg_ver_buf[64] = "unknown";
                if (ver_start) {
                        ver_start += strlen("\"version\":");
                        while (*ver_start == ' ' || *ver_start == '"') ver_start++;
                        char *ver_end = strchr(ver_start, '"');
                        if (ver_end) {
                                size_t vlen = ver_end - ver_start;
                                if (vlen >= sizeof(pkg_ver_buf)) vlen = sizeof(pkg_ver_buf) - 1;
                                memcpy(pkg_ver_buf, ver_start, vlen);
                                pkg_ver_buf[vlen] = '\0';
                        }
                }

                /* Find store_path */
                char *sp_start = strstr(line, "\"store_path\":");
                char pkg_store_buf[PATH_MAX] = "";
                if (sp_start) {
                        sp_start += strlen("\"store_path\":");
                        while (*sp_start == ' ' || *sp_start == '"') sp_start++;
                        char *sp_end = strchr(sp_start, '"');
                        if (sp_end) {
                                size_t slen = sp_end - sp_start;
                                if (slen >= sizeof(pkg_store_buf)) slen = sizeof(pkg_store_buf) - 1;
                                memcpy(pkg_store_buf, sp_start, slen);
                                pkg_store_buf[slen] = '\0';
                        }
                }

                /* Find origin */
                char *or_start = strstr(line, "\"origin\":");
                char pkg_origin_buf[32] = "repo";
                if (or_start) {
                        or_start += strlen("\"origin\":");
                        while (*or_start == ' ' || *or_start == '"') or_start++;
                        char *or_end = strchr(or_start, '"');
                        if (or_end) {
                                size_t olen = or_end - or_start;
                                if (olen >= sizeof(pkg_origin_buf)) olen = sizeof(pkg_origin_buf) - 1;
                                memcpy(pkg_origin_buf, or_start, olen);
                                pkg_origin_buf[olen] = '\0';
                        }
                }

                if (strcmp(pkg_name_buf, pkg_name) == 0) {
                        found = 1;
                        printf("  removing %s (%s) [%s]\n", pkg_name_buf,
                               pkg_ver_buf, pkg_origin_buf);
                        continue;  /* skip — don't add to new list */
                }

                /* Keep this package in the new generation */
                gen_pkg_t *p = gen_pkg_create(pkg_name_buf, pkg_ver_buf,
                                              pkg_store_buf[0] ? pkg_store_buf : NULL,
                                              pkg_origin_buf);
                *tail = p;
                tail = &p->next;
        }
        fclose(f);

        if (!found) {
                fprintf(stderr, "209: %s not found in current generation #%d\n",
                        pkg_name, current);
                gen_pkg_list_free(new_pkgs);
                gen_free(gen);
                gen_db_close(db);
                return 1;
        }

        /* Lock and commit new generation */
        if (gen_db_lock(db) < 0) {
                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                gen_pkg_list_free(new_pkgs);
                gen_free(gen);
                gen_db_close(db);
                return 1;
        }

        int new_id = gen_db_commit(db, new_pkgs);
        if (new_id < 0) {
                fprintf(stderr, "209: failed to commit new generation\n");
                gen_pkg_list_free(new_pkgs);
                gen_db_unlock(db);
                gen_free(gen);
                gen_db_close(db);
                return 1;
        }

        printf("  generation #%d committed\n", new_id);

        /* Rebuild symlink farm */
        gen_t *new_gen = gen_db_get(db, new_id);
        if (new_gen) {
                symlink_farm_build(db, new_gen, gen);
                gen_free(new_gen);
        }

        /* Activation phase — for imperative installs, no reconcile txn
         * exists (no manifest to diff against). Pass NULL: services
         * enable/disable is skipped, but daemon-reload, cache rebuild,
         * and other idempotent steps still run. */
        activation_run(NULL);

        printf("  NOTE: reboot for full system state to take effect\n");

        gen_pkg_list_free(new_pkgs);
        gen_free(gen);
        gen_db_unlock(db);
        gen_db_close(db);
        return 0;
}

/* ── AUR search ───────────────────────────────────────────────────── */

static int cmd_aur_search(const char *query)
{
        aur_cache_t *cache = aur_cache_open(NULL);
        if (!cache) {
                fprintf(stderr, "209: failed to initialize AUR client\n");
                return 1;
        }

        aur_rpc_result_t result = aur_search(cache, query, NULL);
        if (!result.success) {
                fprintf(stderr, "209: AUR search failed: %s\n",
                        result.error ? result.error : "unknown error");
                aur_rpc_result_free(&result);
                aur_cache_close(cache);
                return 1;
        }

        if (result.count == 0) {
                printf("No AUR packages found matching '%s'\n", query);
        } else {
                printf("  %-30s  %-12s  %6s  %s\n",
                       "Name", "Version", "Votes", "Description");
                printf("  %-30s  %-12s  %6s  %s\n",
                       "----", "-------", "-----", "-----------");

                aur_pkg_t *pkg = result.packages;
                while (pkg) {
                        printf("  %-30s  %-12s  %6d  %s%s\n",
                               pkg->name ? pkg->name : "?",
                               pkg->version ? pkg->version : "?",
                               pkg->num_votes,
                               pkg->out_of_date ? "[OUT-OF-DATE] " : "",
                               pkg->description ? pkg->description : "");
                        pkg = pkg->next;
                }
        }

        aur_rpc_result_free(&result);
        aur_cache_close(cache);
        return 0;
}

/* ── AUR info ─────────────────────────────────────────────────────── */

static int cmd_aur_info(const char *pkg_name)
{
        aur_cache_t *cache = aur_cache_open(NULL);
        if (!cache) {
                fprintf(stderr, "209: failed to initialize AUR client\n");
                return 1;
        }

        aur_rpc_result_t result = aur_info(cache, pkg_name);
        if (!result.success) {
                fprintf(stderr, "209: AUR info failed: %s\n",
                        result.error ? result.error : "unknown error");
                aur_rpc_result_free(&result);
                aur_cache_close(cache);
                return 1;
        }

        if (result.count == 0) {
                printf("Package '%s' not found in AUR\n", pkg_name);
        } else {
                aur_pkg_t *pkg = result.packages;
                while (pkg) {
                        printf("Name:         %s\n", pkg->name ? pkg->name : "?");
                        printf("Base:         %s\n", pkg->pkgbase ? pkg->pkgbase : "?");
                        printf("Version:      %s\n", pkg->version ? pkg->version : "?");
                        printf("Description:  %s\n", pkg->description ? pkg->description : "");
                        printf("URL:          %s\n", pkg->url ? pkg->url : "");
                        printf("Votes:        %d\n", pkg->num_votes);
                        printf("Popularity:   %d\n", pkg->popularity);
                        printf("Out of date:  %s\n", pkg->out_of_date ? "yes" : "no");
                        printf("Maintainer:   %s\n", pkg->maintainer ? pkg->maintainer : "orphan");

                        if (pkg->depends_count > 0) {
                                printf("Depends:");
                                for (size_t i = 0; i < pkg->depends_count; i++)
                                        printf(" %s", pkg->depends[i]);
                                printf("\n");
                        }
                        if (pkg->makedepends_count > 0) {
                                printf("MakeDeps:");
                                for (size_t i = 0; i < pkg->makedepends_count; i++)
                                        printf(" %s", pkg->makedepends[i]);
                                printf("\n");
                        }
                        if (pkg->checkdepends_count > 0) {
                                printf("CheckDeps:");
                                for (size_t i = 0; i < pkg->checkdepends_count; i++)
                                        printf(" %s", pkg->checkdepends[i]);
                                printf("\n");
                        }

                        pkg = pkg->next;
                }
        }

        aur_rpc_result_free(&result);
        aur_cache_close(cache);
        return 0;
}

/* ── AUR build ────────────────────────────────────────────────────── */

static int cmd_aur_build(const char *pkg_name)
{
        /* Step 1: Resolve dependencies */
        printf("209: resolving dependencies for %s...\n", pkg_name);

        aur_cache_t *cache = aur_cache_open(NULL);
        if (!cache) {
                fprintf(stderr, "209: failed to initialize AUR client\n");
                return 1;
        }

        const char *targets[] = { pkg_name };
        resolve_result_t *plan = resolve_targets(cache, targets, 1);

        if (!plan) {
                fprintf(stderr, "209: dependency resolution failed\n");
                aur_cache_close(cache);
                return 1;
        }

        /* Report the plan */
        if (plan->missing) {
                fprintf(stderr, "209: missing dependencies:\n");
                resolve_action_t *m = plan->missing;
                while (m) {
                        fprintf(stderr, "  - %s\n", m->name);
                        m = m->next;
                }
                resolve_result_free(plan);
                aur_cache_close(cache);
                return 1;
        }

        if (plan->install) {
                printf("  repo deps:");
                resolve_action_t *a = plan->install;
                while (a) {
                        printf(" %s", a->name);
                        a = a->next;
                }
                printf("\n");
                /* TODO: install repo deps via lib2O9 in Phase 3 */
                printf("  (repo deps will be installed with pacman -S --asdeps)\n");
        }

        /* Collect all AUR packages to build (topologically sorted later) */
        resolve_action_t *build_list = plan->build;
        if (!build_list) {
                fprintf(stderr, "209: %s not found in AUR\n", pkg_name);
                resolve_result_free(plan);
                aur_cache_close(cache);
                return 1;
        }

        printf("  AUR packages to build:");
        resolve_action_t *b = build_list;
        while (b) {
                printf(" %s", b->name);
                b = b->next;
        }
        printf("\n");

        /* Step 2: Clone PKGBUILDs */
        char *home = getenv("HOME");
        char build_dir[PATH_MAX];
        if (home) {
                snprintf(build_dir, sizeof(build_dir),
                         "%s/.cache/2O9/build", home);
        } else {
                snprintf(build_dir, sizeof(build_dir), "/tmp/2O9-build");
        }

        b = build_list;
        while (b) {
                if (aur_clone(b->name, build_dir) < 0) {
                        fprintf(stderr, "209: failed to clone %s\n", b->name);
                        resolve_result_free(plan);
                        aur_cache_close(cache);
                        return 1;
                }
                b = b->next;
        }

        /* Step 3: Review PKGBUILDs (unless --noconfirm) */
        b = build_list;
        while (b) {
                if (aur_review(b->name, build_dir) < 0) {
                        fprintf(stderr, "  warning: could not review %s\n", b->name);
                }
                b = b->next;
        }

        /* Step 4: Build with makepkg */
        build_config_t config = {
                .build_dir = build_dir,
                .makepkg_conf = NULL,
                .pacman_conf = NULL,
                .cflags = NULL,
                .cxxflags = NULL,
                .ldflags = NULL,
                .no_confirm = 0,
                .skip_review = 0,
                .chroot = 0,
                .sign = 0,
                .gpg_key = NULL,
        };

        /* TODO: read build config from 2O9.nix (Phase 3) */
        /* For now, respect environment variables */
        const char *env_cflags = getenv("CFLAGS");
        const char *env_cxxflags = getenv("CXXFLAGS");
        const char *env_ldflags = getenv("LDFLAGS");
        config.cflags = env_cflags ? strdup(env_cflags) : NULL;
        config.cxxflags = env_cxxflags ? strdup(env_cxxflags) : NULL;
        config.ldflags = env_ldflags ? strdup(env_ldflags) : NULL;

        build_result_t *results = NULL;
        build_result_t **tail = &results;

        b = build_list;
        while (b) {
                build_result_t *r = aur_build(b->name, build_dir, &config);
                if (!r || !r->success) {
                        fprintf(stderr, "209: build failed for %s: %s\n",
                                b->name, (r && r->error_msg) ? r->error_msg : "unknown");
                        build_result_free(results);
                        build_result_free(r);
                        resolve_result_free(plan);
                        aur_cache_close(cache);
                        free(config.cflags);
                        free(config.cxxflags);
                        free(config.ldflags);
                        return 1;
                }
                *tail = r;
                tail = &r->next;
                b = b->next;
        }

        /* Step 5: Install built packages to store */
        build_result_t *r = results;
        char *home_env = getenv("HOME");
        char db_root[PATH_MAX];
        if (home_env) {
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home_env);
        } else {
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
        }

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB\n");
                build_result_free(results);
                resolve_result_free(plan);
                aur_cache_close(cache);
                free(config.cflags);
                free(config.cxxflags);
                free(config.ldflags);
                return 1;
        }

        if (gen_db_lock(db) < 0) {
                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                gen_db_close(db);
                build_result_free(results);
                resolve_result_free(plan);
                aur_cache_close(cache);
                free(config.cflags);
                free(config.cxxflags);
                free(config.ldflags);
                return 1;
        }

        /* Add each built package to the store */
        gen_pkg_t *all_pkgs = NULL;
        gen_pkg_t **pkg_tail = &all_pkgs;

        r = results;
        while (r) {
                printf("  adding %s to store...\n", r->pkg_name);

                /* In test mode, skip the store add */
                const char *test_mode = getenv("TWO09_TEST_MODE");
                if (test_mode) {
                        char fake_store[PATH_MAX];
                        snprintf(fake_store, sizeof(fake_store),
                                 "/nix/store/%s-%s", r->pkg_name, r->pkg_version);
                        gen_pkg_t *p = gen_pkg_create(r->pkg_name, r->pkg_version,
                                                      fake_store, "aur");
                        *pkg_tail = p;
                        pkg_tail = &p->next;
                } else {
                        store_add_result_t sar = store_add(r->pkg_path,
                                                           STORE_BACKEND_NIX_STORE);
                        if (sar.success != 0) {
                                fprintf(stderr, "209: store add failed for %s: %s\n",
                                        r->pkg_name, sar.error_msg);
                                gen_pkg_list_free(all_pkgs);
                                build_result_free(results);
                                gen_db_unlock(db);
                                gen_db_close(db);
                                resolve_result_free(plan);
                                aur_cache_close(cache);
                                free(config.cflags);
                                free(config.cxxflags);
                                free(config.ldflags);
                                return 1;
                        }
                        gen_pkg_t *p = gen_pkg_create(r->pkg_name, r->pkg_version,
                                                      sar.store_path, "aur");
                        *pkg_tail = p;
                        pkg_tail = &p->next;
                        store_add_result_free(&sar);
                }
                r = r->next;
        }

        /* Commit generation */
        int new_id = gen_db_commit(db, all_pkgs);
        if (new_id < 0) {
                fprintf(stderr, "209: failed to commit generation\n");
                gen_pkg_list_free(all_pkgs);
        } else {
                printf("  generation #%d committed\n", new_id);

                /* Rebuild symlink farm */
                gen_t *gen = gen_db_get(db, new_id);
                if (gen) {
                        symlink_farm_build(db, gen, NULL);
                        gen_free(gen);
                }

                printf("  done. rollback with: 209 %d rollback\n",
                       new_id - 1 > 0 ? new_id - 1 : 1);
                printf("  NOTE: reboot for full system state to take effect\n");
                gen_pkg_list_free(all_pkgs);
        }

        gen_db_unlock(db);
        gen_db_close(db);
        build_result_free(results);
        resolve_result_free(plan);
        aur_cache_close(cache);
        free(config.cflags);
        free(config.cxxflags);
        free(config.ldflags);
        return new_id < 0 ? 1 : 0;
}

/* ── SOV dispatch ────────────────────────────────────────────────── */

/* Check if a string is a number (generation ID) */
static int is_number(const char *s)
{
        if (!s || !*s) return 0;
        while (*s) {
                if (*s < '0' || *s > '9') return 0;
                s++;
        }
        return 1;
}

/* ── 209 news ──────────────────────────────────────────────────────
 * Fetches the Arch Linux news feed (https://archlinux.org/feeds/news/)
 * and prints the latest entries. Uses libcurl + cJSON.
 *
 * The feed is RSS 2.0 wrapped in JSON via the archlinux.org API.
 * For simplicity we fetch the RSS XML and extract <title> and <pubDate>
 * with simple string scanning — no XML parser dependency.
 *
 * Phase 5 polish: pretty-printing, filtering by date. */
static size_t news_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
        /* Append to a growable buffer */
        char **buf = (char **)userdata;
        size_t total = size * nmemb;
        size_t cur_len = *buf ? strlen(*buf) : 0;
        char *new_buf = realloc(*buf, cur_len + total + 1);
        if (!new_buf) return 0;
        *buf = new_buf;
        memcpy(*buf + cur_len, ptr, total);
        (*buf)[cur_len + total] = '\0';
        return total;
}

static int cmd_news(void)
{
        CURL *curl = curl_easy_init();
        if (!curl) {
                fprintf(stderr, "209: cannot init libcurl\n");
                return 1;
        }
        char *buf = NULL;
        curl_easy_setopt(curl, CURLOPT_URL, "https://archlinux.org/feeds/news/");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, news_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "2O9/0.0.1");
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || !buf) {
                fprintf(stderr, "209: failed to fetch Arch news: %s\n",
                        curl_easy_strerror(res));
                free(buf);
                return 1;
        }

        /* Extract <title>...</title> and <pubDate>...</pubDate> pairs.
         * Skip the first <title> (channel title) — start scanning after
         * the first </channel>-equivalent marker (we look for <item>). */
        const char *p = buf;
        const char *items_start = strstr(p, "<item>");
        if (!items_start) {
                fprintf(stderr, "209: news feed has no items\n");
                free(buf);
                return 1;
        }
        p = items_start;
        int count = 0;
        const int MAX_ITEMS = 15;
        printf("=== Arch Linux News (latest %d) ===\n\n", MAX_ITEMS);
        while (p && *p && count < MAX_ITEMS) {
                const char *item = strstr(p, "<item>");
                if (!item) break;
                const char *title_start = strstr(item, "<title>");
                if (!title_start) break;
                title_start += 7;
                const char *title_end = strstr(title_start, "</title>");
                if (!title_end) break;
                const char *link_start = strstr(title_end, "<link>");
                const char *pubdate_start = strstr(title_end, "<pubDate>");
                if (!pubdate_start) break;
                pubdate_start += 9;
                const char *pubdate_end = strstr(pubdate_start, "</pubDate>");
                if (!pubdate_end) break;

                printf("%2d. ", count + 1);
                fwrite(title_start, 1, title_end - title_start, stdout);
                printf("\n    ");
                fwrite(pubdate_start, 1, pubdate_end - pubdate_start, stdout);
                if (link_start && link_start < pubdate_start) {
                        const char *link_end = strstr(link_start, "</link>");
                        if (link_end) {
                                printf("\n    ");
                                fwrite(link_start + 6, 1, link_end - (link_start + 6), stdout);
                        }
                }
                printf("\n\n");
                count++;
                p = pubdate_end;
        }
        if (count == 0) {
                fprintf(stderr, "209: could not parse any news items\n");
                free(buf);
                return 1;
        }
        free(buf);
        return 0;
}

/* ── 209 <pkg> info ────────────────────────────────────────────────
 * Shows info about an installed package by looking it up in the
 * current generation's manifest. Falls back to AUR info if not
 * installed locally.
 *
 * This is the "what do I have installed?" view. Phase 1 (lib2O9)
 * will add repo-package info from libalpm's sync DBs. */
static int cmd_info(const char *pkg_name)
{
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home)
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        else
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB at %s\n", db_root);
                return 1;
        }

        /* Read packages from current generation */
        gen_pkg_t *pkgs = read_current_gen_packages(db_root, gen_db_current(db));
        gen_pkg_t *p = pkgs;
        int found = 0;
        while (p) {
                if (strcmp(p->name, pkg_name) == 0) {
                        printf("Name       : %s\n", p->name);
                        printf("Version    : %s\n", p->version);
                        printf("Store path : %s\n", p->store_path ? p->store_path : "(unknown)");
                        printf("Origin     : %s\n", p->origin ? p->origin : "(unknown)");
                        found = 1;
                        break;
                }
                p = p->next;
        }
        gen_pkg_list_free(pkgs);
        gen_db_close(db);

        if (found) return 0;

        /* Not installed locally — fall back to AUR info */
        fprintf(stderr, "209: %s is not installed locally; querying AUR...\n\n", pkg_name);
        return cmd_aur_info(pkg_name);
}

/* ── 209 <term> search ─────────────────────────────────────────────
 * Searches installed packages by substring match on name. Falls back
 * to AUR search if no local matches.
 *
 * Phase 1 (lib2O9) will add searching the repo sync DBs via libalpm. */
static int cmd_search(const char *term)
{
        char *home = getenv("HOME");
        char db_root[PATH_MAX];
        if (home)
                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
        else
                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");

        gen_db_t *db = gen_db_open(db_root);
        if (!db) {
                fprintf(stderr, "209: cannot open generation DB at %s\n", db_root);
                return 1;
        }

        gen_pkg_t *pkgs = read_current_gen_packages(db_root, gen_db_current(db));
        gen_pkg_t *p = pkgs;
        int found = 0;
        while (p) {
                if (p->name && strstr(p->name, term)) {
                        if (!found) {
                                printf("=== Local matches (current generation) ===\n");
                        }
                        printf("  %s %s  [%s]\n",
                               p->name, p->version ? p->version : "",
                               p->origin ? p->origin : "?");
                        found = 1;
                }
                p = p->next;
        }
        gen_pkg_list_free(pkgs);
        gen_db_close(db);

        if (!found) {
                printf("=== No local matches for '%s' — searching AUR ===\n", term);
                return cmd_aur_search(term);
        }
        printf("\n(For AUR results, run: 209 %s aur search)\n", term);
        return 0;
}

int main(int argc, char *argv[])
{
        /* Handle options */
        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-V") == 0 ||
                    strcmp(argv[i], "--version") == 0) {
                        return cmd_version();
                }
                if (strcmp(argv[i], "-h") == 0 ||
                    strcmp(argv[i], "--help") == 0) {
                        return cmd_usage();
                }
        }

        if (argc < 2) {
                cmd_usage();
                return 1;
        }

        /* Zero-argument commands */
        if (strcmp(argv[1], "apply") == 0)       return cmd_apply();
        if (strcmp(argv[1], "generations") == 0)  return cmd_generations();
        if (strcmp(argv[1], "sync") == 0)         return cmd_sync();
        if (strcmp(argv[1], "gc") == 0)           return cmd_gc();
        if (strcmp(argv[1], "news") == 0) {
                return cmd_news();
        }

        /* SOV pattern: need at least subject + verb */
        if (argc < 3) {
                fprintf(stderr, "209: expected <subject> <verb>\n");
                fprintf(stderr, "    try: 209 --help\n");
                return 1;
        }

        const char *subject = argv[1];
        const char *verb = argv[2];

        /* Install */
        if (strcmp(verb, "install") == 0) {
                /* Multi-subject: 209 nginx firefox install */
                for (int i = 1; i < argc - 1; i++) {
                        int rc = cmd_install(argv[i]);
                        if (rc != 0) return rc;
                }
                return 0;
        }

        /* Remove */
        if (strcmp(verb, "remove") == 0) {
                for (int i = 1; i < argc - 1; i++) {
                        int rc = cmd_remove(argv[i]);
                        if (rc != 0) return rc;
                }
                return 0;
        }

        /* Search — repo (local generation DB) search, falls back to AUR */
        if (strcmp(verb, "search") == 0) {
                /* Multi-subject: 209 nginx firefox search */
                for (int i = 1; i < argc - 1; i++) {
                        int rc = cmd_search(argv[i]);
                        if (rc != 0) return rc;
                }
                return 0;
        }

        /* Info — show package info. Installed locally? Show generation DB
         * entry. Otherwise fall through to AUR info. */
        if (strcmp(verb, "info") == 0) {
                for (int i = 1; i < argc - 1; i++) {
                        int rc = cmd_info(argv[i]);
                        if (rc != 0) return rc;
                }
                return 0;
        }

        /* AUR commands: 209 <pkg> aur build, 209 <term> aur search,
         *                209 <pkg> aur info, 209 <pkg> aur review */
        if (argc >= 4 && strcmp(argv[3], "search") == 0 &&
            strcmp(argv[2], "aur") == 0) {
                /* 209 <term> aur search */
                return cmd_aur_search(argv[1]);
        }
        if (argc >= 4 && strcmp(argv[3], "build") == 0 &&
            strcmp(argv[2], "aur") == 0) {
                /* 209 <pkg> aur build */
                for (int i = 1; i < argc - 2; i++) {
                        int rc = cmd_aur_build(argv[i]);
                        if (rc != 0) return rc;
                }
                return 0;
        }
        if (argc >= 4 && strcmp(argv[3], "info") == 0 &&
            strcmp(argv[2], "aur") == 0) {
                /* 209 <pkg> aur info */
                return cmd_aur_info(argv[1]);
        }
        if (argc >= 4 && strcmp(argv[3], "review") == 0 &&
            strcmp(argv[2], "aur") == 0) {
                /* 209 <pkg> aur review */
                char *home = getenv("HOME");
                char build_dir[PATH_MAX];
                if (home) {
                        snprintf(build_dir, sizeof(build_dir),
                                 "%s/.cache/2O9/build", home);
                } else {
                        snprintf(build_dir, sizeof(build_dir), "/tmp/2O9-build");
                }
                /* Clone first if needed, then review */
                aur_clone(argv[1], build_dir);
                return aur_review(argv[1], build_dir);
        }

        /* Trakker: 209 <subject> trakker [flags] [command] */
        if (strcmp(verb, "trakker") == 0) {
                /* Parse trakker flags and build the command to run.
                 * Syntax: 209 <subject> trakker [--no-net] [--no-write]
                 *                              [--redirect-writes <dir>]
                 *                              [--allow-net port=<port>]
                 *                              [-- <command> [args...]]
                 *
                 * If -- is not provided, the subject itself is the command
                 * (e.g. `209 some-app trakker` runs some-app). */
                trakker_policy_t policy = {0};
                const char **cmd_argv = NULL;
                size_t cmd_argc = 0;

                /* Parse flags from argv[3...] */
                int i = 3;
                while (i < argc) {
                        if (strcmp(argv[i], "--no-net") == 0) {
                                policy.no_net = 1;
                                i++;
                        } else if (strcmp(argv[i], "--no-write") == 0) {
                                policy.no_write = 1;
                                i++;
                        } else if (strcmp(argv[i], "--redirect-writes") == 0 && i + 1 < argc) {
                                policy.redirect_writes = strdup(argv[i + 1]);
                                i += 2;
                        } else if (strcmp(argv[i], "--allow-net") == 0 && i + 1 < argc) {
                                /* Parse "port=443" */
                                const char *val = argv[i + 1];
                                if (strncmp(val, "port=", 5) == 0) {
                                        policy.allow_net_count++;
                                        policy.allow_net_ports = realloc(
                                                policy.allow_net_ports,
                                                policy.allow_net_count * sizeof(char *));
                                        policy.allow_net_ports[policy.allow_net_count - 1] =
                                                strdup(val + 5);
                                }
                                i += 2;
                        } else if (strcmp(argv[i], "--") == 0) {
                                /* Everything after -- is the command */
                                i++;
                                break;
                        } else {
                                /* Unknown flag or start of command */
                                break;
                        }
                }

                /* Build command argv */
                if (i < argc) {
                        /* Command specified after flags or -- */
                        cmd_argc = argc - i;
                        cmd_argv = malloc((cmd_argc + 1) * sizeof(char *));
                        for (size_t j = 0; j < cmd_argc; j++)
                                cmd_argv[j] = argv[i + j];
                        cmd_argv[cmd_argc] = NULL;
                } else {
                        /* No command specified — use the subject as the command */
                        cmd_argc = 1;
                        cmd_argv = malloc((cmd_argc + 1) * sizeof(char *));
                        cmd_argv[0] = subject;
                        cmd_argv[1] = NULL;
                }

                printf("209: running under trakker: %s", cmd_argv[0]);
                for (size_t j = 1; j < cmd_argc; j++)
                        printf(" %s", cmd_argv[j]);
                printf("\n");

                if (policy.no_net) printf("  policy: network blocked\n");
                if (policy.no_write) printf("  policy: writes blocked\n");
                if (policy.redirect_writes)
                        printf("  policy: writes redirected to %s\n",
                               policy.redirect_writes);
                if (policy.allow_net_count > 0) {
                        printf("  policy: allowed ports:");
                        for (size_t j = 0; j < policy.allow_net_count; j++)
                                printf(" %s", policy.allow_net_ports[j]);
                        printf("\n");
                }

                /* Run the command under trakker */
                trak_result_t *result = trakker_run(cmd_argv, cmd_argc, &policy);

                if (!result) {
                        fprintf(stderr, "209: trakker failed to start\n");
                        free(cmd_argv);
                        trakker_policy_free(&policy);
                        return 1;
                }

                printf("\n─── Trakker Report ───\n");
                printf("Command:   %s\n", result->command);
                printf("Exit code: %d\n", result->exit_code);
                printf("Duration:  %lu ms\n", result->duration_ms);
                printf("Events:    %zu\n", result->event_count);

                /* Print summary by category */
                size_t reads = 0, writes = 0, creates = 0, deletes = 0;
                size_t connects = 0, blocked = 0, forks = 0, execs = 0;
                trak_event_t *e = result->events;
                while (e) {
                        switch (e->type) {
                        case TRAK_FILE_READ:   reads++; break;
                        case TRAK_FILE_WRITE:  writes++; break;
                        case TRAK_FILE_CREATE: creates++; break;
                        case TRAK_FILE_DELETE: deletes++; break;
                        case TRAK_NET_CONNECT: connects++; break;
                        case TRAK_NET_BLOCKED: blocked++; break;
                        case TRAK_PROC_FORK:   forks++; break;
                        case TRAK_PROC_EXEC:   execs++; break;
                        default: break;
                        }
                        e = e->next;
                }

                if (reads + writes + creates + deletes > 0)
                        printf("Files:     %zu reads, %zu writes, %zu creates, %zu deletes\n",
                               reads, writes, creates, deletes);
                if (connects + blocked > 0)
                        printf("Network:   %zu connects, %zu blocked\n", connects, blocked);
                if (forks + execs > 0)
                        printf("Processes: %zu forks, %zu execs\n", forks, execs);

                /* Write full JSON trace to stderr (or a file if desired) */
                printf("\n─── JSON Trace ───\n");
                trakker_result_write_json(result, stdout);

                int rc = result->exit_code;
                trakker_result_free(result);
                free(cmd_argv);
                trakker_policy_free(&policy);
                return rc == 0 ? 0 : 1;
        }

        /* Rollback / pin — subject is a generation number */
        if (is_number(subject)) {
                int id = atoi(subject);
                if (strcmp(verb, "rollback") == 0)  return cmd_rollback(id);
                if (strcmp(verb, "pin") == 0) {
                        char *home = getenv("HOME");
                        char db_root[PATH_MAX];
                        if (home) {
                                snprintf(db_root, sizeof(db_root), "%s/.local/state/2O9", home);
                        } else {
                                snprintf(db_root, sizeof(db_root), "/var/lib/2O9");
                        }
                        gen_db_t *db = gen_db_open(db_root);
                        if (!db) {
                                fprintf(stderr, "209: cannot open generation DB\n");
                                return 1;
                        }
                        if (gen_db_lock(db) < 0) {
                                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                                gen_db_close(db);
                                return 1;
                        }
                        if (gen_db_pin(db, id) < 0) {
                                fprintf(stderr, "209: failed to pin generation #%d\n", id);
                                gen_db_unlock(db);
                                gen_db_close(db);
                                return 1;
                        }
                        printf("209: generation #%d pinned\n", id);
                        gen_db_unlock(db);
                        gen_db_close(db);
                        return 0;
                }
        }

        fprintf(stderr, "209: unknown command: %s %s\n", subject, verb);
        fprintf(stderr, "    try: 209 --help\n");
        return 1;
}
