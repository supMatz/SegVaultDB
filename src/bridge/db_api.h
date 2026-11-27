/*
SCOPO: L'unico punto di contatto tra GUI e DB engine.
       La GUI chiama queste funzioni, non tocca MAI i layer
       interni del DB (heap, btree, wal, ecc.).
       Questo ti permette di cambiare il DB engine senza
       riscrivere un pixel di GUI, e viceversa.
*/

#ifndef DB_API_H
#define DB_API_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// -- strutture risultato --

// Tipo della cella: la GUI lo usa per formattare il valore
typedef enum {
    CELL_TYPE_NULL,
    CELL_TYPE_INT,
    CELL_TYPE_FLOAT,
    CELL_TYPE_STRING,
    CELL_TYPE_BOOL,
    CELL_TYPE_DATE,
    CELL_TYPE_BLOB,
} CellType;

// singola cella
typedef struct {
    char* value;     // valore come stringa (mai NULL: "NULL" se is_null) -> la gui non conosce tipi.
                     // Il testo sta nello stesso blocco di memoria delle celle della riga (vedi db_row_set)
    CellType type;   // tipo della cella
    bool is_null;    // true se valore cella = NULL
} Cell;

// una riga del risultato
typedef struct {
    Cell* cells;   // un solo blocco: le celle seguite dai loro testi -> free(cells) libera tutto
    int num_cells; // numero di celle totali della riga
} Row;

// Riempie una riga: alloca in un solo blocco le ncols celle e i loro testi.
// vals[i] == NULL -> cella NULL; types == NULL -> tutte CELL_TYPE_STRING.
static inline bool db_row_set(Row* row, int ncols, const char* const* vals, const CellType* types) {
    size_t total = 0;
    for (int i = 0; i < ncols; i++) total += strlen(vals[i] ? vals[i] : "NULL") + 1;
    char* block = (char*)malloc(sizeof(Cell) * (size_t)(ncols > 0 ? ncols : 1) + total);
    row->cells = (Cell*)block;
    row->num_cells = block ? ncols : 0;
    if (!block) return false;
    char* p = block + sizeof(Cell) * (size_t)(ncols > 0 ? ncols : 1);
    for (int i = 0; i < ncols; i++) {
        Cell* c = &row->cells[i];
        size_t len = strlen(vals[i] ? vals[i] : "NULL");
        memcpy(p, vals[i] ? vals[i] : "NULL", len + 1);
        c->value = p;
        p += len + 1;
        c->is_null = vals[i] == NULL;
        c->type = c->is_null ? CELL_TYPE_NULL : types ? types[i] : CELL_TYPE_STRING;
    }
    return true;
}

// risultato di una query qualsiasi
typedef struct {
    // colonne (SELECT)
    char** col_names;    // nomi delle colonne (puntatore al puntatore della colonna singola -> array di puntatori)
    char** col_types;   // tipi delle colonne (puntatore al puntatore)
    int num_cols;       // numero colonne
    // origine delle colonne (griglia modificabile): tabella e colonna di base, NULL per le colonne
    // calcolate; presenti solo se ogni riga del risultato è una riga reale (niente GROUP BY,
    // DISTINCT, UNION, viste), altrimenti col_tables == NULL
    char** col_tables;
    char** col_columns;
    char   source_db[64];   // database delle tabelle di origine
    char*  sql;             // statement che ha prodotto il risultato (impostato da chi esegue; liberato con il risultato)

    // righe (SELECT)
    Row* rows;           // puntatore alla riga
    int num_rows;

    // metadata per DML (INSERT/UPDATE/DELETE)
    long long rows_affected;
    long long last_insert_id; // id dell'ultima riga inserita (campo AUTO_INCREMENT); 64 bit: oltre 2^31 non si tronca

    // esecuzione query
    double exec_time_ms; // tempo di esecuzione della query
    bool success;        // se la query è andata a buon termine
    char error[1024];    // 1024 per errori grandi, ma anche riducibile
    int error_line;      // riga del SQL dove è avvenuto l'errore
    int error_col;       // colonna del SQL dove è avvenuto l'errore
    // righe già consegnate a blocchi (db_execute_stream, P1.14): num_rows = 0, streamed_rows = quante
    bool streamed;
    long long streamed_rows;
} QueryResult;

// lista di nomi generica (per sidebar)
typedef struct {
    char** names;
    char** extras;          // info aggiuntiva (es: tipo colonna, engine tabella)
    int    count;
} NameList;

