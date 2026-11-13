// Test di PHP via FastCGI (src/server/fastcgi.c e il sito web del server).
//   make test_php && ./test_php
// PHP non serve: questo stesso eseguibile, avviato come "test_php -b 127.0.0.1:PORTA", fa da php-cgi
// (un responder FastCGI vero, che risponde secondo il contenuto dello "script"). Se php-cgi è
// installato, alla fine viene provato anche quello vero.
#include "src/server/server.h"
#include "src/server/fastcgi.h"
#include "src/bridge/db_api.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int g_fail = 0, g_count = 0;
#define CHECK(cond, ...) do { g_count++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── finto php-cgi ───────────────────────────────────────────────

static bool rd(int fd, void* p, size_t n) {
    uint8_t* b = p;
    while (n) { ssize_t r = recv(fd, b, n, 0); if (r <= 0) return false; b += r; n -= (size_t)r; }
    return true;
}
static void wr(int fd, const void* p, size_t n) {
    const uint8_t* b = p;
    while (n) { ssize_t w = send(fd, b, n, MSG_NOSIGNAL); if (w <= 0) return; b += w; n -= (size_t)w; }
}
static void rec(int fd, int type, const char* data, size_t n) {
    do {
        size_t k = n > 65535 ? 65535 : n;
        uint8_t h[8] = { 1, (uint8_t)type, 0, 1, (uint8_t)(k >> 8), (uint8_t)k, 0, 0 };
        wr(fd, h, 8);
        wr(fd, data, k);
        data += k; n -= k;
    } while (n);
}
static size_t rlen(const uint8_t* p, size_t* i) {
    if (p[*i] < 128) return p[(*i)++];
    size_t v = ((size_t)(p[*i] & 0x7f) << 24) | (size_t)p[*i + 1] << 16 | (size_t)p[*i + 2] << 8 | p[*i + 3];
    *i += 4;
    return v;
}

typedef struct { char* buf; size_t len, cap; } Str;
static void sadd(Str* s, const char* p, size_t n) {
    if (s->len + n + 1 > s->cap) { s->cap = (s->len + n + 1) * 2; s->buf = realloc(s->buf, s->cap); }
    memcpy(s->buf + s->len, p, n); s->len += n; s->buf[s->len] = 0;
}
static void sputs(Str* s, const char* p) { sadd(s, p, strlen(p)); }

static void fake_conn(int fd) {
    Str params = {0}, in = {0};
    bool params_done = false, in_done = false;
    while (!params_done || !in_done) {
        uint8_t h[8];
        if (!rd(fd, h, 8)) return;
        size_t n = (size_t)h[4] << 8 | h[5], pad = h[6];
        char* c = malloc(n + pad + 1);
        if (!rd(fd, c, n + pad)) { free(c); return; }
        if (h[1] == 4) { if (n) sadd(&params, c, n); else params_done = true; }
        if (h[1] == 5) { if (n) sadd(&in, c, n); else in_done = true; }
        free(c);
    }
    // variabili come "NOME=valore\n"
    Str vars = {0};
    char script_file[4096] = "";
    for (size_t i = 0; i < params.len; ) {
        size_t nl = rlen((uint8_t*)params.buf, &i), vl = rlen((uint8_t*)params.buf, &i);
        sadd(&vars, params.buf + i, nl); sputs(&vars, "=");
        sadd(&vars, params.buf + i + nl, vl); sputs(&vars, "\n");
        if (nl == 15 && !memcmp(params.buf + i, "SCRIPT_FILENAME", 15)) snprintf(script_file, sizeof(script_file), "%.*s", (int)vl, params.buf + i + nl);
        i += nl + vl;
    }
    char what[64] = "";
    FILE* f = fopen(script_file, "r");
    if (f) { if (!fgets(what, sizeof(what), f)) what[0] = 0; fclose(f); }
    what[strcspn(what, "\r\n")] = 0;

    Str out = {0};
    if (!strcmp(what, "crash")) exit(3);
    if (!strcmp(what, "sleep")) sleep(2);
    if (!strcmp(what, "redirect")) sputs(&out, "Location: /dest?a=1\r\n\r\n");
    else if (!strcmp(what, "cookies")) sputs(&out, "Status: 201 Created\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nContent-type: application/json\r\nX-Powered-By: fake\r\n\r\n{\"ok\":1}");
    else if (!strcmp(what, "stderr")) { const char* e = "PHP Warning: something odd\n"; rec(fd, 7, e, strlen(e)); sputs(&out, "Content-type: text/plain\n\nunix newlines"); }
    else if (!strcmp(what, "big")) {
        sputs(&out, "Content-type: text/plain\r\n\r\n");
        for (int i = 0; i < 100000; i++) sputs(&out, "0123456789");
    } else {                                     // "echo": variabili, ambiente e corpo
        sputs(&out, "Content-type: text/plain\r\n\r\n");
        sadd(&out, vars.buf ? vars.buf : "", vars.len);
        sputs(&out, "ENV_CHILDREN="); sputs(&out, getenv("PHP_FCGI_CHILDREN") ? getenv("PHP_FCGI_CHILDREN") : "(none)"); sputs(&out, "\n");
        sputs(&out, "BODY="); if (in.len) sadd(&out, in.buf, in.len); sputs(&out, "\n");
    }
    // l'uscita in pezzi di dimensioni diverse, come fa PHP
    for (size_t i = 0; i < out.len; ) { size_t k = out.len - i > 7000 ? 7000 : out.len - i; rec(fd, 6, out.buf + i, k); i += k; }
    rec(fd, 6, "", 0);
    uint8_t end[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    rec(fd, 3, (char*)end, 8);
    free(params.buf); free(in.buf); free(vars.buf); free(out.buf);
}

static int fake_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) || listen(fd, 16)) return -1;
    return fd;
}
static void fake_loop(int lfd) {
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) return;
        fake_conn(c);
        close(c);
    }
}
static void* fake_thread(void* arg) { fake_loop((int)(intptr_t)arg); return NULL; }

// ── client HTTP ─────────────────────────────────────────────────

