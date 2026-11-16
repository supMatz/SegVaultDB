#include "wal.h"
#include "../table/overflow.h"
#include "../table/heap.h"
#include "../storage/page.h"
#include "../storage/crc32c.h"
#include "platform_compat.h"
#include <string.h>
#include <stdio.h>
#include "sv_alloc.h"

#define WAL_MAGIC   "SVWAL2\0\0"
#define WAL_VERSION 2u
#define V1_HDR      23
#define MAX_PAYLOAD (64u << 20)

static int64_t g_initial_lsn = 1;                 // 0 nelle pagine vuol dire "nessun record applicato"
void wal_set_initial_lsn(int64_t lsn) { g_initial_lsn = lsn > 0 ? lsn : 1; }
static int64_t g_recovered_end;
int64_t wal_recovery_end_lsn(void) { return g_recovered_end; }

static void put_u32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get_u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get_u64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void make_header(uint8_t h[WAL_HDR], int64_t base, int64_t contig) {
    memset(h, 0, WAL_HDR);
    memcpy(h, WAL_MAGIC, 8);
    put_u32(h + 8, WAL_VERSION);
    put_u32(h + 12, SV_PAGE_SIZE);
    put_u64(h + 16, (uint64_t)base);
    put_u64(h + 24, (uint64_t)contig);
    put_u32(h + 32, sv_crc32c(0, h, 32));
}

// intestazione v2: 1 = valida, 0 = non è un log v2 (formato v1), -1 = v2 danneggiata
static int parse_header(const uint8_t* h, size_t n, int64_t* base, int64_t* contig) {
    if (n < 8 || memcmp(h, WAL_MAGIC, 8)) return 0;
    if (n < WAL_HDR || get_u32(h + 32) != sv_crc32c(0, h, 32)) return -1;
    if (get_u32(h + 8) != WAL_VERSION || get_u32(h + 12) != SV_PAGE_SIZE) return -1;
    *base = (int64_t)get_u64(h + 16);
    *contig = (int64_t)get_u64(h + 24);
    if (*base < 0 || *contig < WAL_HDR) return -1;
    return 1;
}

WAL* wal_create(const char* path) {
    WAL* wal = sv_calloc(1, sizeof(WAL));
    if (!wal) return NULL;
    snprintf(wal->path, sizeof(wal->path), "%s", path);
    wal->fd = sv_open(path, SV_O_RDWR | SV_O_CREAT, 0600);
    if (wal->fd < 0) { free(wal); return NULL; }
    int64_t size = sv_lseek(wal->fd, 0, SEEK_END);
    if (size < 0) { sv_close(wal->fd); free(wal); return NULL; }
    wal->ver = 2;
    if (size == 0) {                                    // log nuovo: intestazione v2
        uint8_t h[WAL_HDR];
        make_header(h, g_initial_lsn, WAL_HDR);
        if (!sv_write_full(wal->fd, h, WAL_HDR)) { sv_close(wal->fd); free(wal); return NULL; }
        wal->base = g_initial_lsn;
        wal->contig = WAL_HDR;
        wal->fsize = WAL_HDR;
    } else {                                            // log esistente (recupero): stesso formato
        uint8_t h[WAL_HDR];
        ssize_t r = sv_pread(wal->fd, h, WAL_HDR, 0);
        int hv = parse_header(h, r > 0 ? (size_t)r : 0, &wal->base, &wal->contig);
        if (hv < 0) { sv_close(wal->fd); free(wal); return NULL; }
        if (hv == 0) { wal->ver = 1; wal->base = 0; wal->contig = 0; }
        wal->fsize = size;
        sv_lseek(wal->fd, 0, SEEK_END);
    }
    wal->last_lsn = -1;
    wal->bcap = 1u << 20;
    wal->buf = sv_try_malloc(wal->bcap);
    if (!wal->buf) wal->bcap = 0;               // senza buffer: ogni voce scritta subito
    return wal;
}

int64_t wal_size(WAL* wal) {
    if (!wal) return 0;
    if (wal->ver == 1) return wal->fsize + (int64_t)wal->blen;
    return wal->base + (wal->fsize - wal->contig) + (int64_t)wal->blen;
}

int64_t wal_bytes(WAL* wal) { return wal ? wal->fsize + (int64_t)wal->blen : 0; }

int wal_write(WAL* wal, uint8_t type, uint64_t tx_id, uint32_t page_id,
              SVRID rid, const uint8_t* data, uint32_t data_len,
              const uint8_t* old_data, uint32_t old_data_len) {
    if (!wal) return SV_ERR;
    if (wal->failed) return SV_IO_ERROR;
    // le lunghezze nell'intestazione devono corrispondere ai byte scritti davvero
    if (!data) data_len = 0;
    if (!old_data) old_data_len = 0;
    if (data_len > MAX_PAYLOAD || old_data_len > MAX_PAYLOAD) return SV_ERR;
    int64_t lsn = wal_size(wal);
    uint8_t hdr[WAL_REC_HDR];
    size_t hl;
    if (wal->ver == 1) {
        hdr[0] = type;
        memcpy(hdr + 1, &tx_id, 8);
        memcpy(hdr + 9, &page_id, 4);
        memcpy(hdr + 13, &rid.slot_id, 2);
        memcpy(hdr + 15, &data_len, 4);
        memcpy(hdr + 19, &old_data_len, 4);
        hl = V1_HDR;
    } else {
        put_u32(hdr, (uint32_t)(WAL_REC_HDR + data_len + old_data_len));
        put_u64(hdr + 8, (uint64_t)lsn);
        hdr[16] = type;
        memcpy(hdr + 17, &tx_id, 8);
        memcpy(hdr + 25, &page_id, 4);
        memcpy(hdr + 29, &rid.slot_id, 2);
        memcpy(hdr + 31, &data_len, 4);
        memcpy(hdr + 35, &old_data_len, 4);
        uint32_t crc = sv_crc32c(0, hdr + 8, WAL_REC_HDR - 8);
        if (data_len) crc = sv_crc32c(crc, data, data_len);
        if (old_data_len) crc = sv_crc32c(crc, old_data, old_data_len);
        put_u32(hdr + 4, crc);
        hl = WAL_REC_HDR;
    }
    wal->unsynced = true;
    size_t need = hl + (size_t)data_len + (size_t)old_data_len;
    if (wal->blen + need > wal->bcap && wal_write_buffer(wal) != SV_OK) return SV_IO_ERROR;
    if (need > wal->bcap) {                       // voce più grande del buffer: scritta subito
        if (!sv_write_full(wal->fd, hdr, hl) ||
            (data_len > 0 && !sv_write_full(wal->fd, data, data_len)) ||
            (old_data_len > 0 && !sv_write_full(wal->fd, old_data, old_data_len))) {
            fprintf(stderr, "[SegVault] cannot write the transaction log (disk full?)\n");
            wal->failed = true;
            return SV_IO_ERROR;
        }
        wal->fsize += (int64_t)need;
    } else {
        memcpy(wal->buf + wal->blen, hdr, hl);
        if (data_len) memcpy(wal->buf + wal->blen + hl, data, data_len);
        if (old_data_len) memcpy(wal->buf + wal->blen + hl + data_len, old_data, old_data_len);
        wal->blen += need;
    }
    wal->last_lsn = lsn;
    wal->sequence++;
    return SV_OK;
}

// Un errore di scrittura o di fsync del log è definitivo (wal->failed): dopo un fsync fallito il
// kernel può aver già scartato le pagine sporche, ritentare direbbe "riuscito" senza esserlo.
int wal_write_buffer(WAL* wal) {
    if (!wal) return SV_ERR;
    if (wal->failed) return SV_IO_ERROR;
    if (!wal->blen) return SV_OK;
    if (!sv_write_full(wal->fd, wal->buf, wal->blen)) {
        fprintf(stderr, "[SegVault] cannot write the transaction log (disk full?)\n");
        wal->failed = true;
        return SV_IO_ERROR;
    }
    wal->fsize += (int64_t)wal->blen;
    wal->blen = 0;
    return SV_OK;
}

