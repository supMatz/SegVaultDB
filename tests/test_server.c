// Test del server HTTP/JSON: richieste reali su socket locali.
//   make test_server && ./test_server
#include "src/server/server.h"
#include "src/server/json.h"
#include "src/bridge/db_api.h"
#include "src/bridge/auth.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0, g_count = 0;

#define CHECK(cond, ...) do { g_count++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int status; char* body; JsonVal* json; } Resp;

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// ── client MySQL minimo (protocollo 4.1, mysql_native_password) ──
typedef struct { int fd; uint8_t seq; } My;

static bool my_rd(My* m, uint8_t** out, size_t* n) {
    uint8_t h[4];
    size_t got = 0;
    while (got < 4) { ssize_t r = recv(m->fd, h + got, 4 - got, 0); if (r <= 0) return false; got += (size_t)r; }
    size_t len = (size_t)h[0] | (size_t)h[1] << 8 | (size_t)h[2] << 16;
    m->seq = (uint8_t)(h[3] + 1);
    uint8_t* b = malloc(len + 1);
    got = 0;
    while (got < len) { ssize_t r = recv(m->fd, b + got, len - got, 0); if (r <= 0) { free(b); return false; } got += (size_t)r; }
    b[len] = 0;
    *out = b; *n = len;
    return true;
}

static bool my_wr(My* m, const uint8_t* p, size_t n) {
    uint8_t h[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), m->seq++ };
    return send(m->fd, h, 4, 0) == 4 && send(m->fd, p, n, 0) == (ssize_t)n;
}

// connessione e login; NULL + messaggio in err se rifiutata
static bool my_connect(My* m, int port, const char* user, const char* pw, const char* db, char* err, size_t en) {
    err[0] = '\0';
    m->fd = connect_to(port);
    uint8_t* p; size_t n;
    if (m->fd < 0 || !my_rd(m, &p, &n)) { snprintf(err, en, "no handshake"); return false; }
    uint8_t scr[20];
    size_t pos = 1 + strlen((char*)p + 1) + 1 + 4;      // protocollo, versione, id
    memcpy(scr, p + pos, 8);
    pos += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10;
    memcpy(scr + 8, p + pos, 12);
    free(p);
    uint8_t a[20], b[20], buf[40], x[20], tokn[20];
    auth_sha1(pw, strlen(pw), a);
    auth_sha1(a, 20, b);
    memcpy(buf, scr, 20); memcpy(buf + 20, b, 20);
    auth_sha1(buf, 40, x);
    for (int i = 0; i < 20; i++) tokn[i] = a[i] ^ x[i];
    uint8_t lg[300];
    size_t k = 0;
    uint32_t caps = 0x200 | 0x8000 | 0x80000 | (db ? 8 : 0) | 0x10000 | 0x20000;
    lg[k++] = (uint8_t)caps; lg[k++] = (uint8_t)(caps >> 8); lg[k++] = (uint8_t)(caps >> 16); lg[k++] = (uint8_t)(caps >> 24);
    memset(lg + k, 0, 4); k += 4;
    lg[k++] = 45;
    memset(lg + k, 0, 23); k += 23;
    memcpy(lg + k, user, strlen(user) + 1); k += strlen(user) + 1;
    lg[k++] = 20; memcpy(lg + k, tokn, 20); k += 20;
    if (db) { memcpy(lg + k, db, strlen(db) + 1); k += strlen(db) + 1; }
    memcpy(lg + k, "mysql_native_password", 22); k += 22;
    if (!my_wr(m, lg, k) || !my_rd(m, &p, &n)) { snprintf(err, en, "no reply to login"); return false; }
    bool ok = p[0] == 0;
    if (!ok && p[0] == 0xff && n > 9) snprintf(err, en, "%u %s", (unsigned)(p[1] | p[2] << 8), (char*)p + 9);
    free(p);
    return ok;
}

static uint64_t my_lenenc(const uint8_t* p, size_t* pos) {
    uint8_t c = p[(*pos)++];
    if (c < 0xfb) return c;
    int k = c == 0xfc ? 2 : c == 0xfd ? 3 : 8;
    uint64_t v = 0;
    for (int i = 0; i < k; i++) v |= (uint64_t)p[*pos + i] << (8 * i);
    *pos += (size_t)k;
    return v;
}

// esegue una query: "OK:n" / "ERR:codice messaggio" / righe "a|b;c|d" (più risultati separati da " / ")
static void my_query(My* m, const char* sql, char* out, size_t on) {
    out[0] = '\0';
    size_t ol = 0;
    uint8_t q[4096];
    q[0] = 3;
    size_t sl = strlen(sql);
    memcpy(q + 1, sql, sl);
    m->seq = 0;
    if (!my_wr(m, q, sl + 1)) { snprintf(out, on, "send failed"); return; }
    for (bool more = true; more; ) {
        more = false;
        uint8_t* p; size_t n;
        if (!my_rd(m, &p, &n)) { snprintf(out + ol, on - ol, "read failed"); return; }
        if (ol) ol += (size_t)snprintf(out + ol, on - ol, " / ");
        if (p[0] == 0) {
            size_t pos = 1;
            uint64_t aff = my_lenenc(p, &pos);
            my_lenenc(p, &pos);
            unsigned st = p[pos] | p[pos + 1] << 8;
            more = (st & 8) != 0;
            ol += (size_t)snprintf(out + ol, on - ol, "OK:%llu", (unsigned long long)aff);
            free(p);
            continue;
        }
        if (p[0] == 0xff) { ol += (size_t)snprintf(out + ol, on - ol, "ERR:%u %s", (unsigned)(p[1] | p[2] << 8), (char*)p + 9); free(p); return; }
        size_t pos = 0;
        int ncols = (int)my_lenenc(p, &pos);
        free(p);
        for (int i = 0; i <= ncols; i++) { if (!my_rd(m, &p, &n)) return; free(p); }   // colonne + EOF
        bool first = true;
        for (;;) {
            if (!my_rd(m, &p, &n)) return;
            if (p[0] == 0xfe && n < 9) { unsigned st = p[3] | p[4] << 8; more = (st & 8) != 0; free(p); break; }
            if (!first) ol += (size_t)snprintf(out + ol, on - ol, ";");
            first = false;
            pos = 0;
            for (int c = 0; c < ncols; c++) {
                if (c) ol += (size_t)snprintf(out + ol, on - ol, "|");
                if (p[pos] == 0xfb) { pos++; ol += (size_t)snprintf(out + ol, on - ol, "NULL"); continue; }
                uint64_t l = my_lenenc(p, &pos);
                ol += (size_t)snprintf(out + ol, on - ol, "%.*s", (int)l, (char*)p + pos);
                pos += (size_t)l;
            }
            free(p);
        }
    }
}

// prepared statement: PREPARE -> id (0 = errore, messaggio in out), numero di parametri e colonne
static uint32_t my_prepare(My* m, const char* sql, int* np, int* nc, char* out, size_t on) {
    uint8_t q[4096];
    q[0] = 0x16;
    size_t sl = strlen(sql);
    memcpy(q + 1, sql, sl);
    m->seq = 0;
    out[0] = '\0';
    uint8_t* p; size_t n;
    if (!my_wr(m, q, sl + 1) || !my_rd(m, &p, &n)) { snprintf(out, on, "io"); return 0; }
    if (p[0] == 0xff) { snprintf(out, on, "ERR:%u %s", (unsigned)(p[1] | p[2] << 8), (char*)p + 9); free(p); return 0; }
    uint32_t id = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    *nc = p[5] | p[6] << 8;
    *np = p[7] | p[8] << 8;
    free(p);
    int skip = (*np ? *np + 1 : 0) + (*nc ? *nc + 1 : 0);   // definizioni + EOF
    for (int i = 0; i < skip; i++) { if (!my_rd(m, &p, &n)) return 0; free(p); }
    return id;
}

