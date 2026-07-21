#include "splitter.h"

static void splitter_draw(Widget* self, PlatformWindow* win) {
    Splitter* sp = (Splitter*)self;
    Color c = sp->dragging ? sp->color_drag
             : (self->state == WIDGET_STATE_HOVER ? sp->color_hover : sp->color_bar);
    platform_fill_rect(win, self->bounds, c);
}

static bool splitter_handle_event(Widget* self, sEvent* evt) {
    Splitter* sp = (Splitter*)self;

    switch (evt->type) {
        case EVT_MOUSE_DOWN:
            if (widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                sp->dragging = true;
                if (sp->direction == SPLITTER_HORIZONTAL)
                    sp->drag_offset = evt->mouse_y - self->bounds.y;
                else
                    sp->drag_offset = evt->mouse_x - self->bounds.x;
                return true;
            }
            break;

        case EVT_MOUSE_UP:
            if (sp->dragging) {
                sp->dragging = false;
                if (sp->on_drag_end)
                    sp->on_drag_end(sp->position, self->user_data);
                return true;
            }
            break;

        case EVT_MOUSE_MOVE:
            widget_update_hover(self, evt->mouse_x, evt->mouse_y);
            if (sp->dragging) {
                int new_pos;
                if (sp->direction == SPLITTER_HORIZONTAL)
                    new_pos = evt->mouse_y - sp->drag_offset;
                else
                    new_pos = evt->mouse_x - sp->drag_offset;
                sp->position = SV_CLAMP(new_pos, sp->min_pos, sp->max_pos);
                // Update bounds position to show the splitter at new spot
                if (sp->direction == SPLITTER_HORIZONTAL)
                    self->bounds.y = sp->position;
                else
                    self->bounds.x = sp->position;
                return true;
            }
            break;

        default: break;
    }
    return false;
}

static void splitter_destroy_fn(Widget* self) { (void)self; }

Splitter* splitter_create(int x, int y, int w, int h, SplitterDir dir) {
    Splitter* sp = SV_ALLOC(Splitter);
    if (!sp) return NULL;

    sp->base.type         = WIDGET_SPLITTER;
    sp->base.state        = WIDGET_STATE_NORMAL;
    sp->base.bounds       = (Rect){x, y, w, h};
    sp->base.visible      = true;
    sp->base.enabled      = true;
    sp->base.draw         = splitter_draw;
    sp->base.handle_event = splitter_handle_event;
    sp->base.destroy      = splitter_destroy_fn;

    sp->direction  = dir;
    sp->thickness  = (dir == SPLITTER_HORIZONTAL) ? h : w;
    sp->min_pos    = 0;
    sp->max_pos    = 10000;
    sp->position   = (dir == SPLITTER_HORIZONTAL) ? y : x;
    sp->dragging   = false;
    sp->drag_offset = 0;

    sp->color_bar   = (Color){50, 50, 65, 255};
    sp->color_hover = (Color){80, 80, 100, 255};
    sp->color_drag  = (Color){100, 210, 255, 255};

    sp->on_drag_end = NULL;

    return sp;
}
