#include "heap.h"

// limite di sicurezza contro catene corrotte (cicli)
#define HEAP_MAX_CHAIN SV_MAX_PAGE_ID

// ═════════════════════════════════════════════════════════════════
//  Free-space map (FSM)
//  Per ogni tabella si tiene in memoria l'elenco delle sue pagine con lo spazio libero
//  stimato: l'INSERT sceglie subito una pagina adatta invece di leggere tutta la catena
//  (che rendeva quadratico il caricamento di una tabella). I valori sono solo indicazioni:
//  la pagina viene sempre controllata, e se la stima era sbagliata viene corretta.
//  La mappa si costruisce alla prima apertura della tabella e si scarta con
//  heap_cache_clear() quando le pagine cambiano fuori da questo modulo (ROLLBACK).
// ═════════════════════════════════════════════════════════════════

#define FSM_MIN_USEFUL 48   // pagine con meno spazio non vengono più considerate per i buchi

struct HeapMeta {
    BufferPool* bp;
    uint32_t    root;
    uint32_t    last;
    uint32_t    npages;
    uint32_t*   pages;      // pagine in ordine di catena
    uint16_t*   freeb;      // spazio libero stimato di ogni pagina
    uint32_t    cap;
    uint32_t    hint;       // nessuna pagina prima di hint ha spazio utile
    uint32_t*   hkeys;      // tabella hash page_id -> indice (0 = vuoto: la pagina 0 non è mai di dati)
    uint32_t*   hvals;
    uint32_t    hcap;
    struct HeapMeta* next;
};

static HeapMeta* g_heaps;

static void meta_free(HeapMeta* m) {
    free(m->pages); free(m->freeb); free(m->hkeys); free(m->hvals); free(m);
}

void heap_cache_clear(void) {
    while (g_heaps) { HeapMeta* n = g_heaps->next; meta_free(g_heaps); g_heaps = n; }
}

void heap_cache_forget(BufferPool* bp, uint32_t root_page_id) {
    for (HeapMeta** pp = &g_heaps; *pp; pp = &(*pp)->next)
        if ((*pp)->bp == bp && (*pp)->root == root_page_id) { HeapMeta* m = *pp; *pp = m->next; meta_free(m); return; }
}

static bool meta_hash_put(HeapMeta* m, uint32_t pid, uint32_t idx) {
    if ((m->npages + 1) * 2 > m->hcap) {
        uint32_t nc = m->hcap ? m->hcap * 2 : 64;
        uint32_t* nk = calloc(nc, sizeof(uint32_t));
        uint32_t* nv = malloc(sizeof(uint32_t) * nc);
        if (!nk || !nv) { free(nk); free(nv); return false; }
        for (uint32_t i = 0; i < m->hcap; i++) {
            if (!m->hkeys[i]) continue;
            uint32_t j = (m->hkeys[i] * 2654435761u) & (nc - 1);
            while (nk[j]) j = (j + 1) & (nc - 1);
            nk[j] = m->hkeys[i]; nv[j] = m->hvals[i];
        }
        free(m->hkeys); free(m->hvals);
        m->hkeys = nk; m->hvals = nv; m->hcap = nc;
    }
    uint32_t j = (pid * 2654435761u) & (m->hcap - 1);
    while (m->hkeys[j] && m->hkeys[j] != pid) j = (j + 1) & (m->hcap - 1);
    m->hkeys[j] = pid; m->hvals[j] = idx;
    return true;
}

static int64_t meta_index(const HeapMeta* m, uint32_t pid) {
    if (!m->hcap || !pid) return -1;
    uint32_t j = (pid * 2654435761u) & (m->hcap - 1);
    while (m->hkeys[j]) {
        if (m->hkeys[j] == pid) return m->hvals[j];
        j = (j + 1) & (m->hcap - 1);
    }
    return -1;
}

static bool meta_append(HeapMeta* m, uint32_t pid, uint16_t freeb) {
    if (m->npages >= m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 16;
        uint32_t* np = realloc(m->pages, sizeof(uint32_t) * nc);
        if (!np) return false;
        m->pages = np;
        uint16_t* nf = realloc(m->freeb, sizeof(uint16_t) * nc);
        if (!nf) return false;
        m->freeb = nf;
        m->cap = nc;
    }
    if (!meta_hash_put(m, pid, m->npages)) return false;
    m->pages[m->npages] = pid;
    m->freeb[m->npages] = freeb;
    m->npages++;
    m->last = pid;
    return true;
}

