/* 
SCOPO: Widget bottone cliccabile con label testuale.
       Supporta hover, pressed, disabled.
       Il callback on_click viene chiamato al rilascio del mouse.
*/

#ifndef BUTTON_H
#define BUTTON_H

#include "widget.h"
typedef struct {
    Widget base; // sempre per primo nella definizione di tipo dei widget
    
    char label[256]; // testo dentro il bottone
    int font_size; // dimensione label

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

#endif