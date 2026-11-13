/*
SCOPO: cattura delle email di sviluppo (vedi mail.h).
       - server SMTP minimo (EHLO, AUTH accettato con qualsiasi credenziale, MAIL, RCPT, DATA, RSET,
         NOOP, QUIT): ogni messaggio diventa DIR/ID.eml, con le intestazioni Return-Path e
         X-SegVault-Rcpt (i destinatari della busta: anche i Bcc);
       - lettura dei messaggi: intestazioni con le parole codificate (RFC 2047), parti MIME
         (multipart annidati, base64, quoted-printable, ISO-8859-1 -> UTF-8), allegati;
       - pagina web e API JSON sotto /.segvault/mail;
       - client "sendmail" per mail() di PHP.
*/
#include "mail.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <fcntl.h>
  #define BAD_SOCK INVALID_SOCKET
  #define sock_close closesocket
  #define SEND_FLAGS 0
  typedef CRITICAL_SECTION ml_mutex;
  static void mx_init(ml_mutex* m)    { InitializeCriticalSection(m); }
  static void mx_lock(ml_mutex* m)    { EnterCriticalSection(m); }
  static void mx_unlock(ml_mutex* m)  { LeaveCriticalSection(m); }
  static void mx_destroy(ml_mutex* m) { DeleteCriticalSection(m); }
  static double ml_now(void) { return (double)GetTickCount64(); }
  #define mkdir_p(d) _mkdir(d)
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <pthread.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define BAD_SOCK (-1)
  #define sock_close close
  #define SEND_FLAGS MSG_NOSIGNAL
  typedef pthread_mutex_t ml_mutex;
  static void mx_init(ml_mutex* m)    { pthread_mutex_init(m, NULL); }
  static void mx_lock(ml_mutex* m)    { pthread_mutex_lock(m); }
  static void mx_unlock(ml_mutex* m)  { pthread_mutex_unlock(m); }
  static void mx_destroy(ml_mutex* m) { pthread_mutex_destroy(m); }
  static double ml_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
  #define mkdir_p(d) mkdir(d, 0755)
#endif

#define MAIL_MAX_SIZE   (32u << 20)      // un messaggio (con gli allegati)
#define MAIL_MAX_KEEP   2000             // messaggi tenuti: oltre, si cancellano i più vecchi
#define SMTP_IDLE_MS    (120 * 1000)
#define MAX_RCPT        200

typedef struct {
    long      id;
    long long when;                      // ora di arrivo (secondi Unix)
    long long size;
    char      from[256];
    char      to[512];                   // destinatari della busta, separati da ", "
    char      subject[512];
} MailEnt;

struct MailStore {
    char*    dir;
    ml_mutex mx;
    MailEnt* v;                          // dal più vecchio
    int      n, cap;
    long     next_id;
    MailNotify notify;
    void*    notify_ud;
};

void mail_set_notify(MailStore* m, MailNotify fn, void* ud) { m->notify = fn; m->notify_ud = ud; }

// ── testo: decodifiche ──────────────────────────────────────────

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static void b64_decode_to(JsonBuf* o, const char* s, size_t n) {
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64val((unsigned char)s[i]);
        if (v < 0) { if (s[i] == '=') break; continue; }   // spazi e a capo ignorati
        acc = (acc << 6 | (unsigned)v) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) { bits -= 8; char c = (char)((acc >> bits) & 0xFF); jb_raw(o, &c, 1); }
    }
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// quoted-printable; header: "_" vale uno spazio (parole codificate Q)
static void qp_decode_to(JsonBuf* o, const char* s, size_t n, bool header) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '=' && i + 1 < n && (s[i + 1] == '\r' || s[i + 1] == '\n')) {   // a capo morbido
            i++;
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
            continue;
        }
        if (c == '=' && i + 2 < n && hexv((unsigned char)s[i + 1]) >= 0 && hexv((unsigned char)s[i + 2]) >= 0) {
            char d = (char)(hexv((unsigned char)s[i + 1]) * 16 + hexv((unsigned char)s[i + 2]));
            jb_raw(o, &d, 1);
            i += 2;
            continue;
        }
        if (header && c == '_') c = ' ';
        jb_raw(o, &c, 1);
    }
}

static bool latin_charset(const char* cs) {
    return !strcasecmp(cs, "iso-8859-1") || !strcasecmp(cs, "latin1") || !strcasecmp(cs, "iso-8859-15") ||
           !strcasecmp(cs, "windows-1252") || !strcasecmp(cs, "cp1252") || !strcasecmp(cs, "us-ascii");
}

// testo nel charset cs -> UTF-8 (i charset latini vengono convertiti, gli altri lasciati così)
static void to_utf8(JsonBuf* o, const char* s, size_t n, const char* cs) {
    if (!cs || !*cs || !latin_charset(cs)) { jb_raw(o, s, n); return; }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { jb_raw(o, (const char*)&c, 1); continue; }
        char u[2] = { (char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F)) };
        jb_raw(o, u, 2);
    }
}

// valore d'intestazione con parole codificate "=?utf-8?B?...?=" / "=?iso-8859-1?Q?...?=" -> UTF-8
static void rfc2047(const char* in, size_t n, char* out, size_t outsz) {
    JsonBuf o = {0};
    size_t i = 0;
    bool last_encoded = false;
    while (i < n) {
        if (in[i] == '=' && i + 1 < n && in[i + 1] == '?') {
            const char* cs = in + i + 2;
            const char* q1 = memchr(cs, '?', n - (size_t)(cs - in));
            if (q1 && q1 + 2 < in + n && q1[2] == '?') {
                char enc = (char)toupper((unsigned char)q1[1]);
                const char* txt = q1 + 3;
                const char* endw = NULL;
                for (const char* p = txt; p + 1 < in + n; p++) if (p[0] == '?' && p[1] == '=') { endw = p; break; }
                if (endw && (enc == 'B' || enc == 'Q')) {
                    char charset[40];
                    snprintf(charset, sizeof(charset), "%.*s", (int)(q1 - cs), cs);
                    char* star = strchr(charset, '*');            // RFC 2231: "utf-8*it"
                    if (star) *star = '\0';
                    JsonBuf raw = {0};
                    if (enc == 'B') b64_decode_to(&raw, txt, (size_t)(endw - txt));
                    else qp_decode_to(&raw, txt, (size_t)(endw - txt), true);
                    if (raw.data) to_utf8(&o, raw.data, raw.len, charset);
                    free(raw.data);
                    i = (size_t)(endw + 2 - in);
                    last_encoded = true;
                    continue;
                }
            }
        }
        if (last_encoded && (in[i] == ' ' || in[i] == '\t')) {     // spazi tra due parole codificate: spariscono
            size_t j = i;
            while (j < n && (in[j] == ' ' || in[j] == '\t')) j++;
            if (j + 1 < n && in[j] == '=' && in[j + 1] == '?') { i = j; continue; }
        }
        last_encoded = false;
        jb_raw(&o, in + i, 1);
        i++;
    }
    snprintf(out, outsz, "%.*s", o.data ? (int)o.len : 0, o.data ? o.data : "");
    free(o.data);
    // un taglio a metà di un carattere UTF-8 lascerebbe byte non validi
    size_t l = strlen(out);
    if (l + 1 >= outsz) {
        while (l > 0 && ((unsigned char)out[l - 1] & 0xC0) == 0x80) l--;
        if (l > 0 && ((unsigned char)out[l - 1] & 0xC0) == 0xC0) l--;
        out[l] = '\0';
    }
}

