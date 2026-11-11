/*
SCOPO: campo di input a riga singola (barra di ricerca, "vai alla riga", percorsi file).
       Supporta: cursore UTF-8, selezione con Shift e mouse, Ctrl+A/C/X/V,
       Ctrl+Backspace, Home/End, placeholder e scroll orizzontale del testo.
       Invio e Esc NON sono gestiti qui: li gestisce il contenitore.
*/

#ifndef LINE_EDIT_H
#define LINE_EDIT_H

#include "widget.h"

#define LINE_EDIT_MAX 512

typedef struct LineEdit {
    Widget base;

    char text[LINE_EDIT_MAX];
    int  len;
    int  cursor;           // offset in byte
    int  anchor;           // altro estremo della selezione (-1 = nessuna)
    int  scroll_x;         // pixel

    const char* placeholder;
    bool  error;           // bordo rosso (es. nessun risultato)
    bool  secret;          // password: si vede un * per byte, niente copia
    bool  dragging;
    uint64_t blink_ms;
    bool  cursor_visible;

    PlatformWindow* win;

    // chiamato dopo ogni modifica del testo
    void (*on_change)(struct LineEdit* le, void* user_data);
} LineEdit;

LineEdit* line_edit_create(const char* placeholder);

void line_edit_set_text(LineEdit* le, const char* text);   // sposta il cursore alla fine
void line_edit_select_all(LineEdit* le);
void line_edit_focus(LineEdit* le, bool focused);
bool line_edit_focused(const LineEdit* le);
bool line_edit_tick(LineEdit* le, uint64_t now_ms);         // blink: true se serve ridisegnare

#endif
