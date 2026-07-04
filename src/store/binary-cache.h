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
 * Trust model: if bc->public_keys is non-NULL, every fetched narinfo
 * must carry at least one Sig: line that verifies against ANY of the
 * listed public keys. If bc->allow_unsigned is true, unsigned narinfos
 * are accepted (with a warning). If neither (no keys configured and
 * allow_unsigned is false), lookup fails for unsigned narinfos.
 */
#ifndef TWO9_BINARY_CACHE_H
#define TWO9_BINARY_CACHE_H

#include "narinfo.h"
#include "db.h"
#include "cJSON.h"

typedef struct binary_cache {
        char *base_url;         /* e.g. https://cache.example.com or s3://my-bucket */
        char **public_keys;     /* NULL-terminated list of base64 Ed25519 pubkeys, may be NULL */
        int allow_unsigned;     /* 1 = accept unsigned narinfos */
} binary_cache_t;

/* Construct a cache client. base_url is required. public_keys is a
 * NULL-terminated list of base64 Ed25519 public keys (may be NULL);
 * a narinfo is accepted if ANY of them verifies its signature.
 * allow_unsigned controls whether unsigned narinfos are accepted.
 * The new binary_cache_t takes ownership of public_keys (caller must
 * not free it). Caller frees the cache with binary_cache_free().
 * NULL on alloc failure. */
binary_cache_t *binary_cache_new(const char *base_url,
                                  char **public_keys_b64,
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

/* Look up a narinfo by its 32-char base32 hash (the prefix of the
 * store path). Same semantics as binary_cache_lookup but skips the
 * store-path parsing. Returns NULL if not found or sig verification
 * fails. Caller frees with narinfo_free(). */
narinfo_t *binary_cache_lookup_by_hash(binary_cache_t *bc, const char *hash);

/* Download + decompress the NAR referenced by a narinfo. Writes a
 * malloc'd buffer of decompressed NAR bytes to *out_buf and the byte
 * count to *out_len. Caller frees *out_buf. Returns 0 on success,
 * -1 on error. */
int binary_cache_download_nar(binary_cache_t *bc, const narinfo_t *ni,
                              char **out_buf, size_t *out_len);

/* Fetch the cache's index.json. Returns a cJSON object (caller frees
 * with cJSON_Delete) or NULL if the index doesn't exist, fetch
 * failed, or parse failed. The returned object has the shape:
 *   { "version": 1, "updated_at": <ts>, "items": [ ... ] } */
cJSON *binary_cache_fetch_index(binary_cache_t *bc);

/* Append an item to the cache's index.json and re-upload. The item
 * is a cJSON object the caller built. The caller retains ownership
 * of item_json (this function copies it).
 * Returns 0 on success, -1 on error. Last-write-wins on race: two
 * publishers pushing at the same time may clobber each other's
 * index entries; the narinfo+nar files themselves are not affected. */
int binary_cache_push_index_item(binary_cache_t *bc, const cJSON *item_json);

#endif /* TWO9_BINARY_CACHE_H */
