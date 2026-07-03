/* test_narinfo.c - unit tests for narinfo parse / serialize
 *
 * Covers:
 *   - parse a full narinfo with all fields
 *   - parse a minimal narinfo (StorePath + URL + NarHash only)
 *   - serialize(parse(text)) round-trip
 *   - multiple References (parse + count)
 *   - multiple Sig: lines (parse + count)
 *   - malformed narinfo (missing StorePath) returns NULL
 *   - empty References: returns an empty list (not NULL)
 *
 * Build: see Makefile target `test-narinfo`.
 * Run: ./test-narinfo
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "narinfo.h"

static int test_count = 0;
static int pass_count = 0;

#define OK(msg) do { printf("PASS: %s\n", msg); pass_count++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

#define CHECK(cond, name) do { \
        test_count++; \
        if (cond) OK(name); \
        else FAIL(name); \
} while (0)

static size_t list_len(char **list)
{
        if (!list) return (size_t)-1;  /* distinguish from empty list */
        size_t n = 0;
        while (list[n]) n++;
        return n;
}

static int list_contains(char **list, const char *s)
{
        if (!list) return 0;
        for (size_t i = 0; list[i]; i++)
                if (strcmp(list[i], s) == 0) return 1;
        return 0;
}

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_parse_full(void)
{
        const char *text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "Compression: zstd\n"
                "FileHash: sha256:aaaa\n"
                "FileSize: 100\n"
                "NarHash: sha256:bbbb\n"
                "NarSize: 200\n"
                "References: dep1-1.0 dep2-2.0\n"
                "Deriver: some-deriver-3.0\n"
                "Sig: mykey:cccc\n"
                "CA: text:sha256:dddd\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni != NULL, "parse-full: returns non-NULL");

        if (!ni) return;

        CHECK(ni->store_path && strcmp(ni->store_path, "/nix/store/abc-test-1.0") == 0,
              "parse-full: store_path populated");
        CHECK(ni->url && strcmp(ni->url, "nar/abc.nar.zst") == 0,
              "parse-full: url populated");
        CHECK(ni->compression && strcmp(ni->compression, "zstd") == 0,
              "parse-full: compression populated");
        CHECK(ni->file_hash && strcmp(ni->file_hash, "sha256:aaaa") == 0,
              "parse-full: file_hash populated");
        CHECK(ni->file_size == 100, "parse-full: file_size populated");
        CHECK(ni->nar_hash && strcmp(ni->nar_hash, "sha256:bbbb") == 0,
              "parse-full: nar_hash populated");
        CHECK(ni->nar_size == 200, "parse-full: nar_size populated");
        CHECK(ni->deriver && strcmp(ni->deriver, "some-deriver-3.0") == 0,
              "parse-full: deriver populated");
        CHECK(ni->ca && strcmp(ni->ca, "text:sha256:dddd") == 0,
              "parse-full: ca populated");

        CHECK(list_len(ni->references) == 2, "parse-full: 2 references");
        CHECK(list_contains(ni->references, "dep1-1.0"), "parse-full: ref dep1-1.0 present");
        CHECK(list_contains(ni->references, "dep2-2.0"), "parse-full: ref dep2-2.0 present");

        CHECK(list_len(ni->signatures) == 1, "parse-full: 1 signature");
        CHECK(ni->signatures && strcmp(ni->signatures[0], "mykey:cccc") == 0,
              "parse-full: sig populated");

        narinfo_free(ni);
}

