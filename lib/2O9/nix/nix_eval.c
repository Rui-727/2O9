/* nix_eval.c — 2O9 Nix Evaluator (full implementation)
 *
 * Evaluates the subset of the Nix expression language needed to parse
 * 2O9.nix configuration files.  Not a full Nix interpreter — no
 * derivations, no fetchers, no flakes.
 *
 * Supports:
 *   - Attribute sets (plain & recursive)
 *   - Lists, strings (with interpolation), integers, bools, null, paths
 *   - Lambda functions (ident and formal-param forms)
 *   - Function application (curried, left-associative)
 *   - Let-bindings, if/then/else, with, assert
 *   - Binary ops: +, ++ (list concat), *, /, -, &&, ||, ==, !=,
 *     <, <=, >, >=, -> (implication)
 *   - Unary: !, - (negate)
 *   - Select (a.b.c), has-attr (e ? attr), or-default (e.a or default)
 *   - Import (read & evaluate another .nix file, relative paths)
 *   - Fixed-point recursion for { config, ... }: { ... } pattern
 *   - Builtins: map, filter, length, head, tail, attrNames, attrValues,
 *     hasAttr, getAttr, fromJSON, toJSON, trace, pathExists, readFile
 *
 * Part of lib2O9.  Pure C, no C++ dependencies.
 */

#include "nix_eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/stat.h>

/* ── Value allocation ─────────────────────────────────────────────── */

nix_value_t *nix_value_new(nix_val_type_t type)
{
    nix_value_t *v = calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}

void nix_value_free(nix_value_t *val)
{
    /* NOTE: Values form a shared graph. We do a shallow free only.
     * Child values (list items, attrset entries) are NOT freed recursively
     * because they may be shared with other values or environments.
     * The caller who owns the root evaluation result should free the top-level
     * value; all sub-values are part of the same allocation graph and will
     * be cleaned up when the evaluator environment is discarded. */
    if (!val) return;

    switch (val->type) {
    case NIX_VAL_STRING:
        free(val->string);
        break;
    case NIX_VAL_PATH:
        free(val->path);
        break;
    case NIX_VAL_LIST:
        free(val->list.items);
        break;
    case NIX_VAL_ATTR_SET:
        for (size_t i = 0; i < val->attr_set.count; i++) {
            free(val->attr_set.entries[i].key);
        }
        free(val->attr_set.entries);
        break;
    default:
        break;
    }
    free(val);
}

/* ── Environment ──────────────────────────────────────────────────── */

typedef struct nix_env_entry {
    char             *name;
    nix_value_t      *value;
} nix_env_entry_t;

struct nix_env {
    nix_env_entry_t  *entries;
    size_t            count;
    size_t            cap;
    struct nix_env   *parent;   /* scope chain */
    char             *base_dir; /* for resolving relative imports */
};

