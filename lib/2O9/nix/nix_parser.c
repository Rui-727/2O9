/* nix_parser.c — Nix expression language parser
 *
 * Recursive-descent parser producing an AST from token stream.
 * Grammar (loosest to tightest binding):
 *
 *   expr        → if | assert | with | let | lambda | imp_expr
 *   imp_expr    → or_expr ('->' imp_expr)?         right-assoc
 *   or_expr     → and_expr ('||' and_expr)*         left-assoc
 *   and_expr    → eq_expr ('&&' eq_expr)*           left-assoc
 *   eq_expr     → cmp_expr (('=='|'!=') cmp_expr)*  left-assoc
 *   cmp_expr    → concat_expr (('<'|'<='|'>'|'>=') concat_expr)*  left-assoc
 *   concat_expr → add_expr ('++' add_expr)*          right-assoc
 *   add_expr    → mul_expr (('+'|'-') mul_expr)*    left-assoc
 *   mul_expr    → unary (('*'|'/') unary)*          left-assoc
 *   unary       → '!' unary | '-' unary | select
 *   select      → apply ('.' ident)* ('or' default)?
 *   apply       → base (base)*                      left-assoc
 *   base        → '(' expr ')' | attr_set | rec_attr_set | list |
 *                 string | path | ident | int | bool | null | import
 *   attr_set    → '{' bind* '}'
 *   list        → '[' expr* ']'
 *   lambda      → '{' formals '}' ':' expr | ident ':' expr
 *   let_expr    → 'let' bind+ 'in' expr
 *   if_expr     → 'if' expr 'then' expr 'else' expr
 *   with_expr   → 'with' expr ';' expr
 *   assert_expr → 'assert' expr ';' expr
 *   import_expr → 'import' path
 *   bind        → attrpath '=' expr ';' | 'inherit' ('(' expr ')')? idents ';'
 *
 * Part of lib2O9. Pure C, no C++ dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nix_eval.h"

/* ── Parser state ──────────────────────────────────────────────────── */

typedef struct nix_parser {
    nix_lexer_t *lex;
    nix_token_t  cur;          /* current token (lookahead) */
    char       **error;        /* error output pointer */
    int          has_error;
} nix_parser_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void parser_error(nix_parser_t *p, const char *msg)
{
    if (p->has_error) return;
    p->has_error = 1;
    if (p->error) {
        char buf[512];
        snprintf(buf, sizeof(buf), "line %d col %d: %s", p->cur.line, p->cur.col, msg);
        free(*p->error);  /* free any previous error */
        *p->error = strdup(buf);
    }
}

static void advance(nix_parser_t *p)
{
    if (p->cur.text) { free(p->cur.text); p->cur.text = NULL; }
    p->cur = nix_lexer_next(p->lex);
}

static nix_tok_type_t peek_type(nix_parser_t *p)
{
    return p->cur.type;
}

static int expect(nix_parser_t *p, nix_tok_type_t type, const char *msg)
{
    if (peek_type(p) == type) {
        advance(p);
        return 1;
    }
    parser_error(p, msg);
    return 0;
}

/* ── AST allocation ───────────────────────────────────────────────── */

static nix_ast_t *ast_new(nix_node_type_t type, int line, int col)
{
    nix_ast_t *n = calloc(1, sizeof(*n));
    if (n) { n->type = type; n->line = line; n->col = col; }
    return n;
}

/* ── Forward declarations ─────────────────────────────────────────── */

static nix_ast_t *parse_expr(nix_parser_t *p);
static nix_ast_t *parse_apply(nix_parser_t *p);
static nix_ast_t *parse_select(nix_parser_t *p);

/* ── Parse attrpath: a.b.c (for bindings) ────────────────────────── */

typedef struct {
    char **parts;
    size_t count;
} attr_path_t;

static attr_path_t parse_attrpath(nix_parser_t *p)
{
    attr_path_t ap = { .parts = NULL, .count = 0 };
    size_t cap = 8;
    ap.parts = calloc(cap, sizeof(char *));

    /* First part: identifier */
    if (peek_type(p) != NIX_TOK_IDENT) {
        parser_error(p, "expected identifier in attrpath");
        return ap;
    }
    ap.parts[ap.count++] = strdup(p->cur.text);
    advance(p);

    /* Additional parts: . identifier */
    while (peek_type(p) == NIX_TOK_DOT) {
        advance(p);
        if (peek_type(p) != NIX_TOK_IDENT) {
            parser_error(p, "expected identifier after '.'");
            break;
        }
        if (ap.count >= cap) { cap *= 2; ap.parts = realloc(ap.parts, cap * sizeof(char *)); }
        ap.parts[ap.count++] = strdup(p->cur.text);
        advance(p);
    }

    return ap;
}