// EXECUTE con parametri di testo (NULL = valore NULL) o un intero (vals[i] che inizia con '#');
// risultato come my_query, le colonne di tipo BIGINT lette in binario
static void my_execute(My* m, uint32_t id, int np, const char* const* vals, char* out, size_t on) {
    uint8_t q[4096];
    size_t k = 0;
    q[k++] = 0x17;
    for (int i = 0; i < 4; i++) q[k++] = (uint8_t)(id >> (8 * i));
    q[k++] = 0;
    q[k++] = 1; q[k++] = 0; q[k++] = 0; q[k++] = 0;
    if (np) {
        size_t nb = ((size_t)np + 7) / 8;
        memset(q + k, 0, nb);
        for (int i = 0; i < np; i++) if (!vals[i]) q[k + (size_t)i / 8] |= (uint8_t)(1u << (i % 8));
        k += nb;
        q[k++] = 1;
        for (int i = 0; i < np; i++) { q[k++] = vals[i] && vals[i][0] == '#' ? 8 : 0xfd; q[k++] = 0; }
        for (int i = 0; i < np; i++) {
            if (!vals[i]) continue;
            if (vals[i][0] == '#') { long long v = atoll(vals[i] + 1); for (int b = 0; b < 8; b++) q[k++] = (uint8_t)((unsigned long long)v >> (8 * b)); }
            else { size_t l = strlen(vals[i]); q[k++] = (uint8_t)l; memcpy(q + k, vals[i], l); k += l; }
        }
    }
    m->seq = 0;
    out[0] = '\0';
    uint8_t* p; size_t n;
    if (!my_wr(m, q, k) || !my_rd(m, &p, &n)) { snprintf(out, on, "io"); return; }
    if (p[0] == 0) { size_t pos = 1; snprintf(out, on, "OK:%llu", (unsigned long long)my_lenenc(p, &pos)); free(p); return; }
    if (p[0] == 0xff) { snprintf(out, on, "ERR:%u %s", (unsigned)(p[1] | p[2] << 8), (char*)p + 9); free(p); return; }
    size_t pos = 0;
    int ncols = (int)my_lenenc(p, &pos);
    free(p);
    uint8_t types[32];
    for (int i = 0; i < ncols; i++) {                      // tipo: dopo catalogo, schema, tabelle, nomi, 0x0c, charset, len
        if (!my_rd(m, &p, &n)) return;
        pos = 0;
        for (int f = 0; f < 6; f++) { uint64_t l = my_lenenc(p, &pos); pos += (size_t)l; }
        types[i < 32 ? i : 31] = p[pos + 1 + 2 + 4];
        free(p);
    }
    if (!my_rd(m, &p, &n)) return;
    free(p);
    size_t ol = 0;
    for (bool first = true;; first = false) {
        if (!my_rd(m, &p, &n)) return;
        if (p[0] == 0xfe && n < 9) { free(p); break; }
        if (!first) ol += (size_t)snprintf(out + ol, on - ol, ";");
        size_t nb = ((size_t)ncols + 9) / 8;
        pos = 1 + nb;
        for (int c = 0; c < ncols; c++) {
            if (c) ol += (size_t)snprintf(out + ol, on - ol, "|");
            if (p[1 + (c + 2) / 8] & (1u << ((c + 2) % 8))) { ol += (size_t)snprintf(out + ol, on - ol, "NULL"); continue; }
            if (types[c] == 8) {
                long long v = 0;
                for (int b = 0; b < 8; b++) v |= (long long)p[pos + b] << (8 * b);
                pos += 8;
                ol += (size_t)snprintf(out + ol, on - ol, "%lld", v);
            } else if (types[c] == 5) {
                double d; memcpy(&d, p + pos, 8); pos += 8;
                ol += (size_t)snprintf(out + ol, on - ol, "%g", d);
            } else if (types[c] == 1) {
                ol += (size_t)snprintf(out + ol, on - ol, "%d", p[pos++]);
            } else {
                uint64_t l = my_lenenc(p, &pos);
                ol += (size_t)snprintf(out + ol, on - ol, "%.*s", (int)l, (char*)p + pos);
                pos += (size_t)l;
            }
        }
        free(p);
    }
}

static void send_str(int fd, const char* s, size_t n) {
    while (n > 0) { ssize_t w = send(fd, s, n, MSG_NOSIGNAL); if (w <= 0) return; s += w; n -= (size_t)w; }
}

// legge una risposta (Content-Length) dal socket; buf/len conservano i byte in eccesso
static Resp read_resp(int fd, char** buf, size_t* len, size_t* cap) {
    Resp r = { 0, NULL, NULL };
    for (;;) {
        char* he = *buf ? strstr(*buf, "\r\n\r\n") : NULL;
        if (he) {
            size_t hl = (size_t)(he - *buf) + 4;
            sscanf(*buf, "HTTP/1.1 %d", &r.status);
            char* cl = strcasestr(*buf, "Content-Length:");
            size_t blen = cl && cl < he ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
            while (*len < hl + blen) {
                if (*cap < hl + blen + 1) { *cap = hl + blen + 1; *buf = realloc(*buf, *cap); }
                ssize_t g = recv(fd, *buf + *len, *cap - 1 - *len, 0);
                if (g <= 0) return r;
                *len += (size_t)g;
                (*buf)[*len] = '\0';
            }
            r.body = malloc(blen + 1);
            memcpy(r.body, *buf + hl, blen);
            r.body[blen] = '\0';
            char err[128];
            r.json = blen ? json_parse(r.body, blen, err, sizeof(err)) : NULL;
            memmove(*buf, *buf + hl + blen, *len - hl - blen);
            *len -= hl + blen;
            (*buf)[*len] = '\0';
            return r;
        }
        if (*cap < *len + 4097) { *cap = *len + 8192; *buf = realloc(*buf, *cap); }
        ssize_t g = recv(fd, *buf + *len, *cap - 1 - *len, 0);
        if (g <= 0) return r;
        *len += (size_t)g;
        (*buf)[*len] = '\0';
    }
}

static void resp_free(Resp* r) { free(r->body); json_free(r->json); }

// richiesta su una connessione nuova
static Resp request(int port, const char* method, const char* path, const char* headers, const char* body) {
    int fd = connect_to(port);
    Resp r = { 0, NULL, NULL };
    if (fd < 0) return r;
    char head[4096];
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n%s%s", method, path, headers ? headers : "",
                     body ? "" : "\r\n");
    if (body) n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Length: %zu\r\n\r\n", bl);
    send_str(fd, head, (size_t)n);
    if (body) send_str(fd, body, bl);
    char* buf = NULL; size_t len = 0, cap = 0;
    r = read_resp(fd, &buf, &len, &cap);
    free(buf);
    close(fd);
    return r;
}

static Resp query(int port, const char* json_body) {
    return request(port, "POST", "/api/v1/query", "Content-Type: application/json\r\n", json_body);
}

static bool resp_ok(const Resp* r) {
    const JsonVal* ok = json_get(r->json, "ok");
    return ok && ok->type == JSON_BOOL && ok->boolean;
}

