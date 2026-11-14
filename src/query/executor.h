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

struct BindCtx;
struct EvalCtx;

typedef struct {
    Catalog*    catalog;
    BufferPool* bp;
    WAL*        wal;
    uint64_t    tx_id;
    // istruzioni del corpo di un trigger: NEW e OLD sono visibili come una query esterna
    struct BindCtx* trig_bind;
    struct EvalCtx* trig_eval;
    int         trig_depth;       // trigger annidati (un trigger che ne attiva altri)
    // permessi delle istruzioni di una procedura (utente che la chiama); NULL = tutto permesso
    bool      (*auth_hook)(const ASTNode* stmt, char* err, size_t errn);
} ExecCtx;

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt);

// scarta gli indici in memoria (da chiamare dopo ROLLBACK o modifiche alle pagine fuori dall'executor)
void executor_invalidate_indexes(void);

// information_schema: lo statement (di questo thread) legge le tabelle virtuali del catalogo;
// see (NULL = tutto) dice quali database mostrare all'utente
void executor_set_infoschema(bool on);
void executor_set_db_filter(bool (*see)(const char* db));

// modalità MySQL (connessioni del protocollo MySQL): le regole permissive di MySQL al posto di quelle
// dello standard, es. SELECT DISTINCT ... ORDER BY una colonna non selezionata
void executor_set_mysql_mode(bool on);
bool executor_mysql_mode(void);

// CREATE TABLE della tabella (with_fk: con le chiavi esterne) e ALTER TABLE per le sue chiavi
// esterne; stringhe da liberare con free
char* executor_create_statement(Catalog* cat, const char* db, const char* table, bool with_fk);
char* executor_fk_statements(Catalog* cat, const char* db, const char* table);

// ogni modifica di una riga dentro una transazione (tx_id != 0): per le letture della versione confermata
// (vedi heap_snapshot). type: LOG_INSERT / LOG_DELETE / LOG_UPDATE / LOG_UPDATE_MOVE; old: la riga di prima
// serializzata (NULL per INSERT); new_rid: la nuova posizione (UPDATE_MOVE)
typedef void (*ExecChangeHook)(uint64_t tx_id, const char* db, const char* table, uint8_t type, RID rid, RID new_rid,
                               const uint8_t* old, size_t old_len);
void executor_set_change_hook(ExecChangeHook hook);

// SET FOREIGN_KEY_CHECKS = 0 / 1 (dei dump di MySQL): con i controlli spenti una riga può riferire una
// riga che non c'è ancora (i dati arrivano in qualunque ordine), come in MySQL
void executor_set_fk_checks(bool on);
bool executor_fk_checks(void);

#endif
