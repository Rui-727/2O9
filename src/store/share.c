/* share.c - NAR file sharing
 *
 * See share.h for the design. A share is an arbitrary path NAR'd into
 * /nix/store/<base32>-share-<basename>/, pushed to configured subs,
 * fetched back by hash. The share URI is "nar://<hash>" or just the
 * bare hash.
 *
 * share_take() copies the input tree into the store. The copy is a
 * straight recursive copy preserving files, dirs, symlinks, and the
 * executable bit on regular files. Hardlinks are not preserved (each
 * file gets its own inode); this matches what `tar` extraction does
 * for packages, so a share of a directory and the same directory
 * installed as a package produce the same NAR hash.
 *
 * share_push_to_sub() reuses binary_cache_push() (which re-NARs the
 * tree, builds a narinfo, signs it with the sub's key, uploads both
 * the .nar.zst and .narinfo). It then appends an entry to the cache's
 * index.json via binary_cache_push_index_item().
 *
 * share_fetch_from_sub() walks each sub URL in config order, looks up
 * the narinfo by hash, verifies the signature, downloads + decompresses
 * the NAR, and extracts it to the destination directory.
 */
#include "share.h"
#include "nar.h"
#include "narinfo.h"
#include "binary-cache.h"
#include "signing.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#define STORE_ROOT "/nix/store"

/* ── Helpers ──────────────────────────────────────────────────────── */

static int starts_with(const char *s, const char *prefix)
{
        return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Recursively remove a directory tree. */
static int rmtree(const char *path)
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
                        if (S_ISDIR(st.st_mode)) rmtree(child);
                        else unlink(child);
                }
        }
        closedir(d);
        return rmdir(path);
}

/* Copy a single regular file preserving permissions. */
static int copy_file(const char *src, const char *dst)
{
        FILE *in = fopen(src, "rb");
        if (!in) return -1;
        FILE *out = fopen(dst, "wb");
        if (!out) { fclose(in); return -1; }

        char buf[65536];
        size_t n;
        int rc = 0;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
        }
        if (ferror(in)) rc = -1;
        fclose(in);
        if (fclose(out) != 0) rc = -1;
        if (rc != 0) { unlink(dst); return -1; }

        struct stat st;
        if (stat(src, &st) == 0) {
                chmod(dst, st.st_mode & 0777);
        }
        return 0;
}

/* Recursively copy src (file/dir/symlink) into dst (which doesn't
 * exist yet). dst's parent must exist. Returns 0 on success, -1 on
 * error (errno set). */
static int copy_tree(const char *src, const char *dst)
{
        struct stat st;
        if (lstat(src, &st) != 0) return -1;

        if (S_ISREG(st.st_mode)) {
                if (copy_file(src, dst) != 0) return -1;
                return 0;
        }
        if (S_ISLNK(st.st_mode)) {
                char target[PATH_MAX];
                ssize_t n = readlink(src, target, sizeof(target));
                if (n < 0) return -1;
                target[n] = '\0';
                if (symlink(target, dst) != 0) return -1;
                return 0;
        }
        if (S_ISDIR(st.st_mode)) {
                if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST)
                        return -1;
                DIR *d = opendir(src);
                if (!d) return -1;
                struct dirent *ent;
                int rc = 0;
                while ((ent = readdir(d)) != NULL) {
                        if (strcmp(ent->d_name, ".") == 0 ||
                            strcmp(ent->d_name, "..") == 0)
                                continue;
                        char child_src[PATH_MAX];
                        char child_dst[PATH_MAX];
                        if (snprintf(child_src, sizeof(child_src), "%s/%s",
                                     src, ent->d_name) >= (int)sizeof(child_src) ||
                            snprintf(child_dst, sizeof(child_dst), "%s/%s",
                                     dst, ent->d_name) >= (int)sizeof(child_dst)) {
                                rc = -1;
                                break;
                            }
                        if (copy_tree(child_src, child_dst) != 0) {
                                rc = -1;
                                break;
                        }
                }
                closedir(d);
                return rc;
        }
        /* FIFO/socket/device: not representable. */
        errno = EINVAL;
        return -1;
}

