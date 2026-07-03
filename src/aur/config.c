/* config.c - 2O9.conf INI parser implementation
 *
 * Simple line-based INI parser. Handles:
 *   - [section] headers
 *   - key = value (whitespace trimmed)
 *   - # and ; comments
 *   - quoted values ("..." and '...')
 *   - space-separated list values (MFlags, GitFlags)
 *
 * Sections currently understood: [bin] and [chroot].
 * Unknown keys are silently ignored (forward-compat).
 *
 * Intentionally minimal: ~150 LOC. No nested sections, no escapes,
 * no multi-line values. Use 2O9.nix for the heavy stuff.
 */

/* _GNU_SOURCE is provided by the Makefile (-D_GNU_SOURCE). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include "config.h"

#define DEFAULT_MAKEPKG_BIN "makepkg"
#define DEFAULT_GIT_BIN     "git"
#define DEFAULT_GPG_BIN     "gpg"
#define DEFAULT_SUDO_BIN    "sudo"
#define DEFAULT_CHROOT_DIR  "/var/lib/2O9/chroot"

/* ── Helpers ──────────────────────────────────────────────────────── */

static char *str_trim(char *s)
{
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) return s;
        char *end = s + strlen(s) - 1;
        while (end > s && isspace((unsigned char)*end)) {
                *end = '\0';
                end--;
        }
        return s;
}

static char *str_dup_trim(const char *s)
{
        char *copy = strdup(s);
        if (!copy) return NULL;
        char *trimmed = str_trim(copy);
        char *result = strdup(trimmed);
        free(copy);
        return result;
}

/* Split a whitespace-separated string into a NULL-terminated list.
 * Caller frees each string + the list. Returns NULL for empty input. */
static char **split_ws(const char *s)
{
        if (!s || !*s) return NULL;

        char *copy = strdup(s);
        if (!copy) return NULL;

        size_t cap = 8, count = 0;
        char **list = calloc(cap, sizeof(char *));

        char *save = NULL;
        char *tok = strtok_r(copy, " \t", &save);
        while (tok) {
                if (count >= cap) {
                        cap *= 2;
                        list = realloc(list, cap * sizeof(char *));
                }
                list[count++] = strdup(tok);
                tok = strtok_r(NULL, " \t", &save);
        }
        free(copy);

        if (count == 0) {
                free(list);
                return NULL;
        }

        if (count >= cap) {
                cap += 1;
                list = realloc(list, cap * sizeof(char *));
        }
        list[count] = NULL;

        return list;
}

static int parse_bool(const char *v)
{
        if (!v) return 0;
        return (strcasecmp(v, "yes") == 0 ||
                strcasecmp(v, "true") == 0 ||
                strcasecmp(v, "1") == 0   ||
                strcasecmp(v, "on") == 0);
}

static void free_str_list(char **list)
{
        if (!list) return;
        for (size_t i = 0; list[i]; i++)
                free(list[i]);
        free(list);
}

/* ── Apply a parsed key=value to the config ───────────────────────── */

static void apply_kv(two9_config_t *cfg, const char *section,
                     const char *key, const char *value)
{
        if (strcmp(section, "bin") == 0) {
                if (strcasecmp(key, "Makepkg") == 0) {
                        free(cfg->makepkg_bin);
                        cfg->makepkg_bin = str_dup_trim(value);
                } else if (strcasecmp(key, "Git") == 0) {
                        free(cfg->git_bin);
                        cfg->git_bin = str_dup_trim(value);
                } else if (strcasecmp(key, "Gpg") == 0) {
                        free(cfg->gpg_bin);
                        cfg->gpg_bin = str_dup_trim(value);
                } else if (strcasecmp(key, "Sudo") == 0) {
                        free(cfg->sudo_bin);
                        cfg->sudo_bin = str_dup_trim(value);
                } else if (strcasecmp(key, "MFlags") == 0) {
                        free_str_list(cfg->mflags);
                        cfg->mflags = split_ws(value);
                } else if (strcasecmp(key, "GitFlags") == 0) {
                        free_str_list(cfg->git_flags);
                        cfg->git_flags = split_ws(value);
                }
        } else if (strcmp(section, "chroot") == 0) {
                if (strcasecmp(key, "Enabled") == 0) {
                        cfg->use_chroot = parse_bool(value);
                } else if (strcasecmp(key, "Dir") == 0) {
                        free(cfg->chroot_dir);
                        cfg->chroot_dir = str_dup_trim(value);
                }
        } else if (strcmp(section, "substituters") == 0) {
                /* Phase 3: binary cache config. */
                if (strcasecmp(key, "URLs") == 0) {
                        free_str_list(cfg->substituters);
                        cfg->substituters = split_ws(value);
                } else if (strcasecmp(key, "AllowUnsigned") == 0) {
                        cfg->allow_unsigned = parse_bool(value);
                } else if (strcasecmp(key, "SigningKey") == 0) {
                        free(cfg->signing_key_file);
                        cfg->signing_key_file = str_dup_trim(value);
                } else if (strcasecmp(key, "KeyName") == 0) {
                        free(cfg->signing_key_name);
                        cfg->signing_key_name = str_dup_trim(value);
                } else if (strcasecmp(key, "PublicKey") == 0) {
                        free(cfg->public_key_b64);
                        cfg->public_key_b64 = str_dup_trim(value);
                }
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

two9_config_t *two9_config_load(void)
{
        two9_config_t *cfg = two9_config_defaults();
        if (!cfg) return NULL;

        /* Resolve config file path: ~/.config/2O9/2O9.conf
         * When running under sudo, look up the original user's home. */
        const char *home = getenv("HOME");
        if (!home || !*home) {
                const char *sudo_user = getenv("SUDO_USER");
                if (sudo_user && *sudo_user) {
                        char buf[PATH_MAX];
                        snprintf(buf, sizeof(buf), "/home/%s", sudo_user);
                        /* Try this path; if HOME ends up unset, we just
                         * skip loading the config file. */
                        setenv("HOME", buf, 1);
                        home = getenv("HOME");
                }
        }
        if (!home) return cfg;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/.config/2O9/2O9.conf", home);

        FILE *f = fopen(path, "r");
        if (!f) return cfg;  /* file missing - use defaults */

        char section[64] = "";
        char line[4096];

        while (fgets(line, sizeof(line), f)) {
                char *s = str_trim(line);

                if (*s == '\0' || *s == '#' || *s == ';')
                        continue;

                if (*s == '[') {
                        char *end = strchr(s, ']');
                        if (end) {
                                *end = '\0';
                                char *name = str_trim(s + 1);
                                strncpy(section, name, sizeof(section) - 1);
                                section[sizeof(section) - 1] = '\0';
                        }
                        continue;
                }

                char *eq = strchr(s, '=');
                if (!eq) continue;

                *eq = '\0';
                char *key = str_trim(s);
                char *value = str_trim(eq + 1);

                /* Strip matching surrounding quotes from value */
                size_t vlen = strlen(value);
                if (vlen >= 2 &&
                    (*value == '"' || *value == '\'') &&
                    value[vlen - 1] == *value) {
                        value[vlen - 1] = '\0';
                        value++;
                }

                if (*key && *value) {
                        apply_kv(cfg, section, key, value);
                }
        }

        fclose(f);
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