int wal_flush(WAL* wal) {
    if (!wal) return SV_ERR;
    int r = wal_write_buffer(wal);
    if (r != SV_OK) return r;
    if (sv_fsync(wal->fd) != 0) {
        fprintf(stderr, "[SegVault] cannot sync the transaction log to disk\n");
        wal->failed = true;
        return SV_IO_ERROR;
    }
    wal->unsynced = false;
    wal->synced_lsn = wal_size(wal);
    return SV_OK;
}

int wal_log_page_image(WAL* wal, const void* page) {
    const Page* p = page;
    return wal_write(wal, LOG_PAGE_IMAGE, 0, p->page_id, (SVRID){ p->page_id, 0 }, (const uint8_t*)p, SV_PAGE_SIZE, NULL, 0);
}

// LOG_UPDATE_MOVE: posizione nuova all'inizio di data
static bool move_target(const uint8_t* data, uint32_t data_len, uint32_t* pid, uint16_t* slot) {
    if (!data || data_len < 6) return false;
    memcpy(pid, data, 4);
    memcpy(slot, data + 4, 2);
    return *pid != 0;
}

// ── lettura del log ──────────────────────────────────────────────

typedef struct {
    uint8_t  type;
    uint64_t tx;
    uint32_t page;
    uint16_t slot;
    uint32_t dlen, olen;
    uint8_t* data;
    uint8_t* old;
    int64_t  off;          // LSN della voce (v1: la sua posizione nel file)
    bool     dead;         // annullata da un ROLLBACK TO SAVEPOINT (non va annullata di nuovo)
    bool     comp;         // già annullata (c'è il suo CLR)
} Rec;

typedef struct { Rec* v; int n, cap; } RecVec;

static void recs_free(RecVec* rv) {
    for (int i = 0; i < rv->n; i++) { free(rv->v[i].data); free(rv->v[i].old); }
    free(rv->v);
    memset(rv, 0, sizeof(*rv));
}

static bool is_page_rec(uint8_t t) {
    return t == LOG_INSERT || t == LOG_DELETE || t == LOG_UPDATE || t == LOG_UPDATE_MOVE;
}

static void rec_clear(Rec* e) { free(e->data); free(e->old); e->data = e->old = NULL; }

// un log aperto in lettura (v1 o v2), con l'indice dei record portati da un checkpoint
typedef struct { int64_t lsn, pos; } Carry;
typedef struct {
    int      fd;
    int      ver;
    int64_t  base, contig, fsize, start;
    Carry*   carry;
    int      ncarry;
    int64_t  bad_pos;      // primo record danneggiato (-1 = nessuno)
    uint8_t* win;          // finestra di lettura: i record piccoli si leggono a blocchi di LF_WIN byte, non uno a uno
    int64_t  win_pos;
    size_t   win_len;
} LogFile;

#define LF_WIN (4u << 20)

static void lf_close(LogFile* lf) {
    if (lf->fd >= 0) sv_close(lf->fd);
    free(lf->carry);
    free(lf->win);
    memset(lf, 0, sizeof(*lf));
    lf->fd = -1;
}

// come sv_pread, attraverso la finestra (i byte oltre la fine del file non ci sono: lettura corta)
static ssize_t lf_pread(LogFile* lf, void* dst, size_t n, int64_t pos) {
    if (pos >= lf->win_pos && pos + (int64_t)n <= lf->win_pos + (int64_t)lf->win_len) {
        memcpy(dst, lf->win + (pos - lf->win_pos), n);
        return (ssize_t)n;
    }
    if (n > LF_WIN / 2) return sv_pread(lf->fd, dst, n, pos);
    if (!lf->win) { lf->win = sv_try_malloc(LF_WIN); if (!lf->win) return sv_pread(lf->fd, dst, n, pos); }
    ssize_t r = sv_pread(lf->fd, lf->win, LF_WIN, pos);
    if (r < 0) { lf->win_len = 0; return r; }
    lf->win_pos = pos;
    lf->win_len = (size_t)r;
    size_t k = (size_t)r < n ? (size_t)r : n;
    memcpy(dst, lf->win, k);
    return (ssize_t)k;
}

// il record alla posizione q del file è valido? (CRC e LSN atteso, senza tenerlo): per distinguere una
// coda interrotta (dopo il record guasto non c'è niente di valido) da un log danneggiato a metà
static bool valid_after(LogFile* lf, int64_t pos) {
    if (lf->ver != 2) return false;
    int64_t from = pos + 1 > lf->contig ? pos + 1 : lf->contig;
    int64_t n = lf->fsize - from;
    if (n < WAL_REC_HDR) return false;
    if (n > (512LL << 20)) return true;                   // coda enorme dopo un errore: meglio fermarsi
    uint8_t* b = sv_try_malloc((size_t)n);
    if (!b) return true;
    bool found = false;
    if (sv_pread(lf->fd, b, (size_t)n, from) == (ssize_t)n) {
        for (int64_t y = 0; y + WAL_REC_HDR <= n && !found; y++) {
            int64_t want = lf->base + (from + y - lf->contig);
            if ((int64_t)get_u64(b + y + 8) != want) continue;
            uint32_t len = get_u32(b + y);
            if (len < WAL_REC_HDR || (int64_t)len > n - y) continue;
            if (get_u32(b + y + 31) + get_u32(b + y + 35) + WAL_REC_HDR != len) continue;
            if (sv_crc32c(0, b + y + 8, len - 8) == get_u32(b + y + 4)) found = true;
        }
    }
    free(b);
    return found;
}

// record alla posizione pos: 1 letto (*next = posizione del seguente), 0 fine del log (vuoto, troncato o una
// scrittura interrotta in fondo), -1 errore di lettura / memoria, -2 log danneggiato (lf->bad_pos)
static int lf_read_pos(LogFile* lf, int64_t pos, Rec* e, bool payload, int64_t* next) {
    memset(e, 0, sizeof(*e));
    if (pos >= lf->fsize) return 0;
    if (lf->ver == 1) {
        uint8_t hdr[V1_HDR];
        ssize_t r = lf_pread(lf, hdr, V1_HDR, pos);
        if (r < 0) return -1;
        if (r < V1_HDR) return 0;
        e->type = hdr[0];
        memcpy(&e->tx, hdr + 1, 8);
        memcpy(&e->page, hdr + 9, 4);
        memcpy(&e->slot, hdr + 13, 2);
        memcpy(&e->dlen, hdr + 15, 4);
        memcpy(&e->olen, hdr + 19, 4);
        e->off = pos;
        if (e->dlen > MAX_PAYLOAD || e->olen > MAX_PAYLOAD) return 0;     // intestazione danneggiata: fine
        *next = pos + V1_HDR + (int64_t)e->dlen + (int64_t)e->olen;
        if (*next > lf->fsize) return 0;                                  // troncato in fondo
        if (!payload) return 1;
        if (e->dlen) {
            e->data = sv_try_malloc(e->dlen);
            if (!e->data) return -1;
            if (lf_pread(lf, e->data, e->dlen, pos + V1_HDR) != (ssize_t)e->dlen) { rec_clear(e); return 0; }
        }
        if (e->olen) {
            e->old = sv_try_malloc(e->olen);
            if (!e->old) { rec_clear(e); return -1; }
            if (lf_pread(lf, e->old, e->olen, pos + V1_HDR + e->dlen) != (ssize_t)e->olen) { rec_clear(e); return 0; }
        }
        return 1;
    }
    uint8_t hdr[WAL_REC_HDR];
    ssize_t r = lf_pread(lf, hdr, WAL_REC_HDR, pos);
    if (r < 0) return -1;
    bool bad = r < WAL_REC_HDR;
    uint32_t len = bad ? 0 : get_u32(hdr);
    if (!bad) {
        e->off  = (int64_t)get_u64(hdr + 8);
        e->type = hdr[16];
        memcpy(&e->tx, hdr + 17, 8);
        memcpy(&e->page, hdr + 25, 4);
        memcpy(&e->slot, hdr + 29, 2);
        memcpy(&e->dlen, hdr + 31, 4);
        memcpy(&e->olen, hdr + 35, 4);
        bad = e->dlen > MAX_PAYLOAD || e->olen > MAX_PAYLOAD || len != WAL_REC_HDR + e->dlen + e->olen ||
              pos + (int64_t)len > lf->fsize;
        if (!bad) bad = pos >= lf->contig ? e->off != lf->base + (pos - lf->contig) : e->off >= lf->base;
    }
    uint32_t crc = 0;
    if (!bad) {
        crc = sv_crc32c(0, hdr + 8, WAL_REC_HDR - 8);
        if (e->dlen) {
            e->data = sv_try_malloc(e->dlen);
            if (!e->data) return -1;
            if (lf_pread(lf, e->data, e->dlen, pos + WAL_REC_HDR) != (ssize_t)e->dlen) bad = true;
            else crc = sv_crc32c(crc, e->data, e->dlen);
        }
        if (!bad && e->olen) {
            e->old = sv_try_malloc(e->olen);
            if (!e->old) { rec_clear(e); return -1; }
            if (lf_pread(lf, e->old, e->olen, pos + WAL_REC_HDR + e->dlen) != (ssize_t)e->olen) bad = true;
            else crc = sv_crc32c(crc, e->old, e->olen);
        }
        if (!bad && crc != get_u32(hdr + 4)) bad = true;
    }
    if (bad) {
        rec_clear(e);
        // nei record portati (scritti e sincronizzati prima della sostituzione del file) o con record validi
        // dopo: danneggiato. Altrimenti è la coda di una scrittura interrotta: la fine del log
        if (pos < lf->contig || valid_after(lf, pos)) { lf->bad_pos = pos; return -2; }
        return 0;
    }
    if (!payload) rec_clear(e);
    *next = pos + (int64_t)len;
    return 1;
}

