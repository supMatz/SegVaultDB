/*
SCOPO: Cache delle pagine in memoria RAM (buffer pool).
       Evita di accedere al disco per ogni operazione.
       - ricerca di una pagina: tabella hash page_id -> frame, O(1)
       - politica di sostituzione: CLOCK (second chance), l'approssimazione di LRU usata dai
         DBMS reali: ogni frame ha un bit di riferimento, la "lancetta" gira sui frame e
         sceglie il primo non pinnato con il bit a 0 (azzerando i bit che trova a 1)
       Una pagina "pinned" (pin_count > 0) non può essere rimossa dalla cache.
*/

#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include "page.h"

// un frame nella cache: contiene una pagina e i suoi metadati
typedef struct {
    Page*    page;         // la pagina caricata in RAM (NULL = frame libero)
    bool     dirty;        // true = modificata, va scritta su disco
    bool     pinned;       // (compatibilità) non usato: conta pin_count
    bool     ref;          // bit di riferimento di CLOCK
    uint64_t last_access;  // ultimo accesso (informativo)
    int      pin_count;    // numero di pin attivi (può essere pinnata più volte)
    int      hnext;        // catena della tabella hash (-1 = fine)
} Frame;

// contatori per lo studio del comportamento del buffer (SHOW BUFFER POOL)
typedef struct {
    uint64_t requests;     // bp_fetch
    uint64_t hits;         // pagina già in memoria
    uint64_t misses;       // letta dal disco
    uint64_t evictions;    // frame liberati per far posto
    uint64_t dirty_writes; // pagine modificate scritte su disco
    uint64_t new_pages;    // pagine allocate
} BufferStats;

typedef struct {
    Frame*    frames;      // array di frame (dimensione = capacity)
    int       capacity;    // numero massimo di frame
    int       num_used;    // frame attualmente occupati
    int       db_fd;       // file descriptor del file del database
    uint64_t  tick;        // contatore monotono degli accessi
    int*      buckets;     // tabella hash: page_id -> primo frame della catena
    int       nbuckets;    // potenza di 2
    int       hand;        // lancetta di CLOCK
    uint32_t  next_new_id; // prossima pagina da allocare in fondo al file (0 = da calcolare)
    BufferStats stats;
} BufferPool;

BufferPool* bp_create(int capacity, int db_fd);

// porta una pagina in cache (la legge dal disco se non è già presente)
// imposta il pin: la pagina non verrà rimossa finché non chiami bp_unpin
Page* bp_fetch(BufferPool* bp, uint32_t page_id);

// segnala che hai finito di usare la pagina
// dirty=true: hai modificato la pagina (verrà scritta al prossimo flush)
void bp_unpin(BufferPool* bp, uint32_t page_id, bool dirty);

// crea una nuova pagina e la porta in cache (pinnata)
// ritorna il page_id della nuova pagina
int bp_new_page(BufferPool* bp, uint32_t* out_page_id);

// scrive su disco una pagina specifica (se dirty)
int bp_flush_page(BufferPool* bp, uint32_t page_id);

// scrive su disco tutte le pagine dirty (in ordine di page_id: scrittura sequenziale)
int bp_flush_all(BufferPool* bp);

// Rimuove una pagina dalla cache (forza la rilettura dal disco)
void bp_remove_page(BufferPool* bp, uint32_t page_id);

// Rimuove tutte le pagine dalla cache (scrivendo prima quelle modificate)
void bp_remove_all(BufferPool* bp);

// dealloca il buffer pool (fa flush prima di liberare)
void bp_destroy(BufferPool* bp);

#endif