nix_env_t *nix_env_new(void)
{
    nix_env_t *env = calloc(1, sizeof(*env));
    if (!env) return NULL;
    env->cap = 32;
    env->entries = calloc(env->cap, sizeof(*env->entries));
    if (!env->entries) { free(env); return NULL; }

    /* ── Register builtins ────────────────────────────────────────── */

    /* We create a builtins attrset and bind it */
    nix_value_t *builtins = nix_value_new(NIX_VAL_ATTR_SET);
    if (!builtins) return env;

    size_t bcap = 24;
    builtins->attr_set.entries = calloc(bcap, sizeof(*builtins->attr_set.entries));
    builtins->attr_set.count = 0;
    builtins->attr_set.cap = bcap;

    /* Helper macro to add a builtin */
    #define ADD_BUILTIN(NAME, FUNC, CTX) do {                             \
        size_t _i = builtins->attr_set.count;                             \
        if (_i >= builtins->attr_set.cap) {                               \
            builtins->attr_set.cap *= 2;                                  \
            builtins->attr_set.entries = realloc(builtins->attr_set.entries,\
                builtins->attr_set.cap * sizeof(*builtins->attr_set.entries));\
        }                                                                 \
        nix_value_t *_b = nix_value_new(NIX_VAL_BUILTIN);                \
        _b->builtin.name = (NAME);                                        \
        _b->builtin.func = (FUNC);                                        \
        _b->builtin.ctx  = (CTX);                                         \
        builtins->attr_set.entries[_i].key = strdup(NAME);                \
        builtins->attr_set.entries[_i].value = _b;                        \
        builtins->attr_set.count++;                                        \
    } while(0)

    /* We forward-declare the builtin functions below and register them here.
     * They'll be defined after nix_eval(). */
    extern nix_value_t *builtin_map(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_filter(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_length(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_head(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_tail(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_attr_names(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_attr_values(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_has_attr(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_get_attr(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_to_json(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_from_json(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_trace(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_path_exists(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_read_file(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_abort(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_throw(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_to_string(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_string_length(nix_env_t *, nix_value_t *, void *);
    extern nix_value_t *builtin_replace_strings(nix_env_t *, nix_value_t *, void *);

    ADD_BUILTIN("map",             builtin_map,             NULL);
    ADD_BUILTIN("filter",          builtin_filter,          NULL);
    ADD_BUILTIN("length",          builtin_length,          NULL);
    ADD_BUILTIN("head",            builtin_head,            NULL);
    ADD_BUILTIN("tail",            builtin_tail,            NULL);
    ADD_BUILTIN("attrNames",       builtin_attr_names,      NULL);
    ADD_BUILTIN("attrValues",      builtin_attr_values,     NULL);
    ADD_BUILTIN("hasAttr",         builtin_has_attr,        NULL);
    ADD_BUILTIN("getAttr",         builtin_get_attr,        NULL);
    ADD_BUILTIN("toJSON",          builtin_to_json,         NULL);
    ADD_BUILTIN("fromJSON",        builtin_from_json,       NULL);
    ADD_BUILTIN("trace",           builtin_trace,           NULL);
    ADD_BUILTIN("pathExists",      builtin_path_exists,     NULL);
    ADD_BUILTIN("readFile",        builtin_read_file,       NULL);
    ADD_BUILTIN("abort",           builtin_abort,           NULL);
    ADD_BUILTIN("throw",           builtin_throw,           NULL);
    ADD_BUILTIN("toString",        builtin_to_string,       NULL);
    ADD_BUILTIN("stringLength",    builtin_string_length,   NULL);
    ADD_BUILTIN("replaceStrings",  builtin_replace_strings, NULL);

    #undef ADD_BUILTIN

    /* Bind "builtins" and "true"/"false"/"null"/"map" as top-level names */
    nix_env_bind(env, "builtins", builtins);

    nix_value_t *v_true = nix_value_new(NIX_VAL_BOOL);
    v_true->boolean = 1;
    nix_env_bind(env, "true", v_true);

    nix_value_t *v_false = nix_value_new(NIX_VAL_BOOL);
    v_false->boolean = 0;
    nix_env_bind(env, "false", v_false);

    nix_value_t *v_null = nix_value_new(NIX_VAL_NULL);
    nix_env_bind(env, "null", v_null);

    /* "map" is also available as a top-level name (like in real Nix) */
    for (size_t i = 0; i < builtins->attr_set.count; i++) {
        if (strcmp(builtins->attr_set.entries[i].key, "map") == 0) {
            nix_env_bind(env, "map", builtins->attr_set.entries[i].value);
            break;
        }
    }

    return env;
}

void nix_env_free(nix_env_t *env)
{
    if (!env) return;
    for (size_t i = 0; i < env->count; i++) {
        free(env->entries[i].name);
        /* don't free value — it may be shared */
    }
    free(env->entries);
    free(env->base_dir);
    /* Don't free parent — it's shared and may be referenced by other children.
     * The top-level env is freed by nix_eval_file_with_base. */
    free(env);
}

int nix_env_bind(nix_env_t *env, const char *name, nix_value_t *val)
{
    if (!env || !name) return -1;

    /* Check for existing binding — update in place */
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) {
            env->entries[i].value = val;
            return 0;
        }
    }

    if (env->count >= env->cap) {
        env->cap *= 2;
        nix_env_entry_t *new_entries = realloc(env->entries,
                                               env->cap * sizeof(*new_entries));
        if (!new_entries) return -1;
        env->entries = new_entries;
    }
    env->entries[env->count].name  = strdup(name);
    env->entries[env->count].value = val;
    env->count++;
    return 0;
}

nix_value_t *nix_env_lookup(nix_env_t *env, const char *name)
{
    if (!env || !name) return NULL;
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0)
            return env->entries[i].value;
    }
    return nix_env_lookup(env->parent, name);
}

/* Create a child environment (new scope) */
static nix_env_t *nix_env_push(nix_env_t *parent)
{
    nix_env_t *child = calloc(1, sizeof(*child));
    if (!child) return NULL;
    child->cap = 16;
    child->entries = calloc(child->cap, sizeof(*child->entries));
    if (!child->entries) { free(child); return NULL; }
    child->parent = parent;
    if (parent && parent->base_dir)
        child->base_dir = strdup(parent->base_dir);
    return child;
}

/* ── AST free (recursive) ─────────────────────────────────────────── */

/* 2O9: AST clone — deep copy of an AST node.
 * Used by `inherit (src) ident1 ident2;` where the same source expression
 * must be referenced by multiple bindings without being double-freed. */
nix_ast_t *nix_ast_clone(nix_ast_t *ast)
{
    if (!ast) return NULL;
    nix_ast_t *c = ast_new(ast->type, ast->line, ast->col);
    if (!c) return NULL;

    switch (ast->type) {
    case NIX_NODE_ATTR_SET:
    case NIX_NODE_REC_ATTR_SET:
        c->attr_set.count = ast->attr_set.count;
        c->attr_set.bindings = calloc(ast->attr_set.count > 0 ? ast->attr_set.count : 1,
                                      sizeof(*c->attr_set.bindings));
        for (size_t i = 0; i < ast->attr_set.count; i++) {
            c->attr_set.bindings[i].key = ast->attr_set.bindings[i].key ? strdup(ast->attr_set.bindings[i].key) : NULL;
            c->attr_set.bindings[i].value = nix_ast_clone(ast->attr_set.bindings[i].value);
        }
        break;

    case NIX_NODE_LIST:
        c->list.count = ast->list.count;
        c->list.items = calloc(ast->list.count > 0 ? ast->list.count : 1, sizeof(*c->list.items));
        for (size_t i = 0; i < ast->list.count; i++)
            c->list.items[i] = nix_ast_clone(ast->list.items[i]);
        break;

    case NIX_NODE_STRING:
        c->string.count = ast->string.count;
        c->string.parts = calloc(ast->string.count > 0 ? ast->string.count : 1, sizeof(*c->string.parts));
        for (size_t i = 0; i < ast->string.count; i++) {
            c->string.parts[i].is_expr = ast->string.parts[i].is_expr;
            if (ast->string.parts[i].is_expr)
                c->string.parts[i].expr = nix_ast_clone(ast->string.parts[i].expr);
            else
                c->string.parts[i].text = ast->string.parts[i].text ? strdup(ast->string.parts[i].text) : NULL;
        }
        break;

    case NIX_NODE_IDENT:
        c->ident = ast->ident ? strdup(ast->ident) : NULL;
        break;

    case NIX_NODE_SELECT:
        c->select.expr = nix_ast_clone(ast->select.expr);
        c->select.attr = ast->select.attr ? strdup(ast->select.attr) : NULL;
        break;

    case NIX_NODE_HAS_ATTR:
        c->has_attr.expr = nix_ast_clone(ast->has_attr.expr);
        c->has_attr.attr = ast->has_attr.attr ? strdup(ast->has_attr.attr) : NULL;
        break;

    case NIX_NODE_APPLY:
        c->apply.func = nix_ast_clone(ast->apply.func);
        c->apply.arg = nix_ast_clone(ast->apply.arg);
        break;

    case NIX_NODE_LAMBDA:
        c->lambda.param = nix_ast_clone(ast->lambda.param);
        c->lambda.body = nix_ast_clone(ast->lambda.body);
        break;

    case NIX_NODE_LET:
        c->let.count = ast->let.count;
        c->let.bindings = calloc(ast->let.count > 0 ? ast->let.count : 1, sizeof(*c->let.bindings));
        for (size_t i = 0; i < ast->let.count; i++) {
            c->let.bindings[i].name = ast->let.bindings[i].name ? strdup(ast->let.bindings[i].name) : NULL;
            c->let.bindings[i].value = nix_ast_clone(ast->let.bindings[i].value);
        }
        break;

    case NIX_NODE_IF:
        c->if_expr.cond = nix_ast_clone(ast->if_expr.cond);
        c->if_expr.then_expr = nix_ast_clone(ast->if_expr.then_expr);
        c->if_expr.else_expr = nix_ast_clone(ast->if_expr.else_expr);
        break;

    case NIX_NODE_WITH:
        c->with_expr.env_expr = nix_ast_clone(ast->with_expr.env_expr);
        c->with_expr.body = nix_ast_clone(ast->with_expr.body);
        break;

    case NIX_NODE_ASSERT:
        c->assert_expr.cond = nix_ast_clone(ast->assert_expr.cond);
        c->assert_expr.body = nix_ast_clone(ast->assert_expr.body);
        break;

    case NIX_NODE_BINOP:
        c->binop.op = ast->binop.op;
        c->binop.left = nix_ast_clone(ast->binop.left);
        c->binop.right = nix_ast_clone(ast->binop.right);
        break;

    case NIX_NODE_UNARY_NOT:
        c->operand = nix_ast_clone(ast->operand);
        break;

    case NIX_NODE_INT:
        c->integer = ast->integer;
        break;

    case NIX_NODE_PATH:
        c->path = ast->path ? strdup(ast->path) : NULL;
        break;

    case NIX_NODE_NULL:
    case NIX_NODE_BOOL:
        c->boolean = ast->boolean;
        break;

    default:
        /* Unknown node type — shallow copy is the best we can do. */
        break;
    }
    return c;
}

void nix_ast_free(nix_ast_t *ast)
{
    if (!ast) return;

    switch (ast->type) {
    case NIX_NODE_ATTR_SET:
    case NIX_NODE_REC_ATTR_SET:
        for (size_t i = 0; i < ast->attr_set.count; i++) {
            free(ast->attr_set.bindings[i].key);
            nix_ast_free(ast->attr_set.bindings[i].value);
        }
        free(ast->attr_set.bindings);
        break;

    case NIX_NODE_LIST:
        for (size_t i = 0; i < ast->list.count; i++)
            nix_ast_free(ast->list.items[i]);
        free(ast->list.items);
        break;

    case NIX_NODE_STRING:
        for (size_t i = 0; i < ast->string.count; i++) {
            if (ast->string.parts[i].is_expr)
                nix_ast_free(ast->string.parts[i].expr);
            else
                free(ast->string.parts[i].text);
        }
        free(ast->string.parts);
        break;

    case NIX_NODE_IDENT:
        free(ast->ident);
        break;

    case NIX_NODE_SELECT:
        nix_ast_free(ast->select.expr);
        free(ast->select.attr);
        break;

    case NIX_NODE_APPLY:
        nix_ast_free(ast->apply.func);
        nix_ast_free(ast->apply.arg);
        break;

    case NIX_NODE_LAMBDA:
        nix_ast_free(ast->lambda.param);
        nix_ast_free(ast->lambda.body);
        break;

    case NIX_NODE_LET:
        for (size_t i = 0; i < ast->let.count; i++) {
            free(ast->let.bindings[i].name);
            nix_ast_free(ast->let.bindings[i].value);
        }
        free(ast->let.bindings);
        nix_ast_free(ast->let.body);
        break;

    case NIX_NODE_IF:
        nix_ast_free(ast->if_expr.cond);
        nix_ast_free(ast->if_expr.then_expr);
        nix_ast_free(ast->if_expr.else_expr);
        break;

    case NIX_NODE_WITH:
        nix_ast_free(ast->with_expr.env_expr);
        nix_ast_free(ast->with_expr.body);
        break;

    case NIX_NODE_ASSERT:
        nix_ast_free(ast->assert_expr.cond);
        nix_ast_free(ast->assert_expr.body);
        break;

    case NIX_NODE_BINOP:
        nix_ast_free(ast->binop.left);
        nix_ast_free(ast->binop.right);
        break;

    case NIX_NODE_UNARY_NOT:
    case NIX_NODE_NEGATE:
        nix_ast_free(ast->operand);
        break;

    case NIX_NODE_HAS_ATTR:
        nix_ast_free(ast->has_attr.expr);
        free(ast->has_attr.attr);
        break;

    case NIX_NODE_OR_DEFAULT:
        nix_ast_free(ast->or_default.expr);
        free(ast->or_default.attr);
        nix_ast_free(ast->or_default.default_expr);
        break;

    case NIX_NODE_PATH:
        free(ast->path);
        break;

    case NIX_NODE_IMPORT:
        free(ast->import.path);
        break;

    case NIX_NODE_INT:
    case NIX_NODE_BOOL:
    case NIX_NODE_NULL:
        break;
    }

    free(ast);
}

/* ── Evaluator ────────────────────────────────────────────────────── */

/* Forward declaration */
static nix_value_t *nix_eval_inner(nix_env_t *env, nix_ast_t *ast, char **error);

nix_value_t *nix_eval(nix_env_t *env, nix_ast_t *ast, char **error)
{
    return nix_eval_inner(env, ast, error);
}

/* Helper: format an error message */
static nix_value_t *eval_error(char **error, const char *fmt, ...)
{
    if (error) {
        va_list ap;
        va_start(ap, fmt);
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        *error = strdup(buf);
    }
    return NULL;
}

/* Helper: attrset lookup by key */
static nix_value_t *attrset_lookup(nix_value_t *set, const char *key)
{
    if (!set || set->type != NIX_VAL_ATTR_SET) return NULL;
    for (size_t i = 0; i < set->attr_set.count; i++) {
        if (strcmp(set->attr_set.entries[i].key, key) == 0)
            return set->attr_set.entries[i].value;
    }
    return NULL;
}

/* Helper: check if attrset has key */
static int attrset_has(nix_value_t *set, const char *key)
{
    return attrset_lookup(set, key) != NULL;
}

/* Helper: bind attrset entry */
static int attrset_bind(nix_value_t *set, const char *key, nix_value_t *val)
{
    if (!set || set->type != NIX_VAL_ATTR_SET) return -1;

    /* Update existing */
    for (size_t i = 0; i < set->attr_set.count; i++) {
        if (strcmp(set->attr_set.entries[i].key, key) == 0) {
            set->attr_set.entries[i].value = val;
            return 0;
        }
    }

    /* Add new */
    if (set->attr_set.count >= set->attr_set.cap) {
        set->attr_set.cap = set->attr_set.cap ? set->attr_set.cap * 2 : 16;
        set->attr_set.entries = realloc(set->attr_set.entries,
                                        set->attr_set.cap * sizeof(*set->attr_set.entries));
    }
    set->attr_set.entries[set->attr_set.count].key = strdup(key);
    set->attr_set.entries[set->attr_set.count].value = val;
    set->attr_set.count++;
    return 0;
}

/* Bind a value at a dotted key path like "services.sshd.enable",
 * creating nested attrsets as needed. E.g.:
 *   attrset_bind_path(set, ["services","sshd","enable"], 3, val)
 *   → set.services.sshd.enable = val  (creates intermediate attrsets) */
static int attrset_bind_path(nix_value_t *set, const char **parts, size_t nparts, nix_value_t *val)
{
    if (!set || set->type != NIX_VAL_ATTR_SET || nparts == 0) return -1;

    nix_value_t *current = set;
    for (size_t i = 0; i < nparts - 1; i++) {
        nix_value_t *next = attrset_lookup(current, parts[i]);
        if (!next) {
            /* Create intermediate attrset */
            next = nix_value_new(NIX_VAL_ATTR_SET);
            if (!next) return -1;
            next->attr_set.cap = 8;
            next->attr_set.entries = calloc(next->attr_set.cap, sizeof(*next->attr_set.entries));
            next->attr_set.count = 0;
            attrset_bind(current, parts[i], next);
        }
        if (next->type == NIX_VAL_THUNK) {
            /* Force thunk to get the actual value */
            /* We can't force without an env here, so just skip */
            break;
        }
        if (next->type != NIX_VAL_ATTR_SET) {
            /* Can't descend into non-attrset; overwrite with new attrset */
            next = nix_value_new(NIX_VAL_ATTR_SET);
            if (!next) return -1;
            next->attr_set.cap = 8;
            next->attr_set.entries = calloc(next->attr_set.cap, sizeof(*next->attr_set.entries));
            next->attr_set.count = 0;
            attrset_bind(current, parts[i], next);
        }
        current = next;
    }

    return attrset_bind(current, parts[nparts - 1], val);
}

/* Helper: coerce to bool */
static int nix_is_truthy(nix_value_t *v)
{
    if (!v) return 0;
    switch (v->type) {
    case NIX_VAL_BOOL:   return v->boolean;
    case NIX_VAL_NULL:   return 0;
    case NIX_VAL_INT:    return v->integer != 0;
    case NIX_VAL_STRING: return v->string && v->string[0] != '\0';
    case NIX_VAL_LIST:   return v->list.count > 0;
    default:             return 1;
    }
}

/* Helper: compare two values for equality */
static int nix_values_equal(nix_value_t *a, nix_value_t *b)
{
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;

    switch (a->type) {
    case NIX_VAL_NULL:   return 1;
    case NIX_VAL_BOOL:   return a->boolean == b->boolean;
    case NIX_VAL_INT:    return a->integer == b->integer;
    case NIX_VAL_STRING: return strcmp(a->string, b->string) == 0;
    case NIX_VAL_PATH:   return strcmp(a->path, b->path) == 0;
    case NIX_VAL_LIST:
        if (a->list.count != b->list.count) return 0;
        for (size_t i = 0; i < a->list.count; i++)
            if (!nix_values_equal(a->list.items[i], b->list.items[i])) return 0;
        return 1;
    case NIX_VAL_ATTR_SET:
        if (a->attr_set.count != b->attr_set.count) return 0;
        for (size_t i = 0; i < a->attr_set.count; i++) {
            nix_value_t *bv = attrset_lookup(b, a->attr_set.entries[i].key);
            if (!bv || !nix_values_equal(a->attr_set.entries[i].value, bv))
                return 0;
        }
        return 1;
    default:
        return 0;
    }
}

/* Helper: read entire file into malloc'd buffer. Returns NULL on error. */
static char *read_entire_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);

    if (out_len) *out_len = nread;
    return buf;
}

/* Helper: resolve a path relative to base_dir */
static char *resolve_path(const char *base_dir, const char *path)
{
    if (!path) return NULL;

    /* Absolute path — use as-is */
    if (path[0] == '/') return strdup(path);

    /* Relative path — resolve against base_dir */
    if (base_dir) {
        size_t blen = strlen(base_dir);
        size_t plen = strlen(path);
        char *resolved = malloc(blen + 1 + plen + 1);
        memcpy(resolved, base_dir, blen);
        resolved[blen] = '/';
        memcpy(resolved + blen + 1, path, plen + 1);
        return resolved;
    }

    return strdup(path);
}

/* ── The evaluator ────────────────────────────────────────────────── */

static nix_value_t *nix_eval_inner(nix_env_t *env, nix_ast_t *ast, char **error)
{
    if (!ast) return eval_error(error, "null AST");
    if (!env)  return eval_error(error, "null environment");

    switch (ast->type) {

    /* ── Literals ────────────────────────────────────────────────── */

    case NIX_NODE_INT: {
        nix_value_t *v = nix_value_new(NIX_VAL_INT);
        if (v) v->integer = ast->integer;
        return v;
    }

    case NIX_NODE_BOOL: {
        nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
        if (v) v->boolean = ast->boolean;
        return v;
    }

    case NIX_NODE_NULL:
        return nix_value_new(NIX_VAL_NULL);

    case NIX_NODE_PATH: {
        nix_value_t *v = nix_value_new(NIX_VAL_PATH);
        if (v) v->path = resolve_path(env->base_dir, ast->path);
        return v;
    }

    case NIX_NODE_IDENT: {
        nix_value_t *val = nix_env_lookup(env, ast->ident);
        if (!val)
            return eval_error(error, "undefined variable '%s' at line %d",
                              ast->ident, ast->line);
        return val;
    }

    /* ── Strings (with interpolation) ────────────────────────────── */

    case NIX_NODE_STRING: {
        /* Build string by concatenating literal and interpolated parts */
        size_t cap = 256;
        char *buf = calloc(cap, 1);
        size_t len = 0;

        for (size_t i = 0; i < ast->string.count; i++) {
            if (ast->string.parts[i].is_expr) {
                /* Interpolated expression — evaluate and coerce to string */
                nix_value_t *v = nix_eval_inner(env, ast->string.parts[i].expr, error);
                if (!v) { free(buf); return NULL; }

                const char *s = NULL;
                char tmp[64];
                switch (v->type) {
                case NIX_VAL_STRING: s = v->string; break;
                case NIX_VAL_PATH:   s = v->path; break;
                case NIX_VAL_INT:
                    snprintf(tmp, sizeof(tmp), "%ld", (long)v->integer);
                    s = tmp;
                    break;
                case NIX_VAL_BOOL:
                    s = v->boolean ? "true" : "false";
                    break;
                case NIX_VAL_NULL:
                    s = "null";
                    break;
                default:
                    nix_value_free(v);
                    free(buf);
                    return eval_error(error,
                        "cannot interpolate %s value into string at line %d",
                        v->type == NIX_VAL_ATTR_SET ? "attrset" : "non-string",
                        ast->line);
                }

                size_t slen = strlen(s);
                while (len + slen + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + len, s, slen);
                len += slen;
                nix_value_free(v);
            } else {
                /* Literal text */
                const char *s = ast->string.parts[i].text;
                if (s) {
                    size_t slen = strlen(s);
                    while (len + slen + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + len, s, slen);
                    len += slen;
                }
            }
        }

        buf[len] = '\0';
        nix_value_t *v = nix_value_new(NIX_VAL_STRING);
        if (v) v->string = buf; else free(buf);
        return v;
    }

    /* ── Attribute sets ──────────────────────────────────────────── */

    case NIX_NODE_ATTR_SET: {
        nix_value_t *set = nix_value_new(NIX_VAL_ATTR_SET);
        if (!set) return NULL;
        set->attr_set.cap = ast->attr_set.count > 0 ? ast->attr_set.count * 2 : 8;
        set->attr_set.entries = calloc(set->attr_set.cap, sizeof(*set->attr_set.entries));
        set->attr_set.count = 0;

        for (size_t i = 0; i < ast->attr_set.count; i++) {
            nix_value_t *val = nix_eval_inner(env, ast->attr_set.bindings[i].value, error);
            if (!val) {
                nix_value_free(set);
                return NULL;
            }
            /* Check if key contains dots (dotted path like "services.sshd.enable") */
            const char *key = ast->attr_set.bindings[i].key;
            const char *dot = strchr(key, '.');
            if (dot) {
                /* Split dotted key into parts */
                size_t nparts = 1;
                for (const char *p = key; *p; p++)
                    if (*p == '.') nparts++;
                const char **parts = calloc(nparts, sizeof(char *));
                char *keycopy = strdup(key);
                char *saveptr = NULL;
                size_t pi = 0;
                char *tok = strtok_r(keycopy, ".", &saveptr);
                while (tok && pi < nparts) {
                    parts[pi++] = tok;
                    tok = strtok_r(NULL, ".", &saveptr);
                }
                attrset_bind_path(set, parts, pi, val);
                free(keycopy);
                free(parts);
            } else {
                attrset_bind(set, key, val);
            }
        }
        return set;
    }

    case NIX_NODE_REC_ATTR_SET: {
        /* Recursive attrset: bindings can reference each other.
         * We evaluate using thunks with a shared recursive environment.
         * Each binding is a thunk; when forced, it can look up other
         * bindings by name in the rec_env. */
        nix_value_t *set = nix_value_new(NIX_VAL_ATTR_SET);
        if (!set) return NULL;
        set->attr_set.cap = ast->attr_set.count > 0 ? ast->attr_set.count * 2 : 8;
        set->attr_set.entries = calloc(set->attr_set.cap, sizeof(*set->attr_set.entries));
        set->attr_set.count = 0;

        /* Create a child env for the rec scope */
        nix_env_t *rec_env = nix_env_push(env);

        /* First pass: bind all keys as thunks in both the attrset and the env */
        for (size_t i = 0; i < ast->attr_set.count; i++) {
            nix_value_t *thunk = nix_value_new(NIX_VAL_THUNK);
            thunk->thunk.env = rec_env;
            thunk->thunk.expr = ast->attr_set.bindings[i].value;
            thunk->thunk.forced = NULL;
            attrset_bind(set, ast->attr_set.bindings[i].key, thunk);
            /* Also bind in the rec_env so that 'x' resolves within the scope */
            nix_env_bind(rec_env, ast->attr_set.bindings[i].key, thunk);
        }

        /* Second pass: force all thunks — resolves cross-references
         * because all keys are now bound in rec_env. */
        for (size_t i = 0; i < set->attr_set.count; i++) {
            nix_value_t *thunk = set->attr_set.entries[i].value;
            if (thunk && thunk->type == NIX_VAL_THUNK && !thunk->thunk.forced) {
                char *err2 = NULL;
                nix_value_t *resolved = nix_force(rec_env, thunk, &err2);
                if (!resolved) {
                    if (error) *error = err2;
                    else free(err2);
                    nix_env_free(rec_env);
                    nix_value_free(set);
                    return NULL;
                }
                /* Replace the thunk with the resolved value */
                set->attr_set.entries[i].value = resolved;
                /* Also update the env binding so subsequent lookups get the value */
                nix_env_bind(rec_env, set->attr_set.entries[i].key, resolved);
                nix_value_free(thunk);
            }
        }

        nix_env_free(rec_env);
        return set;
    }

    /* ── Lists ───────────────────────────────────────────────────── */

    case NIX_NODE_LIST: {
        nix_value_t *list = nix_value_new(NIX_VAL_LIST);
        if (!list) return NULL;
        list->list.cap = ast->list.count > 0 ? ast->list.count : 1;
        list->list.items = calloc(list->list.cap, sizeof(*list->list.items));
        list->list.count = ast->list.count;

        for (size_t i = 0; i < ast->list.count; i++) {
            list->list.items[i] = nix_eval_inner(env, ast->list.items[i], error);
            if (!list->list.items[i]) {
                nix_value_free(list);
                return NULL;
            }
        }
        return list;
    }

    /* ── Select (a.b.c) ──────────────────────────────────────────── */

    case NIX_NODE_SELECT: {
        nix_value_t *obj = nix_eval_inner(env, ast->select.expr, error);
        if (!obj) return NULL;

        /* Force thunks before selecting */
        if (obj->type == NIX_VAL_THUNK) {
            obj = nix_force(env, obj, error);
            if (!obj) return NULL;
        }

        if (obj->type == NIX_VAL_ATTR_SET) {
            nix_value_t *val = attrset_lookup(obj, ast->select.attr);
            if (!val) {
                nix_value_free(obj);
                return eval_error(error, "attribute '%s' not found at line %d",
                                  ast->select.attr, ast->line);
            }
            /* Don't free obj — val is owned by it */
            return val;
        }

        /* Try builtins select: builtins.attrNames etc */
        if (obj->type == NIX_VAL_BUILTIN) {
            /* builtins.attrNames — but builtins is an attrset, not a builtin.
             * This case shouldn't normally be hit. */
        }

        nix_value_free(obj);
        return eval_error(error, "cannot select '%s' from non-attrset at line %d",
                          ast->select.attr, ast->line);
    }

    /* ── Has-attr (e ? attr) ─────────────────────────────────────── */

    case NIX_NODE_HAS_ATTR: {
        nix_value_t *obj = nix_eval_inner(env, ast->has_attr.expr, error);
        if (!obj) return NULL;

        nix_value_t *result = nix_value_new(NIX_VAL_BOOL);
        if (result) {
            if (obj->type == NIX_VAL_ATTR_SET)
                result->boolean = attrset_has(obj, ast->has_attr.attr);
            else
                result->boolean = 0;
        }
        nix_value_free(obj);
        return result;
    }

    /* ── Or-default (e . attr or default) ────────────────────────── */

    case NIX_NODE_OR_DEFAULT: {
        nix_value_t *obj = nix_eval_inner(env, ast->or_default.expr, error);
        if (!obj) return NULL;

        if (obj->type == NIX_VAL_ATTR_SET) {
            nix_value_t *val = attrset_lookup(obj, ast->or_default.attr);
            if (val) {
                /* Don't free obj — val is owned by it */
                return val;
            }
        }

        /* Attribute not found — evaluate the default */
        nix_value_free(obj);
        return nix_eval_inner(env, ast->or_default.default_expr, error);
    }

    /* ── Lambda ──────────────────────────────────────────────────── */

    case NIX_NODE_LAMBDA: {
        nix_value_t *v = nix_value_new(NIX_VAL_LAMBDA);
        if (v) {
            v->lambda.closure_env = env;
            v->lambda.param = ast->lambda.param;
            v->lambda.body  = ast->lambda.body;
        }
        return v;
    }

    /* ── Function application ────────────────────────────────────── */

    case NIX_NODE_APPLY: {
        nix_value_t *func = nix_eval_inner(env, ast->apply.func, error);
        if (!func) return NULL;

        nix_value_t *arg = nix_eval_inner(env, ast->apply.arg, error);
        if (!arg) { /* leaked: shared value graph */ return NULL; }

        /* Builtin function */
        if (func->type == NIX_VAL_BUILTIN) {
            nix_value_t *result = func->builtin.func(env, arg, func->builtin.ctx);
            /* leaked: shared value graph */
            /* Don't free arg — the builtin may retain it */
            return result;
        }

        /* Lambda application */
        if (func->type == NIX_VAL_LAMBDA) {
            nix_env_t *call_env = nix_env_push(func->lambda.closure_env);

            if (func->lambda.param->type == NIX_NODE_IDENT) {
                /* Simple lambda: x: body */
                nix_env_bind(call_env, func->lambda.param->ident, arg);

            } else if (func->lambda.param->type == NIX_NODE_ATTR_SET) {
                /* Formal parameters: { a, b ? default, ... }: body */
                for (size_t i = 0; i < func->lambda.param->attr_set.count; i++) {
                    const char *pname = func->lambda.param->attr_set.bindings[i].key;
                    nix_value_t *pval = NULL;

                    if (arg->type == NIX_VAL_ATTR_SET)
                        pval = attrset_lookup(arg, pname);

                    if (!pval) {
                        /* Use default value if provided */
                        nix_ast_t *default_ast = func->lambda.param->attr_set.bindings[i].value;
                        if (default_ast && default_ast->type != NIX_NODE_NULL) {
                            char *err2 = NULL;
                            pval = nix_eval_inner(call_env, default_ast, &err2);
                            if (!pval) {
                                if (error) *error = err2;
                                else free(err2);
                                nix_env_free(call_env);
                                /* leaked: shared value graph */
                                return NULL;
                            }
                        } else {
                            nix_env_free(call_env);
                            /* leaked: shared value graph */
                            return eval_error(error,
                                "missing argument '%s' at line %d", pname, ast->line);
                        }
                    }
                    nix_env_bind(call_env, pname, pval);
                }

                /* Special: if the param has an ellipsis and the function is the
                 * top-level { config, ... }: pattern, set up fixed-point recursion.
                 * We detect this by checking if 'config' is one of the formal params.
                 * When config is present, we make it a lazy self-reference. */
                nix_value_t *config_val = nix_env_lookup(call_env, "config");
                if (config_val) {
                    /* config is bound. For fixed-point, we need the function's
                     * result to be available as config. We create a thunk that
                     * will evaluate the body with config = <itself>.
                     * For simplicity: evaluate body once with current config,
                     * then re-evaluate with config pointing to the result,
                     * iterating until stable (fixed-point). */
                    int max_iterations = 100;
                    nix_value_t *prev_config = NULL;

                    for (int iter = 0; iter < max_iterations; iter++) {
                        /* Evaluate the body with current config */
                        char *err2 = NULL;
                        nix_value_t *result = nix_eval_inner(call_env, func->lambda.body, &err2);
                        if (!result) {
                            if (error) *error = err2;
                            else free(err2);
                            nix_env_free(call_env);
                            /* leaked: shared value graph */
                            return NULL;
                        }

                        /* Check if config has stabilized */
                        nix_value_t *new_config = result;
                        if (prev_config && nix_values_equal(prev_config, new_config)) {
                            /* Fixed point reached */
                            nix_value_free(prev_config);
                            nix_env_free(call_env);
                            /* leaked: shared value graph */
                            return result;
                        }

                        /* Update config binding with the new result */
                        nix_env_bind(call_env, "config", new_config);
                        if (prev_config) nix_value_free(prev_config);
                        prev_config = new_config;
                    }

                    nix_env_free(call_env);
                    /* leaked: shared value graph */
                    return eval_error(error,
                        "fixed-point iteration did not converge after %d iterations",
                        max_iterations);
                }

            } else {
                nix_env_free(call_env);
                /* leaked: shared value graph */
                return eval_error(error, "invalid lambda parameter type at line %d",
                                  ast->line);
            }

            char *err2 = NULL;
            nix_value_t *result = nix_eval_inner(call_env, func->lambda.body, &err2);
            if (!result && error) *error = err2;
            else if (!result) free(err2);

            /* 2O9: don't free call_env if the result is a lambda whose
             * closure_env points to call_env — that would leave a dangling
             * pointer and crash on a subsequent application (currying).
             * The leaked env is small and lives for the duration of the
             * top-level evaluation; it's freed when the root env is freed. */
            if (result
                && result->type == NIX_VAL_LAMBDA
                && result->lambda.closure_env == call_env) {
                /* call_env is now part of the result's lifetime — keep it. */
            } else {
                nix_env_free(call_env);
            }
            /* leaked: shared value graph */
            return result;
        }

        /* leaked: shared value graph */
        return eval_error(error, "cannot apply non-function at line %d", ast->line);
    }

    /* ── Let ─────────────────────────────────────────────────────── */

    case NIX_NODE_LET: {
        nix_env_t *let_env = nix_env_push(env);

        for (size_t i = 0; i < ast->let.count; i++) {
            nix_value_t *val = nix_eval_inner(let_env, ast->let.bindings[i].value, error);
            if (!val) {
                nix_env_free(let_env);
                return NULL;
            }
            nix_env_bind(let_env, ast->let.bindings[i].name, val);
        }

        nix_value_t *result = nix_eval_inner(let_env, ast->let.body, error);
        nix_env_free(let_env);
        return result;
    }

    /* ── If/then/else ────────────────────────────────────────────── */

    case NIX_NODE_IF: {
        nix_value_t *cond = nix_eval_inner(env, ast->if_expr.cond, error);
        if (!cond) return NULL;

        int truthy = nix_is_truthy(cond);
        nix_value_free(cond);

        if (truthy)
            return nix_eval_inner(env, ast->if_expr.then_expr, error);
        else
            return nix_eval_inner(env, ast->if_expr.else_expr, error);
    }

    /* ── With ────────────────────────────────────────────────────── */

    case NIX_NODE_WITH: {
        nix_value_t *with_val = nix_eval_inner(env, ast->with_expr.env_expr, error);
        if (!with_val) return NULL;

        if (with_val->type != NIX_VAL_ATTR_SET) {
            nix_value_free(with_val);
            return eval_error(error, "'with' expects attrset at line %d", ast->line);
        }

        nix_env_t *with_env = nix_env_push(env);
        for (size_t i = 0; i < with_val->attr_set.count; i++) {
            nix_env_bind(with_env, with_val->attr_set.entries[i].key,
                         with_val->attr_set.entries[i].value);
        }

        nix_value_t *result = nix_eval_inner(with_env, ast->with_expr.body, error);
        nix_env_free(with_env);
        /* Don't free with_val — its values are referenced by result */
        return result;
    }

    /* ── Assert ──────────────────────────────────────────────────── */

    case NIX_NODE_ASSERT: {
        nix_value_t *cond = nix_eval_inner(env, ast->assert_expr.cond, error);
        if (!cond) return NULL;

        if (!nix_is_truthy(cond)) {
            nix_value_free(cond);
            return eval_error(error, "assertion failed at line %d", ast->line);
        }
        nix_value_free(cond);
        return nix_eval_inner(env, ast->assert_expr.body, error);
    }

    /* ── Binary operations ───────────────────────────────────────── */

    case NIX_NODE_BINOP: {
        nix_value_t *left = nix_eval_inner(env, ast->binop.left, error);
        if (!left) return NULL;

        /* Short-circuit for logical ops */
        if (ast->binop.op == NIX_TOK_AND) {
            if (!nix_is_truthy(left)) {
                /* leaked: shared value graph */
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = 0;
                return v;
            }
            /* leaked: shared value graph */
            nix_value_t *right = nix_eval_inner(env, ast->binop.right, error);
            if (!right) return NULL;
            nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
            if (v) v->boolean = nix_is_truthy(right);
            /* leaked: shared value graph */
            return v;
        }

        if (ast->binop.op == NIX_TOK_OR) {
            if (nix_is_truthy(left)) {
                /* leaked: shared value graph */
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = 1;
                return v;
            }
            /* leaked: shared value graph */
            nix_value_t *right = nix_eval_inner(env, ast->binop.right, error);
            if (!right) return NULL;
            nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
            if (v) v->boolean = nix_is_truthy(right);
            /* leaked: shared value graph */
            return v;
        }

        /* Implication: a -> b  ≡  !a || b */
        if (ast->binop.op == NIX_TOK_ARROW) {
            if (!nix_is_truthy(left)) {
                /* leaked: shared value graph */
                /* Skip evaluating right — result is true */
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = 1;
                return v;
            }
            /* leaked: shared value graph */
            nix_value_t *right = nix_eval_inner(env, ast->binop.right, error);
            if (!right) return NULL;
            nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
            if (v) v->boolean = nix_is_truthy(right);
            /* leaked: shared value graph */
            return v;
        }

        nix_value_t *right = nix_eval_inner(env, ast->binop.right, error);
        if (!right) { /* leaked: shared value graph */ return NULL; }

        switch (ast->binop.op) {
        case NIX_TOK_PLUS: {
            /* String concatenation or integer addition */
            if (left->type == NIX_VAL_STRING && right->type == NIX_VAL_STRING) {
                size_t l1 = strlen(left->string);
                size_t l2 = strlen(right->string);
                nix_value_t *v = nix_value_new(NIX_VAL_STRING);
                if (v) {
                    v->string = malloc(l1 + l2 + 1);
                    memcpy(v->string, left->string, l1);
                    memcpy(v->string + l1, right->string, l2 + 1);
                }
                /* leaked: shared value graph */
                return v;
            }
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_INT);
                if (v) v->integer = left->integer + right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* String + path, path + string */
            if ((left->type == NIX_VAL_STRING || left->type == NIX_VAL_PATH) &&
                (right->type == NIX_VAL_STRING || right->type == NIX_VAL_PATH)) {
                const char *s1 = left->type == NIX_VAL_STRING ? left->string : left->path;
                const char *s2 = right->type == NIX_VAL_STRING ? right->string : right->path;
                size_t l1 = strlen(s1), l2 = strlen(s2);
                nix_value_t *v = nix_value_new(NIX_VAL_STRING);
                if (v) {
                    v->string = malloc(l1 + l2 + 1);
                    memcpy(v->string, s1, l1);
                    memcpy(v->string + l1, s2, l2 + 1);
                }
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot add %d + %d at line %d",
                              left->type, right->type, ast->line);
        }

        case NIX_TOK_PLUS_PLUS: {
            /* List concatenation */
            if (left->type == NIX_VAL_LIST && right->type == NIX_VAL_LIST) {
                nix_value_t *v = nix_value_new(NIX_VAL_LIST);
                if (v) {
                    v->list.count = left->list.count + right->list.count;
                    v->list.cap = v->list.count > 0 ? v->list.count : 1;
                    v->list.items = calloc(v->list.cap, sizeof(*v->list.items));
                    for (size_t i = 0; i < left->list.count; i++)
                        v->list.items[i] = left->list.items[i];
                    for (size_t i = 0; i < right->list.count; i++)
                        v->list.items[left->list.count + i] = right->list.items[i];
                }
                /* Items are moved, not freed individually */
                free(left->list.items); left->list.items = NULL; left->list.count = 0;
                free(right->list.items); right->list.items = NULL; right->list.count = 0;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot concatenate non-lists at line %d", ast->line);
        }

        case NIX_TOK_MINUS: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_INT);
                if (v) v->integer = left->integer - right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot subtract non-integers at line %d", ast->line);
        }

        case NIX_TOK_STAR: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_INT);
                if (v) v->integer = left->integer * right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot multiply non-integers at line %d", ast->line);
        }

        case NIX_TOK_SLASH: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                if (right->integer == 0) {
                    /* leaked: shared value graph */
                    return eval_error(error, "division by zero at line %d", ast->line);
                }
                nix_value_t *v = nix_value_new(NIX_VAL_INT);
                if (v) v->integer = left->integer / right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot divide non-integers at line %d", ast->line);
        }

        case NIX_TOK_EQ: {
            nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
            if (v) v->boolean = nix_values_equal(left, right);
            /* leaked: shared value graph */
            return v;
        }

        case NIX_TOK_NEQ: {
            nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
            if (v) v->boolean = !nix_values_equal(left, right);
            /* leaked: shared value graph */
            return v;
        }

        case NIX_TOK_LT: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = left->integer < right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot compare non-integers with < at line %d", ast->line);
        }

        case NIX_TOK_LE: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = left->integer <= right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot compare non-integers with <= at line %d", ast->line);
        }

        case NIX_TOK_GT: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = left->integer > right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot compare non-integers with > at line %d", ast->line);
        }

        case NIX_TOK_GE: {
            if (left->type == NIX_VAL_INT && right->type == NIX_VAL_INT) {
                nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
                if (v) v->boolean = left->integer >= right->integer;
                /* leaked: shared value graph */
                return v;
            }
            /* leaked: shared value graph */
            return eval_error(error, "cannot compare non-integers with >= at line %d", ast->line);
        }

        default:
            /* leaked: shared value graph */
            return eval_error(error, "unsupported binary op %d at line %d",
                              ast->binop.op, ast->line);
        }
    }

    /* ── Unary operations ────────────────────────────────────────── */

    case NIX_NODE_UNARY_NOT: {
        nix_value_t *operand = nix_eval_inner(env, ast->operand, error);
        if (!operand) return NULL;
        nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
        if (v) v->boolean = !nix_is_truthy(operand);
        /* leaked: shared value graph */
        return v;
    }

    case NIX_NODE_NEGATE: {
        nix_value_t *operand = nix_eval_inner(env, ast->operand, error);
        if (!operand) return NULL;
        if (operand->type != NIX_VAL_INT) {
            /* leaked: shared value graph */
            return eval_error(error, "cannot negate non-integer at line %d", ast->line);
        }
        nix_value_t *v = nix_value_new(NIX_VAL_INT);
        if (v) v->integer = -operand->integer;
        /* leaked: shared value graph */
        return v;
    }

    /* ── Import ──────────────────────────────────────────────────── */

    case NIX_NODE_IMPORT: {
        /* Resolve path relative to the current file's directory */
        char *resolved = resolve_path(env->base_dir, ast->import.path);
        if (!resolved)
            return eval_error(error, "cannot resolve import path at line %d", ast->line);

        /* Read the file */
        size_t file_len = 0;
        char *source = read_entire_file(resolved, &file_len);
        if (!source) {
            char *err = NULL;
            asprintf(&err, "cannot read import '%s' at line %d", resolved, ast->line);
            free(resolved);
            if (error) *error = err; else free(err);
            return NULL;
        }

        /* Determine base_dir for the imported file */
        char *import_base_dir = strdup(resolved);
        if (import_base_dir) {
            /* Strip filename to get directory */
            char *slash = strrchr(import_base_dir, '/');
            if (slash) *slash = '\0';
        }

        /* Parse and evaluate the imported file */
        char *parse_err = NULL;
        nix_ast_t *imported_ast = nix_parse(source, file_len, &parse_err);
        free(source);

        if (!imported_ast) {
            char *err = NULL;
            asprintf(&err, "import parse error in '%s': %s", resolved,
                     parse_err ? parse_err : "unknown");
            free(resolved);
            free(import_base_dir);
            free(parse_err);
            if (error) *error = err; else free(err);
            return NULL;
        }

        /* Create a fresh environment for the import with the import's base dir */
        nix_env_t *import_env = nix_env_new();
        if (import_env) {
            free(import_env->base_dir);
            import_env->base_dir = import_base_dir;
            import_base_dir = NULL; /* ownership transferred */
        }

        char *eval_err = NULL;
        nix_value_t *result = nix_eval_inner(import_env, imported_ast, &eval_err);

        nix_ast_free(imported_ast);
        nix_env_free(import_env);
        free(resolved);
        free(import_base_dir);

        if (!result) {
            if (error) *error = eval_err;
            else free(eval_err);
            return NULL;
        }

        return result;
    }

    default:
        return eval_error(error, "unsupported AST node type %d at line %d",
                          ast->type, ast->line);
    }
}

/* ── Force thunk ──────────────────────────────────────────────────── */

nix_value_t *nix_force(nix_env_t *env, nix_value_t *val, char **error)
{
    if (!val) return NULL;
    if (val->type != NIX_VAL_THUNK) return val;

    if (val->thunk.forced) return val->thunk.forced;

    val->thunk.forced = nix_eval(env, val->thunk.expr, error);
    return val->thunk.forced;
}

/* ── JSON output ──────────────────────────────────────────────────── */

/* Forward declaration for recursive serialization */
static void json_serialize(nix_value_t *val, FILE *f, int indent);

static void json_indent(FILE *f, int indent)
{
    for (int i = 0; i < indent; i++) fputc(' ', f);
}

static void json_escape_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        default:
            if ((unsigned char)*s < 0x20) {
                fprintf(f, "\\u%04x", (unsigned char)*s);
            } else {
                fputc(*s, f);
            }
        }
    }
    fputc('"', f);
}