// ── intestazioni ────────────────────────────────────────────────

// lunghezza del blocco delle intestazioni (fino alla riga vuota esclusa) e inizio del corpo
static size_t head_end(const char* p, size_t n, size_t* body) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\n') continue;
        if (i + 1 < n && p[i + 1] == '\n') { *body = i + 2; return i + 1; }
        if (i + 2 < n && p[i + 1] == '\r' && p[i + 2] == '\n') { *body = i + 3; return i + 1; }
    }
    *body = n;
    return n;
}

// valore grezzo (righe di continuazione unite) dell'intestazione name; false se manca
static bool hdr_raw(const char* h, size_t hl, const char* name, JsonBuf* out) {
    size_t nl = strlen(name);
    for (size_t i = 0; i < hl; ) {
        size_t e = i;
        while (e < hl && h[e] != '\n') e++;
        if (e - i > nl && h[i + nl] == ':' && !strncasecmp(h + i, name, nl)) {
            size_t v = i + nl + 1;
            for (;;) {
                while (v < e && (h[v] == ' ' || h[v] == '\t')) v++;
                size_t ve = e;
                while (ve > v && (h[ve - 1] == '\r' || h[ve - 1] == ' ')) ve--;
                if (out->len && ve > v) jb_lit(out, " ");
                jb_raw(out, h + v, ve - v);
                if (e + 1 < hl && (h[e + 1] == ' ' || h[e + 1] == '\t')) {      // continua sotto
                    v = e + 1;
                    e = v;
                    while (e < hl && h[e] != '\n') e++;
                } else break;
            }
            if (!out->data) jb_raw(out, "", 0);
            return true;
        }
        i = e + 1;
    }
    return false;
}

static bool hdr_get(const char* h, size_t hl, const char* name, char* out, size_t outsz) {
    JsonBuf r = {0};
    bool ok = hdr_raw(h, hl, name, &r);
    if (ok && r.data) rfc2047(r.data, r.len, out, outsz);
    else out[0] = '\0';
    free(r.data);
    return ok;
}

// parametro di un'intestazione: 'charset="utf-8"', 'boundary=abc', "filename*=utf-8''nome%20file"
static void hdr_param(const char* v, const char* key, char* out, size_t outsz) {
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char* p = strchr(v, ';'); p; p = strchr(p + 1, ';')) {
        const char* q = p + 1;
        while (*q == ' ' || *q == '\t') q++;
        bool ext = false;
        if (strncasecmp(q, key, kl)) continue;
        const char* a = q + kl;
        if (*a == '*') { ext = true; a++; }
        while (*a == ' ') a++;
        if (*a != '=') continue;
        a++;
        while (*a == ' ') a++;
        char tmp[512];
        size_t k = 0;
        if (*a == '"') {
            for (a++; *a && *a != '"' && k + 1 < sizeof(tmp); a++) {
                if (*a == '\\' && a[1]) a++;
                tmp[k++] = *a;
            }
        } else while (*a && *a != ';' && *a != ' ' && k + 1 < sizeof(tmp)) tmp[k++] = *a++;
        tmp[k] = '\0';
        if (ext) {                                   // charset''valore%XX
            const char* t = strstr(tmp, "''");
            const char* s = t ? t + 2 : tmp;
            size_t j = 0;
            for (; *s && j + 1 < outsz; s++) {
                if (*s == '%' && hexv((unsigned char)s[1]) >= 0 && hexv((unsigned char)s[2]) >= 0) {
                    out[j++] = (char)(hexv((unsigned char)s[1]) * 16 + hexv((unsigned char)s[2]));
                    s += 2;
                } else out[j++] = *s;
            }
            out[j] = '\0';
        } else rfc2047(tmp, strlen(tmp), out, outsz);
        return;
    }
}

// ── parti MIME ──────────────────────────────────────────────────

typedef struct {
    const char* p;                   // corpo codificato
    size_t      n;
    char        name[256];
    char        ctype[128];
    char        enc[64];
} MimeAtt;

typedef struct {
    JsonBuf text, html;
    bool    has_text, has_html;
    MimeAtt att[64];
    int     natt;
} Mime;

static void decode_body(JsonBuf* o, const char* p, size_t n, const char* enc, const char* charset) {
    JsonBuf raw = {0};
    if (!strcasecmp(enc, "base64")) b64_decode_to(&raw, p, n);
    else if (!strcasecmp(enc, "quoted-printable")) qp_decode_to(&raw, p, n, false);
    else jb_raw(&raw, p, n);
    if (raw.data) to_utf8(o, raw.data, raw.len, charset);
    free(raw.data);
}

