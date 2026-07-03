/* test_nar.c - unit tests for NAR serialisation
 *
 * Covers the cases called out in the more-tests task:
 *   - empty dir / single regular file / nested dirs / symlink / executable
 *   - sort order determinism (same contents, different creation order)
 *   - nar_extract(nar_dump(tree)) round-trip
 *   - large file (>1 MB) not truncated
 *
 * Each test builds a tree in a temp dir, hashes it, and either compares
 * the hash against a hardcoded expected value (computed independently
 * from the documented NAR wire format) or verifies determinism by
 * rebuilding and re-hashing. Round-trip tests compare file contents,
 * modes, and symlink targets between original and extracted trees.
 *
 * Build: see Makefile target `test-nar`.
 * Run: ./test-nar
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include "nar.h"

static int test_count = 0;
static int pass_count = 0;

#define OK(msg) do { printf("PASS: %s\n", msg); pass_count++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

#define CHECK(cond, name) do { \
        test_count++; \
        if (cond) OK(name); \
        else FAIL(name); \
} while (0)

/* Build the test root in a fresh mkdtemp directory. Caller frees with
 * rmtree() + free(). Returns the malloc'd path or NULL on failure. */
static char *make_temp_dir(const char *tag)
{
        char tmpl[128];
        snprintf(tmpl, sizeof(tmpl), "/tmp/209-test-nar-%s-XXXXXX", tag);
        char *d = strdup(tmpl);
        if (!d) return NULL;
        if (mkdtemp(d) == NULL) { free(d); return NULL; }
        return d;
}

static int rmtree(const char *path)
{
        DIR *d = opendir(path);
        if (!d) {
                /* Probably a file or symlink */
                return unlink(path);
        }
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                        continue;
                char child[1024];
                snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                struct stat st;
                if (lstat(child, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) rmtree(child);
                        else unlink(child);
                }
        }
        closedir(d);
        rc = rmdir(path);
        return rc;
}

/* Create a file with the given content. Returns 0 on success. */
static int write_file(const char *path, const char *content, mode_t mode)
{
        FILE *f = fopen(path, "wb");
        if (!f) return -1;
        if (content) fputs(content, f);
        fclose(f);
        if (mode != 0) chmod(path, mode);
        return 0;
}

/* Recursively compare two trees. Returns 0 if identical (contents,
 * regular/symlink/dir type, executable bit, symlink target), nonzero
 * otherwise. Prints a message on first mismatch. */
