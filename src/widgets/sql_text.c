#include "sql_text.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

// salta stringhe e commenti a partire da i; ritorna il nuovo indice (o i se non c'è nulla da saltare)
static int skip_quoted_or_comment(const char* s, int len, int i) {
    char c = s[i];
    if (c == '\'' || c == '"' || c == '`') {
        int j = i + 1;
        while (j < len) {
            if (s[j] == c) {
                if (c == '\'' && j + 1 < len && s[j + 1] == '\'') { j += 2; continue; } // '' escape
                return j + 1;
            }
            if (s[j] == '\\' && j + 1 < len) { j += 2; continue; }
            j++;
        }
        return len;
    }
    if (c == '-' && i + 1 < len && s[i + 1] == '-') {
        int j = i + 2;
        while (j < len && s[j] != '\n') j++;
        return j;
    }
    if (c == '/' && i + 1 < len && s[i + 1] == '*') {
        int j = i + 2;
        while (j + 1 < len && !(s[j] == '*' && s[j + 1] == '/')) j++;
        return (j + 1 < len) ? j + 2 : len;
    }
    return i;
}

static bool word_eq(const char* s, int a, int b, const char* w) {
    int n = (int)strlen(w);
    if (b - a != n) return false;
    for (int k = 0; k < n; k++) if (toupper((unsigned char)s[a + k]) != w[k]) return false;
    return true;
}

// Divide lo script negli statement separati da ';' (fuori da stringhe e commenti).
// Dentro CREATE TRIGGER ... BEGIN ... END i ';' del corpo non separano: si conta la
// profondità dei blocchi (BEGIN e CASE aprono, END chiude; END IF chiude un IF).
int sql_split(const char* s, int len, SqlRange* out, int max_out) {
    int n = 0, i = 0, start = 0, code_start = -1;
    int words = 0, depth = 0;
    bool trigger = false, prev_end = false;
    while (i <= len && n < max_out) {
        if (i == len || (s[i] == ';' && !(trigger && depth > 0))) {
            if (i < len || start < len) {
                SqlRange r;
                r.start      = start;
                r.code_start = code_start >= 0 ? code_start : i;
                r.end        = i;
                r.end_semi   = (i < len) ? i + 1 : i;
                r.has_code   = code_start >= 0;
                out[n++] = r;
            }
            if (i == len) break;
            i++;
            start = i;
            code_start = -1;
            words = depth = 0;
            trigger = prev_end = false;
            continue;
        }
        char c = s[i];
        int j = skip_quoted_or_comment(s, len, i);
        if (j != i) {
            // le stringhe sono codice, i commenti no
            if ((c == '\'' || c == '"' || c == '`') && code_start < 0) code_start = i;
            i = j;
            continue;
        }
        if (!isspace((unsigned char)c) && code_start < 0) code_start = i;
        if (isalpha((unsigned char)c) || c == '_') {
            int e = i;
            while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
            words++;
            if (words <= 4 && word_eq(s, i, e, "TRIGGER") && word_eq(s, code_start, code_start + 6, "CREATE")) trigger = true;
            if (trigger) {
                bool closes_if = prev_end && (word_eq(s, i, e, "IF") || word_eq(s, i, e, "LOOP") || word_eq(s, i, e, "WHILE"));
                if (closes_if) depth++;                          // "END IF": l'END non chiudeva un BEGIN
                if (word_eq(s, i, e, "BEGIN") || word_eq(s, i, e, "CASE")) depth++;
                else if (word_eq(s, i, e, "END")) depth--;
                prev_end = word_eq(s, i, e, "END");
                if (depth < 0) depth = 0;
            }
            i = e;
            continue;
        }
        if (!isspace((unsigned char)c)) prev_end = false;
        i++;
    }
    return n;
}

static bool sql_statement_in(const char* s, int pos, SqlRange* ranges, int n, SqlRange* out);

static int line_of(const char* s, int pos) {
    int l = 0;
    for (int i = 0; i < pos; i++) if (s[i] == '\n') l++;
    return l;
}

