/*
SCOPO: reverse proxy HTTP/1.1 (vedi proxy.h).
       Una connessione al backend per richiesta ("Connection: close"): la fine della risposta è
       data da Content-Length, dalla codifica chunked (seguita byte per byte, così la connessione
       del client resta riutilizzabile) o dalla chiusura del backend (allora si chiude anche quella
       del client). Con "101 Switching Protocols" i due socket vengono collegati finché uno chiude.
*/
#include "proxy.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #define BAD_SOCK INVALID_SOCKET
  #define sock_close closesocket
  #define SEND_FLAGS 0
  static double px_now(void) { return (double)GetTickCount64(); }
#else
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <time.h>
  #include <unistd.h>
  #define BAD_SOCK (-1)
  #define sock_close close
  #define SEND_FLAGS MSG_NOSIGNAL
  static double px_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
#endif

#define HEAD_MAX        (64 * 1024)
#define HEAD_WAIT_MS    (300 * 1000)    // il backend ha 5 minuti per cominciare a rispondere
#define TICK_MS         200             // ogni quanto si ricontrolla stop

bool px_parse_target(const char* t, char* host, size_t hn, int* port) {
    if (!t || !*t) return false;
    if (!strncasecmp(t, "http://", 7)) t += 7;
    else if (strstr(t, "://")) return false;                // https://, ws://: non supportati
    const char* end = t + strcspn(t, "/");
    const char* colon = NULL;
    if (*t == '[') {                                         // [::1]:3000
        const char* rb = memchr(t, ']', (size_t)(end - t));
        if (!rb) return false;
        if ((size_t)(rb - t - 1) >= hn) return false;
        snprintf(host, hn, "%.*s", (int)(rb - t - 1), t + 1);
        colon = rb + 1 < end && rb[1] == ':' ? rb + 1 : NULL;
        if (!colon) return false;
    } else {
        for (const char* q = t; q < end; q++) if (*q == ':') colon = q;
        if (!colon) {                                         // solo la porta: "3000"
            bool digits = end > t;
            for (const char* q = t; q < end; q++) if (!isdigit((unsigned char)*q)) digits = false;
            if (!digits) return false;
            snprintf(host, hn, "127.0.0.1");
            colon = t - 1;
        } else {
            if ((size_t)(colon - t) >= hn || colon == t) return false;
            snprintf(host, hn, "%.*s", (int)(colon - t), t);
        }
    }
    char* e;
    long p = strtol(colon + 1, &e, 10);
    if (e == colon + 1 || e != end || p <= 0 || p > 65535) return false;
    *port = (int)p;
    for (const char* q = host; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '.' && *q != '-' && *q != ':' && *q != '_') return false;
    return true;
}

static bool px_send(px_sock fd, const char* p, size_t n) {
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

// aspetta che fd sia leggibile: 1 sì, 0 tempo scaduto, -1 stop
static int wait_readable(px_sock fd, int ms, atomic_int* stop) {
    double t0 = px_now();
    for (;;) {
        if (stop && *stop) return -1;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        double left = ms - (px_now() - t0);
        if (left <= 0) return 0;
        int tick = left < TICK_MS ? (int)left + 1 : TICK_MS;
        struct timeval tv = { tick / 1000, (tick % 1000) * 1000 };
        int r = select((int)fd + 1, &rd, NULL, NULL, &tv);
        if (r > 0) return 1;
#ifndef _WIN32
        if (r < 0 && errno != EINTR) return 1;               // l'errore lo dirà recv
#else
        if (r < 0) return 1;
#endif
    }
}

static px_sock px_connect(const char* host, int port, char* err, size_t errsz) {
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;            // "localhost" di Node 18+ è spesso solo ::1
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        snprintf(err, errsz, "cannot resolve %s", host);
        return BAD_SOCK;
    }
    px_sock fd = BAD_SOCK;
    for (struct addrinfo* a = res; a && fd == BAD_SOCK; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd == BAD_SOCK) continue;
        if (connect(fd, a->ai_addr, (int)a->ai_addrlen) != 0) { sock_close(fd); fd = BAD_SOCK; }
    }
    // "localhost" senza risposta: si prova anche l'altra famiglia (un server solo IPv4 o solo IPv6)
    if (fd == BAD_SOCK && !strcasecmp(host, "localhost")) {
        static const char* const alt[] = { "127.0.0.1", "::1" };
        for (int i = 0; i < 2 && fd == BAD_SOCK; i++) {
            struct addrinfo* r2 = NULL;
            hints.ai_flags = AI_NUMERICHOST;
            if (getaddrinfo(alt[i], ps, &hints, &r2) == 0 && r2) {
                fd = socket(r2->ai_family, r2->ai_socktype, r2->ai_protocol);
                if (fd != BAD_SOCK && connect(fd, r2->ai_addr, (int)r2->ai_addrlen) != 0) { sock_close(fd); fd = BAD_SOCK; }
                freeaddrinfo(r2);
            }
        }
    }
    freeaddrinfo(res);
    if (fd == BAD_SOCK) { snprintf(err, errsz, "nothing is listening on %s:%d", host, port); return BAD_SOCK; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    return fd;
}

