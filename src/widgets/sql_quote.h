/*
SCOPO: nomi negli SQL che la GUI scrive da sé (anteprime, GRANT, DROP, dump, frammenti nell'editor).
       Un nome scelto da un altro utente ("x; DROP DATABASE y; --" è legale tra backtick, come in MySQL)
       non deve mai diventare SQL: va sempre tra backtick. Il motore rifiuta i nomi nuovi con backtick o
       caratteri di controllo (sv_name_ok), quindi `nome` è sempre un solo identificatore; un oggetto più
       vecchio con un nome così non si può scrivere in SQL e la GUI lo dice invece di provarci.
*/
#ifndef SQL_QUOTE_H
#define SQL_QUOTE_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../../include/common.h"

// `name` in out; false (out = "") se il nome non si può mettere tra backtick o non entra in n byte
static inline bool sql_ident(char* out, size_t n, const char* name) {
    if (n) out[0] = '\0';
    if (!sv_name_ok(name) || strlen(name) + 3 > n) return false;
    snprintf(out, n, "`%s`", name);
    return true;
}

#define SQL_IDENT_MAX (SV_MAX_NAME_LEN + 3)

#include "../../include/sv_strbuf.h"
// `name` in fondo a b; un nome che non si può mettere tra backtick rende il buffer non valido (oom): l'SQL non
// viene eseguito invece di contenere un nome spezzato (CR-B2: prima tre copie senza controllo)
static inline void sql_ident_sb(SvStrBuf* b, const char* name) {
    if (!sv_name_ok(name)) { b->oom = true; return; }
    svsb_putc(b, '`');
    svsb_puts(b, name);
    svsb_putc(b, '`');
}

#endif
