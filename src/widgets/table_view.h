/* 
SCOPO: Griglia per mostrare i risultati delle query SQL.a
       Supporta: header colonne, scroll, selezione riga,
       ridimensionamento colonne, ordinamento.
*/

#ifndef TABLE_VIEW_H
#define TABLE_VIEW_H

#include "widget.h"
#include "../bridge/db_api.h"

#define TABLE_VIEW_MAX_COLS  64
#define TABLE_VIEW_ROW_H     24 // altezza righe
#define TABLE_VIEW_HEADER_H 28 //altezza header colonen

typedef struct {
    Widget base;

    // dati
    QueryResult* result;
    int selected_row; // indice riga selezionata : -1 = nessuna

    // colonne
    int col_widths[TABLE_VIEW_MAX_COLS];
    int sort_col; // colonna di ordinamento : -1 = nessuna
    bool sort_asc; // di base impostato a true

    // scroll
    int scroll_x;
    int scroll_y;

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

    // callback
    void (*on_row_select) (int row_index, void* user_data);
} TableView ;

TableView* table_view_create(int x, int y, int h, int w);

void table_view_set_result(TableView* tv, QueryResult* result); // carica un risultato di query sulla griglia

void table_view_clear(TableView* tv); // il risultato viene liberato in mem prima, lo fa chi chiama questa funzione

void table_view_scroll_to(TableView* tv, int row); // scroll alla riga specifica

#endif