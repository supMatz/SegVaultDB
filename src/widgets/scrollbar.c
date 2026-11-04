#include "scrollbar.h"
#include "theme.h"

#define SB_MIN_THUMB 28

static bool needed(const Scrollbar* sb) { return sb->total > sb->visible; }

static int track_len(const Scrollbar* sb) {
    return sb->direction == SCROLLBAR_VERTICAL ? sb->base.bounds.h : sb->base.bounds.w;
}

static int thumb_len(const Scrollbar* sb) {
    int t = track_len(sb);
    if (!needed(sb)) return t;
    return SV_CLAMP(t * sb->visible / sb->total, SB_MIN_THUMB, t);
}

// offset del thumb dall'inizio della traccia
static int thumb_off(const Scrollbar* sb) {
    int range = sb->total - sb->visible;
    if (range <= 0) return 0;
    return (track_len(sb) - thumb_len(sb)) * sb->position / range;
}

// calcola la posizione e dimensione del thumb in pixel
static void get_thumb_rect(Scrollbar* sb, Rect* out) {
    Rect b = sb->base.bounds;
    int off = thumb_off(sb), len = thumb_len(sb);
    if (sb->direction == SCROLLBAR_VERTICAL) *out = (Rect){b.x, b.y + off, b.w, len};
    else                                     *out = (Rect){b.x + off, b.y, len, b.h};
}

static void set_pos(Scrollbar* sb, int pos) {
    int np = SV_CLAMP(pos, 0, SV_MAX(0, sb->total - sb->visible));
    if (np == sb->position) return;
    sb->position = np;
    if (sb->on_scroll) sb->on_scroll(sb->position, sb->base.user_data);
}

static void scrollbar_draw(Widget* self, PlatformWindow* win) {
    Scrollbar* sb = (Scrollbar*)self;
    if (!needed(sb)) return; // nascosta se tutto il contenuto è visibile

    bool active = self->state == WIDGET_STATE_HOVER || sb->dragging;
    Rect thumb;
    get_thumb_rect(sb, &thumb);

    // thumb sottile che si allarga in hover
    int inset = active ? 2 : 3;
    Rect r = thumb;
    if (sb->direction == SCROLLBAR_VERTICAL) { r.x += inset; r.w -= inset * 2; r.y += 2; r.h -= 4; }
    else                                     { r.y += inset; r.h -= inset * 2; r.x += 2; r.w -= 4; }

    if (active)
        platform_fill_rect(win, self->bounds, ui_alpha(sb->color_track, 150));
    Color tc = sb->dragging ? sb->color_thumb_hover : active ? ui_mix(sb->color_thumb, sb->color_thumb_hover, 0.6f) : sb->color_thumb;
    int rad = (sb->direction == SCROLLBAR_VERTICAL ? r.w : r.h) / 2;
    platform_fill_rect_rounded(win, r, tc, rad);
}

static bool scrollbar_handle_event(Widget* self, sEvent* evt) {
    Scrollbar* sb = (Scrollbar*)self;
    if (!needed(sb)) {
        sb->dragging = false;
        if (self->state == WIDGET_STATE_HOVER) self->state = WIDGET_STATE_NORMAL;
        return false;
    }
    bool vert = sb->direction == SCROLLBAR_VERTICAL;
    int  m    = vert ? evt->mouse_y : evt->mouse_x;
    int  base = vert ? self->bounds.y : self->bounds.x;

    switch (evt->type) {
        case EVT_MOUSE_DOWN:
            if (evt->mouse_button == SV_MOUSE_LEFT && widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                int off = thumb_off(sb), len = thumb_len(sb);
                if (m < base + off || m > base + off + len) {
                    // click sulla traccia: salta in modo che il thumb sia centrato sul mouse
                    int range = sb->total - sb->visible;
                    int free_len = SV_MAX(1, track_len(sb) - len);
                    set_pos(sb, (m - base - len / 2) * range / free_len);
                }
                sb->dragging   = true;
                sb->drag_start = m - (base + thumb_off(sb)); // presa sul thumb
                return true;
            }
            break;

        case EVT_MOUSE_UP:
            if (sb->dragging) { sb->dragging = false; return true; }
            break;

        case EVT_MOUSE_MOVE:
            widget_update_hover(self, evt->mouse_x, evt->mouse_y);
            if (sb->dragging) {
                int range = sb->total - sb->visible;
                int free_len = SV_MAX(1, track_len(sb) - thumb_len(sb));
                set_pos(sb, (m - base - sb->drag_start) * range / free_len);
                return true;
            }
            break;

        case EVT_MOUSE_WHEEL:
            if (widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                set_pos(sb, sb->position - evt->wheel_delta * SV_MAX(1, sb->step));
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
    sb->total      = 0;
    sb->visible    = 1;
    sb->position   = 0;
    sb->step       = 3;
    sb->dragging   = false;

    scrollbar_apply_theme(sb);

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

bool scrollbar_is_needed(Scrollbar* sb) { return sb && needed(sb); }

void scrollbar_apply_theme(Scrollbar* sb) {
    if (!sb) return;
    sb->color_track       = TH_SURFACE_2;
    sb->color_thumb       = TH_SCROLL_THUMB;
    sb->color_thumb_hover = TH_SCROLL_THUMB_HOVER;
}