static void json_serialize(nix_value_t *val, FILE *f, int indent)
{
    if (!val) { fputs("null", f); return; }

    switch (val->type) {
    case NIX_VAL_NULL:
        fputs("null", f);
        break;

    case NIX_VAL_BOOL:
        fputs(val->boolean ? "true" : "false", f);
        break;

    case NIX_VAL_INT:
        fprintf(f, "%ld", (long)val->integer);
        break;

    case NIX_VAL_STRING:
        json_escape_string(f, val->string ? val->string : "");
        break;

    case NIX_VAL_PATH:
        json_escape_string(f, val->path ? val->path : "");
        break;

    case NIX_VAL_LIST: {
        fputc('[', f);
        if (val->list.count > 0) {
            fputc('\n', f);
            for (size_t i = 0; i < val->list.count; i++) {
                json_indent(f, indent + 2);
                json_serialize(val->list.items[i], f, indent + 2);
                if (i + 1 < val->list.count) fputc(',', f);
                fputc('\n', f);
            }
            json_indent(f, indent);
        }
        fputc(']', f);
        break;
    }

    case NIX_VAL_ATTR_SET: {
        fputc('{', f);
        if (val->attr_set.count > 0) {
            fputc('\n', f);
            for (size_t i = 0; i < val->attr_set.count; i++) {
                json_indent(f, indent + 2);
                json_escape_string(f, val->attr_set.entries[i].key);
                fputs(": ", f);
                json_serialize(val->attr_set.entries[i].value, f, indent + 2);
                if (i + 1 < val->attr_set.count) fputc(',', f);
                fputc('\n', f);
            }
            json_indent(f, indent);
        }
        fputc('}', f);
        break;
    }

    case NIX_VAL_LAMBDA:
        fputs("\"<lambda>\"", f);
        break;

    case NIX_VAL_BUILTIN:
        fprintf(f, "\"<builtin:%s>\"", val->builtin.name ? val->builtin.name : "?");
        break;

    case NIX_VAL_THUNK:
        if (val->thunk.forced)
            json_serialize(val->thunk.forced, f, indent);
        else
            fputs("\"<thunk>\"", f);
        break;
    }
}

