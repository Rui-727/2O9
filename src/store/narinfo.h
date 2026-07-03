/* narinfo.h - NAR metadata files for binary caches
 *
 * Phase 3: a binary cache (HTTP server, S3 bucket) advertises packages
 * via .narinfo files - one per store path. Each is a small text file
 * with `Key: Value` lines describing where the NAR lives, its hash,
 * references, and detached signatures.
 *
 * The format mirrors Nix's nar-info.cc exactly so a 2O9 cache can be
 * consumed by Nix tooling (given a Nix-compatible NAR serialiser) and
 * vice versa. The serialiser we ship today is 2O9's variant (see
 * nar.c) - byte-incompatible with Nix's NAR but the narinfo format
 * itself is identical.
 *
 * Lookup: GET <base_url>/<hash>.narinfo where <hash> is the hash part
 * of the store path (the 32-char base32 prefix before the first '-').
 *
 * Signatures: one or more `Sig: <key-name>:<base64-ed25519-sig>` lines.
 * The signature covers signing_fingerprint() of the path - see signing.h.
 */
#ifndef TWO9_NARINFO_H
#define TWO9_NARINFO_H

#include <stdint.h>
#include <stddef.h>

/* Forward decl - store_db_t is defined in db.h. */
typedef struct store_db store_db_t;

typedef struct narinfo {
        char *store_path;       /* /nix/store/<hash>-<name>-<version> */
        char *url;              /* relative URL on the cache, e.g. nar/<hash>.nar.zst */
        char *compression;      /* "zstd", "xz", "bzip2", "none" */
        char *file_hash;        /* "sha256:<hex>" - compressed NAR hash */
        int64_t file_size;      /* compressed NAR byte count */
        char *nar_hash;         /* "sha256:<hex>" - uncompressed NAR hash */
        int64_t nar_size;       /* uncompressed NAR byte count */
        char **references;      /* NULL-terminated list of store path basenames */
        char *deriver;          /* may be NULL */
        char **signatures;      /* NULL-terminated list of "keyname:base64sig" */
        char *ca;               /* content address, may be NULL */
} narinfo_t;

/* Parse a narinfo text blob. Returns NULL on parse failure (caller may
 * free the partial result). Caller frees with narinfo_free(). */
narinfo_t *narinfo_parse(const char *text);

/* Serialise a narinfo back to text. Returns malloc'd NUL-terminated
 * string. Caller frees. NULL on error. */
char *narinfo_serialize(const narinfo_t *ni);

/* Free a narinfo and all owned strings. NULL-safe. */
void narinfo_free(narinfo_t *ni);

/* Build a narinfo from a store path on disk.
 *   db           - store DB for refs lookup (may be NULL; refs left empty)
 *   store_path   - absolute path under /nix/store/
 *   signing_key_name - narinfo Sig: key-name field, may be NULL (skip sig)
 *   signing_secret_key - 32-byte Ed25519 secret key, may be NULL (skip sig)
 *
 * Computes the NAR hash + size by re-serialising the tree (nar_dump),
 * walks the refs graph for References:, optionally signs.
 *
 * file_hash/file_size (the compressed-NAR fields) are left NULL/0 -
 * the binary-cache push path fills them in after compressing the NAR.
 *
 * Returns NULL on error. Caller frees with narinfo_free(). */
narinfo_t *narinfo_from_store_path(store_db_t *db, const char *store_path,
                                    const char *signing_key_name,
                                    const unsigned char *signing_secret_key);

/* Convenience: extract the hash part of a store path (the prefix before
 * the first '-' after the /nix/store/). Returns pointer into store_path
 * (no allocation); writes the length to *len_out. NULL on bad path. */
const char *narinfo_path_hash(const char *store_path, size_t *len_out);

/* Does the narinfo have at least one valid signature from the given
 * public key (base64)? Returns 1 yes, 0 no, -1 on error. */
int narinfo_verify_signed(const narinfo_t *ni, const char *public_key_b64);

#endif /* TWO9_NARINFO_H */
