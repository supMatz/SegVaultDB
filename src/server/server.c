/*
SCOPO: server HTTP/1.1 minimo (socket POSIX / Winsock) e API JSON sopra db_api.h.
       Nessuna dipendenza esterna. Vedi server.h e docs/API.md.
*/
#ifdef _WIN32
  #define _CRT_RAND_S                 // rand_s(): numeri casuali del sistema (token)
#endif
#include "server.h"
#include "json.h"
#include "mysql_wire.h"
#include "fastcgi.h"
#include "proxy.h"
#include "../widgets/png_write.h"
#include "../bridge/db_api.h"
#include "../bridge/auth.h"
#include "../widgets/sql_text.h"
#include "../../include/common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define BAD_SOCK INVALID_SOCKET
  #define sock_close closesocket
  #define SEND_FLAGS 0
  typedef CRITICAL_SECTION sv_mutex;
  static void mx_init(sv_mutex* m)    { InitializeCriticalSection(m); }
  static void mx_lock(sv_mutex* m)    { EnterCriticalSection(m); }
  static void mx_unlock(sv_mutex* m)  { LeaveCriticalSection(m); }
  static void mx_destroy(sv_mutex* m) { DeleteCriticalSection(m); }
  static void sleep_ms(int ms) { Sleep((DWORD)ms); }
  static double now_ms(void) { return (double)GetTickCount64(); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <pthread.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <time.h>
  #include <unistd.h>
  typedef int sock_t;
  #define BAD_SOCK (-1)
  #define sock_close close
  #define SEND_FLAGS MSG_NOSIGNAL
  typedef pthread_mutex_t sv_mutex;
  static void mx_init(sv_mutex* m)    { pthread_mutex_init(m, NULL); }
  static void mx_lock(sv_mutex* m)    { pthread_mutex_lock(m); }
  static void mx_unlock(sv_mutex* m)  { pthread_mutex_unlock(m); }
  static void mx_destroy(sv_mutex* m) { pthread_mutex_destroy(m); }
  static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
  static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
#endif

#define API_PREFIX      "/api/v1"     // prefisso predefinito (vedi SvServerConfig.api_prefix)
#define MAX_HEADER      (16 * 1024)
#define IDLE_TIMEOUT_MS 15000          // connessione keep-alive inattiva
#define MAX_PARAMS      1024

// sessione di un client: stato del motore che resta tra le richieste (vedi db_session_*)
#define SV_MAX_SESSIONS   256
#define SESSION_IDLE_MS   (5 * 60 * 1000)   // una sessione inattiva si chiude
#define TX_IDLE_MS        (30 * 1000)       // una transazione ferma si annulla (non blocca gli altri)
#define SESSION_WAIT_MS   10000             // attesa massima di una richiesta per una transazione altrui
// un sito con nome (vedi SvSite): cartella oppure backend del reverse proxy
typedef struct {
    char  name[128];          // in minuscolo
    char* dir;                // NULL = proxy
    char  phost[256];         // backend del proxy
    int   pport;
} Site;

typedef struct {
    char       id[33];
    DbSession* db;
    char       user[64];
    double     last_ms;
} SvSess;

struct SvServer {
    SvServerConfig cfg;
    char*          token;               // copie delle stringhe della configurazione
    char*          cors;
    char*          default_db;
    char*          static_dir;
    char           api[128];            // prefisso dell'API ("" = spenta)
    char*          doc_root;            // percorso assoluto di static_dir (SCRIPT_FILENAME per PHP)
    FcgiPool*      php;                 // NULL = PHP spento
    Site*          sites;               // siti con nome (--site); quelli automatici si cercano a ogni richiesta
    int            n_sites;
    sock_t         listen_fd;
    int            port;
    sock_t         mysql_fd;            // protocollo MySQL (BAD_SOCK = spento)
    int            mysql_port;
    uint32_t       mysql_ids;           // id delle connessioni MySQL
    atomic_int     stop;                // chiesto da un altro thread o da un gestore di segnale
    sv_mutex       conn_mx;             // conns[] e active
    sock_t*        conns;               // socket aperti (per chiuderli allo stop)
    int            active;
    sv_mutex       exec_mx;             // una richiesta alla volta sul motore
    FILE*          audit;               // registro di audit (scritto con exec_mx)
    // statistiche e registro (protetti da conn_mx)
    double         started_ms;
    long long      n_requests, n_errors, n_rows;
    double         exec_since;          // inizio della richiesta che usa il motore (0 = libero)
    bool           timed_out;           // il watchdog ha interrotto la richiesta in corso
    SvLogEntry     log[SV_LOG_MAX];     // anello: log[(seq - 1) % SV_LOG_MAX]
    SvBucket       series[SV_SERIES_MAX]; // anello per secondo: series[sec % SV_SERIES_MAX]
    SvSess         sess[SV_MAX_SESSIONS];  // sessioni dei client (protette da exec_mx)
};

typedef struct {
    SvServer* s;
    sock_t    fd;
    int       slot;
    char      client[48];
    int       client_port;
    bool      mysql;                   // connessione col protocollo MySQL
} Conn;

// ── richiesta / risposta ────────────────────────────────────────

typedef struct {
    char   method[16];
    char   uri[4096];                   // destinazione grezza, come arrivata (REQUEST_URI)
    char   path[2048];                  // decodificato (%xx)
    char   query[2048];                 // grezzo, dopo '?'
    bool   http10;
    bool   keep_alive;
    bool   has_length;
    bool   chunked;
    size_t content_length;
    char   content_type[128];
    char   auth[512];
    char   user[64];                    // utente autenticato (Basic) o "" (root: token o nessun login)
    char   session[80];                 // header X-SegVault-Session
    char   origin[256];
    char   host[256];
    char*  body;                        // NUL-terminato
    size_t body_len;
    char*  headers;                     // copia delle righe d'intestazione (solo con PHP: diventano HTTP_*)
    char   range[128];                  // Range: bytes=...
    char   if_none_match[256];
    char   if_modified_since[64];
    bool   accept_gzip;                 // Accept-Encoding comprende gzip
    const char* client;                 // indirizzo e porta del client (REMOTE_ADDR / REMOTE_PORT)
    int    client_port;
    // sito scelto dall'Host (vedi resolve_site)
    const char* root;                   // cartella del sito (NULL = nessuna)
    char   root_buf[4096];
    char   root_abs[4096];              // percorso assoluto (DOCUMENT_ROOT di PHP)
    const Site* proxy;                  // sito servito da un altro server (reverse proxy)
    char   site[128];                   // nome del sito ("" = quello predefinito)
    bool   upgrade;                     // Upgrade: websocket
} Request;

typedef struct {
    int         status;
    const char* content_type;
    JsonBuf     body;
    const char* extra_headers;          // righe "Nome: valore\r\n" già formattate
    char*       own_headers;            // intestazioni allocate (risposte di PHP), liberate dopo l'invio
    char        ctype[160];             // Content-Type scelto da PHP
    FILE*       file;                   // file statico inviato a pezzi (corpo vuoto), chiuso dopo l'invio
    long long   file_off, file_len;     // parte del file da inviare (Range)
    bool        close;
    char        detail[120];            // per il registro: SQL eseguito o errore
    long long   rows;                   // righe restituite
    char        client[48];             // indirizzo del client (registro di audit)
} Response;

// sottostringa senza distinzione tra maiuscole e minuscole (strcasestr non c'è su Windows)
static const char* ci_strstr(const char* h, const char* n) {
    size_t nl = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, nl)) return h;
    return nl ? NULL : h;
}

static const char* status_text(int st) {
    switch (st) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return st < 300 ? "OK" : st < 400 ? "Redirect" : st < 500 ? "Client Error" : "Server Error";
    }
}

static bool send_all(sock_t fd, const char* p, size_t n) {
    while (n > 0) {
        int chunk = n > (1u << 30) ? (1 << 30) : (int)n;
        int w = (int)send(fd, p, chunk, SEND_FLAGS);
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

// tipi che vale la pena comprimere (testo); le immagini e i video sono già compressi
static bool compressible(const char* ctype) {
    if (!ctype) return true;                               // JSON dell'API
    return !strncmp(ctype, "text/", 5) || strstr(ctype, "json") || strstr(ctype, "javascript") ||
           strstr(ctype, "xml") || strstr(ctype, "svg") || strstr(ctype, "wasm") || strstr(ctype, "font/ttf");
}

#define GZIP_MIN   1024                // corpi più piccoli: la compressione non conviene
#define GZIP_MAX   (8u << 20)          // corpi più grandi: troppa CPU per richiesta

static bool send_response(SvServer* s, sock_t fd, const Request* rq, Response* rs) {
    JsonBuf h = {0};
    const char* body = rs->body.oom ? NULL : rs->body.data;
    size_t blen = rs->body.oom ? 0 : rs->body.len;
    uint8_t* gz = NULL;
    bool gzipped = false;
    if (rq && rq->accept_gzip && !rs->file && blen >= GZIP_MIN && blen <= GZIP_MAX && (rs->status == 200 || rs->status == 404) &&
        compressible(rs->content_type) && !(rs->extra_headers && ci_strstr(rs->extra_headers, "Content-Encoding"))) {
        size_t gl;
        gz = gzip_compress((const uint8_t*)body, blen, &gl);
        if (gz && gl < blen) { body = (const char*)gz; blen = gl; gzipped = true; }
    }
    unsigned long long clen = rs->file ? (unsigned long long)rs->file_len : (unsigned long long)blen;
    jb_printf(&h,
        "HTTP/1.1 %d %s\r\n"
        "Server: SegVault/" SV_VERSION_STR "\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %llu\r\n"
        "Connection: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        rs->status, status_text(rs->status),
        rs->content_type ? rs->content_type : "application/json; charset=utf-8",
        clen, rs->close ? "close" : "keep-alive");
    if (gzipped) jb_lit(&h, "Content-Encoding: gzip\r\n");
    if (gzipped || (rs->content_type && !rs->file && compressible(rs->content_type) && rs->status == 200))
        jb_lit(&h, "Vary: Accept-Encoding\r\n");
    bool own_acao = rs->extra_headers && ci_strstr(rs->extra_headers, "Access-Control-Allow-Origin");
    if (s->cors && !own_acao) {
        // origine ammessa: "*" oppure quella configurata, ripetuta solo se coincide con la richiesta
        const char* allow = !strcmp(s->cors, "*") ? "*" : (rq && !strcmp(rq->origin, s->cors) ? s->cors : NULL);
        if (allow) jb_printf(&h, "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", allow);
    }
    if (rs->extra_headers) jb_raw(&h, rs->extra_headers, strlen(rs->extra_headers));
    jb_lit(&h, "\r\n");
    bool head_only = (rq && !strcmp(rq->method, "HEAD")) || rs->status == 304;
    // intestazione e corpo piccolo in un solo invio
    if (!h.oom && !head_only && !rs->file && blen > 0 && blen <= 16384) { jb_raw(&h, body, blen); blen = 0; head_only = true; }
    bool ok = !h.oom && send_all(fd, h.data, h.len);
    free(h.data);
    if (ok && !head_only && rs->file) {                     // file statico a pezzi: niente copia intera in memoria
#ifdef _WIN32
        ok = _fseeki64(rs->file, rs->file_off, SEEK_SET) == 0;
#else
        ok = fseeko(rs->file, (off_t)rs->file_off, SEEK_SET) == 0;
#endif
        char chunk[65536];
        long long left = rs->file_len;
        while (ok && left > 0) {
            size_t want = left < (long long)sizeof(chunk) ? (size_t)left : sizeof(chunk);
            size_t got = fread(chunk, 1, want, rs->file);
            if (got == 0) { ok = false; break; }              // file accorciato nel frattempo
            ok = send_all(fd, chunk, got);
            left -= (long long)got;
        }
    } else if (ok && !head_only && blen > 0) ok = send_all(fd, body, blen);
    free(gz);
    return ok;
}

// testo su una riga, accorciato (per il registro)
static void one_line(char* out, size_t outsz, const char* s) {
    size_t k = 0;
    bool sp = false;
    for (; *s && k + 4 < outsz; s++) {
        if (isspace((unsigned char)*s)) { if (k && !sp) out[k++] = ' '; sp = true; }
        else { out[k++] = *s; sp = false; }
    }
    if (*s) { memcpy(out + k, "...", 3); k += 3; }
    out[k] = '\0';
}

static void json_error(Response* rs, int status, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void json_error(Response* rs, int status, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rs->status = status;
    rs->body.len = 0;
    one_line(rs->detail, sizeof(rs->detail), msg);
    jb_lit(&rs->body, "{\"ok\":false,\"error\":{\"message\":");
    jb_str(&rs->body, msg);
    jb_lit(&rs->body, "}}");
}

// ── utilità ─────────────────────────────────────────────────────

static int url_decode(const char* in, size_t n, char* out, size_t outsz, bool plus_space) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (k + 1 >= outsz) return -1;
        char c = in[i];
        if (c == '%' && i + 2 < n + 0 && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char h[3] = { in[i + 1], in[i + 2], 0 };
            c = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (c == '+' && plus_space) c = ' ';
        if (c == '\0') return -1;          // %00: mai accettato
        out[k++] = c;
    }
    out[k] = '\0';
    return (int)k;
}

// valore di un parametro della query string (decodificato); false se manca
static bool query_param(const char* q, const char* name, char* out, size_t outsz) {
    size_t nl = strlen(name);
    while (*q) {
        const char* amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg >= nl && !strncmp(q, name, nl) && (seg == nl || q[nl] == '=')) {
            const char* v = seg == nl ? q + nl : q + nl + 1;
            size_t vl = seg == nl ? 0 : seg - nl - 1;
            return url_decode(v, vl, out, outsz, true) >= 0;
        }
        q += seg + (amp ? 1 : 0);
    }
    return false;
}

// confronto a tempo costante (il token non si indovina misurando i tempi)
static bool secret_equal(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++) d |= (unsigned char)(a[i] ^ b[lb ? i % lb : 0]);
    return d == 0;
}

static bool valid_ident(const char* s) {
    if (!s[0] || strlen(s) > 63) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return false;
    return true;
}

// ── parametri: '?' sostituiti da letterali SQL ──────────────────
// Si salta ciò che il lexer non legge come codice: stringhe ('...' e "..." con '' e \x),
// identificatori tra backtick, commenti -- e /* */.

static bool append_literal(JsonBuf* o, const JsonVal* v, char* err, size_t errsz, int idx) {
    switch (v->type) {
        case JSON_NULL: jb_lit(o, "NULL"); return true;
        case JSON_BOOL: jb_raw(o, v->boolean ? "TRUE" : "FALSE", v->boolean ? 4 : 5); return true;
        case JSON_NUMBER:
            // tra parentesi: "x-?" con -5 diventerebbe "x--5", cioè un commento
            if (v->num_text[0] == '-') { jb_lit(o, "("); jb_raw(o, v->num_text, strlen(v->num_text)); jb_lit(o, ")"); }
            else jb_raw(o, v->num_text, strlen(v->num_text));
            return true;
        case JSON_STRING:
            if (memchr(v->str, '\0', v->len)) { snprintf(err, errsz, "params[%d]: strings cannot contain NUL characters", idx); return false; }
            jb_lit(o, "'");
            for (size_t i = 0; i < v->len; i++) {
                if (v->str[i] == '\'') jb_lit(o, "''");
                else if (v->str[i] == '\\') jb_lit(o, "\\\\");
                else jb_raw(o, &v->str[i], 1);
            }
            jb_lit(o, "'");
            return true;
        default:
            snprintf(err, errsz, "params[%d]: only null, booleans, numbers and strings can be bound", idx);
            return false;
    }
}

// params: array -> segnaposto '?' nell'ordine; oggetto -> segnaposto con nome ':nome'
static char* bind_params(const char* sql, const JsonVal* params, char* err, size_t errsz) {
    JsonBuf o = {0};
    bool named = params && params->type == JSON_OBJECT;
    int used = 0, nparams = params && !named ? params->count : 0;
    size_t n = strlen(sql), i = 0;
    while (i < n) {
        char c = sql[i];
        size_t st = i;
        if (c == '\'' || c == '"' || c == '`') {
            i++;
            while (i < n) {
                if (sql[i] == '\\' && c != '`' && i + 1 < n) { i += 2; continue; }
                if (sql[i] == c) { if (i + 1 < n && sql[i + 1] == c) { i += 2; continue; } i++; break; }
                i++;
            }
            jb_raw(&o, sql + st, i - st);
        } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') i++;
            jb_raw(&o, sql + st, i - st);
        } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            const char* e = strstr(sql + i + 2, "*/");
            i = e ? (size_t)(e - sql) + 2 : n;
            jb_raw(&o, sql + st, i - st);
        } else if (c == ':' && named && i + 1 < n && (isalpha((unsigned char)sql[i + 1]) || sql[i + 1] == '_') &&
                   (i == 0 || (!isalnum((unsigned char)sql[i - 1]) && sql[i - 1] != '_' && sql[i - 1] != ':'))) {
            size_t a = i + 1, b = a;
            while (b < n && (isalnum((unsigned char)sql[b]) || sql[b] == '_')) b++;
            char name[64];
            if (b - a >= sizeof(name)) { snprintf(err, errsz, "parameter name too long"); free(o.data); return NULL; }
            memcpy(name, sql + a, b - a);
            name[b - a] = '\0';
            const JsonVal* v = json_get(params, name);
            if (!v) { snprintf(err, errsz, "missing parameter :%s", name); free(o.data); return NULL; }
            if (!append_literal(&o, v, err, errsz, used)) { free(o.data); return NULL; }
            used++;
            i = b;
        } else if (c == '?') {
            if (named) { snprintf(err, errsz, "use :name placeholders with named parameters"); free(o.data); return NULL; }
            if (used >= nparams) {
                snprintf(err, errsz, "the SQL has more '?' placeholders than params (%d)", nparams);
                free(o.data);
                return NULL;
            }
            if (!append_literal(&o, &params->items[used], err, errsz, used)) { free(o.data); return NULL; }
            used++;
            i++;
        } else {
            while (i < n && sql[i] != '\'' && sql[i] != '"' && sql[i] != '`' && sql[i] != '-' && sql[i] != '/' && sql[i] != '?' && sql[i] != ':') i++;
            if (i == st) i++;
            jb_raw(&o, sql + st, i - st);
        }
    }
    if (!named && used != nparams) {
        snprintf(err, errsz, "%d params given but the SQL has %d '?' placeholders", nparams, used);
        free(o.data);
        return NULL;
    }
    if (o.oom) { snprintf(err, errsz, "out of memory"); free(o.data); return NULL; }
    if (!o.data) return strdup("");
    return o.data;
}

// ── risultati in JSON ───────────────────────────────────────────

// testo che è già un numero JSON valido (così si scrive senza virgolette)
static bool is_json_number(const char* s) {
    const char* p = s;
    if (*p == '-') p++;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (*p >= '0' && *p <= '9') p++;
    else return false;
    if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return false; while (isdigit((unsigned char)*p)) p++; }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    return *p == '\0';
}

static void cell_json(JsonBuf* b, const Cell* c) {
    if (c->is_null || c->type == CELL_TYPE_NULL) { jb_lit(b, "null"); return; }
    switch (c->type) {
        case CELL_TYPE_INT:
        case CELL_TYPE_FLOAT:
            if (is_json_number(c->value)) { jb_raw(b, c->value, strlen(c->value)); return; }
            break;
        case CELL_TYPE_BOOL:
            if (!strcmp(c->value, "1") || !strcasecmp(c->value, "true"))  { jb_lit(b, "true");  return; }
            if (!strcmp(c->value, "0") || !strcasecmp(c->value, "false")) { jb_lit(b, "false"); return; }
            break;
        default: break;
    }
    jb_str(b, c->value);
}

