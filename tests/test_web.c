// Test del server web come ambiente di sviluppo: siti per nome (virtual host automatici e --site),
// cruscotto, elenco dei file e reverse proxy verso un dev server (con WebSocket).
//   make test_web && ./test_web
#include "src/server/server.h"
#include "src/bridge/db_api.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int g_fail = 0, g_count = 0;
#define CHECK(cond, ...) do { g_count++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void wr(int fd, const char* p, size_t n) {
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return; p += w; n -= (size_t)w; }
}

// legge una risposta HTTP completa (Content-Length, chunked decodificato o fino alla chiusura)
typedef struct { int status; char head[8192]; char* body; size_t len; } Resp;

static bool read_resp(int fd, Resp* r, bool head_only) {
    memset(r, 0, sizeof(*r));
    char buf[65536];
    buf[0] = 0;
    size_t have = 0;
    char* he = NULL;
    while (!(he = strstr(buf, "\r\n\r\n"))) {
        ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (g <= 0) return false;
        have += (size_t)g;
        buf[have] = 0;
    }
    size_t hl = (size_t)(he - buf) + 4;
    snprintf(r->head, sizeof(r->head), "%.*s", (int)hl, buf);
    r->status = atoi(buf + 9);
    size_t extra = have - hl;
    char* rest = buf + hl;
    const char* cl = strcasestr(r->head, "\r\nContent-Length:");
    bool chunked = strcasestr(r->head, "Transfer-Encoding: chunked") != NULL;
    size_t cap = 1 << 20;
    r->body = malloc(cap);
    if (head_only || r->status == 304 || r->status == 204 || r->status == 101) {
        r->body[0] = 0;
        return true;
    }
    if (cl && !chunked) {
        size_t want = (size_t)atol(cl + 17);
        memcpy(r->body, rest, extra);
        r->len = extra;
        while (r->len < want) { ssize_t g = recv(fd, r->body + r->len, want - r->len, 0); if (g <= 0) break; r->len += (size_t)g; }
    } else if (chunked) {
        // raccoglie tutto il grezzo finché non arriva il pezzo finale, poi decodifica
        char* raw = malloc(cap);
        size_t rl = extra;
        memcpy(raw, rest, extra);
        raw[rl] = 0;
        while (!strstr(raw, "\r\n0\r\n\r\n") && strncmp(raw, "0\r\n\r\n", 5)) {
            ssize_t g = recv(fd, raw + rl, cap - 1 - rl, 0);
            if (g <= 0) break;
            rl += (size_t)g;
            raw[rl] = 0;
        }
        char* p = raw;
        for (;;) {
            size_t n = strtoul(p, &p, 16);
            p = strstr(p, "\r\n") + 2;
            if (!n) break;
            memcpy(r->body + r->len, p, n);
            r->len += n;
            p += n + 2;
        }
        free(raw);
    } else {
        memcpy(r->body, rest, extra);
        r->len = extra;
        for (;;) { ssize_t g = recv(fd, r->body + r->len, cap - 1 - r->len, 0); if (g <= 0) break; r->len += (size_t)g; }
    }
    r->body[r->len] = 0;
    return true;
}

