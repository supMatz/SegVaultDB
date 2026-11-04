/*
SCOPO: Implementazione del widget bottone.
       Gestisce disegno, hover, click e stato disabled.
*/

#include "button.h"
#include "widget.h"
#include "theme.h"
#include <string.h>

#define BTN_PAD     12
#define BTN_ICON    14
#define BTN_GAP     8

// -- funzioni interne (non esportate)--

static void draw_icon(PlatformWindow* win, ButtonIcon icon, int cx, int cy, Color c) {
    switch (icon) {
        case BUTTON_ICON_PLAY:    ui_icon_play(win, cx, cy, BTN_ICON - 2, c); break;
        case BUTTON_ICON_CHECK:   ui_icon_check(win, cx, cy, BTN_ICON, c);    break;
        case BUTTON_ICON_UNDO:    ui_icon_undo(win, cx, cy, BTN_ICON, c);     break;
        case BUTTON_ICON_REFRESH: ui_icon_refresh(win, cx, cy, BTN_ICON, c);  break;
        case BUTTON_ICON_TRASH:   ui_icon_trash(win, cx, cy, BTN_ICON, c);    break;
        case BUTTON_ICON_STOP: {
            int s = BTN_ICON - 6;
            platform_fill_rect_rounded(win, (Rect){cx - s / 2, cy - s / 2, s, s}, c, 2);
            break;
        }
        default: break;
    }
}

static int hint_width(PlatformWindow* win, Button* btn) {
    if (!btn->hint[0]) return 0;
    return platform_measure_text_ex(win, btn->hint, -1, TH_FONT_SMALL, FONT_MONO) + 10;
}

static void button_draw(Widget* self, PlatformWindow* win) {
    Button* btn = (Button*)self;
    Rect b = self->bounds;
    bool hover   = self->state == WIDGET_STATE_HOVER;
    bool pressed = self->state == WIDGET_STATE_PRESSED;

    // scelta colori in base allo stato
    Color bg = btn->color_bg, text = btn->color_text;
    if (!self->enabled) {
        bg   = (btn->style == BUTTON_STYLE_GHOST) ? (Color){0, 0, 0, 0} : TH_SURFACE_2;
        text = TH_TEXT_FAINT;
    } else if (pressed) bg = btn->color_pressed;
    else if (hover)     bg = btn->color_hover;

    if (btn->style == BUTTON_STYLE_SECONDARY) {
        Color border = (hover && self->enabled) ? TH_BORDER_STRONG : btn->color_border;
        ui_round_box(win, b, bg, border, btn->border_radius);
    } else {
        platform_fill_rect_rounded(win, b, bg, btn->border_radius);
        if (btn->style == BUTTON_STYLE_PRIMARY && self->enabled && !pressed) {
            // riflesso sul bordo superiore
            platform_fill_rect(win, (Rect){b.x + btn->border_radius, b.y, b.w - 2 * btn->border_radius, 1},
                               (Color){255, 255, 255, 40});
        }
    }

    // contenuto centrato: [icona] [testo] [hint]
    FontStyle fs = (btn->style == BUTTON_STYLE_PRIMARY) ? FONT_UI_BOLD : FONT_UI;
    int text_w = btn->label[0] ? platform_measure_text_ex(win, btn->label, -1, btn->font_size, fs) : 0;
    int icon_w = btn->icon ? BTN_ICON + (text_w ? BTN_GAP : 0) : 0;
    int hint_w = hint_width(win, btn);
    int content = icon_w + text_w + (hint_w ? BTN_GAP + hint_w : 0);
    int x = b.x + (b.w - content) / 2;
    int cy = b.y + b.h / 2;
    if (pressed) cy += 1;

    if (btn->icon) {
        Color ic = (btn->icon_color.a && self->enabled) ? btn->icon_color : text;
        draw_icon(win, btn->icon, x + BTN_ICON / 2, cy, ic);
        x += icon_w;
    }
    if (text_w) {
        int ty = ui_text_y(win, (Rect){b.x, b.y + (pressed ? 1 : 0), b.w, b.h}, btn->font_size, fs);
        platform_draw_text_ex(win, btn->label, -1, (Point){x, ty}, text, btn->font_size, fs);
        x += text_w;
    }
    if (hint_w) {
        x += BTN_GAP;
        Rect hr = {x, cy - 9, hint_w, 18};
        Color hbg = (btn->style == BUTTON_STYLE_PRIMARY) ? ui_alpha(TH_ON_ACCENT, 38) : TH_ELEVATED_PRESS;
        platform_fill_rect_rounded(win, hr, hbg, 4);
        Color htc = (btn->style == BUTTON_STYLE_PRIMARY) ? ui_alpha(TH_ON_ACCENT, 220) : TH_TEXT_DIM;
        if (!self->enabled) htc = TH_TEXT_FAINT;
        platform_draw_text_ex(win, btn->hint, -1,
                              (Point){x + 5, ui_text_y(win, hr, TH_FONT_SMALL, FONT_MONO)},
                              htc, TH_FONT_SMALL, FONT_MONO);
    }
}

