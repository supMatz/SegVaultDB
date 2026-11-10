/*
SCOPO: server HTTP/1.1 minimo (socket POSIX / Winsock) e API JSON sopra db_api.h.
       Nessuna dipendenza esterna. Vedi server.h e docs/API.md.
*/
#ifdef _WIN32
  #define _CRT_RAND_S                 // rand_s(): numeri casuali del sistema (token)
#endif
#include "server.h"
#include "json.h"
#include "../bridge/db_api.h"
#include "../widgets/sql_text.h"
#include "../../include/common.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
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

#define API_PREFIX      "/api/v1"
#define MAX_HEADER      (16 * 1024)
#define IDLE_TIMEOUT_MS 15000          // connessione keep-alive inattiva
#define MAX_PARAMS      1024

struct SvServer {
    SvServerConfig cfg;
    char*          token;               // copie delle stringhe della configurazione
    char*          cors;
    char*          default_db;
    char*          static_dir;
    sock_t         listen_fd;
    int            port;
    volatile int   stop;
    sv_mutex       conn_mx;             // conns[] e active
    sock_t*        conns;               // socket aperti (per chiuderli allo stop)
    int            active;
    sv_mutex       exec_mx;             // una richiesta alla volta sul motore
    // statistiche e registro (protetti da conn_mx)
    double         started_ms;
    long long      n_requests, n_errors, n_rows;
    double         exec_since;          // inizio della richiesta che usa il motore (0 = libero)
    bool           timed_out;           // il watchdog ha interrotto la richiesta in corso
    SvLogEntry     log[SV_LOG_MAX];     // anello: log[(seq - 1) % SV_LOG_MAX]
};

typedef struct {
    SvServer* s;
    sock_t    fd;
    int       slot;
    char      client[48];
} Conn;

// ── richiesta / risposta ────────────────────────────────────────

typedef struct {
    char   method[16];
    char   path[2048];                  // decodificato (%xx)
    char   query[2048];                 // grezzo, dopo '?'
    bool   http10;
    bool   keep_alive;
    bool   has_length;
    bool   chunked;
    size_t content_length;
    char   content_type[128];
    char   auth[512];
    char   origin[256];
    char   host[256];
    char*  body;                        // NUL-terminato
    size_t body_len;
} Request;

typedef struct {
    int         status;
    const char* content_type;
    JsonBuf     body;
    const char* extra_headers;          // righe "Nome: valore\r\n" già formattate
    bool        close;
    char        detail[120];            // per il registro: SQL eseguito o errore
    long long   rows;                   // righe restituite
} Response;

