#include "lexer.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

typedef struct {
    const char* start;
    const char* pos;
    int line;
    int col;
} Lexer;

// token in costruzione, con il testo in un buffer (diventa un Token con il testo nel pool)
typedef struct {
    TokenType type;
    char      text[512];
    int       line;
    int       col;
    const char* ext;      // testo lungo fuori dal buffer (letterali stringa oltre 511 byte), o NULL
    size_t      ext_len;
} LexTok;

// letterale stringa più lungo accettato (un testo ne conserva al massimo SV_MAX_TEXT_BYTES: il limite
// della colonna lo controlla l'executor con un messaggio preciso)
#define LEX_MAX_STRING SV_MAX_TEXT_BYTES

// riempie t (senza azzerare tutto il buffer: un token è lungo pochi byte)
static void make_token_into(LexTok* t, Lexer* lx, TokenType type) {
    t->type = type; t->line = lx->line; t->col = lx->col;
    t->ext = NULL;
    t->text[0] = '\0';
    int len = (int)(lx->pos - lx->start);
    if (len > 0 && len < 512) {
        memcpy(t->text, lx->start, (size_t)len);
        t->text[len] = '\0';
    }
}

// durante la scansione text contiene l'offset nel pool (il pool può spostarsi): i puntatori
// vengono sistemati alla fine
static int add_token(TokenList* list, const LexTok* t) {
    if (list->count >= list->capacity) {
        int nc = list->capacity ? list->capacity * 2 : 256;
        Token* nt = realloc(list->tokens, (size_t)nc * sizeof(Token));
        if (!nt) return 0;
        list->tokens = nt;
        list->capacity = nc;
    }
    const char* src = t->ext ? t->ext : t->text;
    size_t l = (t->ext ? t->ext_len : strlen(t->text)) + 1;
    if (list->plen + l > list->pcap) {
        size_t nc = list->pcap ? list->pcap * 2 : 4096;
        while (nc < list->plen + l) nc *= 2;
        char* np = realloc(list->pool, nc);
        if (!np) return 0;
        list->pool = np;
        list->pcap = nc;
    }
    memcpy(list->pool + list->plen, src, l);
    Token* d = &list->tokens[list->count++];
    d->type = t->type;
    d->line = t->line;
    d->col = t->col;
    d->text = (const char*)(uintptr_t)list->plen;
    list->plen += l;
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
    static const struct { const char* kw; TokenType type; } kws[] = {
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
        {"SAVEPOINT",TOK_SAVEPOINT},{"TO",TOK_TO},{"TRIGGER",TOK_TRIGGER},
        {"BEFORE",TOK_BEFORE},{"AFTER",TOK_AFTER},{"FOR",TOK_FOR},
        {"EACH",TOK_EACH},{"ROW",TOK_ROW},{"RENAME",TOK_RENAME},
        {"BOOLEAN",TOK_BOOLEAN},{"CASE",TOK_CASE},{"WHEN",TOK_WHEN},{"THEN",TOK_THEN},
        {"ELSE",TOK_ELSE},{"END",TOK_END},
    };
    // tabella hash delle parole chiave (costruita al primo uso; il motore è usato sotto un mutex)
    enum { KW_SLOTS = 256 };
    static short slot[KW_SLOTS];
    static bool built = false;
    const int nkw = (int)(sizeof(kws) / sizeof(kws[0]));
    #define KW_HASH(h, s, l) do { h = 2166136261u; for (int q_ = 0; q_ < (l); q_++) { h ^= (unsigned)toupper((unsigned char)(s)[q_]); h *= 16777619u; } } while (0)
    if (!built) {
        for (int i = 0; i < KW_SLOTS; i++) slot[i] = -1;
        for (int i = 0; i < nkw; i++) {
            unsigned h;
            KW_HASH(h, kws[i].kw, (int)strlen(kws[i].kw));
            unsigned j = h & (KW_SLOTS - 1);
            while (slot[j] >= 0) j = (j + 1) & (KW_SLOTS - 1);
            slot[j] = (short)i;
        }
        built = true;
    }
    if (len > 16) return TOK_IDENTIFIER;               // nessuna parola chiave è così lunga
    unsigned h;
    KW_HASH(h, word, len);
    #undef KW_HASH
    for (unsigned j = h & (KW_SLOTS - 1); slot[j] >= 0; j = (j + 1) & (KW_SLOTS - 1)) {
        const char* k = kws[slot[j]].kw;
        if ((int)strlen(k) == len && strncasecmp(k, word, (size_t)len) == 0) return kws[slot[j]].type;
    }
    return TOK_IDENTIFIER;
}

