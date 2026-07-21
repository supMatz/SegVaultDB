#include "heap.h"

HeapFile* heap_open(BufferPool* bp, uint32_t root_page_id, uint32_t num_pages) {
    HeapFile* hf = SV_ALLOC(HeapFile);
    if (!hf) return NULL;
    hf->bp           = bp;
    hf->root_page_id = root_page_id;
    hf->num_pages    = num_pages;
    return hf;
}

int heap_insert(HeapFile* hf, Tuple* t, TableSchema* schema, RID* out_rid) {
    if (!hf || !t || !schema) return SV_ERR;

    size_t tuple_len;
    uint8_t* data = tuple_serialize(t, schema, &tuple_len);
    if (!data) return SV_ERR;

    uint16_t data_len = (uint16_t)tuple_len;

    // Find a page with enough space, or create a new one
    uint32_t start = hf->root_page_id;
    uint32_t end   = start + hf->num_pages;

    for (uint32_t pid = start; pid < end; pid++) {
        Page* p = bp_fetch(hf->bp, pid);
        if (!p) continue;

        int slot = page_insert(p, data, data_len);
        if (slot >= 0) {
            if (out_rid) *out_rid = (RID){pid, (uint16_t)slot};
            bp_unpin(hf->bp, pid, true);
            free(data);
            return SV_OK;
        }
        bp_unpin(hf->bp, pid, false);
    }

    // Create a new page
    uint32_t new_pid;
    if (bp_new_page(hf->bp, &new_pid) != SV_OK) {
        free(data);
        return SV_FULL;
    }
    hf->num_pages++;

    Page* p = bp_fetch(hf->bp, new_pid);
    if (!p) { free(data); return SV_ERR; }

    int slot = page_insert(p, data, data_len);
    if (slot >= 0) {
        if (out_rid) *out_rid = (RID){new_pid, (uint16_t)slot};
        bp_unpin(hf->bp, new_pid, true);
        free(data);
        return SV_OK;
    }

    bp_unpin(hf->bp, new_pid, false);
    free(data);
    return SV_FULL;
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

Tuple* heap_scan_next(HeapScan* scan, RID* out_rid) {
    if (!scan || scan->done) return NULL;
    uint32_t end = scan->hf->root_page_id + scan->hf->num_pages;

    while (scan->current_page < end) {
        Page* p = bp_fetch(scan->hf->bp, scan->current_page);
        if (!p) { scan->current_page++; scan->current_slot = 0; continue; }

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
        bp_unpin(scan->hf->bp, scan->current_page, false);
        scan->current_page++;
        scan->current_slot = 0;
    }

    scan->done = true;
    return NULL;
}

void heap_scan_close(HeapScan* scan) { free(scan); }
