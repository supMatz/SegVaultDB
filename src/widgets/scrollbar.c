#include "scrollbar.h"

// calcola la posizione e dimensione del thumb in pixel
static void get_thumb_rect(Scrollbar* sb, Rect* out) {
    Rect b = sb->base.bounds;
    if (sb->total <= 0 || sb->total <= sb->visible) {
        // tutto visibile: thumb occupa tutta la traccia
        *out = b;
        return;
    }
    if (sb->direction == SCROLLBAR_VERTICAL) {
        int track_h  = b.h;
        
        int thumb_h  = SV_MAX(20, track_h * sb->visible / sb->total);

        int thumb_y  = b.y + (track_h - thumb_h) * sb->position / (sb->total - sb->visible);
        
        *out = (Rect){b.x, thumb_y, b.w, thumb_h};
    } else {
        int track_w  = b.w;
        int thumb_w  = SV_MAX(20, track_w * sb->visible / sb->total);
        int thumb_x  = b.x + (track_w - thumb_w) * sb->position
                       / (sb->total - sb->visible);
        *out = (Rect){thumb_x, b.y, thumb_w, b.h};
    }
}

static void scrollbar_draw(Widget* self, PlatformWindow* win) {
    Scrollbar* sb = (Scrollbar*)self;

    // traccia (sfondo)
    platform_fill_rect(win, self->bounds, sb->color_track);

    // thumb 
    Rect thumb;
    get_thumb_rect(sb, &thumb);
    Color tc = (self->state == WIDGET_STATE_HOVER || sb->dragging) ? sb->color_thumb_hover : sb->color_thumb;
    platform_fill_rect(win, thumb, tc);
}

static bool scrollbar_handle_event(Widget* self, sEvent* evt) {
    Scrollbar* sb = (Scrollbar*)self;

    switch (evt->type) {
        case EVT_MOUSE_DOWN:
            if (widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                sb->dragging   = true;
                sb->drag_start = (sb->direction == SCROLLBAR_VERTICAL)
                    ? evt->mouse_y : evt->mouse_x;
                return true;
            }
            break;

        case EVT_MOUSE_UP:
            sb->dragging = false;
            break;

        case EVT_MOUSE_MOVE:
            widget_update_hover(self, evt->mouse_x, evt->mouse_y);
            if (sb->dragging && sb->total > sb->visible) {
                Rect b = self->bounds;
                int delta = (sb->direction == SCROLLBAR_VERTICAL)
                    ? evt->mouse_y - sb->drag_start
                    : evt->mouse_x - sb->drag_start;
                sb->drag_start = (sb->direction == SCROLLBAR_VERTICAL)
                    ? evt->mouse_y : evt->mouse_x;

                int track = (sb->direction == SCROLLBAR_VERTICAL)
                    ? b.h : b.w;
                int scroll_delta = delta * (sb->total - sb->visible)
                                   / track;
                sb->position = SV_CLAMP(sb->position + scroll_delta,
                                         0, sb->total - sb->visible);
                if (sb->on_scroll)
                    sb->on_scroll(sb->position, self->user_data);
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

static void scrollbar_destroy_fn(Widget* self) { (void)self; }

Scrollbar* scrollbar_create(int x, int y, int w, int h, ScrollbarDir dir) {
    Scrollbar* sb = SV_ALLOC(Scrollbar);
    if (!sb) return NULL;

    sb->base.type         = WIDGET_SCROLLBAR;
    sb->base.state        = WIDGET_STATE_NORMAL;
    sb->base.bounds       = (Rect){x, y, w, h};
    sb->base.visible      = true;
    sb->base.enabled      = true;
    sb->base.draw         = scrollbar_draw;
    sb->base.handle_event = scrollbar_handle_event;
    sb->base.destroy      = scrollbar_destroy_fn;

    sb->direction  = dir;
    sb->total      = 100;
    sb->visible    = 10;
    sb->position   = 0;
    sb->dragging   = false;

    sb->color_track       = (Color){28,  28,  34,  255};
    sb->color_thumb       = (Color){70,  70,  85,  255};
    sb->color_thumb_hover = (Color){100, 100, 120, 255};

    return sb;
}

void scrollbar_set_range(Scrollbar* sb, int total, int visible) {
    if (!sb) return;
    sb->total   = SV_MAX(0, total);
    sb->visible = SV_MAX(1, visible);
    sb->position = SV_CLAMP(sb->position, 0, SV_MAX(0, sb->total - sb->visible));
}

void scrollbar_set_position(Scrollbar* sb, int pos) {
    if (!sb) return;
    sb->position = SV_CLAMP(pos, 0, SV_MAX(0, sb->total - sb->visible));
}