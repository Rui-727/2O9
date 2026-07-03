/* config.h - extra.nix evaluator
 *
 * Reads ~/.config/2O9/extra.nix (or /etc/2O9/extra.nix for system-wide)
 * for runtime config: makepkg/git/gpg/sudo binary paths, MFlags and
 * GitFlags pass-through, chroot settings, binary-cache substituters.
 *
 * This is separate from the declarative 2O9.nix config. extra.nix is for
 * build-tool tuning (which binaries, what extra flags, signing keys),
 * while 2O9.nix is the system declaration (which packages, which
 * services). Both are Nix syntax - per locked decision #7 in DESIGN.md,
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
 * both forms. Plain attrset is recommended for extra.nix.
 *
 * Missing file = use defaults (chroot on, default binaries, no mflags).
 * Unknown keys are silently ignored (forward-compat).
 *
 * The CLI (src/cli/main.c) is expected to call two9_config_load() and
 * populate build_config_t from it. aur_build.c also calls it as a
 * fallback when build_config_t fields are unset.
 */
#ifndef TWO9_CONFIG_H
#define TWO9_CONFIG_H

typedef struct two9_config {
        char **mflags;          /* NULL-terminated list, from bin.MFlags */
        char **git_flags;       /* NULL-terminated list, from bin.GitFlags */
        char *makepkg_bin;      /* default "makepkg" */
        char *git_bin;          /* default "git" */
        char *gpg_bin;          /* default "gpg" */
        char *sudo_bin;         /* default "sudo" */
        int use_chroot;         /* default 1 (chroot on, like paru) */
        char *chroot_dir;       /* default /var/lib/2O9/chroot */

        /* Phase 3: binary cache substituters. */
        char **substituters;    /* NULL-terminated list of URLs (http(s):// or s3://) */
        int allow_unsigned;     /* 1 = accept unsigned narinfos */
        char *signing_key_name; /* narinfo Sig: key-name, may be NULL */
        char *signing_key_file; /* path to secret-key file, may be NULL */
        char *public_key_b64;   /* base64 public key for verifying pulled narinfos, may be NULL */
} two9_config_t;

/* Load config from ~/.config/2O9/extra.nix (or /etc/2O9/extra.nix).
 * Returns a config struct with defaults if the file is missing.
 * Never returns NULL. Caller must free with two9_config_free(). */
two9_config_t *two9_config_load(void);

/* Free a config struct and all owned strings/lists. NULL-safe. */
void two9_config_free(two9_config_t *cfg);

#endif /* TWO9_CONFIG_H */
