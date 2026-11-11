/*
SCOPO: modello del diagramma dello schema (senza GUI): tabelle con le colonne, relazioni dalle
       chiavi esterne, posizioni dei riquadri (salvate su file per database), disposizione
       automatica e l'SQL delle azioni fatte sul diagramma (relazioni, eliminazioni).
*/
#ifndef SCHEMA_MODEL_H
#define SCHEMA_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#define SM_MAX_COLS 64

typedef struct {
    char name[64];
    char type[64];             // "INT", "VARCHAR(100)"
    bool pk, notnull, unique, autoinc;
    bool fk;                   // fa parte di una chiave esterna
} SmCol;

typedef struct {
    char  name[64];
    int   ncols;
    SmCol cols[SM_MAX_COLS];
    int   x, y, w, h;          // coordinate del diagramma (w, h calcolati da chi disegna)
    bool  placed;              // posizione letta dal file o scelta dall'utente
} SmTable;

typedef struct {
    char name[64];
    int  child, parent;        // indici delle tabelle (figlia = quella con la chiave esterna)
    int  ncols;
    char cols[8][64];
    bool optional;             // una colonna della chiave ammette NULL: (0,1) invece di (1,1)
    bool one_to_one;           // la chiave esterna è UNIQUE o è la chiave primaria della figlia
    char on_delete[16];
} SmRel;

typedef struct {
    char     db[64];
    SmTable* t;
    int      nt;
    SmRel*   r;
    int      nr;
} SchemaModel;

bool sm_load(SchemaModel* m, const char* db);   // dal catalogo; false se il database non esiste
void sm_free(SchemaModel* m);
int  sm_find(const SchemaModel* m, const char* table);   // -1 se non c'è
int  sm_col(const SmTable* t, const char* col);          // -1 se non c'è

// posizioni: file di testo "tabella x y" (le tabelle senza riga restano placed = false)
bool sm_load_positions(SchemaModel* m, const char* path);
bool sm_save_positions(const SchemaModel* m, const char* path);

// dispone le tabelle non ancora posizionate: genitori a sinistra, figlie a destra (w, h già noti)
void sm_auto_layout(SchemaModel* m, bool all, int gap_x, int gap_y);

// ── SQL (malloc'd; NULL + messaggio in err se l'azione non è possibile) ──
// relazione: col = colonna della figlia ("" = ne crea una nuova con il tipo della chiave del genitore)
char* sm_relation_sql(const SchemaModel* m, int child, const char* col, int parent, char* err, size_t errn);
char* sm_drop_relation_sql(const SchemaModel* m, int rel);
char* sm_relation_required_sql(const SchemaModel* m, int rel, bool required);   // SET / DROP NOT NULL
char* sm_drop_table_sql(const SchemaModel* m, int table);

#endif
