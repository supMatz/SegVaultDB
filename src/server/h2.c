/*
SCOPO: HTTP/2 (vedi h2.h).
       Un thread per connessione con un ciclo di eventi (OpenSSL non consente letture e scritture
       da thread diversi sulla stessa sessione): frame dal client, una connessione HTTP/1.1 al server
       interno per ogni stream (con "Connection: close"), risposte tradotte in HEADERS + DATA
       rispettando le finestre di flusso del client.
*/
#include "h2.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #define BAD_SOCK INVALID_SOCKET
  #define sock_close closesocket
  #define SEND_FLAGS 0
  static void nonblock(tls_sock fd) { u_long m = 1; ioctlsocket(fd, FIONBIO, &m); }
  static bool would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define BAD_SOCK (-1)
  #define sock_close close
  #define SEND_FLAGS MSG_NOSIGNAL
  static void nonblock(tls_sock fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
  static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

static atomic_int g_h2_on = 0;

bool h2_enabled(void) { return atomic_load(&g_h2_on) != 0; }
void h2_set_enabled(bool on) { atomic_store(&g_h2_on, on ? 1 : 0); }

enum { F_DATA = 0, F_HEADERS = 1, F_PRIORITY = 2, F_RST = 3, F_SETTINGS = 4, F_PUSH = 5, F_PING = 6, F_GOAWAY = 7,
       F_WINDOW = 8, F_CONT = 9 };
enum { FL_END_STREAM = 0x1, FL_ACK = 0x1, FL_END_HEADERS = 0x4, FL_PADDED = 0x8, FL_PRIORITY = 0x20 };
enum { E_NO_ERROR = 0, E_PROTOCOL = 1, E_INTERNAL = 2, E_FLOW = 3, E_STREAM_CLOSED = 5, E_FRAME_SIZE = 6,
       E_REFUSED = 7, E_CANCEL = 8, E_COMPRESSION = 9 };

#define MAX_FRAME        16384          // frame accettati e mandati (il valore iniziale del protocollo)
#define MAX_STREAMS      64             // stream aperti insieme per connessione
#define OUR_WINDOW       (1 << 20)      // finestra di ricezione per stream
#define CONN_WINDOW      (16 << 20)     // finestra di ricezione della connessione
#define MAX_HEADER_BLOCK (64 * 1024)
#define BACKLOG_MAX      (1 << 20)      // corpo in attesa di finestra: oltre si smette di leggere dal server interno
#define IDLE_MS          (120 * 1000)

// ── HPACK: Huffman (RFC 7541 appendice B, codice canonico) ──────

static const uint8_t HUFF_LEN[257] = {
    13,23,28,28,28,28,28,28,28,24,30,28,28,30,28,28,28,28,28,28,28,28,30,28,28,28,28,28,28,28,28,28,
    6,10,10,12,13,6,8,11,10,10,8,11,8,6,6,6,5,5,5,6,6,6,6,6,6,6,7,8,15,6,12,10,
    13,6,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,7,8,13,19,13,14,6,
    15,5,6,5,6,5,6,6,6,5,7,7,6,6,6,5,6,7,6,5,5,6,7,7,7,7,7,15,11,14,13,28,
    20,22,20,20,22,22,22,23,22,23,23,23,23,23,24,23,24,24,22,23,24,23,23,23,23,21,22,23,22,23,23,24,
    22,21,20,22,22,23,23,21,23,22,22,24,21,22,23,23,21,21,22,21,23,22,23,23,20,22,22,22,23,22,22,23,
    26,26,20,19,22,23,22,25,26,26,26,27,27,26,24,25,19,21,26,27,27,26,27,24,21,21,26,26,28,27,27,27,
    20,24,20,21,22,21,21,23,22,22,25,25,24,24,26,23,26,27,26,26,27,27,27,27,27,28,27,27,27,27,27,26,
    30 };

static struct {
    int      ready;
    uint32_t first[32];          // primo codice di ogni lunghezza
    int      start[32];          // indice in sym del primo simbolo di quella lunghezza
    int      count[32];
    uint16_t sym[257];           // simboli ordinati per (lunghezza, valore)
} H;

static void huff_init(void) {
    if (H.ready) return;
    int n = 0;
    for (int l = 1; l <= 30; l++) {
        H.start[l] = n;
        for (int s = 0; s < 257; s++) if (HUFF_LEN[s] == l) H.sym[n++] = (uint16_t)s;
        H.count[l] = n - H.start[l];
    }
    uint32_t code = 0;
    for (int l = 1; l <= 30; l++) {
        H.first[l] = code;
        code = (code + (uint32_t)H.count[l]) << 1;
    }
    H.ready = 1;
}

static bool huff_decode(const uint8_t* p, size_t n, JsonBuf* o) {
    uint32_t code = 0;
    int len = 0;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            code = code << 1 | ((p[i] >> b) & 1);
            len++;
            if (len > 30) return false;
            if (H.count[len] && code >= H.first[len] && code - H.first[len] < (uint32_t)H.count[len]) {
                uint16_t s = H.sym[H.start[len] + (int)(code - H.first[len])];
                if (s == 256) return false;                    // EOS nella stringa: errore
                char c = (char)s;
                jb_raw(o, &c, 1);
                code = 0;
                len = 0;
            }
        }
    }
    // riempimento: al massimo 7 bit, tutti a 1
    return len <= 7 && code == (1u << len) - 1;
}

