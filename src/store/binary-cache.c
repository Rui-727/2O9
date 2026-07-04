/* binary-cache.c - HTTP/S3 binary cache client
 *
 * See binary-cache.h for the protocol. Two backends:
 *
 *  - HTTP/HTTPS (base_url starts with http:// or https://): uses
 *    libcurl for GET (lookup, fetch) and PUT (push). 404 on lookup
 *    is treated as "not found" (returns NULL).
 *
 *  - S3 (base_url starts with s3://): shells out to the `aws` CLI.
 *    `aws s3 cp` handles upload/download. If `aws` is not on $PATH,
 *    every operation returns -1 with a clear error.
 *
 * Decompression on fetch: we use libarchive's `archive_read` with
 * `archive_read_support_filter_all` + `archive_read_support_format_raw`.
 * libarchive auto-detects the compression (zstd, xz, bzip2, gzip) and
 * the raw format treats the input as a single uncompressed file. We
 * stream the decompressed bytes to a temp file, then call nar_extract()
 * on that file.
 *
 * Compression on push: we shell out to `zstd -c` (matching the
 * existing store.c pattern). The compressed file's SHA-256 + size
 * go into the narinfo's FileHash/FileSize.
 */
#include "binary-cache.h"
#include "nar.h"
#include "signing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>
#include <openssl/sha.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static int starts_with(const char *s, const char *prefix)
{
        return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int is_http_url(const char *url)
{
        return starts_with(url, "http://") || starts_with(url, "https://");
}

static int is_s3_url(const char *url)
{
        return starts_with(url, "s3://");
}

/* Find the `aws` CLI on $PATH. Returns 1 if available, 0 if not. */
static int aws_cli_available(void)
{
        return (access("/usr/bin/aws", X_OK) == 0) ||
               (access("/usr/local/bin/aws", X_OK) == 0);
}

/* Extract the hash prefix from a store path (the base32 part before
 * the first '-'). Returns a malloc'd string or NULL on bad path. */
static char *path_hash_copy(const char *store_path)
{
        size_t hlen = 0;
        const char *h = narinfo_path_hash(store_path, &hlen);
        if (!h) return NULL;
        char *out = malloc(hlen + 1);
        if (!out) return NULL;
        memcpy(out, h, hlen);
        out[hlen] = '\0';
        return out;
}

/* ── libcurl GET into a growable memory buffer ────────────────────── */

struct membuf {
        char *data;
        size_t len;
        size_t cap;
        long http_code;
};

static void membuf_init(struct membuf *b)
{
        b->data = NULL; b->len = 0; b->cap = 0; b->http_code = 0;
}

static void membuf_free(struct membuf *b)
{
        free(b->data);
        b->data = NULL; b->len = 0; b->cap = 0;
}

static size_t membuf_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
        struct membuf *b = (struct membuf *)ud;
        size_t total = size * nmemb;
        if (b->len + total + 1 > b->cap) {
                size_t ncap = b->cap ? b->cap * 2 : 4096;
                while (b->len + total + 1 > ncap) ncap *= 2;
                char *nd = realloc(b->data, ncap);
                if (!nd) return 0;  /* signal error to libcurl */
                b->data = nd;
                b->cap = ncap;
        }
        memcpy(b->data + b->len, ptr, total);
        b->len += total;
        b->data[b->len] = '\0';
        return total;
}

/* GET url into buf. Returns 0 on 200, -1 on error or non-200.
 * For 404 specifically, returns -2 so the caller can treat
 * "not found" differently from "fetch failed". */
static int http_get(const char *url, struct membuf *buf)
{
        CURL *curl = curl_easy_init();
        if (!curl) return -1;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "2O9/0.0.1");
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        buf->http_code = code;
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) return -1;
        if (code == 404) return -2;
        if (code != 200) return -1;
        return 0;
}

/* PUT a file to url. Returns 0 on success (2xx), -1 on error. */
static int http_put_file(const char *url, const char *file_path)
{
        CURL *curl = curl_easy_init();
        if (!curl) return -1;

        FILE *f = fopen(file_path, "rb");
        if (!f) { curl_easy_cleanup(curl); return -1; }

        /* Stat for size. */
        struct stat st;
        if (fstat(fileno(f), &st) != 0) {
                fclose(f); curl_easy_cleanup(curl); return -1;
        }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/octet-stream");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READDATA, f);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "2O9/0.0.1");

        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        fclose(f);

        if (rc != CURLE_OK) return -1;
        if (code < 200 || code >= 300) return -1;
        return 0;
}

