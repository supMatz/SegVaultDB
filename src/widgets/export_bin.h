/*
SCOPO: export dei risultati in formati binari, senza librerie esterne:
       - XLSX (Excel / LibreOffice / Google Sheets): pacchetto zip non compresso con i file XML
         minimi di Office Open XML; numeri come numeri, intestazione in grassetto, colonne larghe
         quanto il contenuto, prima riga bloccata
       - PDF: tabella in Courier su pagine A4 orizzontali, intestazione ripetuta su ogni pagina,
         numeri allineati a destra, numero di pagina
       Lavorano su un QueryResult e un ordine delle righe (filtro e ordinamento della griglia).
*/
#ifndef SV_EXPORT_BIN_H
#define SV_EXPORT_BIN_H

#include "../bridge/db_api.h"
#include <stddef.h>
#include <stdint.h>

// rows: indici delle righe di r da esportare, nell'ordine (NULL = tutte, in ordine); malloc'd o NULL
uint8_t* export_xlsx(const QueryResult* r, const int* rows, int nrows, size_t* len);
uint8_t* export_pdf(const QueryResult* r, const int* rows, int nrows, const char* title, size_t* len);

uint32_t export_crc32(const uint8_t* p, size_t n);

#endif
