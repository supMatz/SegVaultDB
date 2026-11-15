#include "wal.h"
#include "../table/overflow.h"
#include "../storage/page.h"
#include "platform_compat.h"
#include <string.h>
#include <stdio.h>

WAL* wal_create(const char* path) {
    WAL* wal = calloc(1, sizeof(WAL));
    if (!wal) return NULL;
    snprintf(wal->path, sizeof(wal->path), "%s", path);
    wal->fd = sv_open(path, SV_O_RDWR | SV_O_CREAT | SV_O_APPEND, 0600);
    if (wal->fd < 0) {
        free(wal);
        return NULL;
    }
    wal->bcap = 1u << 20;
    wal->buf = malloc(wal->bcap);
    if (!wal->buf) wal->bcap = 0;               // senza buffer: ogni voce scritta subito
    return wal;
}

int wal_write(WAL* wal, uint8_t type, uint64_t tx_id, uint32_t page_id,
              SVRID rid, const uint8_t* data, uint32_t data_len,
              const uint8_t* old_data, uint32_t old_data_len) {
    if (!wal) return SV_ERR;
    if (wal->failed) return SV_IO_ERROR;
    uint8_t hdr[23];
    hdr[0] = type;
    memcpy(hdr + 1, &tx_id, 8);
    memcpy(hdr + 9, &page_id, 4);
    memcpy(hdr + 13, &rid.slot_id, 2);
    memcpy(hdr + 15, &data_len, 4);
    memcpy(hdr + 19, &old_data_len, 4);

    // le lunghezze nell'intestazione devono corrispondere ai byte scritti davvero
    if (!data) data_len = 0;
    if (!old_data) old_data_len = 0;
    wal->unsynced = true;
    memcpy(hdr + 15, &data_len, 4);
    memcpy(hdr + 19, &old_data_len, 4);
    size_t need = 23 + (size_t)data_len + (size_t)old_data_len;
    if (wal->blen + need > wal->bcap && wal_write_buffer(wal) != SV_OK) return SV_IO_ERROR;
    if (need > wal->bcap) {                       // voce più grande del buffer: scritta subito
        if (!sv_write_full(wal->fd, hdr, 23) ||
            (data_len > 0 && !sv_write_full(wal->fd, data, data_len)) ||
            (old_data_len > 0 && !sv_write_full(wal->fd, old_data, old_data_len))) {
            fprintf(stderr, "[SegVault] cannot write the transaction log (disk full?)\n");
            wal->failed = true;
            return SV_IO_ERROR;
        }
    } else {
        wal->unsynced = true;
        memcpy(wal->buf + wal->blen, hdr, 23);
        if (data_len) memcpy(wal->buf + wal->blen + 23, data, data_len);
        if (old_data_len) memcpy(wal->buf + wal->blen + 23 + data_len, old_data, old_data_len);
        wal->blen += need;
    }
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
    return SV_OK;
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
    int64_t  off;          // posizione della voce nel log
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

// legge le voci dall'offset from (only_tx != 0: solo quella transazione). Una voce troncata in fondo è
// la fine del log (arresto durante la scrittura); un errore di lettura o di memoria è un errore
static int read_log(const char* path, int64_t from, uint64_t only_tx, RecVec* rv, char* err, size_t errn) {
    memset(rv, 0, sizeof(*rv));
    int fd = sv_open(path, SV_O_RDONLY, 0);
    if (fd < 0) return SV_NOT_FOUND;
    if (from > 0 && sv_lseek(fd, from, SEEK_SET) != from) {
        snprintf(err, errn, "cannot seek in the transaction log");
        sv_close(fd);
        return SV_IO_ERROR;
    }
    int64_t pos = from > 0 ? from : 0;
    int rc = SV_OK;
    for (;;) {
        uint8_t hdr[23];
        size_t got = 0;
        while (got < 23) {
            long r = (long)sv_read(fd, hdr + got, (unsigned)(23 - got));
            if (r < 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot read the transaction log"); break; }
            if (r == 0) break;
            got += (size_t)r;
        }
        if (rc != SV_OK || got < 23) break;                      // fine del log (o voce troncata)
        Rec e;
        memset(&e, 0, sizeof(e));
        e.type = hdr[0];
        memcpy(&e.tx, hdr + 1, 8);
        memcpy(&e.page, hdr + 9, 4);
        memcpy(&e.slot, hdr + 13, 2);
        memcpy(&e.dlen, hdr + 15, 4);
        memcpy(&e.olen, hdr + 19, 4);
        e.off = pos;
        if (e.dlen > (64u << 20) || e.olen > (64u << 20)) break;    // intestazione danneggiata: fine
        int64_t next = pos + 23 + (int64_t)e.dlen + (int64_t)e.olen;
        if (only_tx && e.tx != only_tx) {
            if (sv_lseek(fd, next, SEEK_SET) != next) break;
            pos = next;
            continue;
        }
        bool trunc = false;
        if (e.dlen) {
            e.data = malloc(e.dlen);
            if (!e.data) { rc = SV_ERR; snprintf(err, errn, "out of memory while reading the transaction log"); break; }
            if (!sv_read_full(fd, e.data, e.dlen)) trunc = true;
        }
        if (!trunc && e.olen) {
            e.old = malloc(e.olen);
            if (!e.old) { free(e.data); rc = SV_ERR; snprintf(err, errn, "out of memory while reading the transaction log"); break; }
            if (!sv_read_full(fd, e.old, e.olen)) trunc = true;
        }
        if (trunc) { free(e.data); free(e.old); break; }
        if (rv->n == rv->cap) {
            int nc = rv->cap ? rv->cap * 2 : 256;
            Rec* nv = realloc(rv->v, sizeof(Rec) * (size_t)nc);
            if (!nv) { free(e.data); free(e.old); rc = SV_ERR; snprintf(err, errn, "out of memory while reading the transaction log"); break; }
            rv->v = nv;
            rv->cap = nc;
        }
        rv->v[rv->n++] = e;
        pos = next;
    }
    sv_close(fd);
    if (rc != SV_OK) recs_free(rv);
    return rc;
}

static int rec_at(const RecVec* rv, int64_t off) {        // le voci sono in ordine di offset
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
} Undo;

static void undo_fail(Undo* u, int rc, const char* why, uint32_t page) {
    if (u->rc != SV_OK) return;
    u->rc = rc;
    snprintf(u->why, sizeof(u->why), "%s (page %u)", why, page);
}

static Page* undo_page(Undo* u, uint32_t id) {
    for (int i = 0; i < u->npg; i++) if (u->pg[i]->page_id == id) return u->pg[i];
    Page* p = page_read(id, u->db_fd);
    if (!p || p->page_id != id) { page_free(p); undo_fail(u, SV_IO_ERROR, "cannot read a page to undo", id); return NULL; }
    if (u->npg == u->cappg) {
        int nc = u->cappg ? u->cappg * 2 : 64;
        Page** nv = realloc(u->pg, sizeof(Page*) * (size_t)nc);
        if (!nv) { page_free(p); undo_fail(u, SV_ERR, "out of memory while undoing", id); return NULL; }
        u->pg = nv;
        u->cappg = nc;
    }
    u->pg[u->npg++] = p;
    return p;
}

static void undo_clr(Undo* u, uint32_t page, uint16_t slot, int64_t comp, uint8_t action, const uint8_t* img, uint32_t len) {
    if (u->rc != SV_OK) return;
    uint8_t* b = malloc(9 + (size_t)len);
    if (!b) { undo_fail(u, SV_ERR, "out of memory while undoing", page); return; }
    memcpy(b, &comp, 8);
    b[8] = action;
    if (len) memcpy(b + 9, img, len);
    if (wal_write(u->wal, LOG_CLR, u->tx, page, (SVRID){ page, slot }, b, 9 + len, NULL, 0) != SV_OK)
        undo_fail(u, SV_IO_ERROR, "cannot write the transaction log", page);
    free(b);
}

static void undo_delete(Undo* u, uint32_t pid, uint16_t slot, int64_t comp) {
    Page* p = undo_page(u, pid);
    if (!p) return;
    page_delete_slot(p, slot);                       // no-op se già eliminato
    // l'ultimo slot (aggiunto dalla transazione) esce dalla directory: altrimenti ogni riga inserita e poi
    // annullata lascia 4 byte occupati e rimettere le righe di prima può non entrare più nella pagina
    page_pop_slot(p, slot);
    undo_clr(u, pid, slot, comp, CLR_DELETE, NULL, 0);
}

// rimette la riga img nel suo slot (eliminato dalla transazione): stesso RID, e lo spazio c'è sempre
// perché la riga era nella pagina. Prima nel log (CLR), poi nella pagina in memoria
static void undo_revive(Undo* u, uint32_t pid, uint16_t slot, const uint8_t* img, uint32_t len, int64_t comp) {
    Page* p = undo_page(u, pid);
    if (!p) return;
    int r = ov_slot_revive(u->db_fd, p, slot, img, len);
    if (r == SV_DUPLICATE) {                         // slot già vivo: non dovrebbe succedere
        undo_fail(u, SV_ERR, "the row to restore is already present", pid);
        return;
    }
    if (r != SV_OK) { undo_fail(u, r == SV_FULL ? SV_FULL : SV_IO_ERROR, "cannot put a row back", pid); return; }
    undo_clr(u, pid, slot, comp, CLR_REVIVE, img, len);
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
            undo_delete(u, e->page, e->slot, has_old ? -1 : e->off);
            if (has_old) undo_revive(u, e->page, e->slot, e->old, e->olen, e->off);
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
    char err[200];
    RecVec rv;
    int rc = read_log(wal->path, from, tx_id, &rv, err, sizeof(err));
    if (rc == SV_NOT_FOUND) return SV_OK;
    if (rc != SV_OK) { fprintf(stderr, "[SegVault] %s\n", err); return rc; }
    int db_fd = sv_open(db_path, SV_O_RDWR, 0);
    if (db_fd < 0) { recs_free(&rv); return SV_IO_ERROR; }
    mark_done(&rv);
    Undo u = { db_fd, wal, tx_id, NULL, 0, 0, SV_OK, "" };
    for (int i = rv.n - 1; i >= 0 && u.rc == SV_OK; i--) {
        const Rec* e = &rv.v[i];
        if (is_page_rec(e->type) && !e->dead && !e->comp) undo_rec(&u, e);
    }
    if (pages && u.npg) {                            // le pagine riscritte: il chiamante le toglie dalla cache
        *pages = malloc(sizeof(uint32_t) * (size_t)u.npg);
        if (*pages) { for (int i = 0; i < u.npg; i++) (*pages)[i] = u.pg[i]->page_id; *npages = u.npg; }
    }
    rc = undo_finish(&u);
    recs_free(&rv);
    sv_close(db_fd);
    return rc;
}

// ── recupero ─────────────────────────────────────────────────────

// pagina per rifare una voce; create: se manca (mai arrivata su disco) la crea vuota
static Page* redo_page(int db_fd, uint32_t id, bool create) {
    Page* p = page_read(id, db_fd);
    if (p && p->page_id == id) return p;
    page_free(p);
    if (!create) return NULL;
    p = page_create(id);
    if (p) p->flags |= PAGE_FLAG_CHAINED;
    return p;
}

static int redo_write(Page* p, int db_fd) {
    int r = page_write(p, db_fd);
    page_free(p);
    return r;
}

static int replay_move(int db_fd, const Rec* e) {
    uint32_t npid;
    uint16_t nslot;
    if (!move_target(e->data, e->dlen, &npid, &nslot)) return SV_OK;
    // 1. la riga vecchia sparisce (se c'è ancora)
    Page* p = redo_page(db_fd, e->page, false);
    if (p) {
        uint16_t len = 0;
        const void* cur = page_get_slot(p, e->slot, &len);
        if (cur && e->old && ov_slot_equals(db_fd, cur, len, e->old, e->olen)) {
            page_delete_slot(p, e->slot);
            if (redo_write(p, db_fd) != SV_OK) return SV_IO_ERROR;
        } else page_free(p);
    }
    // 2. la riga nuova compare nella sua posizione (se non è già stata scritta)
    p = redo_page(db_fd, npid, true);
    if (!p) return SV_ERR;
    if (nslot == p->num_slots) ov_slot_put(db_fd, p, e->data + 6, e->dlen - 6);
    return redo_write(p, db_fd);
}

static int replay_link(int db_fd, const Rec* e) {
    Page* p = redo_page(db_fd, e->page, true);
    if (!p) return SV_ERR;
    if (redo_write(p, db_fd) != SV_OK) return SV_IO_ERROR;
    uint32_t prev = 0;
    if (e->data && e->dlen >= 4) memcpy(&prev, e->data, 4);
    if (!prev || prev == e->page) return SV_OK;
    Page* q = redo_page(db_fd, prev, false);
    if (!q) return SV_OK;
    if (page_next(q) == 0) {                         // il collegamento non era arrivato su disco
        page_set_next(q, e->page);
        q->flags |= PAGE_FLAG_CHAINED;
        return redo_write(q, db_fd);
    }
    page_free(q);
    return SV_OK;
}

// rifà una voce sul file dei dati (idempotente: gli slot di una pagina sono solo aggiunti, mai riusati,
// quindi lo stato dello slot registrato dice se l'operazione è già su disco)
static int replay_entry(int db_fd, const Rec* e) {
    if (e->page == 0) return SV_OK;
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
    switch (e->type) {
        case LOG_INSERT:
            if (e->slot == p->num_slots) ov_slot_put(db_fd, p, e->data, e->dlen);   // < num_slots: già applicato
            break;
        case LOG_DELETE:
            page_delete_slot(p, e->slot);                                     // no-op se già eliminato
            break;
        case LOG_UPDATE: {
            // ancora da applicare solo se lo slot contiene la versione precedente
            uint16_t len = 0;
            const void* cur = page_get_slot(p, e->slot, &len);
            if (cur && e->old && ov_slot_equals(db_fd, cur, len, e->old, e->olen)) {
                page_delete_slot(p, e->slot);
                ov_slot_put(db_fd, p, e->data, e->dlen);
            }
            break;
        }
        case LOG_CLR:
            if (action == CLR_DELETE) { page_delete_slot(p, e->slot); page_pop_slot(p, e->slot); }   // come undo_delete
            else if (action == CLR_REVIVE) {               // solo se lo slot è eliminato (altrimenti già fatto)
                uint16_t len = 0;
                if (e->slot < p->num_slots && !page_get_slot(p, e->slot, &len)) ov_slot_revive(db_fd, p, e->slot, img, ilen);
            }
            break;
        default: break;
    }
    return redo_write(p, db_fd);
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

// ── recupero in streaming ────────────────────────────────────────
// Memoria proporzionale alle transazioni aperte, non al log: la prima passata legge solo le intestazioni
// e tiene, per ogni transazione non ancora conclusa, gli offset dei suoi record (lista liberata al COMMIT /
// ABORT); la seconda rifà i record uno alla volta; la terza rilegge su richiesta quelli da annullare.

// un record all'offset off: 1 = letto, 0 = fine del log (o record troncato), -1 = errore di lettura / memoria
static int rec_read_at(int fd, int64_t off, Rec* e, bool payload) {
    memset(e, 0, sizeof(*e));
    uint8_t hdr[23];
    ssize_t r = sv_pread(fd, hdr, 23, off);
    if (r < 0) return -1;
    if (r < 23) return 0;
    e->type = hdr[0];
    memcpy(&e->tx, hdr + 1, 8);
    memcpy(&e->page, hdr + 9, 4);
    memcpy(&e->slot, hdr + 13, 2);
    memcpy(&e->dlen, hdr + 15, 4);
    memcpy(&e->olen, hdr + 19, 4);
    e->off = off;
    if (e->dlen > (64u << 20) || e->olen > (64u << 20)) return 0;     // intestazione danneggiata: fine
    if (!payload) return 1;
    if (e->dlen) {
        e->data = malloc(e->dlen);
        if (!e->data) return -1;
        r = sv_pread(fd, e->data, e->dlen, off + 23);
        if (r != (ssize_t)e->dlen) { free(e->data); e->data = NULL; return r < 0 ? -1 : 0; }
    }
    if (e->olen) {
        e->old = malloc(e->olen);
        if (!e->old) { free(e->data); e->data = NULL; return -1; }
        r = sv_pread(fd, e->old, e->olen, off + 23 + e->dlen);
        if (r != (ssize_t)e->olen) { free(e->data); free(e->old); e->data = e->old = NULL; return r < 0 ? -1 : 0; }
    }
    return 1;
}

static void rec_clear(Rec* e) { free(e->data); free(e->old); e->data = e->old = NULL; }

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

int wal_recovery(const char* log_path, const char* db_path, const char* cat_path, char* err, size_t errn) {
    (void)cat_path;
    err[0] = '\0';
    int log_fd = sv_open(log_path, SV_O_RDONLY, 0);
    if (log_fd < 0) return SV_OK;                                 // nessun log: niente da recuperare
    int rc = SV_OK;
    TxInfo* txs = NULL;
    int ntx = 0, captx = 0;
    uint32_t maxp = 0;
    int64_t pos = 0, nrec = 0;
    int64_t file_end = sv_lseek(log_fd, 0, SEEK_END);

    // ── 1. intestazioni: stato delle transazioni, record delle aperte, pagina più alta ──
    for (;;) {
        Rec e;
        int r = rec_read_at(log_fd, pos, &e, false);
        if (r < 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot read the transaction log"); break; }
        if (r == 0) break;
        int64_t next = pos + 23 + (int64_t)e.dlen + (int64_t)e.olen;
        if (file_end >= 0 && next > file_end) break;              // record troncato in fondo: fine del log
        nrec++;
        if (e.page > maxp) maxp = e.page;
        if (e.type == LOG_UPDATE_MOVE && e.dlen >= 6) {           // la posizione nuova: i primi 6 byte
            uint8_t t6[6];
            if (sv_pread(log_fd, t6, 6, pos + 23) == 6) { uint32_t np; memcpy(&np, t6, 4); if (np > maxp) maxp = np; }
        }
        TxInfo* t = e.tx ? tx_find(txs, ntx, e.tx) : NULL;
        if (e.tx && !t) {
            if (ntx == captx) {
                int nc = captx ? captx * 2 : 64;
                TxInfo* nt = realloc(txs, sizeof(TxInfo) * (size_t)nc);
                if (!nt) { rc = SV_ERR; snprintf(err, errn, "out of memory during recovery"); break; }
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
                    TxRec* nr = realloc(t->r, sizeof(TxRec) * (size_t)nc);
                    if (!nr) { rc = SV_ERR; snprintf(err, errn, "out of memory during recovery"); break; }
                    t->r = nr;
                    t->cap = nc;
                }
                t->r[t->n++] = (TxRec){ pos, false };
            } else if (e.type == LOG_ROLLBACK_TO && e.dlen >= 8) {  // record dopo il savepoint: già annullati
                int64_t sp;
                if (sv_pread(log_fd, &sp, 8, pos + 23) == 8) while (t->n > 0 && t->r[t->n - 1].off >= sp) t->n--;
            } else if (e.type == LOG_CLR && e.dlen >= 9) {         // record già annullato
                int64_t c;
                if (sv_pread(log_fd, &c, 8, pos + 23) == 8 && c >= 0) {
                    int lo = 0, hi = t->n - 1;
                    while (lo <= hi) {
                        int m = lo + (hi - lo) / 2;
                        if (t->r[m].off == c) { t->r[m].skip = true; break; }
                        if (t->r[m].off < c) lo = m + 1; else hi = m - 1;
                    }
                }
            }
        }
        pos = next;
    }
    int64_t log_end = pos;
    if (rc == SV_OK && nrec == 0) {                               // log vuoto
        free(txs);
        sv_close(log_fd);
        remove(log_path);
        return SV_OK;
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

    // ── 2. REDO: ogni record che tocca le pagine (anche i CLR), nell'ordine del log, uno alla volta ──
    for (int64_t p2 = 0; rc == SV_OK && p2 < log_end; ) {
        Rec e;
        int r = rec_read_at(log_fd, p2, &e, true);
        if (r < 0) { rc = SV_ERR; snprintf(err, errn, "cannot read the transaction log (offset %lld)", (long long)p2); break; }
        if (r == 0) break;
        if (is_page_rec(e.type) || e.type == LOG_CLR || e.type == LOG_LINK) {
            int rr = replay_entry(db_fd, &e);
            if (rr != SV_OK) { rc = rr; snprintf(err, errn, "cannot replay the transaction log (page %u)", e.page); }
        }
        p2 += 23 + (int64_t)e.dlen + (int64_t)e.olen;
        rec_clear(&e);
    }

    // ── 3. UNDO: le transazioni senza COMMIT né ABORT, dalla fine; ogni passo diventa un CLR nel log ──
    int64_t nlose = 0;
    for (int j = 0; j < ntx; j++) if (!txs[j].done) for (int k = 0; k < txs[j].n; k++) if (!txs[j].r[k].skip) nlose++;
    bool any_loser = false;
    for (int j = 0; j < ntx; j++) if (!txs[j].done) any_loser = true;
    if (rc == SV_OK && any_loser) {
        int64_t* offs = malloc(sizeof(int64_t) * (size_t)(nlose ? nlose : 1));
        WAL* w = offs ? wal_create(log_path) : NULL;
        if (!offs) { rc = SV_ERR; snprintf(err, errn, "out of memory during recovery"); }
        else if (!w) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot write the transaction log"); }
        else {
            int64_t k = 0;
            for (int j = 0; j < ntx; j++) if (!txs[j].done) for (int q = 0; q < txs[j].n; q++) if (!txs[j].r[q].skip) offs[k++] = txs[j].r[q].off;
            qsort(offs, (size_t)k, sizeof(int64_t), off_desc);   // dal più recente al più vecchio
            Undo u = { db_fd, w, 0, NULL, 0, 0, SV_OK, "" };
            for (int64_t i = 0; i < k && u.rc == SV_OK; i++) {
                Rec e;
                int r = rec_read_at(log_fd, offs[i], &e, true);
                if (r <= 0) { undo_fail(&u, SV_IO_ERROR, "cannot read a record to undo", 0); break; }
                undo_rec(&u, &e);
                rec_clear(&e);
            }
            rc = undo_finish(&u);
            for (int j = 0; j < ntx && rc == SV_OK; j++)          // annullamento completo e su disco
                if (!txs[j].done && wal_write(w, LOG_ABORT, txs[j].tx, 0, (SVRID){0, 0}, NULL, 0, NULL, 0) != SV_OK) rc = SV_IO_ERROR;
            if (rc == SV_OK && wal_flush(w) != SV_OK) rc = SV_IO_ERROR;
            if (rc != SV_OK) snprintf(err, errn, "cannot undo the unfinished transactions: %s", u.why[0] ? u.why : "I/O error");
        }
        if (w) wal_destroy(w);
        free(offs);
    }
    for (int j = 0; j < ntx; j++) free(txs[j].r);
    free(txs);
    sv_close(log_fd);

    // il file dei dati su disco prima di eliminare il log (altrimenti una seconda caduta perderebbe tutto)
    if (rc == SV_OK && sv_fsync(db_fd) != 0) { rc = SV_IO_ERROR; snprintf(err, errn, "cannot sync the data file"); }
    if (db_fd >= 0) sv_close(db_fd);
    if (rc != SV_OK) return rc;
    if (remove(log_path) != 0) { snprintf(err, errn, "cannot remove the recovered transaction log"); return SV_IO_ERROR; }
    fsync_dir_of(log_path);
    return SV_OK;
}

int64_t wal_size(WAL* wal) {
    if (!wal) return 0;
    int64_t cur = sv_lseek(wal->fd, 0, SEEK_END);
    return (cur < 0 ? 0 : cur) + (int64_t)wal->blen;
}

int wal_truncate(WAL* wal) {
    if (!wal) return SV_ERR;
    if (wal->failed) return SV_IO_ERROR;
    wal->blen = 0;
    if (sv_ftruncate(wal->fd, 0) != 0) { wal->failed = true; return SV_IO_ERROR; }
    sv_lseek(wal->fd, 0, SEEK_SET);
    if (sv_fsync(wal->fd) != 0) { wal->failed = true; return SV_IO_ERROR; }
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
