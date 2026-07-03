/* two9_init.c - 2O9 programmatic config entrypoint for libalpm
 *
 * Implements MODIFICATIONS.md #3: lib2O9 is configured programmatically
 * from a 2O9 manifest, never from /etc/pacman.conf. The manifest is the
 * JSON output of evaluating 2O9.nix (see src/declarative/gen.c).
 *
 * This file is the bridge between the 2O9 declarative engine and the
 * vendored libalpm. It calls alpm_initialize() + alpm_option_set_*() +
 * alpm_db_register_sync() to construct a fully-configured alpm_handle_t
 * without ever reading a config file.
 *
 * The manifest JSON we consume has the shape produced by cmd_apply's
 * nix_eval_file() call, e.g.:
 *   {
 *     "pacman": {
 *       "options": { "SigLevel": "Required DatabaseOptional",
 *                    "ParallelDownloads": 5,
 *                    "IgnorePkg": ["linux"] },
 *       "repos": { "core":     { "server": "https://..." },
 *                  "extra":    { "server": "https://..." },
 *                  "multilib": { "server": "https://..." } }
 *     }
 *   }
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

#include "alpm.h"
#include "handle.h"

/* cJSON is vendored at src/aur/cJSON.{c,h}. It has no aur/ coupling - 
 * it's a generic JSON library that just happens to live in that dir.
 * We include it from there to avoid duplicating JSON parsing logic. */
#include "cJSON.h"

/* ── SigLevel string parsing ───────────────────────────────────────
 * Mirrors pacman's parseSigLevel() (lib/libalpm/handle.c): tokens are
 * whitespace-separated, a leading '~' means "unset these bits", and
 * the recognised names match pacman.conf(5):
 *   Package PackageOptional PackageRequired PackageMarginalOk PackageTrustUnknown
 *   Database DatabaseOptional DatabaseRequired DatabaseMarginalOk DatabaseTrustUnknown
 *   Required Optional MarginalOk TrustUnknown Never Default
 * Note: the ALPM_SIG_*_TRUST_UNKNOWN enum constant in alpm.h is actually
 * named ALPM_SIG_*_UNKNOWN_OK; the "TrustUnknown" config string maps to it. */

static int siglevel_apply_token(int *level, const char *tok, int unset)
{
    if (strcmp(tok, "Package") == 0) {
        if (unset) *level &= ~ALPM_SIG_PACKAGE;
        else *level |= ALPM_SIG_PACKAGE;
    } else if (strcmp(tok, "PackageOptional") == 0) {
        if (unset) *level &= ~(ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL);
        else *level |= ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL;
    } else if (strcmp(tok, "PackageRequired") == 0) {
        if (unset) *level &= ~ALPM_SIG_PACKAGE;
        else { *level |= ALPM_SIG_PACKAGE; *level &= ~ALPM_SIG_PACKAGE_OPTIONAL; }
    } else if (strcmp(tok, "PackageMarginalOk") == 0) {
        if (unset) *level &= ~ALPM_SIG_PACKAGE_MARGINAL_OK;
        else *level |= ALPM_SIG_PACKAGE_MARGINAL_OK;
    } else if (strcmp(tok, "PackageTrustUnknown") == 0) {
        if (unset) *level &= ~ALPM_SIG_PACKAGE_UNKNOWN_OK;
        else *level |= ALPM_SIG_PACKAGE_UNKNOWN_OK;
    } else if (strcmp(tok, "Database") == 0) {
        if (unset) *level &= ~ALPM_SIG_DATABASE;
        else *level |= ALPM_SIG_DATABASE;
    } else if (strcmp(tok, "DatabaseOptional") == 0) {
        if (unset) *level &= ~(ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL);
        else *level |= ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL;
    } else if (strcmp(tok, "DatabaseRequired") == 0) {
        if (unset) *level &= ~ALPM_SIG_DATABASE;
        else { *level |= ALPM_SIG_DATABASE; *level &= ~ALPM_SIG_DATABASE_OPTIONAL; }
    } else if (strcmp(tok, "DatabaseMarginalOk") == 0) {
        if (unset) *level &= ~ALPM_SIG_DATABASE_MARGINAL_OK;
        else *level |= ALPM_SIG_DATABASE_MARGINAL_OK;
    } else if (strcmp(tok, "DatabaseTrustUnknown") == 0) {
        if (unset) *level &= ~ALPM_SIG_DATABASE_UNKNOWN_OK;
        else *level |= ALPM_SIG_DATABASE_UNKNOWN_OK;
    } else if (strcmp(tok, "Required") == 0) {
        if (unset) *level &= ~(ALPM_SIG_PACKAGE | ALPM_SIG_DATABASE);
        else {
            *level |= ALPM_SIG_PACKAGE | ALPM_SIG_DATABASE;
            *level &= ~(ALPM_SIG_PACKAGE_OPTIONAL | ALPM_SIG_DATABASE_OPTIONAL);
        }
    } else if (strcmp(tok, "Optional") == 0) {
        if (unset) *level &= ~(ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL
                              | ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL);
        else {
            *level |= ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL
                   |  ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL;
        }
    } else if (strcmp(tok, "MarginalOk") == 0) {
        if (unset) *level &= ~(ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_DATABASE_MARGINAL_OK);
        else *level |= ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_DATABASE_MARGINAL_OK;
    } else if (strcmp(tok, "TrustUnknown") == 0) {
        if (unset) *level &= ~(ALPM_SIG_PACKAGE_UNKNOWN_OK | ALPM_SIG_DATABASE_UNKNOWN_OK);
        else *level |= ALPM_SIG_PACKAGE_UNKNOWN_OK | ALPM_SIG_DATABASE_UNKNOWN_OK;
    } else if (strcmp(tok, "Never") == 0) {
        *level = 0;
    } else if (strcmp(tok, "Default") == 0) {
        *level |= ALPM_SIG_USE_DEFAULT;
    } else {
        return -1;
    }
    return 0;
}

