/*
SCOPO: funzioni JSON sui testi (come MySQL): validazione, estrazione con un percorso
       ('$.a.b', '$.lista[2]', '$."con spazi"'), tipo, lunghezza, togliere le virgolette.
       Lavorano su testo UTF-8; nessuna allocazione tranne dove indicato.
*/
#ifndef SV_SQLJSON_H
#define SV_SQLJSON_H

#include <stdbool.h>
#include <stddef.h>

bool        sj_valid(const char* doc);
// valore del percorso dentro doc: [*start, *start + *len) (testo JSON del valore); false se manca
// o se il percorso / il documento non è valido
bool        sj_extract(const char* doc, const char* path, const char** start, size_t* len);
const char* sj_type(const char* value, size_t len);       // OBJECT, ARRAY, STRING, INTEGER, DOUBLE, BOOLEAN, NULL
int         sj_length(const char* value, size_t len);     // elementi / membri (1 per un valore semplice), -1 se invalido
// testo senza virgolette e con gli escape risolti (malloc'd); se non è una stringa JSON, copia
char*       sj_unquote(const char* value, size_t len);
// stringa JSON (con virgolette ed escape) di un testo (malloc'd)
char*       sj_quote(const char* s);

// modifica: op 0 SET, 1 INSERT, 2 REPLACE, 3 REMOVE, 4 ARRAY_APPEND (val: testo JSON). malloc'd; NULL se non valido
char*       sj_modify(const char* doc, const char* path, const char* val, int op);
char*       sj_keys(const char* value, size_t len);                 // ["a", "b"] di un oggetto, NULL altrimenti
bool        sj_contains(const char* target, size_t tl, const char* cand, size_t cl);
int         sj_depth(const char* value);
char*       sj_pretty(const char* doc);
char*       sj_search(const char* doc, bool all, const char* pattern);   // NULL se nessuna corrispondenza
char*       sj_merge_patch(const char* a, const char* patch);

#endif
