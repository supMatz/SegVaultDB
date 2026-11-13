// Test di HTTPS e HTTP/2 (src/server/tls.c, h2.c). Il client è curl (HTTP/2 vero, verifica dei
// certificati con la CA locale): senza curl o senza OpenSSL il test viene saltato.
//   make test_tls && ./test_tls
#include "src/server/server.h"
#include "src/bridge/db_api.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int g_fail = 0, g_count = 0;
#define CHECK(cond, ...) do { g_count++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_ca[600];

// esegue curl e ne raccoglie l'uscita
static char* run(const char* args) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 20 --cacert '%s' %s 2>/dev/null", g_ca, args);
    FILE* p = popen(cmd, "r");
    if (!p) return strdup("");
    size_t cap = 1 << 16, n = 0;
    char* out = malloc(cap);
    size_t got;
    while ((got = fread(out + n, 1, cap - n - 1, p)) > 0) {
        n += got;
        if (cap - n < 4096) { cap *= 2; out = realloc(out, cap); }
    }
    out[n] = 0;
    pclose(p);
    return out;
}

static int count(const char* s, const char* w) {
    int k = 0;
    for (const char* p = s; (p = strstr(p, w)); p += strlen(w)) k++;
    return k;
}

static void put(const char* path, const char* content) {
    FILE* f = fopen(path, "wb");
    fputs(content, f);
    fclose(f);
}

static void* run_server(void* s) { sv_server_run((SvServer*)s); return NULL; }

