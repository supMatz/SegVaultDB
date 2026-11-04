/*
SCOPO: Scrollbar verticale od orizzontale (stile "overlay" sottile).
       Usata da editor, table_view e tree_view per lo scroll.
       Si nasconde automaticamente quando tutto il contenuto è visibile.
*/

#ifndef SCROLLBAR_H
#define SCROLLBAR_H

#include "widget.h"

typedef enum { SCROLLBAR_VERTICAL, SCROLLBAR_HORIZONTAL } ScrollbarDir;

typedef struct {
    Widget       base;
    ScrollbarDir direction;

    int   total;        // numero totale di unità (righe, pixel, ecc.)
    int   visible;      // unità visibili nella viewport
    int   position;     // posizione corrente (0 .. total - visible)
    int   step;         // unità per scatto della rotella

    bool  dragging;     // true = l'utente sta trascinando il thumb
    int   drag_start;   // distanza del mouse dall'inizio del thumb al momento della presa

    Color color_track;  // colore dello sfondo (visibile solo in hover)
    Color color_thumb;  // colore del thumb
    Color color_thumb_hover;

    void (*on_scroll)(int new_position, void* user_data);
} Scrollbar;

Scrollbar* scrollbar_create(int x, int y, int w, int h, ScrollbarDir dir);

void scrollbar_apply_theme(Scrollbar* sb); // ricarica i colori dal tema attivo
void scrollbar_set_range(Scrollbar* sb, int total, int visible);
void scrollbar_set_position(Scrollbar* sb, int pos);
bool scrollbar_is_needed(Scrollbar* sb);

#endif
