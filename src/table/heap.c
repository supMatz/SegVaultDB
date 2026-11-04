#include "heap.h"

// limite di sicurezza contro catene corrotte (cicli)
#define HEAP_MAX_CHAIN SV_MAX_PAGE_ID

HeapFile* heap_open(BufferPool* bp, uint32_t root_page_id, uint32_t num_pages) {
    HeapFile* hf = SV_ALLOC(HeapFile);
    if (!hf) return NULL;
    hf->bp           = bp;
    hf->root_page_id = root_page_id;
    hf->num_pages    = num_pages;
    hf->last_page_id = root_page_id;
    if (root_page_id == 0) return hf;

    Page* root = bp_fetch(bp, root_page_id);
    if (!root) return hf;
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

    // percorre la catena: ultima pagina e numero di pagine effettivo
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

int heap_insert(HeapFile* hf, Tuple* t, TableSchema* schema, RID* out_rid) {
    if (!hf || !t || !schema) return SV_ERR;

    size_t tuple_len;
    uint8_t* data = tuple_serialize(t, schema, &tuple_len);
    if (!data) return SV_ERR;

    uint16_t data_len = (uint16_t)tuple_len;

    // prima pagina della catena con spazio sufficiente (first-fit)
    uint32_t pid = hf->root_page_id, count = 0;
    while (pid && count++ < HEAP_MAX_CHAIN) {
        Page* p = bp_fetch(hf->bp, pid);
        if (!p) break;
        uint32_t next = page_next(p);
        int slot = page_insert(p, data, data_len);
        if (slot >= 0) {
            if (out_rid) *out_rid = (RID){pid, (uint16_t)slot};
            bp_unpin(hf->bp, pid, true);
            free(data);
            return SV_OK;
        }
        bp_unpin(hf->bp, pid, false);
        if (!next) hf->last_page_id = pid;
        pid = next;
    }

    // nuova pagina in fondo al file, collegata all'ultima della catena
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
    int rc = page_delete_slot(p, rid.slot_id);
    bp_unpin(hf->bp, rid.page_id, (rc == SV_OK));
    return rc;
}

int heap_update(HeapFile* hf, RID rid, Tuple* t, TableSchema* schema) {
    if (!hf || !t || !schema) return SV_ERR;
    // For simplicity: delete + re-insert
    int rc = heap_delete(hf, rid);
    if (rc != SV_OK) return rc;
    return heap_insert(hf, t, schema, NULL);
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

Tuple* heap_scan_next(HeapScan* scan, RID* out_rid) {
    if (!scan || scan->done) return NULL;
    if (atomic_load(&g_sv_scan_cancel)) { scan->done = true; return NULL; }
    while (scan->current_page != 0) {
        Page* p = bp_fetch(scan->hf->bp, scan->current_page);
        if (!p) break;

        while (scan->current_slot < p->num_slots) {
            uint16_t len;
            const void* data = page_get_slot(p, scan->current_slot, &len);
            if (data) {
                RID rid = {scan->current_page, scan->current_slot};
                scan->current_slot++;
                Tuple* t = tuple_deserialize((const uint8_t*)data, len, scan->schema);
                if (t) {
                    t->rid = rid;
                    if (out_rid) *out_rid = rid;
                }
                bp_unpin(scan->hf->bp, rid.page_id, false);
                return t;
            }
            scan->current_slot++;
        }
        uint32_t next = page_next(p);   // pagina successiva della tabella (0 = fine)
        bp_unpin(scan->hf->bp, scan->current_page, false);
        if (++scan->pages_seen >= HEAP_MAX_CHAIN) break;
        scan->current_page = next;
        scan->current_slot = 0;
    }

    scan->done = true;
    return NULL;
}

void heap_scan_close(HeapScan* scan) { free(scan); }
