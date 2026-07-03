/* narinfo.c - NAR metadata file parse / serialise / build-from-store
 *
 * See narinfo.h for the format. The parser is line-based: each
 * `Key: Value\n` line sets one field. Unknown keys are accepted and
 * dropped (forward-compat). Multiple Sig: lines accumulate.
 *
 * Serialiser emits the same line-based format. Order matches Nix's
 * canonical emit (StorePath, URL, Compression, FileHash, FileSize,
 * NarHash, NarSize, References, Deriver, Sig, CA) so a 2O9 cache
 * looks like a Nix cache to outside tooling.
 *
 * narinfo_from_store_path rebuilds a narinfo from a live store path:
 * re-NARs the tree to compute the hash + size (cheap on local disk),
 * pulls References: from the store DB's refs graph, and signs the
 * fingerprint with the caller's secret key.
 */
#include "narinfo.h"
#include "nar.h"
#include "signing.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static char *strndup_safe(const char *s, size_t n)
{
        char *out = malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, s, n);
        out[n] = '\0';
        return out;
}

static void str_list_free(char **list)
{
        if (!list) return;
        for (size_t i = 0; list[i]; i++) free(list[i]);
        free(list);
}

/* Append a string to a NULL-terminated list. Grows the list as needed.
 * Takes ownership of `s`. Returns 0 on success, -1 on alloc failure
 * (s is freed on failure). */
static int list_append(char ***list_ptr, size_t *count, size_t *cap, char *s)
{
        if (*count == *cap) {
                size_t ncap = *cap ? *cap * 2 : 4;
                char **nl = realloc(*list_ptr, ncap * sizeof(char *));
                if (!nl) { free(s); return -1; }
                *list_ptr = nl;
                *cap = ncap;
        }
        (*list_ptr)[(*count)++] = s;
        return 0;
}

/* Finalise a list: ensure NULL terminator. */
static int list_finish(char ***list_ptr, size_t *count, size_t *cap)
{
        if (*count == *cap) {
                size_t ncap = *cap + 1;
                char **nl = realloc(*list_ptr, ncap * sizeof(char *));
                if (!nl) return -1;
                *list_ptr = nl;
                *cap = ncap;
        }
        (*list_ptr)[*count] = NULL;
        return 0;
}

/* ── Path helpers ─────────────────────────────────────────────────── */

const char *narinfo_path_hash(const char *store_path, size_t *len_out)
{
        if (!store_path || !len_out) return NULL;
        /* Accept any "/.../<hash>-<name>-<version>" path: take the
         * basename (after the last '/') and find the first '-'. The
         * prefix before the '-' is the hash. */
        const char *base = strrchr(store_path, '/');
        base = base ? base + 1 : store_path;
        const char *dash = strchr(base, '-');
        if (!dash) return NULL;
        *len_out = (size_t)(dash - base);
        return base;
}

/* ── Parse ────────────────────────────────────────────────────────── */

/* Trim leading + trailing whitespace in place. Returns start pointer. */
static char *trim_inplace(char *s)
{
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) return s;
        char *end = s + strlen(s) - 1;
        while (end > s && isspace((unsigned char)*end)) {
                *end = '\0';
                end--;
        }
        return s;
}

