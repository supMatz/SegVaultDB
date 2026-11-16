#include "buffer_pool.h"
#include "platform_compat.h"
#include "sv_alloc.h"
#ifndef _WIN32
  #include <sched.h>
#endif

// ── partizioni (P1.10) ──────────────────────────────────────────
// Ogni pagina vive in una sola partizione (scelta dal page_id): ricerca, pin, CLOCK e statistiche di pagine
// diverse non passano più da un latch unico. Una pagina che manca si legge dal disco senza tenere il latch:
// il suo frame resta "loading" (pinnato, nella tabella hash) e chi la chiede nel frattempo aspetta lei sola.
// Le scritture di pagine modificate (sfratto di una pagina sporca, flush) passano da wmx: before_write (il
// log va scritto prima della pagina) non è pensato per più thread insieme.

static unsigned mix_id(uint32_t page_id) { return (unsigned)(page_id * 2654435761u); }

static BpPart* part_of(BufferPool* bp, uint32_t page_id) {
    return &bp->parts[bp->nparts > 1 ? (mix_id(page_id) >> 4) % (unsigned)bp->nparts : 0];
}

static unsigned bp_hash(const BpPart* pt, uint32_t page_id) {
    return (mix_id(page_id) >> 7) & (unsigned)(pt->nbuckets - 1);   // Knuth
}

static void yield_cpu(void) {
#ifdef _WIN32
    sv_sleep_ms(0);
#else
    sched_yield();
#endif
}

static bool part_init(BpPart* pt, int capacity) {
    memset(pt, 0, sizeof(*pt));
    int nb = 1;
    while (nb < capacity * 2) nb <<= 1;
    pt->frames = SV_ALLOC_N(Frame, capacity);
    pt->buckets = sv_malloc(sizeof(int) * (size_t)nb);
    if (!pt->frames || !pt->buckets) { free(pt->frames); free(pt->buckets); return false; }
    for (int i = 0; i < nb; i++) pt->buckets[i] = -1;
    for (int i = 0; i < capacity; i++) pt->frames[i].hnext = -1;
    pt->nbuckets = nb;
    pt->capacity = capacity;
    sv_rw_init(&pt->latch);
    return true;
}

static void part_free_frames(BpPart* pt) {
    for (int i = 0; pt->frames && i < pt->capacity; i++) if (pt->frames[i].page) page_free(pt->frames[i].page);
    free(pt->frames);
    free(pt->buckets);
    pt->frames = NULL;
    pt->buckets = NULL;
}

// partizioni per una cache di capacity frame: almeno 64 frame ciascuna
static int nparts_for(int capacity) {
    int n = BP_PARTS;
    while (n > 1 && capacity / n < 64) n /= 2;
    return n;
}

static bool parts_build(BufferPool* bp, int capacity) {
    int n = nparts_for(capacity);
    for (int i = 0; i < n; i++) {
        int c = capacity / n + (i < capacity % n ? 1 : 0);
        if (!part_init(&bp->parts[i], c)) { for (int k = 0; k < i; k++) part_free_frames(&bp->parts[k]); return false; }
    }
    bp->nparts = n;
    bp->capacity = capacity;
    return true;
}

BufferPool* bp_create(int capacity, int db_fd) {
    if (capacity < 4) capacity = 4;
    BufferPool* bp = SV_ALLOC(BufferPool);
    if (!bp) return NULL;
    if (!parts_build(bp, capacity)) { free(bp); return NULL; }
    sv_rw_init(&bp->alloc);
    sv_rw_init(&bp->wmx);
    bp->db_fd = db_fd;
    return bp;
}

// Cerca un page_id nella partizione, ritorna l'indice del frame o -1
static int part_find(BpPart* pt, uint32_t page_id) {
    for (int i = pt->buckets[bp_hash(pt, page_id)]; i >= 0; i = pt->frames[i].hnext)
        if (pt->frames[i].pid == page_id) return i;
    return -1;
}

