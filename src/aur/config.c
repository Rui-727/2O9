/* config.c - extra.nix evaluator for 2O9 runtime config
 *
 * Reads /nix/config/<user>.extra.nix (or /nix/config/extra.nix for
 * system-wide) and evaluates it with the project's own C Nix evaluator
 * (lib/2O9/nix). The evaluator returns a JSON string, which we parse
 * with cJSON and walk to populate two9_config_t.
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
 *     subs = {
 *       "personal" = {
 *         URLs = [ "https://cache.example.com" "s3://backup-bucket" ];
 *         PublicKeys = [ "key1base64==" "key2base64==" ];
 *         AllowUnsigned = false;
 *         SigningKey = "/etc/2O9/personal-secret-key";
 *         KeyName = "personal-1";
 *       };
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
 * Backward compat: the old flat `substituters` block (with a single
 * PublicKey string) is still parsed as a single sub named "legacy".
 * A deprecation warning is printed to stderr.
 *
 * The two9_config_t struct, two9_config_load() / two9_config_free()
 * signatures are unchanged from the old INI parser; only the file format
 * and parser change. Callers (src/cli/main.c, src/aur/aur_build.c) are
 * not affected by the file format change, but main.c's substituter
 * iteration was updated to walk cfg->subs (linked list).
 */

/* _GNU_SOURCE is provided by the Makefile (-D_GNU_SOURCE). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pwd.h>

#include "config.h"
#include "nix_eval.h"
#include "cJSON.h"

#define DEFAULT_MAKEPKG_BIN "makepkg"
#define DEFAULT_GIT_BIN     "git"
#define DEFAULT_GPG_BIN     "gpg"
#define DEFAULT_SUDO_BIN    "sudo"
#define DEFAULT_CHROOT_DIR  "/var/lib/2O9/chroot"

/* The config dir is fixed at /nix/config per the v2 layout.
 * TWO9_CONFIG_DIR env var overrides for testing. */
static const char *config_dir(void)
{
        const char *env = getenv("TWO9_CONFIG_DIR");
        if (env && *env) return env;
        return "/nix/config";
}

/* Resolve the username for the per-user extra.nix path.
 * If running as root via sudo, use SUDO_USER; else getpwuid(getuid()). */
static const char *current_username(void)
{
        if (getuid() == 0) {
                const char *su = getenv("SUDO_USER");
                if (su && *su) return su;
        }
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name) return pw->pw_name;
        return NULL;
}

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

/* ── two9_sub_t helpers ───────────────────────────────────────────── */

static two9_sub_t *sub_new(const char *name)
{
        two9_sub_t *s = calloc(1, sizeof(*s));
        if (!s) return NULL;
        if (name) {
                s->name = strdup(name);
                if (!s->name) { free(s); return NULL; }
        }
        return s;
}

static void sub_free(two9_sub_t *s)
{
        if (!s) return;
        free(s->name);
        free_str_list(s->urls);
        free_str_list(s->public_keys);
        free(s->signing_key_file);
        free(s->signing_key_name);
        free(s);
}

static void subs_free_all(two9_sub_t *head)
{
        while (head) {
                two9_sub_t *next = head->next;
                sub_free(head);
                head = next;
        }
}

/* Append sub to the end of cfg->subs. */
static void subs_append(two9_config_t *cfg, two9_sub_t *s)
{
        if (!cfg || !s) return;
        s->next = NULL;
        if (!cfg->subs) {
                cfg->subs = s;
                return;
        }
        two9_sub_t *tail = cfg->subs;
        while (tail->next) tail = tail->next;
        tail->next = s;
}

/* Parse one sub attrset (the value side of subs.<name>). */
static void apply_sub_attrs(two9_sub_t *s, const cJSON *obj)
{
        if (!s || !obj || !cJSON_IsObject(obj)) return;
        json_set_str_list(&s->urls,         obj, "URLs");
        json_set_str_list(&s->public_keys,  obj, "PublicKeys");
        json_set_bool(&s->allow_unsigned,   obj, "AllowUnsigned");
        json_set_str(&s->signing_key_file,  obj, "SigningKey");
        json_set_str(&s->signing_key_name,  obj, "KeyName");
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

        /* New format: named subs attrset. */
        cJSON *subs = cJSON_GetObjectItemCaseSensitive(root, "subs");
        if (subs && cJSON_IsObject(subs)) {
                for (cJSON *child = subs->child; child; child = child->next) {
                        if (!cJSON_IsObject(child)) continue;
                        two9_sub_t *s = sub_new(child->string ? child->string : "sub");
                        if (!s) continue;
                        apply_sub_attrs(s, child);
                        subs_append(cfg, s);
                }
        }

        /* Backward compat: old flat substituters block. Parse as a
         * single sub named "legacy" with one PublicKey. Warn. */
        cJSON *legacy = cJSON_GetObjectItemCaseSensitive(root, "substituters");
        if (legacy && cJSON_IsObject(legacy)) {
                fprintf(stderr,
                        "209: warning: extra.nix uses the deprecated `substituters` block.\n"
                        "    Rename it to `subs.legacy` and convert `PublicKey` (string)\n"
                        "    to `PublicKeys` (list of strings).\n");
                two9_sub_t *s = sub_new("legacy");
                if (!s) goto done;
                apply_sub_attrs(s, legacy);
                /* If the old code used the singular `PublicKey` field,
                 * fold it into the public_keys list as a single entry. */
                cJSON *pk = cJSON_GetObjectItemCaseSensitive(legacy, "PublicKey");
                if (pk && cJSON_IsString(pk) && !s->public_keys) {
                        s->public_keys = calloc(2, sizeof(char *));
                        if (s->public_keys) {
                                s->public_keys[0] = strdup(pk->valuestring);
                                s->public_keys[1] = NULL;
                        }
                }
                subs_append(cfg, s);
        }

done:
        /* Unknown sections/keys: silently ignored (forward-compat). */
        ;
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

/* Resolve the config file path. Only /nix/config/extra.nix is loaded
 * directly. User side configs (<user>.extra.nix) take effect only if
 * extra.nix imports them via standard Nix import. Returns a malloc'd
 * path the caller must free, or NULL if it doesn't exist. */
static char *resolve_config_path(void)
{
        const char *dir = config_dir();

        char syspath[PATH_MAX];
        snprintf(syspath, sizeof(syspath), "%s/extra.nix", dir);
        if (access(syspath, R_OK) == 0)
                return strdup(syspath);

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
        subs_free_all(cfg->subs);
        free(cfg);
}
