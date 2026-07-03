/* config.c - extra.nix evaluator for 2O9 runtime config
 *
 * Reads ~/.config/2O9/extra.nix (or /etc/2O9/extra.nix for system-wide)
 * and evaluates it with the project's own C Nix evaluator (lib/2O9/nix).
 * The evaluator returns a JSON string, which we parse with cJSON and walk
 * to populate two9_config_t.
 *
 * This honors locked decision #7 in DESIGN.md: "One declarative config
 * format: Nix." There is no separate INI file anymore.
 *
 * File format (Nix attrset, evaluated to JSON by nix_eval_file_with_base):
 *
 *   {
 *     bin = {
 *       Makepkg = "makepkg";
 *       Git = "git";
 *       Gpg = "gpg";
 *       Sudo = "sudo";
 *       MFlags = [ "--skippgpcheck" "--nocheck" ];
 *       GitFlags = [ "--depth" "1" ];
 *     };
 *
 *     chroot = {
 *       Enabled = true;
 *       Dir = "/var/lib/2O9/chroot";
 *     };
 *
 *     substituters = {
 *       URLs = [ "https://cache.example.com" ];
 *       PublicKey = "r634rsy7nIo/UH2Xux5k+GSFOh6rsqsGG5R2fNJFR9o=";
 *       AllowUnsigned = false;
 *       SigningKey = "/etc/2O9/secret-key";
 *       KeyName = "cache.example.com-1";
 *     };
 *   }
 *
 * The file may also be written as `{ config, ... }: { ... }` (a function
 * taking the fixed-point `config` argument); the evaluator auto-applies
 * both forms. Plain attrset is recommended for extra.nix since it has no
 * need for the `config` self-reference that 2O9.nix uses.
 *
 * Missing file = use defaults (chroot on, default binaries, no mflags).
 * Unknown keys are silently ignored (forward-compat).
 *
 * The two9_config_t struct, two9_config_load() / two9_config_free()
 * signatures are unchanged from the old INI parser; only the file format
 * and parser change. Callers (src/cli/main.c, src/aur/aur_build.c) are
 * not affected.
 */

/* _GNU_SOURCE is provided by the Makefile (-D_GNU_SOURCE). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "config.h"
#include "nix_eval.h"
#include "cJSON.h"

#define DEFAULT_MAKEPKG_BIN "makepkg"
#define DEFAULT_GIT_BIN     "git"
#define DEFAULT_GPG_BIN     "gpg"
#define DEFAULT_SUDO_BIN    "sudo"
#define DEFAULT_CHROOT_DIR  "/var/lib/2O9/chroot"

/* ── Helpers ──────────────────────────────────────────────────────── */

static void free_str_list(char **list)
{
        if (!list) return;
        for (size_t i = 0; list[i]; i++)
                free(list[i]);
        free(list);
}

/* Build a NULL-terminated char** from a cJSON array of strings.
 * Returns NULL if `arr` is NULL or not an array or empty.
 * Each entry is strdup'd so the caller owns the result and may
 * cJSON_Delete() the source. */
static char **json_array_to_str_list(const cJSON *arr)
{
        if (!arr || !cJSON_IsArray(arr)) return NULL;
        int n = cJSON_GetArraySize(arr);
        if (n <= 0) return NULL;

        char **list = calloc((size_t)n + 1, sizeof(char *));
        if (!list) return NULL;

        size_t count = 0;
        for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                if (!item || !cJSON_IsString(item)) continue;
                list[count] = strdup(item->valuestring);
                if (!list[count]) {
                        free_str_list(list);
                        return NULL;
                }
                count++;
        }
        /* list[count] is already NULL from calloc. */
        return list;
}

/* strdup a cJSON string field into *out, freeing any prior value. */
static void json_set_str(char **out, const cJSON *obj, const char *key)
{
        cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (!item || !cJSON_IsString(item)) return;
        free(*out);
        *out = strdup(item->valuestring);
}

static void json_set_str_list(char ***out, const cJSON *obj, const char *key)
{
        cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (!item || !cJSON_IsArray(item)) return;
        free_str_list(*out);
        *out = json_array_to_str_list(item);
}

static void json_set_bool(int *out, const cJSON *obj, const char *key)
{
        cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (!item || !cJSON_IsBool(item)) return;
        *out = cJSON_IsTrue(item);
}

/* ── Apply the parsed JSON to the config ──────────────────────────── */