static void attrpath_free(attr_path_t *ap)
{
    for (size_t i = 0; i < ap->count; i++) free(ap->parts[i]);
    free(ap->parts);
}

/* ── Parse binding: attrpath = expr ; ────────────────────────────── */

typedef struct nix_bind {
    attr_path_t  path;
    nix_ast_t   *value;
} nix_bind_t;

static nix_bind_t *parse_bind(nix_parser_t *p, size_t *count, size_t *cap, nix_bind_t *binds)
{
    /* inherit? */
    if (peek_type(p) == NIX_TOK_IDENT && p->cur.text &&
        strcmp(p->cur.text, "inherit") == 0) {
        advance(p); /* consume 'inherit' */

        /* TODO: inherit (expr) idents ; — for now just idents */
        while (peek_type(p) == NIX_TOK_IDENT) {
            if (*count >= *cap) { *cap *= 2; binds = realloc(binds, *cap * sizeof(nix_bind_t)); }
            attr_path_t ap = { .parts = NULL, .count = 1 };
            ap.parts = calloc(1, sizeof(char *));
            ap.parts[0] = strdup(p->cur.text);
            binds[*count].path = ap;
            binds[*count].value = ast_new(NIX_NODE_IDENT, p->cur.line, p->cur.col);
            binds[*count].value->ident = strdup(p->cur.text);
            (*count)++;
            advance(p);
        }
        expect(p, NIX_TOK_SEMICOLON, "expected ';' after inherit");
        return binds;
    }

    /* Regular binding: attrpath = expr ; */
    attr_path_t ap = parse_attrpath(p);
    if (p->has_error) { attrpath_free(&ap); return binds; }

    expect(p, NIX_TOK_EQUALS, "expected '=' in binding");
    if (p->has_error) { attrpath_free(&ap); return binds; }

    nix_ast_t *val = parse_expr(p);
    if (p->has_error) { attrpath_free(&ap); return binds; }

    expect(p, NIX_TOK_SEMICOLON, "expected ';' after binding");
    if (p->has_error) { attrpath_free(&ap); nix_ast_free(val); return binds; }

    if (*count >= *cap) { *cap *= 2; binds = realloc(binds, *cap * sizeof(nix_bind_t)); }
    binds[*count].path = ap;
    binds[*count].value = val;
    (*count)++;
    return binds;
}

/* ── Parse string with interpolation ──────────────────────────────── */

static nix_ast_t *parse_string(nix_parser_t *p, const char *text)
{
    nix_ast_t *node = ast_new(NIX_NODE_STRING, p->cur.line, p->cur.col);
    if (!text) { node->string.parts = NULL; node->string.count = 0; return node; }

    size_t cap = 4;
    node->string.parts = calloc(cap, sizeof(*node->string.parts));
    node->string.count = 0;

    size_t i = 0;
    size_t len = strlen(text);

    while (i < len) {
        if (text[i] == '$' && i + 1 < len && text[i + 1] == '{') {
            /* Interpolation start — find matching } */
            size_t start = i + 2;
            int depth = 1;
            size_t j = start;
            while (j < len && depth > 0) {
                if (text[j] == '{') depth++;
                else if (text[j] == '}') depth--;
                j++;
            }
            /* Store the interpolation expression text */
            if (node->string.count >= cap) { cap *= 2; node->string.parts = realloc(node->string.parts, cap * sizeof(*node->string.parts)); }
            size_t expr_len = j - start - 1;
            char *expr_text = calloc(1, expr_len + 1);
            memcpy(expr_text, text + start, expr_len);
            node->string.parts[node->string.count].is_expr = 1;
            node->string.parts[node->string.count].text = NULL;
            /* Parse the interpolation expression */
            nix_parser_t sub;
            nix_lexer_t *sublex = nix_lexer_new(expr_text, expr_len);
            sub.lex = sublex;
            sub.cur = nix_lexer_next(sublex);
            sub.error = p->error;
            sub.has_error = 0;
            node->string.parts[node->string.count].expr = parse_expr(&sub);
            nix_lexer_free(sublex);
            free(expr_text);
            node->string.count++;
            i = j;
        } else {
            /* Literal text until next ${ or end */
            size_t start = i;
            while (i < len && !(text[i] == '$' && i + 1 < len && text[i + 1] == '{')) i++;
            size_t lit_len = i - start;
            char *lit = calloc(1, lit_len + 1);
            memcpy(lit, text + start, lit_len);
            if (node->string.count >= cap) { cap *= 2; node->string.parts = realloc(node->string.parts, cap * sizeof(*node->string.parts)); }
            node->string.parts[node->string.count].is_expr = 0;
            node->string.parts[node->string.count].text = lit;
            node->string.parts[node->string.count].expr = NULL;
            node->string.count++;
        }
    }

    return node;
}

