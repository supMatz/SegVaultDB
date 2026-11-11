#include "sqljson.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char* out = malloc(len + 1);
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
    char* out = malloc(n * 6 + 3);
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
