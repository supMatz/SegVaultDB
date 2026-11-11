#include "overflow.h"
#include "../storage/page.h"
#include "platform_compat.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t MAGIC[8] = { 0xFF, 0x00, 'S', 'V', 'O', 'V', 'F', '1' };
#define CHUNK ((int)sizeof(((Page*)0)->data) - 2)      // byte di dati in una pagina di overflow

static uint32_t fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void make_stub(uint8_t* s, uint32_t total, uint32_t first, uint32_t npages) {
    memcpy(s, MAGIC, 8);
    memcpy(s + 8, &total, 4);
    memcpy(s + 12, &first, 4);
    memcpy(s + 16, &npages, 4);
    uint32_t c = fnv(s, 20);
    memcpy(s + 20, &c, 4);
}

bool ov_is_stub(const uint8_t* d, size_t n, uint32_t* total, uint32_t* first) {
    if (!d || n != OV_STUB_LEN || memcmp(d, MAGIC, 8)) return false;
    uint32_t c;
    memcpy(&c, d + 20, 4);
    if (c != fnv(d, 20)) return false;
    if (total) memcpy(total, d + 8, 4);
    if (first) memcpy(first, d + 12, 4);
    return true;
}

static uint32_t chunks_for(size_t n) { return (uint32_t)((n + (size_t)CHUNK - 1) / (size_t)CHUNK); }

static void fill_page(Page* p, const uint8_t* data, size_t n, uint32_t next) {
    uint16_t len = (uint16_t)n;
    p->flags = PAGE_FLAG_OVERFLOW;
    page_set_next(p, next);
    p->num_slots = 0;
    p->free_space = 0;
    memcpy(p->data, &len, 2);
    memcpy(p->data + 2, data, n);
}

// ── buffer pool ─────────────────────────────────────────────────

int ov_store_bp(BufferPool* bp, const uint8_t* data, size_t n, uint8_t* stub) {
    if (n > OV_MAX_ROW) return SV_FULL;
    uint32_t k = chunks_for(n), first = 0;
    uint32_t* ids = malloc(sizeof(uint32_t) * (k ? k : 1));
    if (!ids) return SV_ERR;
    for (uint32_t i = 0; i < k; i++) {
        if (bp_new_page(bp, &ids[i]) != SV_OK) { free(ids); return SV_FULL; }
        bp_unpin(bp, ids[i], false);                     // pin di bp_new_page
    }
    // dall'ultima alla prima: ogni pagina conosce già la successiva; tutte subito su disco
    for (uint32_t i = k; i-- > 0;) {
        Page* p = bp_fetch(bp, ids[i]);
        if (!p) { free(ids); return SV_ERR; }
        size_t off = (size_t)i * (size_t)CHUNK, len = n - off < (size_t)CHUNK ? n - off : (size_t)CHUNK;
        fill_page(p, data + off, len, i + 1 < k ? ids[i + 1] : 0);
        bp_unpin(bp, ids[i], true);
        bp_flush_page(bp, ids[i]);
    }
    first = k ? ids[0] : 0;
    free(ids);
    make_stub(stub, (uint32_t)n, first, k);
    return SV_OK;
}

// lettura comune: get(page_id) -> pagina (pinnata o letta), put(...) la rilascia
typedef Page* (*GetFn)(void* ctx, uint32_t id);
typedef void  (*PutFn)(void* ctx, uint32_t id, Page* p);

static uint8_t* load(const uint8_t* stub, size_t stub_len, size_t* out_n, GetFn get, PutFn put, void* ctx) {
    uint32_t total, pid;
    if (!ov_is_stub(stub, stub_len, &total, &pid) || total > OV_MAX_ROW) return NULL;
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) return NULL;
    size_t got = 0;
    uint32_t guard = 0;
    while (got < total && pid && guard++ <= chunks_for(total)) {
        Page* p = get(ctx, pid);
        if (!p) break;
        uint16_t len;
        memcpy(&len, p->data, 2);
        bool ok = (p->flags & PAGE_FLAG_OVERFLOW) && len <= CHUNK && got + len <= total;
        if (ok) memcpy(buf + got, p->data + 2, len);
        uint32_t next = page_next(p);
        put(ctx, pid, p);
        if (!ok) break;
        got += len;
        pid = next;
    }
    if (got != total) { free(buf); return NULL; }
    *out_n = total;
    return buf;
}