static void hash_insert(BpPart* pt, int idx) {
    unsigned h = bp_hash(pt, pt->frames[idx].pid);
    pt->frames[idx].hnext = pt->buckets[h];
    pt->buckets[h] = idx;
}

static void hash_remove(BpPart* pt, int idx) {
    unsigned h = bp_hash(pt, pt->frames[idx].pid);
    int* pp = &pt->buckets[h];
    while (*pp >= 0 && *pp != idx) pp = &pt->frames[*pp].hnext;
    if (*pp == idx) *pp = pt->frames[idx].hnext;
    pt->frames[idx].hnext = -1;
}

static bool before_write(BufferPool* bp, Page* const* pages, int n) {
    if (bp->before_write && !bp->before_write(bp->before_write_ctx, pages, n)) { bp->io_error = true; return false; }
    return true;
}

static bool write_page_raw(BufferPool* bp, BpPart* pt, Frame* f) {
    bool ov = (f->page->flags & 0x08) != 0;            // 0x08 = pagina di overflow
    // una pagina di heap (con stub che puntano a catene) non arriva su disco prima delle catene che riferisce:
    // le catene scritte da poco vanno su disco prima (le catene liberate si riusano subito, P1.5)
    if (!ov && bp->ov_unsynced) {
        if (sv_fsync(bp->db_fd) != 0) { bp->io_error = true; return false; }
        bp->ov_unsynced = false;
    }
    if (ov) bp->ov_unsynced = true;
    if (page_write(f->page, bp->db_fd) != SV_OK) {
        if (!bp->io_error) fprintf(stderr, "[SegVault] cannot write page %u of the data file\n", f->page->page_id);
        bp->io_error = true;
        return false;
    }
    f->dirty = false;
    pt->stats.dirty_writes++;
    return true;
}

// scrive una pagina modificata (sotto wmx); un errore resta (io_error) e la pagina resta sporca in memoria
static bool write_dirty(BufferPool* bp, BpPart* pt, Frame* f) {
    if (!f->dirty) return true;
    sv_rw_wr(&bp->wmx);
    Page* one = f->page;
    // immagine solo per le pagine che esistevano all'ultimo checkpoint (le altre si rifanno dal log da vuote)
    // e non di overflow (0x08: immutabili, ricostruite dal record della riga)
    bool ok = before_write(bp, &one, (one->flags & 0x08) || f->fresh ? 0 : 1) && write_page_raw(bp, pt, f);
    sv_rw_wrunlock(&bp->wmx);
    return ok;
}

// libera il frame (scrivendo la pagina se modificata); false se la scrittura non è riuscita: il
// frame resta com'è (una pagina sporca non si scarta mai, le letture successive la vedrebbero vecchia)
static bool frame_release(BufferPool* bp, BpPart* pt, int idx) {
    Frame* f = &pt->frames[idx];
    if (!f->page || f->loading) return f->page == NULL;
    if (!write_dirty(bp, pt, f)) return false;
    hash_remove(pt, idx);
    page_free(f->page);
    f->page      = NULL;
    f->dirty     = false;
    f->ref       = false;
    f->pin_count = 0;
    pt->num_used--;
    return true;
}

// Trova un frame da usare: uno libero, oppure la vittima scelta da CLOCK
static int part_evict(BufferPool* bp, BpPart* pt) {
    if (pt->num_used < pt->capacity) {
        for (int k = 0; k < pt->capacity; k++) {
            int i = (pt->hand + k) % pt->capacity;
            if (!pt->frames[i].page) { pt->hand = (i + 1) % pt->capacity; return i; }
        }
    }
    // al massimo due giri: il primo azzera i bit di riferimento, il secondo trova la vittima
    for (int k = 0; k < 2 * pt->capacity; k++) {
        int i = pt->hand;
        pt->hand = (pt->hand + 1) % pt->capacity;
        Frame* f = &pt->frames[i];
        if (f->pin_count > 0 || f->loading) continue;  // in uso: non sostituibile
        if (f->ref) { f->ref = false; continue; }      // seconda possibilità
        if (!frame_release(bp, pt, i)) continue;       // non scrivibile: resta in memoria
        pt->stats.evictions++;
        return i;
    }
    return -1;   // tutti i frame sono pinnati
}

