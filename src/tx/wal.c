#include "wal.h"
#include "../table/overflow.h"
#include "../storage/page.h"
#include "platform_compat.h"
#include <string.h>
#include <stdio.h>

WAL* wal_create(const char* path) {
    WAL* wal = calloc(1, sizeof(WAL));
    snprintf(wal->path, sizeof(wal->path), "%s", path);
    wal->fd = sv_open(path, SV_O_RDWR | SV_O_CREAT | SV_O_APPEND, 0644);
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
    memcpy(hdr + 15, &data_len, 4);
    memcpy(hdr + 19, &old_data_len, 4);
    size_t need = 23 + (size_t)data_len + (size_t)old_data_len;
    if (wal->blen + need > wal->bcap && wal_write_buffer(wal) != SV_OK) return SV_ERR;
    if (need > wal->bcap) {                       // voce più grande del buffer: scritta subito
        if (!sv_write_full(wal->fd, hdr, 23) ||
            (data_len > 0 && !sv_write_full(wal->fd, data, data_len)) ||
            (old_data_len > 0 && !sv_write_full(wal->fd, old_data, old_data_len))) {
            fprintf(stderr, "[SegVault] cannot write the transaction log (disk full?)\n");
            return SV_ERR;
        }
    } else {
        memcpy(wal->buf + wal->blen, hdr, 23);
        if (data_len) memcpy(wal->buf + wal->blen + 23, data, data_len);
        if (old_data_len) memcpy(wal->buf + wal->blen + 23 + data_len, old_data, old_data_len);
        wal->blen += need;
    }
    wal->sequence++;
    return SV_OK;
}

// legge il contenuto di una voce del log (len byte); false se il file finisce prima o la
// lunghezza è assurda (intestazione danneggiata)
static bool read_payload(int fd, uint32_t len, uint8_t** out) {
    *out = NULL;
    if (len == 0) return true;
    if (len > (64u << 20)) return false;
    uint8_t* b = malloc(len);
    if (!b) return false;
    if (!sv_read_full(fd, b, len)) { free(b); return false; }
    *out = b;
    return true;
}

int wal_write_buffer(WAL* wal) {
    if (!wal) return SV_ERR;
    if (!wal->blen) return SV_OK;
    if (!sv_write_full(wal->fd, wal->buf, wal->blen)) {
        fprintf(stderr, "[SegVault] cannot write the transaction log (disk full?)\n");
        return SV_ERR;
    }
    wal->blen = 0;
    return SV_OK;
}

int wal_flush(WAL* wal) {
    if (!wal) return SV_ERR;
    int r = wal_write_buffer(wal);
    sv_fsync(wal->fd);
    return r;
}

// LOG_UPDATE_MOVE: posizione nuova all'inizio di data
static bool move_target(const uint8_t* data, uint32_t data_len, uint32_t* pid, uint16_t* slot) {
    if (!data || data_len < 6) return false;
    memcpy(pid, data, 4);
    memcpy(slot, data + 4, 2);
    return *pid != 0;
}

static void replay_move(int db_fd, uint32_t page_id, uint16_t slot_id, const uint8_t* data, uint32_t data_len,
                        const uint8_t* old_data, uint32_t old_data_len) {
    uint32_t npid;
    uint16_t nslot;
    if (!move_target(data, data_len, &npid, &nslot)) return;
    // 1. la riga vecchia sparisce (se c'è ancora)
    Page* p = page_read(page_id, db_fd);
    if (p) {
        uint16_t len = 0;
        const void* cur = page_get_slot(p, slot_id, &len);
        if (cur && old_data && ov_slot_equals(db_fd, cur, len, old_data, old_data_len)) {
            page_delete_slot(p, slot_id);
            page_write(p, db_fd);
        }
        page_free(p);
    }
    // 2. la riga nuova compare nella sua posizione (se non è già stata scritta)
    p = page_read(npid, db_fd);
    if (p) {
        if (nslot == p->num_slots && ov_slot_put(db_fd, p, data + 6, data_len - 6) >= 0) page_write(p, db_fd);
        page_free(p);
    }
}

static void undo_move(int db_fd, uint32_t page_id, const uint8_t* data, uint32_t data_len,
                      const uint8_t* old_data, uint32_t old_data_len) {
    uint32_t npid;
    uint16_t nslot;
    if (!move_target(data, data_len, &npid, &nslot)) return;
    Page* p = page_read(npid, db_fd);
    if (p) { page_delete_slot(p, nslot); page_write(p, db_fd); page_free(p); }
    p = page_read(page_id, db_fd);
    if (p) {
        if (old_data && old_data_len > 0) ov_slot_put(db_fd, p, old_data, old_data_len);
        page_write(p, db_fd);
        page_free(p);
    }
}

