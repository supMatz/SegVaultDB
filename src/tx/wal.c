#include "wal.h"
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

    sv_write(wal->fd, hdr, 23);
    if (data && data_len > 0) sv_write(wal->fd, data, data_len);
    if (old_data && old_data_len > 0) sv_write(wal->fd, old_data, old_data_len);
    wal->sequence++;
    return SV_OK;
}

int wal_flush(WAL* wal) {
    if (!wal) return SV_ERR;
    sv_fsync(wal->fd);
    return SV_OK;
}

// Replay a single WAL entry on the database file
static void replay_entry(int db_fd, uint8_t type, uint32_t page_id,
                          uint16_t slot_id,
                          const uint8_t* data, uint32_t data_len,
                          const uint8_t* old_data, uint32_t old_data_len) {
    if (page_id == 0) return;
    // Read the page from db file
    Page* p = page_read(page_id, db_fd);
    if (!p && type == LOG_INSERT) {
        // Page doesn't exist yet - create it
        // For recovery, pages should already exist
        return;
    }
    if (!p) return;

    switch (type) {
        case LOG_INSERT:
            page_insert(p, data, data_len);
            break;
        case LOG_DELETE:
            page_delete_slot(p, slot_id);
            break;
        case LOG_UPDATE:
            // Delete old slot, insert new data
            page_delete_slot(p, slot_id);
            page_insert(p, data, data_len);
            break;
    }
    page_write(p, db_fd);
    page_free(p);
}

static void undo_entry(int db_fd, uint8_t type, uint32_t page_id,
                        uint16_t slot_id,
                        const uint8_t* data, uint32_t data_len,
                        const uint8_t* old_data, uint32_t old_data_len) {
    if (page_id == 0) return;
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
                page_insert(p, old_data, old_data_len);
            break;
        case LOG_UPDATE:
            // Undo update = restore old data (delete new, insert old)
            page_delete_slot(p, slot_id);
            if (old_data && old_data_len > 0)
                page_insert(p, old_data, old_data_len);
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
    #define MAX_ENTRIES 65536
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

    LogEntry entries[MAX_ENTRIES];
    int num_entries = 0;

    uint64_t active_tx[64];
    int num_active = 0;

    uint8_t hdr[23];
    while (sv_read(log_fd, hdr, 23) == 23 && num_entries < MAX_ENTRIES) {
        LogEntry* e = &entries[num_entries];
        e->type = hdr[0];
        memcpy(&e->tx_id, hdr + 1, 8);
        memcpy(&e->page_id, hdr + 9, 4);
        memcpy(&e->slot_id, hdr + 13, 2);
        memcpy(&e->data_len, hdr + 15, 4);
        memcpy(&e->old_data_len, hdr + 19, 4);

        e->data = NULL;
        e->old_data = NULL;

        if (e->data_len > 0) {
            e->data = malloc(e->data_len);
            sv_read(log_fd, e->data, e->data_len);
        }
        if (e->old_data_len > 0) {
            e->old_data = malloc(e->old_data_len);
            sv_read(log_fd, e->old_data, e->old_data_len);
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

    sv_close(db_fd);
    remove(log_path);
    return SV_OK;
}

int wal_undo_tx(const char* log_path, const char* db_path, uint64_t tx_id) {
    int log_fd = sv_open(log_path, SV_O_RDONLY, 0);
    if (log_fd < 0) return SV_NOT_FOUND;

    int db_fd = sv_open(db_path, SV_O_RDWR, 0);
    if (db_fd < 0) { sv_close(log_fd); return SV_ERR; }

    #define MAX_ENTRIES 65536
    typedef struct {
        uint8_t  type;
        uint32_t page_id;
        uint16_t slot_id;
        uint32_t data_len;
        uint32_t old_data_len;
        uint8_t* data;
        uint8_t* old_data;
    } LogEntry;

    LogEntry entries[MAX_ENTRIES];
    int num_entries = 0;

    uint8_t hdr[23];
    while (sv_read(log_fd, hdr, 23) == 23 && num_entries < MAX_ENTRIES) {
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

        if (e->data_len > 0) {
            e->data = malloc(e->data_len);
            sv_read(log_fd, e->data, e->data_len);
        }
        if (e->old_data_len > 0) {
            e->old_data = malloc(e->old_data_len);
            sv_read(log_fd, e->old_data, e->old_data_len);
        }
        num_entries++;
    }
    sv_close(log_fd);

    // Undo in reverse order
    for (int i = num_entries - 1; i >= 0; i--) {
        LogEntry* e = &entries[i];
        undo_entry(db_fd, e->type, e->page_id, e->slot_id,
                   e->data, e->data_len, e->old_data, e->old_data_len);
        free(e->data);
        free(e->old_data);
    }

    sv_close(db_fd);
    return SV_OK;
}

void wal_destroy(WAL* wal) {
    if (!wal) return;
    if (wal->fd >= 0) sv_close(wal->fd);
    free(wal);
}
