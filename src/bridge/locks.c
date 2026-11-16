/*
SCOPO: gestore dei lock delle tabelle (vedi locks.h).
       Tabella dei lock: (database, tabella) -> proprietari con il loro modo. Un mutex proprio la
       protegge. Le attese sono in coda (FIFO): chi aspetta riceve un numero d'ordine e chi arriva dopo
       non lo scavalca su una tabella in conflitto, così un flusso continuo di letture non affama una
       scrittura. Chi aspetta dorme su una variabile di condizione, svegliato a ogni rilascio (nessun
       polling); il rilevamento dei deadlock segue tutto il grafo delle attese (titolari e chi è in coda
       davanti) con un insieme dei visitati.
*/
#include "locks.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "sv_alloc.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  static SRWLOCK            g_mx = SRWLOCK_INIT;
  static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
  static void mx_lock(void)   { AcquireSRWLockExclusive(&g_mx); }
  static void mx_unlock(void) { ReleaseSRWLockExclusive(&g_mx); }
  static void cv_wait_ms(int ms) { SleepConditionVariableSRW(&g_cv, &g_mx, (DWORD)(ms > 0 ? ms : 0), 0); }
  static void cv_broadcast(void) { WakeAllConditionVariable(&g_cv); }
  static void cv_init(void) {}
#else
  #include <pthread.h>
  #include <time.h>
  #include <errno.h>
  static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
  static pthread_cond_t  g_cv;
  static void mx_lock(void)   { pthread_mutex_lock(&g_mx); }
  static void mx_unlock(void) { pthread_mutex_unlock(&g_mx); }
  static void cv_init(void) {
      pthread_condattr_t a;
      pthread_condattr_init(&a);
  #if !defined(__APPLE__)
      pthread_condattr_setclock(&a, CLOCK_MONOTONIC);     // l'ora di sistema che cambia non allunga le attese
  #endif
      pthread_cond_init(&g_cv, &a);
      pthread_condattr_destroy(&a);
  }
  static void cv_wait_ms(int ms) {
      if (ms <= 0) return;
      struct timespec ts;
  #if !defined(__APPLE__)
      clock_gettime(CLOCK_MONOTONIC, &ts);
  #else
      clock_gettime(CLOCK_REALTIME, &ts);
  #endif
      ts.tv_sec  += ms / 1000;
      ts.tv_nsec += (long)(ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cv, &g_mx, &ts);
  }
  static void cv_broadcast(void) { pthread_cond_broadcast(&g_cv); }
#endif

typedef struct { const void* owner; uint8_t mode; } LkHolder;

typedef struct {
    int       db;
    char      table[64];
    LkHolder* h;
    int       nh, caph;
} LkEntry;

// attesa in corso: owner aspetta i lock req (tutti), in coda col numero ticket
typedef struct {
    const void*        owner;
    unsigned long long ticket;
    LkReq*             req;
    int                n;
} LkWait;

static LkEntry* g_e;
static int      g_ne, g_cape;
static LkWait*  g_w;
static int      g_nw, g_capw;
static bool     g_ready;
static unsigned long long g_ticket;          // ultimo numero d'ordine dato
static unsigned long long g_seq;             // cambia a ogni rilascio di un lock (sveglia chi aspetta)
static unsigned long long g_sess_seq;        // cambia quando una sessione esce dal motore (lk_notify)
static unsigned long long g_lk_waits, g_lk_deadlocks;

void lk_init(void) {
    if (g_ready) return;
    cv_init();
    g_ready = true;
}

