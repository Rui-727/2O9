/* test_nix_eval.c — Test the Nix evaluator end-to-end */

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
        printf("FAIL: %s — error: %s\n", name, err ? err : "null"); \
    } else if (strstr(json, expected_substr)) { \
        printf("PASS: %s\n", name); \
        pass_count++; \
    } else { \
        printf("FAIL: %s — expected '%s' in:\n  %s\n", name, expected_substr, json); \
    } \
    free(json); \
    /* Don't free err — values are shared and may have been freed already */ \
} while(0)

int main(void)
{
    printf("=== Nix Evaluator Tests ===\n\n");

    /* Simple attrset */
    TEST("simple attrset",
         "{ x = 1; y = \"hello\"; }",
         "\"x\": 1");

    /* Nested attrset */
    TEST("nested attrset",
         "{ a = { b = 2; }; }",
         "\"b\": 2");

    /* List */
    TEST("list",
         "[ 1 2 3 ]",
         "1");

    /* String list */
    TEST("string list",
         "[ \"firefox\" \"neovim\" ]",
         "firefox");

    /* Let expression */
    TEST("let",
         "let x = 42; in x",
         "42");

    /* Let with string */
    TEST("let string",
         "let name = \"world\"; in \"hello ${name}\"",
         "hello world");

    /* If/then/else — true branch */
    TEST("if true",
         "if true then \"yes\" else \"no\"",
         "yes");

    /* If/then/else — false branch */
    TEST("if false",
         "if false then \"yes\" else \"no\"",
         "no");

    /* Boolean ops */
    TEST("and true",
         "if (true && true) then \"yes\" else \"no\"",
         "yes");

    TEST("and false",
         "if (true && false) then \"yes\" else \"no\"",
         "no");

    TEST("or true",
         "if (false || true) then \"yes\" else \"no\"",
         "yes");

    /* Comparison */
    TEST("eq true",
         "if (1 == 1) then \"yes\" else \"no\"",
         "yes");

    TEST("neq true",
         "if (1 != 2) then \"yes\" else \"no\"",
         "yes");

    /* Arithmetic */
    TEST("addition",
         "1 + 2",
         "3");

    TEST("subtraction",
         "10 - 3",
         "7");

    TEST("multiplication",
         "4 * 5",
         "20");

    TEST("division",
         "20 / 4",
         "5");

    /* List concat */
    TEST("list concat",
         "[ 1 2 ] ++ [ 3 4 ]",
         "4");

    /* String concat */
    TEST("string concat",
         "\"hello\" + \" \" + \"world\"",
         "hello world");

    /* Select */
    TEST("select",
         "{ a = { b = 99; }; }.a.b",
         "99");

    /* With */
    TEST("with",
         "with { x = 42; }; x",
         "42");

    /* Negate */
    TEST("negate",
         "-5",
         "-5");

    /* Not */
    TEST("not",
         "!false",
         "true");

    /* Lambda: simple */
    TEST("lambda simple",
         "(x: x + 1) 5",
         "6");

    /* Lambda: formal params */
    TEST("lambda formals",
         "({ a, b }: a + b) { a = 10; b = 20; }",
         "30");

    /* Recursive attrset */
    TEST("rec attrset",
         "rec { x = 1; y = x + 1; }.y",
         "2");

    /* Null */
    TEST("null value",
         "null",
         "null");

    /* Bool */
    TEST("bool true",
         "true",
         "true");

    /* Builtin: length */
    TEST("length",
         "builtins.length [ 1 2 3 ]",
         "3");

    /* Builtin: attrNames */
    TEST("attrNames",
         "builtins.attrNames { z = 1; a = 2; }",
         "z");

    /* Builtin: map */
    TEST("map",
         "map (x: x * 2) [ 1 2 3 ]",
         "6");

    /* Builtin: filter */
    TEST("filter",
         "builtins.filter (x: x > 2) [ 1 2 3 4 ]",
         "3");

    /* Builtin: head */
    TEST("head",
         "builtins.head [ 10 20 30 ]",
         "10");

    /* Builtin: tail */
    TEST("tail",
         "builtins.length (builtins.tail [ 10 20 30 ])",
         "2");

    /* Function form with config (2O9.nix pattern) */
    TEST("2O9.nix pattern",
         "{ config, ... }:\n"
         "{\n"
         "  packages = [ \"firefox\" ];\n"
         "  services = { sshd = { enable = true; }; };\n"
         "}",
         "firefox");

    /* Self-reference via config */
    TEST("config self-ref",
         "{ config, ... }:\n"
         "{\n"
         "  services.sshd.enable = true;\n"
         "  packages = if config.services.sshd.enable then [ \"openssh\" ] else [];\n"
         "}",
         "openssh");

    /* Implication */
    TEST("implication true->true",
         "if (true -> true) then \"yes\" else \"no\"",
         "yes");

    TEST("implication true->false",
         "if (true -> false) then \"yes\" else \"no\"",
         "no");

    TEST("implication false->anything",
         "if (false -> false) then \"yes\" else \"no\"",
         "yes");

    /* Curried lambdas — used to segfault because call_env was freed before
     * the returned lambda's closure_env was dereferenced. */
    TEST("curried lambda",
         "(x: y: x + y) 3 4",
         "7");

    TEST("curried lambda with strings",
         "(a: b: a + b) \"hello \" \"world\"",
         "hello world");

    TEST("three-deep curry",
         "(x: y: z: x + y + z) 1 2 3",
         "6");

    /* Binop precedence — * binds tighter than + */
    TEST("mul before add",
         "1 + 2 * 3",
         "7");

    TEST("add right-assoc via parens",
         "(1 + 2) * 3",
         "9");

    /* Lambda with formal parameters (commas) — was a known gap, now works */
    TEST("formal lambda",
         "({ a, b }: a + b) { a = 3; b = 4; }",
         "7");

    TEST("formal lambda with default",
         "({ a, b ? 10 }: a + b) { a = 5; }",
         "15");

    /* inherit (src) ident; — pulls attributes from a source expression */
    TEST("inherit from source",
         "let s = { x = 1; y = 2; }; in { inherit (s) x y; }",
         "\"x\": 1");

    TEST("inherit plain ident",
         "let x = 42; in { inherit x; }",
         "\"x\": 42");

    TEST("inherit multiple from attrset",
         "let pkg = { name = \"firefox\"; version = \"120\"; }; in { inherit (pkg) name version; }",
         "firefox");

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return pass_count == test_count ? 0 : 1;
}