static int trees_match(const char *a, const char *b, int verbose)
{
        struct stat sa, sb;
        if (lstat(a, &sa) < 0) {
                if (verbose) printf("  trees_match: lstat(%s) failed: %s\n",
                                    a, strerror(errno));
                return -1;
        }
        if (lstat(b, &sb) < 0) {
                if (verbose) printf("  trees_match: lstat(%s) failed: %s\n",
                                    b, strerror(errno));
                return -1;
        }
        if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
                if (verbose) printf("  trees_match: type mismatch at %s vs %s\n",
                                    a, b);
                return -1;
        }
        if (S_ISLNK(sa.st_mode)) {
                char ta[1024], tb[1024];
                ssize_t na = readlink(a, ta, sizeof(ta) - 1);
                ssize_t nb = readlink(b, tb, sizeof(tb) - 1);
                if (na < 0 || nb < 0 || na != nb || memcmp(ta, tb, na) != 0) {
                        if (verbose) printf("  trees_match: symlink target mismatch (%s vs %s)\n",
                                            a, b);
                        return -1;
                }
                return 0;
        }
        if (S_ISREG(sa.st_mode)) {
                /* Check executable bit (only the user-x bit matters for NAR). */
                int ax = (sa.st_mode & S_IXUSR) ? 1 : 0;
                int bx = (sb.st_mode & S_IXUSR) ? 1 : 0;
                if (ax != bx) {
                        if (verbose) printf("  trees_match: exec bit mismatch at %s\n", a);
                        return -1;
                }
                if (sa.st_size != sb.st_size) {
                        if (verbose) printf("  trees_match: size mismatch at %s (%lld vs %lld)\n",
                                            a, (long long)sa.st_size, (long long)sb.st_size);
                        return -1;
                }
                FILE *fa = fopen(a, "rb");
                FILE *fb = fopen(b, "rb");
                if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return -1; }
                char ba[8192], bb[8192];
                size_t na, nb;
                do {
                        na = fread(ba, 1, sizeof(ba), fa);
                        nb = fread(bb, 1, sizeof(bb), fb);
                        if (na != nb || memcmp(ba, bb, na) != 0) {
                                fclose(fa); fclose(fb);
                                if (verbose) printf("  trees_match: content mismatch at %s\n", a);
                                return -1;
                        }
                } while (na > 0);
                fclose(fa); fclose(fb);
                return 0;
        }
        if (S_ISDIR(sa.st_mode)) {
                DIR *da = opendir(a);
                DIR *db = opendir(b);
                if (!da || !db) { if (da) closedir(da); if (db) closedir(db); return -1; }
                struct dirent *ea;
                int rc = 0;
                while ((ea = readdir(da)) != NULL) {
                        if (strcmp(ea->d_name, ".") == 0 ||
                            strcmp(ea->d_name, "..") == 0) continue;
                        char ca[2048], cb[2048];
                        snprintf(ca, sizeof(ca), "%s/%s", a, ea->d_name);
                        snprintf(cb, sizeof(cb), "%s/%s", b, ea->d_name);
                        if (trees_match(ca, cb, verbose) != 0) { rc = -1; break; }
                }
                /* Also ensure b has no extra entries (compare counts). */
                if (rc == 0) {
                        rewinddir(db);
                        int bcount = 0;
                        struct dirent *eb;
                        while ((eb = readdir(db)) != NULL) {
                                if (strcmp(eb->d_name, ".") == 0 ||
                                    strcmp(eb->d_name, "..") == 0) continue;
                                bcount++;
                        }
                        rewinddir(da);
                        int acount = 0;
                        while ((ea = readdir(da)) != NULL) {
                                if (strcmp(ea->d_name, ".") == 0 ||
                                    strcmp(ea->d_name, "..") == 0) continue;
                                acount++;
                        }
                        if (acount != bcount) {
                                if (verbose) printf("  trees_match: entry count mismatch at %s (%d vs %d)\n",
                                                    a, acount, bcount);
                                rc = -1;
                        }
                }
                closedir(da); closedir(db);
                return rc;
        }
        return -1;
}

/* ── Test cases ───────────────────────────────────────────────────── */

static void test_empty_dir_hash(void)
{
        char *d = make_temp_dir("empty");
        if (!d) { CHECK(0, "empty dir: mkdtemp"); return; }

        char hash[65];
        size_t size = 0;
        int rc = nar_hash_directory(d, hash, &size);

        /* Computed independently from the documented wire format:
         *   "nix-archive-1\ntype:directory\nend\n" (33 bytes). */
        CHECK(rc == 0, "empty dir: nar_hash_directory returns 0");
        CHECK(strcmp(hash, "0bf2ecf38e6640d6ba2adb113f35478fc5390c3e3fb61edb9460666d9184c1d9") == 0,
              "empty dir: hash matches expected");
        CHECK(size == 33, "empty dir: NAR size is 33 bytes");

        rmtree(d); free(d);
}

static void test_single_regular_file_hash(void)
{
        /* Pass the file path directly (not the containing dir) so the
         * NAR is a single-file archive. nar_hash_directory accepts any
         * filesystem node - the "directory" in the name is historical. */
        char *d = make_temp_dir("regfile");
        if (!d) { CHECK(0, "regfile: mkdtemp"); return; }
        char p[1024];
        snprintf(p, sizeof(p), "%s/hello.txt", d);
        write_file(p, "hello\n", 0644);

        char hash[65];
        size_t size = 0;
        int rc = nar_hash_directory(p, hash, &size);

        /* nix-archive-1\n + type:regular\n + contents:<BE 8 bytes=6>hello\n = 50 bytes */
        CHECK(rc == 0, "regfile: nar_hash_directory returns 0");
        CHECK(strcmp(hash, "bc835131c33a4dabe3005c06799fecb8a533fff194141c21329582df51ef80f7") == 0,
              "regfile: hash matches expected");
        CHECK(size == 50, "regfile: NAR size is 50 bytes");

        rmtree(d); free(d);
}

