#include "sql_text.h"
#include <ctype.h>
#include <string.h>

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

int sql_split(const char* s, int len, SqlRange* out, int max_out) {
    int n = 0, i = 0, start = 0, code_start = -1;
    while (i <= len && n < max_out) {
        if (i == len || s[i] == ';') {
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
        i++;
    }
    return n;
}

static int line_of(const char* s, int pos) {
    int l = 0;
    for (int i = 0; i < pos; i++) if (s[i] == '\n') l++;
    return l;
}

bool sql_statement_at(const char* s, int len, int pos, SqlRange* out) {
    SqlRange ranges[512];
    int n = sql_split(s, len, ranges, 512);
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
    "CASCADE","COLUMNS","INDEXES","TEMPORARY","OF",
};
static const char* TYPES[] = {
    "INT","INTEGER","BIGINT","SMALLINT","TINYINT","FLOAT","DOUBLE","REAL","DECIMAL","NUMERIC",
    "VARCHAR","CHAR","TEXT","BOOL","BOOLEAN","DATE","DATETIME","TIMESTAMP","TIME","BLOB",
};
static const char* FUNCS[] = {
    "COUNT","SUM","AVG","MIN","MAX","UPPER","LOWER","LENGTH","COALESCE","NOW","ABS","ROUND",
    "SUBSTR","SUBSTRING","CONCAT","IFNULL","NULLIF","CAST",
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
