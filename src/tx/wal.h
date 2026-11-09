#ifndef WAL_H
#define WAL_H

#include "../../include/common.h"
#include "../table/tuple.h"

#define LOG_BEGIN       1
#define LOG_COMMIT      2
#define LOG_ABORT       3
#define LOG_INSERT      4
#define LOG_DELETE      5
#define LOG_UPDATE      6
#define LOG_SAVEPOINT   7
#define LOG_ROLLBACK_TO 8
// modifica di una riga che può cambiare posizione: un solo record con le due posizioni
//   intestazione: page_id / slot della riga vecchia
//   data:     [page_id nuovo: 4 byte][slot nuovo: 2 byte][riga nuova]
//   old_data: riga vecchia
#define LOG_UPDATE_MOVE 9

typedef struct {
    uint32_t page_id;
    uint16_t slot_id;
} SVRID;

typedef struct {
    int      fd;
    char     path[512];
    uint64_t sequence;
    uint64_t checkpoints;   // checkpoint eseguiti in questa sessione
} WAL;

WAL* wal_create(const char* path);
int  wal_write(WAL* wal, uint8_t type, uint64_t tx_id, uint32_t page_id,
               SVRID rid, const uint8_t* data, uint32_t data_len,
               const uint8_t* old_data, uint32_t old_data_len);
int  wal_flush(WAL* wal);
int  wal_recovery(const char* log_path, const char* db_path, const char* cat_path);
int  wal_undo_tx(const char* log_path, const char* db_path, uint64_t tx_id);
// annulla le voci della transazione a partire dall'offset 'from' del log (ROLLBACK TO)
int  wal_undo_tx_from(const char* log_path, const char* db_path, uint64_t tx_id, long from);
// dimensione del log in byte
long wal_size(WAL* wal);
// svuota il log (checkpoint: da chiamare solo con tutte le pagine già su disco e nessuna
// transazione aperta)
int  wal_truncate(WAL* wal);
void wal_destroy(WAL* wal);

#endif
