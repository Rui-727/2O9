/* signing.h - Ed25519 detached signatures over path fingerprints
 *
 * Phase 3: 2O9 needs to sign narinfo files so a binary cache can prove
 * a NAR came from a trusted key. The scheme matches Nix's: each narinfo
 * gets one or more `Sig: <key-name>:<base64-sig>` lines, where the sig
 * is an Ed25519 signature over the path's fingerprint string:
 *
 *   "1;<store-path>;<nar-hash>;<nar-size>;<refs-space-separated>"
 *
 * (see nix/src/libstore/path-info.cc fingerprint()). The leading "1;" is
 * a version tag.
 *
 * Library selection: libsodium is preferred (smaller, easier API). The
 * Makefile probes for it via pkg-config and sets HAVE_SODIUM. If not
 * available, the implementation falls back to OpenSSL 1.1+ Ed25519 via
 * EVP_DigestSign / EVP_DigestVerify (already linked as -lcrypto).
 *
 * Key files: a single line of the form `<key-name>:<base64-public>:<base64-secret>`
 * where both keys are 32 raw bytes encoded as base64. The same format Nix
 * uses for its secret-key files (minus the trailing comment line).
 */
#ifndef TWO9_SIGNING_H
#define TWO9_SIGNING_H

#include <stddef.h>
#include <stdint.h>

/* Ed25519 key size in bytes. */
#define TWO9_ED25519_PUBKEY_LEN 32
#define TWO9_ED25519_SECKEY_LEN 32
#define TWO9_ED25519_SIG_LEN    64

/* Compute the path fingerprint string that gets signed.
 * Format: "1;<path>;<nar-hash>;<nar-size>;<refs-space-separated>"
 * Matches Nix's fingerprint() in nix/src/libstore/path-info.cc.
 *   store_path - absolute store path (e.g. /nix/store/<hash>-name-ver)
 *   nar_hash   - NAR hash in "sha256:<hex>" form (caller prefixes algo)
 *   nar_size   - NAR serialised byte count
 *   references - NULL-terminated list of store path basenames, or NULL
 * Returns a malloc'd string (caller frees) or NULL on alloc failure. */
char *signing_fingerprint(const char *store_path, const char *nar_hash,
                          int64_t nar_size, char **references);

/* Sign a fingerprint with Ed25519. Returns base64-encoded signature
 * (caller frees). NULL on error. secret_key must be 32 bytes. */
char *signing_sign(const char *fingerprint,
                   const unsigned char *secret_key);

/* Verify a base64-encoded Ed25519 signature over a fingerprint.
 * public_key_b64 is the base64-encoded 32-byte public key.
 * Returns 1 if valid, 0 if invalid, -1 on decode/library error. */
int signing_verify(const char *fingerprint, const char *signature_b64,
                   const char *public_key_b64);

/* Verify using a raw 32-byte public key. Same return as signing_verify. */
int signing_verify_raw(const char *fingerprint,
                       const unsigned char *signature /* 64 bytes */,
                       const unsigned char *public_key /* 32 bytes */);

/* Generate a new Ed25519 keypair. Writes 32 bytes to each buffer.
 * Returns 0 on success, -1 on error. */
int signing_keygen(unsigned char *public_key,
                   unsigned char *secret_key);

/* Base64 helpers (URL-safe not needed; standard alphabet).
 * b64_encode: returns malloc'd NUL-terminated string, caller frees.
 * b64_decode: writes at most *out_len bytes to out, sets *out_len to
 *             actual length. Returns 0 on success, -1 on error. */
char *b64_encode(const unsigned char *data, size_t len);
int b64_decode(const char *str, unsigned char *out, size_t *out_len);

/* Load a 2O9 secret-key file. Format (one logical line):
 *     <key-name>:<base64-public>:<base64-secret>
 * On success: *out_name is malloc'd, out_pub and out_sec each receive
 * 32 bytes. Returns 0 on success, -1 on error. */
int signing_load_keyfile(const char *path, char **out_name,
                         unsigned char *out_pub,
                         unsigned char *out_sec);

#endif /* TWO9_SIGNING_H */
