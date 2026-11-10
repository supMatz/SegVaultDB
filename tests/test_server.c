// Test del server HTTP/JSON: richieste reali su socket locali.
//   make test_server && ./test_server
#include "src/server/server.h"
#include "src/server/json.h"
#include "src/bridge/db_api.h"
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