// apre la catena (convertendo il formato contiguo precedente) e costruisce la mappa
static HeapMeta* meta_build(BufferPool* bp, uint32_t root_page_id, uint32_t num_pages) {
    Page* root = bp_fetch(bp, root_page_id);
    if (!root) return NULL;
    if (!(root->flags & PAGE_FLAG_CHAINED)) {
        // formato precedente: pagine contigue root..root+num_pages-1 -> convertite in catena
        bp_unpin(bp, root_page_id, false);
        uint32_t n = num_pages ? num_pages : 1;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t pid = root_page_id + k;
            Page* p = bp_fetch(bp, pid);
            if (!p) break;
            page_set_next(p, k + 1 < n ? pid + 1 : 0);
            p->flags |= PAGE_FLAG_CHAINED;
            bp_unpin(bp, pid, true);
        }
    } else {
        bp_unpin(bp, root_page_id, false);
    }

    HeapMeta* m = calloc(1, sizeof(HeapMeta));
    if (!m) return NULL;
    m->bp = bp;
    m->root = root_page_id;
    m->last = root_page_id;
    uint32_t pid = root_page_id, count = 0;
    while (pid && count < HEAP_MAX_CHAIN) {
        if (meta_index(m, pid) >= 0) break;          // ciclo nella catena: si ferma
        Page* p = bp_fetch(bp, pid);
        if (!p) break;
        uint32_t next = page_next(p);
        uint16_t fb = page_usable_space(p);
        bp_unpin(bp, pid, false);
        if (!meta_append(m, pid, fb)) { meta_free(m); return NULL; }
        count++;
        pid = next;
    }
    m->next = g_heaps;
    g_heaps = m;
    return m;
}

HeapFile* heap_open(BufferPool* bp, uint32_t root_page_id, uint32_t num_pages) {
    HeapFile* hf = SV_ALLOC(HeapFile);
    if (!hf) return NULL;
    hf->bp           = bp;
    hf->root_page_id = root_page_id;
    hf->num_pages    = num_pages;
    hf->last_page_id = root_page_id;
    if (root_page_id == 0) return hf;

    HeapMeta* m = NULL;
    for (HeapMeta* x = g_heaps; x; x = x->next)
        if (x->bp == bp && x->root == root_page_id) { m = x; break; }
    if (!m) m = meta_build(bp, root_page_id, num_pages);
    if (m) {
        hf->meta = m;
        hf->num_pages = m->npages;
        hf->last_page_id = m->last;
        return hf;
    }

    // senza memoria per la mappa: percorre la catena
    uint32_t pid = root_page_id, count = 0;
    while (pid && count < HEAP_MAX_CHAIN) {
        Page* p = bp_fetch(bp, pid);
        if (!p) break;
        uint32_t next = page_next(p);
        bp_unpin(bp, pid, false);
        hf->last_page_id = pid;
        count++;
        pid = next;
    }
    hf->num_pages = count;
    return hf;
}

// prova a inserire nella pagina pid; aggiorna la stima dello spazio (idx >= 0)
static int try_page(HeapFile* hf, uint32_t pid, int64_t idx, const uint8_t* data, uint16_t len, uint32_t* next_out) {
    Page* p = bp_fetch(hf->bp, pid);
    if (!p) return -2;
    if (next_out) *next_out = page_next(p);
    int slot = page_insert(p, data, len);
    if (hf->meta && idx >= 0)
        hf->meta->freeb[idx] = slot >= 0 ? p->free_space : page_usable_space(p);
    bp_unpin(hf->bp, pid, slot >= 0);
    return slot;
}