/* Extract the basename from a path. Returns pointer into path (no
 * allocation). */
static const char *basename_of(const char *path)
{
        const char *b = strrchr(path, '/');
        return b ? b + 1 : path;
}

/* ── share_take ───────────────────────────────────────────────────── */

char *share_take(const char *path)
{
        if (!path) { errno = EINVAL; return NULL; }

        /* MUST be absolute. No ~, no relative. */
        if (path[0] != '/') {
                fprintf(stderr, "209: share: path must be absolute: %s\n", path);
                errno = EINVAL;
                return NULL;
        }

        /* Source must exist. */
        struct stat st;
        if (lstat(path, &st) != 0) {
                fprintf(stderr, "209: share: cannot stat %s: %s\n",
                        path, strerror(errno));
                return NULL;
        }

        /* Compute NAR hash + size of the input tree. */
        char nar_hex[65];
        size_t nar_size = 0;
        if (nar_hash_directory(path, nar_hex, &nar_size) != 0) {
                fprintf(stderr, "209: share: NAR hash failed for %s\n", path);
                return NULL;
        }

        /* Compute store path: /nix/store/<base32>-share-<basename>. */
        const char *base = basename_of(path);
        char *store_path = compute_store_path(nar_hex, "share", base);
        if (!store_path) {
                fprintf(stderr, "209: share: cannot compute store path\n");
                return NULL;
        }

        /* If already exists, return as-is. */
        struct stat sp_st;
        if (stat(store_path, &sp_st) == 0) {
                return store_path;
        }

        /* Copy the input tree into the store path. */
        if (mkdir(store_path, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "209: share: cannot create %s: %s\n",
                        store_path, strerror(errno));
                free(store_path);
                return NULL;
        }

        /* For a single-file input, copy it under the store path with
         * its original basename. For a directory input, copy each
         * child into the store path. */
        if (S_ISDIR(st.st_mode)) {
                DIR *d = opendir(path);
                if (!d) {
                        fprintf(stderr, "209: share: cannot open %s: %s\n",
                                path, strerror(errno));
                        rmtree(store_path); free(store_path);
                        return NULL;
                }
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                        if (strcmp(ent->d_name, ".") == 0 ||
                            strcmp(ent->d_name, "..") == 0)
                                continue;
                        char child_src[PATH_MAX];
                        char child_dst[PATH_MAX];
                        if (snprintf(child_src, sizeof(child_src), "%s/%s",
                                     path, ent->d_name) >= (int)sizeof(child_src) ||
                            snprintf(child_dst, sizeof(child_dst), "%s/%s",
                                     store_path, ent->d_name) >= (int)sizeof(child_dst)) {
                                fprintf(stderr, "209: share: path too long\n");
                                closedir(d); rmtree(store_path);
                                free(store_path);
                                return NULL;
                        }
                        if (copy_tree(child_src, child_dst) != 0) {
                                fprintf(stderr, "209: share: copy %s failed: %s\n",
                                        child_src, strerror(errno));
                                closedir(d); rmtree(store_path);
                                free(store_path);
                                return NULL;
                        }
                }
                closedir(d);
        } else {
                /* Single file: copy under <store_path>/<basename>. */
                char dst[PATH_MAX];
                if (snprintf(dst, sizeof(dst), "%s/%s", store_path, base)
                    >= (int)sizeof(dst)) {
                        fprintf(stderr, "209: share: path too long\n");
                        rmtree(store_path); free(store_path);
                        return NULL;
                }
                if (copy_tree(path, dst) != 0) {
                        fprintf(stderr, "209: share: copy %s failed: %s\n",
                                path, strerror(errno));
                        rmtree(store_path); free(store_path);
                        return NULL;
                }
        }

        return store_path;
}

/* ── share_list_local ─────────────────────────────────────────────── */