char *nix_to_json(nix_value_t *val)
{
    /* Use open_memstream for a dynamically-growing buffer */
    char *buf = NULL;
    size_t buf_size = 0;
    FILE *f = open_memstream(&buf, &buf_size);
    if (!f) return strdup("{}");

    json_serialize(val, f, 0);
    fclose(f);

    return buf ? buf : strdup("{}");
}

/* ── Convenience: one-shot eval ───────────────────────────────────── */

char *nix_eval_file(const char *source, size_t len, char **error)
{
    return nix_eval_file_with_base(source, len, NULL, error);
}

char *nix_eval_file_with_base(const char *source, size_t len,
                               const char *base_dir, char **error)
{
    nix_env_t    *env = nix_env_new();
    if (!env) {
        if (error) *error = strdup("failed to create evaluation environment");
        return NULL;
    }

    /* Set base directory for import resolution */
    if (base_dir) {
        free(env->base_dir);
        env->base_dir = strdup(base_dir);
    }

    nix_ast_t    *ast = nix_parse(source, len, error);
    nix_value_t  *val = NULL;
    char         *json = NULL;

    if (!ast) goto done;

    val = nix_eval(env, ast, error);
    if (!val) goto done;

    /* Auto-apply: if the result is a lambda (e.g., { config, ... }: { ... }),
     * apply it with a fixed-point pattern. This is how 2O9.nix files work:
     * they define a function that takes { config, ... } and returns the
     * configuration attrset, where config refers to the result itself.
     *
     * Strategy: evaluate the body attrset binding-by-binding, making each
     * result immediately visible through the config reference. This way,
     * `packages = if config.services.sshd.enable ...` works because
     * `services` was already added to the result (which config points to). */
    if (val->type == NIX_VAL_LAMBDA) {
        /* Create the result attrset upfront */
        nix_value_t *result_set = nix_value_new(NIX_VAL_ATTR_SET);
        if (!result_set) goto done;
        result_set->attr_set.cap = 16;
        result_set->attr_set.entries = calloc(result_set->attr_set.cap,
                                               sizeof(*result_set->attr_set.entries));
        result_set->attr_set.count = 0;

        /* Create a thunk that, when forced, returns the result attrset.
         * As bindings are added to result_set, they become visible through
         * config automatically. */
        nix_value_t *config_thunk = nix_value_new(NIX_VAL_THUNK);
        if (!config_thunk) goto done;
        config_thunk->thunk.env = NULL;
        config_thunk->thunk.expr = NULL;
        config_thunk->thunk.forced = result_set;

        /* Build the argument: { config = <thunk pointing to result>; } */
        nix_value_t *arg = nix_value_new(NIX_VAL_ATTR_SET);
        if (!arg) goto done;
        arg->attr_set.cap = 8;
        arg->attr_set.entries = calloc(arg->attr_set.cap, sizeof(*arg->attr_set.entries));
        arg->attr_set.count = 0;
        attrset_bind(arg, "config", config_thunk);

        /* Create the call environment with formal params */
        nix_env_t *call_env = nix_env_push(val->lambda.closure_env);
        for (size_t j = 0; j < val->lambda.param->attr_set.count; j++) {
            const char *pname = val->lambda.param->attr_set.bindings[j].key;
            nix_value_t *pval = attrset_lookup(arg, pname);
            if (!pval) {
                nix_ast_t *default_ast = val->lambda.param->attr_set.bindings[j].value;
                if (default_ast && default_ast->type != NIX_NODE_NULL) {
                    char *err2 = NULL;
                    pval = nix_eval(call_env, default_ast, &err2);
                    if (err2) free(err2);
                }
                if (!pval) pval = nix_value_new(NIX_VAL_NULL);
            }
            nix_env_bind(call_env, pname, pval);
        }

        /* Check if the body is an attrset that we can evaluate binding-by-binding */
        nix_ast_t *body = val->lambda.body;
        nix_value_t *final_result = NULL;

        if (body->type == NIX_NODE_ATTR_SET) {
            /* Evaluate each binding one at a time, adding to result_set.
             * Since config_thunk points to result_set, each new binding
             * is immediately visible through config. */
            for (size_t i = 0; i < body->attr_set.count; i++) {
                char *bind_err = NULL;
                nix_value_t *bind_val = nix_eval(call_env,
                    body->attr_set.bindings[i].value, &bind_err);
                if (bind_err) {
                    if (error) *error = bind_err;
                    else free(bind_err);
                    nix_env_free(call_env);
                    goto done;
                }
                /* Handle dotted key paths like "services.sshd.enable" */
                const char *key = body->attr_set.bindings[i].key;
                const char *dot = strchr(key, '.');
                if (dot) {
                    size_t nparts = 1;
                    for (const char *p = key; *p; p++)
                        if (*p == '.') nparts++;
                    const char **parts = calloc(nparts, sizeof(char *));
                    char *keycopy = strdup(key);
                    char *saveptr = NULL;
                    size_t pi = 0;
                    char *tok2 = strtok_r(keycopy, ".", &saveptr);
                    while (tok2 && pi < nparts) {
                        parts[pi++] = tok2;
                        tok2 = strtok_r(NULL, ".", &saveptr);
                    }
                    attrset_bind_path(result_set, parts, pi, bind_val);
                    free(keycopy);
                    free(parts);
                } else {
                    attrset_bind(result_set, key, bind_val);
                }
            }
            final_result = result_set;
        } else {
            /* Body is not an attrset — just evaluate normally */
            char *eval_err = NULL;
            final_result = nix_eval(call_env, body, &eval_err);
            if (eval_err) {
                if (error) *error = eval_err;
                else free(eval_err);
            }
        }

        nix_env_free(call_env);
        if (final_result) val = final_result;
    }

    json = nix_to_json(val);
    /* NOTE: We intentionally do NOT free val here. Values form a shared
     * graph — the result may contain the same nix_value_t pointers that
     * are also referenced from the evaluation environment. Shallow-free
     * of the top-level value would leave dangling pointers in the env;
     * a deep free would double-free shared children. The entire value
     * graph will be reclaimed when the process exits. A proper GC or
     * reference-counting scheme would be needed for long-lived interpreters. */

done:
    nix_ast_free(ast);
    nix_env_free(env);
    return json;
}

