#ifndef BPTREE_H
#define BPTREE_H

/*
SCOPO: indici B+tree su disco, di sola lettura (P1.6, passo 1).
       Un albero si costruisce in una volta dalle chiavi già ordinate (bottom-up: nessuno split), al checkpoint,
       dall'indice in memoria della tabella; non si modifica mai: la prima scrittura sulla tabella lo invalida
       (vedi executor.c, "alberi su disco") e il checkpoint successivo ne scrive uno nuovo.
       Pagine con PAGE_FLAG_INDEX (foglie) o PAGE_FLAG_INDEX | PAGE_FLAG_INNER (nodi interni), voci negli slot
       della pagina in ordine di chiave:
         foglia:  [u16 lunghezza chiave][chiave][u16 numero di RID][RID: u32 pagina + u16 slot]...
         interno: [u16 lunghezza chiave][chiave = prima chiave del figlio][u32 figlio]
       Le chiavi sono il testo canonico degli indici in memoria (forma corta, confronto byte per byte).
*/

#include "../../include/common.h"
#include "../storage/buffer_pool.h"
#include "../table/tuple.h"

#define PAGE_FLAG_INDEX 0x20
#define PAGE_FLAG_INNER 0x40

#define BPT_MAX_KEY     240     // chiave più lunga (in byte) che l'albero accetta
#define BPT_MAX_RIDS    600     // RID di una chiave in una foglia (oltre: l'indice non si scrive)

typedef struct {
    const char* key;            // testo della chiave (senza \0 interni)
    const RID*  rids;
    int         n;
} BptEntry;

// costruisce l'albero dalle voci (ordinate per strcmp della chiave, chiavi distinte). Le pagine nuove restano
// sporche nella cache (le scrive il checkpoint). *root = radice; *pages = pagine usate. SV_OK, SV_FULL se una
// chiave o una lista di RID non entra (nessuna pagina resta allocata: quelle già prese tornano libere con lsn)
int bpt_build(BufferPool* bp, const BptEntry* e, int n, uint64_t lsn, uint32_t* root, int* pages);

// RID della chiave: 1 trovata (*rids malloc'd, *n), 0 assente, -1 albero illeggibile (pagina danneggiata o non
// di indice): chi chiama ripiega sulla lettura della tabella. *reads = pagine lette (può essere NULL)
int bpt_lookup(BufferPool* bp, uint32_t root, const char* key, RID** rids, int* n, int* reads);

// ogni chiave con i suoi RID, in ordine di chiave (fn: false = basta). false se una pagina non si legge
bool bpt_iterate(BufferPool* bp, uint32_t root, bool (*fn)(void* ctx, const char* key, const RID* rids, int n), void* ctx);

// chiama fn per ogni pagina dell'albero (liberazione, VACUUM). false se una pagina non si legge
bool bpt_pages(BufferPool* bp, uint32_t root, void (*fn)(void* ctx, uint32_t page), void* ctx);

#endif