// -- piano di esecuzione (EXPLAIN)--

typedef struct {
    char operation[64];     // tipo di operazione (seqScan, indexScan, nestedLoop, ecc. ..)
    char target[64];        // nome tabella/indice coinvolto
    char condition[64];     // condizione applicata (WHERE, ON, ecc. ...)
    int estimated_rows;     // righe che ci si aspettano di ritorno
    double estimated_cost;  // costo stimato
    int depth;              // profondità del piano -> per indentare visualizzazione
} ExplainNode;

typedef struct {
    ExplainNode* nodes;     // nodi di EXPLAIN 
    int count;              // numero di nodi
} ExplainResult;

// ── informazioni su oggetti DB ──

// informazioni su una colonna (per DESCRIBE / SHOW COLUMNS)
typedef struct {
    char name[64];
    char type[64];          // INT, VARCHAR(255), ecc.
    bool nullable;
    bool primary_key;
    bool auto_increment;
    char default_val[128];  // valore di default (NULL, 0, ciao)
    char extra[128];        // extra info (auto_increment, ecc.)
    bool unique;            // chiave UNIQUE su questa sola colonna
    char unique_name[64];   // nome del vincolo UNIQUE ("" se è un attributo della colonna senza nome)
    bool indexed;           // ha un indice (CREATE INDEX) su questa sola colonna
    char index_name[64];
} ColumnInfo;

// chiave esterna (per il diagramma dello schema)
typedef struct {
    char name[64];
    char table[64];            // tabella con la chiave esterna (figlia)
    char ref_table[64];        // tabella riferita (genitore)
    int  ncols;
    char cols[16][64];         // colonne della figlia (SV_MAX_KEY_COLS)
    char ref_cols[16][64];     // colonne riferite ("" = la chiave primaria del genitore)
    char on_delete[16];        // "NO ACTION", "CASCADE", ...
    char on_update[16];
} ForeignKeyInfo;

// informazioni su una tabella
typedef struct {
    char     name[64];
    char     engine[32];    // MySQL, SQLite, SegVaultDB, ecc. ...
    uint64_t  num_rows;
    uint64_t size_bytes;    // dimensione su disco
    char     collation[64];
    char     comment[256];
} TableInfo;

typedef struct {
    char name[64];
    char column[64];
    bool unique;
    char type[32]; // BTREE o HASH
} IndexInfo;

// info su stored procedure o function
typedef struct {
    char name[64];
    char type[64]; // procedure o function
    char returns[64]; // ovviamente solo per le function
    char body[4096]; // corpo della routine
    char created_at[32]; // data creazione (superflua ma la metto ok)
} RoutineInfo;

// ── stato della connessione / sessione al DB ──
typedef struct {
    char current_db[64];     // db selezionato
    uint64_t tx_id;          // id transazione attiva (query in esecuzione, 0 = nessuna)
    bool in_transaction;     // se in transazione, = true
    bool auto_commit;        // se ogni statement è una transazione, = true
    char server_version[32]; // versione di questo DB engine
    uint64_t uptime_seconds;         // da quanto è up il DB 
} SessionInfo;

// ── API principale ──
// Stack: i thread che chiamano db_execute* devono avere almeno 512 KB di stack (verificato con le query più
// annidate che il motore accetta); il server usa 8 MB, la GUI 16 MB. Con 256 KB una query annidata può
// esaurire lo stack prima che il motore la rifiuti (CR-M1).

// inizializzazione db engine
bool db_init(const char* data_directory);
const char* db_init_error(void);   // motivo dell'ultimo db_init fallito

// SELECT con le righe consegnate a blocchi mentre si calcolano (P1.14): per una SELECT su una tabella sola senza
// ORDER BY / GROUP BY / DISTINCT / LIMIT / finestre la memoria resta quella di un blocco (2 048 righe lette). begin
// riceve il primo blocco (colonne), rows ogni blocco; false = interrompere. Ritorna il QueryResult di db_execute:
// streamed = true (righe già consegnate, streamed_rows quante) oppure il risultato normale (righe in r->rows)
typedef struct {
    void* ud;
    bool (*begin)(void* ud, const QueryResult* first_block);
    bool (*rows)(void* ud, const QueryResult* block);
} DbRowSink;
QueryResult* db_execute_stream(const char* sql, DbRowSink* sink);