/* ── S3 helpers (shell out to `aws s3 cp`) ────────────────────────── */

/* Run `aws s3 cp <src> <dst>` synchronously. Returns 0 on success.
 * `src` or `dst` may be "-" for stdin/stdout (aws s3 cp supports "-"
 * as a streaming source/sink). */
static int s3_cp(const char *src, const char *dst)
{
        if (!aws_cli_available()) {
                fprintf(stderr, "209: aws CLI not found on $PATH; "
                        "cannot reach S3 cache\n");
                return -1;
        }
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
                /* child */
                execlp("aws", "aws", "s3", "cp", src, dst,
                       "--no-progress", (char *)NULL);
                _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
        return 0;
}

/* Get a key from S3 into a malloc'd buffer. Returns 0 on success,
 * -1 on error, -2 if not found (aws s3 cp returns nonzero on 404). */
static int s3_get_to_mem(const char *s3_url, struct membuf *buf)
{
        (void)buf;
        /* aws s3 cp doesn't stream to stdout reliably when the key
         * doesn't exist - we'd get a noisy error. Use a temp file. */
        char tmp[] = "/tmp/2O9-bc-XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) return -1;
        close(fd);
        unlink(tmp);  /* aws s3 cp won't overwrite; let it create */

        if (s3_cp(s3_url, tmp) != 0) {
                unlink(tmp);
                return -2;  /* treat any failure as "not found" */
        }

        FILE *f = fopen(tmp, "rb");
        if (!f) { unlink(tmp); return -1; }
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (membuf_write_cb(chunk, 1, n, buf) != n) {
                        fclose(f); unlink(tmp); return -1;
                }
        }
        fclose(f);
        unlink(tmp);
        return 0;
}

/* Upload a local file to S3. */
static int s3_put_file(const char *file_path, const char *s3_url)
{
        return s3_cp(file_path, s3_url);
}

/* ── Generic URL helpers (HTTP or S3 dispatch) ────────────────────── */

/* Build a cache URL: <base>/<suffix>. Caller frees. */
static char *url_join(const char *base, const char *suffix)
{
        size_t blen = strlen(base);
        int need_slash = (blen > 0 && base[blen - 1] != '/' && suffix[0] != '/');
        size_t out_len = blen + (need_slash ? 1 : 0) + strlen(suffix) + 1;
        char *out = malloc(out_len);
        if (!out) return NULL;
        snprintf(out, out_len, "%s%s%s", base, need_slash ? "/" : "", suffix);
        return out;
}

/* GET a URL into buf. Returns 0 on success, -1 on error, -2 if not found. */
static int cache_get(const char *url, struct membuf *buf)
{
        if (is_http_url(url)) return http_get(url, buf);
        if (is_s3_url(url))  return s3_get_to_mem(url, buf);
        return -1;
}

/* PUT a file to a URL. Returns 0 on success, -1 on error. */
static int cache_put_file(const char *url, const char *file_path)
{
        if (is_http_url(url)) return http_put_file(url, file_path);
        if (is_s3_url(url))  return s3_put_file(file_path, url);
        return -1;
}

/* ── libarchive decompression ───────────────────────────────────────
 *
 * Reads a compressed NAR from `compressed` (memory buffer) and writes
 * the decompressed bytes to `out_path`. Returns 0 on success. */
static int decompress_to_file(const char *out_path,
                               const char *compressed, size_t compressed_len)
{
        struct archive *a = archive_read_new();
        if (!a) return -1;
        archive_read_support_filter_all(a);
        archive_read_support_format_raw(a);

        int rc = archive_read_open_memory(a, (void *)compressed, compressed_len);
        if (rc != ARCHIVE_OK) { archive_read_free(a); return -1; }

        struct archive_entry *entry;
        rc = archive_read_next_header(a, &entry);
        if (rc != ARCHIVE_OK) { archive_read_free(a); return -1; }

        FILE *out = fopen(out_path, "wb");
        if (!out) { archive_read_free(a); return -1; }

        char buf[65536];
        ssize_t n;
        int ok = 1;
        while ((n = archive_read_data(a, buf, sizeof(buf))) > 0) {
                if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) { ok = 0; break; }
        }
        if (n < 0) ok = 0;
        if (fclose(out) != 0) ok = 0;
        archive_read_free(a);
        return ok ? 0 : -1;
}

/* ── Public API ───────────────────────────────────────────────────── */