static void apply_json(two9_config_t *cfg, const cJSON *root)
{
        if (!root || !cJSON_IsObject(root)) return;

        cJSON *bin = cJSON_GetObjectItemCaseSensitive(root, "bin");
        if (bin && cJSON_IsObject(bin)) {
                json_set_str(&cfg->makepkg_bin, bin, "Makepkg");
                json_set_str(&cfg->git_bin,     bin, "Git");
                json_set_str(&cfg->gpg_bin,     bin, "Gpg");
                json_set_str(&cfg->sudo_bin,    bin, "Sudo");
                json_set_str_list(&cfg->mflags,    bin, "MFlags");
                json_set_str_list(&cfg->git_flags, bin, "GitFlags");
        }

        cJSON *chroot = cJSON_GetObjectItemCaseSensitive(root, "chroot");
        if (chroot && cJSON_IsObject(chroot)) {
                json_set_bool(&cfg->use_chroot, chroot, "Enabled");
                json_set_str(&cfg->chroot_dir,  chroot, "Dir");
        }

        cJSON *subs = cJSON_GetObjectItemCaseSensitive(root, "substituters");
        if (subs && cJSON_IsObject(subs)) {
                json_set_str_list(&cfg->substituters, subs, "URLs");
                json_set_bool(&cfg->allow_unsigned,  subs, "AllowUnsigned");
                json_set_str(&cfg->signing_key_file, subs, "SigningKey");
                json_set_str(&cfg->signing_key_name, subs, "KeyName");
                json_set_str(&cfg->public_key_b64,   subs, "PublicKey");
        }
        /* Unknown sections/keys: silently ignored (forward-compat). */
}

/* ── Defaults + load ──────────────────────────────────────────────── */

static two9_config_t *two9_config_defaults(void)
{
        two9_config_t *cfg = calloc(1, sizeof(*cfg));
        if (!cfg) return NULL;
        cfg->makepkg_bin = strdup(DEFAULT_MAKEPKG_BIN);
        cfg->git_bin     = strdup(DEFAULT_GIT_BIN);
        cfg->gpg_bin     = strdup(DEFAULT_GPG_BIN);
        cfg->sudo_bin    = strdup(DEFAULT_SUDO_BIN);
        cfg->use_chroot  = 1;
        cfg->chroot_dir  = strdup(DEFAULT_CHROOT_DIR);
        return cfg;
}

/* Resolve the config file path. Tries ~/.config/2O9/extra.nix first
 * (with SUDO_USER resolution), then falls back to /etc/2O9/extra.nix.
 * Returns a malloc'd path the caller must free, or NULL if neither
 * exists. *path_out is set to the malloc'd path on success. */
static char *resolve_config_path(void)
{
        /* User-local: ~/.config/2O9/extra.nix
         * When running under sudo, look up the original user's home. */
        const char *home = getenv("HOME");
        if (!home || !*home) {
                const char *sudo_user = getenv("SUDO_USER");
                if (sudo_user && *sudo_user) {
                        char buf[PATH_MAX];
                        snprintf(buf, sizeof(buf), "/home/%s", sudo_user);
                        setenv("HOME", buf, 1);
                        home = getenv("HOME");
                }
        }

        if (home && *home) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/.config/2O9/extra.nix", home);
                if (access(path, R_OK) == 0)
                        return strdup(path);
        }

        /* System-wide fallback. */
        if (access("/etc/2O9/extra.nix", R_OK) == 0)
                return strdup("/etc/2O9/extra.nix");

        return NULL;
}

/* Read a file fully into a malloc'd, NUL-terminated buffer. */
static char *read_file_all(const char *path)
{
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
        long fsize = ftell(f);
        if (fsize < 0) { fclose(f); return NULL; }
        rewind(f);
        char *buf = malloc((size_t)fsize + 1);
        if (!buf) { fclose(f); return NULL; }
        size_t nread = fread(buf, 1, (size_t)fsize, f);
        buf[nread] = '\0';
        fclose(f);
        return buf;
}

two9_config_t *two9_config_load(void)
{
        two9_config_t *cfg = two9_config_defaults();
        if (!cfg) return NULL;

        char *path = resolve_config_path();
        if (!path) return cfg;  /* file missing - use defaults */

        char *source = read_file_all(path);
        if (!source) { free(path); return cfg; }

        /* base_dir is the directory of the config file, for resolving
         * any relative imports inside extra.nix (rare, but supported). */
        char *base_dir = strdup(path);
        if (base_dir) {
                char *slash = strrchr(base_dir, '/');
                if (slash) *slash = '\0';
        }

        char *eval_err = NULL;
        char *json = nix_eval_file_with_base(source, strlen(source),
                                              base_dir, &eval_err);

        free(source);
        free(base_dir);

        if (!json) {
                /* Eval failure: warn but keep defaults so 209 still runs. */
                fprintf(stderr, "209: warning: failed to evaluate %s: %s\n",
                        path, eval_err ? eval_err : "(unknown error)");
                free(eval_err);
                free(path);
                return cfg;
        }

        cJSON *root = cJSON_Parse(json);
        if (!root) {
                fprintf(stderr, "209: warning: %s evaluated to invalid JSON\n",
                        path);
        } else {
                apply_json(cfg, root);
                cJSON_Delete(root);
        }

        free(json);
        free(path);
        return cfg;
}

void two9_config_free(two9_config_t *cfg)
{
        if (!cfg) return;
        free_str_list(cfg->mflags);
        free_str_list(cfg->git_flags);
        free(cfg->makepkg_bin);
        free(cfg->git_bin);
        free(cfg->gpg_bin);
        free(cfg->sudo_bin);
        free(cfg->chroot_dir);
        free_str_list(cfg->substituters);
        free(cfg->signing_key_name);
        free(cfg->signing_key_file);
        free(cfg->public_key_b64);
        free(cfg);
}
