#include "sqljson.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sv_alloc.h"

static const char* ws(const char* p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static const char* skip_value(const char* p, int depth);

static const char* skip_string(const char* p) {
    if (*p != '"') return NULL;
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) return NULL; continue; }
        if (*p == '"') return p + 1;
        if ((unsigned char)*p < 0x20) return NULL;
    }
    return NULL;
}

static const char* skip_number(const char* p) {
    const char* s = p;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return NULL;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return NULL; while (isdigit((unsigned char)*p)) p++; }
    if (*p == 'e' || *p == 'E') { p++; if (*p == '+' || *p == '-') p++; if (!isdigit((unsigned char)*p)) return NULL; while (isdigit((unsigned char)*p)) p++; }
    return p > s ? p : NULL;
}

static const char* skip_value(const char* p, int depth) {
    if (depth > 128) return NULL;
    p = ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        bool obj = *p == '{';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (obj) {
                p = skip_string(ws(p));
                if (!p) return NULL;
                p = ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = skip_value(p, depth + 1);
            if (!p) return NULL;
            p = ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    return skip_number(p);
}

bool sj_valid(const char* doc) {
    if (!doc) return false;
    const char* e = skip_value(doc, 0);
    return e && !*ws(e);
}

// chiave di un membro (stringa JSON in p..e) uguale a key (lunghezza kn)?
static bool key_equals(const char* p, const char* e, const char* key, size_t kn) {
    char* k = sj_unquote(p, (size_t)(e - p));
    bool eq = k && strlen(k) == kn && !memcmp(k, key, kn);
    free(k);
    return eq;
}

bool sj_extract(const char* doc, const char* path, const char** start, size_t* len) {
    if (!doc || !path) return false;
    if (!sj_valid(doc)) return false;
    const char* p = ws(doc);
    const char* q = path;
    while (*q == ' ') q++;
    if (*q != '$') return false;
    q++;
    while (*q) {
        if (*q == '.') {                                   // .chiave o ."chiave"
            q++;
            char key[256];
            size_t kn = 0;
            if (*q == '"') {
                for (q++; *q && *q != '"' && kn < sizeof(key) - 1; q++) key[kn++] = *q;
                if (*q != '"') return false;
                q++;
            } else {
                while (*q && *q != '.' && *q != '[' && kn < sizeof(key) - 1) key[kn++] = *q++;
            }
            if (!kn || *p != '{') return false;
            const char* m = ws(p + 1);
            bool found = false;
            while (*m && *m != '}') {
                const char* ke = skip_string(m);
                if (!ke) return false;
                bool eq = key_equals(m, ke, key, kn);
                const char* v = ws(ws(ke) + 1);            // dopo ':'
                const char* ve = skip_value(v, 0);
                if (!ve) return false;
                if (eq) { p = v; found = true; break; }
                m = ws(ve);
                if (*m == ',') m = ws(m + 1);
            }
            if (!found) return false;
        } else if (*q == '[') {                            // [n]
            q++;
            char* end;
            long idx = strtol(q, &end, 10);
            if (end == q || *end != ']' || idx < 0) return false;
            q = end + 1;
            if (*p != '[') return false;
            const char* m = ws(p + 1);
            long i = 0;
            bool found = false;
            while (*m && *m != ']') {
                const char* ve = skip_value(m, 0);
                if (!ve) return false;
                if (i++ == idx) { p = m; found = true; break; }
                m = ws(ve);
                if (*m == ',') m = ws(m + 1);
            }
            if (!found) return false;
        } else return false;
    }
    const char* e = skip_value(p, 0);
    if (!e) return false;
    *start = p;
    *len = (size_t)(e - p);
    return true;
}

const char* sj_type(const char* v, size_t len) {
    if (!len) return NULL;
    switch (*v) {
        case '{': return "OBJECT";
        case '[': return "ARRAY";
        case '"': return "STRING";
        case 't': case 'f': return "BOOLEAN";
        case 'n': return "NULL";
        default: return memchr(v, '.', len) || memchr(v, 'e', len) || memchr(v, 'E', len) ? "DOUBLE" : "INTEGER";
    }
}

int sj_length(const char* v, size_t len) {
    (void)len;
    if (*v != '{' && *v != '[') return 1;
    bool obj = *v == '{';
    char close = obj ? '}' : ']';
    const char* m = ws(v + 1);
    int n = 0;
    while (*m && *m != close) {
        if (obj) { m = skip_string(m); if (!m) return -1; m = ws(ws(m) + 1); }
        const char* e = skip_value(m, 0);
        if (!e) return -1;
        n++;
        m = ws(e);
        if (*m == ',') m = ws(m + 1);
    }
    return n;
}

static void put_utf8(char* out, size_t* k, unsigned cp) {
    if (cp < 0x80) out[(*k)++] = (char)cp;
    else if (cp < 0x800) { out[(*k)++] = (char)(0xC0 | (cp >> 6)); out[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out[(*k)++] = (char)(0xE0 | (cp >> 12)); out[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
    else { out[(*k)++] = (char)(0xF0 | (cp >> 18)); out[(*k)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
}

char* sj_unquote(const char* v, size_t len) {
    char* out = sv_malloc(len + 1);
    if (!out) return NULL;
    if (len < 2 || v[0] != '"' || v[len - 1] != '"') { memcpy(out, v, len); out[len] = '\0'; return out; }
    size_t k = 0;
    for (size_t i = 1; i + 1 < len; i++) {
        char c = v[i];
        if (c != '\\' || i + 2 >= len) { out[k++] = c; continue; }   // backslash finale: tenuto com'è
        c = v[++i];
        switch (c) {
            case 'n': out[k++] = '\n'; break;
            case 't': out[k++] = '\t'; break;
            case 'r': out[k++] = '\r'; break;
            case 'b': out[k++] = '\b'; break;
            case 'f': out[k++] = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                if (i + 4 < len && sscanf(v + i + 1, "%4x", &cp) == 1) {
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < len && v[i + 1] == '\\' && v[i + 2] == 'u') {
                        unsigned lo = 0;
                        if (sscanf(v + i + 3, "%4x", &lo) == 1 && lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    put_utf8(out, &k, cp);
                }
                break;
            }
            default: out[k++] = c; break;              // \" \\ \/
        }
    }
    out[k] = '\0';
    return out;
}

char* sj_quote(const char* s) {
    size_t n = strlen(s);
    char* out = sv_malloc(n * 6 + 3);
    if (!out) return NULL;
    size_t k = 0;
    out[k++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (c == '\t') { out[k++] = '\\'; out[k++] = 't'; }
        else if (c == '\r') { out[k++] = '\\'; out[k++] = 'r'; }
        else if (c < 0x20) k += (size_t)sprintf(out + k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k++] = '"';
    out[k] = '\0';
    return out;
}

// ── modifiche e ricerche (JSON_SET / INSERT / REPLACE / REMOVE / ARRAY_APPEND, JSON_KEYS, JSON_CONTAINS,
//    JSON_SEARCH, JSON_PRETTY, JSON_DEPTH, JSON_MERGE_PATCH) ─────────────────────────────────────────

typedef struct { char* d; size_t n, cap; bool err; } SjBuf;
static void sb_put(SjBuf* b, const char* s, size_t n) {
    if (b->err) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + n + 1) nc *= 2;
        char* nd = sv_realloc(b->d, nc);
        if (!nd) { b->err = true; return; }
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
    b->d[b->n] = '\0';
}
static void sb_puts(SjBuf* b, const char* s) { sb_put(b, s, strlen(s)); }
static char* sb_done(SjBuf* b) { if (b->err) { free(b->d); return NULL; } if (!b->d) return sv_strdup(""); return b->d; }

// ultimo passo del percorso: parent riceve il percorso senza l'ultimo passo
static bool split_last(const char* path, char* parent, size_t pn, char* key, size_t kn, long* idx) {
    const char* q = path;
    while (*q == ' ') q++;
    if (*q != '$') return false;
    const char* last = NULL;
    const char* p = q + 1;
    while (*p) {
        last = p;
        if (*p == '.') {
            p++;
            if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p != '"') return false; p++; }
            else while (*p && *p != '.' && *p != '[') p++;
        } else if (*p == '[') {
            while (*p && *p != ']') p++;
            if (*p != ']') return false;
            p++;
        } else return false;
    }
    if (!last) return false;
    snprintf(parent, pn, "%.*s", (int)(last - q), q);
    key[0] = '\0';
    *idx = -1;
    if (*last == '.') {
        const char* k = last + 1;
        size_t l = 0;
        if (*k == '"') { k++; while (k[l] && k[l] != '"') l++; }
        else while (k[l] && k[l] != '.' && k[l] != '[') l++;
        if (!l || l >= kn) return false;
        memcpy(key, k, l); key[l] = '\0';
    } else {
        char* end;
        *idx = strtol(last + 1, &end, 10);
        if (end == last + 1 || *end != ']' || *idx < 0) return false;
    }
    return true;
}

// dentro il contenitore c: posizione del membro / elemento (con la virgola da togliere per REMOVE)
static bool find_member(const char* c, const char* key, long idx, const char** vs, const char** ve,
                        const char** rs, const char** re) {
    bool obj = *c == '{';
    if ((obj && idx >= 0) || (!obj && (*c != '[' || idx < 0))) return false;
    const char* m = ws(c + 1);
    const char* prev_end = NULL;                     // fine dell'elemento precedente (per togliere ", x")
    long i = 0;
    char close = obj ? '}' : ']';
    while (*m && *m != close) {
        const char* start = m;
        const char* v = m;
        bool hit;
        if (obj) {
            const char* ke = skip_string(m);
            if (!ke) return false;
            hit = key_equals(m, ke, key, strlen(key));
            v = ws(ws(ke) + 1);
        } else hit = i == idx;
        const char* e = skip_value(v, 0);
        if (!e) return false;
        if (hit) {
            *vs = ws(v); *ve = e;
            const char* after = ws(e);
            if (*after == ',') { *rs = start; *re = ws(after + 1); }
            else if (prev_end) { *rs = prev_end; *re = e; }
            else { *rs = start; *re = e; }
            return true;
        }
        i++;
        prev_end = e;
        m = ws(e);
        if (*m == ',') m = ws(m + 1);
    }
    return false;
}

// op: 0 SET, 1 INSERT, 2 REPLACE, 3 REMOVE, 4 ARRAY_APPEND. NULL se documento o percorso non validi
char* sj_modify(const char* doc, const char* path, const char* val, int op) {
    if (!sj_valid(doc)) return NULL;
    if (op == 4) {                                    // aggiunge in fondo all'array (un valore semplice diventa [v, nuovo])
        const char* t; size_t tl;
        if (!sj_extract(doc, path, &t, &tl)) return sv_strdup(doc);
        SjBuf b = {0};
        sb_put(&b, doc, (size_t)(t - doc));
        if (*t == '[') {
            const char* close = t + tl - 1;
            bool empty = *ws(t + 1) == ']';
            sb_put(&b, t, (size_t)(close - t));
            if (!empty) sb_puts(&b, ", ");
            sb_puts(&b, val);
            sb_puts(&b, "]");
        } else {
            sb_puts(&b, "["); sb_put(&b, t, tl); sb_puts(&b, ", "); sb_puts(&b, val); sb_puts(&b, "]");
        }
        sb_puts(&b, t + tl);
        return sb_done(&b);
    }
    char parent[1024], key[256];
    long idx;
    if (!split_last(path, parent, sizeof(parent), key, sizeof(key), &idx)) {
        // "$": SET / REPLACE sostituiscono tutto il documento
        const char* q = path; while (*q == ' ') q++;
        if (!strcmp(q, "$") && (op == 0 || op == 2)) return sv_strdup(val);
        return strcmp(q, "$") ? NULL : sv_strdup(doc);
    }
    const char* c; size_t cl;
    if (!sj_extract(doc, parent, &c, &cl)) return sv_strdup(doc);           // contenitore che non c'è: invariato
    const char *vs, *ve, *rs, *re;
    bool found = find_member(c, key, idx, &vs, &ve, &rs, &re);
    SjBuf b = {0};
    if (found) {
        if (op == 1) return sv_strdup(doc);                                  // INSERT: c'è già
        if (op == 3) { sb_put(&b, doc, (size_t)(rs - doc)); sb_puts(&b, re); return sb_done(&b); }
        sb_put(&b, doc, (size_t)(vs - doc)); sb_puts(&b, val); sb_puts(&b, ve);
        return sb_done(&b);
    }
    if (op == 2 || op == 3) return sv_strdup(doc);                           // REPLACE / REMOVE: non c'è
    bool obj = *c == '{';
    if ((obj && idx >= 0) || (!obj && *c != '[') || (!obj && idx < 0)) return sv_strdup(doc);
    const char* close = c + cl - 1;                                       // nuovo membro / elemento in fondo
    bool empty = *ws(c + 1) == (obj ? '}' : ']');
    sb_put(&b, doc, (size_t)(close - doc));
    if (!empty) sb_puts(&b, ", ");
    if (obj) { char* qk = sj_quote(key); if (qk) { sb_puts(&b, qk); free(qk); } sb_puts(&b, ": "); }
    sb_puts(&b, val);
    sb_puts(&b, close);
    return sb_done(&b);
}

char* sj_keys(const char* v, size_t len) {
    (void)len;
    if (*v != '{') return NULL;
    SjBuf b = {0};
    sb_puts(&b, "[");
    const char* m = ws(v + 1);
    int n = 0;
    while (*m && *m != '}') {
        const char* ke = skip_string(m);
        if (!ke) { free(b.d); return NULL; }
        if (n++) sb_puts(&b, ", ");
        sb_put(&b, m, (size_t)(ke - m));
        const char* e = skip_value(ws(ws(ke) + 1), 0);
        if (!e) { free(b.d); return NULL; }
        m = ws(e);
        if (*m == ',') m = ws(m + 1);
    }
    sb_puts(&b, "]");
    return sb_done(&b);
}

// testo JSON senza spazi fuori dalle stringhe (per i confronti)
static char* compact(const char* v, size_t len) {
    char* o = sv_malloc(len + 1);
    if (!o) return NULL;
    size_t k = 0;
    bool str = false;
    for (size_t i = 0; i < len; i++) {
        char ch = v[i];
        if (str) { o[k++] = ch; if (ch == '\\' && i + 1 < len) o[k++] = v[++i]; else if (ch == '"') str = false; continue; }
        if (ch == '"') { str = true; o[k++] = ch; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        o[k++] = ch;
    }
    o[k] = '\0';
    return o;
}

static bool sj_eq(const char* a, size_t al, const char* b, size_t bl) {
    char* x = compact(a, al);
    char* y = compact(b, bl);
    bool eq = x && y && (!strcmp(x, y) || (strtod(x, NULL) == strtod(y, NULL) && isdigit((unsigned char)(*x == '-' ? x[1] : *x)) &&
                                           isdigit((unsigned char)(*y == '-' ? y[1] : *y))));
    free(x); free(y);
    return eq;
}

// target contiene candidate? (regole di JSON_CONTAINS di MySQL)
bool sj_contains(const char* t, size_t tl, const char* c, size_t cl) {
    t = ws(t); c = ws(c);
    if (*t == '[') {
        if (*c == '[') {                                   // ogni elemento del candidato è nell'array
            const char* m = ws(c + 1);
            while (*m && *m != ']') {
                const char* e = skip_value(m, 0);
                if (!e || !sj_contains(t, tl, m, (size_t)(e - m))) return false;
                m = ws(e); if (*m == ',') m = ws(m + 1);
            }
            return true;
        }
        const char* m = ws(t + 1);                         // un elemento contiene il candidato
        while (*m && *m != ']') {
            const char* e = skip_value(m, 0);
            if (!e) return false;
            if (sj_contains(m, (size_t)(e - m), c, cl)) return true;
            m = ws(e); if (*m == ',') m = ws(m + 1);
        }
        return false;
    }
    if (*t == '{') {
        if (*c != '{') return false;
        const char* m = ws(c + 1);                         // ogni membro del candidato è nell'oggetto
        while (*m && *m != '}') {
            const char* ke = skip_string(m);
            if (!ke) return false;
            const char* v = ws(ws(ke) + 1);
            const char* e = skip_value(v, 0);
            if (!e) return false;
            char* k = sj_unquote(m, (size_t)(ke - m));
            const char *vs, *ve, *rs, *re;
            bool ok = k && find_member(t, k, -1, &vs, &ve, &rs, &re) && sj_contains(vs, (size_t)(ve - vs), v, (size_t)(e - v));
            free(k);
            if (!ok) return false;
            m = ws(e); if (*m == ',') m = ws(m + 1);
        }
        return true;
    }
    return sj_eq(t, tl, c, cl);
}

int sj_depth(const char* v) {
    v = ws(v);
    if (*v != '{' && *v != '[') return 1;
    bool obj = *v == '{';
    char close = obj ? '}' : ']';
    const char* m = ws(v + 1);
    int best = 0;
    while (*m && *m != close) {
        if (obj) { m = skip_string(m); if (!m) return -1; m = ws(ws(m) + 1); }
        int d = sj_depth(m);
        if (d > best) best = d;
        const char* e = skip_value(m, 0);
        if (!e) return -1;
        m = ws(e); if (*m == ',') m = ws(m + 1);
    }
    return best + 1;
}

static void pretty(SjBuf* b, const char* v, int ind) {
    v = ws(v);
    if (*v != '{' && *v != '[') { const char* e = skip_value(v, 0); sb_put(b, v, e ? (size_t)(e - v) : strlen(v)); return; }
    bool obj = *v == '{';
    char close = obj ? '}' : ']';
    const char* m = ws(v + 1);
    if (*m == close) { sb_puts(b, obj ? "{}" : "[]"); return; }
    sb_puts(b, obj ? "{\n" : "[\n");
    int n = 0;
    while (*m && *m != close) {
        if (n++) sb_puts(b, ",\n");
        for (int i = 0; i < ind + 2; i++) sb_puts(b, " ");
        if (obj) {
            const char* ke = skip_string(m);
            if (!ke) return;
            sb_put(b, m, (size_t)(ke - m));
            sb_puts(b, ": ");
            m = ws(ws(ke) + 1);
        }
        pretty(b, m, ind + 2);
        const char* e = skip_value(m, 0);
        if (!e) return;
        m = ws(e); if (*m == ',') m = ws(m + 1);
    }
    sb_puts(b, "\n");
    for (int i = 0; i < ind; i++) sb_puts(b, " ");
    sb_put(b, &close, 1);
}

char* sj_pretty(const char* doc) {
    if (!sj_valid(doc)) return NULL;
    SjBuf b = {0};
    pretty(&b, doc, 0);
    return sb_done(&b);
}

// LIKE di MySQL (% e _, \ protegge il carattere dopo) sul testo t. Iterativo, con ritorno solo all'ultimo %:
// la versione ricorsiva scendeva di un livello per carattere (stack overflow con testo e pattern di 100 000
// caratteri) ed era esponenziale con più % (P3.9)
static bool sj_like(const char* t, const char* p) {
    const char *star_p = NULL, *star_t = NULL;
    while (*t) {
        if (*p == '%') {
            while (*p == '%') p++;
            if (!*p) return true;
            star_p = p;
            star_t = t;
            continue;
        }
        bool esc = *p == '\\' && p[1];
        if (*p && ((!esc && *p == '_') || (esc ? p[1] : *p) == *t)) { p += esc ? 2 : 1; t++; continue; }
        if (!star_p) return false;
        p = star_p;
        t = ++star_t;
    }
    while (*p == '%') p++;
    return !*p;
}

static void search(SjBuf* out, int* found, bool all, const char* v, const char* pat, char* path, size_t pl) {
    if (*found && !all) return;
    v = ws(v);
    if (*v == '"') {
        const char* e = skip_string(v);
        char* s = e ? sj_unquote(v, (size_t)(e - v)) : NULL;
        if (s && sj_like(s, pat)) {
            if ((*found)++) sb_puts(out, ", ");
            char* q = sj_quote(path);
            if (q) { sb_puts(out, q); free(q); }
        }
        free(s);
        return;
    }
    if (*v != '{' && *v != '[') return;
    bool obj = *v == '{';
    char close = obj ? '}' : ']';
    const char* m = ws(v + 1);
    long i = 0;
    while (*m && *m != close) {
        size_t base = strlen(path);
        if (obj) {
            const char* ke = skip_string(m);
            if (!ke) return;
            char* k = sj_unquote(m, (size_t)(ke - m));
            bool plain = k && k[0] && strspn(k, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == strlen(k);
            snprintf(path + base, pl - base, plain ? ".%s" : ".\"%s\"", k ? k : "");
            free(k);
            m = ws(ws(ke) + 1);
        } else snprintf(path + base, pl - base, "[%ld]", i);
        search(out, found, all, m, pat, path, pl);
        path[base] = '\0';
        const char* e = skip_value(m, 0);
        if (!e) return;
        m = ws(e); if (*m == ',') m = ws(m + 1);
        i++;
    }
}

// percorsi delle stringhe che corrispondono a pat (LIKE): "$.a" con one, ["$.a", "$.b"] con all; NULL se nessuna
char* sj_search(const char* doc, bool all, const char* pat) {
    if (!sj_valid(doc)) return NULL;
    SjBuf b = {0};
    char path[1024] = "$";
    int found = 0;
    search(&b, &found, all, doc, pat, path, sizeof(path));
    char* r = sb_done(&b);
    if (!r || !found) { free(r); return NULL; }
    if (found == 1 || !all) return r;
    size_t l = strlen(r);
    char* w = sv_malloc(l + 3);
    if (w) { w[0] = '['; memcpy(w + 1, r, l); w[l + 1] = ']'; w[l + 2] = '\0'; }
    free(r);
    return w;
}

// RFC 7396: un oggetto unisce i membri (null li toglie), qualunque altro valore sostituisce
static void merge(SjBuf* b, const char* a, const char* p) {
    a = ws(a); p = ws(p);
    if (*p != '{') { const char* e = skip_value(p, 0); sb_put(b, p, e ? (size_t)(e - p) : 0); return; }
    if (*a != '{') a = "{}";
    sb_puts(b, "{");
    int n = 0;
    const char* m = ws(a + 1);                             // membri di a (aggiornati da p o tolti con null)
    while (*m && *m != '}') {
        const char* ke = skip_string(m);
        if (!ke) return;
        const char* v = ws(ws(ke) + 1);
        const char* e = skip_value(v, 0);
        if (!e) return;
        char* k = sj_unquote(m, (size_t)(ke - m));
        const char *vs, *ve, *rs, *re;
        bool inp = k && find_member(p, k, -1, &vs, &ve, &rs, &re);
        free(k);
        if (!(inp && !strncmp(vs, "null", 4))) {
            if (n++) sb_puts(b, ", ");
            sb_put(b, m, (size_t)(ke - m));
            sb_puts(b, ": ");
            if (inp) merge(b, v, vs); else sb_put(b, v, (size_t)(e - v));
        }
        m = ws(e); if (*m == ',') m = ws(m + 1);
    }
    m = ws(p + 1);                                         // membri nuovi di p
    while (*m && *m != '}') {
        const char* ke = skip_string(m);
        if (!ke) return;
        const char* v = ws(ws(ke) + 1);
        const char* e = skip_value(v, 0);
        if (!e) return;
        char* k = sj_unquote(m, (size_t)(ke - m));
        const char *vs, *ve, *rs, *re;
        bool ina = k && find_member(a, k, -1, &vs, &ve, &rs, &re);
        free(k);
        if (!ina && strncmp(v, "null", 4)) {
            if (n++) sb_puts(b, ", ");
            sb_put(b, m, (size_t)(ke - m));
            sb_puts(b, ": ");
            merge(b, "null", v);
        }
        m = ws(e); if (*m == ',') m = ws(m + 1);
    }
    sb_puts(b, "}");
}

char* sj_merge_patch(const char* a, const char* p) {
    if (!sj_valid(a) || !sj_valid(p)) return NULL;
    SjBuf b = {0};
    merge(&b, a, p);
    return sb_done(&b);
}