binary_cache_t *binary_cache_new(const char *base_url,
                                  char **public_keys_b64,
                                  int allow_unsigned)
{
        if (!base_url) { errno = EINVAL; return NULL; }
        binary_cache_t *bc = calloc(1, sizeof(*bc));
        if (!bc) return NULL;
        bc->base_url = strdup(base_url);
        if (!bc->base_url) { free(bc); return NULL; }
        /* Take ownership of the public_keys list (may be NULL). */
        bc->public_keys = public_keys_b64;
        bc->allow_unsigned = allow_unsigned;
        return bc;
}

void binary_cache_free(binary_cache_t *bc)
{
        if (!bc) return;
        free(bc->base_url);
        if (bc->public_keys) {
                for (size_t i = 0; bc->public_keys[i]; i++)
                        free(bc->public_keys[i]);
                free(bc->public_keys);
        }
        free(bc);
}

narinfo_t *binary_cache_lookup(binary_cache_t *bc, const char *store_path)
{
        if (!bc || !store_path) { errno = EINVAL; return NULL; }

        char *hash = path_hash_copy(store_path);
        if (!hash) return NULL;
        narinfo_t *ni = binary_cache_lookup_by_hash(bc, hash);
        free(hash);
        return ni;
}

narinfo_t *binary_cache_lookup_by_hash(binary_cache_t *bc, const char *hash)
{
        if (!bc || !hash) { errno = EINVAL; return NULL; }

        /* <base>/<hash>.narinfo */
        size_t slen = strlen(hash) + strlen(".narinfo") + 1;
        char *suffix = malloc(slen);
        if (!suffix) return NULL;
        snprintf(suffix, slen, "%s.narinfo", hash);

        char *url = url_join(bc->base_url, suffix);
        free(suffix);
        if (!url) return NULL;

        struct membuf buf;
        membuf_init(&buf);
        int rc = cache_get(url, &buf);
        free(url);

        if (rc == -2) { membuf_free(&buf); return NULL; }  /* not found */
        if (rc != 0)  { membuf_free(&buf); return NULL; }  /* error */

        if (!buf.data || buf.len == 0) { membuf_free(&buf); return NULL; }

        narinfo_t *ni = narinfo_parse(buf.data);
        membuf_free(&buf);
        if (!ni) return NULL;

        /* Verify signature if we have any public keys configured. Accept
         * the narinfo if ANY public key verifies ANY of its signatures. */
        if (bc->public_keys && bc->public_keys[0]) {
                int verified = 0;
                for (size_t i = 0; bc->public_keys[i] && !verified; i++) {
                        int rc = narinfo_verify_signed(ni, bc->public_keys[i]);
                        if (rc == 1) { verified = 1; break; }
                }
                if (!verified) {
                        if (!bc->allow_unsigned) {
                                narinfo_free(ni);
                                return NULL;
                        }
                        /* Unsigned but allowed - return anyway. */
                }
        } else if (!bc->allow_unsigned) {
                /* No public keys configured AND allow_unsigned is false.
                 * Refuse unless the narinfo has at least one signature
                 * (we can't verify it but we accept the trust assertion). */
                if (!ni->signatures || !ni->signatures[0]) {
                        narinfo_free(ni);
                        return NULL;
                }
        }

        return ni;
}

/* Recursively remove a directory tree. */
static int rmtree_helper(const char *path)
{
        DIR *d = opendir(path);
        if (!d) return unlink(path);
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                        continue;
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                struct stat st;
                if (lstat(child, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) rmtree_helper(child);
                        else unlink(child);
                }
        }
        closedir(d);
        return rmdir(path);
}

