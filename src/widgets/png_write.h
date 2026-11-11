/*
SCOPO: scrittura di immagini PNG (RGB a 8 bit) senza librerie esterne: compressione deflate con
       LZ77 e codici di Huffman fissi, sufficiente per immagini dell'interfaccia (diagrammi,
       ampie zone di colore uniforme).
*/
#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// pixels: w * h valori 0x00RRGGBB, riga per riga. false se non si può scrivere il file
bool png_write_file(const char* path, int w, int h, const uint32_t* pixels);

// PNG in memoria (malloc'd, lunghezza in *len); NULL se manca memoria
uint8_t* png_encode(int w, int h, const uint32_t* pixels, size_t* len);

// flusso zlib (deflate) di data: usato da png_encode, esposto per i test
uint8_t* png_zlib_compress(const uint8_t* data, size_t n, size_t* out_len);

uint32_t png_crc32(const uint8_t* p, size_t n);

#endif
