#ifndef PAGE_H
#define PAGE_H

/*
SCOPO: Struttura e operazioni su una pagina disco (4 KB).
       Tutto il database è letto/scritto in blocchi da PAGE_SIZE.
       Layout interno di data[]:
         [slot0_off|slot0_len][slot1_off|slot1_len]...[tuple1][tuple0]
          ^── slot directory (cresce →)        ^── tuple (crescono ←)
*/

#include "../../include/common.h"

#define SV_PAGE_HEADER_SIZE  12   // Byte occupati dall'header (id+free+slots+flags)
#define SV_SLOT_ENTRY_SIZE    4   // Byte per ogni entry nella slot directory
#define SV_MAX_SLOTS        ((SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE) / SV_SLOT_ENTRY_SIZE)

// Flag di stato della pagina
#define PAGE_FLAG_DIRTY   0x01  // Pagina modificata ma non scritta su disco
#define PAGE_FLAG_DELETED 0x02  // Pagina marcata per eliminazione

#pragma pack(push, 1)  // evita padding: la struttura deve essere esattamente PAGE_SIZE byte
typedef struct {
    uint32_t page_id;                        // ID univoco della pagina
    uint16_t free_space;                     // byte liberi rimanenti
    uint16_t num_slots;                      // numero di slot occupati
    uint16_t flags;                          // bitmask PAGE_FLAG_*
    uint16_t next_page_id;                   // prossima pagina nella chain (0 = fine)
    uint8_t  data[SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE]; // payload
} Page;
#pragma pack(pop)

// Una slot entry: descrive dove si trova una tuple nel data[]
typedef struct {
    uint16_t offset;  // Offset della tuple dentro data[]
    uint16_t length;  // Lunghezza della tuple in byte (0 = slot eliminato)
} SlotEntry;

// ── API ──────────────────────────────────────────────────────────

Page* page_create(uint32_t id);
void    page_free(Page* p);

// I/O su file descriptor (usa pread/pwrite per accesso posizionale)
int     page_write(Page* p, int fd);
Page* page_read(uint32_t id, int fd);

// Inserisce dati nel payload, aggiorna la slot directory
// Ritorna lo slot_id assegnato, o SV_FULL se non c'è spazio
int     page_insert(Page* p, const void* data, uint16_t len);

// Legge i dati di uno slot (puntatore dentro p->data — non copiare)
const void* page_get_slot(Page* p, uint16_t slot_id, uint16_t* out_len);

// Marca uno slot come eliminato (len = 0)
int     page_delete_slot(Page* p, uint16_t slot_id);

// Compatta la pagina: rimuove i slot eliminati e compatta il payload
void    page_compact(Page* p);

// Ritorna la slot entry di un dato slot
SlotEntry page_get_slot_entry(Page* p, uint16_t slot_id);

#endif