// SV_OK, SV_NOT_FOUND (nessun log), SV_IO_ERROR, SV_ERR (log danneggiato: why)
static int lf_open(const char* path, LogFile* lf, bool writable, char* why, size_t whyn) {
    memset(lf, 0, sizeof(*lf));
    lf->bad_pos = -1;
    lf->fd = sv_open(path, writable ? SV_O_RDWR : SV_O_RDONLY, 0);
    if (lf->fd < 0) { lf->fd = -1; return SV_NOT_FOUND; }
    lf->fsize = sv_lseek(lf->fd, 0, SEEK_END);
    if (lf->fsize < 0) { lf_close(lf); snprintf(why, whyn, "cannot read the transaction log"); return SV_IO_ERROR; }
    uint8_t h[WAL_HDR];
    ssize_t r = sv_pread(lf->fd, h, WAL_HDR, 0);
    if (r < 0) { lf_close(lf); snprintf(why, whyn, "cannot read the transaction log"); return SV_IO_ERROR; }
    int hv = parse_header(h, (size_t)r, &lf->base, &lf->contig);
    if (hv < 0) {
        // intestazione scritta a metà in un log nuovo (nessun record dopo): un log vuoto
        if (lf->fsize <= WAL_HDR) { lf->ver = 2; lf->base = 0; lf->contig = WAL_HDR; lf->start = WAL_HDR; lf->fsize = WAL_HDR; return SV_OK; }
        lf_close(lf);
        snprintf(why, whyn, "the transaction log header is damaged (checksum or format)");
        return SV_ERR;
    }
    if (hv == 0) { lf->ver = 1; lf->base = 0; lf->contig = 0; lf->start = 0; return SV_OK; }
    lf->ver = 2;
    lf->start = WAL_HDR;
    if (lf->contig > lf->fsize) { lf_close(lf); snprintf(why, whyn, "the transaction log is shorter than its header says"); return SV_ERR; }
    // indice dei record portati (fra l'intestazione e contig)
    int cap = 0;
    for (int64_t pos = WAL_HDR; pos < lf->contig; ) {
        Rec e;
        int64_t nx = 0;
        int rr = lf_read_pos(lf, pos, &e, false, &nx);
        if (rr != 1) {
            lf_close(lf);
            snprintf(why, whyn, rr == -1 ? "cannot read the transaction log" : "the transaction log is damaged at byte %lld", (long long)pos);
            return rr == -1 ? SV_IO_ERROR : SV_ERR;
        }
        if (lf->ncarry == cap) {
            int nc = cap ? cap * 2 : 64;
            Carry* nv = sv_try_realloc(lf->carry, sizeof(Carry) * (size_t)nc);
            if (!nv) { lf_close(lf); snprintf(why, whyn, "out of memory while reading the transaction log"); return SV_ERR; }
            lf->carry = nv;
            cap = nc;
        }
        lf->carry[lf->ncarry++] = (Carry){ e.off, pos };
        pos = nx;
    }
    return SV_OK;
}

// posizione nel file del record con quell'LSN (-1 = non c'è)
static int64_t lf_pos_of(const LogFile* lf, int64_t lsn) {
    if (lf->ver == 1) return lsn;
    if (lsn >= lf->base) return lf->contig + (lsn - lf->base);
    int a = 0, b = lf->ncarry - 1;
    while (a <= b) {
        int m = a + (b - a) / 2;
        if (lf->carry[m].lsn == lsn) return lf->carry[m].pos;
        if (lf->carry[m].lsn < lsn) a = m + 1; else b = m - 1;
    }
    return -1;
}

static int lf_read_lsn(LogFile* lf, int64_t lsn, Rec* e, bool payload) {
    int64_t pos = lf_pos_of(lf, lsn), nx;
    if (pos < 0) return 0;
    int r = lf_read_pos(lf, pos, e, payload, &nx);
    if (r == 1 && e->off != lsn) { rec_clear(e); return -2; }
    return r;
}

static int vec_push(RecVec* rv, const Rec* e) {
    if (rv->n == rv->cap) {
        int nc = rv->cap ? rv->cap * 2 : 256;
        Rec* nv = sv_try_realloc(rv->v, sizeof(Rec) * (size_t)nc);
        if (!nv) return SV_ERR;
        rv->v = nv;
        rv->cap = nc;
    }
    rv->v[rv->n++] = *e;
    return SV_OK;
}

// legge le voci dall'LSN from (only_tx != 0: solo quella transazione), in ordine di LSN. Una scrittura
// interrotta in fondo è la fine del log; un errore di lettura, di memoria o un log danneggiato è un errore
static int read_log(const char* path, int64_t from, uint64_t only_tx, RecVec* rv, char* err, size_t errn) {
    memset(rv, 0, sizeof(*rv));
    LogFile lf;
    int rc = lf_open(path, &lf, false, err, errn);
    if (rc != SV_OK) return rc;
    // 1. record portati (LSN < base), poi 2. quelli contigui da max(from, base)
    for (int i = 0; i < lf.ncarry && rc == SV_OK; i++) {
        if (lf.carry[i].lsn < from) continue;
        Rec e;
        int64_t nx;
        int r = lf_read_pos(&lf, lf.carry[i].pos, &e, true, &nx);
        if (r != 1) { rc = r == -1 ? SV_IO_ERROR : SV_ERR; snprintf(err, errn, "cannot read the transaction log"); break; }
        if (only_tx && e.tx != only_tx) { rec_clear(&e); continue; }
        if (vec_push(rv, &e) != SV_OK) { rec_clear(&e); rc = SV_ERR; snprintf(err, errn, "out of memory while reading the transaction log"); }
    }
    int64_t pos = lf.ver == 1 ? (from > 0 ? from : 0) : lf.contig + (from > lf.base ? from - lf.base : 0);
    while (rc == SV_OK) {
        Rec e;
        int64_t nx = 0;
        int r = lf_read_pos(&lf, pos, &e, true, &nx);
        if (r == 0) break;
        if (r < 0) {
            rc = r == -1 ? SV_IO_ERROR : SV_ERR;
            if (r == -1) snprintf(err, errn, "cannot read the transaction log");
            else snprintf(err, errn, "the transaction log is damaged at byte %lld", (long long)lf.bad_pos);
            break;
        }
        pos = nx;
        if (only_tx && e.tx != only_tx) { rec_clear(&e); continue; }
        if (vec_push(rv, &e) != SV_OK) { rec_clear(&e); rc = SV_ERR; snprintf(err, errn, "out of memory while reading the transaction log"); }
    }
    lf_close(&lf);
    if (rc != SV_OK) recs_free(rv);
    return rc;
}

