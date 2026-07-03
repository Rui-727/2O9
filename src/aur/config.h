/* config.h - 2O9.conf INI parser
 *
 * Reads ~/.config/2O9/2O9.conf for runtime config: makepkg/git/gpg/sudo
 * binary paths, MFlags and GitFlags pass-through, chroot settings.
 *
 * This is separate from the declarative 2O9.nix config; 2O9.conf is for
 * build-tool tuning (which binaries, what extra flags), while 2O9.nix
 * is the system declaration (which packages, which services).
 *
 * File format (INI):
 *
 *   [bin]
 *   Makepkg = makepkg
 *   Git = git
 *   Gpg = gpg
 *   Sudo = sudo
 *   MFlags = --skippgpcheck --nocheck
 *   GitFlags = --depth 1
 *
 *   [chroot]
 *   Enabled = yes
 *   Dir = /var/lib/2O9/chroot
 *
 * Lines starting with # or ; are comments. Whitespace around key and
 * value is trimmed. Values may be wrapped in single or double quotes.
 *
 * Missing file = use defaults (chroot on, default binaries, no mflags).
 *
 * The CLI (src/cli/main.c) is expected to call two9_config_load() and
 * populate build_config_t from it. aur_build.c also calls it as a
 * fallback when build_config_t fields are unset.
 */
#ifndef TWO9_CONFIG_H
#define TWO9_CONFIG_H

typedef struct two9_config {
        char **mflags;          /* NULL-terminated list, from [bin] MFlags */
        char **git_flags;       /* NULL-terminated list, from [bin] GitFlags */
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

/* Load config from ~/.config/2O9/2O9.conf.
 * Returns a config struct with defaults if the file is missing.
 * Never returns NULL. Caller must free with two9_config_free(). */
two9_config_t *two9_config_load(void);

/* Free a config struct and all owned strings/lists. NULL-safe. */
void two9_config_free(two9_config_t *cfg);

#endif /* TWO9_CONFIG_H */
