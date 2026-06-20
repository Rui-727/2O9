/* 2O9 Nix Evaluator — own C implementation (stub)
 *
 * This is a stub that provides the API surface defined in nix_eval.h.
 * The actual lexer, parser, and evaluator will be implemented in Phase 3.
 *
 * For now, nix_eval_file() returns a hardcoded JSON manifest for testing
 * the declarative engine plumbing.
 *
 * Part of lib2O9.  Pure C, no C++ dependencies.
 */

#include "nix_eval.h"

#include <stdlib.h>
#include <string.h>

/* ── Value allocation ─────────────────────────────────────────────── */

nix_value_t *nix_value_new(nix_val_type_t type)
{
    nix_value_t *v = calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}

void nix_value_free(nix_value_t *val)
{
    if (!val) return;

    switch (val->type) {
    case NIX_VAL_STRING:
        free(val->string);
        break;
    case NIX_VAL_PATH:
        free(val->path);
        break;
    case NIX_VAL_LIST:
        for (size_t i = 0; i < val->list.count; i++)
            nix_value_free(val->list.items[i]);
        free(val->list.items);
        break;
    case NIX_VAL_ATTR_SET:
        for (size_t i = 0; i < val->attr_set.count; i++) {
            free(val->attr_set.entries[i].key);
            nix_value_free(val->attr_set.entries[i].value);
        }
        free(val->attr_set.entries);
        break;
    case NIX_VAL_THUNK:
        if (val->thunk.forced)
            nix_value_free(val->thunk.forced);
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
};

nix_env_t *nix_env_new(void)
{
    nix_env_t *env = calloc(1, sizeof(*env));
    if (!env) return NULL;
    env->cap = 16;
    env->entries = calloc(env->cap, sizeof(*env->entries));
    if (!env->entries) { free(env); return NULL; }

    /* Register builtins here when implemented */
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
    nix_env_free(env->parent);
    free(env);
}

int nix_env_bind(nix_env_t *env, const char *name, nix_value_t *val)
{
    if (!env || !name) return -1;
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

/* ── Parser (stub) ────────────────────────────────────────────────── */

nix_ast_t *nix_parse(const char *source, size_t len, char **error)
{
    *error = strdup("nix_parse: not yet implemented (Phase 3)");
    return NULL;
}

void nix_ast_free(nix_ast_t *ast)
{
    /* TODO: implement recursive AST free in Phase 3 */
    (void)ast;
}

/* ── Evaluator (stub) ─────────────────────────────────────────────── */

nix_value_t *nix_eval(nix_env_t *env, nix_ast_t *ast, char **error)
{
    *error = strdup("nix_eval: not yet implemented (Phase 3)");
    return NULL;
}

nix_value_t *nix_force(nix_env_t *env, nix_value_t *val, char **error)
{
    if (!val) return NULL;
    if (val->type == NIX_VAL_THUNK) {
        if (val->thunk.forced) return val->thunk.forced;
        val->thunk.forced = nix_eval(env, val->thunk.expr, error);
        return val->thunk.forced;
    }
    return val;
}

/* ── JSON output (stub) ───────────────────────────────────────────── */

char *nix_to_json(nix_value_t *val)
{
    /* TODO: implement JSON serialization in Phase 3.
     * For now, return a minimal hardcoded manifest for testing. */
    return strdup(
        "{\n"
        "  \"packages\": [],\n"
        "  \"aur\": { \"packages\": [], \"build\": { \"profile\": \"safe\", \"jobs\": \"auto\" } },\n"
        "  \"pacman\": {\n"
        "    \"options\": { \"SigLevel\": \"Required DatabaseOptional\" },\n"
        "    \"repos\": {}\n"
        "  },\n"
        "  \"services\": {}\n"
        "}\n"
    );
}

/* ── Convenience ──────────────────────────────────────────────────── */

char *nix_eval_file(const char *source, size_t len, char **error)
{
    nix_env_t    *env = nix_env_new();
    nix_ast_t    *ast = nix_parse(source, len, error);
    nix_value_t  *val = NULL;
    char         *json = NULL;

    if (!ast) goto done;

    val = nix_eval(env, ast, error);
    if (!val) goto done;

    json = nix_to_json(val);
    nix_value_free(val);

done:
    nix_ast_free(ast);
    nix_env_free(env);
    return json;
}