static int rec_at(const RecVec* rv, int64_t off) {        // le voci sono in ordine di LSN
    int a = 0, b = rv->n - 1;
    while (a <= b) {
        int m = a + (b - a) / 2;
        if (rv->v[m].off == off) return m;
        if (rv->v[m].off < off) a = m + 1; else b = m - 1;
    }
    return -1;
}

// ROLLBACK TO (voci dopo il savepoint già annullate) e CLR (voce già annullata)
static void mark_done(RecVec* rv) {
    for (int i = 0; i < rv->n; i++) {
        Rec* e = &rv->v[i];
        if (e->type == LOG_ROLLBACK_TO && e->data && e->dlen >= 8) {
            int64_t sp;
            memcpy(&sp, e->data, 8);
            for (int j = 0; j < i; j++)
                if (rv->v[j].tx == e->tx && rv->v[j].off >= sp) rv->v[j].dead = true;
        } else if (e->type == LOG_CLR && e->data && e->dlen >= 9) {
            int64_t c;
            memcpy(&c, e->data, 8);
            int k = c >= 0 ? rec_at(rv, c) : -1;
            if (k >= 0) rv->v[k].comp = true;
        }
    }
}

// ── annullamento con CLR ─────────────────────────────────────────
// Le pagine toccate restano in memoria; ogni passo è un CLR nel log. Alla fine: fsync del log (i CLR
// prima delle pagine, regola del WAL), poi le pagine e fsync del file dei dati.

typedef struct {
    int      db_fd;
    WAL*     wal;
    uint64_t tx;
    Page**   pg;
    int      npg, cappg;
    int      rc;
    char     why[200];
    BufferPool* bp;        // != NULL: annullamento sulle pagine della cache (ROLLBACK a motore acceso), niente file
    uint32_t*   touched;   // (cache) pagine cambiate, per il chiamante
    int         ntouched, captouched;
} Undo;

static void undo_fail(Undo* u, int rc, const char* why, uint32_t page) {
    if (u->rc != SV_OK) return;
    u->rc = rc;
    snprintf(u->why, sizeof(u->why), "%s (page %u)", why, page);
}

static void undo_touch(Undo* u, uint32_t id) {
    for (int i = u->ntouched - 1; i >= 0 && i >= u->ntouched - 8; i--) if (u->touched[i] == id) return;
    if (u->ntouched == u->captouched) {
        int nc = u->captouched ? u->captouched * 2 : 64;
        uint32_t* nv = sv_try_realloc(u->touched, sizeof(uint32_t) * (size_t)nc);
        if (!nv) return;
        u->touched = nv;
        u->captouched = nc;
    }
    u->touched[u->ntouched++] = id;
}

static Page* undo_page(Undo* u, uint32_t id) {
    if (u->bp) {                                     // dalla cache, pinnata fino a undo_release
        Page* bp_p = bp_fetch(u->bp, id);
        if (!bp_p) undo_fail(u, SV_IO_ERROR, "cannot read a page to undo (damaged?)", id);
        return bp_p;
    }
    for (int i = 0; i < u->npg; i++) if (u->pg[i]->page_id == id) return u->pg[i];
    Page* p = page_read(id, u->db_fd);
    if (!p || !page_validate(p, id)) { page_free(p); undo_fail(u, SV_IO_ERROR, "cannot read a page to undo (damaged?)", id); return NULL; }
    if (u->npg == u->cappg) {
        int nc = u->cappg ? u->cappg * 2 : 64;
        Page** nv = sv_try_realloc(u->pg, sizeof(Page*) * (size_t)nc);
        if (!nv) { page_free(p); undo_fail(u, SV_ERR, "out of memory while undoing", id); return NULL; }
        u->pg = nv;
        u->cappg = nc;
    }
    u->pg[u->npg++] = p;
    return p;
}

static void undo_clr(Undo* u, uint32_t page, uint16_t slot, int64_t comp, uint8_t action, const uint8_t* img, uint32_t len) {
    if (u->rc != SV_OK) return;
    uint8_t* b = sv_try_malloc(9 + (size_t)len);
    if (!b) { undo_fail(u, SV_ERR, "out of memory while undoing", page); return; }
    memcpy(b, &comp, 8);
    b[8] = action;
    if (len) memcpy(b + 9, img, len);
    if (wal_write(u->wal, LOG_CLR, u->tx, page, (SVRID){ page, slot }, b, 9 + len, NULL, 0) != SV_OK)
        undo_fail(u, SV_IO_ERROR, "cannot write the transaction log", page);
    free(b);
}

// la pagina in memoria porta l'LSN del CLR che la cambia (log v2); con la cache: modificata e rilasciata
static void undo_mark(Undo* u, Page* p) {
    if (u->rc == SV_OK && u->wal->ver == 2 && u->wal->last_lsn > 0 && (int64_t)p->lsn < u->wal->last_lsn) p->lsn = (uint64_t)u->wal->last_lsn;
}

static void undo_release(Undo* u, Page* p) {
    if (!u->bp || !p) return;
    undo_touch(u, p->page_id);
    bp_unpin(u->bp, p->page_id, true);
}

// riga rimessa nel suo slot; una riga grande prende una catena nuova (dalla cache o sul file)
static int undo_slot_revive(Undo* u, Page* p, uint16_t slot, const uint8_t* img, uint32_t len) {
    if (!u->bp) return ov_slot_revive(u->db_fd, p, slot, img, len);
    if (len <= OV_INLINE_MAX && !ov_is_stub(img, len, NULL, NULL)) return page_revive_slot(p, slot, img, (uint16_t)len);
    uint8_t stub[OV_STUB_LEN];
    if (ov_store_bp(u->bp, img, len, stub) != SV_OK) return SV_IO_ERROR;
    return page_revive_slot(p, slot, stub, OV_STUB_LEN);
}

static void undo_delete(Undo* u, uint32_t pid, uint16_t slot, int64_t comp) {
    Page* p = undo_page(u, pid);
    if (!p) return;
    if (u->bp) {                                     // riga grande inserita dalla transazione: la sua catena si libera
        uint16_t cl = 0;
        const void* cur = page_get_slot(p, slot, &cl);
        uint32_t ovt = 0, ovf = 0;
        if (cur && ov_is_stub(cur, cl, &ovt, &ovf)) heap_chain_abandoned(ovf, ovt);
    }
    page_delete_slot(p, slot);                       // no-op se già eliminato
    // l'ultimo slot (aggiunto dalla transazione) esce dalla directory: altrimenti ogni riga inserita e poi
    // annullata lascia 4 byte occupati e rimettere le righe di prima può non entrare più nella pagina
    page_pop_slot(p, slot);
    undo_clr(u, pid, slot, comp, CLR_DELETE, NULL, 0);
    undo_mark(u, p);
    undo_release(u, p);
}