bool sql_statement_at(const char* s, int len, int pos, SqlRange* out) {
    // al massimo un intervallo per ';' più uno finale
    int cap = 2;
    for (int i = 0; i < len; i++) if (s[i] == ';') cap++;
    SqlRange stack_ranges[64];
    SqlRange* ranges = cap <= 64 ? stack_ranges : malloc(sizeof(SqlRange) * cap);
    if (!ranges) return false;
    bool found = sql_statement_in(s, pos, ranges, sql_split(s, len, ranges, cap), out);
    if (ranges != stack_ranges) free(ranges);
    return found;
}

static bool sql_statement_in(const char* s, int pos, SqlRange* ranges, int n, SqlRange* out) {
    int prev = -1;
    for (int k = 0; k < n; k++) {
        SqlRange* r = &ranges[k];
        // gli statement sono contigui: [start, end_semi]. Con il cursore subito dopo
        // il ';' vince lo statement che termina lì (viene trovato per primo).
        if (pos >= r->start && pos <= r->end_semi) {
            // cursore negli spazi/commenti iniziali di r ma sulla stessa riga del ';'
            // precedente: l'utente intende lo statement precedente
            bool in_lead = pos < r->code_start || !r->has_code;
            if (in_lead && prev >= 0 &&
                (!r->has_code || line_of(s, pos) == line_of(s, ranges[prev].end))) {
                *out = ranges[prev];
                return true;
            }
            if (r->has_code) { *out = *r; return true; }
            return false;
        }
        if (r->has_code) prev = k;
    }
    if (prev >= 0) { *out = ranges[prev]; return true; }
    return false;
}

// -- parole chiave --

static const char* KEYWORDS[] = {
    "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","SET","DELETE","CREATE","DROP",
    "TABLE","DATABASE","DATABASES","TABLES","INDEX","VIEW","TRIGGER","PROCEDURE","FUNCTION","USE","SHOW",
    "DESCRIBE","DESC","ASC","ORDER","BY","GROUP","HAVING","LIMIT","OFFSET","DISTINCT","AS","ON",
    "JOIN","INNER","LEFT","RIGHT","OUTER","CROSS","FULL","NATURAL","AND","OR","NOT","IN","IS",
    "LIKE","BETWEEN","EXISTS","UNION","ALL","ANY","CASE","WHEN","THEN","ELSE","END","BEGIN",
    "COMMIT","ROLLBACK","SAVEPOINT","TO","TRANSACTION","ALTER","RENAME","ADD","COLUMN","PRIMARY",
    "KEY","FOREIGN","REFERENCES","UNIQUE","DEFAULT","AUTO_INCREMENT","CONSTRAINT","CHECK",
    "EXPLAIN","TRUNCATE","IF","BEFORE","AFTER","FOR","EACH","ROW","REPLACE","WITH","RETURNS",
    "CASCADE","COLUMNS","INDEXES","TEMPORARY","OF","INTERSECT","EXCEPT","SOME","CHECKPOINT",
    "RESTRICT","ACTION","NO","STATUS","OVER","PARTITION","ROWS","RANGE","PRECEDING","FOLLOWING",
    "UNBOUNDED","CURRENT","RECURSIVE","USING","ANALYZE","SIGNAL","ELSEIF","STATEMENT","MINUS",
    "MODIFY","TYPE","DATA",
};
static const char* TYPES[] = {
    "INT","INTEGER","BIGINT","SMALLINT","TINYINT","FLOAT","DOUBLE","REAL","DECIMAL","NUMERIC",
    "VARCHAR","CHAR","TEXT","BOOL","BOOLEAN","DATE","DATETIME","TIMESTAMP","TIME","BLOB",
};
static const char* FUNCS[] = {
    "COUNT","SUM","AVG","MIN","MAX","UPPER","LOWER","LENGTH","COALESCE","NOW","ABS","ROUND",
    "SUBSTR","SUBSTRING","CONCAT","IFNULL","NULLIF","CAST","MOD","ROW_NUMBER","RANK","DENSE_RANK",
    "NTILE","LAG","LEAD","FIRST_VALUE","LAST_VALUE",
};
static const char* LITERALS[] = { "NULL", "TRUE", "FALSE" };

static bool in_list(const char** list, int count, const char* w, int len) {
    char up[32];
    if (len <= 0 || len >= (int)sizeof(up)) return false;
    for (int i = 0; i < len; i++) up[i] = (char)toupper((unsigned char)w[i]);
    up[len] = '\0';
    for (int i = 0; i < count; i++)
        if (strcmp(list[i], up) == 0) return true;
    return false;
}

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

