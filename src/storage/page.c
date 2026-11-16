#include "page.h"
#include "common.h"
#include "crc32c.h"
#include "platform_compat.h"
#include <string.h>

Page* page_create(uint32_t id) {
    Page* p = (Page*)sv_calloc(1, sizeof(Page));
    if (!p) return NULL;
    p->page_id   = id;
    p->free_space = SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE;
    p->num_slots  = 0;
    p->flags      = 0;
    p->next_page_id = 0;
    return p;
}

void page_free(Page* p) { free(p); }

static uint32_t page_crc(const Page* p) {
    uint32_t c = sv_crc32c(0, p, offsetof(Page, crc));
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    c = sv_crc32c(c, zero, 4);
    return sv_crc32c(c, p->data, sizeof(p->data));
}

bool page_crc_ok(const Page* p) { return p->crc == page_crc(p); }

int page_write(Page* p, int fd) {
    p->crc = page_crc(p);                            // ogni pagina su disco ha il suo CRC
    int64_t offset = (int64_t)p->page_id * SV_PAGE_SIZE;
    ssize_t written = sv_pwrite(fd, p, SV_PAGE_SIZE, offset);
    return (written == SV_PAGE_SIZE) ? SV_OK : SV_IO_ERROR;
}

bool page_header_read(int fd, uint64_t* next_lsn) {
    uint8_t h[28];
    if (sv_pread(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) return false;
    if (memcmp(h, SV_DATA_MAGIC, 8)) return false;
    uint32_t fmt, psz, crc;
    memcpy(&fmt, h + 8, 4);
    memcpy(&psz, h + 12, 4);
    memcpy(&crc, h + 24, 4);
    if (fmt != SV_DATA_FORMAT || psz != SV_PAGE_SIZE || crc != sv_crc32c(0, h, 24)) return false;
    if (next_lsn) memcpy(next_lsn, h + 16, 8);
    return true;
}

int page_header_write(int fd, uint64_t next_lsn) {
    uint8_t pg[SV_PAGE_SIZE];
    memset(pg, 0, sizeof(pg));
    memcpy(pg, SV_DATA_MAGIC, 8);
    uint32_t fmt = SV_DATA_FORMAT, psz = SV_PAGE_SIZE;
    memcpy(pg + 8, &fmt, 4);
    memcpy(pg + 12, &psz, 4);
    memcpy(pg + 16, &next_lsn, 8);
    uint32_t crc = sv_crc32c(0, pg, 24);
    memcpy(pg + 24, &crc, 4);
    return sv_pwrite(fd, pg, SV_PAGE_SIZE, 0) == SV_PAGE_SIZE ? SV_OK : SV_IO_ERROR;
}

Page* page_read(uint32_t id, int fd) {
    Page* p = (Page*)sv_malloc(sizeof(Page));
    if (!p) return NULL;
    int64_t offset = (int64_t)id * SV_PAGE_SIZE;
    ssize_t n = sv_pread(fd, p, SV_PAGE_SIZE, offset);
    if (n != SV_PAGE_SIZE) { free(p); return NULL; }
    return p;
}

bool page_validate(const Page* p, uint32_t id) {
    if (p->page_id != id) return false;                  // pagina mai scritta, o di un altro posto
    if (!page_crc_ok(p)) return false;                   // scrittura interrotta o byte cambiati sul disco
    if (p->num_slots > SV_MAX_SLOTS) return false;
    uint32_t dir = (uint32_t)p->num_slots * SV_SLOT_ENTRY_SIZE, size = (uint32_t)sizeof(p->data);
    if (dir > size || p->free_space > size - dir) return false;
    const SlotEntry* e = (const SlotEntry*)p->data;
    for (uint16_t i = 0; i < p->num_slots; i++) {
        if (e[i].length == 0) continue;
        if (e[i].offset < dir || (uint32_t)e[i].offset + e[i].length > size) return false;
    }
    return true;
}

uint16_t page_usable_space(const Page* p) {
    // spazio libero contiguo + byte delle tuple cancellate (recuperabili compattando)
    uint32_t live = 0;
    const SlotEntry* e = (const SlotEntry*)p->data;
    for (uint16_t i = 0; i < p->num_slots; i++) live += e[i].length;
    uint32_t total = (uint32_t)sizeof(p->data) - (uint32_t)p->num_slots * SV_SLOT_ENTRY_SIZE;
    return live >= total ? 0 : (uint16_t)(total - live);
}

int page_insert(Page* p, const void* data, uint16_t len) {
    uint32_t needed = (uint32_t)len + SV_SLOT_ENTRY_SIZE;
    if (p->num_slots >= SV_MAX_SLOTS) return SV_FULL;
    if (p->free_space < needed) {
        // le tuple cancellate lasciano buchi: se compattando lo spazio basta, si compatta
        // (gli slot non cambiano numero, solo posizione: i RID restano validi)
        if (page_usable_space(p) < needed) return SV_FULL;
        page_compact(p);
        if (p->free_space < needed) return SV_FULL;
    }

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
    p->free_space = (uint16_t)(p->free_space - needed);
    p->flags |= PAGE_FLAG_DIRTY;

    return p->num_slots - 1; // Ritorna slot_id
}

void page_pop_slot(Page* p, uint16_t slot_id) {
    if (p->num_slots == 0 || slot_id != p->num_slots - 1) return;
    SlotEntry* entry = (SlotEntry*)(p->data) + slot_id;
    if (entry->length != 0) return;
    p->num_slots--;
    p->free_space = (uint16_t)(p->free_space + SV_SLOT_ENTRY_SIZE);
    p->flags |= PAGE_FLAG_DIRTY;
}

int page_revive_slot(Page* p, uint16_t slot_id, const void* data, uint16_t len) {
    if (slot_id >= p->num_slots || len == 0) return SV_NOT_FOUND;
    SlotEntry* entry = (SlotEntry*)(p->data) + slot_id;
    if (entry->length != 0) return SV_DUPLICATE;                 // già viva
    if (p->free_space < len) {
        // lo slot esiste già: serve solo lo spazio della riga (compattando, se basta)
        uint32_t live = 0;
        const SlotEntry* e = (const SlotEntry*)p->data;
        for (uint16_t i = 0; i < p->num_slots; i++) live += e[i].length;
        uint32_t total = (uint32_t)sizeof(p->data) - (uint32_t)p->num_slots * SV_SLOT_ENTRY_SIZE;
        if (live + len > total) return SV_FULL;
        page_compact(p);
        if (p->free_space < len) return SV_FULL;
    }
    uint16_t slot_dir_size = p->num_slots * SV_SLOT_ENTRY_SIZE;
    uint16_t used_tuple_space = (uint16_t)(sizeof(p->data) - p->free_space - slot_dir_size);
    uint16_t off = (uint16_t)(sizeof(p->data) - used_tuple_space - len);
    memcpy(p->data + off, data, len);
    entry = (SlotEntry*)(p->data) + slot_id;
    entry->offset = off;
    entry->length = len;
    p->free_space = (uint16_t)(p->free_space - len);
    p->flags |= PAGE_FLAG_DIRTY;
    return SV_OK;
}

const void* page_get_slot(Page* p, uint16_t slot_id,
                           uint16_t* out_len) {
    if (slot_id >= p->num_slots || slot_id >= SV_MAX_SLOTS) return NULL;
    SlotEntry* entry = (SlotEntry*)(p->data) + slot_id;
    if (entry->length == 0) return NULL; // Slot eliminato
    if ((uint32_t)entry->offset + entry->length > sizeof(p->data)) return NULL;   // danneggiato
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
    uint16_t nslots = p->num_slots <= SV_MAX_SLOTS ? p->num_slots : SV_MAX_SLOTS;
    uint32_t dir = (uint32_t)nslots * SV_SLOT_ENTRY_SIZE;

    for (uint16_t i = 0; i < nslots; i++) {
        SlotEntry* entry = (SlotEntry*)(p->data) + i;
        if (entry->length == 0) continue; // Slot eliminato: salta
        // pagina danneggiata (page_validate la rifiuta alla lettura): niente scritture fuori da temp
        if ((uint32_t)entry->offset + entry->length > sizeof(p->data) || entry->length > write_pos - dir) { entry->length = 0; continue; }
        write_pos -= entry->length;
        memcpy(temp + write_pos, p->data + entry->offset, entry->length);
        entry->offset = write_pos; // Aggiorna l'offset nella slot directory
    }
    // Copia le tuple compattate in data[]
    memcpy(p->data + write_pos, temp + write_pos,
           sizeof(p->data) - write_pos);
    // Ricalcola free_space
    p->free_space = (uint16_t)(write_pos - dir);
    p->flags |= PAGE_FLAG_DIRTY;
}

SlotEntry page_get_slot_entry(Page* p, uint16_t slot_id) {
    if (slot_id >= p->num_slots) return (SlotEntry){0, 0};
    return *((SlotEntry*)(p->data) + slot_id);
}