static void result_json(JsonBuf* b, const QueryResult* r, bool objects) {
    jb_lit(b, "{");
    if (r->num_cols > 0) {
        jb_lit(b, "\"columns\":[");
        for (int j = 0; j < r->num_cols; j++) {
            jb_raw(b, j ? ",{\"name\":" : "{\"name\":", j ? 9 : 8);
            jb_str(b, r->col_names[j]);
            jb_lit(b, ",\"type\":");
            jb_str(b, r->col_types && r->col_types[j] ? r->col_types[j] : "");
            jb_lit(b, "}");
        }
        jb_lit(b, "],\"rows\":[");
        for (int i = 0; i < r->num_rows && !b->oom; i++) {
            const Row* row = &r->rows[i];
            if (i) jb_lit(b, ",");
            jb_raw(b, objects ? "{" : "[", 1);
            for (int j = 0; j < row->num_cells && j < r->num_cols; j++) {
                if (j) jb_lit(b, ",");
                if (objects) { jb_str(b, r->col_names[j]); jb_lit(b, ":"); }
                cell_json(b, &row->cells[j]);
            }
            jb_raw(b, objects ? "}" : "]", 1);
        }
        jb_printf(b, "],\"row_count\":%d", r->num_rows);
    } else {
        jb_printf(b, "\"rows_affected\":%d", r->rows_affected);
        if (r->last_insert_id > 0) jb_printf(b, ",\"last_insert_id\":%d", r->last_insert_id);
    }
    jb_printf(b, ",\"time_ms\":%.3f}", r->exec_time_ms);
}

static int line_of(const char* s, int pos) {
    int line = 1;
    for (int i = 0; i < pos && s[i]; i++) if (s[i] == '\n') line++;
    return line;
}

static bool in_transaction(void) {
    SessionInfo* si = db_session_info();
    bool r = si && si->in_transaction;
    db_session_free(si);
    return r;
}

// il motore è occupato da questa richiesta: il watchdog (sv_server_run) la può interrompere
static void exec_begin(SvServer* s) {
    mx_lock(&s->exec_mx);
    mx_lock(&s->conn_mx);
    s->exec_since = now_ms();
    s->timed_out = false;
    mx_unlock(&s->conn_mx);
}

static bool exec_timed_out(SvServer* s) {
    mx_lock(&s->conn_mx);
    bool t = s->timed_out;
    mx_unlock(&s->conn_mx);
    return t;
}

static void exec_end(SvServer* s) {
    mx_lock(&s->conn_mx);
    s->exec_since = 0;
    if (s->timed_out) db_clear_cancel();
    s->timed_out = false;
    mx_unlock(&s->conn_mx);
    mx_unlock(&s->exec_mx);
}

// ── POST /api/v1/query ──────────────────────────────────────────

// ── sessioni ────────────────────────────────────────────────────

// chiude le sessioni inattive e annulla le transazioni ferme (con exec_mx preso)
static void expire_sessions(SvServer* s) {
    double now = now_ms();
    for (int i = 0; i < SV_MAX_SESSIONS; i++) {
        SvSess* x = &s->sess[i];
        if (!x->db) continue;
        double idle = now - x->last_ms;
        if (idle > SESSION_IDLE_MS || (db_session_in_transaction(x->db) && idle > TX_IDLE_MS)) {
            db_session_end(x->db);
            memset(x, 0, sizeof(*x));
        }
    }
}

// (con exec_mx preso dal chiamante oppure no: si prende qui per la ricerca)
static SvSess* find_session(SvServer* s, const char* id) {
    mx_lock(&s->exec_mx);
    SvSess* r = NULL;
    for (int i = 0; i < SV_MAX_SESSIONS && !r; i++)
        if (s->sess[i].db && secret_equal(s->sess[i].id, id)) r = &s->sess[i];
    mx_unlock(&s->exec_mx);
    return r;
}

// POST /api/v1/sessions (nuova) | DELETE /api/v1/sessions/ID (chiusa, transazione annullata)
static void handle_sessions(SvServer* s, Request* rq, Response* rs, const char* rest) {
    if (!strcmp(rq->method, "POST") && !rest[0]) {
        mx_lock(&s->exec_mx);
        expire_sessions(s);
        SvSess* x = NULL;
        for (int i = 0; i < SV_MAX_SESSIONS && !x; i++) if (!s->sess[i].db) x = &s->sess[i];
        if (x && sv_random_token(x->id, 16) && (x->db = db_session_new())) {
            snprintf(x->user, sizeof(x->user), "%s", rq->user);
            x->last_ms = now_ms();
            rs->status = 200;
            jb_printf(&rs->body, "{\"ok\":true,\"session\":\"%s\",\"idle_timeout_s\":%d,\"transaction_idle_timeout_s\":%d}",
                      x->id, SESSION_IDLE_MS / 1000, TX_IDLE_MS / 1000);
        } else {
            if (x) memset(x, 0, sizeof(*x));
            json_error(rs, 503, "too many open sessions (max %d): close some with DELETE", SV_MAX_SESSIONS);
        }
        mx_unlock(&s->exec_mx);
        return;
    }
    if (!strcmp(rq->method, "DELETE") && rest[0] == '/' && rest[1]) {
        SvSess* x = find_session(s, rest + 1);
        if (!x || strcmp(x->user, rq->user)) { json_error(rs, 404, "session not found"); return; }
        mx_lock(&s->exec_mx);
        db_session_end(x->db);                   // una transazione aperta viene annullata
        memset(x, 0, sizeof(*x));
        mx_unlock(&s->exec_mx);
        rs->status = 200;
        jb_lit(&rs->body, "{\"ok\":true}");
        return;
    }
    json_error(rs, 405, "use POST /api/v1/sessions or DELETE /api/v1/sessions/ID");
}

// tutti gli statement cominciano con SELECT o WITH (dopo spazi e commenti)? Il motore lo ricontrolla
static bool reads_only(const char* script, const SqlRange* ranges, int n) {
    int any = 0;
    for (int k = 0; k < n; k++) {
        if (!ranges[k].has_code) continue;
        const char* p = script + ranges[k].code_start;
        while (isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "SELECT", 6) && strncasecmp(p, "WITH", 4)) return false;
        char c = p[toupper((unsigned char)p[0]) == 'S' ? 6 : 4];
        if (isalnum((unsigned char)c) || c == '_') return false;
        any++;
    }
    return any > 0;
}

// registro di audit: una riga per ogni istruzione che può cambiare qualcosa (anche se fallisce)
//   2026-11-11 17:20:01  127.0.0.1  anna  shop  ok 3 rows  INSERT INTO ...
static void audit_stmt(SvServer* s, const Response* rs, const char* user, const char* stmt, const QueryResult* r) {
    if (!s->audit) return;
    const char* p = stmt;
    while (isspace((unsigned char)*p)) p++;
    static const char* const READS[] = { "SELECT", "WITH", "SHOW", "DESCRIBE", "DESC", "EXPLAIN" };
    for (int i = 0; i < 6; i++) {
        size_t l = strlen(READS[i]);
        if (!strncasecmp(p, READS[i], l) && !isalnum((unsigned char)p[l]) && p[l] != '_') return;
    }
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char when[32], text[1024];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    one_line(text, sizeof(text), p);
    const char* db = db_current_database();
    if (r && r->success)
        fprintf(s->audit, "%s  %s  %s  %s  ok %d rows  %s\n", when, rs->client[0] ? rs->client : "-", user && user[0] ? user : "root",
                db && db[0] ? db : "-", r->rows_affected, text);
    else {
        char e[200];
        one_line(e, sizeof(e), r ? r->error : "out of memory");
        fprintf(s->audit, "%s  %s  %s  %s  ERROR %s  %s\n", when, rs->client[0] ? rs->client : "-", user && user[0] ? user : "root",
                db && db[0] ? db : "-", e, text);
    }
    fflush(s->audit);
}

// esegue lo script (già con i parametri) nel database indicato e scrive la risposta;
// sql_for_log: il testo originale, con i segnaposto, per il registro. Libera script.
static void run_script(SvServer* s, Response* rs, char* script, const char* sql_for_log, const char* dbname,
                       bool want_tx, bool objects, const char* user, SvSess* ss) {
    int len = (int)strlen(script), cap = 64, nst;
    SqlRange* ranges = NULL;
    for (;;) {                                  // quanti statement: si raddoppia finché bastano
        free(ranges);
        ranges = malloc(sizeof(SqlRange) * (size_t)cap);
        if (!ranges) { free(script); json_error(rs, 500, "out of memory"); return; }
        nst = sql_split(script, len, ranges, cap);
        if (nst < cap) break;
        cap *= 2;
    }

    double t0 = now_ms();
    one_line(rs->detail, sizeof(rs->detail), sql_for_log);   // con i '?': i valori dei parametri non finiscono nel registro
    JsonBuf res = {0}, *b = &res;               // i risultati; la risposta si compone alla fine
    bool failed = false, first = true;
    char errmsg[1100] = "";
    int err_stmt = -1, err_line = 0, err_col = 0;
    const char* warning = NULL;
    bool session_tx = false;
    // solo SELECT, senza sessione né transazione: in parallelo con le altre letture
    bool parallel = !ss && !want_tx && reads_only(script, ranges, nst);
    if (parallel) {
        int idx = 0;
        for (int k = 0; k < nst; k++) {
            if (!ranges[k].has_code) continue;
            int a = ranges[k].code_start, e = ranges[k].end;
            char saved = script[e];
            script[e] = '\0';
            bool refused = false;
            QueryResult* r = db_execute_readonly(script + a, dbname, user, s->cfg.timeout_ms, &refused);
            script[e] = saved;
            if (refused) { parallel = false; break; }   // la via normale rifà tutto (sono solo letture)
            if (!r->success) {
                failed = true;
                err_stmt = idx;
                if (strstr(r->error, "time limit")) snprintf(errmsg, sizeof(errmsg), "the request took longer than %d ms and was stopped", s->cfg.timeout_ms);
                else snprintf(errmsg, sizeof(errmsg), "%s", r->error);
                err_line = r->error_line > 0 ? line_of(script, a) + r->error_line - 1 : line_of(script, a);
                if (r->error_line > 0) err_col = r->error_col;
                db_result_free(r);
                break;
            }
            if (!first) jb_lit(b, ",");
            first = false;
            result_json(b, r, objects);
            rs->rows += r->num_rows;
            db_result_free(r);
            idx++;
        }
        if (!parallel) {                        // si ricomincia per la via normale
            free(res.data);
            memset(&res, 0, sizeof(res));
            failed = false; first = true; errmsg[0] = '\0'; err_stmt = -1; err_line = err_col = 0;
            rs->rows = 0;
        }
    }
    if (!parallel) {
    exec_begin(s);
    expire_sessions(s);
    // un'altra sessione ha una transazione aperta: si aspetta che finisca (o che scada)
    for (double waited = 0; !db_session_enter(ss ? ss->db : NULL); waited += 20) {
        exec_end(s);
        if (waited >= SESSION_WAIT_MS) {
            free(ranges);
            free(script);
            json_error(rs, 409, "another session has an open transaction: retry after its COMMIT / ROLLBACK "
                                "(an idle transaction is rolled back after %d s)", TX_IDLE_MS / 1000);
            return;
        }
        sleep_ms(20);
        exec_begin(s);
        expire_sessions(s);
    }
    // permessi dell'utente della richiesta (vuoto = root), solo per gli statement di questa richiesta
    if (!db_set_user(user)) {
        if (ss) db_session_leave(ss->db);
        exec_end(s);
        free(ranges);
        free(script);
        json_error(rs, 401, "user '%s' no longer exists", user ? user : "");
        return;
    }
    // il database corrente si ripristina alla fine: una richiesta non cambia quello delle successive
    char prev_db[128] = "";
    const char* cur = db_current_database();
    if (cur) snprintf(prev_db, sizeof(prev_db), "%s", cur);


    if (dbname[0]) {
        char use[96];
        snprintf(use, sizeof(use), "USE %.63s", dbname);       // valid_ident: al massimo 63 caratteri
        QueryResult* r = db_execute(use);
        if (!r || !r->success) {
            failed = true;
            snprintf(errmsg, sizeof(errmsg), "%s", r ? r->error : "out of memory");
        }
        db_result_free(r);
    }
    if (!failed && want_tx && !db_begin()) {
        failed = true;
        snprintf(errmsg, sizeof(errmsg), "cannot start a transaction");
    }
    int idx = 0;
    for (int k = 0; !failed && k < nst; k++) {
        if (!ranges[k].has_code) continue;
        int a = ranges[k].code_start, e = ranges[k].end;
        char saved = script[e];
        script[e] = '\0';
        QueryResult* r = db_execute(script + a);
        audit_stmt(s, rs, user, script + a, r);
        script[e] = saved;
        bool timeout = exec_timed_out(s);
        if (!r || !r->success || timeout) {
            failed = true;
            err_stmt = idx;
            if (timeout) snprintf(errmsg, sizeof(errmsg), "the request took longer than %d ms and was stopped", s->cfg.timeout_ms);
            else snprintf(errmsg, sizeof(errmsg), "%s", r ? r->error : "out of memory");
            if (r && r->error_line > 0) { err_line = line_of(script, a) + r->error_line - 1; err_col = r->error_col; }
            else err_line = line_of(script, a);
        } else {
            if (!first) jb_lit(b, ",");
            first = false;
            result_json(b, r, objects);
            rs->rows += r->num_rows;
        }
        db_result_free(r);
        idx++;
    }
    if (want_tx) {
        if (failed) { if (in_transaction()) db_rollback(); }
        else if (!db_commit()) { failed = true; snprintf(errmsg, sizeof(errmsg), "COMMIT failed"); }
    }
    if (ss) {                                   // la sessione conserva database e transazione
        session_tx = in_transaction();
        db_session_leave(ss->db);
        ss->last_ms = now_ms();
        prev_db[0] = '\0';
    } else if (in_transaction()) {              // BEGIN senza COMMIT: la richiesta è un'unità chiusa
        db_rollback();
        warning = failed ? "the open transaction was rolled back"
                         : "a transaction was left open and has been rolled back (end it with COMMIT)";
    }
    db_set_user(NULL);
    cur = db_current_database();
    if (prev_db[0] && (!cur || strcmp(cur, prev_db))) {
        char use[160];
        snprintf(use, sizeof(use), "USE %s", prev_db);
        db_result_free(db_execute(use));        // fallisce solo se il database è stato eliminato
    }
    exec_end(s);
    }

    b = &rs->body;
    jb_raw(b, failed ? "{\"ok\":false,\"results\":[" : "{\"ok\":true,\"results\":[", failed ? 23 : 22);
    if (res.data) jb_raw(b, res.data, res.len);
    if (res.oom) b->oom = true;
    free(res.data);
    jb_lit(b, "]");
    if (failed) {
        one_line(rs->detail, sizeof(rs->detail), errmsg);
        jb_lit(b, ",\"error\":{\"message\":");
        jb_str(b, errmsg);
        if (err_stmt >= 0) jb_printf(b, ",\"statement\":%d,\"line\":%d", err_stmt, err_line);
        if (err_stmt >= 0 && err_col > 0) jb_printf(b, ",\"column\":%d", err_col);
        jb_lit(b, "}");
    }
    if (warning) { jb_lit(b, ",\"warning\":"); jb_str(b, warning); }
    if (ss) jb_printf(b, ",\"session\":{\"id\":\"%s\",\"in_transaction\":%s}", ss->id, session_tx ? "true" : "false");
    jb_printf(b, ",\"time_ms\":%.3f}", now_ms() - t0);
    rs->status = failed ? 400 : 200;
    if (b->oom) json_error(rs, 500, "result too large for memory");
    free(ranges);
    free(script);
}

// legge una richiesta /query (JSON o SQL puro); false = errore già scritto in rs.
// *root riceve l'albero JSON (lo libera chi chiama, anche in caso di errore)
static bool read_query_request(SvServer* s, Request* rq, Response* rs, JsonVal** root, const char** sql,
                               char* dbname, size_t dbn, const JsonVal** params, bool* want_tx, bool* objects) {
    char err[512];
    char ct[128];
    snprintf(ct, sizeof(ct), "%s", rq->content_type);
    for (char* p = ct; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strncmp(ct, "text/plain", 10) || !strncmp(ct, "application/sql", 15)) {
        // corpo = SQL; database e formato nella query string
        *sql = rq->body;
        query_param(rq->query, "database", dbname, dbn);
        char f[32];
        if (query_param(rq->query, "rows", f, sizeof(f))) *objects = !strcmp(f, "objects");
    } else if (!ct[0] || !strncmp(ct, "application/json", 16)) {
        *root = json_parse(rq->body, rq->body_len, err, sizeof(err));
        if (!*root) { json_error(rs, 400, "invalid JSON: %s", err); return false; }
        if ((*root)->type != JSON_OBJECT) { json_error(rs, 400, "the body must be a JSON object"); return false; }
        const JsonVal* v = json_get(*root, "sql");
        if (!v || v->type != JSON_STRING) { json_error(rs, 400, "missing \"sql\" (string)"); return false; }
        if (memchr(v->str, '\0', v->len)) { json_error(rs, 400, "\"sql\" cannot contain NUL characters"); return false; }
        *sql = v->str;
        v = json_get(*root, "database");
        if (v && v->type == JSON_STRING) snprintf(dbname, dbn, "%s", v->str);
        else if (v && v->type != JSON_NULL) { json_error(rs, 400, "\"database\" must be a string"); return false; }
        *params = json_get(*root, "params");
        if (*params && (*params)->type == JSON_NULL) *params = NULL;
        if (*params && (*params)->type != JSON_ARRAY) { json_error(rs, 400, "\"params\" must be an array"); return false; }
        if (*params && (*params)->count > MAX_PARAMS) { json_error(rs, 400, "too many params (max %d)", MAX_PARAMS); return false; }
        v = json_get(*root, "transaction");
        *want_tx = v && v->type == JSON_BOOL && v->boolean;
        v = json_get(*root, "rows");
        if (v && v->type == JSON_STRING) *objects = !strcmp(v->str, "objects");
    } else {
        json_error(rs, 415, "use Content-Type application/json or text/plain");
        return false;
    }
    if (!dbname[0] && s->default_db) snprintf(dbname, dbn, "%s", s->default_db);
    if (dbname[0] && !valid_ident(dbname)) { json_error(rs, 400, "invalid database name"); return false; }
    return true;
}

static void handle_query(SvServer* s, Request* rq, Response* rs) {
    JsonVal* root = NULL;
    const char* sql = NULL;
    const JsonVal* params = NULL;
    char dbname[128] = "";
    bool want_tx = false, objects = false;
    if (read_query_request(s, rq, rs, &root, &sql, dbname, sizeof(dbname), &params, &want_tx, &objects)) {
        char err[512];
        char* script = bind_params(sql, params, err, sizeof(err));
        if (!script) json_error(rs, 400, "%s", err);
        else {
            SvSess* ss = NULL;
            if (rq->session[0]) {
                ss = find_session(s, rq->session);
                if (!ss) { free(script); json_free(root); json_error(rs, 404, "session not found or expired: create one with POST /api/v1/sessions"); return; }
                if (strcmp(ss->user, rq->user)) { free(script); json_free(root); json_error(rs, 403, "this session belongs to another user"); return; }
            }
            run_script(s, rs, script, sql, dbname, want_tx, objects, rq->user, ss);   // libera script
        }
    }
    json_free(root);
}