// results[i].rows[row][col] (righe come array)
static const JsonVal* cell_at(const Resp* r, int res, int row, int col) {
    const JsonVal* rs = json_get(r->json, "results");
    if (!rs || rs->type != JSON_ARRAY || res >= rs->count) return NULL;
    const JsonVal* rows = json_get(&rs->items[res], "rows");
    if (!rows || row >= rows->count) return NULL;
    const JsonVal* rw = &rows->items[row];
    if (rw->type == JSON_ARRAY) return col < rw->count ? &rw->items[col] : NULL;
    return col < rw->count ? &rw->items[col] : NULL;
}

static const char* err_msg(const Resp* r) {
    const JsonVal* e = json_get(json_get(r->json, "error"), "message");
    return e && e->type == JSON_STRING ? e->str : "";
}

static void* run_server(void* s) { sv_server_run((SvServer*)s); return NULL; }

// ── concorrenza: più client che scrivono insieme ──
static int g_port;
static volatile int g_waiter_done, g_waiter_count = -1;
// richiesta senza sessione mentre un'altra sessione ha una transazione aperta
static void* count_after_commit(void* arg) {
    (void)arg;
    Resp r = request(g_port, "POST", "/api/v1/query", "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\n",
                     "{\"sql\":\"SELECT COUNT(*) FROM conc WHERE id = 900001\"}");
    const JsonVal* c = cell_at(&r, 0, 0, 0);
    g_waiter_count = c ? (int)c->number : -2;
    resp_free(&r);
    g_waiter_done = 1;
    return NULL;
}
static void* writer(void* arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 25; i++) {
        char b[256];
        snprintf(b, sizeof(b), "{\"database\":\"t\",\"sql\":\"INSERT INTO conc VALUES (?, ?)\",\"params\":[%d,%d]}", id * 1000 + i, id);
        Resp r = query(g_port, b);
        if (!resp_ok(&r)) { g_fail++; printf("FAIL concurrent insert: %s\n", r.body ? r.body : "(no response)"); }
        resp_free(&r);
    }
    return NULL;
}

