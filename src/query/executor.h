#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "../../include/common.h"
#include "../catalog/schema.h"
#include "../storage/buffer_pool.h"
#include "../table/tuple.h"
#include "../table/heap.h"
#include "../index/btree.h"
#include "../bridge/db_api.h"
#include "../tx/wal.h"
#include "parser.h"

typedef struct {
    Catalog*    catalog;
    BufferPool* bp;
    WAL*        wal;
    uint64_t    tx_id;
} ExecCtx;

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt);

// scarta gli indici in memoria (da chiamare dopo ROLLBACK o modifiche alle pagine fuori dall'executor)
void executor_invalidate_indexes(void);

// CREATE TABLE della tabella (with_fk: con le chiavi esterne) e ALTER TABLE per le sue chiavi
// esterne; stringhe da liberare con free
char* executor_create_statement(Catalog* cat, const char* db, const char* table, bool with_fk);
char* executor_fk_statements(Catalog* cat, const char* db, const char* table);

#endif
