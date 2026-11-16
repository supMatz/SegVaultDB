/*
SCOPO: Politica unica per le allocazioni del motore (src/catalog, storage, table, index, query, tx,
       bridge): ogni allocazione passa da sv_malloc / sv_calloc / sv_realloc / sv_strdup / sv_strndup.

REGOLA: memoria esaurita = arresto controllato. Se il sistema non concede la memoria, sv_oom()
       scrive "out of memory" su stderr e chiama abort(): nessun puntatore NULL arriva al codice
       che lo userebbe (niente crash a caso, niente dati corrotti in RAM scritti su disco). I dati
       confermati sono già nel WAL (COMMIT ha fatto fsync secondo `synchronous`), quindi al riavvio
       il recupero li riporta e annulla le transazioni a metà: lo verifica lo sweep di
       tests/test_durability.c, che fa fallire l'N-esima allocazione per N = 1…k.
       Perché non un percorso d'errore per ogni allocazione: sono più di 400 punti, e un ramo
       d'errore mai eseguito è quello che corrompe; questa regola è la stessa di Redis e di molti
       motori embedded.

ECCEZIONE: le richieste la cui dimensione viene dai dati e può essere irragionevole (un valore di
       16 MB, il buffer del recupero) usano sv_try_malloc / sv_try_realloc, che restituiscono NULL:
       chi le chiama DEVE gestire il NULL e fallire l'istruzione con un errore.
*/
#ifndef SV_ALLOC_H
#define SV_ALLOC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SV_FAULT_INJECTION
// build di prova: l'N-esima allocazione fallisce (tests/test_durability.c, sweep delle allocazioni)
int sv_fi_alloc_fail(void);
#define SV_ALLOC_FAULT() sv_fi_alloc_fail()
#else
#define SV_ALLOC_FAULT() 0
#endif

#if defined(__GNUC__)
__attribute__((noreturn, cold))
#endif
static inline void sv_oom(size_t n) {
    fprintf(stderr, "[SegVault] out of memory (%llu bytes): stopping; committed data is recovered from the log at the next start\n", (unsigned long long)n);
    fflush(stderr);
    abort();
}

static inline void* sv_malloc(size_t n) {
    void* p = SV_ALLOC_FAULT() ? NULL : malloc(n ? n : 1);
    if (!p) sv_oom(n);
    return p;
}

static inline void* sv_calloc(size_t count, size_t size) {
    void* p = SV_ALLOC_FAULT() ? NULL : calloc(count ? count : 1, size ? size : 1);
    if (!p) sv_oom(count * size);
    return p;
}

static inline void* sv_realloc(void* old, size_t n) {
    void* p = SV_ALLOC_FAULT() ? NULL : realloc(old, n ? n : 1);
    if (!p) sv_oom(n);
    return p;
}

static inline char* sv_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)sv_malloc(n);
    memcpy(p, s, n);
    return p;
}

static inline char* sv_strndup(const char* s, size_t max) {
    size_t n = strnlen(s, max);
    char* p = (char*)sv_malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

// dimensione decisa dai dati: NULL se manca la memoria, il chiamante fallisce l'istruzione
static inline void* sv_try_malloc(size_t n) {
    return SV_ALLOC_FAULT() ? NULL : malloc(n ? n : 1);
}

static inline void* sv_try_calloc(size_t count, size_t size) {
    return SV_ALLOC_FAULT() ? NULL : calloc(count ? count : 1, size ? size : 1);
}

static inline void* sv_try_realloc(void* old, size_t n) {
    return SV_ALLOC_FAULT() ? NULL : realloc(old, n ? n : 1);
}

#endif