static void mime_walk(const char* p, size_t n, Mime* m, int depth) {
    size_t body;
    size_t hl = head_end(p, n, &body);
    char ct[512], cte[64], cd[512];
    hdr_get(p, hl, "Content-Type", ct, sizeof(ct));
    hdr_get(p, hl, "Content-Transfer-Encoding", cte, sizeof(cte));
    hdr_get(p, hl, "Content-Disposition", cd, sizeof(cd));
    char type[128];
    size_t tl = strcspn(ct, "; \t");
    snprintf(type, sizeof(type), "%.*s", (int)(tl < sizeof(type) ? tl : sizeof(type) - 1), ct);
    for (char* c = type; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!type[0]) snprintf(type, sizeof(type), "text/plain");
    for (char* c = cte; *c; c++) if (*c == ' ') *c = '\0';
    const char* b = p + body;
    size_t bn = n - body;

    if (!strncmp(type, "multipart/", 10) && depth < 12) {
        char bnd[256];
        hdr_param(ct, "boundary", bnd, sizeof(bnd));
        size_t bl = strlen(bnd);
        if (!bl) return;
        const char* part = NULL;
        for (size_t i = 0; i < bn; ) {
            size_t e = i;
            while (e < bn && b[e] != '\n') e++;
            if (e - i >= bl + 2 && b[i] == '-' && b[i + 1] == '-' && !memcmp(b + i + 2, bnd, bl)) {
                if (part) {
                    size_t end = i;                          // il CRLF prima del separatore è suo
                    if (end > 0 && b[end - 1] == '\n') end--;
                    if (end > 0 && b[end - 1] == '\r') end--;
                    if (end > (size_t)(part - b)) mime_walk(part, end - (size_t)(part - b), m, depth + 1);
                }
                if (e - i >= bl + 4 && b[i + 2 + bl] == '-' && b[i + 3 + bl] == '-') return;   // --confine--
                part = b + e + 1;
            }
            i = e + 1;
        }
        return;
    }
    if (!strcmp(type, "message/rfc822") && depth < 12) { mime_walk(b, bn, m, depth + 1); return; }

    char name[256] = "";
    hdr_param(cd, "filename", name, sizeof(name));
    if (!name[0]) hdr_param(ct, "name", name, sizeof(name));
    bool attachment = !strncasecmp(cd, "attachment", 10) || (name[0] && strncmp(type, "text/", 5));
    char charset[64];
    hdr_param(ct, "charset", charset, sizeof(charset));
    if (!attachment && !strcmp(type, "text/plain") && !m->has_text) {
        decode_body(&m->text, b, bn, cte, charset);
        m->has_text = true;
    } else if (!attachment && !strcmp(type, "text/html") && !m->has_html) {
        decode_body(&m->html, b, bn, cte, charset);
        m->has_html = true;
    } else if (m->natt < 64) {
        MimeAtt* a = &m->att[m->natt++];
        a->p = b;
        a->n = bn;
        snprintf(a->name, sizeof(a->name), "%s", name[0] ? name : "attachment");
        snprintf(a->ctype, sizeof(a->ctype), "%s", type);
        snprintf(a->enc, sizeof(a->enc), "%s", cte);
    }
}

static void mime_free(Mime* m) { free(m->text.data); free(m->html.data); }

// ── archivio ────────────────────────────────────────────────────

static void eml_path(const MailStore* m, long id, char* out, size_t n) { snprintf(out, n, "%s/%ld.eml", m->dir, id); }

static char* read_file(const char* path, size_t* n, size_t max) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    JsonBuf b = {0};
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0 && b.len < max) jb_raw(&b, chunk, got);
    fclose(f);
    if (b.oom) { free(b.data); return NULL; }
    if (!b.data) jb_raw(&b, "", 0);
    *n = b.len;
    return b.data;
}

// riassunto (mittente, destinatari, oggetto) dalle intestazioni del messaggio
static void summarize(MailEnt* e, const char* p, size_t n) {
    size_t body;
    size_t hl = head_end(p, n, &body);
    hdr_get(p, hl, "From", e->from, sizeof(e->from));
    if (!e->from[0]) hdr_get(p, hl, "Return-Path", e->from, sizeof(e->from));
    if (!hdr_get(p, hl, "X-SegVault-Rcpt", e->to, sizeof(e->to))) hdr_get(p, hl, "To", e->to, sizeof(e->to));
    hdr_get(p, hl, "Subject", e->subject, sizeof(e->subject));
}

static void add_entry(MailStore* m, const MailEnt* e) {
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 64;
        MailEnt* nv = realloc(m->v, sizeof(MailEnt) * (size_t)nc);
        if (!nv) return;
        m->v = nv;
        m->cap = nc;
    }
    int i = m->n;                                       // in ordine di id
    while (i > 0 && m->v[i - 1].id > e->id) { m->v[i] = m->v[i - 1]; i--; }
    m->v[i] = *e;
    m->n++;
}

MailStore* mail_store_open(const char* dir, char* err, size_t errsz) {
    MailStore* m = calloc(1, sizeof(MailStore));
    if (!m) { snprintf(err, errsz, "out of memory"); return NULL; }
    m->dir = strdup(dir);
    size_t l = m->dir ? strlen(m->dir) : 0;
    while (l > 1 && (m->dir[l - 1] == '/' || m->dir[l - 1] == '\\')) m->dir[--l] = '\0';
    mx_init(&m->mx);
    m->next_id = 1;
    struct stat st;
    if (stat(m->dir, &st) != 0) mkdir_p(m->dir);
    DIR* d = opendir(m->dir);
    if (!d) { snprintf(err, errsz, "cannot open the mail folder %.200s", dir); mail_store_free(m); return NULL; }
    struct dirent* de;
    while ((de = readdir(d))) {
        char* end;
        long id = strtol(de->d_name, &end, 10);
        if (id <= 0 || strcmp(end, ".eml")) continue;
        MailEnt e;
        memset(&e, 0, sizeof(e));
        e.id = id;
        char path[1200];
        eml_path(m, id, path, sizeof(path));
        if (stat(path, &st) != 0) continue;
        e.when = (long long)st.st_mtime;
        e.size = (long long)st.st_size;
        size_t n;
        char* p = read_file(path, &n, 64 * 1024);        // le intestazioni bastano
        if (p) { summarize(&e, p, n); free(p); }
        add_entry(m, &e);
        if (id >= m->next_id) m->next_id = id + 1;
    }
    closedir(d);
    return m;
}

void mail_store_free(MailStore* m) {
    if (!m) return;
    mx_destroy(&m->mx);
    free(m->v);
    free(m->dir);
    free(m);
}

int mail_count(MailStore* m) {
    mx_lock(&m->mx);
    int n = m->n;
    mx_unlock(&m->mx);
    return n;
}