static void frame_install(BpPart* pt, int idx, Page* p, uint32_t pid, bool loading) {
    Frame* f = &pt->frames[idx];
    f->page        = p;
    f->pid         = pid;
    f->dirty       = false;
    f->pinned      = false;
    f->ref         = true;
    f->pin_count   = 1;
    f->loading     = loading;
    f->fresh       = false;
    f->last_access = ++pt->tick;
    hash_insert(pt, idx);
    pt->num_used++;
}

Page* bp_fetch(BufferPool* bp, uint32_t page_id) {
    BpPart* pt = part_of(bp, page_id);
    sv_rw_wr(&pt->latch);
    for (;;) {
        pt->stats.requests++;
        int idx = part_find(pt, page_id);
        if (idx >= 0) {
            Frame* f = &pt->frames[idx];
            if (f->loading) {                           // un altro thread la sta leggendo: si aspetta
                pt->stats.requests--;
                sv_rw_wrunlock(&pt->latch);
                yield_cpu();
                sv_rw_wr(&pt->latch);
                continue;
            }
            f->last_access = ++pt->tick;
            f->ref = true;
            f->pin_count++;
            pt->stats.hits++;
            Page* r = f->page;
            sv_rw_wrunlock(&pt->latch);
            return r;
        }
        // Non in cache: un frame (libero o sostituito), riservato con il flag loading
        idx = part_evict(bp, pt);
        if (idx < 0) { sv_rw_wrunlock(&pt->latch); return NULL; }  // cache piena e tutto pinnato
        Page* p = (Page*)sv_malloc(sizeof(Page));
        if (!p) { sv_rw_wrunlock(&pt->latch); return NULL; }
        frame_install(pt, idx, p, page_id, true);
        sv_rw_wrunlock(&pt->latch);
        // la lettura dal disco senza il latch: gli altri thread della partizione vanno avanti
        int64_t offset = (int64_t)page_id * SV_PAGE_SIZE;
        bool ok = sv_pread(bp->db_fd, p, SV_PAGE_SIZE, offset) == SV_PAGE_SIZE;
        if (ok && !page_validate(p, page_id)) {      // danneggiata: la lettura fallisce
            if (page_id) fprintf(stderr, "[SegVault] page %u of the data file is damaged\n", page_id);
            ok = false;
        }
        sv_rw_wr(&pt->latch);
        Frame* f = &pt->frames[idx];
        f->loading = false;
        if (!ok) {
            hash_remove(pt, idx);
            page_free(f->page);
            f->page = NULL;
            f->pin_count = 0;
            f->ref = false;
            pt->num_used--;
            sv_rw_wrunlock(&pt->latch);
            return NULL;
        }
        pt->stats.misses++;
        sv_rw_wrunlock(&pt->latch);
        return p;
    }
}

void bp_unpin(BufferPool* bp, uint32_t page_id, bool dirty) {
    BpPart* pt = part_of(bp, page_id);
    sv_rw_wr(&pt->latch);
    int idx = part_find(pt, page_id);
    if (idx >= 0) {
        if (pt->frames[idx].pin_count > 0) pt->frames[idx].pin_count--;
        if (dirty) pt->frames[idx].dirty = true;
    }
    sv_rw_wrunlock(&pt->latch);
}

static void remove_page_locked(BufferPool* bp, uint32_t page_id) {
    BpPart* pt = part_of(bp, page_id);
    int idx = part_find(pt, page_id);
    if (idx >= 0) frame_release(bp, pt, idx);
}

void bp_remove_page(BufferPool* bp, uint32_t page_id) {
    if (!bp) return;
    BpPart* pt = part_of(bp, page_id);
    sv_rw_wr(&pt->latch);
    remove_page_locked(bp, page_id);
    sv_rw_wrunlock(&pt->latch);
}

