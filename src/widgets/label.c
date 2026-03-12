#include "label.h"
#include "widget.h"
#include <string.h>

static void label_draw(Widget* self, PlatformWindow* win) {
    Label* lbl = (Label*)self;

    int text_w = platform_measure_text(win, lbl->text, lbl->font_size);
    int x;

    switch(lbl->align) {
        case LABEL_ALIGN_CENTER :
            x = self->bounds.x + (self->bounds.w - text_w) / 2;
            break;
        case LABEL_ALIGN_RIGHT:
            x = self->bounds.x + self->bounds.w - text_w;
            break;
        default: // LEFT
            x = self->bounds.x;
            break;
    }

    int y = self->bounds.y + (self->bounds.h - lbl->font_size) / 2;
    platform_draw_text(win, lbl->text,(Point){x, y}, lbl->color, lbl->font_size);
}

static bool label_handle_event(Widget* self, sEvent* evt) {
    (void)self; (void)evt;
    return false; // le label non hanno eventi
}

static void label_destroy(Widget* self) { (void)self; }

Label* label_create(int x, int y, int w, int h, const char* text, int font_size, Color color) {
    Label* lbl = SV_ALLOC(Label);

    if(!lbl) return NULL;

    lbl->base.type = WIDGET_LABEL;
    lbl->base.state = WIDGET_STATE_NORMAL;
    lbl->base.bounds = (Rect) {x,y,h,w};

    lbl->base.visible      = true;
    lbl->base.enabled      = false; // Label non interagisce
    lbl->base.draw         = label_draw;
    lbl->base.handle_event = label_handle_event;
    lbl->base.destroy      = label_destroy;
 
    strncpy(lbl->text, text, sizeof(lbl->text) - 1);
    lbl->font_size = font_size;
    lbl->color     = color;
    lbl->align     = LABEL_ALIGN_LEFT;
    lbl->clip      = true;
 
    return lbl;
}

void label_set_text(Label* lbl, const char* text) {
    if (!lbl || !text) return;
    strncpy(lbl->text, text, sizeof(lbl->text) - 1);
}
 
void label_set_color(Label* lbl, Color color) {
    if (!lbl) return;
    lbl->color = color;
}