// ── endpoint salvati: /api/v1/endpoints[/nome] ──────────────────
// Una query con un nome, salvata nella tabella api_endpoints del database:
//   CREATE TABLE api_endpoints (name VARCHAR(60) PRIMARY KEY, sql TEXT NOT NULL,
//                               access VARCHAR(10) NOT NULL DEFAULT 'token', description VARCHAR(200));
// GET / POST /api/v1/endpoints/nome?database=db&param=valore esegue lo SQL legando i :param
// (dalla query string o da un oggetto JSON nel corpo). access = 'public': chiamabile senza token e
// da qualsiasi sito (per una pagina pubblica); 'token': come il resto dell'API.

#define ENDPOINTS_TABLE "api_endpoints"

// 1 = trovato, 0 = non esiste (o manca la tabella), -1 = errore (database inesistente, ...)
static int endpoint_lookup(SvServer* s, const char* db, const char* name, char** sql_out, bool* is_public,
                           char* err, size_t errn) {
    *sql_out = NULL;
    *is_public = false;
    mx_lock(&s->exec_mx);
    char prev[128] = "";
    const char* cur = db_current_database();
    if (cur) snprintf(prev, sizeof(prev), "%s", cur);
    char q[300];
    snprintf(q, sizeof(q), "USE %.63s", db);
    QueryResult* r = db_execute(q);
    int found = -1;
    if (!r || !r->success) snprintf(err, errn, "database '%.63s' not found", db);
    else {
        db_result_free(r);
        snprintf(q, sizeof(q), "SELECT `sql`, `access` FROM " ENDPOINTS_TABLE " WHERE `name` = '%.63s'", name);
        r = db_execute(q);
        if (!r || !r->success) found = 0;                       // nessuna tabella api_endpoints
        else if (r->num_rows == 0) found = 0;
        else {
            found = 1;
            *sql_out = strdup(r->rows[0].cells[0].is_null ? "" : r->rows[0].cells[0].value);
            *is_public = !r->rows[0].cells[1].is_null && !strcasecmp(r->rows[0].cells[1].value, "public");
        }
    }
    db_result_free(r);
    cur = db_current_database();
    if (prev[0] && (!cur || strcmp(cur, prev))) { snprintf(q, sizeof(q), "USE %.63s", prev); db_result_free(db_execute(q)); }
    mx_unlock(&s->exec_mx);
    return found;
}

// la chiave è già nell'oggetto JSON in costruzione? (vale la prima occorrenza)
static bool seen_key(const JsonBuf* j, const char* key) {
    JsonBuf k = {0};
    jb_str(&k, key);
    jb_lit(&k, ":");
    bool seen = j->data && k.data && strstr(j->data, k.data) != NULL;
    free(k.data);
    return seen;
}

// parametri dalla query string ("max=10&nome=pane", tranne database e rows): numeri come numeri
static JsonVal* query_params_json(const char* q) {
    JsonBuf j = {0};
    jb_lit(&j, "{");
    int n = 0;
    while (*q) {
        const char* amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        const char* eq = memchr(q, '=', seg);
        char key[128], val[2048];
        if (eq && url_decode(q, (size_t)(eq - q), key, sizeof(key), true) > 0 &&
            url_decode(eq + 1, seg - (size_t)(eq - q) - 1, val, sizeof(val), true) >= 0 &&
            strcmp(key, "database") && strcmp(key, "rows") && !seen_key(&j, key)) {
            if (n++) jb_lit(&j, ",");
            jb_str(&j, key);
            jb_lit(&j, ":");
            if (is_json_number(val)) jb_raw(&j, val, strlen(val));
            else jb_str(&j, val);
        }
        q += seg + (amp ? 1 : 0);
    }
    jb_lit(&j, "}");
    char err[64];
    JsonVal* v = j.oom ? NULL : json_parse(j.data, j.len, err, sizeof(err));
    free(j.data);
    return v;
}

static void handle_endpoint(SvServer* s, Request* rq, Response* rs, const char* name, const char* db, const char* sql) {
    char err[512];
    JsonVal* params = NULL;
    bool objects = true;
    char f[32];
    if (query_param(rq->query, "rows", f, sizeof(f))) objects = strcmp(f, "arrays") != 0;
    if (!strcmp(rq->method, "POST") && rq->body_len > 0) {
        params = json_parse(rq->body, rq->body_len, err, sizeof(err));
        if (!params) { json_error(rs, 400, "invalid JSON: %s", err); return; }
        if (params->type != JSON_OBJECT) { json_free(params); json_error(rs, 400, "the body must be a JSON object of parameters"); return; }
    } else {
        params = query_params_json(rq->query);
        if (!params) { json_error(rs, 400, "invalid parameters"); return; }
    }
    char* script = bind_params(sql, params, err, sizeof(err));
    json_free(params);
    if (!script) { json_error(rs, 400, "endpoint '%s': %s", name, err); return; }
    char label[200];
    snprintf(label, sizeof(label), "endpoint %.60s: %.120s", name, sql);
    run_script(s, rs, script, label, db, true, objects, NULL, NULL);   // sempre atomico; con i permessi di chi l'ha salvato
}

static void handle_endpoint_list(SvServer* s, Request* rq, Response* rs, const char* db) {
    mx_lock(&s->exec_mx);
    char prev[128] = "";
    const char* cur = db_current_database();
    if (cur) snprintf(prev, sizeof(prev), "%s", cur);
    char q[200];
    snprintf(q, sizeof(q), "USE %.63s", db);
    QueryResult* r = db_execute(q);
    if (!r || !r->success) { db_result_free(r); r = NULL; json_error(rs, 404, "database '%s' not found", db); }
    else {
        db_result_free(r);
        r = db_execute("SELECT `name`, `access`, `sql`, `description` FROM " ENDPOINTS_TABLE " ORDER BY `name`");
        JsonBuf* b = &rs->body;
        rs->status = 200;
        jb_lit(b, "{\"ok\":true,\"database\":");
        jb_str(b, db);
        jb_lit(b, ",\"endpoints\":[");
        for (int i = 0; r && r->success && i < r->num_rows; i++) {
            const Cell* c = r->rows[i].cells;
            if (i) jb_lit(b, ",");
            jb_lit(b, "{\"name\":"); jb_str(b, c[0].value);
            jb_lit(b, ",\"access\":"); jb_str(b, c[1].is_null ? "token" : c[1].value);
            jb_lit(b, ",\"sql\":"); jb_str(b, c[2].is_null ? "" : c[2].value);
            jb_lit(b, ",\"description\":"); if (c[3].is_null) jb_lit(b, "null"); else jb_str(b, c[3].value);
            jb_lit(b, "}");
        }
        jb_lit(b, "]}");
    }
    db_result_free(r);
    cur = db_current_database();
    if (prev[0] && (!cur || strcmp(cur, prev))) { snprintf(q, sizeof(q), "USE %.63s", prev); db_result_free(db_execute(q)); }
    mx_unlock(&s->exec_mx);
    (void)rq;
}

// ── catalogo: /api/v1/databases[/db/tables[/t]] ─────────────────

// con gli utenti (--users) si vede solo ciò su cui si ha qualche permesso; il resto è "not found"
static void handle_catalog(SvServer* s, const char* user, const char* rest, Response* rs) {
    char parts[3][128] = { "", "", "" };
    int np = 0;
    while (*rest == '/') rest++;
    while (*rest && np < 4) {
        const char* sl = strchr(rest, '/');
        size_t l = sl ? (size_t)(sl - rest) : strlen(rest);
        if (np == 3 || l >= sizeof(parts[0])) { json_error(rs, 404, "not found"); return; }
        memcpy(parts[np], rest, l);
        parts[np][l] = '\0';
        np++;
        rest += l;
        while (*rest == '/') rest++;
    }
    JsonBuf* b = &rs->body;
    rs->status = 200;
    mx_lock(&s->exec_mx);
    if (np == 0) {
        NameList* nl = db_list_databases();
        jb_lit(b, "{\"ok\":true,\"databases\":[");
        for (int i = 0, n = 0; nl && i < nl->count; i++) {
            if (!db_user_can_see(user, nl->names[i], NULL)) continue;
            if (n++) jb_lit(b, ",");
            jb_str(b, nl->names[i]);
        }
        jb_lit(b, "]}");
        db_namelist_free(nl);
    } else if (np == 2 && !strcmp(parts[1], "tables")) {
        bool vis = db_user_can_see(user, parts[0], NULL);
        NameList* tl = vis ? db_list_tables(parts[0]) : NULL;
        NameList* vl = vis ? db_list_views(parts[0]) : NULL;
        if (!tl) json_error(rs, 404, "database '%s' not found", parts[0]);
        else {
            jb_lit(b, "{\"ok\":true,\"database\":");
            jb_str(b, parts[0]);
            jb_lit(b, ",\"tables\":[");
            for (int i = 0, n = 0; i < tl->count; i++) {
                if (!db_user_can_see(user, parts[0], tl->names[i])) continue;
                if (n++) jb_lit(b, ",");
                jb_str(b, tl->names[i]);
            }
            jb_lit(b, "],\"views\":[");
            for (int i = 0, n = 0; vl && i < vl->count; i++) {
                if (!db_user_can_see(user, parts[0], vl->names[i])) continue;
                if (n++) jb_lit(b, ",");
                jb_str(b, vl->names[i]);
            }
            jb_lit(b, "]}");
        }
        db_namelist_free(tl);
        db_namelist_free(vl);
    } else if (np == 3 && !strcmp(parts[1], "tables")) {
        int nc = 0;
        ColumnInfo* cols = db_user_can_see(user, parts[0], parts[2]) ? db_list_columns(parts[0], parts[2], &nc) : NULL;
        if (!cols) json_error(rs, 404, "table '%s.%s' not found", parts[0], parts[2]);
        else {
            jb_lit(b, "{\"ok\":true,\"database\":");
            jb_str(b, parts[0]);
            jb_lit(b, ",\"table\":");
            jb_str(b, parts[2]);
            jb_lit(b, ",\"columns\":[");
            for (int i = 0; i < nc; i++) {
                if (i) jb_lit(b, ",");
                jb_lit(b, "{\"name\":");      jb_str(b, cols[i].name);
                jb_lit(b, ",\"type\":");      jb_str(b, cols[i].type);
                jb_printf(b, ",\"nullable\":%s,\"primary_key\":%s,\"auto_increment\":%s",
                          cols[i].nullable ? "true" : "false", cols[i].primary_key ? "true" : "false",
                          cols[i].auto_increment ? "true" : "false");
                jb_lit(b, ",\"default\":");
                if (cols[i].default_val[0]) jb_str(b, cols[i].default_val); else jb_lit(b, "null");
                jb_lit(b, "}");
            }
            jb_lit(b, "]");
            char* create = db_get_create_statement(parts[0], parts[2]);
            if (create) { jb_lit(b, ",\"create\":"); jb_str(b, create); db_free_string(create); }
            jb_lit(b, "}");
            db_columns_free(cols, nc);
        }
    } else json_error(rs, 404, "not found");
    mx_unlock(&s->exec_mx);
}

// ── file statici (una web app servita dallo stesso server: niente CORS) ──