/* ── Builtin functions ────────────────────────────────────────────── */

/* builtin_map : (a -> b) -> [a] -> [b] */
nix_value_t *builtin_map(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)ctx;
    if (!arg) return NULL;

    /* map takes a function and returns a partially-applied builtin.
     * Since our builtins only take one arg, we return a lambda-like
     * value that captures the function. For simplicity, we expect
     * map to be called as (map f list) which is ((map f) list).
     * The parser desugars this to APPLY(APPLY(map, f), list).
     * So we get called first with f, then with list. */

    /* First call: arg is the function */
    if (arg->type == NIX_VAL_LAMBDA || arg->type == NIX_VAL_BUILTIN) {
        /* Return a "partial application" — we use a BUILTIN with ctx holding the func */
        nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
        if (partial) {
            partial->builtin.name = "map(partial)";
            partial->builtin.func = builtin_map;
            partial->builtin.ctx = arg;  /* store the function */
        }
        return partial;
    }

    /* Second call: arg is the list, ctx is the function */
    if (arg->type == NIX_VAL_LIST && ctx) {
        nix_value_t *func = (nix_value_t *)ctx;
        nix_value_t *result = nix_value_new(NIX_VAL_LIST);
        if (!result) return NULL;
        result->list.cap = arg->list.count > 0 ? arg->list.count : 1;
        result->list.items = calloc(result->list.cap, sizeof(*result->list.items));
        result->list.count = arg->list.count;

        for (size_t i = 0; i < arg->list.count; i++) {
            /* Apply func to each element */
            if (func->type == NIX_VAL_LAMBDA) {
                nix_env_t *call_env = nix_env_push(func->lambda.closure_env);
                if (func->lambda.param->type == NIX_NODE_IDENT)
                    nix_env_bind(call_env, func->lambda.param->ident,
                                 arg->list.items[i]);
                char *err = NULL;
                result->list.items[i] = nix_eval(call_env, func->lambda.body, &err);
                nix_env_free(call_env);
                if (!result->list.items[i]) {
                    result->list.items[i] = nix_value_new(NIX_VAL_NULL);
                }
            } else if (func->type == NIX_VAL_BUILTIN) {
                result->list.items[i] = func->builtin.func(env, arg->list.items[i],
                                                            func->builtin.ctx);
                if (!result->list.items[i])
                    result->list.items[i] = nix_value_new(NIX_VAL_NULL);
            } else {
                result->list.items[i] = nix_value_new(NIX_VAL_NULL);
            }
        }
        return result;
    }

    return NULL;
}

