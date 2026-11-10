#include "json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── lettura ──────────────────────────────────────────────────────

#define JSON_MAX_DEPTH 64

typedef struct {
    const char* s;
    size_t      i, n;
    char*       err;
    size_t      errsz;
    bool        failed;
} JP;

static void jp_fail(JP* p, const char* msg) {
    if (!p->failed) snprintf(p->err, p->errsz, "%s at byte %zu", msg, p->i);
    p->failed = true;
}

static void jp_ws(JP* p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

static bool jp_lit(JP* p, const char* w) {
    size_t l = strlen(w);
    if (p->n - p->i < l || memcmp(p->s + p->i, w, l)) return false;
    p->i += l;
    return true;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool jp_hex4(JP* p, unsigned* out) {
    if (p->n - p->i < 4) return false;
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(p->s[p->i + k]);
        if (h < 0) return false;
        v = v * 16 + (unsigned)h;
    }
    p->i += 4;
    *out = v;
    return true;
}

static size_t utf8_put(char* o, unsigned cp) {
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// stringa: p->i sull'apice di apertura
static char* jp_string(JP* p, size_t* out_len) {
    p->i++;
    // il risultato non è mai più lungo del testo sorgente (un escape \uXXXX dà al più 4 byte su 6)
    size_t start = p->i, end = start;
    while (end < p->n && p->s[end] != '"') end += (p->s[end] == '\\') ? 2 : 1;
    char* o = malloc(end - start + 1);
    if (!o) { jp_fail(p, "out of memory"); return NULL; }
    size_t k = 0;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') { p->i++; o[k] = '\0'; *out_len = k; return o; }
        if (c < 0x20) { jp_fail(p, "control character in string"); break; }
        if (c != '\\') { o[k++] = (char)c; p->i++; continue; }
        if (++p->i >= p->n) break;
        char e = p->s[p->i++];
        switch (e) {
            case '"': o[k++] = '"'; break;
            case '\\': o[k++] = '\\'; break;
            case '/': o[k++] = '/'; break;
            case 'b': o[k++] = '\b'; break;
            case 'f': o[k++] = '\f'; break;
            case 'n': o[k++] = '\n'; break;
            case 'r': o[k++] = '\r'; break;
            case 't': o[k++] = '\t'; break;
            case 'u': {
                unsigned cp;
                if (!jp_hex4(p, &cp)) { jp_fail(p, "bad \\u escape"); goto fail; }
                if (cp >= 0xD800 && cp <= 0xDBFF) {           // coppia surrogata
                    unsigned lo;
                    if (p->n - p->i < 6 || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') { jp_fail(p, "lone surrogate"); goto fail; }
                    p->i += 2;
                    if (!jp_hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF) { jp_fail(p, "bad surrogate pair"); goto fail; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) { jp_fail(p, "lone surrogate"); goto fail; }
                k += utf8_put(o + k, cp);
                break;
            }
            default: jp_fail(p, "bad escape"); goto fail;
        }
    }
    jp_fail(p, "unterminated string");
fail:
    free(o);
    return NULL;
}

static bool jp_value(JP* p, JsonVal* v, int depth);
static void json_free_inner(JsonVal* v);

static bool jp_push(JP* p, JsonVal* v, int* cap) {
    if (v->count < *cap) return true;
    int nc = *cap ? *cap * 2 : 8;
    JsonVal* ni = realloc(v->items, sizeof(JsonVal) * (size_t)nc);
    if (!ni) { jp_fail(p, "out of memory"); return false; }
    v->items = ni;
    if (v->type == JSON_OBJECT) {
        char** nk = realloc(v->keys, sizeof(char*) * (size_t)nc);
        if (!nk) { jp_fail(p, "out of memory"); return false; }
        v->keys = nk;
    }
    *cap = nc;
    return true;
}

static bool jp_value(JP* p, JsonVal* v, int depth) {
    memset(v, 0, sizeof(*v));
    if (depth > JSON_MAX_DEPTH) { jp_fail(p, "nesting too deep"); return false; }
    jp_ws(p);
    if (p->i >= p->n) { jp_fail(p, "unexpected end"); return false; }
    char c = p->s[p->i];
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        v->type = obj ? JSON_OBJECT : JSON_ARRAY;
        p->i++;
        int cap = 0;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) { p->i++; return true; }
        for (;;) {
            if (!jp_push(p, v, &cap)) return false;
            if (obj) {
                jp_ws(p);
                if (p->i >= p->n || p->s[p->i] != '"') { jp_fail(p, "expected key"); return false; }
                size_t kl;
                char* key = jp_string(p, &kl);
                if (!key) return false;
                v->keys[v->count] = key;
                jp_ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') { free(key); jp_fail(p, "expected ':'"); return false; }
                p->i++;
            }
            if (!jp_value(p, &v->items[v->count], depth + 1)) {
                json_free_inner(&v->items[v->count]);   // la parte già letta dell'elemento
                if (obj) free(v->keys[v->count]);
                return false;
            }
            v->count++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) { p->i++; return true; }
            jp_fail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
            return false;
        }
    }
    if (c == '"') {
        v->type = JSON_STRING;
        v->str = jp_string(p, &v->len);
        return v->str != NULL;
    }
    if (jp_lit(p, "true"))  { v->type = JSON_BOOL; v->boolean = true; return true; }
    if (jp_lit(p, "false")) { v->type = JSON_BOOL; return true; }
    if (jp_lit(p, "null"))  { v->type = JSON_NULL; return true; }
    // numero: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    size_t st = p->i;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    if (p->i < p->n && p->s[p->i] == '0') p->i++;
    else if (p->i < p->n && p->s[p->i] >= '1' && p->s[p->i] <= '9') while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    else { p->i = st; jp_fail(p, "unexpected character"); return false; }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') { jp_fail(p, "bad number"); return false; }
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') { jp_fail(p, "bad number"); return false; }
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    v->type = JSON_NUMBER;
    v->num_text = malloc(p->i - st + 1);
    if (!v->num_text) { jp_fail(p, "out of memory"); return false; }
    memcpy(v->num_text, p->s + st, p->i - st);
    v->num_text[p->i - st] = '\0';
    v->number = strtod(v->num_text, NULL);
    return true;
}