int main(void) {
    if (system("curl --version 2>/dev/null | grep -q HTTP2") != 0) { printf("test_tls: curl with HTTP/2 not found, skipped\n"); return 0; }
    char tmp[] = "/tmp/sv_tls_XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    char data[300], www[300], certs[300], p[600];
    snprintf(data, sizeof(data), "%s/data", tmp);
    snprintf(www, sizeof(www), "%s/www", tmp);
    snprintf(certs, sizeof(certs), "%s/certs", tmp);
    mkdir(data, 0755);
    mkdir(www, 0755);
    if (!db_init(data)) { printf("db_init failed\n"); return 1; }
    snprintf(p, sizeof(p), "%s/shop", www); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/shop/index.html", www); put(p, "shop home");
    snprintf(p, sizeof(p), "%s/shop/yes.txt", www); put(p, "https seen");
    snprintf(p, sizeof(p), "%s/shop/no.txt", www); put(p, "plain http");
    snprintf(p, sizeof(p), "%s/shop/.htaccess", www);
    put(p, "RewriteEngine On\nRewriteCond %{HTTPS} on\nRewriteRule ^check$ yes.txt [L]\nRewriteRule ^check$ no.txt [L]\n");
    for (int i = 0; i < 120; i++) {
        char c[64];
        snprintf(p, sizeof(p), "%s/shop/m%d.css", www, i);
        snprintf(c, sizeof(c), "body{order:%d}", i);
        put(p, c);
    }
    // file grande (oltre le finestre di flusso iniziali di HTTP/2)
    snprintf(p, sizeof(p), "%s/shop/big.bin", www);
    FILE* f = fopen(p, "wb");
    for (int i = 0; i < 3000000; i++) fputc((i * 7 + i / 1000) & 0xff, f);
    fclose(f);

    SvServerConfig cfg = { .port = 0, .static_dir = www, .quiet = true, .https = true, .https_port = 0, .cert_dir = certs, .http2 = true };
    char err[300];
    SvServer* s = sv_server_start(&cfg, err, sizeof(err));
    if (!s && strstr(err, "OpenSSL")) { printf("test_tls: %s, skipped\n", err); return 0; }
    if (!s) { printf("server: %s\n", err); return 1; }
    snprintf(g_ca, sizeof(g_ca), "%s/ca.crt", certs);
    struct stat st;
    CHECK(stat(g_ca, &st) == 0, "CA created");
    char key[600];
    snprintf(key, sizeof(key), "%s/ca.key", certs);
    CHECK(stat(key, &st) == 0 && (st.st_mode & 077) == 0, "CA key private: %o", st.st_mode & 0777);
    pthread_t th;
    pthread_create(&th, NULL, run_server, s);
    int hp = sv_server_https_port(s), port = sv_server_port(s);
    CHECK(hp > 0, "https port");
    char a[2048];
    char* o;

    // certificato del sito (SNI) verificato con la CA, HTTP/2
    snprintf(a, sizeof(a), "-w ' %%{http_version} %%{ssl_verify_result}' https://shop.localhost:%d/", hp);
    o = run(a);
    CHECK(strstr(o, "shop home 2 0"), "h2 + verified certificate: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "--http1.1 -w ' %%{http_version}' https://shop.localhost:%d/ https://shop.localhost:%d/yes.txt", hp, hp);
    o = run(a);
    CHECK(strstr(o, "shop home 1.1") && strstr(o, "https seen 1.1"), "HTTP/1.1 over TLS, keep-alive: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "-w ' %%{http_version}' https://localhost:%d/api/v1/health", hp);
    o = run(a);
    CHECK(strstr(o, "\"server\":\"SegVault\"") && strstr(o, " 2"), "localhost certificate, API over h2: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "https://127.0.0.1:%d/api/v1/health", hp);
    o = run(a);
    CHECK(strstr(o, "SegVault"), "IP address in the certificate: [%s]", o);
    free(o);

    // il sito sa che è HTTPS (%{HTTPS} nel .htaccess); su HTTP no
    snprintf(a, sizeof(a), "https://shop.localhost:%d/check", hp);
    o = run(a);
    CHECK(!strcmp(o, "https seen"), "HTTPS flag over h2: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "--http1.1 https://shop.localhost:%d/check", hp);
    o = run(a);
    CHECK(!strcmp(o, "https seen"), "HTTPS flag over h1: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "http://shop.localhost:%d/check", port);
    o = run(a);
    CHECK(!strcmp(o, "plain http"), "plain HTTP: [%s]", o);
    free(o);

    // molte richieste insieme su una connessione
    {
        char* big = malloc(200000);
        int k = snprintf(big, 200000, "--parallel --parallel-max 100 -w '[%%{http_version}]'");
        for (int i = 0; i < 120; i++) k += snprintf(big + k, 200000 - (size_t)k, " https://shop.localhost:%d/m%d.css", hp, i);
        o = run(big);
        CHECK(count(o, "body{order:") == 120 && count(o, "[2]") == 120, "120 parallel h2 requests: %d %d", count(o, "body{order:"), count(o, "[2]"));
        free(o);
        free(big);
    }
    // file grande: finestre di flusso
    {
        char out[400];
        snprintf(out, sizeof(out), "%s/dl.bin", tmp);
        snprintf(a, sizeof(a), "-o '%s' -w '%%{size_download}' https://shop.localhost:%d/big.bin", out, hp);
        o = run(a);
        CHECK(!strcmp(o, "3000000"), "3 MB download: [%s]", o);
        free(o);
        char cmd[1300];
        snprintf(cmd, sizeof(cmd), "cmp -s '%s' '%s/shop/big.bin'", out, www);
        CHECK(system(cmd) == 0, "download identical");
        snprintf(a, sizeof(a), "-r 1000-1009 -w ' %%{http_code}' -o /dev/null https://shop.localhost:%d/big.bin", hp);
        o = run(a);
        CHECK(strstr(o, "206"), "Range over h2: [%s]", o);
        free(o);
    }
    // corpi delle richieste: con Content-Length e senza (chunked verso il server interno)
    snprintf(a, sizeof(a), "-H 'Content-Type: application/json' -d '{\"sql\":\"SELECT 40 + 2 AS x\"}' https://localhost:%d/api/v1/query", hp);
    o = run(a);
    CHECK(strstr(o, "42"), "POST over h2: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "-H 'Content-Type: application/json' -H 'Transfer-Encoding:' -T - https://localhost:%d/api/v1/query <<'EOF'\n{\"sql\":\"SELECT 'ok' AS x\"}\nEOF\n", hp);
    o = run(a);
    CHECK(strstr(o, "\"ok\""), "body without length over h2: [%s]", o);
    free(o);
    // HEAD, 304, 404
    snprintf(a, sizeof(a), "-I https://shop.localhost:%d/big.bin", hp);
    o = run(a);
    CHECK(strstr(o, "HTTP/2 200") && strstr(o, "content-length: 3000000"), "HEAD: [%s]", o);
    free(o);
    snprintf(a, sizeof(a), "-o /dev/null -w '%%{http_code}' https://shop.localhost:%d/nope", hp);
    o = run(a);
    CHECK(!strcmp(o, "404"), "404: [%s]", o);
    free(o);
    // cruscotto
    snprintf(a, sizeof(a), "https://localhost:%d/.segvault/", hp);
    o = run(a);
    CHECK(strstr(o, "HTTPS + HTTP/2"), "dashboard: %.200s", o);
    free(o);

    // la riga segreta del ponte: sbagliata, la connessione si chiude senza rispondere
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(fd, (struct sockaddr*)&sa, sizeof(sa));
        struct timeval tv = { 3, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const char* q = "SVTLS wrongsecret 6.6.6.6 1\r\nGET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
        if (send(fd, q, strlen(q), 0)) {}
        char buf[256];
        ssize_t g = recv(fd, buf, sizeof(buf), 0);
        CHECK(g == 0, "forged TLS marker refused: %zd", g);
        close(fd);
    }
    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);

    // senza HTTP/2; la CA resta la stessa
    cfg.http2 = false;
    s = sv_server_start(&cfg, err, sizeof(err));
    pthread_create(&th, NULL, run_server, s);
    hp = sv_server_https_port(s);
    snprintf(a, sizeof(a), "-w ' %%{http_version}' https://shop.localhost:%d/", hp);
    o = run(a);
    CHECK(strstr(o, "shop home 1.1"), "--no-http2 and the same CA after a restart: [%s]", o);
    free(o);
    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);

    db_shutdown();
    char rm[400];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmp);
    if (system(rm)) {}
    printf("test_tls: %d checks, %d failed\n", g_count, g_fail);
    return g_fail ? 1 : 0;
}