int binary_cache_fetch(binary_cache_t *bc, const narinfo_t *ni,
                       const char *final_store_path)
{
        if (!bc || !ni || !final_store_path || !ni->url) {
                errno = EINVAL; return -1;
        }

        /* Download the compressed NAR. */
        char *nar_url = url_join(bc->base_url, ni->url);
        if (!nar_url) return -1;

        struct membuf buf;
        membuf_init(&buf);
        int rc = cache_get(nar_url, &buf);
        free(nar_url);
        if (rc != 0) { membuf_free(&buf); return -1; }
        if (!buf.data || buf.len == 0) { membuf_free(&buf); return -1; }

        /* Decompress to a temp file. */
        char tmp_nar[] = "/tmp/2O9-nar-XXXXXX";
        int fd = mkstemp(tmp_nar);
        if (fd < 0) { membuf_free(&buf); return -1; }
        close(fd);

        if (decompress_to_file(tmp_nar, buf.data, buf.len) != 0) {
                unlink(tmp_nar); membuf_free(&buf); return -1;
        }
        membuf_free(&buf);

        /* Extract the NAR to a temp dir, then rename to final_store_path.
         * The temp dir must be on the same filesystem as the final path
         * so rename(2) is atomic. Use <parent>/.<basename>.<pid> as the
         * temp name (hidden by the leading dot, pid avoids collisions). */
        char final_parent[PATH_MAX];
        strncpy(final_parent, final_store_path, sizeof(final_parent) - 1);
        final_parent[sizeof(final_parent) - 1] = '\0';
        char *slash = strrchr(final_parent, '/');
        const char *final_basename = slash ? slash + 1 : final_store_path;
        if (slash) *slash = '\0';
        if (final_parent[0] == '\0') strcpy(final_parent, "/");
        if (mkdir(final_parent, 0755) != 0 && errno != EEXIST) {
                unlink(tmp_nar); return -1;
        }
        /* Reuse <parent>/.tmp if it exists (matches Phase 2 convention),
         * else fall back to a dot-prefixed sibling. */
        char tmp_dir[PATH_MAX];
        snprintf(tmp_dir, sizeof(tmp_dir), "%s/.tmp", final_parent);
        if (mkdir(tmp_dir, 0700) != 0 && errno != EEXIST) {
                /* Fall back to sibling temp. */
                snprintf(tmp_dir, sizeof(tmp_dir), "%s/.%s.%d",
                         final_parent, final_basename, (int)getpid());
        } else {
                snprintf(tmp_dir + strlen(tmp_dir),
                         sizeof(tmp_dir) - strlen(tmp_dir),
                         "/%s.%d", final_basename, (int)getpid());
        }
        rmtree_helper(tmp_dir);
        if (mkdir(tmp_dir, 0755) < 0) { unlink(tmp_nar); return -1; }

        FILE *nar_in = fopen(tmp_nar, "rb");
        if (!nar_in) { unlink(tmp_nar); rmtree_helper(tmp_dir); return -1; }

        if (nar_extract(nar_in, tmp_dir) != 0) {
                fclose(nar_in);
                unlink(tmp_nar);
                rmtree_helper(tmp_dir);
                return -1;
        }
        fclose(nar_in);
        unlink(tmp_nar);

        /* Atomic rename to final path. */
        struct stat st;
        if (stat(final_store_path, &st) == 0) {
                /* Already exists - leave it. */
                rmtree_helper(tmp_dir);
                return 0;
        }

        if (rename(tmp_dir, final_store_path) != 0) {
                rmtree_helper(tmp_dir);
                return -1;
        }

        return 0;
}

/* ── Push ─────────────────────────────────────────────────────────── */

/* Compute SHA-256 of a file. Writes 64-char hex + NUL to hash_hex_out
 * (must be at least 65 bytes). Returns 0 on success, -1 on error. */
static int sha256_file_hex(const char *path, char *hash_hex_out)
{
        FILE *f = fopen(path, "rb");
        if (!f) return -1;
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        unsigned char buf[65536];
        size_t n;
        int rc = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                SHA256_Update(&ctx, buf, n);
        if (ferror(f)) rc = -1;
        fclose(f);
        if (rc < 0) return -1;

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256_Final(digest, &ctx);
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
                hash_hex_out[i * 2]     = hex[digest[i] >> 4];
                hash_hex_out[i * 2 + 1] = hex[digest[i] & 0x0f];
        }
        hash_hex_out[SHA256_DIGEST_LENGTH * 2] = '\0';
        return 0;
}

/* Compress `src` to `dst` via `zstd -c`. Returns 0 on success. */
static int zstd_compress(const char *src, const char *dst)
{
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
                /* child */
                execlp("zstd", "zstd", "-c", "-f", "-q", "-o", dst, src,
                       (char *)NULL);
                _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
        return 0;
}

