/*
SCOPO: motore di espressioni regolari a backtracking (vedi regex.h).
       Il pattern diventa un albero di nodi (alternative, sequenze, ripetizioni, classi); la ricerca
       prova ogni posizione di partenza. Un limite di passi impedisce che un pattern patologico
       blocchi il server (in quel caso: nessuna corrispondenza).
*/
#include "regex.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RN_CHAR, RN_ANY, RN_CLASS, RN_BOL, RN_EOL, RN_GROUP, RN_ALT, RN_SEQ, RN_REPEAT } RNType;

typedef struct RNode {
    RNType type;
    unsigned char ch;
    uint8_t set[32];             // RN_CLASS: bit per byte
    int min, max;                // RN_REPEAT (max < 0 = infinito)
    bool lazy;
    struct RNode** kids;         // RN_SEQ / RN_ALT: figli; RN_GROUP / RN_REPEAT: kids[0]
    int nkids;
} RNode;

struct SvRegex {
    RNode* root;
    bool icase;
};

typedef struct {
    const char* p;
    bool icase;
    char* err;
    size_t errn;
    bool failed;
} RParse;

static RNode* rn_new(RNType t) {
    RNode* n = calloc(1, sizeof(RNode));
    if (n) n->type = t;
    return n;
}

static void rn_free(RNode* n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) rn_free(n->kids[i]);
    free(n->kids);
    free(n);
}

static bool rn_add(RNode* n, RNode* k) {
    RNode** nk = realloc(n->kids, sizeof(RNode*) * (size_t)(n->nkids + 1));
    if (!nk) return false;
    n->kids = nk;
    n->kids[n->nkids++] = k;
    return true;
}