static LkEntry* find(int db, const char* table, bool create) {
    for (int i = 0; i < g_ne; i++) if (g_e[i].db == db && !strcmp(g_e[i].table, table)) return &g_e[i];
    if (!create) return NULL;
    if (g_ne >= g_cape) {
        int nc = g_cape ? g_cape * 2 : 64;
        LkEntry* ne = sv_realloc(g_e, sizeof(LkEntry) * (size_t)nc);
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

static uint8_t held_mode(const LkEntry* e, const void* owner) {
    for (int i = 0; e && i < e->nh; i++) if (e->h[i].owner == owner) return e->h[i].mode;
    return 0;
}

static bool conflicts(uint8_t a, uint8_t b) { return a == LK_X || b == LK_X; }

static LkWait* wait_of(const void* owner) {
    for (int i = 0; i < g_nw; i++) if (g_w[i].owner == owner) return &g_w[i];
    return NULL;
}

static void drop_wait(const void* owner) {
    for (int i = 0; i < g_nw; i++)
        if (g_w[i].owner == owner) { free(g_w[i].req); g_w[i] = g_w[--g_nw]; return; }
}

// registra (o aggiorna) l'attesa di owner; il numero d'ordine resta quello della prima volta
static LkWait* set_wait(const void* owner, const LkReq* req, int n, bool* is_new) {
    LkWait* w = wait_of(owner);
    *is_new = w == NULL;
    if (!w) {
        if (g_nw >= g_capw) {
            int nc = g_capw ? g_capw * 2 : 16;
            LkWait* nw = sv_realloc(g_w, sizeof(LkWait) * (size_t)nc);
            if (!nw) return NULL;
            g_w = nw;
            g_capw = nc;
        }
        w = &g_w[g_nw++];
        memset(w, 0, sizeof(*w));
        w->owner = owner;
        w->ticket = ++g_ticket;
    }
    LkReq* copy = malloc(sizeof(LkReq) * (size_t)(n > 0 ? n : 1));
    if (copy) {
        memcpy(copy, req, sizeof(LkReq) * (size_t)n);
        free(w->req);
        w->req = copy;
        w->n = n;
    }
    return w;
}

// chi impedisce a owner (in coda con my_ticket, 0 = non ancora in coda: dietro a tutti) di prendere q:
// i titolari in conflitto e, se owner non tiene già la tabella, chi è in coda prima di lui in conflitto.
// cb(o) per ognuno; ritorna il numero trovato
typedef void (*BlockerFn)(const void* o, void* ud);
static int blockers_of(const void* owner, unsigned long long my_ticket, const LkReq* q, BlockerFn cb, void* ud) {
    int k = 0;
    LkEntry* e = find(q->db, q->table, false);
    uint8_t mine = held_mode(e, owner);
    if (mine >= q->mode) return 0;                       // già suo (anche S dentro X)
    for (int j = 0; e && j < e->nh; j++) {
        if (e->h[j].owner == owner || !conflicts(q->mode, e->h[j].mode)) continue;
        k++;
        if (cb) cb(e->h[j].owner, ud);
    }
    if (mine) return k;                                  // conversione S -> X: non si mette in coda
    for (int i = 0; i < g_nw; i++) {
        const LkWait* w = &g_w[i];
        if (w->owner == owner || (my_ticket && w->ticket >= my_ticket)) continue;
        for (int r = 0; r < w->n; r++)
            if (w->req[r].db == q->db && !strcmp(w->req[r].table, q->table) && conflicts(q->mode, w->req[r].mode)) {
                k++;
                if (cb) cb(w->owner, ud);
                break;
            }
    }
    return k;
}

static bool all_free(const void* owner, unsigned long long my_ticket, const LkReq* req, int n) {
    for (int i = 0; i < n; i++) if (blockers_of(owner, my_ticket, &req[i], NULL, NULL)) return false;
    return true;
}

static bool grant(LkEntry* e, const void* owner, uint8_t mode) {
    for (int i = 0; i < e->nh; i++)
        if (e->h[i].owner == owner) { if (mode > e->h[i].mode) e->h[i].mode = mode; return true; }   // S -> X
    if (e->nh >= e->caph) {
        int nc = e->caph ? e->caph * 2 : 4;
        LkHolder* nh = sv_realloc(e->h, sizeof(LkHolder) * (size_t)nc);
        if (!nh) return false;
        e->h = nh;
        e->caph = nc;
    }
    e->h[e->nh].owner = owner;
    e->h[e->nh].mode = mode;
    e->nh++;
    return true;
}

static bool take_all(const void* owner, const LkReq* req, int n) {
    for (int i = 0; i < n; i++) {
        LkEntry* e = find(req[i].db, req[i].table, true);
        if (!e || !grant(e, owner, req[i].mode)) return false;
    }
    return true;
}

// ── rilevamento dei deadlock: visita del grafo "aspetta" partendo da chi blocca owner ──
typedef struct {
    const void** stack;
    int          ns, caps;
    const void** seen;
    int          nseen, capseen;
    bool         oom;
} Walk;

static bool push_unique(const void*** arr, int* n, int* cap, const void* o) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 32;
        const void** na = realloc((void*)*arr, sizeof(void*) * (size_t)nc);
        if (!na) return false;
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = o;
    return true;
}

static void walk_visit(const void* o, void* ud) {
    Walk* w = ud;
    for (int i = 0; i < w->nseen; i++) if (w->seen[i] == o) return;
    if (!push_unique(&w->seen, &w->nseen, &w->capseen, o) || !push_unique(&w->stack, &w->ns, &w->caps, o)) w->oom = true;
}

// owner, che aspetterebbe req, chiuderebbe un ciclo? (tutti i nodi raggiungibili, ognuno una volta)
static bool would_deadlock(const void* owner, unsigned long long my_ticket, const LkReq* req, int n) {
    Walk w = {0};
    for (int i = 0; i < n; i++) blockers_of(owner, my_ticket, &req[i], walk_visit, &w);
    bool cycle = false;
    while (w.ns > 0 && !cycle && !w.oom) {
        const void* o = w.stack[--w.ns];
        if (o == owner) { cycle = true; break; }
        const LkWait* ow = wait_of(o);
        if (!ow) continue;                               // o non aspetta nessuno: il ramo finisce
        for (int r = 0; r < ow->n; r++) blockers_of(o, ow->ticket, &ow->req[r], walk_visit, &w);
    }
    for (int i = 0; i < w.nseen && !cycle; i++) if (w.seen[i] == owner) cycle = true;
    free((void*)w.stack);
    free((void*)w.seen);
    return cycle;
}

void lk_stats(unsigned long long* waits, unsigned long long* deadlocks) {
    mx_lock();
    *waits = g_lk_waits;
    *deadlocks = g_lk_deadlocks;
    mx_unlock();
}

int lk_acquire(const void* owner, const LkReq* req, int n) {
    if (n <= 0) return LK_GRANTED;
    mx_lock();
    LkWait* w = wait_of(owner);
    unsigned long long ticket = w ? w->ticket : 0;
    int r;
    if (all_free(owner, ticket ? ticket : ~0ULL, req, n)) {
        r = take_all(owner, req, n) ? LK_GRANTED : LK_WAIT;
        if (r == LK_GRANTED && w) { drop_wait(owner); g_seq++; cv_broadcast(); }   // la coda è avanzata
    } else if (would_deadlock(owner, ticket ? ticket : ~0ULL, req, n)) {
        if (w) { drop_wait(owner); g_seq++; cv_broadcast(); }
        g_lk_deadlocks++;
        r = LK_DEADLOCK;
    } else {
        bool is_new = false;
        set_wait(owner, req, n, &is_new);
        if (is_new) g_lk_waits++;                        // un'attesa conta una volta, non a ogni nuovo tentativo
        r = LK_WAIT;
    }
    mx_unlock();
    return r;
}

bool lk_try(const void* owner, const LkReq* req, int n) {
    if (n <= 0) return true;
    mx_lock();
    bool ok = all_free(owner, ~0ULL, req, n) && take_all(owner, req, n);   // dietro a chi è in coda
    mx_unlock();
    return ok;
}

void lk_cancel_wait(const void* owner) {
    mx_lock();
    if (wait_of(owner)) { drop_wait(owner); g_seq++; cv_broadcast(); }
    mx_unlock();
}

void lk_reset(void) {
    mx_lock();
    for (int i = 0; i < g_ne; i++) free(g_e[i].h);
    g_ne = 0;
    for (int i = 0; i < g_nw; i++) free(g_w[i].req);
    g_nw = 0;
    g_seq++;
    cv_broadcast();
    mx_unlock();
}

void lk_release_all(const void* owner) {
    mx_lock();
    bool changed = false;
    for (int i = 0; i < g_ne; ) {
        LkEntry* e = &g_e[i];
        for (int j = 0; j < e->nh; ) {
            if (e->h[j].owner == owner) { e->h[j] = e->h[--e->nh]; changed = true; }
            else j++;
        }
        if (e->nh == 0) { free(e->h); g_e[i] = g_e[--g_ne]; }
        else i++;
    }
    if (wait_of(owner)) { drop_wait(owner); changed = true; }
    if (changed) { g_seq++; cv_broadcast(); }
    mx_unlock();
}

void lk_modes(const void* owner, const LkReq* req, int n, uint8_t* out) {
    mx_lock();
    for (int i = 0; i < n; i++) out[i] = held_mode(find(req[i].db, req[i].table, false), owner);
    mx_unlock();
}

void lk_release_reads(const void* owner, const LkReq* req, int n, const uint8_t* prior) {
    mx_lock();
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (prior[i] || req[i].db < 0) continue;         // tenuto prima di questo statement, o il lock globale
        LkEntry* e = find(req[i].db, req[i].table, false);
        for (int j = 0; e && j < e->nh; j++)
            if (e->h[j].owner == owner) {
                if (e->h[j].mode == LK_S) { e->h[j] = e->h[--e->nh]; changed = true; }
                break;
            }
        if (e && e->nh == 0) { free(e->h); *e = g_e[--g_ne]; }
    }
    if (changed) { g_seq++; cv_broadcast(); }
    mx_unlock();
}