static const char* mime_of(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const char* const map[][2] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" }, { ".mjs", "text/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" }, { ".json", "application/json" }, { ".map", "application/json" },
        { ".webmanifest", "application/manifest+json" }, { ".xml", "application/xml" }, { ".rss", "application/rss+xml" },
        { ".atom", "application/atom+xml" }, { ".csv", "text/csv; charset=utf-8" }, { ".md", "text/markdown; charset=utf-8" },
        { ".txt", "text/plain; charset=utf-8" }, { ".sql", "text/plain; charset=utf-8" }, { ".ics", "text/calendar" },
        { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" }, { ".ico", "image/x-icon" }, { ".webp", "image/webp" }, { ".avif", "image/avif" },
        { ".bmp", "image/bmp" }, { ".tif", "image/tiff" }, { ".tiff", "image/tiff" },
        { ".woff2", "font/woff2" }, { ".woff", "font/woff" }, { ".ttf", "font/ttf" }, { ".otf", "font/otf" },
        { ".mp4", "video/mp4" }, { ".m4v", "video/mp4" }, { ".webm", "video/webm" }, { ".ogv", "video/ogg" }, { ".mov", "video/quicktime" },
        { ".mp3", "audio/mpeg" }, { ".m4a", "audio/mp4" }, { ".ogg", "audio/ogg" }, { ".oga", "audio/ogg" }, { ".wav", "audio/wav" },
        { ".flac", "audio/flac" }, { ".opus", "audio/opus" }, { ".vtt", "text/vtt; charset=utf-8" },
        { ".pdf", "application/pdf" }, { ".zip", "application/zip" }, { ".gz", "application/gzip" }, { ".tar", "application/x-tar" },
        { ".7z", "application/x-7z-compressed" }, { ".wasm", "application/wasm" },
        { ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { ".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcasecmp(dot, map[i][0])) return map[i][1];
    return "application/octet-stream";
}

static bool path_is(const char* path, int kind) {     // kind: S_IFREG o S_IFDIR
    struct stat st;
    return stat(path, &st) == 0 && (int)(st.st_mode & S_IFMT) == kind;
}

// dimensione e ora di modifica (anche file oltre 2 GB su Windows)
static bool file_info(const char* path, long long* size, long long* mtime) {
#ifdef _WIN32
    struct __stat64 st;
    if (_stat64(path, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) return false;
#else
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) return false;
#endif
    *size = (long long)st.st_size;
    *mtime = (long long)st.st_mtime;
    return true;
}

// data HTTP (RFC 9110): "Sun, 06 Nov 1994 08:49:37 GMT", indipendente dalla lingua del sistema
static void http_date(long long t, char* out, size_t n) {
    static const char* const D[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* const M[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    snprintf(out, n, "%s, %02d %s %04d %02d:%02d:%02d GMT", D[g.tm_wday % 7], g.tm_mday, M[g.tm_mon % 12],
             g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
}

// "bytes=a-b" | "bytes=a-" | "bytes=-n" (un solo intervallo). 1 = ok, 0 = da ignorare (file intero), -1 = 416
static int parse_range(const char* r, long long size, long long* from, long long* len) {
    if (strncmp(r, "bytes=", 6) || strchr(r, ',')) return 0;    // più intervalli: si manda tutto (consentito)
    const char* p = r + 6;
    char* e;
    if (*p == '-') {
        long long n = strtoll(p + 1, &e, 10);
        if (e == p + 1 || *e || n <= 0) return n == 0 && e != p + 1 && !*e ? -1 : 0;
        if (n > size) n = size;
        if (size == 0) return -1;
        *from = size - n; *len = n;
        return 1;
    }
    long long a = strtoll(p, &e, 10);
    if (e == p || *e != '-' || a < 0) return 0;
    const char* q = e + 1;
    long long b = size - 1;
    if (*q) { b = strtoll(q, &e, 10); if (e == q || *e || b < a) return 0; }
    if (a >= size) return -1;
    if (b >= size) b = size - 1;
    *from = a; *len = b - a + 1;
    return 1;
}

// una delle etichette di If-None-Match ("*", "x", W/"x", separate da virgole)
static bool etag_matches(const char* list, const char* etag) {
    if (!list[0]) return false;
    if (!strcmp(list, "*")) return true;
    const char* bare = etag + (etag[0] == 'W' ? 2 : 0);
    for (const char* p = list; (p = strstr(p, bare)); p += strlen(bare)) {
        char after = p[strlen(bare)];
        if (after == '\0' || after == ',' || after == ' ') return true;
    }
    return false;
}

static void set_own_headers(Response* rs, const char* text) {
    free(rs->own_headers);
    rs->own_headers = strdup(text);
    rs->extra_headers = rs->own_headers;
}

// 404 del sito: la pagina 404.html della cartella, se c'è
static void web_not_found(const Request* rq, Response* rs) {
    char p[4300];
    snprintf(p, sizeof(p), "%s/404.html", rq->root ? rq->root : ".");
    FILE* f = fopen(p, "rb");
    if (!f) { json_error(rs, 404, "not found"); return; }
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0 && rs->body.len < (1u << 20)) jb_raw(&rs->body, chunk, got);
    fclose(f);
    rs->status = 404;
    rs->content_type = "text/html; charset=utf-8";
}

// file statico: 304 se il browser l'ha già, 206 per un intervallo (video, download ripresi), altrimenti
// tutto; i testi piccoli vengono compressi da send_response, gli altri inviati a pezzi
static void serve_file(SvServer* s, const Request* rq, Response* rs, const char* full) {
    long long size, mtime;
    if (!file_info(full, &size, &mtime)) { web_not_found(rq, rs); return; }
    const char* ctype = mime_of(full);
    bool range = rq->range[0] && !strcmp(rq->method, "GET");
    bool gz = rq->accept_gzip && !range && compressible(ctype) && size >= GZIP_MIN && size <= (long long)GZIP_MAX;
    char etag[64], lm[40], hdr[400];
    snprintf(etag, sizeof(etag), "\"%llx-%llx%s\"", (unsigned long long)size, (unsigned long long)mtime, gz ? "-gz" : "");
    http_date(mtime, lm, sizeof(lm));
    int hl = snprintf(hdr, sizeof(hdr), "ETag: %s\r\nLast-Modified: %s\r\nCache-Control: no-cache\r\nAccept-Ranges: bytes\r\n", etag, lm);

    // già nella cache del browser (no-cache: chiede ogni volta, ma riceve il file solo se è cambiato)
    if (etag_matches(rq->if_none_match, etag) || (!rq->if_none_match[0] && rq->if_modified_since[0] && !strcmp(rq->if_modified_since, lm))) {
        rs->status = 304;
        rs->content_type = ctype;
        set_own_headers(rs, hdr);
        return;
    }
    long long from = 0, len = size;
    int r = range ? parse_range(rq->range, size, &from, &len) : 0;
    if (r < 0) {
        snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "Content-Range: bytes */%lld\r\n", size);
        set_own_headers(rs, hdr);
        rs->status = 416;
        rs->content_type = "text/plain; charset=utf-8";
        jb_lit(&rs->body, "range not satisfiable\n");
        return;
    }
    FILE* f = fopen(full, "rb");
    if (!f) { web_not_found(rq, rs); return; }
    if (r > 0) snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "Content-Range: bytes %lld-%lld/%lld\r\n", from, from + len - 1, size);
    set_own_headers(rs, hdr);
    rs->status = r > 0 ? 206 : 200;
    rs->content_type = ctype;
    if (gz) {                                              // in memoria: send_response lo comprime
        char chunk[65536];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) jb_raw(&rs->body, chunk, got);
        fclose(f);
        if (rs->body.oom) json_error(rs, 500, "out of memory");
        return;
    }
    rs->file = f;
    rs->file_off = from;
    rs->file_len = len;
}

// ── PHP (FastCGI) ───────────────────────────────────────────────

#define MAX_CGI_PARAMS 160

// risposta CGI di PHP (intestazioni, riga vuota, corpo) -> Response
static void cgi_response(Response* rs, JsonBuf* out) {
    size_t hl = 0, skip = 0;
    for (size_t i = 0; i < out->len; i++) {
        if (out->data[i] != '\n') continue;
        if (i + 1 < out->len && out->data[i + 1] == '\n') { hl = i + 1; skip = 1; break; }
        if (i + 2 < out->len && out->data[i + 1] == '\r' && out->data[i + 2] == '\n') { hl = i + 1; skip = 2; break; }
    }
    if (!hl) {
        free(out->data);
        json_error(rs, 502, "PHP sent no headers");
        return;
    }
    JsonBuf hb = {0};
    int status = 0;
    bool location = false;
    rs->ctype[0] = '\0';
    char* p = out->data;
    char* end = out->data + hl;
    while (p < end) {
        char* e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        size_t l = (size_t)(e - p);
        if (l > 0 && p[l - 1] == '\r') l--;
        char* colon = memchr(p, ':', l);
        if (colon && colon > p) {
            size_t nl = (size_t)(colon - p);
            char* v = colon + 1;
            while (v < p + l && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)(p + l - v);
            #define IS(name) (nl == sizeof(name) - 1 && !strncasecmp(p, name, nl))
            if (IS("Status")) status = atoi(v);
            else if (IS("Content-Type")) snprintf(rs->ctype, sizeof(rs->ctype), "%.*s", (int)vl, v);
            else if (IS("Content-Length") || IS("Connection") || IS("Transfer-Encoding") || IS("Keep-Alive")) {}
            else {
                if (IS("Location")) location = true;
                jb_raw(&hb, p, nl);
                jb_lit(&hb, ": ");
                jb_raw(&hb, v, vl);
                jb_lit(&hb, "\r\n");
            }
            #undef IS
        }
        p = e + 1;
    }
    if (status < 100 || status > 999) status = location ? 302 : 200;
    size_t body_at = hl + skip;
    memmove(out->data, out->data + body_at, out->len - body_at);
    out->len -= body_at;
    free(rs->body.data);
    rs->body = *out;
    rs->status = status;
    rs->content_type = rs->ctype[0] ? rs->ctype : "text/html; charset=UTF-8";
    if (hb.oom) { free(hb.data); hb.data = NULL; }
    rs->own_headers = hb.data;                 // NUL-terminato da jb_raw
    rs->extra_headers = rs->own_headers;
}

// script: percorso nel sito ("/blog/index.php"); info: PATH_INFO ("" se manca)
static void run_php(SvServer* s, Request* rq, Response* rs, const char* script, const char* info) {
    char filename[4096], translated[4096] = "", port[16], rport[16], clen[24], server_name[256];
    snprintf(filename, sizeof(filename), "%s%s", rq->root_abs, script);
    if (info[0]) snprintf(translated, sizeof(translated), "%s%s", rq->root_abs, info);
    snprintf(port, sizeof(port), "%d", s->port);
    snprintf(rport, sizeof(rport), "%d", rq->client_port);
    snprintf(clen, sizeof(clen), "%zu", rq->body_len);
    snprintf(server_name, sizeof(server_name), "%s", rq->host[0] ? rq->host : s->cfg.bind_addr);
    char* colon = server_name[0] == '[' ? strchr(server_name, ']') : server_name;   // senza la porta
    if (colon && (colon = strchr(colon, ':'))) *colon = '\0';

    FcgiParam pr[MAX_CGI_PARAMS];
    int n = 0;
    #define P(k, v) do { if (n < MAX_CGI_PARAMS) { pr[n].name = (k); pr[n].value = (v); n++; } } while (0)
    P("GATEWAY_INTERFACE", "CGI/1.1");
    P("SERVER_SOFTWARE", "SegVault/" SV_VERSION_STR);
    P("SERVER_PROTOCOL", rq->http10 ? "HTTP/1.0" : "HTTP/1.1");
    P("SERVER_NAME", server_name);
    P("SERVER_ADDR", s->cfg.bind_addr);
    P("SERVER_PORT", port);
    P("REQUEST_METHOD", rq->method);
    P("REQUEST_URI", rq->uri);
    P("QUERY_STRING", rq->query);
    P("DOCUMENT_ROOT", rq->root_abs);
    P("SCRIPT_NAME", script);
    P("SCRIPT_FILENAME", filename);
    if (info[0]) { P("PATH_INFO", info); P("PATH_TRANSLATED", translated); }
    P("REMOTE_ADDR", rq->client ? rq->client : "");
    P("REMOTE_PORT", rport);
    P("REDIRECT_STATUS", "200");                 // php-cgi con cgi.force_redirect lo richiede
    if (rq->has_length || rq->body_len) P("CONTENT_LENGTH", clen);
    if (rq->content_type[0]) P("CONTENT_TYPE", rq->content_type);

    // intestazioni della richiesta -> HTTP_NOME (in un solo blocco liberato alla fine)
    size_t hlen = rq->headers ? strlen(rq->headers) : 0;
    char* names = hlen ? malloc(hlen * 2 + 16) : NULL;
    size_t used = 0;
    for (char* line = names ? rq->headers : NULL; line && *line; ) {
        char* e = strstr(line, "\r\n");
        size_t l = e ? (size_t)(e - line) : strlen(line);
        char* c = memchr(line, ':', l);
        if (c && c > line) {
            size_t nl = (size_t)(c - line);
            // Content-Type / Length hanno già le loro variabili; Proxy aprirebbe la falla "httpoxy"
            bool skip = (nl == 12 && !strncasecmp(line, "Content-Type", 12)) || (nl == 14 && !strncasecmp(line, "Content-Length", 14)) ||
                        (nl == 5 && !strncasecmp(line, "Proxy", 5)) ||
                        (nl == 17 && !strncasecmp(line, "Transfer-Encoding", 17));   // il corpo arriva già decodificato
            if (!skip && used + nl + 6 + l + 2 < hlen * 2 + 16) {
                char* nm = names + used;
                memcpy(nm, "HTTP_", 5);
                for (size_t i = 0; i < nl; i++) {
                    char ch = line[i];
                    nm[5 + i] = ch == '-' ? '_' : (char)toupper((unsigned char)ch);
                }
                nm[5 + nl] = '\0';
                used += 5 + nl + 1;
                const char* v = c + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vl = (size_t)(line + l - v);
                while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                char* vv = names + used;
                memcpy(vv, v, vl);
                vv[vl] = '\0';
                used += vl + 1;
                bool dup = false;
                for (int i = 0; i < n; i++) if (!strcmp(pr[i].name, nm)) dup = true;   // primo valore vince
                if (!dup) P(nm, vv);
            }
        }
        line = e ? e + 2 : NULL;
    }
    #undef P

    JsonBuf out = {0}, errout = {0};
    char err[256];
    int r = fcgi_request(s->php, pr, n, rq->body, rq->body_len, s->cfg.timeout_ms, 0, &out, &errout, err, sizeof(err));
    free(names);
    if (errout.len && !errout.oom) {
        jb_raw(&errout, "", 1);
        if (!s->cfg.quiet) fprintf(stderr, "[php] %s: %s%s", script, errout.data, errout.data[errout.len - 2] == '\n' ? "" : "\n");
    }
    if (r) {
        free(out.data);
        json_error(rs, r, "PHP: %s", err);
        free(errout.data);
        return;
    }
    cgi_response(rs, &out);
    if (errout.len && !errout.oom) one_line(rs->detail, sizeof(rs->detail), errout.data);
    else snprintf(rs->detail, sizeof(rs->detail), "php %s", script);
    free(errout.data);
}


// ── siti (virtual host) ─────────────────────────────────────────

// percorso assoluto con "/" come separatore (false se non esiste)
static bool abs_path(const char* in, char* out, size_t n) {
#ifdef _WIN32
    char tmp[4096];
    if (!_fullpath(tmp, in, sizeof(tmp))) return false;
    for (char* q = tmp; *q; q++) if (*q == '\\') *q = '/';
    if (!path_is(tmp, S_IFDIR)) return false;
#else
    char* tmp = realpath(in, NULL);
    if (!tmp) return false;
#endif
    size_t l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = '\0';
    bool ok = l < n;
    if (ok) memcpy(out, tmp, l + 1);
#ifndef _WIN32
    free(tmp);
#endif
    return ok;
}

// nome di una cartella usabile come sito: lettere, cifre, '-' e '_' (niente punti: niente "..")
static bool site_label(const char* s, size_t n) {
    if (n == 0 || n > 63) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return false;
    return true;
}

static bool has_index(const char* dir) {
    char p[4200];
    snprintf(p, sizeof(p), "%s/index.php", dir);
    if (path_is(p, S_IFREG)) return true;
    snprintf(p, sizeof(p), "%s/index.html", dir);
    return path_is(p, S_IFREG);
}

// radice di un sito automatico: la cartella, o la sua public/ se lei non ha un index (Laravel, Symfony)
static void auto_root(const char* dir, char* out, size_t n) {
    snprintf(out, n, "%s", dir);
    if (has_index(dir)) return;
    char pub[4200];
    snprintf(pub, sizeof(pub), "%s/public", dir);
    if (path_is(pub, S_IFDIR)) snprintf(out, n, "%s", pub);
}

static const Site* find_site(SvServer* s, const char* name, size_t n) {
    for (int i = 0; i < s->n_sites; i++)
        if (strlen(s->sites[i].name) == n && !strncmp(s->sites[i].name, name, n)) return &s->sites[i];
    return NULL;
}

static bool use_site(SvServer* s, Request* rq, const char* name, size_t n) {
    const Site* st = find_site(s, name, n);
    char dir[4200];
    if (st && !st->dir) {
        rq->proxy = st;
    } else if (st) {
        snprintf(rq->root_buf, sizeof(rq->root_buf), "%s", st->dir);
    } else if (s->static_dir && !s->cfg.no_auto_sites && site_label(name, n)) {
        snprintf(dir, sizeof(dir), "%s/%.*s", s->static_dir, (int)n, name);
        if (!path_is(dir, S_IFDIR)) return false;
        auto_root(dir, rq->root_buf, sizeof(rq->root_buf));
    } else return false;
    snprintf(rq->site, sizeof(rq->site), "%.*s", (int)n, name);
    if (!rq->proxy) {
        rq->root = rq->root_buf;
        if (!abs_path(rq->root, rq->root_abs, sizeof(rq->root_abs))) snprintf(rq->root_abs, sizeof(rq->root_abs), "%s", rq->root);
    }
    return true;
}

// sito della richiesta dall'Host: "blog.localhost" / "blog.test" / "api.blog.localhost" -> sito "blog",
// un nome completo configurato ("shop.example.com") -> quel sito; altrimenti la cartella del server
static void resolve_site(SvServer* s, Request* rq) {
    rq->root = s->static_dir;
    rq->proxy = NULL;
    rq->site[0] = '\0';
    if (s->doc_root) snprintf(rq->root_abs, sizeof(rq->root_abs), "%s", s->doc_root);
    else if (!s->static_dir || !abs_path(s->static_dir, rq->root_abs, sizeof(rq->root_abs)))
        snprintf(rq->root_abs, sizeof(rq->root_abs), "%s", s->static_dir ? s->static_dir : "");
    char h[256];
    snprintf(h, sizeof(h), "%s", rq->host);
    if (h[0] == '[') return;                                 // indirizzo IPv6
    char* colon = strchr(h, ':');
    if (colon) *colon = '\0';
    size_t l = strlen(h);
    while (l > 0 && h[l - 1] == '.') h[--l] = '\0';          // "blog.localhost."
    for (size_t i = 0; i < l; i++) h[i] = (char)tolower((unsigned char)h[i]);
    if (!l) return;
    if (find_site(s, h, l)) { use_site(s, rq, h, l); return; }
    static const char* const SUF[] = { ".localhost", ".test" };
    for (int k = 0; k < 2; k++) {
        size_t sl = strlen(SUF[k]);
        if (l <= sl || strcmp(h + l - sl, SUF[k])) continue;
        size_t bl = l - sl;
        if (use_site(s, rq, h, bl)) return;                  // "api.blog" (un sito con quel nome)
        const char* dot = NULL;                              // sottodominio: si guarda l'ultima etichetta
        for (size_t i = 0; i < bl; i++) if (h[i] == '.') dot = h + i;
        if (dot) use_site(s, rq, dot + 1, (size_t)(h + bl - dot - 1));
        return;
    }
}

static bool client_is_local(const Request* rq) {
    return rq->client && (!strncmp(rq->client, "127.", 4) || !strcmp(rq->client, "::1"));
}

static void html_esc(JsonBuf* b, const char* t) {
    for (; *t; t++) {
        switch (*t) {
        case '<': jb_lit(b, "&lt;"); break;
        case '>': jb_lit(b, "&gt;"); break;
        case '&': jb_lit(b, "&amp;"); break;
        case '"': jb_lit(b, "&quot;"); break;
        case '\'': jb_lit(b, "&#39;"); break;
        default: jb_raw(b, t, 1);
        }
    }
}

// percorso in un URL (%xx per gli spazi e i caratteri speciali, "/" resta)
static void url_esc(JsonBuf* b, const char* t) {
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (isalnum(c) || strchr("/-_.~", c)) jb_raw(b, t, 1);
        else jb_printf(b, "%%%02X", c);
    }
}

static const char* const PAGE_CSS =
    ":root{--bg:#f6f7f9;--fg:#1d2330;--mut:#667085;--card:#fff;--line:#e4e7ec;--acc:#2563eb;--ok:#16a34a;--off:#9aa1ad}"
    "@media(prefers-color-scheme:dark){:root{--bg:#111318;--fg:#e6e8ec;--mut:#98a2b3;--card:#1a1d24;--line:#2a2f3a;--acc:#60a5fa;--ok:#4ade80;--off:#6b7280}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "main{max-width:1000px;margin:0 auto;padding:28px 16px}h1{font-size:22px;margin:0 0 4px}h2{font-size:15px;margin:28px 0 10px;color:var(--mut);"
    "text-transform:uppercase;letter-spacing:.06em}a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}"
    ".sub{color:var(--mut);margin:0 0 20px}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:12px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}.card b{display:block;font-size:13px;color:var(--mut);font-weight:500}"
    ".card span{font-size:16px;word-break:break-all}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--off)}"
    ".dot.on{background:var(--ok)}table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}"
    "th,td{text-align:left;padding:9px 14px;border-bottom:1px solid var(--line);vertical-align:top}th{font-size:13px;color:var(--mut);font-weight:500}"
    "tr:last-child td{border-bottom:0}td.n{text-align:right;white-space:nowrap;color:var(--mut)}.tag{font-size:12px;border:1px solid var(--line);border-radius:99px;padding:1px 8px;color:var(--mut);white-space:nowrap}"
    ".path{color:var(--mut);font-size:13px;word-break:break-all}.wrap{overflow-x:auto}code{font:13px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}";

static void page_start(JsonBuf* b, const char* title) {
    jb_lit(b, "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    html_esc(b, title);
    jb_lit(b, "</title><style>");
    jb_raw(b, PAGE_CSS, strlen(PAGE_CSS));
    jb_lit(b, "</style></head><body><main>");
}

// che cosa c'è in una cartella di progetto (per il cruscotto)
static const char* project_kind(const char* dir) {
    static const struct { const char* file; const char* kind; } K[] = {
        { "artisan", "Laravel" }, { "wp-load.php", "WordPress" }, { "wp-config.php", "WordPress" }, { "bin/console", "Symfony" },
        { "craft", "Craft CMS" }, { "spark", "CodeIgniter" }, { "core/lib/Drupal.php", "Drupal" }, { "web/core/lib/Drupal.php", "Drupal" },
        { "configuration.php", "Joomla" }, { "composer.json", "PHP" }, { "index.php", "PHP" }, { "public/index.php", "PHP" },
        { "package.json", "Node" }, { "index.html", "Static" },
    };
    char p[4300];
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, K[i].file);
        if (path_is(p, S_IFREG)) return K[i].kind;
    }
    return "Folder";
}

typedef struct { char name[256]; bool dir; long long size, mtime; } DirEnt;

static int dirent_cmp(const void* a, const void* b) {
    const DirEnt* x = a; const DirEnt* y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

// voci di una cartella senza i file nascosti, cartelle prima (NULL se non si apre)
static DirEnt* list_dir(const char* dir, int* count) {
    *count = 0;
    DIR* d = opendir(dir);
    if (!d) return NULL;
    int cap = 64, n = 0;
    DirEnt* v = malloc(sizeof(DirEnt) * (size_t)cap);
    struct dirent* e;
    while (v && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(v[0].name)) continue;
        if (n == cap) {
            if (cap >= 100000) break;
            DirEnt* nv = realloc(v, sizeof(DirEnt) * (size_t)cap * 2);
            if (!nv) break;
            v = nv; cap *= 2;
        }
        char p[4400];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        DirEnt* de = &v[n];
        snprintf(de->name, sizeof(de->name), "%s", e->d_name);
        de->dir = path_is(p, S_IFDIR);
        de->size = de->mtime = 0;
        if (!de->dir && !file_info(p, &de->size, &de->mtime)) continue;
        n++;
    }
    closedir(d);
    if (v) qsort(v, (size_t)n, sizeof(DirEnt), dirent_cmp);
    *count = n;
    return v;
}

static void human_size(long long n, char* out, size_t sz) {
    if (n < 1024) snprintf(out, sz, "%lld B", n);
    else if (n < 1024 * 1024) snprintf(out, sz, "%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024) snprintf(out, sz, "%.1f MB", n / 1048576.0);
    else snprintf(out, sz, "%.1f GB", n / 1073741824.0);
}

// elenco dei file di una cartella senza index (--dir-listing)
static void dir_listing(const Request* rq, Response* rs, const char* full) {
    int n;
    DirEnt* v = list_dir(full, &n);
    if (!v) { web_not_found(rq, rs); return; }
    JsonBuf* b = &rs->body;
    char title[2200];
    snprintf(title, sizeof(title), "Index of %s", rq->path);
    page_start(b, title);
    jb_lit(b, "<h1>Index of ");
    html_esc(b, rq->path);
    jb_lit(b, "</h1><p class=\"sub\">");
    html_esc(b, rq->site[0] ? rq->site : "localhost");
    jb_lit(b, "</p><div class=\"wrap\"><table><tr><th>Name</th><th>Size</th><th>Modified</th></tr>");
    if (strcmp(rq->path, "/")) jb_lit(b, "<tr><td><a href=\"../\">../</a></td><td></td><td></td></tr>");
    for (int i = 0; i < n; i++) {
        jb_lit(b, "<tr><td><a href=\"");
        url_esc(b, v[i].name);
        if (v[i].dir) jb_lit(b, "/");
        jb_lit(b, "\">");
        html_esc(b, v[i].name);
        if (v[i].dir) jb_lit(b, "/");
        jb_lit(b, "</a></td><td class=\"n\">");
        if (!v[i].dir) {
            char hs[32], when[32];
            human_size(v[i].size, hs, sizeof(hs));
            time_t t = (time_t)v[i].mtime;
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
            jb_printf(b, "%s</td><td class=\"n\">%s", hs, when);
        } else jb_lit(b, "</td><td class=\"n\">");
        jb_lit(b, "</td></tr>");
    }
    jb_lit(b, "</table></div></main></body></html>");
    free(v);
    rs->status = 200;
    rs->content_type = "text/html; charset=utf-8";
    rs->extra_headers = "Cache-Control: no-store\r\n";
}

static void dash_site_row(SvServer* s, JsonBuf* b, const char* name, const char* kind, const char* where, bool link) {
    jb_lit(b, "<tr><td>");
    if (link) {
        jb_lit(b, "<a href=\"http://");
        html_esc(b, name);
        jb_printf(b, ".localhost:%d/\">", s->port);
        html_esc(b, name);
        jb_printf(b, ".localhost:%d</a>", s->port);
    } else html_esc(b, name);
    jb_lit(b, "</td><td><span class=\"tag\">");
    html_esc(b, kind);
    jb_lit(b, "</span></td><td class=\"path\">");
    html_esc(b, where);
    jb_lit(b, "</td></tr>");
}

// cruscotto di SegVault (http://localhost:PORTA/ senza index, o /.segvault/): servizi e siti
static void dashboard(SvServer* s, Response* rs) {
    JsonBuf* b = &rs->body;
    page_start(b, "SegVault");
    jb_lit(b, "<h1>SegVault</h1><p class=\"sub\">Local development server &middot; version " SV_VERSION_STR "</p><h2>Services</h2><div class=\"grid\">");
    jb_printf(b, "<div class=\"card\"><b><i class=\"dot on\"></i>Web</b><span>http://localhost:%d</span></div>", s->port);
    jb_lit(b, "<div class=\"card\"><b>");
    jb_printf(b, "<i class=\"dot%s\"></i>PHP</b><span>", s->php ? " on" : "");
    html_esc(b, s->php ? fcgi_pool_describe(s->php) : "off (start with --php auto)");
    jb_lit(b, "</span></div>");
    if (s->mysql_fd != BAD_SOCK)
        jb_printf(b, "<div class=\"card\"><b><i class=\"dot on\"></i>MySQL protocol</b><span>127.0.0.1:%d</span></div>", s->mysql_port);
    else
        jb_lit(b, "<div class=\"card\"><b><i class=\"dot\"></i>MySQL protocol</b><span>off (--mysql-port 3306)</span></div>");
    if (s->api[0]) {
        jb_lit(b, "<div class=\"card\"><b><i class=\"dot on\"></i>HTTP API</b><span>");
        html_esc(b, s->api);
        jb_lit(b, "</span></div>");
    }
    jb_lit(b, "</div><h2>Sites</h2>");
    int shown = 0;
    JsonBuf rows = {0};
    for (int i = 0; i < s->n_sites; i++) {
        const Site* st = &s->sites[i];
        char where[600];
        if (st->dir) snprintf(where, sizeof(where), "%s", st->dir);
        else snprintf(where, sizeof(where), "proxy \xe2\x86\x92 http://%s:%d", st->phost, st->pport);
        bool full = strchr(st->name, '.') != NULL;
        dash_site_row(s, &rows, st->name, st->dir ? project_kind(st->dir) : "Proxy", where, !full);
        shown++;
    }
    if (s->static_dir && !s->cfg.no_auto_sites) {
        int n;
        DirEnt* v = list_dir(s->static_dir, &n);
        for (int i = 0; v && i < n; i++) {
            if (!v[i].dir || !site_label(v[i].name, strlen(v[i].name)) || find_site(s, v[i].name, strlen(v[i].name))) continue;
            char dir[4400], root[4400];
            snprintf(dir, sizeof(dir), "%s/%s", s->static_dir, v[i].name);
            auto_root(dir, root, sizeof(root));
            dash_site_row(s, &rows, v[i].name, project_kind(dir), root, true);
            shown++;
        }
        free(v);
    }
    if (shown) {
        jb_lit(b, "<div class=\"wrap\"><table><tr><th>URL</th><th>Type</th><th>Root</th></tr>");
        if (rows.data) jb_raw(b, rows.data, rows.len);
        jb_lit(b, "</table></div>");
    } else {
        jb_lit(b, "<p class=\"sub\">No sites yet. Every folder in the web folder");
        if (s->static_dir) { jb_lit(b, " (<code>"); html_esc(b, s->static_dir); jb_lit(b, "</code>)"); }
        jb_lit(b, " becomes <code>http://NAME.localhost</code>; a dev server becomes a site with <code>--site NAME=http://127.0.0.1:5173</code>.</p>");
    }
    free(rows.data);
    jb_lit(b, "</main></body></html>");
    rs->status = 200;
    rs->content_type = "text/html; charset=utf-8";
    rs->extra_headers = "Cache-Control: no-store\r\n";
}

// sito web (static_dir): file statici, script .php, cartelle con index.php / index.html,
// e il "front controller" (un percorso che non esiste va a /index.php: URL di WordPress e Laravel)
static void handle_web(SvServer* s, Request* rq, Response* rs) {
    const char* p = rq->path;
    // niente "..", backslash o percorsi nascosti: si resta dentro static_dir
    for (const char* q = p; *q; q++) {
        if (*q == '\\' || *q == ':') { web_not_found(rq, rs); return; }
        // file nascosti e ".." no; /.well-known/ sì (certificati ACME, security.txt, ...)
        if (*q == '/' && q[1] == '.' && !(q == p && !strncmp(q, "/.well-known/", 13))) { web_not_found(rq, rs); return; }
    }
    bool get = !strcmp(rq->method, "GET") || !strcmp(rq->method, "HEAD");
    char script[2048] = "", info[2048] = "", full[4096];

    // /a/b.php o /a/b.php/resto (PATH_INFO)
    for (const char* q = p; (q = ci_strstr(q, ".php")); q += 4)
        if (q[4] == '\0' || q[4] == '/') {
            snprintf(script, sizeof(script), "%.*s", (int)(q + 4 - p), p);
            snprintf(info, sizeof(info), "%s", q + 4);
            break;
        }
    if (!script[0]) {
        int n = snprintf(full, sizeof(full), "%s%s", rq->root, p);
        if (n < 0 || (size_t)n >= sizeof(full) - 16) { web_not_found(rq, rs); return; }
        if (path_is(full, S_IFDIR)) {
            size_t pl = strlen(p);
            if (pl == 0 || p[pl - 1] != '/') {             // /cartella -> /cartella/ (i link relativi funzionano)
                const char* qm = strchr(rq->uri, '?');
                size_t ul = qm ? (size_t)(qm - rq->uri) : strlen(rq->uri);
                rs->own_headers = malloc(ul + strlen(rq->uri) + 32);
                if (!rs->own_headers) { json_error(rs, 500, "out of memory"); return; }
                sprintf(rs->own_headers, "Location: %.*s/%s\r\n", (int)ul, rq->uri, qm ? qm : "");
                rs->extra_headers = rs->own_headers;
                rs->status = 301;
                rs->content_type = "text/plain; charset=utf-8";
                jb_lit(&rs->body, "moved\n");
                return;
            }
            char idx[4096 + 16];
            snprintf(idx, sizeof(idx), "%sindex.php", full);
            if (path_is(idx, S_IFREG)) snprintf(script, sizeof(script), "%sindex.php", p);
            else {
                snprintf(idx, sizeof(idx), "%sindex.html", full);
                if (!path_is(idx, S_IFREG)) {
                    if (!get) { web_not_found(rq, rs); return; }
                    if (!strcmp(p, "/") && !rq->site[0] && client_is_local(rq)) dashboard(s, rs);
                    else if (s->cfg.dir_listing) dir_listing(rq, rs, full);
                    else web_not_found(rq, rs);
                    return;
                }
                strcat(full, "index.html");
            }
        } else if (!path_is(full, S_IFREG)) {
            char idx[4096];
            snprintf(idx, sizeof(idx), "%s/index.php", rq->root);
            if (s->php && path_is(idx, S_IFREG)) snprintf(script, sizeof(script), "/index.php");
            else { web_not_found(rq, rs); return; }
        }
    }
    if (script[0]) {
        if (!s->php) {                    // il codice sorgente (con le password del database) non si mostra mai
            json_error(rs, 403, "PHP is not enabled on this server (start it with --php)");
            return;
        }
        snprintf(full, sizeof(full), "%s%s", rq->root, script);
        if (!path_is(full, S_IFREG)) { web_not_found(rq, rs); return; }
        run_php(s, rq, rs, script, info);
        return;
    }
    if (!get) {
        rs->extra_headers = "Allow: GET, HEAD\r\n";
        json_error(rs, 405, "use GET");
        return;
    }
    serve_file(s, rq, rs, full);
}

// ── instradamento ───────────────────────────────────────────────

// "localhost", "127.x.x.x" o "[::1]", con o senza porta
static bool is_loopback_host(const char* h) {
    char name[256];
    snprintf(name, sizeof(name), "%s", h);
    char* colon = name[0] == '[' ? strchr(name, ']') : strrchr(name, ':');
    if (colon && name[0] == '[') colon[1] = '\0';
    else if (colon) *colon = '\0';
    size_t nl = strlen(name);
    if (nl > 10 && !strcasecmp(name + nl - 10, ".localhost")) return true;    // RFC 6761: sempre questa macchina
    return !strcasecmp(name, "localhost") || !strncmp(name, "127.", 4) || !strcmp(name, "[::1]");
}

// Difesa dai siti aperti nel browser dell'utente:
// - un'altra pagina può inviare un POST text/plain senza preflight CORS: con un Origin non
//   ammesso la richiesta viene rifiutata prima di eseguire qualsiasi cosa;
// - DNS rebinding (un dominio che punta a 127.0.0.1): senza token l'Host deve essere locale.
static const char* origin_problem(SvServer* s, const Request* rq) {
    if (!s->token && rq->host[0] && !is_loopback_host(rq->host) && !strncmp(s->cfg.bind_addr, "127.", 4))
        return "Host not allowed (the server only answers to localhost)";
    if (!rq->origin[0]) return NULL;                       // non da un browser (curl, app, backend)
    if (s->cors && (!strcmp(s->cors, "*") || !strcmp(s->cors, rq->origin))) return NULL;
    // stessa origine: la pagina è servita da questo server (--static)
    const char* o = rq->origin;
    if (!strncmp(o, "http://", 7) && rq->host[0] && !strcasecmp(o + 7, rq->host)) return NULL;
    return "Origin not allowed (start the server with --cors to allow this web app)";
}

// ── login con utente e password (Authorization: Basic) ─────────

static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static size_t b64_decode(const char* in, char* out, size_t cap) {
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64v(*in);
        if (v < 0) { if (*in == ' ') continue; return 0; }
        acc = (acc << 6 | (unsigned)v) & 0xFFFFu;        // bastano gli ultimi 16 bit
        bits += 6;
        if (bits >= 8) { bits -= 8; if (n + 1 >= cap) return 0; out[n++] = (char)((acc >> bits) & 0xFF); }
    }
    out[n] = '\0';
    return n;
}

// le credenziali giuste si ricordano per un minuto (l'hash della password è volutamente lento);
// chiave = SHA-256 di "utente:password", mai la password. Una password cambiata smette di valere
// entro un minuto, un utente eliminato subito (la query controlla che esista)
#define LOGIN_CACHE 16
#define LOGIN_TTL_MS (60 * 1000)
static struct { uint8_t key[32]; char user[64]; double until; } g_login_cache[LOGIN_CACHE];

static bool basic_login(SvServer* s, const char* b64, char* user, size_t un) {
    char dec[400];
    if (!b64_decode(b64, dec, sizeof(dec))) return false;
    char* colon = strchr(dec, ':');
    if (!colon) return false;
    *colon = '\0';
    uint8_t key[32];
    *colon = ':';
    auth_sha256(dec, strlen(dec), key);
    *colon = '\0';
    double now = now_ms();
    mx_lock(&s->conn_mx);
    for (int i = 0; i < LOGIN_CACHE; i++)
        if (g_login_cache[i].until > now && !memcmp(g_login_cache[i].key, key, 32) && !strcmp(g_login_cache[i].user, dec)) {
            snprintf(user, un, "%s", dec);
            mx_unlock(&s->conn_mx);
            return true;
        }
    mx_unlock(&s->conn_mx);
    bool ok = db_login(dec, colon + 1);
    memset(colon + 1, 0, strlen(colon + 1));
    if (!ok) {
        sleep_ms(300);                           // rallenta i tentativi di indovinare la password
        return false;
    }
    snprintf(user, un, "%s", dec);
    mx_lock(&s->conn_mx);
    int slot = 0;
    for (int i = 1; i < LOGIN_CACHE; i++) if (g_login_cache[i].until < g_login_cache[slot].until) slot = i;
    memcpy(g_login_cache[slot].key, key, 32);
    snprintf(g_login_cache[slot].user, sizeof(g_login_cache[slot].user), "%s", dec);
    g_login_cache[slot].until = now + LOGIN_TTL_MS;
    mx_unlock(&s->conn_mx);
    return true;
}

// percorsi dell'API di SegVault (dopo il prefisso)
static bool api_route(const char* sub) {
    static const char* const R[] = { "/health", "/status", "/query", "/endpoints", "/sessions", "/databases" };
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        size_t l = strlen(R[i]);
        if (!strncmp(sub, R[i], l) && (sub[l] == '\0' || sub[l] == '/')) return true;
    }
    return false;
}

