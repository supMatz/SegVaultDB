/*
SCOPO: conversione del file dei dati dal formato 1 al formato 2 (vedi page_migrate.h).
*/
#include "page_migrate.h"
#include "page.h"
#include "../table/overflow.h"
#include "common.h"
#include "platform_compat.h"
#include "sv_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t page_id;
    uint16_t free_space;
    uint16_t num_slots;
    uint16_t flags;
    uint16_t next_page_id;
    uint8_t  data[SV_PAGE_SIZE - 12];
} PageV1;
#pragma pack(pop)

#define V1_DATA  ((int)sizeof(((PageV1*)0)->data))
#define V1_CHUNK (V1_DATA - 2)
#define SHIFT    (SV_PAGE_HEADER_SIZE - 12)          // byte in meno per i dati di una pagina

static uint32_t v1_next(const PageV1* p) { return (uint32_t)p->next_page_id | ((uint32_t)(p->flags >> 8) << 16); }

static bool v1_read(int fd, uint32_t id, PageV1* p) {
    return sv_pread(fd, p, SV_PAGE_SIZE, (int64_t)id * SV_PAGE_SIZE) == SV_PAGE_SIZE && p->page_id == id;
}

static void v1_compact(PageV1* p) {
    uint8_t tmp[SV_PAGE_SIZE];
    int dir = p->num_slots * SV_SLOT_ENTRY_SIZE, wp = V1_DATA;
    for (int i = 0; i < p->num_slots; i++) {
        SlotEntry* e = (SlotEntry*)p->data + i;
        if (!e->length) continue;
        if (e->offset + e->length > V1_DATA || e->length > wp - dir) { e->length = 0; continue; }
        wp -= e->length;
        memcpy(tmp + wp, p->data + e->offset, e->length);
        e->offset = (uint16_t)wp;
    }
    memcpy(p->data + wp, tmp + wp, (size_t)(V1_DATA - wp));
    p->free_space = (uint16_t)(wp - dir);
}

// la riga di uno stub, dalla catena nel formato 1 (src = la copia del file)
static uint8_t* v1_load(int src, const uint8_t* stub, size_t* out_n) {
    uint32_t total, pid;
    if (!ov_is_stub(stub, OV_STUB_LEN, &total, &pid) || total > OV_MAX_ROW) return NULL;
    uint8_t* buf = sv_try_malloc(total ? total : 1);
    if (!buf) return NULL;
    size_t got = 0;
    uint32_t guard = total / (uint32_t)V1_CHUNK + 2;
    PageV1 p;
    while (got < total && pid && guard--) {
        if (!v1_read(src, pid, &p) || !(p.flags & PAGE_FLAG_OVERFLOW)) break;
        uint16_t len;
        memcpy(&len, p.data, 2);
        if (len > V1_CHUNK || got + len > total) break;
        memcpy(buf + got, p.data + 2, len);
        got += len;
        pid = v1_next(&p);
    }
    if (got != total) { free(buf); return NULL; }
    *out_n = total;
    return buf;
}

static bool copy_file(int from, int to, int64_t n) {
    uint8_t* b = sv_try_malloc(1u << 20);
    if (!b) return false;
    bool ok = sv_ftruncate(to, 0) == 0;
    for (int64_t off = 0; ok && off < n; ) {
        size_t k = n - off < (1 << 20) ? (size_t)(n - off) : (size_t)(1 << 20);
        ok = sv_pread(from, b, k, off) == (ssize_t)k && sv_pwrite(to, b, k, off) == (ssize_t)k;
        off += (int64_t)k;
    }
    free(b);
    return ok && sv_fsync(to) == 0;
}

static uint32_t file_pages(int fd) {
    int64_t end = sv_lseek(fd, 0, SEEK_END);
    return end > 0 ? (uint32_t)((end + SV_PAGE_SIZE - 1) / SV_PAGE_SIZE) : 0;
}