/* ── Parse base expressions ──────────────────────────────────────── */

static nix_ast_t *parse_base(nix_parser_t *p)
{
    nix_token_t tok = p->cur;

    switch (tok.type) {
    case NIX_TOK_LPAREN: {
        advance(p);
        nix_ast_t *inner = parse_expr(p);
        expect(p, NIX_TOK_RPAREN, "expected ')'");
        return inner;
    }

    case NIX_TOK_LBRACE: {
        /* Distinguish between attrset { a = 1; b = 2; } and
         * lambda formals { a, b ? default, ... }: body.
         *
         * Strategy: consume first identifier normally (via advance),
         * then check what follows:
         *   - ',' or '?' or '}'  →  formals (lambda param)
         *   - '=' or '.'         →  binding (attrset)
         */
        int line = tok.line, col = tok.col;
        advance(p); /* consume '{' */

        /* Empty braces: { } */
        if (peek_type(p) == NIX_TOK_RBRACE) {
            advance(p);
            /* { } : body → lambda with empty formals */
            if (peek_type(p) == NIX_TOK_COLON) {
                advance(p);
                nix_ast_t *body = parse_expr(p);
                nix_ast_t *lambda = ast_new(NIX_NODE_LAMBDA, line, col);
                lambda->lambda.param = ast_new(NIX_NODE_ATTR_SET, line, col);
                lambda->lambda.param->attr_set.count = 0;
                lambda->lambda.body = body;
                return lambda;
            }
            /* Plain empty attrset */
            nix_ast_t *node = ast_new(NIX_NODE_ATTR_SET, line, col);
            node->attr_set.count = 0;
            return node;
        }

        /* Check for ellipsis at start: { ... } */
        if (peek_type(p) == NIX_TOK_DOT) {
            /* Could be ... — consume the first dot and check */
            advance(p); /* consume first dot */
            if (peek_type(p) == NIX_TOK_DOT) {
                advance(p); /* second dot */
                if (peek_type(p) == NIX_TOK_DOT) {
                    advance(p); /* third dot — it's ... */
                    /* Parse rest as formals starting with ellipsis */
                    nix_ast_t *formals = ast_new(NIX_NODE_ATTR_SET, line, col);
                    size_t fcap = 8, fcount = 0;
                    formals->attr_set.bindings = calloc(fcap, sizeof(*formals->attr_set.bindings));
                    if (peek_type(p) == NIX_TOK_COMMA) advance(p);
                    /* Parse remaining formals */
                    while (peek_type(p) != NIX_TOK_RBRACE && peek_type(p) != NIX_TOK_EOF) {
                        if (peek_type(p) != NIX_TOK_IDENT) {
                            parser_error(p, "expected identifier in formals");
                            break;
                        }
                        if (fcount >= fcap) { fcap *= 2; formals->attr_set.bindings = realloc(formals->attr_set.bindings, fcap * sizeof(*formals->attr_set.bindings)); }
                        formals->attr_set.bindings[fcount].key = strdup(p->cur.text);
                        advance(p);
                        if (peek_type(p) == NIX_TOK_QUESTION) {
                            advance(p);
                            formals->attr_set.bindings[fcount].value = parse_expr(p);
                        } else {
                            formals->attr_set.bindings[fcount].value = ast_new(NIX_NODE_NULL, 0, 0);
                        }
                        fcount++;
                        if (peek_type(p) == NIX_TOK_COMMA) advance(p);
                    }
                    formals->attr_set.count = fcount;
                    expect(p, NIX_TOK_RBRACE, "expected '}'");
                    expect(p, NIX_TOK_COLON, "expected ':' after formals");
                    nix_ast_t *body = parse_expr(p);
                    nix_ast_t *lambda = ast_new(NIX_NODE_LAMBDA, line, col);
                    lambda->lambda.param = formals;
                    lambda->lambda.body = body;
                    return lambda;
                }
            }
            /* Not ellipsis — error: unexpected dot */
            parser_error(p, "unexpected '.' in attrset");
            return NULL;
        }

        /* First item must be an identifier */
        if (peek_type(p) != NIX_TOK_IDENT) {
            parser_error(p, "expected identifier or '}' in attrset");
            return NULL;
        }

        /* Consume the first identifier */
        char *first_name = strdup(p->cur.text);
        advance(p); /* consume ident — now p->cur is the token after it */

        /* Decide: formals or bindings? */
        nix_tok_type_t after_first = peek_type(p);

        if (after_first == NIX_TOK_COMMA || after_first == NIX_TOK_QUESTION ||
            after_first == NIX_TOK_RBRACE) {
            /* ── FORMALS MODE ────────────────────────────────────────── */
            nix_ast_t *formals = ast_new(NIX_NODE_ATTR_SET, line, col);
            size_t fcap = 8, fcount = 0;
            formals->attr_set.bindings = calloc(fcap, sizeof(*formals->attr_set.bindings));

            /* First formal */
            formals->attr_set.bindings[fcount].key = first_name;
            first_name = NULL; /* ownership transferred */
            if (after_first == NIX_TOK_QUESTION) {
                advance(p); /* consume '?' */
                formals->attr_set.bindings[fcount].value = parse_expr(p);
            } else {
                formals->attr_set.bindings[fcount].value = ast_new(NIX_NODE_NULL, 0, 0);
            }
            fcount++;

            if (peek_type(p) == NIX_TOK_COMMA) advance(p);

            /* Parse remaining formals */
            while (peek_type(p) != NIX_TOK_RBRACE && peek_type(p) != NIX_TOK_EOF) {
                /* Ellipsis: three dots */
                if (peek_type(p) == NIX_TOK_DOT) {
                    advance(p); /* first dot */
                    if (peek_type(p) == NIX_TOK_DOT) {
                        advance(p); /* second dot */
                        if (peek_type(p) == NIX_TOK_DOT) {
                            advance(p); /* third dot */
                            if (peek_type(p) == NIX_TOK_COMMA) advance(p);
                            continue;
                        }
                    }
                    parser_error(p, "unexpected '.' in formals (did you mean '...'?)");
                    break;
                }

                if (peek_type(p) != NIX_TOK_IDENT) {
                    parser_error(p, "expected identifier in formals");
                    break;
                }

                if (fcount >= fcap) { fcap *= 2; formals->attr_set.bindings = realloc(formals->attr_set.bindings, fcap * sizeof(*formals->attr_set.bindings)); }
                formals->attr_set.bindings[fcount].key = strdup(p->cur.text);
                advance(p);

                if (peek_type(p) == NIX_TOK_QUESTION) {
                    advance(p);
                    formals->attr_set.bindings[fcount].value = parse_expr(p);
                } else {
                    formals->attr_set.bindings[fcount].value = ast_new(NIX_NODE_NULL, 0, 0);
                }
                fcount++;

                if (peek_type(p) == NIX_TOK_COMMA) advance(p);
            }
            formals->attr_set.count = fcount;

            expect(p, NIX_TOK_RBRACE, "expected '}'");
            expect(p, NIX_TOK_COLON, "expected ':' after formals");

            nix_ast_t *body = parse_expr(p);
            nix_ast_t *lambda = ast_new(NIX_NODE_LAMBDA, line, col);
            lambda->lambda.param = formals;
            lambda->lambda.body = body;
            return lambda;
        }

        /* ── BINDINGS MODE ──────────────────────────────────────────── */
        /* We already consumed the first identifier. Build the first binding
         * manually, then continue with parse_bind for the rest. */
        {
            size_t cap = 8, count = 0;
            nix_bind_t *binds = calloc(cap, sizeof(nix_bind_t));

            /* Build attrpath from first_name + any .ident continuations */
            attr_path_t ap;
            size_t ap_cap = 8;
            ap.parts = calloc(ap_cap, sizeof(char *));
            ap.count = 1;
            ap.parts[0] = first_name;
            first_name = NULL; /* ownership transferred */

            while (peek_type(p) == NIX_TOK_DOT) {
                advance(p);
                if (peek_type(p) != NIX_TOK_IDENT) {
                    parser_error(p, "expected identifier after '.'");
                    attrpath_free(&ap);
                    free(binds);
                    return NULL;
                }
                if (ap.count >= ap_cap) { ap_cap *= 2; ap.parts = realloc(ap.parts, ap_cap * sizeof(char *)); }
                ap.parts[ap.count++] = strdup(p->cur.text);
                advance(p);
            }

            expect(p, NIX_TOK_EQUALS, "expected '=' in binding");
            if (p->has_error) { attrpath_free(&ap); free(binds); return NULL; }

            nix_ast_t *val = parse_expr(p);
            if (p->has_error) { attrpath_free(&ap); free(binds); return NULL; }

            expect(p, NIX_TOK_SEMICOLON, "expected ';' after binding");
            if (p->has_error) { attrpath_free(&ap); nix_ast_free(val); free(binds); return NULL; }

            binds[count].path = ap;
            binds[count].value = val;
            count++;

            /* Parse remaining bindings */
            while (peek_type(p) != NIX_TOK_RBRACE && peek_type(p) != NIX_TOK_EOF) {
                binds = parse_bind(p, &count, &cap, binds);
                if (p->has_error) {
                    for (size_t i = 0; i < count; i++) { attrpath_free(&binds[i].path); nix_ast_free(binds[i].value); }
                    free(binds);
                    return NULL;
                }
            }

            expect(p, NIX_TOK_RBRACE, "expected '}'");

            /* Check if this is followed by ':' — making it a lambda */
            if (peek_type(p) == NIX_TOK_COLON) {
                advance(p); /* consume : */
                nix_ast_t *body = parse_expr(p);
                nix_ast_t *lambda = ast_new(NIX_NODE_LAMBDA, line, col);
                lambda->lambda.param = ast_new(NIX_NODE_ATTR_SET, line, col);
                lambda->lambda.param->attr_set.count = count;
                lambda->lambda.param->attr_set.bindings = calloc(count, sizeof(*lambda->lambda.param->attr_set.bindings));
                for (size_t i = 0; i < count; i++) {
                    lambda->lambda.param->attr_set.bindings[i].key = binds[i].path.parts[0];
                    binds[i].path.parts[0] = NULL;
                    lambda->lambda.param->attr_set.bindings[i].value = binds[i].value;
                    binds[i].value = NULL;
                    for (size_t j = 1; j < binds[i].path.count; j++) free(binds[i].path.parts[j]);
                    free(binds[i].path.parts);
                }
                free(binds);
                lambda->lambda.body = body;
                return lambda;
            }

            /* Regular attrset */
            nix_ast_t *node = ast_new(NIX_NODE_ATTR_SET, line, col);
            node->attr_set.count = count;
            node->attr_set.bindings = calloc(count, sizeof(*node->attr_set.bindings));
            for (size_t i = 0; i < count; i++) {
                if (binds[i].path.count == 1) {
                    node->attr_set.bindings[i].key = binds[i].path.parts[0];
                    binds[i].path.parts[0] = NULL;
                    node->attr_set.bindings[i].value = binds[i].value;
                    binds[i].value = NULL;
                    free(binds[i].path.parts);
                } else {
                    size_t klen = 0;
                    for (size_t j = 0; j < binds[i].path.count; j++)
                        klen += strlen(binds[i].path.parts[j]) + 1;
                    char *key = calloc(1, klen);
                    for (size_t j = 0; j < binds[i].path.count; j++) {
                        if (j > 0) strcat(key, ".");
                        strcat(key, binds[i].path.parts[j]);
                    }
                    node->attr_set.bindings[i].key = key;
                    node->attr_set.bindings[i].value = binds[i].value;
                    binds[i].value = NULL;
                    attrpath_free(&binds[i].path);
                }
            }
            free(binds);
            return node;
        }
    }

    case NIX_TOK_LBRACKET: {
        int line = tok.line, col = tok.col;
        advance(p);
        size_t cap = 8, count = 0;
        nix_ast_t **items = calloc(cap, sizeof(nix_ast_t *));

        while (peek_type(p) != NIX_TOK_RBRACKET && peek_type(p) != NIX_TOK_EOF) {
            if (count >= cap) { cap *= 2; items = realloc(items, cap * sizeof(nix_ast_t *)); }
            /* List items are select expressions (no function application or
             * binary operators). Use parens for full expressions: [(1 + 2)]. */
            items[count++] = parse_select(p);
            if (p->has_error) {
                for (size_t i = 0; i < count; i++) nix_ast_free(items[i]);
                free(items);
                return NULL;
            }
        }

        expect(p, NIX_TOK_RBRACKET, "expected ']'");
        nix_ast_t *node = ast_new(NIX_NODE_LIST, line, col);
        node->list.items = items;
        node->list.count = count;
        return node;
    }

    case NIX_TOK_STRING: {
        /* Save text before advancing — advance frees p->cur.text */
        char *str_text = tok.text ? strdup(tok.text) : NULL;
        advance(p);
        nix_ast_t *result = parse_string(p, str_text);
        free(str_text);
        return result;
    }

    case NIX_TOK_PATH: {
        nix_ast_t *node = ast_new(NIX_NODE_PATH, tok.line, tok.col);
        node->path = strdup(tok.text);
        advance(p);
        return node;
    }

    case NIX_TOK_IDENT: {
        /* Could be: plain identifier, or start of lambda (ident ':' expr) */
        int line = tok.line, col = tok.col;
        char *name = strdup(tok.text);
        advance(p);

        /* Lambda: ident ':' expr */
        if (peek_type(p) == NIX_TOK_COLON) {
            advance(p);
            nix_ast_t *body = parse_expr(p);
            nix_ast_t *node = ast_new(NIX_NODE_LAMBDA, line, col);
            node->lambda.param = ast_new(NIX_NODE_IDENT, line, col);
            node->lambda.param->ident = name;
            node->lambda.body = body;
            return node;
        }

        /* Import: 'import' path */
        if (strcmp(name, "import") == 0 && peek_type(p) == NIX_TOK_PATH) {
            nix_ast_t *node = ast_new(NIX_NODE_IMPORT, line, col);
            node->import.path = strdup(p->cur.text);
            advance(p);
            free(name);
            return node;
        }

        /* Boolean literals */
        if (strcmp(name, "true") == 0) {
            nix_ast_t *node = ast_new(NIX_NODE_BOOL, line, col);
            node->boolean = 1;
            free(name);
            return node;
        }
        if (strcmp(name, "false") == 0) {
            nix_ast_t *node = ast_new(NIX_NODE_BOOL, line, col);
            node->boolean = 0;
            free(name);
            return node;
        }
        if (strcmp(name, "null") == 0) {
            nix_ast_t *node = ast_new(NIX_NODE_NULL, line, col);
            free(name);
            return node;
        }

        /* Plain identifier */
        nix_ast_t *node = ast_new(NIX_NODE_IDENT, line, col);
        node->ident = name;
        return node;
    }

    case NIX_TOK_INT: {
        nix_ast_t *node = ast_new(NIX_NODE_INT, tok.line, tok.col);
        node->integer = tok.integer;
        advance(p);
        return node;
    }

    case NIX_TOK_KW_REC: {
        int line = tok.line, col = tok.col;
        advance(p);
        /* rec { ... } — recursive attrset */
        expect(p, NIX_TOK_LBRACE, "expected '{' after 'rec'");
        if (p->has_error) return NULL;

        size_t cap = 8, count = 0;
        nix_bind_t *binds = calloc(cap, sizeof(nix_bind_t));
        while (peek_type(p) != NIX_TOK_RBRACE && peek_type(p) != NIX_TOK_EOF) {
            binds = parse_bind(p, &count, &cap, binds);
            if (p->has_error) {
                for (size_t i = 0; i < count; i++) attrpath_free(&binds[i].path);
                free(binds);
                return NULL;
            }
        }
        expect(p, NIX_TOK_RBRACE, "expected '}'");

        nix_ast_t *node = ast_new(NIX_NODE_REC_ATTR_SET, line, col);
        node->attr_set.count = count;
        node->attr_set.bindings = calloc(count, sizeof(*node->attr_set.bindings));
        for (size_t i = 0; i < count; i++) {
            node->attr_set.bindings[i].key = binds[i].path.count == 1 ? binds[i].path.parts[0] : NULL;
            if (binds[i].path.count == 1) binds[i].path.parts[0] = NULL;
            node->attr_set.bindings[i].value = binds[i].value;
            binds[i].value = NULL;
            attrpath_free(&binds[i].path);
        }
        free(binds);
        return node;
    }

    default:
        parser_error(p, "unexpected token");
        return NULL;
    }
}