// una pagina liberata da VACUUM, se su disco è ancora libera (l'elenco può essere vecchio dopo un
// crash: il flag sul disco decide). 0 = nessuna. Sotto bp->alloc
static uint32_t take_free(BufferPool* bp, uint64_t* lsn) {
    while (bp->nfree > 0) {
        uint32_t id = bp->free_ids[--bp->nfree];
        Page* d = page_read(id, bp->db_fd);
        bool ok = d && page_validate(d, id) && (d->flags & PAGE_FLAG_FREE);
        if (ok && lsn) *lsn = d->lsn;
        page_free(d);
        if (ok) {
            bp_remove_page(bp, id);                      // via la copia vecchia dalla cache
            return id;
        }
    }
    return 0;
}

// installa la pagina nuova id (già scritta sul disco) nella sua partizione, pinnata
static int install_new(BufferPool* bp, uint32_t id, Page* p) {
    BpPart* pt = part_of(bp, id);
    sv_rw_wr(&pt->latch);
    int idx = part_evict(bp, pt);
    if (idx < 0) { sv_rw_wrunlock(&pt->latch); return SV_ERR; }
    pt->stats.new_pages++;
    frame_install(pt, idx, p, id, false);
    pt->frames[idx].fresh = true;
    sv_rw_wrunlock(&pt->latch);
    return SV_OK;
}

int bp_new_page(BufferPool* bp, uint32_t* out_page_id) {
    sv_rw_wr(&bp->alloc);
    // Prima una pagina liberata da VACUUM (dopo un checkpoint nessun record del WAL la nomina);
    // altrimenti in fondo al file.
    uint64_t reuse_lsn = 0;
    uint32_t reuse = take_free(bp, &reuse_lsn);
    if (reuse) {
        Page* p = page_create(reuse);
        if (!p) { sv_rw_wrunlock(&bp->alloc); return SV_ERR; }
        p->lsn = reuse_lsn;                              // i record della vita precedente della pagina restano indietro
        if (page_write(p, bp->db_fd) != SV_OK) {          // il flag FREE sparisce subito dal disco
            page_free(p);
            bp->free_ids[bp->nfree++] = reuse;
            bp->io_error = true;
            sv_rw_wrunlock(&bp->alloc);
            return SV_IO_ERROR;
        }
        int r = install_new(bp, reuse, p);
        if (r != SV_OK) { page_free(p); bp->free_ids[bp->nfree++] = reuse; sv_rw_wrunlock(&bp->alloc); return r; }
        if (out_page_id) *out_page_id = reuse;
        sv_rw_wrunlock(&bp->alloc);
        return SV_OK;
    }
    // la fine del file a ogni allocazione: anche altre parti del motore allungano il file (pagine di overflow)
    uint32_t new_id = 0;
    {
        int64_t end = sv_lseek(bp->db_fd, 0, SEEK_END);
        new_id = end > 0 ? (uint32_t)((end + SV_PAGE_SIZE - 1) / SV_PAGE_SIZE) : 0;
        if (new_id == 0) {
            // la pagina 0 è l'intestazione del file (0 indica anche "fine catena" nelle pagine di heap)
            if (page_header_write(bp->db_fd, 1) != SV_OK) { bp->io_error = true; sv_rw_wrunlock(&bp->alloc); return SV_IO_ERROR; }
            new_id = 1;
        }
    }
    if (bp->next_new_id > new_id) new_id = bp->next_new_id;
    if (new_id > SV_MAX_PAGE_ID) { sv_rw_wrunlock(&bp->alloc); return SV_FULL; }
    Page* p = page_create(new_id);
    if (!p) { sv_rw_wrunlock(&bp->alloc); return SV_ERR; }
    // Scrivi immediatamente sul disco per "registrare" la pagina
    if (page_write(p, bp->db_fd) != SV_OK) { page_free(p); bp->io_error = true; sv_rw_wrunlock(&bp->alloc); return SV_IO_ERROR; }
    bp->next_new_id = new_id + 1;
    int r = install_new(bp, new_id, p);
    if (r != SV_OK) { page_free(p); sv_rw_wrunlock(&bp->alloc); return r; }
    if (out_page_id) *out_page_id = new_id;
    sv_rw_wrunlock(&bp->alloc);
    return SV_OK;
}