unsigned long long lk_seq(void) {
    mx_lock();
    unsigned long long s = g_seq;
    mx_unlock();
    return s;
}

void lk_notify(void) {
    mx_lock();
    g_sess_seq++;
    cv_broadcast();
    mx_unlock();
}

unsigned long long lk_sess_seq(void) {
    mx_lock();
    unsigned long long s = g_sess_seq;
    mx_unlock();
    return s;
}

bool lk_wait_sess(unsigned long long seen, int max_ms) {
    mx_lock();
    if (g_sess_seq == seen) cv_wait_ms(max_ms);
    bool changed = g_sess_seq != seen;
    mx_unlock();
    return changed;
}

bool lk_wait_change(unsigned long long seen, int max_ms) {
    mx_lock();
    if (g_seq == seen) cv_wait_ms(max_ms);
    bool changed = g_seq != seen;
    mx_unlock();
    return changed;
}

int lk_count(const void* owner) {
    mx_lock();
    int n = 0;
    for (int i = 0; i < g_ne; i++) for (int j = 0; j < g_e[i].nh; j++) if (g_e[i].h[j].owner == owner) n++;
    mx_unlock();
    return n;
}

static void first_blocker(const void* o, void* ud) { const void** p = ud; if (!*p) *p = o; }

const void* lk_blocker(const void* owner, const LkReq* req, int n) {
    mx_lock();
    const void* b = NULL;
    LkWait* w = wait_of(owner);
    for (int i = 0; i < n && !b; i++) blockers_of(owner, w ? w->ticket : ~0ULL, &req[i], first_blocker, &b);
    mx_unlock();
    return b;
}

const void* lk_x_holder(int db, const char* table, const void* except) {
    mx_lock();
    const void* o = NULL;
    LkEntry* e = find(db, table, false);
    for (int j = 0; e && j < e->nh && !o; j++)
        if (e->h[j].owner != except && e->h[j].mode == LK_X) o = e->h[j].owner;
    mx_unlock();
    return o;
}
