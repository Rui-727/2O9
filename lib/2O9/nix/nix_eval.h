/* 2O9 Nix Evaluator — own C implementation
 *
 * Evaluates the subset of the Nix expression language needed to parse
 * 2O9.nix configuration files.  Not a full Nix interpreter — no
 * derivations, no fetchers, no flakes.
 *
 * Part of lib2O9.  Pure C, no C++ dependencies.
 */

#ifndef TWO09_NIX_EVAL_H
#define TWO09_NIX_EVAL_H

#include <stddef.h>
#include <stdint.h>

/* ── Value types ──────────────────────────────────────────────────── */

typedef enum nix_val_type {
    NIX_VAL_NULL,
    NIX_VAL_BOOL,
    NIX_VAL_INT,
    NIX_VAL_STRING,
    NIX_VAL_PATH,
    NIX_VAL_LIST,
    NIX_VAL_ATTR_SET,
    NIX_VAL_LAMBDA,
    NIX_VAL_THUNK,      /* unevaluated (lazy) */
    NIX_VAL_BUILTIN,    /* C function primop */
} nix_val_type_t;

/* Forward declarations */
typedef struct nix_value nix_value_t;
typedef struct nix_env   nix_env_t;
typedef struct nix_ast   nix_ast_t;

/* Attribute set entry: key-value pair */
typedef struct nix_attr_entry {
    char             *key;
    nix_value_t      *value;
} nix_attr_entry_t;

/* The value union — tagged by nix_val_type_t */
struct nix_value {
    nix_val_type_t type;
    union {
        int              boolean;       /* NIX_VAL_BOOL   */
        int64_t          integer;       /* NIX_VAL_INT    */
        char            *string;        /* NIX_VAL_STRING */
        char            *path;          /* NIX_VAL_PATH   */
        struct {                        /* NIX_VAL_LIST   */
            nix_value_t **items;
            size_t        count;
            size_t        cap;
        } list;
        struct {                        /* NIX_VAL_ATTR_SET */
            nix_attr_entry_t *entries;
            size_t            count;
            size_t            cap;
        } attr_set;
        struct {                        /* NIX_VAL_LAMBDA */
            nix_env_t   *closure_env;
            nix_ast_t   *param;        /* identifier or pattern */
            nix_ast_t   *body;
        } lambda;
        struct {                        /* NIX_VAL_BUILTIN */
            const char     *name;
            nix_value_t *(*func)(nix_env_t *env, nix_value_t *arg, void *ctx);
            void           *ctx;
        } builtin;
        struct {                        /* NIX_VAL_THUNK  */
            nix_env_t   *env;
            nix_ast_t   *expr;
            nix_value_t *forced;        /* NULL until forced */
        } thunk;
    };
};

/* ── Lexer ────────────────────────────────────────────────────────── */

typedef enum nix_tok_type {
    NIX_TOK_EOF,
    NIX_TOK_IDENT,
    NIX_TOK_INT,
    NIX_TOK_STRING,        /* "..." with possible interpolation */
    NIX_TOK_PATH,          /* /absolute/path */
    NIX_TOK_LBRACE,        /* { */
    NIX_TOK_RBRACE,        /* } */
    NIX_TOK_LBRACKET,      /* [ */
    NIX_TOK_RBRACKET,      /* ] */
    NIX_TOK_LPAREN,        /* ( */
    NIX_TOK_RPAREN,        /* ) */
    NIX_TOK_COLON,         /* : */
    NIX_TOK_SEMICOLON,     /* ; */
    NIX_TOK_COMMA,         /* , */
    NIX_TOK_DOT,           /* . */
    NIX_TOK_EQUALS,        /* = */
    NIX_TOK_QUESTION,      /* ? */
    NIX_TOK_PLUS,          /* + */
    NIX_TOK_PLUS_PLUS,     /* ++ */
    NIX_TOK_MINUS,         /* - */
    NIX_TOK_STAR,          /* * */
    NIX_TOK_SLASH,         /* / */
    NIX_TOK_AND,           /* && */
    NIX_TOK_OR,            /* || */
    NIX_TOK_NOT,           /* ! */
    NIX_TOK_LT,            /* < */
    NIX_TOK_LE,            /* <= */
    NIX_TOK_GT,            /* > */
    NIX_TOK_GE,            /* >= */
    NIX_TOK_EQ,            /* == */
    NIX_TOK_NEQ,           /* != */
    NIX_TOK_ARROW,         /* -> */
    NIX_TOK_KW_IF,
    NIX_TOK_KW_THEN,
    NIX_TOK_KW_ELSE,
    NIX_TOK_KW_LET,
    NIX_TOK_KW_IN,
    NIX_TOK_KW_WITH,
    NIX_TOK_KW_REC,
    NIX_TOK_KW_ASSERT,
    NIX_TOK_KW_OR,         /* attrset `or` default */
    NIX_TOK_KW_IMPORT,     /* import keyword */
} nix_tok_type_t;

typedef struct nix_token {
    nix_tok_type_t  type;
    char           *text;          /* heap-allocated text (ident, string, etc.) */
    int64_t         integer;       /* for NIX_TOK_INT */
    int             line;
    int             col;
} nix_token_t;

typedef struct nix_lexer nix_lexer_t;

/* Create a lexer for the given source text. */
nix_lexer_t *nix_lexer_new(const char *source, size_t len);
void         nix_lexer_free(nix_lexer_t *lex);

/* Get next token.  Returns NIX_TOK_EOF at end. */
nix_token_t  nix_lexer_next(nix_lexer_t *lex);

/* ── Parser ───────────────────────────────────────────────────────── */

