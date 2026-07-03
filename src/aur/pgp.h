/* pgp.h - PGP key auto-import for AUR builds
 *
 * Rewrites paru's src/keys.rs in C.
 *
 * Reads validpgpkeys from .SRCINFO, checks the local gpg keyring,
 * and prompts the user to import any missing keys via gpg --recv-keys.
 *
 * If the user declines, the build is aborted (it would fail at
 * `makepkg --verifysource` anyway).
 */
#ifndef TWO9_PGP_H
#define TWO9_PGP_H

/* Parse .SRCINFO from clone_dir, extract validpgpkeys=(...) entries.
 * If .SRCINFO doesn't exist, run `makepkg --printsrcinfo > .SRCINFO`
 * to generate it first.
 *
 * Returns a NULL-terminated list of fingerprints (caller frees each
 * string and the list itself via pgp_free_list()).
 * Returns NULL on error or when no validpgpkeys are declared. */
char **pgp_read_valid_keys(const char *clone_dir);

/* For each fingerprint, check if it's in the local keyring via
 * `gpg --list-keys`. Returns a NULL-terminated list of MISSING keys
 * (caller frees via pgp_free_list()).
 * Returns NULL if all keys are present or no keys were given. */
char **pgp_find_missing(char **keys);

/* Prompt the user (y/N) and import missing keys via `gpg --recv-keys`.
 * If gpg is not installed, warn and return -1.
 * If the user declines any prompt, return -1.
 * Returns 0 on success (all missing keys imported). */
int pgp_import_missing(char **missing);

/* Free a NULL-terminated list of strings (as returned by the functions
 * above). NULL-safe. */
void pgp_free_list(char **list);

#endif /* TWO9_PGP_H */
