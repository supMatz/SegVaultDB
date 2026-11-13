/*
SCOPO: i .htaccess dei siti (vedi rewrite.h).
*/
#include "rewrite.h"
#include "../query/regex.h"
#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_LEVELS  32               // cartelle annidate con un .htaccess
#define MAX_PASSES  10               // riscritture interne una dopo l'altra (difesa dai cicli)
#define NCAPS       10

enum { C_REGEX, C_FILE, C_DIR, C_SIZE, C_LINK, C_EQ, C_LT, C_GT, C_EXEC };

typedef struct {
    char*    test;
    char*    pat;
    SvRegex* re;
    int      kind;
    bool     neg, nc, or_next;
} Cond;

typedef struct {
    SvRegex* re;
    bool     neg;
    char*    subst;
    Cond*    conds;
    int      nconds;
    bool     last, end, qsa, qsd, chain, forbid, gone;
    int      redirect;               // 0 = no
    int      skip;
} Rule;

typedef struct {
    bool     engine;
    char     base[512];
    bool     has_base;
    Rule*    rules;
    int      nrules;
    int      access;                 // 0 = non detto, 1 = negato, 2 = concesso
    int      listing;                // 0 = non detto, 1 = no (-Indexes), 2 = sì
    SvRegex* deny_files[16];         // <Files> / <FilesMatch> con l'accesso negato
    int      ndeny;
    char     url[2048];              // cartella nel sito: "/" o "/sub/"
    char     dir[4096];              // cartella sul disco
} HtFile;

static void ht_free(HtFile* h) {
    if (!h) return;
    for (int i = 0; i < h->nrules; i++) {
        Rule* r = &h->rules[i];
        sv_regex_free(r->re);
        free(r->subst);
        for (int k = 0; k < r->nconds; k++) { free(r->conds[k].test); free(r->conds[k].pat); sv_regex_free(r->conds[k].re); }
        free(r->conds);
    }
    free(h->rules);
    for (int i = 0; i < h->ndeny; i++) sv_regex_free(h->deny_files[i]);
    free(h);
}

// ── lettura ─────────────────────────────────────────────────────

