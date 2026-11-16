#include "bptree.h"
#include "../storage/page.h"
#include "sv_alloc.h"
#include <string.h>
#include <stdlib.h>

// ── costruzione ─────────────────────────────────────────────────

typedef struct {
    uint32_t page;
    uint16_t klen;
    char     key[BPT_MAX_KEY];
} FirstKey;                                   // prima chiave di ogni pagina di un livello

typedef struct {
    BufferPool* bp;
    uint64_t    lsn;
    uint32_t*   all;                          // pagine prese (per restituirle se qualcosa va storto)
    int         nall, capall;
    FirstKey*   lvl;                          // pagine del livello in costruzione
    int         nlvl, caplvl;
    Page*       cur;
    uint32_t    cur_id;
    bool        inner;
} Builder;

static bool grow_any(void** v, int* cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void* nv = sv_try_realloc(*v, elem * (size_t)nc);
    if (!nv) return false;
    *v = nv;
    *cap = nc;
    return true;
}

static void close_page(Builder* b) {
    if (!b->cur) return;
    bp_unpin(b->bp, b->cur_id, true);
    b->cur = NULL;
}

// pagina nuova del livello, con la sua prima chiave
static bool open_page(Builder* b, const char* key, uint16_t klen) {
    close_page(b);
    uint32_t id;
    if (bp_new_page(b->bp, &id) != SV_OK) return false;
    Page* p = bp_fetch(b->bp, id);
    bp_unpin(b->bp, id, false);                  // il pin di bp_new_page (resta quello di bp_fetch)
    if (!p) return false;
    if (!grow_any((void**)&b->all, &b->capall, b->nall + 1, sizeof(uint32_t)) ||
        !grow_any((void**)&b->lvl, &b->caplvl, b->nlvl + 1, sizeof(FirstKey))) { bp_unpin(b->bp, id, false); return false; }
    b->all[b->nall++] = id;
    p->flags = (uint16_t)(PAGE_FLAG_INDEX | (b->inner ? PAGE_FLAG_INNER : 0));
    page_set_next(p, 0);
    FirstKey* f = &b->lvl[b->nlvl++];
    f->page = id;
    f->klen = klen;
    memcpy(f->key, key, klen);
    b->cur = p;
    b->cur_id = id;
    return true;
}