static void remove_at(MailStore* m, int i) {
    char path[1200];
    eml_path(m, m->v[i].id, path, sizeof(path));
    remove(path);
    memmove(m->v + i, m->v + i + 1, sizeof(MailEnt) * (size_t)(m->n - i - 1));
    m->n--;
}

// salva un messaggio arrivato; ritorna l'id (0 se non si è potuto scrivere)
static long store_message(MailStore* m, const char* client, const char* from, char rcpt[][256], int nrcpt, const char* data, size_t n) {
    JsonBuf f = {0};
    jb_printf(&f, "Return-Path: <%s>\r\nX-SegVault-Rcpt: ", from);
    for (int i = 0; i < nrcpt; i++) jb_printf(&f, "%s%s", i ? ", " : "", rcpt[i]);
    jb_lit(&f, "\r\n");
    jb_raw(&f, data, n);
    if (f.oom) { free(f.data); return 0; }
    mx_lock(&m->mx);
    long id = m->next_id++;
    char path[1200];
    eml_path(m, id, path, sizeof(path));
    FILE* fp = fopen(path, "wb");
    bool ok = fp && fwrite(f.data, 1, f.len, fp) == f.len;
    if (fp && fclose(fp) != 0) ok = false;
    MailEnt e;
    memset(&e, 0, sizeof(e));
    if (ok) {
        e.id = id;
        e.when = (long long)time(NULL);
        e.size = (long long)f.len;
        summarize(&e, f.data, f.len);
        add_entry(m, &e);
        while (m->n > MAIL_MAX_KEEP) remove_at(m, 0);
    } else remove(path);
    mx_unlock(&m->mx);
    if (ok && m->notify) m->notify(m->notify_ud, client ? client : "", e.from, e.to, e.subject, id);
    free(f.data);
    return ok ? id : 0;
}

// ── SMTP ────────────────────────────────────────────────────────

typedef struct {
    ml_sock     fd;
    char        buf[16384];
    size_t      len, pos;
    atomic_int* stop;
} LineIn;