// rimette la riga img nel suo slot (eliminato dalla transazione): stesso RID, e lo spazio c'è sempre
// perché la riga era nella pagina. Prima nel log (CLR), poi nella pagina in memoria
static void undo_revive(Undo* u, uint32_t pid, uint16_t slot, const uint8_t* img, uint32_t len, int64_t comp) {
    Page* p = undo_page(u, pid);
    if (!p) return;
    int r = undo_slot_revive(u, p, slot, img, len);
    if (r == SV_DUPLICATE) {                         // slot già vivo: non dovrebbe succedere
        undo_fail(u, SV_ERR, "the row to restore is already present", pid);
        undo_release(u, p);
        return;
    }
    if (r != SV_OK) { undo_fail(u, r == SV_FULL ? SV_FULL : SV_IO_ERROR, "cannot put a row back", pid); undo_release(u, p); return; }
    undo_clr(u, pid, slot, comp, CLR_REVIVE, img, len);
    undo_mark(u, p);
    undo_release(u, p);
}

// UPDATE sul posto annullato: lo slot (vivo, con la riga nuova) riprende la riga vecchia, stesso RID
static void undo_replace(Undo* u, uint32_t pid, uint16_t slot, const uint8_t* img, uint32_t len, int64_t comp) {
    Page* p = undo_page(u, pid);
    if (!p) return;
    uint16_t cl = 0;
    const void* cur = page_get_slot(p, slot, &cl);
    uint32_t ovt = 0, ovf = 0;
    bool stub = cur && ov_is_stub(cur, cl, &ovt, &ovf);
    page_delete_slot(p, slot);
    int r = undo_slot_revive(u, p, slot, img, len);
    if (r != SV_OK) { undo_fail(u, r == SV_FULL ? SV_FULL : SV_IO_ERROR, "cannot put a row back", pid); undo_release(u, p); return; }
    if (stub && u->bp) heap_chain_abandoned(ovf, ovt);   // la catena della riga nuova non serve più
    undo_clr(u, pid, slot, comp, CLR_REPLACE, img, len);
    undo_mark(u, p);
    undo_release(u, p);
}

static void undo_rec(Undo* u, const Rec* e) {
    u->tx = e->tx;
    bool has_old = e->old && e->olen > 0;
    switch (e->type) {
        case LOG_INSERT:
            undo_delete(u, e->page, e->slot, e->off);
            break;
        case LOG_DELETE:
            if (has_old) undo_revive(u, e->page, e->slot, e->old, e->olen, e->off);
            break;
        case LOG_UPDATE:
            if (has_old) undo_replace(u, e->page, e->slot, e->old, e->olen, e->off);
            else undo_delete(u, e->page, e->slot, e->off);
            break;
        case LOG_UPDATE_MOVE: {
            uint32_t npid;
            uint16_t nslot;
            if (!move_target(e->data, e->dlen, &npid, &nslot)) break;
            if (npid != e->page || nslot != e->slot) undo_delete(u, npid, nslot, has_old ? -1 : e->off);
            else undo_delete(u, npid, nslot, -1);
            if (has_old) undo_revive(u, e->page, e->slot, e->old, e->olen, e->off);
            break;
        }
        default: break;
    }
}

static int undo_finish(Undo* u) {
    // doppia scrittura: le immagini delle pagine nel log (durevole) prima delle pagine nel file dei dati
    for (int i = 0; i < u->npg && u->rc == SV_OK && u->wal->ver == 2; i++)
        if (wal_log_page_image(u->wal, u->pg[i]) != SV_OK) undo_fail(u, SV_IO_ERROR, "cannot write the transaction log", u->pg[i]->page_id);
    if (u->rc == SV_OK && u->npg && wal_flush(u->wal) != SV_OK) undo_fail(u, SV_IO_ERROR, "cannot write the transaction log", 0);
    for (int i = 0; i < u->npg; i++) {
        if (u->rc == SV_OK && page_write(u->pg[i], u->db_fd) != SV_OK) undo_fail(u, SV_IO_ERROR, "cannot write a page", u->pg[i]->page_id);
        page_free(u->pg[i]);
    }
    free(u->pg);
    u->pg = NULL;
    u->npg = u->cappg = 0;
    if (u->rc == SV_OK && sv_fsync(u->db_fd) != 0) undo_fail(u, SV_IO_ERROR, "cannot sync the data file", 0);
    if (u->rc != SV_OK) fprintf(stderr, "[SegVault] undo failed: %s\n", u->why);
    return u->rc;
}

int wal_undo_tx(WAL* wal, const char* db_path, uint64_t tx_id) {
    return wal_undo_tx_from(wal, db_path, tx_id, 0, NULL, NULL);
}

int wal_undo_tx_from(WAL* wal, const char* db_path, uint64_t tx_id, int64_t from, uint32_t** pages, int* npages) {
    if (pages) { *pages = NULL; *npages = 0; }
    if (!wal || wal->failed) return SV_IO_ERROR;
    if (wal_write_buffer(wal) != SV_OK) return SV_IO_ERROR;
    char err[200];
    RecVec rv;
    int rc = read_log(wal->path, from, tx_id, &rv, err, sizeof(err));
    if (rc == SV_NOT_FOUND) return SV_OK;
    if (rc != SV_OK) { fprintf(stderr, "[SegVault] %s\n", err); return rc; }
    int db_fd = sv_open(db_path, SV_O_RDWR, 0);
    if (db_fd < 0) { recs_free(&rv); return SV_IO_ERROR; }
    mark_done(&rv);
    Undo u = { db_fd, wal, tx_id, NULL, 0, 0, SV_OK, "", NULL, NULL, 0, 0 };
    for (int i = rv.n - 1; i >= 0 && u.rc == SV_OK; i--) {
        const Rec* e = &rv.v[i];
        if (is_page_rec(e->type) && !e->dead && !e->comp) undo_rec(&u, e);
    }
    if (pages && u.npg) {                            // le pagine riscritte: il chiamante le toglie dalla cache
        *pages = sv_malloc(sizeof(uint32_t) * (size_t)u.npg);
        if (*pages) { for (int i = 0; i < u.npg; i++) (*pages)[i] = u.pg[i]->page_id; *npages = u.npg; }
    }
    rc = undo_finish(&u);
    recs_free(&rv);
    sv_close(db_fd);
    return rc;
}

int wal_undo_tx_cache(WAL* wal, BufferPool* bp, uint64_t tx_id, int64_t from, uint32_t** pages, int* npages) {
    if (pages) { *pages = NULL; *npages = 0; }
    if (!wal || wal->failed || !bp) return SV_IO_ERROR;
    if (wal_write_buffer(wal) != SV_OK) return SV_IO_ERROR;   // i record da leggere: nel file (senza fsync)
    char err[200];
    RecVec rv;
    int rc = read_log(wal->path, from, tx_id, &rv, err, sizeof(err));
    if (rc == SV_NOT_FOUND) return SV_OK;
    if (rc != SV_OK) { fprintf(stderr, "[SegVault] %s\n", err); return rc; }
    mark_done(&rv);
    Undo u;
    memset(&u, 0, sizeof(u));
    u.db_fd = -1;
    u.wal = wal;
    u.tx = tx_id;
    u.rc = SV_OK;
    u.bp = bp;
    for (int i = rv.n - 1; i >= 0 && u.rc == SV_OK; i--) {
        const Rec* e = &rv.v[i];
        if (is_page_rec(e->type) && !e->dead && !e->comp) undo_rec(&u, e);
    }
    recs_free(&rv);
    if (u.rc != SV_OK) fprintf(stderr, "[SegVault] undo failed: %s\n", u.why);
    if (pages) { *pages = u.touched; *npages = u.ntouched; } else free(u.touched);
    return u.rc;
}

// ── recupero ─────────────────────────────────────────────────────