// argomenti di una riga: separati da spazi, "tra virgolette" con \" protette
static int split_args(char* line, char** av, int max) {
    int n = 0;
    char* p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            av[n++] = p;
            char* w = p;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; *w++ = *p++; }
            if (*p) p++;
            *w = '\0';
        } else {
            av[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return n;
}

static bool has_flag(const char* flags, const char* name, char* val, size_t vn) {
    if (!flags) return false;
    const char* p = flags;
    if (*p == '[') p++;
    size_t nl = strlen(name);
    while (*p && *p != ']') {
        while (*p == ',' || *p == ' ') p++;
        const char* s = p;
        while (*p && *p != ',' && *p != ']') p++;
        size_t l = (size_t)(p - s);
        if (l >= nl && !strncasecmp(s, name, nl) && (l == nl || s[nl] == '=')) {
            if (val) snprintf(val, vn, "%.*s", l > nl ? (int)(l - nl - 1) : 0, l > nl ? s + nl + 1 : "");
            return true;
        }
    }
    return false;
}

// <Files "*.log">: dal glob alla regex
static SvRegex* glob_re(const char* g) {
    char re[600];
    size_t k = 0;
    re[k++] = '^';
    for (; *g && k + 4 < sizeof(re); g++) {
        if (*g == '*') { re[k++] = '.'; re[k++] = '*'; }
        else if (*g == '?') re[k++] = '.';
        else if (strchr(".+()[]{}^$|\\", *g)) { re[k++] = '\\'; re[k++] = *g; }
        else re[k++] = *g;
    }
    re[k++] = '$';
    re[k] = '\0';
    char err[100];
    return sv_regex_compile(re, true, err, sizeof(err));
}

static HtFile* ht_load(const char* dir, const char* url, char* err, size_t errsz) {
    char path[4200];
    snprintf(path, sizeof(path), "%s/.htaccess", dir);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    HtFile* h = calloc(1, sizeof(HtFile));
    if (!h) { fclose(f); return NULL; }
    snprintf(h->url, sizeof(h->url), "%s", url);
    snprintf(h->dir, sizeof(h->dir), "%s", dir);
    Cond pending[32];
    int npend = 0;
    int skip_depth = 0;                       // dentro un blocco da saltare
    int depth = 0;
    SvRegex* files_re = NULL;                 // dentro <Files>/<FilesMatch>
    int files_depth = -1;
    char line[8192];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t')) line[--l] = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (*p == '<') {
            if (p[1] == '/') {                                        // </Blocco>
                if (skip_depth && depth == skip_depth) skip_depth = 0;
                if (files_depth == depth) { sv_regex_free(files_re); files_re = NULL; files_depth = -1; }
                if (depth > 0) depth--;
                continue;
            }
            depth++;
            if (skip_depth) continue;
            char* gt = strrchr(p, '>');
            if (gt) *gt = '\0';
            char* av[4];
            int ac = split_args(p + 1, av, 4);
            if (ac >= 1 && !strcasecmp(av[0], "IfModule") && ac >= 2 && av[1][0] == '!') skip_depth = depth;   // modulo "assente"
            else if (ac >= 2 && (!strcasecmp(av[0], "Files") || !strcasecmp(av[0], "FilesMatch"))) {
                char e[100];
                bool rx = !strcasecmp(av[0], "FilesMatch") || (ac >= 3 && !strcmp(av[1], "~"));
                const char* pat = ac >= 3 && !strcmp(av[1], "~") ? av[2] : av[1];
                sv_regex_free(files_re);
                files_re = rx ? sv_regex_compile(pat, true, e, sizeof(e)) : glob_re(pat);
                files_depth = depth;
            } else if (ac >= 1 && (!strcasecmp(av[0], "Limit") || !strcasecmp(av[0], "LimitExcept") ||
                                   !strcasecmp(av[0], "If") || !strcasecmp(av[0], "ElseIf") || !strcasecmp(av[0], "Else"))) {
                skip_depth = depth;                                   // condizioni che non valutiamo: meglio saltare
            }
            continue;
        }
        if (skip_depth) continue;
        char* av[6];
        int ac = split_args(p, av, 6);
        if (ac < 1) continue;
        const char* d = av[0];
        bool deny = (ac >= 3 && !strcasecmp(d, "Require") && !strcasecmp(av[1], "all") && !strcasecmp(av[2], "denied")) ||
                    (ac >= 3 && !strcasecmp(d, "Deny") && !strcasecmp(av[1], "from") && !strcasecmp(av[2], "all"));
        bool grant = (ac >= 3 && !strcasecmp(d, "Require") && !strcasecmp(av[1], "all") && !strcasecmp(av[2], "granted")) ||
                     (ac >= 3 && !strcasecmp(d, "Allow") && !strcasecmp(av[1], "from") && !strcasecmp(av[2], "all"));
        if (files_re) {
            if (deny && h->ndeny < 16) { h->deny_files[h->ndeny++] = files_re; files_re = NULL; files_depth = -1; skip_depth = depth; }
            continue;
        }
        if (deny) h->access = 1;
        else if (grant) h->access = 2;
        else if (!strcasecmp(d, "Options")) {
            for (int i = 1; i < ac; i++) {
                if (!strcasecmp(av[i], "-Indexes")) h->listing = 1;
                else if (!strcasecmp(av[i], "+Indexes") || !strcasecmp(av[i], "Indexes") || !strcasecmp(av[i], "All")) h->listing = 2;
            }
        } else if (!strcasecmp(d, "RewriteEngine") && ac >= 2) h->engine = !strcasecmp(av[1], "on");
        else if (!strcasecmp(d, "RewriteBase") && ac >= 2) {
            snprintf(h->base, sizeof(h->base), "%s%s", av[1], av[1][0] && av[1][strlen(av[1]) - 1] == '/' ? "" : "/");
            h->has_base = true;
        } else if (!strcasecmp(d, "RewriteCond") && ac >= 3) {
            if (npend >= 32) continue;
            Cond* c = &pending[npend];
            memset(c, 0, sizeof(*c));
            const char* pat = av[2];
            if (*pat == '!') { c->neg = true; pat++; }
            c->nc = has_flag(ac >= 4 ? av[3] : NULL, "NC", NULL, 0) || has_flag(ac >= 4 ? av[3] : NULL, "nocase", NULL, 0);
            c->or_next = has_flag(ac >= 4 ? av[3] : NULL, "OR", NULL, 0) || has_flag(ac >= 4 ? av[3] : NULL, "ornext", NULL, 0);
            c->kind = C_REGEX;
            if (!strcmp(pat, "-f") || !strcmp(pat, "-F") || !strcmp(pat, "-U")) c->kind = C_FILE;
            else if (!strcmp(pat, "-d")) c->kind = C_DIR;
            else if (!strcmp(pat, "-s")) c->kind = C_SIZE;
            else if (!strcmp(pat, "-l") || !strcmp(pat, "-L") || !strcmp(pat, "-h")) c->kind = C_LINK;
            else if (!strcmp(pat, "-x")) c->kind = C_EXEC;
            else if (*pat == '=') { c->kind = C_EQ; pat++; if (!strcmp(pat, "\"\"")) pat = ""; }
            else if (*pat == '<' && pat[1] != '=') { c->kind = C_LT; pat++; }
            else if (*pat == '>' && pat[1] != '=') { c->kind = C_GT; pat++; }
            c->test = strdup(av[1]);
            c->pat = strdup(pat);
            if (c->kind == C_REGEX) {
                char e[160];
                c->re = sv_regex_compile(pat, c->nc, e, sizeof(e));
                if (!c->re) {
                    snprintf(err, errsz, "%.80s line %d: RewriteCond pattern: %.80s", path, lineno, e);
                    free(c->test); free(c->pat);
                    continue;
                }
            }
            npend++;
        } else if (!strcasecmp(d, "RewriteRule") && ac >= 3) {
            Rule r;
            memset(&r, 0, sizeof(r));
            const char* pat = av[1];
            if (*pat == '!') { r.neg = true; pat++; }
            const char* fl = ac >= 4 ? av[3] : NULL;
            char v[64];
            bool nc = has_flag(fl, "NC", NULL, 0) || has_flag(fl, "nocase", NULL, 0);
            char e[160];
            r.re = sv_regex_compile(pat, nc, e, sizeof(e));
            if (!r.re) {
                snprintf(err, errsz, "%.80s line %d: RewriteRule pattern: %.80s", path, lineno, e);
                for (int k = 0; k < npend; k++) { free(pending[k].test); free(pending[k].pat); sv_regex_free(pending[k].re); }
                npend = 0;
                continue;
            }
            r.subst = strdup(av[2]);
            r.last = has_flag(fl, "L", NULL, 0) || has_flag(fl, "last", NULL, 0);
            r.end = has_flag(fl, "END", NULL, 0);
            r.qsa = has_flag(fl, "QSA", NULL, 0) || has_flag(fl, "qsappend", NULL, 0);
            r.qsd = has_flag(fl, "QSD", NULL, 0) || has_flag(fl, "qsdiscard", NULL, 0);
            r.chain = has_flag(fl, "C", NULL, 0) || has_flag(fl, "chain", NULL, 0);
            r.forbid = has_flag(fl, "F", NULL, 0) || has_flag(fl, "forbidden", NULL, 0);
            r.gone = has_flag(fl, "G", NULL, 0) || has_flag(fl, "gone", NULL, 0);
            if (has_flag(fl, "R", v, sizeof(v)) || has_flag(fl, "redirect", v, sizeof(v))) {
                r.redirect = 302;
                if (!strcasecmp(v, "permanent")) r.redirect = 301;
                else if (!strcasecmp(v, "seeother")) r.redirect = 303;
                else if (isdigit((unsigned char)v[0])) r.redirect = atoi(v);
                if (r.redirect < 300 || r.redirect > 399) r.redirect = 302;
            }
            if (has_flag(fl, "S", v, sizeof(v)) || has_flag(fl, "skip", v, sizeof(v))) r.skip = atoi(v);
            if (npend) {
                r.conds = malloc(sizeof(Cond) * (size_t)npend);
                if (r.conds) { memcpy(r.conds, pending, sizeof(Cond) * (size_t)npend); r.nconds = npend; }
                npend = 0;
            }
            Rule* nr = realloc(h->rules, sizeof(Rule) * (size_t)(h->nrules + 1));
            if (!nr) { sv_regex_free(r.re); free(r.subst); free(r.conds); continue; }
            h->rules = nr;
            h->rules[h->nrules++] = r;
        }
    }
    for (int k = 0; k < npend; k++) { free(pending[k].test); free(pending[k].pat); sv_regex_free(pending[k].re); }
    sv_regex_free(files_re);
    fclose(f);
    return h;
}

