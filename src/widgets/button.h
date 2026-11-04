/*
SCOPO: Widget bottone cliccabile con label testuale, icona opzionale
       e suggerimento della scorciatoia da tastiera.
       Supporta hover, pressed, disabled e tre stili (primario, secondario, ghost).
       Il callback on_click viene chiamato al rilascio del mouse.
*/

#ifndef BUTTON_H
#define BUTTON_H

#include "widget.h"

typedef enum {
    BUTTON_STYLE_SECONDARY = 0,  // superficie rialzata con bordo
    BUTTON_STYLE_PRIMARY,        // colore d'accento pieno
    BUTTON_STYLE_GHOST,          // trasparente, visibile solo in hover
} ButtonStyle;

typedef enum {
    BUTTON_ICON_NONE = 0,
    BUTTON_ICON_PLAY,
    BUTTON_ICON_CHECK,
    BUTTON_ICON_UNDO,
    BUTTON_ICON_REFRESH,
    BUTTON_ICON_TRASH,
    BUTTON_ICON_STOP,
} ButtonIcon;

typedef struct {
    Widget base; // sempre per primo nella definizione di tipo dei widget

    char label[256]; // testo dentro il bottone
    char hint[32];   // scorciatoia mostrata a destra (es. "F5"), vuota = nessuna
    int  font_size;  // dimensione label

    ButtonStyle style;
    ButtonIcon  icon;
    Color       icon_color;   // colore dell'icona (alpha 0 = come il testo)

    Color color_bg;
    Color color_hover;
    Color color_pressed;
    Color color_text;
    Color color_border;
    int border_radius; // per angoli arrotondati

    void (*on_click)(void* user_data); // callback : chiamata sul click, user_data = base.user_data, passato per contesto
} Button;

// -- metodi della struct button --

Button* button_create(int x, int y, int w, int h, const char* label, void (*on_click)(void* user_data), void* user_data);
void button_set_label(Button* btn, const char* label);
void button_set_enabled(Button* btn, bool enabled);
void button_set_style(Button* btn, ButtonStyle style);
void button_set_icon(Button* btn, ButtonIcon icon, Color color);
void button_set_hint(Button* btn, const char* hint);

// larghezza ideale per contenere icona, testo e hint
int  button_preferred_width(Button* btn, PlatformWindow* win);

#endif
