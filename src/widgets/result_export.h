/*
SCOPO: esportazione dei risultati della griglia in vari formati (CSV, TSV, JSON,
       Markdown, HTML, SQL INSERT). Esporta le righe visualizzate (filtro e
       ordinamento compresi); le opzioni CSV/JSON vengono da Settings.
       Funzioni pure: nessuna dipendenza dalla finestra, testabili da sole.
*/
#ifndef RESULT_EXPORT_H
#define RESULT_EXPORT_H

#include "table_view.h"

typedef enum {
    EXPORT_CSV, EXPORT_TSV, EXPORT_JSON, EXPORT_MARKDOWN, EXPORT_HTML, EXPORT_SQL, EXPORT_FORMAT_COUNT
} ExportFormat;

typedef struct {
    char delimiter;      // CSV
    bool header;         // CSV / TSV
    bool null_as_text;   // CSV: NULL scritto come "NULL" (altrimenti campo vuoto)
    bool json_pretty;
    const char* table;   // SQL: tabella degli INSERT
} ExportOptions;

ExportFormat export_format_from_path(const char* path);   // dall'estensione (default CSV)
const char*  export_format_ext(ExportFormat f);           // "csv", "json", ...
const char*  export_format_name(ExportFormat f);          // "CSV", "JSON", ...

// testo malloc'd (free() del chiamante) o NULL se non c'è una griglia
char* export_result(const TableView* tv, ExportFormat f, const ExportOptions* opt);

// letterale SQL di un valore: NULL, numeri così come sono, testo tra apici con '' raddoppiati
char* export_sql_literal(const Cell* c);

#endif
