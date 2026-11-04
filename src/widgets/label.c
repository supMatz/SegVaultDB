#include "label.h"
#include "widget.h"
#include "theme.h"
#include <string.h>

static void label_draw(Widget* self, PlatformWindow* win) {
    Label* lbl = (Label*)self;
    Rect b = self->bounds;

    int text_w = platform_measure_text_ex(win, lbl->text, -1, lbl->font_size, lbl->style);
    if (lbl->clip && text_w > b.w) text_w = b.w;
    int x;

    switch (lbl->align) {
        case LABEL_ALIGN_CENTER: x = b.x + (b.w - text_w) / 2; break;
        case LABEL_ALIGN_RIGHT:  x = b.x + b.w - text_w;       break;
        default:                 x = b.x;                      break;
    }

    int y = ui_text_y(win, b, lbl->font_size, lbl->style);
    if (lbl->clip)
        ui_text_clipped(win, lbl->text, x, y, b.w, lbl->color, lbl->font_size, lbl->style);
    else
        platform_draw_text_ex(win, lbl->text, -1, (Point){x, y}, lbl->color, lbl->font_size, lbl->style);
}

static bool label_handle_event(Widget* self, sEvent* evt) {
    (void)self; (void)evt;
    return false; // le label non hanno eventi
}

static void label_destroy(Widget* self) { (void)self; }

Label* label_create(int x, int y, int w, int h, const char* text, int font_size, Color color) {
    Label* lbl = SV_ALLOC(Label);
    if (!lbl) return NULL;

    lbl->base.type         = WIDGET_LABEL;
    lbl->base.state        = WIDGET_STATE_NORMAL;
    lbl->base.bounds       = (Rect){x, y, w, h};
    lbl->base.visible      = true;
    lbl->base.enabled      = false; // Label non interagisce
    lbl->base.draw         = label_draw;
    lbl->base.handle_event = label_handle_event;
    lbl->base.destroy      = label_destroy;

    strncpy(lbl->text, text ? text : "", sizeof(lbl->text) - 1);
    lbl->font_size = font_size;
    lbl->color     = color;
    lbl->align     = LABEL_ALIGN_LEFT;
    lbl->style     = FONT_UI;
    lbl->clip      = true;

    return lbl;
}

void label_set_text(Label* lbl, const char* text) {
    if (!lbl || !text) return;
    strncpy(lbl->text, text, sizeof(lbl->text) - 1);
    lbl->text[sizeof(lbl->text) - 1] = '\0';
}

void label_set_color(Label* lbl, Color color) {
    if (!lbl) return;
    lbl->color = color;
}

void label_set_style(Label* lbl, FontStyle style) {
    if (!lbl) return;
    lbl->style = style;
}
