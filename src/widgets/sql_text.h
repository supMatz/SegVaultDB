/*
SCOPO: utilità testuali SQL lato GUI (nessuna dipendenza dal motore DB).
       - divisione di uno script in statement separati da ';'
         ignorando i ';' dentro stringhe e commenti
       - individuazione dello statement sotto il cursore
       - riconoscimento keyword/tipi/funzioni per il syntax highlighting
*/

#ifndef SQL_TEXT_H
#define SQL_TEXT_H

#include <stdbool.h>

typedef struct {
    int start;       // primo byte dello statement (subito dopo il ';' precedente)
    int code_start;  // primo byte che non è spazio o commento
    int end;         // byte del ';' finale (o fine testo) — escluso
    int end_semi;    // byte dopo il ';' (== end se manca il ';')
    bool has_code;   // false se contiene solo spazi/commenti
} SqlRange;

// divide il testo in statement; ritorna quanti ne ha scritti in out (max max_out)
int  sql_split(const char* s, int len, SqlRange* out, int max_out);

// statement che contiene la posizione pos; false se non c'è codice
bool sql_statement_at(const char* s, int len, int pos, SqlRange* out);

// classificazione parole per l'highlighting
typedef enum { SQL_WORD_IDENT, SQL_WORD_KEYWORD, SQL_WORD_TYPE, SQL_WORD_FUNC, SQL_WORD_LITERAL } SqlWordKind;
SqlWordKind sql_classify_word(const char* w, int len);

#endif
