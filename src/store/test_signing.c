/* test_signing.c - unit tests for Ed25519 signing + base64 helpers
 *
 * Covers signing_keygen/signing_sign/signing_verify, tamper detection
 * (fingerprint, signature, wrong public key), b64 round-trip on edge
 * lengths (empty, 32, 64 bytes), and the Nix-compatible fingerprint
 * format ("1;<path>;<nar-hash>;<nar-size>;<refs>").
 *
 * Build: see Makefile target `test-signing`.
 * Run: ./test-signing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "signing.h"

static int test_count = 0;
static int pass_count = 0;

#define OK(msg) do { printf("PASS: %s\n", msg); pass_count++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

#define CHECK(cond, name) do { \
        test_count++; \
        if (cond) OK(name); \
        else FAIL(name); \
} while (0)

/* ── Keygen + sign + verify round-trip ────────────────────────────── */

static void test_keygen_produces_32_byte_keys(void)
{
        unsigned char pub[32], sec[32];
        int rc = signing_keygen(pub, sec);
        CHECK(rc == 0, "keygen: returns 0");

        /* Keys should be non-zero (1 in 2^256 chance of all-zero, but
         * in practice a constant-zero result means a bug). */
        int pub_nonzero = 0, sec_nonzero = 0;
        for (int i = 0; i < 32; i++) {
                if (pub[i] != 0) pub_nonzero = 1;
                if (sec[i] != 0) sec_nonzero = 1;
        }
        CHECK(pub_nonzero, "keygen: public key is non-zero");
        CHECK(sec_nonzero, "keygen: secret key is non-zero");
}

static void test_sign_returns_nonempty_b64(void)
{
        unsigned char pub[32], sec[32];
        if (signing_keygen(pub, sec) != 0) { CHECK(0, "sign: keygen"); return; }

        const char *fp = "1;/nix/store/abc-test-1.0;sha256:deadbeef;100;";
        char *sig = signing_sign(fp, sec);
        CHECK(sig != NULL, "sign: returns non-NULL");
        CHECK(strlen(sig) > 0, "sign: signature is non-empty");

        /* Ed25519 sig is 64 raw bytes -> 88 base64 chars (with padding
         * '==' since 64 % 3 == 1). */
        CHECK(strlen(sig) == 88, "sign: signature length is 88 chars (64 bytes b64)");

        free(sig);
}

static void test_verify_valid_sig(void)
{
        unsigned char pub[32], sec[32];
        if (signing_keygen(pub, sec) != 0) { CHECK(0, "verify: keygen"); return; }

        const char *fp = "1;/nix/store/abc-test-1.0;sha256:feedface;200;ref1 ref2";
        char *sig = signing_sign(fp, sec);
        char *pub_b64 = b64_encode(pub, 32);
        if (!sig || !pub_b64) { CHECK(0, "verify: sign/encode"); free(sig); free(pub_b64); return; }

        int rc = signing_verify(fp, sig, pub_b64);
        CHECK(rc == 1, "verify: valid signature returns 1");

        free(sig); free(pub_b64);
}

static void test_verify_tampered_fingerprint(void)
{
        unsigned char pub[32], sec[32];
        if (signing_keygen(pub, sec) != 0) { CHECK(0, "tamper-fp: keygen"); return; }

        const char *fp_orig = "1;/nix/store/abc-test-1.0;sha256:feedface;200;";
        const char *fp_tampered = "1;/nix/store/abc-test-1.0;sha256:feedfacf;200;"; /* 1-bit flip */
        char *sig = signing_sign(fp_orig, sec);
        char *pub_b64 = b64_encode(pub, 32);
        if (!sig || !pub_b64) { CHECK(0, "tamper-fp: sign/encode"); free(sig); free(pub_b64); return; }

        int rc = signing_verify(fp_tampered, sig, pub_b64);
        CHECK(rc == 0, "tamper-fp: tampered fingerprint returns 0");

        free(sig); free(pub_b64);
}

static void test_verify_tampered_signature(void)
{
        unsigned char pub[32], sec[32];
        if (signing_keygen(pub, sec) != 0) { CHECK(0, "tamper-sig: keygen"); return; }

        const char *fp = "1;/nix/store/abc-test-1.0;sha256:feedface;200;";
        char *sig = signing_sign(fp, sec);
        char *pub_b64 = b64_encode(pub, 32);
        if (!sig || !pub_b64) { CHECK(0, "tamper-sig: sign/encode"); free(sig); free(pub_b64); return; }

        /* Flip a character in the signature. If it's the last char and
         * happens to be '=', move to an earlier char. */
        size_t slen = strlen(sig);
        if (slen > 0 && sig[slen - 1] != '=') {
                sig[slen - 1] = (sig[slen - 1] == 'A') ? 'B' : 'A';
        } else if (slen > 2) {
                sig[0] = (sig[0] == 'A') ? 'B' : 'A';
        }

        int rc = signing_verify(fp, sig, pub_b64);
        CHECK(rc == 0, "tamper-sig: tampered signature returns 0");

        free(sig); free(pub_b64);
}

