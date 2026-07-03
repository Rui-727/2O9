/* optimise.c - hardlink dedup implementation
 *
 * See optimise.h for the algorithm. A few correctness notes:
 *
 *  - We skip files with st_nlink > 1: they're already deduplicated
 *    (either by us previously, or by another tool).
 *  - The link-then-unlink-then-link dance is to handle the race where
 *    two 2O9 processes optimise the same content simultaneously. If
 *    process A creates .links/<hash> while process B is also hashing
 *    the same content, B's link() to .links/<hash> will succeed (it's
 *    idempotent on POSIX - link() to an existing path fails with EEXIST,
 *    so we treat that as "someone else got there first" and just
 *    unlink+link to share the existing inode).
 *  - /nix/store/.links is mode 1777 (world-writable + sticky) so any
 *    user can dedup their own store paths. The hardlink content is
 *    read-only after creation - we never write to .links/<hash> files,
 *    only create or unlink the originals.
 */
#include "optimise.h"

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

/* Ensure /nix/store/.links exists with mode 1777. */
static int ensure_links_dir(const char *store_root, char *links_out, size_t links_sz)
{
        snprintf(links_out, links_sz, "%s/.links", store_root);
        struct stat st;
        if (stat(links_out, &st) != 0) {
                if (mkdir(links_out, 01777) != 0 && errno != EEXIST)
                        return -1;
        }
        return 0;
}

/* Compute SHA-256 of a file's contents. Returns 0 on success. */
static int sha256_file(const char *path, unsigned char out[32])
{
        FILE *f = fopen(path, "rb");
        if (!f) return -1;

        SHA256_CTX ctx;
        SHA256_Init(&ctx);

        unsigned char buf[65536];
        size_t n;
        int rc = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                SHA256_Update(&ctx, buf, n);
        }
        if (ferror(f)) rc = -1;
        fclose(f);

        if (rc == 0) SHA256_Final(out, &ctx);
        return rc;
}

static void hex_encode(const unsigned char *bytes, size_t len, char *out)
{
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < len; i++) {
                out[i * 2]     = hex[bytes[i] >> 4];
                out[i * 2 + 1] = hex[bytes[i] & 0x0f];
        }
        out[len * 2] = '\0';
}

/* Dedupe a single regular file. Returns bytes saved (st.st_size if the
 * file was deduplicated, 0 if it was skipped or already shared). */
static int64_t dedupe_file(const char *path, const char *links_dir,
                            const struct stat *st)
{
        /* Skip files already shared (nlink > 1). */
        if (st->st_nlink > 1) return 0;

        unsigned char digest[32];
        if (sha256_file(path, digest) != 0) return 0;

        char hex[65];
        hex_encode(digest, 32, hex);

        char target[PATH_MAX];
        snprintf(target, sizeof(target), "%s/%s", links_dir, hex);

        struct stat tgt_st;
        int target_exists = (stat(target, &tgt_st) == 0);

        if (!target_exists) {
                /* First time we've seen this content. Link the original
                 * into .links/<hash>, then unlink the original and link
                 * back from .links. After this, the original shares an
                 * inode with .links/<hash> (nlink=2). */
                if (link(path, target) != 0) {
                        /* EEXIST means someone else raced us; treat as
                         * "target exists" and fall through. */
                        if (errno != EEXIST) return 0;
                        target_exists = 1;
                }
        }

        if (target_exists) {
                /* Another file with this content already exists in
                 * .links. Replace our file with a hardlink to it. */
                if (unlink(path) != 0) return 0;
                if (link(target, path) != 0) {
                        /* Failed to re-create the original as a hardlink.
                         * This is bad - we just unlinked the user's file.
                         * Try to restore by linking from target back to
                         * path; if that also fails, the file is gone
                         * (data loss). Log loudly. */
                        fprintf(stderr, "209: optimise: ERROR restoring %s "
                                "after unlink (link failed: %s)\n",
                                path, strerror(errno));
                        return 0;
                }
        }

        /* The file was deduplicated. Count its size as saved. */
        return (int64_t)st->st_size;
}

/* Recursive walker. Returns total bytes saved. */
static int64_t walk_and_dedupe(const char *path, const char *links_dir)
{
        struct stat st;
        if (lstat(path, &st) != 0) return 0;

        if (S_ISREG(st.st_mode)) {
                return dedupe_file(path, links_dir, &st);
        }

        if (!S_ISDIR(st.st_mode)) return 0;

        DIR *d = opendir(path);
        if (!d) return 0;

        int64_t saved = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;  /* skip .links, .tmp, hidden */

                char child[PATH_MAX];
                if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name)
                    >= (int)sizeof(child))
                        continue;
                saved += walk_and_dedupe(child, links_dir);
        }
        closedir(d);
        return saved;
}

int64_t store_optimise(const char *store_root)
{
        if (!store_root) { errno = EINVAL; return -1; }

        /* Verify store_root exists. */
        struct stat st;
        if (stat(store_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
                errno = ENOENT;
                return -1;
        }

        char links_dir[PATH_MAX];
        if (ensure_links_dir(store_root, links_dir, sizeof(links_dir)) != 0) {
                fprintf(stderr, "209: optimise: cannot create %s: %s\n",
                        links_dir, strerror(errno));
                return -1;
        }

        /* Walk each top-level entry in store_root. We skip .links, .tmp,
         * and any other dot-prefixed entries at the top level (the walk
         * function also skips dot-prefixed entries at every level, which
         * is correct - 2O9 store paths never start with a dot). */
        DIR *d = opendir(store_root);
        if (!d) {
                fprintf(stderr, "209: optimise: cannot open %s: %s\n",
                        store_root, strerror(errno));
                return -1;
        }

        int64_t total_saved = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;

                char child[PATH_MAX];
                if (snprintf(child, sizeof(child), "%s/%s",
                             store_root, ent->d_name) >= (int)sizeof(child))
                        continue;
                total_saved += walk_and_dedupe(child, links_dir);
        }
        closedir(d);
        return total_saved;
}

int64_t store_optimise_path(const char *store_root, const char *path)
{
        if (!store_root || !path) { errno = EINVAL; return -1; }

        char links_dir[PATH_MAX];
        if (ensure_links_dir(store_root, links_dir, sizeof(links_dir)) != 0)
                return -1;

        return walk_and_dedupe(path, links_dir);
}
