// Test del server HTTP/JSON: richieste reali su socket locali.
//   make test_server && ./test_server
#include "src/server/server.h"
#include "src/server/mysql_wire.h"
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
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <ctype.h>

static int g_fail = 0, g_count = 0;

#define CHECK(cond, ...) do { g_count++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int status; char* body; JsonVal* json; } Resp;

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    int one = 1;                     // intestazione e pacchetto in due send: senza, Nagle + ACK ritardato = 40 ms
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// ── client MySQL minimo (protocollo 4.1, mysql_native_password) ──
typedef struct { int fd; uint8_t seq; uint8_t scr[20]; } My;

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

// risposta al nonce per il plugin: mysql_native_password (20 byte) o caching_sha2_password (32 byte)
static size_t my_scramble(const char* plugin, const char* pw, const uint8_t nonce[20], uint8_t out[32]) {
    if (!pw[0]) return 0;
    if (!strcmp(plugin, "caching_sha2_password")) {
        uint8_t s1[32], s2[32], buf[52], x[32];
        auth_sha256(pw, strlen(pw), s1);
        auth_sha256(s1, 32, s2);
        memcpy(buf, s2, 32); memcpy(buf + 32, nonce, 20);
        auth_sha256(buf, 52, x);
        for (int i = 0; i < 32; i++) out[i] = s1[i] ^ x[i];
        return 32;
    }
    uint8_t a[20], b[20], buf[40], x[20];
    auth_sha1(pw, strlen(pw), a);
    auth_sha1(a, 20, b);
    memcpy(buf, nonce, 20); memcpy(buf + 20, b, 20);
    auth_sha1(buf, 40, x);
    for (int i = 0; i < 20; i++) out[i] = a[i] ^ x[i];
    return 20;
}

static const char* g_my_plugin = "mysql_native_password";   // plugin della prima risposta
static bool g_my_full_auth;                                  // l'ultimo login ha chiesto la password in chiaro?
static int  g_my_switches;                                   // cambi di plugin chiesti dal server nell'ultimo login

// dopo la risposta al login: cambio di plugin (0xfe), accesso veloce (0x01 0x03), accesso completo (0x01 0x04:
// password in chiaro, solo se secure); true con l'OK finale
static bool my_finish_login(My* m, const char* pw, bool secure, char* err, size_t en) {
    uint8_t* p; size_t n;
    g_my_full_auth = false;
    g_my_switches = 0;
    if (!my_rd(m, &p, &n)) { snprintf(err, en, "no reply to login"); return false; }
    for (int step = 0; step < 4; step++) {
        if (p[0] == 0xfe && n > 1) {                         // AuthSwitchRequest
            g_my_switches++;
            char plugin[64];
            snprintf(plugin, sizeof(plugin), "%s", (char*)p + 1);
            size_t pl = strlen(plugin) + 2;
            uint8_t nonce[20] = {0};
            if (n >= pl + 20) memcpy(nonce, p + pl, 20);
            memcpy(m->scr, nonce, 20);
            free(p);
            uint8_t r[32];
            size_t rl = my_scramble(plugin, pw, nonce, r);
            if (!my_wr(m, r, rl) || !my_rd(m, &p, &n)) { snprintf(err, en, "no reply after auth switch"); return false; }
            continue;
        }
        if (p[0] == 0x01 && n == 2 && p[1] == 0x03 && !g_my_full_auth) {   // accesso veloce riuscito: segue l'OK
            free(p);
            if (!my_rd(m, &p, &n)) { snprintf(err, en, "no OK after fast auth"); return false; }
            continue;
        }
        if (p[0] == 0x01 && n == 2 && p[1] == 0x04) {        // serve la password in chiaro
            free(p);
            g_my_full_auth = true;
            if (!secure) { snprintf(err, en, "full authentication needs a secure channel"); return false; }
            if (!my_wr(m, (const uint8_t*)pw, strlen(pw) + 1) || !my_rd(m, &p, &n)) { snprintf(err, en, "no reply to full auth"); return false; }
            continue;
        }
        break;
    }
    bool ok = p[0] == 0;
    if (!ok && p[0] == 0xff && n > 9) snprintf(err, en, "%u %s", (unsigned)(p[1] | p[2] << 8), (char*)p + 9);
    free(p);
    return ok;
}

// login su un socket già connesso
static bool my_login_fd(My* m, const char* user, const char* pw, const char* db, bool secure, char* err, size_t en) {
    err[0] = '\0';
    uint8_t* p; size_t n;
    if (m->fd < 0 || !my_rd(m, &p, &n)) { snprintf(err, en, "no handshake"); return false; }
    uint8_t scr[20];
    size_t pos = 1 + strlen((char*)p + 1) + 1 + 4;      // protocollo, versione, id
    memcpy(scr, p + pos, 8);
    pos += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10;
    memcpy(scr + 8, p + pos, 12);
    memcpy(m->scr, scr, 20);
    free(p);
    uint8_t tokn[32];
    size_t tl = my_scramble(g_my_plugin, pw, scr, tokn);
    uint8_t lg[300];
    size_t k = 0;
    uint32_t caps = 0x200 | 0x8000 | 0x80000 | (db ? 8 : 0) | 0x10000 | 0x20000;
    lg[k++] = (uint8_t)caps; lg[k++] = (uint8_t)(caps >> 8); lg[k++] = (uint8_t)(caps >> 16); lg[k++] = (uint8_t)(caps >> 24);
    memset(lg + k, 0, 4); k += 4;
    lg[k++] = 45;
    memset(lg + k, 0, 23); k += 23;
    memcpy(lg + k, user, strlen(user) + 1); k += strlen(user) + 1;
    lg[k++] = (uint8_t)tl; memcpy(lg + k, tokn, tl); k += tl;
    if (db) { memcpy(lg + k, db, strlen(db) + 1); k += strlen(db) + 1; }
    memcpy(lg + k, g_my_plugin, strlen(g_my_plugin) + 1); k += strlen(g_my_plugin) + 1;
    if (!my_wr(m, lg, k)) { snprintf(err, en, "no reply to login"); return false; }
    return my_finish_login(m, pw, secure, err, en);
}

// connessione e login; NULL + messaggio in err se rifiutata
static bool my_connect(My* m, int port, const char* user, const char* pw, const char* db, char* err, size_t en) {
    m->fd = connect_to(port);
    return my_login_fd(m, user, pw, db, false, err, en);
}

