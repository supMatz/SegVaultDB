#include "page.h"
#include "../../include/common.h"
#include <unistd.h>  // pread, pwrite

Page* page_create(uint32_t id) {
    Page* p = (Page*)calloc(1, sizeof(Page));
    if (!p) return NULL;
    p->page_id   = id;
    p->free_space = SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE;
    p->num_slots  = 0;
    p->flags      = 0;
    p->next_page_id = 0;
    return p;
}

void page_free(Page* p) { free(p); }

int page_write(Page* p, int fd) {
    off_t offset = (off_t)p->page_id * SV_PAGE_SIZE;
    ssize_t written = pwrite(fd, p, SV_PAGE_SIZE, offset);
    return (written == SV_PAGE_SIZE) ? SV_OK : SV_IO_ERROR;
}

Page* page_read(uint32_t id, int fd) {
    Page* p = (Page*)malloc(sizeof(Page));
    if (!p) return NULL;
    off_t offset = (off_t)id * SV_PAGE_SIZE;
    ssize_t n = pread(fd, p, SV_PAGE_SIZE, offset);
    if (n != SV_PAGE_SIZE) { free(p); return NULL; }
    return p;
}

int page_insert(Page* p, const void* data, uint16_t len) {
    uint16_t needed = len + SV_SLOT_ENTRY_SIZE;
    if (p->free_space < needed) return SV_FULL;

    // slot directory: cresce dall'inizio di data[]
    uint16_t slot_dir_size = p->num_slots * SV_SLOT_ENTRY_SIZE;

    // tuple: crescono dalla fine di data[] verso l'inizio
    uint16_t data_size = sizeof(p->data);
    // spazio usato dalle tuple già esistenti
    uint16_t used_tuple_space = data_size - p->free_space - slot_dir_size;
    uint16_t tuple_offset = data_size - used_tuple_space - len;

    // Scrivi la tuple
    memcpy(p->data + tuple_offset, data, len);

    // aggiungi la slot entry
    SlotEntry* slot = (SlotEntry*)(p->data + slot_dir_size);
    slot->offset = tuple_offset;
    slot->length = len;

    p->num_slots++;
    p->free_space -= needed;
    p->flags |= PAGE_FLAG_DIRTY;

    return p->num_slots - 1; // Ritorna slot_id
}

const void* page_get_slot(Page* p, uint16_t slot_id,
                           uint16_t* out_len) {
    if (slot_id >= p->num_slots) return NULL;
    SlotEntry* entry = (SlotEntry*)(p->data) + slot_id;
    if (entry->length == 0) return NULL; // Slot eliminato
    if (out_len) *out_len = entry->length;
    return p->data + entry->offset;
}

int page_delete_slot(Page* p, uint16_t slot_id) {
    if (slot_id >= p->num_slots) return SV_NOT_FOUND;
    SlotEntry* entry = (SlotEntry*)(p->data) + slot_id;
    if (entry->length == 0) return SV_NOT_FOUND; // Già eliminato
    // Marca come eliminato (length = 0), recupera spazio solo dopo compact
    entry->length = 0;
    p->flags |= PAGE_FLAG_DIRTY;
    return SV_OK;
}

void page_compact(Page* p) {
    // Raccoglie tutte le tuple valide e le ricopia compattate
    uint8_t temp[SV_PAGE_SIZE];
    uint16_t write_pos = sizeof(p->data); // Parte dalla fine

    for (uint16_t i = 0; i < p->num_slots; i++) {
        SlotEntry* entry = (SlotEntry*)(p->data) + i;
        if (entry->length == 0) continue; // Slot eliminato: salta
        write_pos -= entry->length;
        memcpy(temp + write_pos, p->data + entry->offset, entry->length);
        entry->offset = write_pos; // Aggiorna l'offset nella slot directory
    }
    // Copia le tuple compattate in data[]
    memcpy(p->data + write_pos, temp + write_pos,
           sizeof(p->data) - write_pos);
    // Ricalcola free_space
    uint16_t slot_dir_size = p->num_slots * SV_SLOT_ENTRY_SIZE;
    p->free_space = write_pos - slot_dir_size;
    p->flags |= PAGE_FLAG_DIRTY;
}

SlotEntry page_get_slot_entry(Page* p, uint16_t slot_id) {
    if (slot_id >= p->num_slots) return (SlotEntry){0, 0};
    return *((SlotEntry*)(p->data) + slot_id);
}