/* config.h - extra.nix evaluator
 *
 * Reads /nix/config/<user>.extra.nix (user scope) or /nix/config/extra.nix
 * (system scope) for runtime config: makepkg/git/gpg/sudo binary paths,
 * MFlags and GitFlags pass-through, chroot settings, binary-cache subs.
 *
 * This is separate from the declarative 2O9.nix config. extra.nix is for
 * build-tool tuning (which binaries, what extra flags, signing keys),
 * while 2O9.nix is the system declaration (which packages, which
 * services). Both are Nix syntax per locked decision #7 in DESIGN.md,
 * "One declarative config format: Nix."
 *
 * File format (Nix attrset, evaluated by the project's own C Nix
 * evaluator at lib/2O9/nix):
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
 *       "friend" = {
 *         URLs = [ "https://friend.example.org/cache" ];
 *         PublicKeys = [ "friendkeybase64==" ];
 *         AllowUnsigned = false;
 *       };
 *     };
 *   }
 *
 * The file may also be written as `{ config, ... }: { ... }` (a function
 * taking the fixed-point `config` argument); the evaluator auto-applies
 * both forms. Plain attrset is recommended for extra.nix.
 *
 * Missing file = use defaults (chroot on, default binaries, no mflags).
 * Unknown keys are silently ignored (forward-compat).
 *
 * Backward compat: the old flat `substituters` block (with a single
 * PublicKey string) is still parsed as a single sub named "legacy".
 * A deprecation warning is printed.
 *
 * The CLI (src/cli/main.c) is expected to call two9_config_load() and
 * populate build_config_t from it. aur_build.c also calls it as a
 * fallback when build_config_t fields are unset.
 */
#ifndef TWO9_CONFIG_H
#define TWO9_CONFIG_H

/* One named binary-cache substituter. A sub may have multiple URLs and
 * multiple PublicKeys. A narinfo is accepted if ANY of the listed
 * PublicKeys verifies its signature. */
typedef struct two9_sub {
        char *name;                 /* attrset key, e.g. "personal", "legacy" */
        char **urls;                /* NULL-terminated list, http(s):// or s3:// */
        char **public_keys;         /* NULL-terminated list of base64 pubkeys */
        int allow_unsigned;         /* 1 = accept unsigned narinfos */
        char *signing_key_file;     /* path to secret-key file, may be NULL */
        char *signing_key_name;     /* narinfo Sig: key-name, may be NULL */
        struct two9_sub *next;      /* linked list, preserves config order */
} two9_sub_t;

typedef struct two9_config {
        char **mflags;          /* NULL-terminated list, from bin.MFlags */
        char **git_flags;       /* NULL-terminated list, from bin.GitFlags */
        char *makepkg_bin;      /* default "makepkg" */
        char *git_bin;          /* default "git" */
        char *gpg_bin;          /* default "gpg" */
        char *sudo_bin;         /* default "sudo" */
        int use_chroot;         /* default 1 (chroot on, like paru) */
        char *chroot_dir;       /* default /var/lib/2O9/chroot */

        /* Named binary-cache substituters, linked list, may be NULL. */
        two9_sub_t *subs;
} two9_config_t;

/* Load config from /nix/config/<user>.extra.nix (or /nix/config/extra.nix
 * for system-wide). Returns a config struct with defaults if the file
 * is missing. Never returns NULL. Caller must free with two9_config_free(). */
two9_config_t *two9_config_load(void);

/* Free a config struct and all owned strings/lists. NULL-safe. */
void two9_config_free(two9_config_t *cfg);

#endif /* TWO9_CONFIG_H */