/* ── Parse select: base . attr (. attr)* (or default)? ──────────── */
/* NOTE: select calls parse_base (NOT parse_apply), so that in list
 * contexts we can use parse_select to get individual items without
 * function application consuming subsequent items.                    */

static nix_ast_t *parse_select(nix_parser_t *p)
{
    nix_ast_t *expr = parse_base(p);
    if (!expr || p->has_error) return expr;

    while (peek_type(p) == NIX_TOK_DOT) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);

        if (peek_type(p) != NIX_TOK_IDENT) {
            parser_error(p, "expected attribute name after '.'");
            nix_ast_free(expr);
            return NULL;
        }

        nix_ast_t *node = ast_new(NIX_NODE_SELECT, line, col);
        node->select.expr = expr;
        node->select.attr = strdup(p->cur.text);
        advance(p);

        /* or default */
        if (peek_type(p) == NIX_TOK_KW_OR) {
            advance(p);
            node->type = NIX_NODE_OR_DEFAULT;
            node->or_default.expr = node->select.expr;
            node->or_default.attr = node->select.attr;
            node->or_default.default_expr = parse_select(p);
        }

        expr = node;
    }

    return expr;
}

/* ── Parse function application: select (select)* ───────────────── */
/* Function application is left-associative: f x y = ((f x) y).
 * Each argument is a select expression, not a full expression.       */

