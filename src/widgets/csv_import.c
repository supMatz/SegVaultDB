#include "csv_import.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define CSV_MAX_COLS   64      // limite del motore per INSERT
#define CSV_MAX_FIELD  500     // limite del motore per un valore

typedef struct { char* p; size_t n, cap; bool ok; } Buf;

static void put(Buf* b, const char* s, size_t n) {
    if (!b->ok) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap * 2 + n + 4096;
        char* p = realloc(b->p, cap);
        if (!p) { b->ok = false; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void puts_(Buf* b, const char* s) { put(b, s, strlen(s)); }

// ── lettura dei record ───────────────────────────────────────────

typedef struct {
    const char* s;
    char delim;
    int  line;
} Reader;

typedef struct {
    char* f[CSV_MAX_COLS];
    bool  quoted[CSV_MAX_COLS];
    int   n;
    bool  too_many;
} Record;

static void record_clear(Record* r) {
    for (int i = 0; i < r->n; i++) free(r->f[i]);
    r->n = 0;
    r->too_many = false;
}

// legge un record; false a fine file
static bool read_record(Reader* rd, Record* rec) {
    record_clear(rec);
    if (!*rd->s) return false;
    Buf field = { NULL, 0, 0, true };
    bool quoted = false, in_q = false;
    for (;;) {
        char c = *rd->s;
        if (in_q) {
            if (!c) break;                                   // virgolette non chiuse: fine file
            if (c == '"') {
                if (rd->s[1] == '"') { put(&field, "\"", 1); rd->s += 2; continue; }
                in_q = false; rd->s++; continue;
            }
            if (c == '\n') rd->line++;
            put(&field, &c, 1);
            rd->s++;
            continue;
        }
        if (c == '"' && field.n == 0 && !quoted) { in_q = quoted = true; rd->s++; continue; }
        if (c == rd->delim || c == '\n' || c == '\r' || !c) {
            if (rec->n < CSV_MAX_COLS) {
                rec->f[rec->n] = field.p ? field.p : calloc(1, 1);
                rec->quoted[rec->n] = quoted;
                rec->n++;
            } else {
                free(field.p);
                rec->too_many = true;
            }
            field = (Buf){ NULL, 0, 0, true };
            quoted = false;
            if (c == rd->delim) { rd->s++; continue; }
            if (c == '\r') rd->s++;
            if (*rd->s == '\n') { rd->s++; rd->line++; }
            return true;
        }
        put(&field, &c, 1);
        rd->s++;
    }
    // virgolette mai chiuse: il resto del file è l'ultimo campo
    if (rec->n < CSV_MAX_COLS) {
        rec->f[rec->n] = field.p ? field.p : calloc(1, 1);
        rec->quoted[rec->n] = quoted;
        rec->n++;
    } else {
        free(field.p);
        rec->too_many = true;
    }
    return true;
}

static bool blank_record(const Record* r) { return r->n == 1 && !r->f[0][0] && !r->quoted[0]; }

// separatore più frequente fuori dalle virgolette nella prima riga
static char detect_delimiter(const char* s) {
    int counts[3] = {0, 0, 0};
    const char cand[3] = {',', ';', '\t'};
    bool q = false;
    for (; *s && (q || (*s != '\n' && *s != '\r')); s++) {
        if (*s == '"') q = !q;
        else if (!q) for (int k = 0; k < 3; k++) if (*s == cand[k]) counts[k]++;
    }
    int best = 0;
    for (int k = 1; k < 3; k++) if (counts[k] > counts[best]) best = k;
    return cand[best];
}

// ── tipi ─────────────────────────────────────────────────────────

typedef enum { T_NONE, T_INT, T_BIGINT, T_FLOAT, T_TEXT } ColType;

static ColType value_type(const char* v) {
    const char* s = v;
    if (*s == '-' || *s == '+') s++;
    if (!isdigit((unsigned char)*s)) return T_TEXT;
    bool dot = false, exp = false;
    for (const char* p = s; *p; p++) {
        if (isdigit((unsigned char)*p)) continue;
        if (*p == '.' && !dot && !exp) { dot = true; continue; }
        if ((*p == 'e' || *p == 'E') && !exp && isdigit((unsigned char)p[1 + (p[1] == '-' || p[1] == '+')])) {
            exp = true;
            if (p[1] == '-' || p[1] == '+') p++;
            continue;
        }
        return T_TEXT;
    }
    if (dot || exp) return T_FLOAT;
    // zeri iniziali (CAP, codici, telefoni): resta testo per non perdere lo zero
    if (s[0] == '0' && s[1]) return T_TEXT;
    errno = 0;
    long long x = strtoll(v, NULL, 10);
    if (errno == ERANGE) return T_TEXT;
    return (x >= INT_MIN && x <= INT_MAX) ? T_INT : T_BIGINT;
}

static ColType merge(ColType a, ColType b) {
    if (a == T_NONE) return b;
    if (b == T_NONE) return a;
    if (a == T_TEXT || b == T_TEXT) return T_TEXT;
    if (a == T_FLOAT || b == T_FLOAT) return T_FLOAT;
    if (a == T_BIGINT || b == T_BIGINT) return T_BIGINT;
    return T_INT;
}

// ── nomi ─────────────────────────────────────────────────────────

// lettera latina accentata (UTF-8, 2 byte) -> lettera ASCII; 0 se non riconosciuta
static char fold_accent(unsigned char a, unsigned char b) {
    if (a != 0xC3) return 0;
    static const char MAP[65] =   // U+00C0 .. U+00FF
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    char c = MAP[b & 0x3F];
    return (c == 'x' || c == '/') ? 0 : c;
}

static void sanitize_name(const char* in, char* out, int n, int index) {
    int k = 0;
    bool us = false;
    for (const unsigned char* p = (const unsigned char*)in; *p && k < n - 1; p++) {
        char f = (p[0] >= 0xC0 && p[1]) ? fold_accent(p[0], p[1]) : 0;
        if (f) { out[k++] = (char)tolower((unsigned char)f); us = false; p++; continue; }
        if (isalnum(*p)) { out[k++] = (char)tolower(*p); us = false; }
        else if (k > 0 && !us) { out[k++] = '_'; us = true; }
    }
    while (k > 0 && out[k - 1] == '_') k--;
    out[k] = '\0';
    if (!k) snprintf(out, n, "col%d", index + 1);
    else if (isdigit((unsigned char)out[0])) {   // un identificatore non può iniziare con una cifra
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "c_%s", out);
        snprintf(out, n, "%s", tmp);
    }
}

void csv_table_name_from_path(const char* path, char* out, int n) {
    const char* b = path;
    for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", b);
    char* dot = strrchr(tmp, '.');
    if (dot) *dot = '\0';
    sanitize_name(tmp, out, n, 0);
    if (!strcmp(out, "col1")) snprintf(out, n, "imported");
}

static void sql_quote(Buf* b, const char* v) {
    put(b, "'", 1);
    for (const char* p = v; *p; p++) {
        if (*p == '\'') put(b, "''", 2);
        else if (*p == '\\') put(b, "\\\\", 2);   // il lexer tratta \ come escape
        else put(b, p, 1);
    }
    put(b, "'", 1);
}

// ── conversione ──────────────────────────────────────────────────

char* csv_to_sql(const char* csv, const char* table, bool create_table, char delimiter, CsvImportInfo* info) {
    CsvImportInfo dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof(*info));
    if (!csv || !table || !table[0]) { snprintf(info->error, sizeof(info->error), "Missing CSV text or table name"); return NULL; }
    if ((unsigned char)csv[0] == 0xEF && (unsigned char)csv[1] == 0xBB && (unsigned char)csv[2] == 0xBF) csv += 3;  // BOM

    Reader rd = { csv, delimiter ? delimiter : detect_delimiter(csv), 1 };
    info->delimiter = rd.delim;
    Record hdr = {0}, rec = {0};
    while (read_record(&rd, &hdr) && blank_record(&hdr)) {}
    if (hdr.n == 0 || blank_record(&hdr)) {
        record_clear(&hdr);
        snprintf(info->error, sizeof(info->error), "The file is empty");
        return NULL;
    }
    if (hdr.too_many) {
        record_clear(&hdr);
        snprintf(info->error, sizeof(info->error), "Too many columns (the limit is %d)", CSV_MAX_COLS);
        return NULL;
    }
    int nc = hdr.n;
    info->cols = nc;
    char names[CSV_MAX_COLS][64];
    for (int c = 0; c < nc; c++) {
        sanitize_name(hdr.f[c], names[c], sizeof(names[c]), c);
        for (int k = 0; k < c; k++)                       // nomi duplicati: suffisso _2, _3, ...
            if (!strcmp(names[k], names[c])) {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%.50s_%d", names[c], c + 1);
                snprintf(names[c], sizeof(names[c]), "%s", tmp);
                k = -1;
            }
    }
    record_clear(&hdr);

    // prima passata: tipi e lunghezze
    ColType types[CSV_MAX_COLS] = {0};
    int maxlen[CSV_MAX_COLS] = {0};
    const char* data_start = rd.s;
    int data_line = rd.line;
    while (read_record(&rd, &rec)) {
        if (blank_record(&rec)) continue;
        if (rec.n != nc || rec.too_many) {
            snprintf(info->error, sizeof(info->error), "Line %d has %d field%s, the header has %d",
                     rd.line - 1, rec.n, rec.n == 1 ? "" : "s", nc);
            record_clear(&rec);
            return NULL;
        }
        for (int c = 0; c < nc; c++) {
            int len = (int)strlen(rec.f[c]);
            if (len > CSV_MAX_FIELD) {
                snprintf(info->error, sizeof(info->error), "Line %d, column %s: value longer than %d bytes",
                         rd.line - 1, names[c], CSV_MAX_FIELD);
                record_clear(&rec);
                return NULL;
            }
            if (len > maxlen[c]) maxlen[c] = len;
            if (len == 0 && !rec.quoted[c]) continue;          // vuoto = NULL
            types[c] = merge(types[c], rec.quoted[c] ? T_TEXT : value_type(rec.f[c]));
        }
        info->rows++;
    }
    record_clear(&rec);

    Buf b = { NULL, 0, 0, true };
    char line[256];
    if (create_table) {
        snprintf(line, sizeof(line), "CREATE TABLE %.200s (", table);
        puts_(&b, line);
        for (int c = 0; c < nc; c++) {
            const char* t;
            char vb[32];
            switch (types[c]) {
                case T_INT:    t = "INT"; break;
                case T_BIGINT: t = "BIGINT"; break;
                case T_FLOAT:  t = "FLOAT"; break;
                default: {
                    int n = maxlen[c] <= 32 ? 32 : maxlen[c] <= 64 ? 64 : maxlen[c] <= 255 ? 255 : 0;
                    if (n) { snprintf(vb, sizeof(vb), "VARCHAR(%d)", n); t = vb; } else t = "TEXT";
                }
            }
            snprintf(line, sizeof(line), "%s\n    %.63s %.31s", c ? "," : "", names[c], t);
            puts_(&b, line);
        }
        puts_(&b, "\n);\n");
    }

    // seconda passata: INSERT
    Buf head = { NULL, 0, 0, true };
    snprintf(line, sizeof(line), "INSERT INTO %.200s (", table);
    puts_(&head, line);
    for (int c = 0; c < nc; c++) { if (c) puts_(&head, ", "); puts_(&head, names[c]); }
    puts_(&head, ") VALUES (");
    rd.s = data_start;
    rd.line = data_line;
    while (head.ok && read_record(&rd, &rec)) {
        if (blank_record(&rec)) continue;
        puts_(&b, head.p);
        for (int c = 0; c < nc; c++) {
            if (c) puts_(&b, ", ");
            const char* v = rec.f[c];
            if (!v[0] && !rec.quoted[c]) puts_(&b, "NULL");
            else if (types[c] == T_INT || types[c] == T_BIGINT || types[c] == T_FLOAT) puts_(&b, v[0] == '+' ? v + 1 : v);
            else sql_quote(&b, v);
        }
        puts_(&b, ");\n");
    }
    record_clear(&rec);
    free(head.p);
    if (!b.ok || !head.ok) {
        free(b.p);
        snprintf(info->error, sizeof(info->error), "Out of memory");
        return NULL;
    }
    return b.p;
}
