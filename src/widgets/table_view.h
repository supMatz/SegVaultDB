/*
SCOPO: griglia per mostrare i risultati delle query SQL.
       Supporta: header colonne con tipo, numeri di riga, scroll verticale e
       orizzontale (rotella / Shift+rotella), hover e selezione di riga e cella,
       copia della cella (Ctrl+C), ridimensionamento delle colonne trascinando
       il bordo dell'header (doppio click = adatta), messaggi di stato
       (nessun risultato, query OK, errore).
*/

#ifndef TABLE_VIEW_H
#define TABLE_VIEW_H

#include "widget.h"
#include "../bridge/db_api.h"

#define TABLE_VIEW_MAX_COLS  64
#define TABLE_VIEW_ROW_H     28 // altezza righe
#define TABLE_VIEW_HEADER_H  34 // altezza header colonne
#define TABLE_VIEW_AUTOSIZE_SCAN_LIMIT 500 // righe max da scansionare per l'autosize

typedef enum {
    TABLE_MSG_NONE = 0,   // mostra la griglia (o lo stato vuoto)
    TABLE_MSG_OK,         // statement senza risultato tabellare eseguito con successo
    TABLE_MSG_ERROR,      // errore
} TableMessageKind;

typedef struct {
    Widget base;

    // dati
    QueryResult* result;
    int selected_row; // indice riga selezionata : -1 = nessuna
    int selected_col; // cella selezionata nella riga : -1 = nessuna
    int hover_row;

    // colonne
    int  col_widths[TABLE_VIEW_MAX_COLS];
    bool needs_autosize;   // misura le colonne al prossimo draw (serve la finestra)
    bool col_numeric[TABLE_VIEW_MAX_COLS]; // colonna con soli valori numerici (allineata a destra)
    int  rownum_w;         // larghezza colonna numeri di riga
    int  sort_col; // colonna di ordinamento : -1 = nessuna
    bool sort_asc; // di base impostato a true

    // ridimensionamento colonne
    int  resize_col;       // -1 = nessun resize in corso
    int  resize_start_x;
    int  resize_start_w;
    uint64_t last_header_click;

    // scroll
    int scroll_x;          // pixel
    int scroll_y;          // righe

    // messaggio al posto della griglia
    TableMessageKind msg_kind;
    char msg_title[128];
    char msg_detail[1024];

    // stile
    int   font_size;
    Color color_header_bg;
    Color color_header_text;
    Color color_row_even;
    Color color_row_odd;
    Color color_row_selected;
    Color color_text;
    Color color_text_null;    // colore speciale per valori NULL
    Color color_border;

    PlatformWindow* win;      // per misura testo e appunti

    // callback
    void (*on_row_select) (int row_index, void* user_data);
} TableView ;

TableView* table_view_create(int x, int y, int w, int h);

void table_view_set_result(TableView* tv, QueryResult* result); // carica un risultato di query sulla griglia

void table_view_set_message(TableView* tv, TableMessageKind kind, const char* title, const char* detail);

void table_view_clear(TableView* tv); // il risultato viene liberato in mem prima, lo fa chi chiama questa funzione

void table_view_scroll_to(TableView* tv, int row); // scroll alla riga specifica

// metriche per scrollbar
int  table_view_content_width(TableView* tv);  // pixel
int  table_view_visible_rows(TableView* tv);
int  table_view_viewport_width(TableView* tv);
void table_view_clamp_scroll(TableView* tv);

// true se (mx, my) è sul bordo ridimensionabile di una colonna (per il cursore)
bool table_view_hit_resize(TableView* tv, int mx, int my);

#endif
