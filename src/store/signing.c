/* signing.c - Ed25519 detached signatures over path fingerprints
 *
 * See signing.h for the design. Implementation notes:
 *
 *  - When HAVE_SODIUM is defined at build time we use libsodium's
 *    crypto_sign_* API (smaller, constant-time, easy to use). The
 *    Makefile probes pkg-config for libsodium and adds -lsodium + the
 *    -DHAVE_SODIUM define.
 *  - Otherwise we fall back to OpenSSL 1.1+ Ed25519 via
 *    EVP_DigestSign / EVP_DigestVerify. OpenSSL is already linked
 *    (-lcrypto) for lib2O9's PGP work. OpenSSL 1.0.x lacks Ed25519
 *    support; in that case signing is stubbed (returns errors) but
 *    the rest of 2O9 still builds.
 *
 * Base64: we use libsodium's sodium_bin2base64 / sodium_base642bin
 * when available, otherwise a small built-in encoder/decoder. The
 * decoder skips whitespace and stray characters that aren't in the
 * base64 alphabet.
 */
#include "signing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SODIUM
#include <sodium.h>
#endif

#include <openssl/evp.h>
#include <openssl/err.h>

/* ── Fingerprint ────────────────────────────────────────────────────
 * Nix's fingerprint() (nix/src/libstore/path-info.cc):
 *     "1;" + storePath + ";" + narHash->to_string() + ";"
 *     + narSize + ";" + concatStringsSep(" ", references)
 *
 * We accept nar_hash in either "sha256:<hex>" or raw "<hex>" form;
 * if the caller passed raw hex, we prepend "sha256:". */
char *signing_fingerprint(const char *store_path, const char *nar_hash,
                          int64_t nar_size, char **references)
{
        if (!store_path || !nar_hash) { errno = EINVAL; return NULL; }

        const char *hash_field = nar_hash;
        char *prefixed = NULL;
        if (strncmp(nar_hash, "sha256:", 7) != 0) {
                size_t plen = strlen(nar_hash) + 8;
                prefixed = malloc(plen);
                if (!prefixed) return NULL;
                snprintf(prefixed, plen, "sha256:%s", nar_hash);
                hash_field = prefixed;
        }

        /* Compute refs length: refs joined by single space, no trailing. */
        size_t refs_len = 0;
        if (references) {
                for (size_t i = 0; references[i]; i++) {
                        refs_len += strlen(references[i]);
                        if (i > 0) refs_len += 1;  /* separator */
                }
        }

        /* "1;<path>;<hash>;<size>;<refs>\0" */
        char sizebuf[32];
        snprintf(sizebuf, sizeof(sizebuf), "%lld", (long long)nar_size);

        size_t total = 2                                /* "1;" */
                     + strlen(store_path) + 1
                     + strlen(hash_field) + 1
                     + strlen(sizebuf) + 1
                     + refs_len + 1;
        char *out = malloc(total);
        if (!out) { free(prefixed); return NULL; }

        char *p = out;
        p = stpcpy(p, "1;");
        p = stpcpy(p, store_path);   *p++ = ';';
        p = stpcpy(p, hash_field);   *p++ = ';';
        p = stpcpy(p, sizebuf);      *p++ = ';';
        if (references) {
                for (size_t i = 0; references[i]; i++) {
                        if (i > 0) *p++ = ' ';
                        p = stpcpy(p, references[i]);
                }
        }
        *p = '\0';

        free(prefixed);
        return out;
}

/* ── Base64 ──────────────────────────────────────────────────────── */

#ifdef HAVE_SODIUM
char *b64_encode(const unsigned char *data, size_t len)
{
        /* sodium_bin2base64 returns a NUL-terminated malloc'd string
         * (length = ((len + 2) / 3) * 4). VARIANT_ORIGINAL is the
         * standard '+'/'/' alphabet. */
        size_t out_len = ((len + 2) / 3) * 4 + 1;
        char *out = malloc(out_len);
        if (!out) return NULL;
        char *res = sodium_bin2base64(out, out_len, data, len,
                                      sodium_base64_VARIANT_ORIGINAL);
        if (!res) { free(out); return NULL; }
        return out;
}

