/* Smoke test for Phase 3 narinfo + signing + nar_extract round-trip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "store/nar.h"
#include "store/narinfo.h"
#include "store/signing.h"

int main(void)
{
        const char *src = "/tmp/nar-smoke/tree";
        const char *nar_file = "/tmp/nar-smoke/tree.nar";
        const char *extracted = "/tmp/nar-smoke/extracted";

        /* 1. Dump NAR. */
        FILE *out = fopen(nar_file, "wb");
        if (!out) { perror("fopen nar"); return 1; }
        if (nar_dump(src, out) != 0) { fprintf(stderr, "nar_dump failed\n"); return 1; }
        fclose(out);

        /* 2. Compute NAR hash. */
        char hash_hex[65];
        size_t nar_size = 0;
        if (nar_hash_directory(src, hash_hex, &nar_size) != 0) {
                fprintf(stderr, "nar_hash_directory failed\n");
                return 1;
        }
        printf("NAR hash: %s (size %zu)\n", hash_hex, nar_size);

        /* 3. Extract NAR back to a fresh dir. */
        rmdir(extracted);
        if (mkdir(extracted, 0755) != 0) { perror("mkdir extracted"); return 1; }
        FILE *in = fopen(nar_file, "rb");
        if (!in) { perror("fopen nar"); return 1; }
        if (nar_extract(in, extracted) != 0) {
                fprintf(stderr, "nar_extract failed\n");
                return 1;
        }
        fclose(in);
        printf("nar_extract: OK\n");

        /* 4. Re-hash extracted tree, verify same hash. */
        char hash2[65];
        if (nar_hash_directory(extracted, hash2, NULL) != 0) {
                fprintf(stderr, "re-hash failed\n");
                return 1;
        }
        if (strcmp(hash_hex, hash2) != 0) {
                fprintf(stderr, "HASH MISMATCH: %s vs %s\n", hash_hex, hash2);
                return 1;
        }
        printf("round-trip hash matches: %s\n", hash2);

        /* 5. Verify extracted contents. */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "diff -r %s %s", src, extracted);
        int rc = system(cmd);
        if (rc != 0) { fprintf(stderr, "diff failed\n"); return 1; }
        printf("tree contents match: OK\n");

        /* 6. Test signing. */
        unsigned char pub[32], sec[32];
        if (signing_keygen(pub, sec) != 0) {
                fprintf(stderr, "keygen failed\n");
                return 1;
        }
        char *fp = signing_fingerprint("/nix/store/abc-test-1.0",
                                        "sha256:abcd1234", 1234, NULL);
        if (!fp) { fprintf(stderr, "fingerprint failed\n"); return 1; }
        printf("fingerprint: %s\n", fp);
        char *sig = signing_sign(fp, sec);
        if (!sig) { fprintf(stderr, "sign failed\n"); return 1; }
        printf("signature: %s\n", sig);
        char *pub_b64 = b64_encode(pub, 32);
        int verified = signing_verify(fp, sig, pub_b64);
        printf("verify: %s\n", verified == 1 ? "OK" : "FAIL");
        free(fp); free(sig); free(pub_b64);
        if (verified != 1) return 1;

        /* 7. Test narinfo parse/serialize round-trip. */
        const char *ni_text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "Compression: zstd\n"
                "FileHash: sha256:aaaa\n"
                "FileSize: 100\n"
                "NarHash: sha256:bbbb\n"
                "NarSize: 200\n"
                "References: dep1-1.0 dep2-2.0\n"
                "Sig: mykey:cccc\n";
        narinfo_t *ni = narinfo_parse(ni_text);
        if (!ni) { fprintf(stderr, "parse failed\n"); return 1; }
        printf("parsed: store_path=%s url=%s refs[0]=%s sigs[0]=%s\n",
               ni->store_path, ni->url, ni->references[0], ni->signatures[0]);
        char *re_text = narinfo_serialize(ni);
        printf("--- re-serialized ---\n%s---\n", re_text);
        free(re_text);
        narinfo_free(ni);

        printf("\nAll smoke tests passed.\n");
        return 0;
}
