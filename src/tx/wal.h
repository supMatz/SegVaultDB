#ifndef WAL_H
#define WAL_H

#include "../../include/common.h"
#include "../table/tuple.h"

#define LOG_BEGIN   1
#define LOG_COMMIT  2
#define LOG_ABORT   3
#define LOG_INSERT  4
#define LOG_DELETE  5
#define LOG_UPDATE  6

typedef struct {
    uint32_t page_id;
    uint16_t slot_id;
} SVRID;

typedef struct {
    int      fd;
    char     path[512];
    uint64_t sequence;
} WAL;

WAL* wal_create(const char* path);
int  wal_write(WAL* wal, uint8_t type, uint64_t tx_id, uint32_t page_id,
               SVRID rid, const uint8_t* data, uint32_t data_len,
               const uint8_t* old_data, uint32_t old_data_len);
int  wal_flush(WAL* wal);
int  wal_recovery(const char* log_path, const char* db_path, const char* cat_path);
void wal_destroy(WAL* wal);

#endif