// una voce nella pagina corrente, o in una nuova se non c'è posto
static bool put(Builder* b, const uint8_t* rec, uint16_t len, const char* key, uint16_t klen) {
    if (b->cur && page_insert(b->cur, rec, len) >= 0) return true;
    if (!open_page(b, key, klen)) return false;
    return page_insert(b->cur, rec, len) >= 0;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int bpt_build(BufferPool* bp, const BptEntry* e, int n, uint64_t lsn, uint32_t* root, int* pages) {
    *root = 0;
    if (pages) *pages = 0;
    Builder b;
    memset(&b, 0, sizeof(b));
    b.bp = bp;
    b.lsn = lsn;
    int rc = SV_OK;
    uint8_t* rec = sv_try_malloc(4 + BPT_MAX_KEY + 6 * BPT_MAX_RIDS + 8);
    if (!rec) return SV_ERR;
    // foglie
    for (int i = 0; i < n && rc == SV_OK; i++) {
        size_t kl = strlen(e[i].key);
        if (e[i].n <= 0) continue;                               // chiave senza righe (cancellate)
        if (kl > BPT_MAX_KEY || e[i].n > BPT_MAX_RIDS) { rc = SV_FULL; break; }
        size_t len = 0;
        put16(rec, (uint16_t)kl); len = 2;
        memcpy(rec + len, e[i].key, kl); len += kl;
        put16(rec + len, (uint16_t)e[i].n); len += 2;
        for (int k = 0; k < e[i].n; k++) { put32(rec + len, e[i].rids[k].page_id); put16(rec + len + 4, e[i].rids[k].slot_id); len += 6; }
        if (!put(&b, rec, (uint16_t)len, e[i].key, (uint16_t)kl)) rc = SV_ERR;
    }
    if (rc == SV_OK && b.nlvl == 0 && !open_page(&b, "", 0)) rc = SV_ERR;   // nessuna chiave: una foglia vuota
    close_page(&b);
    // livelli interni, finché ne resta una pagina sola
    while (rc == SV_OK && b.nlvl > 1) {
        FirstKey* below = b.lvl;
        int nbelow = b.nlvl;
        b.lvl = NULL;
        b.nlvl = b.caplvl = 0;
        b.inner = true;
        for (int i = 0; i < nbelow && rc == SV_OK; i++) {
            size_t len = 0;
            put16(rec, below[i].klen); len = 2;
            memcpy(rec + len, below[i].key, below[i].klen); len += below[i].klen;
            put32(rec + len, below[i].page); len += 4;
            if (!put(&b, rec, (uint16_t)len, below[i].key, below[i].klen)) rc = SV_ERR;
        }
        close_page(&b);
        free(below);
    }
    free(rec);
    if (rc == SV_OK) {
        *root = b.lvl[0].page;
        if (pages) *pages = b.nall;
    } else {
        close_page(&b);
        for (int i = 0; i < b.nall; i++) bp_free_page_now(bp, b.all[i], lsn);
    }
    free(b.all);
    free(b.lvl);
    return rc;
}

// ── ricerca ─────────────────────────────────────────────────────

// chiave della voce s della pagina (false = voce non valida)
static bool slot_key(Page* p, int s, const uint8_t** key, uint16_t* klen, const uint8_t** rest, uint16_t* restlen) {
    uint16_t len = 0;
    const uint8_t* d = page_get_slot(p, (uint16_t)s, &len);
    if (!d || len < 2) return false;
    uint16_t kl = get16(d);
    if ((size_t)kl + 2 > len) return false;
    *key = d + 2;
    *klen = kl;
    *rest = d + 2 + kl;
    *restlen = (uint16_t)(len - 2 - kl);
    return true;
}

static int key_cmp(const uint8_t* a, uint16_t al, const char* b, size_t bl) {
    size_t m = al < bl ? al : bl;
    int c = memcmp(a, b, m);
    if (c) return c;
    return al < bl ? -1 : al > bl;
}

int bpt_lookup(BufferPool* bp, uint32_t root, const char* key, RID** rids, int* n, int* reads) {
    *rids = NULL;
    *n = 0;
    if (reads) *reads = 0;
    size_t kl = strlen(key);
    uint32_t pid = root;
    for (int depth = 0; depth < 16 && pid; depth++) {
        Page* p = bp_fetch(bp, pid);
        if (!p) return -1;
        if (reads) (*reads)++;
        if (!(p->flags & PAGE_FLAG_INDEX)) { bp_unpin(bp, pid, false); return -1; }
        bool inner = (p->flags & PAGE_FLAG_INNER) != 0;
        int lo = 0, hi = p->num_slots - 1, found = -1;
        const uint8_t *k, *rest;
        uint16_t klen, restlen;
        if (inner) {                               // ultima voce con chiave <= key
            while (lo <= hi) {
                int m = lo + (hi - lo) / 2;
                if (!slot_key(p, m, &k, &klen, &rest, &restlen)) { bp_unpin(bp, pid, false); return -1; }
                if (key_cmp(k, klen, key, kl) <= 0) { found = m; lo = m + 1; } else hi = m - 1;
            }
            if (found < 0) { bp_unpin(bp, pid, false); return 0; }      // più piccola di ogni chiave
            if (!slot_key(p, found, &k, &klen, &rest, &restlen) || restlen < 4) { bp_unpin(bp, pid, false); return -1; }
            uint32_t child = get32(rest);
            bp_unpin(bp, pid, false);
            pid = child;
            continue;
        }
        while (lo <= hi) {
            int m = lo + (hi - lo) / 2;
            if (!slot_key(p, m, &k, &klen, &rest, &restlen)) { bp_unpin(bp, pid, false); return -1; }
            int c = key_cmp(k, klen, key, kl);
            if (c == 0) { found = m; break; }
            if (c < 0) lo = m + 1; else hi = m - 1;
        }
        if (found < 0) { bp_unpin(bp, pid, false); return 0; }
        if (restlen < 2) { bp_unpin(bp, pid, false); return -1; }
        int nr = get16(rest);
        if (nr <= 0 || (size_t)restlen < 2 + 6 * (size_t)nr) { bp_unpin(bp, pid, false); return -1; }
        RID* r = sv_try_malloc(sizeof(RID) * (size_t)nr);
        if (!r) { bp_unpin(bp, pid, false); return -1; }
        for (int i = 0; i < nr; i++) {
            r[i].page_id = get32(rest + 2 + 6 * i);
            r[i].slot_id = get16(rest + 2 + 6 * i + 4);
        }
        bp_unpin(bp, pid, false);
        *rids = r;
        *n = nr;
        return 1;
    }
    return -1;                                     // più di 16 livelli: non è un albero scritto da qui
}

bool bpt_pages(BufferPool* bp, uint32_t root, void (*fn)(void* ctx, uint32_t page), void* ctx) {
    uint32_t* st = NULL;
    int n = 0, cap = 0;
    bool ok = grow_any((void**)&st, &cap, 1, sizeof(uint32_t));
    if (ok) st[n++] = root;
    int guard = 0;
    while (ok && n > 0 && guard++ < (1 << 24)) {
        uint32_t pid = st[--n];
        Page* p = bp_fetch(bp, pid);
        if (!p) { ok = false; break; }
        if (!(p->flags & PAGE_FLAG_INDEX)) { bp_unpin(bp, pid, false); ok = false; break; }
        if (p->flags & PAGE_FLAG_INNER) {
            for (int s = 0; s < p->num_slots && ok; s++) {
                const uint8_t *k, *rest;
                uint16_t klen, restlen;
                if (!slot_key(p, s, &k, &klen, &rest, &restlen) || restlen < 4) { ok = false; break; }
                if (!grow_any((void**)&st, &cap, n + 1, sizeof(uint32_t))) { ok = false; break; }
                st[n++] = get32(rest);
            }
        }
        bp_unpin(bp, pid, false);
        fn(ctx, pid);
    }
    free(st);
    return ok;
}