static void route(SvServer* s, Request* rq, Response* rs) {
    size_t apl = strlen(s->api);
    bool api = apl && !strncmp(rq->path, s->api, apl) && (rq->path[apl] == '\0' || rq->path[apl] == '/');
    const char* sub = api ? rq->path + apl : NULL;
    // con un sito PHP, /api/v1/utenti di Laravel è del sito: solo i percorsi dell'API restano a SegVault
    if (api && s->php && !api_route(sub)) { api = false; sub = NULL; }

    // endpoint salvato: va cercato prima dei controlli (uno pubblico non chiede token né origine)
    bool ep = api && !strncmp(sub, "/endpoints/", 11) && sub[11];
    bool ep_public = false;
    char* ep_sql = NULL;
    char ep_db[128] = "";
    if (api && !strncmp(sub, "/endpoints", 10) && (sub[10] == '\0' || sub[10] == '/')) {
        query_param(rq->query, "database", ep_db, sizeof(ep_db));
        if (!ep_db[0] && s->default_db) snprintf(ep_db, sizeof(ep_db), "%s", s->default_db);
        if (!ep_db[0] || !valid_ident(ep_db)) { json_error(rs, 400, "add ?database=name (or start the server with --database)"); return; }
    }
    if (ep) {
        const char* name = sub + 11;
        if (!valid_ident(name)) { json_error(rs, 404, "endpoint not found"); return; }
        char err[200];
        int f = endpoint_lookup(s, ep_db, name, &ep_sql, &ep_public, err, sizeof(err));
        if (f < 0) { json_error(rs, 404, "%s", err); return; }
        if (f == 0) { json_error(rs, 404, "endpoint '%s' not found in database '%s'", name, ep_db); return; }
    }

    const char* bad_origin = api && !ep_public ? origin_problem(s, rq) : NULL;
    if (bad_origin) { free(ep_sql); json_error(rs, 403, "%s", bad_origin); return; }

    if (!strcmp(rq->method, "OPTIONS") && (api || !s->php)) {   // preflight CORS (con PHP lo gestisce lo script)
        rs->status = 204;
        free(ep_sql);
        rs->extra_headers = ep_public ?
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type\r\n"
                            "Access-Control-Max-Age: 600\r\n" :
                            "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type, Authorization, X-SegVault-Session\r\n"
                            "Access-Control-Max-Age: 600\r\n";
        return;
    }
    if (api && !strcmp(sub, "/health")) {
        rs->status = 200;
        jb_lit(&rs->body, "{\"ok\":true,\"server\":\"SegVault\",\"version\":\"" SV_VERSION_STR "\"}");
        return;
    }
    if (api && !ep_public) {
        // token = accesso completo; utente e password (Basic) = i permessi di quell'utente
        const char* a = rq->auth;
        bool bearer = s->token && !strncasecmp(a, "Bearer ", 7) && secret_equal(a + 7, s->token);
        if (!strncasecmp(a, "Basic ", 6)) {
            if (!basic_login(s, a + 6, rq->user, sizeof(rq->user))) {
                rs->extra_headers = "WWW-Authenticate: Basic realm=\"SegVault\"\r\n";
                json_error(rs, 401, "wrong user or password");
                free(ep_sql);
                return;
            }
        } else if (!bearer && (s->token || s->cfg.require_login)) {
            rs->extra_headers = s->cfg.require_login ? "WWW-Authenticate: Basic realm=\"SegVault\"\r\n" : "WWW-Authenticate: Bearer\r\n";
            json_error(rs, 401, s->cfg.require_login ? "log in: send \"Authorization: Basic base64(user:password)\"%s"
                                                     : "missing or wrong token (send \"Authorization: Bearer <token>\")%s",
                       s->token && s->cfg.require_login ? " or the token" : "");
            free(ep_sql);
            return;
        }
    }
    if (api) {
        rs->extra_headers = "Cache-Control: no-store\r\n";
        if (!strcmp(sub, "/status")) {
            SvStats st;
            sv_server_stats(s, &st);
            rs->status = 200;
            jb_printf(&rs->body, "{\"ok\":true,\"version\":\"" SV_VERSION_STR "\",\"uptime_s\":%.0f,\"requests\":%lld,"
                      "\"errors\":%lld,\"rows_sent\":%lld,\"connections\":%d,\"busy\":%s,\"timeout_ms\":%d}",
                      st.uptime_s, st.requests, st.errors, st.rows_sent, st.connections, st.busy ? "true" : "false",
                      s->cfg.timeout_ms);
            return;
        }
        if (ep) {
            if (strcmp(rq->method, "GET") && strcmp(rq->method, "POST")) { free(ep_sql); json_error(rs, 405, "use GET or POST"); return; }
            if (ep_public) rs->extra_headers = "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
            handle_endpoint(s, rq, rs, sub + 11, ep_db, ep_sql);
            free(ep_sql);
            return;
        }
        if (!strcmp(sub, "/endpoints")) {
            if (strcmp(rq->method, "GET")) { json_error(rs, 405, "use GET"); return; }
            handle_endpoint_list(s, rq, rs, ep_db);
            return;
        }
        if (!strncmp(sub, "/sessions", 9) && (sub[9] == '\0' || sub[9] == '/')) {
            handle_sessions(s, rq, rs, sub + 9);
            return;
        }
        if (!strcmp(sub, "/query")) {
            if (strcmp(rq->method, "POST")) { json_error(rs, 405, "use POST"); return; }
            handle_query(s, rq, rs);
            return;
        }
        if (!strncmp(sub, "/databases", 10) && (sub[10] == '\0' || sub[10] == '/')) {
            if (strcmp(rq->method, "GET") && strcmp(rq->method, "HEAD")) { json_error(rs, 405, "use GET"); return; }
            handle_catalog(s, rq->user, sub + 10, rs);
            return;
        }
        json_error(rs, 404, "unknown endpoint %s", rq->path);
        return;
    }
    if ((!strcmp(rq->path, "/.segvault") || !strncmp(rq->path, "/.segvault/", 11)) && client_is_local(rq)) { dashboard(s, rs); return; }
    if (rq->root) { handle_web(s, rq, rs); return; }
    if (!strcmp(rq->path, "/") && client_is_local(rq)) { dashboard(s, rs); return; }
    if (apl) json_error(rs, 404, "not found (the API is under %s)", s->api);
    else json_error(rs, 404, "not found");
}

// ── lettura della richiesta ─────────────────────────────────────