/* Parse a SigLevel config string (whitespace-separated tokens, ~ = unset).
 * Returns 0 on success (out_level always written, 0 if string empty),
 * -1 on unknown token (out_level left untouched). */
static int parse_siglevel(const char *str, int *out_level)
{
    int level = 0;
    if (!str || !*str) {
        *out_level = 0;
        return 0;
    }

    char *copy = strdup(str);
    if (!copy) return -1;

    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    while (tok) {
        int unset = 0;
        char *actual = tok;
        if (*actual == '~') { unset = 1; actual++; }
        if (siglevel_apply_token(&level, actual, unset) != 0) {
            fprintf(stderr, "2O9: unknown SigLevel token '%s'\n", actual);
            free(copy);
            return -1;
        }
        tok = strtok_r(NULL, " \t", &save);
    }

    free(copy);
    *out_level = level;
    return 0;
}

/* ── Public: build a configured alpm_handle_t from a 2O9 manifest ── */

alpm_handle_t *two9_alpm_init_from_manifest(const char *manifest_json)
{
    if (!manifest_json) return NULL;

    /* Determine DB path. When running as root via sudo, resolve the
     * original user's home via SUDO_USER so we use the same dbpath
     * that 209 sync wrote to. This way apply (root) can read the
     * sync DBs that sync (user) downloaded. */
    char dbpath[512];
    const char *home_dir = NULL;
    char home_buf[512];

    if (getuid() == 0) {
        /* Root: check SUDO_USER first */
        const char *sudo_user = getenv("SUDO_USER");
        if (sudo_user && sudo_user[0]) {
            struct passwd *pw = getpwnam(sudo_user);
            if (pw && pw->pw_dir) {
                home_dir = pw->pw_dir;
            }
        }
        /* Check HOME (might be preserved via --preserve-env=HOME) */
        if (!home_dir) {
            const char *env_home = getenv("HOME");
            if (env_home && strcmp(env_home, "/root") != 0)
                home_dir = env_home;
        }
    } else {
        home_dir = getenv("HOME");
    }

    if (home_dir)
        snprintf(dbpath, sizeof(dbpath), "%s/.local/state/2O9", home_dir);
    else
        snprintf(dbpath, sizeof(dbpath), "/var/lib/2O9");

    /* Create the DB directory if it doesn't exist */
    /* Walk the path creating each component */
    char tmp[512];
    strncpy(tmp, dbpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);

    alpm_errno_t err = 0;
    alpm_handle_t *handle = alpm_initialize("/", dbpath, &err);
    if (!handle) {
        fprintf(stderr, "2O9: alpm_initialize failed (dbpath=%s): %s\n",
                dbpath, alpm_strerror(err));
        return NULL;
    }

    /* Set cache dir: same user logic as dbpath */
    char cache_dir[512];
    if (home_dir)
        snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/2O9/pkg", home_dir);
    else
        snprintf(cache_dir, sizeof(cache_dir), "/var/cache/2O9/pkg");
    alpm_list_t *cachedirs = alpm_list_add(NULL, strdup(cache_dir));
    alpm_option_set_cachedirs(handle, cachedirs);

    /* Set architectures */
    alpm_list_t *arches = alpm_list_add(NULL, strdup("x86_64"));
    alpm_option_set_architectures(handle, arches);

    /* Parse the manifest JSON */
    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) {
        /* Manifest didn't parse - return handle with defaults applied */
        return handle;
    }

    /* Walk pacman.options and pacman.repos */
    cJSON *pacman = cJSON_GetObjectItem(root, "pacman");
    if (pacman) {
        int siglevel = 0;  /* 0 = no signature checking (legacy default) */
        int have_siglevel = 0;

        cJSON *options = cJSON_GetObjectItem(pacman, "options");
        if (options) {
            cJSON *pd = cJSON_GetObjectItem(options, "ParallelDownloads");
            if (cJSON_IsNumber(pd) && pd->valueint > 0) {
                handle->parallel_downloads = (unsigned int)pd->valueint;
            }

            /* SigLevel: parse the pacman.conf-style string into a bitmask
             * (Package/Database/Required/Optional/MarginalOk/TrustUnknown/
             *  Never/Default, leading '~' to unset). Applied to the handle's
             *  default + local/remote file siglevels, AND passed as the level
             *  arg to alpm_register_syncdb so database signatures are checked
             *  by alpm_db_update() automatically. */
            cJSON *sl = cJSON_GetObjectItem(options, "SigLevel");
            if (cJSON_IsString(sl) && sl->valuestring &&
                parse_siglevel(sl->valuestring, &siglevel) == 0) {
                have_siglevel = 1;
                if (siglevel != 0) {
                    alpm_option_set_default_siglevel(handle, siglevel);
                    /* Mirror pacman: the local-file and remote-file levels
                     * inherit from the default unless overridden separately.
                     * 2O9 doesn't expose LocalFileSigLevel / RemoteFileSigLevel
                     * yet, so we set both to the same value. */
                    alpm_option_set_local_file_siglevel(handle, siglevel);
                    alpm_option_set_remote_file_siglevel(handle, siglevel);
                }
            }

            /* IgnorePkg list */
            cJSON *ignore = cJSON_GetObjectItem(options, "IgnorePkg");
            if (cJSON_IsArray(ignore)) {
                cJSON *pkg;
                cJSON_ArrayForEach(pkg, ignore) {
                    if (cJSON_IsString(pkg)) {
                        alpm_option_add_ignorepkg(handle, pkg->valuestring);
                    }
                }
            }
        }

        /* Register sync DBs from pacman.repos.
         * Per alpm_register_syncdb(3): "what level of signature checking to
         * perform on the database". Passing the parsed SigLevel here makes
         * alpm_db_update() verify .db.sig files automatically when the level
         * includes ALPM_SIG_DATABASE. */
        cJSON *repos = cJSON_GetObjectItem(pacman, "repos");
        if (cJSON_IsObject(repos)) {
            cJSON *repo;
            cJSON_ArrayForEach(repo, repos) {
                const char *repo_name = repo->string;
                cJSON *server = cJSON_GetObjectItem(repo, "server");
                if (!cJSON_IsString(server)) continue;

                int db_level = have_siglevel ? siglevel : 0;
                alpm_db_t *db = alpm_register_syncdb(handle, repo_name, db_level);
                if (db) {
                    alpm_db_add_server(db, server->valuestring);
                }
            }
        }
    }

    cJSON_Delete(root);
    return handle;
}

/* ── Public: register 2O9 callbacks on an existing handle ────────── */

void two9_alpm_register_backends(alpm_handle_t *handle,
                                 char *(*install_backend)(alpm_handle_t *, alpm_pkg_t *, const char *),
                                 int (*installed_set_loader)(alpm_handle_t *, alpm_db_t *))
{
    if (!handle) return;
    handle->install_backend = install_backend;
    handle->installed_set_loader = installed_set_loader;
}
