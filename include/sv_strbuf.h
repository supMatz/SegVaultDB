/*
SCOPO: un solo buffer di testo che cresce, per motore, server e GUI (CR-B2: prima 13 copie con politiche
       diverse di crescita e di errore).
       - crescita geometrica con controllo dell'overflow di size_t;
       - realloc normale: se la memoria manca il buffer NON termina il processo, segna oom e ignora le
         aggiunte successive (flag "appiccicoso": basta controllarlo una volta alla fine);
       - max (facoltativo, 0 = nessun limite): oltre max byte il buffer segna toolong (e oom) invece di crescere;
       - p è sempre terminato da '\0' quando non è NULL.
       Uso: SvStrBuf b = {0}; svsb_puts(&b, "x"); ... char* s = svsb_take(&b);   // NULL se qualcosa è andato perso
*/
#ifndef SV_STRBUF_H
#define SV_STRBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char*  p;        // testo (NULL finché vuoto)
    size_t n;        // byte usati, senza il '\0'
    size_t cap;      // byte allocati
    size_t max;      // 0 = nessun limite; altrimenti lunghezza massima del testo
    bool   oom;      // un'aggiunta è andata persa (memoria finita o oltre max): il contenuto è incompleto
    bool   toolong;  // in particolare: superato max
} SvStrBuf;

// crescita comune a ogni buffer di testo (anche JsonBuf di server/json.c): spazio per used + add byte e il '\0'
// in *p (capacità *cap), raddoppiando senza traboccare. 0 = fatto, 1 = memoria finita, 2 = oltre max (0 = nessuno)
static inline int svsb_grow(char** p, size_t* cap, size_t used, size_t add, size_t max) {
    if (add > SIZE_MAX - used - 1) return 1;
    size_t need = used + add + 1;
    if (max && need > max + 1) return 2;
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap : 256;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) { nc = need; break; }
        nc *= 2;
    }
    if (max && nc > max + 1) nc = max + 1;
    char* np = (char*)realloc(*p, nc);
    if (!np) return 1;
    *p = np;
    *cap = nc;
    return 0;
}

static inline bool svsb_reserve(SvStrBuf* b, size_t add) {
    if (b->oom) return false;
    int r = svsb_grow(&b->p, &b->cap, b->n, add, b->max);
    if (r) { b->oom = true; b->toolong = r == 2; return false; }
    return true;
}

static inline void svsb_put(SvStrBuf* b, const void* s, size_t n) {
    if (!svsb_reserve(b, n)) return;
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static inline void svsb_puts(SvStrBuf* b, const char* s) { svsb_put(b, s, strlen(s)); }
static inline void svsb_putc(SvStrBuf* b, char c) { svsb_put(b, &c, 1); }

static inline void svsb_vprintf(SvStrBuf* b, const char* fmt, va_list ap) {
    if (b->oom) return;
    va_list ap2;
    va_copy(ap2, ap);
    char small[256];
    int k = vsnprintf(small, sizeof(small), fmt, ap2);
    va_end(ap2);
    if (k < 0) { b->oom = true; return; }
    if ((size_t)k < sizeof(small)) { svsb_put(b, small, (size_t)k); return; }
    if (!svsb_reserve(b, (size_t)k)) return;
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap);
    b->n += (size_t)k;
}

static inline void svsb_printf(SvStrBuf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void svsb_printf(SvStrBuf* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    svsb_vprintf(b, fmt, ap);
    va_end(ap);
}

static inline void svsb_free(SvStrBuf* b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

// il testo (malloc, "" se vuoto) e il buffer torna vuoto; NULL (e buffer liberato) se un'aggiunta è andata persa
static inline char* svsb_take(SvStrBuf* b) {
    if (b->oom) { svsb_free(b); return NULL; }
    char* r = b->p;
    if (!r && (r = (char*)malloc(1)) != NULL) r[0] = '\0';
    b->p = NULL;
    b->n = b->cap = 0;
    return r;
}

#endif