static void set_timeout(sock_t fd, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static void header_value(const char* line, char* out, size_t outsz) {
    while (*line == ' ' || *line == '\t') line++;
    size_t l = strlen(line);
    while (l > 0 && (line[l - 1] == ' ' || line[l - 1] == '\t')) l--;
    if (l >= outsz) l = outsz - 1;
    memcpy(out, line, l);
    out[l] = '\0';
}

// Accept-Encoding: "gzip, deflate, br" / "gzip;q=0.5" / "*;q=0.1" / "gzip;q=0" (= rifiutato)
static bool accepts_gzip(const char* v) {
    while (*v) {
        while (*v == ' ' || *v == ',') v++;
        const char* tok = v;
        while (*v && *v != ',' && *v != ';' && *v != ' ') v++;
        size_t tl = (size_t)(v - tok);
        double q = 1.0;
        while (*v && *v != ',') {
            if ((*v == 'q' || *v == 'Q') && v[1] == '=') q = atof(v + 2);
            v++;
        }
        if (((tl == 4 && !strncasecmp(tok, "gzip", 4)) || (tl == 1 && *tok == '*')) && q > 0) return true;
    }
    return false;
}

// 0 = ok, -1 = connessione chiusa / errore, altrimenti codice HTTP d'errore
static int parse_head(SvServer* s, char* head, Request* rq) {
    char* line_end = strstr(head, "\r\n");
    if (!line_end) return 400;
    *line_end = '\0';
    char target[4096], version[16];
    if (sscanf(head, "%15s %4095s %15s", rq->method, target, version) != 3) return 400;
    if (strncmp(version, "HTTP/1.", 7)) return 400;
    rq->http10 = !strcmp(version, "HTTP/1.0");
    rq->keep_alive = !rq->http10;
    snprintf(rq->uri, sizeof(rq->uri), "%s", target);
    char* qm = strchr(target, '?');
    if (qm) { *qm = '\0'; snprintf(rq->query, sizeof(rq->query), "%s", qm + 1); }
    if (target[0] != '/' || url_decode(target, strlen(target), rq->path, sizeof(rq->path), false) < 0) return 400;

    char* p = line_end + 2;
    while (*p) {
        char* e = strstr(p, "\r\n");
        if (!e) break;
        *e = '\0';
        char* colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            const char* v = colon + 1;
            char val[512];
            header_value(v, val, sizeof(val));
            if (!strcasecmp(p, "Content-Length")) {
                char* end;
                unsigned long long cl = strtoull(val, &end, 10);
                if (end == val || *end || val[0] == '-') return 400;
                if (rq->has_length && cl != rq->content_length) return 400;   // due valori diversi: richiesta ambigua
                rq->has_length = true;
                rq->content_length = (size_t)cl;
                if (cl > s->cfg.max_body) return 413;
            } else if (!strcasecmp(p, "Transfer-Encoding")) {
                if (strcasecmp(val, "chunked")) return 501;      // gzip, deflate, ...: non supportati
                if (rq->chunked) return 400;
                rq->chunked = true;
            }
            else if (!strcasecmp(p, "Connection")) {
                // "close", "keep-alive", "Upgrade", "keep-alive, Upgrade" (Firefox)
                for (const char* t = val; *t; ) {
                    while (*t == ' ' || *t == ',') t++;
                    size_t tl = strcspn(t, ", ");
                    if (tl == 5 && !strncasecmp(t, "close", 5)) rq->keep_alive = false;
                    else if (tl == 10 && !strncasecmp(t, "keep-alive", 10)) rq->keep_alive = true;
                    else if (tl == 7 && !strncasecmp(t, "upgrade", 7)) rq->upgrade = true;
                    t += tl;
                }
            }
            else if (!strcasecmp(p, "Content-Type"))  header_value(v, rq->content_type, sizeof(rq->content_type));
            else if (!strcasecmp(p, "Authorization")) header_value(v, rq->auth, sizeof(rq->auth));
            else if (!strcasecmp(p, "X-SegVault-Session")) header_value(v, rq->session, sizeof(rq->session));
            else if (!strcasecmp(p, "Origin"))        header_value(v, rq->origin, sizeof(rq->origin));
            else if (!strcasecmp(p, "Host"))          header_value(v, rq->host, sizeof(rq->host));
            else if (!strcasecmp(p, "Range"))         header_value(v, rq->range, sizeof(rq->range));
            else if (!strcasecmp(p, "If-None-Match")) header_value(v, rq->if_none_match, sizeof(rq->if_none_match));
            else if (!strcasecmp(p, "If-Modified-Since")) header_value(v, rq->if_modified_since, sizeof(rq->if_modified_since));
            else if (!strcasecmp(p, "Accept-Encoding")) rq->accept_gzip = accepts_gzip(val);
        }
        p = e + 2;
    }
    // Content-Length e chunked insieme: due lunghezze possibili, la via del "request smuggling"
    if (rq->chunked && rq->has_length) return 400;
    if (!strcmp(rq->method, "POST") && !rq->has_length && !rq->chunked) return 411;
    return 0;
}

typedef struct {
    char*  data;
    size_t len, cap;
} InBuf;

// legge finché in buf c'è almeno need byte; false su chiusura/timeout
static bool fill(sock_t fd, InBuf* in, size_t need, atomic_int* stop) {
    while (in->len < need) {
        if (*stop) return false;
        if (in->cap < need + 1) {
            size_t nc = in->cap ? in->cap : 8192;
            while (nc < need + 1) nc *= 2;
            char* nd = realloc(in->data, nc);
            if (!nd) return false;
            in->data = nd;
            in->cap = nc;
        }
        size_t room = in->cap - 1 - in->len;
        int r = (int)recv(fd, in->data + in->len, room > (1u << 30) ? (1 << 30) : (int)room, 0);
        if (r <= 0) {
#ifndef _WIN32
            if (r < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        in->len += (size_t)r;
    }
    in->data[in->len] = '\0';
    return true;
}

static void log_request(SvServer* s, const char* client, const Request* rq, const Response* rs, double ms) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    mx_lock(&s->conn_mx);
    s->n_requests++;
    if (rs->status >= 400) s->n_errors++;
    s->n_rows += rs->rows;
    SvLogEntry* e = &s->log[(s->n_requests - 1) % SV_LOG_MAX];
    memset(e, 0, sizeof(*e));
    e->seq = s->n_requests;
    strftime(e->when, sizeof(e->when), "%H:%M:%S", &tmv);
    snprintf(e->method, sizeof(e->method), "%.7s", rq->method);
    one_line(e->path, sizeof(e->path), rq->path);
    snprintf(e->client, sizeof(e->client), "%s", client);
    one_line(e->detail, sizeof(e->detail), rs->detail);
    snprintf(e->user, sizeof(e->user), "%s", rq->user);
    e->status = rs->status;
    e->ms = ms;
    long long sec = (long long)((now_ms() - s->started_ms) / 1000.0);
    SvBucket* b = &s->series[sec % SV_SERIES_MAX];
    if (b->sec != sec) { memset(b, 0, sizeof(*b)); b->sec = sec; }
    b->requests++;
    if (rs->status >= 400) b->errors++;
    b->total_ms += ms;
    if (ms > b->max_ms) b->max_ms = ms;
    b->rows += rs->rows;
    mx_unlock(&s->conn_mx);
}

// posizione di "\r\n" in in->data a partire da pos (i dati possono contenere byte zero)
static size_t find_crlf(const InBuf* in, size_t pos) {
    for (size_t i = pos; i + 1 < in->len; i++) if (in->data[i] == '\r' && in->data[i + 1] == '\n') return i;
    return (size_t)-1;
}

// corpo "Transfer-Encoding: chunked" che inizia a in->data[pos]: decodificato in out,
// *end = primo byte dopo il corpo. 0 = ok, -1 = connessione chiusa, altrimenti codice HTTP
static int read_chunked(SvServer* s, Conn* c, InBuf* in, size_t pos, JsonBuf* out, size_t* end) {
    for (;;) {
        size_t le;
        while ((le = find_crlf(in, pos)) == (size_t)-1) {
            if (in->len - pos > 1024) return 400;               // riga della dimensione troppo lunga
            if (!fill(c->fd, in, in->len + 1, &s->stop)) return -1;
        }
        char* q = in->data + pos;
        size_t size = 0;
        int digits = 0;
        for (; q < in->data + le && isxdigit((unsigned char)*q); q++, digits++) {
            if (digits >= 15) return 413;
            size = size * 16 + (size_t)(isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10));
        }
        if (!digits || (q < in->data + le && *q != ';' && *q != ' ' && *q != '\t')) return 400;   // ";estensione" ignorata
        pos = le + 2;
        if (size == 0) {                                        // ultimo pezzo: eventuali trailer, poi riga vuota
            for (;;) {
                while ((le = find_crlf(in, pos)) == (size_t)-1) {
                    if (in->len - pos > MAX_HEADER) return 431;
                    if (!fill(c->fd, in, in->len + 1, &s->stop)) return -1;
                }
                bool empty = le == pos;
                pos = le + 2;
                if (empty) break;
            }
            *end = pos;
            return 0;
        }
        if (out->len + size > s->cfg.max_body) return 413;
        if (!fill(c->fd, in, pos + size + 2, &s->stop)) return -1;
        if (in->data[pos + size] != '\r' || in->data[pos + size + 1] != '\n') return 400;
        jb_raw(out, in->data + pos, size);
        if (out->oom) return 413;
        pos += size + 2;
    }
}


// ── reverse proxy (siti --site nome=http://host:porta) ─────────

static bool hop_header(const char* line, size_t nl) {
    static const char* const H[] = { "Connection", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding", "Content-Length",
                                     "TE", "Trailer", "Upgrade", "Proxy", "X-Forwarded-For", "X-Forwarded-Proto",
                                     "X-Forwarded-Host", "X-Forwarded-Port", "X-Real-IP" };
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++)
        if (strlen(H[i]) == nl && !strncasecmp(line, H[i], nl)) return true;
    return false;
}

static void proxy_log(SvServer* s, Conn* c, Request* rq, int status, double t0, const char* detail) {
    Response lr = { .status = status };
    snprintf(lr.detail, sizeof(lr.detail), "%s", detail);
    double ms = now_ms() - t0;
    if (!s->cfg.quiet)
        fprintf(stderr, "[segvault-server] %s %s %s%s -> %d (proxy, %.1f ms)\n", c->client, rq->method, rq->host, rq->uri, status, ms);
    log_request(s, c->client, rq, &lr, ms);
}

// true = la connessione del client resta aperta per la prossima richiesta
static bool proxy_request(SvServer* s, Conn* c, InBuf* in, Request* rq, size_t used, double t0) {
    const Site* st = rq->proxy;
    JsonBuf h = {0};
    jb_printf(&h, "%s %s HTTP/1.1\r\n", rq->method, rq->uri);
    char upgrade[128] = "", xff[512] = "";
    for (char* line = rq->headers; line && *line; ) {
        char* e = strstr(line, "\r\n");
        size_t l = e ? (size_t)(e - line) : strlen(line);
        char* colon = memchr(line, ':', l);
        if (colon && colon > line) {
            size_t nl = (size_t)(colon - line);
            if (nl == 7 && !strncasecmp(line, "Upgrade", 7)) {
                const char* v = colon + 1;
                while (*v == ' ') v++;
                snprintf(upgrade, sizeof(upgrade), "%.*s", (int)(line + l - v), v);
            }
            if (nl == 15 && !strncasecmp(line, "X-Forwarded-For", 15)) {
                const char* v = colon + 1;
                while (*v == ' ') v++;
                snprintf(xff, sizeof(xff), "%.*s, ", (int)(line + l - v) > 400 ? 400 : (int)(line + l - v), v);
            }
            if (!hop_header(line, nl)) { jb_raw(&h, line, l); jb_lit(&h, "\r\n"); }
        }
        line = e ? e + 2 : NULL;
    }
    jb_printf(&h, "X-Forwarded-For: %s%s\r\nX-Forwarded-Proto: http\r\nX-Real-IP: %s\r\n", xff, c->client, c->client);
    if (rq->host[0]) jb_printf(&h, "X-Forwarded-Host: %s\r\n", rq->host);
    jb_printf(&h, "X-Forwarded-Port: %d\r\n", s->port);
    if (rq->body_len || rq->has_length) jb_printf(&h, "Content-Length: %llu\r\n", (unsigned long long)rq->body_len);
    bool up = rq->upgrade && upgrade[0];
    if (up) jb_printf(&h, "Connection: Upgrade\r\nUpgrade: %s\r\n\r\n", upgrade);
    else jb_lit(&h, "Connection: close\r\n\r\n");
    if (h.oom) { free(h.data); return false; }

    // con un Upgrade il client può aver già mandato i primi messaggi: vanno al backend
    const char* early = up && in->len > used ? in->data + used : NULL;
    size_t early_len = early ? in->len - used : 0;
    PxResult pr;
    char err[256] = "";
    // la risposta del backend non ha limiti di tempo per il client (eventi, download lenti)
    set_timeout(c->fd, 0);
    int r = px_forward(c->fd, st->phost, st->pport, h.data, h.len, rq->body, rq->body_len, early, early_len,
                       !strcmp(rq->method, "HEAD"), rq->keep_alive && !s->stop, &s->stop, &pr, err, sizeof(err));
    set_timeout(c->fd, IDLE_TIMEOUT_MS);
    free(h.data);
    char detail[300];
    if (r < 0) {
        Response rs = {0};
        rs.close = !rq->keep_alive || s->stop;
        JsonBuf* b = &rs.body;
        page_start(b, "502 Bad Gateway");
        jb_lit(b, "<h1>The dev server is not answering</h1><p class=\"sub\">");
        html_esc(b, rq->site);
        jb_printf(b, " is served by <code>http://%s:%d</code>: ", st->phost, st->pport);
        html_esc(b, err);
        jb_lit(b, ".</p><p>Start it (for example <code>npm run dev</code>, <code>php artisan serve</code>, "
                  "<code>python manage.py runserver</code>) and reload this page.</p></main></body></html>");
        rs.status = 502;
        rs.content_type = "text/html; charset=utf-8";
        rs.extra_headers = "Cache-Control: no-store\r\nRetry-After: 1\r\n";
        bool sent = send_response(s, c->fd, rq, &rs);
        free(rs.body.data);
        snprintf(detail, sizeof(detail), "proxy %s:%d: %s", st->phost, st->pport, err);
        proxy_log(s, c, rq, 502, t0, detail);
        if (up) in->len = used;                 // i byte in anticipo non servono più
        return sent && !rs.close && !up;
    }
    snprintf(detail, sizeof(detail), "proxy %s:%d%s", st->phost, st->pport, pr.status == 101 ? " (websocket)" : "");
    proxy_log(s, c, rq, pr.status, t0, detail);
    if (pr.status == 101) in->len = used;
    return pr.keep_alive && !s->stop;
}

// risponde a una richiesta della connessione; false = connessione da chiudere
static bool serve_one(SvServer* s, Conn* c, InBuf* in) {
    // intestazione: fino a \r\n\r\n
    char* hend = NULL;
    while (!(hend = in->data ? strstr(in->data, "\r\n\r\n") : NULL)) {
        if (in->len >= MAX_HEADER) {
            Response rs = { .close = true };
            json_error(&rs, 431, "request headers too large");
            send_response(s, c->fd, NULL, &rs);
            free(rs.body.data);
            return false;
        }
        if (!fill(c->fd, in, in->len + 1, &s->stop)) return false;
    }
    size_t head_len = (size_t)(hend - in->data) + 4;
    double t0 = now_ms();
    Request rq;
    memset(&rq, 0, sizeof(rq));
    char saved = in->data[head_len - 2];
    in->data[head_len - 2] = '\0';              // la testata termina con l'ultimo \r\n
    rq.client = c->client;
    rq.client_port = c->client_port;
    {                                            // tutte le intestazioni servono a PHP (HTTP_*) e al proxy
        const char* first = strstr(in->data, "\r\n");
        rq.headers = first ? strdup(first + 2) : NULL;
    }
    int perr = parse_head(s, in->data, &rq);
    in->data[head_len - 2] = saved;
    Response rs = {0};
    snprintf(rs.client, sizeof(rs.client), "%s", c->client);
    if (perr) {
        rs.close = true;
        json_error(&rs, perr, "%s", perr == 413 ? "request body too large" :
                                    perr == 411 ? "a Content-Length header (or a chunked body) is required" :
                                    perr == 501 ? "unsupported Transfer-Encoding (only chunked)" : "malformed request");
        send_response(s, c->fd, &rq, &rs);
        free(rs.body.data);
        free(rq.headers);
        return false;
    }
    size_t used;                                 // byte della richiesta in in->data (intestazione + corpo)
    JsonBuf chunked = {0};
    char after = 0;
    if (rq.chunked) {
        int cerr = read_chunked(s, c, in, head_len, &chunked, &used);
        if (cerr) {
            free(chunked.data);
            if (cerr > 0) {
                rs.close = true;
                json_error(&rs, cerr, "%s", cerr == 413 ? "request body too large" : "malformed chunked body");
                send_response(s, c->fd, &rq, &rs);
                free(rs.body.data);
            }
            free(rq.headers);
            return false;
        }
        jb_raw(&chunked, "", 1);                 // terminatore (il corpo resta di chunked.len - 1 byte)
        if (chunked.oom) { free(chunked.data); free(rq.headers); return false; }
        rq.body = chunked.data;
        rq.body_len = rq.content_length = chunked.len - 1;
        rq.has_length = true;
    } else {
        if (!fill(c->fd, in, head_len + rq.content_length, &s->stop)) { free(rq.headers); return false; }
        used = head_len + rq.content_length;
        after = in->data[used];
        in->data[used] = '\0';
        rq.body = in->data + head_len;
        rq.body_len = rq.content_length;
    }

    resolve_site(s, &rq);
    if (rq.proxy) {                              // sito servito da un dev server: la richiesta passa a lui
        if (!rq.chunked) in->data[used] = after; // il corpo ha la sua lunghezza; i byte dopo vanno inoltrati intatti
        bool keep = proxy_request(s, c, in, &rq, used, t0);
        free(chunked.data);
        free(rq.headers);
        if (!keep) return false;
        memmove(in->data, in->data + used, in->len - used);
        in->len -= used;
        in->data[in->len] = '\0';
        return true;
    }
    route(s, &rq, &rs);
    rs.close = !rq.keep_alive || s->stop;
    if (!rq.chunked) in->data[used] = after;
    free(chunked.data);
    rq.body = NULL;
    bool sent = send_response(s, c->fd, &rq, &rs);
    double ms = now_ms() - t0;
    if (!s->cfg.quiet)
        fprintf(stderr, "[segvault-server] %s %s %s -> %d (%llu bytes, %.1f ms)\n",
                c->client, rq.method, rq.path, rs.status, (unsigned long long)rs.body.len, ms);
    if (strcmp(rq.method, "OPTIONS")) log_request(s, c->client, &rq, &rs, ms);
    free(rs.body.data);
    free(rs.own_headers);
    if (rs.file) fclose(rs.file);
    free(rq.headers);
    if (!sent || rs.close) return false;
    // richieste in coda (pipelining): si sposta il resto all'inizio
    memmove(in->data, in->data + used, in->len - used);
    in->len -= used;
    in->data[in->len] = '\0';
    return true;
}

static void serve_connection(Conn* c) {
    InBuf in = {0};
    set_timeout(c->fd, IDLE_TIMEOUT_MS);
    while (serve_one(c->s, c, &in)) {}          // keep-alive: una richiesta dopo l'altra
    free(in.data);
}

// ── protocollo MySQL ────────────────────────────────────────────
// Una connessione MySQL è una sessione del motore (database e transazione propri), con l'utente
// del login. Ogni comando prende il motore come una richiesta HTTP (exec_mx).

typedef struct { uint8_t* p; size_t n; uint8_t seq; } MyPkt;

// legge un pacchetto (anche spezzato in più parti da 16 MB); false su chiusura o errore
static bool my_read(SvServer* s, Conn* c, InBuf* in, MyPkt* out, size_t max) {
    size_t total = 0;
    uint8_t* buf = NULL;
    for (;;) {
        if (!fill(c->fd, in, 4, &s->stop)) { free(buf); return false; }
        const uint8_t* h = (const uint8_t*)in->data;
        size_t len = (size_t)h[0] | (size_t)h[1] << 8 | (size_t)h[2] << 16;
        out->seq = h[3];
        if (total + len > max) { free(buf); return false; }
        if (!fill(c->fd, in, 4 + len, &s->stop)) { free(buf); return false; }
        uint8_t* nb = realloc(buf, total + len + 1);
        if (!nb) { free(buf); return false; }
        buf = nb;
        memcpy(buf + total, in->data + 4, len);
        total += len;
        memmove(in->data, in->data + 4 + len, in->len - 4 - len);
        in->len -= 4 + len;
        if (len < 0xffffff) break;
    }
    buf[total] = 0;
    out->p = buf;
    out->n = total;
    return true;
}

