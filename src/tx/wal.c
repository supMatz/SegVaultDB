#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

WAL* wal_create(const char* path) {
    WAL* wal = calloc(1, sizeof(WAL));
    snprintf(wal->path, sizeof(wal->path), "%s", path);
    wal->fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
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
    // Entry format: |type(1)|tx_id(8)|page_id(4)|slot_id(2)|data_len(4)|old_len(4)|data|old|
    uint8_t hdr[23];
    hdr[0] = type;
    memcpy(hdr + 1, &tx_id, 8);
    memcpy(hdr + 9, &page_id, 4);
    memcpy(hdr + 13, &rid.slot_id, 2);
    memcpy(hdr + 15, &data_len, 4);
    memcpy(hdr + 19, &old_data_len, 4);

    write(wal->fd, hdr, 23);
    if (data && data_len > 0) write(wal->fd, data, data_len);
    if (old_data && old_data_len > 0) write(wal->fd, old_data, old_data_len);
    wal->sequence++;
    return SV_OK;
}

int wal_flush(WAL* wal) {
    if (!wal) return SV_ERR;
    fsync(wal->fd);
    return SV_OK;
}

int wal_recovery(const char* log_path, const char* db_path, const char* cat_path) {
    (void)db_path; (void)cat_path;
    // Simple truncation-based recovery: delete the WAL
    // (redo/undo not implemented for minimal WAL)
    FILE* f = fopen(log_path, "r");
    if (f) {
        fclose(f);
        remove(log_path);
    }
    return SV_OK;
}

void wal_destroy(WAL* wal) {
    if (!wal) return;
    if (wal->fd >= 0) close(wal->fd);
    free(wal);
}