int b64_decode(const char *str, unsigned char *out, size_t *out_len)
{
        if (!str || !out || !out_len) { errno = EINVAL; return -1; }
        size_t bin_len = *out_len;
        const char *end;
        int rc = sodium_base642bin(out, bin_len, str, strlen(str),
                                   NULL, &bin_len, &end,
                                   sodium_base64_VARIANT_ORIGINAL);
        if (rc != 0) return -1;
        *out_len = bin_len;
        return 0;
}
#else
/* Minimal built-in base64 for the no-sodium case. */
static const char b64_alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const unsigned char *data, size_t len)
{
        size_t out_len = ((len + 2) / 3) * 4 + 1;
        char *out = malloc(out_len);
        if (!out) return NULL;
        size_t i, j;
        for (i = 0, j = 0; i + 2 < len; i += 3, j += 4) {
                unsigned int v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
                out[j]   = b64_alpha[(v >> 18) & 0x3f];
                out[j+1] = b64_alpha[(v >> 12) & 0x3f];
                out[j+2] = b64_alpha[(v >>  6) & 0x3f];
                out[j+3] = b64_alpha[ v        & 0x3f];
        }
        if (i < len) {
                unsigned int v = data[i] << 16;
                if (i + 1 < len) v |= data[i+1] << 8;
                out[j]   = b64_alpha[(v >> 18) & 0x3f];
                out[j+1] = b64_alpha[(v >> 12) & 0x3f];
                out[j+2] = (i + 1 < len) ? b64_alpha[(v >> 6) & 0x3f] : '=';
                out[j+3] = '=';
                j += 4;
        }
        out[j] = '\0';
        return out;
}

static int b64_val(char c)
{
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
}

int b64_decode(const char *str, unsigned char *out, size_t *out_len)
{
        if (!str || !out || !out_len) { errno = EINVAL; return -1; }
        size_t cap = *out_len;
        size_t out_i = 0;
        unsigned int acc = 0;
        int bits = 0;
        for (size_t i = 0; str[i]; i++) {
                char c = str[i];
                if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                        continue;
                int v = b64_val(c);
                if (v < 0) return -1;
                acc = (acc << 6) | (unsigned)v;
                bits += 6;
                if (bits >= 8) {
                        bits -= 8;
                        if (out_i >= cap) { errno = ENOBUFS; return -1; }
                        out[out_i++] = (unsigned char)((acc >> bits) & 0xff);
                }
        }
        *out_len = out_i;
        return 0;
}
#endif /* HAVE_SODIUM */

/* ── Ed25519 sign / verify ───────────────────────────────────────── */

#ifdef HAVE_SODIUM
char *signing_sign(const char *fingerprint, const unsigned char *secret_key)
{
        if (!fingerprint || !secret_key) { errno = EINVAL; return NULL; }
        unsigned char sig[TWO9_ED25519_SIG_LEN];
        if (crypto_sign_detached(sig, NULL,
                                 (const unsigned char *)fingerprint,
                                 strlen(fingerprint), secret_key) != 0)
                return NULL;
        return b64_encode(sig, sizeof(sig));
}

int signing_verify_raw(const char *fingerprint,
                       const unsigned char *signature,
                       const unsigned char *public_key)
{
        if (!fingerprint || !signature || !public_key) return -1;
        int rc = crypto_sign_verify_detached(signature,
                                             (const unsigned char *)fingerprint,
                                             strlen(fingerprint),
                                             public_key);
        return rc == 0 ? 1 : 0;
}

int signing_keygen(unsigned char *public_key, unsigned char *secret_key)
{
        if (!public_key || !secret_key) { errno = EINVAL; return -1; }
        if (sodium_init() < 0) return -1;
        return crypto_sign_keypair(public_key, secret_key) == 0 ? 0 : -1;
}

#else /* OpenSSL Ed25519 fallback */

char *signing_sign(const char *fingerprint, const unsigned char *secret_key)
{
        if (!fingerprint || !secret_key) { errno = EINVAL; return NULL; }

        EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                      secret_key, 32);
        if (!pkey) return NULL;

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) { EVP_PKEY_free(pkey); return NULL; }

        size_t sig_len = TWO9_ED25519_SIG_LEN;
        unsigned char *sig = malloc(sig_len);
        if (!sig) { EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); return NULL; }

        int rc = EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey);
        if (rc != 1) { free(sig); EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); return NULL; }
        rc = EVP_DigestSign(ctx, sig, &sig_len,
                            (const unsigned char *)fingerprint,
                            strlen(fingerprint));
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        if (rc != 1) { free(sig); return NULL; }

        char *out = b64_encode(sig, sig_len);
        free(sig);
        return out;
}