static bool my_send(Conn* c, MwOut* o) {
    bool ok = !o->oom && send_all(c->fd, (const char*)o->p, o->n);
    o->n = 0;
    return ok;
}

// codice d'errore MySQL dal messaggio (i driver ne usano alcuni: 1062 duplicato, 1146 tabella, ...)
static unsigned my_errcode(const char* m, const char** state) {
    *state = "HY000";
    if (strstr(m, "Access denied")) { *state = "42000"; return 1142; }
    if (strstr(m, "Duplicate") || strstr(m, "duplicate") || strstr(m, "UNIQUE")) { *state = "23000"; return 1062; }
    if (strstr(m, "FOREIGN KEY") || strstr(m, "foreign key")) { *state = "23000"; return 1452; }
    if (strstr(m, "cannot be NULL") || strstr(m, "NOT NULL")) { *state = "23000"; return 1048; }
    if (strstr(m, "Unknown column")) { *state = "42S22"; return 1054; }
    if (strstr(m, "Unknown database") || strstr(m, "Database not found") || strstr(m, "database not found")) { *state = "42000"; return 1049; }
    if ((strstr(m, "Table") || strstr(m, "table")) && strstr(m, "not found")) { *state = "42S02"; return 1146; }
    if (strstr(m, "already exists")) { *state = "42S01"; return 1050; }
    if (strstr(m, "Expected") || strstr(m, "Unexpected") || strstr(m, "syntax")) { *state = "42000"; return 1064; }
    return 1105;
}

static void my_error(MwOut* o, const char* msg) {
    const char* st;
    unsigned code = my_errcode(msg, &st);
    mw_err(o, code, st, msg);
}

// login: utenti di SegVault (verificatore mysql_native_password); "root" col token = accesso completo;
// server senza token né --users = locale e aperto: qualunque nome entra come root
static int my_login(SvServer* s, const MwLogin* lg, const uint8_t* resp, size_t rlen, const uint8_t scramble[20], char* user, size_t un) {
    user[0] = '\0';
    int r = db_login_native(lg->user, scramble, resp, rlen);
    if (r == 1) { snprintf(user, un, "%s", lg->user); return 1; }
    if (r == -1) return -1;
    if (s->token && !strcmp(lg->user, "root")) {
        uint8_t a[20], b[20];
        auth_sha1(s->token, strlen(s->token), a);
        auth_sha1(a, 20, b);
        uint8_t buf[40], x[20], cand[20], chk[20];
        memcpy(buf, scramble, 20);
        memcpy(buf + 20, b, 20);
        auth_sha1(buf, 40, x);
        for (int i = 0; i < 20 && rlen == 20; i++) cand[i] = resp[i] ^ x[i];
        auth_sha1(cand, 20, chk);
        if (rlen == 20 && !memcmp(chk, b, 20)) return 1;          // root
        return 0;
    }
    if (!s->token && !s->cfg.require_login) return 1;          // locale, aperto (come l'HTTP senza token)
    return 0;
}

static bool my_enter(SvServer* s, DbSession* ds) {
    exec_begin(s);
    for (double waited = 0; !db_session_enter(ds); waited += 20) {
        exec_end(s);
        if (waited >= SESSION_WAIT_MS || s->stop) return false;
        sleep_ms(20);
        exec_begin(s);
    }
    return true;
}

static void my_leave(SvServer* s, DbSession* ds) {
    db_session_leave(ds);
    exec_end(s);
}

static unsigned my_status(bool autocommit) {
    return (in_transaction() ? MW_STATUS_IN_TRANS : 0) | (autocommit ? MW_STATUS_AUTOCOMMIT : 0);
}

// COM_QUERY: le istruzioni una dopo l'altra; ogni risultato è un resultset o un OK
// (binary: risposta a COM_STMT_EXECUTE, righe in formato binario).
// *autocommit è quello della connessione: con SET autocommit = 0 la prima istruzione che
// modifica i dati apre una transazione, che resta aperta fino a COMMIT / ROLLBACK (come MySQL;
// le sole letture non la aprono, così una connessione che legge non blocca chi scrive)
static void my_query(SvServer* s, Conn* c, MwOut* o, char* sql, const char* user, bool binary, bool* autocommit) {
    int len = (int)strlen(sql), cap = 32, nst;
    SqlRange* ranges = NULL;
    for (;;) {
        free(ranges);
        ranges = malloc(sizeof(SqlRange) * (size_t)cap);
        if (!ranges) { mw_err(o, 1105, "HY000", "out of memory"); return; }
        nst = sql_split(sql, len, ranges, cap);
        if (nst < cap) break;
        cap *= 2;
    }
    int last = -1;
    for (int k = 0; k < nst; k++) if (ranges[k].has_code) last = k;
    if (last < 0) { mw_ok(o, 0, 0, my_status(*autocommit), NULL); free(ranges); return; }
    Response rs = {0};
    snprintf(rs.client, sizeof(rs.client), "%s", c->client);
    for (int k = 0; k <= last; k++) {
        if (!ranges[k].has_code) continue;
        int a = ranges[k].code_start, e = ranges[k].end;
        char saved = sql[e];
        sql[e] = '\0';
        unsigned more = k < last ? MW_STATUS_MORE_RESULTS : 0;
        const char* one = sql + a;
        int ac = mw_set_autocommit(one);
        if (ac >= 0) {                                     // SET autocommit = 1 conferma la transazione aperta
            sql[e] = saved;
            if (ac == 1 && in_transaction() && !db_commit()) { my_error(o, "Failed to commit"); break; }
            *autocommit = ac == 1;
            mw_ok(o, 0, 0, my_status(*autocommit) | more, NULL);
            continue;
        }
        int kind = mw_stmt_kind(one);
        if ((kind == MW_KIND_COMMIT || kind == MW_KIND_ROLLBACK) && !in_transaction()) {
            sql[e] = saved;                                // niente da confermare: in MySQL non è un errore
            mw_ok(o, 0, 0, my_status(*autocommit) | more, NULL);
            continue;
        }
        if (mw_client_noise(o, one, my_status(*autocommit) | more)) { sql[e] = saved; continue; }
        if (kind == MW_KIND_DML && !*autocommit && !in_transaction() && !db_begin()) {
            sql[e] = saved;
            my_error(o, "Failed to begin transaction");
            break;
        }
        QueryResult* r = db_execute(one);
        audit_stmt(s, &rs, user, one, r);
        sql[e] = saved;
        bool timeout = exec_timed_out(s);
        if (!r || !r->success || timeout) {
            my_error(o, timeout ? "Query execution was interrupted (time limit)" : r ? r->error : "out of memory");
            db_result_free(r);
            break;
        }
        unsigned st = my_status(*autocommit) | more;
        const char* cur = db_current_database();
        if (r->num_cols > 0) { if (binary) mw_resultset_binary(o, r, cur ? cur : "", st); else mw_resultset(o, r, cur ? cur : "", st); }
        else mw_ok(o, (uint64_t)(r->rows_affected > 0 ? r->rows_affected : 0),
                   (uint64_t)(r->last_insert_id > 0 ? r->last_insert_id : 0), st, NULL);
        db_result_free(r);
    }
    free(ranges);
}

// prepared statement di una connessione
typedef struct {
    uint32_t    id;
    char*       sql;
    int         nparams;
    uint8_t*    types;          // 2 byte per parametro, dall'ultimo EXECUTE che li ha mandati
    bool        types_set;
    MwLongData* ld;             // valori mandati con COM_STMT_SEND_LONG_DATA
} MyStmt;

typedef struct { MyStmt* v; int n, cap; uint32_t next_id; bool autocommit; } MyStmts;   // + autocommit della connessione

#define MAX_STMTS 4096           // come max_prepared_stmt_count, ma per connessione

static void stmt_clear_ld(MyStmt* st) {
    for (int i = 0; st->ld && i < st->nparams; i++) { free(st->ld[i].p); st->ld[i] = (MwLongData){0}; }
}

static void stmt_free(MyStmt* st) {
    stmt_clear_ld(st);
    free(st->ld);
    free(st->types);
    free(st->sql);
}

static MyStmt* stmt_find(MyStmts* ss, uint32_t id) {
    for (int i = 0; i < ss->n; i++) if (ss->v[i].id == id) return &ss->v[i];
    return NULL;
}

static void stmt_close(MyStmts* ss, uint32_t id) {
    MyStmt* st = stmt_find(ss, id);
    if (!st) return;
    stmt_free(st);
    *st = ss->v[--ss->n];
}

static void stmts_free(MyStmts* ss) {
    for (int i = 0; i < ss->n; i++) stmt_free(&ss->v[i]);
    free(ss->v);
    *ss = (MyStmts){0};
}

static bool has_word(const char* s, const char* w) {
    size_t l = strlen(w);
    for (const char* p = s; *p; p++)
        if (!strncasecmp(p, w, l) && (p == s || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
            !isalnum((unsigned char)p[l]) && p[l] != '_') return true;
    return false;
}

// istruzione che legge soltanto: per conoscere le colonne la si può eseguire già al PREPARE
static bool stmt_reads_only(const char* sql) {
    while (isspace((unsigned char)*sql) || *sql == '(') sql++;
    static const char* const W[] = { "SELECT", "WITH", "SHOW", "DESCRIBE", "DESC", "EXPLAIN", "VALUES", "TABLE" };
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++) {
        size_t l = strlen(W[i]);
        if (!strncasecmp(sql, W[i], l) && !isalnum((unsigned char)sql[l]) && sql[l] != '_') {
            if (i == 1) return !has_word(sql, "INSERT") && !has_word(sql, "UPDATE") && !has_word(sql, "DELETE");
            return true;
        }
    }
    return false;
}

// COM_STMT_PREPARE: si contano i '?'; per una lettura si esegue l'istruzione con i parametri a NULL
// così il client conosce subito le colonne (mysqli_stmt_result_metadata, JDBC getMetaData)
static void my_prepare(MyStmts* ss, MwOut* o, const char* sql) {
    if (ss->n >= MAX_STMTS) { mw_err(o, 1461, "42000", "Too many prepared statements on this connection (max 4096): close the ones you do not need"); return; }
    const char* q = sql;
    while (isspace((unsigned char)*q)) q++;
    if (!*q) { mw_err(o, 1065, "42000", "Query was empty"); return; }
    int np = mw_count_params(sql);
    if (np > 65535) { mw_err(o, 1390, "HY000", "Prepared statement contains too many placeholders"); return; }
    if (ss->n == ss->cap) {
        int nc = ss->cap ? ss->cap * 2 : 8;
        MyStmt* nv = realloc(ss->v, sizeof(MyStmt) * (size_t)nc);
        if (!nv) { mw_err(o, 1105, "HY000", "out of memory"); return; }
        ss->v = nv;
        ss->cap = nc;
    }
    MyStmt st = { .id = ++ss->next_id, .sql = strdup(sql), .nparams = np };
    st.types = calloc((size_t)(np ? np : 1), 2);
    st.ld = calloc((size_t)(np ? np : 1), sizeof(MwLongData));
    if (!st.sql || !st.types || !st.ld) { stmt_free(&st); mw_err(o, 1105, "HY000", "out of memory"); return; }
    QueryResult* meta = NULL;
    if (stmt_reads_only(sql)) {
        // i '?' come NULL: stessa forma del risultato, quasi sempre senza righe da leggere
        char* text = mw_with_nulls(sql);
        if (text && !mw_client_noise(&(MwOut){0}, text, 0)) meta = db_execute(text);
        free(text);
        if (meta && !meta->success) {                      // errore vero (tabella o colonna sbagliata): come MySQL
            my_error(o, meta->error);
            db_result_free(meta);
            stmt_free(&st);
            return;
        }
    }
    ss->v[ss->n++] = st;
    const char* cur = db_current_database();
    mw_prepare_ok(o, st.id, np, meta, cur ? cur : "", my_status(ss->autocommit));
    db_result_free(meta);
}

static void my_execute(SvServer* s, Conn* c, MyStmts* ss, MwOut* o, const uint8_t* p, size_t n, const char* user) {
    if (n < 5) { mw_err(o, 1835, "HY000", "Malformed COM_STMT_EXECUTE packet"); return; }
    uint32_t id = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    MyStmt* st = stmt_find(ss, id);
    if (!st) { mw_err(o, 1243, "HY000", "Unknown prepared statement handler given to mysqld_stmt_execute"); return; }
    char err[160];
    char* sql = mw_bind_execute(st->sql, p + 5, n - 5, st->nparams, st->types, &st->types_set, st->ld, err, sizeof(err));
    stmt_clear_ld(st);                                     // i dati lunghi valgono per un solo EXECUTE
    if (!sql) { mw_err(o, 1210, "HY000", err); return; }
    my_query(s, c, o, sql, user, true, &ss->autocommit);
    free(sql);
}

// COM_STMT_SEND_LONG_DATA: stmt id, numero del parametro, un pezzo del valore (nessuna risposta)
static void my_long_data(MyStmts* ss, const uint8_t* p, size_t n) {
    if (n < 7) return;
    uint32_t id = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    unsigned k = (unsigned)p[5] | (unsigned)p[6] << 8;
    MyStmt* st = stmt_find(ss, id);
    if (!st || (int)k >= st->nparams) return;
    MwLongData* d = &st->ld[k];
    size_t add = n - 7;
    char* q = realloc(d->p, d->n + add + 1);
    if (!q) return;
    memcpy(q + d->n, p + 7, add);
    d->p = q;
    d->n += add;
    d->p[d->n] = '\0';
    d->set = true;
}

static void serve_mysql(Conn* c) {
    db_set_mysql_mode(true);                     // questo thread serve solo la connessione MySQL
    SvServer* s = c->s;
    InBuf in = {0};
    MwOut o = {0};
    set_timeout(c->fd, 8 * 3600 * 1000);                 // wait_timeout di MySQL: 8 ore
    uint8_t scramble[20];
    char tok[48];
    if (!sv_random_token(tok, 20)) return;
    for (int i = 0; i < 20; i++) scramble[i] = (uint8_t)((tok[i] % 94) + 33);   // byte stampabili, mai 0
    mx_lock(&s->conn_mx);
    uint32_t id = ++s->mysql_ids;
    mx_unlock(&s->conn_mx);
    mw_handshake(&o, id, scramble);
    if (!my_send(c, &o)) { free(o.p); return; }

    MyPkt pk;
    if (!my_read(s, c, &in, &pk, 65536)) { free(o.p); free(in.data); return; }
    MwLogin lg;
    bool parsed = mw_parse_login(pk.p, pk.n, &lg);
    o.seq = (uint8_t)(pk.seq + 1);
    free(pk.p);
    if (!parsed) { mw_err(&o, 1043, "08S01", "Bad handshake (protocol 4.1 required)"); my_send(c, &o); free(o.p); free(in.data); return; }
    uint8_t resp[64];
    size_t rlen = lg.auth_len;
    memcpy(resp, lg.auth, rlen);
    if (lg.plugin[0] && strcmp(lg.plugin, "mysql_native_password")) {     // es. caching_sha2_password: si chiede l'altro
        mw_auth_switch(&o, scramble);
        if (!my_send(c, &o) || !my_read(s, c, &in, &pk, 4096)) { free(o.p); free(in.data); return; }
        o.seq = (uint8_t)(pk.seq + 1);
        rlen = pk.n < sizeof(resp) ? pk.n : sizeof(resp);
        memcpy(resp, pk.p, rlen);
        free(pk.p);
    }
    char user[64];
    int lr = my_login(s, &lg, resp, rlen, scramble, user, sizeof(user));
    if (lr != 1) {
        char m[300];
        if (lr == -1) snprintf(m, sizeof(m), "User '%.63s' was created before MySQL logins were supported: set the password again (ALTER USER %.63s IDENTIFIED BY '...')", lg.user, lg.user);
        else snprintf(m, sizeof(m), "Access denied for user '%.63s' (using password: %s)", lg.user, rlen ? "YES" : "NO");
        sleep_ms(300);                                      // rallenta i tentativi
        mw_err(&o, 1045, "28000", m);
        my_send(c, &o);
        if (!s->cfg.quiet) fprintf(stderr, "%s mysql login refused for '%s'\n", c->client, lg.user);
        free(o.p); free(in.data);
        return;
    }
    mx_lock(&s->exec_mx);
    DbSession* ds = db_session_new();
    mx_unlock(&s->exec_mx);
    if (!ds) { mw_err(&o, 1040, "08004", "Too many connections"); my_send(c, &o); free(o.p); free(in.data); return; }
    const char* first_db = lg.db[0] ? lg.db : s->default_db;
    bool ok = true;
    if (first_db && first_db[0]) {
        if (!my_enter(s, ds)) ok = false;
        else {
            db_set_user(user);
            char use[96];
            snprintf(use, sizeof(use), "USE `%.63s`", first_db);
            QueryResult* r = db_execute(use);
            if (!r || !r->success) { my_error(&o, r ? r->error : "out of memory"); ok = false; }
            db_result_free(r);
            db_set_user(NULL);
            my_leave(s, ds);
        }
    }
    MyStmts stmts = { .autocommit = true };
    if (ok) mw_ok(&o, 0, 0, MW_STATUS_AUTOCOMMIT, NULL);
    else if (!o.n) mw_err(&o, 1205, "HY000", "Lock wait timeout exceeded");
    if (!my_send(c, &o) || !ok) goto done;
    if (!s->cfg.quiet) fprintf(stderr, "%s mysql login '%s'\n", c->client, user[0] ? user : "root");

    size_t max = s->cfg.max_body ? s->cfg.max_body : (16u << 20);
    while (!s->stop && my_read(s, c, &in, &pk, max)) {
        o.seq = (uint8_t)(pk.seq + 1);
        uint8_t cmd = pk.n ? pk.p[0] : 0;
        if (cmd == MW_COM_QUIT) { free(pk.p); break; }
        if (cmd == MW_COM_PING) mw_ok(&o, 0, 0, MW_STATUS_AUTOCOMMIT, NULL);
        else if (cmd == MW_COM_FIELD_LIST) mw_eof(&o, MW_STATUS_AUTOCOMMIT);
        else if (cmd == MW_COM_STMT_CLOSE) {               // nessuna risposta
            if (pk.n >= 5) stmt_close(&stmts, (uint32_t)pk.p[1] | (uint32_t)pk.p[2] << 8 | (uint32_t)pk.p[3] << 16 | (uint32_t)pk.p[4] << 24);
        } else if (cmd == MW_COM_STMT_SEND_LONG_DATA) my_long_data(&stmts, pk.p, pk.n);   // nessuna risposta
        else if (cmd == MW_COM_STMT_RESET) {
            MyStmt* st = pk.n >= 5 ? stmt_find(&stmts, (uint32_t)pk.p[1] | (uint32_t)pk.p[2] << 8 | (uint32_t)pk.p[3] << 16 | (uint32_t)pk.p[4] << 24) : NULL;
            if (!st) mw_err(&o, 1243, "HY000", "Unknown prepared statement handler given to mysqld_stmt_reset");
            else { stmt_clear_ld(st); mw_ok(&o, 0, 0, my_status(stmts.autocommit), NULL); }
        } else if (cmd == MW_COM_QUERY || cmd == MW_COM_INIT_DB || cmd == MW_COM_RESET_CONNECTION ||
                   cmd == MW_COM_STMT_PREPARE || cmd == MW_COM_STMT_EXECUTE) {
            if (!my_enter(s, ds)) mw_err(&o, 1205, "HY000", "Lock wait timeout exceeded: another session has an open transaction");
            else {
                if (!db_set_user(user)) mw_err(&o, 1045, "28000", "The user no longer exists");
                else if (cmd == MW_COM_QUERY) {
                    char* sql = (char*)pk.p + 1;
                    pk.p[pk.n] = 0;
                    my_query(s, c, &o, sql, user, false, &stmts.autocommit);
                } else if (cmd == MW_COM_STMT_PREPARE) {
                    pk.p[pk.n] = 0;
                    my_prepare(&stmts, &o, (char*)pk.p + 1);
                } else if (cmd == MW_COM_STMT_EXECUTE) {
                    my_execute(s, c, &stmts, &o, pk.p, pk.n, user);
                } else if (cmd == MW_COM_INIT_DB) {
                    char use[160];
                    snprintf(use, sizeof(use), "USE `%.100s`", (char*)pk.p + 1);
                    QueryResult* r = db_execute(use);
                    if (r && r->success) mw_ok(&o, 0, 0, MW_STATUS_AUTOCOMMIT, NULL);
                    else my_error(&o, r ? r->error : "out of memory");
                    db_result_free(r);
                } else {                                   // COM_RESET_CONNECTION: transazione annullata
                    if (in_transaction()) db_rollback();
                    stmts_free(&stmts);
                    stmts.autocommit = true;
                    mw_ok(&o, 0, 0, MW_STATUS_AUTOCOMMIT, NULL);
                }
                db_set_user(NULL);
                my_leave(s, ds);
            }
        } else mw_err(&o, 1047, "08S01", "Unknown command");
        free(pk.p);
        if (o.n && !my_send(c, &o)) break;
    }
done:
    stmts_free(&stmts);
    mx_lock(&s->exec_mx);
    db_session_end(ds);                                    // una transazione lasciata aperta viene annullata
    mx_unlock(&s->exec_mx);
    free(o.p);
    free(in.data);
}