static int is_share_dir(const char *dname)
{
        /* Match <hash>-share-<name>. The hash is 32 chars of base32. */
        if (strlen(dname) < 34) return 0;  /* 32 + "-share-" + at least 1 */
        if (dname[32] != '-') return 0;
        if (strncmp(dname + 33, "share-", 6) != 0) return 0;
        /* Hash chars must be base32 (Nix alphabet). */
        static const char b32[] = "0123456789abcdfghijklmnpqrsvwxyz";
        for (int i = 0; i < 32; i++) {
                if (strchr(b32, dname[i]) == NULL) return 0;
        }
        return 1;
}

share_entry_t *share_list_local(void)
{
        DIR *d = opendir(STORE_ROOT);
        if (!d) return NULL;

        share_entry_t *head = NULL, **tail = &head;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                if (!is_share_dir(ent->d_name)) continue;

                char full[PATH_MAX];
                if (snprintf(full, sizeof(full), "%s/%s",
                             STORE_ROOT, ent->d_name) >= (int)sizeof(full))
                        continue;

                struct stat st;
                if (stat(full, &st) != 0) continue;

                share_entry_t *e = calloc(1, sizeof(*e));
                if (!e) continue;
                e->store_path = strdup(full);
                e->hash = strndup(ent->d_name, 32);
                e->name = strdup(ent->d_name + 33 + 6);  /* skip "<hash>-share-" */
                e->nar_size = (int64_t)st.st_size;
                e->shared_at = (int64_t)st.st_mtime;
                if (!e->store_path || !e->hash || !e->name) {
                        free(e->store_path); free(e->hash);
                        free(e->name); free(e);
                        continue;
                }
                *tail = e;
                tail = &e->next;
        }
        closedir(d);

        /* Sort by name (simple insertion sort on the linked list). */
        share_entry_t *sorted = NULL;
        while (head) {
                share_entry_t *next = head->next;
                head->next = NULL;
                if (!sorted || strcmp(head->name, sorted->name) < 0) {
                        head->next = sorted;
                        sorted = head;
                } else {
                        share_entry_t *p = sorted;
                        while (p->next && strcmp(head->name, p->next->name) > 0)
                                p = p->next;
                        head->next = p->next;
                        p->next = head;
                }
                head = next;
        }
        return sorted;
}

void share_entry_list_free(share_entry_t *head)
{
        while (head) {
                share_entry_t *next = head->next;
                free(head->store_path);
                free(head->hash);
                free(head->name);
                free(head);
                head = next;
        }
}

/* ── share_remove ─────────────────────────────────────────────────── */

int share_remove(const char *hash)
{
        if (!hash) { errno = EINVAL; return -1; }

        DIR *d = opendir(STORE_ROOT);
        if (!d) return -1;

        int removed = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (!is_share_dir(ent->d_name)) continue;
                if (strncmp(ent->d_name, hash, 32) != 0) continue;
                char full[PATH_MAX];
                if (snprintf(full, sizeof(full), "%s/%s",
                             STORE_ROOT, ent->d_name) >= (int)sizeof(full))
                        continue;
                if (rmtree(full) == 0) {
                        removed = 1;
                        break;
                }
        }
        closedir(d);
        return removed ? 0 : -1;
}

/* ── share_push_to_sub ────────────────────────────────────────────── */