static bool button_handle_event(Widget* self, sEvent* evt) {
    Button* btn = (Button*)self;

    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            widget_update_hover(self, evt->mouse_x, evt->mouse_y);
            return false;

        case EVT_MOUSE_DOWN:
            if (evt->mouse_button == SV_MOUSE_LEFT && widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                self->state = WIDGET_STATE_PRESSED;
                return true;
            }
            break;

        case EVT_MOUSE_UP:
            if (self->state == WIDGET_STATE_PRESSED) {
                if (widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                    self->state = WIDGET_STATE_HOVER;
                    if (btn->on_click) btn->on_click(self->user_data);
                } else {
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

Button* button_create(int x, int y, int w, int h, const char* label, void (*on_click)(void* user_data), void* user_data) {
    Button* btn = SV_ALLOC(Button);
    if (!btn) return NULL;

    // inizializzazione base widget
    btn->base.type         = WIDGET_BUTTON;
    btn->base.state        = WIDGET_STATE_NORMAL;
    btn->base.bounds       = (Rect){x, y, w, h};
    btn->base.visible      = true;
    btn->base.enabled      = true;
    btn->base.user_data    = user_data;
    btn->base.draw         = button_draw;
    btn->base.handle_event = button_handle_event;
    btn->base.destroy      = button_destroy;

    // campi specifici del bottone
    strncpy(btn->label, label ? label : "", sizeof(btn->label) - 1);
    btn->font_size     = TH_FONT_UI;
    btn->on_click      = on_click;
    btn->border_radius = TH_RADIUS;
    button_set_style(btn, BUTTON_STYLE_SECONDARY);

    return btn;
}

void button_set_style(Button* btn, ButtonStyle style) {
    if (!btn) return;
    btn->style = style;
    switch (style) {
        case BUTTON_STYLE_PRIMARY:
            btn->color_bg      = TH_ACCENT;
            btn->color_hover   = TH_ACCENT_HOVER;
            btn->color_pressed = TH_ACCENT_PRESS;
            btn->color_text    = TH_ON_ACCENT;
            btn->color_border  = TH_ACCENT;
            break;
        case BUTTON_STYLE_GHOST:
            btn->color_bg      = (Color){0, 0, 0, 0};
            btn->color_hover   = TH_ELEVATED;
            btn->color_pressed = TH_ELEVATED_PRESS;
            btn->color_text    = TH_TEXT_MUTED;
            btn->color_border  = (Color){0, 0, 0, 0};
            break;
        default:
            btn->color_bg      = TH_ELEVATED;
            btn->color_hover   = TH_ELEVATED_HOVER;
            btn->color_pressed = TH_ELEVATED_PRESS;
            btn->color_text    = TH_TEXT;
            btn->color_border  = TH_BORDER_STRONG;
            break;
    }
}

void button_set_icon(Button* btn, ButtonIcon icon, Color color) {
    if (!btn) return;
    btn->icon = icon;
    btn->icon_color = color;
}

void button_set_hint(Button* btn, const char* hint) {
    if (!btn) return;
    strncpy(btn->hint, hint ? hint : "", sizeof(btn->hint) - 1);
}

int button_preferred_width(Button* btn, PlatformWindow* win) {
    FontStyle fs = (btn->style == BUTTON_STYLE_PRIMARY) ? FONT_UI_BOLD : FONT_UI;
    int text_w = btn->label[0] ? platform_measure_text_ex(win, btn->label, -1, btn->font_size, fs) : 0;
    int icon_w = btn->icon ? BTN_ICON + (text_w ? BTN_GAP : 0) : 0;
    int hint_w = hint_width(win, btn);
    int w = BTN_PAD * 2 + icon_w + text_w + (hint_w ? BTN_GAP + hint_w : 0);
    if (!text_w && !hint_w) w = BTN_ICON + 16; // bottone solo icona
    return w;
}

void button_set_label(Button *btn, const char *label) {
    if (!btn || !label) return;
    strncpy(btn->label, label, sizeof(btn->label) - 1); // -1 per lo spazio per il terminatore '\0'
}

void button_set_enabled(Button *btn, bool enabled) {
    if (!btn) return;
    btn->base.enabled = enabled;
    btn->base.state   = enabled ? WIDGET_STATE_NORMAL : WIDGET_STATE_DISABLED;
}
