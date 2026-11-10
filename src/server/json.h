/*
SCOPO: JSON minimo per il server HTTP (nessuna dipendenza).
       - lettura: albero di JsonVal da un testo (oggetti, array, stringhe con escape
         \uXXXX e coppie surrogate, numeri, true/false/null)
       - scrittura: buffer crescente con escape delle stringhe
*/
#ifndef SV_JSON_H
#define SV_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct JsonVal {
    JsonType type;
    bool     boolean;
    double   number;
    char*    num_text;            // testo originale del numero (interi grandi senza perdite)
    char*    str;                 // JSON_STRING (UTF-8, può contenere \0 solo se len lo dice)
    size_t   len;                 // lunghezza di str
    struct JsonVal* items;        // JSON_ARRAY / JSON_OBJECT (valori)
    char**   keys;                // JSON_OBJECT
    int      count;
} JsonVal;

// NULL se il testo non è JSON valido (err: messaggio con la posizione)
JsonVal* json_parse(const char* text, size_t len, char* err, size_t errsz);
void     json_free(JsonVal* v);
const JsonVal* json_get(const JsonVal* obj, const char* key);   // NULL se manca

// ── scrittura ──
typedef struct {
    char*  data;
    size_t len, cap;
    bool   oom;                   // un'allocazione è fallita: il contenuto è incompleto
} JsonBuf;

void jb_raw(JsonBuf* b, const char* s, size_t n);
void jb_str(JsonBuf* b, const char* s);           // "..." con escape
void jb_strn(JsonBuf* b, const char* s, size_t n);
void jb_printf(JsonBuf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define jb_lit(b, s) jb_raw((b), (s), sizeof(s) - 1)

#endif