int share_push_to_sub(const char *store_path, const two9_sub_t *sub)
{
        if (!store_path || !sub) { errno = EINVAL; return -1; }
        if (!sub->urls || !sub->urls[0]) {
                fprintf(stderr, "209: sub '%s' has no URLs\n",
                        sub->name ? sub->name : "(unnamed)");
                return -1;
        }
        if (!sub->signing_key_file) {
                fprintf(stderr, "209: sub '%s' has no SigningKey, skipping push\n",
                        sub->name ? sub->name : "(unnamed)");
                return -1;
        }

        /* Load the signing key. */
        char *key_name = NULL;
        unsigned char sec[32];
        unsigned char pub[32];
        if (signing_load_keyfile(sub->signing_key_file, &key_name, pub, sec) != 0) {
                fprintf(stderr, "209: sub '%s': cannot load SigningKey %s\n",
                        sub->name ? sub->name : "(unnamed)",
                        sub->signing_key_file);
                return -1;
        }
        /* Prefer config-supplied KeyName. */
        if (sub->signing_key_name) {
                free(key_name);
                key_name = strdup(sub->signing_key_name);
        }

        /* Compute NAR hash + size for the index entry. */
        char nar_hex[65];
        size_t nar_size = 0;
        if (nar_hash_directory(store_path, nar_hex, &nar_size) != 0) {
                free(key_name);
                return -1;
        }
        char nar_hash_field[80];
        snprintf(nar_hash_field, sizeof(nar_hash_field), "sha256:%s", nar_hex);

        /* Extract the hash prefix from the store path for the index. */
        size_t path_hash_len = 0;
        const char *path_hash = narinfo_path_hash(store_path, &path_hash_len);
        char *hash_copy = NULL;
        if (path_hash) {
                hash_copy = malloc(path_hash_len + 1);
                if (hash_copy) {
                        memcpy(hash_copy, path_hash, path_hash_len);
                        hash_copy[path_hash_len] = '\0';
                }
        }

        /* The name field for the index: the part after "share-" in the
         * store path basename. */
        const char *base = strrchr(store_path, '/');
        base = base ? base + 1 : store_path;
        const char *name_part = strstr(base, "share-");
        name_part = name_part ? name_part + 6 : base;

        int failed = 0;
        int pushed = 0;
        for (size_t u = 0; sub->urls[u]; u++) {
                binary_cache_t *bc = binary_cache_new(sub->urls[u], NULL, 1);
                if (!bc) {
                        fprintf(stderr, "209: cannot init cache for %s\n",
                                sub->urls[u]);
                        failed = 1;
                        continue;
                }

                printf("pushing share to %s (sub '%s')\n",
                       sub->urls[u],
                       sub->name ? sub->name : "(unnamed)");

                if (binary_cache_push(bc, store_path, NULL, key_name, sec) != 0) {
                        fprintf(stderr, "  push failed on %s\n", sub->urls[u]);
                        failed = 1;
                } else {
                        pushed = 1;
                        /* Append to the index. Best-effort: a failure
                         * here doesn't fail the whole push (the NAR
                         * and narinfo are already up). */
                        cJSON *item = cJSON_CreateObject();
                        if (item) {
                                cJSON_AddStringToObject(item, "hash",
                                        hash_copy ? hash_copy : "");
                                cJSON_AddStringToObject(item, "name", name_part);
                                cJSON_AddStringToObject(item, "type", "share");
                                cJSON_AddNumberToObject(item, "nar_size",
                                        (double)nar_size);
                                cJSON_AddStringToObject(item, "nar_hash",
                                        nar_hash_field);
                                cJSON_AddNumberToObject(item, "pushed_at",
                                        (double)time(NULL));
                                cJSON_AddStringToObject(item, "signed_by",
                                        key_name ? key_name : "");
                                if (binary_cache_push_index_item(bc, item) != 0) {
                                        fprintf(stderr,
                                                "  warning: index update failed on %s\n",
                                                sub->urls[u]);
                                }
                                cJSON_Delete(item);
                        }
                }
                binary_cache_free(bc);
        }
        free(key_name);
        free(hash_copy);

        if (!pushed) return -1;
        return failed ? -1 : 0;
}

/* ── share_fetch_from_sub ─────────────────────────────────────────── */

int share_parse_uri(const char *uri, char **out_hash)
{
        if (!uri || !out_hash) { errno = EINVAL; return -1; }
        *out_hash = NULL;

        const char *h = uri;
        if (starts_with(uri, "nar://")) h = uri + 6;
        /* Validate: 32 chars of Nix base32. */
        if (strlen(h) != 32) {
                fprintf(stderr, "209: bad share URI: %s (expected nar://<32-char-hash>)\n",
                        uri);
                errno = EINVAL;
                return -1;
        }
        static const char b32[] = "0123456789abcdfghijklmnpqrsvwxyz";
        for (int i = 0; i < 32; i++) {
                if (strchr(b32, h[i]) == NULL) {
                        fprintf(stderr, "209: bad share URI: %s (invalid base32 char)\n",
                                uri);
                        errno = EINVAL;
                        return -1;
                }
        }
        *out_hash = strdup(h);
        return *out_hash ? 0 : -1;
}