static nix_ast_t *parse_apply(nix_parser_t *p)
{
    nix_ast_t *func = parse_select(p);
    if (!func || p->has_error) return func;

    /* Apply while next token could start a select expression.
     * Note: '-' and '!' are NOT valid starts for function arguments
     * because they are unary operators handled at a higher precedence level.
     * IDENT is valid (bare identifier as argument).                      */
    while (!p->has_error) {
        nix_tok_type_t t = peek_type(p);
        if (t == NIX_TOK_IDENT || t == NIX_TOK_INT || t == NIX_TOK_STRING ||
            t == NIX_TOK_PATH || t == NIX_TOK_LPAREN || t == NIX_TOK_LBRACE ||
            t == NIX_TOK_LBRACKET || t == NIX_TOK_KW_REC) {
            nix_ast_t *arg = parse_select(p);
            if (!arg) break;
            nix_ast_t *node = ast_new(NIX_NODE_APPLY, func->line, func->col);
            node->apply.func = func;
            node->apply.arg = arg;
            func = node;
        } else {
            break;
        }
    }

    return func;
}

/* ── Binary operator parsing with precedence ──────────────────────── */

/* Helper: parse a left-associative binary operator level */
static nix_ast_t *parse_binop_left(nix_parser_t *p,
                                    nix_ast_t *(*next_level)(nix_parser_t *),
                                    const nix_tok_type_t *ops, int op_count)
{
    nix_ast_t *left = next_level(p);
    if (!left || p->has_error) return left;

    while (!p->has_error) {
        nix_tok_type_t t = peek_type(p);
        int found = 0;
        for (int i = 0; i < op_count; i++) {
            if (t == ops[i]) { found = 1; break; }
        }
        if (!found) break;

        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *right = next_level(p);
        if (!right) { nix_ast_free(left); return NULL; }

        nix_ast_t *node = ast_new(NIX_NODE_BINOP, line, col);
        node->binop.op = t;
        node->binop.left = left;
        node->binop.right = right;
        left = node;
    }

    return left;
}

