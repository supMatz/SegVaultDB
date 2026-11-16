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
    uint32_t pid;          // pagina del frame (chiave della tabella hash, valida anche durante la lettura)
    bool     loading;      // la lettura dal disco è in corso fuori dal latch: chi la vuole aspetta
    bool     fresh;        // creata dopo l'ultimo checkpoint: il log ha tutti i suoi record, nessuna immagine serve
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

#include "../platform_lock.h"

// una partizione della cache (P1.10): le pagine si dividono per page_id fra BP_PARTS partizioni, ognuna con il
// suo latch, i suoi frame, la sua tabella hash e la sua lancetta di CLOCK. Lettori di pagine diverse non si
// aspettano più l'un l'altro, e la lettura dal disco avviene fuori dal latch (frame "loading")
#define BP_PARTS 16
typedef struct {
    Frame*      frames;
    int         capacity;
    int         num_used;
    int*        buckets;
    int         nbuckets;
    int         hand;
    uint64_t    tick;
    BufferStats stats;
    SvRwLock    latch;
    // buffer di pagine sfrattate, riusati per le prossime letture: un malloc / free da 4 KB per ogni pagina letta
    // faceva restituire memoria al sistema da glibc (trim) e ripagare i page fault a ogni scansione (P1.10:
    // una scansione di una tabella più grande della cache era 1,9 volte più lenta in un processo appena avviato)
    Page**      spare;
    int         nspare;
} BpPart;

typedef struct {
    BpPart    parts[BP_PARTS];
    int       nparts;      // partizioni usate (una sola per una cache piccola)
    int       capacity;    // numero massimo di frame (tutte le partizioni)
    int       num_used;    // (aggiornato da bp_stats)
    int       db_fd;       // file descriptor del file del database
    SvRwLock  alloc;       // allocazione delle pagine nuove ed elenco delle pagine libere
    SvRwLock  wmx;         // scritture di pagine modificate (regola del WAL: before_write non è rientrante)
    uint32_t  next_new_id; // prossima pagina da allocare in fondo al file (0 = da calcolare)
    uint32_t* free_ids;    // pagine liberate da VACUUM (flag PAGE_FLAG_FREE su disco), riusate per prime
    int       nfree, free_cap;
    BufferStats stats;
    // chiamata prima di scrivere una pagina su disco (regola del WAL: prima il log); false = il log
    // non è stato scritto, quindi nemmeno la pagina può esserlo
    // regola del WAL prima di scrivere pagine modificate: le immagini di pages[0..n) nel log, poi il log su disco
    // (n == 0: solo il log). Le pagine di overflow non hanno immagine (immutabili, ricostruite dal log)
    bool      (*before_write)(void* ctx, Page* const* pages, int n);
    void*     before_write_ctx;
    // pagina nuova di una tabella collegata dopo prev (0 = prima pagina): il WAL la registra (LOG_LINK)
    void      (*on_link)(void* ctx, uint32_t prev, uint32_t page);
    void*     on_link_ctx;
    // una scrittura o un fsync del file dei dati non è riuscito: le pagine non scritte restano in
    // memoria (mai scartate), il motore smette di accettare modifiche
    bool      io_error;
    bool      ov_unsynced; // pagine di overflow scritte dopo l'ultimo fsync: prima di una pagina di heap, fsync (P1.5)
} BufferPool;

// contatori di tutte le partizioni (e bp->num_used aggiornato)
void bp_stats(BufferPool* bp, BufferStats* out);

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

// scrive su disco tutte le pagine dirty (in ordine di page_id: scrittura sequenziale) e fa fsync;
// SV_OK solo se tutto è arrivato su disco
int bp_flush_all(BufferPool* bp);

// Rimuove una pagina dalla cache (forza la rilettura dal disco)
void bp_remove_page(BufferPool* bp, uint32_t page_id);

// Rimuove tutte le pagine dalla cache (scrivendo prima quelle modificate)
void bp_remove_all(BufferPool* bp);

// nuova capacità (pagine): scrive le pagine modificate e svuota la cache; false se una pagina è in uso
// (pinnata) o manca memoria (la cache resta com'era)
bool bp_resize(BufferPool* bp, int capacity);

// dealloca il buffer pool (fa flush prima di liberare)
void bp_destroy(BufferPool* bp);

// pagine libere (VACUUM): riusate da bp_new_page solo se su disco hanno ancora PAGE_FLAG_FREE
void bp_add_free(BufferPool* bp, uint32_t page_id);
int  bp_free_count(BufferPool* bp);
int  bp_free_list(BufferPool* bp, uint32_t* out, int max);

// la pagina (se è nella cache) è stata cambiata dal record del log lsn: il suo LSN sale (doppia scrittura e redo)
void bp_note_lsn(BufferPool* bp, uint32_t page_id, int64_t lsn);
// checkpoint riuscito (tutte le pagine su disco): da qui ogni pagina ha un contenuto da proteggere con l'immagine
void bp_checkpoint_done(BufferPool* bp);

// pagina libera subito (P1.5): via dalla cache, scritta libera sul file con l'LSN dato, nella lista delle libere.
// Solo per pagine che nessuna riga confermata o recuperabile riferisce più
int bp_free_page_now(BufferPool* bp, uint32_t page_id, uint64_t lsn);

// VACUUM FULL: le pagine libere si riusano dalla più bassa; poi le libere in fondo al file vengono tolte (file più corto).
// Ritorna le pagine tolte (-1 = errore)
void bp_free_lowest_first(BufferPool* bp);
int  bp_truncate_free_tail(BufferPool* bp);

#endif