static const char* status_text(int st) {
    switch (st) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
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

static bool send_response(SvServer* s, sock_t fd, const Request* rq, Response* rs) {
    char head[1024];
    size_t blen = rs->body.oom ? 0 : rs->body.len;
    int n = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Server: SegVault/" SV_VERSION_STR "\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        rs->status, status_text(rs->status),
        rs->content_type ? rs->content_type : "application/json; charset=utf-8",
        blen, rs->close ? "close" : "keep-alive");
    if (s->cors && n > 0 && (size_t)n < sizeof(head)) {
        // origine ammessa: "*" oppure quella configurata, ripetuta solo se coincide con la richiesta
        const char* allow = !strcmp(s->cors, "*") ? "*" : (rq && !strcmp(rq->origin, s->cors) ? s->cors : NULL);
        if (allow)
            n += snprintf(head + n, sizeof(head) - (size_t)n,
                          "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", allow);
    }
    if (rs->extra_headers && n > 0 && (size_t)n < sizeof(head))
        n += snprintf(head + n, sizeof(head) - (size_t)n, "%s", rs->extra_headers);
    if (n > 0 && (size_t)n < sizeof(head)) n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
    if (n <= 0 || (size_t)n >= sizeof(head)) return false;
    if (!send_all(fd, head, (size_t)n)) return false;
    bool head_only = rq && !strcmp(rq->method, "HEAD");
    return head_only || blen == 0 || send_all(fd, rs->body.data, blen);
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

static char* bind_params(const char* sql, const JsonVal* params, char* err, size_t errsz) {
    JsonBuf o = {0};
    int used = 0, nparams = params ? params->count : 0;
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
        } else if (c == '?') {
            if (used >= nparams) {
                snprintf(err, errsz, "the SQL has more '?' placeholders than params (%d)", nparams);
                free(o.data);
                return NULL;
            }
            if (!append_literal(&o, &params->items[used], err, errsz, used)) { free(o.data); return NULL; }
            used++;
            i++;
        } else {
            while (i < n && sql[i] != '\'' && sql[i] != '"' && sql[i] != '`' && sql[i] != '-' && sql[i] != '/' && sql[i] != '?') i++;
            if (i == st) i++;
            jb_raw(&o, sql + st, i - st);
        }
    }
    if (used != nparams) {
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

static void handle_query(SvServer* s, Request* rq, Response* rs) {
    char err[512];
    JsonVal* root = NULL;
    const char* sql = NULL;
    char dbname[128] = "";
    const JsonVal* params = NULL;
    bool want_tx = false, objects = false;

    char ct[128];
    snprintf(ct, sizeof(ct), "%s", rq->content_type);
    for (char* p = ct; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strncmp(ct, "text/plain", 10) || !strncmp(ct, "application/sql", 15)) {
        // corpo = SQL; database e formato nella query string
        sql = rq->body;
        query_param(rq->query, "database", dbname, sizeof(dbname));
        char f[32];
        if (query_param(rq->query, "rows", f, sizeof(f))) objects = !strcmp(f, "objects");
    } else if (!ct[0] || !strncmp(ct, "application/json", 16)) {
        root = json_parse(rq->body, rq->body_len, err, sizeof(err));
        if (!root) { json_error(rs, 400, "invalid JSON: %s", err); return; }
        if (root->type != JSON_OBJECT) { json_error(rs, 400, "the body must be a JSON object"); goto out; }
        const JsonVal* v = json_get(root, "sql");
        if (!v || v->type != JSON_STRING) { json_error(rs, 400, "missing \"sql\" (string)"); goto out; }
        if (memchr(v->str, '\0', v->len)) { json_error(rs, 400, "\"sql\" cannot contain NUL characters"); goto out; }
        sql = v->str;
        v = json_get(root, "database");
        if (v && v->type == JSON_STRING) snprintf(dbname, sizeof(dbname), "%s", v->str);
        else if (v && v->type != JSON_NULL) { json_error(rs, 400, "\"database\" must be a string"); goto out; }
        params = json_get(root, "params");
        if (params && params->type == JSON_NULL) params = NULL;
        if (params && params->type != JSON_ARRAY) { json_error(rs, 400, "\"params\" must be an array"); goto out; }
        if (params && params->count > MAX_PARAMS) { json_error(rs, 400, "too many params (max %d)", MAX_PARAMS); goto out; }
        v = json_get(root, "transaction");
        want_tx = v && v->type == JSON_BOOL && v->boolean;
        v = json_get(root, "rows");
        if (v && v->type == JSON_STRING) objects = !strcmp(v->str, "objects");
    } else {
        json_error(rs, 415, "use Content-Type application/json or text/plain");
        return;
    }
    if (!dbname[0] && s->default_db) snprintf(dbname, sizeof(dbname), "%s", s->default_db);
    if (dbname[0] && !valid_ident(dbname)) { json_error(rs, 400, "invalid database name"); goto out; }

    char* script = bind_params(sql, params, err, sizeof(err));
    if (!script) { json_error(rs, 400, "%s", err); goto out; }

    int len = (int)strlen(script), cap = 64, nst;
    SqlRange* ranges = NULL;
    for (;;) {                                  // quanti statement: si raddoppia finché bastano
        free(ranges);
        ranges = malloc(sizeof(SqlRange) * (size_t)cap);
        if (!ranges) { free(script); json_error(rs, 500, "out of memory"); goto out; }
        nst = sql_split(script, len, ranges, cap);
        if (nst < cap) break;
        cap *= 2;
    }

    double t0 = now_ms();
    one_line(rs->detail, sizeof(rs->detail), script);
    exec_begin(s);
    // il database corrente si ripristina alla fine: una richiesta non cambia quello delle successive
    char prev_db[128] = "";
    const char* cur = db_current_database();
    if (cur) snprintf(prev_db, sizeof(prev_db), "%s", cur);

    JsonBuf res = {0}, *b = &res;               // i risultati; la risposta si compone alla fine
    bool failed = false, first = true;
    char errmsg[1100] = "";
    int err_stmt = -1, err_line = 0, err_col = 0;
    const char* warning = NULL;

    if (dbname[0]) {
        char use[96];
        snprintf(use, sizeof(use), "USE %s", dbname);
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
    if (in_transaction()) {                     // BEGIN senza COMMIT: la richiesta è un'unità chiusa
        db_rollback();
        warning = failed ? "the open transaction was rolled back"
                         : "a transaction was left open and has been rolled back (end it with COMMIT)";
    }
    cur = db_current_database();
    if (prev_db[0] && (!cur || strcmp(cur, prev_db))) {
        char use[160];
        snprintf(use, sizeof(use), "USE %s", prev_db);
        db_result_free(db_execute(use));        // fallisce solo se il database è stato eliminato
    }
    exec_end(s);

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
    jb_printf(b, ",\"time_ms\":%.3f}", now_ms() - t0);
    rs->status = failed ? 400 : 200;
    if (b->oom) json_error(rs, 500, "result too large for memory");
    free(ranges);
    free(script);
out:
    json_free(root);
}

// ── catalogo: /api/v1/databases[/db/tables[/t]] ─────────────────

static void handle_catalog(SvServer* s, const char* rest, Response* rs) {
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
        for (int i = 0; nl && i < nl->count; i++) { if (i) jb_lit(b, ","); jb_str(b, nl->names[i]); }
        jb_lit(b, "]}");
        db_namelist_free(nl);
    } else if (np == 2 && !strcmp(parts[1], "tables")) {
        NameList* tl = db_list_tables(parts[0]);
        NameList* vl = db_list_views(parts[0]);
        if (!tl) json_error(rs, 404, "database '%s' not found", parts[0]);
        else {
            jb_lit(b, "{\"ok\":true,\"database\":");
            jb_str(b, parts[0]);
            jb_lit(b, ",\"tables\":[");
            for (int i = 0; i < tl->count; i++) { if (i) jb_lit(b, ","); jb_str(b, tl->names[i]); }
            jb_lit(b, "],\"views\":[");
            for (int i = 0; vl && i < vl->count; i++) { if (i) jb_lit(b, ","); jb_str(b, vl->names[i]); }
            jb_lit(b, "]}");
        }
        db_namelist_free(tl);
        db_namelist_free(vl);
    } else if (np == 3 && !strcmp(parts[1], "tables")) {
        int nc = 0;
        ColumnInfo* cols = db_list_columns(parts[0], parts[2], &nc);
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
        { ".css", "text/css; charset=utf-8" }, { ".json", "application/json" },
        { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".ico", "image/x-icon" },
        { ".webp", "image/webp" }, { ".woff2", "font/woff2" }, { ".woff", "font/woff" },
        { ".ttf", "font/ttf" }, { ".txt", "text/plain; charset=utf-8" }, { ".sql", "text/plain; charset=utf-8" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcasecmp(dot, map[i][0])) return map[i][1];
    return "application/octet-stream";
}

static void handle_static(SvServer* s, const Request* rq, Response* rs) {
    const char* p = rq->path;
    // niente "..", backslash o percorsi nascosti: si resta dentro static_dir
    for (const char* q = p; *q; q++) {
        if (*q == '\\' || *q == ':') { json_error(rs, 404, "not found"); return; }
        if (*q == '/' && q[1] == '.') { json_error(rs, 404, "not found"); return; }
    }
    char full[4096];
    size_t pl = strlen(p);
    int n = snprintf(full, sizeof(full), "%s%s%s", s->static_dir, p, (pl == 0 || p[pl - 1] == '/') ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof(full)) { json_error(rs, 404, "not found"); return; }
    struct stat st;
    if (stat(full, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
        if ((size_t)n + 12 >= sizeof(full)) { json_error(rs, 404, "not found"); return; }
        strcat(full, "/index.html");
    }
    FILE* f = fopen(full, "rb");
    if (!f) { json_error(rs, 404, "not found"); return; }
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) jb_raw(&rs->body, chunk, got);
    fclose(f);
    if (rs->body.oom) { json_error(rs, 500, "file too large"); return; }
    rs->status = 200;
    rs->content_type = mime_of(full);
}

// ── instradamento ───────────────────────────────────────────────

// "localhost", "127.x.x.x" o "[::1]", con o senza porta
static bool is_loopback_host(const char* h) {
    char name[256];
    snprintf(name, sizeof(name), "%s", h);
    char* colon = name[0] == '[' ? strchr(name, ']') : strrchr(name, ':');
    if (colon && name[0] == '[') colon[1] = '\0';
    else if (colon) *colon = '\0';
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

static void route(SvServer* s, Request* rq, Response* rs) {
    bool api = !strncmp(rq->path, API_PREFIX, sizeof(API_PREFIX) - 1) &&
               (rq->path[sizeof(API_PREFIX) - 1] == '\0' || rq->path[sizeof(API_PREFIX) - 1] == '/');
    const char* sub = api ? rq->path + sizeof(API_PREFIX) - 1 : NULL;

    const char* bad_origin = api ? origin_problem(s, rq) : NULL;
    if (bad_origin) { json_error(rs, 403, "%s", bad_origin); return; }

    if (!strcmp(rq->method, "OPTIONS")) {        // preflight CORS
        rs->status = 204;
        rs->extra_headers = "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                            "Access-Control-Max-Age: 600\r\n";
        return;
    }
    if (api && !strcmp(sub, "/health")) {
        rs->status = 200;
        jb_lit(&rs->body, "{\"ok\":true,\"server\":\"SegVault\",\"version\":\"" SV_VERSION_STR "\"}");
        return;
    }
    if (api && s->token && strcmp(rq->method, "OPTIONS")) {
        const char* a = rq->auth;
        bool ok = !strncasecmp(a, "Bearer ", 7) && secret_equal(a + 7, s->token);
        if (!ok) {
            rs->extra_headers = "WWW-Authenticate: Bearer\r\n";
            json_error(rs, 401, "missing or wrong token (send \"Authorization: Bearer <token>\")");
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
        if (!strcmp(sub, "/query")) {
            if (strcmp(rq->method, "POST")) { json_error(rs, 405, "use POST"); return; }
            handle_query(s, rq, rs);
            return;
        }
        if (!strncmp(sub, "/databases", 10) && (sub[10] == '\0' || sub[10] == '/')) {
            if (strcmp(rq->method, "GET") && strcmp(rq->method, "HEAD")) { json_error(rs, 405, "use GET"); return; }
            handle_catalog(s, sub + 10, rs);
            return;
        }
        json_error(rs, 404, "unknown endpoint %s", rq->path);
        return;
    }
    if (s->static_dir && (!strcmp(rq->method, "GET") || !strcmp(rq->method, "HEAD"))) { handle_static(s, rq, rs); return; }
    json_error(rs, 404, "not found (the API is under " API_PREFIX ")");
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
            } else if (!strcasecmp(p, "Transfer-Encoding")) rq->chunked = true;
            else if (!strcasecmp(p, "Connection")) {
                if (!strcasecmp(val, "close")) rq->keep_alive = false;
                else if (!strcasecmp(val, "keep-alive")) rq->keep_alive = true;
            }
            else if (!strcasecmp(p, "Content-Type"))  header_value(v, rq->content_type, sizeof(rq->content_type));
            else if (!strcasecmp(p, "Authorization")) header_value(v, rq->auth, sizeof(rq->auth));
            else if (!strcasecmp(p, "Origin"))        header_value(v, rq->origin, sizeof(rq->origin));
            else if (!strcasecmp(p, "Host"))          header_value(v, rq->host, sizeof(rq->host));
        }
        p = e + 2;
    }
    if (rq->chunked) return 411;                 // corpo chunked non supportato: serve Content-Length
    if (!strcmp(rq->method, "POST") && !rq->has_length) return 411;
    return 0;
}

typedef struct {
    char*  data;
    size_t len, cap;
} InBuf;

// legge finché in buf c'è almeno need byte; false su chiusura/timeout
static bool fill(sock_t fd, InBuf* in, size_t need, volatile int* stop) {
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
    snprintf(e->method, sizeof(e->method), "%s", rq->method);
    one_line(e->path, sizeof(e->path), rq->path);
    snprintf(e->client, sizeof(e->client), "%s", client);
    one_line(e->detail, sizeof(e->detail), rs->detail);
    e->status = rs->status;
    e->ms = ms;
    mx_unlock(&s->conn_mx);
}

static void serve_connection(Conn* c) {
    SvServer* s = c->s;
    InBuf in = {0};
    set_timeout(c->fd, IDLE_TIMEOUT_MS);
    for (;;) {
        // intestazione: fino a \r\n\r\n
        char* hend = NULL;
        while (!(hend = in.data ? strstr(in.data, "\r\n\r\n") : NULL)) {
            if (in.len >= MAX_HEADER) {
                Response rs = { .close = true };
                json_error(&rs, 431, "request headers too large");
                send_response(s, c->fd, NULL, &rs);
                free(rs.body.data);
                goto done;
            }
            if (!fill(c->fd, &in, in.len + 1, &s->stop)) goto done;
        }
        size_t head_len = (size_t)(hend - in.data) + 4;
        double t0 = now_ms();
        Request rq;
        memset(&rq, 0, sizeof(rq));
        char saved = in.data[head_len - 2];
        in.data[head_len - 2] = '\0';              // la testata termina con l'ultimo \r\n
        int perr = parse_head(s, in.data, &rq);
        in.data[head_len - 2] = saved;
        Response rs = {0};
        if (perr) {
            rs.close = true;
            json_error(&rs, perr, "%s", perr == 413 ? "request body too large" :
                                        perr == 411 ? "a Content-Length header is required (no chunked bodies)" : "malformed request");
            send_response(s, c->fd, &rq, &rs);
            free(rs.body.data);
            goto done;
        }
        if (!fill(c->fd, &in, head_len + rq.content_length, &s->stop)) goto done;
        char after = in.data[head_len + rq.content_length];
        in.data[head_len + rq.content_length] = '\0';
        rq.body = in.data + head_len;
        rq.body_len = rq.content_length;

        route(s, &rq, &rs);
        rs.close = !rq.keep_alive || s->stop;
        in.data[head_len + rq.content_length] = after;
        bool sent = send_response(s, c->fd, &rq, &rs);
        double ms = now_ms() - t0;
        if (!s->cfg.quiet)
            fprintf(stderr, "[segvault-server] %s %s %s -> %d (%zu bytes, %.1f ms)\n",
                    c->client, rq.method, rq.path, rs.status, rs.body.len, ms);
        if (strcmp(rq.method, "OPTIONS")) log_request(s, c->client, &rq, &rs, ms);
        free(rs.body.data);
        if (!sent || rs.close) goto done;
        // richieste in coda (pipelining): si sposta il resto all'inizio
        size_t used = head_len + rq.content_length;
        memmove(in.data, in.data + used, in.len - used);
        in.len -= used;
        in.data[in.len] = '\0';
    }
done:
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
static DWORD WINAPI conn_thread(LPVOID arg) { serve_connection((Conn*)arg); conn_finish((Conn*)arg); return 0; }
static bool spawn(Conn* c) {
    HANDLE h = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
    if (!h) return false;
    CloseHandle(h);
    return true;
}
#else
static void* conn_thread(void* arg) { serve_connection((Conn*)arg); conn_finish((Conn*)arg); return NULL; }
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
    mx_init(&s->conn_mx);
    mx_init(&s->exec_mx);
    if (!s->conns) { snprintf(err, errsz, "out of memory"); sv_server_free(s); return NULL; }
    for (int i = 0; i < s->cfg.max_clients; i++) s->conns[i] = BAD_SOCK;
    s->started_ms = now_ms();
    if (s->default_db && !valid_ident(s->default_db)) { snprintf(err, errsz, "invalid default database name"); sv_server_free(s); return NULL; }

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
    return s;
}

int sv_server_port(const SvServer* s) { return s->port; }

void sv_server_stop(SvServer* s) { s->stop = 1; }

void sv_server_run(SvServer* s) {
    while (!s->stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s->listen_fd, &rd);
        struct timeval tv = { 0, 200000 };           // si ricontrolla stop 5 volte al secondo
        int r = select((int)s->listen_fd + 1, &rd, NULL, NULL, &tv);
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
        sock_t fd = accept(s->listen_fd, (struct sockaddr*)&peer, &pl);
        if (fd == BAD_SOCK) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        mx_lock(&s->conn_mx);
        int slot = -1;
        for (int i = 0; i < s->cfg.max_clients && slot < 0; i++) if (s->conns[i] == BAD_SOCK) slot = i;
        if (slot >= 0) { s->conns[slot] = fd; s->active++; }
        mx_unlock(&s->conn_mx);
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
            c->s = s; c->fd = fd; c->slot = slot;
            const unsigned char* ip = (const unsigned char*)&peer.sin_addr.s_addr;
            snprintf(c->client, sizeof(c->client), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
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
    mx_destroy(&s->conn_mx);
    mx_destroy(&s->exec_mx);
    free(s->conns);
    free(s->token);
    free(s->cors);
    free(s->default_db);
    free(s->static_dir);
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
