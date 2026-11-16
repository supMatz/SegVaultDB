#ifndef HEAP_H
#define HEAP_H

#include "../../include/common.h"
#include "../storage/buffer_pool.h"
#include "../catalog/schema.h"
#include "tuple.h"

// Le pagine di una tabella formano una catena (page_next) a partire da root_page_id:
// non sono contigue nel file, perché più tabelle crescono alternandosi.
typedef struct HeapMeta HeapMeta;   // free-space map della tabella (in memoria, vedi heap.c)

typedef struct {
    BufferPool* bp;
    HeapMeta*   meta;          // NULL = mappa non disponibile (si percorre la catena)
    uint32_t    root_page_id;
    uint32_t    num_pages;     // aggiornato da heap_open/heap_insert (salvato nel catalogo)
    uint32_t    last_page_id;  // ultima pagina della catena
} HeapFile;

HeapFile* heap_open(BufferPool* bp, uint32_t root_page_id, uint32_t num_pages);
int heap_insert(HeapFile* hf, Tuple* t, TableSchema* schema, RID* out_rid);
Tuple* heap_get(HeapFile* hf, RID rid, TableSchema* schema);
int heap_delete(HeapFile* hf, RID rid);
int heap_update(HeapFile* hf, RID rid, Tuple* t, TableSchema* schema, RID* out_new_rid);  // la riga può cambiare posizione

typedef struct {
    HeapFile* hf;
    TableSchema* schema;
    uint32_t current_page;
    uint16_t current_slot;
    uint32_t pages_seen;
    Page* page;            // pagina corrente, pinnata finché la scansione è su di essa
    bool done;
    uint8_t* ovbuf;        // riga grande ricomposta dalle pagine di overflow (fino alla riga seguente)
    const struct HeapSnapshot* snap;   // versione confermata da leggere (MVCC), NULL = le righe come sono
    int extra_i;           // righe cancellate dalla transazione già restituite in fondo alla scansione
} HeapScan;

// Versione confermata di una tabella per chi legge mentre un'altra transazione la sta modificando
// (letture che non aspettano le scritture): le righe nuove non confermate si nascondono, quelle
// modificate tornano alla versione di prima, quelle cancellate si aggiungono in fondo. I dati sono righe
// serializzate (tuple_serialize). Attiva solo per il thread che la imposta.
typedef struct HeapSnapshot {
    uint32_t  root;               // la tabella (prima pagina)
    RID*      hide;  int nhide;   // righe da nascondere (ordinate)
    RID*      repl_rid;           // righe da sostituire (ordinate) con repl[i] / repl_len[i]
    uint8_t** repl;  size_t* repl_len; int nrepl;
    uint8_t** extra; size_t* extra_len; int nextra;
} HeapSnapshot;
void heap_snapshot_set(const HeapSnapshot* snaps, int n);   // NULL, 0 = nessuna
bool heap_snapshot_active(uint32_t root);

HeapScan* heap_scan_open(HeapFile* hf, TableSchema* schema);
Tuple* heap_scan_next(HeapScan* scan, RID* out_rid);
// riga successiva senza deserializzarla: puntatore dentro la pagina, valido fino alla chiamata seguente
// (le righe grandi sono ricomposte dalle pagine di overflow: vedi overflow.h)
const uint8_t* heap_scan_next_raw(HeapScan* scan, RID* out_rid, size_t* out_len);

// scarta le free-space map (da chiamare quando le pagine cambiano fuori da heap.c: ROLLBACK)
void heap_cache_clear(void);
// VACUUM: segna nella bitmap le pagine di overflow usate dalle righe della tabella
#include <stdatomic.h>
// pagine abbandonate dall'ultimo VACUUM (catene di overflow di righe eliminate o sostituite, tabelle
// eliminate): fanno partire un VACUUM automatico
extern atomic_int g_sv_abandoned_pages;
// se non NULL, heap_mark_overflow vi segna anche le pagine della catena della tabella (VACUUM)
extern uint8_t* heap_pages_bitmap;
void heap_mark_overflow(HeapFile* hf, uint8_t* bitmap, uint32_t nbits);
void heap_cache_forget(BufferPool* bp, uint32_t root_page_id);

// != 0: le scansioni in corso terminano subito (cancellazione di una SELECT dalla GUI).
// Viene attivato solo durante le query in sola lettura, mai a metà di una modifica.
#include <stdatomic.h>
extern atomic_int g_sv_scan_cancel;
// scadenza delle letture di questo thread (ms di sv_get_time_ms, 0 = nessuna)
void sv_scan_set_deadline(double deadline_ms);
// la lettura in corso va interrotta? (annullamento globale o scadenza del thread)
bool sv_scan_cancelled(void);
// flag di annullamento della connessione di questo thread (KILL QUERY id): le scansioni lo guardano anche
void sv_scan_set_cancel(atomic_int* flag);
// solo il flag di questo thread (non quello globale della GUI): per le attese di un lock
bool sv_thread_cancelled(void);
void heap_scan_close(HeapScan* scan);

// Una scansione di questo thread ha trovato una pagina illeggibile o danneggiata, una catena di overflow
// rotta o una riga che non si decodifica: lo statement deve fallire, mai finire con meno righe.
// heap_damage: true se è successo dall'ultimo heap_damage_clear (messaggio in msg)
void heap_damage_clear(void);
bool heap_damage(char* msg, size_t n);

// catene di overflow abbandonate (P1.5): first = prima pagina, total = byte della riga
void heap_set_abandon_hook(void (*hook)(uint32_t first, uint32_t total));
void heap_chain_abandoned(uint32_t first, uint32_t total);

#endif