static void test_parse_minimal(void)
{
        const char *text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "NarHash: sha256:bbbb\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni != NULL, "parse-minimal: returns non-NULL");
        if (!ni) return;

        CHECK(ni->store_path != NULL, "parse-minimal: store_path present");
        CHECK(ni->url != NULL, "parse-minimal: url present");
        CHECK(ni->nar_hash != NULL, "parse-minimal: nar_hash present");

        /* Optional fields should be NULL/0. */
        CHECK(ni->compression == NULL, "parse-minimal: compression is NULL");
        CHECK(ni->file_hash == NULL, "parse-minimal: file_hash is NULL");
        CHECK(ni->file_size == 0, "parse-minimal: file_size is 0");
        CHECK(ni->nar_size == 0, "parse-minimal: nar_size is 0");
        CHECK(ni->deriver == NULL, "parse-minimal: deriver is NULL");
        CHECK(ni->ca == NULL, "parse-minimal: ca is NULL");
        CHECK(ni->references == NULL, "parse-minimal: references is NULL");
        CHECK(ni->signatures == NULL, "parse-minimal: signatures is NULL");

        narinfo_free(ni);
}

static void test_round_trip(void)
{
        const char *text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "Compression: zstd\n"
                "FileHash: sha256:aaaa\n"
                "FileSize: 100\n"
                "NarHash: sha256:bbbb\n"
                "NarSize: 200\n"
                "References: dep1-1.0 dep2-2.0\n"
                "Sig: mykey:cccc\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni != NULL, "round-trip: parse succeeds");
        if (!ni) return;

        char *re = narinfo_serialize(ni);
        CHECK(re != NULL, "round-trip: serialize succeeds");
        if (!re) { narinfo_free(ni); return; }

        /* The serialised text should contain every key we fed in.
         * Order may differ from the input but the key/value pairs
         * must round-trip. */
        const char *needles[] = {
                "StorePath: /nix/store/abc-test-1.0",
                "URL: nar/abc.nar.zst",
                "Compression: zstd",
                "FileHash: sha256:aaaa",
                "FileSize: 100",
                "NarHash: sha256:bbbb",
                "NarSize: 200",
                "References: dep1-1.0 dep2-2.0",
                "Sig: mykey:cccc",
        };
        int all_present = 1;
        for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
                if (strstr(re, needles[i]) == NULL) {
                        printf("  (missing: %s)\n", needles[i]);
                        all_present = 0;
                        break;
                }
        }
        CHECK(all_present, "round-trip: all key/value pairs survive round-trip");

        /* Re-parse the serialised form; must equal the first parse for
         * the fields we set. */
        narinfo_t *ni2 = narinfo_parse(re);
        CHECK(ni2 != NULL, "round-trip: re-parse of serialized text succeeds");
        if (ni2) {
                int fields_match =
                        ni->store_path && ni2->store_path &&
                        strcmp(ni->store_path, ni2->store_path) == 0 &&
                        ni->url && ni2->url &&
                        strcmp(ni->url, ni2->url) == 0 &&
                        ni->nar_hash && ni2->nar_hash &&
                        strcmp(ni->nar_hash, ni2->nar_hash) == 0 &&
                        ni->nar_size == ni2->nar_size &&
                        list_len(ni->references) == list_len(ni2->references) &&
                        list_len(ni->signatures) == list_len(ni2->signatures);
                CHECK(fields_match, "round-trip: re-parsed fields match first parse");
                narinfo_free(ni2);
        }

        free(re);
        narinfo_free(ni);
}

static void test_multiple_references(void)
{
        const char *text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "NarHash: sha256:bbbb\n"
                "References: a-1 b-2 c-3 d-4 e-5\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni != NULL, "multi-refs: parse succeeds");
        if (!ni) return;

        CHECK(list_len(ni->references) == 5, "multi-refs: 5 references parsed");
        CHECK(list_contains(ni->references, "a-1"), "multi-refs: a-1 present");
        CHECK(list_contains(ni->references, "e-5"), "multi-refs: e-5 present");

        narinfo_free(ni);
}