// ── HPACK: tabelle ───────────────────────────────────────────────

static const char* const STATIC_T[62][2] = {
    { "", "" }, { ":authority", "" }, { ":method", "GET" }, { ":method", "POST" }, { ":path", "/" }, { ":path", "/index.html" },
    { ":scheme", "http" }, { ":scheme", "https" }, { ":status", "200" }, { ":status", "204" }, { ":status", "206" },
    { ":status", "304" }, { ":status", "400" }, { ":status", "404" }, { ":status", "500" }, { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" }, { "accept-language", "" }, { "accept-ranges", "" }, { "accept", "" },
    { "access-control-allow-origin", "" }, { "age", "" }, { "allow", "" }, { "authorization", "" }, { "cache-control", "" },
    { "content-disposition", "" }, { "content-encoding", "" }, { "content-language", "" }, { "content-length", "" },
    { "content-location", "" }, { "content-range", "" }, { "content-type", "" }, { "cookie", "" }, { "date", "" },
    { "etag", "" }, { "expect", "" }, { "expires", "" }, { "from", "" }, { "host", "" }, { "if-match", "" },
    { "if-modified-since", "" }, { "if-none-match", "" }, { "if-range", "" }, { "if-unmodified-since", "" },
    { "last-modified", "" }, { "link", "" }, { "location", "" }, { "max-forwards", "" }, { "proxy-authenticate", "" },
    { "proxy-authorization", "" }, { "range", "" }, { "referer", "" }, { "refresh", "" }, { "retry-after", "" },
    { "server", "" }, { "set-cookie", "" }, { "strict-transport-security", "" }, { "transfer-encoding", "" },
    { "user-agent", "" }, { "vary", "" }, { "via", "" }, { "www-authenticate", "" },
};

typedef struct { char* name; char* value; size_t size; } DynEnt;

typedef struct {
    DynEnt* e;                    // e[0] = la più recente
    int     n, cap;
    size_t  size, max;
} DynTable;

static void dyn_evict(DynTable* d) {
    while (d->n > 0 && d->size > d->max) {
        DynEnt* x = &d->e[d->n - 1];
        d->size -= x->size;
        free(x->name);
        free(x->value);
        d->n--;
    }
}

static void dyn_add(DynTable* d, const char* name, size_t nl, const char* value, size_t vl) {
    size_t sz = nl + vl + 32;
    while (d->n > 0 && d->size + sz > d->max) {        // le più vecchie lasciano il posto
        DynEnt* x = &d->e[d->n - 1];
        d->size -= x->size;
        free(x->name);
        free(x->value);
        d->n--;
    }
    if (sz > d->max) return;                           // più grande della tabella: la tabella resta vuota
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 16;
        DynEnt* ne = realloc(d->e, sizeof(DynEnt) * (size_t)nc);
        if (!ne) return;
        d->e = ne;
        d->cap = nc;
    }
    char* nm = malloc(nl + 1);
    char* vv = malloc(vl + 1);
    if (!nm || !vv) { free(nm); free(vv); return; }
    memcpy(nm, name, nl); nm[nl] = '\0';
    memcpy(vv, value, vl); vv[vl] = '\0';
    memmove(d->e + 1, d->e, sizeof(DynEnt) * (size_t)d->n);
    d->e[0].name = nm;
    d->e[0].value = vv;
    d->e[0].size = sz;
    d->n++;
    d->size += sz;
}

static void dyn_free(DynTable* d) {
    for (int i = 0; i < d->n; i++) { free(d->e[i].name); free(d->e[i].value); }
    free(d->e);
    memset(d, 0, sizeof(*d));
}

static bool table_get(const DynTable* d, uint64_t idx, const char** name, const char** value) {
    if (idx == 0) return false;
    if (idx <= 61) { *name = STATIC_T[idx][0]; *value = STATIC_T[idx][1]; return true; }
    uint64_t k = idx - 62;
    if (k >= (uint64_t)d->n) return false;
    *name = d->e[k].name;
    *value = d->e[k].value;
    return true;
}

