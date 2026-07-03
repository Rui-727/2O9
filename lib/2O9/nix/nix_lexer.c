/* nix_lexer.c - Nix expression language lexer
 *
 * Tokenizes Nix source into tokens for the parser. Supports:
 * - Identifiers and keywords (let, in, if, then, else, with, rec, assert, import, or)
 * - Integer literals
 * - String literals with interpolation ("hello ${expr}")
 * - Path literals (/absolute/path, ./relative/path)
 * - All operators and delimiters
 * - Line comments (#) and block comments (/* *​/)
 * - Whitespace skipping
 *
 * Part of lib2O9. Pure C, no C++ dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "nix_eval.h"

/* ── Lexer state ──────────────────────────────────────────────────── */

struct nix_lexer {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line;
    int         col;
};

/* ── Keyword table ────────────────────────────────────────────────── */

typedef struct {
    const char     *word;
    nix_tok_type_t  type;
} keyword_t;

static const keyword_t keywords[] = {
    { "if",     NIX_TOK_KW_IF     },
    { "then",   NIX_TOK_KW_THEN   },
    { "else",   NIX_TOK_KW_ELSE   },
    { "let",    NIX_TOK_KW_LET    },
    { "in",     NIX_TOK_KW_IN     },
    { "with",   NIX_TOK_KW_WITH   },
    { "rec",    NIX_TOK_KW_REC    },
    { "assert", NIX_TOK_KW_ASSERT },
    { "import", NIX_TOK_KW_IMPORT },
    { "or",     NIX_TOK_KW_OR     },
    { NULL,     NIX_TOK_EOF       },
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static char peek(struct nix_lexer *lex)
{
    if (lex->pos >= lex->len) return '\0';
    return lex->src[lex->pos];
}

static char peek_at(struct nix_lexer *lex, size_t offset)
{
    if (lex->pos + offset >= lex->len) return '\0';
    return lex->src[lex->pos + offset];
}

static char advance(struct nix_lexer *lex)
{
    if (lex->pos >= lex->len) return '\0';
    char c = lex->src[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

static void skip_whitespace(struct nix_lexer *lex)
{
    while (lex->pos < lex->len) {
        char c = peek(lex);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            advance(lex);
        } else if (c == '#') {
            /* Line comment */
            while (lex->pos < lex->len && peek(lex) != '\n')
                advance(lex);
        } else if (c == '/' && peek_at(lex, 1) == '*') {
            /* Block comment */
            advance(lex); /* / */
            advance(lex); /* * */
            int depth = 1;
            while (lex->pos < lex->len && depth > 0) {
                if (peek(lex) == '/' && peek_at(lex, 1) == '*') {
                    advance(lex); advance(lex);
                    depth++;
                } else if (peek(lex) == '*' && peek_at(lex, 1) == '/') {
                    advance(lex); advance(lex);
                    depth--;
                } else {
                    advance(lex);
                }
            }
        } else {
            break;
        }
    }
}

/* ── String literal with interpolation ────────────────────────────── */

static nix_token_t lex_string(struct nix_lexer *lex)
{
    nix_token_t tok = { .type = NIX_TOK_STRING, .line = lex->line, .col = lex->col };

    /* We'll build the string text. For interpolation, we emit the
     * string up to the ${, then the parser handles the interpolation
     * as a separate step. For the lexer, we just return the full
     * string text with ${} markers intact - the parser splits them. */

    size_t cap = 256;
    char *buf = calloc(cap, 1);
    size_t len = 0;

    advance(lex); /* opening " */

    while (lex->pos < lex->len) {
        char c = peek(lex);

        if (c == '"') {
            advance(lex);
            break;
        }

        if (c == '\\') {
            advance(lex);
            char escaped = peek(lex);
            switch (escaped) {
            case 'n':  c = '\n'; advance(lex); break;
            case 't':  c = '\t'; advance(lex); break;
            case 'r':  c = '\r'; advance(lex); break;
            case '"':  c = '"';  advance(lex); break;
            case '\\': c = '\\'; advance(lex); break;
            case '$':  c = '$';  advance(lex); break;
            default:   c = escaped; advance(lex); break;
            }
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = c;
            continue;
        }

        if (c == '$' && peek_at(lex, 1) == '{') {
            /* Interpolation: include ${...} verbatim in the token text.
             * The parser will split this into parts. */
            if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = '$';
            buf[len++] = '{';
            advance(lex); /* $ */
            advance(lex); /* { */

            /* Read until matching } */
            int depth = 1;
            while (lex->pos < lex->len && depth > 0) {
                c = peek(lex);
                if (c == '{') { depth++; }
                else if (c == '}') { depth--; }
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = c;
                advance(lex);
            }
            continue;
        }

        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
        advance(lex);
    }

    buf[len] = '\0';
    tok.text = buf;
    return tok;
}

/* ── Path literal ──────────────────────────────────────────────────── */

static nix_token_t lex_path(struct nix_lexer *lex)
{
    nix_token_t tok = { .type = NIX_TOK_PATH, .line = lex->line, .col = lex->col };

    size_t cap = 256;
    char *buf = calloc(cap, 1);
    size_t len = 0;

    while (lex->pos < lex->len) {
        char c = peek(lex);
        /* Path characters: alphanumeric, /, ., -, _, +, ~ */
        if (isalnum(c) || c == '/' || c == '.' || c == '-' ||
            c == '_' || c == '+' || c == '~') {
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = c;
            advance(lex);
        } else {
            break;
        }
    }

    buf[len] = '\0';
    tok.text = buf;
    return tok;
}

/* ── Identifier or keyword ────────────────────────────────────────── */

static nix_token_t lex_ident(struct nix_lexer *lex)
{
    nix_token_t tok = { .line = lex->line, .col = lex->col };

    size_t cap = 64;
    char *buf = calloc(cap, 1);
    size_t len = 0;

    while (lex->pos < lex->len) {
        char c = peek(lex);
        if (isalnum(c) || c == '_' || c == '-' || c == '\'') {
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = c;
            advance(lex);
        } else {
            break;
        }
    }

    buf[len] = '\0';

    /* Check for keywords */
    for (const keyword_t *kw = keywords; kw->word; kw++) {
        if (strcmp(buf, kw->word) == 0) {
            tok.type = kw->type;
            tok.text = buf;
            return tok;
        }
    }

    /* Check for boolean/null literals */
    if (strcmp(buf, "true") == 0) {
        tok.type = NIX_TOK_IDENT;  /* parser handles true/false/null as idents */
        tok.text = buf;
        return tok;
    }
    if (strcmp(buf, "false") == 0) {
        tok.type = NIX_TOK_IDENT;
        tok.text = buf;
        return tok;
    }
    if (strcmp(buf, "null") == 0) {
        tok.type = NIX_TOK_IDENT;
        tok.text = buf;
        return tok;
    }

    tok.type = NIX_TOK_IDENT;
    tok.text = buf;
    return tok;
}

/* ── Integer literal ───────────────────────────────────────────────── */

static nix_token_t lex_integer(struct nix_lexer *lex)
{
    nix_token_t tok = { .type = NIX_TOK_INT, .line = lex->line, .col = lex->col };

    int64_t val = 0;
    while (lex->pos < lex->len && isdigit(peek(lex))) {
        val = val * 10 + (advance(lex) - '0');
    }

    tok.integer = val;
    tok.text = NULL;
    return tok;
}

/* ── Main lexer entry point ───────────────────────────────────────── */

nix_token_t nix_lexer_next(struct nix_lexer *lex)
{
    skip_whitespace(lex);

    nix_token_t tok = { .type = NIX_TOK_EOF, .line = lex->line, .col = lex->col, .text = NULL };

    if (lex->pos >= lex->len)
        return tok;

    char c = peek(lex);

    /* String literal */
    if (c == '"')
        return lex_string(lex);

    /* Path literal: starts with / or ./ or ~/
     * But / followed by space or digit is the division operator, not a path. */
    if (c == '/') {
        char next = peek_at(lex, 1);
        if (isalpha(next) || next == '_' || next == '/' || next == '.' ||
            next == '-' || next == '~' || next == '+') {
            return lex_path(lex);
        }
        /* Otherwise it's the / operator - fall through to single-char ops */
    }
    if ((c == '.' && peek_at(lex, 1) == '/') ||
        (c == '~' && peek_at(lex, 1) == '/'))
        return lex_path(lex);

    /* Identifier or keyword */
    if (isalpha(c) || c == '_')
        return lex_ident(lex);

    /* Integer literal */
    if (isdigit(c))
        return lex_integer(lex);

    /* Two-character operators */
    char c2 = peek_at(lex, 1);

    if (c == '+' && c2 == '+') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_PLUS_PLUS;
        return tok;
    }
    if (c == '=' && c2 == '=') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_EQ;
        return tok;
    }
    if (c == '!' && c2 == '=') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_NEQ;
        return tok;
    }
    if (c == '<' && c2 == '=') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_LE;
        return tok;
    }
    if (c == '>' && c2 == '=') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_GE;
        return tok;
    }
    if (c == '&' && c2 == '&') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_AND;
        return tok;
    }
    if (c == '|' && c2 == '|') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_OR;
        return tok;
    }
    if (c == '-' && c2 == '>') {
        advance(lex); advance(lex);
        tok.type = NIX_TOK_ARROW;
        return tok;
    }

    /* Single-character tokens */
    advance(lex);
    switch (c) {
    case '{': tok.type = NIX_TOK_LBRACE;    return tok;
    case '}': tok.type = NIX_TOK_RBRACE;    return tok;
    case '[': tok.type = NIX_TOK_LBRACKET;  return tok;
    case ']': tok.type = NIX_TOK_RBRACKET;  return tok;
    case '(': tok.type = NIX_TOK_LPAREN;    return tok;
    case ')': tok.type = NIX_TOK_RPAREN;    return tok;
    case ':': tok.type = NIX_TOK_COLON;     return tok;
    case ';': tok.type = NIX_TOK_SEMICOLON; return tok;
    case ',': tok.type = NIX_TOK_COMMA;     return tok;
    case '.': tok.type = NIX_TOK_DOT;       return tok;
    case '=': tok.type = NIX_TOK_EQUALS;    return tok;
    case '?': tok.type = NIX_TOK_QUESTION;  return tok;
    case '+': tok.type = NIX_TOK_PLUS;      return tok;
    case '-': tok.type = NIX_TOK_MINUS;     return tok;
    case '*': tok.type = NIX_TOK_STAR;      return tok;
    case '/': tok.type = NIX_TOK_SLASH;     return tok;
    case '<': tok.type = NIX_TOK_LT;        return tok;
    case '>': tok.type = NIX_TOK_GT;        return tok;
    case '!': tok.type = NIX_TOK_NOT;       return tok;
    default:
        /* Unknown character - skip and return EOF */
        fprintf(stderr, "nix_lexer: unexpected character '%c' at line %d col %d\n",
                c, lex->line, lex->col);
        tok.type = NIX_TOK_EOF;
        return tok;
    }
}

/* ── Create / free ────────────────────────────────────────────────── */

nix_lexer_t *nix_lexer_new(const char *source, size_t len)
{
    nix_lexer_t *lex = calloc(1, sizeof(*lex));
    if (lex) {
        lex->src  = source;
        lex->len  = len;
        lex->pos  = 0;
        lex->line = 1;
        lex->col  = 1;
    }
    return lex;
}

void nix_lexer_free(nix_lexer_t *lex)
{
    free(lex);
}