static Page* get_bp(void* ctx, uint32_t id) { return bp_fetch((BufferPool*)ctx, id); }
static void  put_bp(void* ctx, uint32_t id, Page* p) { (void)p; bp_unpin((BufferPool*)ctx, id, false); }

uint8_t* ov_load_bp(BufferPool* bp, const uint8_t* stub, size_t stub_len, size_t* n) {
    return load(stub, stub_len, n, get_bp, put_bp, bp);
}

// ── direttamente sul file (WAL: recupero e annullamento) ────────

static Page* get_fd(void* ctx, uint32_t id) { return page_read(id, *(int*)ctx); }
static void  put_fd(void* ctx, uint32_t id, Page* p) { (void)ctx; (void)id; page_free(p); }

uint8_t* ov_load_fd(int db_fd, const uint8_t* stub, size_t stub_len, size_t* n) {
    return load(stub, stub_len, n, get_fd, put_fd, &db_fd);
}

int ov_store_fd(int db_fd, const uint8_t* data, size_t n, uint8_t* stub) {
    if (n > OV_MAX_ROW) return SV_FULL;
    long end = sv_lseek(db_fd, 0, SEEK_END);
    uint32_t base = end > 0 ? (uint32_t)((end + SV_PAGE_SIZE - 1) / SV_PAGE_SIZE) : 0;
    if (base == 0) {                                  // la pagina 0 è riservata
        Page* z = page_create(0);
        if (!z) return SV_ERR;
        page_write(z, db_fd);
        page_free(z);
        base = 1;
    }
    uint32_t k = chunks_for(n);
    for (uint32_t i = 0; i < k; i++) {
        Page* p = page_create(base + i);
        if (!p) return SV_ERR;
        size_t off = (size_t)i * (size_t)CHUNK, len = n - off < (size_t)CHUNK ? n - off : (size_t)CHUNK;
        fill_page(p, data + off, len, i + 1 < k ? base + i + 1 : 0);
        int rc = page_write(p, db_fd);
        page_free(p);
        if (rc != SV_OK) return SV_ERR;
    }
    make_stub(stub, (uint32_t)n, k ? base : 0, k);
    return SV_OK;
}

int ov_slot_put(int db_fd, void* page, const uint8_t* data, size_t n) {
    Page* p = page;
    if (n <= OV_INLINE_MAX) return page_insert(p, data, (uint16_t)n);
    uint8_t stub[OV_STUB_LEN];
    if (ov_store_fd(db_fd, data, n, stub) != SV_OK) return -1;
    return page_insert(p, stub, OV_STUB_LEN);
}

bool ov_slot_equals(int db_fd, const uint8_t* cur, size_t cur_len, const uint8_t* data, size_t n) {
    if (!cur || !data) return false;
    if (cur_len == n && !memcmp(cur, data, n)) return true;
    if (!ov_is_stub(cur, cur_len, NULL, NULL)) return false;
    size_t ln = 0;
    uint8_t* full = ov_load_fd(db_fd, cur, cur_len, &ln);
    bool eq = full && ln == n && !memcmp(full, data, n);
    free(full);
    return eq;
}

void ov_mark_bp(BufferPool* bp, const uint8_t* stub, size_t stub_len, uint8_t* bitmap, uint32_t nbits) {
    uint32_t total, pid;
    if (!ov_is_stub(stub, stub_len, &total, &pid)) return;
    uint32_t guard = chunks_for(total) + 1;
    while (pid && pid < nbits && guard--) {
        if (bitmap[pid / 8] & (1u << (pid % 8))) break;          // già vista: niente cicli
        bitmap[pid / 8] |= (uint8_t)(1u << (pid % 8));
        Page* p = bp_fetch(bp, pid);
        if (!p) break;
        uint32_t next = (p->flags & PAGE_FLAG_OVERFLOW) ? page_next(p) : 0;
        bp_unpin(bp, pid, false);
        pid = next;
    }
}