/* builtin_filter : (a -> bool) -> [a] -> [a] */
nix_value_t *builtin_filter(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env;
    if (!arg) return NULL;

    /* First call: arg is the predicate function */
    if (arg->type == NIX_VAL_LAMBDA || arg->type == NIX_VAL_BUILTIN) {
        nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
        if (partial) {
            partial->builtin.name = "filter(partial)";
            partial->builtin.func = builtin_filter;
            partial->builtin.ctx = arg;
        }
        return partial;
    }

    /* Second call: arg is the list */
    if (arg->type == NIX_VAL_LIST && ctx) {
        nix_value_t *pred = (nix_value_t *)ctx;
        nix_value_t *result = nix_value_new(NIX_VAL_LIST);
        if (!result) return NULL;
        result->list.cap = arg->list.count > 0 ? arg->list.count : 1;
        result->list.items = calloc(result->list.cap, sizeof(*result->list.items));
        result->list.count = 0;

        for (size_t i = 0; i < arg->list.count; i++) {
            int keep = 0;
            /* Force the list item (it might be a thunk) */
            nix_value_t *item = arg->list.items[i];
            if (item && item->type == NIX_VAL_THUNK) {
                char *err2 = NULL;
                item = nix_force(env, item, &err2);
                if (err2) free(err2);
            }

            if (pred->type == NIX_VAL_LAMBDA) {
                nix_env_t *call_env = nix_env_push(pred->lambda.closure_env);
                if (pred->lambda.param->type == NIX_NODE_IDENT)
                    nix_env_bind(call_env, pred->lambda.param->ident, item);
                else if (pred->lambda.param->type == NIX_NODE_ATTR_SET) {
                    /* Formal params: match attrset items to formal params */
                    for (size_t j = 0; j < pred->lambda.param->attr_set.count; j++) {
                        const char *pname = pred->lambda.param->attr_set.bindings[j].key;
                        nix_value_t *pval = item;
                        if (item && item->type == NIX_VAL_ATTR_SET)
                            pval = attrset_lookup(item, pname);
                        if (!pval) pval = nix_value_new(NIX_VAL_NULL);
                        nix_env_bind(call_env, pname, pval);
                    }
                }
                char *err = NULL;
                nix_value_t *v = nix_eval(call_env, pred->lambda.body, &err);
                if (v) { keep = nix_is_truthy(v); }
                if (err) free(err);
                nix_env_free(call_env);
            } else if (pred->type == NIX_VAL_BUILTIN) {
                /* Apply builtin predicate to item */
                nix_value_t *v = pred->builtin.func(env, item, pred->builtin.ctx);
                if (v) { keep = nix_is_truthy(v); }
            }
            if (keep) {
                result->list.items[result->list.count++] = item;
            }
        }
        return result;
    }

    return NULL;
}