// Redo con il log v2: ogni pagina porta l'LSN dell'ultimo record applicato, un record si rifà solo se è più
// recente (g_redo_lsn). Con i log v1 restano solo i controlli sul contenuto degli slot (sempre applicati anche
// con v2: una pagina con l'LSN giusto li supera comunque)
static bool g_redo_lsn;
// pagine ricostruite da vuote durante il redo perché illeggibili su disco (scrittura interrotta): il loro LSN non
// dice che il contenuto è completo, quindi la prossima immagine della pagina nel log si applica sempre
static uint8_t* g_rebuilt;
static uint32_t g_rebuilt_n;
static bool rebuilt_get(uint32_t id) { return g_rebuilt && id < g_rebuilt_n && (g_rebuilt[id / 8] >> (id % 8) & 1); }
static void rebuilt_set(uint32_t id, bool on) {
    if (!g_rebuilt || id >= g_rebuilt_n) return;
    if (on) g_rebuilt[id / 8] |= (uint8_t)(1u << (id % 8));
    else g_rebuilt[id / 8] &= (uint8_t)~(1u << (id % 8));
}

// pagina per rifare una voce; create: se manca (mai arrivata su disco) o è illeggibile la crea vuota (un'immagine
// più avanti nel log la rimette intera se era una scrittura interrotta)
static Page* redo_page(int db_fd, uint32_t id, bool create) {
    Page* p = page_read(id, db_fd);
    if (p && page_validate(p, id)) return p;
    page_free(p);
    if (!create) return NULL;
    p = page_create(id);
    if (p) p->flags |= PAGE_FLAG_CHAINED;
    rebuilt_set(id, true);
    return p;
}

static bool redo_done(const Page* p, const Rec* e) { return g_redo_lsn && p->lsn && (int64_t)p->lsn >= e->off; }

static int redo_write_lsn(Page* p, int db_fd, const Rec* e) {
    if (g_redo_lsn && (int64_t)p->lsn < e->off) p->lsn = (uint64_t)e->off;
    int r = page_write(p, db_fd);
    page_free(p);
    return r;
}

// immagine intera: se la pagina su disco è illeggibile (scrittura interrotta) o più vecchia, al suo posto
static int replay_image(int db_fd, const Rec* e) {
    if (!e->data || e->dlen != SV_PAGE_SIZE) return SV_OK;
    Page* img = sv_try_malloc(sizeof(Page));
    if (!img) return SV_ERR;
    memcpy(img, e->data, SV_PAGE_SIZE);
    if (img->page_id != e->page) { free(img); return SV_OK; }
    Page* cur = page_read(e->page, db_fd);
    bool use = !cur || !page_validate(cur, e->page) || cur->lsn < img->lsn || rebuilt_get(e->page);
    page_free(cur);
    int r = SV_OK;
    if (use) { r = page_write(img, db_fd); rebuilt_set(e->page, false); }
    free(img);
    return r;
}

static int replay_move(int db_fd, const Rec* e) {
    uint32_t npid;
    uint16_t nslot;
    if (!move_target(e->data, e->dlen, &npid, &nslot)) return SV_OK;
    // stessa pagina: le due metà sullo stesso oggetto, poi una scrittura (con l'LSN dopo entrambe)
    if (npid == e->page) {
        Page* p = redo_page(db_fd, npid, true);
        if (!p) return SV_ERR;
        if (redo_done(p, e)) { page_free(p); return SV_OK; }
        uint16_t len = 0;
        const void* cur = page_get_slot(p, e->slot, &len);
        if (cur && e->old && ov_slot_equals(db_fd, cur, len, e->old, e->olen)) page_delete_slot(p, e->slot);
        if (nslot == p->num_slots) ov_slot_put(db_fd, p, e->data + 6, e->dlen - 6);
        return redo_write_lsn(p, db_fd, e);
    }
    // 1. la riga vecchia sparisce (se c'è ancora)
    Page* p = redo_page(db_fd, e->page, false);
    if (p && redo_done(p, e)) { page_free(p); p = NULL; }
    if (p) {
        uint16_t len = 0;
        const void* cur = page_get_slot(p, e->slot, &len);
        if (cur && e->old && ov_slot_equals(db_fd, cur, len, e->old, e->olen)) page_delete_slot(p, e->slot);
        if (redo_write_lsn(p, db_fd, e) != SV_OK) return SV_IO_ERROR;
    }
    // 2. la riga nuova compare nella sua posizione (se non è già stata scritta)
    p = redo_page(db_fd, npid, true);
    if (!p) return SV_ERR;
    if (redo_done(p, e)) { page_free(p); return SV_OK; }
    if (nslot == p->num_slots) ov_slot_put(db_fd, p, e->data + 6, e->dlen - 6);
    return redo_write_lsn(p, db_fd, e);
}

static int replay_link(int db_fd, const Rec* e) {
    Page* p = redo_page(db_fd, e->page, true);
    if (!p) return SV_ERR;
    if (redo_done(p, e)) page_free(p);
    else if (redo_write_lsn(p, db_fd, e) != SV_OK) return SV_IO_ERROR;
    uint32_t prev = 0;
    if (e->data && e->dlen >= 4) memcpy(&prev, e->data, 4);
    if (!prev || prev == e->page) return SV_OK;
    Page* q = redo_page(db_fd, prev, false);
    if (!q) return SV_OK;
    if (!redo_done(q, e) && page_next(q) == 0) {     // il collegamento non era arrivato su disco
        page_set_next(q, e->page);
        q->flags |= PAGE_FLAG_CHAINED;
        return redo_write_lsn(q, db_fd, e);
    }
    page_free(q);
    return SV_OK;
}

// rifà una voce sul file dei dati (idempotente: con il log v2 per l'LSN della pagina; in più gli slot di una
// pagina sono solo aggiunti, mai riusati, quindi lo stato dello slot registrato dice se l'operazione è già su disco)
static int replay_entry(int db_fd, const Rec* e) {
    if (e->page == 0) return SV_OK;
    if (e->type == LOG_PAGE_IMAGE) return replay_image(db_fd, e);
    if (e->type == LOG_UPDATE_MOVE) return replay_move(db_fd, e);
    if (e->type == LOG_LINK) return replay_link(db_fd, e);
    uint8_t action = 0;
    const uint8_t* img = NULL;
    uint32_t ilen = 0;
    if (e->type == LOG_CLR) {
        if (!e->data || e->dlen < 9) return SV_OK;
        action = e->data[8];
        img = e->data + 9;
        ilen = e->dlen - 9;
    }
    bool adds = e->type == LOG_INSERT;
    Page* p = redo_page(db_fd, e->page, adds);
    if (!p) return adds ? SV_ERR : SV_OK;
    if (redo_done(p, e)) { page_free(p); return SV_OK; }
    switch (e->type) {
        case LOG_INSERT:
            if (e->slot == p->num_slots) ov_slot_put(db_fd, p, e->data, e->dlen);   // < num_slots: già applicato
            break;
        case LOG_DELETE:
            page_delete_slot(p, e->slot);                                     // no-op se già eliminato
            break;
        case LOG_UPDATE: {
            uint16_t len = 0;
            const void* cur = page_get_slot(p, e->slot, &len);
            if (g_redo_lsn) {                              // v2: sul posto, nello stesso slot (l'LSN dice che manca)
                if (cur) page_delete_slot(p, e->slot);
                if (e->slot < p->num_slots) ov_slot_revive(db_fd, p, e->slot, e->data, e->dlen);
            } else if (cur && e->old && ov_slot_equals(db_fd, cur, len, e->old, e->olen)) {
                // v1: ancora da applicare solo se lo slot contiene la versione precedente
                page_delete_slot(p, e->slot);
                ov_slot_put(db_fd, p, e->data, e->dlen);
            }
            break;
        }
        case LOG_CLR:
            if (action == CLR_DELETE) { page_delete_slot(p, e->slot); page_pop_slot(p, e->slot); }   // come undo_delete
            else if (action == CLR_REPLACE) {              // come undo_replace (l'LSN dice che manca)
                page_delete_slot(p, e->slot);
                if (e->slot < p->num_slots) ov_slot_revive(db_fd, p, e->slot, img, ilen);
            }
            else if (action == CLR_REVIVE) {               // solo se lo slot è eliminato (altrimenti già fatto)
                uint16_t len = 0;
                if (e->slot < p->num_slots && !page_get_slot(p, e->slot, &len)) ov_slot_revive(db_fd, p, e->slot, img, ilen);
            }
            break;
        default: break;
    }
    return redo_write_lsn(p, db_fd, e);
}