static void json_free_inner(JsonVal* v) {
    for (int i = 0; i < v->count; i++) {
        json_free_inner(&v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v->num_text);
    memset(v, 0, sizeof(*v));
}

JsonVal* json_parse(const char* text, size_t len, char* err, size_t errsz) {
    JP p = { text, 0, len, err, errsz, false };
    if (errsz) err[0] = '\0';
    JsonVal* v = calloc(1, sizeof(JsonVal));
    if (!v) { snprintf(err, errsz, "out of memory"); return NULL; }
    if (jp_value(&p, v, 0)) {
        jp_ws(&p);
        if (p.i == p.n) return v;
        jp_fail(&p, "trailing characters");
    }
    json_free_inner(v);
    free(v);
    return NULL;
}

void json_free(JsonVal* v) {
    if (!v) return;
    json_free_inner(v);
    free(v);
}

const JsonVal* json_get(const JsonVal* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->count; i++)
        if (!strcmp(obj->keys[i], key)) return &obj->items[i];
    return NULL;
}

// ── scrittura ────────────────────────────────────────────────────

static bool jb_reserve(JsonBuf* b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 1024;
    while (nc < b->len + extra + 1) nc *= 2;
    char* nd = realloc(b->data, nc);
    if (!nd) { b->oom = true; return false; }
    b->data = nd;
    b->cap = nc;
    return true;
}

void jb_raw(JsonBuf* b, const char* s, size_t n) {
    if (!jb_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void jb_strn(JsonBuf* b, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    if (!jb_reserve(b, n + 2)) return;
    b->data[b->len++] = '"';
    size_t run = 0;                       // tratto senza escape copiato in blocco
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') { run++; continue; }
        jb_raw(b, s + i - run, run);
        run = 0;
        char e[7];
        switch (c) {
            case '"':  jb_lit(b, "\\\""); break;
            case '\\': jb_lit(b, "\\\\"); break;
            case '\n': jb_lit(b, "\\n"); break;
            case '\r': jb_lit(b, "\\r"); break;
            case '\t': jb_lit(b, "\\t"); break;
            default:
                e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hex[c >> 4]; e[5] = hex[c & 15];
                jb_raw(b, e, 6);
        }
    }
    jb_raw(b, s + n - run, run);
    jb_lit(b, "\"");
}

void jb_str(JsonBuf* b, const char* s) { jb_strn(b, s ? s : "", s ? strlen(s) : 0); }

void jb_printf(JsonBuf* b, const char* fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { jb_raw(b, tmp, (size_t)n); return; }
    if (!jb_reserve(b, (size_t)n)) return;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}