// ── valutazione ─────────────────────────────────────────────────

typedef struct {
    const RwRequest* rq;
    const char* path;              // percorso attuale (cambia con le riscritture)
    const char* query;
    const char* rel;               // percorso relativo alla cartella del .htaccess (per $N)
    size_t      rcaps[NCAPS][2];
    const char* ctext;             // testo dell'ultima RewriteCond con regex riuscita (per %N)
    char        cbuf[4096];
    size_t      ccaps[NCAPS][2];
} Ctx;

static bool header_of(const char* headers, const char* name, char* out, size_t n) {
    out[0] = '\0';
    size_t nl = strlen(name);
    for (const char* l = headers; l && *l; ) {
        const char* e = strstr(l, "\r\n");
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll > nl && l[nl] == ':' && !strncasecmp(l, name, nl)) {
            const char* v = l + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            snprintf(out, n, "%.*s", (int)(l + ll - v), v);
            return true;
        }
        l = e ? e + 2 : NULL;
    }
    return false;
}

static void var_value(const Ctx* c, const char* name, size_t nl, char* out, size_t n) {
    char nm[128];
    snprintf(nm, sizeof(nm), "%.*s", (int)(nl < 127 ? nl : 127), name);
    const RwRequest* rq = c->rq;
    out[0] = '\0';
    if (!strcasecmp(nm, "REQUEST_FILENAME") || !strcasecmp(nm, "SCRIPT_FILENAME")) snprintf(out, n, "%s%s", rq->root, c->path);
    else if (!strcasecmp(nm, "REQUEST_URI") || !strcasecmp(nm, "SCRIPT_NAME")) snprintf(out, n, "%s", c->path);
    else if (!strcasecmp(nm, "QUERY_STRING")) snprintf(out, n, "%s", c->query ? c->query : "");
    else if (!strcasecmp(nm, "HTTP_HOST")) snprintf(out, n, "%s", rq->host ? rq->host : "");
    else if (!strcasecmp(nm, "SERVER_NAME")) { snprintf(out, n, "%s", rq->host ? rq->host : ""); char* col = strchr(out, ':'); if (col) *col = '\0'; }
    else if (!strcasecmp(nm, "REQUEST_METHOD")) snprintf(out, n, "%s", rq->method ? rq->method : "GET");
    else if (!strcasecmp(nm, "HTTPS")) snprintf(out, n, "off");
    else if (!strcasecmp(nm, "REQUEST_SCHEME")) snprintf(out, n, "http");
    else if (!strcasecmp(nm, "SERVER_PORT")) snprintf(out, n, "%d", rq->port);
    else if (!strcasecmp(nm, "REMOTE_ADDR")) snprintf(out, n, "%s", rq->client ? rq->client : "");
    else if (!strcasecmp(nm, "DOCUMENT_ROOT")) snprintf(out, n, "%s", rq->root);
    else if (!strcasecmp(nm, "THE_REQUEST")) snprintf(out, n, "%s", rq->the_request ? rq->the_request : "");
    else if (!strcasecmp(nm, "SERVER_PROTOCOL")) snprintf(out, n, "HTTP/1.1");
    else if (!strncasecmp(nm, "HTTP:", 5)) header_of(rq->headers, nm + 5, out, n);
    else if (!strncasecmp(nm, "HTTP_", 5)) {                 // HTTP_USER_AGENT -> User-Agent
        char h[128];
        size_t k = 0;
        for (const char* q = nm + 5; *q && k + 1 < sizeof(h); q++) h[k++] = *q == '_' ? '-' : *q;
        h[k] = '\0';
        header_of(rq->headers, h, out, n);
    }
    else if (!strncasecmp(nm, "ENV:", 4)) { const char* v = getenv(nm + 4); if (v && strncmp(nm + 4, "REDIRECT_", 9)) snprintf(out, n, "%s", v); }
    // altre variabili (TIME_*, SERVER_ADDR, ...): vuote
}

