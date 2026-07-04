/* share.h - NAR file sharing
 *
 * A share is a NAR blob of an arbitrary path, stored in the 2O9 store
 * as `/nix/store/<base32-hash>-share-<basename>/`. The hash is computed
 * the same way as package paths: SHA-256 of the canonical NAR
 * serialisation, fed into compute_store_path() with name="share" and
 * version="<basename>". Two shares of the same input path produce the
 * same store path.
 *
 * Shares are pushed to configured subs (extra.nix `subs` block) signed
 * with each sub's SigningKey, and fetched back by hash. The cache index
 * (index.json) records each pushed share so `209 subs` can list them.
 *
 * URI scheme: `nar://<hash>` (or just `<hash>`). The hash is the
 * base32 store-path hash, the 32-char prefix before the first '-' in
 * the store path basename.
 */
#ifndef TWO9_SHARE_H
#define TWO9_SHARE_H

#include <stdint.h>
#include <stddef.h>

/* Forward decl - two9_sub_t is defined in aur/config.h. */
typedef struct two9_sub two9_sub_t;

/* One local share entry, as returned by share_list_local(). */
typedef struct share_entry {
        char *store_path;       /* /nix/store/<hash>-share-<basename> */
        char *hash;             /* 32-char base32 hash prefix */
        char *name;             /* basename (the part after "share-") */
        int64_t nar_size;       /* NAR serialised byte count, from stat */
        int64_t shared_at;      /* mtime of the store path dir, Unix seconds */
        struct share_entry *next;
} share_entry_t;

/* Take a path and turn it into a share in /nix/store/.
 *
 *   path - absolute path to share. MUST be absolute. No ~ expansion,
 *          no relative paths. Caller-checked.
 *
 * NAR-hashes the path, computes the content-addressed store path
 * `/nix/store/<base32>-share-<basename>/`, copies the input tree into
 * that path (preserving files, dirs, symlinks, executable bits), and
 * returns the store path (malloc'd, caller frees). If the store path
 * already exists, returns a strdup of it without re-copying.
 *
 * Returns NULL on error (errno set). */
char *share_take(const char *path);

/* List all local shares in /nix/store/. Returns a linked list of
 * share_entry_t (caller frees with share_entry_list_free). Returns
 * NULL if no shares exist or on error. The list is sorted by name. */
share_entry_t *share_list_local(void);

/* Free a share_entry_t list (and each entry). NULL-safe. */
void share_entry_list_free(share_entry_t *head);

/* Remove a share from the store by hash. The hash is the 32-char
 * base32 prefix. Removes the matching `/nix/store/<hash>-share-*`
 * directory. Returns 0 on success, -1 if not found or on error. */
int share_remove(const char *hash);

/* Push a share to every URL in a single sub. The sub must have a
 * SigningKey configured; the share is signed with that key. Also
 * appends an index.json entry on each URL.
 *
 *   store_path - the share's /nix/store/<hash>-share-<basename> path
 *   sub        - the configured sub to push to
 *
 * Returns 0 on success, -1 on error (any URL failing fails the call,
 * but every URL is attempted). */
int share_push_to_sub(const char *store_path, const two9_sub_t *sub);

/* Fetch a share by URI from any configured sub.
 *
 *   uri   - "nar://<hash>" or just "<hash>"
 *   dest  - directory to extract into. Must exist. If NULL, uses the
 *           current working directory.
 *
 * Walks each sub URL in config order. For each URL: fetches
 * `<url>/<hash>.narinfo`, verifies the signature against ANY of the
 * sub's PublicKeys (or accepts if AllowUnsigned is true). On success,
 * downloads the NAR, decompresses, and extracts to <dest>. Returns 0
 * on the first success. Returns -1 if no sub has it or no signature
 * verifies. */
int share_fetch_from_sub(const char *uri, const char *dest);

/* Parse a share URI ("nar://<hash>" or "<hash>"). Writes a malloc'd
 * copy of the hash to *out_hash. Returns 0 on success, -1 on bad URI
 * (caller frees *out_hash). */
int share_parse_uri(const char *uri, char **out_hash);

#endif /* TWO9_SHARE_H */
