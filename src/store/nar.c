/* nar.c - NAR serialisation + content-addressed store paths
 *
 * See nar.h for the format rationale. The serialisation here follows the
 * Phase 2 spec (big-endian length prefixes, no padding, no parens). It is
 * a canonical byte stream - the same input tree always produces the same
 * bytes - so the SHA-256 is a stable content address.
 *
 * Note: this is 2O9's NAR variant, not byte-compatible with Nix's wire
 * format (which uses little-endian lengths + 8-byte padding). The two
 * share the same structural shape (sorted directory walk, type tags,
 * length-prefixed contents) so cross-tool comparison is straightforward
 * if needed later.
 */
#include "nar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>

#include <openssl/sha.h>

/* ── Writer abstraction ──────────────────────────────────────────────
 * nar_dump_internal() never writes directly to a FILE* or SHA256_CTX.
 * Instead it calls a writer callback per chunk. This lets the same
 * recursive walker feed both a file (for Phase 3 binary cache export)
 * and a SHA256_CTX (for content addressing) without buffering the
 * whole serialisation in memory. */

typedef int (*nar_writer_fn)(const void *buf, size_t len, void *ctx);

static int write_to_file(const void *buf, size_t len, void *ctx)
{
        return fwrite(buf, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

struct sha_writer_ctx {
        SHA256_CTX sha;
        size_t total;
};

static int write_to_sha(const void *buf, size_t len, void *ctx)
{
        struct sha_writer_ctx *c = (struct sha_writer_ctx *)ctx;
        SHA256_Update(&c->sha, buf, len);
        c->total += len;
        return 0;
}

/* Emit an 8-byte big-endian length prefix. */
static int emit_u64_be(uint64_t n, nar_writer_fn w, void *ctx)
{
        unsigned char buf[8];
        for (int i = 0; i < 8; i++)
                buf[i] = (unsigned char)((n >> (8 * (7 - i))) & 0xff);
        return w(buf, 8, ctx);
}

/* ── Recursive node dumper ───────────────────────────────────────────
 * Dumps a single filesystem node (file/symlink/dir) starting with its
 * `type:` line. The caller is responsible for emitting the
 * `nix-archive-1\n` magic before the root dump_node call. */

static int dump_node(const char *path, nar_writer_fn w, void *ctx)
{
        struct stat st;
        if (lstat(path, &st) < 0) return -1;

        if (S_ISREG(st.st_mode)) {
                if (w("type:regular\n", 13, ctx) < 0) return -1;
                if (st.st_mode & S_IXUSR) {
                        if (w("executable\n", 11, ctx) < 0) return -1;
                }
                if (w("contents:", 9, ctx) < 0) return -1;
                if (emit_u64_be((uint64_t)st.st_size, w, ctx) < 0) return -1;

                FILE *f = fopen(path, "rb");
                if (!f) return -1;
                char buf[65536];
                size_t n;
                int rc = 0;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                        if (w(buf, n, ctx) < 0) { rc = -1; break; }
                }
                if (ferror(f)) rc = -1;
                fclose(f);
                return rc;
        }

        if (S_ISLNK(st.st_mode)) {
                if (w("type:symlink\n", 13, ctx) < 0) return -1;
                char target[PATH_MAX];
                ssize_t n = readlink(path, target, sizeof(target));
                if (n < 0) return -1;
                if (w("target:", 7, ctx) < 0) return -1;
                if (w(target, (size_t)n, ctx) < 0) return -1;
                return 0;
        }

        if (S_ISDIR(st.st_mode)) {
                if (w("type:directory\n", 15, ctx) < 0) return -1;

                struct dirent **names = NULL;
                int cnt = scandir(path, &names, NULL, alphasort);
                if (cnt < 0) return -1;

                /* Note: we do NOT free names[i] inside the loop. The
                 * cleanup loop at the end frees every entry unconditionally,
                 * even on early break. This avoids the double-free that
                 * would happen if we freed in both places. */
                int rc = 0;
                for (int i = 0; i < cnt; i++) {
                        const char *name = names[i]->d_name;
                        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                                continue;

                        size_t nlen = strlen(name);

                        if (w("entry\n", 6, ctx) < 0 ||
                            w("name:", 5, ctx) < 0 ||
                            w(name, nlen, ctx) < 0 ||
                            w("\n", 1, ctx) < 0) {
                                rc = -1;
                                break;
                        }

                        char child[PATH_MAX];
                        if (snprintf(child, sizeof(child), "%s/%s", path, name)
                            >= (int)sizeof(child)) {
                                rc = -1;
                                break;
                        }
                        if (dump_node(child, w, ctx) < 0) {
                                rc = -1;
                                break;
                        }
                }
                for (int i = 0; i < cnt; i++) free(names[i]);
                free(names);
                return rc;
        }

        /* FIFO/socket/device: not representable in a NAR. Refuse so the
         * hash is well-defined (these should not appear in 2O9 store
         * paths anyway - only files/symlinks/dirs come out of tar). */
        errno = EINVAL;
        return -1;
}

static int nar_dump_internal(const char *path, nar_writer_fn w, void *ctx)
{
        if (w("nix-archive-1\n", 14, ctx) < 0) return -1;
        return dump_node(path, w, ctx);
}

int nar_dump(const char *path, FILE *out)
{
        if (!path || !out) { errno = EINVAL; return -1; }
        return nar_dump_internal(path, write_to_file, out);
}

int nar_hash_directory(const char *path, char *hash_out, size_t *size_out)
{
        if (!path || !hash_out) { errno = EINVAL; return -1; }

        struct sha_writer_ctx c;
        SHA256_Init(&c.sha);
        c.total = 0;

        if (nar_dump_internal(path, write_to_sha, &c) < 0)
                return -1;

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256_Final(digest, &c.sha);

        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
                hash_out[i * 2]     = hex[digest[i] >> 4];
                hash_out[i * 2 + 1] = hex[digest[i] & 0x0f];
        }
        hash_out[SHA256_DIGEST_LENGTH * 2] = '\0';

        if (size_out) *size_out = c.total;
        return 0;
}

