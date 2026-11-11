#include "buffer_pool.h"
#include "platform_compat.h"

static unsigned bp_hash(const BufferPool* bp, uint32_t page_id) {
    return (unsigned)((page_id * 2654435761u) >> 7) & (unsigned)(bp->nbuckets - 1);   // Knuth
}

BufferPool* bp_create(int capacity, int db_fd) {
    if (capacity < 4) capacity = 4;
    BufferPool* bp = SV_ALLOC(BufferPool);
    if (!bp) return NULL;
    int nb = 1;
    while (nb < capacity * 2) nb <<= 1;
    bp->frames   = SV_ALLOC_N(Frame, capacity);
    bp->buckets  = malloc(sizeof(int) * (size_t)nb);
    if (!bp->frames || !bp->buckets) { free(bp->frames); free(bp->buckets); free(bp); return NULL; }
    for (int i = 0; i < nb; i++) bp->buckets[i] = -1;
    for (int i = 0; i < capacity; i++) bp->frames[i].hnext = -1;
    bp->nbuckets = nb;
    sv_rw_init(&bp->latch);
    bp->capacity = capacity;
    bp->db_fd    = db_fd;
    return bp;
}

// Cerca un page_id nella cache, ritorna l'indice del frame o -1
static int bp_find(BufferPool* bp, uint32_t page_id) {
    for (int i = bp->buckets[bp_hash(bp, page_id)]; i >= 0; i = bp->frames[i].hnext)
        if (bp->frames[i].page->page_id == page_id) return i;
    return -1;
}

static void hash_insert(BufferPool* bp, int idx) {
    unsigned h = bp_hash(bp, bp->frames[idx].page->page_id);
    bp->frames[idx].hnext = bp->buckets[h];
    bp->buckets[h] = idx;
}

static void hash_remove(BufferPool* bp, int idx) {
    unsigned h = bp_hash(bp, bp->frames[idx].page->page_id);
    int* pp = &bp->buckets[h];
    while (*pp >= 0 && *pp != idx) pp = &bp->frames[*pp].hnext;
    if (*pp == idx) *pp = bp->frames[idx].hnext;
    bp->frames[idx].hnext = -1;
}

// libera il frame (scrivendo la pagina se modificata)
static void before_write(BufferPool* bp) {
    if (bp->before_write) bp->before_write(bp->before_write_ctx);
}

static void frame_release(BufferPool* bp, int idx) {
    Frame* f = &bp->frames[idx];
    if (!f->page) return;
    if (f->dirty) before_write(bp);
    if (f->dirty && page_write(f->page, bp->db_fd) == SV_OK) bp->stats.dirty_writes++;
    hash_remove(bp, idx);
    page_free(f->page);
    f->page      = NULL;
    f->dirty     = false;
    f->ref       = false;
    f->pin_count = 0;
    bp->num_used--;
}

// Trova un frame da usare: uno libero, oppure la vittima scelta da CLOCK
static int bp_evict(BufferPool* bp) {
    if (bp->num_used < bp->capacity) {
        for (int k = 0; k < bp->capacity; k++) {
            int i = (bp->hand + k) % bp->capacity;
            if (!bp->frames[i].page) { bp->hand = (i + 1) % bp->capacity; return i; }
        }
    }
    // al massimo due giri: il primo azzera i bit di riferimento, il secondo trova la vittima
    for (int k = 0; k < 2 * bp->capacity; k++) {
        int i = bp->hand;
        bp->hand = (bp->hand + 1) % bp->capacity;
        Frame* f = &bp->frames[i];
        if (f->pin_count > 0) continue;            // in uso: non sostituibile
        if (f->ref) { f->ref = false; continue; }  // seconda possibilità
        frame_release(bp, i);
        bp->stats.evictions++;
        return i;
    }
    return -1;   // tutti i frame sono pinnati
}

static void frame_install(BufferPool* bp, int idx, Page* p) {
    Frame* f = &bp->frames[idx];
    f->page        = p;
    f->dirty       = false;
    f->pinned      = false;
    f->ref         = true;
    f->pin_count   = 1;
    f->last_access = ++bp->tick;
    hash_insert(bp, idx);
    bp->num_used++;
}

static Page* bp_fetch_nl(BufferPool* bp, uint32_t page_id) {
    bp->stats.requests++;
    int idx = bp_find(bp, page_id);
    if (idx >= 0) {
        Frame* f = &bp->frames[idx];
        f->last_access = ++bp->tick;
        f->ref = true;
        f->pin_count++;
        bp->stats.hits++;
        return f->page;
    }

    // Non in cache: trova un frame libero
    idx = bp_evict(bp);
    if (idx < 0) return NULL; // Cache piena e tutto pinnato

    Page* p = page_read(page_id, bp->db_fd);
    if (!p) return NULL;
    bp->stats.misses++;
    frame_install(bp, idx, p);
    return p;
}

static void bp_unpin_nl(BufferPool* bp, uint32_t page_id, bool dirty) {
    int idx = bp_find(bp, page_id);
    if (idx < 0) return;
    if (bp->frames[idx].pin_count > 0)
        bp->frames[idx].pin_count--;
    if (dirty) bp->frames[idx].dirty = true;
}

