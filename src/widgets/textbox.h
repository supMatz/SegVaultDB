/*
SCOPO: Widget di input testo multilinea per l'editor SQL.
       Supporta: cursore UTF-8, selezione (mouse e tastiera), copia/taglia/incolla,
       annulla/ripeti, syntax highlighting SQL, numeri di riga, evidenziazione
       della riga corrente, dello statement corrente e delle parentesi,
       auto-indentazione, auto-chiusura di parentesi e apici, commento di righe,
       zoom del font, scroll con rotella e marcatore della riga di errore.

       Scorciatoie principali:
         Ctrl+Enter        esegue la selezione o lo statement sotto il cursore
         Ctrl+A/C/X/V      seleziona tutto / copia / taglia / incolla
         Ctrl+Z, Ctrl+Y    annulla / ripeti (anche Ctrl+Shift+Z)
         Ctrl+D            duplica riga
         Ctrl+/            commenta / decommenta le righe selezionate
         Tab / Shift+Tab   indenta / de-indenta
         Ctrl + rotella    zoom (anche Ctrl + '+' / '-' / '0')
*/

#ifndef TEXTBOX_H
#define TEXTBOX_H

#include "widget.h"
#include "platform_compat.h"

#define TEXTBOX_MAX_LEN   SV_MAX_SQL_LEN  // lunghezza massima del testo (64 KB)
#define TEXTBOX_UNDO_CAP  256             // passi di annulla memorizzati

typedef struct {
    char* text;
    int   cursor;
    int   anchor;
} TextSnapshot;

typedef struct {
    Widget base;

    // contenuto (UTF-8)
    char* text;
    int   text_len;
    int   text_cap;        // capacità allocata nel buffer

    // indice di inizio di ogni riga (ricostruito ad ogni modifica)
    int*  line_starts;
    int   num_lines;
    int   lines_cap;

    // cursore e selezione (offset in byte)
    int   cursor_pos;      // posizione cursore nel testo
    int   cursor_line;     // riga del cursore (0-based)
    int   cursor_col;      // colonna del cursore in caratteri (0-based)
    int   sel_anchor;      // altro estremo della selezione (-1 = nessuna selezione)
    int   pref_col;        // colonna "desiderata" per i movimenti su/giù
    uint64_t cursor_blink; // timestamp dell'ultimo blink del cursore
    bool  cursor_visible;

    // mouse
    bool     dragging;
    uint64_t last_click_ms;
    int      click_count;
    int      last_click_x, last_click_y;

    // scroll
    int scroll_x;          // pixel scrollati orizzontalmente
    int scroll_y;          // prima riga visibile

    // annulla / ripeti
    TextSnapshot undo[TEXTBOX_UNDO_CAP];
    int          undo_count;
    TextSnapshot redo[TEXTBOX_UNDO_CAP];
    int          redo_count;
    int          last_edit_kind;
    uint64_t     last_edit_ms;

    // metriche (aggiornate in draw)
    int font_size;
    int line_height;       // altezza riga in pixel
    int char_w;            // larghezza di un carattere monospace
    int gutter_w;          // larghezza colonna numeri di riga
    int pad_top;

    // stile
    bool show_line_nums;
    const char* placeholder;

    // riga da evidenziare come errore (-1 = nessuna)
    int error_line;

    PlatformWindow* win;   // per appunti e misura del testo

    // -- callback --
    void (*on_change)(const char* text, void* user_data);
    void (*on_execute)(const char* text, void* user_data); // Ctrl+Enter: testo da eseguire
} TextBox;

TextBox* textbox_create(int x, int y, int w, int h); // editor SQL

void textbox_set_text(TextBox* tb, const char* text); // imposta il testo (annullabile)

const char* textbox_get_text(TextBox* tb); // ritorna il testo corrente della tb

void textbox_clear(TextBox* tb); // pulizia contenuto tb

void textbox_insert(TextBox* tb, const char* text); // inserisce testo al cursore (sostituisce la selezione)

bool textbox_tick(TextBox* tb, uint64_t now_ms); // blink del cursore; true se serve ridisegnare

void textbox_focus(TextBox* tb, bool focused);

bool  textbox_has_selection(TextBox* tb);
char* textbox_get_selection(TextBox* tb);      // malloc'd, free() del chiamante; NULL se vuota
void  textbox_get_selection_range(TextBox* tb, int* start, int* end);

// testo da eseguire con Ctrl+Enter: la selezione o lo statement sotto il cursore.
// ritorna l'offset di inizio nel testo (per mappare gli errori sulle righe), -1 se vuoto
int   textbox_get_run_text(TextBox* tb, char** out);

int   textbox_line_of_offset(TextBox* tb, int offset);
int   textbox_visible_lines(TextBox* tb);
void  textbox_set_error_line(TextBox* tb, int line);   // -1 per rimuovere
void  textbox_scroll_to_line(TextBox* tb, int line);

#endif