// esecuzione di qualsiasi statement
QueryResult* db_execute(const char* sql);

// esecuzione di più statement separati da ";" in sequenza, ritorna un array di QueryResult, uno per statement
QueryResult** db_execute_multi(const char* sql, int* out_count);

// ── annullamento (chiamabile da un altro thread mentre db_execute è in corso) ──
// Le SELECT vengono interrotte subito; gli statement che modificano dati non vengono
// mai interrotti a metà: l'annullamento vale per gli statement successivi.
void db_request_cancel(void);
void db_clear_cancel(void);
bool db_cancel_requested(void);

// liberazione mem
void db_result_free(QueryResult* r); // statement singolo / query singola
void db_result_free_multi(QueryResult** results, int count); // statement multiplo / query multipla

// ── EXPLAIN ──
ExplainResult* db_explain(const char* sql);
void db_explain_free(ExplainResult* r);

// -- API sidebar : databases --
NameList* db_list_procedure(const char* db_name);
NameList* db_list_functions(const char* db_name);
RoutineInfo* db_get_routine(const char* db_name, const char* name, const char* type); // type per procedure o function
bool db_drop_routine(const char* db_name, const char* name, const char* type);
void db_routine_free(RoutineInfo* r);

// ── API sidebar: trigger ───
NameList* db_list_triggers(const char* db_name, const char* table);
bool      db_drop_trigger(const char* db_name, const char* name);

// ── transazioni (chiamate esplicite dalla GUI) ──
bool db_begin(void);
bool db_commit(void);
bool db_rollback(void);
bool db_savepoint(const char* name);
bool db_rollback_to(const char* name);

// ── sessione ──
SessionInfo* db_session_info(void); // void, perchè la funzione non accetta parametri, se lascio () ne accetta indeterminati -> bad practise
void db_session_free(SessionInfo* s);
const char* db_current_database(void);   // copia del thread (4 buffer a rotazione), NULL se nessun database

// ── utilities ──

char* db_get_create_statement(const char* db_name, const char* table); // genera lo script CREATE TABLE di una tabella (per "Copy as SQL")
char* db_get_create_statement_nofk(const char* db_name, const char* table); // CREATE TABLE senza le FOREIGN KEY (dump)
char* db_get_foreign_keys(const char* db_name, const char* table);         // ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY (dump)
// 0 = CREATE VIEW, 1 = CREATE TRIGGER, 2 = CREATE PROCEDURE di tutto il database (dump); da liberare con db_free_string
char* db_get_objects_sql(const char* db_name, int what);
// copia coerente di tutti i dati in dir (cartella nuova o vuota); anche con l'istruzione BACKUP TO 'dir'
bool db_backup(const char* dir, char* err, size_t errn);
// cartella (con le sottocartelle) che lo statement SQL BACKUP TO non può usare: la registra il server web per
// la sua radice e per i siti (on = false la toglie). Il backup da db_backup non ha questo limite (CR-B6)
void db_backup_protect(const char* dir, bool on);
// sostituisce i dati con quelli di un backup; i dati di prima restano in <dati>/before-restore
bool db_restore(const char* dir, char* err, size_t errn);

void db_free_string(char* s); // libera una stringa allocata dal DB engine 

void db_namelist_free(NameList* nl); // libera una NameList

void db_shutdown(void); // shutdown

NameList* db_list_databases(void);

NameList* db_list_tables(const char* db_name);

NameList* db_list_views(const char* db_name);

ColumnInfo* db_list_columns(const char* db_name, const char* table, int* out_count);

void db_columns_free(ColumnInfo* cols, int count);

