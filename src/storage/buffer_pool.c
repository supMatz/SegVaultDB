#include "buffer_pool.h"
#include <unistd.h>

BufferPool* bp_create(int capacity, int db_fd) {
    BufferPool* bp = SV_ALLOC(BufferPool);
    if (!bp) return NULL;
    bp->frames   = SV_ALLOC_N(Frame, capacity);
    bp->capacity = capacity;
    bp->num_used = 0;
    bp->db_fd    = db_fd;
    bp->tick     = 0;
    return bp;
}

// Cerca un page_id nella cache, ritorna l'indice del frame o -1
static int bp_find(BufferPool* bp, uint32_t page_id) {
    for (int i = 0; i < bp->capacity; i++) {
        if (bp->frames[i].page &&
            bp->frames[i].page->page_id == page_id)
            return i;
    }
    return -1;
}

// Trova il frame da rimpiazzare (LRU non pinned)
static int bp_evict(BufferPool* bp) {
    int    lru_idx  = -1;
    uint64_t lru_ts = UINT64_MAX;

    for (int i = 0; i < bp->capacity; i++) {
        Frame* f = &bp->frames[i];
        if (!f->page) return i;            // Frame vuoto: usa direttamente
        if (f->pinned || f->pin_count > 0) continue; // Non evictable
        if (f->last_access < lru_ts) {
            lru_ts  = f->last_access;
            lru_idx = i;
        }
    }
    if (lru_idx < 0) return -1; // Tutti i frame sono pinnati

    // Scrivi su disco se dirty prima di rimuovere
    Frame* victim = &bp->frames[lru_idx];
    if (victim->dirty) page_write(victim->page, bp->db_fd);
    page_free(victim->page);
    victim->page      = NULL;
    victim->dirty     = false;
    victim->pin_count = 0;
    return lru_idx;
}

Page* bp_fetch(BufferPool* bp, uint32_t page_id) {
    // Cerca in cache
    int idx = bp_find(bp, page_id);
    if (idx >= 0) {
        bp->frames[idx].last_access = ++bp->tick;
        bp->frames[idx].pin_count++;
        return bp->frames[idx].page;
    }

    // Non in cache: trova un frame libero
    idx = bp_evict(bp);
    if (idx < 0) return NULL; // Cache piena e tutto pinnato

    // Leggi dal disco
    Page* p = page_read(page_id, bp->db_fd);
    if (!p) return NULL;

    bp->frames[idx].page        = p;
    bp->frames[idx].dirty       = false;
    bp->frames[idx].pinned      = false;
    bp->frames[idx].pin_count   = 1;
    bp->frames[idx].last_access = ++bp->tick;
    return p;
}

void bp_unpin(BufferPool* bp, uint32_t page_id, bool dirty) {
    int idx = bp_find(bp, page_id);
    if (idx < 0) return;
    if (bp->frames[idx].pin_count > 0)
        bp->frames[idx].pin_count--;
    if (dirty) bp->frames[idx].dirty = true;
}

int bp_new_page(BufferPool* bp, uint32_t* out_page_id) {
    // Trova il prossimo page_id disponibile cercando il massimo esistente
    uint32_t max_id = 0;
    for (int i = 0; i < bp->capacity; i++) {
        if (bp->frames[i].page &&
            bp->frames[i].page->page_id > max_id)
            max_id = bp->frames[i].page->page_id;
    }
    uint32_t new_id = max_id + 1;

    int idx = bp_evict(bp);
    if (idx < 0) return SV_ERR;

    Page* p = page_create(new_id);
    if (!p) return SV_ERR;

    // Scrivi immediatamente sul disco per "registrare" la pagina
    page_write(p, bp->db_fd);

    bp->frames[idx].page        = p;
    bp->frames[idx].dirty       = false;
    bp->frames[idx].pin_count   = 1;
    bp->frames[idx].last_access = ++bp->tick;

    if (out_page_id) *out_page_id = new_id;
    return SV_OK;
}

int bp_flush_page(BufferPool* bp, uint32_t page_id) {
    int idx = bp_find(bp, page_id);
    if (idx < 0) return SV_NOT_FOUND;
    if (!bp->frames[idx].dirty) return SV_OK;
    int r = page_write(bp->frames[idx].page, bp->db_fd);
    if (r == SV_OK) bp->frames[idx].dirty = false;
    return r;
}

int bp_flush_all(BufferPool* bp) {
    for (int i = 0; i < bp->capacity; i++) {
        if (bp->frames[i].page && bp->frames[i].dirty)
            page_write(bp->frames[i].page, bp->db_fd);
    }
    fsync(bp->db_fd); // Garantisce che tutto sia su disco
    return SV_OK;
}

void bp_destroy(BufferPool* bp) {
    if (!bp) return;
    bp_flush_all(bp);
    for (int i = 0; i < bp->capacity; i++)
        if (bp->frames[i].page) page_free(bp->frames[i].page);
    free(bp->frames);
    free(bp);
}