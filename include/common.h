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

// versioni
#define SV_VERSION_MAJOR 0
#define SV_VERSION_MINOR 0
#define SV_VERSION_PATCH 0
#define SV_VERSION_STR  "0.1.0"
#define SV_NAME         "SegVault"

// codici di ritorno
#define SV_OK         0      // operazione completata con successo
#define SV_ERR       -1     // errore generico (se non si vuole specificare o non si sa la ragione)
#define SV_NOT_FOUND -2    // elemento non trovato 
#define SV_FULL      -3   // struttura piena (pagina, buffer, ecc. ..)
#define SV_DUPLICATE -4  // elemento già esistente
#define SV_IO_ERROR  -5 // errore di I/O su  disco 

#define SV_MAX_DATABASES    64   // max database
#define SV_MAX_TABLES       256  // max tabelle per db
#define SV_MAX_COLUMNS      64   // max colonne per tabella
#define SV_MAX_INDEXES      16   // max index per tabella
#define SV_MAX_NAME_LEN     64   // max lunghezza nomi tabelle, colonne
#define SV_MAX_SQL_LEN     65536 // max lunghezza di una query SQL
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

// controlla se un puntatore è NULL
#define SV_CHECK_NULL(msg) do { \
    fprintf(stderr, "[ERROR] %s:%d: NULL pointer\n", __FILE__, __LINE__, msg); \
    return SV_ERR; \
} while (0)

// -- macro di debug -- attivate solo con -DDEBUG come parametro di compilazione

#ifdef DEBUG
    #define SV_DEBUG(fmt, ...) \
        fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__) \
#else
    #define SV_DEBUG(fmt, ...) // no-op in release
#endif

// macro di utilities
#define SV_MIN(a,b) ((a) < (b) ? (a) : (b))
#define SV_MAX(a,b) ((a) > (b) ? (a) : (b))
#define SV_CLAMP(v, lo, hi) (SV_MIN(SV_MAX((v), (lo)), (hi))) // se v < lo => v = lo; se v > hi => v = hi; mantiene v in un range di valori passati
#define SV_ARRAY_LEN(arr) (sizeof(arr) / sizeof(arr[0]));

#define SV_ALLOC(type) ((type*)calloc(1, sizeof(type))) // alloca e azzera — simile a calloc ma per tipo singolo
#define SV_ALLOC_N(type, n) ((type*)calloc((n), sizeof(type))) // alloca array e azzera

#endif