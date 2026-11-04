// SCOPO: implementazione del campo di input a riga singola (vedi line_edit.h).

#include "line_edit.h"
#include "theme.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LE_FONT TH_FONT_UI
#define LE_PAD  9

static bool is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int prev_pos(const LineEdit* le, int p) {
    if (p <= 0) return 0;
    p--;
    while (p > 0 && is_cont(le->text[p])) p--;
    return p;
}

static int next_pos(const LineEdit* le, int p) {
    if (p >= le->len) return le->len;
    p++;
    while (p < le->len && is_cont(le->text[p])) p++;
    return p;
}

static bool word_char(char c) {
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == '_' || u >= 0x80;
}

static int word_left(const LineEdit* le, int p) {
    while (p > 0 && le->text[p - 1] == ' ') p--;
    if (p > 0 && word_char(le->text[p - 1])) { while (p > 0 && word_char(le->text[p - 1])) p--; }
    else if (p > 0) p = prev_pos(le, p);
    return p;
}

static int word_right(const LineEdit* le, int p) {
    if (p < le->len && word_char(le->text[p])) { while (p < le->len && word_char(le->text[p])) p++; }
    else if (p < le->len) p = next_pos(le, p);
    while (p < le->len && le->text[p] == ' ') p++;
    return p;
}

static bool has_sel(const LineEdit* le) { return le->anchor >= 0 && le->anchor != le->cursor; }

static void sel_range(const LineEdit* le, int* a, int* b) {
    if (!has_sel(le)) { *a = *b = le->cursor; return; }
    *a = SV_MIN(le->anchor, le->cursor);
    *b = SV_MAX(le->anchor, le->cursor);
}

static void reset_blink(LineEdit* le) {
    le->cursor_visible = true;
    le->blink_ms = platform_time_ms();
}

static void changed(LineEdit* le) {
    le->error = false;
    reset_blink(le);
    if (le->on_change) le->on_change(le, le->base.user_data);
}

static void replace_sel(LineEdit* le, const char* s, int n) {
    int a, b;
    sel_range(le, &a, &b);
    // niente a capo in un campo a riga singola
    char clean[LINE_EDIT_MAX];
    int m = 0;
    for (int i = 0; i < n && m < LINE_EDIT_MAX - 1; i++)
        if (s[i] != '\n' && s[i] != '\r') clean[m++] = s[i] == '\t' ? ' ' : s[i];
    int room = LINE_EDIT_MAX - 1 - (le->len - (b - a));
    if (m > room) { m = SV_MAX(0, room); while (m > 0 && is_cont(clean[m])) m--; }
    memmove(le->text + a + m, le->text + b, le->len - b);
    memcpy(le->text + a, clean, m);
    le->len = le->len - (b - a) + m;
    le->text[le->len] = '\0';
    le->cursor = a + m;
    le->anchor = -1;
    changed(le);
}

static void move_to(LineEdit* le, int p, bool select) {
    if (select) { if (le->anchor < 0) le->anchor = le->cursor; }
    else le->anchor = -1;
    le->cursor = SV_CLAMP(p, 0, le->len);
    reset_blink(le);
}

static int hit(LineEdit* le, int mx) {
    if (!le->win) return le->len;
    int x = le->base.bounds.x + LE_PAD - le->scroll_x;
    int best = 0, best_d = 1 << 30;
    for (int p = 0; ; p = next_pos(le, p)) {
        int w = platform_measure_text_ex(le->win, le->text, p, LE_FONT, FONT_UI);
        int d = abs(x + w - mx);
        if (d < best_d) { best_d = d; best = p; }
        if (p >= le->len) break;
    }
    return best;
}

