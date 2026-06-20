/* test_nix_lexer.c — standalone test for the Nix lexer */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nix_eval.h"

static const char *tok_name(nix_tok_type_t type)
{
    switch (type) {
    case NIX_TOK_EOF:        return "EOF";
    case NIX_TOK_IDENT:      return "IDENT";
    case NIX_TOK_INT:        return "INT";
    case NIX_TOK_STRING:     return "STRING";
    case NIX_TOK_PATH:       return "PATH";
    case NIX_TOK_LBRACE:     return "LBRACE";
    case NIX_TOK_RBRACE:     return "RBRACE";
    case NIX_TOK_LBRACKET:   return "LBRACKET";
    case NIX_TOK_RBRACKET:   return "RBRACKET";
    case NIX_TOK_LPAREN:     return "LPAREN";
    case NIX_TOK_RPAREN:     return "RPAREN";
    case NIX_TOK_COLON:      return "COLON";
    case NIX_TOK_SEMICOLON:  return "SEMICOLON";
    case NIX_TOK_COMMA:      return "COMMA";
    case NIX_TOK_DOT:        return "DOT";
    case NIX_TOK_EQUALS:     return "EQUALS";
    case NIX_TOK_QUESTION:   return "QUESTION";
    case NIX_TOK_PLUS:       return "PLUS";
    case NIX_TOK_PLUS_PLUS:  return "PLUS_PLUS";
    case NIX_TOK_MINUS:      return "MINUS";
    case NIX_TOK_STAR:       return "STAR";
    case NIX_TOK_SLASH:      return "SLASH";
    case NIX_TOK_AND:        return "AND";
    case NIX_TOK_OR:         return "OR";
    case NIX_TOK_NOT:        return "NOT";
    case NIX_TOK_LT:         return "LT";
    case NIX_TOK_LE:         return "LE";
    case NIX_TOK_GT:         return "GT";
    case NIX_TOK_GE:         return "GE";
    case NIX_TOK_EQ:         return "EQ";
    case NIX_TOK_NEQ:        return "NEQ";
    case NIX_TOK_ARROW:      return "ARROW";
    case NIX_TOK_KW_IF:      return "KW_IF";
    case NIX_TOK_KW_THEN:    return "KW_THEN";
    case NIX_TOK_KW_ELSE:    return "KW_ELSE";
    case NIX_TOK_KW_LET:     return "KW_LET";
    case NIX_TOK_KW_IN:      return "KW_IN";
    case NIX_TOK_KW_WITH:    return "KW_WITH";
    case NIX_TOK_KW_REC:     return "KW_REC";
    case NIX_TOK_KW_ASSERT:  return "KW_ASSERT";
    case NIX_TOK_KW_OR:      return "KW_OR";
    case NIX_TOK_KW_IMPORT:  return "KW_IMPORT";
    default:                 return "UNKNOWN";
    }
}

static int test_lexer(const char *name, const char *source)
{
    printf("─── %s ───\n", name);
    nix_lexer_t *lex = nix_lexer_new(source, strlen(source));
    int count = 0;

    while (1) {
        nix_token_t tok = nix_lexer_next(lex);
        printf("  %-3d  %-14s  line %d col %d",
               count, tok_name(tok.type), tok.line, tok.col);

        if (tok.text)
            printf("  \"%s\"", tok.text);
        if (tok.type == NIX_TOK_INT)
            printf("  (%lld)", (long long)tok.integer);

        printf("\n");

        free(tok.text);
        count++;

        if (tok.type == NIX_TOK_EOF) break;
    }

    nix_lexer_free(lex);
    return count;
}

int main(void)
{
    /* Test 1: Simple attrset */
    test_lexer("Simple attrset",
        "{ packages = [ \"firefox\" \"neovim\" ]; }");

    /* Test 2: Function form */
    test_lexer("Function form",
        "{ config, ... }:\n"
        "{\n"
        "  services.sshd.enable = true;\n"
        "}");

    /* Test 3: Import */
    test_lexer("Import",
        "let pkgs = import ./packages.nix; in pkgs");

    /* Test 4: Conditional */
    test_lexer("Conditional",
        "if config.services.sshd.enable then [ \"openssh\" ] else []");

    /* Test 5: Operators */
    test_lexer("Operators",
        "a ++ b + c == d && e || f != g");

    /* Test 6: Comments */
    test_lexer("Comments",
        "# line comment\n"
        "let /* block\n"
        "comment */ x = 1; in x");

    /* Test 7: String interpolation */
    test_lexer("String interpolation",
        "\"hello ${name} world\"");

    /* Test 8: Path literals */
    test_lexer("Path literals",
        "/etc/2O9/2O9.nix ./packages.nix");

    printf("\nAll lexer tests done.\n");
    return 0;
}