static void test_verify_wrong_pubkey(void)
{
        unsigned char pub1[32], sec1[32];
        unsigned char pub2[32], sec2[32];
        if (signing_keygen(pub1, sec1) != 0 || signing_keygen(pub2, sec2) != 0) {
                CHECK(0, "wrong-pub: keygen"); return;
        }

        const char *fp = "1;/nix/store/abc-test-1.0;sha256:feedface;200;";
        char *sig = signing_sign(fp, sec1);
        char *pub2_b64 = b64_encode(pub2, 32);  /* Note: pub2, not pub1 */
        if (!sig || !pub2_b64) { CHECK(0, "wrong-pub: sign/encode"); free(sig); free(pub2_b64); return; }

        int rc = signing_verify(fp, sig, pub2_b64);
        CHECK(rc == 0, "wrong-pub: verify with wrong public key returns 0");

        free(sig); free(pub2_b64);
}

/* ── Base64 round-trip on edge lengths ────────────────────────────── */

static void test_b64_roundtrip(const unsigned char *data, size_t len, const char *desc)
{
        char *enc = b64_encode(data, len);
        test_count++;
        if (!enc) { FAIL(desc); return; }

        unsigned char *dec = malloc(len + 16);
        if (!dec) { free(enc); FAIL(desc); return; }
        size_t dec_len = len + 16;
        int rc = b64_decode(enc, dec, &dec_len);

        int ok = (rc == 0) && (dec_len == len) &&
                 (len == 0 || memcmp(dec, data, len) == 0);
        if (ok) OK(desc); else FAIL(desc);

        free(enc); free(dec);
}

static void test_b64_edge_cases(void)
{
        /* Empty input. */
        test_b64_roundtrip((const unsigned char *)"", 0,
                           "b64: empty input round-trips");

        /* 1 byte. */
        unsigned char one[1] = { 0xFF };
        test_b64_roundtrip(one, 1, "b64: 1-byte input round-trips");

        /* 2 bytes (1 byte short of full group). */
        unsigned char two[2] = { 0x01, 0x02 };
        test_b64_roundtrip(two, 2, "b64: 2-byte input round-trips");

        /* 3 bytes (exactly one group). */
        unsigned char three[3] = { 'a', 'b', 'c' };
        test_b64_roundtrip(three, 3, "b64: 3-byte input round-trips");

        /* 32 bytes (Ed25519 pubkey size). */
        unsigned char thirty_two[32];
        for (int i = 0; i < 32; i++) thirty_two[i] = (unsigned char)(i * 7 + 1);
        test_b64_roundtrip(thirty_two, 32, "b64: 32-byte (pubkey) round-trips");

        /* 64 bytes (Ed25519 signature size). */
        unsigned char sixty_four[64];
        for (int i = 0; i < 64; i++) sixty_four[i] = (unsigned char)(i * 3 + 5);
        test_b64_roundtrip(sixty_four, 64, "b64: 64-byte (signature) round-trips");

        /* 100 bytes (non-multiple of 3, exercises padding logic). */
        unsigned char hundred[100];
        for (int i = 0; i < 100; i++) hundred[i] = (unsigned char)(i ^ 0xAA);
        test_b64_roundtrip(hundred, 100, "b64: 100-byte (non-aligned) round-trips");
}

/* ── Fingerprint format ───────────────────────────────────────────── */

static void test_fingerprint_format(void)
{
        /* No references: "1;<path>;<hash>;<size>;" with empty refs. */
        char *fp = signing_fingerprint("/nix/store/abc-test-1.0",
                                       "sha256:deadbeef", 100, NULL);
        CHECK(fp != NULL, "fingerprint: returns non-NULL (no refs)");

        const char *expected_noref = "1;/nix/store/abc-test-1.0;sha256:deadbeef;100;";
        CHECK(strcmp(fp, expected_noref) == 0,
              "fingerprint: matches expected format with no refs");
        free(fp);

        /* With references: refs joined by single space after final ';'. */
        char *refs[] = { "dep1-1.0", "dep2-2.0", NULL };
        char *fp2 = signing_fingerprint("/nix/store/abc-test-1.0",
                                        "sha256:deadbeef", 100, refs);
        CHECK(fp2 != NULL, "fingerprint: returns non-NULL (with refs)");

        const char *expected_ref = "1;/nix/store/abc-test-1.0;sha256:deadbeef;100;dep1-1.0 dep2-2.0";
        CHECK(strcmp(fp2, expected_ref) == 0,
              "fingerprint: matches expected format with refs");
        free(fp2);
}

static void test_fingerprint_prefixes_sha256(void)
{
        /* Caller passes raw hex (no "sha256:" prefix); the impl should
         * prepend "sha256:" so the result is well-formed. */
        char *fp = signing_fingerprint("/nix/store/abc-test-1.0",
                                       "deadbeef", 100, NULL);
        CHECK(fp != NULL, "fingerprint: raw hex returns non-NULL");
        CHECK(strstr(fp, "sha256:deadbeef") != NULL,
              "fingerprint: prepends sha256: to raw hex");
        free(fp);
}

int main(void)
{
        printf("=== Ed25519 signing + base64 tests ===\n\n");

        test_keygen_produces_32_byte_keys();
        test_sign_returns_nonempty_b64();
        test_verify_valid_sig();
        test_verify_tampered_fingerprint();
        test_verify_tampered_signature();
        test_verify_wrong_pubkey();
        test_b64_edge_cases();
        test_fingerprint_format();
        test_fingerprint_prefixes_sha256();

        printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
        return pass_count == test_count ? 0 : 1;
}