TokenList* lexer_tokenize(const char* sql) {
    TokenList* list = calloc(1, sizeof(TokenList));
    if (!list) return NULL;

    Lexer lx;
    LexTok tok;                       // token in costruzione (riusato)
    tok.ext = NULL;
    char*  sbuf = NULL;               // testo dei letterali stringa (cresce a richiesta)
    size_t scap = 0;
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

        // Backtick-quoted identifiers (e.g. `table_name`)
        if (*lx.pos == '`') {
            lx.pos++;
            while (*lx.pos && *lx.pos != '`') { lx.pos++; }
            make_token_into(&tok, &lx, TOK_IDENTIFIER);
            int blen = strlen(tok.text);
            if (blen > 0 && tok.text[0] == '`') memmove(tok.text, tok.text + 1, blen);
            blen = strlen(tok.text);
            if (blen > 0 && tok.text[blen-1] == '`') tok.text[blen-1] = '\0';
            if (*lx.pos) lx.pos++;
            lx.start = lx.pos;
            add_token(list, &tok);
            continue;
        }

        // String literals: '' (o "") dentro la stringa vale un apice; \\ \' \" \n \t \r sono escape.
        // Il testo va in sbuf (nessun limite fisso, fino a LEX_MAX_STRING byte)
        if (*lx.pos == '\'' || *lx.pos == '"') {
            char quote = *lx.pos;
            tok.type = TOK_STRING; tok.line = lx.line; tok.col = lx.col; tok.text[0] = '\0'; tok.ext = NULL;
            size_t n = 0;
            bool overflow = false, oom = false;
            lx.pos++;
            #define SPUT(chr) do { \
                if (n + 2 > scap) { \
                    if (n + 2 > LEX_MAX_STRING) { overflow = true; break; } \
                    size_t nc = scap ? scap * 2 : 1024; \
                    char* nb = realloc(sbuf, nc); \
                    if (!nb) { oom = true; break; } \
                    sbuf = nb; scap = nc; \
                } \
                sbuf[n++] = (chr); \
            } while (0)
            while (*lx.pos) {
                char ch = *lx.pos;
                if (ch == quote) {
                    if (lx.pos[1] != quote) break;            // fine stringa
                    lx.pos += 2;                              // apice raddoppiato
                } else if (ch == '\\' && lx.pos[1]) {
                    char e = lx.pos[1];
                    lx.pos += 2;
                    switch (e) {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case 'r': ch = '\r'; break;
                        case '\\': case '\'': case '"': ch = e; break;
                        default:                              // escape sconosciuto: resta com'è
                            if (!overflow && !oom) SPUT('\\');
                            ch = e;
                            break;
                    }
                } else {
                    if (ch == '\n') lx.line++;
                    lx.pos++;
                }
                if (!overflow && !oom) SPUT(ch);
            }
            #undef SPUT
            bool closed = *lx.pos != '\0';
            if (closed) lx.pos++;                             // apice di chiusura
            if (!closed) {
                snprintf(tok.text, sizeof(tok.text), "Unterminated string literal");
                tok.type = TOK_ERROR;
            } else if (overflow || oom) {
                snprintf(tok.text, sizeof(tok.text), oom ? "Out of memory reading a string literal"
                                                         : "String literal is longer than %u bytes", LEX_MAX_STRING);
                tok.type = TOK_ERROR;
            } else if (n < sizeof(tok.text)) {
                if (n) memcpy(tok.text, sbuf, n);
                tok.text[n] = '\0';
            } else {
                sbuf[n] = '\0';
                tok.ext = sbuf;
                tok.ext_len = n;
            }
            lx.start = lx.pos;
            add_token(list, &tok);
            tok.ext = NULL;
            continue;
        }

        // Numbers: interi e decimali (un '-' iniziale fa parte del numero quando non può
        // essere una sottrazione, es. VALUES (1, -5) o WHERE x = -3)
        bool neg = false;
        if (*lx.pos == '-' && isdigit((unsigned char)lx.pos[1])) {
            TokenType pt = list->count > 0 ? list->tokens[list->count - 1].type : TOK_EOF;
            neg = !(pt == TOK_IDENTIFIER || pt == TOK_INTEGER || pt == TOK_FLOAT ||
                    pt == TOK_STRING || pt == TOK_RPAREN);
        }
        if (isdigit((unsigned char)*lx.pos) || neg) {
            bool is_float = false;
            if (neg) lx.pos++;
            while (isdigit((unsigned char)*lx.pos) || (*lx.pos == '.' && !is_float)) {
                if (*lx.pos == '.') is_float = true;
                lx.pos++;
            }
            if ((*lx.pos == 'e' || *lx.pos == 'E') &&
                (isdigit((unsigned char)lx.pos[1]) ||
                 ((lx.pos[1] == '-' || lx.pos[1] == '+') && isdigit((unsigned char)lx.pos[2])))) {
                is_float = true;
                lx.pos += 2;
                while (isdigit((unsigned char)*lx.pos)) lx.pos++;
            }
            make_token_into(&tok, &lx, is_float ? TOK_FLOAT : TOK_INTEGER);
            add_token(list, &tok);
            continue;
        }

        // Identifiers and keywords
        if (isalpha((unsigned char)*lx.pos) || *lx.pos == '_') {
            while (isalnum((unsigned char)*lx.pos) || *lx.pos == '_') lx.pos++;
            TokenType tt = keyword_or_id(lx.start, (int)(lx.pos - lx.start));
            make_token_into(&tok, &lx, tt);
            if (tt == TOK_IDENTIFIER && (!strcasecmp(tok.text, "TRUE") || !strcasecmp(tok.text, "FALSE"))) {
                bool on = (tok.text[0] | 0x20) == 't';   // letterali booleani = 1 / 0
                tok.type = TOK_INTEGER;
                snprintf(tok.text, sizeof(tok.text), "%d", on ? 1 : 0);
            }
            add_token(list, &tok);
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
            case '%': tt = TOK_PERCENT; break;
            case '=': tt = TOK_EQ; break;
            case '!': if (*(lx.pos+1) == '=') { tt = TOK_NEQ; advance = 2; } break;
            case '<': tt = *(lx.pos+1) == '=' ? (advance=2,TOK_LE)
                         : *(lx.pos+1) == '>' ? (advance=2,TOK_NEQ) : TOK_LT; break;
            case '>': tt = *(lx.pos+1) == '=' ? (advance=2,TOK_GE) : TOK_GT; break;
        }
        if (tt != TOK_ERROR) {
            lx.pos += advance;
            make_token_into(&tok, &lx, tt);
            add_token(list, &tok);
            continue;
        }

        lx.pos++;
        make_token_into(&tok, &lx, TOK_ERROR);
        add_token(list, &tok);
    }

    make_token_into(&tok, &lx, TOK_EOF);
    add_token(list, &tok);
    for (int i = 0; i < list->count; i++)
        list->tokens[i].text = list->pool + (uintptr_t)list->tokens[i].text;
    free(sbuf);
    return list;
}

void lexer_free(TokenList* list) {
    if (!list) return;
    free(list->tokens);
    free(list->pool);
    free(list);
}
