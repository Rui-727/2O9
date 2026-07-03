/* test_nix_eval_edge.c - Nix evaluator edge cases
 *
 * Companion to test_nix_eval.c. Each case is a Nix expression that
 * exercises a corner of the language the main test suite doesn't
 * already cover (empty containers, nested let, with shadowing,
 * builtins.map, string interpolation with toString). The expected
 * substring is matched against the JSON the evaluator emits.
 *
 * Build: see Makefile target `test-nix-eval-edge`.
 * Run: ./test-nix-eval-edge
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nix_eval.h"

static int test_count = 0;
static int pass_count = 0;

#define TEST(name, source, expected_substr) do { \
    test_count++; \
    char *err = NULL; \
    char *json = nix_eval_file(source, strlen(source), &err); \
    if (!json) { \
        printf("FAIL: %s - error: %s\n", name, err ? err : "null"); \
    } else if (strstr(json, expected_substr)) { \
        printf("PASS: %s\n", name); \
        pass_count++; \
    } else { \
        printf("FAIL: %s - expected '%s' in:\n  %s\n", name, expected_substr, json); \
    } \
    free(json); \
} while(0)

int main(void)
{
    printf("=== Nix evaluator edge cases ===\n\n");

    /* Empty list */
    TEST("empty list",
         "[]",
         "[]");

    /* Empty attrset */
    TEST("empty attrset",
         "{}",
         "{}");

    /* Nested let - inner let sees outer's bindings */
    TEST("nested let",
         "let x = 1; in let y = 2; in x + y",
         "3");

    /* Curried lambda applied two at once (already in main suite, but
     * re-tested here for the edge-case suite's independence). */
    TEST("curried lambda apply",
         "(x: y: x + y) 3 4",
         "7");

    /* inherit (attrs) key1 key2; - both keys pulled into the new scope */
    TEST("inherit from attrset",
         "let s = { key1 = 11; key2 = 22; }; in { inherit (s) key1 key2; }",
         "\"key1\": 11");

    TEST("inherit from attrset key2",
         "let s = { key1 = 11; key2 = 22; }; in { inherit (s) key1 key2; }",
         "\"key2\": 22");

    /* with shadowing: inner with overrides outer with for the same key. */
    TEST("with shadowing",
         "with { x = 1; }; with { x = 99; }; x",
         "99");

    /* with provides a fallback for names not bound by let. The let
     * binding y=7 is visible; with provides x=99 only because y is
     * NOT in the with's scope. */
    TEST("with fallback for unbound name",
         "let y = 7; in with { x = 99; }; x + y",
         "106");

    /* builtins.map (x: x * 2) [1 2 3] -> [2 4 6] */
    TEST("builtins.map doubles",
         "builtins.map (x: x * 2) [ 1 2 3 ]",
         "2");

    TEST("builtins.map all three",
         "builtins.map (x: x * 2) [ 1 2 3 ]",
         "6");

    /* String interpolation with builtins.toString */
    TEST("string interpolation toString",
         "\"${builtins.toString 42}\"",
         "42");

    /* builtins.toString on an int */
    TEST("builtins.toString on int",
         "builtins.toString 255",
         "255");

    /* Empty list has length 0 */
    TEST("length of empty list",
         "builtins.length []",
         "0");

    /* Empty attrset has 0 attr names */
    TEST("attrNames of empty",
         "builtins.length (builtins.attrNames {})",
         "0");

    /* Nested let shadowing - inner y shadows outer y */
    TEST("nested let shadowing",
         "let y = 100; in let y = 5; in y",
         "5");

    /* List of strings (2O9.nix packages pattern) */
    TEST("list of strings",
         "[ \"alpha\" \"beta\" \"gamma\" ]",
         "alpha");

    /* Select missing attr returns NULL json with an error message.
     * The TEST macro prints "FAIL" on NULL json, but we accept that
     * here - the goal is to confirm the evaluator doesn't crash on
     * the missing-attr path. Verified by the test completing. */
    test_count++;
    {
        char *err = NULL;
        char *json = nix_eval_file("{}.missing", strlen("{}.missing"), &err);
        if (json == NULL && err && strstr(err, "not found")) {
            printf("PASS: select missing attr returns clear error\n");
            pass_count++;
        } else if (json) {
            printf("FAIL: select missing attr - expected error, got json: %s\n", json);
            free(json);
        } else {
            printf("FAIL: select missing attr - unexpected error format: %s\n",
                   err ? err : "null");
        }
    }

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return pass_count == test_count ? 0 : 1;
}