static void test_executable_file_hash(void)
{
        char *d = make_temp_dir("execfile");
        if (!d) { CHECK(0, "execfile: mkdtemp"); return; }
        char p[1024];
        snprintf(p, sizeof(p), "%s/hello.sh", d);
        write_file(p, "hello\n", 0755);

        char hash[65];
        size_t size = 0;
        int rc = nar_hash_directory(p, hash, &size);

        /* Adds "executable\n" (11 bytes) before "contents:". 50 + 11 = 61 bytes. */
        CHECK(rc == 0, "execfile: nar_hash_directory returns 0");
        CHECK(strcmp(hash, "5aa5cb53914ed82b421e4e74a9a1470aee503fa948b413ffc1eac6684ec4bcb6") == 0,
              "execfile: hash matches expected (executable flag emitted)");
        CHECK(size == 61, "execfile: NAR size is 61 bytes");

        rmtree(d); free(d);
}

static void test_symlink_hash(void)
{
        char *d = make_temp_dir("symlink");
        if (!d) { CHECK(0, "symlink: mkdtemp"); return; }
        char p[1024];
        snprintf(p, sizeof(p), "%s/link", d);
        if (symlink("target", p) != 0) { CHECK(0, "symlink: symlink() failed"); rmtree(d); free(d); return; }

        char hash[65];
        size_t size = 0;
        int rc = nar_hash_directory(p, hash, &size);

        /* nix-archive-1\n + type:symlink\n + target:<BE 8=6>target = 14+13+7+8+6 = 48 bytes */
        CHECK(rc == 0, "symlink: nar_hash_directory returns 0");
        CHECK(strcmp(hash, "14b976557ca04f8feda654f125aeabbe30e4b2f5e927b5cb3fd2b895aa6bb6c4") == 0,
              "symlink: hash matches expected");
        CHECK(size == 48, "symlink: NAR size is 48 bytes");

        rmtree(d); free(d);
}

static void test_nested_dirs_hash(void)
{
        /* root/a/b.txt("hello\n"), root/c.txt("world\n") */
        char *d = make_temp_dir("nested");
        if (!d) { CHECK(0, "nested: mkdtemp"); return; }
        char sub[1024], bf[1024], cf[1024];
        snprintf(sub, sizeof(sub), "%s/a", d);
        mkdir(sub, 0755);
        snprintf(bf, sizeof(bf), "%s/a/b.txt", d);
        write_file(bf, "hello\n", 0644);
        snprintf(cf, sizeof(cf), "%s/c.txt", d);
        write_file(cf, "world\n", 0644);

        char hash[65];
        size_t size = 0;
        int rc = nar_hash_directory(d, hash, &size);

        CHECK(rc == 0, "nested: nar_hash_directory returns 0");
        CHECK(strcmp(hash, "f55356ded1ca58896a92b98ee477ee7d462c4c5ecb88f80d933965f62b74ebbc") == 0,
              "nested: hash matches expected");
        CHECK(size == 192, "nested: NAR size is 192 bytes");

        rmtree(d); free(d);
}

static void test_sort_order_determinism(void)
{
        /* Two trees with the same contents but created in different
         * orders (mkdir + add 'z' first, then 'a'; vs the reverse)
         * must produce identical NAR hashes (alphasort normalises). */
        char *d1 = make_temp_dir("sort1");
        char *d2 = make_temp_dir("sort2");
        if (!d1 || !d2) { CHECK(0, "sort: mkdtemp"); free(d1); free(d2); return; }

        /* Tree 1: create z first, then a */
        char p[1024];
        snprintf(p, sizeof(p), "%s/z.txt", d1); write_file(p, "z\n", 0644);
        snprintf(p, sizeof(p), "%s/a.txt", d1); write_file(p, "a\n", 0644);
        snprintf(p, sizeof(p), "%s/m.txt", d1); write_file(p, "m\n", 0644);

        /* Tree 2: create a first, then m, then z (same final contents) */
        snprintf(p, sizeof(p), "%s/a.txt", d2); write_file(p, "a\n", 0644);
        snprintf(p, sizeof(p), "%s/m.txt", d2); write_file(p, "m\n", 0644);
        snprintf(p, sizeof(p), "%s/z.txt", d2); write_file(p, "z\n", 0644);

        char h1[65], h2[65];
        int r1 = nar_hash_directory(d1, h1, NULL);
        int r2 = nar_hash_directory(d2, h2, NULL);

        CHECK(r1 == 0 && r2 == 0, "sort: both hashes computed");
        CHECK(strcmp(h1, h2) == 0, "sort: different creation order produces same hash");

        rmtree(d1); rmtree(d2); free(d1); free(d2);
}

/* Dump a tree to a file, then extract it into a fresh dir, and compare
 * the trees. */
