/* 
SCOPO: Widget di input testo multilinea per l'editor SQL.
       Supporta: cursore, selezione, copia/incolla, scroll,
       syntax highlighting base (keyword SQL in colore diverso).
*/

#ifndef TEXTBOX_H
#define TEXTBOX_H
 
#include "widget.h"
#include <sys/types.h>

#define TEXTBOX_MAX_LEN 65536 // 64KB id lunghezza massima
#define TEXTBOX_MAX_LINES 4096 // num max di righe

typedef struct {
    Widget base;

    // contenuto
    char* text;
    int text_len;
    int text_cap; // capacità allocata nel buffer

    // cursore
    int cursor_pos; // posizione cursore nel testo (indice byte)
    int cursor_line; // numero di linea del cursore
    int cursor_col; // numero di colonna dove si trova il cursore
    uint64_t cursor_blink; // timestamp dell'ultimo blink del cursore (per animazione)
    bool cursor_visible; // se visibile

    // selezione testo
    int      sel_start;     // inizio selezione (-1 se nessuna selezione)
    int      sel_end;       // fine selezione

    // scroll
    int scroll_x; // pixel scrollati orizzontalmente
    int scroll_y; // ... e verticalmente

    // stile
    int font_size; 
    int line_height; // altezza riga in un pixel
    Color color_bg;
    Color color_text;
    Color color_cursor;
    Color color_select;
    Color color_line_num; // numeri di riga
    Color color_keyword; // keyword SQL 
    Color color_string; // stringhe SQL
    Color color_number; // valori numerici
    Color color_comment; // commenti
    
    bool show_line_nums; // mostra numeri di riga

    // -- callback --
    void (*on_change)(const char* text, void* user_data);
    void (*on_execute)(const char* text, void* user_data); // quando eseguo con [Ctrl + Enter]
} TextBox;

TextBox* textbox_create(int x, int y, int h, int w); // editor SQL

void textbox_set_text(TextBox* tb, const char* text); // imposta il testo

const char* textbox_get_text(TextBox* tb); // ritorna il testo corrente della tb

void textbox_clear(TextBox* tb); // pulizia contenuto tb

void textbox_insert(TextBox* tb, const char* text); // inserisce testo dove sta il cursore

void textbox_tick(TextBox* tb, uint64_t now_ms); // aggiorna il cursore (da chiamare ogni frame per il blink)

#endif