SqlWordKind sql_classify_word(const char* w, int len) {
    if (in_list(KEYWORDS, COUNT_OF(KEYWORDS), w, len)) return SQL_WORD_KEYWORD;
    if (in_list(TYPES,    COUNT_OF(TYPES),    w, len)) return SQL_WORD_TYPE;
    if (in_list(LITERALS, COUNT_OF(LITERALS), w, len)) return SQL_WORD_LITERAL;
    if (in_list(FUNCS,    COUNT_OF(FUNCS),    w, len)) return SQL_WORD_FUNC;
    return SQL_WORD_IDENT;
}

int sql_word_list(SqlWordKind kind, const char* const** out) {
    switch (kind) {
        case SQL_WORD_KEYWORD: *out = KEYWORDS; return COUNT_OF(KEYWORDS);
        case SQL_WORD_TYPE:    *out = TYPES;    return COUNT_OF(TYPES);
        case SQL_WORD_FUNC:    *out = FUNCS;    return COUNT_OF(FUNCS);
        case SQL_WORD_LITERAL: *out = LITERALS; return COUNT_OF(LITERALS);
        default:               *out = NULL;     return 0;
    }
}

// ═════════════════════════════════════════════════════════════════
//  Formattazione
// ═════════════════════════════════════════════════════════════════

#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#define SV_MIN_INT(a, b) ((a) < (b) ? (a) : (b))

typedef struct { char* b; int len, cap; bool ok; } SBuf;

static void sb_put(SBuf* sb, const char* s, int n) {
    if (!sb->ok) return;
    if (sb->len + n + 1 > sb->cap) {
        int cap = sb->cap * 2 + n + 64;
        char* nb = realloc(sb->b, cap);
        if (!nb) { sb->ok = false; return; }
        sb->b = nb;
        sb->cap = cap;
    }
    memcpy(sb->b + sb->len, s, n);
    sb->len += n;
    sb->b[sb->len] = '\0';
}

static char sb_last(const SBuf* sb) { return sb->len ? sb->b[sb->len - 1] : '\n'; }

static void sb_trim_spaces(SBuf* sb) {
    while (sb->len && sb->b[sb->len - 1] == ' ') sb->b[--sb->len] = '\0';
}

// va a capo (se non lo è già) e indenta
int  g_sql_format_indent = 4;      // spazi di indentazione (Settings)
bool g_sql_format_lower  = false;  // parole chiave in minuscolo (Settings)

static void sb_newline(SBuf* sb, int indent) {
    if (indent > 0) indent = g_sql_format_indent;
    sb_trim_spaces(sb);
    if (sb->len && sb_last(sb) != '\n') sb_put(sb, "\n", 1);
    for (int i = 0; i < indent; i++) sb_put(sb, " ", 1);
}

// riga vuota di separazione tra istruzioni
static void sb_blank_line(SBuf* sb) {
    sb_trim_spaces(sb);
    if (!sb->len) return;
    if (sb_last(sb) != '\n') sb_put(sb, "\n", 1);
    if (sb->len < 2 || sb->b[sb->len - 2] != '\n') sb_put(sb, "\n", 1);
}

static void sb_space(SBuf* sb) {
    char c = sb_last(sb);
    if (c != ' ' && c != '\n' && sb->len) sb_put(sb, " ", 1);
}

static bool word_is(const char* w, int n, const char* kw) {
    return (int)strlen(kw) == n && strncasecmp(w, kw, n) == 0;
}

static bool word_in(const char* w, int n, const char* const* list) {
    for (int i = 0; list[i]; i++) if (word_is(w, n, list[i])) return true;
    return false;
}

enum { TK_NONE, TK_WORD, TK_KEYWORD, TK_OPEN, TK_CLOSE, TK_COMMA, TK_OP, TK_DOT, TK_SEMI, TK_VALUE, TK_STAR };

