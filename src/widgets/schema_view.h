/*
SCOPO: pagina "Designer" — il diagramma dello schema di un database, in notazione ER
       (entità, attributi, relazioni a rombo con le cardinalità) o UML (classi con attributi
       e molteplicità). Le tabelle si spostano trascinandole (posizioni salvate per database),
       le relazioni si creano trascinando dal pallino di una tabella (o di una sua colonna)
       a un'altra; tabelle e relazioni si modificano ed eliminano dal diagramma.
       La pagina non esegue SQL: lo consegna alla finestra (SCV_EXEC), che lo esegue e
       risponde con schema_view_exec_done.
*/
#ifndef SCHEMA_VIEW_H
#define SCHEMA_VIEW_H

#include "../platform/platform.h"

typedef enum {
    SCV_NONE = 0,
    SCV_REDRAW,
    SCV_NEW_TABLE,     // aprire il modulo per una tabella nuova in schema_view_db()
    SCV_EDIT_TABLE,    // aprire il modulo della struttura di schema_view_table()
    SCV_BROWSE_TABLE,  // mostrare i dati di schema_view_table() nell'editor
    SCV_EXEC,          // eseguire schema_view_sql() (statement separati da ";\n")
} SchemaViewAction;

typedef struct SchemaView SchemaView;

SchemaView* schema_view_create(PlatformWindow* win);
void        schema_view_destroy(SchemaView* v);
void        schema_view_set_data_dir(SchemaView* v, const char* dir);   // dove salvare le posizioni

void schema_view_show_db(SchemaView* v, const char* db, const char* focus_table);  // NULL = lascia com'è
void schema_view_invalidate(SchemaView* v);       // lo schema può essere cambiato: ricarica al prossimo sync
void schema_view_sync(SchemaView* v);             // ricarica se serve (solo con il motore libero)
void schema_view_table_created(SchemaView* v, const char* name);            // va nel punto scelto
void schema_view_table_renamed(SchemaView* v, const char* from, const char* to);

void             schema_view_draw(SchemaView* v, Rect r);
SchemaViewAction schema_view_event(SchemaView* v, sEvent* evt);
bool             schema_view_tick(SchemaView* v, uint64_t now_ms);

const char* schema_view_db(const SchemaView* v);
const char* schema_view_table(const SchemaView* v);
const char* schema_view_sql(const SchemaView* v);
void        schema_view_exec_done(SchemaView* v, bool ok, const char* message);

#endif