// uno statement su una connessione MySQL in un thread (transazioni concorrenti)
typedef struct { My* m; const char* sql; char out[512]; } CcArg;
static void* cc_thread(void* p) { CcArg* a = p; char o[512]; my_query(a->m, a->sql, o, sizeof(o)); snprintf(a->out, sizeof(a->out), "%s", o); return NULL; }

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    char dir[] = "/tmp/svserverXXXXXX";
    if (!mkdtemp(dir)) return 1;
    if (!db_init(dir)) { printf("init: %s\n", db_init_error()); return 1; }

    // file statici
    char sdir[600], path[700];
    snprintf(sdir, sizeof(sdir), "%s/www", dir);
    mkdir(sdir, 0755);
    snprintf(path, sizeof(path), "%s/index.html", sdir);
    FILE* f = fopen(path, "w"); fputs("<h1>ciao</h1>", f); fclose(f);

    char err[256];
    SvServerConfig cfg = { .port = 0, .cors_origin = "http://app.local", .static_dir = sdir, .quiet = true, .max_body = 1 << 20 };
    SvServer* s = sv_server_start(&cfg, err, sizeof(err));
    if (!s) { printf("start: %s\n", err); return 1; }
    int port = sv_server_port(s);
    pthread_t th;
    pthread_create(&th, NULL, run_server, s);

    SvServerConfig cfg2 = { .port = 0, .token = "s3cret", .default_db = "t", .quiet = true, .timeout_ms = 150 };
    SvServer* s2 = sv_server_start(&cfg2, err, sizeof(err));
    if (!s2) { printf("start 2: %s\n", err); return 1; }
    int port2 = sv_server_port(s2);
    pthread_t th2;
    pthread_create(&th2, NULL, run_server, s2);

    Resp r;
    // ── health ──
    r = request(port, "GET", "/api/v1/health", NULL, NULL);
    CHECK(r.status == 200 && resp_ok(&r), "health: %d %s", r.status, r.body);
    resp_free(&r);

    // ── schema e dati ──
    r = query(port, "{\"sql\":\"CREATE DATABASE t; USE t; CREATE TABLE p (id INT PRIMARY KEY, nome VARCHAR(60), prezzo FLOAT, ok BOOL);"
                    " CREATE TABLE conc (id INT PRIMARY KEY, w INT)\"}");
    CHECK(r.status == 200 && resp_ok(&r), "create: %s", r.body);
    resp_free(&r);

    // tutti i tipi, stringhe con apici, backslash, a capo e unicode, via parametri
    r = query(port, "{\"database\":\"t\",\"sql\":\"INSERT INTO p VALUES (?, ?, ?, ?), (?, ?, ?, ?)\","
                    "\"params\":[1, \"l'acqua \\\\ \\\"x\\\"\\n\\u00e8 \\ud83d\\ude00\", 1.25, true, 2, null, -3, false]}");
    CHECK(r.status == 200 && resp_ok(&r), "insert params: %s", r.body);
    resp_free(&r);

    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT id, nome, prezzo, ok FROM p ORDER BY id\"}");
    CHECK(r.status == 200 && resp_ok(&r), "select: %s", r.body);
    const JsonVal* c = cell_at(&r, 0, 0, 1);
    CHECK(c && c->type == JSON_STRING && !strcmp(c->str, "l'acqua \\ \"x\"\n\xc3\xa8 \xf0\x9f\x98\x80"), "string roundtrip: %s", r.body);
    c = cell_at(&r, 0, 0, 0);  CHECK(c && c->type == JSON_NUMBER && c->number == 1, "int cell");
    c = cell_at(&r, 0, 0, 2);  CHECK(c && c->type == JSON_NUMBER && c->number == 1.25, "float cell: %s", r.body);
    c = cell_at(&r, 0, 0, 3);  CHECK(c && c->type == JSON_BOOL && c->boolean, "bool cell: %s", r.body);
    c = cell_at(&r, 0, 1, 1);  CHECK(c && c->type == JSON_NULL, "null cell");
    c = cell_at(&r, 0, 1, 2);  CHECK(c && c->type == JSON_NUMBER && c->number == -3, "negative param");
    const JsonVal* cols = json_get(&json_get(r.json, "results")->items[0], "columns");
    CHECK(cols && cols->count == 4 && !strcmp(json_get(&cols->items[1], "name")->str, "nome"), "columns");
    resp_free(&r);

    // righe come oggetti
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT id, nome FROM p WHERE id = 2\",\"rows\":\"objects\"}");
    CHECK(resp_ok(&r) && strstr(r.body, "\"rows\":[{\"id\":2,\"nome\":null}]"), "rows as objects: %s", r.body);
    resp_free(&r);

    // ── parametri: injection, '?' in stringhe e commenti, numero sbagliato ──
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT COUNT(*) FROM p WHERE nome = ?\",\"params\":[\"x'; DROP TABLE p; --\"]}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(resp_ok(&r) && c && c->number == 0, "injection is a plain string: %s", r.body);
    resp_free(&r);
    {
        SvLogEntry last;
        CHECK(sv_server_recent(s, &last, 1) == 1 && strstr(last.detail, "nome = ?") && !strstr(last.detail, "DROP"),
              "log shows the SQL with '?', not the parameter values: %s", last.detail);
    }
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT '?' -- ?\\n, id - ? /* ? */ FROM p WHERE id = 1\",\"params\":[-5]}");
    c = cell_at(&r, 0, 0, 1);
    const JsonVal* c0 = cell_at(&r, 0, 0, 0);
    CHECK(resp_ok(&r) && c && c->number == 6 && c0 && !strcmp(c0->str, "?"), "placeholders outside strings/comments: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT ? , ?\",\"params\":[1]}");
    CHECK(r.status == 400 && !resp_ok(&r) && strstr(err_msg(&r), "placeholders"), "too few params: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT 1\",\"params\":[1]}");
    CHECK(r.status == 400, "too many params: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT ?\",\"params\":[[1]]}");
    CHECK(r.status == 400, "array param rejected: %s", r.body);
    resp_free(&r);

    // ── errori SQL: statement e riga ──
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT 1;\\nSELECT * FROM nonesiste\"}");
    const JsonVal* st = json_get(json_get(r.json, "error"), "statement");
    const JsonVal* ln = json_get(json_get(r.json, "error"), "line");
    CHECK(r.status == 400 && st && st->number == 1 && ln && ln->number == 2 && json_get(r.json, "results")->count == 1,
          "sql error position: %s", r.body);
    resp_free(&r);

    // ── transazioni ──
    r = query(port, "{\"database\":\"t\",\"transaction\":true,\"sql\":\"INSERT INTO p VALUES (3,'a',1,TRUE); INSERT INTO p VALUES (3,'b',1,TRUE)\"}");
    CHECK(r.status == 400, "tx duplicate: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT COUNT(*) FROM p\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(c && c->number == 2, "transaction:true rolled back everything: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"BEGIN; DELETE FROM p\"}");
    CHECK(resp_ok(&r) && json_get(r.json, "warning"), "open tx warning: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT COUNT(*) FROM p\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(c && c->number == 2, "open transaction rolled back: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"transaction\":true,\"sql\":\"INSERT INTO p VALUES (3,'c',2,TRUE); UPDATE p SET prezzo = prezzo * 2 WHERE id = 3\"}");
    CHECK(resp_ok(&r), "tx commit: %s", r.body);
    resp_free(&r);

    // regressione: dopo 64 transazioni "transaction": true veniva rifiutato per sempre
    {
        int ok_n = 0;
        for (int i = 0; i < 100; i++) {
            r = query(port, "{\"database\":\"t\",\"transaction\":true,\"sql\":\"UPDATE p SET prezzo = prezzo + 0 WHERE id = 3\"}");
            if (resp_ok(&r)) ok_n++;
            resp_free(&r);
        }
        CHECK(ok_n == 100, "100 transactions in a row: %d ok", ok_n);
    }

    // testo lungo come parametro (oltre i vecchi 511 byte), con apici e backslash
    {
        char* big = malloc(5000);
        int k = sprintf(big, "{\"database\":\"t\",\"sql\":\"CREATE TABLE lungo (s TEXT); INSERT INTO lungo VALUES (?); "
                             "SELECT LENGTH(s), RIGHT(s, 3) FROM lungo\",\"params\":[\"");
        for (int i = 0; i < 2000; i++) big[k++] = "ab'\\"[i % 3 == 2 ? (i % 2 ? 2 : 3) : i % 2] == '\\' ? 'x' : "ab'"[i % 3];
        k += sprintf(big + k, "end\"]}");
        r = query(port, big);
        c = cell_at(&r, 2, 0, 0);
        const JsonVal* c2 = cell_at(&r, 2, 0, 1);
        CHECK(resp_ok(&r) && c && c->number == 2003 && c2 && !strcmp(c2->str, "end"), "long text param: %.300s", r.body);
        resp_free(&r);
        free(big);
    }

    // id generato da AUTO_INCREMENT e valori di default nel catalogo
    r = query(port, "{\"database\":\"t\",\"sql\":\"CREATE TABLE gen (id INT PRIMARY KEY AUTO_INCREMENT, v INT DEFAULT 7); "
                    "INSERT INTO gen (v) VALUES (1), (2); INSERT INTO gen (v) VALUES (3)\"}");
    {
        const JsonVal* rs1 = json_get(r.json, "results");
        const JsonVal* li1 = rs1 && rs1->count == 3 ? json_get(&rs1->items[1], "last_insert_id") : NULL;
        const JsonVal* li2 = rs1 && rs1->count == 3 ? json_get(&rs1->items[2], "last_insert_id") : NULL;
        CHECK(li1 && li1->number == 2 && li2 && li2->number == 3, "last_insert_id: %s", r.body);
    }
    resp_free(&r);
    r = request(port, "GET", "/api/v1/databases/t/tables/gen", NULL, NULL);
    CHECK(r.body && strstr(r.body, "\"default\":\"7\""), "column default: %s", r.body);
    resp_free(&r);

    // ── endpoint salvati ──
    r = query(port, "{\"database\":\"t\",\"sql\":\"CREATE TABLE api_endpoints (name VARCHAR(60) PRIMARY KEY, sql TEXT NOT NULL, "
                    "access VARCHAR(10) NOT NULL DEFAULT 'token', description VARCHAR(200)); "
                    "INSERT INTO api_endpoints VALUES ('economici', 'SELECT id, nome FROM p WHERE prezzo < :max ORDER BY id', 'public', 'Cheap products'), "
                    "('per_nome', 'SELECT id FROM p WHERE nome = :nome', 'public', NULL), "
                    "('aggiungi', 'INSERT INTO p (id, nome, prezzo, ok) VALUES (:id, :nome, :prezzo, TRUE); SELECT COUNT(*) AS n FROM p', 'token', NULL), "
                    "('rotto', 'INSERT INTO p (id, nome, prezzo, ok) VALUES (90, ''a'', 1, TRUE); INSERT INTO p (id, nome, prezzo, ok) VALUES (90, ''b'', 1, TRUE)', 'public', NULL)\"}");
    CHECK(resp_ok(&r), "create endpoints: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/endpoints/economici?database=t&max=0", NULL, NULL);
    CHECK(resp_ok(&r) && strstr(r.body, "\"rows\":[{\"id\":2,\"nome\":null}]"), "public endpoint, GET param: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/endpoints/economici?database=t", NULL, NULL);
    CHECK(r.status == 400 && strstr(err_msg(&r), "missing parameter :max"), "missing param: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/endpoints/per_nome?database=t&nome=x%27%20OR%20%271%27%3D%271", NULL, NULL);
    CHECK(resp_ok(&r) && strstr(r.body, "\"rows\":[]"), "injection through an endpoint parameter: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/endpoints/nope?database=t", NULL, NULL);
    CHECK(r.status == 404, "unknown endpoint: %d", r.status);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/endpoints/rotto?database=t", NULL, NULL);
    CHECK(r.status == 400, "failing endpoint: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT COUNT(*) FROM p WHERE id = 90\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(c && c->number == 0, "endpoint is atomic: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/endpoints?database=t", NULL, NULL);
    CHECK(resp_ok(&r) && strstr(r.body, "\"name\":\"aggiungi\"") && strstr(r.body, "\"access\":\"public\""), "endpoint list: %s", r.body);
    resp_free(&r);
    // con il token (secondo server): i pubblici senza token e da qualsiasi origine, gli altri no
    r = request(port2, "GET", "/api/v1/endpoints/economici?max=2", "Origin: http://altro.sito\r\n", NULL);
    CHECK(r.status == 200, "public endpoint without token, foreign origin: %d %s", r.status, r.body);
    resp_free(&r);
    {
        int fd = connect_to(port2);
        const char* q = "GET /api/v1/endpoints/economici?max=2 HTTP/1.1\r\nHost: localhost\r\nOrigin: http://altro.sito\r\nConnection: close\r\n\r\n";
        send_str(fd, q, strlen(q));
        char b[2048]; size_t got = 0; ssize_t g;
        while (got < sizeof(b) - 1 && (g = recv(fd, b + got, sizeof(b) - 1 - got, 0)) > 0) got += (size_t)g;
        b[got] = '\0';
        CHECK(strstr(b, "Access-Control-Allow-Origin: *"), "public endpoint CORS header: %s", b);
        close(fd);
    }
    r = request(port2, "POST", "/api/v1/endpoints/aggiungi", "Content-Type: application/json\r\n", "{\"id\":70,\"nome\":\"x\",\"prezzo\":1}");
    CHECK(r.status == 401, "token endpoint without token: %d", r.status);
    resp_free(&r);
    r = request(port2, "POST", "/api/v1/endpoints/aggiungi", "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\n",
                "{\"id\":70,\"nome\":\"l'olio\",\"prezzo\":4.5}");
    CHECK(resp_ok(&r) && strstr(r.body, "\"n\":"), "token endpoint, JSON params: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT nome FROM p WHERE id = 70; DELETE FROM p WHERE id = 70\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(c && c->type == JSON_STRING && !strcmp(c->str, "l'olio"), "value bound by the endpoint: %s", r.body);
    resp_free(&r);

    // ── corpo text/plain ──
    r = request(port, "POST", "/api/v1/query?database=t", "Content-Type: text/plain\r\n", "SELECT prezzo FROM p WHERE id = 3");
    c = cell_at(&r, 0, 0, 0);
    CHECK(resp_ok(&r) && c && c->number == 4, "text/plain: %s", r.body);
    resp_free(&r);

    // ── catalogo ──
    r = request(port, "GET", "/api/v1/databases", NULL, NULL);
    CHECK(resp_ok(&r) && strstr(r.body, "\"t\""), "databases: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/databases/t/tables", NULL, NULL);
    CHECK(resp_ok(&r) && strstr(r.body, "\"conc\""), "tables: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/databases/t/tables/p", NULL, NULL);
    CHECK(resp_ok(&r) && strstr(r.body, "\"primary_key\":true") && strstr(r.body, "CREATE TABLE"), "table: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/databases/t/tables/nope", NULL, NULL);
    CHECK(r.status == 404, "missing table: %d", r.status);
    resp_free(&r);

    // ── errori HTTP ──
    r = query(port, "{\"sql\": ");
    CHECK(r.status == 400 && strstr(err_msg(&r), "invalid JSON"), "bad json: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t\"}");
    CHECK(r.status == 400, "missing sql: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"database\":\"t; DROP\",\"sql\":\"SELECT 1\"}");
    CHECK(r.status == 400, "bad database name: %s", r.body);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/query", NULL, NULL);
    CHECK(r.status == 405, "GET query: %d", r.status);
    resp_free(&r);
    r = request(port, "GET", "/api/v1/nope", NULL, NULL);
    CHECK(r.status == 404, "unknown endpoint: %d", r.status);
    resp_free(&r);
    r = request(port, "POST", "/api/v1/query", "Content-Type: image/png\r\n", "x");
    CHECK(r.status == 415, "content type: %d", r.status);
    resp_free(&r);
    r = request(port, "POST", "/api/v1/query", "Content-Type: application/json\r\n", NULL);
    CHECK(r.status == 411, "no content-length: %d", r.status);
    resp_free(&r);
    {
        char big[256];
        int fd = connect_to(port);
        int n = snprintf(big, sizeof(big), "POST /api/v1/query HTTP/1.1\r\nContent-Length: %d\r\n\r\n", 2 << 20);
        send_str(fd, big, (size_t)n);
        char* buf = NULL; size_t len = 0, cap = 0;
        r = read_resp(fd, &buf, &len, &cap);
        CHECK(r.status == 413, "body too large: %d", r.status);
        resp_free(&r); free(buf); close(fd);
    }

    // ── CORS ──
    r = request(port, "OPTIONS", "/api/v1/query", "Origin: http://app.local\r\n", NULL);
    CHECK(r.status == 204, "preflight: %d", r.status);
    resp_free(&r);
    {
        int fd = connect_to(port);
        const char* q = "GET /api/v1/health HTTP/1.1\r\nOrigin: http://app.local\r\nConnection: close\r\n\r\n";
        send_str(fd, q, strlen(q));
        char b[2048]; size_t got = 0; ssize_t g;
        while (got < sizeof(b) - 1 && (g = recv(fd, b + got, sizeof(b) - 1 - got, 0)) > 0) got += (size_t)g;
        b[got] = '\0';
        CHECK(strstr(b, "Access-Control-Allow-Origin: http://app.local"), "cors header: %s", b);
        close(fd);
    }

    // ── siti non ammessi: niente esecuzione (CSRF con text/plain, DNS rebinding) ──
    r = request(port, "POST", "/api/v1/query?database=t", "Content-Type: text/plain\r\nOrigin: http://evil.example\r\n",
                "DELETE FROM p");
    CHECK(r.status == 403, "foreign origin rejected: %d %s", r.status, r.body);
    resp_free(&r);
    {
        int fd = connect_to(port);
        const char* q = "POST /api/v1/query?database=t HTTP/1.1\r\nHost: evil.example:8484\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 13\r\nConnection: close\r\n\r\nDELETE FROM p";
        send_str(fd, q, strlen(q));
        char* buf = NULL; size_t len = 0, cap = 0;
        r = read_resp(fd, &buf, &len, &cap);
        CHECK(r.status == 403, "rebinding host rejected: %d", r.status);
        resp_free(&r); free(buf); close(fd);
    }
    {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "Origin: http://localhost:%d\r\nContent-Type: application/json\r\n", port);
        int fd = connect_to(port);
        char q[512];
        const char* body = "{\"database\":\"t\",\"sql\":\"SELECT COUNT(*) FROM p\"}";
        snprintf(q, sizeof(q), "POST /api/v1/query HTTP/1.1\r\nHost: localhost:%d\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                 port, hdr, strlen(body), body);
        send_str(fd, q, strlen(q));
        char* buf = NULL; size_t len = 0, cap = 0;
        r = read_resp(fd, &buf, &len, &cap);
        c = cell_at(&r, 0, 0, 0);
        CHECK(r.status == 200 && c && c->number == 3, "same origin allowed, nothing deleted: %d %s", r.status, r.body);
        resp_free(&r); free(buf); close(fd);
    }

    // ── keep-alive e pipelining: tre richieste in un solo invio ──
    {
        int fd = connect_to(port);
        const char* body = "{\"sql\":\"SELECT 7\"}";
        char one[512], all[2048] = "";
        snprintf(one, sizeof(one), "POST /api/v1/query HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
        for (int i = 0; i < 3; i++) strcat(all, one);
        send_str(fd, all, strlen(all));
        char* buf = NULL; size_t len = 0, cap = 0;
        for (int i = 0; i < 3; i++) {
            r = read_resp(fd, &buf, &len, &cap);
            c = cell_at(&r, 0, 0, 0);
            CHECK(r.status == 200 && c && c->number == 7, "pipelined %d: %s", i, r.body);
            resp_free(&r);
        }
        free(buf);
        close(fd);
    }

    // ── file statici ──
    r = request(port, "GET", "/", NULL, NULL);
    CHECK(r.status == 200 && r.body && !strcmp(r.body, "<h1>ciao</h1>"), "static index: %d", r.status);
    resp_free(&r);
    r = request(port, "GET", "/../../etc/passwd", NULL, NULL);
    CHECK(r.status == 404, "path traversal blocked: %d", r.status);
    resp_free(&r);
    r = request(port, "GET", "/%2e%2e/%2e%2e/etc/passwd", NULL, NULL);
    CHECK(r.status == 404, "encoded traversal blocked: %d", r.status);
    resp_free(&r);

    // ── una richiesta non cambia il database corrente delle successive ──
    r = query(port, "{\"sql\":\"CREATE DATABASE t2; USE t2\"}");
    CHECK(resp_ok(&r), "create t2: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"sql\":\"SELECT COUNT(*) FROM p\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(resp_ok(&r) && c && c->number == 3, "current database restored after USE: %s", r.body);
    resp_free(&r);

    // ── stato e registro ──
    r = request(port, "GET", "/api/v1/status", NULL, NULL);
    const JsonVal* nreq = json_get(r.json, "requests");
    CHECK(resp_ok(&r) && nreq && nreq->number > 10, "status: %s", r.body);
    resp_free(&r);
    {
        SvLogEntry log[3];
        int n = sv_server_recent(s, log, 3);
        CHECK(n == 3 && !strcmp(log[0].path, "/api/v1/status") && strstr(log[1].detail, "SELECT COUNT(*) FROM p")
              && !strcmp(log[0].client, "127.0.0.1") && log[0].seq == log[1].seq + 1, "recent log: %d %s %s", n, log[0].path, log[1].detail);
        SvStats st;
        sv_server_stats(s, &st);
        CHECK(st.requests >= 30 && st.errors >= 10 && st.rows_sent > 0, "stats: %lld %lld %lld", st.requests, st.errors, st.rows_sent);
    }

    // ── token e database di default (secondo server) ──
    r = request(port2, "GET", "/api/v1/health", NULL, NULL);
    CHECK(r.status == 200, "health without token: %d", r.status);
    resp_free(&r);
    r = request(port2, "POST", "/api/v1/query", "Content-Type: application/json\r\n", "{\"sql\":\"SELECT 1\"}");
    CHECK(r.status == 401, "no token: %d", r.status);
    resp_free(&r);
    r = request(port2, "POST", "/api/v1/query", "Content-Type: application/json\r\nAuthorization: Bearer wrong\r\n", "{\"sql\":\"SELECT 1\"}");
    CHECK(r.status == 401, "wrong token: %d", r.status);
    resp_free(&r);
    r = request(port2, "POST", "/api/v1/query", "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\n", "{\"sql\":\"SELECT COUNT(*) FROM p\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(r.status == 200 && c && c->number == 3, "token + default database: %s", r.body);
    resp_free(&r);

    // ── concorrenza ──
    g_port = port;
    pthread_t w[8];
    for (long i = 0; i < 8; i++) pthread_create(&w[i], NULL, writer, (void*)i);
    for (int i = 0; i < 8; i++) pthread_join(w[i], NULL);
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT COUNT(*) FROM conc\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(c && c->number == 200, "concurrent inserts: %s", r.body);
    resp_free(&r);

    // ── timeout: una query troppo lunga viene fermata e il server resta utilizzabile ──
    r = request(port2, "POST", "/api/v1/query", "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\n",
                "{\"sql\":\"CREATE TABLE big (id INT, w INT); INSERT INTO big SELECT a.id * 10000 + b.id, a.w FROM conc a, conc b;"
                " SELECT COUNT(*) FROM big a WHERE (SELECT COUNT(*) FROM big b WHERE b.w > a.w + b.id % 7) > 0\"}");
    CHECK(r.status == 400 && strstr(err_msg(&r), "longer than"), "timeout: %s", r.body);
    resp_free(&r);
    r = request(port2, "POST", "/api/v1/query", "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\n", "{\"sql\":\"SELECT 5\"}");
    c = cell_at(&r, 0, 0, 0);
    CHECK(resp_ok(&r) && c && c->number == 5, "after timeout: %s", r.body);
    resp_free(&r);

    // ── risultato grande: JSON valido ──
    r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT a.id, b.id, a.w FROM conc a, conc b\"}");
    const JsonVal* rs0 = json_get(r.json, "results");
    const JsonVal* rc = rs0 && rs0->count ? json_get(&rs0->items[0], "row_count") : NULL;
    CHECK(r.json && rc && rc->number == 40000, "large result parses: status %d", r.status);
    resp_free(&r);

    // ── sessioni: stato e transazioni che durano più richieste; gli altri aspettano il COMMIT ──
    {
        r = request(port2, "POST", "/api/v1/sessions", "Authorization: Bearer s3cret\r\n", "");
        const JsonVal* sid = json_get(r.json, "session");
        char id[64] = "";
        if (sid && sid->str) snprintf(id, sizeof(id), "%s", sid->str);
        CHECK(r.status == 200 && strlen(id) == 32, "new session: %s", r.body);
        resp_free(&r);
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\nX-SegVault-Session: %s\r\n", id);
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"BEGIN; INSERT INTO conc VALUES (900001, 1)\"}");
        CHECK(resp_ok(&r) && strstr(r.body, "\"in_transaction\":true"), "begin in session: %s", r.body);
        resp_free(&r);
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT COUNT(*) FROM conc WHERE id = 900001\"}");
        c = cell_at(&r, 0, 0, 0);
        CHECK(resp_ok(&r) && c && c->number == 1, "own uncommitted row visible in the session: %s", r.body);
        resp_free(&r);
        // una richiesta senza sessione aspetta il COMMIT, poi vede la riga
        g_port = port2;
        pthread_t waiter;
        pthread_create(&waiter, NULL, count_after_commit, NULL);
        struct timespec ts = { 0, 300 * 1000000L };
        nanosleep(&ts, NULL);
        CHECK(!g_waiter_done, "other request waits while the session holds a transaction");
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"COMMIT\"}");
        CHECK(resp_ok(&r) && strstr(r.body, "\"in_transaction\":false"), "commit in a later request: %s", r.body);
        resp_free(&r);
        pthread_join(waiter, NULL);
        CHECK(g_waiter_count == 1, "the waiting request ran after the commit and saw the row: %d", g_waiter_count);
        // il database resta impostato nella sessione; chiusa con una transazione aperta = rollback
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"BEGIN; DELETE FROM conc\"}");
        resp_free(&r);
        char path[128];
        snprintf(path, sizeof(path), "/api/v1/sessions/%s", id);
        r = request(port2, "DELETE", path, "Authorization: Bearer s3cret\r\n", NULL);
        CHECK(r.status == 200, "delete session: %d %s", r.status, r.body);
        resp_free(&r);
        r = request(port2, "POST", "/api/v1/query", "Content-Type: application/json\r\nAuthorization: Bearer s3cret\r\n",
                    "{\"sql\":\"SELECT COUNT(*) FROM conc WHERE id = 900001\"}");
        c = cell_at(&r, 0, 0, 0);
        CHECK(resp_ok(&r) && c && c->number == 1, "closing the session rolled back its delete: %s", r.body);
        resp_free(&r);
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT 1\"}");
        CHECK(r.status == 404, "closed session: %d", r.status);
        resp_free(&r);
    }

    // ── utenti (terzo server con --users): login Basic, permessi dell'utente, token = accesso completo ──
    {
        QueryResult* q;
        q = db_execute("CREATE USER anna IDENTIFIED BY 'anna-pass'"); db_result_free(q);
        q = db_execute("GRANT SELECT ON t.p TO anna"); db_result_free(q);
        q = db_execute("CREATE DATABASE t2"); db_result_free(q);
        q = db_execute("USE t"); db_result_free(q);
        q = db_execute("CREATE USER boss IDENTIFIED BY 'boss-pass'"); db_result_free(q);
        q = db_execute("GRANT ALL ON *.* TO boss WITH GRANT OPTION"); db_result_free(q);
        remove("/tmp/sv_audit_test.log");
        SvServerConfig cfg3 = { .port = 0, .token = "tok3", .default_db = "t", .quiet = true, .require_login = true,
                                .audit_file = "/tmp/sv_audit_test.log" };
        SvServer* s3 = sv_server_start(&cfg3, err, sizeof(err));
        int port3 = s3 ? sv_server_port(s3) : 0;
        pthread_t th3;
        if (s3) pthread_create(&th3, NULL, run_server, s3);
        const char* J = "Content-Type: application/json\r\n";
        char hdr[256];
        r = request(port3, "POST", "/api/v1/query", J, "{\"sql\":\"SELECT 1\"}");
        CHECK(r.status == 401 && strstr(r.body, "Basic"), "login required: %d %s", r.status, r.body);
        resp_free(&r);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic YW5uYTp3cm9uZw==\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT 1\"}");
        CHECK(r.status == 401 && strstr(r.body, "wrong user or password"), "wrong password: %d %s", r.status, r.body);
        resp_free(&r);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic YW5uYTphbm5hLXBhc3M=\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT COUNT(*) FROM p\"}");
        c = cell_at(&r, 0, 0, 0);
        CHECK(resp_ok(&r) && c && c->number == 3, "anna reads p: %s", r.body);
        resp_free(&r);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"DELETE FROM p\"}");
        CHECK(!resp_ok(&r) && strstr(err_msg(&r), "Access denied"), "anna cannot delete: %s", r.body);
        resp_free(&r);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT * FROM conc\"}");
        CHECK(!resp_ok(&r) && strstr(err_msg(&r), "Access denied"), "anna cannot read conc: %s", r.body);
        resp_free(&r);
        // catalogo: solo ciò su cui anna ha permessi
        r = request(port3, "GET", "/api/v1/databases/t/tables", hdr, NULL);
        CHECK(resp_ok(&r) && strstr(r.body, "\"p\"") && !strstr(r.body, "\"conc\""), "anna sees only p: %s", r.body);
        resp_free(&r);
        r = request(port3, "GET", "/api/v1/databases/t/tables/conc", hdr, NULL);
        CHECK(r.status == 404, "anna cannot describe conc: %d %s", r.status, r.body);
        resp_free(&r);
        r = request(port3, "GET", "/api/v1/databases", hdr, NULL);
        CHECK(resp_ok(&r) && strstr(r.body, "\"t\"") && !strstr(r.body, "\"t2\""), "databases filtered: %s", r.body);
        resp_free(&r);
        // di nuovo (credenziali in cache): stesso esito, e la sessione del motore torna root dopo la richiesta
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT COUNT(*) FROM p\"}");
        CHECK(resp_ok(&r), "cached login: %s", r.body);
        resp_free(&r);
        CHECK(!db_session_user()[0], "engine back to root after the request");
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic Ym9zczpib3NzLXBhc3M=\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT COUNT(*) FROM conc\"}");
        CHECK(resp_ok(&r), "admin reads anything: %s", r.body);
        resp_free(&r);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Bearer tok3\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT COUNT(*) FROM conc\"}");
        CHECK(resp_ok(&r), "token = full access: %s", r.body);
        resp_free(&r);
        // utente eliminato: subito fuori, anche con il login in cache
        q = db_execute("DROP USER anna"); db_result_free(q);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic YW5uYTphbm5hLXBhc3M=\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT COUNT(*) FROM p\"}");
        CHECK(r.status == 401, "dropped user: %d %s", r.status, r.body);
        resp_free(&r);
        if (s3) { sv_server_stop(s3); pthread_join(th3, NULL); sv_server_free(s3); }
        // registro di audit: le modifiche (anche rifiutate) con utente e client, non le letture
        FILE* af = fopen("/tmp/sv_audit_test.log", "r");
        char audit[8192] = "";
        size_t an = af ? fread(audit, 1, sizeof(audit) - 1, af) : 0;
        audit[an] = '\0';
        if (af) fclose(af);
        CHECK(strstr(audit, "127.0.0.1  anna  t  ERROR Access denied") && strstr(audit, "DELETE FROM p"), "audit: denied delete logged: %s", audit);
        CHECK(!strstr(audit, "SELECT"), "audit: reads not logged: %s", audit);
        remove("/tmp/sv_audit_test.log");
    }

    // ── protocollo MySQL (quarto server: --mysql-port con utenti) ──
    {
        QueryResult* q;
        q = db_execute("CREATE USER mia IDENTIFIED BY 'mia-pass'"); db_result_free(q);
        q = db_execute("GRANT SELECT, INSERT ON t.* TO mia"); db_result_free(q);
        SvServerConfig cfg4 = { .port = 0, .token = "tok4", .quiet = true, .require_login = true, .mysql = true, .mysql_port = 0 };
        SvServer* s4 = sv_server_start(&cfg4, err, sizeof(err));
        int mp = s4 ? sv_server_mysql_port(s4) : 0;
        pthread_t th4;
        if (s4) pthread_create(&th4, NULL, run_server, s4);
        CHECK(mp > 0, "mysql port: %s", err);
        My m;
        char e[300], out[4096];
        CHECK(!my_connect(&m, mp, "mia", "sbagliata", NULL, e, sizeof(e)) && strstr(e, "1045"), "wrong password refused: %s", e);
        close(m.fd);
        CHECK(my_connect(&m, mp, "mia", "mia-pass", "t", e, sizeof(e)), "login: %s", e);
        my_query(&m, "SELECT COUNT(*) FROM p", out, sizeof(out));
        CHECK(!strcmp(out, "3"), "select: %s", out);
        my_query(&m, "SELECT DATABASE(), @@version", out, sizeof(out));
        CHECK(!strncmp(out, "ERR", 3) || strstr(out, "t|"), "database(): %s", out);
        my_query(&m, "SELECT @@version_comment LIMIT 1", out, sizeof(out));
        CHECK(!strcmp(out, "SegVault"), "client variable: %s", out);
        my_query(&m, "SET NAMES utf8mb4", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0"), "set names: %s", out);
        my_query(&m, "DELETE FROM p", out, sizeof(out));
        CHECK(strstr(out, "ERR:1142"), "privileges enforced: %s", out);
        my_query(&m, "SELECT * FROM nessuna", out, sizeof(out));
        CHECK(strstr(out, "ERR:1146"), "missing table code: %s", out);
        my_query(&m, "SELECT 1; SELECT 2, NULL", out, sizeof(out));
        CHECK(!strcmp(out, "1 / 2|NULL"), "multi statements: %s", out);
        close(m.fd);
        CHECK(my_connect(&m, mp, "root", "tok4", NULL, e, sizeof(e)), "root with the token: %s", e);
        my_query(&m, "USE t", out, sizeof(out));
        my_query(&m, "BEGIN; DELETE FROM p WHERE id = 1; SELECT COUNT(*) FROM p; ROLLBACK", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0 / OK:1 / 2 / OK:0"), "transaction: %s", out);
        my_query(&m, "SELECT COUNT(*) FROM p", out, sizeof(out));
        CHECK(!strcmp(out, "3"), "rolled back: %s", out);

        // prepared statement lato server (mysqli->prepare, JDBC, mysql-connector prepared=True)
        int np = -1, nc = -1;
        uint32_t sid = my_prepare(&m, "SELECT id, nome FROM p WHERE id >= ? AND nome <> ? ORDER BY id", &np, &nc, out, sizeof(out));
        CHECK(sid && np == 2 && nc == 2, "prepare select: id %u params %d cols %d %s", sid, np, nc, out);
        const char* v1[] = { "#1", "x' OR '1'='1" };
        my_execute(&m, sid, 2, v1, out, sizeof(out));
        CHECK(!strncmp(out, "1|", 2) && strchr(out, ';'), "execute select (binary rows, quoted text): %s", out);
        const char* v2[] = { "#3", "zzz" };
        my_execute(&m, sid, 2, v2, out, sizeof(out));
        CHECK(!strncmp(out, "3|", 2) && !strchr(out, ';'), "same statement, other values: %s", out);
        uint32_t ins = my_prepare(&m, "INSERT INTO p (id, nome, prezzo) VALUES (?, ?, ?)", &np, &nc, out, sizeof(out));
        CHECK(ins && np == 3 && nc == 0, "prepare insert: %u %d %d %s", ins, np, nc, out);
        const char* v3[] = { "#40", "it's \\ ok -- not a comment", NULL };
        my_execute(&m, ins, 3, v3, out, sizeof(out));
        CHECK(!strcmp(out, "OK:1"), "execute insert: %s", out);
        my_query(&m, "SELECT nome, prezzo FROM p WHERE id = 40", out, sizeof(out));
        CHECK(!strcmp(out, "it's \\ ok -- not a comment|NULL"), "inserted values kept as data: %s", out);
        my_execute(&m, ins, 3, v3, out, sizeof(out));
        CHECK(strstr(out, "ERR:1062"), "duplicate through a prepared insert: %s", out);
        uint32_t lit = my_prepare(&m, "SELECT '?' AS q, ? AS v -- ?", &np, &nc, out, sizeof(out));
        CHECK(lit && np == 1, "'?' inside strings and comments is not a parameter: %d", np);
        CHECK(!my_prepare(&m, "SELECT * FROM nessuna WHERE id = ?", &np, &nc, out, sizeof(out)) && strstr(out, "ERR:1146"),
              "prepare reports a missing table: %s", out);
        uint8_t cl[5] = { 0x19, (uint8_t)sid, (uint8_t)(sid >> 8), (uint8_t)(sid >> 16), (uint8_t)(sid >> 24) };
        m.seq = 0;
        my_wr(&m, cl, 5);                                  // COM_STMT_CLOSE: nessuna risposta
        my_execute(&m, sid, 2, v1, out, sizeof(out));
        CHECK(strstr(out, "ERR:1243"), "closed statement: %s", out);
        my_query(&m, "DELETE FROM p WHERE id = 40", out, sizeof(out));

        // autocommit = 0: le modifiche restano in sospeso fino a COMMIT / ROLLBACK
        my_query(&m, "COMMIT", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0"), "COMMIT with nothing to commit: %s", out);
        my_query(&m, "SET autocommit = 0; DELETE FROM p WHERE id = 1; ROLLBACK", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0 / OK:1 / OK:0"), "autocommit off: %s", out);
        my_query(&m, "SELECT COUNT(*) FROM p", out, sizeof(out));
        CHECK(!strcmp(out, "3"), "implicit transaction rolled back: %s", out);
        my_query(&m, "UPDATE p SET nome = 'ac' WHERE id = 1; SET @@session.autocommit = 1; ROLLBACK; SELECT nome FROM p WHERE id = 1", out, sizeof(out));
        CHECK(strstr(out, " / ac"), "SET autocommit = 1 commits: %s", out);
        my_query(&m, "START TRANSACTION; DELETE FROM p WHERE id = 1; ROLLBACK WORK; SELECT COUNT(*) FROM p", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0 / OK:1 / OK:0 / 3"), "START TRANSACTION / ROLLBACK WORK: %s", out);

        // transazioni concorrenti: due connessioni con transazioni aperte insieme su tabelle diverse;
        // chi tocca una tabella in uso aspetta il COMMIT; un deadlock annulla una delle due (1213)
        my_query(&m, "CREATE TABLE q (id INT PRIMARY KEY, v INT); INSERT INTO q VALUES (1, 0)", out, sizeof(out));
        My m2;
        CHECK(my_connect(&m2, mp, "root", "tok4", "t", e, sizeof(e)), "second connection: %s", e);
        my_query(&m, "START TRANSACTION; UPDATE p SET prezzo = 1 WHERE id = 1", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0 / OK:1"), "A opens a transaction on p: %s", out);
        my_query(&m2, "START TRANSACTION; UPDATE q SET v = v + 1; SELECT v FROM q", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0 / OK:1 / 1"), "B opens one on q at the same time: %s", out);
        CcArg ca = { &m2, "UPDATE p SET prezzo = 2 WHERE id = 1", "" };
        pthread_t ct;
        pthread_create(&ct, NULL, cc_thread, &ca);
        usleep(150000);
        CHECK(!ca.out[0], "B waits for the row A is writing: %s", ca.out);
        my_query(&m, "COMMIT", out, sizeof(out));
        pthread_join(ct, NULL);
        CHECK(!strcmp(ca.out, "OK:1"), "B continues after A's COMMIT: %s", ca.out);
        my_query(&m2, "COMMIT", out, sizeof(out));
        my_query(&m, "SELECT prezzo FROM p WHERE id = 1; SELECT v FROM q", out, sizeof(out));
        CHECK(!strcmp(out, "2 / 1"), "both committed: %s", out);
        // deadlock
        my_query(&m, "START TRANSACTION; UPDATE p SET prezzo = 3 WHERE id = 1", out, sizeof(out));
        my_query(&m2, "START TRANSACTION; UPDATE q SET v = 10", out, sizeof(out));
        CcArg da = { &m, "UPDATE q SET v = 20", "" }, db2 = { &m2, "UPDATE p SET prezzo = 4 WHERE id = 1", "" };
        pthread_t d1, d2;
        pthread_create(&d1, NULL, cc_thread, &da);
        usleep(100000);
        pthread_create(&d2, NULL, cc_thread, &db2);
        pthread_join(d1, NULL);
        pthread_join(d2, NULL);
        bool a_lost = strstr(da.out, "ERR:1213") != NULL, b_lost = strstr(db2.out, "ERR:1213") != NULL;
        CHECK(a_lost != b_lost && (a_lost ? !strcmp(db2.out, "OK:1") : !strcmp(da.out, "OK:1")), "deadlock: one gets 1213: [%s] [%s]", da.out, db2.out);
        my_query(&m, "COMMIT", out, sizeof(out));
        my_query(&m2, "COMMIT", out, sizeof(out));
        my_query(&m, "SELECT prezzo FROM p WHERE id = 1; SELECT v FROM q", out, sizeof(out));
        CHECK(!strcmp(out, a_lost ? "4 / 10" : "3 / 20"), "the winner's changes only: %s", out);
        my_query(&m, "DROP TABLE q", out, sizeof(out));
        close(m2.fd);
        close(m.fd);
        if (s4) { sv_server_stop(s4); pthread_join(th4, NULL); sv_server_free(s4); }
    }

    sv_server_stop(s);
    sv_server_stop(s2);
    pthread_join(th, NULL);
    pthread_join(th2, NULL);
    sv_server_free(s);
    sv_server_free(s2);
    db_shutdown();
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) {}
    if (g_fail) { printf("server tests: %d of %d checks FAILED\n", g_fail, g_count); return 1; }
    printf("OK: %d/%d server checks passed\n", g_count, g_count);
    return 0;
}
