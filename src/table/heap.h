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
} HeapScan;

HeapScan* heap_scan_open(HeapFile* hf, TableSchema* schema);
Tuple* heap_scan_next(HeapScan* scan, RID* out_rid);
// riga successiva senza deserializzarla: puntatore dentro la pagina, valido fino alla chiamata seguente
const uint8_t* heap_scan_next_raw(HeapScan* scan, RID* out_rid, uint16_t* out_len);

// scarta le free-space map (da chiamare quando le pagine cambiano fuori da heap.c: ROLLBACK)
void heap_cache_clear(void);
void heap_cache_forget(BufferPool* bp, uint32_t root_page_id);

// != 0: le scansioni in corso terminano subito (cancellazione di una SELECT dalla GUI).
// Viene attivato solo durante le query in sola lettura, mai a metà di una modifica.
#include <stdatomic.h>
extern atomic_int g_sv_scan_cancel;
void heap_scan_close(HeapScan* scan);

#endif