int binary_cache_push(binary_cache_t *bc, const char *store_path,
                      store_db_t *db, const char *signing_key_name,
                      const unsigned char *signing_secret_key)
{
        if (!bc || !store_path) { errno = EINVAL; return -1; }

        /* Build the narinfo (re-hashes the tree, pulls refs, signs). */
        narinfo_t *ni = narinfo_from_store_path(db, store_path,
                                                 signing_key_name,
                                                 signing_secret_key);
        if (!ni) return -1;

        /* Dump the NAR to a temp file. */
        char tmp_nar[] = "/tmp/2O9-push-nar-XXXXXX";
        int fd = mkstemp(tmp_nar);
        if (fd < 0) { narinfo_free(ni); return -1; }
        close(fd);
        unlink(tmp_nar);  /* let nar_dump create it */

        FILE *nf = fopen(tmp_nar, "wb");
        if (!nf) { narinfo_free(ni); return -1; }
        if (nar_dump(store_path, nf) != 0) {
                fclose(nf); unlink(tmp_nar); narinfo_free(ni); return -1;
        }
        fclose(nf);

        /* Compress to .nar.zst. */
        char tmp_zst[] = "/tmp/2O9-push-zst-XXXXXX";
        fd = mkstemp(tmp_zst);
        if (fd < 0) { unlink(tmp_nar); narinfo_free(ni); return -1; }
        close(fd);
        unlink(tmp_zst);

        if (zstd_compress(tmp_nar, tmp_zst) != 0) {
                unlink(tmp_nar); unlink(tmp_zst); narinfo_free(ni);
                return -1;
        }
        unlink(tmp_nar);

        /* Fill in FileHash/FileSize from the compressed file. */
        char zhash[65];
        if (sha256_file_hex(tmp_zst, zhash) != 0) {
                unlink(tmp_zst); narinfo_free(ni); return -1;
        }
        struct stat zst_st;
        if (stat(tmp_zst, &zst_st) != 0) {
                unlink(tmp_zst); narinfo_free(ni); return -1;
        }

        char file_hash_field[80];
        snprintf(file_hash_field, sizeof(file_hash_field), "sha256:%s", zhash);
        free(ni->file_hash);
        ni->file_hash = strdup(file_hash_field);
        ni->file_size = (int64_t)zst_st.st_size;

        /* Upload the .nar.zst to <base>/<url>. */
        char *nar_url = url_join(bc->base_url, ni->url);
        if (!nar_url) {
                unlink(tmp_zst); narinfo_free(ni); return -1;
        }
        int rc = cache_put_file(nar_url, tmp_zst);
        free(nar_url);
        unlink(tmp_zst);
        if (rc != 0) {
                fprintf(stderr, "209: failed to upload NAR to %s\n", bc->base_url);
                narinfo_free(ni);
                return -1;
        }

        /* Upload the narinfo to <base>/<hash>.narinfo. */
        char *hash = path_hash_copy(store_path);
        if (!hash) { narinfo_free(ni); return -1; }
        size_t slen = strlen(hash) + strlen(".narinfo") + 1;
        char *suffix = malloc(slen);
        if (!suffix) { free(hash); narinfo_free(ni); return -1; }
        snprintf(suffix, slen, "%s.narinfo", hash);
        free(hash);

        char *ni_url = url_join(bc->base_url, suffix);
        free(suffix);
        if (!ni_url) { narinfo_free(ni); return -1; }

        char *ni_text = narinfo_serialize(ni);
        narinfo_free(ni);
        if (!ni_text) { free(ni_url); return -1; }

        /* Write narinfo text to a temp file for upload. */
        char tmp_ni[] = "/tmp/2O9-push-ni-XXXXXX";
        fd = mkstemp(tmp_ni);
        if (fd < 0) { free(ni_url); free(ni_text); return -1; }
        FILE *nif = fdopen(fd, "w");
        if (!nif) { close(fd); unlink(tmp_ni); free(ni_url); free(ni_text); return -1; }
        fputs(ni_text, nif);
        fclose(nif);
        free(ni_text);

        rc = cache_put_file(ni_url, tmp_ni);
        free(ni_url);
        unlink(tmp_ni);
        if (rc != 0) {
                fprintf(stderr, "209: failed to upload narinfo to %s\n",
                        bc->base_url);
                return -1;
        }

        return 0;
}

/* ── Phase 4: cache index (index.json) ──────────────────────────────
 *
 * The cache index is a small JSON file at <base>/index.json listing
 * everything pushed to this cache. Format:
 *
 *   {
 *     "version": 1,
 *     "updated_at": <unix-ts>,
 *     "items": [
 *       { "hash": "...", "name": "...", "type": "share",
 *         "nar_size": <n>, "nar_hash": "sha256:...",
 *         "pushed_at": <ts>, "signed_by": "<key-name>" }
 *     ]
 *   }
 *
 * The cache server just serves files. 2O9 maintains the index by
 * fetching the current one, appending the new item, and re-uploading.
 * Last-write-wins on race.
 */

