/*
SCOPO: import di file CSV. Converte il testo CSV in uno script SQL
       (CREATE TABLE con i tipi dedotti + un INSERT per riga) che la finestra
       esegue nel thread di lavoro come qualsiasi altro script.
       Supporta virgolette, a capo dentro i campi, CRLF, BOM UTF-8 e riconosce
       da solo il separatore ( , ; tab ). Funzione pura, testabile da sola.
*/
#ifndef CSV_IMPORT_H
#define CSV_IMPORT_H

#include <stdbool.h>

typedef struct {
    int  rows;           // righe di dati convertite
    int  cols;
    char delimiter;      // separatore usato
    char error[256];     // messaggio se ritorna NULL
} CsvImportInfo;

// csv: contenuto del file. table: tabella di destinazione. create_table: aggiunge CREATE TABLE
// con i tipi dedotti (INT, BIGINT, FLOAT, VARCHAR(n), TEXT). delimiter 0 = automatico.
// Ritorna lo script SQL malloc'd o NULL (vedi info->error).
char* csv_to_sql(const char* csv, const char* table, bool create_table, char delimiter, CsvImportInfo* info);

// nome di tabella valido ricavato dal nome del file ("Vendite 2024.csv" -> "vendite_2024")
void csv_table_name_from_path(const char* path, char* out, int n);

#endif
