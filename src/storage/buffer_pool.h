/*
SCOPO: Cache LRU delle pagine in memoria RAM.
       Evita di accedere al disco per ogni operazione.
       Politica di eviction: LRU (Least Recently Used).
       Una pagina "pinned" non può essere rimossa dalla cache.
*/

#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include "page.h"

// un frame nella cache: contiene una pagina e i suoi metadati
typedef struct {
    Page*  page;          // la pagina caricata in RAM
    bool     dirty;       // true = modificata, va scritta su disco
    bool     pinned;      // true = qualcuno la sta usando, non evictable
    uint64_t last_access; // timestamp ultimo accesso (per LRU)
    int      pin_count;   // numero di pin attivi (può essere pinnata più volte)
} Frame;

typedef struct {
    Frame*  frames;     // array di frame (dimensione = capacity)
    int       capacity;   // numero massimo di frame
    int       num_used;   // frame attualmente occupati
    int       db_fd;      // file descriptor del file del database
    uint64_t  tick;       // contatore monotono per i timestamp LRU
} BufferPool;

BufferPool* bp_create(int capacity, int db_fd);

// porta una pagina in cache (la legge dal disco se non è già presente)
// imposta il pin: la pagina non verrà rimossa finché non chiami bp_unpin
Page* bp_fetch(BufferPool* bp, uint32_t page_id);

// segnala che hai finito di usare la pagina
// dirty=true: hai modificato la pagina (verrà scritta al prossimo flush)
void bp_unpin(BufferPool* bp, uint32_t page_id, bool dirty);

// crea una nuova pagina e la porta in cache
// ritorna il page_id della nuova pagina
int bp_new_page(BufferPool* bp, uint32_t* out_page_id);

// scrive su disco una pagina specifica (se dirty)
int bp_flush_page(BufferPool* bp, uint32_t page_id);

// scrive su disco tutte le pagine dirty
int bp_flush_all(BufferPool* bp);

// dealloca il buffer pool (fa flush prima di liberare)
void bp_destroy(BufferPool* bp);

#endif