// ── corpo chunked: si segue la codifica per sapere dove finisce ──

typedef struct {
    int st;                       // 0 cifre della dimensione, 1 resto della riga, 2 dati, 3 CRLF dopo i dati,
                                  // 4 inizio di una riga dei trailer, 5 dentro una riga dei trailer
    unsigned long long left;
    int digits;
} Chunked;

// byte consumati fino alla fine del corpo (compresa), -1 se il corpo continua, -2 se non è chunked valido
static long long chunked_feed(Chunked* c, const char* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        switch (c->st) {
        case 0:
            if (isxdigit((unsigned char)ch)) {
                if (++c->digits > 15) return -2;
                c->left = c->left * 16 + (unsigned)(isdigit((unsigned char)ch) ? ch - '0' : tolower((unsigned char)ch) - 'a' + 10);
                break;
            }
            if (!c->digits) return -2;
            if (ch != '\n') { c->st = 1; break; }
            /* fallthrough */
        case 1:
            if (ch != '\n') break;
            c->st = c->left ? 2 : 4;
            break;
        case 2: {
            size_t take = (size_t)(c->left < (unsigned long long)(n - i) ? c->left : (unsigned long long)(n - i));
            i += take - 1;
            c->left -= take;
            if (!c->left) c->st = 3;
            break;
        }
        case 3:
            if (ch == '\n') { c->st = 0; c->digits = 0; c->left = 0; }
            else if (ch != '\r') return -2;
            break;
        case 4:
            if (ch == '\n') return (long long)i + 1;          // riga vuota: fine
            if (ch != '\r') c->st = 5;
            break;
        case 5:
            if (ch == '\n') c->st = 4;
            break;
        }
    }
    return -1;
}

// ── WebSocket: i due socket collegati ──

static void pipe_both(px_sock a, px_sock b, atomic_int* stop) {
    char buf[65536];
    for (;;) {
        if (*stop) return;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(a, &rd);
        FD_SET(b, &rd);
        struct timeval tv = { 0, TICK_MS * 1000 };
        int r = select((int)(a > b ? a : b) + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return;
        }
        if (r == 0) continue;
        px_sock from[2] = { a, b }, to[2] = { b, a };
        for (int k = 0; k < 2; k++) {
            if (!FD_ISSET(from[k], &rd)) continue;
            int got = (int)recv(from[k], buf, sizeof(buf), 0);
            if (got <= 0) return;
            if (!px_send(to[k], buf, (size_t)got)) return;
        }
    }
}

// ── intestazione della risposta ──

static bool header_is(const char* line, size_t len, const char* name) {
    size_t n = strlen(name);
    return len > n && line[n] == ':' && !strncasecmp(line, name, n);
}

static void header_val(const char* line, size_t len, size_t name_len, char* out, size_t outsz) {
    const char* v = line + name_len + 1;
    const char* e = line + len;
    while (v < e && (*v == ' ' || *v == '\t')) v++;
    while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    snprintf(out, outsz, "%.*s", (int)(e - v), v);
}