/* Helper: parse a right-associative binary operator level */
static nix_ast_t *parse_binop_right(nix_parser_t *p,
                                     nix_ast_t *(*next_level)(nix_parser_t *),
                                     nix_tok_type_t op)
{
    nix_ast_t *left = next_level(p);
    if (!left || p->has_error) return left;

    if (peek_type(p) == op) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *right = parse_binop_right(p, next_level, op);
        if (!right) { nix_ast_free(left); return NULL; }

        nix_ast_t *node = ast_new(NIX_NODE_BINOP, line, col);
        node->binop.op = op;
        node->binop.left = left;
        node->binop.right = right;
        return node;
    }

    return left;
}

/* Forward declarations for precedence levels (loosest to tightest) */
static nix_ast_t *parse_implication(nix_parser_t *p);
static nix_ast_t *parse_or(nix_parser_t *p);
static nix_ast_t *parse_and(nix_parser_t *p);
static nix_ast_t *parse_equality(nix_parser_t *p);
static nix_ast_t *parse_comparison(nix_parser_t *p);
static nix_ast_t *parse_concat(nix_parser_t *p);
static nix_ast_t *parse_add(nix_parser_t *p);
static nix_ast_t *parse_mul(nix_parser_t *p);
static nix_ast_t *parse_unary(nix_parser_t *p);