int heap_insert(HeapFile* hf, Tuple* t, TableSchema* schema, RID* out_rid) {
    if (!hf || !t || !schema) return SV_ERR;

    size_t tuple_len;
    uint8_t* data = tuple_serialize(t, schema, &tuple_len);
    if (!data) return SV_ERR;
    if (tuple_len > SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE - SV_SLOT_ENTRY_SIZE) { free(data); return SV_FULL; }
    uint16_t data_len = (uint16_t)tuple_len;
    uint32_t need = (uint32_t)data_len + SV_SLOT_ENTRY_SIZE;

    HeapMeta* m = hf->meta;
    if (m) {
        // 1. l'ultima pagina (caso tipico: tabella che cresce)
        // 2. una pagina con un buco abbastanza grande (dopo DELETE)
        int64_t li = (int64_t)m->npages - 1;
        if (li >= 0 && m->freeb[li] >= need) {
            int slot = try_page(hf, m->pages[li], li, data, data_len, NULL);
            if (slot >= 0) { if (out_rid) *out_rid = (RID){m->pages[li], (uint16_t)slot}; free(data); return SV_OK; }
        }
        while (m->hint + 1 < m->npages && m->freeb[m->hint] < FSM_MIN_USEFUL) m->hint++;
        for (uint32_t i = m->hint; i + 1 < m->npages; i++) {
            if (m->freeb[i] < need) continue;
            int slot = try_page(hf, m->pages[i], i, data, data_len, NULL);
            if (slot >= 0) { if (out_rid) *out_rid = (RID){m->pages[i], (uint16_t)slot}; free(data); return SV_OK; }
        }
        hf->last_page_id = m->last;
    } else {
        // prima pagina della catena con spazio sufficiente (first-fit)
        uint32_t pid = hf->root_page_id, count = 0;
        while (pid && count++ < HEAP_MAX_CHAIN) {
            uint32_t next = 0;
            int slot = try_page(hf, pid, -1, data, data_len, &next);
            if (slot == -2) break;
            if (slot >= 0) { if (out_rid) *out_rid = (RID){pid, (uint16_t)slot}; free(data); return SV_OK; }
            if (!next) hf->last_page_id = pid;
            pid = next;
        }
    }

    // nuova pagina in fondo al file, collegata all'ultima della catena
    if (hf->root_page_id) {
        // l'ultima pagina nota deve chiudere la catena (altrimenti la si segue fino alla fine)
        uint32_t guard = 0;
        for (;;) {
            Page* last = bp_fetch(hf->bp, hf->last_page_id);
            if (!last) break;
            uint32_t nx = page_next(last);
            bp_unpin(hf->bp, hf->last_page_id, false);
            if (!nx || ++guard > HEAP_MAX_CHAIN) break;
            hf->last_page_id = nx;
            if (m && meta_index(m, nx) < 0) meta_append(m, nx, 0);
        }
    }
    uint32_t new_pid;
    if (bp_new_page(hf->bp, &new_pid) != SV_OK) {
        free(data);
        return SV_FULL;
    }
    Page* p = bp_fetch(hf->bp, new_pid);
    if (!p) { bp_unpin(hf->bp, new_pid, false); free(data); return SV_ERR; }
    p->flags |= PAGE_FLAG_CHAINED;
    page_set_next(p, 0);

    // i collegamenti della catena vanno subito su disco (prima la pagina nuova, poi il
    // puntatore): il WAL ricostruisce le righe, non i collegamenti, e dopo un crash le
    // pagine non collegate sarebbero irraggiungibili
    bp_unpin(hf->bp, new_pid, true);
    bp_flush_page(hf->bp, new_pid);
    p = bp_fetch(hf->bp, new_pid);
    if (!p) { bp_unpin(hf->bp, new_pid, false); free(data); return SV_ERR; }
    if (hf->root_page_id == 0) {
        hf->root_page_id = new_pid;
    } else {
        Page* last = bp_fetch(hf->bp, hf->last_page_id);
        if (last) {
            page_set_next(last, new_pid);
            last->flags |= PAGE_FLAG_CHAINED;
            bp_unpin(hf->bp, hf->last_page_id, true);
            bp_flush_page(hf->bp, hf->last_page_id);
        }
    }
    hf->last_page_id = new_pid;
    hf->num_pages++;

    int slot = page_insert(p, data, data_len);
    if (m && !meta_append(m, new_pid, p->free_space)) {   // mappa incompleta: scartata
        heap_cache_forget(hf->bp, m->root);
        hf->meta = m = NULL;
    }
    bp_unpin(hf->bp, new_pid, true);
    bp_unpin(hf->bp, new_pid, false);   // pin di bp_new_page
    free(data);
    if (slot < 0) return SV_FULL;
    if (out_rid) *out_rid = (RID){new_pid, (uint16_t)slot};
    return SV_OK;
}

