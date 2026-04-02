#ifndef SCHEMA_H
#define SCHEMA_H

/* 
SCOPO: Definisce la struttura dei metadati del database:
       tipi, colonne, tabelle, viste, indici, procedure, trigger.
       Il catalogo è il "dizionario" del DB: senza di esso
       non si può interpretare i byte nelle pagine.
*/ 

#include "../../include/common.h"

// -- tipi di dato supportati --
typedef enum {
    SV_TYPE_INT,          // 4 byte, signed
    SV_TYPE_BIGINT,       // 8 byte, signed
    SV_TYPE_FLOAT,        // 8 byte (double)
    SV_TYPE_VARCHAR,      // stringa a lunghezza variabile (max size byte)
    SV_TYPE_CHAR,         // stringa a lunghezza fissa
    SV_TYPE_TEXT,         // stringa lunga (max 65535 byte)
    SV_TYPE_BOOL,         // 1 byte: 0 o 1
    SV_TYPE_DATE,         // 4 byte: giorni dal 1970-01-01
    SV_TYPE_DATETIME,     // 8 byte: millisecondi dal 1970-01-01
    SV_TYPE_BLOB,         // dati binari
    SV_TYPE_DECIMAL,      // numero decimale (precisione fissa)
} DataType;

// ritorna la dimensione in byte di un tipo (0 = variabile)
int datatype_size(DataType t);
const char* datatype_name(DataType t);

// -- definizione colonna --
typedef struct {
    char       name[SV_MAX_NAME_LEN];
    DataType type;
    int        size;           // per VARCHAR/CHAR: lunghezza max
    int        decimal_prec;   //per DECIMAL: cifre decimali
    bool       nullable;
    bool       primary_key;
    bool       auto_increment;
    bool       unique;
    bool       has_default;
    char       default_val[128];
    char       comment[256];
} Column;

// -- definizione indice --
typedef struct {
    char     name[SV_MAX_NAME_LEN];
    char     col_name[SV_MAX_NAME_LEN];
    bool     unique;
    char     type[16];         // "BTREE" o "HASH"
    uint32_t root_page_id;     // Prima pagina del B+Tree di questo indice
} Index;

// -- definizione tabella --
typedef struct {
    char      name[SV_MAX_NAME_LEN];
    Column  columns[SV_MAX_COLUMNS];
    int       num_columns;
    Index   indexes[SV_MAX_INDEXES];
    int       num_indexes;
    uint32_t  root_page_id;    // prima pagina dell'heap file
    uint32_t  num_pages;       // numero di pagine allocate
    uint64_t  num_rows;        // numero di righe (approssimativo)
    char      comment[256];
} TableSchema;

// -- definizione vista --
typedef struct {
    char name[SV_MAX_NAME_LEN];
    char definition[SV_MAX_SQL_LEN]; // Il SELECT che definisce la vista
} ViewSchema;

// ── Definizione procedura/funzione ──────────────────────────────
typedef struct {
    char name[SV_MAX_NAME_LEN];
    bool is_function;             // true = FUNCTION, false = PROCEDURE
    char body[SV_MAX_SQL_LEN];    // Corpo della procedura
    char return_type[64];         // Solo per FUNCTION
} RoutineSchema;

// ── Definizione trigger ──────────────────────────────────────────
typedef struct {
    char name[SV_MAX_NAME_LEN];
    char table_name[SV_MAX_NAME_LEN];
    bool before;              // true = BEFORE, false = AFTER
    char event[16];           // "INSERT", "UPDATE", "DELETE"
    char body[SV_MAX_SQL_LEN];
} TriggerSchema;

// -- database --
typedef struct {
    char             name[SV_MAX_NAME_LEN];
    TableSchema    tables[SV_MAX_TABLES];
    int              num_tables;
    ViewSchema     views[64];
    int              num_views;
    RoutineSchema  routines[64];
    int              num_routines;
    TriggerSchema  triggers[64];
    int              num_triggers;
} Database;

// -- catalogo globale --
typedef struct {
    Database  databases[SV_MAX_DATABASES];
    int         num_databases;
    int         current_db;      // Indice del database corrente (-1 = nessuno)
    uint32_t    next_page_id;    // Prossima page_id libera (globale)
} Catalog;

// -- API --

Catalog* catalog_create(void);
Catalog* catalog_load(const char* path);
int catalog_save(Catalog* c, const char* path);
void catalog_free(Catalog* c);

// Database
Database* catalog_get_db(Catalog* c, const char* name);
int catalog_add_db(Catalog* c, const char* name);
int catalog_drop_db(Catalog* c, const char* name);
int catalog_use_db(Catalog* c, const char* name);
Database* catalog_current_db(Catalog* c);

// Tabelle
TableSchema* catalog_get_table(Catalog* c, const char* db, const char* table);
int catalog_add_table(Catalog* c, const char* db, TableSchema* schema);
int catalog_drop_table(Catalog* c, const char* db, const char* table);

// Viste
ViewSchema* catalog_get_view(Catalog* c, const char* db, const char* name);
int catalog_add_view(Catalog* c, const char* db, ViewSchema* schema);
int catalog_drop_view(Catalog* c, const char* db, const char* name);

// Indici
int catalog_add_index(Catalog* c, const char* db, const char* table, Index* idx);

// Colonne
int catalog_get_col_index(TableSchema* t, const char* col_name);

#endif 