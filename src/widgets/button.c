/* 
SCOPO: Implementazione del widget bottone.
       Gestisce disegno, hover, click e stato disabled.
*/

#include "button.h"
#include "widget.h"
#include <string.h>

// -- funzioni interne (non esportate)--

static void button_draw(Widget* self, PlatformWindow* win){
       Button* btn = (Button*)self;

       // scelta colori in base allo stato
       Color bg;
       if(!self->enabled) {
              // disabiliato : sfondo spento
              bg = (Color){50, 50, 55, 255};
       }
       else if(self->state == WIDGET_STATE_PRESSED) {
              bg = btn->color_pressed;
       }
       else if(self->state == WIDGET_STATE_HOVER) {
              bg = btn->color_hover;
       }
       else{
              bg = btn->color_bg;
       }

       // disegno sfondo bottone
       platform_fill_rect(win, self->bounds, bg);

       // disegno il bordo
       platform_draw_rect(win, self->bounds, btn->color_border, 1);

       // calcolo per posizionare il testo al centro
       int text_w = platform_measure_text(win, btn->label, btn->font_size);
       int text_x = self->bounds.x + (self->bounds.w - text_w) / 2;
       int text_y = self->bounds.y + self->bounds.y / 2 + 1;

       // se non è enabled metto un colore piu scuro dello standard
       Color tc = self->enabled ? btn->color_text : (Color) {100, 100, 110, 255};

       // disegno del testo nel bottone
       platform_draw_text(win, btn->label, (Point){text_x, text_y}, tc, btn->font_size);
}

static bool button_handle_event(Widget* self, sEvent* evt) {
       Button* btn = (Button*)self;

       switch(evt->type) {
              case EVT_MOUSE_MOVE : widget_update_hover(self, evt->mouse_x, evt->mouse_y); return false; break;
              case EVT_MOUSE_DOWN : 
                     if(evt->mouse_button == 1 && widget_contains_point(self, evt->mouse_x, evt->mouse_y)){
                            self->state = WIDGET_STATE_PRESSED;
                            return true;
                     }
                     break;
              case EVT_MOUSE_UP : 
                     if(self->state == WIDGET_STATE_PRESSED) {
                            // rilascio : controlla se il mouse è ancora sopra
                            if(widget_contains_point(self, evt->mouse_x, evt->mouse_y)){
                                   self->state = WIDGET_STATE_HOVER;
                            }
                            else {
                                   self->state = WIDGET_STATE_NORMAL;
                            }
                            return true;
                     }
                     break;
              default:
                     break;
       }
       return false;
}

static void button_destroy(Widget* self) {
       // niente da liberare : label è un array statico, free(self) è fatto da widget_destroy
       (void)self;
}

// -- API pubblica --

Button* button_create(int x, int y, int w, int h, const char* label,void (*on_click)(void* user_data), void* user_data) {
       Button* btn = SV_ALLOC(Button);
       if(!btn) return NULL;

       // inizializzazione base widget
       btn->base.type = WIDGET_BUTTON;
       btn->base.state = WIDGET_STATE_NORMAL;
       btn->base.bounds = (Rect) {x, y, h, w};
       btn->base.visible = true;
       btn->base.enabled = true;
       btn->base.user_data = user_data;
       btn->base.draw = button_draw;
       btn->base.handle_event = button_handle_event;
       btn->base.destroy = button_destroy;

       // campi specifici del bottone
       strncpy(btn->label, label, sizeof(btn->label) - 1 );
       btn->font_size = 14;
       btn->on_click = on_click;

       // colori default
       btn->color_bg      = (Color){45,  45,  52,  255};
       btn->color_pressed = (Color){30,  80,  120, 255};
       btn->color_hover   = (Color){60,  60,  70,  255};
       btn->color_text    = (Color){212, 212, 212, 255};
       btn->color_border  = (Color){70,  70,  80,  255};
       btn->border_radius = 4;

       return btn;
} 

void button_set_label(Button *btn, const char *label) { 
       if(!btn || !label) return;
       strncpy(btn->label, label, sizeof(btn->label) - 1 ); // -1 per lo spazio per il terminatore '\0'
}

void button_set_enabled(Button *btn, bool enabled) {
       if(!btn) return;
       btn->base.enabled = enabled;
       
       if(!enabled) btn->base.state = WIDGET_STATE_DISABLED;
       else btn->base.state = WIDGET_STATE_NORMAL;
}