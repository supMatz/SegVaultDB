/*
SCOPO: strumenti sul database intero, senza GUI (come la ricerca e la panoramica di phpMyAdmin):
       - ricerca di un testo in tutte le colonne di tutte le tabelle: un risultato con una riga
         per ogni valore trovato (tabella, colonna, valore, chiave della riga)
       - panoramica: una riga per tabella con righe, colonne, chiave, indici, relazioni e spazio
       I risultati sono QueryResult da mostrare nella griglia (liberati con db_result_free).
       Da chiamare con il motore libero; le query usano il database corrente (USE prima).
*/
#ifndef DB_TOOLS_H
#define DB_TOOLS_H

#include "../bridge/db_api.h"

// SELECT * FROM `table` WHERE <ogni colonna> LIKE '%needle%' ESCAPE '!' (% e _ del testo valgono per sé;
// malloc'd; NULL se la tabella non esiste)
char* dbt_search_select(const char* db, const char* table, const char* needle);

// ricerca senza distinzione tra maiuscole e minuscole; limit = valori al massimo (0 = 1000).
// tables_hit: tabelle con almeno un valore trovato; truncated: superato il limite
QueryResult* dbt_search(const char* db, const char* needle, int limit, int* tables_hit, bool* truncated);

QueryResult* dbt_overview(const char* db);

#endif
