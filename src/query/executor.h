#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "../../include/common.h"
#include "../catalog/schema.h"
#include "../storage/buffer_pool.h"
#include "../table/tuple.h"
#include "../table/heap.h"
#include "../index/bptree.h"
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
    // routine in esecuzione (variabili locali, cursori, handler): NULL fuori dalle routine
    void*       routine;
    bool        rt_notfound;      // l'ultima SELECT ... INTO non ha trovato righe (condizione NOT FOUND)
    // permessi sulle tabelle: ogni tabella letta o scritta si controlla qui, dove l'executor la apre
    // (NULL o "" = tutto permesso). priv_off > 0 dentro il corpo di una vista: i permessi del corpo
    // sono stati controllati per chi l'ha creata (CREATE VIEW esegue la definizione)
    const char* user;
    int         priv_off;
} ExecCtx;

// l'utente ha il permesso priv (AUTH_*) sulla tabella db.table? Altrimenti scrive l'errore in err
bool executor_priv_ok(const ExecCtx* x, const char* db, const char* table, unsigned priv, char* err, size_t errn);

// variabili di sessione (@x): ogni sessione ha le sue; db_api le scambia insieme al resto dello stato
typedef struct UserVars UserVars;
UserVars* executor_uservars(void);
void      executor_set_uservars(UserVars* v);
void      executor_uservars_free(UserVars* v);
// PREPARE / EXECUTE / DEALLOCATE PREPARE della sessione
bool        executor_prepare_store(const char* name, const char* sql);
const char* executor_prepare_get(const char* name);
bool        executor_prepare_drop(const char* name);
char*       executor_uservar_literal(const char* name);      // @nome come letterale SQL (malloc'd)
// sequenze (CREATE SEQUENCE): file di stato nella cartella dei dati (caricato qui)
void        executor_set_seq_file(const char* path);
void        executor_drop_db_sequences(const char* db);
// assegnazioni di SET @a = espr, ... (NODE_SET)
QueryResult* executor_set_session_vars(ExecCtx* ctx, ASTNode* stmt);

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt);

// scarta gli indici in memoria (da chiamare dopo ROLLBACK o modifiche alle pagine fuori dall'executor)
void executor_invalidate_indexes(void);
// solo quella tabella (indici in memoria e mappa dello spazio libero): dopo un ROLLBACK che l'ha toccata
void executor_invalidate_table(BufferPool* bp, const char* db, const char* table, uint32_t root_page_id);

// information_schema: lo statement (di questo thread) legge le tabelle virtuali del catalogo;
// see (NULL = tutto) dice quali database mostrare all'utente
void executor_set_infoschema(bool on);
void executor_set_db_filter(bool (*see)(const char* db));

// modalità MySQL (connessioni del protocollo MySQL): le regole permissive di MySQL al posto di quelle
// dello standard, es. SELECT DISTINCT ... ORDER BY una colonna non selezionata
void executor_set_mysql_mode(bool on);
// descrizione (PREPARE): le tabelle sembrano vuote, una SELECT senza FROM non produce righe, NEXTVAL / SETVAL e le
// funzioni dell'utente non fanno niente. Il risultato ha le colonne giuste e nessuna riga
void executor_set_describe(bool on);
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

// tabella eliminata: la sua catena di pagine (root) non è più riferita dal catalogo (P1.5: il motore la libera)
void executor_set_drop_hook(void (*hook)(uint32_t root));
// indici su disco (P1.6): hook chiamato quando una scrittura azzera gli alberi di una tabella (il motore lo registra
// nel log prima della riga); scrittura degli alberi al checkpoint; radici abbandonate da liberare; contatore
void executor_set_tree_hook(void (*hook)(void));
// risultati in streaming (P1.14): per la prossima SELECT di primo livello di questo thread. begin riceve il primo
// blocco (nomi e tipi delle colonne), rows ogni blocco (anche il primo); false = interrompere. La SELECT restituisce
// allora un QueryResult senza righe con streamed = true e streamed_rows (se la query non si può fare a blocchi,
// il risultato normale con streamed = false: le righe le manda chi chiama)
typedef struct {
    void* ud;
    bool (*begin)(void* ud, const QueryResult* first_block);
    bool (*rows)(void* ud, const QueryResult* block);
} ExecRowSink;
void executor_set_stream_sink(ExecRowSink* sink);
// chiusura: sequenze salvate col valore esatto (senza la riserva di NEXTVAL)
void executor_seq_flush(void);
int  executor_trees_write(ExecCtx* ctx, uint64_t lsn);
int  executor_trees_garbage(uint32_t** roots);
void executor_trees_drop_all(Catalog* c);
void executor_trees_count(Catalog* c);

// VACUUM FULL: riscrive le righe della tabella in pagine nuove; NULL = fatto (radice e pagine in out_*)
const char* executor_rebuild_table(ExecCtx* ctx, TableSchema* ts, uint32_t* out_root, uint32_t* out_pages);

#endif