typedef struct { int status; char* head; char* body; size_t blen; } Resp;

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static Resp req(int port, const char* method, const char* path, const char* headers, const char* body) {
    Resp r = {0};
    int fd = connect_to(port);
    if (fd < 0) return r;
    Str q = {0};
    char line[4096];
    snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: localhost:%d\r\nConnection: close\r\n%s", method, path, port, headers ? headers : "");
    sputs(&q, line);
    if (body) { snprintf(line, sizeof(line), "Content-Length: %zu\r\n", strlen(body)); sputs(&q, line); }
    sputs(&q, "\r\n");
    if (body) sputs(&q, body);
    wr(fd, q.buf, q.len);
    free(q.buf);
    Str in = {0};
    char buf[65536];
    for (;;) { ssize_t g = recv(fd, buf, sizeof(buf), 0); if (g <= 0) break; sadd(&in, buf, (size_t)g); }
    close(fd);
    if (!in.buf) return r;
    sscanf(in.buf, "HTTP/1.1 %d", &r.status);
    char* he = strstr(in.buf, "\r\n\r\n");
    if (!he) { free(in.buf); return r; }
    *he = 0;
    r.head = in.buf;
    r.blen = in.len - (size_t)(he + 4 - in.buf);
    r.body = malloc(r.blen + 1);
    memcpy(r.body, he + 4, r.blen);
    r.body[r.blen] = 0;
    if (strcasestr(r.head, "Transfer-Encoding: chunked")) {     // risposta in streaming: si decodifica
        char* src = r.body;
        char* dst = malloc(r.blen + 1);
        size_t dl = 0;
        char* p = src;
        for (;;) {
            size_t n = strtoul(p, &p, 16);
            char* nl = strstr(p, "\r\n");
            if (!nl || !n) break;
            p = nl + 2;
            memcpy(dst + dl, p, n);
            dl += n;
            p += n + 2;
        }
        dst[dl] = 0;
        free(src);
        r.body = dst;
        r.blen = dl;
    }
    return r;
}
// richiesta scritta a mano (corpi chunked, richieste malformate)
static Resp raw(int port, const char* text, size_t n) {
    Resp r = {0};
    int fd = connect_to(port);
    if (fd < 0) return r;
    wr(fd, text, n);
    Str in = {0};
    char buf[65536];
    for (;;) { ssize_t g = recv(fd, buf, sizeof(buf), 0); if (g <= 0) break; sadd(&in, buf, (size_t)g); }
    close(fd);
    if (!in.buf) return r;
    sscanf(in.buf, "HTTP/1.1 %d", &r.status);
    char* he = strstr(in.buf, "\r\n\r\n");
    if (he) { *he = 0; r.body = strdup(he + 4); }
    r.head = in.buf;
    return r;
}
#define RAW(port, lit) raw(port, lit, sizeof(lit) - 1)

static void rfree(Resp* r) { free(r->head); free(r->body); memset(r, 0, sizeof(*r)); }
static bool has(const Resp* r, const char* s) { return r->body && strstr(r->body, s); }
static bool hdr(const Resp* r, const char* s) { return r->head && strcasestr(r->head, s); }

static void put(const char* dir, const char* name, const char* content) {
    char p[1200];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE* f = fopen(p, "w");
    fputs(content, f);
    fclose(f);
}


