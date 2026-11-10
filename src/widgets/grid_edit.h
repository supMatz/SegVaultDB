/*
SCOPO: modifica dei dati nella griglia dei risultati (come phpMyAdmin / DBeaver).
       Da un risultato (QueryResult con l'origine delle colonne) capisce cosa si può
       modificare e genera l'SQL: UPDATE / INSERT / DELETE identificando le righe con la
       chiave primaria, e la SELECT che rilegge una riga dopo la modifica.
       Nessuna dipendenza dalla finestra: l'SQL viene eseguito da chi chiama.
*/
#ifndef GRID_EDIT_H
#define GRID_EDIT_H

#include "../bridge/db_api.h"
#include <stdbool.h>
#include <stddef.h>

#define GE_MAX_TABLES 8
#define GE_MAX_KEY    8

typedef struct {
    char name[64];
    int  key[GE_MAX_KEY];        // colonne del risultato che formano la chiave primaria
    int  nkey;
    bool key_ok;                 // la chiave primaria è tutta nel risultato
    int  auto_col;               // colonna del risultato AUTO_INCREMENT (-1 = nessuna)
} GeTable;

typedef struct {
    const QueryResult* r;
    char    db[64];
    GeTable t[GE_MAX_TABLES];
    int     nt;
    int     col_table[64];       // tabella di ogni colonna del risultato (-1 = calcolata)
    char    col_type[64][64];    // tipo SQL della colonna di origine ("INT", "VARCHAR(20)", ...)
    bool    col_nullable[64];
    char    col_default[64][128];
    int     row_table;           // l'unica tabella del risultato (-1 = più tabelle o nessuna)
    char    why[160];            // perché non si può modificare (vuoto se si può)
} GridEdit;

// analizza il risultato (legge lo schema con db_list_columns: da chiamare con il motore libero)
void grid_edit_analyze(GridEdit* ge, const QueryResult* r);
bool grid_edit_any(const GridEdit* ge);                  // almeno una cella modificabile
bool grid_edit_cell_ok(const GridEdit* ge, int col);     // la colonna si può modificare
bool grid_edit_rows_ok(const GridEdit* ge);              // si possono inserire / eliminare righe

// SQL generato (malloc'd, NULL + messaggio in err se non si può)
//   value: testo scritto dall'utente; set_null = imposta NULL
char* grid_edit_update_sql(const GridEdit* ge, const Row* row, int col, const char* value, bool set_null,
                           char* err, size_t errn);
char* grid_edit_delete_sql(const GridEdit* ge, const Row* row, char* err, size_t errn);
//   insert: values[c] / nulls[c] per le colonne con set[c] == true; le altre prendono il DEFAULT
char* grid_edit_insert_sql(const GridEdit* ge, const char* const* values, const bool* nulls, const bool* set,
                           char* err, size_t errn);
// SELECT che rilegge la riga (le colonne del risultato che vengono dalla tabella t) identificata
// dai valori di chiave key_vals (letterali SQL, nell'ordine di ge->t[t].key)
char* grid_edit_fetch_sql(const GridEdit* ge, int t, const char* const* key_vals);

// letterale SQL del valore scritto per la colonna (numero, booleano o testo tra apici)
char* grid_edit_literal(const GridEdit* ge, int col, const char* value);
// letterale SQL del valore di una cella (per le chiavi)
char* grid_edit_cell_literal(const Cell* c);

#endif