static int bp_new_page_nl(BufferPool* bp, uint32_t* out_page_id) {
    // La nuova pagina va sempre in fondo al file (le pagine non vengono riusate: il WAL
    // identifica le righe con page_id + slot).
    long end = sv_lseek(bp->db_fd, 0, SEEK_END);
    uint32_t new_id = end > 0 ? (uint32_t)((end + SV_PAGE_SIZE - 1) / SV_PAGE_SIZE) : 0;
    if (new_id == 0) {
        // la pagina 0 è riservata: 0 indica "fine catena" nelle pagine di heap
        Page* zero = page_create(0);
        if (!zero) return SV_ERR;
        page_write(zero, bp->db_fd);
        page_free(zero);
        new_id = 1;
    }
    if (bp->next_new_id > new_id) new_id = bp->next_new_id;
    if (new_id > SV_MAX_PAGE_ID) return SV_FULL;

    int idx = bp_evict(bp);
    if (idx < 0) return SV_ERR;

    Page* p = page_create(new_id);
    if (!p) return SV_ERR;

    // Scrivi immediatamente sul disco per "registrare" la pagina
    page_write(p, bp->db_fd);
    bp->next_new_id = new_id + 1;
    bp->stats.new_pages++;
    frame_install(bp, idx, p);

    if (out_page_id) *out_page_id = new_id;
    return SV_OK;
}

static int bp_flush_page_nl(BufferPool* bp, uint32_t page_id) {
    int idx = bp_find(bp, page_id);
    if (idx < 0) return SV_NOT_FOUND;
    if (!bp->frames[idx].dirty) return SV_OK;
    before_write(bp);
    int r = page_write(bp->frames[idx].page, bp->db_fd);
    if (r == SV_OK) { bp->frames[idx].dirty = false; bp->stats.dirty_writes++; }
    return r;
}

// ordine di page_id per il flush (il buffer pool è usato sotto il mutex del motore)
static const BufferPool* g_sort_bp;
static int cmp_frame_page(const void* a, const void* b) {
    uint32_t x = g_sort_bp->frames[*(const int*)a].page->page_id, y = g_sort_bp->frames[*(const int*)b].page->page_id;
    return x < y ? -1 : x > y;
}

static int bp_flush_all_nl(BufferPool* bp) {
    before_write(bp);
    int* dirty = malloc(sizeof(int) * (size_t)bp->capacity);
    int n = 0;
    for (int i = 0; i < bp->capacity; i++) {
        if (!bp->frames[i].page || !bp->frames[i].dirty) continue;
        if (dirty) dirty[n++] = i;
        else if (page_write(bp->frames[i].page, bp->db_fd) == SV_OK) {   // senza memoria: in ordine di frame
            bp->frames[i].dirty = false;
            bp->stats.dirty_writes++;
        }
    }
    if (dirty) {
        g_sort_bp = bp;
        qsort(dirty, (size_t)n, sizeof(int), cmp_frame_page);   // scrittura sequenziale sul file
        for (int k = 0; k < n; k++) {
            Frame* f = &bp->frames[dirty[k]];
            if (page_write(f->page, bp->db_fd) == SV_OK) { f->dirty = false; bp->stats.dirty_writes++; }
        }
        free(dirty);
    }
    sv_fsync(bp->db_fd);
    return SV_OK;
}

static void bp_remove_page_nl(BufferPool* bp, uint32_t page_id) {
    if (!bp) return;
    int idx = bp_find(bp, page_id);
    if (idx >= 0) frame_release(bp, idx);
}

static void bp_remove_all_nl(BufferPool* bp) {
    if (!bp) return;
    for (int i = 0; i < bp->capacity; i++) frame_release(bp, i);
}

void bp_destroy(BufferPool* bp) {
    if (!bp) return;
    bp_flush_all_nl(bp);
    for (int i = 0; i < bp->capacity; i++)
        if (bp->frames[i].page) page_free(bp->frames[i].page);
    free(bp->frames);
    free(bp->buckets);
    free(bp);
}

// ── latch: letture in parallelo (tabella hash, CLOCK, pin e statistiche condivisi) ──
Page* bp_fetch(BufferPool* bp, uint32_t page_id) {
    sv_rw_wr(&bp->latch);
    Page* r = bp_fetch_nl(bp, page_id);
    sv_rw_wrunlock(&bp->latch);
    return r;
}

void bp_unpin(BufferPool* bp, uint32_t page_id, bool dirty) {
    sv_rw_wr(&bp->latch);
    bp_unpin_nl(bp, page_id, dirty);
    sv_rw_wrunlock(&bp->latch);
}

int bp_new_page(BufferPool* bp, uint32_t* out_page_id) {
    sv_rw_wr(&bp->latch);
    int r = bp_new_page_nl(bp, out_page_id);
    sv_rw_wrunlock(&bp->latch);
    return r;
}

int bp_flush_page(BufferPool* bp, uint32_t page_id) {
    sv_rw_wr(&bp->latch);
    int r = bp_flush_page_nl(bp, page_id);
    sv_rw_wrunlock(&bp->latch);
    return r;
}

int bp_flush_all(BufferPool* bp) {
    sv_rw_wr(&bp->latch);
    int r = bp_flush_all_nl(bp);
    sv_rw_wrunlock(&bp->latch);
    return r;
}

void bp_remove_page(BufferPool* bp, uint32_t page_id) {
    sv_rw_wr(&bp->latch);
    bp_remove_page_nl(bp, page_id);
    sv_rw_wrunlock(&bp->latch);
}

void bp_remove_all(BufferPool* bp) {
    sv_rw_wr(&bp->latch);
    bp_remove_all_nl(bp);
    sv_rw_wrunlock(&bp->latch);
}