int page_migrate(int fd, const char* path, bool log_present, char* err, size_t errn) {
    char bak[1100];
    snprintf(bak, sizeof(bak), "%s.v1", path);
    if (page_header_read(fd, NULL)) { remove(bak); return SV_OK; }          // già del formato 2
    int64_t size = sv_lseek(fd, 0, SEEK_END);
    if (size <= 0) {                                                         // file nuovo
        if (page_header_write(fd, 1) != SV_OK || sv_fsync(fd) != 0) { snprintf(err, errn, "cannot write the data file header"); return SV_IO_ERROR; }
        return SV_OK;
    }
    // la copia del formato 1: se c'è già, una conversione precedente si è interrotta e si riparte da lì
    int src = sv_open(bak, SV_O_RDONLY, 0);
    if (src >= 0) {
        int64_t bs = sv_lseek(src, 0, SEEK_END);
        if (bs <= 0 || !copy_file(src, fd, bs)) { sv_close(src); snprintf(err, errn, "cannot restore %s to resume the format upgrade", bak); return SV_IO_ERROR; }
        size = bs;
    } else {
        int b = sv_open(bak, SV_O_RDWR | SV_O_CREAT | SV_O_TRUNC, 0600);
        if (b < 0 || !copy_file(fd, b, size)) {
            if (b >= 0) sv_close(b);
            remove(bak);
            snprintf(err, errn, "cannot copy the data file to %s before upgrading its format (disk full?)", bak);
            return SV_IO_ERROR;
        }
        sv_close(b);
        src = sv_open(bak, SV_O_RDONLY, 0);
        if (src < 0) { snprintf(err, errn, "cannot reopen %s", bak); return SV_IO_ERROR; }
    }
    uint32_t n = (uint32_t)((size + SV_PAGE_SIZE - 1) / SV_PAGE_SIZE);
    uint32_t* freed = NULL;
    int nfreed = 0, capf = 0, rc = SV_OK, moved = 0, rechained = 0;
    PageV1 o;
    uint8_t zero[SV_PAGE_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t id = 1; id < n && rc == SV_OK; id++) {
        if (!v1_read(src, id, &o)) {                                          // mai scritta o illeggibile: come prima
            if (sv_pwrite(fd, zero, SV_PAGE_SIZE, (int64_t)id * SV_PAGE_SIZE) != SV_PAGE_SIZE) rc = SV_IO_ERROR;
            continue;
        }
        Page* np = page_create(id);
        if (!np) { rc = SV_ERR; break; }
        if (o.flags & (PAGE_FLAG_OVERFLOW | PAGE_FLAG_FREE)) {               // catene vecchie: riscritte sotto
            np->flags = PAGE_FLAG_FREE;
            if (nfreed == capf) {
                int nc = capf ? capf * 2 : 256;
                uint32_t* nv = sv_try_realloc(freed, sizeof(uint32_t) * (size_t)nc);
                if (nv) { freed = nv; capf = nc; }
            }
            if (nfreed < capf) freed[nfreed++] = id;
            if (page_write(np, fd) != SV_OK) rc = SV_IO_ERROR;
            page_free(np);
            continue;
        }
        int ns = o.num_slots;
        if (ns > (V1_DATA / SV_SLOT_ENTRY_SIZE)) ns = 0;                   // danneggiata: vuota (come la leggeva la 1.3)
        o.num_slots = (uint16_t)ns;
        // catene di overflow con il passo nuovo: lo stub cambia al suo posto (stessa lunghezza)
        for (int i = 0; i < ns && rc == SV_OK; i++) {
            SlotEntry* e = (SlotEntry*)o.data + i;
            if (e->length != OV_STUB_LEN || e->offset + e->length > V1_DATA) continue;
            uint8_t* st = o.data + e->offset;
            if (!ov_is_stub(st, OV_STUB_LEN, NULL, NULL)) continue;
            size_t rn = 0;
            uint8_t* row = v1_load(src, st, &rn);
            if (!row) continue;                                               // catena già rotta: resta com'era
            uint8_t nst[OV_STUB_LEN];
            if (ov_store_fd(fd, row, rn, nst) != SV_OK) rc = SV_IO_ERROR;
            else { memcpy(st, nst, OV_STUB_LEN); rechained++; }
            free(row);
        }
        if (rc != SV_OK) { page_free(np); break; }
        // 12 byte in meno per i dati: compattando di solito bastano; altrimenti righe in una pagina nuova della catena
        if (o.free_space < SHIFT) v1_compact(&o);
        Page* extra = NULL;
        while (o.free_space < SHIFT) {
            if (log_present) {
                snprintf(err, errn, "page %u is full and a transaction log must be recovered first: start SegVault 1.3 once "
                                    "(it recovers the log), then this version", id);
                rc = SV_ERR;
                break;
            }
            int big = -1;
            for (int i = 0; i < ns; i++) {
                SlotEntry* e = (SlotEntry*)o.data + i;
                if (e->length && (big < 0 || e->length > ((SlotEntry*)o.data + big)->length)) big = i;
            }
            if (big < 0) break;
            SlotEntry* be = (SlotEntry*)o.data + big;
            if (!extra) {
                uint32_t xid = file_pages(fd);
                extra = page_create(xid);
                if (!extra) { rc = SV_ERR; break; }
                extra->flags = PAGE_FLAG_CHAINED;
                page_set_next(extra, v1_next(&o));
                uint32_t nx = xid;
                o.next_page_id = (uint16_t)(nx & 0xFFFF);
                o.flags = (uint16_t)((o.flags & 0x00FF) | (((nx >> 16) & 0xFF) << 8) | PAGE_FLAG_CHAINED);
                // la pagina nuova occupa subito il suo posto (le catene riscritte vanno dopo)
                if (page_write(extra, fd) != SV_OK) { rc = SV_IO_ERROR; break; }
            }
            if (page_insert(extra, o.data + be->offset, be->length) < 0) { rc = SV_ERR; snprintf(err, errn, "cannot move a row of page %u", id); break; }
            be->length = 0;
            moved++;
            v1_compact(&o);
        }
        if (rc != SV_OK) { page_free(np); page_free(extra); break; }
        // stessa disposizione, i dati 12 byte più avanti: la directory resta all'inizio, le righe nella stessa
        // posizione assoluta (offset - 12), il buco libero si accorcia di 12 byte
        int dir = ns * SV_SLOT_ENTRY_SIZE;
        np->free_space = (uint16_t)(o.free_space - SHIFT);
        np->num_slots = (uint16_t)ns;
        np->flags = o.flags;
        np->next_page_id = o.next_page_id;
        memcpy(np->data, o.data, (size_t)dir);
        memcpy(np->data + dir, o.data + dir + SHIFT, (size_t)(V1_DATA - dir - SHIFT));
        for (int i = 0; i < ns; i++) {
            SlotEntry* e = (SlotEntry*)np->data + i;
            if (e->length) e->offset = (uint16_t)(e->offset - SHIFT);
            else if (e->offset >= SHIFT) e->offset = (uint16_t)(e->offset - SHIFT);
        }
        if (page_write(np, fd) != SV_OK) rc = SV_IO_ERROR;
        if (extra && rc == SV_OK && page_write(extra, fd) != SV_OK) rc = SV_IO_ERROR;
        page_free(np);
        page_free(extra);
    }
    sv_close(src);
    if (rc == SV_OK && sv_fsync(fd) != 0) rc = SV_IO_ERROR;
    if (rc == SV_OK && (page_header_write(fd, 1) != SV_OK || sv_fsync(fd) != 0)) rc = SV_IO_ERROR;
    if (rc != SV_OK) {
        free(freed);
        if (!err[0]) snprintf(err, errn, "cannot upgrade the data file to format 2 (the original is kept in %s)", bak);
        return rc;
    }
    // le pagine delle catene vecchie: libere (riusabili)
    if (nfreed) {
        char fl[1100];
        snprintf(fl, sizeof(fl), "%s", path);
        char* sl = strrchr(fl, '/');
#ifdef _WIN32
        char* bs = strrchr(fl, '\\');
        if (!sl || (bs && bs > sl)) sl = bs;
#endif
        if (sl) snprintf(sl + 1, sizeof(fl) - (size_t)(sl + 1 - fl), "segvault.free");
        else snprintf(fl, sizeof(fl), "segvault.free");
        FILE* f = fopen(fl, "a");
        if (f) { for (int i = 0; i < nfreed; i++) fprintf(f, "%u\n", freed[i]); fclose(f); }
    }
    free(freed);
    remove(bak);
    fprintf(stderr, "[SegVault] data file upgraded to format 2 (page checksums): %u pages, %d large rows rewritten, %d rows moved\n",
            n, rechained, moved);
    return SV_OK;
}