int signing_verify_raw(const char *fingerprint,
                       const unsigned char *signature,
                       const unsigned char *public_key)
{
        if (!fingerprint || !signature || !public_key) return -1;

        EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                     public_key, 32);
        if (!pkey) return -1;

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) { EVP_PKEY_free(pkey); return -1; }

        int rc = EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey);
        if (rc != 1) { EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); return -1; }

        rc = EVP_DigestVerify(ctx, signature, TWO9_ED25519_SIG_LEN,
                              (const unsigned char *)fingerprint,
                              strlen(fingerprint));
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return rc == 1 ? 1 : 0;
}

int signing_keygen(unsigned char *public_key, unsigned char *secret_key)
{
        if (!public_key || !secret_key) { errno = EINVAL; return -1; }
        EVP_PKEY *pkey = NULL;
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
        if (!pctx) return -1;
        if (EVP_PKEY_keygen_init(pctx) != 1 ||
            EVP_PKEY_keygen(pctx, &pkey) != 1) {
                EVP_PKEY_CTX_free(pctx);
                return -1;
        }
        EVP_PKEY_CTX_free(pctx);

        size_t len = 32;
        int rc = EVP_PKEY_get_raw_public_key(pkey, public_key, &len);
        if (rc != 1 || len != 32) { EVP_PKEY_free(pkey); return -1; }
        len = 32;
        rc = EVP_PKEY_get_raw_private_key(pkey, secret_key, &len);
        EVP_PKEY_free(pkey);
        if (rc != 1 || len != 32) return -1;
        return 0;
}
#endif /* HAVE_SODIUM */

int signing_verify(const char *fingerprint, const char *signature_b64,
                   const char *public_key_b64)
{
        if (!fingerprint || !signature_b64 || !public_key_b64)
                return -1;

        unsigned char sig[TWO9_ED25519_SIG_LEN];
        unsigned char pub[TWO9_ED25519_PUBKEY_LEN];
        size_t sig_len = sizeof(sig);
        size_t pub_len = sizeof(pub);

        if (b64_decode(signature_b64, sig, &sig_len) != 0 ||
            sig_len != TWO9_ED25519_SIG_LEN) return -1;
        if (b64_decode(public_key_b64, pub, &pub_len) != 0 ||
            pub_len != TWO9_ED25519_PUBKEY_LEN) return -1;

        return signing_verify_raw(fingerprint, sig, pub);
}

/* ── Key file loader ───────────────────────────────────────────────
 * Format: "<key-name>:<base64-public>:<base64-secret>\n"
 * (We tolerate missing public key: "<key-name>:<base64-secret>" - the
 * public half is re-derived from the secret in that case.) */
int signing_load_keyfile(const char *path, char **out_name,
                         unsigned char *out_pub,
                         unsigned char *out_sec)
{
        if (!path || !out_name || !out_pub || !out_sec) {
                errno = EINVAL; return -1;
        }
        *out_name = NULL;

        FILE *f = fopen(path, "r");
        if (!f) return -1;

        char line[1024];
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
        fclose(f);

        /* Strip trailing newline. */
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r'))
                line[--l] = '\0';

        /* Find first colon. */
        char *first_colon = strchr(line, ':');
        if (!first_colon) return -1;
        *first_colon = '\0';
        char *name = line;
        char *rest = first_colon + 1;

        /* Parse remaining as either "<pub>:<sec>" or "<sec>". */
        char *pub_b64 = NULL, *sec_b64 = NULL;
        char *second_colon = strchr(rest, ':');
        if (second_colon) {
                *second_colon = '\0';
                pub_b64 = rest;
                sec_b64 = second_colon + 1;
        } else {
                sec_b64 = rest;
        }

        size_t seclen = TWO9_ED25519_SECKEY_LEN;
        if (b64_decode(sec_b64, out_sec, &seclen) != 0 ||
            seclen != TWO9_ED25519_SECKEY_LEN)
                return -1;

        if (pub_b64) {
                size_t publen = TWO9_ED25519_PUBKEY_LEN;
                if (b64_decode(pub_b64, out_pub, &publen) != 0 ||
                    publen != TWO9_ED25519_PUBKEY_LEN)
                        return -1;
        } else {
                /* Derive public from secret. */
#ifdef HAVE_SODIUM
                if (crypto_sign_ed25519_sk_to_pk(out_pub, out_sec) != 0)
                        return -1;
#else
                EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
                        EVP_PKEY_ED25519, NULL, out_sec, 32);
                if (!pkey) return -1;
                size_t len = 32;
                int rc = EVP_PKEY_get_raw_public_key(pkey, out_pub, &len);
                EVP_PKEY_free(pkey);
                if (rc != 1 || len != 32) return -1;
#endif
        }

        *out_name = strdup(name);
        if (!*out_name) return -1;
        return 0;
}