// ── sessioni (client del server con stato proprio) ──
// Ogni sessione ha database corrente, utente, transazione e savepoint propri. Una sola sessione
// alla volta può avere una transazione aperta: finché non fa COMMIT / ROLLBACK le altre non entrano
// (non vedono dati non confermati e un ROLLBACK non tocca il lavoro altrui).
// La sessione predefinita (NULL) è quella della GUI e delle richieste senza sessione.
typedef struct DbSession DbSession;
DbSession* db_session_new(void);
// sessione per una sola richiesta (HTTP senza sessione): parte dal database corrente della sessione
// predefinita; db_session_end alla fine annulla una transazione lasciata aperta
DbSession* db_session_new_temp(void);
// Più transazioni possono essere aperte insieme (sessioni diverse): ogni statement prende lock S / X
// sulle tabelle che usa (vedi locks.h) e li tiene fino a COMMIT / ROLLBACK. Chi trova una tabella
// occupata aspetta in coda (FIFO): esce dal motore, chiama leave(ud) (il server lascia il suo turno),
// dorme finché un lock viene rilasciato (nessun polling), poi enter(ud). Hook del thread, NULL = nessuno.
// Dopo 30 s: "Lock wait timeout exceeded"; un ciclo di attese: "Deadlock found" e la transazione viene
// annullata (come MySQL); il flag di annullamento del thread (db_set_thread_cancel) ferma l'attesa.
void db_set_lock_wait_hooks(void (*leave)(void* ud), void (*enter)(void* ud), void* ud);
// db_session_enter è fallita (un'altra sessione è dentro il motore): db_session_seq prima di provare,
// poi db_session_wait dorme finché una sessione esce (o max_ms)
unsigned long long db_session_seq(void);
void db_session_wait(unsigned long long seq, int max_ms);
void db_set_lock_wait_timeout(int ms);         // attesa massima di un lock (default 30000)
// rende attiva s (NULL = una richiesta senza sessione); false se un'altra sessione ha una transazione
// aperta (riprovare più tardi)
bool db_session_enter(DbSession* s);
void db_session_leave(DbSession* s);            // salva lo stato di s e torna alla sessione predefinita
void db_session_end(DbSession* s);              // annulla la sua transazione e la libera (non attiva)
bool db_session_in_transaction(const DbSession* s);
bool db_session_blocked(void);                  // una sessione ha una transazione aperta

// ── letture in parallelo ──
// Esegue una sola SELECT accanto ad altre letture (lock condiviso): dbname = database (NULL / "" =
// quello corrente), user = permessi (NULL / "" = root), timeout_ms = limite (0 = nessuno).
// *refused = true (e NULL) se non si può fare in parallelo: non è una SELECT, una transazione è
// aperta, il database non esiste, ...: allora va eseguita con db_execute.
// flag MySQL (NOT_NULL, PRI_KEY, UNIQUE_KEY, MULTIPLE_KEY, UNSIGNED, AUTO_INCREMENT) di una colonna di tabella
unsigned db_column_flags(const char* db, const char* table, const char* column);
// true se l'SQL contiene solo letture senza effetti (SELECT senza INTO / funzioni dell'utente, SHOW, DESCRIBE)
bool db_statement_is_read(const char* sql);
QueryResult* db_execute_readonly(const char* sql, const char* dbname, const char* user, int timeout_ms, bool* refused);

// modalità MySQL per il thread corrente (connessioni del protocollo MySQL): regole permissive di MySQL
void db_set_mysql_mode(bool on);

// ── statistiche del motore (pagina Monitor del server; come SHOW STATUS, senza sessione) ──
typedef struct {
    int       page_size;
    long long db_pages;            // pagine del file dei dati
    int       pool_frames, pool_used;
    unsigned long long pool_requests, pool_hits, pool_misses, pool_evictions, pages_written;
    long long wal_bytes;
    unsigned long long wal_records, checkpoints;
    int       databases, tables;
    bool      in_transaction;
    int       open_transactions;   // transazioni aperte in tutte le sessioni
    unsigned long long lock_waits, deadlocks;
} DbEngineStats;
bool db_engine_stats(DbEngineStats* out);

