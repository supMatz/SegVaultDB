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

#endif
