/*
SCOPO: righe più grandi di una pagina. Una riga serializzata oltre OV_INLINE_MAX byte va in una
       catena di pagine di overflow; nello slot della pagina di heap resta uno "stub" di 24 byte
       (codice magico, lunghezza, prima pagina, numero di pagine, checksum).
       Le pagine di overflow sono IMMUTABILI: scritte una volta (subito su disco) e mai modificate
       né riusate. Così il WAL resta invariato: registra la riga intera, e annullamento e recupero
       creano a loro volta una catena nuova quando rimettono una riga grande (ov_slot_put); una
       catena non più riferita è solo spazio perso, mai un dato sbagliato.
       Due varianti: tramite buffer pool (heap) e direttamente sul file (WAL, a motore fermo).
*/
#ifndef SV_OVERFLOW_H
#define SV_OVERFLOW_H

#include "../storage/buffer_pool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OV_STUB_LEN     24
#define OV_INLINE_MAX   2048          // righe più lunghe: fuori dalla pagina
#define PAGE_FLAG_OVERFLOW 0x08       // pagina di una catena di overflow
#define OV_MAX_ROW      (64u << 20)   // riga serializzata massima (64 MB)

// lo slot contiene uno stub? (total = lunghezza della riga, first = prima pagina)
bool ov_is_stub(const uint8_t* d, size_t n, uint32_t* total, uint32_t* first);

// scrive data in una catena nuova e prepara lo stub (stub = OV_STUB_LEN byte); SV_OK / SV_FULL / SV_ERR
int      ov_store_bp(BufferPool* bp, const uint8_t* data, size_t n, uint8_t* stub);
// rilegge la riga di uno stub (malloc'd, lunghezza in *n); NULL se la catena è danneggiata
uint8_t* ov_load_bp(BufferPool* bp, const uint8_t* stub, size_t stub_len, size_t* n);

int      ov_store_fd(int db_fd, const uint8_t* data, size_t n, uint8_t* stub);
uint8_t* ov_load_fd(int db_fd, const uint8_t* stub, size_t stub_len, size_t* n);

// per il WAL: inserisce la riga nella pagina (con uno stub se è grande); slot o < 0
int  ov_slot_put(int db_fd, void* page, const uint8_t* data, size_t n);
// il contenuto dello slot (anche uno stub) è proprio data?
bool ov_slot_equals(int db_fd, const uint8_t* cur, size_t cur_len, const uint8_t* data, size_t n);

#endif
