/* binary-cache.h - HTTP/S3 binary cache client for substitution
 *
 * Phase 3: 2O9 can pull store paths from any content-addressed cache
 * that publishes .narinfo + .nar.zst files. This module is the client
 * side: lookup a path, fetch+extract, and (for cache operators) push
 * a path's closure.
 *
 * Cache layout (matches Nix's binary-cache spec):
 *   <base>/<hash>.narinfo          - narinfo for store path with that hash
 *   <base>/<url-from-narinfo>      - compressed NAR file
 *
 * base_url examples:
 *   https://cache.example.com      - HTTP/HTTPS via libcurl
 *   s3://my-bucket/path            - S3 via the `aws` CLI (shelled out)
 *
 * Trust model: if bc->public_key is set, every fetched narinfo must
 * carry at least one Sig: line that verifies against that key. If
 * bc->allow_unsigned is true, unsigned narinfos are accepted (with a
 * warning). If neither, lookup fails for unsigned narinfos.
 */
#ifndef TWO9_BINARY_CACHE_H
#define TWO9_BINARY_CACHE_H

#include "narinfo.h"
#include "db.h"

typedef struct binary_cache {
        char *base_url;         /* e.g. https://cache.example.com or s3://my-bucket */
        char *public_key;       /* base64-encoded 32-byte Ed25519 public key, may be NULL */
        int allow_unsigned;     /* 1 = accept unsigned narinfos */
} binary_cache_t;

/* Construct a cache client. base_url is required. public_key and
 * allow_unsigned control signature verification. Caller frees with
 * binary_cache_free(). NULL on alloc failure. */
binary_cache_t *binary_cache_new(const char *base_url,
                                  const char *public_key_b64,
                                  int allow_unsigned);
void binary_cache_free(binary_cache_t *bc);

/* Look up a store path on the cache. Returns NULL if not found or
 * signature verification fails. Caller must narinfo_free() the result.
 *
 *   bc          - the cache
 *   store_path  - absolute /nix/store/<hash>-<name>-<version>
 *
 * The lookup URL is <base_url>/<hash>.narinfo where <hash> is the
 * 32-char base32 prefix of the store path. */
narinfo_t *binary_cache_lookup(binary_cache_t *bc, const char *store_path);

/* Download the NAR for a narinfo, decompress, stream-extract to a
 * temp dir, then rename to the final store path.
 * Returns 0 on success, -1 on error. */
int binary_cache_fetch(binary_cache_t *bc, const narinfo_t *ni,
                       const char *final_store_path);

/* Upload a narinfo + NAR to the cache. Used by `209 cache push`.
 *   bc                  - the cache
 *   store_path          - absolute /nix/store/<hash>-<name>-<version>
 *   db                  - store DB (for refs), may be NULL
 *   signing_key_name    - narinfo Sig: key name, may be NULL
 *   signing_secret_key  - 32-byte Ed25519 secret, may be NULL
 * Returns 0 on success, -1 on error. */
int binary_cache_push(binary_cache_t *bc, const char *store_path,
                      store_db_t *db, const char *signing_key_name,
                      const unsigned char *signing_secret_key);

#endif /* TWO9_BINARY_CACHE_H */