// COM_CHANGE_USER: "OK" o "ERR:codice"
static void my_change_user(My* m, const char* user, const char* pw, const char* db, char* out, size_t on) {
    uint8_t a[20], b[20], buf[40], x[20], tokn[20];
    auth_sha1(pw, strlen(pw), a);
    auth_sha1(a, 20, b);
    memcpy(buf, m->scr, 20); memcpy(buf + 20, b, 20);
    auth_sha1(buf, 40, x);
    for (int i = 0; i < 20; i++) tokn[i] = a[i] ^ x[i];
    uint8_t q[300];
    size_t k = 0;
    q[k++] = 0x11;
    memcpy(q + k, user, strlen(user) + 1); k += strlen(user) + 1;
    q[k++] = 20; memcpy(q + k, tokn, 20); k += 20;
    memcpy(q + k, db, strlen(db) + 1); k += strlen(db) + 1;
    q[k++] = 45; q[k++] = 0;
    memcpy(q + k, "mysql_native_password", 22); k += 22;
    m->seq = 0;
    char e[300];
    if (!my_wr(m, q, k)) { snprintf(out, on, "io"); return; }
    if (my_finish_login(m, pw, false, e, sizeof(e))) snprintf(out, on, "OK");
    else snprintf(out, on, "ERR:%u", (unsigned)atoi(e));
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

static unsigned g_my_warn;                               // avvisi dell'ultimo OK di my_query

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
            g_my_warn = p[pos + 2] | p[pos + 3] << 8;
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

// SELECT -> flag delle definizioni di colonna (P2.8), restituisce il numero di colonne (-1 = errore)
static int my_col_flags(My* m, const char* sql, unsigned* flags, int maxc) {
    uint8_t q[4096];
    q[0] = 3;
    size_t sl = strlen(sql);
    memcpy(q + 1, sql, sl);
    m->seq = 0;
    uint8_t* p; size_t n;
    if (!my_wr(m, q, sl + 1) || !my_rd(m, &p, &n)) return -1;
    if (p[0] == 0xff || p[0] == 0) { free(p); return -1; }
    size_t pos = 0;
    int ncols = (int)my_lenenc(p, &pos);
    free(p);
    for (int i = 0; i < ncols; i++) {
        if (!my_rd(m, &p, &n)) return -1;
        pos = 0;
        for (int k = 0; k < 6; k++) { uint64_t l = my_lenenc(p, &pos); pos += (size_t)l; }
        pos += 1 + 2 + 4 + 1;                                  // 0x0c, charset, lunghezza, tipo
        if (i < maxc) flags[i] = p[pos] | p[pos + 1] << 8;
        free(p);
    }
    for (int eofs = 0; eofs < 2; ) {                           // EOF, righe, EOF
        if (!my_rd(m, &p, &n)) return -1;
        if (p[0] == 0xfe && n < 9) eofs++;
        free(p);
    }
    return ncols;
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

// EXECUTE con un solo parametro di byte arbitrari (anche 0x00): solo l'esito OK / ERR
static void my_execute_bytes(My* m, uint32_t id, const uint8_t* v, size_t vl, char* out, size_t on) {
    uint8_t q[1024];
    size_t k = 0;
    q[k++] = 0x17;
    for (int i = 0; i < 4; i++) q[k++] = (uint8_t)(id >> (8 * i));
    q[k++] = 0;
    q[k++] = 1; q[k++] = 0; q[k++] = 0; q[k++] = 0;
    q[k++] = 0;                                            // bitmap dei NULL
    q[k++] = 1;
    q[k++] = 0xfc; q[k++] = 0;                             // tipo BLOB
    q[k++] = 0xfc; q[k++] = (uint8_t)vl; q[k++] = (uint8_t)(vl >> 8);
    memcpy(q + k, v, vl);
    k += vl;
    m->seq = 0;
    uint8_t* p; size_t n;
    if (!my_wr(m, q, k) || !my_rd(m, &p, &n)) { snprintf(out, on, "io"); return; }
    if (p[0] == 0) snprintf(out, on, "OK");
    else snprintf(out, on, "ERR:%u %s", (unsigned)(p[1] | p[2] << 8), (char*)p + 9);
    free(p);
}

// EXECUTE senza parametri con CURSOR_TYPE_READ_ONLY: colonne ed EOF; restituisce lo stato dell'EOF (-1 = errore)
static int my_execute_cursor(My* m, uint32_t id) {
    uint8_t q[16];
    size_t k = 0;
    q[k++] = 0x17;
    for (int i = 0; i < 4; i++) q[k++] = (uint8_t)(id >> (8 * i));
    q[k++] = 1;                                            // CURSOR_TYPE_READ_ONLY
    q[k++] = 1; q[k++] = 0; q[k++] = 0; q[k++] = 0;
    m->seq = 0;
    uint8_t* p; size_t n;
    if (!my_wr(m, q, k) || !my_rd(m, &p, &n)) return -1;
    if (p[0] == 0xff || p[0] == 0) { free(p); return -1; }
    size_t pos = 0;
    int ncols = (int)my_lenenc(p, &pos);
    free(p);
    for (int i = 0; i < ncols; i++) { if (!my_rd(m, &p, &n)) return -1; free(p); }
    if (!my_rd(m, &p, &n)) return -1;
    int st = p[0] == 0xfe ? (p[3] | p[4] << 8) : -1;
    free(p);
    return st;
}

// COM_STMT_FETCH: quante righe arrivano, lo stato dell'EOF in *st (out: "ERR:..." se errore)
static int my_fetch(My* m, uint32_t id, uint32_t rows, int* st, char* out, size_t on) {
    uint8_t q[9] = { 0x1c };
    for (int i = 0; i < 4; i++) { q[1 + i] = (uint8_t)(id >> (8 * i)); q[5 + i] = (uint8_t)(rows >> (8 * i)); }
    m->seq = 0;
    out[0] = 0;
    *st = -1;
    if (!my_wr(m, q, 9)) return -1;
    int got = 0;
    for (;;) {
        uint8_t* p; size_t n;
        if (!my_rd(m, &p, &n)) return -1;
        if (p[0] == 0xff) { snprintf(out, on, "ERR:%u", (unsigned)(p[1] | p[2] << 8)); free(p); return -1; }
        if (p[0] == 0xfe && n < 9) { *st = p[3] | p[4] << 8; free(p); return got; }
        got++;
        free(p);
    }
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
                unsigned long long u = 0;
                for (int b = 0; b < 8; b++) u |= (unsigned long long)p[pos + b] << (8 * b);
                long long v = (long long)u;
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

// P2.8: pacchetti MySQL di 16 MB e più spezzati (0xFFFFFF + resto, sequenze consecutive)
static void check_packet_split(void) {
    size_t sizes[] = { 100, 0xFFFFFE, 0xFFFFFF, 0xFFFFFF + 10, 2 * 0xFFFFFF, 2 * 0xFFFFFF + 3 };
    for (size_t t = 0; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
        size_t len = sizes[t];
        MwOut o = {0};
        o.seq = 5;
        uint8_t* payload = malloc(len);
        for (size_t i = 0; i < len; i++) payload[i] = (uint8_t)(i * 7 + i / 251);
        mw_begin(&o);
        mw_bytes(&o, payload, len);
        mw_end(&o);
        // ricomposizione come un client
        size_t pos = 0, got = 0;
        uint8_t seq = 5;
        bool ok = !o.oom;
        for (;;) {
            if (pos + 4 > o.n) { ok = false; break; }
            size_t pl = o.p[pos] | (size_t)o.p[pos + 1] << 8 | (size_t)o.p[pos + 2] << 16;
            if (o.p[pos + 3] != seq++) ok = false;
            if (pos + 4 + pl > o.n || memcmp(o.p + pos + 4, payload + got, pl)) { ok = false; break; }
            got += pl;
            pos += 4 + pl;
            if (pl < 0xFFFFFF) break;                         // l'ultimo pacchetto è più corto
        }
        CHECK(ok && got == len && pos == o.n && o.seq == seq, "packet of %zu bytes split and reassembled", len);
        free(payload);
        free(o.p);
    }
}

int main(void) {
    check_packet_split();
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
    {   // la voce si scrive dopo la risposta: si aspetta che arrivi (sotto carico può tardare un attimo)
        SvLogEntry last;
        int got = 0;
        for (int w = 0; w < 200; w++) {
            got = sv_server_recent(s, &last, 1);
            if (got == 1 && strstr(last.detail, "nome = ?")) break;
            usleep(5000);
        }
        CHECK(got == 1 && strstr(last.detail, "nome = ?") && !strstr(last.detail, "DROP"),
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
    // P0.8: byte 0x00 attraverso l'API HTTP (parametro "\u0000" e risultato "\u0000")
    r = query(port, "{\"database\":\"t\",\"sql\":\"CREATE TABLE binh (id INT PRIMARY KEY, b BLOB); "
                    "INSERT INTO binh VALUES (1, ?); SELECT LENGTH(b), HEX(b), b FROM binh\",\"params\":[\"a\\u0000b\\u0000\"]}");
    {
        const JsonVal* rs1 = json_get(r.json, "results");
        const JsonVal* rows = rs1 && rs1->count == 3 ? json_get(&rs1->items[2], "rows") : NULL;
        const JsonVal* row = rows && rows->count == 1 ? &rows->items[0] : NULL;
        const JsonVal* bl = row && row->type == JSON_ARRAY && row->count == 3 ? &row->items[2] : NULL;
        CHECK(resp_ok(&r) && strstr(r.body, "[4,\"61006200\",\"a\\u0000b\\u0000\"]") && bl && bl->type == JSON_STRING && bl->len == 4 &&
              !memcmp(bl->str, "a\0b\0", 4), "NUL bytes through the HTTP API: %s", r.body);
    }
    resp_free(&r);
    // P2.12: id oltre 2^31 restituito intero (prima: troncato a 32 bit, o assente)
    r = query(port, "{\"database\":\"t\",\"sql\":\"CREATE TABLE gen64 (id BIGINT PRIMARY KEY AUTO_INCREMENT, v INT) AUTO_INCREMENT = 3000000000; "
                    "INSERT INTO gen64 (v) VALUES (1)\"}");
    CHECK(resp_ok(&r) && strstr(r.body, "\"last_insert_id\":3000000000"), "64-bit last_insert_id: %s", r.body);
    resp_free(&r);
    // P2.12: max_rows + cursore. 25 righe a pagine da 10: 10 + next, 10 + next, 5 senza next, poi il cursore non c'è più
    r = query(port, "{\"database\":\"t\",\"sql\":\"CREATE TABLE pg (id INT PRIMARY KEY); "
                    "INSERT INTO pg SELECT * FROM GENERATE_SERIES(1, 25)\"}");
    CHECK(resp_ok(&r), "pagination table: %s", r.body);
    resp_free(&r);
    {
        char next[64] = "", body[200];
        int got = 0, pages = 0, last_id = 0;
        r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT id FROM pg ORDER BY id\",\"max_rows\":10,\"rows\":\"arrays\"}");
        for (;;) {
            const JsonVal* res0 = r.json && json_get(r.json, "results") ? &json_get(r.json, "results")->items[0] : NULL;
            const JsonVal* rows = res0 ? json_get(res0, "rows") : NULL;
            const JsonVal* nx = res0 ? json_get(res0, "next") : NULL;
            if (!rows) break;
            pages++;
            for (int i = 0; i < rows->count; i++) { got++; last_id = (int)rows->items[i].items[0].number; }
            CHECK(rows->count == (pages < 3 ? 10 : 5), "page %d has %d rows", pages, rows->count);
            if (!nx || nx->type != JSON_STRING) { next[0] = '\0'; break; }
            snprintf(next, sizeof(next), "%s", nx->str);
            resp_free(&r);
            snprintf(body, sizeof(body), "{\"cursor\":\"%s\",\"max_rows\":10}", next);
            r = query(port, body);
        }
        resp_free(&r);
        CHECK(got == 25 && pages == 3 && last_id == 25, "25 rows in 3 pages through the cursor: %d rows, %d pages, last %d", got, pages, last_id);
        snprintf(body, sizeof(body), "{\"cursor\":\"%s\"}", "0123456789abcdef0123456789abcdef");
        r = query(port, body);
        CHECK(r.status == 404, "unknown cursor: %d", r.status);
        resp_free(&r);
        // il resto in una volta: senza max_rows nella richiesta del cursore
        r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT id FROM pg\",\"max_rows\":20}");
        const JsonVal* res0 = r.json && json_get(r.json, "results") ? &json_get(r.json, "results")->items[0] : NULL;
        const JsonVal* nx = res0 ? json_get(res0, "next") : NULL;
        const JsonVal* left = res0 ? json_get(res0, "rows_left") : NULL;
        CHECK(nx && left && left->number == 5, "rows_left: %s", r.body);
        if (nx) snprintf(next, sizeof(next), "%s", nx->str);
        resp_free(&r);
        snprintf(body, sizeof(body), "{\"cursor\":\"%s\"}", next);
        r = query(port, body);
        res0 = r.json && json_get(r.json, "results") ? &json_get(r.json, "results")->items[0] : NULL;
        CHECK(res0 && json_get(res0, "row_count")->number == 5 && !json_get(res0, "next"), "rest of the cursor: %s", r.body);
        resp_free(&r);
        // senza max_rows nessun cursore (come prima)
        r = query(port, "{\"database\":\"t\",\"sql\":\"SELECT id FROM pg\"}");
        CHECK(resp_ok(&r) && strstr(r.body, "\"row_count\":25") && !strstr(r.body, "\"next\""), "no max_rows: whole result");
        resp_free(&r);
    }
    // P2.12: interi oltre 2^53 come stringhe se chiesto (JavaScript li arrotonderebbe)
    r = query(port, "{\"sql\":\"SELECT 9007199254740993 AS big, 9007199254740991 AS ok, -9223372036854775807 AS neg\",\"big_ints\":\"string\"}");
    CHECK(resp_ok(&r) && strstr(r.body, "[\"9007199254740993\",9007199254740991,\"-9223372036854775807\"]"), "big_ints as strings: %s", r.body);
    resp_free(&r);
    r = query(port, "{\"sql\":\"SELECT 9007199254740993 AS big\"}");
    CHECK(resp_ok(&r) && strstr(r.body, "[[9007199254740993]]"), "big_ints default: numbers: %s", r.body);
    resp_free(&r);
    // P2.12: /metrics nel formato di Prometheus: ogni riga è un commento HELP / TYPE o "nome{etichette} valore";
    // gli istogrammi sono cumulativi e +Inf = _count
    r = request(port, "GET", "/api/v1/metrics", NULL, NULL);
    {
        bool fmt = r.status == 200 && r.body && strstr(r.body, "segvault_requests_total{service=\"api\",code=\"2xx\"}") &&
                   strstr(r.body, "segvault_buffer_pool_hits_total ") && strstr(r.body, "segvault_lock_waits_total ") &&
                   strstr(r.body, "# TYPE segvault_request_duration_seconds histogram");
        char bad[200] = "";
        unsigned long long prev = 0, inf = 0, cnt = 0;
        for (char* line = r.body; fmt && line && *line; ) {
            char* nl = strchr(line, '\n');
            if (!nl) { snprintf(bad, sizeof(bad), "no final newline"); fmt = false; break; }
            *nl = '\0';
            if (line[0] == '#') {
                if (strncmp(line, "# HELP segvault_", 16) && strncmp(line, "# TYPE segvault_", 16)) { snprintf(bad, sizeof(bad), "%s", line); fmt = false; }
            } else {
                const char* p = line;
                if (!(isalpha((unsigned char)*p) || *p == '_')) fmt = false;
                while (isalnum((unsigned char)*p) || *p == '_' || *p == ':') p++;
                if (*p == '{') {
                    p++;
                    while (fmt && *p != '}') {
                        while (isalnum((unsigned char)*p) || *p == '_') p++;
                        if (*p++ != '=' || *p++ != '"') { fmt = false; break; }
                        while (*p && *p != '"') p++;
                        if (*p++ != '"') { fmt = false; break; }
                        if (*p == ',') p++;
                    }
                    if (fmt) p++;
                }
                char* e;
                if (fmt && (*p != ' ' || (strtod(p + 1, &e), *e != '\0'))) fmt = false;
                if (!fmt) snprintf(bad, sizeof(bad), "%s", line);
                if (strstr(line, "_bucket{service=\"api\"")) {
                    unsigned long long v = strtoull(strrchr(line, ' ') + 1, NULL, 10);
                    if (v < prev) { fmt = false; snprintf(bad, sizeof(bad), "not cumulative: %s", line); }
                    prev = v;
                    if (strstr(line, "le=\"+Inf\"")) inf = v;
                }
                if (strstr(line, "duration_seconds_count{service=\"api\"}")) cnt = strtoull(strrchr(line, ' ') + 1, NULL, 10);
            }
            *nl = '\n';
            line = nl + 1;
        }
        CHECK(fmt && inf > 0 && inf == cnt, "Prometheus text format: %s (inf %llu, count %llu)", bad, inf, cnt);
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
        SvStats svst;
        sv_server_stats(s, &svst);
        CHECK(svst.requests >= 30 && svst.errors >= 10 && svst.rows_sent > 0, "stats: %lld %lld %lld", svst.requests, svst.errors, svst.rows_sent);
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
        // una richiesta senza sessione non aspetta: legge la versione confermata (MVCC), senza la riga
        g_port = port2;
        pthread_t waiter;
        pthread_create(&waiter, NULL, count_after_commit, NULL);
        pthread_join(waiter, NULL);
        CHECK(g_waiter_done && g_waiter_count == 0, "other request reads the committed version at once: %d", g_waiter_count);
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"COMMIT\"}");
        CHECK(resp_ok(&r) && strstr(r.body, "\"in_transaction\":false"), "commit in a later request: %s", r.body);
        resp_free(&r);
        g_waiter_done = false;
        pthread_create(&waiter, NULL, count_after_commit, NULL);
        pthread_join(waiter, NULL);
        CHECK(g_waiter_count == 1, "after the commit the row is visible: %d", g_waiter_count);
        // il database resta impostato nella sessione; chiusa con una transazione aperta = rollback
        r = request(port2, "POST", "/api/v1/query", hdr, "{\"sql\":\"BEGIN; DELETE FROM conc\"}");
        resp_free(&r);
        char epath[128];
        snprintf(epath, sizeof(epath), "/api/v1/sessions/%s", id);
        r = request(port2, "DELETE", epath, "Authorization: Bearer s3cret\r\n", NULL);
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
        // P2.6: una password cambiata non vale più subito, anche con il login in cache
        q = db_execute("ALTER USER boss IDENTIFIED BY 'boss-new-pass'"); db_result_free(q);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic Ym9zczpib3NzLXBhc3M=\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"sql\":\"SELECT 1\"}");
        CHECK(r.status == 401, "old password refused right after ALTER USER (cached login dropped): %d %s", r.status, r.body);
        resp_free(&r);
        q = db_execute("ALTER USER boss IDENTIFIED BY 'boss-pass'"); db_result_free(q);
        // P2.7: nel registro di audit niente password e niente byte di controllo
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Bearer tok3\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr,
                    "{\"sql\":\"CREATE USER zed IDENTIFIED BY 'zed-s3cret''pw'; DROP USER zed; INSERT INTO p (id, nome) VALUES (77, 'a\\u001b[2Jb'); DELETE FROM p WHERE id = 77\"}");
        CHECK(resp_ok(&r), "statements for the audit check: %s", r.body);
        resp_free(&r);
        // P2.7: il tipo lo decide il parser: SELECT … INTO @v e SELECT di una funzione che scrive vanno nel registro
        r = request(port3, "POST", "/api/v1/query", hdr,
                    "{\"sql\":\"CREATE FUNCTION audf() RETURNS INT BEGIN INSERT INTO p (id, nome) VALUES (78, 'f'); RETURN 1; END; "
                    "SELECT audf(); SELECT 5 INTO @audv; SELECT COUNT(*) FROM p; DELETE FROM p WHERE id = 78; DROP FUNCTION audf\"}");
        CHECK(resp_ok(&r), "statements for the audit-kind check: %s", r.body);
        resp_free(&r);
        // P0.2: un endpoint gira con i permessi di chi l'ha salvato (definer, scritto dal motore), mai come root
        q = db_execute("CREATE USER eva IDENTIFIED BY 'eva-pass'"); db_result_free(q);
        q = db_execute("GRANT ALL ON t2.* TO eva"); db_result_free(q);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic ZXZhOmV2YS1wYXNz\r\n", J);
        r = request(port3, "POST", "/api/v1/query", hdr,
                    "{\"database\":\"t2\",\"sql\":\"CREATE TABLE api_endpoints (name VARCHAR(60) PRIMARY KEY, sql TEXT NOT NULL, "
                    "access VARCHAR(10) NOT NULL DEFAULT 'token', description VARCHAR(200), definer VARCHAR(64)); "
                    "INSERT INTO api_endpoints VALUES ('spia', 'SELECT COUNT(*) FROM t.conc', 'public', '', 'boss')\"}");
        CHECK(resp_ok(&r), "eva saves an endpoint: %s", r.body);
        resp_free(&r);
        q = db_execute("SELECT definer FROM t2.api_endpoints");
        CHECK(q && q->success && q->num_rows == 1 && !strcmp(q->rows[0].cells[0].value, "eva"), "definer written by the engine: %s",
              q && q->success && q->num_rows ? q->rows[0].cells[0].value : "?");
        db_result_free(q);
        r = request(port3, "GET", "/api/v1/endpoints/spia?database=t2", NULL, NULL);
        CHECK(r.status == 401, "'public' saved by a non-administrator still needs a login: %d %s", r.status, r.body);
        resp_free(&r);
        r = request(port3, "GET", "/api/v1/endpoints/spia?database=t2", hdr, NULL);
        CHECK(!resp_ok(&r) && strstr(r.body, "Access denied"), "endpoint runs with the definer's rights: %d %s", r.status, r.body);
        resp_free(&r);
        r = request(port3, "POST", "/api/v1/query", hdr, "{\"database\":\"t2\",\"sql\":\"ALTER TABLE api_endpoints ADD COLUMN x INT\"}");
        CHECK(!resp_ok(&r) && strstr(r.body, "only administrators"), "structure of api_endpoints: admins only: %s", r.body);
        resp_free(&r);
        // tabella senza definer scrivibile da chi non è amministratore: l'endpoint è rifiutato
        r = request(port3, "POST", "/api/v1/query", hdr,
                    "{\"database\":\"t2\",\"sql\":\"DROP TABLE api_endpoints; CREATE TABLE api_endpoints (name VARCHAR(60) PRIMARY KEY, "
                    "sql TEXT NOT NULL, access VARCHAR(10)); INSERT INTO api_endpoints VALUES ('spia', 'SELECT COUNT(*) FROM t.conc', 'public')\"}");
        CHECK(resp_ok(&r), "legacy table: %s", r.body);
        resp_free(&r);
        r = request(port3, "GET", "/api/v1/endpoints/spia?database=t2", hdr, NULL);
        CHECK(r.status == 403 && strstr(r.body, "no author"), "legacy endpoint writable by a non-administrator refused: %d %s", r.status, r.body);
        resp_free(&r);
        // la tabella di prima (t, scrivibile solo dagli amministratori): funziona come prima, anche pubblica
        r = request(port3, "GET", "/api/v1/endpoints/economici?database=t&max=2", NULL, NULL);
        CHECK(resp_ok(&r), "legacy endpoint of an admin-only table still works: %d %s", r.status, r.body);
        resp_free(&r);
        snprintf(hdr, sizeof(hdr), "%sAuthorization: Basic YW5uYTphbm5hLXBhc3M=\r\n", J);
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
        CHECK(!strstr(audit, "SELECT COUNT(*) FROM p"), "audit: reads not logged: %s", audit);
        CHECK(strstr(audit, "SELECT audf()") && strstr(audit, "SELECT 5 INTO @audv"), "audit: SELECT with side effects logged: %s", audit);
        CHECK(strstr(audit, "CREATE USER zed IDENTIFIED BY '***'") && !strstr(audit, "s3cret") && !strstr(audit, "''pw"),
              "audit: password redacted: %s", audit);
        CHECK(!strchr(audit, 0x1b) && strstr(audit, "a\\x1b[2Jb"), "audit: control bytes escaped");
        remove("/tmp/sv_audit_test.log");
    }

    // ── protocollo MySQL (quarto server: --mysql-port con utenti) ──
    {
        QueryResult* q;
        q = db_execute("CREATE USER mia IDENTIFIED BY 'mia-pass'"); db_result_free(q);
        q = db_execute("GRANT SELECT, INSERT ON t.* TO mia"); db_result_free(q);
        // P2.6: utenti per i metodi di login; poi un riavvio del motore, così SHA256(SHA256(pw)) di caching_sha2
        // non è in memoria per nessuno (come dopo il riavvio del server)
        q = db_execute("CREATE USER nat IDENTIFIED WITH mysql_native_password BY 'nat-pass'"); db_result_free(q);
        q = db_execute("CREATE USER rsa IDENTIFIED BY 'rsa-pass'"); db_result_free(q);
        q = db_execute("CREATE USER lock IDENTIFIED BY 'lock-pass'"); db_result_free(q);
        q = db_execute("GRANT SELECT ON t.* TO nat"); db_result_free(q);
        q = db_execute("GRANT SELECT ON t.* TO rsa"); db_result_free(q);
        q = db_execute("GRANT SELECT ON t.* TO lock"); db_result_free(q);
        db_shutdown();
        CHECK(db_init(dir), "restart before the MySQL login tests: %s", db_init_error());
        SvServerConfig cfg4 = { .port = 0, .token = "tok4", .quiet = true, .require_login = true, .mysql = true, .mysql_port = 0,
                                .mysql_socket = "/tmp/sv_test_mysql.sock" };
        SvServer* s4 = sv_server_start(&cfg4, err, sizeof(err));
        int mp = s4 ? sv_server_mysql_port(s4) : 0;
        pthread_t th4;
        if (s4) pthread_create(&th4, NULL, run_server, s4);
        {   // P2.13: MySQL anche sul socket Unix (handshake del protocollo 10), e HTTP su [::1]
            int uf = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un ua;
            memset(&ua, 0, sizeof(ua));
            ua.sun_family = AF_UNIX;
            snprintf(ua.sun_path, sizeof(ua.sun_path), "/tmp/sv_test_mysql.sock");
            uint8_t hs[64] = {0};
            struct timeval tvu = { 3, 0 };
            setsockopt(uf, SOL_SOCKET, SO_RCVTIMEO, &tvu, sizeof(tvu));
            bool okc = uf >= 0 && connect(uf, (struct sockaddr*)&ua, sizeof(ua)) == 0;
            ssize_t gn = okc ? recv(uf, hs, sizeof(hs), 0) : -1;
            struct stat sst;
            CHECK(okc && gn > 5 && hs[4] == 10 && stat("/tmp/sv_test_mysql.sock", &sst) == 0 && (sst.st_mode & 077) == 0,
                  "MySQL handshake over the Unix socket (owner-only): %zd", gn);
            if (uf >= 0) close(uf);
            int f6 = socket(AF_INET6, SOCK_STREAM, 0);
            struct sockaddr_in6 a6;
            memset(&a6, 0, sizeof(a6));
            a6.sin6_family = AF_INET6;
            a6.sin6_port = htons((uint16_t)sv_server_port(s4));
            a6.sin6_addr = in6addr_loopback;
            setsockopt(f6, SOL_SOCKET, SO_RCVTIMEO, &tvu, sizeof(tvu));
            char b6[512] = "";
            if (f6 >= 0 && connect(f6, (struct sockaddr*)&a6, sizeof(a6)) == 0) {
                const char* rq6 = "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                if (send(f6, rq6, strlen(rq6), 0) > 0) { ssize_t g6 = recv(f6, b6, sizeof(b6) - 1, 0); if (g6 > 0) b6[g6] = 0; }
                CHECK(strstr(b6, " 200 ") && strstr(b6, "SegVault"), "HTTP over IPv6 [::1]: %.60s", b6);
            } else printf("(IPv6 loopback not available: skipped)\n");
            if (f6 >= 0) close(f6);
        }
        CHECK(mp > 0, "mysql port: %s", err);
        My m;
        char e[300], out[4096];
        {   // P2.6: caching_sha2_password. Senza SHA256(SHA256(pw)) in memoria, su TCP senza TLS serve l'accesso
            // completo con RSA (qui rifiutato: il client di prova non cifra); sul socket Unix la password passa
            // in chiaro, e da lì in poi l'accesso veloce funziona anche su TCP
            g_my_plugin = "caching_sha2_password";
            CHECK(!my_connect(&m, mp, "mia", "mia-pass", "t", e, sizeof(e)) && g_my_full_auth, "sha2 without cache asks for full auth: %s", e);
            close(m.fd);
            int uf = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un ua;
            memset(&ua, 0, sizeof(ua));
            ua.sun_family = AF_UNIX;
            snprintf(ua.sun_path, sizeof(ua.sun_path), "/tmp/sv_test_mysql.sock");
            struct timeval tvu = { 10, 0 };
            setsockopt(uf, SOL_SOCKET, SO_RCVTIMEO, &tvu, sizeof(tvu));
            m.fd = connect(uf, (struct sockaddr*)&ua, sizeof(ua)) == 0 ? uf : -1;
            CHECK(my_login_fd(&m, "mia", "mia-pass", "t", true, e, sizeof(e)) && g_my_full_auth, "full auth in clear text over the Unix socket: %s", e);
            my_query(&m, "SELECT COUNT(*) FROM p", out, sizeof(out));
            CHECK(!strcmp(out, "3"), "query after full auth: %s", out);
            close(uf);
            CHECK(my_connect(&m, mp, "mia", "mia-pass", "t", e, sizeof(e)) && !g_my_full_auth, "then fast auth over TCP: %s", e);
            close(m.fd);
            CHECK(!my_connect(&m, mp, "mia", "sbagliata", "t", e, sizeof(e)) && strstr(e, "1045"), "sha2 wrong password: %s", e);
            close(m.fd);
            g_my_plugin = "mysql_native_password";
            // un client che parte con native: per chi non ha il verificatore native si passa a caching_sha2
            CHECK(my_connect(&m, mp, "mia", "mia-pass", "t", e, sizeof(e)) && g_my_switches == 1, "native client switched to sha2: %s", e);
            close(m.fd);
            // utente con mysql_native_password: nessun cambio, nessun accesso completo (anche senza cache)
            CHECK(my_connect(&m, mp, "nat", "nat-pass", "t", e, sizeof(e)) && g_my_switches == 0 && !g_my_full_auth, "native user: %s", e);
            close(m.fd);
            // root col token, con i due metodi
            CHECK(my_connect(&m, mp, "root", "tok4", NULL, e, sizeof(e)), "root token, native: %s", e);
            close(m.fd);
            g_my_plugin = "caching_sha2_password";
            CHECK(my_connect(&m, mp, "root", "tok4", NULL, e, sizeof(e)) && !g_my_full_auth, "root token, sha2: %s", e);
            close(m.fd);
            g_my_plugin = "mysql_native_password";
            // RSA: PHP (mysqlnd) chiede la chiave pubblica del server e manda la password cifrata
            if (system("php -n -d extension=mysqli -r 'exit(class_exists(\"mysqli\") ? 0 : 1);' 2>/dev/null") == 0) {
                char cmd[700];
                snprintf(cmd, sizeof(cmd), "php -n -d extension=mysqli -r '"
                         "mysqli_report(MYSQLI_REPORT_OFF); $m = @new mysqli(\"127.0.0.1\", \"rsa\", \"rsa-pass\", \"t\", %d);"
                         "if ($m->connect_errno) { echo \"ERR \", $m->connect_error; exit(1); }"
                         "echo $m->query(\"SELECT COUNT(*) FROM p\")->fetch_row()[0];' 2>&1", mp);
                FILE* pp = popen(cmd, "r");
                char po[400] = "";
                size_t pn = pp ? fread(po, 1, sizeof(po) - 1, pp) : 0;
                po[pn] = '\0';
                if (pp) pclose(pp);
                CHECK(!strcmp(po, "3"), "PHP mysqli, caching_sha2 with the server's RSA key over TCP: %s", po);
            } else printf("(php mysqli not available: RSA login not tested)\n");
            // tentativi sbagliati: dopo 5 l'account (da questo indirizzo) aspetta, anche con la password giusta
            for (int i = 0; i < 5; i++) { my_connect(&m, mp, "lock", "nope", NULL, e, sizeof(e)); close(m.fd); }
            CHECK(!my_connect(&m, mp, "lock", "lock-pass", NULL, e, sizeof(e)) && strstr(e, "Too many wrong passwords"), "back-off after 5 failures: %s", e);
            close(m.fd);
            usleep(1100 * 1000);
            g_my_plugin = "caching_sha2_password";               // lock non ha la cache: si vede che si arriva fino
            CHECK(!my_connect(&m, mp, "lock", "lock-pass", NULL, e, sizeof(e)) && g_my_full_auth,   // alla verifica
                  "login attempted again after the wait: %s", e);
            close(m.fd);
            g_my_plugin = "mysql_native_password";
        }
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
        my_query(&m, "SHOW STATUS LIKE 'Innodb_buffer_pool_read_requests'", out, sizeof(out));
        CHECK(!strncmp(out, "Innodb_buffer_pool_read_requests|", 33) && atoll(out + 33) > 0, "SHOW STATUS has the engine counters: %s", out);
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
        // P2.8: cursore lato server (JDBC useCursorFetch=true): EXECUTE con CURSOR_TYPE_READ_ONLY, poi COM_STMT_FETCH
        {
            int cnp = -1, cnc = -1, est, fst;
            uint32_t cid = my_prepare(&m, "SELECT id FROM p ORDER BY id", &cnp, &cnc, out, sizeof(out));
            long long total = -1;
            my_query(&m, "SELECT COUNT(*) FROM p", out, sizeof(out));
            total = atoll(out);
            est = my_execute_cursor(&m, cid);
            CHECK(est >= 0 && (est & 0x40), "EXECUTE with a cursor: no rows, CURSOR_EXISTS (status 0x%x)", est);
            int got = my_fetch(&m, cid, 2, &fst, out, sizeof(out));
            CHECK(got == (total < 2 ? (int)total : 2) && fst >= 0, "COM_STMT_FETCH 2 rows: %d (status 0x%x) %s", got, fst, out);
            int rest = 0, more;
            while ((more = my_fetch(&m, cid, 1000, &fst, out, sizeof(out))) > 0) rest += more;
            CHECK(more == 0 || (fst & 0x80), "fetch reaches the end");
            CHECK(got + rest == total && (fst & 0x80), "all %lld rows through the cursor (%d + %d), LAST_ROW_SENT (0x%x)", total, got, rest, fst);
            my_fetch(&m, cid, 1, &fst, out, sizeof(out));
            CHECK(fst & 0x80, "fetch after the end: LAST_ROW_SENT again (0x%x) %s", fst, out);
            my_fetch(&m, sid, 1, &fst, out, sizeof(out));
            CHECK(!strncmp(out, "ERR:1421", 8), "fetch without an open cursor: 1421 (%s)", out);
        }
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

        {   // P2.2: il testo di un prepared statement si analizza una volta; ogni EXECUTE riusa l'albero coi valori
            My rt;
            CHECK(my_connect(&rt, mp, "root", "tok4", "t", e, sizeof(e)), "root for the prepared tests: %s", e);
            my_query(&rt, "CREATE TABLE pp (id INT PRIMARY KEY, a VARCHAR(40))", out, sizeof(out));
            my_query(&rt, "INSERT INTO pp VALUES (1, 'uno'), (2, 'due'), (3, 'tre')", out, sizeof(out));
            unsigned long long r0, p0, r1, p1;
            db_prepared_counts(&r0, &p0);
            int pnp, pnc;
            uint32_t q1 = my_prepare(&rt, "SELECT a FROM pp WHERE id = ?", &pnp, &pnc, out, sizeof(out));
            const char* k1[] = { "#1" }, *k2[] = { "#2" }, *k3[] = { "#3" };
            char o1[200], o2[200], o3[200];
            my_execute(&rt, q1, 1, k1, o1, sizeof(o1));
            my_execute(&rt, q1, 1, k2, o2, sizeof(o2));
            my_execute(&rt, q1, 1, k3, o3, sizeof(o3));
            db_prepared_counts(&r1, &p1);
            CHECK(!strcmp(o1, "uno") && !strcmp(o2, "due") && !strcmp(o3, "tre"), "three executes, three values: %s %s %s", o1, o2, o3);
            CHECK(r1 - r0 == 3 && p1 - p0 == 1, "parsed once, run three times from the tree: %llu runs, %llu parses", r1 - r0, p1 - p0);
            // valori difficili: apici, barre, commenti, NULL, negativi, interi a 64 bit
            uint32_t q2 = my_prepare(&rt, "SELECT ?, ?, ?, ?", &pnp, &pnc, out, sizeof(out));
            const char* kv[] = { "it's \\ -- x ? 'y'", NULL, "#-42", "#9223372036854775807" };
            my_execute(&rt, q2, 4, kv, out, sizeof(out));
            CHECK(!strcmp(out, "it's \\ -- x ? 'y'|NULL|-42|9223372036854775807"), "typed values bound into the tree: %s", out);
            // lo schema cambia fra due EXECUTE: l'albero si rifà
            my_query(&rt, "DROP TABLE pp", out, sizeof(out));
            my_query(&rt, "CREATE TABLE pp (x INT, id INT PRIMARY KEY, a VARCHAR(40))", out, sizeof(out));
            my_query(&rt, "INSERT INTO pp VALUES (9, 1, 'nuova')", out, sizeof(out));
            db_prepared_counts(&r0, &p0);
            my_execute(&rt, q1, 1, k1, o1, sizeof(o1));
            db_prepared_counts(&r1, &p1);
            CHECK(!strcmp(o1, "nuova") && p1 - p0 == 1 && r1 - r0 == 1, "re-parsed after DDL: %s (%llu parses)", o1, p1 - p0);
            // NEXTVAL: un EXECUTE, un passo della sequenza (il PREPARE non la tocca)
            my_query(&rt, "CREATE SEQUENCE psq", out, sizeof(out));
            uint32_t q3 = my_prepare(&rt, "SELECT NEXTVAL('psq')", &pnp, &pnc, out, sizeof(out));
            my_execute(&rt, q3, 0, NULL, o1, sizeof(o1));
            my_query(&rt, "SELECT NEXTVAL('psq')", o2, sizeof(o2));
            CHECK(!strcmp(o1, "1") && !strcmp(o2, "2"), "prepared NEXTVAL advances once: %s then %s", o1, o2);
            // ciò che l'albero non copre torna al testo con i valori: UPDATE su più tabelle con un '?' nella condizione
            my_query(&rt, "CREATE TABLE pq (id INT PRIMARY KEY, v INT)", out, sizeof(out));
            my_query(&rt, "INSERT INTO pq VALUES (1, 0)", out, sizeof(out));
            uint32_t q4 = my_prepare(&rt, "UPDATE pq, pp SET pq.v = pp.x WHERE pq.id = pp.id AND pp.a = ?", &pnp, &pnc, out, sizeof(out));
            const char* k4[] = { "nuova" };
            my_execute(&rt, q4, 1, k4, out, sizeof(out));
            my_query(&rt, "SELECT v FROM pq", o1, sizeof(o1));
            CHECK(!strcmp(out, "OK:1") && !strcmp(o1, "9"), "multi-table UPDATE falls back to the text: %s, v = %s", out, o1);
            // velocità: 3000 ricerche per chiave preparate contro 3000 COM_QUERY col valore nel testo
            my_query(&rt, "CREATE TABLE pk (id INT PRIMARY KEY, a VARCHAR(20), b INT)", out, sizeof(out));
            my_query(&rt, "INSERT INTO pk SELECT x, CONCAT('n', x), x * 3 FROM GENERATE_SERIES(1, 3000) AS g(x)", out, sizeof(out));
            uint32_t q5 = my_prepare(&rt, "SELECT a, b FROM pk WHERE id = ?", &pnp, &pnc, out, sizeof(out));
            struct timespec ta, tb, tc;
            clock_gettime(CLOCK_MONOTONIC, &ta);
            bool same = true;
            for (int i = 1; i <= 3000; i++) {
                char kq[24], want[48];
                snprintf(kq, sizeof(kq), "#%d", i);
                const char* kk[] = { kq };
                my_execute(&rt, q5, 1, kk, out, sizeof(out));
                snprintf(want, sizeof(want), "n%d|%d", i, i * 3);
                if (strcmp(out, want)) same = false;
            }
            clock_gettime(CLOCK_MONOTONIC, &tb);
            for (int i = 1; i <= 3000; i++) {
                char qq[80];
                snprintf(qq, sizeof(qq), "SELECT a, b FROM pk WHERE id = %d", i);
                my_query(&rt, qq, out, sizeof(out));
            }
            clock_gettime(CLOCK_MONOTONIC, &tc);
            double tp = (double)(tb.tv_sec - ta.tv_sec) * 1e3 + (double)(tb.tv_nsec - ta.tv_nsec) / 1e6;
            double tq = (double)(tc.tv_sec - tb.tv_sec) * 1e3 + (double)(tc.tv_nsec - tb.tv_nsec) / 1e6;
            printf("(prepared point lookups: %.1f us each, text queries %.1f us: %.2fx)\n", tp * 1000 / 3000, tq * 1000 / 3000, tq / tp);
            CHECK(same, "3000 prepared lookups return the right rows");
            my_query(&rt, "DROP TABLE pk", out, sizeof(out));
            my_query(&rt, "DROP TABLE pq", out, sizeof(out));
            my_query(&rt, "DROP TABLE pp", out, sizeof(out));
            my_query(&rt, "DROP SEQUENCE psq", out, sizeof(out));
            close(rt.fd);
        }

        // P2.8: COM_STATISTICS, COM_SET_OPTION, COM_CHANGE_USER
        {
            uint8_t q1 = 0x09;
            m.seq = 0;
            uint8_t* p; size_t n;
            bool g = my_wr(&m, &q1, 1) && my_rd(&m, &p, &n);
            CHECK(g && n > 7 && !memcmp(p, "Uptime:", 7), "COM_STATISTICS: %.40s", g ? (char*)p : "io");
            if (g) free(p);
            uint8_t q2[3] = { 0x1b, 1, 0 };
            m.seq = 0;
            g = my_wr(&m, q2, 3) && my_rd(&m, &p, &n);
            CHECK(g && p[0] == 0xfe, "COM_SET_OPTION answered with EOF");
            if (g) free(p);
            my_query(&m, "SET @keep = 5", out, sizeof(out));
            my_change_user(&m, "root", "tok4", "t", out, sizeof(out));
            CHECK(!strcmp(out, "OK"), "COM_CHANGE_USER to root: %s", out);
            my_query(&m, "SELECT COUNT(*) FROM p", out, sizeof(out));
            CHECK(!strstr(out, "ERR"), "queries after COM_CHANGE_USER: %s", out);
            my_query(&m, "SHOW VARIABLES LIKE 'max_connections'", out, sizeof(out));
            CHECK(strstr(out, "max_connections"), "max_connections reflects the configuration: %s", out);
        }
        // P2.1: SET @v e FOREIGN_KEY_CHECKS arrivano al motore (prima erano confermati e scartati)
        my_query(&m, "SET @v = 41; SELECT @v + 1", out, sizeof(out));
        CHECK(strstr(out, " / 42"), "user variable over the MySQL protocol: %s", out);
        my_query(&m, "SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0; "
                     "CREATE TABLE fk_child (id INT PRIMARY KEY, par INT, FOREIGN KEY (par) REFERENCES fk_par(id)); "
                     "INSERT INTO fk_child VALUES (1, 7); "
                     "CREATE TABLE fk_par (id INT PRIMARY KEY); INSERT INTO fk_par VALUES (7); "
                     "SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS; SELECT @OLD_FOREIGN_KEY_CHECKS", out, sizeof(out));
        CHECK(!strstr(out, "ERR") && strstr(out, " / 1"), "dump with tables in foreign-key order loads: %s", out);
        my_query(&m, "INSERT INTO fk_child VALUES (2, 99)", out, sizeof(out));
        CHECK(strstr(out, "ERR"), "foreign keys checked again after the dump: %s", out);
        my_query(&m, "DROP TABLE fk_child; DROP TABLE fk_par", out, sizeof(out));
        // P2.8: flag delle colonne dal catalogo (NOT_NULL, PRI_KEY, UNIQUE, MULTIPLE_KEY, UNSIGNED, AUTO_INCREMENT)
        {
            my_query(&m, "CREATE TABLE cflags (id INT UNSIGNED PRIMARY KEY AUTO_INCREMENT, code VARCHAR(10) NOT NULL UNIQUE, "
                         "grp INT, note TEXT, KEY (grp))", out, sizeof(out));
            my_query(&m, "INSERT INTO cflags (code, grp) VALUES ('a', 1)", out, sizeof(out));
            unsigned fl[8] = {0};
            int ncf = my_col_flags(&m, "SELECT id, code, grp, note, id + 1 AS calc FROM cflags", fl, 8);
            CHECK(ncf == 5, "column flags: %d columns", ncf);
            CHECK((fl[0] & 1) && (fl[0] & 2) && (fl[0] & 32) && (fl[0] & 512), "id: NOT_NULL | PRI_KEY | UNSIGNED | AUTO_INCREMENT (0x%x)", fl[0]);
            CHECK((fl[1] & 1) && (fl[1] & 4) && !(fl[1] & 2), "code: NOT_NULL | UNIQUE_KEY (0x%x)", fl[1]);
            CHECK(!(fl[2] & 1) && (fl[2] & 8), "grp: nullable, MULTIPLE_KEY (0x%x)", fl[2]);
            CHECK(!(fl[3] & (1 | 2 | 4 | 8)), "note: no key flags (0x%x)", fl[3]);
            CHECK(!(fl[4] & (2 | 512)), "computed column: no key flags (0x%x)", fl[4]);
            my_query(&m, "DROP TABLE cflags", out, sizeof(out));
        }
        // P2.1: un SET senza effetto su una variabile sconosciuta risponde con un avviso, SHOW WARNINGS lo mostra
        my_query(&m, "SET SESSION sql_mode = 'STRICT_TRANS_TABLES', NAMES utf8mb4", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0") && g_my_warn == 0, "known system variables: no warning (%s, %u)", out, g_my_warn);
        my_query(&m, "SET @@session.no_such_variable_x = 1", out, sizeof(out));
        CHECK(!strcmp(out, "OK:0") && g_my_warn == 1, "unknown system variable: one warning (%s, %u)", out, g_my_warn);
        my_query(&m, "SHOW WARNINGS", out, sizeof(out));
        CHECK(strstr(out, "Warning|1193|") && strstr(out, "no_such_variable_x"), "SHOW WARNINGS names it: %s", out);
        my_query(&m, "SET GLOBAL synchronous = FULL; SELECT @@synchronous; SET GLOBAL synchronous = NORMAL", out, sizeof(out));
        CHECK(strstr(out, "FULL"), "SET GLOBAL synchronous reaches the engine over the MySQL protocol: %s", out);
        my_query(&m, "SELECT 1", out, sizeof(out));
        my_query(&m, "SHOW WARNINGS", out, sizeof(out));
        CHECK(!strstr(out, "no_such_variable_x"), "the warning is cleared by the next statement: %s", out);

        // P0.8: i 256 valori di byte, anche 0x00 e 0xC0, interi attraverso il protocollo MySQL
        {
            char hexall[600] = "", sql[800];
            uint8_t all[256];
            for (int i = 0; i < 256; i++) { all[i] = (uint8_t)i; snprintf(hexall + 2 * i, 3, "%02X", i); }
            my_query(&m, "CREATE TABLE binm (id INT PRIMARY KEY, b BLOB)", out, sizeof(out));
            snprintf(sql, sizeof(sql), "INSERT INTO binm VALUES (1, X'%s')", hexall);
            my_query(&m, sql, out, sizeof(out));
            my_query(&m, "SELECT LENGTH(b), HEX(b) = UNHEX('00') OR 1, HEX(b) FROM binm WHERE id = 1", out, sizeof(out));
            char want[700];
            snprintf(want, sizeof(want), "256|1|%s", hexall);
            CHECK(!strcmp(out, want), "X'' literal with all 256 bytes: %.80s", out);
            int bnp = 0, bnc = 0;
            uint32_t pid = my_prepare(&m, "INSERT INTO binm VALUES (2, ?)", &bnp, &bnc, out, sizeof(out));
            CHECK(pid && bnp == 1, "prepare binary insert: %s", out);
            my_execute_bytes(&m, pid, all, 256, out, sizeof(out));
            CHECK(!strcmp(out, "OK"), "prepared statement with a binary parameter containing NUL: %s", out);
            my_query(&m, "SELECT LENGTH(b), HEX(b) FROM binm WHERE id = 2", out, sizeof(out));
            snprintf(want, sizeof(want), "256|%s", hexall);
            CHECK(!strcmp(out, want), "binary parameter stored byte-exact: %.80s", out);
            // il risultato di testo: i byte veri (letti a mano: 256 byte, il primo è 0x00)
            uint8_t q3[64] = { 3 };
            const char* sel = "SELECT b FROM binm WHERE id = 1";
            memcpy(q3 + 1, sel, strlen(sel));
            m.seq = 0;
            uint8_t* p; size_t n;
            bool okb = my_wr(&m, q3, 1 + strlen(sel));
            for (int i = 0; okb && i < 3; i++) { okb = my_rd(&m, &p, &n); if (okb) free(p); }   // colonne, definizione, EOF
            okb = okb && my_rd(&m, &p, &n);
            size_t pos = 0;
            uint64_t vl = okb ? my_lenenc(p, &pos) : 0;
            CHECK(okb && vl == 256 && !memcmp(p + pos, all, 256), "text result carries the real bytes: %llu", (unsigned long long)vl);
            if (okb) free(p);
            for (int i = 0; i < 1; i++) { if (my_rd(&m, &p, &n)) free(p); }                      // EOF finale
            my_query(&m, "SELECT LENGTH(UNHEX('410042')), LENGTH(CONCAT('a', CHAR(0), 'b')), LENGTH('x\\0y'), HEX('\\0')", out, sizeof(out));
            CHECK(!strcmp(out, "3|3|3|00"), "NUL bytes in functions and escapes: %s", out);
            my_query(&m, "DROP TABLE binm", out, sizeof(out));
        }

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
        // P2.9: SHOW PROCESSLIST e KILL QUERY da un'altra connessione, anche su una scrittura (che non scrive niente)
        {
            my_query(&m, "CREATE TABLE kq (id INT PRIMARY KEY, v INT)", out, sizeof(out));
            for (int b = 0; b < 1500; b += 250) {            // (my_query ha un buffer di 4 KB)
                char insb[4000];
                int l = snprintf(insb, sizeof(insb), "INSERT INTO kq VALUES ");
                for (int i = b; i < b + 250; i++) l += snprintf(insb + l, sizeof(insb) - (size_t)l, "%s(%d, %d)", i > b ? "," : "", i, i);
                my_query(&m, insb, out, sizeof(out));
            }
            const char* longq[2] = { "SELECT COUNT(*) FROM kq a, kq b, kq c WHERE a.v + b.v = c.v + 7777777",
                                     "DELETE FROM kq WHERE id IN (SELECT a.id FROM kq a, kq b, kq c WHERE a.v + b.v + c.v = -1) OR id >= 0" };
            for (int t = 0; t < 2; t++) {
                CcArg ka = { &m, longq[t], "" };
                pthread_t kt;
                pthread_create(&kt, NULL, cc_thread, &ka);
                usleep(300000);
                char pl[4096];
                my_query(&m2, "SHOW PROCESSLIST", pl, sizeof(pl));
                const char* hit = strstr(pl, "kq a, kq b");
                CHECK(hit != NULL && strstr(pl, "Query"), "SHOW PROCESSLIST shows the running statement: %.200s", pl);
                // l'id: inizio della riga che contiene l'istruzione
                const char* row = hit;
                while (row > pl && row[-1] != '/' && row[-1] != '\n') row--;
                while (*row == ' ') row++;
                char kill[64];
                snprintf(kill, sizeof(kill), "KILL QUERY %ld", strtol(row, NULL, 10));
                my_query(&m2, kill, out, sizeof(out));
                CHECK(!strcmp(out, "OK:0"), "%s: %s", kill, out);
                pthread_join(kt, NULL);
                CHECK(strstr(ka.out, "ERR") && strstr(ka.out, "cancel"), "killed statement reports the cancellation: %s", ka.out);
            }
            my_query(&m, "SELECT COUNT(*) FROM kq", out, sizeof(out));
            CHECK(!strcmp(out, "1500"), "the killed DELETE changed nothing: %s", out);
            my_query(&m2, "KILL QUERY 999999", out, sizeof(out));
            CHECK(strstr(out, "ERR:1094"), "unknown thread id: %s", out);
            my_query(&m, "DROP TABLE kq", out, sizeof(out));
        }
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

        // servizi accesi e spenti con il server in funzione; le query MySQL e le email nel registro
        SvServerConfig cfg5 = { .port = 0, .token = "tok5", .quiet = true, .mysql_port = 0, .mail_port = 0, .mail_dir = "/tmp/sv_svc_mail" };
        SvServer* s5 = sv_server_start(&cfg5, err, sizeof(err));
        pthread_t th5;
        pthread_create(&th5, NULL, run_server, s5);
        char e5[300];
        CHECK(!sv_server_service_on(s5, SV_SVC_MYSQL) && sv_server_mysql_port(s5) == 0, "MySQL off at start");
        CHECK(sv_server_service(s5, SV_SVC_MYSQL, true, e5, sizeof(e5)) && sv_server_mysql_port(s5) > 0, "MySQL turned on: %s", e5);
        int mp5 = sv_server_mysql_port(s5);
        My m5;
        CHECK(my_connect(&m5, mp5, "root", "tok5", NULL, e, sizeof(e)), "connect after turning MySQL on: %s", e);
        my_query(&m5, "SELECT 6 * 7", out, sizeof(out));
        CHECK(!strcmp(out, "42"), "query: %s", out);
        SvLogEntry le[4];
        int nle = sv_server_recent(s5, le, 4);
        CHECK(nle >= 1 && !strcmp(le[0].svc, "mysql") && strstr(le[0].path, "SELECT 6 * 7") && !strcmp(le[0].detail, "1 row"),
              "query in the log: %s %s %s", nle ? le[0].svc : "", nle ? le[0].path : "", nle ? le[0].detail : "");
        CHECK(sv_server_service(s5, SV_SVC_MYSQL, false, e5, sizeof(e5)) && !sv_server_service_on(s5, SV_SVC_MYSQL), "MySQL turned off");
        my_query(&m5, "SELECT 1", out, sizeof(out));
        CHECK(!strcmp(out, "read failed") || !strncmp(out, "send", 4), "open MySQL connection closed: %s", out);
        close(m5.fd);
        usleep(300000);
        int fdc = connect_to(mp5);
        CHECK(fdc < 0, "nothing listens on the MySQL port any more");
        if (fdc >= 0) close(fdc);
        r = request(sv_server_port(s5), "GET", "/api/v1/health", NULL, NULL);
        CHECK(r.status == 200, "HTTP untouched by the MySQL switch: %d", r.status);
        resp_free(&r);
        // posta: accesa, un messaggio, nel registro con il servizio "mail"
        CHECK(sv_server_service(s5, SV_SVC_MAIL, true, e5, sizeof(e5)) && sv_server_mail_port(s5) > 0, "mail on: %s", e5);
        int sf = connect_to(sv_server_mail_port(s5));
        char sb[512];
        const char* sm = "HELO x\r\nMAIL FROM:<a@x>\r\nRCPT TO:<b@x>\r\nDATA\r\nSubject: svc test\r\n\r\nhi\r\n.\r\nQUIT\r\n";
        if (send(sf, sm, strlen(sm), 0)) {}
        size_t sg = 0;
        for (;;) { ssize_t g = recv(sf, sb + sg, sizeof(sb) - 1 - sg, 0); if (g <= 0) break; sg += (size_t)g; }
        sb[sg] = 0;
        close(sf);
        nle = sv_server_recent(s5, le, 4);
        CHECK(nle >= 1 && !strcmp(le[0].svc, "mail") && strstr(le[0].detail, "svc test") && strstr(le[0].path, "b@x"),
              "mail in the log: %s %s %s", nle ? le[0].svc : "", nle ? le[0].path : "", nle ? le[0].detail : "");
        CHECK(sv_server_service(s5, SV_SVC_MAIL, false, e5, sizeof(e5)) && sv_server_mail_port(s5) == 0, "mail off");
        r = request(sv_server_port(s5), "GET", "/api/v1/health", NULL, NULL);
        nle = sv_server_recent(s5, le, 1);
        CHECK(nle == 1 && !strcmp(le[0].svc, "api"), "API requests tagged: %s", nle ? le[0].svc : "");
        resp_free(&r);
        sv_server_stop(s5); pthread_join(th5, NULL); sv_server_free(s5);
        if (system("rm -rf /tmp/sv_svc_mail")) {}
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
