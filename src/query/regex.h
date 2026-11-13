/*
SCOPO: espressioni regolari per REGEXP / RLIKE / REGEXP_LIKE / REGEXP_REPLACE / REGEXP_SUBSTR /
       REGEXP_INSTR (sintassi estesa POSIX, quella di MySQL), senza librerie esterne (regex.h non
       c'è su Windows).
       ^ $ . [abc] [^a-z] [[:alpha:]] \d \w \s * + ? {m} {m,} {m,n} (gruppi) a|b, e i caratteri protetti
       con \. Confronto senza distinzione fra maiuscole e minuscole (come le collation _ci di MySQL),
       salvo icase = false. Lavora sui byte: i caratteri UTF-8 multi-byte valgono come sequenze letterali.
*/
#ifndef SV_REGEX_H
#define SV_REGEX_H

#include <stdbool.h>
#include <stddef.h>

typedef struct SvRegex SvRegex;

// NULL + messaggio in err se il pattern non è valido
SvRegex* sv_regex_compile(const char* pattern, bool icase, char* err, size_t errn);
void     sv_regex_free(SvRegex* re);

// prima corrispondenza in s a partire da from: true e [*start, *end) se trovata
bool     sv_regex_search(const SvRegex* re, const char* s, size_t from, size_t* start, size_t* end);
bool     sv_regex_match(const SvRegex* re, const char* s);     // c'è una corrispondenza in s?
// come sv_regex_search, con i gruppi: caps[0] = tutta la corrispondenza, caps[i] = gruppo i
// ((size_t)-1 se il gruppo non ha partecipato); ncaps = posti in caps (gruppi oltre: non registrati)
bool     sv_regex_search_caps(const SvRegex* re, const char* s, size_t from, size_t caps[][2], int ncaps);

#endif