int share_fetch_from_sub(const char *uri, const char *dest)
{
        char *hash = NULL;
        if (share_parse_uri(uri, &hash) != 0) return -1;

        if (!dest) dest = ".";
        struct stat dst_st;
        if (stat(dest, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
                fprintf(stderr, "209: get: destination %s is not a directory\n", dest);
                free(hash);
                return -1;
        }

        two9_config_t *cfg = two9_config_load();
        if (!cfg) {
                fprintf(stderr, "209: cannot load config\n");
                free(hash);
                return -1;
        }
        if (!cfg->subs) {
                fprintf(stderr, "209: no subs configured\n");
                two9_config_free(cfg);
                free(hash);
                return -1;
        }

        int ok = 0;
        for (two9_sub_t *s = cfg->subs; s && !ok; s = s->next) {
                if (!s->urls) continue;
                for (size_t u = 0; s->urls[u] && !ok; u++) {
                        /* Each cache needs its own keys list copy because
                         * binary_cache_new takes ownership. */
                        char **keys_copy = NULL;
                        if (s->public_keys) {
                                size_t n = 0;
                                while (s->public_keys[n]) n++;
                                keys_copy = calloc(n + 1, sizeof(char *));
                                if (!keys_copy) continue;
                                for (size_t i = 0; i < n; i++) {
                                        keys_copy[i] = strdup(s->public_keys[i]);
                                        if (!keys_copy[i]) {
                                                for (size_t j = 0; j < i; j++)
                                                        free(keys_copy[j]);
                                                free(keys_copy);
                                                keys_copy = NULL;
                                                break;
                                        }
                                }
                                if (!keys_copy) continue;
                        }
                        binary_cache_t *bc = binary_cache_new(s->urls[u], keys_copy,
                                                              s->allow_unsigned);
                        if (!bc) {
                                if (keys_copy) {
                                        for (size_t i = 0; keys_copy[i]; i++)
                                                free(keys_copy[i]);
                                        free(keys_copy);
                                }
                                continue;
                        }

                        narinfo_t *ni = binary_cache_lookup_by_hash(bc, hash);
                        if (!ni) {
                                binary_cache_free(bc);
                                continue;
                        }

                        printf("  found on %s (sub '%s')\n",
                               s->urls[u],
                               s->name ? s->name : "(unnamed)");

                        char *nar_buf = NULL;
                        size_t nar_len = 0;
                        if (binary_cache_download_nar(bc, ni, &nar_buf, &nar_len) != 0) {
                                fprintf(stderr, "  download failed on %s\n", s->urls[u]);
                                narinfo_free(ni);
                                binary_cache_free(bc);
                                continue;
                        }

                        /* Extract the NAR to dest. fmemopen gives us a
                         * FILE* over the in-memory buffer. */
                        FILE *mf = fmemopen(nar_buf, nar_len, "rb");
                        if (!mf) {
                                free(nar_buf);
                                narinfo_free(ni);
                                binary_cache_free(bc);
                                continue;
                        }
                        if (nar_extract(mf, dest) != 0) {
                                fprintf(stderr, "  extract failed on %s\n", s->urls[u]);
                                fclose(mf);
                                free(nar_buf);
                                narinfo_free(ni);
                                binary_cache_free(bc);
                                continue;
                        }
                        fclose(mf);
                        free(nar_buf);
                        narinfo_free(ni);
                        binary_cache_free(bc);
                        ok = 1;
                }
        }

        two9_config_free(cfg);
        free(hash);

        if (!ok) {
                fprintf(stderr, "209: share not found on any configured sub\n");
                return -1;
        }
        return 0;
}