typedef enum nix_node_type {
    NIX_NODE_ATTR_SET,     /* { key = value; ... } */
    NIX_NODE_REC_ATTR_SET, /* rec { ... } */
    NIX_NODE_LIST,         /* [ ... ] */
    NIX_NODE_STRING,       /* "..." with interpolation parts */
    NIX_NODE_IDENT,        /* identifier */
    NIX_NODE_SELECT,       /* a.b.c */
    NIX_NODE_APPLY,        /* f x */
    NIX_NODE_LAMBDA,       /* x: body */
    NIX_NODE_LET,          /* let ... in ... */
    NIX_NODE_IF,           /* if ... then ... else ... */
    NIX_NODE_WITH,         /* with ...; ... */
    NIX_NODE_ASSERT,       /* assert ...; ... */
    NIX_NODE_BINOP,        /* a + b, a ++ b, etc. */
    NIX_NODE_UNARY_NOT,    /* ! e */
    NIX_NODE_NEGATE,       /* - e (unary minus) */
    NIX_NODE_HAS_ATTR,     /* e ? attr */
    NIX_NODE_OR_DEFAULT,   /* e . attr or default */
    NIX_NODE_INT,          /* integer literal */
    NIX_NODE_BOOL,         /* bool literal */
    NIX_NODE_NULL,         /* null */
    NIX_NODE_PATH,         /* path literal */
    NIX_NODE_IMPORT,       /* import <path> — read and evaluate another .nix file */
} nix_node_type_t;

struct nix_ast {
    nix_node_type_t  type;
    int              line;
    int              col;
    union {
        struct {                        /* ATTR_SET, REC_ATTR_SET */
            struct {
                char      *key;
            nix_ast_t *value;
            }             *bindings;
            size_t          count;
        } attr_set;
        struct {                        /* LIST */
            nix_ast_t **items;
            size_t      count;
        } list;
        struct {                        /* STRING — parts are either
                                           literal text or ${expr} */
            struct {
                int       is_expr;     /* 0 = literal, 1 = interpolated */
                char     *text;        /* literal text if !is_expr */
            nix_ast_t *expr;        /* AST if is_expr */
            }            *parts;
            size_t        count;
        } string;
        char                *ident;     /* IDENT */
        struct {                        /* SELECT */
        nix_ast_t *expr;
            char      *attr;
        } select;
        struct {                        /* APPLY */
            nix_ast_t *func;
            nix_ast_t *arg;
        } apply;
        struct {                        /* LAMBDA */
            nix_ast_t *param;       /* IDENT or attr pattern */
            nix_ast_t *body;
        } lambda;
        struct {                        /* LET */
            struct {
                char      *name;
            nix_ast_t *value;
            }             *bindings;
            size_t         count;
            nix_ast_t  *body;
        } let;
        struct {                        /* IF */
            nix_ast_t *cond;
            nix_ast_t *then_expr;
            nix_ast_t *else_expr;
        } if_expr;
        struct {                        /* WITH */
            nix_ast_t *env_expr;
            nix_ast_t *body;
        } with_expr;
        struct {                        /* ASSERT */
            nix_ast_t *cond;
            nix_ast_t *body;
        } assert_expr;
        struct {                        /* BINOP */
            int          op;           /* NIX_TOK_PLUS etc. */
            nix_ast_t *left;
            nix_ast_t *right;
        } binop;
        nix_ast_t        *operand;      /* UNARY_NOT, NEGATE */
        struct {                        /* HAS_ATTR */
            nix_ast_t *expr;
            char      *attr;
        } has_attr;
        struct {                        /* OR_DEFAULT */
            nix_ast_t *expr;
            char      *attr;
            nix_ast_t *default_expr;
        } or_default;
        int64_t             integer;    /* INT */
        int                 boolean;    /* BOOL */
        char               *path;       /* PATH */
        struct {                        /* IMPORT */
            char      *path;       /* path to the .nix file */
        } import;
    };
};

/* Parse source text into an AST.  Returns NULL on parse error. */
nix_ast_t  *nix_parse(const char *source, size_t len, char **error);
void        nix_ast_free(nix_ast_t *ast);

/* ── Evaluator ────────────────────────────────────────────────────── */

/* Create a fresh evaluation environment with builtins registered. */
nix_env_t  *nix_env_new(void);
void        nix_env_free(nix_env_t *env);

/* Bind a name in the environment. */
int         nix_env_bind(nix_env_t *env, const char *name, nix_value_t *val);

/* Look up a name.  Returns NULL if not found. */
nix_value_t *nix_env_lookup(nix_env_t *env, const char *name);

/* Evaluate an AST in the given environment.  Returns the result value
   or NULL on error (sets *error). */
nix_value_t *nix_eval(nix_env_t *env, nix_ast_t *ast, char **error);

/* Force a thunk (lazy evaluation). */
nix_value_t *nix_force(nix_env_t *env, nix_value_t *val, char **error);

/* ── JSON output ──────────────────────────────────────────────────── */

/* Convert a Nix value to a JSON string.  Caller frees. */
char       *nix_to_json(nix_value_t *val);

/* ── Convenience: one-shot eval ───────────────────────────────────── */

/* Evaluate a Nix source string and return the JSON manifest.
   Caller frees the returned string.  On error, returns NULL and
   sets *error. */
char       *nix_eval_file(const char *source, size_t len, char **error);

/* Evaluate with a base directory for resolving import paths.
   base_dir is the directory of the importing file (for relative paths).
   If base_dir is NULL, uses the current working directory. */
char       *nix_eval_file_with_base(const char *source, size_t len,
                                     const char *base_dir, char **error);

/* ── Memory ───────────────────────────────────────────────────────── */

void        nix_value_free(nix_value_t *val);

/* Allocate a value of the given type.  Returns zero-initialized value. */
nix_value_t *nix_value_new(nix_val_type_t type);

#endif /* TWO09_NIX_EVAL_H */