int px_forward(px_sock client, const char* host, int port, const char* head, size_t head_len,
               const char* body, size_t body_len, const char* early, size_t early_len,
               bool head_request, bool client_wants_keep, atomic_int* stop, PxResult* res,
               char* err, size_t errsz) {
    memset(res, 0, sizeof(*res));
    px_sock b = px_connect(host, port, err, errsz);
    if (b == BAD_SOCK) return -1;
    if (!px_send(b, head, head_len) || (body_len && !px_send(b, body, body_len)) || (early_len && !px_send(b, early, early_len))) {
        snprintf(err, errsz, "%s:%d closed the connection", host, port);
        sock_close(b);
        return -1;
    }

    // intestazione della risposta
    char* in = malloc(HEAD_MAX + 1);
    if (!in) { snprintf(err, errsz, "out of memory"); sock_close(b); return -1; }
    size_t have = 0;
    char* hend = NULL;
    for (;;) {
        int w = wait_readable(b, HEAD_WAIT_MS, stop);
        if (w <= 0) {
            snprintf(err, errsz, w == 0 ? "%s:%d did not answer in time" : "server stopping", host, port);
            free(in); sock_close(b); return -1;
        }
        int got = (int)recv(b, in + have, (int)(HEAD_MAX - have), 0);
        if (got <= 0) {
            snprintf(err, errsz, "%s:%d closed the connection without an answer", host, port);
            free(in); sock_close(b); return -1;
        }
        have += (size_t)got;
        in[have] = '\0';
        // 1xx informativi diversi da 101 (100 Continue, 103 Early Hints): si saltano
        while ((hend = strstr(in, "\r\n\r\n")) && !strncmp(in, "HTTP/1.", 7) && in[9] == '1' && strncmp(in + 9, "101", 3)) {
            size_t skip = (size_t)(hend - in) + 4;
            memmove(in, in + skip, have - skip);
            have -= skip;
            in[have] = '\0';
        }
        if (hend) break;
        if (have >= HEAD_MAX) { snprintf(err, errsz, "%s:%d sent headers that are too large", host, port); free(in); sock_close(b); return -1; }
    }
    size_t hlen = (size_t)(hend - in) + 4;
    if (strncmp(in, "HTTP/1.", 7) || have < 12) { snprintf(err, errsz, "%s:%d did not answer with HTTP", host, port); free(in); sock_close(b); return -1; }
    int status = atoi(in + 9);
    res->status = status;

    // intestazione per il client: senza le intestazioni "hop-by-hop" della connessione al backend
    bool chunked = false, has_len = false;
    long long clen = 0;
    char* out = malloc(hlen + 128);
    if (!out) { snprintf(err, errsz, "out of memory"); free(in); sock_close(b); return -1; }
    size_t ol = 0;
    const char* line = in;
    const char* eol = strstr(line, "\r\n");
    ol += (size_t)sprintf(out, "HTTP/1.1%.*s\r\n", (int)(eol - line - 8), line + 8);
    for (line = eol + 2; line < in + hlen - 2; line = eol + 2) {
        eol = strstr(line, "\r\n");
        size_t len = (size_t)(eol - line);
        char v[64];
        if (header_is(line, len, "Transfer-Encoding")) {
            header_val(line, len, 17, v, sizeof(v));
            for (char* q = v; *q; q++) *q = (char)tolower((unsigned char)*q);
            if (strstr(v, "chunked")) chunked = true;
        } else if (header_is(line, len, "Content-Length")) {
            header_val(line, len, 14, v, sizeof(v));
            clen = atoll(v);
            has_len = clen >= 0;
        }
        if (status != 101 && (header_is(line, len, "Connection") || header_is(line, len, "Keep-Alive") ||
                              header_is(line, len, "Proxy-Connection"))) continue;
        memcpy(out + ol, line, len + 2);
        ol += len + 2;
    }
    bool no_body = head_request || status == 204 || status == 304 || (status >= 100 && status < 200);
    bool keep = client_wants_keep && status != 101 && (no_body || chunked || has_len);
    if (status != 101) ol += (size_t)sprintf(out + ol, "Connection: %s\r\n", keep ? "keep-alive" : "close");
    memcpy(out + ol, "\r\n", 2);
    ol += 2;
    bool ok = px_send(client, out, ol);
    free(out);
    size_t extra = have - hlen;                // corpo già arrivato insieme all'intestazione
    const char* rest = in + hlen;

    if (ok && status == 101) {                 // WebSocket (o un altro protocollo): da qui solo byte
        if (extra) ok = px_send(client, rest, extra);
        if (ok) pipe_both(client, b, stop);
        free(in);
        sock_close(b);
        res->keep_alive = false;
        return 0;
    }

    Chunked ck = {0};
    long long left = has_len && !chunked ? clen : -1;     // -1: fino alla chiusura (o alla fine del chunked)
    if (no_body) left = 0;
    bool done = left == 0;
    char* buf = in;                            // si riusa il buffer dell'intestazione
    size_t n = extra;
    if (n) memmove(buf, rest, n);
    while (ok && !done) {
        if (n == 0) {
            int w = wait_readable(b, HEAD_WAIT_MS, stop);
            if (w <= 0) { ok = false; break; }
            int got = (int)recv(b, buf, HEAD_MAX, 0);
            if (got <= 0) {                    // il backend ha chiuso
                if (left > 0 || chunked) ok = false;     // risposta troncata: il client deve accorgersene
                break;
            }
            n = (size_t)got;
        }
        size_t take = n;
        if (chunked) {
            long long end = chunked_feed(&ck, buf, n);
            if (end == -2) { ok = false; break; }
            if (end >= 0) { take = (size_t)end; done = true; }
        } else if (left >= 0) {
            if ((long long)take > left) take = (size_t)left;
            left -= (long long)take;
            if (left == 0) done = true;
        }
        if (!px_send(client, buf, take)) { ok = false; break; }
        res->bytes += (long long)take;
        n = 0;
    }
    free(in);
    sock_close(b);
    res->keep_alive = ok && keep && done;
    return 0;
}