Tuple* heap_get(HeapFile* hf, RID rid, TableSchema* schema) {
    if (!hf || !schema) return NULL;

    Page* p = bp_fetch(hf->bp, rid.page_id);
    if (!p) return NULL;

    uint16_t len;
    const void* data = page_get_slot(p, rid.slot_id, &len);
    if (!data) { bp_unpin(hf->bp, rid.page_id, false); return NULL; }

    Tuple* t = tuple_deserialize((const uint8_t*)data, len, schema);
    if (t) t->rid = rid;

    bp_unpin(hf->bp, rid.page_id, false);
    return t;
}

int heap_delete(HeapFile* hf, RID rid) {
    if (!hf) return SV_ERR;
    Page* p = bp_fetch(hf->bp, rid.page_id);
    if (!p) return SV_NOT_FOUND;
    uint16_t len = 0;
    page_get_slot(p, rid.slot_id, &len);
    int rc = page_delete_slot(p, rid.slot_id);
    if (rc == SV_OK && hf->meta) {
        // lo spazio della riga torna utilizzabile (con la compattazione della pagina)
        int64_t idx = meta_index(hf->meta, rid.page_id);
        if (idx >= 0) {
            uint32_t f = (uint32_t)hf->meta->freeb[idx] + len;
            hf->meta->freeb[idx] = (uint16_t)(f > 0xFFFF ? 0xFFFF : f);
            if ((uint32_t)idx < hf->meta->hint) hf->meta->hint = (uint32_t)idx;
        }
    }
    bp_unpin(hf->bp, rid.page_id, (rc == SV_OK));
    return rc;
}

int heap_update(HeapFile* hf, RID rid, Tuple* t, TableSchema* schema, RID* out_new_rid) {
    if (!hf || !t || !schema) return SV_ERR;
    // delete + insert: la riga può cambiare posizione (il WAL registra il nuovo RID)
    int rc = heap_delete(hf, rid);
    if (rc != SV_OK) return rc;
    return heap_insert(hf, t, schema, out_new_rid);
}

HeapScan* heap_scan_open(HeapFile* hf, TableSchema* schema) {
    if (!hf || !schema) return NULL;
    HeapScan* scan = SV_ALLOC(HeapScan);
    if (!scan) return NULL;
    scan->hf           = hf;
    scan->schema       = schema;
    scan->current_page = hf->root_page_id;
    scan->current_slot = 0;
    scan->done         = false;
    return scan;
}

atomic_int g_sv_scan_cancel = 0;

static void scan_release(HeapScan* scan) {
    if (scan->page) { bp_unpin(scan->hf->bp, scan->page->page_id, false); scan->page = NULL; }
}

const uint8_t* heap_scan_next_raw(HeapScan* scan, RID* out_rid, uint16_t* out_len) {
    if (!scan || scan->done) return NULL;
    if (atomic_load(&g_sv_scan_cancel)) { scan_release(scan); scan->done = true; return NULL; }
    while (scan->current_page != 0) {
        // la pagina resta pinnata finché la scansione non passa alla successiva
        if (!scan->page || scan->page->page_id != scan->current_page) {
            scan_release(scan);
            scan->page = bp_fetch(scan->hf->bp, scan->current_page);
            if (!scan->page) break;
        }
        Page* p = scan->page;
        while (scan->current_slot < p->num_slots) {
            uint16_t len;
            const void* data = page_get_slot(p, scan->current_slot, &len);
            uint16_t slot = scan->current_slot++;
            if (data) {
                if (out_rid) *out_rid = (RID){scan->current_page, slot};
                if (out_len) *out_len = len;
                return (const uint8_t*)data;
            }
        }
        uint32_t next = page_next(p);   // pagina successiva della tabella (0 = fine)
        if (++scan->pages_seen >= HEAP_MAX_CHAIN) break;
        scan->current_page = next;
        scan->current_slot = 0;
    }
    scan_release(scan);
    scan->done = true;
    return NULL;
}

Tuple* heap_scan_next(HeapScan* scan, RID* out_rid) {
    RID rid;
    uint16_t len;
    const uint8_t* data;
    while ((data = heap_scan_next_raw(scan, &rid, &len)) != NULL) {
        Tuple* t = tuple_deserialize(data, len, scan->schema);
        if (!t) continue;                     // riga illeggibile: saltata come prima
        t->rid = rid;
        if (out_rid) *out_rid = rid;
        return t;
    }
    return NULL;
}

void heap_scan_close(HeapScan* scan) {
    if (!scan) return;
    scan_release(scan);
    free(scan);
}