narinfo_t *narinfo_parse(const char *text)
{
        if (!text) { errno = EINVAL; return NULL; }

        narinfo_t *ni = calloc(1, sizeof(*ni));
        if (!ni) return NULL;
        ni->file_size = 0;
        ni->nar_size = 0;

        /* Make a mutable copy so we can strtok/modify. */
        char *copy = strdup(text);
        if (!copy) { narinfo_free(ni); return NULL; }

        char *save = NULL;
        char *line = strtok_r(copy, "\r\n", &save);
        while (line) {
                char *colon = strchr(line, ':');
                if (!colon) { line = strtok_r(NULL, "\r\n", &save); continue; }

                *colon = '\0';
                char *key = trim_inplace(line);
                char *val = trim_inplace(colon + 1);

                if (strcmp(key, "StorePath") == 0) {
                        free(ni->store_path);
                        ni->store_path = strdup(val);
                } else if (strcmp(key, "URL") == 0) {
                        free(ni->url);
                        ni->url = strdup(val);
                } else if (strcmp(key, "Compression") == 0) {
                        free(ni->compression);
                        ni->compression = strdup(val);
                } else if (strcmp(key, "FileHash") == 0) {
                        free(ni->file_hash);
                        ni->file_hash = strdup(val);
                } else if (strcmp(key, "FileSize") == 0) {
                        ni->file_size = strtoll(val, NULL, 10);
                } else if (strcmp(key, "NarHash") == 0) {
                        free(ni->nar_hash);
                        ni->nar_hash = strdup(val);
                } else if (strcmp(key, "NarSize") == 0) {
                        ni->nar_size = strtoll(val, NULL, 10);
                } else if (strcmp(key, "References") == 0) {
                        str_list_free(ni->references);
                        ni->references = NULL;
                        if (*val) {
                                size_t count = 0, cap = 0;
                                char *rsave = NULL;
                                char *tok = strtok_r(val, " \t", &rsave);
                                while (tok) {
                                        char *copy_tok = strdup(tok);
                                        if (!copy_tok) goto parse_fail;
                                        if (list_append(&ni->references, &count, &cap, copy_tok) < 0)
                                                goto parse_fail;
                                        tok = strtok_r(NULL, " \t", &rsave);
                                }
                                if (list_finish(&ni->references, &count, &cap) < 0)
                                        goto parse_fail;
                        }
                } else if (strcmp(key, "Deriver") == 0) {
                        free(ni->deriver);
                        ni->deriver = strdup(val);
                } else if (strcmp(key, "Sig") == 0) {
                        size_t count = 0, cap = 0;
                        if (ni->signatures) {
                                while (ni->signatures[count]) count++;
                                cap = count;
                        }
                        char *copy_sig = strdup(val);
                        if (!copy_sig) goto parse_fail;
                        if (list_append(&ni->signatures, &count, &cap, copy_sig) < 0)
                                goto parse_fail;
                        if (list_finish(&ni->signatures, &count, &cap) < 0)
                                goto parse_fail;
                } else if (strcmp(key, "CA") == 0) {
                        free(ni->ca);
                        ni->ca = strdup(val);
                }
                /* Unknown keys silently ignored. */

                line = strtok_r(NULL, "\r\n", &save);
        }

        free(copy);

        /* Minimal validation: store_path is required. */
        if (!ni->store_path) {
                errno = EINVAL;
                narinfo_free(ni);
                return NULL;
        }
        return ni;

parse_fail:
        free(copy);
        narinfo_free(ni);
        return NULL;
}

/* ── Serialise ────────────────────────────────────────────────────── */

/* Append a formatted line to a growable buffer. */
static int buf_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
        __attribute__((format(printf, 4, 5)));
