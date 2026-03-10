/*
SCOPO: L'unico punto di contatto tra GUI e DB engine.
       La GUI chiama queste funzioni, non tocca MAI i layer
       interni del DB (heap, btree, wal, ecc.).
       Questo ti permette di cambiare il DB engine senza
       riscrivere un pixel di GUI, e viceversa.
*/

#ifndef DB_API_H
#define DB_API_H

#include <cstdint>
#include <stdbool.h>
#include <stdint.h>

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
    char value[512]; // valore come stringa -> la gui non conosce tipi
    CellType type;   // tipo della cella
    bool is_null;    // true se valore cella = NULL
} Cell;

// una riga del risultato
typedef struct {
    Cell* cells;   // puntatore alla prima Cell seguita dalle altre
    int num_cells; // numero di celle totali della riga
} Row;

// risultato di una query qualsiasi
typedef struct {
    // colonne (SELECT)
    char** col_names;    // nomi delle colonne (puntatore al puntatore della colonna singola -> array di puntatori)
    char** col_types;   // tipi delle colonne (puntatore al puntatore)
    int num_cols;       // numero colonne

    // righe (SELECT)
    Row* rows;           // puntatore alla riga
    int num_rows;

    // metadata per DML (INSERT/UPDATE/DELETE)
    int rows_affected;
    int last_insert_id; // id dell'ultima riga inserita (campo AUTO_INCREMENT)

    // esecuzione query
    double exec_time_ms; // tempo di esecuzione della query
    bool success;        // se la query è andata a buon termine
    char error[1024];    // 1024 per errori grandi, ma anche riducibile
    int error_line;      // riga del SQL dove è avvenuto l'errore
    int error_col;       // colonna del SQL dove è avvenuto l'errore
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
} ColumnInfo;

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

// inizializzazione db engine
bool db_init(const char* data_directory);

// esecuzione di qualsiasi statement
QueryResult* db_execute(const char* sql);

// esecuzione di più statement separati da ";" in sequenza, ritorna un array di QueryResult, uno per statement
QueryResult** db_execute_multi(const char* sql, int* out_count);

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
const char* db_current_database(void);

// ── utilities ──

char* db_get_create_statement(const char* db_name, const char* table); // genera lo script CREATE TABLE di una tabella (per "Copy as SQL")

void db_free_string(char* s); // libera una stringa allocata dal DB engine 

void db_namelist_free(NameList* nl); // libera una NameList

void db_shutdown(void); // shutdown

#endif