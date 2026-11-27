// HTTP/2 a livello di frame (P2.11): h2_serve pilotato da una socketpair, con frame scritti a mano.
// Intestazioni non valide chiudono lo stream (RST_STREAM PROTOCOL_ERROR); un fiume di RST_STREAM o PING
// chiude la connessione (GOAWAY ENHANCE_YOUR_CALM).
#include "src/server/h2.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "harness.h"



// ── lato server: l'I/O di h2_serve su un capo della socketpair ──
static int io_recv(void* ssl, char* buf, int n) {
    int fd = *(int*)ssl;
    ssize_t r = recv(fd, buf, (size_t)n, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}
static bool io_send(void* ssl, tls_sock fd, const char* p, size_t n) {
    (void)fd;
    int s = *(int*)ssl;
    while (n) { ssize_t w = send(s, p, n, MSG_NOSIGNAL); if (w <= 0) return false; p += w; n -= (size_t)w; }
    return true;
}
static int io_pending(void* ssl) { (void)ssl; return 0; }

typedef struct { int fd; atomic_int stop; } Srv;
static void* serve(void* a) {
    Srv* s = a;
    H2Io io = { &s->fd, (tls_sock)s->fd, io_recv, io_send, io_pending };
    h2_serve(&io, 1, "SVTEST\n", &s->stop);              // porta interna inesistente: non serve qui
    shutdown(s->fd, SHUT_RDWR);
    return NULL;
}

// ── lato client ──
static void frame(int fd, int type, int flags, uint32_t id, const void* p, size_t n) {
    uint8_t h[9] = { (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)type, (uint8_t)flags,
                     (uint8_t)(id >> 24 & 0x7f), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    if (send(fd, h, 9, MSG_NOSIGNAL) != 9) return;
    if (n && send(fd, p, n, MSG_NOSIGNAL) != (ssize_t)n) return;
}

// HPACK: campo letterale senza indice, nome nuovo, niente Huffman (nomi e valori < 127 byte)
static size_t lit(uint8_t* b, const char* name, const char* value) {
    size_t k = 0, nl = strlen(name), vl = strlen(value);
    b[k++] = 0x00;
    b[k++] = (uint8_t)nl; memcpy(b + k, name, nl); k += nl;
    b[k++] = (uint8_t)vl; memcpy(b + k, value, vl); k += vl;
    return k;
}

// legge frame fino a trovarne uno del tipo voluto (per lo stream id, se id != 0); codice d'errore in *code
static bool wait_frame(int fd, int want, uint32_t id, uint32_t* code) {
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        uint8_t h[9];
        size_t got = 0;
        while (got < 9) { ssize_t r = recv(fd, h + got, 9 - got, 0); if (r <= 0) return false; got += (size_t)r; }
        size_t n = (size_t)h[0] << 16 | (size_t)h[1] << 8 | h[2];
        uint32_t sid = ((uint32_t)h[5] & 0x7f) << 24 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 8 | h[8];
        uint8_t* p = malloc(n ? n : 1);
        got = 0;
        while (got < n) { ssize_t r = recv(fd, p + got, n - got, 0); if (r <= 0) { free(p); return false; } got += (size_t)r; }
        bool hit = h[3] == want && (!id || sid == id);
        if (hit && code) {
            const uint8_t* c = want == 7 ? p + 4 : p;            // GOAWAY: last-stream-id, poi il codice
            *code = n >= (want == 7 ? 8u : 4u) ? (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3] : 0xFFFFFFFF;
        }
        free(p);
        if (hit) return true;
    }
}

static int start(Srv* s, pthread_t* th) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    s->fd = sv[0];
    atomic_store(&s->stop, 0);
    pthread_create(th, NULL, serve, s);
    const char* pre = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (send(sv[1], pre, strlen(pre), MSG_NOSIGNAL) < 0) return -1;
    frame(sv[1], 4, 0, 0, NULL, 0);                      // SETTINGS vuoto
    return sv[1];
}

static void finish(Srv* s, pthread_t th, int cfd) {
    atomic_store(&s->stop, 1);
    shutdown(cfd, SHUT_RDWR);
    pthread_join(th, NULL);
    close(cfd);
    close(s->fd);
}

// richiesta con un'intestazione in più; true = lo stream è stato annullato con PROTOCOL_ERROR
static bool rejected(const char* name, const char* value, bool pseudo_after) {
    Srv s;
    pthread_t th;
    int c = start(&s, &th);
    uint8_t b[512];
    size_t k = 0;
    k += lit(b + k, ":method", "GET");
    k += lit(b + k, ":scheme", "https");
    if (!pseudo_after) k += lit(b + k, ":path", "/");
    k += lit(b + k, name, value);
    if (pseudo_after) k += lit(b + k, ":path", "/");
    frame(c, 1, 0x4 | 0x1, 1, b, k);                     // HEADERS, END_HEADERS | END_STREAM
    uint32_t code = 0;
    bool rst = wait_frame(c, 3, 1, &code);
    finish(&s, th, c);
    return rst && code == 1;
}

int main(void) {
    test_enter_tmpdir();                                  // cartella privata (CR-M9)
    h2_set_enabled(true);
    CHECK(rejected("X-Upper", "1", false), "upper-case header name refused");
    CHECK(rejected("bad name", "1", false), "header name with a space refused");
    CHECK(rejected("connection", "keep-alive", false), "connection-specific field refused");
    CHECK(rejected("te", "gzip", false), "te other than trailers refused");
    CHECK(rejected(":method", "POST", false), "duplicated pseudo-header refused");
    CHECK(rejected(":protocol", "x", false), "unknown pseudo-header refused");
    CHECK(rejected("accept", "*/*", true), "pseudo-header after a regular one refused");

    // rapid reset / fiume di PING: GOAWAY ENHANCE_YOUR_CALM
    {
        Srv s;
        pthread_t th;
        int c = start(&s, &th);
        uint8_t b[256];
        size_t k = 0;
        k += lit(b + k, ":method", "GET");
        k += lit(b + k, ":scheme", "https");
        k += lit(b + k, ":path", "/");
        uint8_t cancel[4] = { 0, 0, 0, 8 };
        for (uint32_t i = 0; i < 400; i++) {
            frame(c, 1, 0x4 | 0x1, 2 * i + 1, b, k);       // HEADERS ...
            frame(c, 3, 0, 2 * i + 1, cancel, 4);          // ... e subito RST_STREAM CANCEL
        }
        uint32_t code = 0;
        CHECK(wait_frame(c, 7, 0, &code) && code == 11, "rapid reset loop answered with GOAWAY(ENHANCE_YOUR_CALM): %u", code);
        finish(&s, th, c);
    }
    {
        Srv s;
        pthread_t th;
        int c = start(&s, &th);
        uint8_t ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        for (int i = 0; i < 400; i++) frame(c, 6, 0, 0, ping, 8);
        uint32_t code = 0;
        CHECK(wait_frame(c, 7, 0, &code) && code == 11, "PING flood answered with GOAWAY(ENHANCE_YOUR_CALM): %u", code);
        finish(&s, th, c);
    }
    if (g_fail) { printf("h2 tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d h2 checks passed\n", g_tests, g_tests);
    return 0;
}
