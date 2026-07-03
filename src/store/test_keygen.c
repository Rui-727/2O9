/* test_keygen.c - Ed25519 keygen round-trip verifier (CLI helper)
 *
 * Reads a base64 public key + base64 secret key from argv, signs a
 * test fingerprint with the secret, verifies the signature with the
 * public key, and prints OK or FAIL. Used by the shell test
 * test_cache_keygen_roundtrip.sh to confirm `209 keygen` output is
 * well-formed and the keypair actually works for sign + verify.
 *
 * Build: see Makefile target `test-keygen`.
 * Run: ./test-keygen <pub_b64> <sec_b64>
 *   Exit 0 on success, 1 on failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "signing.h"

int main(int argc, char **argv)
{
        if (argc != 3) {
                fprintf(stderr, "usage: %s <pub_b64> <sec_b64>\n", argv[0]);
                return 1;
        }
        const char *pub_b64 = argv[1];
        const char *sec_b64 = argv[2];

        /* Decode pub + sec; verify each is exactly 32 bytes. */
        unsigned char pub[32], sec[32];
        size_t pub_len = sizeof(pub);
        size_t sec_len = sizeof(sec);
        if (b64_decode(pub_b64, pub, &pub_len) != 0 || pub_len != 32) {
                printf("FAIL: pub_b64 does not decode to 32 bytes (got %zu)\n", pub_len);
                return 1;
        }
        if (b64_decode(sec_b64, sec, &sec_len) != 0 || sec_len != 32) {
                printf("FAIL: sec_b64 does not decode to 32 bytes (got %zu)\n", sec_len);
                return 1;
        }
        printf("OK: pub decodes to 32 bytes\n");
        printf("OK: sec decodes to 32 bytes\n");

        /* Construct a realistic fingerprint (matches the format used
         * by narinfo signing). */
        char *fp = signing_fingerprint(
                "/nix/store/abc-test-keygen-1.0",
                "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                12345, NULL);
        if (!fp) {
                printf("FAIL: signing_fingerprint returned NULL\n");
                return 1;
        }

        /* Sign + verify round-trip. */
        char *sig = signing_sign(fp, sec);
        if (!sig) {
                printf("FAIL: signing_sign returned NULL\n");
                free(fp);
                return 1;
        }

        int rc = signing_verify(fp, sig, pub_b64);
        if (rc == 1) {
                printf("OK: signing_verify(pub_b64) returns 1 for round-trip\n");
        } else {
                printf("FAIL: signing_verify returned %d (expected 1)\n", rc);
                free(fp); free(sig);
                return 1;
        }

        /* Negative control: tampering with the fingerprint must fail. */
        char *fp_bad = strdup(fp);
        if (fp_bad) {
                /* Flip the last character of the path component. */
                char *p = strrchr(fp_bad, ';');
                if (p) {
                        p++;
                        if (*p && p[strlen(p) - 1] == '0')
                                p[strlen(p) - 1] = '1';
                        else
                                p[strlen(p) - 1] = '0';
                }
                int rc_bad = signing_verify(fp_bad, sig, pub_b64);
                if (rc_bad == 0) {
                        printf("OK: tampered fingerprint rejected (returns 0)\n");
                } else {
                        printf("FAIL: tampered fingerprint not rejected (rc=%d)\n", rc_bad);
                        free(fp_bad); free(fp); free(sig);
                        return 1;
                }
                free(fp_bad);
        }

        free(fp);
        free(sig);
        printf("All keygen round-trip checks passed.\n");
        return 0;
}