int bp_flush_page(BufferPool* bp, uint32_t page_id) {
    BpPart* pt = part_of(bp, page_id);
    sv_rw_wr(&pt->latch);
    int idx = part_find(pt, page_id);
    int r = idx < 0 ? SV_NOT_FOUND : write_dirty(bp, pt, &pt->frames[idx]) ? SV_OK : SV_IO_ERROR;
    sv_rw_wrunlock(&pt->latch);
    return r;
}

static void lock_all(BufferPool* bp) { for (int i = 0; i < bp->nparts; i++) sv_rw_wr(&bp->parts[i].latch); }
static void unlock_all(BufferPool* bp) { for (int i = bp->nparts - 1; i >= 0; i--) sv_rw_wrunlock(&bp->parts[i].latch); }

// pagina sporca da scrivere: ordinate per page_id (scrittura sequenziale), senza contesto globale
typedef struct { uint32_t pid; int part, idx; } DirtyRef;
static int cmp_dirty(const void* a, const void* b) {
    uint32_t x = ((const DirtyRef*)a)->pid, y = ((const DirtyRef*)b)->pid;
    return x < y ? -1 : x > y;
}

static int flush_all_locked(BufferPool* bp) {
    DirtyRef* dirty = sv_malloc(sizeof(DirtyRef) * (size_t)(bp->capacity > 0 ? bp->capacity : 1));
    int n = 0;
    bool ok = true;
    for (int k = 0; k < bp->nparts; k++) {
        BpPart* pt = &bp->parts[k];
        for (int i = 0; i < pt->capacity; i++) {
            if (!pt->frames[i].page || !pt->frames[i].dirty || pt->frames[i].loading) continue;
            if (dirty) dirty[n++] = (DirtyRef){ pt->frames[i].pid, k, i };
            else if (!write_dirty(bp, pt, &pt->frames[i])) ok = false;   // senza memoria: in ordine di frame
        }
    }
    if (dirty) {
        qsort(dirty, (size_t)n, sizeof(DirtyRef), cmp_dirty);
        // prima le pagine di overflow (le catene), poi le altre: un solo fsync in mezzo (write_page_raw)
        int nov = 0;
        for (int k = 0; k < n; k++)
            if (bp->parts[dirty[k].part].frames[dirty[k].idx].page->flags & 0x08) { DirtyRef t = dirty[nov]; dirty[nov++] = dirty[k]; dirty[k] = t; }
        // doppia scrittura: tutte le immagini nel log e un solo fsync, poi le pagine
        Page** pages = sv_malloc(sizeof(Page*) * (size_t)(n ? n : 1));
        int np = 0;
        for (int k = 0; k < n && pages; k++) {
            Frame* fr = &bp->parts[dirty[k].part].frames[dirty[k].idx];
            if (!(fr->page->flags & 0x08) && !fr->fresh) pages[np++] = fr->page;
        }
        sv_rw_wr(&bp->wmx);
        bool bw = pages && before_write(bp, pages, np);
        for (int k = 0; k < n && bw; k++) {
            BpPart* pt = &bp->parts[dirty[k].part];
            if (!write_page_raw(bp, pt, &pt->frames[dirty[k].idx])) ok = false;
        }
        sv_rw_wrunlock(&bp->wmx);
        if (!bw) ok = false;
        free(pages);
        free(dirty);
    } else {
        sv_rw_wr(&bp->wmx);
        if (!before_write(bp, NULL, 0)) ok = false;
        sv_rw_wrunlock(&bp->wmx);
    }
    // un fsync fallito non si ritenta: il kernel può aver già scartato le pagine che non ha scritto
    if (bp->io_error) ok = false;
    else if (sv_fsync(bp->db_fd) != 0) {
        fprintf(stderr, "[SegVault] cannot sync the data file to disk\n");
        bp->io_error = true;
        ok = false;
    } else bp->ov_unsynced = false;
    // tutto su disco: da qui ogni pagina ha un contenuto che solo un'immagine protegge da una scrittura
    // interrotta (anche le righe scritte senza log, come la riscrittura di ALTER TABLE)
    if (ok)
        for (int k = 0; k < bp->nparts; k++)
            for (int i = 0; i < bp->parts[k].capacity; i++) bp->parts[k].frames[i].fresh = false;
    return ok ? SV_OK : SV_IO_ERROR;
}