// ── thread, accettazione ────────────────────────────────────────

static void conn_finish(Conn* c) {
    SvServer* s = c->s;
    mx_lock(&s->conn_mx);
    s->conns[c->slot] = BAD_SOCK;
    s->active--;
    mx_unlock(&s->conn_mx);
    sock_close(c->fd);
    free(c);
}

#ifdef _WIN32
static DWORD WINAPI conn_thread(LPVOID arg) { Conn* c = (Conn*)arg; if (c->mysql) serve_mysql(c); else serve_connection(c); conn_finish(c); return 0; }
static bool spawn(Conn* c) {
    HANDLE h = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
    if (!h) return false;
    CloseHandle(h);
    return true;
}
#else
static void* conn_thread(void* arg) { Conn* c = (Conn*)arg; if (c->mysql) serve_mysql(c); else serve_connection(c); conn_finish(c); return NULL; }
static bool spawn(Conn* c) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&t, &a, conn_thread, c) == 0;
    pthread_attr_destroy(&a);
    return ok;
}
#endif

SvServer* sv_server_start(const SvServerConfig* cfg, char* err, size_t errsz) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { snprintf(err, errsz, "WSAStartup failed"); return NULL; }
#endif
    SvServer* s = calloc(1, sizeof(SvServer));
    if (!s) { snprintf(err, errsz, "out of memory"); return NULL; }
    s->listen_fd = BAD_SOCK;
    s->mysql_fd = BAD_SOCK;
    s->cfg = *cfg;
    if (!s->cfg.bind_addr) s->cfg.bind_addr = "127.0.0.1";
    if (!s->cfg.max_body) s->cfg.max_body = 16u << 20;
    if (s->cfg.max_clients <= 0) s->cfg.max_clients = 64;
    s->token      = cfg->token && cfg->token[0] ? strdup(cfg->token) : NULL;
    s->cors       = cfg->cors_origin && cfg->cors_origin[0] ? strdup(cfg->cors_origin) : NULL;
    s->default_db = cfg->default_db && cfg->default_db[0] ? strdup(cfg->default_db) : NULL;
    if (cfg->static_dir && cfg->static_dir[0]) {
        s->static_dir = strdup(cfg->static_dir);
        size_t l = s->static_dir ? strlen(s->static_dir) : 0;
        while (l > 1 && (s->static_dir[l - 1] == '/' || s->static_dir[l - 1] == '\\')) s->static_dir[--l] = '\0';
    }
    s->conns = malloc(sizeof(sock_t) * (size_t)s->cfg.max_clients);
    s->listen_fd = BAD_SOCK;
    s->mysql_fd = BAD_SOCK;
    mx_init(&s->conn_mx);
    mx_init(&s->exec_mx);
    if (!s->conns) { snprintf(err, errsz, "out of memory"); sv_server_free(s); return NULL; }
    for (int i = 0; i < s->cfg.max_clients; i++) s->conns[i] = BAD_SOCK;
    s->started_ms = now_ms();
    if (s->default_db && !valid_ident(s->default_db)) { snprintf(err, errsz, "invalid default database name"); sv_server_free(s); return NULL; }
    if (cfg->audit_file && cfg->audit_file[0]) {
        s->audit = fopen(cfg->audit_file, "a");
        if (!s->audit) { snprintf(err, errsz, "cannot open the audit log %.200s: %s", cfg->audit_file, strerror(errno)); sv_server_free(s); return NULL; }
    }
    s->cfg.audit_file = NULL;                              // il file è già aperto: la stringa non serve più
    if (cfg->n_sites > 0) {
        s->sites = calloc((size_t)cfg->n_sites, sizeof(Site));
        if (!s->sites) { snprintf(err, errsz, "out of memory"); sv_server_free(s); return NULL; }
        for (int i = 0; i < cfg->n_sites; i++) {
            const char* nm = cfg->sites[i].name ? cfg->sites[i].name : "";
            const char* tg = cfg->sites[i].target ? cfg->sites[i].target : "";
            Site* st = &s->sites[s->n_sites];
            size_t nl = strlen(nm);
            bool ok = nl > 0 && nl < sizeof(st->name) && nm[0] != '.' && nm[nl - 1] != '.' && !strstr(nm, "..");
            for (size_t k = 0; k < nl && ok; k++) ok = isalnum((unsigned char)nm[k]) || nm[k] == '-' || nm[k] == '_' || nm[k] == '.';
            if (!ok) { snprintf(err, errsz, "invalid site name '%.100s' (letters, digits, '-', '_' and dots)", nm); sv_server_free(s); return NULL; }
            for (size_t k = 0; k <= nl; k++) st->name[k] = (char)tolower((unsigned char)nm[k]);
            if (find_site(s, st->name, nl)) { snprintf(err, errsz, "site '%.100s' given twice", nm); sv_server_free(s); return NULL; }
            if (!strncasecmp(tg, "http", 4) || strspn(tg, "0123456789") == strlen(tg) || (strchr(tg, ':') && !path_is(tg, S_IFDIR))) {
                if (!px_parse_target(tg, st->phost, sizeof(st->phost), &st->pport)) {
                    snprintf(err, errsz, "site %.100s: invalid address '%.200s' (e.g. http://127.0.0.1:5173)", nm, tg);
                    sv_server_free(s); return NULL;
                }
            } else {
                if (!path_is(tg, S_IFDIR)) { snprintf(err, errsz, "site %.100s: folder not found: %.200s", nm, tg); sv_server_free(s); return NULL; }
                st->dir = strdup(tg);
                if (!st->dir) { snprintf(err, errsz, "out of memory"); sv_server_free(s); return NULL; }
                size_t dl = strlen(st->dir);
                while (dl > 1 && (st->dir[dl - 1] == '/' || st->dir[dl - 1] == '\\')) st->dir[--dl] = '\0';
            }
            s->n_sites++;
        }
    }
    s->cfg.sites = NULL;
    if ((cfg->php_cgi && cfg->php_cgi[0]) || (cfg->php_fcgi && cfg->php_fcgi[0])) {
        bool any_dir = s->static_dir != NULL;
        for (int i = 0; i < s->n_sites; i++) if (s->sites[i].dir) any_dir = true;
        if (!any_dir) { snprintf(err, errsz, "PHP needs a web folder (--static DIR or --site NAME=DIR)"); sv_server_free(s); return NULL; }
        char abs[4096] = "";
        if (s->static_dir) {
#ifdef _WIN32
        if (!_fullpath(abs, s->static_dir, sizeof(abs))) abs[0] = '\0';
        for (char* q = abs; *q; q++) if (*q == '\\') *q = '/';
#else
        if (!realpath(s->static_dir, abs)) abs[0] = '\0';
#endif
        if (!abs[0]) { snprintf(err, errsz, "web folder not found: %.200s", s->static_dir); sv_server_free(s); return NULL; }
        size_t al = strlen(abs);
        while (al > 1 && abs[al - 1] == '/') abs[--al] = '\0';
        s->doc_root = strdup(abs);
        }
        char perr[300];
        s->php = cfg->php_fcgi && cfg->php_fcgi[0] ? fcgi_pool_external(cfg->php_fcgi, perr, sizeof(perr))
                                                    : fcgi_pool_spawn(cfg->php_cgi, cfg->php_workers, perr, sizeof(perr));
        if (!s->php) { snprintf(err, errsz, "PHP: %s", perr); sv_server_free(s); return NULL; }
    }
    s->cfg.php_cgi = s->cfg.php_fcgi = NULL;
    {
        const char* ap = cfg->api_prefix ? cfg->api_prefix : API_PREFIX;
        if (!strcmp(ap, "none") || !strcmp(ap, "off")) ap = "";
        while (*ap == '/') ap++;
        size_t l = strlen(ap);
        while (l > 0 && ap[l - 1] == '/') l--;
        bool ok = l < sizeof(s->api) - 2;
        for (size_t i = 0; i < l && ok; i++) {
            char c = ap[i];
            ok = isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '.';
            if (c == '.' && (i == 0 || ap[i - 1] == '/')) ok = false;          // niente "/." (file nascosti, "..")
        }
        if (!ok) { snprintf(err, errsz, "invalid API prefix '%.100s' (e.g. /api/v1, /_segvault, none)", cfg->api_prefix); sv_server_free(s); return NULL; }
        if (l) snprintf(s->api, sizeof(s->api), "/%.*s", (int)l, ap);
        s->cfg.api_prefix = NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->port);
    addr.sin_addr.s_addr = inet_addr(s->cfg.bind_addr);
    if (addr.sin_addr.s_addr == INADDR_NONE && strcmp(s->cfg.bind_addr, "255.255.255.255")) {
        snprintf(err, errsz, "invalid IPv4 address '%s'", s->cfg.bind_addr);
        sv_server_free(s);
        return NULL;
    }
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd == BAD_SOCK) { snprintf(err, errsz, "socket() failed"); sv_server_free(s); return NULL; }
    int one = 1;
#ifndef _WIN32
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));   // riavvio rapido sulla stessa porta
#else
    setsockopt(s->listen_fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
#endif
    if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        snprintf(err, errsz, "cannot listen on %s:%d (port in use?)", s->cfg.bind_addr, cfg->port);
        sv_server_free(s);
        return NULL;
    }
    if (listen(s->listen_fd, 64) != 0) { snprintf(err, errsz, "listen() failed"); sv_server_free(s); return NULL; }
    socklen_t al = sizeof(addr);
    getsockname(s->listen_fd, (struct sockaddr*)&addr, &al);
    s->port = ntohs(addr.sin_port);
    if (cfg->mysql) {
        s->mysql_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->mysql_fd == BAD_SOCK) { snprintf(err, errsz, "socket() failed"); sv_server_free(s); return NULL; }
#ifndef _WIN32
        setsockopt(s->mysql_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#else
        setsockopt(s->mysql_fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
#endif
        addr.sin_port = htons((unsigned short)cfg->mysql_port);
        addr.sin_addr.s_addr = inet_addr(s->cfg.bind_addr);
        if (bind(s->mysql_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(s->mysql_fd, 64) != 0) {
            snprintf(err, errsz, "cannot listen on %s:%d for the MySQL protocol (port in use?)", s->cfg.bind_addr, cfg->mysql_port);
            sv_server_free(s);
            return NULL;
        }
        al = sizeof(addr);
        getsockname(s->mysql_fd, (struct sockaddr*)&addr, &al);
        s->mysql_port = ntohs(addr.sin_port);
    }
    return s;
}

int sv_server_mysql_port(const SvServer* s) { return s->mysql_fd != BAD_SOCK ? s->mysql_port : 0; }

const char* sv_server_php(const SvServer* s) { return s->php ? fcgi_pool_describe(s->php) : ""; }

const char* sv_server_api_prefix(const SvServer* s) { return s->api; }

int sv_server_port(const SvServer* s) { return s->port; }

void sv_server_stop(SvServer* s) { s->stop = 1; }

void sv_server_run(SvServer* s) {
    while (!s->stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s->listen_fd, &rd);
        sock_t top = s->listen_fd;
        if (s->mysql_fd != BAD_SOCK) { FD_SET(s->mysql_fd, &rd); if (s->mysql_fd > top) top = s->mysql_fd; }
        struct timeval tv = { 0, 200000 };           // si ricontrolla stop 5 volte al secondo
        int r = select((int)top + 1, &rd, NULL, NULL, &tv);
        // watchdog: una richiesta oltre timeout_ms viene interrotta (le SELECT si fermano subito,
        // le modifiche alla fine dello statement in corso)
        mx_lock(&s->conn_mx);
        if (s->cfg.timeout_ms > 0 && s->exec_since > 0 && !s->timed_out && now_ms() - s->exec_since > s->cfg.timeout_ms) {
            s->timed_out = true;
            db_request_cancel();
        }
        mx_unlock(&s->conn_mx);
        if (r <= 0) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        bool is_mysql = s->mysql_fd != BAD_SOCK && FD_ISSET(s->mysql_fd, &rd);
        sock_t fd = accept(is_mysql ? s->mysql_fd : s->listen_fd, (struct sockaddr*)&peer, &pl);
        if (fd == BAD_SOCK) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        mx_lock(&s->conn_mx);
        int slot = -1;
        for (int i = 0; i < s->cfg.max_clients && slot < 0; i++) if (s->conns[i] == BAD_SOCK) slot = i;
        if (slot >= 0) { s->conns[slot] = fd; s->active++; }
        mx_unlock(&s->conn_mx);
        if (slot < 0 && is_mysql) {
            MwOut o = {0};
            mw_err(&o, 1040, "08004", "Too many connections");
            if (!o.oom) send_all(fd, (const char*)o.p, o.n);
            free(o.p);
            sock_close(fd);
            continue;
        }
        if (slot < 0) {
            Response rs = { .close = true };
            json_error(&rs, 503, "too many connections");
            set_timeout(fd, 1000);
            send_response(s, fd, NULL, &rs);
            free(rs.body.data);
            sock_close(fd);
            continue;
        }
        Conn* c = malloc(sizeof(Conn));
        if (c) {
            c->s = s; c->fd = fd; c->slot = slot; c->mysql = is_mysql;
            const unsigned char* ip = (const unsigned char*)&peer.sin_addr.s_addr;
            snprintf(c->client, sizeof(c->client), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            c->client_port = ntohs(peer.sin_port);
        }
        if (!c || !spawn(c)) {
            mx_lock(&s->conn_mx);
            s->conns[slot] = BAD_SOCK;
            s->active--;
            mx_unlock(&s->conn_mx);
            sock_close(fd);
            free(c);
        }
    }
}

void sv_server_free(SvServer* s) {
    if (!s) return;
    s->stop = 1;
    if (s->listen_fd != BAD_SOCK) sock_close(s->listen_fd);
    if (s->mysql_fd != BAD_SOCK) sock_close(s->mysql_fd);
    // le connessioni aperte vengono interrotte; si aspetta che i thread finiscano
    for (int tries = 0; s->conns; tries++) {
        mx_lock(&s->conn_mx);
        int active = s->active;
        if (tries == 0)
            for (int i = 0; i < s->cfg.max_clients; i++)
                if (s->conns[i] != BAD_SOCK) shutdown(s->conns[i], 2);   // SHUT_RDWR / SD_BOTH
        mx_unlock(&s->conn_mx);
        if (active == 0) break;
        if (tries > 500) return;   // un thread non risponde: meglio non liberare ciò che usa ancora
        sleep_ms(10);
    }
    for (int i = 0; i < SV_MAX_SESSIONS; i++)            // le transazioni rimaste aperte si annullano
        if (s->sess[i].db) { db_session_end(s->sess[i].db); s->sess[i].db = NULL; }
    mx_destroy(&s->conn_mx);
    mx_destroy(&s->exec_mx);
    free(s->conns);
    free(s->token);
    free(s->cors);
    free(s->default_db);
    free(s->static_dir);
    free(s->doc_root);
    for (int i = 0; i < s->n_sites; i++) free(s->sites[i].dir);
    free(s->sites);
    fcgi_pool_free(s->php);
    if (s->audit) fclose(s->audit);
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}

void sv_server_stats(SvServer* s, SvStats* out) {
    mx_lock(&s->conn_mx);
    out->requests    = s->n_requests;
    out->errors      = s->n_errors;
    out->rows_sent   = s->n_rows;
    out->connections = s->active;
    out->busy        = s->exec_since > 0;
    out->uptime_s    = (now_ms() - s->started_ms) / 1000.0;
    mx_unlock(&s->conn_mx);
}

int sv_server_series(SvServer* s, SvBucket* out, int n) {
    if (n > SV_SERIES_MAX) n = SV_SERIES_MAX;
    mx_lock(&s->conn_mx);
    long long now = (long long)((now_ms() - s->started_ms) / 1000.0);
    for (int i = 0; i < n; i++) {
        long long sec = now - (n - 1) + i;
        out[i] = (SvBucket){ .sec = sec };
        if (sec < 0) continue;
        const SvBucket* b = &s->series[sec % SV_SERIES_MAX];
        if (b->sec == sec) out[i] = *b;
    }
    mx_unlock(&s->conn_mx);
    return n;
}

int sv_server_recent(SvServer* s, SvLogEntry* out, int max) {
    mx_lock(&s->conn_mx);
    int n = 0;
    for (long long q = s->n_requests; q >= 1 && n < max && n < SV_LOG_MAX; q--)
        out[n++] = s->log[(q - 1) % SV_LOG_MAX];
    mx_unlock(&s->conn_mx);
    return n;
}

bool sv_random_token(char* out, int bytes) {
    unsigned char raw[64];
    if (bytes <= 0 || bytes > (int)sizeof(raw)) return false;
#ifdef _WIN32
    for (int i = 0; i < bytes; i += 4) {
        unsigned int v;
        if (rand_s(&v) != 0) return false;
        for (int k = 0; k < 4 && i + k < bytes; k++) raw[i + k] = (unsigned char)(v >> (8 * k));
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = fread(raw, 1, (size_t)bytes, f);
    fclose(f);
    if (got != (size_t)bytes) return false;
#endif
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < bytes; i++) { out[2 * i] = hex[raw[i] >> 4]; out[2 * i + 1] = hex[raw[i] & 15]; }
    out[2 * bytes] = '\0';
    return true;
}

bool sv_local_ip(char* out, size_t n) {
    // l'indirizzo con cui questa macchina esce in rete: un socket UDP "connesso" non invia nulla
    sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == BAD_SOCK) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = inet_addr("192.0.2.1");          // TEST-NET: mai raggiunto davvero
    bool ok = connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0;
    socklen_t al = sizeof(a);
    if (ok) ok = getsockname(fd, (struct sockaddr*)&a, &al) == 0 && a.sin_addr.s_addr != 0;
    sock_close(fd);
    if (ok) {
        const unsigned char* ip = (const unsigned char*)&a.sin_addr.s_addr;
        snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    return ok;
}
