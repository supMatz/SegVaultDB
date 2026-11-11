/*
SCOPO: pannello del designer di tabelle (come la struttura di phpMyAdmin): crea una tabella o
       ne modifica le colonne con un modulo, mostra l'SQL generato (table_design.h) e lo
       consegna alla finestra, che lo esegue.
*/
#ifndef TABLE_DESIGNER_H
#define TABLE_DESIGNER_H

#include "../platform/platform.h"

typedef enum { TD_NONE = 0, TD_REDRAW, TD_CLOSE, TD_APPLY, TD_TO_EDITOR } TdAction;

typedef struct TableDesigner TableDesigner;

TableDesigner* td_create(PlatformWindow* win);
void        td_destroy(TableDesigner* td);
bool        td_open(TableDesigner* td, const char* db, const char* table);   // table NULL = tabella nuova
bool        td_is_open(const TableDesigner* td);
void        td_close(TableDesigner* td);
void        td_draw(TableDesigner* td, int w, int h);
TdAction    td_event(TableDesigner* td, sEvent* evt);
bool        td_tick(TableDesigner* td, uint64_t now_ms);
const char* td_sql(TableDesigner* td);           // SQL generato (NULL se la struttura non è valida)
void        td_set_error(TableDesigner* td, const char* msg);
bool        td_is_new(const TableDesigner* td);
const char* td_db(const TableDesigner* td);
const char* td_table(const TableDesigner* td);   // nome (dopo le modifiche)
const char* td_orig_table(const TableDesigner* td);   // nome com'era all'apertura

#endif