int bp_flush_all(BufferPool* bp) {
    lock_all(bp);
    int r = flush_all_locked(bp);
    unlock_all(bp);
    return r;
}

void bp_remove_all(BufferPool* bp) {
    if (!bp) return;
    lock_all(bp);
    for (int k = 0; k < bp->nparts; k++)
        for (int i = 0; i < bp->parts[k].capacity; i++) frame_release(bp, &bp->parts[k], i);
    unlock_all(bp);
}

bool bp_resize(BufferPool* bp, int capacity) {
    if (capacity < 4) capacity = 4;
    sv_rw_wr(&bp->alloc);
    lock_all(bp);
    bool ok = true;
    for (int k = 0; k < bp->nparts && ok; k++)
        for (int i = 0; i < bp->parts[k].capacity && ok; i++)
            if (bp->parts[k].frames[i].page && (bp->parts[k].frames[i].pin_count > 0 || bp->parts[k].frames[i].loading)) ok = false;
    if (ok) ok = flush_all_locked(bp) == SV_OK;
    BufferPool tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (ok) ok = parts_build(&tmp, capacity);
    if (!ok) { unlock_all(bp); sv_rw_wrunlock(&bp->alloc); return false; }
    int old = bp->nparts;
    BufferStats keep = { 0 };
    for (int k = 0; k < old; k++) {                    // i contatori continuano (SHOW STATUS, /metrics)
        BufferStats* st = &bp->parts[k].stats;
        keep.requests += st->requests; keep.hits += st->hits; keep.misses += st->misses;
        keep.evictions += st->evictions; keep.dirty_writes += st->dirty_writes; keep.new_pages += st->new_pages;
        part_free_frames(&bp->parts[k]);
    }
    for (int k = old - 1; k >= 0; k--) sv_rw_wrunlock(&bp->parts[k].latch);
    for (int k = 0; k < tmp.nparts; k++) bp->parts[k] = tmp.parts[k];
    bp->parts[0].stats = keep;
    bp->nparts = tmp.nparts;
    bp->capacity = tmp.capacity;
    bp->num_used = 0;
    sv_rw_wrunlock(&bp->alloc);
    return true;
}

void bp_destroy(BufferPool* bp) {
    if (!bp) return;
    if (!bp->io_error) bp_flush_all(bp);         // dopo un errore di I/O: niente pagine senza log
    for (int k = 0; k < bp->nparts; k++) part_free_frames(&bp->parts[k]);
    free(bp->free_ids);
    free(bp);
}

void bp_stats(BufferPool* bp, BufferStats* out) {
    memset(out, 0, sizeof(*out));
    int used = 0;
    for (int k = 0; k < bp->nparts; k++) {
        BpPart* pt = &bp->parts[k];
        sv_rw_wr(&pt->latch);
        out->requests += pt->stats.requests; out->hits += pt->stats.hits; out->misses += pt->stats.misses;
        out->evictions += pt->stats.evictions; out->dirty_writes += pt->stats.dirty_writes; out->new_pages += pt->stats.new_pages;
        used += pt->num_used;
        sv_rw_wrunlock(&pt->latch);
    }
    bp->num_used = used;
    bp->stats = *out;
}

void bp_add_free(BufferPool* bp, uint32_t page_id) {
    sv_rw_wr(&bp->alloc);
    if (bp->nfree >= bp->free_cap) {
        int nc = bp->free_cap ? bp->free_cap * 2 : 256;
        uint32_t* n = sv_realloc(bp->free_ids, sizeof(uint32_t) * (size_t)nc);
        if (n) { bp->free_ids = n; bp->free_cap = nc; }
    }
    if (bp->nfree < bp->free_cap) bp->free_ids[bp->nfree++] = page_id;
    sv_rw_wrunlock(&bp->alloc);
}