static void test_round_trip(const char *tag, int (*build)(const char *dir),
                            const char *desc)
{
        char *src = make_temp_dir(tag);
        char *dump = NULL;
        char *extracted = NULL;
        if (!src) { CHECK(0, desc); return; }

        if (build(src) != 0) {
                CHECK(0, desc);
                goto cleanup;
        }

        /* Dump NAR to a temp file. */
        char dump_path[1024];
        snprintf(dump_path, sizeof(dump_path), "%s/../dump-%s.nar", src, tag);
        FILE *out = fopen(dump_path, "wb");
        if (!out) { CHECK(0, desc); goto cleanup; }
        if (nar_dump(src, out) != 0) { fclose(out); CHECK(0, desc); goto cleanup; }
        fclose(out);

        /* Extract into a fresh dir. */
        extracted = make_temp_dir("extracted");
        if (!extracted) { CHECK(0, desc); goto cleanup; }
        FILE *in = fopen(dump_path, "rb");
        if (!in) { CHECK(0, desc); goto cleanup; }
        if (nar_extract(in, extracted) != 0) {
                fclose(in); CHECK(0, desc); goto cleanup;
        }
        fclose(in);

        /* Compare trees. */
        CHECK(trees_match(src, extracted, 1) == 0, desc);

cleanup:
        if (extracted) { rmtree(extracted); free(extracted); }
        if (src) {
                char dump_path2[1024];
                snprintf(dump_path2, sizeof(dump_path2), "%s/../dump-%s.nar", src, tag);
                unlink(dump_path2);
                rmtree(src); free(src);
        }
        (void)dump;
}

/* Builder callbacks for round-trip tests. */

static int build_simple_files(const char *dir)
{
        char p[1024];
        snprintf(p, sizeof(p), "%s/a.txt", dir);
        if (write_file(p, "alpha\n", 0644) != 0) return -1;
        snprintf(p, sizeof(p), "%s/b.txt", dir);
        if (write_file(p, "beta beta\n", 0644) != 0) return -1;
        return 0;
}

static int build_nested_with_exec(const char *dir)
{
        char p[2048];
        snprintf(p, sizeof(p), "%s/sub", dir);
        if (mkdir(p, 0755) != 0) return -1;
        snprintf(p, sizeof(p), "%s/sub/run.sh", dir);
        if (write_file(p, "#!/bin/sh\necho hi\n", 0755) != 0) return -1;
        snprintf(p, sizeof(p), "%s/sub/data", dir);
        if (write_file(p, "data bytes\n", 0644) != 0) return -1;
        snprintf(p, sizeof(p), "%s/top.txt", dir);
        if (write_file(p, "top\n", 0644) != 0) return -1;
        return 0;
}

static int build_with_symlink(const char *dir)
{
        char p[1024];
        snprintf(p, sizeof(p), "%s/real.txt", dir);
        if (write_file(p, "real content\n", 0644) != 0) return -1;
        snprintf(p, sizeof(p), "%s/link", dir);
        if (symlink("real.txt", p) != 0) return -1;
        /* Absolute-path symlink target - tests target length encoding. */
        snprintf(p, sizeof(p), "%s/abslink", dir);
        if (symlink("/usr/bin/true", p) != 0) return -1;
        return 0;
}

static int build_deep_nesting(const char *dir)
{
        char p[2048];
        snprintf(p, sizeof(p), "%s/a", dir); if (mkdir(p, 0755) != 0) return -1;
        snprintf(p, sizeof(p), "%s/a/b", dir); if (mkdir(p, 0755) != 0) return -1;
        snprintf(p, sizeof(p), "%s/a/b/c", dir); if (mkdir(p, 0755) != 0) return -1;
        snprintf(p, sizeof(p), "%s/a/b/c/deep.txt", dir);
        if (write_file(p, "deep\n", 0644) != 0) return -1;
        return 0;
}

