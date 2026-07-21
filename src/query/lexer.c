#include "lexer.h"
#include <ctype.h>
#include <string.h>

typedef struct {
    const char* start;
    const char* pos;
    int line;
    int col;
} Lexer;

static Token make_token(Lexer* lx, TokenType type) {
    Token t = {type, "", lx->line, lx->col};
    int len = (int)(lx->pos - lx->start);
    if (len > 0 && len < 512) {
        memcpy(t.text, lx->start, len);
        t.text[len] = '\0';
    }
    return t;
}

static int add_token(TokenList* list, Token t) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 256;
        list->tokens = realloc(list->tokens, list->capacity * sizeof(Token));
        if (!list->tokens) return 0;
    }
    list->tokens[list->count++] = t;
    return 1;
}

static void skip_whitespace(Lexer* lx) {
    while (*lx->pos) {
        if (*lx->pos == ' ' || *lx->pos == '\t') { lx->pos++; lx->col++; }
        else if (*lx->pos == '\n') { lx->pos++; lx->line++; lx->col = 1; }
        else if (*lx->pos == '\r') { lx->pos++; }
        else break;
    }
}

static TokenType keyword_or_id(const char* word, int len) {
    struct { const char* kw; TokenType type; } kws[] = {
        {"SELECT",TOK_SELECT},{"FROM",TOK_FROM},{"WHERE",TOK_WHERE},
        {"INSERT",TOK_INSERT},{"INTO",TOK_INTO},{"VALUES",TOK_VALUES},
        {"UPDATE",TOK_UPDATE},{"SET",TOK_SET},{"DELETE",TOK_DELETE},
        {"CREATE",TOK_CREATE},{"TABLE",TOK_TABLE},{"DATABASE",TOK_DATABASE},
        {"DROP",TOK_DROP},{"ALTER",TOK_ALTER},{"INDEX",TOK_INDEX},
        {"VIEW",TOK_VIEW},{"AND",TOK_AND},{"OR",TOK_OR},{"NOT",TOK_NOT},
        {"IN",TOK_IN},{"IS",TOK_IS},{"NULL",TOK_NULL},{"LIKE",TOK_LIKE},
        {"ORDER",TOK_ORDER},{"BY",TOK_BY},{"ASC",TOK_ASC},{"DESC",TOK_DESC},
        {"LIMIT",TOK_LIMIT},{"JOIN",TOK_JOIN},{"LEFT",TOK_LEFT},
        {"RIGHT",TOK_RIGHT},{"ON",TOK_ON},{"AS",TOK_AS},
        {"DISTINCT",TOK_DISTINCT},{"GROUP",TOK_GROUP},{"HAVING",TOK_HAVING},
        {"BEGIN",TOK_BEGIN},{"COMMIT",TOK_COMMIT},{"ROLLBACK",TOK_ROLLBACK},
        {"TRANSACTION",TOK_TRANSACTION},{"USE",TOK_USE},{"SHOW",TOK_SHOW},
        {"DESCRIBE",TOK_DESCRIBE},{"EXPLAIN",TOK_EXPLAIN},
        {"TRUNCATE",TOK_TRUNCATE},{"UNION",TOK_UNION},{"ALL",TOK_ALL},
        {"EXISTS",TOK_EXISTS},{"BETWEEN",TOK_BETWEEN},{"KEY",TOK_KEY},
    };
    char buf[64];
    int n = len < 63 ? len : 63;
    memcpy(buf, word, n); buf[n] = '\0';
    for (int i = 0; i < n; i++) buf[i] = toupper((unsigned char)buf[i]);

    for (int i = 0; i < (int)(sizeof(kws)/sizeof(kws[0])); i++)
        if (strcmp(buf, kws[i].kw) == 0)
            return kws[i].type;
    return TOK_IDENTIFIER;
}

TokenList* lexer_tokenize(const char* sql) {
    TokenList* list = calloc(1, sizeof(TokenList));
    if (!list) return NULL;

    Lexer lx;
    lx.start = lx.pos = sql;
    lx.line = 1; lx.col = 1;

    while (*lx.pos) {
        skip_whitespace(&lx);

        if (!*lx.pos) break;

        lx.start = lx.pos;

        // Single-line comments
        if (*lx.pos == '-' && *(lx.pos+1) == '-') {
            while (*lx.pos && *lx.pos != '\n') lx.pos++;
            continue;
        }

        // Block comments
        if (*lx.pos == '/' && *(lx.pos+1) == '*') {
            lx.pos += 2;
            while (*lx.pos && !(*lx.pos == '*' && *(lx.pos+1) == '/')) {
                if (*lx.pos == '\n') { lx.line++; lx.col = 1; }
                lx.pos++;
            }
            if (*lx.pos) lx.pos += 2;
            continue;
        }

        // String literals
        if (*lx.pos == '\'' || *lx.pos == '"') {
            char quote = *lx.pos;
            lx.pos++;
            while (*lx.pos && *lx.pos != quote) {
                if (*lx.pos == '\\') lx.pos++;
                lx.pos++;
            }
            if (*lx.pos) lx.pos++;
            add_token(list, make_token(&lx, TOK_STRING));
            continue;
        }

        // Numbers
        if (isdigit((unsigned char)*lx.pos)) {
            while (isdigit((unsigned char)*lx.pos) || *lx.pos == '.') lx.pos++;
            add_token(list, make_token(&lx, TOK_INTEGER));
            continue;
        }

        // Identifiers and keywords
        if (isalpha((unsigned char)*lx.pos) || *lx.pos == '_') {
            while (isalnum((unsigned char)*lx.pos) || *lx.pos == '_') lx.pos++;
            TokenType tt = keyword_or_id(lx.start, (int)(lx.pos - lx.start));
            add_token(list, make_token(&lx, tt));
            continue;
        }

        // Single and double char operators
        TokenType tt = TOK_ERROR;
        int advance = 1;
        switch (*lx.pos) {
            case '*': tt = TOK_STAR; break;
            case ',': tt = TOK_COMMA; break;
            case ';': tt = TOK_SEMICOLON; break;
            case '.': tt = TOK_DOT; break;
            case '(': tt = TOK_LPAREN; break;
            case ')': tt = TOK_RPAREN; break;
            case '+': tt = TOK_PLUS; break;
            case '-': tt = TOK_MINUS; break;
            case '/': tt = TOK_SLASH; break;
            case '=': tt = TOK_EQ; break;
            case '!': if (*(lx.pos+1) == '=') { tt = TOK_NEQ; advance = 2; } break;
            case '<': tt = *(lx.pos+1) == '=' ? (advance=2,TOK_LE) : TOK_LT; break;
            case '>': tt = *(lx.pos+1) == '=' ? (advance=2,TOK_GE) : TOK_GT; break;
        }
        if (tt != TOK_ERROR) {
            lx.pos += advance;
            add_token(list, make_token(&lx, tt));
            continue;
        }

        lx.pos++;
        add_token(list, make_token(&lx, TOK_ERROR));
    }

    add_token(list, make_token(&lx, TOK_EOF));
    return list;
}

void lexer_free(TokenList* list) {
    if (!list) return;
    free(list->tokens);
    free(list);
}