/* -> : right-associative */
static nix_ast_t *parse_implication(nix_parser_t *p)
{
    return parse_binop_right(p, parse_or, NIX_TOK_ARROW);
}

/* || : left-associative */
static nix_ast_t *parse_or(nix_parser_t *p)
{
    static const nix_tok_type_t ops[] = { NIX_TOK_OR };
    return parse_binop_left(p, parse_and, ops, 1);
}

/* && : left-associative */
static nix_ast_t *parse_and(nix_parser_t *p)
{
    static const nix_tok_type_t ops[] = { NIX_TOK_AND };
    return parse_binop_left(p, parse_equality, ops, 1);
}

/* == != : left-associative */
static nix_ast_t *parse_equality(nix_parser_t *p)
{
    static const nix_tok_type_t ops[] = { NIX_TOK_EQ, NIX_TOK_NEQ };
    return parse_binop_left(p, parse_comparison, ops, 2);
}

/* < <= > >= : left-associative */
static nix_ast_t *parse_comparison(nix_parser_t *p)
{
    static const nix_tok_type_t ops[] = { NIX_TOK_LT, NIX_TOK_LE, NIX_TOK_GT, NIX_TOK_GE };
    return parse_binop_left(p, parse_concat, ops, 4);
}

/* ++ : right-associative (list concatenation) */
static nix_ast_t *parse_concat(nix_parser_t *p)
{
    return parse_binop_right(p, parse_add, NIX_TOK_PLUS_PLUS);
}

/* + - : left-associative */
static nix_ast_t *parse_add(nix_parser_t *p)
{
    static const nix_tok_type_t ops[] = { NIX_TOK_PLUS, NIX_TOK_MINUS };
    return parse_binop_left(p, parse_mul, ops, 2);
}

/* * / : left-associative */
static nix_ast_t *parse_mul(nix_parser_t *p)
{
    static const nix_tok_type_t ops[] = { NIX_TOK_STAR, NIX_TOK_SLASH };
    return parse_binop_left(p, parse_unary, ops, 2);
}

