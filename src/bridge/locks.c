/*
SCOPO: gestore dei lock delle tabelle (vedi locks.h).
       Tabella dei lock: (database, tabella) -> proprietari con il loro modo. Un mutex proprio la
       protegge; le attese le fa il chiamante (polling breve), così il gestore non dipende da
       variabili di condizione diverse per ogni sistema.
*/
#include "locks.h"
#include "../platform_lock.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

typedef struct { const void* owner; uint8_t mode; } LkHolder;

typedef struct {
    int       db;
    char      table[64];
    LkHolder* h;
    int       nh, caph;
} LkEntry;

// attesa in corso (per il grafo dei deadlock): owner aspetta i lock req
typedef struct {
    const void* owner;
    LkReq       req[32];
    int         n;
} LkWait;

static SvRwLock g_mx;
static LkEntry* g_e;
static int      g_ne, g_cape;
static LkWait*  g_w;
static int      g_nw, g_capw;
static bool     g_ready;

void lk_init(void) {
    if (g_ready) return;
    sv_rw_init(&g_mx);
    g_ready = true;
}

static LkEntry* find(int db, const char* table, bool create) {
    for (int i = 0; i < g_ne; i++) if (g_e[i].db == db && !strcmp(g_e[i].table, table)) return &g_e[i];
    if (!create) return NULL;
    if (g_ne >= g_cape) {
        int nc = g_cape ? g_cape * 2 : 64;
        LkEntry* ne = realloc(g_e, sizeof(LkEntry) * (size_t)nc);
        if (!ne) return NULL;
        g_e = ne;
        g_cape = nc;
    }
    LkEntry* e = &g_e[g_ne++];
    memset(e, 0, sizeof(*e));
    e->db = db;
    snprintf(e->table, sizeof(e->table), "%s", table);
    return e;
}

// owner può avere il lock? (i suoi lock non contano come conflitto)
static bool compatible(const LkEntry* e, const void* owner, uint8_t mode) {
    for (int i = 0; i < e->nh; i++) {
        if (e->h[i].owner == owner) continue;
        if (mode == LK_X || e->h[i].mode == LK_X) return false;
    }
    return true;
}

static bool grant(LkEntry* e, const void* owner, uint8_t mode) {
    for (int i = 0; i < e->nh; i++)
        if (e->h[i].owner == owner) { if (mode > e->h[i].mode) e->h[i].mode = mode; return true; }   // S -> X
    if (e->nh >= e->caph) {
        int nc = e->caph ? e->caph * 2 : 4;
        LkHolder* nh = realloc(e->h, sizeof(LkHolder) * (size_t)nc);
        if (!nh) return false;
        e->h = nh;
        e->caph = nc;
    }
    e->h[e->nh].owner = owner;
    e->h[e->nh].mode = mode;
    e->nh++;
    return true;
}

static bool all_free(const void* owner, const LkReq* req, int n) {
    for (int i = 0; i < n; i++) {
        LkEntry* e = find(req[i].db, req[i].table, false);
        if (e && !compatible(e, owner, req[i].mode)) return false;
    }
    return true;
}

static bool take_all(const void* owner, const LkReq* req, int n) {
    for (int i = 0; i < n; i++) {
        LkEntry* e = find(req[i].db, req[i].table, true);
        if (!e || !grant(e, owner, req[i].mode)) return false;
    }
    return true;
}

static LkWait* wait_of(const void* owner, bool create) {
    for (int i = 0; i < g_nw; i++) if (g_w[i].owner == owner) return &g_w[i];
    if (!create) return NULL;
    if (g_nw >= g_capw) {
        int nc = g_capw ? g_capw * 2 : 16;
        LkWait* nw = realloc(g_w, sizeof(LkWait) * (size_t)nc);
        if (!nw) return NULL;
        g_w = nw;
        g_capw = nc;
    }
    LkWait* w = &g_w[g_nw++];
    memset(w, 0, sizeof(*w));
    w->owner = owner;
    return w;
}

static void drop_wait(const void* owner) {
    for (int i = 0; i < g_nw; i++)
        if (g_w[i].owner == owner) { g_w[i] = g_w[--g_nw]; return; }
}