static void expand(const Ctx* c, const char* t, JsonBuf* o) {
    for (const char* p = t; *p; p++) {
        if (*p == '\\' && (p[1] == '$' || p[1] == '%')) { jb_raw(o, p + 1, 1); p++; continue; }
        if ((*p == '$' || *p == '%') && isdigit((unsigned char)p[1])) {
            int k = p[1] - '0';
            const char* src = *p == '$' ? c->rel : c->ctext;
            const size_t (*caps)[2] = *p == '$' ? (const size_t (*)[2])c->rcaps : (const size_t (*)[2])c->ccaps;
            if (src && caps[k][0] != (size_t)-1 && caps[k][1] != (size_t)-1 && caps[k][1] >= caps[k][0])
                jb_raw(o, src + caps[k][0], caps[k][1] - caps[k][0]);
            p++;
            continue;
        }
        if (*p == '%' && p[1] == '{') {
            const char* e = strchr(p + 2, '}');
            if (e) {
                char v[4096];
                var_value(c, p + 2, (size_t)(e - p - 2), v, sizeof(v));
                jb_raw(o, v, strlen(v));
                p = e;
                continue;
            }
        }
        jb_raw(o, p, 1);
    }
    if (!o->data) jb_raw(o, "", 0);
}

static bool cond_ok(Ctx* c, Cond* k) {
    JsonBuf t = {0};
    expand(c, k->test, &t);
    const char* s = t.data ? t.data : "";
    bool r = false;
    struct stat st;
    switch (k->kind) {
    case C_FILE: r = stat(s, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG; break;
    case C_DIR:  r = stat(s, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR; break;
    case C_SIZE: r = stat(s, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG && st.st_size > 0; break;
#ifndef _WIN32
    case C_LINK: r = lstat(s, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK; break;
    case C_EXEC: r = stat(s, &st) == 0 && (st.st_mode & 0111); break;
#else
    case C_LINK: r = false; break;
    case C_EXEC: r = stat(s, &st) == 0; break;
#endif
    case C_EQ: r = k->nc ? !strcasecmp(s, k->pat) : !strcmp(s, k->pat); break;
    case C_LT: r = (k->nc ? strcasecmp(s, k->pat) : strcmp(s, k->pat)) < 0; break;
    case C_GT: r = (k->nc ? strcasecmp(s, k->pat) : strcmp(s, k->pat)) > 0; break;
    default: {
        size_t caps[NCAPS][2];
        r = sv_regex_search_caps(k->re, s, 0, caps, NCAPS);
        if (r && !k->neg) {                     // %N: dall'ultima condizione riuscita
            snprintf(c->cbuf, sizeof(c->cbuf), "%s", s);
            c->ctext = c->cbuf;
            memcpy(c->ccaps, caps, sizeof(caps));
        }
    }
    }
    free(t.data);
    return k->neg ? !r : r;
}

static bool conds_ok(Ctx* c, Rule* r) {
    bool or_open = false, or_val = false;
    for (int i = 0; i < r->nconds; i++) {
        Cond* k = &r->conds[i];
        if (or_open && or_val) {                       // il gruppo OR è già vero: niente da valutare
            if (!k->or_next) { or_open = false; or_val = false; }
            continue;
        }
        bool v = cond_ok(c, k);
        if (k->or_next) { or_open = true; or_val = or_val || v; continue; }
        if (or_open) { v = v || or_val; or_open = false; or_val = false; }
        if (!v) return false;
    }
    return !or_open || or_val;
}

static void pct_decode(const char* in, size_t n, char* out, size_t outsz) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < outsz; i++) {
        if (in[i] == '%' && i + 2 < n && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char h[3] = { in[i + 1], in[i + 2], 0 };
            out[k++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else out[k++] = in[i];
    }
    out[k] = '\0';
}

// i .htaccess delle cartelle del percorso, dalla radice in giù
static int load_levels(const RwRequest* rq, const char* path, HtFile** lv, char* err, size_t errsz) {
    int n = 0;
    char dir[4200], url[2100];
    snprintf(dir, sizeof(dir), "%s", rq->root);
    snprintf(url, sizeof(url), "/");
    const char* p = path;
    for (;;) {
        HtFile* h = ht_load(dir, url, err, errsz);
        if (h && n < MAX_LEVELS) lv[n++] = h; else ht_free(h);
        while (*p == '/') p++;
        const char* e = strchr(p, '/');
        if (!e) break;                                   // l'ultimo pezzo è il file (o una cartella senza "/")
        size_t dl = strlen(dir), ul = strlen(url);
        if (dl + (size_t)(e - p) + 2 >= sizeof(dir) || ul + (size_t)(e - p) + 2 >= sizeof(url)) break;
        snprintf(dir + dl, sizeof(dir) - dl, "/%.*s", (int)(e - p), p);
        snprintf(url + ul, sizeof(url) - ul, "%.*s/", (int)(e - p), p);
        struct stat st;
        if (stat(dir, &st) != 0 || (st.st_mode & S_IFMT) != S_IFDIR) break;
        p = e;
    }
    // la cartella stessa ("/sub" senza la barra finale)
    if (*p) {
        size_t dl = strlen(dir), ul = strlen(url);
        char d2[4400];
        snprintf(d2, sizeof(d2), "%s/%s", dir, p);
        struct stat st;
        if (stat(d2, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR && dl + strlen(p) + 2 < sizeof(dir) && ul + strlen(p) + 2 < sizeof(url)) {
            char u2[2200];
            snprintf(u2, sizeof(u2), "%s%s/", url, p);
            HtFile* h = ht_load(d2, u2, err, errsz);
            if (h && n < MAX_LEVELS) lv[n++] = h; else ht_free(h);
        }
    }
    return n;
}

void rw_apply(const RwRequest* rq, RwResult* out) {
    memset(out, 0, sizeof(*out));
    char path[2048], query[2048];
    snprintf(path, sizeof(path), "%s", rq->path);
    snprintf(query, sizeof(query), "%s", rq->query ? rq->query : "");
    bool changed_any = false;
    HtFile* lv[MAX_LEVELS];
    int n = 0;
    for (int pass = 0; pass < MAX_PASSES; pass++) {
        n = load_levels(rq, path, lv, out->error, sizeof(out->error));
        HtFile* h = NULL;                              // il .htaccess più profondo con le regole attive
        for (int i = n - 1; i >= 0 && !h; i--) if (lv[i]->engine && lv[i]->nrules) h = lv[i];
        bool changed = false, stop = false;
        if (h) {
            size_t ul = strlen(h->url);
            const char* rel = path;
            if (!strncmp(path, h->url, ul)) rel = path + ul;
            else if (!strncmp(path, h->url, ul - 1) && path[ul - 1] == '\0') rel = "";
            while (*rel == '/') rel++;
            char relbuf[2048];
            snprintf(relbuf, sizeof(relbuf), "%s", rel);
            Ctx c;
            memset(&c, 0, sizeof(c));
            c.rq = rq;
            c.path = path;
            c.query = query;
            c.rel = relbuf;
            bool chain_failed = false;
            for (int i = 0; i < h->nrules; i++) {
                Rule* r = &h->rules[i];
                if (chain_failed) { chain_failed = r->chain; continue; }      // resto della catena saltato
                bool m = sv_regex_search_caps(r->re, relbuf, 0, c.rcaps, NCAPS);
                if (r->neg) { m = !m; for (int k = 0; k < NCAPS; k++) c.rcaps[k][0] = c.rcaps[k][1] = (size_t)-1; }
                c.ctext = NULL;
                if (m && r->nconds) m = conds_ok(&c, r);
                if (!m) { chain_failed = r->chain; continue; }
                if (r->forbid) { out->action = RW_FORBIDDEN; goto done; }
                if (r->gone) { out->action = RW_GONE; goto done; }
                if (!strcmp(r->subst, "-")) {
                    if (r->qsd) { query[0] = '\0'; changed_any = true; }
                    if (r->redirect) {                  // "-" con R: ridirige allo stesso indirizzo
                        out->action = RW_REDIRECT;
                        out->status = r->redirect;
                        snprintf(out->location, sizeof(out->location), "%s%s%s", path, query[0] ? "?" : "", query);
                        goto done;
                    }
                    if (r->last || r->end) { stop = r->end; break; }
                    i += r->skip;
                    continue;
                }
                JsonBuf s = {0};
                expand(&c, r->subst, &s);
                const char* sub = s.data ? s.data : "";
                bool absolute = !strncasecmp(sub, "http://", 7) || !strncasecmp(sub, "https://", 8);
                char np[4096], nq[4200];
                const char* qm = strchr(sub, '?');
                size_t pl = qm ? (size_t)(qm - sub) : strlen(sub);
                if (absolute) snprintf(np, sizeof(np), "%.*s", (int)pl, sub);
                else if (sub[0] == '/') snprintf(np, sizeof(np), "%.*s", (int)pl, sub);
                else snprintf(np, sizeof(np), "%s%.*s", h->has_base ? h->base : h->url, (int)pl, sub);
                if (qm) {
                    if (r->qsa && query[0]) snprintf(nq, sizeof(nq), "%s%s%s", qm + 1, qm[1] ? "&" : "", query);
                    else snprintf(nq, sizeof(nq), "%s", qm + 1);
                } else snprintf(nq, sizeof(nq), "%s", r->qsd ? "" : query);
                free(s.data);
                if (absolute || r->redirect) {
                    out->action = RW_REDIRECT;
                    out->status = r->redirect ? r->redirect : 302;
                    snprintf(out->location, sizeof(out->location), "%s%s%s", np, nq[0] ? "?" : "", nq);
                    goto done;
                }
                char dec[2048];
                pct_decode(np, strlen(np), dec, sizeof(dec));
                if (strcmp(dec, path) || strcmp(nq, query)) { changed = true; changed_any = true; }
                snprintf(path, sizeof(path), "%s", dec);
                nq[sizeof(query) - 1] = '\0';                 // QUERY_STRING al massimo 2047 byte
                memcpy(query, nq, strlen(nq) + 1);
                c.path = path;
                c.query = query;
                if (r->last || r->end) { stop = r->end; break; }
                // senza L le regole seguenti vedono il nuovo percorso
                rel = path;
                if (!strncmp(path, h->url, ul)) rel = path + ul;
                while (*rel == '/') rel++;
                snprintf(relbuf, sizeof(relbuf), "%s", rel);
                i += r->skip;
            }
        }
        if (!changed || stop || pass == MAX_PASSES - 1) break;
        for (int i = 0; i < n; i++) ht_free(lv[i]);
        n = 0;
    }
    // accesso e elenco dei file: dal .htaccess più vicino che ne parla (sul percorso finale)
    {
        if (changed_any) {                            // le cartelle del percorso finale
            for (int i = 0; i < n; i++) ht_free(lv[i]);
            n = load_levels(rq, path, lv, out->error, sizeof(out->error));
        }
        int access = 0;
        for (int i = 0; i < n; i++) {
            if (lv[i]->access) access = lv[i]->access;
            if (lv[i]->listing) out->no_listing = lv[i]->listing == 1;
        }
        if (access == 1) { out->action = RW_FORBIDDEN; goto done; }
        const char* base = strrchr(path, '/');
        base = base ? base + 1 : path;
        for (int i = 0; i < n && *base; i++)
            for (int k = 0; k < lv[i]->ndeny; k++)
                if (sv_regex_match(lv[i]->deny_files[k], base)) { out->action = RW_FORBIDDEN; goto done; }
    }
    if (changed_any) {
        out->action = RW_REWRITE;
        snprintf(out->path, sizeof(out->path), "%s", path);
        snprintf(out->query, sizeof(out->query), "%s", query);
    }
done:
    for (int i = 0; i < n; i++) ht_free(lv[i]);
}