static bool ml_send(ml_sock fd, const char* p, size_t n) {
    while (n > 0) {
        int w = (int)send(fd, p, (int)n, SEND_FLAGS);
        if (w <= 0) {
#ifndef _WIN32
            if (w < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool reply(ml_sock fd, const char* s) { return ml_send(fd, s, strlen(s)); }

// una riga (senza CRLF) aggiunta a out; false su chiusura, tempo scaduto o stop
static bool read_line(LineIn* in, JsonBuf* out, size_t max) {
    double t0 = ml_now();
    for (;;) {
        while (in->pos < in->len) {
            char c = in->buf[in->pos++];
            if (c == '\n') {
                if (out->len && out->data[out->len - 1] == '\r') out->len--;
                if (!out->data) jb_raw(out, "", 0);
                else out->data[out->len] = '\0';
                return true;
            }
            if (out->len < max) jb_raw(out, &c, 1);
        }
        if (*in->stop || ml_now() - t0 > SMTP_IDLE_MS) return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(in->fd, &rd);
        struct timeval tv = { 0, 200000 };
        int r = select((int)in->fd + 1, &rd, NULL, NULL, &tv);
        if (r <= 0) continue;
        int got = (int)recv(in->fd, in->buf, (int)sizeof(in->buf), 0);
        if (got <= 0) return false;
        in->len = (size_t)got;
        in->pos = 0;
    }
}

// indirizzo tra < > dopo "MAIL FROM:" / "RCPT TO:"
static void smtp_addr(const char* arg, char* out, size_t n) {
    while (*arg == ' ') arg++;
    const char* lt = strchr(arg, '<');
    const char* gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt) snprintf(out, n, "%.*s", (int)(gt - lt - 1), lt + 1);
    else snprintf(out, n, "%.*s", (int)strcspn(arg, " "), arg);
}

void mail_smtp_serve(MailStore* m, ml_sock fd, const char* client, atomic_int* stop) {
    LineIn in = { .fd = fd, .stop = stop };
    char from[256] = "";
    char (*rcpt)[256] = malloc(sizeof(char[MAX_RCPT][256]));
    int nrcpt = 0;
    bool have_from = false;
    if (!rcpt) return;
    if (!reply(fd, "220 segvault ESMTP SegVault mail catcher (messages are kept, never delivered)\r\n")) { free(rcpt); return; }
    for (;;) {
        JsonBuf line = {0};
        if (!read_line(&in, &line, 4096)) { free(line.data); free(rcpt); return; }
        char* l = line.data;
        char verb[16];
        size_t vl = strcspn(l, " ");
        snprintf(verb, sizeof(verb), "%.*s", (int)(vl < 15 ? vl : 15), l);
        for (char* c = verb; *c; c++) *c = (char)toupper((unsigned char)*c);
        const char* arg = l + vl;
        while (*arg == ' ') arg++;
        bool ok = true;
        if (!strcmp(verb, "EHLO")) {
            ok = reply(fd, "250-segvault\r\n250-SIZE 33554432\r\n250-8BITMIME\r\n250-SMTPUTF8\r\n250-PIPELINING\r\n"
                           "250-AUTH PLAIN LOGIN\r\n250-AUTH=PLAIN LOGIN\r\n250 ENHANCEDSTATUSCODES\r\n");
        } else if (!strcmp(verb, "HELO")) {
            ok = reply(fd, "250 segvault\r\n");
        } else if (!strcmp(verb, "AUTH")) {
            // qualsiasi credenziale va bene: l'app è configurata come in produzione
            if (!strncasecmp(arg, "PLAIN", 5)) {
                if (!arg[5]) {
                    JsonBuf x = {0};
                    ok = reply(fd, "334 \r\n") && read_line(&in, &x, 4096);
                    free(x.data);
                }
                ok = ok && reply(fd, "235 2.7.0 Authentication successful\r\n");
            } else if (!strncasecmp(arg, "LOGIN", 5)) {
                JsonBuf x = {0};
                if (!arg[5]) { ok = reply(fd, "334 VXNlcm5hbWU6\r\n") && read_line(&in, &x, 4096); x.len = 0; }
                ok = ok && reply(fd, "334 UGFzc3dvcmQ6\r\n") && read_line(&in, &x, 4096);
                free(x.data);
                ok = ok && reply(fd, "235 2.7.0 Authentication successful\r\n");
            } else ok = reply(fd, "504 5.5.4 Unrecognized authentication type\r\n");
        } else if (!strcmp(verb, "MAIL") && !strncasecmp(arg, "FROM:", 5)) {
            smtp_addr(arg + 5, from, sizeof(from));
            have_from = true;
            nrcpt = 0;
            ok = reply(fd, "250 2.1.0 Ok\r\n");
        } else if (!strcmp(verb, "RCPT") && !strncasecmp(arg, "TO:", 3)) {
            if (!have_from) ok = reply(fd, "503 5.5.1 Need MAIL first\r\n");
            else if (nrcpt >= MAX_RCPT) ok = reply(fd, "452 4.5.3 Too many recipients\r\n");
            else { smtp_addr(arg + 3, rcpt[nrcpt++], sizeof(rcpt[0])); ok = reply(fd, "250 2.1.5 Ok\r\n"); }
        } else if (!strcmp(verb, "DATA")) {
            if (!have_from || !nrcpt) ok = reply(fd, "503 5.5.1 Need MAIL and RCPT first\r\n");
            else if ((ok = reply(fd, "354 End data with <CR><LF>.<CR><LF>\r\n"))) {
                JsonBuf msg = {0};
                bool too_big = false;
                for (;;) {
                    JsonBuf dl = {0};
                    if (!read_line(&in, &dl, MAIL_MAX_SIZE)) { free(dl.data); free(msg.data); free(line.data); free(rcpt); return; }
                    if (dl.len == 1 && dl.data[0] == '.') { free(dl.data); break; }
                    const char* t = dl.data && dl.data[0] == '.' ? dl.data + 1 : dl.data;   // ".." -> "."
                    size_t tn = dl.data && dl.data[0] == '.' ? dl.len - 1 : dl.len;
                    if (msg.len + tn + 2 > MAIL_MAX_SIZE) too_big = true;
                    else { if (t) jb_raw(&msg, t, tn); jb_lit(&msg, "\r\n"); }
                    free(dl.data);
                }
                if (too_big || msg.oom) ok = reply(fd, "552 5.3.4 Message too big\r\n");
                else {
                    long id = store_message(m, client, from, rcpt, nrcpt, msg.data ? msg.data : "", msg.len);
                    char r[96];
                    if (id) snprintf(r, sizeof(r), "250 2.0.0 Ok: queued as %ld\r\n", id);
                    else snprintf(r, sizeof(r), "451 4.3.0 Cannot store the message\r\n");
                    ok = reply(fd, r);
                }
                free(msg.data);
                have_from = false;
                nrcpt = 0;
            }
        } else if (!strcmp(verb, "RSET")) {
            have_from = false; nrcpt = 0;
            ok = reply(fd, "250 2.0.0 Ok\r\n");
        } else if (!strcmp(verb, "NOOP")) {
            ok = reply(fd, "250 2.0.0 Ok\r\n");
        } else if (!strcmp(verb, "VRFY")) {
            ok = reply(fd, "252 2.5.0 Cannot verify, but will accept\r\n");
        } else if (!strcmp(verb, "STARTTLS")) {
            ok = reply(fd, "454 4.7.0 TLS not available (use an unencrypted connection to the local catcher)\r\n");
        } else if (!strcmp(verb, "QUIT")) {
            reply(fd, "221 2.0.0 Bye\r\n");
            free(line.data);
            free(rcpt);
            return;
        } else ok = reply(fd, "500 5.5.2 Command not recognized\r\n");
        free(line.data);
        if (!ok) { free(rcpt); return; }
    }
}

// ── web ─────────────────────────────────────────────────────────

static const char MAIL_PAGE[] =
"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SegVault Mail</title><style>"
":root{--bg:#f6f7f9;--fg:#1d2330;--mut:#667085;--card:#fff;--line:#e4e7ec;--acc:#2563eb;--sel:#e8effd}"
"@media(prefers-color-scheme:dark){:root{--bg:#111318;--fg:#e6e8ec;--mut:#98a2b3;--card:#1a1d24;--line:#2a2f3a;--acc:#60a5fa;--sel:#1e2a44}}"
"*{box-sizing:border-box}html,body{height:100%}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;display:flex;flex-direction:column}"
"header{display:flex;align-items:center;gap:12px;padding:10px 16px;border-bottom:1px solid var(--line);background:var(--card)}"
"header h1{font-size:16px;margin:0}header .sp{flex:1}button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:6px;padding:4px 10px;cursor:pointer}"
"button:hover{border-color:var(--acc)}input{font:inherit;border:1px solid var(--line);background:var(--bg);color:var(--fg);border-radius:6px;padding:4px 8px;width:220px;max-width:40vw}"
".wrap{flex:1;display:flex;min-height:0}#list{width:38%;min-width:260px;max-width:480px;overflow:auto;border-right:1px solid var(--line);background:var(--card)}"
".it{padding:9px 14px;border-bottom:1px solid var(--line);cursor:pointer}.it:hover{background:var(--bg)}.it.sel{background:var(--sel)}"
".it .s{font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.it .m{color:var(--mut);font-size:12.5px;display:flex;gap:8px}.it .m span:first-child{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
"#view{flex:1;display:flex;flex-direction:column;min-width:0}#meta{padding:14px 18px;border-bottom:1px solid var(--line)}#meta h2{margin:0 0 6px;font-size:18px}"
"#meta table td{padding:1px 10px 1px 0;vertical-align:top}#meta td:first-child{color:var(--mut)}.tabs{display:flex;gap:6px;padding:8px 18px;border-bottom:1px solid var(--line)}"
".tabs button.on{border-color:var(--acc);color:var(--acc)}#body{flex:1;min-height:0;overflow:auto}#body iframe{border:0;width:100%;height:100%;background:#fff}"
"#body pre{margin:0;padding:16px 18px;white-space:pre-wrap;word-break:break-word;font:13px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
".empty{color:var(--mut);padding:40px;text-align:center}.att a{margin-right:12px}"
"@media(max-width:700px){.wrap{flex-direction:column}#list{width:auto;max-width:none;max-height:40vh;border-right:0;border-bottom:1px solid var(--line)}}"
"</style></head><body><header><h1>&#9993; SegVault Mail</h1><span id=\"cnt\" style=\"color:var(--mut)\"></span><span class=\"sp\"></span>"
"<input id=\"q\" placeholder=\"Search\"><button id=\"clr\">Delete all</button></header>"
"<div class=\"wrap\"><div id=\"list\"></div><div id=\"view\"><div class=\"empty\">Messages sent to the SMTP port appear here.</div></div></div>"
"<script>"
"let msgs=[],cur=null,tab='html',sig='';const $=s=>document.querySelector(s);"
"const esc=s=>String(s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));"
"function when(t){const d=new Date(t*1000),n=new Date();return d.toDateString()==n.toDateString()?d.toLocaleTimeString():d.toLocaleString()}"
"function size(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB'}"
"async function load(){try{const r=await fetch('api/messages');const j=await r.json();const s=JSON.stringify(j.messages.map(m=>m.id));"
"msgs=j.messages;$('#cnt').textContent=msgs.length+' message'+(msgs.length==1?'':'s');if(s!=sig){sig=s;list()}}catch(e){}}"
"function list(){const q=$('#q').value.toLowerCase();const v=msgs.filter(m=>!q||(m.subject+' '+m.from+' '+m.to).toLowerCase().includes(q));"
"$('#list').innerHTML=v.length?v.map(m=>'<div class=\"it'+(m.id==cur?' sel':'')+'\" data-id=\"'+m.id+'\"><div class=\"s\">'+esc(m.subject||'(no subject)')+'</div>"
"<div class=\"m\"><span>'+esc(m.to)+'</span><span>'+when(m.time)+'</span></div></div>').join(''):'<div class=\"empty\">No messages</div>';"
"document.querySelectorAll('.it').forEach(e=>e.onclick=()=>show(+e.dataset.id))}"
"async function show(id){cur=id;list();const r=await fetch('api/messages/'+id);if(!r.ok){$('#view').innerHTML='<div class=\"empty\">Message deleted</div>';return}const m=await r.json();"
"if(!m.html&&tab=='html')tab='text';let h='<div id=\"meta\"><h2>'+esc(m.subject||'(no subject)')+'</h2><table>'+"
"'<tr><td>From</td><td>'+esc(m.from)+'</td></tr><tr><td>To</td><td>'+esc(m.to)+'</td></tr>'+(m.cc?'<tr><td>Cc</td><td>'+esc(m.cc)+'</td></tr>':'')+"
"'<tr><td>Date</td><td>'+esc(m.date||when(m.time))+'</td></tr><tr><td>Size</td><td>'+size(m.size)+' &middot; <a href=\"'+id+'.eml\" download>.eml</a> &middot; <a href=\"#\" id=\"del\">delete</a></td></tr>'+"
"(m.attachments.length?'<tr><td>Files</td><td class=\"att\">'+m.attachments.map((a,i)=>'<a href=\"'+id+'/att/'+i+'\" download=\"'+esc(a.name)+'\">'+esc(a.name)+'</a> <span style=\"color:var(--mut)\">'+size(a.size)+'</span>').join(' ')+'</td></tr>':'')+"
"'</table></div><div class=\"tabs\">'+(m.html?'<button data-t=\"html\">HTML</button>':'')+'<button data-t=\"text\">Text</button><button data-t=\"source\">Source</button></div><div id=\"body\"></div>';"
"$('#view').innerHTML=h;document.querySelectorAll('.tabs button').forEach(b=>{b.classList.toggle('on',b.dataset.t==tab);b.onclick=()=>{tab=b.dataset.t;show(id)}});"
"$('#del').onclick=async e=>{e.preventDefault();await fetch('api/messages/'+id,{method:'DELETE'});cur=null;$('#view').innerHTML='';load()};"
"if(tab=='html')$('#body').innerHTML='<iframe sandbox src=\"'+id+'.html\"></iframe>';"
"else if(tab=='text')$('#body').innerHTML='<pre>'+esc(m.text||'')+'</pre>';"
"else{const s=await (await fetch(id+'.eml')).text();$('#body').innerHTML='<pre>'+esc(s)+'</pre>'}}"
"$('#q').oninput=list;$('#clr').onclick=async()=>{if(!msgs.length)return;await fetch('api/messages',{method:'DELETE'});cur=null;$('#view').innerHTML='';load()};"
"load();setInterval(load,1500);"
"</script></body></html>";

static int find_idx(MailStore* m, long id) {
    for (int i = 0; i < m->n; i++) if (m->v[i].id == id) return i;
    return -1;
}

static bool contains_ci(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) return true;
    for (; *hay; hay++) if (!strncasecmp(hay, needle, nl)) return true;
    return false;
}

static void query_val(const char* q, const char* key, char* out, size_t n) {
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char* p = q; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char* v = p + kl + 1;
            size_t j = 0;
            for (; *v && *v != '&' && j + 1 < n; v++) {
                if (*v == '+') out[j++] = ' ';
                else if (*v == '%' && hexv((unsigned char)v[1]) >= 0 && hexv((unsigned char)v[2]) >= 0) {
                    out[j++] = (char)(hexv((unsigned char)v[1]) * 16 + hexv((unsigned char)v[2]));
                    v += 2;
                } else out[j++] = *v;
            }
            out[j] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void ent_json(JsonBuf* o, const MailEnt* e) {
    jb_printf(o, "{\"id\":%ld,\"time\":%lld,\"size\":%lld,\"from\":", e->id, e->when, e->size);
    jb_str(o, e->from);
    jb_lit(o, ",\"to\":");
    jb_str(o, e->to);
    jb_lit(o, ",\"subject\":");
    jb_str(o, e->subject);
    jb_lit(o, "}");
}

// messaggio letto dal disco (NULL se non c'è più)
static char* load_msg(MailStore* m, long id, size_t* n, MailEnt* ent) {
    mx_lock(&m->mx);
    int i = find_idx(m, id);
    if (i >= 0 && ent) *ent = m->v[i];
    mx_unlock(&m->mx);
    if (i < 0) return NULL;
    char path[1200];
    eml_path(m, id, path, sizeof(path));
    return read_file(path, n, MAIL_MAX_SIZE + 4096);
}

int mail_web(MailStore* m, const char* method, const char* sub, const char* query, JsonBuf* out,
             const char** ctype, const char** extra_headers) {
    bool get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    bool del = !strcmp(method, "DELETE");
    *extra_headers = "Cache-Control: no-store\r\n";
    if (!sub[0]) { *extra_headers = "Location: /.segvault/mail/\r\n"; *ctype = "text/plain"; jb_lit(out, "moved\n"); return 301; }
    if (!strcmp(sub, "/")) {
        if (!get) return 405;
        *ctype = "text/html; charset=utf-8";
        jb_raw(out, MAIL_PAGE, sizeof(MAIL_PAGE) - 1);
        return 200;
    }
    *ctype = "application/json; charset=utf-8";
    if (!strcmp(sub, "/api/messages")) {
        if (del) {
            mx_lock(&m->mx);
            while (m->n > 0) remove_at(m, m->n - 1);
            mx_unlock(&m->mx);
            jb_lit(out, "{\"ok\":true}");
            return 200;
        }
        if (!get) return 405;
        // filtri per i test automatici: ?to=, ?from=, ?subject=, ?q= (sottostringhe), ?limit=
        char to[256], from[256], subj[256], q[256], lim[16];
        query_val(query, "to", to, sizeof(to));
        query_val(query, "from", from, sizeof(from));
        query_val(query, "subject", subj, sizeof(subj));
        query_val(query, "q", q, sizeof(q));
        query_val(query, "limit", lim, sizeof(lim));
        int limit = lim[0] ? atoi(lim) : 0;
        mx_lock(&m->mx);
        jb_printf(out, "{\"total\":%d,\"messages\":[", m->n);
        int shown = 0;
        for (int i = m->n - 1; i >= 0 && (!limit || shown < limit); i--) {   // dal più recente
            const MailEnt* e = &m->v[i];
            if (!contains_ci(e->to, to) || !contains_ci(e->from, from) || !contains_ci(e->subject, subj)) continue;
            if (q[0] && !contains_ci(e->to, q) && !contains_ci(e->from, q) && !contains_ci(e->subject, q)) continue;
            if (shown++) jb_lit(out, ",");
            ent_json(out, e);
        }
        mx_unlock(&m->mx);
        jb_lit(out, "]}");
        return 200;
    }
    if (!strncmp(sub, "/api/messages/", 14)) {
        char* end;
        long id = strtol(sub + 14, &end, 10);
        bool latest = !strcmp(sub + 14, "latest");
        if (latest) {
            mx_lock(&m->mx);
            id = m->n ? m->v[m->n - 1].id : 0;
            mx_unlock(&m->mx);
        } else if (*end || id <= 0) return 404;
        if (del) {
            mx_lock(&m->mx);
            int i = find_idx(m, id);
            if (i >= 0) remove_at(m, i);
            mx_unlock(&m->mx);
            if (i < 0) { jb_lit(out, "{\"ok\":false,\"error\":\"message not found\"}"); return 404; }
            jb_lit(out, "{\"ok\":true}");
            return 200;
        }
        if (!get) return 405;
        size_t n;
        MailEnt e;
        char* p = load_msg(m, id, &n, &e);
        if (!p) { jb_lit(out, "{\"ok\":false,\"error\":\"message not found\"}"); return 404; }
        size_t body;
        size_t hl = head_end(p, n, &body);
        char cc[1024], date[128], mid[256];
        hdr_get(p, hl, "Cc", cc, sizeof(cc));
        hdr_get(p, hl, "Date", date, sizeof(date));
        hdr_get(p, hl, "Message-ID", mid, sizeof(mid));
        Mime mm;
        memset(&mm, 0, sizeof(mm));
        mime_walk(p, n, &mm, 0);
        ent_json(out, &e);
        out->len--;                                          // si riapre l'oggetto
        jb_lit(out, ",\"cc\":");
        jb_str(out, cc);
        jb_lit(out, ",\"date\":");
        jb_str(out, date);
        jb_lit(out, ",\"message_id\":");
        jb_str(out, mid);
        jb_lit(out, ",\"headers\":");
        jb_strn(out, p, hl);
        jb_lit(out, ",\"text\":");
        jb_strn(out, mm.text.data ? mm.text.data : "", mm.text.len);
        jb_lit(out, ",\"html\":");
        if (mm.has_html) jb_strn(out, mm.html.data ? mm.html.data : "", mm.html.len);
        else jb_lit(out, "null");
        jb_lit(out, ",\"attachments\":[");
        for (int i = 0; i < mm.natt; i++) {
            JsonBuf dec = {0};
            decode_body(&dec, mm.att[i].p, mm.att[i].n, mm.att[i].enc, NULL);
            jb_printf(out, "%s{\"name\":", i ? "," : "");
            jb_str(out, mm.att[i].name);
            jb_lit(out, ",\"type\":");
            jb_str(out, mm.att[i].ctype);
            jb_printf(out, ",\"size\":%llu}", (unsigned long long)dec.len);
            free(dec.data);
        }
        jb_lit(out, "]}");
        mime_free(&mm);
        free(p);
        return 200;
    }
    // /ID.eml, /ID.html, /ID/att/N
    char* end;
    long id = strtol(sub + 1, &end, 10);
    if (id <= 0 || !get) return id <= 0 ? 0 : 405;
    size_t n;
    char* p = load_msg(m, id, &n, NULL);
    if (!p) { *ctype = "text/plain; charset=utf-8"; jb_lit(out, "message not found\n"); return 404; }
    int st = 404;
    if (!strcmp(end, ".eml")) {
        *ctype = "message/rfc822";
        jb_raw(out, p, n);
        st = 200;
    } else {
        Mime mm;
        memset(&mm, 0, sizeof(mm));
        mime_walk(p, n, &mm, 0);
        if (!strcmp(end, ".html") && mm.has_html) {
            // l'HTML del messaggio gira in un iframe senza script né accesso alla pagina
            *ctype = "text/html; charset=utf-8";
            *extra_headers = "Cache-Control: no-store\r\nContent-Security-Policy: sandbox; default-src 'none'; "
                             "img-src data: http: https:; style-src 'unsafe-inline' http: https:; font-src data: http: https:\r\n";
            jb_raw(out, mm.html.data ? mm.html.data : "", mm.html.len);
            st = 200;
        } else if (!strncmp(end, "/att/", 5)) {
            int k = atoi(end + 5);
            if (k >= 0 && k < mm.natt) {
                decode_body(out, mm.att[k].p, mm.att[k].n, mm.att[k].enc, NULL);
                *ctype = "application/octet-stream";
                *extra_headers = "Cache-Control: no-store\r\nContent-Disposition: attachment\r\n";
                st = 200;
            }
        }
        mime_free(&mm);
        if (st == 404) { *ctype = "text/plain; charset=utf-8"; jb_lit(out, "not found\n"); }
    }
    free(p);
    return st;
}

// ── sendmail ────────────────────────────────────────────────────

// indirizzi di un'intestazione "Nome <a@b>, c@d" aggiunti a list
static void header_addrs(const char* v, char list[][256], int* n, int max) {
    const char* p = v;
    while (*p && *n < max) {
        const char* s = p;
        int depth = 0;
        bool q = false;
        while (*p && (q || depth || *p != ',')) {
            if (*p == '"') q = !q;
            else if (!q && *p == '<') depth++;
            else if (!q && *p == '>' && depth) depth--;
            p++;
        }
        char one[512];
        snprintf(one, sizeof(one), "%.*s", (int)(p - s), s);
        char* lt = strrchr(one, '<');
        char* addr = one;
        if (lt) { addr = lt + 1; char* gt = strchr(addr, '>'); if (gt) *gt = '\0'; }
        while (*addr == ' ' || *addr == '\t') addr++;
        size_t l = strlen(addr);
        while (l > 0 && (addr[l - 1] == ' ' || addr[l - 1] == '\t' || addr[l - 1] == '\r')) addr[--l] = '\0';
        if (l && strchr(addr, '@')) snprintf(list[(*n)++], 256, "%s", addr);
        if (*p == ',') p++;
    }
}

static bool smtp_expect(ml_sock fd, char* buf, size_t n, int code) {
    // risposta (anche su più righe "250-..."): si legge fino alla riga "NNN "
    size_t have = 0;
    for (;;) {
        int got = (int)recv(fd, buf + have, (int)(n - 1 - have), 0);
        if (got <= 0) return false;
        have += (size_t)got;
        buf[have] = '\0';
        for (char* line = buf; ; ) {                      // la riga "NNN testo" chiude la risposta
            char* e = strstr(line, "\r\n");
            if (!e) break;
            if (e - line >= 3 && (e - line == 3 || line[3] == ' ')) return atoi(line) == code;
            line = e + 2;
        }
        if (have >= n - 1) return false;
    }
}

int mail_sendmail_main(int port, int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    static char rcpt[MAX_RCPT][256];
    int nrcpt = 0;
    char from[256] = "";
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-f") && i + 1 < argc) snprintf(from, sizeof(from), "%s", argv[++i]);
        else if (!strncmp(a, "-f", 2) && a[2]) snprintf(from, sizeof(from), "%s", a + 2);
        else if (a[0] != '-' && strchr(a, '@') && nrcpt < MAX_RCPT) snprintf(rcpt[nrcpt++], 256, "%s", a);
    }
    JsonBuf msg = {0};
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), stdin)) > 0 && msg.len < MAIL_MAX_SIZE) jb_raw(&msg, chunk, got);
    if (!msg.data) { fprintf(stderr, "sendmail: empty message\n"); return 64; }
    size_t body;
    size_t hl = head_end(msg.data, msg.len, &body);
    if (!nrcpt) {                                    // come "sendmail -t": i destinatari dalle intestazioni
        static const char* const H[] = { "To", "Cc", "Bcc" };
        for (int k = 0; k < 3; k++) {
            JsonBuf v = {0};
            if (hdr_raw(msg.data, hl, H[k], &v) && v.data) header_addrs(v.data, rcpt, &nrcpt, MAX_RCPT);
            free(v.data);
        }
    }
    if (!from[0]) {
        JsonBuf v = {0};
        char one[1][256];
        int n1 = 0;
        if (hdr_raw(msg.data, hl, "From", &v) && v.data) header_addrs(v.data, one, &n1, 1);
        free(v.data);
        snprintf(from, sizeof(from), "%s", n1 ? one[0] : "php@localhost");
    }
    if (!nrcpt) { fprintf(stderr, "sendmail: no recipients\n"); free(msg.data); return 64; }

    ml_sock fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd == BAD_SOCK || connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
        fprintf(stderr, "sendmail: SegVault mail catcher not reachable on port %d\n", port);
        free(msg.data);
        return 75;
    }
    char buf[4096];
    bool ok = smtp_expect(fd, buf, sizeof(buf), 220) && reply(fd, "EHLO localhost\r\n") && smtp_expect(fd, buf, sizeof(buf), 250);
    if (ok) { snprintf(buf, sizeof(buf), "MAIL FROM:<%s>\r\n", from); ok = reply(fd, buf) && smtp_expect(fd, buf, sizeof(buf), 250); }
    for (int i = 0; ok && i < nrcpt; i++) {
        snprintf(buf, sizeof(buf), "RCPT TO:<%.255s>\r\n", rcpt[i]);
        ok = reply(fd, buf) && smtp_expect(fd, buf, sizeof(buf), 250);
    }
    ok = ok && reply(fd, "DATA\r\n") && smtp_expect(fd, buf, sizeof(buf), 354);
    if (ok) {
        // righe con CRLF, "." iniziale raddoppiato, Bcc tolto (resta solo nella busta)
        JsonBuf o = {0};
        bool in_head = true, skip_cont = false;
        for (size_t i = 0; i < msg.len; ) {
            size_t e = i;
            while (e < msg.len && msg.data[e] != '\n') e++;
            size_t l = e - i;
            if (l && msg.data[i + l - 1] == '\r') l--;
            const char* line = msg.data + i;
            if (in_head && l == 0) in_head = false;
            bool drop = false;
            if (in_head) {
                if (skip_cont && (line[0] == ' ' || line[0] == '\t')) drop = true;
                else { skip_cont = l > 4 && !strncasecmp(line, "Bcc:", 4); drop = skip_cont; }
            }
            if (!drop) {
                if (l && line[0] == '.') jb_lit(&o, ".");
                jb_raw(&o, line, l);
                jb_lit(&o, "\r\n");
            }
            i = e + 1;
        }
        jb_lit(&o, ".\r\n");
        ok = !o.oom && ml_send(fd, o.data, o.len) && smtp_expect(fd, buf, sizeof(buf), 250);
        free(o.data);
    }
    reply(fd, "QUIT\r\n");
    sock_close(fd);
    free(msg.data);
    if (!ok) { fprintf(stderr, "sendmail: the mail catcher refused the message\n"); return 75; }
    return 0;
}