static void fsync_dir_of(const char* path) {
#ifndef _WIN32
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", path);
    char* sl = strrchr(dir, '/');
    if (sl) *sl = '\0'; else snprintf(dir, sizeof(dir), ".");
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
#else
    (void)path;
#endif
}

static int off_desc(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x > y ? -1 : x < y;
}

typedef struct { int64_t off; bool skip; } TxRec;       // skip: compensata da un CLR
typedef struct {
    uint64_t tx;
    bool     done;          // COMMIT o ABORT
    TxRec*   r;             // record di pagina della transazione (solo finché è aperta), in ordine di offset
    int      n, cap;
} TxInfo;

static TxInfo* tx_find(TxInfo* t, int n, uint64_t tx) {
    for (int i = n - 1; i >= 0; i--) if (t[i].tx == tx) return &t[i];
    return NULL;
}

// ── recupero in streaming ────────────────────────────────────────
// Memoria proporzionale alle transazioni aperte, non al log: la prima passata verifica ogni record (CRC) e
// tiene, per ogni transazione non ancora conclusa, gli LSN dei suoi record (lista liberata al COMMIT /
// ABORT); la seconda rifà i record uno alla volta; la terza rilegge su richiesta quelli da annullare.

int wal_recovery(const char* log_path, const char* db_path, const char* cat_path, char* err, size_t errn) {
    (void)cat_path;
    err[0] = '\0';
    LogFile lf;
    char why[200] = "";
    int orc = lf_open(log_path, &lf, true, why, sizeof(why));
    if (orc == SV_NOT_FOUND) return SV_OK;                       // nessun log: niente da recuperare
    if (orc != SV_OK) {
        snprintf(err, errn, "%s: it was left unchanged (restore a backup, or move it away to lose the changes it holds)", why);
        return orc;
    }
    int rc = SV_OK;
    TxInfo* txs = NULL;
    int ntx = 0, captx = 0;
    uint32_t maxp = 0;
    int64_t nrec = 0;
    g_redo_lsn = lf.ver == 2;
    g_recovered_end = 0;

    // ── 1. ogni record (CRC): stato delle transazioni, record delle aperte, pagina più alta ──
    int64_t pos = lf.start, log_end = lf.start;
    for (;;) {
        Rec e;
        int64_t next = 0;
        int r = lf_read_pos(&lf, pos, &e, true, &next);
        if (r == -1) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot read the transaction log"); break; }
        if (r == -2) {
            rc = SV_ERR;
            snprintf(err, errn, "the transaction log is damaged at byte %lld (a record in the middle fails its checksum): "
                     "it was left unchanged and nothing was applied", (long long)lf.bad_pos);
            break;
        }
        if (r == 0) break;
        nrec++;
        if (e.page > maxp) maxp = e.page;
        if (e.type == LOG_UPDATE_MOVE && e.dlen >= 6) { uint32_t np; memcpy(&np, e.data, 4); if (np > maxp) maxp = np; }
        TxInfo* t = e.tx ? tx_find(txs, ntx, e.tx) : NULL;
        if (e.tx && !t) {
            if (ntx == captx) {
                int nc = captx ? captx * 2 : 64;
                TxInfo* nt = sv_try_realloc(txs, sizeof(TxInfo) * (size_t)nc);
                if (!nt) { rec_clear(&e); rc = SV_ERR; snprintf(err, errn, "out of memory during recovery"); break; }
                txs = nt;
                captx = nc;
            }
            t = &txs[ntx++];
            memset(t, 0, sizeof(*t));
            t->tx = e.tx;
        }
        if (t && !t->done) {
            if (e.type == LOG_COMMIT || e.type == LOG_ABORT) {     // conclusa: i suoi record non servono più
                t->done = true;
                free(t->r);
                t->r = NULL;
                t->n = t->cap = 0;
            } else if (is_page_rec(e.type)) {
                if (t->n == t->cap) {
                    int nc = t->cap ? t->cap * 2 : 16;
                    TxRec* nr = sv_try_realloc(t->r, sizeof(TxRec) * (size_t)nc);
                    if (!nr) { rec_clear(&e); rc = SV_ERR; snprintf(err, errn, "out of memory during recovery"); break; }
                    t->r = nr;
                    t->cap = nc;
                }
                t->r[t->n++] = (TxRec){ e.off, false };
            } else if (e.type == LOG_ROLLBACK_TO && e.dlen >= 8) {  // record dopo il savepoint: già annullati
                int64_t sp;
                memcpy(&sp, e.data, 8);
                while (t->n > 0 && t->r[t->n - 1].off >= sp) t->n--;
            } else if (e.type == LOG_CLR && e.dlen >= 9) {         // record già annullato
                int64_t c;
                memcpy(&c, e.data, 8);
                if (c >= 0) {
                    int lo = 0, hi = t->n - 1;
                    while (lo <= hi) {
                        int m = lo + (hi - lo) / 2;
                        if (t->r[m].off == c) { t->r[m].skip = true; break; }
                        if (t->r[m].off < c) lo = m + 1; else hi = m - 1;
                    }
                }
            }
        }
        rec_clear(&e);
        pos = next;
        log_end = next;
    }
    if (rc == SV_OK && lf.ver == 2) g_recovered_end = lf.base + (log_end - lf.contig);   // anche con il log vuoto
    if (rc == SV_OK && nrec == 0) {                               // log vuoto
        free(txs);
        lf_close(&lf);
        remove(log_path);
        return SV_OK;
    }
    // una scrittura interrotta in fondo (mai confermata): via, così i CLR del recupero la seguono subito
    if (rc == SV_OK && log_end < lf.fsize) {
        fprintf(stderr, "[SegVault] the transaction log ends with %lld bytes of an incomplete or damaged record (an interrupted "
                        "write, never confirmed): dropped\n", (long long)(lf.fsize - log_end));
        if (sv_ftruncate(lf.fd, log_end) != 0 || sv_fsync(lf.fd) != 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot cut the interrupted end of the transaction log"); }
        else lf.fsize = log_end;
    }

    int db_fd = rc == SV_OK ? sv_open(db_path, SV_O_RDWR, 0) : -1;
    if (rc == SV_OK && db_fd < 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot open the data file"); }

    // le pagine nominate devono esistere nel file: le allocazioni a fine file (catene di overflow)
    // non devono prendere il posto di una pagina nuova che non era arrivata su disco
    if (rc == SV_OK) {
        int64_t need = ((int64_t)maxp + 1) * SV_PAGE_SIZE;
        int64_t size = sv_lseek(db_fd, 0, SEEK_END);
        if (size >= 0 && size < need && sv_ftruncate(db_fd, need) != 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot extend the data file"); }
    }

    g_rebuilt_n = maxp + 1;
    g_rebuilt = rc == SV_OK ? sv_try_calloc((size_t)g_rebuilt_n / 8 + 1, 1) : NULL;
    // ── 2. REDO: ogni record che tocca le pagine (anche i CLR), nell'ordine del log, uno alla volta ──
    for (int64_t p2 = lf.start; rc == SV_OK && p2 < log_end; ) {
        Rec e;
        int64_t nx = 0;
        int r = lf_read_pos(&lf, p2, &e, true, &nx);
        if (r <= 0) { rc = SV_ERR; snprintf(err, errn, "cannot read the transaction log (byte %lld)", (long long)p2); break; }
        if (is_page_rec(e.type) || e.type == LOG_CLR || e.type == LOG_LINK || e.type == LOG_PAGE_IMAGE) {
            int rr = replay_entry(db_fd, &e);
            if (rr != SV_OK) { rc = rr; snprintf(err, errn, "cannot replay the transaction log (page %u)", e.page); }
        }
        p2 = nx;
        rec_clear(&e);
    }

    free(g_rebuilt);
    g_rebuilt = NULL;
    g_rebuilt_n = 0;
    // ── 3. UNDO: le transazioni senza COMMIT né ABORT, dalla fine; ogni passo diventa un CLR nel log ──
    int64_t nlose = 0;
    for (int j = 0; j < ntx; j++) if (!txs[j].done) for (int k = 0; k < txs[j].n; k++) if (!txs[j].r[k].skip) nlose++;
    bool any_loser = false;
    for (int j = 0; j < ntx; j++) if (!txs[j].done) any_loser = true;
    if (rc == SV_OK && any_loser) {
        int64_t* offs = sv_try_malloc(sizeof(int64_t) * (size_t)(nlose ? nlose : 1));
        WAL* w = offs ? wal_create(log_path) : NULL;
        if (!offs) { rc = SV_ERR; snprintf(err, errn, "out of memory during recovery"); }
        else if (!w) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot write the transaction log"); }
        else {
            int64_t k = 0;
            for (int j = 0; j < ntx; j++) if (!txs[j].done) for (int q = 0; q < txs[j].n; q++) if (!txs[j].r[q].skip) offs[k++] = txs[j].r[q].off;
            qsort(offs, (size_t)k, sizeof(int64_t), off_desc);   // dal più recente al più vecchio
            Undo u = { db_fd, w, 0, NULL, 0, 0, SV_OK, "", NULL, NULL, 0, 0 };
            for (int64_t i = 0; i < k && u.rc == SV_OK; i++) {
                Rec e;
                int r = lf_read_lsn(&lf, offs[i], &e, true);
                if (r <= 0) { undo_fail(&u, SV_IO_ERROR, "cannot read a record to undo", 0); break; }
                undo_rec(&u, &e);
                rec_clear(&e);
            }
            rc = undo_finish(&u);
            for (int j = 0; j < ntx && rc == SV_OK; j++)          // annullamento completo e su disco
                if (!txs[j].done && wal_write(w, LOG_ABORT, txs[j].tx, 0, (SVRID){0, 0}, NULL, 0, NULL, 0) != SV_OK) rc = SV_IO_ERROR;
            if (rc == SV_OK && wal_flush(w) != SV_OK) rc = SV_IO_ERROR;
            if (rc == SV_OK && w->ver == 2 && wal_size(w) > g_recovered_end) g_recovered_end = wal_size(w);
            if (rc != SV_OK) snprintf(err, errn, "cannot undo the unfinished transactions: %s", u.why[0] ? u.why : "I/O error");
        }
        if (w) wal_destroy(w);
        free(offs);
    }
    for (int j = 0; j < ntx; j++) free(txs[j].r);
    free(txs);
    lf_close(&lf);

    // il file dei dati su disco prima di eliminare il log (altrimenti una seconda caduta perderebbe tutto)
    if (rc == SV_OK && sv_fsync(db_fd) != 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot sync the data file"); }
    if (db_fd >= 0) sv_close(db_fd);
    if (rc != SV_OK) return rc;
    if (remove(log_path) != 0) { snprintf(err, errn, "cannot remove the recovered transaction log"); return SV_IO_ERROR; }
    fsync_dir_of(log_path);
    return SV_OK;
}

int wal_truncate(WAL* wal) {
    if (!wal) return SV_ERR;
    if (wal->failed) return SV_IO_ERROR;
    int64_t next = wal_size(wal);                      // l'LSN continua
    wal->blen = 0;
    // prima via i record, poi l'intestazione nuova: una caduta in mezzo lascia un log vuoto
    if (sv_ftruncate(wal->fd, WAL_HDR) != 0) { wal->failed = true; return SV_IO_ERROR; }
    uint8_t h[WAL_HDR];
    make_header(h, next, WAL_HDR);
    if (sv_pwrite(wal->fd, h, WAL_HDR, 0) != WAL_HDR) { wal->failed = true; return SV_IO_ERROR; }
    if (sv_fsync(wal->fd) != 0) { wal->failed = true; return SV_IO_ERROR; }
    sv_lseek(wal->fd, 0, SEEK_END);
    wal->ver = 2;
    wal->base = next;
    wal->contig = WAL_HDR;
    wal->fsize = WAL_HDR;
    wal->unsynced = false;
    wal->checkpoints++;
    return SV_OK;
}

static bool tx_in(uint64_t tx, const uint64_t* v, int n) {
    for (int i = 0; i < n; i++) if (v[i] == tx) return true;
    return false;
}

int wal_checkpoint_keep(WAL* wal, const uint64_t* active, int n) {
    if (!wal) return SV_ERR;
    if (n <= 0) return wal_truncate(wal);
    if (wal->failed || wal->ver != 2) return SV_IO_ERROR;
    if (wal_write_buffer(wal) != SV_OK) return SV_IO_ERROR;
    int64_t next = wal_size(wal);
    char tmp[600], why[200];
    snprintf(tmp, sizeof(tmp), "%s.ckpt", wal->path);
    LogFile lf;
    if (lf_open(wal->path, &lf, false, why, sizeof(why)) != SV_OK) return SV_IO_ERROR;
    int out = sv_open(tmp, SV_O_RDWR | SV_O_CREAT | SV_O_TRUNC, 0600);
    if (out < 0) { lf_close(&lf); return SV_IO_ERROR; }
    uint8_t h[WAL_HDR];
    memset(h, 0, sizeof(h));
    bool ok = sv_write_full(out, h, WAL_HDR);          // l'intestazione vera quando si sa contig
    int64_t kept = 0;
    uint8_t* buf = NULL;
    size_t bcap = 0;
    // in ordine di LSN: i record portati, poi quelli contigui; si tengono solo quelli delle transazioni aperte
    for (int pass = 0; pass < 2 && ok; pass++) {
        int64_t pos = pass == 0 ? WAL_HDR : lf.contig, end = pass == 0 ? lf.contig : lf.fsize;
        while (ok && pos < end) {
            Rec e;
            int64_t nx = 0;
            int r = lf_read_pos(&lf, pos, &e, false, &nx);
            if (r != 1) { ok = r == 0 && pass == 1; break; }
            if (e.tx && tx_in(e.tx, active, n)) {
                size_t len = (size_t)(nx - pos);
                if (len > bcap) {
                    uint8_t* nb = sv_try_realloc(buf, len);
                    if (!nb) { ok = false; break; }
                    buf = nb;
                    bcap = len;
                }
                ok = lf_pread(&lf, buf, len, pos) == (ssize_t)len && sv_write_full(out, buf, len);
                kept += (int64_t)len;
            }
            pos = nx;
        }
    }
    free(buf);
    lf_close(&lf);
    if (ok) {
        make_header(h, next, WAL_HDR + kept);
        ok = sv_pwrite(out, h, WAL_HDR, 0) == WAL_HDR && sv_fsync(out) == 0;
    }
    sv_close(out);
    if (!ok) { remove(tmp); return SV_IO_ERROR; }
    // il file nuovo prende il posto del vecchio (chiuso prima: Windows non rinomina sopra un file aperto)
    sv_close(wal->fd);
    wal->fd = -1;
    int rr = sv_replace_file(tmp, wal->path);
    fsync_dir_of(wal->path);
    wal->fd = sv_open(wal->path, SV_O_RDWR, 0);
    if (wal->fd < 0) { wal->failed = true; return SV_IO_ERROR; }
    if (rr != 0) { remove(tmp); sv_lseek(wal->fd, 0, SEEK_END); return SV_IO_ERROR; }   // resta il log di prima
    wal->fsize = sv_lseek(wal->fd, 0, SEEK_END);
    wal->base = next;
    wal->contig = WAL_HDR + kept;
    wal->unsynced = false;
    wal->checkpoints++;
    return SV_OK;
}

void wal_destroy(WAL* wal) {
    if (!wal) return;
    wal_write_buffer(wal);
    free(wal->buf);
    if (wal->fd >= 0) sv_close(wal->fd);
    free(wal);
}