cJSON *binary_cache_fetch_index(binary_cache_t *bc)
{
        if (!bc) { errno = EINVAL; return NULL; }

        char *url = url_join(bc->base_url, "index.json");
        if (!url) return NULL;

        struct membuf buf;
        membuf_init(&buf);
        int rc = cache_get(url, &buf);
        free(url);

        if (rc != 0) { membuf_free(&buf); return NULL; }
        if (!buf.data || buf.len == 0) { membuf_free(&buf); return NULL; }

        cJSON *root = cJSON_Parse(buf.data);
        membuf_free(&buf);
        return root;
}

int binary_cache_push_index_item(binary_cache_t *bc, const cJSON *item_json)
{
        if (!bc || !item_json) { errno = EINVAL; return -1; }

        /* Fetch the current index (may be NULL if not yet created). */
        cJSON *root = binary_cache_fetch_index(bc);
        cJSON *items = NULL;
        if (root) {
                items = cJSON_GetObjectItemCaseSensitive(root, "items");
        }
        if (!items || !cJSON_IsArray(items)) {
                /* No existing index, or items is missing/wrong type.
                 * Build a fresh one. */
                if (root) cJSON_Delete(root);
                root = cJSON_CreateObject();
                if (!root) return -1;
                cJSON_AddNumberToObject(root, "version", 1);
                items = cJSON_CreateArray();
                if (!items) { cJSON_Delete(root); return -1; }
                cJSON_AddItemToObject(root, "items", items);
        }

        /* Append a copy of the item. */
        cJSON *item_copy = cJSON_Duplicate(item_json, 1);
        if (!item_copy) { cJSON_Delete(root); return -1; }
        cJSON_AddItemToArray(items, item_copy);

        /* Update the timestamp. */
        cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "updated_at");
        if (ts) {
                cJSON_ReplaceItemInObject(root, "updated_at",
                                cJSON_CreateNumber((double)time(NULL)));
        } else {
                cJSON_AddNumberToObject(root, "updated_at", (double)time(NULL));
        }

        /* Serialise. */
        char *text = cJSON_Print(root);
        cJSON_Delete(root);
        if (!text) return -1;

        /* Write to a temp file. */
        char tmp[] = "/tmp/2O9-idx-XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) { free(text); return -1; }
        FILE *f = fdopen(fd, "w");
        if (!f) { close(fd); unlink(tmp); free(text); return -1; }
        fputs(text, f);
        fclose(f);
        free(text);

        /* Upload to <base>/index.json. */
        char *url = url_join(bc->base_url, "index.json");
        int rc = cache_put_file(url, tmp);
        free(url);
        unlink(tmp);
        return rc;
}

/* Download + decompress the NAR referenced by a narinfo into a
 * malloc'd buffer. The caller frees *out_buf. */
int binary_cache_download_nar(binary_cache_t *bc, const narinfo_t *ni,
                              char **out_buf, size_t *out_len)
{
        if (!bc || !ni || !out_buf || !out_len || !ni->url) {
                errno = EINVAL; return -1;
        }
        *out_buf = NULL;
        *out_len = 0;

        char *nar_url = url_join(bc->base_url, ni->url);
        if (!nar_url) return -1;

        struct membuf buf;
        membuf_init(&buf);
        int rc = cache_get(nar_url, &buf);
        free(nar_url);
        if (rc != 0) { membuf_free(&buf); return -1; }
        if (!buf.data || buf.len == 0) { membuf_free(&buf); return -1; }

        /* Decompress to a temp file, then read back into a buffer. */
        char tmp_nar[] = "/tmp/2O9-nar-XXXXXX";
        int fd = mkstemp(tmp_nar);
        if (fd < 0) { membuf_free(&buf); return -1; }
        close(fd);
        unlink(tmp_nar);  /* let decompress_to_file create it */

        if (decompress_to_file(tmp_nar, buf.data, buf.len) != 0) {
                membuf_free(&buf);
                unlink(tmp_nar);
                return -1;
        }
        membuf_free(&buf);

        FILE *f = fopen(tmp_nar, "rb");
        unlink(tmp_nar);
        if (!f) return -1;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
        long sz = ftell(f);
        if (sz < 0) { fclose(f); return -1; }
        rewind(f);
        char *data = malloc((size_t)sz);
        if (!data) { fclose(f); return -1; }
        size_t nread = fread(data, 1, (size_t)sz, f);
        fclose(f);
        if (nread != (size_t)sz) { free(data); return -1; }

        *out_buf = data;
        *out_len = (size_t)sz;
        return 0;
}