// ── utenti e permessi (auth.c) ──
// SQL: CREATE USER u IDENTIFIED BY 'pw' | ALTER USER ... | DROP USER u |
//      GRANT ALL | SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, ALTER ON *.* | db.* | db.t | t TO u [WITH GRANT OPTION] |
//      REVOKE ... FROM u | SHOW USERS | SHOW GRANTS [FOR u]
#define DB_PRIV_SELECT 0x01u
#define DB_PRIV_INSERT 0x02u
#define DB_PRIV_UPDATE 0x04u
#define DB_PRIV_DELETE 0x08u
#define DB_PRIV_CREATE 0x10u
#define DB_PRIV_DROP   0x20u
#define DB_PRIV_ALTER  0x40u
#define DB_PRIV_ALL    0x7Fu
typedef struct { char name[64]; bool admin; } DbUserInfo;
bool        db_set_user(const char* name);        // utente della sessione: NULL / "" = root (tutto permesso)
const char* db_session_user(void);
bool        db_login(const char* name, const char* password);   // credenziali giuste?
// l'utente ha qualche permesso nel database (table NULL) o sulla tabella? "" = root, sempre
// login col protocollo MySQL: 1 = ok, 0 = no, -1 = password da reimpostare (utente creato prima)
int         db_login_native(const char* user, const uint8_t scramble[20], const uint8_t* resp, size_t rlen);
// prepared statement analizzati una volta (P2.2): NULL se lo statement non si presta (il chiamante usa il
// testo con i valori). exec: lits[i] = valore del '?' numero i+1 come letterale SQL ("12", "'x'", "NULL");
// NULL = usare il testo (schema cambiato in modo che lo statement non si analizza più, valore non letterale)
typedef struct DbPrepared DbPrepared;
DbPrepared*  db_prepared_new(const char* sql);
QueryResult* db_prepared_exec(DbPrepared* p, const char* const* lits, int n);
int          db_prepared_params(const DbPrepared* p);
void         db_prepared_free(DbPrepared* p);
void         db_prepared_counts(unsigned long long* runs, unsigned long long* parses);   // per i test

// caching_sha2_password, accesso veloce: 1 = giusto, 0 = sbagliato, -2 = serve la password in chiaro (db_login)
int         db_login_sha2(const char* user, const uint8_t nonce[20], const uint8_t* resp, size_t rlen);
bool        db_user_has_native(const char* user);   // l'utente ha il verificatore mysql_native_password?
bool        db_user_can_see(const char* user, const char* db, const char* table);
bool        db_has_users(void);
// durabilità delle conferme: "OFF", "NORMAL" (predefinito), "FULL" (anche SET [GLOBAL] synchronous = ...).
// db_sync_tick: da chiamare periodicamente (il server lo fa) perché in NORMAL le ultime scritture in
// autocommit arrivino su disco entro 100 ms anche senza statement successivi
bool        db_set_synchronous(const char* mode);
// cache delle pagine in MB: da chiamare prima di db_init (0 = automatica: 1/8 della RAM, 4-512 MB);
// a motore avviato SET GLOBAL cache_size = MB. db_cache_size: quella in uso
void        db_set_cache_size(long mb);
long        db_cache_size(void);
const char* db_synchronous(void);
void        db_sync_tick(void);
// byte di log oltre i quali parte un checkpoint (default 16 MB; per i test e i carichi grandi)
void        db_set_checkpoint_bytes(long long bytes);
// flag di annullamento della connessione di questo thread (KILL QUERY): != 0 ferma lo statement in corso
void        db_set_thread_cancel(atomic_int* flag);
// colonne di una SELECT senza eseguirla (nessuna riga, nessun effetto): NULL se sql non è una SELECT sola
QueryResult* db_describe(const char* sql);
// Cell.value è testo; un valore binario ha i byte 0x00 e 0xC0 nella forma interna C0 80 / C0 81
// (src/table/binesc.h). db_value_bytes scrive in out (strlen(v) byte bastano) i byte veri e ne dà il numero
size_t      db_value_bytes(const char* v, char* out);
unsigned long db_users_version(void);   // cambia a ogni modifica di utenti o permessi (cache dei login)
// endpoint salvato dell'API (riga di api_endpoints nel database db): con quali permessi gira.
// definer = valore della colonna definer (NULL se la colonna manca o è NULL). true: user_out è
// l'utente ("" = tutti i permessi) e *may_public dice se access = 'public' vale (con gli utenti attivi
// solo per un amministratore); false: rifiutato, motivo in err
bool        db_endpoint_runas(const char* db, const char* definer, char* user_out, size_t un, bool* may_public,
                              char* err, size_t errn);
int         db_list_users(DbUserInfo* out, int max);
unsigned    db_user_grant(const char* user, const char* db, const char* table);   // permessi dati proprio lì
int         db_user_table_grants(const char* user, char (*db)[64], char (*table)[64], unsigned* privs, int max);

// tutte le chiavi esterne delle tabelle di un database (NULL se il database non esiste o è vuoto)
ForeignKeyInfo* db_list_foreign_keys(const char* db_name, int* out_count);
// righe (stima del catalogo), dimensione su disco e numero di indici di una tabella; false se non esiste
bool db_table_info(const char* db_name, const char* table, TableInfo* out, int* num_indexes);
void db_foreign_keys_free(ForeignKeyInfo* fks);

#endif