// valore di un'intestazione della risposta ("" se manca)
static void hval(const Resp* r, const char* name, char* out, size_t n) {
    out[0] = 0;
    char key[80];
    snprintf(key, sizeof(key), "\r\n%s:", name);
    const char* p = r->head ? strcasestr(r->head, key) : NULL;
    if (!p) return;
    p += strlen(key);
    while (*p == ' ') p++;
    size_t l = strcspn(p, "\r\n");
    if (l >= n) l = n - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

// decomprime un corpo gzip con il gzip di sistema; NULL se non è gzip valido
static char* gunzip(const char* dir, const char* data, size_t n, size_t* out_n) {
    char in[1200], cmd[2600];
    snprintf(in, sizeof(in), "%s/body.gz", dir);
    FILE* f = fopen(in, "wb"); fwrite(data, 1, n, f); fclose(f);
    snprintf(cmd, sizeof(cmd), "gzip -dc '%s' 2>/dev/null", in);
    FILE* pp = popen(cmd, "r");
    if (!pp) return NULL;
    Str o = {0};
    char buf[65536];
    size_t g;
    while ((g = fread(buf, 1, sizeof(buf), pp)) > 0) sadd(&o, buf, g);
    int st = pclose(pp);
    if (st != 0) { free(o.buf); return NULL; }
    *out_n = o.len;
    return o.buf ? o.buf : calloc(1, 1);
}

static void* run_server(void* s) { sv_server_run((SvServer*)s); return NULL; }

typedef struct { int port; int ok; } Par;
static void* parallel_req(void* a) {
    Par* p = a;
    Resp r = req(p->port, "GET", "/echo.php?par=1", NULL, NULL);
    p->ok = r.status == 200 && has(&r, "QUERY_STRING=par=1");
    if (!p->ok) printf("parallel request: %d %s\n", r.status, r.body ? r.body : "(no body)");
    rfree(&r);
    return NULL;
}

static SvServer* start(SvServerConfig* cfg, pthread_t* th) {
    char err[300];
    SvServer* s = sv_server_start(cfg, err, sizeof(err));
    if (!s) { printf("start: %s\n", err); exit(1); }
    pthread_create(th, NULL, run_server, s);
    return s;
}
static void stop(SvServer* s, pthread_t th) { sv_server_stop(s); pthread_join(th, NULL); sv_server_free(s); }

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "-b")) {          // modalità "php-cgi -b 127.0.0.1:PORTA"
        const char* c = strrchr(argv[2], ':');
        int lfd = fake_listen(c ? atoi(c + 1) : 0);
        if (lfd < 0) return 1;
        fake_loop(lfd);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    char self[1024];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) return 1;
    self[sl] = 0;

    char dir[] = "/tmp/svphpXXXXXX";
    if (!mkdtemp(dir)) return 1;
    char data[1100], www[1100], sub[1200], real_www[4096];
    snprintf(data, sizeof(data), "%s/data", dir);
    snprintf(www, sizeof(www), "%s/www", dir);
    snprintf(sub, sizeof(sub), "%s/sub", www);
    mkdir(data, 0755); mkdir(www, 0755); mkdir(sub, 0755);
    if (!realpath(www, real_www)) return 1;
    if (!db_init(data)) { printf("init: %s\n", db_init_error()); return 1; }
    put(www, "index.php", "echo\n");
    put(www, "echo.php", "echo\n<?php $password = 'segreta'; ?>\n");
    put(www, "redirect.php", "redirect\n");
    put(www, "cookies.php", "cookies\n");
    put(www, "warn.php", "stderr\n");
    put(www, "big.php", "big\n");
    put(www, "crash.php", "crash\n");
    put(www, "slow.php", "sleep\n");
    put(www, "style.css", "body{}");
    put(sub, "index.php", "echo\n");

    Resp r;
    pthread_t th;

    // ── PHP spento: il sorgente di un .php non viene mai mostrato ──
    {
        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true };
        SvServer* s = start(&cfg, &th);
        int port = sv_server_port(s);
        r = req(port, "GET", "/echo.php", NULL, NULL);
        CHECK(r.status == 403 && !has(&r, "segreta") && has(&r, "--php"), "php off: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/sub/", NULL, NULL);
        CHECK(r.status == 403 && !has(&r, "echo"), "index.php with php off: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/style.css", NULL, NULL);
        CHECK(r.status == 200 && hdr(&r, "text/css") && !strcmp(r.body, "body{}"), "static with php off: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/nessuno", NULL, NULL);
        CHECK(r.status == 404, "missing file with php off: %d", r.status);
        rfree(&r);
        stop(s, th);
    }

    // ── php-cgi avviati da SegVault (qui: questo eseguibile) ──
    {
        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .php_cgi = self, .php_workers = 3, .timeout_ms = 1000 };
        SvServer* s = start(&cfg, &th);
        int port = sv_server_port(s);
        CHECK(strstr(sv_server_php(s), "x3"), "describe: %s", sv_server_php(s));

        r = req(port, "GET", "/echo.php?x=1&y=%20", "X-Test: abc\r\nProxy: http://evil\r\nUser-Agent: t/1\r\n", NULL);
        CHECK(r.status == 200 && hdr(&r, "Content-Type: text/plain"), "echo: %d %s", r.status, r.head);
        CHECK(has(&r, "REQUEST_METHOD=GET\n") && has(&r, "QUERY_STRING=x=1&y=%20\n") && has(&r, "REQUEST_URI=/echo.php?x=1&y=%20\n"),
              "method / query / uri: %s", r.body);
        char want[4200];
        snprintf(want, sizeof(want), "SCRIPT_FILENAME=%s/echo.php\n", real_www);
        CHECK(has(&r, "SCRIPT_NAME=/echo.php\n") && has(&r, want), "script: %s", r.body);
        snprintf(want, sizeof(want), "DOCUMENT_ROOT=%s\n", real_www);
        CHECK(has(&r, want) && has(&r, "REDIRECT_STATUS=200\n") && has(&r, "REMOTE_ADDR=127.0.0.1\n") && has(&r, "SERVER_NAME=localhost\n"),
              "cgi vars: %s", r.body);
        CHECK(has(&r, "HTTP_X_TEST=abc\n") && has(&r, "HTTP_USER_AGENT=t/1\n") && has(&r, "HTTP_HOST=localhost:"), "HTTP_* headers: %s", r.body);
        CHECK(!has(&r, "HTTP_PROXY"), "Proxy header dropped (httpoxy): %s", r.body);
        CHECK(has(&r, "ENV_CHILDREN=0\n") && !has(&r, "PATH_INFO="), "worker environment: %s", r.body);
        rfree(&r);

        r = req(port, "POST", "/echo.php", "Content-Type: application/x-www-form-urlencoded\r\n", "nome=anna&eta=30");
        CHECK(r.status == 200 && has(&r, "BODY=nome=anna&eta=30\n") && has(&r, "CONTENT_LENGTH=16\n") &&
              has(&r, "CONTENT_TYPE=application/x-www-form-urlencoded\n") && !has(&r, "HTTP_CONTENT_TYPE"), "POST form: %s", r.body);
        rfree(&r);
        r = req(port, "DELETE", "/echo.php?id=4", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "REQUEST_METHOD=DELETE\n"), "DELETE goes to PHP: %d", r.status);
        rfree(&r);

        // corpi chunked: a PHP arrivano decodificati, con CONTENT_LENGTH
        r = RAW(port, "POST /echo.php HTTP/1.1\r\nHost: x\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n"
                      "Content-Type: text/plain\r\n\r\n5;ext=1\r\nciao \r\nB\r\nmondo\0zero!\r\n0\r\nX-Trailer: 1\r\n\r\n");
        CHECK(r.status == 200 && r.body && strstr(r.body, "CONTENT_LENGTH=16\n") && strstr(r.body, "BODY=ciao mondo") &&
              !strstr(r.body, "HTTP_TRANSFER_ENCODING"), "chunked body to PHP: %d %s", r.status, r.body);
        rfree(&r);
        r = RAW(port, "POST /api/v1/query HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Type: application/json\r\n\r\n"
                      "8\r\n{\"sql\":\"\r\n10\r\nSELECT 6 * 7 AS \r\n4\r\nr\"}\n\r\n0\r\n\r\n"
                      "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        CHECK(r.status == 200 && r.body && strstr(r.body, "42"), "chunked JSON to the API: %d %s", r.status, r.body);
        CHECK(r.body && strstr(r.body, "HTTP/1.1 200") && strstr(r.body, "\"server\":\"SegVault\""), "pipelined request after a chunked one: %s", r.body);
        rfree(&r);
        r = RAW(port, "POST /echo.php HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
        CHECK(r.status == 400, "Content-Length + chunked rejected (smuggling): %d", r.status);
        rfree(&r);
        r = RAW(port, "POST /echo.php HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
        CHECK(r.status == 501, "other transfer codings: %d", r.status);
        rfree(&r);
        r = RAW(port, "POST /echo.php HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n");
        CHECK(r.status == 400, "bad chunk size: %d", r.status);
        rfree(&r);
        r = RAW(port, "POST /echo.php HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXX0\r\n\r\n");
        CHECK(r.status == 400, "chunk without CRLF: %d", r.status);
        rfree(&r);
        r = RAW(port, "POST /echo.php HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFF\r\n");
        CHECK(r.status == 413, "huge chunk size: %d", r.status);
        rfree(&r);

        r = req(port, "GET", "/echo.php/utenti/7", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/echo.php\n") && has(&r, "PATH_INFO=/utenti/7\n"), "PATH_INFO: %s", r.body);
        rfree(&r);

        r = req(port, "GET", "/", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n"), "index.php at /: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/sub/?a=b", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/sub/index.php\n") && has(&r, "QUERY_STRING=a=b\n"), "sub/index.php: %s", r.body);
        rfree(&r);
        r = req(port, "GET", "/sub?a=b", NULL, NULL);
        CHECK(r.status == 301 && hdr(&r, "Location: /sub/?a=b"), "folder without slash redirects: %d %s", r.status, r.head);
        rfree(&r);

        r = req(port, "GET", "/blog/2026/ciao-mondo?p=2", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n") && has(&r, "REQUEST_URI=/blog/2026/ciao-mondo?p=2\n"),
              "front controller (pretty URLs): %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/manca.php", NULL, NULL);
        CHECK(r.status == 404, "missing .php: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/style.css", NULL, NULL);
        CHECK(r.status == 200 && hdr(&r, "text/css") && !strcmp(r.body, "body{}"), "static file next to PHP: %d", r.status);
        rfree(&r);
        r = req(port, "PUT", "/style.css", NULL, "x");
        CHECK(r.status == 405, "PUT on a static file: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/api/v1/health", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "\"ok\":true"), "API still answers: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/api/v1/utenti/3?x=1", NULL, NULL);        // una route di Laravel, non di SegVault
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n") && has(&r, "REQUEST_URI=/api/v1/utenti/3?x=1\n"),
              "non-API path under /api/v1 goes to PHP: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "POST", "/api/v1/query", "Content-Type: application/json\r\n", "{\"sql\":\"SELECT 41 + 1\"}");
        CHECK(r.status == 200 && has(&r, "42"), "API paths stay with SegVault: %d %s", r.status, r.body);
        rfree(&r);

        r = req(port, "GET", "/redirect.php", NULL, NULL);
        CHECK(r.status == 302 && hdr(&r, "Location: /dest?a=1"), "Location -> 302: %d %s", r.status, r.head);
        rfree(&r);
        r = req(port, "GET", "/cookies.php", NULL, NULL);
        CHECK(r.status == 201 && hdr(&r, "Set-Cookie: a=1\r\n") && hdr(&r, "Set-Cookie: b=2") && hdr(&r, "Content-Type: application/json") &&
              hdr(&r, "X-Powered-By: fake") && !strcmp(r.body, "{\"ok\":1}"), "Status, cookies, headers: %d %s", r.status, r.head);
        rfree(&r);
        r = req(port, "GET", "/warn.php", NULL, NULL);
        CHECK(r.status == 200 && !strcmp(r.body, "unix newlines"), "stderr and \\n\\n headers: %d [%s]", r.status, r.body);
        rfree(&r);
        SvLogEntry last;
        CHECK(sv_server_recent(s, &last, 1) == 1 && strstr(last.detail, "PHP Warning"), "PHP warning in the log: %s", last.detail);
        r = req(port, "HEAD", "/echo.php", NULL, NULL);
        CHECK(r.status == 200 && r.blen == 0 && hdr(&r, "Content-Length: "), "HEAD: %d %zu", r.status, r.blen);
        rfree(&r);
        r = req(port, "GET", "/big.php", NULL, NULL);
        bool big_ok = r.status == 200 && r.blen == 1000000;
        for (size_t i = 0; big_ok && i < r.blen; i++) big_ok = r.body[i] == (char)('0' + i % 10);
        CHECK(big_ok, "1 MB response over many records: %d %zu", r.status, r.blen);
        CHECK(hdr(&r, "Transfer-Encoding: chunked") && !hdr(&r, "Content-Length"), "large PHP response streamed: %s", r.head);
        rfree(&r);

        // PHP spento e riacceso con il server in funzione (gli altri servizi non si fermano)
        {
            char e2[300];
            CHECK(sv_server_service(s, SV_SVC_PHP, false, e2, sizeof(e2)) && !sv_server_service_on(s, SV_SVC_PHP), "PHP off");
            r = req(port, "GET", "/echo.php", NULL, NULL);
            CHECK(r.status == 403 && !has(&r, "segreta"), "php off at run time: %d", r.status);
            rfree(&r);
            r = req(port, "GET", "/style.css", NULL, NULL);
            CHECK(r.status == 200, "static files still served: %d", r.status);
            rfree(&r);
            CHECK(sv_server_service(s, SV_SVC_PHP, true, e2, sizeof(e2)) && sv_server_service_on(s, SV_SVC_PHP), "PHP on again: %s", e2);
            r = req(port, "GET", "/echo.php", NULL, NULL);
            CHECK(r.status == 200 && has(&r, "REQUEST_METHOD=GET"), "php back: %d", r.status);
            rfree(&r);
            SvLogEntry le;
            CHECK(sv_server_recent(s, &le, 1) == 1 && !strcmp(le.svc, "php"), "PHP requests tagged: %s", le.svc);
        }

        // un processo che muore viene riavviato
        r = req(port, "GET", "/crash.php", NULL, NULL);
        CHECK(r.status == 502, "crashed worker: %d %s", r.status, r.body);
        rfree(&r);
        int ok = 0;
        for (int i = 0; i < 6; i++) { r = req(port, "GET", "/echo.php", NULL, NULL); ok += r.status == 200; rfree(&r); }
        CHECK(ok == 6, "requests after a crash: %d/6", ok);

        // tempo scaduto: 504, il processo bloccato viene sostituito
        r = req(port, "GET", "/slow.php", NULL, NULL);
        CHECK(r.status == 504, "timeout: %d %s", r.status, r.body);
        rfree(&r);
        ok = 0;
        for (int i = 0; i < 6; i++) { r = req(port, "GET", "/echo.php", NULL, NULL); ok += r.status == 200; rfree(&r); }
        CHECK(ok == 6, "requests after a timeout: %d/6", ok);

        // più richieste insieme che processi
        pthread_t pt[10];
        Par pp[10];
        for (int i = 0; i < 10; i++) { pp[i].port = port; pp[i].ok = 0; pthread_create(&pt[i], NULL, parallel_req, &pp[i]); }
        ok = 0;
        for (int i = 0; i < 10; i++) { pthread_join(pt[i], NULL); ok += pp[i].ok; }
        CHECK(ok == 10, "10 parallel PHP requests on 3 workers: %d", ok);
        stop(s, th);
    }

    // ── prefisso dell'API spostato o spento ──
    {
        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .php_cgi = self, .php_workers = 1, .api_prefix = "/_segvault/" };
        SvServer* s = start(&cfg, &th);
        int port = sv_server_port(s);
        CHECK(!strcmp(sv_server_api_prefix(s), "/_segvault"), "normalized prefix: %s", sv_server_api_prefix(s));
        r = req(port, "GET", "/_segvault/health", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "\"ok\":true"), "API under the new prefix: %d", r.status);
        rfree(&r);
        r = req(port, "POST", "/api/v1/query", "Content-Type: application/json\r\n", "{}");
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n") && has(&r, "REQUEST_METHOD=POST"), "/api/v1/query belongs to the site: %d %s", r.status, r.body);
        rfree(&r);
        stop(s, th);
        SvServerConfig off = { .port = 0, .static_dir = www, .quiet = true, .api_prefix = "none" };
        s = start(&off, &th);
        CHECK(!strcmp(sv_server_api_prefix(s), ""), "API off");
        r = req(sv_server_port(s), "GET", "/api/v1/health", NULL, NULL);
        CHECK(r.status == 404 && !has(&r, "\"ok\":true"), "API off: %d %s", r.status, r.body);
        rfree(&r);
        r = req(sv_server_port(s), "GET", "/style.css", NULL, NULL);
        CHECK(r.status == 200, "site with the API off: %d", r.status);
        rfree(&r);
        stop(s, th);
        char err[300];
        SvServerConfig bad = { .port = 0, .quiet = true, .api_prefix = "/a/../b" };
        CHECK(!sv_server_start(&bad, err, sizeof(err)) && strstr(err, "invalid API prefix"), "bad prefix: %s", err);
    }

    // ── un processo riavviato dal thread di una connessione sopravvive alla connessione ──
    // (PR_SET_PDEATHSIG è legato al thread che fa fork: i processi nascono da un thread apposito)
    {
        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .php_cgi = self, .php_workers = 1 };
        SvServer* s = start(&cfg, &th);
        int port = sv_server_port(s);
        r = req(port, "GET", "/crash.php", NULL, NULL);
        CHECK(r.status == 502, "single worker crash: %d", r.status);
        rfree(&r);
        int ok = 0;
        for (int i = 0; i < 5; i++) { r = req(port, "GET", "/echo.php", NULL, NULL); ok += r.status == 200; rfree(&r); usleep(50000); }
        CHECK(ok == 5, "restarted worker survives the connection that restarted it: %d/5", ok);
        stop(s, th);
    }

    // ── backend esterno (php-fpm): host:porta ──
    {
        int lfd = fake_listen(0);
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        getsockname(lfd, (struct sockaddr*)&a, &al);
        char addr[64];
        snprintf(addr, sizeof(addr), "127.0.0.1:%d", ntohs(a.sin_port));
        pthread_t ft;
        pthread_create(&ft, NULL, fake_thread, (void*)(intptr_t)lfd);
        pthread_detach(ft);
        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .php_fcgi = addr };
        SvServer* s = start(&cfg, &th);
        r = req(sv_server_port(s), "GET", "/echo.php?ext=1", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "QUERY_STRING=ext=1\n") && has(&r, "ENV_CHILDREN=(none)"), "external FastCGI: %d %s", r.status, r.body);
        rfree(&r);
        stop(s, th);
        SvServerConfig bad = { .port = 0, .static_dir = www, .quiet = true, .php_fcgi = "127.0.0.1:1" };
        s = start(&bad, &th);
        r = req(sv_server_port(s), "GET", "/echo.php", NULL, NULL);
        CHECK(r.status == 502, "FastCGI server down: %d", r.status);
        rfree(&r);
        stop(s, th);
    }

    // ── configurazioni sbagliate ──
    {
        char err[300];
        SvServerConfig c1 = { .port = 0, .quiet = true, .php_cgi = self };
        CHECK(!sv_server_start(&c1, err, sizeof(err)) && strstr(err, "--static"), "PHP without a web folder: %s", err);
        SvServerConfig c2 = { .port = 0, .static_dir = www, .quiet = true, .php_cgi = "/nessun/php-cgi" };
        CHECK(!sv_server_start(&c2, err, sizeof(err)) && strstr(err, "not found"), "missing php-cgi: %s", err);
        SvServerConfig c3 = { .port = 0, .static_dir = www, .quiet = true, .php_cgi = "/bin/true" };
        CHECK(!sv_server_start(&c3, err, sizeof(err)) && strstr(err, "exited"), "not a php-cgi: %s", err);
        SvServerConfig c4 = { .port = 0, .static_dir = www, .quiet = true, .php_fcgi = "senza-porta" };
        CHECK(!sv_server_start(&c4, err, sizeof(err)) && strstr(err, "host:port"), "bad FastCGI address: %s", err);
    }

    // ── sito web: file statici ──
    {
        char wk[1300], git[1300], big[1300];
        snprintf(wk, sizeof(wk), "%s/.well-known", www); mkdir(wk, 0755);
        snprintf(wk, sizeof(wk), "%s/.well-known/acme", www); mkdir(wk, 0755);
        put(wk, "tok", "acme-ok");
        snprintf(git, sizeof(git), "%s/.git", www); mkdir(git, 0755);
        put(git, "config", "[secret]");
        // testo grande (comprimibile) e "video" binario
        Str js = {0};
        for (int i = 0; i < 4000; i++) { char l[64]; snprintf(l, sizeof(l), "console.log('riga %d');\n", i); sputs(&js, l); }
        put(www, "app.js", js.buf);
        snprintf(big, sizeof(big), "%s/clip.mp4", www);
        FILE* f = fopen(big, "wb");
        for (int i = 0; i < 300000; i++) fputc((i * 7 + i / 256) & 0xff, f);
        fclose(f);
        f = fopen(big, "rb");
        char* vid = malloc(300000);
        if (fread(vid, 1, 300000, f) != 300000) return 1;
        fclose(f);

        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true };
        SvServer* s = start(&cfg, &th);
        int port = sv_server_port(s);
        char v[200], etag[200], lm[200];

        r = req(port, "GET", "/app.js", NULL, NULL);
        hval(&r, "Content-Encoding", v, sizeof(v));
        CHECK(r.status == 200 && !v[0] && r.blen == js.len && !memcmp(r.body, js.buf, js.len) && hdr(&r, "text/javascript"),
              "text without Accept-Encoding: %d %zu %s", r.status, r.blen, v);
        hval(&r, "ETag", etag, sizeof(etag));
        hval(&r, "Last-Modified", lm, sizeof(lm));
        CHECK(etag[0] == '"' && strstr(lm, " GMT") && hdr(&r, "Accept-Ranges: bytes") && hdr(&r, "Cache-Control: no-cache"), "validators: %s / %s", etag, lm);
        rfree(&r);

        r = req(port, "GET", "/app.js", "Accept-Encoding: gzip, deflate, br\r\n", NULL);
        hval(&r, "Content-Encoding", v, sizeof(v));
        size_t un = 0;
        char* plain = r.body ? gunzip(dir, r.body, r.blen, &un) : NULL;
        CHECK(r.status == 200 && !strcmp(v, "gzip") && r.blen < js.len / 3 && plain && un == js.len && !memcmp(plain, js.buf, js.len),
              "gzip: %d %s %zu -> %zu", r.status, v, js.len, r.blen);
        free(plain);
        char gz_etag[200];
        hval(&r, "ETag", gz_etag, sizeof(gz_etag));
        CHECK(strcmp(gz_etag, etag) && hdr(&r, "Vary: Accept-Encoding"), "gzip variant has its own ETag: %s %s", gz_etag, etag);
        rfree(&r);
        r = req(port, "GET", "/app.js", "Accept-Encoding: gzip;q=0, identity\r\n", NULL);
        CHECK(r.status == 200 && !hdr(&r, "Content-Encoding"), "gzip;q=0 refuses gzip: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/style.css", "Accept-Encoding: gzip\r\n", NULL);
        CHECK(r.status == 200 && !hdr(&r, "Content-Encoding") && !strcmp(r.body, "body{}"), "tiny file not compressed");
        rfree(&r);

        // cache del browser
        char h2[400];
        snprintf(h2, sizeof(h2), "If-None-Match: %s\r\n", etag);
        r = req(port, "GET", "/app.js", h2, NULL);
        CHECK(r.status == 304 && r.blen == 0 && hdr(&r, "ETag"), "If-None-Match -> 304: %d %zu", r.status, r.blen);
        rfree(&r);
        snprintf(h2, sizeof(h2), "If-None-Match: \"altro\", W/%s\r\n", etag);
        r = req(port, "GET", "/app.js", h2, NULL);
        CHECK(r.status == 304, "weak tag in a list -> 304: %d", r.status);
        rfree(&r);
        snprintf(h2, sizeof(h2), "If-Modified-Since: %s\r\n", lm);
        r = req(port, "GET", "/app.js", h2, NULL);
        CHECK(r.status == 304, "If-Modified-Since -> 304: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/app.js", "If-None-Match: \"vecchio\"\r\n", NULL);
        CHECK(r.status == 200 && r.blen == js.len, "changed file -> 200: %d", r.status);
        rfree(&r);

        // file binario a pezzi e intervalli
        r = req(port, "GET", "/clip.mp4", "Accept-Encoding: gzip\r\n", NULL);
        CHECK(r.status == 200 && r.blen == 300000 && !memcmp(r.body, vid, 300000) && hdr(&r, "video/mp4") && !hdr(&r, "Content-Encoding"),
              "streamed binary file: %d %zu", r.status, r.blen);
        rfree(&r);
        r = req(port, "HEAD", "/clip.mp4", NULL, NULL);
        hval(&r, "Content-Length", v, sizeof(v));
        CHECK(r.status == 200 && r.blen == 0 && !strcmp(v, "300000"), "HEAD of a big file: %d %s %zu", r.status, v, r.blen);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: bytes=100-199\r\n", NULL);
        hval(&r, "Content-Range", v, sizeof(v));
        CHECK(r.status == 206 && r.blen == 100 && !memcmp(r.body, vid + 100, 100) && !strcmp(v, "bytes 100-199/300000"), "range: %d %s %zu", r.status, v, r.blen);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: bytes=-10\r\n", NULL);
        CHECK(r.status == 206 && r.blen == 10 && !memcmp(r.body, vid + 299990, 10), "suffix range: %d %zu", r.status, r.blen);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: bytes=299000-\r\n", NULL);
        hval(&r, "Content-Range", v, sizeof(v));
        CHECK(r.status == 206 && r.blen == 1000 && !memcmp(r.body, vid + 299000, 1000) && !strcmp(v, "bytes 299000-299999/300000"), "open range: %d %s", r.status, v);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: bytes=299990-999999\r\n", NULL);
        CHECK(r.status == 206 && r.blen == 10, "range past the end is clipped: %d %zu", r.status, r.blen);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: bytes=400000-\r\n", NULL);
        hval(&r, "Content-Range", v, sizeof(v));
        CHECK(r.status == 416 && !strcmp(v, "bytes */300000"), "unsatisfiable range: %d %s", r.status, v);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: bytes=0-1,5-6\r\n", NULL);
        CHECK(r.status == 200 && r.blen == 300000, "several ranges -> whole file: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/clip.mp4", "Range: pezzi=1-2\r\n", NULL);
        CHECK(r.status == 200 && r.blen == 300000, "unknown range unit ignored: %d", r.status);
        rfree(&r);

        // percorsi
        r = req(port, "GET", "/.well-known/acme/tok", NULL, NULL);
        CHECK(r.status == 200 && !strcmp(r.body, "acme-ok"), "/.well-known served: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/.git/config", NULL, NULL);
        CHECK(r.status == 404 && !has(&r, "secret"), "hidden folders stay hidden: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/.well-known/../.git/config", NULL, NULL);
        CHECK(r.status == 404 && !has(&r, "secret"), "no escape through /.well-known: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/manca.html", NULL, NULL);
        CHECK(r.status == 404 && hdr(&r, "application/json"), "default 404: %d", r.status);
        rfree(&r);
        put(www, "404.html", "<h1>Pagina non trovata</h1>");
        r = req(port, "GET", "/manca.html", NULL, NULL);
        CHECK(r.status == 404 && hdr(&r, "text/html") && has(&r, "Pagina non trovata"), "custom 404.html: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/api/v1/nessuno", NULL, NULL);
        CHECK(r.status == 404 && hdr(&r, "application/json"), "API 404 stays JSON: %d", r.status);
        rfree(&r);

        // l'API comprime i risultati grandi
        r = req(port, "POST", "/api/v1/query", "Content-Type: application/json\r\nAccept-Encoding: gzip\r\n",
                "{\"sql\":\"WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 900) SELECT i, 'testo ripetuto' AS t FROM n\"}");
        hval(&r, "Content-Encoding", v, sizeof(v));
        plain = r.body ? gunzip(dir, r.body, r.blen, &un) : NULL;
        CHECK(r.status == 200 && !strcmp(v, "gzip") && plain && strstr(plain, "\"ok\":true") && strstr(plain, "900"), "gzip JSON: %d %s %zu %.200s", r.status, v, r.blen, r.body);
        free(plain);
        rfree(&r);
        stop(s, th);
        unlink(big);
        free(vid);
        free(js.buf);
        char p404[1300];
        snprintf(p404, sizeof(p404), "%s/404.html", www);
        unlink(p404);
    }

    // ── .htaccess (mod_rewrite di WordPress e Laravel, accesso negato, Options -Indexes) ──
    {
        char w2[1200], d[1300];
        snprintf(w2, sizeof(w2), "%s/w2", dir);
        mkdir(w2, 0755);
        put(w2, "index.php", "echo\n");
        put(w2, "echo.php", "echo\n");
        put(w2, "style.css", "body{}");
        put(w2, "app.log", "log segreto");
        put(w2, "app.txt", "testo");
        put(w2, ".htaccess",
            "# BEGIN WordPress\n"
            "<IfModule mod_rewrite.c>\n"
            "RewriteEngine On\n"
            "RewriteRule .* - [E=HTTP_AUTHORIZATION:%{HTTP:Authorization}]\n"
            "RewriteBase /\n"
            "RewriteRule ^old/(.*)$ /new/$1 [R=301,L]\n"
            "RewriteRule ^shop/item/([0-9]+)/?$ echo.php?id=$1 [QSA,L]\n"
            "RewriteRule ^blocked - [F]\n"
            "RewriteRule ^removed - [G,L]\n"
            "RewriteCond %{HTTP_HOST} ^www\\.(.+)$ [NC]\n"
            "RewriteRule ^(.*)$ http://%1/$1 [R=301,L]\n"
            "RewriteCond %{QUERY_STRING} (^|&)lang=(it|en) [OR]\n"
            "RewriteCond %{HTTP:X-Lang} ^(de)$\n"
            "RewriteRule ^hello$ echo.php?greeting=1 [L]\n"
            "RewriteRule ^index\\.php$ - [L]\n"
            "RewriteCond %{REQUEST_FILENAME} !-f\n"
            "RewriteCond %{REQUEST_FILENAME} !-d\n"
            "RewriteRule . /index.php [L]\n"
            "</IfModule>\n"
            "<IfModule !mod_rewrite.c>\n"
            "Require all denied\n"
            "</IfModule>\n"
            "<FilesMatch \"\\.(log|ini)$\">\n"
            "    Require all denied\n"
            "</FilesMatch>\n"
            "# END WordPress\n");
        snprintf(d, sizeof(d), "%s/private", w2); mkdir(d, 0755);
        put(d, ".htaccess", "Deny from all\n");
        put(d, "secret.txt", "password");
        snprintf(d, sizeof(d), "%s/private/open", w2); mkdir(d, 0755);
        put(d, ".htaccess", "Require all granted\n");
        put(d, "ok.txt", "aperto");
        snprintf(d, sizeof(d), "%s/files", w2); mkdir(d, 0755);
        put(d, ".htaccess", "Options -Indexes +FollowSymLinks\n");
        put(d, "a.txt", "a");
        // Laravel: il sito lara.localhost ha public/ come radice
        snprintf(d, sizeof(d), "%s/lara", w2); mkdir(d, 0755);
        snprintf(d, sizeof(d), "%s/lara/public", w2); mkdir(d, 0755);
        put(d, "index.php", "echo\n");
        put(d, ".htaccess",
            "<IfModule mod_rewrite.c>\n"
            "    <IfModule mod_negotiation.c>\n"
            "        Options -MultiViews -Indexes\n"
            "    </IfModule>\n\n"
            "    RewriteEngine On\n\n"
            "    # Handle Authorization Header\n"
            "    RewriteCond %{HTTP:Authorization} .\n"
            "    RewriteRule .* - [E=HTTP_AUTHORIZATION:%{HTTP:Authorization}]\n\n"
            "    # Redirect Trailing Slashes If Not A Folder...\n"
            "    RewriteCond %{REQUEST_FILENAME} !-d\n"
            "    RewriteCond %{REQUEST_URI} (.+)/$\n"
            "    RewriteRule ^ %1 [L,R=301]\n\n"
            "    # Send Requests To Front Controller...\n"
            "    RewriteCond %{REQUEST_FILENAME} !-d\n"
            "    RewriteCond %{REQUEST_FILENAME} !-f\n"
            "    RewriteRule ^ index.php [L]\n"
            "</IfModule>\n");

        SvServerConfig cfg = { .port = 0, .static_dir = w2, .quiet = true, .php_cgi = self, .php_workers = 2, .dir_listing = true };
        SvServer* s = start(&cfg, &th);
        int port = sv_server_port(s);
        r = req(port, "GET", "/2026/11/ciao-mondo/?p=3", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n") && has(&r, "REQUEST_URI=/2026/11/ciao-mondo/?p=3\n") &&
              has(&r, "QUERY_STRING=p=3\n"), "WordPress permalink: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/style.css", NULL, NULL);
        CHECK(r.status == 200 && !strcmp(r.body, "body{}"), "existing file not rewritten: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/old/page?x=1", NULL, NULL);
        CHECK(r.status == 301 && hdr(&r, "Location: /new/page?x=1"), "R=301 with $1: %d %s", r.status, r.head);
        rfree(&r);
        r = req(port, "GET", "/shop/item/42/?ref=mail", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/echo.php\n") && has(&r, "QUERY_STRING=id=42&ref=mail\n") &&
              has(&r, "REQUEST_URI=/shop/item/42/?ref=mail\n"), "QSA + RewriteBase: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/blocked/x", NULL, NULL);
        CHECK(r.status == 403, "[F]: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/removed", NULL, NULL);
        CHECK(r.status == 410, "[G]: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/a/b?c=d", "Host: www.example.test\r\n", NULL);
        CHECK(r.status == 301 && hdr(&r, "Location: http://example.test/a/b?c=d"), "%%1 from RewriteCond, host redirect: %d %s", r.status, r.head);
        rfree(&r);
        r = req(port, "GET", "/hello?lang=it", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "QUERY_STRING=greeting=1\n"), "[OR] first: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/hello", "X-Lang: de\r\n", NULL);
        CHECK(r.status == 200 && has(&r, "QUERY_STRING=greeting=1\n"), "[OR] second (%%{HTTP:X-Lang}): %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/hello?lang=fr", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n") && has(&r, "QUERY_STRING=lang=fr\n"), "[OR] none: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/app.log", NULL, NULL);
        CHECK(r.status == 403 && !has(&r, "segreto"), "FilesMatch denied: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/app.txt", NULL, NULL);
        CHECK(r.status == 200 && !strcmp(r.body, "testo"), "other files allowed: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/private/secret.txt", NULL, NULL);
        CHECK(r.status == 403 && !has(&r, "password"), "Deny from all: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/private/open/ok.txt", NULL, NULL);
        CHECK(r.status == 200 && !strcmp(r.body, "aperto"), "deeper Require all granted: %d", r.status);
        rfree(&r);
        r = req(port, "GET", "/files/", NULL, NULL);
        CHECK(r.status != 200 || !has(&r, "a.txt"), "Options -Indexes: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/.htaccess", NULL, NULL);
        CHECK(r.status == 404 && !has(&r, "RewriteEngine"), ".htaccess never served: %d", r.status);
        rfree(&r);
        // Laravel
        r = req(port, "GET", "/users?page=2", "Host: lara.localhost\r\nAuthorization: Bearer abc\r\n", NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n") && has(&r, "QUERY_STRING=page=2\n") &&
              has(&r, "REQUEST_URI=/users?page=2\n") && has(&r, "HTTP_AUTHORIZATION=Bearer abc\n"), "Laravel front controller: %d %s", r.status, r.body);
        rfree(&r);
        r = req(port, "GET", "/users/", "Host: lara.localhost\r\n", NULL);
        CHECK(r.status == 301 && hdr(&r, "Location: /users") && !hdr(&r, "Location: /users/"), "Laravel trailing slash redirect: %d %s", r.status, r.head);
        rfree(&r);
        stop(s, th);

        // --no-htaccess: le regole non valgono più (resta il front controller di SegVault)
        SvServerConfig c2 = cfg;
        c2.no_htaccess = true;
        s = start(&c2, &th);
        port = sv_server_port(s);
        r = req(port, "GET", "/old/page", NULL, NULL);
        CHECK(r.status == 200 && has(&r, "SCRIPT_NAME=/index.php\n"), "htaccess off: %d %s", r.status, r.head);
        rfree(&r);
        r = req(port, "GET", "/app.log", NULL, NULL);
        CHECK(r.status == 200, "htaccess off, FilesMatch ignored: %d", r.status);
        rfree(&r);
        stop(s, th);
    }

    // ── php-cgi vero, se c'è ──
    char php[1024];
    if (fcgi_find_php_cgi(php, sizeof(php))) {
        put(www, "real.php", "<?php header('X-Real: 1'); echo 'sum=', 2 + 3, ' m=', $_SERVER['REQUEST_METHOD'], ' q=', $_GET['q'] ?? '', ' p=', $_POST['p'] ?? '';");
        SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .php_cgi = php, .php_workers = 2 };
        SvServer* s = start(&cfg, &th);
        r = req(sv_server_port(s), "POST", "/real.php?q=7", "Content-Type: application/x-www-form-urlencoded\r\n", "p=ok");
        CHECK(r.status == 200 && hdr(&r, "X-Real: 1") && has(&r, "sum=5 m=POST q=7 p=ok"), "real php-cgi (%s): %d %s", php, r.status, r.body);
        rfree(&r);
        // Server-Sent Events: il primo evento arriva subito, prima che lo script finisca
        put(www, "sse.php", "<?php header('Content-Type: text/event-stream'); header('Cache-Control: no-cache');"
                            "while (ob_get_level()) ob_end_flush(); echo \"data: uno\\n\\n\"; flush(); usleep(800000); echo \"data: due\\n\\n\";");
        {
            int fd = connect_to(sv_server_port(s));
            const char* q = "GET /sse.php HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            wr(fd, q, strlen(q));
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            char buf[4096];
            size_t have = 0;
            while (have < sizeof(buf) - 1) {
                ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
                if (g <= 0) break;
                have += (size_t)g;
                buf[have] = 0;
                if (strstr(buf, "data: uno")) break;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            CHECK(strstr(buf, "data: uno") && !strstr(buf, "data: due") && ms < 600 && strcasestr(buf, "Transfer-Encoding: chunked") &&
                  strcasestr(buf, "text/event-stream"), "SSE first event streamed (%.0f ms): %s", ms, buf);
            for (;;) { ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0); if (g <= 0) break; have += (size_t)g; }
            buf[have] = 0;
            CHECK(strstr(buf, "data: due") && strstr(buf, "\r\n0\r\n\r\n"), "SSE second event and end: %s", buf);
            close(fd);
        }
        stop(s, th);
        printf("(real php-cgi tested: %s)\n", php);
    } else printf("(php-cgi not installed: real PHP test skipped)\n");

    db_shutdown();
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) {}
    if (g_fail) { printf("php tests: %d of %d checks FAILED\n", g_fail, g_count); return 1; }
    printf("OK: %d/%d PHP checks passed\n", g_count, g_count);
    return 0;
}
