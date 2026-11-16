/*
SCOPO: statistiche delle colonne per l'ottimizzatore (P1.13), raccolte da ANALYZE TABLE.
       Per ogni colonna: valori distinti, frazione di NULL e, per le colonne numeriche, minimo, massimo e un
       istogramma equi-depth (STATS_BUCKETS fasce con lo stesso numero di righe: un valore molto frequente
       occupa più fasce, così la stima di "col = valore" vede la distribuzione storta).
       Vivono in memoria e in DATA/segvault.stats (testo); una tabella cambiata (colonne diverse) le ignora
       finché non si rifà ANALYZE.
*/
#ifndef SV_STATS_H
#define SV_STATS_H

#include <stdbool.h>
#include <stdint.h>

#define STATS_BUCKETS 16

typedef struct {
    char   name[64];
    double ndv;                      // valori distinti (senza NULL)
    double null_frac;
    bool   numeric;                  // min / max / hist validi
    double min, max;
    double hist[STATS_BUCKETS + 1];  // estremi delle fasce (hist[0] = min, hist[STATS_BUCKETS] = max)
} ColStats;

typedef struct {
    char      db[64], table[64];
    double    rows;
    int       ncols;
    ColStats* cols;
} TableStats;

void              stats_put(const TableStats* t);                 // copia (sostituisce quelle della tabella)
const TableStats* stats_get(const char* db, const char* table);   // NULL se mai analizzata
const ColStats*   stats_col(const char* db, const char* table, const char* col);
void              stats_drop(const char* db, const char* table);  // table NULL = tutto il database
void              stats_clear(void);
bool              stats_save(const char* path);
bool              stats_load(const char* path);

// stime (0..1). Senza statistiche: i valori di default di System R
double stats_sel_eq(const ColStats* c, bool have_value, double value);
double stats_sel_range(const ColStats* c, int op, double value);   // op: '<', 'l' (<=), '>', 'g' (>=)

#endif