// Replay a single WAL entry on the database file
static void replay_entry(int db_fd, uint8_t type, uint32_t page_id,
                          uint16_t slot_id,
                          const uint8_t* data, uint32_t data_len,
                          const uint8_t* old_data, uint32_t old_data_len) {
    if (page_id == 0) return;
    if (type == LOG_UPDATE_MOVE) { replay_move(db_fd, page_id, slot_id, data, data_len, old_data, old_data_len); return; }
    // Read the page from db file
    Page* p = page_read(page_id, db_fd);
    if (!p && type == LOG_INSERT) {
        // Page doesn't exist yet - create it
        // For recovery, pages should already exist
        return;
    }
    if (!p) return;

    // REDO idempotente: gli slot di una pagina sono solo aggiunti (mai riusati), quindi lo
    // stato dello slot registrato dice se l'operazione è già su disco. Rieseguirla alla cieca
    // duplicava le righe a ogni riavvio.
    switch (type) {
        case LOG_INSERT:
            if (slot_id == p->num_slots) ov_slot_put(db_fd, p, data, data_len);   // < num_slots: già applicato
            break;
        case LOG_DELETE:
            page_delete_slot(p, slot_id);                                   // no-op se già eliminato
            break;
        case LOG_UPDATE: {
            // ancora da applicare solo se lo slot contiene la versione precedente
            uint16_t len = 0;
            const void* cur = page_get_slot(p, slot_id, &len);
            if (cur && old_data && ov_slot_equals(db_fd, cur, len, old_data, old_data_len)) {
                page_delete_slot(p, slot_id);
                ov_slot_put(db_fd, p, data, data_len);
            }
            break;
        }
    }
    page_write(p, db_fd);
    page_free(p);
}

static void undo_entry(int db_fd, uint8_t type, uint32_t page_id,
                        uint16_t slot_id,
                        const uint8_t* data, uint32_t data_len,
                        const uint8_t* old_data, uint32_t old_data_len) {
    if (page_id == 0) return;
    if (type == LOG_UPDATE_MOVE) { undo_move(db_fd, page_id, data, data_len, old_data, old_data_len); return; }
    Page* p = page_read(page_id, db_fd);
    if (!p) return;

    switch (type) {
        case LOG_INSERT:
            // Undo insert = delete the slot
            page_delete_slot(p, slot_id);
            break;
        case LOG_DELETE:
            // Undo delete = re-insert old data
            if (old_data && old_data_len > 0)
                ov_slot_put(db_fd, p, old_data, old_data_len);
            break;
        case LOG_UPDATE:
            // Undo update = restore old data (delete new, insert old)
            page_delete_slot(p, slot_id);
            if (old_data && old_data_len > 0)
                ov_slot_put(db_fd, p, old_data, old_data_len);
            break;
    }
    page_write(p, db_fd);
    page_free(p);
}

int wal_recovery(const char* log_path, const char* db_path, const char* cat_path) {
    (void)cat_path;
    int log_fd = sv_open(log_path, SV_O_RDONLY, 0);
    if (log_fd < 0) return SV_OK; // No log to recover

    int db_fd = sv_open(db_path, SV_O_RDWR, 0);
    if (db_fd < 0) { sv_close(log_fd); return SV_ERR; }

    // Phase 1: Read all entries, track transaction state
    typedef struct {
        uint8_t  type;
        uint64_t tx_id;
        uint32_t page_id;
        uint16_t slot_id;
        uint32_t data_len;
        uint32_t old_data_len;
        uint8_t* data;
        uint8_t* old_data;
        off_t    offset; // unused but kept for structure
    } LogEntry;

    int cap_entries = 1024;
    LogEntry* entries = calloc((size_t)cap_entries, sizeof(LogEntry));
    if (!entries) { close(log_fd); close(db_fd); return SV_ERR; }
    int num_entries = 0;

    uint64_t active_tx[64];
    int num_active = 0;

    uint8_t hdr[23];
    while (sv_read(log_fd, hdr, 23) == 23) {
        if (num_entries >= cap_entries) {
            LogEntry* ne = realloc(entries, sizeof(LogEntry) * (size_t)cap_entries * 2);
            if (!ne) break;
            entries = ne;
            cap_entries *= 2;
        }
        LogEntry* e = &entries[num_entries];
        e->type = hdr[0];
        memcpy(&e->tx_id, hdr + 1, 8);
        memcpy(&e->page_id, hdr + 9, 4);
        memcpy(&e->slot_id, hdr + 13, 2);
        memcpy(&e->data_len, hdr + 15, 4);
        memcpy(&e->old_data_len, hdr + 19, 4);

        e->data = NULL;
        e->old_data = NULL;

        // voce troncata (arresto durante la scrittura): è la fine del log
        if (!read_payload(log_fd, e->data_len, &e->data) || !read_payload(log_fd, e->old_data_len, &e->old_data)) {
            free(e->data);
            e->data = NULL;
            break;
        }

        // Track transaction state
        if (e->type == LOG_BEGIN) {
            if (num_active < 64) active_tx[num_active++] = e->tx_id;
        } else if (e->type == LOG_COMMIT || e->type == LOG_ABORT) {
            for (int i = 0; i < num_active; i++) {
                if (active_tx[i] == e->tx_id) {
                    active_tx[i] = active_tx[--num_active];
                    break;
                }
            }
        }
        num_entries++;
    }
    sv_close(log_fd);

    // Phase 2: REDO — replay all entries (idempotent)
    for (int i = 0; i < num_entries; i++) {
        LogEntry* e = &entries[i];
        if (e->type == LOG_BEGIN || e->type == LOG_COMMIT ||
            e->type == LOG_ABORT || e->type == LOG_SAVEPOINT ||
            e->type == LOG_ROLLBACK_TO) continue;
        replay_entry(db_fd, e->type, e->page_id, e->slot_id,
                     e->data, e->data_len, e->old_data, e->old_data_len);
    }

    // Phase 3: UNDO — undo entries for still-active transactions (in reverse)
    for (int i = num_entries - 1; i >= 0; i--) {
        LogEntry* e = &entries[i];
        if (e->type == LOG_BEGIN || e->type == LOG_COMMIT ||
            e->type == LOG_ABORT || e->type == LOG_SAVEPOINT ||
            e->type == LOG_ROLLBACK_TO) continue;

        bool is_active = false;
        for (int j = 0; j < num_active; j++) {
            if (active_tx[j] == e->tx_id) { is_active = true; break; }
        }
        if (is_active) {
            undo_entry(db_fd, e->type, e->page_id, e->slot_id,
                       e->data, e->data_len, e->old_data, e->old_data_len);
        }
    }

    // Cleanup
    for (int i = 0; i < num_entries; i++) {
        free(entries[i].data);
        free(entries[i].old_data);
    }
    free(entries);

    close(db_fd);
    remove(log_path);
    return SV_OK;
}