static Resp get(int port, const char* host, const char* path) {
    Resp r = {0};
    int fd = connect_to(port);
    char q[2048];
    int n = snprintf(q, sizeof(q), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    wr(fd, q, (size_t)n);
    if (!read_resp(fd, &r, false)) r.status = -1;
    close(fd);
    if (!r.body) { r.body = malloc(1); r.body[0] = 0; }
    return r;
}

static bool has(const Resp* r, const char* s) { return r->body && strstr(r->body, s); }

static void put(const char* path, const char* content) {
    FILE* f = fopen(path, "wb");
    fputs(content, f);
    fclose(f);
}

// ── finto dev server (il backend del proxy) ──

static int g_backend_port;
static char g_last_head[8192];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static void backend_conn(int fd) {
    char buf[65536];
    size_t have = 0;
    char* he;
    buf[0] = 0;
    while (!(he = strstr(buf, "\r\n\r\n"))) {
        ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (g <= 0) { close(fd); return; }
        have += (size_t)g;
        buf[have] = 0;
    }
    size_t hl = (size_t)(he - buf) + 4;
    pthread_mutex_lock(&g_mx);
    snprintf(g_last_head, sizeof(g_last_head), "%.*s", (int)hl, buf);
    pthread_mutex_unlock(&g_mx);
    char path[1024];
    sscanf(buf, "%*s %1023s", path);
    const char* cl = strcasestr(buf, "\r\nContent-Length:");
    size_t blen = cl ? (size_t)atol(cl + 17) : 0;
    while (have < hl + blen) { ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0); if (g <= 0) break; have += (size_t)g; }
    buf[have] = 0;
    char out[70000];
    int n;
    if (!strcmp(path, "/hello")) {
        n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\nConnection: keep-alive\r\nX-Backend: yes\r\n\r\nhello world");
        wr(fd, out, (size_t)n);
    } else if (!strcmp(path, "/chunked")) {
        n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nuno, \r\n3;ext=1\r\ndue\r\n0\r\nX-Trailer: t\r\n\r\n");
        wr(fd, out, (size_t)n);
        // un backend che ignora "Connection: close": il proxy deve capire da solo dove finisce
        usleep(300000);
    } else if (!strcmp(path, "/eof")) {
        n = snprintf(out, sizeof(out), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nfino alla chiusura");
        wr(fd, out, (size_t)n);
    } else if (!strcmp(path, "/echo")) {
        n = snprintf(out, sizeof(out), "HTTP/1.1 201 Created\r\nContent-Length: %zu\r\n\r\n%s", have - hl, buf + hl);
        wr(fd, out, (size_t)n);
    } else if (!strcmp(path, "/big")) {
        n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n");
        wr(fd, out, (size_t)n);
        memset(out, 'x', 50000);
        for (int i = 0; i < 20; i++) wr(fd, out, 50000);
    } else if (!strcmp(path, "/continue")) {
        n = snprintf(out, sizeof(out), "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n");
        wr(fd, out, (size_t)n);
    } else if (!strcmp(path, "/ws")) {
        n = snprintf(out, sizeof(out), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
        wr(fd, out, (size_t)n);
        if (have > hl) wr(fd, buf + hl, have - hl);           // byte arrivati insieme alla richiesta
        for (;;) {                                            // eco
            ssize_t g = recv(fd, out, sizeof(out), 0);
            if (g <= 0) break;
            if (g >= 4 && !memcmp(out, "quit", 4)) break;
            wr(fd, out, (size_t)g);
        }
    } else {
        n = snprintf(out, sizeof(out), "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found");
        wr(fd, out, (size_t)n);
    }
    close(fd);
}

static void* backend_conn_thread(void* a) { backend_conn((int)(intptr_t)a); return NULL; }

static void* backend_main(void* a) {
    int lfd = (int)(intptr_t)a;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) return NULL;
        pthread_t t;
        pthread_create(&t, NULL, backend_conn_thread, (void*)(intptr_t)fd);
        pthread_detach(t);
    }
}

static int start_backend(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (struct sockaddr*)&a, sizeof(a));
    listen(lfd, 16);
    socklen_t al = sizeof(a);
    getsockname(lfd, (struct sockaddr*)&a, &al);
    pthread_t t;
    pthread_create(&t, NULL, backend_main, (void*)(intptr_t)lfd);
    pthread_detach(t);
    return ntohs(a.sin_port);
}

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr*)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(fd, (struct sockaddr*)&a, &al);
    close(fd);
    return ntohs(a.sin_port);
}

static void* run_server(void* s) { sv_server_run((SvServer*)s); return NULL; }

