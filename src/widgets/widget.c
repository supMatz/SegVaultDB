/* 
SCOPO: Implementazione delle funzioni generiche su SVWidget.
       Le funzioni specifiche (draw, handle_event, destroy)
       sono implementate nei file di ogni widget concreto.
*/

#include "widget.h"

void widget_draw(Widget *w, PlatformWindow *win) {
    // salta se il widget non ha funzione draw o è nascosto
    if(!w || !w->visible || !w->draw) return;
    w->draw(w, win);
}

bool widget_handle_event(Widget *w, sEvent *evt) {
    // salta se il widget non ha handler o è disabilitato
    if (!w || !w->visible || !w->enabled || !w->handle_event) return false;
    return w->handle_event(w, evt);
}

void widget_destroy(Widget *w) {
    if(!w) return; // controllo sull'esistenza del widget
    if(w->destroy) w->destroy(w); // se ha metodo destroy lo distruggo
    free(w); // libero il puntatore in mem
}

bool widget_contains_point(Widget *w, int px, int py) {
    if(!w || !w->visible) return false;

    return px >= w->bounds.x && // se sta dopo la x iniziale del widget
           px <= w->bounds.x + w->bounds.w && // controlla se sta all interno della larghezza del widget
           py >= w->bounds.y && // stessa cosa per le y...
           py <= w->bounds.h;
}

void widget_update_hover(Widget *w, int mx, int my) { // mouse x e y
    if(!w || !w->enabled) return;
    if(widget_contains_point(w, mx, my)){
        if(w->state != WIDGET_STATE_PRESSED) // solo se gia premuto, non sovrascrive pressed ad hover
            w->state = WIDGET_STATE_HOVER; // mouse entrato
    } 
    else {
        if(w->state == WIDGET_STATE_HOVER) // mouse uscito
            w->state = WIDGET_STATE_NORMAL; // torna normale
    }
}