/* ── Nix-style store path derivation ─────────────────────────────────
 * The store path hash is the SHA-256 of:
 *     "output:out:sha256:<nar-hash-hex>:/nix/store:<name>-<version>"
 * truncated to 20 bytes (160 bits), then encoded in Nix's base32
 * alphabet. The base32 alphabet omits e/o/t/u to avoid look-alikes.
 *
 * Encoding: 20 bytes = 4 groups of 5 bytes (40 bits each). Each 5-byte
 * group is read little-endian into a 64-bit integer, then 8 base32
 * chars are emitted LSB-first. Total: 32 chars. */

char *compute_store_path(const char *nar_hash_hex, const char *name,
                         const char *version)
{
        if (!nar_hash_hex || !name || !version) {
                errno = EINVAL;
                return NULL;
        }

        /* Build the fingerprint string. */
        size_t fp_len = strlen("output:out:sha256:") +
                        strlen(nar_hash_hex) +
                        strlen(":/nix/store:") +
                        strlen(name) + 1 + strlen(version) + 1;
        char *fp = malloc(fp_len);
        if (!fp) return NULL;
        snprintf(fp, fp_len, "output:out:sha256:%s:/nix/store:%s-%s",
                 nar_hash_hex, name, version);

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char *)fp, strlen(fp), digest);
        free(fp);

        /* Nix base32 encode first 20 bytes -> 32 chars. */
        static const char base32[] = "0123456789abcdfghijklmnpqrsvwxyz";
        char hash32[33];
        for (int g = 0; g < 4; g++) {
                unsigned long long n = 0;
                for (int b = 0; b < 5; b++)
                        n |= ((unsigned long long)digest[g * 5 + b]) << (8 * b);
                for (int c = 0; c < 8; c++) {
                        hash32[g * 8 + c] = base32[n & 0x1f];
                        n >>= 5;
                }
        }
        hash32[32] = '\0';

        /* Final path: /nix/store/<base32>-<name>-<version> */
        size_t out_len = strlen("/nix/store/") + 32 + 1 +
                         strlen(name) + 1 + strlen(version) + 1;
        char *out = malloc(out_len);
        if (!out) return NULL;
        snprintf(out, out_len, "/nix/store/%s-%s-%s", hash32, name, version);
        return out;
}