static void test_large_file(void)
{
        /* 1.5 MB file: ensures the writer/reader don't truncate at
         * 64 KiB (the chunk size used by dump_node). The file lives
         * inside a temp dir; the NAR dump is written to a SIBLING temp
         * file (not inside the dir being dumped) so nar_dump doesn't
         * pick it up as a directory entry mid-write. */
        char *d = make_temp_dir("large");
        char *dump_dir = make_temp_dir("large-dump");
        if (!d || !dump_dir) { CHECK(0, "large: mkdtemp"); free(d); free(dump_dir); return; }

        char p[1024];
        snprintf(p, sizeof(p), "%s/big.bin", d);
        FILE *f = fopen(p, "wb");
        if (!f) { CHECK(0, "large: create file"); rmtree(d); free(d); rmtree(dump_dir); free(dump_dir); return; }
        /* Write 1.5 MB of predictable bytes (i % 256). */
        size_t total = 1024 * 1024 + 512 * 1024;
        unsigned char buf[65536];
        size_t written = 0;
        while (written < total) {
                size_t chunk = total - written;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                for (size_t i = 0; i < chunk; i++)
                        buf[i] = (unsigned char)((written + i) & 0xff);
                if (fwrite(buf, 1, chunk, f) != chunk) {
                        fclose(f); CHECK(0, "large: write"); rmtree(d); free(d); rmtree(dump_dir); free(dump_dir); return;
                }
                written += chunk;
        }
        fclose(f);

        /* Compute NAR hash + size of the dir containing big.bin.
         * Framing for a single-entry dir:
         *   nix-archive-1\n        (14)
         *   type:directory\n       (15)
         *   entry\n                (6)
         *   name:<BE 8>big.bin     (5 + 8 + 7 = 20)
         *   type:regular\n         (13)
         *   contents:<BE 8><file>  (9 + 8 + total)
         *   end\n                  (4)
         * Total = 14 + 15 + 6 + 20 + 13 + 9 + 8 + total + 4 = 89 + total. */
        char hash[65];
        size_t size = 0;
        int rc = nar_hash_directory(d, hash, &size);
        CHECK(rc == 0, "large: nar_hash_directory returns 0");

        size_t expected_size = 89 + total;
        CHECK(size == expected_size, "large: NAR size matches dir framing + file size");

        /* Round-trip: dump + extract, verify size + contents preserved. */
        char dump_path[1024];
        snprintf(dump_path, sizeof(dump_path), "%s/dump.nar", dump_dir);
        FILE *out = fopen(dump_path, "wb");
        if (out) {
                int drc = nar_dump(d, out);
                fclose(out);
                CHECK(drc == 0, "large: nar_dump returns 0");

                char *ex = make_temp_dir("large-ex");
                if (ex) {
                        FILE *in = fopen(dump_path, "rb");
                        if (in) {
                                int erc = nar_extract(in, ex);
                                fclose(in);
                                CHECK(erc == 0, "large: nar_extract returns 0");
                                /* Verify the extracted file size. */
                                char ep[1024];
                                snprintf(ep, sizeof(ep), "%s/big.bin", ex);
                                struct stat st;
                                if (stat(ep, &st) == 0) {
                                        CHECK((size_t)st.st_size == total,
                                              "large: extracted file size matches original");
                                } else {
                                        CHECK(0, "large: extracted file exists");
                                }
                                /* Verify contents. */
                                FILE *ef = fopen(ep, "rb");
                                if (ef) {
                                        unsigned char rb[65536];
                                        size_t got = 0;
                                        int ok = 1;
                                        size_t off = 0;
                                        size_t n;
                                        while ((n = fread(rb, 1, sizeof(rb), ef)) > 0) {
                                                for (size_t i = 0; i < n; i++) {
                                                        if (rb[i] != (unsigned char)((off + i) & 0xff)) {
                                                                ok = 0; break;
                                                        }
                                                }
                                                if (!ok) break;
                                                off += n; got += n;
                                        }
                                        fclose(ef);
                                        CHECK(ok && got == total,
                                              "large: extracted file contents match byte-for-byte");
                                }
                        }
                        rmtree(ex); free(ex);
                }
        }

        rmtree(d); free(d);
        rmtree(dump_dir); free(dump_dir);
}

int main(void)
{
        printf("=== NAR serialisation tests ===\n\n");

        test_empty_dir_hash();
        test_single_regular_file_hash();
        test_executable_file_hash();
        test_symlink_hash();
        test_nested_dirs_hash();
        test_sort_order_determinism();

        test_round_trip("simple", build_simple_files,
                        "round-trip: simple files preserve contents + modes");
        test_round_trip("nestexec", build_nested_with_exec,
                        "round-trip: nested dirs with exec file");
        test_round_trip("syms", build_with_symlink,
                        "round-trip: symlinks (relative + absolute targets)");
        test_round_trip("deep", build_deep_nesting,
                        "round-trip: deep nesting");

        test_large_file();

        printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
        return pass_count == test_count ? 0 : 1;
}