int wal_undo_tx(const char* log_path, const char* db_path, uint64_t tx_id) {
    return wal_undo_tx_from(log_path, db_path, tx_id, 0);
}

int wal_undo_tx_from(const char* log_path, const char* db_path, uint64_t tx_id, long from) {
    int log_fd = sv_open(log_path, SV_O_RDONLY, 0);
    if (log_fd < 0) return SV_NOT_FOUND;
    if (from > 0 && sv_lseek(log_fd, from, SEEK_SET) != from) { sv_close(log_fd); return SV_ERR; }

    int db_fd = sv_open(db_path, SV_O_RDWR, 0);
    if (db_fd < 0) { sv_close(log_fd); return SV_ERR; }

    typedef struct {
        uint8_t  type;
        uint32_t page_id;
        uint16_t slot_id;
        uint32_t data_len;
        uint32_t old_data_len;
        uint8_t* data;
        uint8_t* old_data;
    } LogEntry;

    int cap_entries = 256;
    LogEntry* entries = calloc((size_t)cap_entries, sizeof(LogEntry));
    if (!entries) { close(log_fd); close(db_fd); return SV_ERR; }
    int num_entries = 0;

    uint8_t hdr[23];
    while (sv_read(log_fd, hdr, 23) == 23) {
        if (num_entries >= cap_entries) {
            LogEntry* ne = realloc(entries, sizeof(LogEntry) * (size_t)cap_entries * 2);
            if (!ne) break;
            entries = ne;
            cap_entries *= 2;
        }
        LogEntry* e = &entries[num_entries];
        e->type = hdr[0];
        uint64_t entry_tx_id;
        memcpy(&entry_tx_id, hdr + 1, 8);
        memcpy(&e->page_id, hdr + 9, 4);
        memcpy(&e->slot_id, hdr + 13, 2);
        memcpy(&e->data_len, hdr + 15, 4);
        memcpy(&e->old_data_len, hdr + 19, 4);
        e->data = NULL;
        e->old_data = NULL;

        if (entry_tx_id != tx_id) {
            // Skip but advance file position
            if (e->data_len > 0) sv_lseek(log_fd, e->data_len, SEEK_CUR);
            if (e->old_data_len > 0) sv_lseek(log_fd, e->old_data_len, SEEK_CUR);
            continue;
        }

        if (!read_payload(log_fd, e->data_len, &e->data) || !read_payload(log_fd, e->old_data_len, &e->old_data)) {
            free(e->data);
            e->data = NULL;
            break;                                       // voce troncata: fine del log
        }
        num_entries++;
    }
    sv_close(log_fd);

    // Undo in reverse order (le voci di controllo non toccano le pagine)
    for (int i = num_entries - 1; i >= 0; i--) {
        LogEntry* e = &entries[i];
        if (e->type == LOG_INSERT || e->type == LOG_DELETE || e->type == LOG_UPDATE || e->type == LOG_UPDATE_MOVE)
            undo_entry(db_fd, e->type, e->page_id, e->slot_id,
                       e->data, e->data_len, e->old_data, e->old_data_len);
        free(e->data);
        free(e->old_data);
    }

    free(entries);
    sv_close(db_fd);
    return SV_OK;
}

long wal_size(WAL* wal) {
    if (!wal) return 0;
    long cur = sv_lseek(wal->fd, 0, SEEK_END);
    return (cur < 0 ? 0 : cur) + (long)wal->blen;
}

int wal_truncate(WAL* wal) {
    if (!wal) return SV_ERR;
    wal->blen = 0;
    if (sv_ftruncate(wal->fd, 0) != 0) return SV_IO_ERROR;
    sv_lseek(wal->fd, 0, SEEK_SET);
    sv_fsync(wal->fd);
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