static bool hp_int(const uint8_t* p, size_t n, size_t* i, int prefix, uint64_t* out) {
    if (*i >= n) return false;
    uint64_t mask = (1u << prefix) - 1;
    uint64_t v = p[*i] & mask;
    (*i)++;
    if (v < mask) { *out = v; return true; }
    int shift = 0;
    for (;;) {
        if (*i >= n || shift > 56) return false;
        uint8_t b = p[(*i)++];
        v += (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    *out = v;
    return true;
}

static bool hp_str(const uint8_t* p, size_t n, size_t* i, JsonBuf* o) {
    if (*i >= n) return false;
    bool huff = p[*i] & 0x80;
    uint64_t len;
    if (!hp_int(p, n, i, 7, &len) || len > n - *i) return false;
    bool ok = huff ? huff_decode(p + *i, (size_t)len, o) : (jb_raw(o, (const char*)p + *i, (size_t)len), true);
    *i += (size_t)len;
    if (!o->data) jb_raw(o, "", 0);
    return ok && !o->oom;
}

typedef struct { char name[128]; char* value; } Hdr;

// blocco di intestazioni -> elenco (nomi in minuscolo come richiede HTTP/2)
static bool hpack_decode(DynTable* d, size_t max_table, const uint8_t* p, size_t n, Hdr** out, int* nout) {
    int cap = 32, k = 0;
    Hdr* v = malloc(sizeof(Hdr) * (size_t)cap);
    if (!v) return false;
    size_t i = 0;
    bool ok = true;
    while (i < n && ok) {
        uint8_t b = p[i];
        const char* nm = NULL;
        const char* val = NULL;
        JsonBuf nb = {0}, vb = {0};
        bool add = false;
        if (b & 0x80) {                                  // campo indicizzato
            uint64_t idx;
            ok = hp_int(p, n, &i, 7, &idx) && table_get(d, idx, &nm, &val);
        } else if ((b & 0xE0) == 0x20) {                 // aggiornamento della dimensione della tabella
            uint64_t sz;
            ok = hp_int(p, n, &i, 5, &sz) && sz <= max_table;
            if (ok) { d->max = (size_t)sz; dyn_evict(d); }
            continue;
        } else {
            int prefix = (b & 0xC0) == 0x40 ? 6 : 4;     // 01: con indicizzazione; 0000 / 0001: senza
            add = prefix == 6;
            uint64_t idx;
            const char* tv;
            ok = hp_int(p, n, &i, prefix, &idx);
            if (ok && idx) ok = table_get(d, idx, &nm, &tv);
            else if (ok) { ok = hp_str(p, n, &i, &nb); nm = nb.data; }
            if (ok) { ok = hp_str(p, n, &i, &vb); val = vb.data; }
        }
        if (ok) {
            if (k == cap) { cap *= 2; Hdr* nv = realloc(v, sizeof(Hdr) * (size_t)cap); if (!nv) { ok = false; } else v = nv; }
            if (ok && k < 512) {
                snprintf(v[k].name, sizeof(v[k].name), "%s", nm);
                for (char* c = v[k].name; *c; c++) *c = (char)tolower((unsigned char)*c);
                v[k].value = strdup(val ? val : "");
                k++;
            }
            if (ok && add) dyn_add(d, nm, strlen(nm), val ? val : "", val ? strlen(val) : 0);
        }
        free(nb.data);
        free(vb.data);
    }
    if (!ok) { for (int j = 0; j < k; j++) free(v[j].value); free(v); return false; }
    *out = v;
    *nout = k;
    return true;
}

static void hdrs_free(Hdr* v, int n) { for (int i = 0; i < n; i++) free(v[i].value); free(v); }

// codifica: niente tabella dinamica, niente Huffman (semplice e sempre valido)
static void hp_put_int(JsonBuf* o, uint8_t first, int prefix, uint64_t v) {
    uint64_t mask = (1u << prefix) - 1;
    if (v < mask) { uint8_t c = (uint8_t)(first | v); jb_raw(o, (const char*)&c, 1); return; }
    uint8_t c = (uint8_t)(first | mask);
    jb_raw(o, (const char*)&c, 1);
    v -= mask;
    while (v >= 128) { c = (uint8_t)(v % 128 + 128); jb_raw(o, (const char*)&c, 1); v /= 128; }
    c = (uint8_t)v;
    jb_raw(o, (const char*)&c, 1);
}

static void hp_put_str(JsonBuf* o, const char* s, size_t n) {
    hp_put_int(o, 0x00, 7, n);
    jb_raw(o, s, n);
}

static void hp_put_header(JsonBuf* o, const char* name, size_t nl, const char* value, size_t vl) {
    for (int i = 1; i <= 61; i++)                      // nome della tabella statica: indice
        if (strlen(STATIC_T[i][0]) == nl && !strncmp(STATIC_T[i][0], name, nl)) {
            hp_put_int(o, 0x00, 4, (uint64_t)i);
            hp_put_str(o, value, vl);
            return;
        }
    jb_lit(o, "\x00");
    hp_put_str(o, name, nl);
    hp_put_str(o, value, vl);
}

// ── stream ───────────────────────────────────────────────────────

typedef struct {
    int st;                       // 0 dimensione, 1 resto della riga, 2 dati, 3 CRLF dopo i dati, 4 trailer, 5 riga di trailer, 6 fine
    unsigned long long left;
    int digits;
} ChunkDec;

typedef struct Stream {
    uint32_t  id;
    tls_sock  fd;                 // connessione al server interno
    bool      connected;
    JsonBuf   to_inner;           // byte da scrivere al server interno
    size_t    to_inner_off;
    bool      req_chunked;        // corpo della richiesta mandato chunked
    bool      req_end;            // END_STREAM ricevuto
    bool      head_req;           // HEAD
    // risposta
    JsonBuf   head;
    bool      head_done;
    int       framing;            // 0 fino alla chiusura, 1 Content-Length, 2 chunked, 3 senza corpo
    long long left;
    ChunkDec  cd;
    JsonBuf   out;                // corpo in attesa della finestra
    size_t    out_off;
    bool      resp_done;          // tutto il corpo è in out
    bool      end_sent;
    long long window;             // finestra di invio verso il client
    struct Stream* next;
} Stream;

typedef struct {
    H2Io*        io;
    int          inner_port;
    const char*  marker;
    atomic_int*  stop;
    DynTable     dec;
    long long    conn_window;     // finestra di invio della connessione
    long long    init_window;     // finestra iniziale degli stream (impostazione del client)
    uint32_t     last_id;
    Stream*      streams;
    int          nstreams;
    // intestazioni in arrivo su più frame (HEADERS + CONTINUATION)
    JsonBuf      hblock;
    uint32_t     hstream;
    bool         hend_stream;
    bool         in_headers;
    bool         goaway;
} Conn2;

static bool send_frame(Conn2* c, int type, int flags, uint32_t id, const void* payload, size_t n) {
    uint8_t h[9] = { (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)type, (uint8_t)flags,
                     (uint8_t)((id >> 24) & 0x7f), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    if (n == 0) return c->io->send(c->io->ssl, c->io->fd, (const char*)h, 9);
    // intestazione e contenuto in un solo record TLS
    char* buf = malloc(9 + n);
    if (!buf) return false;
    memcpy(buf, h, 9);
    memcpy(buf + 9, payload, n);
    bool ok = c->io->send(c->io->ssl, c->io->fd, buf, 9 + n);
    free(buf);
    return ok;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool send_rst(Conn2* c, uint32_t id, uint32_t code) { uint8_t p[4]; put32(p, code); return send_frame(c, F_RST, 0, id, p, 4); }

static bool send_goaway(Conn2* c, uint32_t code) {
    uint8_t p[8];
    put32(p, c->last_id);
    put32(p + 4, code);
    return send_frame(c, F_GOAWAY, 0, 0, p, 8);
}

static bool send_window(Conn2* c, uint32_t id, uint32_t inc) {
    if (!inc) return true;
    uint8_t p[4];
    put32(p, inc & 0x7fffffffu);
    return send_frame(c, F_WINDOW, 0, id, p, 4);
}

static Stream* find_stream(Conn2* c, uint32_t id) {
    for (Stream* s = c->streams; s; s = s->next) if (s->id == id) return s;
    return NULL;
}

static void stream_free(Conn2* c, Stream* s) {
    Stream** pp = &c->streams;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    if (s->fd != BAD_SOCK) sock_close(s->fd);
    free(s->to_inner.data);
    free(s->head.data);
    free(s->out.data);
    free(s);
    c->nstreams--;
}

static tls_sock connect_inner(int port) {
    tls_sock fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK) return BAD_SOCK;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { sock_close(fd); return BAD_SOCK; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    nonblock(fd);
    return fd;
}

static bool hop_by_hop(const char* n) {
    return !strcmp(n, "connection") || !strcmp(n, "keep-alive") || !strcmp(n, "proxy-connection") ||
           !strcmp(n, "transfer-encoding") || !strcmp(n, "upgrade") || !strcmp(n, "te") || !strcmp(n, "host");
}

static bool valid_field(const char* s) {
    for (; *s; s++) if (*s == '\r' || *s == '\n' || *s == '\0') return false;
    return true;
}

// HEADERS completo: nuovo stream con la sua richiesta HTTP/1.1 verso il server interno
static bool open_stream(Conn2* c, uint32_t id, const uint8_t* block, size_t n, bool end_stream) {
    Hdr* h = NULL;
    int nh = 0;
    if (!hpack_decode(&c->dec, 4096, block, n, &h, &nh)) { send_goaway(c, E_COMPRESSION); return false; }
    const char *method = NULL, *path = NULL, *authority = NULL, *scheme = NULL;
    bool has_len = false, bad = false;
    for (int i = 0; i < nh; i++) {
        if (!valid_field(h[i].value)) bad = true;
        if (!strcmp(h[i].name, ":method")) method = h[i].value;
        else if (!strcmp(h[i].name, ":path")) path = h[i].value;
        else if (!strcmp(h[i].name, ":authority")) authority = h[i].value;
        else if (!strcmp(h[i].name, ":scheme")) scheme = h[i].value;
        else if (!strcmp(h[i].name, "content-length")) has_len = true;
        else if (!strcmp(h[i].name, "host") && !authority) authority = h[i].value;
    }
    (void)scheme;
    if (!method || !path || !path[0] || bad || !strcmp(method, "CONNECT")) {
        hdrs_free(h, nh);
        return send_rst(c, id, E_PROTOCOL);
    }
    if (c->nstreams >= MAX_STREAMS) { hdrs_free(h, nh); return send_rst(c, id, E_REFUSED); }
    Stream* s = calloc(1, sizeof(Stream));
    if (!s) { hdrs_free(h, nh); return send_rst(c, id, E_INTERNAL); }
    s->id = id;
    s->fd = BAD_SOCK;
    s->window = c->init_window;
    s->head_req = !strcmp(method, "HEAD");
    s->req_end = end_stream;
    JsonBuf* r = &s->to_inner;
    jb_raw(r, c->marker, strlen(c->marker));
    jb_printf(r, "%s %s HTTP/1.1\r\n", method, path);
    if (authority) jb_printf(r, "Host: %s\r\n", authority);
    // i cookie possono arrivare in più campi: in HTTP/1.1 vanno uniti con "; "
    JsonBuf ck = {0};
    for (int i = 0; i < nh; i++) {
        if (h[i].name[0] == ':' || hop_by_hop(h[i].name)) continue;
        if (!strcmp(h[i].name, "cookie")) {
            if (ck.len) jb_lit(&ck, "; ");
            jb_raw(&ck, h[i].value, strlen(h[i].value));
            continue;
        }
        jb_printf(r, "%s: %s\r\n", h[i].name, h[i].value);
    }
    if (ck.len) { jb_lit(r, "Cookie: "); jb_raw(r, ck.data, ck.len); jb_lit(r, "\r\n"); }
    free(ck.data);
    // corpo senza lunghezza dichiarata: chunked verso il server interno
    if (!end_stream && !has_len) { jb_lit(r, "Transfer-Encoding: chunked\r\n"); s->req_chunked = true; }
    else if (end_stream && !has_len && (!strcmp(method, "POST") || !strcmp(method, "PUT") || !strcmp(method, "PATCH")))
        jb_lit(r, "Content-Length: 0\r\n");
    jb_lit(r, "Connection: close\r\n\r\n");
    hdrs_free(h, nh);
    if (r->oom) { free(r->data); free(s); return send_rst(c, id, E_INTERNAL); }
    s->fd = connect_inner(c->inner_port);
    if (s->fd == BAD_SOCK) {
        free(r->data);
        free(s);
        // il server interno non accetta (troppe connessioni): 503 come risposta dello stream
        JsonBuf hb = {0};
        hp_put_int(&hb, 0x00, 4, 8);
        hp_put_str(&hb, "503", 3);
        bool ok = send_frame(c, F_HEADERS, FL_END_HEADERS | FL_END_STREAM, id, hb.data, hb.len);
        free(hb.data);
        return ok;
    }
    s->next = c->streams;
    c->streams = s;
    c->nstreams++;
    return true;
}

// corpo della richiesta: DATA -> server interno
static void stream_body(Stream* s, const uint8_t* p, size_t n, bool end) {
    if (s->req_chunked) {
        if (n) { jb_printf(&s->to_inner, "%llx\r\n", (unsigned long long)n); jb_raw(&s->to_inner, (const char*)p, n); jb_lit(&s->to_inner, "\r\n"); }
        if (end) jb_lit(&s->to_inner, "0\r\n\r\n");
    } else if (n) jb_raw(&s->to_inner, (const char*)p, n);
    if (end) s->req_end = true;
}

// ── risposta del server interno ─────────────────────────────────

// decodifica chunked: aggiunge i dati a out, true alla fine del corpo; false se non valido
static bool chunk_feed(ChunkDec* d, const char* p, size_t n, JsonBuf* out, bool* done, bool* bad) {
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        switch (d->st) {
        case 0:
            if (isxdigit((unsigned char)ch)) {
                if (++d->digits > 15) { *bad = true; return false; }
                d->left = d->left * 16 + (unsigned)(isdigit((unsigned char)ch) ? ch - '0' : tolower((unsigned char)ch) - 'a' + 10);
                break;
            }
            if (!d->digits) { *bad = true; return false; }
            d->st = 1;
            /* fallthrough */
        case 1:
            if (ch == '\n') d->st = d->left ? 2 : 4;
            break;
        case 2: {
            size_t take = (size_t)(d->left < (unsigned long long)(n - i) ? d->left : (unsigned long long)(n - i));
            jb_raw(out, p + i, take);
            i += take - 1;
            d->left -= take;
            if (!d->left) d->st = 3;
            break;
        }
        case 3:
            if (ch == '\n') { d->st = 0; d->digits = 0; d->left = 0; }
            break;
        case 4:
            if (ch == '\n') { d->st = 6; *done = true; return true; }
            if (ch != '\r') d->st = 5;
            break;
        case 5:
            if (ch == '\n') d->st = 4;
            break;
        default:
            return true;
        }
    }
    return true;
}

// intestazione HTTP/1.1 completa -> HEADERS
static bool send_response_head(Conn2* c, Stream* s, size_t hl) {
    char* h = s->head.data;
    h[hl - 2] = '\0';
    int status = atoi(h + 9);
    if (strncmp(h, "HTTP/1.", 7) || status < 200 || status > 999) return false;
    JsonBuf hb = {0};
    char st[8];
    snprintf(st, sizeof(st), "%d", status);
    int idx = status == 200 ? 8 : status == 204 ? 9 : status == 206 ? 10 : status == 304 ? 11 : status == 400 ? 12 :
              status == 404 ? 13 : status == 500 ? 14 : 0;
    if (idx) hp_put_int(&hb, 0x80, 7, (uint64_t)idx);
    else { hp_put_int(&hb, 0x00, 4, 8); hp_put_str(&hb, st, strlen(st)); }
    bool chunked = false, has_len = false;
    long long clen = 0;
    char* line = strstr(h, "\r\n");
    while (line && line[2]) {
        line += 2;
        char* e = strstr(line, "\r\n");
        size_t ll = e ? (size_t)(e - line) : strlen(line);
        char* colon = memchr(line, ':', ll);
        if (colon && colon > line) {
            char name[128];
            size_t nl = (size_t)(colon - line);
            if (nl >= sizeof(name)) nl = sizeof(name) - 1;
            for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)line[i]);
            name[nl] = '\0';
            char* v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(line + ll - v);
            if (!strcmp(name, "transfer-encoding")) { if (vl >= 7 && strstr(v, "chunked")) chunked = true; }
            else if (!strcmp(name, "content-length")) { has_len = true; clen = atoll(v); hp_put_header(&hb, name, nl, v, vl); }
            else if (!hop_by_hop(name)) hp_put_header(&hb, name, nl, v, vl);
        }
        line = e;
    }
    bool no_body = s->head_req || status == 204 || status == 304 || (has_len && clen == 0 && !chunked);
    s->framing = no_body ? 3 : chunked ? 2 : has_len ? 1 : 0;
    s->left = clen;
    if (hb.oom) { free(hb.data); return false; }
    // intestazioni oltre un frame: HEADERS + CONTINUATION
    bool ok = true;
    size_t off = 0;
    int type = F_HEADERS;
    do {
        size_t k = hb.len - off > MAX_FRAME ? MAX_FRAME : hb.len - off;
        bool last = off + k == hb.len;
        int flags = (last ? FL_END_HEADERS : 0) | (type == F_HEADERS && no_body ? FL_END_STREAM : 0);
        ok = send_frame(c, type, flags, s->id, hb.data + off, k);
        off += k;
        type = F_CONT;
    } while (ok && off < hb.len);
    free(hb.data);
    if (no_body) { s->end_sent = true; s->resp_done = true; }
    return ok;
}

// manda il corpo in attesa finché le finestre lo permettono
static bool flush_stream(Conn2* c, Stream* s) {
    while (!s->end_sent) {
        size_t avail = s->out.len - s->out_off;
        if (avail == 0) {
            if (!s->resp_done) return true;
            s->end_sent = true;
            return send_frame(c, F_DATA, FL_END_STREAM, s->id, NULL, 0);
        }
        long long win = s->window < c->conn_window ? s->window : c->conn_window;
        if (win <= 0) return true;
        size_t k = avail;
        if ((long long)k > win) k = (size_t)win;
        if (k > MAX_FRAME) k = MAX_FRAME;
        bool last = s->resp_done && k == avail;
        if (!send_frame(c, F_DATA, last ? FL_END_STREAM : 0, s->id, s->out.data + s->out_off, k)) return false;
        s->out_off += k;
        s->window -= (long long)k;
        c->conn_window -= (long long)k;
        if (last) s->end_sent = true;
        if (s->out_off == s->out.len) { s->out.len = 0; s->out_off = 0; }
        else if (s->out_off > (1u << 20)) {                // si compatta il buffer
            memmove(s->out.data, s->out.data + s->out_off, s->out.len - s->out_off);
            s->out.len -= s->out_off;
            s->out_off = 0;
        }
    }
    return true;
}

// byte arrivati dal server interno (n = 0: ha chiuso)
static bool inner_data(Conn2* c, Stream* s, const char* p, size_t n) {
    if (!s->head_done) {
        if (n == 0) {                                    // chiuso senza risposta
            JsonBuf hb = {0};
            hp_put_int(&hb, 0x00, 4, 8);
            hp_put_str(&hb, "502", 3);
            bool ok = send_frame(c, F_HEADERS, FL_END_HEADERS | FL_END_STREAM, s->id, hb.data, hb.len);
            free(hb.data);
            s->end_sent = s->resp_done = true;
            return ok;
        }
        jb_raw(&s->head, p, n);
        if (s->head.oom || s->head.len > MAX_HEADER_BLOCK) return send_rst(c, s->id, E_INTERNAL) && (s->end_sent = s->resp_done = true);
        char* he = strstr(s->head.data, "\r\n\r\n");
        if (!he) return true;
        size_t hl = (size_t)(he - s->head.data) + 4;
        size_t extra = s->head.len - hl;
        char* rest = NULL;
        if (extra) { rest = malloc(extra); if (rest) memcpy(rest, s->head.data + hl, extra); }
        s->head_done = true;
        bool ok = send_response_head(c, s, hl);
        if (!ok) { free(rest); return send_rst(c, s->id, E_INTERNAL) && (s->end_sent = s->resp_done = true); }
        bool r = true;
        if (rest && !s->end_sent) r = inner_data(c, s, rest, extra);
        free(rest);
        return r;
    }
    if (s->resp_done) return true;
    if (n == 0) {                                        // chiusura: fine del corpo (o corpo troncato)
        if (s->framing == 0) s->resp_done = true;
        else if (!s->resp_done) { s->end_sent = s->resp_done = true; return send_rst(c, s->id, E_INTERNAL); }
        return flush_stream(c, s);
    }
    if (s->framing == 1) {
        size_t k = (long long)n > s->left ? (size_t)s->left : n;
        jb_raw(&s->out, p, k);
        s->left -= (long long)k;
        if (s->left == 0) s->resp_done = true;
    } else if (s->framing == 2) {
        bool done = false, bad = false;
        chunk_feed(&s->cd, p, n, &s->out, &done, &bad);
        if (bad) { s->end_sent = s->resp_done = true; return send_rst(c, s->id, E_INTERNAL); }
        if (done) s->resp_done = true;
    } else if (s->framing == 0) jb_raw(&s->out, p, n);
    if (s->out.oom) { s->end_sent = s->resp_done = true; return send_rst(c, s->id, E_INTERNAL); }
    return flush_stream(c, s);
}

// ── frame dal client ─────────────────────────────────────────────

static bool handle_settings(Conn2* c, const uint8_t* p, size_t n, int flags) {
    if (flags & FL_ACK) return true;
    if (n % 6) { send_goaway(c, E_FRAME_SIZE); return false; }
    for (size_t i = 0; i < n; i += 6) {
        int id = p[i] << 8 | p[i + 1];
        uint32_t v = get32(p + i + 2);
        if (id == 4) {                                   // SETTINGS_INITIAL_WINDOW_SIZE
            if (v > 0x7fffffffu) { send_goaway(c, E_FLOW); return false; }
            long long delta = (long long)v - c->init_window;
            c->init_window = v;
            for (Stream* s = c->streams; s; s = s->next) s->window += delta;
        } else if (id == 5) {                            // SETTINGS_MAX_FRAME_SIZE: noi mandiamo sempre <= 16384
            if (v < 16384 || v > 16777215) { send_goaway(c, E_PROTOCOL); return false; }
        }
    }
    if (!send_frame(c, F_SETTINGS, FL_ACK, 0, NULL, 0)) return false;
    for (Stream* s = c->streams; s; s = s->next) if (!flush_stream(c, s)) return false;
    return true;
}

// un frame completo; false = la connessione si chiude
static bool handle_frame(Conn2* c, int type, int flags, uint32_t id, const uint8_t* p, size_t n) {
    if (c->in_headers && (type != F_CONT || id != c->hstream)) { send_goaway(c, E_PROTOCOL); return false; }
    switch (type) {
    case F_DATA: {
        Stream* s = find_stream(c, id);
        size_t pad = 0, off = 0;
        if (flags & FL_PADDED) { if (n < 1) { send_goaway(c, E_PROTOCOL); return false; } pad = p[0]; off = 1; }
        if (off + pad > n) { send_goaway(c, E_PROTOCOL); return false; }
        // la finestra della connessione si riapre comunque (anche per stream già chiusi)
        if (n && !send_window(c, 0, (uint32_t)n)) return false;
        if (!s || s->req_end) return id && id <= c->last_id ? send_rst(c, id, E_STREAM_CLOSED) : (send_goaway(c, E_PROTOCOL), false);
        bool end = flags & FL_END_STREAM;
        stream_body(s, p + off, n - off - pad, end);
        if (!end && n && !send_window(c, id, (uint32_t)n)) return false;
        return true;
    }
    case F_HEADERS: {
        if (!id || !(id & 1)) { send_goaway(c, E_PROTOCOL); return false; }
        Stream* s = find_stream(c, id);
        if (s) {                                        // trailer della richiesta: ignorati, chiudono il corpo
            if (flags & FL_END_STREAM) stream_body(s, NULL, 0, true);
            if (!(flags & FL_END_HEADERS)) { c->in_headers = true; c->hstream = id; c->hblock.len = 0; c->hend_stream = false; }
            return true;
        }
        if (id <= c->last_id) { send_goaway(c, E_PROTOCOL); return false; }
        c->last_id = id;
        size_t off = 0, pad = 0;
        if (flags & FL_PADDED) { if (n < 1) { send_goaway(c, E_PROTOCOL); return false; } pad = p[0]; off = 1; }
        if (flags & FL_PRIORITY) off += 5;
        if (off + pad > n) { send_goaway(c, E_PROTOCOL); return false; }
        c->hblock.len = 0;
        jb_raw(&c->hblock, (const char*)p + off, n - off - pad);
        c->hstream = id;
        c->hend_stream = flags & FL_END_STREAM;
        if (!(flags & FL_END_HEADERS)) { c->in_headers = true; return true; }
        return open_stream(c, id, (const uint8_t*)c->hblock.data, c->hblock.len, c->hend_stream);
    }
    case F_CONT: {
        if (!c->in_headers || id != c->hstream) { send_goaway(c, E_PROTOCOL); return false; }
        jb_raw(&c->hblock, (const char*)p, n);
        if (c->hblock.len > MAX_HEADER_BLOCK) { send_goaway(c, E_PROTOCOL); return false; }
        if (!(flags & FL_END_HEADERS)) return true;
        c->in_headers = false;
        if (find_stream(c, id)) return true;            // trailer
        return open_stream(c, id, (const uint8_t*)c->hblock.data, c->hblock.len, c->hend_stream);
    }
    case F_PRIORITY:
        return true;
    case F_RST: {
        Stream* s = find_stream(c, id);
        if (s) stream_free(c, s);
        return true;
    }
    case F_SETTINGS:
        if (id) { send_goaway(c, E_PROTOCOL); return false; }
        return handle_settings(c, p, n, flags);
    case F_PING:
        if (n != 8) { send_goaway(c, E_FRAME_SIZE); return false; }
        if (flags & FL_ACK) return true;
        return send_frame(c, F_PING, FL_ACK, 0, p, 8);
    case F_GOAWAY:
        c->goaway = true;
        return true;
    case F_WINDOW: {
        if (n != 4) { send_goaway(c, E_FRAME_SIZE); return false; }
        uint32_t inc = get32(p) & 0x7fffffffu;
        if (!id) {
            c->conn_window += inc;
            if (c->conn_window > 0x7fffffffLL) { send_goaway(c, E_FLOW); return false; }
            for (Stream* s = c->streams; s; s = s->next) if (!flush_stream(c, s)) return false;
            return true;
        }
        Stream* s = find_stream(c, id);
        if (!s) return true;
        s->window += inc;
        return flush_stream(c, s);
    }
    case F_PUSH:
        send_goaway(c, E_PROTOCOL);
        return false;
    default:
        return true;                                     // tipi sconosciuti: ignorati
    }
}

// ── ciclo ────────────────────────────────────────────────────────

static double now_ms2(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

void h2_serve(H2Io* io, int inner_port, const char* marker, atomic_int* stop) {
    huff_init();
    Conn2 c;
    memset(&c, 0, sizeof(c));
    c.io = io;
    c.inner_port = inner_port;
    c.marker = marker;
    c.stop = stop;
    c.dec.max = 4096;
    c.conn_window = 65535;
    c.init_window = 65535;
    // impostazioni del server: stream insieme, finestra per stream; poi la finestra della connessione
    uint8_t set[12];
    set[0] = 0; set[1] = 3; put32(set + 2, MAX_STREAMS);
    set[6] = 0; set[7] = 4; put32(set + 8, OUR_WINDOW);
    if (!send_frame(&c, F_SETTINGS, 0, 0, set, 12) || !send_window(&c, 0, CONN_WINDOW - 65535)) return;

    JsonBuf in = {0};
    bool preface = false;
    char buf[65536];
    double last = now_ms2();
    bool alive = true;
    while (alive && !*stop) {
        // 1. dal client
        bool got = false;
        for (int k = 0; k < 64; k++) {
            int r = io->recv(io->ssl, buf, sizeof(buf));
            if (r < 0) { alive = false; break; }
            if (r == 0) break;
            jb_raw(&in, buf, (size_t)r);
            got = true;
            if (in.len > (4u << 20)) break;
        }
        if (!alive || in.oom) break;
        if (got) last = now_ms2();
        if (!preface && in.len >= 24) {
            if (memcmp(in.data, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24)) break;
            preface = true;
            memmove(in.data, in.data + 24, in.len - 24);
            in.len -= 24;
        }
        size_t pos = 0;
        while (preface && alive && in.len - pos >= 9) {
            const uint8_t* h = (const uint8_t*)in.data + pos;
            size_t n = (size_t)h[0] << 16 | (size_t)h[1] << 8 | h[2];
            if (n > MAX_FRAME) { send_goaway(&c, E_FRAME_SIZE); alive = false; break; }
            if (in.len - pos < 9 + n) break;
            uint32_t id = get32(h + 5) & 0x7fffffffu;
            if (!handle_frame(&c, h[3], h[4], id, h + 9, n)) alive = false;
            pos += 9 + n;
        }
        if (pos) { memmove(in.data, in.data + pos, in.len - pos); in.len -= pos; }
        if (!alive) break;

        // 2. verso i server interni (richieste e corpi)
        for (Stream* s = c.streams; s; s = s->next) {
            while (s->fd != BAD_SOCK && s->to_inner_off < s->to_inner.len) {
                int w = (int)send(s->fd, s->to_inner.data + s->to_inner_off, (int)(s->to_inner.len - s->to_inner_off), SEND_FLAGS);
                if (w > 0) { s->to_inner_off += (size_t)w; continue; }
                if (w < 0 && would_block()) break;
                s->to_inner_off = s->to_inner.len;          // il server interno ha chiuso: la risposta dirà
                break;
            }
            if (s->to_inner_off == s->to_inner.len && s->to_inner.len) { s->to_inner.len = 0; s->to_inner_off = 0; }
        }

        // 3. stream finiti: via
        for (Stream* s = c.streams; s; ) {
            Stream* nx = s->next;
            if (s->end_sent) stream_free(&c, s);
            s = nx;
        }
        if (c.goaway && !c.streams) break;

        // 4. attesa: client, server interni leggibili (se c'è finestra) o scrivibili
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(io->fd, &rd);
        tls_sock top = io->fd;
        for (Stream* s = c.streams; s; s = s->next) {
            if (s->fd == BAD_SOCK) continue;
            if (!s->resp_done && s->out.len - s->out_off < BACKLOG_MAX) { FD_SET(s->fd, &rd); if (s->fd > top) top = s->fd; }
            if (s->to_inner.len > s->to_inner_off) { FD_SET(s->fd, &wr); if (s->fd > top) top = s->fd; }
        }
        int pend = io->pending(io->ssl);
        struct timeval tv = { 0, pend > 0 ? 0 : 200000 };
        int r = select((int)top + 1, &rd, &wr, NULL, &tv);
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        if (r == 0 && !pend && !c.streams && now_ms2() - last > IDLE_MS) { send_goaway(&c, E_NO_ERROR); break; }
        for (Stream* s = c.streams; s && alive; s = s->next) {
            if (s->fd == BAD_SOCK || !FD_ISSET(s->fd, &rd)) continue;
            int g = (int)recv(s->fd, buf, sizeof(buf), 0);
            if (g < 0 && would_block()) continue;
            if (g <= 0) {
                sock_close(s->fd);
                s->fd = BAD_SOCK;
                if (!inner_data(&c, s, NULL, 0)) alive = false;
                continue;
            }
            if (!inner_data(&c, s, buf, (size_t)g)) alive = false;
            last = now_ms2();
        }
    }
    while (c.streams) stream_free(&c, c.streams);
    dyn_free(&c.dec);
    free(c.hblock.data);
    free(in.data);
}
