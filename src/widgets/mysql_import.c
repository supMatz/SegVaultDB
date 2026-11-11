#include "mysql_import.h"
#include "sql_text.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { char* p; size_t n, cap; bool oom; } Buf;

static void put(Buf* b, const char* s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char* p = realloc(b->p, cap);
        if (!p) { b->oom = true; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void puts_(Buf* b, const char* s) { put(b, s, strlen(s)); }

bool mysql_looks_like_dump(const char* sql) {
    if (!sql) return false;
    return strstr(sql, "MySQL dump") || strstr(sql, "MariaDB dump") || strstr(sql, "phpMyAdmin SQL Dump") ||
           strstr(sql, "ENGINE=") || strstr(sql, "LOCK TABLES") || strstr(sql, "/*!40101");
}

// ── token di una definizione: parola, stringa, `nome`, (gruppo), simbolo ──
typedef enum { T_WORD, T_STR, T_BQ, T_PAREN, T_PUNCT } TType;
typedef struct { TType t; const char* s; int n; } Tk;

static int skip_quoted(const char* s, int i, int n, char q) {
    for (i++; i < n; i++) {
        if (s[i] == '\\' && q != '`') { i++; continue; }
        if (s[i] == q) { if (i + 1 < n && s[i + 1] == q) { i++; continue; } return i + 1; }
    }
    return n;
}

static int tokenize(const char* s, int n, Tk* out, int max) {
    int k = 0, i = 0;
    while (i < n && k < max) {
        char c = s[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {          // commento /* ... */ (anche /*! ... */)
            const char* e = strstr(s + i + 2, "*/");
            i = e && e - s < n ? (int)(e - s) + 2 : n;
            continue;
        }
        Tk t = { T_PUNCT, s + i, 1 };
        if (c == '\'' || c == '"') { int e = skip_quoted(s, i, n, c); t = (Tk){ T_STR, s + i, e - i }; }
        else if (c == '`') { int e = skip_quoted(s, i, n, '`'); t = (Tk){ T_BQ, s + i, e - i }; }
        else if (c == '(') {
            int d = 0, j = i;
            for (; j < n; j++) {
                if (s[j] == '\'' || s[j] == '"' || s[j] == '`') { j = skip_quoted(s, j, n, s[j]) - 1; continue; }
                if (s[j] == '(') d++;
                else if (s[j] == ')' && --d == 0) { j++; break; }
            }
            t = (Tk){ T_PAREN, s + i, j - i };
        } else if (isalnum((unsigned char)c) || c == '_' || c == '$' || c == '.' || c == '-' || c == '+') {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '$' || s[j] == '.' || ((s[j] == '-' || s[j] == '+') && j == i))) j++;
            t = (Tk){ T_WORD, s + i, j - i };
        }
        out[k++] = t;
        i += t.n;
    }
    return k;
}

static bool is(const Tk* t, const char* w) { return t->t == T_WORD && (int)strlen(w) == t->n && !strncasecmp(t->s, w, (size_t)t->n); }

// colonne di "(a, b)": quante sono, e la prima in out (con i `)
static int paren_cols(const Tk* p, char* first, size_t fn) {
    Tk in[64];
    int k = tokenize(p->s + 1, p->n - 2, in, 64), cols = 0;
    first[0] = '\0';
    for (int i = 0; i < k; i++) {
        if (in[i].t == T_BQ || in[i].t == T_WORD) {
            if (!cols) snprintf(first, fn, "%.*s", in[i].n, in[i].s);
            cols++;
            if (i + 1 < k && in[i + 1].t == T_PAREN) i++;          // prefisso: col(10)
        }
    }
    return cols;
}

// tipo MySQL -> tipo SegVault (args = "(...)" o NULL)
static void map_type(const Tk* type, const Tk* args, char* out, size_t n, bool* numeric) {
    char t[32];
    snprintf(t, sizeof(t), "%.*s", type->n < 31 ? type->n : 31, type->s);
    for (char* c = t; *c; c++) *c = (char)tolower((unsigned char)*c);
    *numeric = false;
    static const char* const INTS[] = { "tinyint", "smallint", "mediumint", "int", "integer", "year", "bit", NULL };
    for (int i = 0; INTS[i]; i++) if (!strcmp(t, INTS[i])) { snprintf(out, n, "INT"); *numeric = true; return; }
    if (!strcmp(t, "bigint")) { snprintf(out, n, "BIGINT"); *numeric = true; return; }
    if (!strcmp(t, "float") || !strcmp(t, "double") || !strcmp(t, "real")) { snprintf(out, n, "FLOAT"); *numeric = true; return; }
    if (!strcmp(t, "decimal") || !strcmp(t, "numeric") || !strcmp(t, "dec")) { snprintf(out, n, "DECIMAL"); *numeric = true; return; }
    if (!strcmp(t, "bool") || !strcmp(t, "boolean")) { snprintf(out, n, "BOOL"); return; }
    if ((!strcmp(t, "varchar") || !strcmp(t, "char")) && args) { snprintf(out, n, "%s%.*s", t[0] == 'v' ? "VARCHAR" : "CHAR", args->n, args->s); return; }
    if (!strcmp(t, "date")) { snprintf(out, n, "DATE"); return; }
    if (!strcmp(t, "datetime") || !strcmp(t, "timestamp")) { snprintf(out, n, "DATETIME"); return; }
    if (!strcmp(t, "time")) { snprintf(out, n, "VARCHAR(16)"); return; }
    if ((!strcmp(t, "enum") || !strcmp(t, "set")) && args) {     // il valore più lungo (SET: tutti insieme)
        Tk in[256];
        int k = tokenize(args->s + 1, args->n - 2, in, 256), mx = 1, sum = 0;
        for (int i = 0; i < k; i++) if (in[i].t == T_STR) { int l = in[i].n - 2; if (l > mx) mx = l; sum += l + 1; }
        snprintf(out, n, "VARCHAR(%d)", t[0] == 's' ? (sum > 1 ? sum : 1) : mx);
        return;
    }
    if (strstr(t, "blob") || strstr(t, "binary")) { snprintf(out, n, "BLOB"); return; }
    snprintf(out, n, "TEXT");                       // text, tinytext, mediumtext, longtext, json, ...
}

static void tk_put(Buf* b, const Tk* t) { put(b, t->s, (size_t)t->n); }

// una colonna: nome tipo[(args)] attributi...
static void column(Buf* b, const Tk* tk, int k) {
    if (k < 2) { for (int i = 0; i < k; i++) { tk_put(b, &tk[i]); puts_(b, " "); } return; }
    tk_put(b, &tk[0]);
    int i = 1;
    const Tk* args = i + 1 < k && tk[i + 1].t == T_PAREN ? &tk[i + 1] : NULL;
    char type[64];
    bool numeric;
    map_type(&tk[i], args, type, sizeof(type), &numeric);
    puts_(b, " ");
    puts_(b, type);
    i += args ? 2 : 1;
    while (i < k) {
        const Tk* t = &tk[i];
        if (is(t, "unsigned") || is(t, "signed") || is(t, "zerofill")) { i++; continue; }
        if (is(t, "CHARACTER") && i + 2 < k) { i += 3; continue; }
        if ((is(t, "CHARSET") || is(t, "COLLATE") || is(t, "COMMENT") || is(t, "USING")) && i + 1 < k) { i += 2; continue; }
        if (is(t, "ON") && i + 1 < k && is(&tk[i + 1], "UPDATE")) { i += 3; if (i < k && tk[i].t == T_PAREN) i++; continue; }
        if (is(t, "GENERATED") || is(t, "AS") || is(t, "STORED") || is(t, "VIRTUAL") || is(t, "ALWAYS")) { i++; if (i < k && tk[i].t == T_PAREN) i++; continue; }
        if (is(t, "DEFAULT") && i + 1 < k) {
            const Tk* v = &tk[i + 1];
            i += 2;
            if (i < k && tk[i].t == T_PAREN) i++;
            if (is(v, "CURRENT_TIMESTAMP") || is(v, "NOW") || is(v, "CURRENT_DATE") || is(v, "LOCALTIMESTAMP")) continue;
            puts_(b, " DEFAULT ");
            if (numeric && v->t == T_STR && v->n > 2) put(b, v->s + 1, (size_t)v->n - 2);   // '0.00' -> 0.00
            else tk_put(b, v);
            continue;
        }
        puts_(b, " ");
        tk_put(b, t);
        i++;
    }
}

// CREATE TABLE di MySQL -> SegVault (+ CREATE INDEX dopo)
static void create_table(Buf* out, const char* s, int n, MyImportStats* st) {
    Tk tk[16];
    int k = tokenize(s, n, tk, 16), body = -1;
    for (int i = 0; i < k; i++) if (tk[i].t == T_PAREN) { body = i; break; }
    if (body < 3) { put(out, s, (size_t)n); puts_(out, ";\n"); return; }
    const Tk* name = &tk[body - 1];
    for (int i = 0; i < body; i++) { tk_put(out, &tk[i]); puts_(out, " "); }
    puts_(out, "(\n");
    Buf post = {0};
    // elementi separati dalle virgole al primo livello
    const char* p = tk[body].s + 1;
    const char* end = tk[body].s + tk[body].n - 1;
    bool first = true;
    while (p < end) {
        const char* q = p;
        int d = 0;
        for (; q < end; q++) {
            if (*q == '\'' || *q == '"' || *q == '`') { q = tk[body].s + skip_quoted(tk[body].s, (int)(q - tk[body].s), tk[body].n, *q) - 1; continue; }
            if (*q == '(') d++;
            else if (*q == ')') d--;
            else if (*q == ',' && d == 0) break;
        }
        Tk it[128];
        int m = tokenize(p, (int)(q - p), it, 128);
        p = q + 1;
        if (!m) continue;
        Buf item = {0};
        if (is(&it[0], "KEY") || is(&it[0], "INDEX") || is(&it[0], "FULLTEXT") || is(&it[0], "SPATIAL")) {
            int pi = -1;
            for (int i = 0; i < m; i++) if (it[i].t == T_PAREN) { pi = i; break; }
            char col[80];
            if ((is(&it[0], "KEY") || is(&it[0], "INDEX")) && pi > 0 && paren_cols(&it[pi], col, sizeof(col)) == 1) {
                char line[400];
                if (pi >= 2) snprintf(line, sizeof(line), "CREATE INDEX %.*s ON %.*s (%s);\n", it[1].n, it[1].s, name->n, name->s, col);
                else snprintf(line, sizeof(line), "CREATE INDEX ix_%.*s_%s ON %.*s (%s);\n", name->n, name->s, col, name->n, name->s, col);
                puts_(&post, line);
            } else st->indexes_dropped++;
            continue;
        }
        if (is(&it[0], "UNIQUE")) {
            int pi = -1;
            for (int i = 0; i < m; i++) if (it[i].t == T_PAREN) { pi = i; break; }
            if (pi < 0) continue;
            int ni = pi - 1;
            if (ni >= 1 && !is(&it[ni], "KEY") && !is(&it[ni], "INDEX") && !is(&it[ni], "UNIQUE")) {
                puts_(&item, "CONSTRAINT "); tk_put(&item, &it[ni]); puts_(&item, " ");
            }
            puts_(&item, "UNIQUE ");
            tk_put(&item, &it[pi]);
        } else if (is(&it[0], "PRIMARY") || is(&it[0], "CONSTRAINT") || is(&it[0], "FOREIGN") || is(&it[0], "CHECK")) {
            for (int i = 0; i < m; i++) {
                if (is(&it[i], "USING") && i + 1 < m) { i++; continue; }
                tk_put(&item, &it[i]);
                puts_(&item, " ");
            }
        } else column(&item, it, m);
        if (item.p) {
            puts_(out, first ? "    " : ",\n    ");
            puts_(out, item.p);
            first = false;
        }
        free(item.p);
    }
    puts_(out, "\n);\n");                    // le opzioni della tabella (ENGINE=, CHARSET, ...) restano fuori
    if (post.p) { puts_(out, post.p); st->statements += 1; }
    free(post.p);
    if (post.oom || out->oom) out->oom = true;
}

char* mysql_translate(const char* sql, MyImportStats* st) {
    MyImportStats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    int len = (int)strlen(sql), cap = 256, n;
    SqlRange* r = NULL;
    for (;;) {
        free(r);
        r = malloc(sizeof(SqlRange) * (size_t)cap);
        if (!r) return NULL;
        n = sql_split(sql, len, r, cap);
        if (n < cap) break;
        cap *= 2;
    }
    Buf out = {0};
    puts_(&out, "-- translated from a MySQL dump by SegVault\n");
    for (int i = 0; i < n; i++) {
        if (!r[i].has_code) continue;
        const char* s = sql + r[i].code_start;
        int sn = r[i].end - r[i].code_start;
        Tk tk[6];
        int k = tokenize(s, sn, tk, 6);
        if (!k) { st->skipped++; continue; }                        // solo /*! ... */
        if (is(&tk[0], "SET") || is(&tk[0], "LOCK") || is(&tk[0], "UNLOCK") ||
            (is(&tk[0], "ALTER") && k >= 4 && (is(&tk[3], "DISABLE") || is(&tk[3], "ENABLE")))) { st->skipped++; continue; }
        st->statements++;
        if (is(&tk[0], "START") && k >= 2 && is(&tk[1], "TRANSACTION")) { puts_(&out, "BEGIN;\n"); continue; }
        if (is(&tk[0], "CREATE") && k >= 2 && is(&tk[1], "TABLE")) { create_table(&out, s, sn, st); continue; }
        put(&out, s, (size_t)sn);
        puts_(&out, ";\n");
    }
    free(r);
    if (out.oom) { free(out.p); return NULL; }
    return out.p;
}
