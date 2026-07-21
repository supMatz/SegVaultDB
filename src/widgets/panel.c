#include "panel.h"

static void panel_draw(Widget* self, PlatformWindow* win) {
    Panel* p = (Panel*)self;
    platform_fill_rect(win, self->bounds, p->color_bg);
    if (p->border_width > 0)
        platform_draw_rect(win, self->bounds, p->color_border, p->border_width);
    for (int i = 0; i < p->num_children; i++)
        widget_draw(p->children[i], win);
}

static bool panel_handle_event(Widget* self, sEvent* evt) {
    Panel* p = (Panel*)self;
    for (int i = p->num_children - 1; i >= 0; i--) {
        if (widget_handle_event(p->children[i], evt))
            return true;
    }
    return false;
}

static void panel_destroy_fn(Widget* self) {
    Panel* p = (Panel*)self;
    for (int i = 0; i < p->num_children; i++)
        widget_destroy(p->children[i]);
}

Panel* panel_create(int x, int y, int w, int h) {
    Panel* p = SV_ALLOC(Panel);
    if (!p) return NULL;

    p->base.type         = WIDGET_PANEL;
    p->base.state        = WIDGET_STATE_NORMAL;
    p->base.bounds       = (Rect){x, y, w, h};
    p->base.visible      = true;
    p->base.enabled      = true;
    p->base.draw         = panel_draw;
    p->base.handle_event = panel_handle_event;
    p->base.destroy      = panel_destroy_fn;

    p->num_children = 0;
    p->color_bg     = COLOR_PANEL;
    p->color_border = COLOR_BORDER;
    p->border_width = 0;
    p->padding      = 4;

    return p;
}

void panel_add_child(Panel* p, Widget* child) {
    if (!p || !child || p->num_children >= PANEL_MAX_CHILDREN) return;
    p->children[p->num_children++] = child;
}

bool panel_remove_child(Panel* p, Widget* child) {
    if (!p || !child) return false;
    for (int i = 0; i < p->num_children; i++) {
        if (p->children[i] == child) {
            for (int j = i; j < p->num_children - 1; j++)
                p->children[j] = p->children[j + 1];
            p->num_children--;
            return true;
        }
    }
    return false;
}

void panel_clear(Panel* p) {
    if (!p) return;
    for (int i = 0; i < p->num_children; i++)
        widget_destroy(p->children[i]);
    p->num_children = 0;
}
