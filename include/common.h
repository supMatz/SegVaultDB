/* 
SCOPO: Header incluso da TUTTI i moduli del progetto.
       Definisce tipi, macro e costanti globali condivise.
       Non include mai header specifici di OS (windows.h, Xlib.h).
*/

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sv_alloc.h"

// modo di fopen che chiude il file nei processi figli (O_CLOEXEC): "e" con glibc, nulla altrove
#if defined(__GLIBC__)
  #define SV_FE "e"
#else
  #define SV_FE ""
#endif

// versioni
#define SV_VERSION_MAJOR 1
#define SV_VERSION_MINOR 5
#define SV_VERSION_PATCH 1
#define SV_VERSION_STR  "1.5.1"
#define SV_DOCS_URL     "https://github.com/supMatz/SegVaultDB/blob/main/README.md"   // Help > Online documentation
#define SV_NAME         "SegVault"

// codici di ritorno
#define SV_OK         0      // operazione completata con successo
#define SV_ERR       -1     // errore generico (se non si vuole specificare o non si sa la ragione)
#define SV_NOT_FOUND -2    // elemento non trovato 
#define SV_FULL      -3   // struttura piena (pagina, buffer, ecc. ..)
#define SV_DUPLICATE -4  // elemento già esistente
#define SV_IO_ERROR  -5 // errore di I/O su  disco 
#define SV_BAD_NAME  -6 // nome non ammesso (sv_name_ok)

// nome di un oggetto nuovo (database, tabella, colonna, indice, vincolo, vista, routine, trigger, sequenza):
// non vuoto, senza backtick e senza caratteri di controllo. Così `nome` tra backtick è sempre un solo
// identificatore e chi costruisce SQL dai nomi (GUI, dump, statistiche) non può essere portato fuori dalle
// virgolette da un nome scelto da un altro utente (CREATE DATABASE 'x`; DROP ...' era accettato)
static inline int sv_name_ok(const char* s) {
    if (!s || !*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F || c == '`') return 0;
    }
    return 1;
}
// nome facoltativo: vuoto = scelto dal motore
static inline int sv_name_ok_opt(const char* s) { return !s || !*s || sv_name_ok(s); }

// nome come una sola parola nei file di testo del motore (utenti, statistiche): i byte <= ' ', '%' e 0x7F diventano
// %XX. Prima i nomi con spazi spostavano i campi delle righe (CR-C1, CR-B5). out: almeno 3 * strlen(s) + 1 byte
static inline void sv_name_encode(const char* s, char* out, size_t n) {
    static const char HX[] = "0123456789ABCDEF";
    size_t k = 0;
    for (; *s && k + 4 <= n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= ' ' || c == '%' || c == 0x7F) { out[k++] = '%'; out[k++] = HX[c >> 4]; out[k++] = HX[c & 15]; }
        else out[k++] = (char)c;
    }
    if (n) out[k] = '\0';
}

static inline int sv_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// decodifica sul posto; false se la codifica è sbagliata, il nome è vuoto o non sta in max byte (NUL compreso)
static inline bool sv_name_decode(char* s, size_t max) {
    size_t k = 0;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '%') {
            int h = sv_hexval(p[1]), l = h >= 0 ? sv_hexval(p[2]) : -1;
            if (h < 0 || l < 0) return false;
            c = (char)(h << 4 | l);
            p += 2;
            if (!c) return false;
        }
        s[k++] = c;
    }
    s[k] = '\0';
    return k > 0 && k < max;
}

#define SV_MAX_DATABASES    100000   // max database (liste dinamiche dal formato v6 del catalogo)
#define SV_MAX_TABLES       1000000  // max tabelle per db (liste dinamiche)
#define SV_MAX_COLUMNS      1024 // max colonne per tabella (MySQL: 4096, InnoDB: 1017)
#define SV_MAX_INDEXES      64   // max indici per tabella, come MySQL (P3.12: erano 16)
#define SV_MAX_NAME_LEN     64   // max lunghezza nomi tabelle, colonne
#define SV_MAX_SQL_LEN     65536 // max lunghezza di una query SQL
#define SV_MAX_TEXT_BYTES  (16u << 20) // un testo al massimo (le righe grandi vanno in pagine di overflow)
#define SV_PAGE_SIZE       4096  // max dimensione pagina in byte (4 KB)
#define SV_BUFFER_POOL_CAP 1024  // max pagine in cache RAM

// macro per gestione errori

// stampa file e linea dell errore + messaggio 
#define SV_ERROR(msg) do {\
    fprintf(stderr, "[ERROR] %s:%d: %s:\n", __FILE__, __LINE__, msg); \
    return SV_ERR; \
} while (0)

// stesso di sopra ma ritorna NULL
#define SV_ERROR_NULL(msg) do {\
    fprintf(stderr, "[ERROR] %s:%d: %s:\n", __FILE__, __LINE__, msg); \
    return NULL; \
} while (0)

// controlla se un puntatore è NULL: se sì, messaggio e return SV_ERR
#define SV_CHECK_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        fprintf(stderr, "[ERROR] %s:%d: NULL pointer: %s\n", __FILE__, __LINE__, msg); \
        return SV_ERR; \
    } \
} while (0)

// -- macro di debug -- attivate solo con -DDEBUG come parametro di compilazione

#ifdef DEBUG
    #define SV_DEBUG(fmt, ...) \
        fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define SV_DEBUG(fmt, ...) do { } while (0)   // no-op in release
#endif

// macro di utilities
#define SV_MIN(a,b) ((a) < (b) ? (a) : (b))
#define SV_MAX(a,b) ((a) > (b) ? (a) : (b))
#define SV_CLAMP(v, lo, hi) (SV_MIN(SV_MAX((v), (lo)), (hi))) // se v < lo => v = lo; se v > hi => v = hi; mantiene v in un range di valori passati
#define SV_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#define SV_ALLOC(type) ((type*)sv_calloc(1, sizeof(type))) // alloca e azzera — simile a calloc ma per tipo singolo
#define SV_ALLOC_N(type, n) ((type*)sv_calloc((n), sizeof(type))) // alloca array e azzera

#endif