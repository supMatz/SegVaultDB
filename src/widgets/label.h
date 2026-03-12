/* 
SCOPO: Widget testo non interattivo.
       Usato per titoli, etichette, status bar, ecc.
*/

#ifndef LABEL_H
#define LABEL_H

#include "widget.h"

typedef enum {
    LABEL_ALIGN_LEFT,
    LABEL_ALIGN_CENTER,
    LABEL_ALIGN_RIGHT,
} LabelAlign;

typedef struct {
    Widget base;
    char text[512]; // testo da mostrare
    int font_size;
    Color color;
    LabelAlign align;
    bool clip; // se true taglia il testo ai bordi per non uscires
} Label;

Label* label_crate(int x, int y, int w, int h, const char* text, int font_size, Color color);
void label_set_text(Label* lbl, const char* text);
void label_set_color(Label* lbl, Color color);

#endif