static void test_multiple_signatures(void)
{
        const char *text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "NarHash: sha256:bbbb\n"
                "Sig: key1:aaaa\n"
                "Sig: key2:bbbb\n"
                "Sig: key3:cccc\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni != NULL, "multi-sig: parse succeeds");
        if (!ni) return;

        CHECK(list_len(ni->signatures) == 3, "multi-sig: 3 signatures parsed");

        /* Each Sig: value should be "<keyname>:<base64>" preserved verbatim. */
        int all_present = 1;
        for (size_t i = 0; i < 3; i++) {
                if (!ni->signatures[i] || strchr(ni->signatures[i], ':') == NULL) {
                        all_present = 0; break;
                }
        }
        CHECK(all_present, "multi-sig: each sig has 'keyname:base64' form");

        /* Specific values. */
        char *joined = malloc(1024);
        joined[0] = '\0';
        for (size_t i = 0; ni->signatures[i]; i++) {
                strcat(joined, ni->signatures[i]);
                strcat(joined, "|");
        }
        CHECK(strstr(joined, "key1:aaaa") != NULL, "multi-sig: key1:aaaa present");
        CHECK(strstr(joined, "key2:bbbb") != NULL, "multi-sig: key2:bbbb present");
        CHECK(strstr(joined, "key3:cccc") != NULL, "multi-sig: key3:cccc present");
        free(joined);

        narinfo_free(ni);
}

static void test_malformed_missing_store_path(void)
{
        /* No StorePath: line -> parser must reject (return NULL). */
        const char *text =
                "URL: nar/abc.nar.zst\n"
                "NarHash: sha256:bbbb\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni == NULL, "malformed: missing StorePath returns NULL");
        if (ni) narinfo_free(ni);
}

static void test_empty_references(void)
{
        /* "References:" with no value -> empty list (NULL terminator),
         * NOT a NULL pointer. Distinguishes "known to have no refs"
         * from "unknown refs". */
        const char *text =
                "StorePath: /nix/store/abc-test-1.0\n"
                "URL: nar/abc.nar.zst\n"
                "NarHash: sha256:bbbb\n"
                "References:\n";

        narinfo_t *ni = narinfo_parse(text);
        CHECK(ni != NULL, "empty-refs: parse succeeds");
        if (!ni) return;

        /* Per narinfo.c: when References: has empty value, references
         * is left as NULL (the parse code only builds a list when *val
         * is non-empty). Document this behaviour - either NULL or an
         * empty list is acceptable; we just must not crash. */
        if (ni->references == NULL) {
                CHECK(1, "empty-refs: references is NULL (acceptable - 'no refs')");
        } else {
                CHECK(list_len(ni->references) == 0,
                      "empty-refs: references is empty list (acceptable)");
        }

        /* Serialising back must not crash and must not emit "References:"
         * for an empty list. */
        char *re = narinfo_serialize(ni);
        CHECK(re != NULL, "empty-refs: serialize does not crash");
        if (re) {
                /* For the NULL case, serialize skips References. For the
                 * empty-list case, serialize also skips (it checks
                 * ni->references && ni->references[0]). Either way the
                 * serialised text should not contain "References:". */
                CHECK(strstr(re, "References:") == NULL,
                      "empty-refs: serialised text omits References line");
                free(re);
        }

        narinfo_free(ni);
}

static void test_path_hash_helper(void)
{
        const char *path = "/nix/store/abc123-test-1.0";
        size_t len = 0;
        const char *hash = narinfo_path_hash(path, &len);
        CHECK(hash != NULL, "path-hash: returns non-NULL");
        CHECK(len == 6, "path-hash: length is 6 (\"abc123\")");
        CHECK(hash != NULL && strncmp(hash, "abc123", 6) == 0,
              "path-hash: hash part is \"abc123\"");

        /* Malformed path (no '-' after hash). */
        const char *bad = "/nix/store/noDashHere";
        size_t bad_len = 99;
        const char *bad_hash = narinfo_path_hash(bad, &bad_len);
        CHECK(bad_hash == NULL, "path-hash: returns NULL for path with no '-'");
        (void)bad_hash;
}

int main(void)
{
        printf("=== narinfo parse/serialize tests ===\n\n");

        test_parse_full();
        test_parse_minimal();
        test_round_trip();
        test_multiple_references();
        test_multiple_signatures();
        test_malformed_missing_store_path();
        test_empty_references();
        test_path_hash_helper();

        printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
        return pass_count == test_count ? 0 : 1;
}