/* builtin_length : [a] -> int */
nix_value_t *builtin_length(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    nix_value_t *v = nix_value_new(NIX_VAL_INT);
    if (v) {
        if (arg && arg->type == NIX_VAL_LIST)
            v->integer = (int64_t)arg->list.count;
        else if (arg && arg->type == NIX_VAL_STRING)
            v->integer = (int64_t)strlen(arg->string);
        else
            v->integer = 0;
    }
    return v;
}

/* builtin_head : [a] -> a */
nix_value_t *builtin_head(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_LIST || arg->list.count == 0) return NULL;
    return arg->list.items[0];
}

/* builtin_tail : [a] -> [a] */
nix_value_t *builtin_tail(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_LIST || arg->list.count == 0) return NULL;
    nix_value_t *v = nix_value_new(NIX_VAL_LIST);
    if (!v) return NULL;
    v->list.count = arg->list.count - 1;
    v->list.cap = v->list.count > 0 ? v->list.count : 1;
    v->list.items = calloc(v->list.cap, sizeof(*v->list.items));
    for (size_t i = 1; i < arg->list.count; i++)
        v->list.items[i - 1] = arg->list.items[i];
    return v;
}

/* builtin_attrNames : attrset -> [string] */
nix_value_t *builtin_attr_names(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_ATTR_SET) return NULL;
    nix_value_t *v = nix_value_new(NIX_VAL_LIST);
    if (!v) return NULL;
    v->list.cap = arg->attr_set.count > 0 ? arg->attr_set.count : 1;
    v->list.items = calloc(v->list.cap, sizeof(*v->list.items));
    v->list.count = arg->attr_set.count;
    for (size_t i = 0; i < arg->attr_set.count; i++) {
        nix_value_t *s = nix_value_new(NIX_VAL_STRING);
        if (s) s->string = strdup(arg->attr_set.entries[i].key);
        v->list.items[i] = s;
    }
    return v;
}

/* builtin_attrValues : attrset -> [value] */
nix_value_t *builtin_attr_values(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_ATTR_SET) return NULL;
    nix_value_t *v = nix_value_new(NIX_VAL_LIST);
    if (!v) return NULL;
    v->list.cap = arg->attr_set.count > 0 ? arg->attr_set.count : 1;
    v->list.items = calloc(v->list.cap, sizeof(*v->list.items));
    v->list.count = arg->attr_set.count;
    for (size_t i = 0; i < arg->attr_set.count; i++)
        v->list.items[i] = arg->attr_set.entries[i].value;
    return v;
}

/* builtin_hasAttr : string -> attrset -> bool */
nix_value_t *builtin_has_attr(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env;
    if (!arg) return NULL;

    /* First call: arg is the key string */
    if (arg->type == NIX_VAL_STRING && !ctx) {
        nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
        if (partial) {
            partial->builtin.name = "hasAttr(partial)";
            partial->builtin.func = builtin_has_attr;
            partial->builtin.ctx = arg;
        }
        return partial;
    }

    /* Second call: arg is the attrset, ctx is the key */
    if (arg->type == NIX_VAL_ATTR_SET && ctx) {
        nix_value_t *key = (nix_value_t *)ctx;
        nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
        if (v) {
            const char *kname = key->type == NIX_VAL_STRING ? key->string : "";
            v->boolean = attrset_has(arg, kname);
        }
        return v;
    }

    return NULL;
}

/* builtin_getAttr : string -> attrset -> value */
nix_value_t *builtin_get_attr(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env;
    if (!arg) return NULL;

    if (arg->type == NIX_VAL_STRING && !ctx) {
        nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
        if (partial) {
            partial->builtin.name = "getAttr(partial)";
            partial->builtin.func = builtin_get_attr;
            partial->builtin.ctx = arg;
        }
        return partial;
    }

    if (arg->type == NIX_VAL_ATTR_SET && ctx) {
        nix_value_t *key = (nix_value_t *)ctx;
        const char *kname = key->type == NIX_VAL_STRING ? key->string : "";
        return attrset_lookup(arg, kname);
    }

    return NULL;
}

/* builtin_toJSON : value -> string */
nix_value_t *builtin_to_json(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg) return NULL;
    char *json = nix_to_json(arg);
    nix_value_t *v = nix_value_new(NIX_VAL_STRING);
    if (v) v->string = json ? json : strdup("");
    else free(json);
    return v;
}

/* ── JSON parser for fromJSON builtin ─────────────────────────── */

static nix_value_t *json_parse_value(const char *js, size_t *p);

static nix_value_t *json_parse_string(const char *js, size_t *p)
{
    if (js[*p] != '"') return NULL;
    (*p)++; /* skip opening " */
    size_t cap = 64;
    char *buf = calloc(cap, 1);
    size_t len = 0;

    while (js[*p] && js[*p] != '"') {
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        if (js[*p] == '\\') {
            (*p)++;
            switch (js[*p]) {
            case '"':  buf[len++] = '"';  break;
            case '\\': buf[len++] = '\\'; break;
            case '/':  buf[len++] = '/';  break;
            case 'n':  buf[len++] = '\n'; break;
            case 't':  buf[len++] = '\t'; break;
            case 'r':  buf[len++] = '\r'; break;
            default:   buf[len++] = js[*p]; break;
            }
        } else {
            buf[len++] = js[*p];
        }
        (*p)++;
    }
    if (js[*p] == '"') (*p)++;
    buf[len] = '\0';

    nix_value_t *v = nix_value_new(NIX_VAL_STRING);
    if (v) v->string = buf; else free(buf);
    return v;
}