// proprietari che bloccano owner sui lock req
static int blockers(const void* owner, const LkReq* req, int n, const void** out, int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        LkEntry* e = find(req[i].db, req[i].table, false);
        if (!e) continue;
        for (int j = 0; j < e->nh; j++) {
            const void* o = e->h[j].owner;
            if (o == owner || (req[i].mode != LK_X && e->h[j].mode != LK_X)) continue;
            bool seen = false;
            for (int q = 0; q < k; q++) if (out[q] == o) seen = true;
            if (!seen && k < max) out[k++] = o;
        }
    }
    return k;
}

// owner raggiunge target seguendo le attese? (ricerca in profondità con limite)
static bool reaches(const void* from, const void* target, int depth) {
    if (depth > 64) return false;
    LkWait* w = wait_of(from, false);
    if (!w) return false;
    const void* b[64];
    int nb = blockers(from, w->req, w->n, b, 64);
    for (int i = 0; i < nb; i++) {
        if (b[i] == target) return true;
        if (reaches(b[i], target, depth + 1)) return true;
    }
    return false;
}

int lk_acquire(const void* owner, const LkReq* req, int n) {
    if (n <= 0) return LK_GRANTED;
    sv_rw_wr(&g_mx);
    int r;
    if (all_free(owner, req, n)) {
        r = take_all(owner, req, n) ? LK_GRANTED : LK_WAIT;
        if (r == LK_GRANTED) drop_wait(owner);
    } else {
        // chi blocca owner aspetta (direttamente o no) owner stesso? Allora è un deadlock
        const void* b[64];
        int nb = blockers(owner, req, n, b, 64);
        bool cycle = false;
        LkWait* w = wait_of(owner, true);
        if (w) {
            w->n = n < 32 ? n : 32;
            memcpy(w->req, req, sizeof(LkReq) * (size_t)w->n);
        }
        for (int i = 0; i < nb && !cycle; i++) cycle = b[i] == owner || reaches(b[i], owner, 0);
        if (cycle) drop_wait(owner);
        r = cycle ? LK_DEADLOCK : LK_WAIT;
    }
    sv_rw_wrunlock(&g_mx);
    return r;
}

bool lk_try(const void* owner, const LkReq* req, int n) {
    if (n <= 0) return true;
    sv_rw_wr(&g_mx);
    bool ok = all_free(owner, req, n) && take_all(owner, req, n);
    sv_rw_wrunlock(&g_mx);
    return ok;
}

void lk_cancel_wait(const void* owner) {
    sv_rw_wr(&g_mx);
    drop_wait(owner);
    sv_rw_wrunlock(&g_mx);
}

void lk_release_all(const void* owner) {
    sv_rw_wr(&g_mx);
    for (int i = 0; i < g_ne; ) {
        LkEntry* e = &g_e[i];
        for (int j = 0; j < e->nh; ) {
            if (e->h[j].owner == owner) e->h[j] = e->h[--e->nh];
            else j++;
        }
        if (e->nh == 0) { free(e->h); g_e[i] = g_e[--g_ne]; }
        else i++;
    }
    drop_wait(owner);
    sv_rw_wrunlock(&g_mx);
}

int lk_count(const void* owner) {
    sv_rw_wr(&g_mx);
    int n = 0;
    for (int i = 0; i < g_ne; i++) for (int j = 0; j < g_e[i].nh; j++) if (g_e[i].h[j].owner == owner) n++;
    sv_rw_wrunlock(&g_mx);
    return n;
}

const void* lk_blocker(const void* owner, const LkReq* req, int n) {
    sv_rw_wr(&g_mx);
    const void* b[1];
    int nb = blockers(owner, req, n, b, 1);
    sv_rw_wrunlock(&g_mx);
    return nb ? b[0] : NULL;
}

const void* lk_x_holder(int db, const char* table, const void* except) {
    sv_rw_wr(&g_mx);
    const void* o = NULL;
    LkEntry* e = find(db, table, false);
    for (int j = 0; e && j < e->nh && !o; j++)
        if (e->h[j].owner != except && e->h[j].mode == LK_X) o = e->h[j].owner;
    sv_rw_wrunlock(&g_mx);
    return o;
}