static int buf_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
{
        char small[512];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(small, sizeof(small), fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        size_t need = (size_t)n + 1;
        if (*len + need > *cap) {
                size_t ncap = *cap ? *cap * 2 : 256;
                while (*len + need > ncap) ncap *= 2;
                char *nb = realloc(*buf, ncap);
                if (!nb) return -1;
                *buf = nb;
                *cap = ncap;
        }
        memcpy(*buf + *len, small, need - 1);
        *len += need - 1;
        (*buf)[*len] = '\0';
        return 0;
}

char *narinfo_serialize(const narinfo_t *ni)
{
        if (!ni || !ni->store_path) { errno = EINVAL; return NULL; }

        char *buf = NULL;
        size_t len = 0, cap = 0;

        if (buf_append(&buf, &len, &cap, "StorePath: %s\n", ni->store_path) < 0) goto fail;

        if (ni->url)
                if (buf_append(&buf, &len, &cap, "URL: %s\n", ni->url) < 0) goto fail;
        if (ni->compression)
                if (buf_append(&buf, &len, &cap, "Compression: %s\n", ni->compression) < 0) goto fail;
        if (ni->file_hash)
                if (buf_append(&buf, &len, &cap, "FileHash: %s\n", ni->file_hash) < 0) goto fail;
        if (ni->file_size > 0)
                if (buf_append(&buf, &len, &cap, "FileSize: %lld\n",
                               (long long)ni->file_size) < 0) goto fail;
        if (ni->nar_hash)
                if (buf_append(&buf, &len, &cap, "NarHash: %s\n", ni->nar_hash) < 0) goto fail;
        if (ni->nar_size > 0)
                if (buf_append(&buf, &len, &cap, "NarSize: %lld\n",
                               (long long)ni->nar_size) < 0) goto fail;

        if (ni->references && ni->references[0]) {
                if (buf_append(&buf, &len, &cap, "References:") < 0) goto fail;
                for (size_t i = 0; ni->references[i]; i++) {
                        if (buf_append(&buf, &len, &cap, " %s", ni->references[i]) < 0) goto fail;
                }
                if (buf_append(&buf, &len, &cap, "\n") < 0) goto fail;
        }

        if (ni->deriver)
                if (buf_append(&buf, &len, &cap, "Deriver: %s\n", ni->deriver) < 0) goto fail;

        if (ni->signatures) {
                for (size_t i = 0; ni->signatures[i]; i++) {
                        if (buf_append(&buf, &len, &cap, "Sig: %s\n", ni->signatures[i]) < 0)
                                goto fail;
                }
        }

        if (ni->ca)
                if (buf_append(&buf, &len, &cap, "CA: %s\n", ni->ca) < 0) goto fail;

        return buf;

fail:
        free(buf);
        return NULL;
}

/* ── Build from store path ────────────────────────────────────────── */

narinfo_t *narinfo_from_store_path(store_db_t *db, const char *store_path,
                                    const char *signing_key_name,
                                    const unsigned char *signing_secret_key)
{
        if (!store_path) { errno = EINVAL; return NULL; }

        struct stat st;
        if (stat(store_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                errno = ENOENT;
                return NULL;
        }

        narinfo_t *ni = calloc(1, sizeof(*ni));
        if (!ni) return NULL;
        ni->store_path = strdup(store_path);
        if (!ni->store_path) { narinfo_free(ni); return NULL; }

        /* Compute NAR hash + size by re-serialising. nar_hash_directory
         * gives us the hex hash; we add the "sha256:" prefix to match
         * the narinfo field format. */
        char nar_hex[65];
        size_t nar_size = 0;
        if (nar_hash_directory(store_path, nar_hex, &nar_size) != 0) {
                narinfo_free(ni);
                return NULL;
        }
        char hash_field[80];
        snprintf(hash_field, sizeof(hash_field), "sha256:%s", nar_hex);
        ni->nar_hash = strdup(hash_field);
        ni->nar_size = (int64_t)nar_size;
        if (!ni->nar_hash) { narinfo_free(ni); return NULL; }

        /* URL: nar/<hash>.nar.zst. <hash> is the path hash (32-char
         * base32 prefix). Compression is zstd by default. */
        size_t path_hash_len = 0;
        const char *path_hash = narinfo_path_hash(store_path, &path_hash_len);
        if (path_hash) {
                char *hash_copy = strndup_safe(path_hash, path_hash_len);
                if (hash_copy) {
                        size_t url_len = strlen("nar/.nar.zst") + path_hash_len + 1;
                        char *url = malloc(url_len);
                        if (url) {
                                snprintf(url, url_len, "nar/%s.nar.zst",
                                         hash_copy);
                                ni->url = url;
                        }
                        free(hash_copy);
                }
        }
        ni->compression = strdup("zstd");

        /* References: from the store DB's refs graph (direct refs only). */
        if (db) {
                char **refs = store_db_get_refs(db, store_path);
                if (refs) {
                        ni->references = refs;  /* takes ownership */
                }
        }

        /* Deriver: 2O9 doesn't track derivations yet. Leave NULL. */

        /* Sign if both key name + secret key were provided. */
        if (signing_key_name && signing_secret_key) {
                char *fp = signing_fingerprint(store_path, ni->nar_hash,
                                               ni->nar_size, ni->references);
                if (fp) {
                        char *sig_b64 = signing_sign(fp, signing_secret_key);
                        if (sig_b64) {
                                size_t siglen = strlen(signing_key_name) + 1
                                              + strlen(sig_b64) + 1;
                                char *sig_line = malloc(siglen);
                                if (sig_line) {
                                        snprintf(sig_line, siglen, "%s:%s",
                                                 signing_key_name, sig_b64);
                                        /* Build a one-element list. */
                                        ni->signatures = calloc(2, sizeof(char *));
                                        if (ni->signatures) {
                                                ni->signatures[0] = sig_line;
                                                ni->signatures[1] = NULL;
                                        } else {
                                                free(sig_line);
                                        }
                                }
                                free(sig_b64);
                        }
                        free(fp);
                }
        }

        return ni;
}

/* ── Verify ──────────────────────────────────────────────────────── */

int narinfo_verify_signed(const narinfo_t *ni, const char *public_key_b64)
{
        if (!ni || !public_key_b64 || !ni->signatures || !ni->nar_hash)
                return 0;

        char *fp = signing_fingerprint(ni->store_path, ni->nar_hash,
                                       ni->nar_size, ni->references);
        if (!fp) return -1;

        int found_valid = 0;
        for (size_t i = 0; ni->signatures[i]; i++) {
                /* Each sig is "keyname:base64sig". We verify against
                 * the configured public key regardless of keyname. */
                const char *colon = strchr(ni->signatures[i], ':');
                if (!colon) continue;
                const char *sig_b64 = colon + 1;
                int rc = signing_verify(fp, sig_b64, public_key_b64);
                if (rc == 1) { found_valid = 1; break; }
        }

        free(fp);
        return found_valid;
}

/* ── Free ─────────────────────────────────────────────────────────── */

void narinfo_free(narinfo_t *ni)
{
        if (!ni) return;
        free(ni->store_path);
        free(ni->url);
        free(ni->compression);
        free(ni->file_hash);
        free(ni->nar_hash);
        str_list_free(ni->references);
        free(ni->deriver);
        str_list_free(ni->signatures);
        free(ni->ca);
        free(ni);
}