int bp_free_count(BufferPool* bp) {
    sv_rw_wr(&bp->alloc);
    int n = bp->nfree;
    sv_rw_wrunlock(&bp->alloc);
    return n;
}

int bp_free_list(BufferPool* bp, uint32_t* out, int max) {
    sv_rw_wr(&bp->alloc);
    int n = bp->nfree < max ? bp->nfree : max;
    if (n > 0) memcpy(out, bp->free_ids, sizeof(uint32_t) * (size_t)n);
    sv_rw_wrunlock(&bp->alloc);
    return n;
}

void bp_note_lsn(BufferPool* bp, uint32_t page_id, int64_t lsn) {
    if (!bp || !page_id || lsn <= 0) return;
    BpPart* pt = part_of(bp, page_id);
    sv_rw_wr(&pt->latch);
    int idx = part_find(pt, page_id);
    if (idx >= 0 && pt->frames[idx].page && (int64_t)pt->frames[idx].page->lsn < lsn) pt->frames[idx].page->lsn = (uint64_t)lsn;
    sv_rw_wrunlock(&pt->latch);
}

void bp_checkpoint_done(BufferPool* bp) {
    if (!bp) return;
    for (int k = 0; k < bp->nparts; k++) {
        BpPart* pt = &bp->parts[k];
        sv_rw_wr(&pt->latch);
        for (int i = 0; i < pt->capacity; i++) pt->frames[i].fresh = false;
        sv_rw_wrunlock(&pt->latch);
    }
}

int bp_free_page_now(BufferPool* bp, uint32_t page_id, uint64_t lsn) {
    if (!bp || !page_id) return SV_ERR;
    bp_remove_page(bp, page_id);                         // la copia nella cache non serve più (anche se sporca)
    Page* p = page_create(page_id);
    if (!p) return SV_ERR;
    p->flags = PAGE_FLAG_FREE;
    p->lsn = lsn;                                        // il redo salta ogni record più vecchio per questa pagina
    int r = page_write(p, bp->db_fd);
    page_free(p);
    if (r != SV_OK) { bp->io_error = true; return r; }
    bp_add_free(bp, page_id);
    return SV_OK;
}

static int cmp_desc_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

void bp_free_lowest_first(BufferPool* bp) {
    sv_rw_wr(&bp->alloc);
    if (bp->nfree > 1) qsort(bp->free_ids, (size_t)bp->nfree, sizeof(uint32_t), cmp_desc_u32);   // si prende dal fondo
    sv_rw_wrunlock(&bp->alloc);
}

int bp_truncate_free_tail(BufferPool* bp) {
    sv_rw_wr(&bp->alloc);
    int64_t end = sv_lseek(bp->db_fd, 0, SEEK_END);
    uint32_t n = end > 0 ? (uint32_t)(end / SV_PAGE_SIZE) : 0;
    if (bp->next_new_id > n) n = bp->next_new_id;
    uint32_t keep = n;
    // pagine libere in fondo al file: via (dal fondo finché sono nella lista delle libere)
    for (;;) {
        if (keep <= 1) break;
        bool is_free = false;
        for (int i = 0; i < bp->nfree && !is_free; i++) if (bp->free_ids[i] == keep - 1) is_free = true;
        if (!is_free) break;
        keep--;
    }
    int cut = (int)(n - keep);
    if (cut > 0) {
        int k = 0;
        for (int i = 0; i < bp->nfree; i++) if (bp->free_ids[i] < keep) bp->free_ids[k++] = bp->free_ids[i];
        bp->nfree = k;
        for (uint32_t id = keep; id < n; id++) bp_remove_page(bp, id);
        if (sv_ftruncate(bp->db_fd, (int64_t)keep * SV_PAGE_SIZE) != 0) { bp->io_error = true; cut = -1; }
        bp->next_new_id = keep;
    }
    sv_rw_wrunlock(&bp->alloc);
    return cut;
}
