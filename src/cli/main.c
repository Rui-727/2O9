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

#include "store/store.h"
#include "declarative/gen.h"
#include "store/symlinks.h"
#include "aur/aur_rpc.h"
#include "aur/build.h"
#include "aur/resolver.h"

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
        printf("  209 <term> search  Search repos\n");
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
                        printf("  %3d  %7zu  %s%s\n",
                               gens[i]->id,
                               gens[i]->pkg_count,
                               gens[i]->is_pinned ? "yes" : "no",
                               gens[i]->id == current ? "  ← current" : "");
                }
        }

        gen_list_free(gens, count);
        gen_db_close(db);
        return 0;
}

/* ── Install ─────────────────────────────────────────────────────── */

static int cmd_install(const char *pkg_name)
{
        /* Phase 1 MVP: this is the imperative install path.
         * The package lands in the current generation temporarily.
         * Next `209 apply` will flag it for removal unless it's in 2O9.nix.
         */

        printf("209: installing %s...\n", pkg_name);

        /* Step 1: resolve package via lib209 (TODO — Phase 1 proper)
         *   For now, we assume the .pkg.tar.zst is already downloaded
         *   and we just add it to the store. */

        /* Step 2: add to store */
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
                /* No pkg path and no test mode — can't proceed */
                fprintf(stderr, "209: package resolution not yet implemented\n");
                fprintf(stderr, "    set TWO09_PKG_PATH=/path/to/pkg.tar.zst to test store add\n");
                fprintf(stderr, "    or set TWO09_TEST_MODE=1 for end-to-end pipeline test\n");
                return 1;
        } else {
                result = store_add(pkg_path, STORE_BACKEND_NIX_STORE);
                if (result.success != 0) {
                        fprintf(stderr, "209: store add failed: %s\n", result.error_msg);
                        store_add_result_free(&result);
                        return 1;
                }
        }

        printf("  store path: %s\n", result.store_path);

        /* Step 3: commit new generation */
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

        /* Take lock before mutating */
        if (gen_db_lock(db) < 0) {
                fprintf(stderr, "209: another 2O9 process is running. Try again.\n");
                gen_db_close(db);
                store_add_result_free(&result);
                return 1;
        }

        /* Build package entry for the new generation.
         * TODO: merge with current generation's packages, add new one. */
        gen_pkg_t *pkg = gen_pkg_create(pkg_name, "0.0.0",
                                        result.store_path, "imperative");

        int new_id = gen_db_commit(db, pkg);
        if (new_id < 0) {
                fprintf(stderr, "209: failed to commit generation\n");
                gen_pkg_list_free(pkg);
                gen_db_close(db);
                store_add_result_free(&result);
                return 1;
        }

        printf("  generation #%d committed\n", new_id);

        /* Step 4: build symlink farm */
        gen_t *gen = gen_db_get(db, new_id);
        if (gen) {
                /* For MVP, no previous generation diff */
                symlink_farm_build(db, gen, NULL);
                gen_free(gen);
        }

        printf("  done. rollback with: 209 %d rollback\n", new_id - 1 > 0 ? new_id - 1 : 1);
        printf("  NOTE: reboot for full system state to take effect\n");

        gen_pkg_list_free(pkg);
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

/* ── Apply (declarative) ─────────────────────────────────────────── */

static int cmd_apply(void)
{
        /* TODO: Phase 3 — evaluate 2O9.nix, reconcile with current generation,
         * build transaction, commit, rebuild symlink farm. */
        fprintf(stderr, "209: apply not yet implemented (Phase 3)\n");
        return 1;
}

/* ── Sync ────────────────────────────────────────────────────────── */

static int cmd_sync(void)
{
        /* TODO: invoke lib209's sync to refresh repo databases */
        fprintf(stderr, "209: sync not yet implemented (needs lib209)\n");
        return 1;
}

/* ── GC ──────────────────────────────────────────────────────────── */

static int cmd_gc(void)
{
        /* TODO: walk store, find paths not referenced by any generation,
         * delete them. Similar to nix-collect-garbage. */
        fprintf(stderr, "209: gc not yet implemented\n");
        return 1;
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

        /* Disable services from the removed package.
         * TODO: scan the store path for systemd units and stop them.
         * For now, just remind the user. */
        printf("  NOTE: check if any services need to be stopped manually\n");
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
                fprintf(stderr, "209: news not yet implemented\n");
                return 1;
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

        /* Search */
        if (strcmp(verb, "search") == 0) {
                fprintf(stderr, "209: search not yet implemented\n");
                return 1;
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

        /* Trakker */
        if (strcmp(verb, "trakker") == 0) {
                fprintf(stderr, "209: trakker not yet implemented (Phase 4)\n");
                return 1;
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
