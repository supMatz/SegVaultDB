/*
SCOPO: modello del designer di tabelle (senza GUI): la struttura di una tabella come la si
       modifica nel modulo, il caricamento dal catalogo e l'SQL che la realizza
       (CREATE TABLE per una tabella nuova, ALTER TABLE per le modifiche).
*/
#ifndef TABLE_DESIGN_H
#define TABLE_DESIGN_H

#include <stdbool.h>
#include <stddef.h>

#define DESIGN_MAX_COLS 64

typedef struct {
    int  orig;               // colonna originale (indice in DesignTable dell'originale), -1 = nuova
    char name[64];
    int  type;               // indice in design_types()
    char size[16];           // VARCHAR / CHAR: lunghezza
    char def[128];           // DEFAULT come scritto ("" = nessuno, "NULL" = DEFAULT NULL)
    bool pk, notnull, unique, autoinc;
    char unique_name[64];    // nome del vincolo UNIQUE esistente (per eliminarlo)
} DesignCol;

typedef struct {
    char      name[64];
    int       ncols;
    DesignCol cols[DESIGN_MAX_COLS];
} DesignTable;

const char* const* design_types(int* n);       // INT, BIGINT, FLOAT, DECIMAL, VARCHAR, CHAR, TEXT, BOOL, DATE, DATETIME, BLOB
bool design_type_sized(int type);              // VARCHAR / CHAR: serve la lunghezza
int  design_type_index(const char* name);      // -1 se sconosciuto

// struttura della tabella dal catalogo (orig = posizione); false se non esiste
bool design_load(DesignTable* t, const char* db, const char* table);
void design_new(DesignTable* t);                // tabella nuova con la colonna id INT PRIMARY KEY AUTO_INCREMENT
void design_add_column(DesignTable* t);

// SQL che porta da orig (NULL = tabella nuova) a cur; malloc'd, statement separati da ";\n".
// NULL + messaggio in err se la struttura non è valida.
char* design_sql(const DesignTable* orig, const DesignTable* cur, char* err, size_t errn);

#endif