/* unary: !expr | -expr | apply */
static nix_ast_t *parse_unary(nix_parser_t *p)
{
    if (peek_type(p) == NIX_TOK_NOT) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *operand = parse_unary(p);
        if (!operand) return NULL;
        nix_ast_t *node = ast_new(NIX_NODE_UNARY_NOT, line, col);
        node->operand = operand;
        return node;
    }

    if (peek_type(p) == NIX_TOK_MINUS) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *operand = parse_unary(p);
        if (!operand) return NULL;
        nix_ast_t *node = ast_new(NIX_NODE_NEGATE, line, col);
        node->operand = operand;
        return node;
    }

    return parse_apply(p);
}

/* ── Parse expression (top-level) ────────────────────────────────── */

static nix_ast_t *parse_expr(nix_parser_t *p)
{
    if (p->has_error) return NULL;

    nix_tok_type_t t = peek_type(p);

    /* if expr then expr else expr */
    if (t == NIX_TOK_KW_IF) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *cond = parse_expr(p);
        expect(p, NIX_TOK_KW_THEN, "expected 'then'");
        nix_ast_t *then_expr = parse_expr(p);
        expect(p, NIX_TOK_KW_ELSE, "expected 'else'");
        nix_ast_t *else_expr = parse_expr(p);
        nix_ast_t *node = ast_new(NIX_NODE_IF, line, col);
        node->if_expr.cond = cond;
        node->if_expr.then_expr = then_expr;
        node->if_expr.else_expr = else_expr;
        return node;
    }

    /* assert expr ; expr */
    if (t == NIX_TOK_KW_ASSERT) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *cond = parse_expr(p);
        expect(p, NIX_TOK_SEMICOLON, "expected ';' after assert");
        nix_ast_t *body = parse_expr(p);
        nix_ast_t *node = ast_new(NIX_NODE_ASSERT, line, col);
        node->assert_expr.cond = cond;
        node->assert_expr.body = body;
        return node;
    }

    /* with expr ; expr */
    if (t == NIX_TOK_KW_WITH) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        nix_ast_t *env_expr = parse_expr(p);
        expect(p, NIX_TOK_SEMICOLON, "expected ';' after with");
        nix_ast_t *body = parse_expr(p);
        nix_ast_t *node = ast_new(NIX_NODE_WITH, line, col);
        node->with_expr.env_expr = env_expr;
        node->with_expr.body = body;
        return node;
    }

    /* let bindings in expr */
    if (t == NIX_TOK_KW_LET) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);

        size_t cap = 8, count = 0;
        nix_bind_t *binds = calloc(cap, sizeof(nix_bind_t));
        while (peek_type(p) != NIX_TOK_KW_IN && peek_type(p) != NIX_TOK_EOF) {
            binds = parse_bind(p, &count, &cap, binds);
            if (p->has_error) {
                for (size_t i = 0; i < count; i++) { attrpath_free(&binds[i].path); nix_ast_free(binds[i].value); }
                free(binds);
                return NULL;
            }
        }

        expect(p, NIX_TOK_KW_IN, "expected 'in' after let bindings");
        nix_ast_t *body = parse_expr(p);

        nix_ast_t *node = ast_new(NIX_NODE_LET, line, col);
        node->let.count = count;
        node->let.bindings = calloc(count, sizeof(*node->let.bindings));
        for (size_t i = 0; i < count; i++) {
            /* Flatten to single key for let bindings */
            node->let.bindings[i].name = binds[i].path.count == 1 ? binds[i].path.parts[0] : NULL;
            if (binds[i].path.count == 1) binds[i].path.parts[0] = NULL;
            node->let.bindings[i].value = binds[i].value;
            binds[i].value = NULL;
            attrpath_free(&binds[i].path);
        }
        free(binds);
        node->let.body = body;
        return node;
    }

    /* Otherwise: binary ops (precedence chain) */
    return parse_implication(p);
}

/* ── Public API ──────────────────────────────────────────────────── */

nix_ast_t *nix_parse(const char *source, size_t len, char **error)
{
    nix_parser_t p;
    p.lex = nix_lexer_new(source, len);
    if (!p.lex) {
        if (error) *error = strdup("failed to create lexer");
        return NULL;
    }
    p.cur = nix_lexer_next(p.lex);
    p.error = error;
    p.has_error = 0;

    nix_ast_t *ast = parse_expr(&p);

    if (!p.has_error && peek_type(&p) != NIX_TOK_EOF) {
        parser_error(&p, "unexpected token after expression");
        if (ast) { nix_ast_free(ast); ast = NULL; }
    }

    if (p.cur.text) free(p.cur.text);
    nix_lexer_free(p.lex);

    return ast;
}