int main(void) {
    char tmp[] = "/tmp/sv_web_XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    char data[300], www[300], p[600];
    snprintf(data, sizeof(data), "%s/data", tmp);
    snprintf(www, sizeof(www), "%s/www", tmp);
    mkdir(data, 0755);
    mkdir(www, 0755);
    if (!db_init(data)) { printf("db_init failed\n"); return 1; }

    // siti: blog (index.html), lara (Laravel: public/index.html), files (nessun index), other (cartella esterna)
    snprintf(p, sizeof(p), "%s/blog", www); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/blog/index.html", www); put(p, "<h1>blog home</h1>");
    snprintf(p, sizeof(p), "%s/blog/style.css", www); put(p, "body{color:red}");
    snprintf(p, sizeof(p), "%s/lara", www); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/lara/artisan", www); put(p, "#!/usr/bin/env php");
    snprintf(p, sizeof(p), "%s/lara/.env", www); put(p, "DB_PASSWORD=segreta");
    snprintf(p, sizeof(p), "%s/lara/public", www); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/lara/public/index.html", www); put(p, "laravel public");
    snprintf(p, sizeof(p), "%s/files", www); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/files/report <1>.txt", www); put(p, "dati");
    snprintf(p, sizeof(p), "%s/files/sub", www); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/files/.hidden", www); put(p, "x");
    snprintf(p, sizeof(p), "%s/other", tmp); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/other/index.html", tmp); put(p, "external site");

    int bport = start_backend();
    g_backend_port = bport;
    char other[400], bk[64], dead[64];
    snprintf(other, sizeof(other), "%s/other", tmp);
    snprintf(bk, sizeof(bk), "http://127.0.0.1:%d", bport);
    snprintf(dead, sizeof(dead), "%d", free_port());
    SvSite sites[] = { { "ext", other }, { "app", bk }, { "Shop.Example.Com", other }, { "down", dead } };
    SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .dir_listing = true, .sites = sites, .n_sites = 4 };
    char err[300];
    SvServer* s = sv_server_start(&cfg, err, sizeof(err));
    if (!s) { printf("server: %s\n", err); return 1; }
    pthread_t th;
    pthread_create(&th, NULL, run_server, s);
    int port = sv_server_port(s);
    Resp r;

    // ── siti automatici ──
    r = get(port, "blog.localhost", "/");
    CHECK(r.status == 200 && has(&r, "blog home"), "blog.localhost: %d %s", r.status, r.body);
    free(r.body);
    r = get(port, "BLOG.localhost:8080", "/style.css");
    CHECK(r.status == 200 && has(&r, "color:red"), "host case and port: %d", r.status);
    free(r.body);
    r = get(port, "api.blog.localhost", "/");
    CHECK(r.status == 200 && has(&r, "blog home"), "subdomain of a site: %d", r.status);
    free(r.body);
    r = get(port, "blog.test", "/");
    CHECK(r.status == 200 && has(&r, "blog home"), "blog.test: %d", r.status);
    free(r.body);
    r = get(port, "lara.localhost", "/");
    CHECK(r.status == 200 && has(&r, "laravel public"), "Laravel public/ as root: %d %s", r.status, r.body);
    free(r.body);
    r = get(port, "lara.localhost", "/../.env");
    CHECK(r.status == 404 && !has(&r, "segreta"), ".env outside public/: %d", r.status);
    free(r.body);
    r = get(port, "nosuchsite.localhost", "/blog/");
    CHECK(r.status == 200 && has(&r, "blog home"), "unknown site falls back to the web folder: %d", r.status);
    free(r.body);
    r = get(port, "localhost", "/blog/style.css");
    CHECK(r.status == 200, "plain localhost still serves the web folder: %d", r.status);
    free(r.body);
    r = get(port, "x..localhost", "/");
    CHECK(r.status == 200 && has(&r, "SegVault"), "odd host name: %d", r.status);
    free(r.body);

    // ── siti con nome ──
    r = get(port, "ext.localhost", "/");
    CHECK(r.status == 200 && has(&r, "external site"), "--site NAME=DIR: %d", r.status);
    free(r.body);
    r = get(port, "shop.example.com", "/");
    CHECK(r.status == 200 && has(&r, "external site"), "full host name: %d", r.status);
    free(r.body);

    // ── cruscotto ed elenco dei file ──
    r = get(port, "localhost", "/");
    CHECK(r.status == 200 && has(&r, "blog.localhost:") && has(&r, "Laravel") && has(&r, "lara/public") &&
          has(&r, "http://127.0.0.1:") && has(&r, ">app.localhost"), "dashboard: %d %.300s", r.status, r.body);
    free(r.body);
    r = get(port, "blog.localhost", "/.segvault/");
    CHECK(r.status == 200 && has(&r, "Services"), "dashboard on /.segvault/: %d", r.status);
    free(r.body);
    r = get(port, "files.localhost", "/");
    CHECK(r.status == 200 && has(&r, "report%20%3C1%3E.txt") && has(&r, "report &lt;1&gt;.txt") && has(&r, "sub/") && !has(&r, ".hidden"),
          "directory listing: %d %s", r.status, r.body);
    free(r.body);
    r = get(port, "files.localhost", "/sub");
    CHECK(r.status == 301, "folder redirect: %d", r.status);
    free(r.body);

    // ── reverse proxy ──
    r = get(port, "app.localhost", "/hello");
    CHECK(r.status == 200 && !strcmp(r.body, "hello world") && strcasestr(r.head, "X-Backend: yes") &&
          strcasestr(r.head, "Connection: close"), "proxy: %d [%s] %s", r.status, r.body, r.head);
    free(r.body);
    pthread_mutex_lock(&g_mx);
    CHECK(strcasestr(g_last_head, "Host: app.localhost") && strcasestr(g_last_head, "X-Forwarded-For: 127.0.0.1") &&
          strcasestr(g_last_head, "Connection: close") && strcasestr(g_last_head, "X-Forwarded-Proto: http"), "forwarded headers: %s", g_last_head);
    pthread_mutex_unlock(&g_mx);
    r = get(port, "app.localhost", "/api/v1/health");
    CHECK(r.status == 404 && has(&r, "not found") && !has(&r, "SegVault"), "the whole path space belongs to the app: %d %s", r.status, r.body);
    free(r.body);
    r = get(port, "app.localhost", "/eof");
    CHECK(r.status == 200 && has(&r, "fino alla chiusura"), "body until close: %d %s", r.status, r.body);
    free(r.body);
    r = get(port, "app.localhost", "/big");
    CHECK(r.status == 200 && r.len == 1000000, "large body: %d %zu", r.status, r.len);
    free(r.body);
    r = get(port, "app.localhost", "/continue");
    CHECK(r.status == 204, "1xx skipped: %d", r.status);
    free(r.body);

    {   // keep-alive: Content-Length, chunked (backend che non chiude), POST con corpo, sulla stessa connessione
        int fd = connect_to(port);
        const char* q1 = "GET /hello HTTP/1.1\r\nHost: app.localhost\r\n\r\n";
        wr(fd, q1, strlen(q1));
        CHECK(read_resp(fd, &r, false) && r.status == 200 && !strcmp(r.body, "hello world") && strcasestr(r.head, "Connection: keep-alive"),
              "keep-alive 1: %d %s", r.status, r.head);
        free(r.body);
        const char* q2 = "GET /chunked HTTP/1.1\r\nHost: app.localhost\r\n\r\n";
        wr(fd, q2, strlen(q2));
        CHECK(read_resp(fd, &r, false) && r.status == 200 && !strcmp(r.body, "uno, due"), "chunked: %d [%s]", r.status, r.body);
        free(r.body);
        const char* q3 = "POST /echo HTTP/1.1\r\nHost: app.localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nciao\r\n6\r\n mondo\r\n0\r\n\r\n";
        wr(fd, q3, strlen(q3));
        CHECK(read_resp(fd, &r, false) && r.status == 201 && !strcmp(r.body, "ciao mondo"), "POST body (chunked in, length out): %d [%s]", r.status, r.body);
        free(r.body);
        const char* q4 = "HEAD /hello HTTP/1.1\r\nHost: app.localhost\r\n\r\n";
        wr(fd, q4, strlen(q4));
        CHECK(read_resp(fd, &r, true) && r.status == 200, "HEAD: %d", r.status);
        free(r.body);
        wr(fd, q1, strlen(q1));
        CHECK(read_resp(fd, &r, false) && r.status == 200 && !strcmp(r.body, "hello world"), "after HEAD: %d [%s]", r.status, r.body);
        free(r.body);
        close(fd);
    }
    {   // WebSocket: 101 e poi byte nei due sensi (anche quelli mandati insieme alla richiesta)
        int fd = connect_to(port);
        const char* q = "GET /ws HTTP/1.1\r\nHost: app.localhost\r\nConnection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\nearly";
        wr(fd, q, strlen(q));
        char buf[4096];
        size_t have = 0;
        while (have < sizeof(buf) - 1) {
            ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
            if (g <= 0) break;
            have += (size_t)g;
            buf[have] = 0;
            if (strstr(buf, "early")) break;
        }
        buf[have] = 0;
        CHECK(strstr(buf, "101 Switching") && strcasestr(buf, "Upgrade: websocket") && strstr(buf, "\r\n\r\nearly"), "upgrade: %s", buf);
        pthread_mutex_lock(&g_mx);
        CHECK(strcasestr(g_last_head, "Connection: Upgrade") && strcasestr(g_last_head, "Upgrade: websocket") &&
              strcasestr(g_last_head, "Sec-WebSocket-Key: dGhl"), "upgrade headers forwarded: %s", g_last_head);
        pthread_mutex_unlock(&g_mx);
        wr(fd, "ping-123", 8);
        ssize_t g = recv(fd, buf, sizeof(buf) - 1, 0);
        buf[g > 0 ? g : 0] = 0;
        CHECK(!strcmp(buf, "ping-123"), "websocket echo: [%s]", buf);
        wr(fd, "quit", 4);
        g = recv(fd, buf, sizeof(buf), 0);
        CHECK(g == 0, "closed with the backend: %zd", g);
        close(fd);
    }
    r = get(port, "down.localhost", "/");
    CHECK(r.status == 502 && has(&r, "not answering") && has(&r, "npm run dev"), "dev server down: %d %s", r.status, r.body);
    free(r.body);

    // ── API: un sito *.localhost è un'origine locale ──
    {
        int fd = connect_to(port);
        const char* q = "GET /api/v1/health HTTP/1.1\r\nHost: blog.localhost\r\nConnection: close\r\n\r\n";
        wr(fd, q, strlen(q));
        CHECK(read_resp(fd, &r, false) && r.status == 200 && strstr(r.body, "SegVault"), "API from blog.localhost: %d", r.status);
        free(r.body);
        close(fd);
    }

    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);

    // ── senza elenco dei file e senza siti automatici ──
    SvServerConfig c2 = { .port = 0, .static_dir = www, .quiet = true, .no_auto_sites = true };
    s = sv_server_start(&c2, err, sizeof(err));
    pthread_create(&th, NULL, run_server, s);
    port = sv_server_port(s);
    r = get(port, "localhost", "/files/");
    CHECK(r.status == 404, "listing off: %d", r.status);
    free(r.body);
    r = get(port, "blog.localhost", "/");
    CHECK(r.status == 200 && has(&r, "SegVault") && !has(&r, "blog home"), "auto sites off: %d", r.status);
    free(r.body);
    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);

    // porte occupate: tutte in un messaggio, con il programma che le usa
    {
        SvServerConfig c4 = { .port = 0, .quiet = true };
        SvServer* busy = sv_server_start(&c4, err, sizeof(err));
        int bp = sv_server_port(busy);
        SvServerConfig c5 = { .port = bp, .quiet = true, .mail = true, .mail_port = bp, .mail_dir = tmp };
        SvServer* x = sv_server_start(&c5, err, sizeof(err));
        char want[64];
        snprintf(want, sizeof(want), "port %d (web) is in use by test_web", bp);
        CHECK(!x && strstr(err, want) && strstr(err, "(mail catcher) is in use"), "busy ports reported with their owner: %s", err);
        char who[160];
        CHECK(sv_port_owner(bp, who, sizeof(who)) && strstr(who, "test_web (pid "), "port owner: %s", who);
        sv_server_free(busy);
    }

    // configurazioni sbagliate
    SvSite bad1[] = { { "../x", www } };
    SvServerConfig c3 = { .port = 0, .quiet = true, .sites = bad1, .n_sites = 1 };
    CHECK(!sv_server_start(&c3, err, sizeof(err)) && strstr(err, "invalid site name"), "bad name: %s", err);
    SvSite bad2[] = { { "a", "/nonexistent/folder" } };
    c3.sites = bad2;
    CHECK(!sv_server_start(&c3, err, sizeof(err)) && strstr(err, "not found"), "bad folder: %s", err);
    SvSite bad3[] = { { "a", "http://127.0.0.1:99999" } };
    c3.sites = bad3;
    CHECK(!sv_server_start(&c3, err, sizeof(err)) && strstr(err, "invalid address"), "bad port: %s", err);

    db_shutdown();
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
    if (system(cmd)) {}
    printf("test_web: %d checks, %d failed\n", g_count, g_fail);
    return g_fail ? 1 : 0;
}