char* sql_format(const char* s, int len) {
    static const char* const CLAUSES[] = { "FROM", "WHERE", "GROUP", "ORDER", "HAVING", "LIMIT", "VALUES",
                                           "SET", "UNION", "INTERSECT", "EXCEPT", "SELECT", NULL };
    static const char* const JOIN_MODS[] = { "LEFT", "RIGHT", "INNER", "OUTER", "CROSS", "FULL", "NATURAL", NULL };

    SBuf sb = { malloc(len + 64), 0, len + 64, true };
    if (!sb.b) return NULL;
    sb.b[0] = '\0';

    int depth = 0, ct_depth = -1;          // profondità parentesi; lista colonne di CREATE TABLE
    int prev = TK_NONE;
    char prev_kw[16] = "";                 // ultima keyword (maiuscola)
    bool create_seen = false, create_table = false;
    bool cond_clause = false;              // dentro WHERE / HAVING / ON: AND/OR vanno a capo
    bool between = false;                  // il prossimo AND appartiene a BETWEEN
    bool stmt_break = false;               // serve una riga vuota prima del prossimo token
    bool src_nl = false;                   // c'era un a capo nel sorgente prima del token
    bool obj_name = false;                 // l'ultima parola è il nome di un oggetto (TABLE x, INTO x...)
    static const char* const OBJ_KW[] = { "TABLE", "INTO", "REFERENCES", "VIEW", "INDEX", "EXISTS", "TRIGGER", NULL };

    int i = 0;
    while (i < len) {
        char c = s[i];
        if (isspace((unsigned char)c)) { if (c == '\n') src_nl = true; i++; continue; }

        // -- commenti --
        if ((c == '-' && i + 1 < len && s[i + 1] == '-') || (c == '/' && i + 1 < len && s[i + 1] == '*')) {
            int j = skip_quoted_or_comment(s, len, i);
            bool line = c == '-';
            if (stmt_break && src_nl) { sb_blank_line(&sb); stmt_break = false; }
            else if (src_nl && sb.len && sb_last(&sb) != '\n') sb_newline(&sb, ct_depth >= 0 ? 4 : 0);
            else sb_space(&sb);
            int e = j;
            while (e > i && (s[e - 1] == '\n' || s[e - 1] == '\r')) e--;
            sb_put(&sb, s + i, e - i);
            if (line) sb_newline(&sb, ct_depth >= 0 ? 4 : (stmt_break || prev == TK_NONE ? 0 : 4));
            i = j;
            src_nl = line;
            continue;
        }

        // nuova istruzione dopo ';': riga vuota
        if (stmt_break) { sb_blank_line(&sb); stmt_break = false; }
        src_nl = false;

        // -- stringhe e identificatori quotati --
        if (c == '\'' || c == '"' || c == '`') {
            int j = skip_quoted_or_comment(s, len, i);
            if (prev != TK_OPEN && prev != TK_DOT) sb_space(&sb);
            sb_put(&sb, s + i, j - i);
            prev = c == '\'' ? TK_VALUE : TK_WORD;
            i = j;
            continue;
        }

        // -- numeri --
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1]) && prev != TK_WORD)) {
            int j = i;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            if (prev != TK_OPEN && prev != TK_DOT && !(prev == TK_OP && sb_last(&sb) != ' ')) sb_space(&sb);
            sb_put(&sb, s + i, j - i);
            prev = TK_VALUE;
            i = j;
            continue;
        }

        // -- parole --
        if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
            int j = i;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_' || (unsigned char)s[j] >= 0x80)) j++;
            const char* w = s + i;
            int n = j - i;
            SqlWordKind k = prev == TK_DOT ? SQL_WORD_IDENT : sql_classify_word(w, n);
            bool kw = k == SQL_WORD_KEYWORD;

            if (kw && depth == 0) {
                bool after_mod = word_in(prev_kw, (int)strlen(prev_kw), JOIN_MODS) && prev == TK_KEYWORD;
                bool brk = false;
                if (word_in(w, n, CLAUSES)) brk = true;
                if (word_in(w, n, JOIN_MODS) && !after_mod) brk = true;
                if (word_is(w, n, "JOIN") && !after_mod) brk = true;
                if (word_is(w, n, "SET") && word_is(prev_kw, (int)strlen(prev_kw), "TRANSACTION")) brk = false;
                if ((word_is(w, n, "AND") || word_is(w, n, "OR")) && cond_clause) {
                    if (between && word_is(w, n, "AND")) between = false;
                    else { sb_newline(&sb, 4); brk = false; prev = TK_NONE; }
                }
                if (brk) sb_newline(&sb, 0);

                if (word_is(w, n, "WHERE") || word_is(w, n, "HAVING") || word_is(w, n, "ON")) cond_clause = true;
                else if (word_in(w, n, CLAUSES) || word_is(w, n, "JOIN")) cond_clause = false;
            }
            if (kw && word_is(w, n, "BETWEEN")) between = true;
            if (kw && word_is(w, n, "CREATE")) create_seen = true;
            else if (kw && create_seen && word_is(w, n, "TABLE")) create_table = true;
            else if (kw) create_seen = false;

            if (prev != TK_OPEN && prev != TK_DOT && !(prev == TK_OP && sb_last(&sb) != ' ')) sb_space(&sb);
            if (k == SQL_WORD_IDENT) {
                sb_put(&sb, w, n);
            } else {
                char up[64];
                int m = SV_MIN_INT(n, 63);
                for (int q = 0; q < m; q++)
                    up[q] = (char)(g_sql_format_lower ? tolower((unsigned char)w[q]) : toupper((unsigned char)w[q]));
                sb_put(&sb, up, m);
                if (n > 63) sb_put(&sb, w + 63, n - 63);
            }
            if (kw) { snprintf(prev_kw, sizeof(prev_kw), "%.*s", SV_MIN_INT(n, 15), w);
                      for (char* p = prev_kw; *p; p++) *p = (char)toupper((unsigned char)*p); }
            obj_name = !kw && prev == TK_KEYWORD && word_in(prev_kw, (int)strlen(prev_kw), OBJ_KW);
            prev = (kw || k == SQL_WORD_LITERAL) ? TK_KEYWORD : TK_WORD;
            i = j;
            continue;
        }

        // -- punteggiatura e operatori --
        switch (c) {
            case '(': {
                bool call = prev == TK_WORD && !obj_name;
                if (!call && prev != TK_OPEN && prev != TK_NONE) sb_space(&sb);
                sb_put(&sb, "(", 1);
                depth++;
                if (create_table && depth == 1 && ct_depth < 0) { ct_depth = 1; sb_newline(&sb, 4); }
                prev = TK_OPEN;
                break;
            }
            case ')':
                if (ct_depth == depth && depth == 1) { sb_newline(&sb, 0); ct_depth = -1; create_table = false; }
                else sb_trim_spaces(&sb);
                sb_put(&sb, ")", 1);
                if (depth > 0) depth--;
                prev = TK_CLOSE;
                break;
            case ',':
                sb_trim_spaces(&sb);
                sb_put(&sb, ",", 1);
                if (ct_depth >= 0 && depth == ct_depth) sb_newline(&sb, 4);
                prev = TK_COMMA;
                break;
            case '.':
                sb_trim_spaces(&sb);
                sb_put(&sb, ".", 1);
                prev = TK_DOT;
                break;
            case ';':
                sb_trim_spaces(&sb);
                sb_put(&sb, ";", 1);
                depth = 0; ct_depth = -1;
                create_seen = create_table = cond_clause = between = false;
                prev_kw[0] = '\0';
                prev = TK_SEMI;
                stmt_break = true;
                break;
            case '*':
                if (prev != TK_OPEN && prev != TK_DOT) sb_space(&sb);
                sb_put(&sb, "*", 1);
                prev = TK_STAR;
                break;
            default: {
                if (!strchr("=<>!+-/%|&^~?:", c)) {   // carattere sconosciuto: copiato com'è
                    sb_space(&sb);
                    sb_put(&sb, s + i, 1);
                    prev = TK_WORD;
                    break;
                }
                int j = i + 1;
                while (j < len && strchr("=<>!|&", s[j]) && strchr("=<>!|&", c)) j++;
                bool unary = (c == '-' || c == '+') && j == i + 1 &&
                             (prev == TK_NONE || prev == TK_OP || prev == TK_OPEN || prev == TK_COMMA ||
                              prev == TK_KEYWORD || prev == TK_SEMI);
                if (prev != TK_OPEN || !unary) sb_space(&sb);
                sb_put(&sb, s + i, j - i);
                if (!unary) sb_put(&sb, " ", 1);
                prev = TK_OP;
                i = j;
                continue;
            }
        }
        i++;
    }
    sb_trim_spaces(&sb);
    if (len > 0 && s[len - 1] == '\n' && sb_last(&sb) != '\n') sb_put(&sb, "\n", 1);
    if (!sb.ok) { free(sb.b); return NULL; }
    return sb.b;
}
