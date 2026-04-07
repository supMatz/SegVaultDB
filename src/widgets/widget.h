/*
SCOPO: Struttura base da cui derivano tutti i widget grafici.
       Ogni widget (button, textbox, table_view, ecc.) inizia
       con un SVWidget come primo campo — questo permette di
       passare qualsiasi widget come SVWidget* alle funzioni
       generiche (pattern simile all'ereditarietà in C).
*/
#ifndef WIDGET_H
#define WIDGET_H

#include "../../include/common.h"
#include "../platform/platform.h" // sEvent, sKeyCode, sEventType

// tipo widget per distinguerlo a runtime
typedef enum {
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_TEXTBOX,
    WIDGET_TABLE_VIEW,
    WIDGET_TREE_VIEW,
    WIDGET_SCROLLBAR,
    WIDGET_PANEL,
    WIDGET_SPLITTER,
} WidgetType;

// stato visivo del widget
typedef enum {
    WIDGET_STATE_NORMAL,    // stato base
    WIDGET_STATE_HOVER,     // mouse sopra
    WIDGET_STATE_PRESSED,   // mouse premuto sopra
    WIDGET_STATE_FOCUSED,   // selezionato (es: textbox attiva)
    WIDGET_STATE_DISABLED,  // non interagibile, disabilitato
} WidgetState;

// struttura base di ogni widget, inclusa come primo campo in ogni widget
typedef struct Widget {
    WidgetType  type;       // Tipo del widget
    WidgetState state;      // Stato visivo corrente
    Rect bounds;     // Posizione e dimensioni (x, y, w, h)
    bool visible;    // Se false: non disegnato e non interagibile
    bool enabled;    // Se false: disegnato ma non interagibile
    void* user_data;  // Puntatore a dati custom (callback context)
 
    // -- funzioni virtuali -- ogni widget implementa le sue
    void (*draw)(struct Widget* self, PlatformWindow* win); // disegna il widget sulla finestra
    bool (*handle_event)(struct Widget* self, sEvent* evt); // gestisce un evento (mouse, tastiera, ecc.) ritorna true se l'evento è andato a buon termine (non passare ad altri)
    void (*destroy)(struct Widget* self); // dealloca la memoria del widget (lo distrugge)
} Widget;

// -- funzioni generiche di ogni widget --

void widget_draw(Widget* w, PlatformWindow* win);       // disegna il widget
bool widget_handle_event(Widget* w, sEvent* evt);       // passa un evento al widget, chiama self->handle_event
void widget_destroy(Widget* w);                         // distrugge il widget, chiama self->destroy e poi free al puntatore
bool widget_contains_point(Widget* w, int px, int py);  // controlla se un punto è contenuto in un widget
void widget_update_hover(Widget* w, int mx, int my);    // aggiorna lo stato hover/normal in base alla posizione del mouse

#endif