static bool line_edit_handle_event(Widget* self, sEvent* evt) {
    LineEdit* le = (LineEdit*)self;
    bool focused = self->state == WIDGET_STATE_FOCUSED;

    switch (evt->type) {
        case EVT_MOUSE_DOWN:
            if (!widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                if (focused) self->state = WIDGET_STATE_NORMAL;
                le->dragging = false;
                return false;
            }
            self->state = WIDGET_STATE_FOCUSED;
            move_to(le, hit(le, evt->mouse_x), (evt->modifiers & SV_MOD_SHIFT) != 0);
            if (!(evt->modifiers & SV_MOD_SHIFT)) le->anchor = le->cursor;
            le->dragging = true;
            return true;

        case EVT_MOUSE_MOVE:
            if (!le->dragging) return false;
            le->cursor = hit(le, evt->mouse_x);
            reset_blink(le);
            return true;

        case EVT_MOUSE_UP:
            if (le->dragging) { le->dragging = false; return true; }
            return false;

        case EVT_CHAR: {
            if (!focused) return false;
            char buf[5] = {0};
            uint32_t cp = evt->character;
            int n;
            if (cp < 0x80)         { buf[0] = (char)cp; n = 1; }
            else if (cp < 0x800)   { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
            else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                     buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
            else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                   buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
            replace_sel(le, buf, n);
            return true;
        }

        case EVT_KEY_DOWN: {
            if (!focused) return false;
            bool ctrl  = evt->modifiers & SV_MOD_CTRL;
            bool shift = evt->modifiers & SV_MOD_SHIFT;
            switch (evt->key) {
                case KEY_BACKSPACE:
                    if (!has_sel(le)) {
                        if (le->cursor == 0) return true;
                        le->anchor = ctrl ? word_left(le, le->cursor) : prev_pos(le, le->cursor);
                    }
                    replace_sel(le, "", 0);
                    return true;
                case KEY_DELETE:
                    if (!has_sel(le)) {
                        if (le->cursor >= le->len) return true;
                        le->anchor = ctrl ? word_right(le, le->cursor) : next_pos(le, le->cursor);
                    }
                    replace_sel(le, "", 0);
                    return true;
                case KEY_LEFT:
                    if (has_sel(le) && !shift) { int a, b; sel_range(le, &a, &b); move_to(le, a, false); }
                    else move_to(le, ctrl ? word_left(le, le->cursor) : prev_pos(le, le->cursor), shift);
                    return true;
                case KEY_RIGHT:
                    if (has_sel(le) && !shift) { int a, b; sel_range(le, &a, &b); move_to(le, b, false); }
                    else move_to(le, ctrl ? word_right(le, le->cursor) : next_pos(le, le->cursor), shift);
                    return true;
                case KEY_HOME: move_to(le, 0, shift); return true;
                case KEY_END:  move_to(le, le->len, shift); return true;
                case KEY_LETTER:
                    if (!ctrl) return false;
                    switch (evt->character) {
                        case 'a': line_edit_select_all(le); return true;
                        case 'c': case 'x': {
                            if (!has_sel(le) || !le->win) return true;
                            int a, b;
                            sel_range(le, &a, &b);
                            char tmp[LINE_EDIT_MAX];
                            memcpy(tmp, le->text + a, b - a);
                            tmp[b - a] = '\0';
                            platform_set_clipboard(le->win, tmp);
                            if (evt->character == 'x') replace_sel(le, "", 0);
                            return true;
                        }
                        case 'v': {
                            if (!le->win) return true;
                            char* s = platform_get_clipboard(le->win);
                            if (s) { replace_sel(le, s, (int)strlen(s)); free(s); }
                            return true;
                        }
                        default: return false;
                    }
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

static void line_edit_draw(Widget* self, PlatformWindow* win) {
    LineEdit* le = (LineEdit*)self;
    le->win = win;
    Rect b = self->bounds;
    bool focused = self->state == WIDGET_STATE_FOCUSED;

    Color border = le->error ? TH_ERROR : focused ? TH_ACCENT : TH_BORDER_STRONG;
    ui_round_box(win, b, TH_EDITOR_BG, border, 5);

    int inner_w = b.w - LE_PAD * 2;
    int cur_x = platform_measure_text_ex(win, le->text, le->cursor, LE_FONT, FONT_UI);
    if (cur_x - le->scroll_x > inner_w) le->scroll_x = cur_x - inner_w;
    if (cur_x - le->scroll_x < 0)       le->scroll_x = cur_x;
    int full_w = platform_measure_text_ex(win, le->text, le->len, LE_FONT, FONT_UI);
    le->scroll_x = SV_CLAMP(le->scroll_x, 0, SV_MAX(0, full_w - inner_w));

    int tx = b.x + LE_PAD - le->scroll_x;
    int ty = ui_text_y(win, b, LE_FONT, FONT_UI);
    platform_set_clip(win, (Rect){b.x + 2, b.y + 1, b.w - 4, b.h - 2});

    if (has_sel(le)) {
        int a, e;
        sel_range(le, &a, &e);
        int xa = platform_measure_text_ex(win, le->text, a, LE_FONT, FONT_UI);
        int xe = platform_measure_text_ex(win, le->text, e, LE_FONT, FONT_UI);
        platform_fill_rect(win, (Rect){tx + xa, b.y + 5, xe - xa, b.h - 10}, TH_EDITOR_SELECT);
    }
    if (le->len == 0 && le->placeholder)
        platform_draw_text_ex(win, le->placeholder, -1, (Point){b.x + LE_PAD, ty}, TH_TEXT_FAINT, LE_FONT, FONT_UI);
    else
        platform_draw_text_ex(win, le->text, le->len, (Point){tx, ty}, TH_TEXT, LE_FONT, FONT_UI);

    if (focused && le->cursor_visible)
        platform_fill_rect(win, (Rect){tx + cur_x, b.y + 6, 1, b.h - 12}, TH_ACCENT_TEXT);

    platform_reset_clip(win);
}

LineEdit* line_edit_create(const char* placeholder) {
    LineEdit* le = SV_ALLOC(LineEdit);
    if (!le) return NULL;
    le->base.type         = WIDGET_TEXTBOX;
    le->base.state        = WIDGET_STATE_NORMAL;
    le->base.visible      = true;
    le->base.enabled      = true;
    le->base.draw         = line_edit_draw;
    le->base.handle_event = line_edit_handle_event;
    le->anchor      = -1;
    le->placeholder = placeholder;
    le->cursor_visible = true;
    return le;
}

void line_edit_set_text(LineEdit* le, const char* text) {
    if (!le) return;
    le->len = 0;
    le->text[0] = '\0';
    le->cursor = 0;
    le->anchor = -1;
    replace_sel(le, text ? text : "", text ? (int)strlen(text) : 0);
}

void line_edit_select_all(LineEdit* le) {
    if (!le) return;
    le->anchor = 0;
    le->cursor = le->len;
    reset_blink(le);
}

void line_edit_focus(LineEdit* le, bool focused) {
    if (!le) return;
    le->base.state = focused ? WIDGET_STATE_FOCUSED : WIDGET_STATE_NORMAL;
    reset_blink(le);
}

bool line_edit_focused(const LineEdit* le) {
    return le && le->base.visible && le->base.state == WIDGET_STATE_FOCUSED;
}

bool line_edit_tick(LineEdit* le, uint64_t now_ms) {
    if (!line_edit_focused(le)) return false;
    if (now_ms - le->blink_ms >= 530) {
        le->cursor_visible = !le->cursor_visible;
        le->blink_ms = now_ms;
        return true;
    }
    return false;
}