static nix_value_t *json_parse_value(const char *js, size_t *p)
{
    while (js[*p] == ' ' || js[*p] == '\t' || js[*p] == '\n' || js[*p] == '\r') (*p)++;

    if (js[*p] == '"') return json_parse_string(js, p);

    if (js[*p] == '[') {
        (*p)++;
        nix_value_t *list = nix_value_new(NIX_VAL_LIST);
        list->list.cap = 8;
        list->list.items = calloc(list->list.cap, sizeof(*list->list.items));
        list->list.count = 0;

        while (js[*p]) {
            while (js[*p] == ' ' || js[*p] == '\t' || js[*p] == '\n' || js[*p] == '\r' || js[*p] == ',') (*p)++;
            if (js[*p] == ']') { (*p)++; break; }
            nix_value_t *elem = json_parse_value(js, p);
            if (!elem) break;
            if (list->list.count >= list->list.cap) {
                list->list.cap *= 2;
                list->list.items = realloc(list->list.items,
                                           list->list.cap * sizeof(*list->list.items));
            }
            list->list.items[list->list.count++] = elem;
        }
        return list;
    }

    if (js[*p] == '{') {
        (*p)++;
        nix_value_t *obj = nix_value_new(NIX_VAL_ATTR_SET);
        obj->attr_set.cap = 8;
        obj->attr_set.entries = calloc(obj->attr_set.cap, sizeof(*obj->attr_set.entries));
        obj->attr_set.count = 0;

        while (js[*p]) {
            while (js[*p] == ' ' || js[*p] == '\t' || js[*p] == '\n' || js[*p] == '\r' || js[*p] == ',') (*p)++;
            if (js[*p] == '}') { (*p)++; break; }
            nix_value_t *key = json_parse_string(js, p);
            if (!key) break;
            while (js[*p] == ' ' || js[*p] == '\t' || js[*p] == '\n' || js[*p] == '\r' || js[*p] == ':') (*p)++;
            nix_value_t *val = json_parse_value(js, p);
            if (!val) { nix_value_free(key); break; }
            attrset_bind(obj, key->string, val);
            nix_value_free(key);
        }
        return obj;
    }

    if (strncmp(js + *p, "true", 4) == 0) {
        *p += 4;
        nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
        if (v) v->boolean = 1;
        return v;
    }

    if (strncmp(js + *p, "false", 5) == 0) {
        *p += 5;
        nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
        if (v) v->boolean = 0;
        return v;
    }

    if (strncmp(js + *p, "null", 4) == 0) {
        *p += 4;
        return nix_value_new(NIX_VAL_NULL);
    }

    /* Number */
    if (js[*p] == '-' || (js[*p] >= '0' && js[*p] <= '9')) {
        int64_t n = 0;
        int neg = 0;
        if (js[*p] == '-') { neg = 1; (*p)++; }
        while (js[*p] >= '0' && js[*p] <= '9') {
            n = n * 10 + (js[*p] - '0');
            (*p)++;
        }
        nix_value_t *v = nix_value_new(NIX_VAL_INT);
        if (v) v->integer = neg ? -n : n;
        return v;
    }

    return NULL;
}

/* builtin_fromJSON : string -> value */
nix_value_t *builtin_from_json(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_STRING || !arg->string) return NULL;

    size_t pos = 0;
    return json_parse_value(arg->string, &pos);
}

/* builtin_trace : string -> a -> a (prints to stderr, returns second arg) */
nix_value_t *builtin_trace(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg) return NULL;

    /* First call: arg is the message string */
    if (arg->type == NIX_VAL_STRING && !ctx) {
        nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
        if (partial) {
            partial->builtin.name = "trace(partial)";
            partial->builtin.func = builtin_trace;
            partial->builtin.ctx = arg;
        }
        return partial;
    }

    /* Second call: print ctx (the message), return arg (the value) */
    if (ctx) {
        nix_value_t *msg = (nix_value_t *)ctx;
        if (msg->type == NIX_VAL_STRING && msg->string)
            fprintf(stderr, "trace: %s\n", msg->string);
    }
    return arg;
}

/* builtin_pathExists : string -> bool */
nix_value_t *builtin_path_exists(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_STRING || !arg->string) {
        nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
        if (v) v->boolean = 0;
        return v;
    }

    struct stat st;
    nix_value_t *v = nix_value_new(NIX_VAL_BOOL);
    if (v) v->boolean = (stat(arg->string, &st) == 0);
    return v;
}

/* builtin_readFile : string -> string */
nix_value_t *builtin_read_file(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg || arg->type != NIX_VAL_STRING || !arg->string) return NULL;

    size_t len = 0;
    char *content = read_entire_file(arg->string, &len);
    if (!content) return NULL;

    nix_value_t *v = nix_value_new(NIX_VAL_STRING);
    if (v) v->string = content;
    else free(content);
    return v;
}

/* builtin_abort : string -> ! (never returns) */
nix_value_t *builtin_abort(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (arg && arg->type == NIX_VAL_STRING && arg->string)
        fprintf(stderr, "abort: %s\n", arg->string);
    else
        fprintf(stderr, "abort called\n");
    return NULL;
}

/* builtin_throw : string -> ! (like abort but catchable in theory) */
nix_value_t *builtin_throw(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (arg && arg->type == NIX_VAL_STRING && arg->string)
        fprintf(stderr, "throw: %s\n", arg->string);
    else
        fprintf(stderr, "throw called\n");
    return NULL;
}

/* builtin_toString : value -> string */
nix_value_t *builtin_to_string(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    if (!arg) return NULL;

    nix_value_t *v = nix_value_new(NIX_VAL_STRING);
    if (!v) return NULL;

    switch (arg->type) {
    case NIX_VAL_STRING:
        v->string = strdup(arg->string ? arg->string : "");
        break;
    case NIX_VAL_INT: {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%ld", (long)arg->integer);
        v->string = strdup(tmp);
        break;
    }
    case NIX_VAL_BOOL:
        v->string = strdup(arg->boolean ? "true" : "false");
        break;
    case NIX_VAL_NULL:
        v->string = strdup("null");
        break;
    case NIX_VAL_PATH:
        v->string = strdup(arg->path ? arg->path : "");
        break;
    default: {
        char *json = nix_to_json(arg);
        v->string = json ? json : strdup("");
        break;
    }
    }
    return v;
}

/* builtin_stringLength : string -> int */
nix_value_t *builtin_string_length(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env; (void)ctx;
    nix_value_t *v = nix_value_new(NIX_VAL_INT);
    if (v) {
        if (arg && arg->type == NIX_VAL_STRING && arg->string)
            v->integer = (int64_t)strlen(arg->string);
        else
            v->integer = 0;
    }
    return v;
}

/* builtin_replaceStrings : [string] -> [string] -> string -> string */
nix_value_t *builtin_replace_strings(nix_env_t *env, nix_value_t *arg, void *ctx)
{
    (void)env;
    if (!arg) return NULL;

    /* Three-arg builtin via partial application.
     * (replaceStrings from) to str = ...
     * Called as ((replaceStrings from) to) str */

    if (!ctx) {
        /* First call: arg = from list */
        nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
        if (partial) {
            partial->builtin.name = "replaceStrings(partial1)";
            partial->builtin.func = builtin_replace_strings;
            partial->builtin.ctx = arg;
        }
        return partial;
    } else {
        /* Check if ctx is the from list (2nd call) or a pair (3rd call) */
        nix_value_t *from = (nix_value_t *)ctx;

        /* Check if we already have 'to' baked in.
         * We use a simple convention: if ctx is a list, it's 'from'.
         * If ctx is an attrset with keys __from and __to, it's the pair. */
        if (from->type == NIX_VAL_LIST) {
            /* Second call: arg = to list */
            nix_value_t *pair = nix_value_new(NIX_VAL_ATTR_SET);
            if (pair) {
                pair->attr_set.cap = 4;
                pair->attr_set.entries = calloc(pair->attr_set.cap, sizeof(*pair->attr_set.entries));
                pair->attr_set.count = 0;
                attrset_bind(pair, "__from", from);
                attrset_bind(pair, "__to", arg);
            }
            nix_value_t *partial = nix_value_new(NIX_VAL_BUILTIN);
            if (partial) {
                partial->builtin.name = "replaceStrings(partial2)";
                partial->builtin.func = builtin_replace_strings;
                partial->builtin.ctx = pair;
            }
            return partial;
        }

        if (from->type == NIX_VAL_ATTR_SET) {
            /* Third call: arg = source string, ctx has __from and __to */
            nix_value_t *from_list = attrset_lookup(from, "__from");
            nix_value_t *to_list = attrset_lookup(from, "__to");

            if (!arg || arg->type != NIX_VAL_STRING || !arg->string ||
                !from_list || from_list->type != NIX_VAL_LIST ||
                !to_list || to_list->type != NIX_VAL_LIST) {
                return nix_value_new(NIX_VAL_STRING);
            }

            /* Simple replacement: for each (from[i], to[i]) pair,
             * replace occurrences in the string */
            const char *src = arg->string;
            size_t src_len = strlen(src);
            size_t cap = src_len * 2 + 1;
            char *result = calloc(cap, 1);
            size_t rlen = 0;

            size_t i = 0;
            while (i < src_len) {
                int replaced = 0;
                for (size_t j = 0; j < from_list->list.count; j++) {
                    nix_value_t *from_str = from_list->list.items[j];
                    if (!from_str || from_str->type != NIX_VAL_STRING || !from_str->string)
                        continue;
                    size_t flen = strlen(from_str->string);
                    if (flen == 0) continue;
                    if (i + flen <= src_len &&
                        memcmp(src + i, from_str->string, flen) == 0) {
                        nix_value_t *to_str = j < to_list->list.count ?
                            to_list->list.items[j] : NULL;
                        const char *replacement = (to_str && to_str->type == NIX_VAL_STRING) ?
                            to_str->string : "";
                        size_t rlen2 = strlen(replacement);
                        while (rlen + rlen2 + 1 >= cap) { cap *= 2; result = realloc(result, cap); }
                        memcpy(result + rlen, replacement, rlen2);
                        rlen += rlen2;
                        i += flen;
                        replaced = 1;
                        break;
                    }
                }
                if (!replaced) {
                    if (rlen + 1 >= cap) { cap *= 2; result = realloc(result, cap); }
                    result[rlen++] = src[i++];
                }
            }
            result[rlen] = '\0';

            nix_value_t *v = nix_value_new(NIX_VAL_STRING);
            if (v) v->string = result;
            else free(result);
            return v;
        }
    }

    return NULL;
}
