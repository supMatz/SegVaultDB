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
           strstr(sql, "ENGINE=") || strstr(sql, "LOCK TABLES") || strstr(sql, "/*!40101") ||
           !strncasecmp(sql, "DELIMITER ", 10) || strstr(sql, "\nDELIMITER ") || strstr(sql, "\ndelimiter ");
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

// Prima della divisione in istruzioni:
//  - DELIMITER ;; ... ;; (procedure e trigger dei dump): le righe DELIMITER spariscono e il
//    delimitatore scelto diventa ';' (dentro BEGIN ... END i ';' non separano comunque)
//  - i commenti versionati di viste, trigger e procedure (/*!50001 CREATE VIEW ... */,
//    /*!50003 TRIGGER ... */, /*!50013 DEFINER=... */) diventano testo normale: MySQL li esegue.
//    Gli altri (/*!40101 SET ... */, /*!80016 DEFAULT ENCRYPTION ... */) restano commenti.
static bool versioned_ok(const char* p) {
    static const char* const V[] = { "50001", "50003", "50013", "50017", "50020" };
    for (int i = 0; i < 5; i++) if (!strncmp(p, V[i], 5)) return true;
    return false;
}

static char* mysql_prepare(const char* in) {
    size_t n = strlen(in);
    Buf out = {0};
    char delim[16] = ";";
    size_t dl = 1;
    bool line_start = true;
    for (size_t i = 0; i < n; ) {
        char c = in[i];
        if (line_start) {                                    // DELIMITER x  (a inizio riga)
            size_t j = i;
            while (j < n && (in[j] == ' ' || in[j] == '\t')) j++;
            if (n - j > 10 && !strncasecmp(in + j, "DELIMITER", 9) && (in[j + 9] == ' ' || in[j + 9] == '\t')) {
                j += 9;
                while (j < n && (in[j] == ' ' || in[j] == '\t')) j++;
                size_t k = 0;
                while (j < n && !isspace((unsigned char)in[j]) && k < sizeof(delim) - 1) delim[k++] = in[j++];
                if (k) { delim[k] = '\0'; dl = k; }
                while (j < n && in[j] != '\n') j++;
                i = j < n ? j + 1 : j;
                continue;
            }
        }
        line_start = false;
        if (c == '\'' || c == '"' || c == '`') {             // stringhe e nomi: copiati così come sono
            size_t j = i + 1;
            while (j < n && in[j] != c) { if (in[j] == '\\' && c != '`' && j + 1 < n) j++; j++; }
            if (j < n) j++;
            put(&out, in + i, j - i);
            i = j;
            continue;
        }
        if (c == '-' && i + 1 < n && in[i + 1] == '-') {     // commento di riga
            size_t j = i;
            while (j < n && in[j] != '\n') j++;
            put(&out, in + i, j - i);
            i = j;
            continue;
        }
        if (c == '/' && i + 2 < n && in[i + 1] == '*') {
            const char* e = strstr(in + i + 2, "*/");
            size_t j = e ? (size_t)(e - in) + 2 : n;
            // anche /*!40014 SET ... FOREIGN_KEY_CHECKS ... */: i dati di mysqldump arrivano in qualunque ordine
            bool fk = false;
            for (size_t q = i + 3; e && q + 18 <= (size_t)(e - in); q++) if (!strncasecmp(in + q, "FOREIGN_KEY_CHECKS", 18)) { fk = true; break; }
            if (in[i + 2] == '!' && i + 8 <= n && (versioned_ok(in + i + 3) || fk)) {
                puts_(&out, " ");
                put(&out, in + i + 8, (e ? (size_t)(e - in) : n) - (i + 8));
                puts_(&out, " ");
            } else put(&out, in + i, j - i);
            i = j;
            continue;
        }
        if (dl > 1 || delim[0] != ';') {
            if (!strncmp(in + i, delim, dl)) { puts_(&out, ";"); i += dl; continue; }
        }
        if (c == '\n') line_start = true;
        put(&out, in + i, 1);
        i++;
    }
    if (out.oom) { free(out.p); return NULL; }
    return out.p ? out.p : strdup("");
}

char* mysql_translate(const char* sql_in, MyImportStats* st) {
    MyImportStats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    char* prepared = mysql_prepare(sql_in);
    if (!prepared) return NULL;
    const char* sql = prepared;
    int len = (int)strlen(sql), cap = 256, n;
    SqlRange* r = NULL;
    for (;;) {
        free(r);
        r = malloc(sizeof(SqlRange) * (size_t)cap);
        if (!r) { free(prepared); return NULL; }
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
        // SET FOREIGN_KEY_CHECKS resta: con i controlli spenti i dati possono arrivare in qualunque ordine
        bool fkset = false;                     // SET ... FOREIGN_KEY_CHECKS = ... (anche dopo @OLD_... = @@...)
        for (int q = 0; is(&tk[0], "SET") && q + 18 <= sn; q++) if (!strncasecmp(s + q, "FOREIGN_KEY_CHECKS", 18)) { fkset = true; break; }
        if (fkset) {
            st->statements++;
            put(&out, s, (size_t)sn);
            puts_(&out, ";\n");
            continue;
        }
        if (is(&tk[0], "SET") || is(&tk[0], "LOCK") || is(&tk[0], "UNLOCK") ||
            (is(&tk[0], "ALTER") && k >= 4 && (is(&tk[3], "DISABLE") || is(&tk[3], "ENABLE")))) { st->skipped++; continue; }
        st->statements++;
        if (is(&tk[0], "START") && k >= 2 && is(&tk[1], "TRANSACTION")) { puts_(&out, "BEGIN;\n"); continue; }
        // CREATE TABLE: il motore capisce il dialetto di MySQL (tipi, KEY, FULLTEXT, ENGINE=, ...): passa così com'è
        put(&out, s, (size_t)sn);
        puts_(&out, ";\n");
    }
    free(r);
    free(prepared);
    if (out.oom) { free(out.p); return NULL; }
    return out.p;
}