static void set_bit(uint8_t* set, unsigned c) { set[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static bool get_bit(const uint8_t* set, unsigned c) { return (set[c >> 3] >> (c & 7)) & 1; }

static void rfail(RParse* r, const char* msg) {
    if (!r->failed) snprintf(r->err, r->errn, "Invalid regular expression: %s", msg);
    r->failed = true;
}

static void class_named(uint8_t* set, const char* name, size_t n) {
    for (unsigned c = 0; c < 256; c++) {
        bool in = false;
        if (n == 5 && !strncmp(name, "alpha", 5)) in = isalpha((int)c) || c >= 0x80;
        else if (n == 5 && !strncmp(name, "digit", 5)) in = isdigit((int)c);
        else if (n == 5 && !strncmp(name, "alnum", 5)) in = isalnum((int)c) || c >= 0x80;
        else if (n == 5 && !strncmp(name, "space", 5)) in = isspace((int)c);
        else if (n == 5 && !strncmp(name, "upper", 5)) in = isupper((int)c);
        else if (n == 5 && !strncmp(name, "lower", 5)) in = islower((int)c);
        else if (n == 5 && !strncmp(name, "punct", 5)) in = ispunct((int)c);
        else if (n == 6 && !strncmp(name, "xdigit", 6)) in = isxdigit((int)c);
        else if (n == 5 && !strncmp(name, "blank", 5)) in = c == ' ' || c == '\t';
        else if (n == 5 && !strncmp(name, "cntrl", 5)) in = iscntrl((int)c);
        else if (n == 5 && !strncmp(name, "print", 5)) in = isprint((int)c) || c >= 0x80;
        else if (n == 5 && !strncmp(name, "graph", 5)) in = isgraph((int)c) || c >= 0x80;
        if (in) set_bit(set, c);
    }
}

// \d \w \s (e maiuscole: negate) in un insieme
static bool class_escape(uint8_t* set, char e) {
    uint8_t tmp[32] = {0};
    char l = (char)tolower((unsigned char)e);
    if (l == 'd') class_named(tmp, "digit", 5);
    else if (l == 'w') { class_named(tmp, "alnum", 5); set_bit(tmp, '_'); }
    else if (l == 's') class_named(tmp, "space", 5);
    else return false;
    bool neg = isupper((unsigned char)e);
    for (int i = 0; i < 32; i++) set[i] |= neg ? (uint8_t)~tmp[i] : tmp[i];
    return true;
}

static RNode* parse_alt(RParse* r);

static RNode* parse_class(RParse* r) {
    RNode* n = rn_new(RN_CLASS);
    if (!n) { rfail(r, "out of memory"); return NULL; }
    bool neg = false;
    r->p++;                                         // '['
    if (*r->p == '^') { neg = true; r->p++; }
    bool first = true;
    while (*r->p && (*r->p != ']' || first)) {
        first = false;
        if (r->p[0] == '[' && r->p[1] == ':') {     // [:alpha:]
            const char* e = strstr(r->p + 2, ":]");
            if (!e) { rfail(r, "unterminated [: :]"); rn_free(n); return NULL; }
            class_named(n->set, r->p + 2, (size_t)(e - r->p - 2));
            r->p = e + 2;
            continue;
        }
        unsigned char lo = (unsigned char)*r->p++;
        if (lo == '\\' && *r->p) {
            if (class_escape(n->set, *r->p)) { r->p++; continue; }
            lo = (unsigned char)*r->p++;
        }
        unsigned char hi = lo;
        if (r->p[0] == '-' && r->p[1] && r->p[1] != ']') {
            r->p++;
            hi = (unsigned char)*r->p++;
            if (hi == '\\' && *r->p) hi = (unsigned char)*r->p++;
            if (hi < lo) { rfail(r, "invalid range in [ ]"); rn_free(n); return NULL; }
        }
        for (unsigned c = lo; c <= hi; c++) {
            set_bit(n->set, c);
            if (r->icase) { set_bit(n->set, (unsigned)tolower((int)c)); set_bit(n->set, (unsigned)toupper((int)c)); }
        }
    }
    if (*r->p != ']') { rfail(r, "unterminated [ ]"); rn_free(n); return NULL; }
    r->p++;
    if (neg) for (int i = 0; i < 32; i++) n->set[i] = (uint8_t)~n->set[i];
    return n;
}

static RNode* parse_atom(RParse* r) {
    char c = *r->p;
    if (c == '(') {
        r->p++;
        if (r->p[0] == '?' && r->p[1] == ':') r->p += 2;   // (?: ...) senza cattura: uguale
        RNode* inner = parse_alt(r);
        if (r->failed) { rn_free(inner); return NULL; }
        if (*r->p != ')') { rfail(r, "missing )"); rn_free(inner); return NULL; }
        r->p++;
        RNode* g = rn_new(RN_GROUP);
        if (!g || !rn_add(g, inner)) { rn_free(g); rn_free(inner); rfail(r, "out of memory"); return NULL; }
        return g;
    }
    if (c == '[') return parse_class(r);
    if (c == '.') { r->p++; return rn_new(RN_ANY); }
    if (c == '^') { r->p++; return rn_new(RN_BOL); }
    if (c == '$') { r->p++; return rn_new(RN_EOL); }
    if (c == '\\') {
        r->p++;
        if (!*r->p) { rfail(r, "trailing backslash"); return NULL; }
        RNode* n = rn_new(RN_CLASS);
        if (n && class_escape(n->set, *r->p)) { r->p++; return n; }
        rn_free(n);
        n = rn_new(RN_CHAR);
        if (n) {
            char e = *r->p++;
            n->ch = (unsigned char)(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
        }
        return n;
    }
    if (c == '*' || c == '+' || c == '?' || c == '{') { rfail(r, "repetition without an expression"); return NULL; }
    r->p++;
    RNode* n = rn_new(RN_CHAR);
    if (n) n->ch = (unsigned char)c;
    return n;
}

static RNode* parse_repeat(RParse* r) {
    RNode* a = parse_atom(r);
    if (!a || r->failed) return a;
    for (;;) {
        int mn, mx;
        char c = *r->p;
        if (c == '*') { mn = 0; mx = -1; r->p++; }
        else if (c == '+') { mn = 1; mx = -1; r->p++; }
        else if (c == '?') { mn = 0; mx = 1; r->p++; }
        else if (c == '{' && isdigit((unsigned char)r->p[1])) {
            const char* q = r->p + 1;
            mn = (int)strtol(q, (char**)&q, 10);
            mx = mn;
            if (*q == ',') { q++; mx = isdigit((unsigned char)*q) ? (int)strtol(q, (char**)&q, 10) : -1; }
            if (*q != '}' || mn > 1000 || mx > 1000 || (mx >= 0 && mx < mn)) { rfail(r, "invalid {m,n}"); rn_free(a); return NULL; }
            r->p = q + 1;
        } else break;
        RNode* rep = rn_new(RN_REPEAT);
        if (!rep || !rn_add(rep, a)) { rn_free(rep); rn_free(a); rfail(r, "out of memory"); return NULL; }
        rep->min = mn;
        rep->max = mx;
        if (*r->p == '?') { rep->lazy = true; r->p++; }
        a = rep;
    }
    return a;
}

static RNode* parse_seq(RParse* r) {
    RNode* s = rn_new(RN_SEQ);
    if (!s) { rfail(r, "out of memory"); return NULL; }
    while (*r->p && *r->p != '|' && *r->p != ')' && !r->failed) {
        RNode* k = parse_repeat(r);
        if (!k) break;
        if (!rn_add(s, k)) { rn_free(k); rfail(r, "out of memory"); break; }
    }
    return s;
}

static RNode* parse_alt(RParse* r) {
    RNode* first = parse_seq(r);
    if (r->failed || *r->p != '|') return first;
    RNode* alt = rn_new(RN_ALT);
    if (!alt || !rn_add(alt, first)) { rn_free(alt); rn_free(first); rfail(r, "out of memory"); return NULL; }
    while (*r->p == '|' && !r->failed) {
        r->p++;
        RNode* k = parse_seq(r);
        if (!k || !rn_add(alt, k)) { rn_free(k); break; }
    }
    return alt;
}

SvRegex* sv_regex_compile(const char* pattern, bool icase, char* err, size_t errn) {
    RParse r = { pattern ? pattern : "", icase, err, errn, false };
    if (errn) err[0] = '\0';
    RNode* root = parse_alt(&r);
    if (!r.failed && *r.p) rfail(&r, "unbalanced )");
    if (r.failed || !root) { rn_free(root); if (!r.failed) rfail(&r, "out of memory"); return NULL; }
    SvRegex* re = malloc(sizeof(SvRegex));
    if (!re) { rn_free(root); rfail(&r, "out of memory"); return NULL; }
    re->root = root;
    re->icase = icase;
    return re;
}

void sv_regex_free(SvRegex* re) {
    if (!re) return;
    rn_free(re->root);
    free(re);
}

// ── ricerca: continuazioni (cosa fare dopo un nodo) per il backtracking ──

typedef struct Cont {
    const RNode* node;           // nodo da provare dopo (NULL = fine: successo)
    int idx;                     // RN_SEQ: indice del prossimo figlio
    int count;                   // RN_REPEAT: ripetizioni già fatte
    size_t last;                 // RN_REPEAT: posizione all'ultima ripetizione (niente cicli a vuoto)
    const struct Cont* next;
} Cont;

typedef struct {
    const char* s;
    size_t len;
    bool icase;
    long steps;
    int depth;
} MState;

#define MAX_STEPS 2000000L
#define MAX_DEPTH 4000          // ricorsione massima (lo stack dei thread su Windows è di 1 MB)

static bool single_char(const RNode* n) { return n->type == RN_CHAR || n->type == RN_ANY || n->type == RN_CLASS; }

static bool one_match(const MState* m, const RNode* n, size_t pos) {
    if (pos >= m->len) return false;
    unsigned char c = (unsigned char)m->s[pos];
    if (n->type == RN_CHAR) return m->icase ? tolower(c) == tolower(n->ch) : c == n->ch;
    if (n->type == RN_ANY) return c != '\n';
    return get_bit(n->set, c);
}

static bool m_node(MState* m, const RNode* n, size_t pos, const Cont* k, size_t* end);

static bool m_cont(MState* m, size_t pos, const Cont* k, size_t* end) {
    if (!k) { *end = pos; return true; }
    if (k->node->type == RN_SEQ) {
        if (k->idx >= k->node->nkids) return m_cont(m, pos, k->next, end);
        Cont c = { k->node, k->idx + 1, 0, 0, k->next };
        return m_node(m, k->node->kids[k->idx], pos, &c, end);
    }
    if (k->node->type == RN_REPEAT && k->count == 0 && single_char(k->node->kids[0])) {
        // x*, [a-z]+, .{2,5}: si contano i caratteri che vanno bene, poi si prova il resto (senza ricorsione per carattere)
        const RNode* r = k->node;
        size_t n = 0;
        while ((r->max < 0 || n < (size_t)r->max) && one_match(m, r->kids[0], pos + n)) n++;
        if (n < (size_t)r->min) return false;
        if (r->lazy) {
            for (size_t i = (size_t)r->min; i <= n; i++) if (m_cont(m, pos + i, k->next, end)) return true;
        } else {
            for (size_t i = n + 1; i-- > (size_t)r->min; ) if (m_cont(m, pos + i, k->next, end)) return true;
        }
        return false;
    }
    if (k->node->type == RN_REPEAT) {
        const RNode* r = k->node;
        bool can_more = (r->max < 0 || k->count < r->max) && !(k->count > r->min && pos == k->last);
        bool can_stop = k->count >= r->min;
        Cont again = { r, 0, k->count + 1, pos, k->next };
        if (r->lazy) {
            if (can_stop && m_cont(m, pos, k->next, end)) return true;
            return can_more && m_node(m, r->kids[0], pos, &again, end);
        }
        if (can_more && m_node(m, r->kids[0], pos, &again, end)) return true;
        return can_stop && m_cont(m, pos, k->next, end);
    }
    return m_cont(m, pos, k->next, end);
}

static bool m_node_in(MState* m, const RNode* n, size_t pos, const Cont* k, size_t* end);

static bool m_node(MState* m, const RNode* n, size_t pos, const Cont* k, size_t* end) {
    if (++m->steps > MAX_STEPS || m->depth >= MAX_DEPTH) { m->steps = MAX_STEPS + 1; return false; }
    m->depth++;
    bool r = m_node_in(m, n, pos, k, end);
    m->depth--;
    return r;
}

static bool m_node_in(MState* m, const RNode* n, size_t pos, const Cont* k, size_t* end) {
    switch (n->type) {
        case RN_CHAR: {
            if (pos >= m->len) return false;
            unsigned char c = (unsigned char)m->s[pos];
            bool ok = m->icase ? tolower(c) == tolower(n->ch) : c == n->ch;
            return ok && m_cont(m, pos + 1, k, end);
        }
        case RN_ANY:
            return pos < m->len && m->s[pos] != '\n' && m_cont(m, pos + 1, k, end);
        case RN_CLASS:
            return pos < m->len && get_bit(n->set, (unsigned char)m->s[pos]) && m_cont(m, pos + 1, k, end);
        case RN_BOL:
            return pos == 0 && m_cont(m, pos, k, end);
        case RN_EOL:
            return pos == m->len && m_cont(m, pos, k, end);
        case RN_GROUP:
            return m_node(m, n->kids[0], pos, k, end);
        case RN_ALT:
            for (int i = 0; i < n->nkids; i++) if (m_node(m, n->kids[i], pos, k, end)) return true;
            return false;
        case RN_SEQ: {
            Cont c = { n, 0, 0, 0, k };
            return m_cont(m, pos, &c, end);
        }
        case RN_REPEAT: {
            Cont c = { n, 0, 0, (size_t)-1, k };
            return m_cont(m, pos, &c, end);
        }
    }
    return false;
}

bool sv_regex_search(const SvRegex* re, const char* s, size_t from, size_t* start, size_t* end) {
    if (!re || !s) return false;
    MState m = { s, strlen(s), re->icase, 0, 0 };
    for (size_t p = from; p <= m.len; p++) {
        size_t e;
        if (m_node(&m, re->root, p, NULL, &e)) { *start = p; *end = e; return true; }
        if (m.steps > MAX_STEPS) return false;
    }
    return false;
}

bool sv_regex_match(const SvRegex* re, const char* s) {
    size_t a, b;
    return sv_regex_search(re, s, 0, &a, &b);
}
