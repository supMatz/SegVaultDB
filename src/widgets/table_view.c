#include "table_view.h"
#include "theme.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define CELL_PAD 10
#define CELL_FONT 13

// ── geometria ────────────────────────────────────────────────────

static bool has_grid(const TableView* tv) {
    return tv->msg_kind == TABLE_MSG_NONE && tv->result && tv->result->success && tv->result->num_cols > 0;
}

static int num_cols(const TableView* tv) {
    return SV_MIN(tv->result->num_cols, TABLE_VIEW_MAX_COLS);
}

// riga visualizzata in posizione i (tiene conto dell'ordinamento)
static Row* row_at(const TableView* tv, int i) {
    return &tv->result->rows[tv->order ? tv->order[i] : i];
}

int table_view_visible_rows(TableView* tv) {
    return SV_MAX(1, (tv->base.bounds.h - TABLE_VIEW_HEADER_H) / TABLE_VIEW_ROW_H);
}

int table_view_content_width(TableView* tv) {
    if (!has_grid(tv)) return 0;
    int w = 0;
    for (int c = 0; c < num_cols(tv); c++) w += tv->col_widths[c];
    return w;
}

int table_view_viewport_width(TableView* tv) {
    return SV_MAX(1, tv->base.bounds.w - tv->rownum_w);
}

void table_view_clamp_scroll(TableView* tv) {
    if (!has_grid(tv)) { tv->scroll_x = tv->scroll_y = 0; return; }
    int max_y = SV_MAX(0, tv->result->num_rows - table_view_visible_rows(tv));
    int max_x = SV_MAX(0, table_view_content_width(tv) - table_view_viewport_width(tv));
    tv->scroll_y = SV_CLAMP(tv->scroll_y, 0, max_y);
    tv->scroll_x = SV_CLAMP(tv->scroll_x, 0, max_x);
}

// x (assoluta) del bordo sinistro della colonna c
static int col_x(const TableView* tv, int c) {
    int x = tv->base.bounds.x + tv->rownum_w - tv->scroll_x;
    for (int i = 0; i < c; i++) x += tv->col_widths[i];
    return x;
}

static int col_at(const TableView* tv, int mx) {
    if (mx < tv->base.bounds.x + tv->rownum_w) return -1;
    int x = col_x(tv, 0);
    for (int c = 0; c < num_cols(tv); c++) {
        if (mx >= x && mx < x + tv->col_widths[c]) return c;
        x += tv->col_widths[c];
    }
    return -1;
}

// colonna il cui bordo destro è vicino a mx (nell'header), -1 se nessuna
static int resize_col_at(const TableView* tv, int mx, int my) {
    if (!has_grid(tv)) return -1;
    Rect b = tv->base.bounds;
    if (my < b.y || my > b.y + TABLE_VIEW_HEADER_H) return -1;
    if (mx < b.x + tv->rownum_w) return -1;
    int x = col_x(tv, 0);
    for (int c = 0; c < num_cols(tv); c++) {
        x += tv->col_widths[c];
        if (mx >= x - 4 && mx <= x + 4) return c;
    }
    return -1;
}

bool table_view_hit_resize(TableView* tv, int mx, int my) {
    return tv->resize_col >= 0 || resize_col_at(tv, mx, my) >= 0;
}

static bool is_numeric(CellType t) { return t == CELL_TYPE_INT || t == CELL_TYPE_FLOAT; }

// il motore può restituire tutto come stringa: riconosce i valori numerici dal testo
static bool looks_numeric(const char* v) {
    if (!v || !*v) return false;
    if (*v == '-' || *v == '+') v++;
    bool digit = false, dot = false;
    for (; *v; v++) {
        if (*v >= '0' && *v <= '9') digit = true;
        else if (*v == '.' && !dot) dot = true;
        else if ((*v == 'e' || *v == 'E') && digit) { v++; if (*v == '-' || *v == '+') v++; if (!*v) return false; }
        else return false;
    }
    return digit;
}

static bool column_is_numeric(const QueryResult* r, int c) {
    int rows = SV_MIN(r->num_rows, TABLE_VIEW_AUTOSIZE_SCAN_LIMIT), seen = 0;
    for (int row = 0; row < rows; row++) {
        const Cell* cell = &r->rows[row].cells[c];
        if (cell->is_null) continue;
        if (!is_numeric(cell->type) && !looks_numeric(cell->value)) return false;
        seen++;
    }
    return seen > 0;
}

static int measure_col(TableView* tv, PlatformWindow* win, int c) {
    QueryResult* r = tv->result;
    int w = platform_measure_text_ex(win, r->col_names[c], -1, TH_FONT_UI, FONT_UI_BOLD);
    if (r->col_types && r->col_types[c] && r->col_types[c][0])
        w += 8 + platform_measure_text_ex(win, r->col_types[c], -1, TH_FONT_SMALL, FONT_MONO);
    int rows = SV_MIN(r->num_rows, TABLE_VIEW_AUTOSIZE_SCAN_LIMIT);
    for (int row = 0; row < rows; row++) {
        Cell* cell = &r->rows[row].cells[c];
        int cw = platform_measure_text_ex(win, cell->is_null ? "NULL" : cell->value, -1, CELL_FONT, FONT_MONO);
        if (cw > w) w = cw;
    }
    return SV_CLAMP(w + CELL_PAD * 2 + 4, 64, 380);
}

static void autosize(TableView* tv, PlatformWindow* win) {
    tv->needs_autosize = false;
    if (!has_grid(tv)) return;
    for (int c = 0; c < num_cols(tv); c++) {
        tv->col_widths[c]  = measure_col(tv, win, c);
        tv->col_numeric[c] = column_is_numeric(tv->result, c);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tv->result->num_rows);
    tv->rownum_w = SV_MAX(44, platform_measure_text_ex(win, buf, -1, TH_FONT_SMALL, FONT_MONO) + 24);
}

// ── disegno ──────────────────────────────────────────────────────

// testo a capo su più righe entro max_w; ritorna l'altezza usata
static int draw_wrapped(PlatformWindow* win, const char* text, int x, int y, int max_w, int max_lines,
                        Color c, int size, FontStyle fs, bool center, int center_w) {
    int len = (int)strlen(text), pos = 0, lines = 0;
    int lh = platform_text_height(win, size, fs) + 4;
    while (pos < len && lines < max_lines) {
        while (pos < len && text[pos] == ' ') pos++;
        int best = -1, i = pos;
        // cerca il punto di spezzatura più lontano che entra
        while (i <= len) {
            if (i == len || text[i] == ' ' || text[i] == '\n') {
                if (platform_measure_text_ex(win, text + pos, i - pos, size, fs) <= max_w) best = i;
                else break;
                if (i < len && text[i] == '\n') break;
            }
            i++;
        }
        if (best <= pos) {
            // parola più lunga della riga: taglia a forza
            best = pos + 1;
            while (best < len && platform_measure_text_ex(win, text + pos, best - pos + 1, size, fs) <= max_w) best++;
        }
        bool last = (lines == max_lines - 1) && best < len;
        int n = best - pos;
        int dx = x;
        if (center) dx = x + (center_w - platform_measure_text_ex(win, text + pos, n, size, fs)) / 2;
        if (last) {
            char tmp[1100];
            snprintf(tmp, sizeof(tmp), "%.*s", n, text + pos);
            ui_text_clipped(win, tmp, dx, y + lines * lh, max_w, c, size, fs);
        } else {
            platform_draw_text_ex(win, text + pos, n, (Point){dx, y + lines * lh}, c, size, fs);
        }
        lines++;
        pos = best;
        if (pos < len && text[pos] == '\n') pos++;
    }
    return lines * lh;
}

static void draw_message(TableView* tv, PlatformWindow* win) {
    Rect b = tv->base.bounds;
    int cx = b.x + b.w / 2;
    int cy = b.y + SV_MAX(56, b.h / 2 - 40);

    const char* title  = tv->msg_title;
    const char* detail = tv->msg_detail;
    Color tc = TH_TEXT;

    switch (tv->msg_kind) {
        case TABLE_MSG_OK:
            ui_circle(win, cx, cy, 22, TH_SUCCESS_SOFT);
            ui_icon_check(win, cx, cy + 1, 20, TH_SUCCESS);
            break;
        case TABLE_MSG_ERROR:
            ui_circle(win, cx, cy, 22, TH_ERROR_SOFT);
            ui_icon_cross(win, cx, cy, 20, TH_ERROR);
            break;
        default:
            ui_icon_table(win, cx, cy, 34, TH_TEXT_FAINT);
            title  = tv->result && tv->result->success ? "No columns returned" : "No results yet";
            detail = "Run a query with F5, or Ctrl+Enter for the statement under the cursor";
            tc = TH_TEXT_MUTED;
            break;
    }

    int y = cy + 36;
    int tw = platform_measure_text_ex(win, title, -1, TH_FONT_UI + 2, FONT_UI_BOLD);
    platform_draw_text_ex(win, title, -1, (Point){cx - tw / 2, y}, tc, TH_FONT_UI + 2, FONT_UI_BOLD);
    y += platform_text_height(win, TH_FONT_UI + 2, FONT_UI_BOLD) + 8;

    if (detail && detail[0]) {
        int max_w = SV_MIN(b.w - 48, 640);
        FontStyle fs = tv->msg_kind == TABLE_MSG_ERROR ? FONT_MONO : FONT_UI;
        Color dc = tv->msg_kind == TABLE_MSG_ERROR ? TH_ERROR_TEXT : TH_TEXT_DIM;
        draw_wrapped(win, detail, cx - max_w / 2, y, max_w, 6, dc, TH_FONT_UI, fs, true, max_w);
    }
}

static void table_view_draw(Widget* self, PlatformWindow* win) {
    TableView* tv = (TableView*)self;
    Rect b = self->bounds; // bordi
    tv->win = win;

    platform_fill_rect(win, b, tv->color_row_even);

    if (!has_grid(tv)) {
        draw_message(tv, win);
        return;
    }
    if (tv->needs_autosize) autosize(tv, win);
    table_view_clamp_scroll(tv);

    QueryResult* r = tv->result;
    int ncols = num_cols(tv);
    int hh = TABLE_VIEW_HEADER_H, rh = TABLE_VIEW_ROW_H;
    int data_x = b.x + tv->rownum_w;
    Rect data_clip = {data_x, b.y, b.w - tv->rownum_w, b.h};
    bool focused = self->state == WIDGET_STATE_FOCUSED;

    int visible_rows = table_view_visible_rows(tv) + 1;
    int first_row = tv->scroll_y;
    int last_row  = SV_MIN(first_row + visible_rows, r->num_rows);
    int content_right = col_x(tv, ncols);

    // -- righe --
    for (int row = first_row; row < last_row; row++) {
        int ry = b.y + hh + (row - first_row) * rh;
        Color bg = (row == tv->selected_row) ? tv->color_row_selected
                 : (row == tv->hover_row)    ? TH_HOVER
                 : (row % 2 == 0)            ? tv->color_row_even : tv->color_row_odd;
        platform_fill_rect(win, (Rect){b.x, ry, b.w, rh}, bg);

        platform_set_clip(win, data_clip);
        int cx = col_x(tv, 0);
        for (int c = 0; c < ncols; c++) {
            int cw = tv->col_widths[c];
            if (cx + cw >= data_x && cx <= b.x + b.w) {
                Cell* cell = &row_at(tv, row)->cells[c];
                int ty = ui_text_y(win, (Rect){0, ry, 0, rh}, CELL_FONT, FONT_MONO);
                int max_w = cw - CELL_PAD * 2;
                if (cell->is_null) {
                    ui_text_clipped(win, "NULL", cx + CELL_PAD,
                                    ui_text_y(win, (Rect){0, ry, 0, rh}, TH_FONT_SMALL, FONT_MONO),
                                    max_w, tv->color_text_null, TH_FONT_SMALL, FONT_MONO);
                } else if (is_numeric(cell->type) || tv->col_numeric[c]) {
                    int w = platform_measure_text_ex(win, cell->value, -1, CELL_FONT, FONT_MONO);
                    int x = (w <= max_w) ? cx + cw - CELL_PAD - w : cx + CELL_PAD;
                    ui_text_clipped(win, cell->value, x, ty, max_w, TH_CELL_NUMBER, CELL_FONT, FONT_MONO);
                } else if (cell->type == CELL_TYPE_BOOL) {
                    bool t = cell->value[0] == '1' || cell->value[0] == 't' || cell->value[0] == 'T';
                    ui_circle(win, cx + CELL_PAD + 4, ry + rh / 2, 3, t ? TH_SUCCESS : TH_TEXT_FAINT);
                    ui_text_clipped(win, t ? "true" : "false", cx + CELL_PAD + 14, ty, max_w - 14,
                                    t ? TH_TEXT : TH_TEXT_DIM, CELL_FONT, FONT_MONO);
                } else {
                    ui_text_clipped(win, cell->value, cx + CELL_PAD, ty, max_w, tv->color_text, CELL_FONT, FONT_MONO);
                }
                // separatore verticale
                platform_fill_rect(win, (Rect){cx + cw - 1, ry, 1, rh}, ui_alpha(tv->color_border, 120));

                if (row == tv->selected_row && c == tv->selected_col) {
                    platform_draw_rect(win, (Rect){cx, ry, cw, rh}, focused ? TH_ACCENT : TH_BORDER_STRONG, 1);
                }
            }
            cx += cw;
        }
        platform_reset_clip(win);

        // numero di riga
        char num[16];
        snprintf(num, sizeof(num), "%d", row + 1);
        int nw = platform_measure_text_ex(win, num, -1, TH_FONT_SMALL, FONT_MONO);
        Color nbg = (row == tv->selected_row) ? tv->color_row_selected : TH_SURFACE;
        platform_fill_rect(win, (Rect){b.x, ry, tv->rownum_w, rh}, nbg);
        platform_draw_text_ex(win, num, -1,
            (Point){b.x + tv->rownum_w - 12 - nw, ui_text_y(win, (Rect){0, ry, 0, rh}, TH_FONT_SMALL, FONT_MONO)},
            row == tv->selected_row ? TH_ACCENT_TEXT : TH_TEXT_FAINT, TH_FONT_SMALL, FONT_MONO);
    }

    // nessuna riga
    if (r->num_rows == 0) {
        const char* t = "Query returned no rows";
        int tw = platform_measure_text_ex(win, t, -1, TH_FONT_UI, FONT_UI);
        platform_draw_text_ex(win, t, -1, (Point){b.x + (b.w - tw) / 2, b.y + hh + 32}, TH_TEXT_DIM, TH_FONT_UI, FONT_UI);
    }

    // -- header --
    platform_fill_rect(win, (Rect){b.x, b.y, b.w, hh}, tv->color_header_bg);
    platform_set_clip(win, data_clip);
    int cx = col_x(tv, 0);
    for (int c = 0; c < ncols; c++) {
        int cw = tv->col_widths[c];
        if (cx + cw >= data_x && cx <= b.x + b.w) {
            Rect hr = {cx, b.y, cw, hh};
            if (c == tv->selected_col) platform_fill_rect(win, hr, TH_ELEVATED);
            int ty = ui_text_y(win, hr, TH_FONT_UI, FONT_UI_BOLD);
            int avail = cw - CELL_PAD * 2;
            if (c == tv->sort_col) {
                avail -= 14;
                ui_icon_chevron(win, cx + cw - CELL_PAD - 4, b.y + hh / 2, 9,
                                tv->sort_asc ? CHEVRON_UP : CHEVRON_DOWN, TH_ACCENT_TEXT);
            }
            int nw = ui_text_clipped(win, r->col_names[c], cx + CELL_PAD, ty, avail, tv->color_header_text,
                                     TH_FONT_UI, FONT_UI_BOLD);
            const char* type = (r->col_types && r->col_types[c]) ? r->col_types[c] : NULL;
            if (type && type[0] && nw + 20 < avail) {
                ui_text_clipped(win, type, cx + CELL_PAD + nw + 8,
                                ui_text_y(win, hr, TH_FONT_SMALL, FONT_MONO),
                                avail - nw - 8, TH_TEXT_DIM, TH_FONT_SMALL, FONT_MONO);
            }
            Color sep = (tv->resize_col == c) ? TH_ACCENT : TH_BORDER_STRONG;
            platform_fill_rect(win, (Rect){cx + cw - 1, b.y + 8, 1, hh - 16}, sep);
        }
        cx += cw;
    }
    platform_reset_clip(win);
    (void)content_right;

    // angolo "#"
    platform_fill_rect(win, (Rect){b.x, b.y, tv->rownum_w, hh}, tv->color_header_bg);
    platform_draw_text_ex(win, "#", -1,
        (Point){b.x + tv->rownum_w - 12 - platform_measure_text_ex(win, "#", -1, TH_FONT_SMALL, FONT_MONO),
                ui_text_y(win, (Rect){0, b.y, 0, hh}, TH_FONT_SMALL, FONT_MONO)},
        TH_TEXT_FAINT, TH_FONT_SMALL, FONT_MONO);

    platform_fill_rect(win, (Rect){b.x, b.y + hh - 1, b.w, 1}, TH_BORDER_STRONG);
    platform_fill_rect(win, (Rect){b.x + tv->rownum_w - 1, b.y, 1, b.h}, TH_BORDER);
}

// ── eventi ───────────────────────────────────────────────────────

static void copy_cell(TableView* tv) {
    if (!tv->win || !has_grid(tv) || tv->selected_row < 0) return;
    QueryResult* r = tv->result;
    if (tv->selected_row >= r->num_rows) return;
    if (tv->selected_col >= 0) {
        Cell* cell = &row_at(tv, tv->selected_row)->cells[tv->selected_col];
        platform_set_clipboard(tv->win, cell->is_null ? "NULL" : cell->value);
        return;
    }
    // riga intera separata da tab
    char buf[8192];
    int n = 0;
    for (int c = 0; c < num_cols(tv) && n < (int)sizeof(buf) - 1; c++) {
        Cell* cell = &row_at(tv, tv->selected_row)->cells[c];
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s", c ? "\t" : "", cell->is_null ? "NULL" : cell->value);
    }
    platform_set_clipboard(tv->win, buf);
}

static void ensure_selection_visible(TableView* tv) {
    int vis = table_view_visible_rows(tv);
    if (tv->selected_row < tv->scroll_y) tv->scroll_y = tv->selected_row;
    else if (tv->selected_row >= tv->scroll_y + vis) tv->scroll_y = tv->selected_row - vis + 1;
    if (tv->selected_col >= 0) {
        int x0 = 0;
        for (int c = 0; c < tv->selected_col; c++) x0 += tv->col_widths[c];
        int x1 = x0 + tv->col_widths[tv->selected_col];
        int vw = table_view_viewport_width(tv);
        if (x0 < tv->scroll_x) tv->scroll_x = x0;
        else if (x1 > tv->scroll_x + vw) tv->scroll_x = x1 - vw;
    }
    table_view_clamp_scroll(tv);
}

static bool table_view_handle_event(Widget* self, sEvent* evt) {
    TableView* tv = (TableView*)self;
    bool inside = widget_contains_point(self, evt->mouse_x, evt->mouse_y);

    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            if (tv->resize_col >= 0) {
                tv->col_widths[tv->resize_col] = SV_MAX(40, tv->resize_start_w + evt->mouse_x - tv->resize_start_x);
                table_view_clamp_scroll(tv);
                return true;
            }
            tv->hover_row = -1;
            if (inside && has_grid(tv)) {
                int rel = evt->mouse_y - self->bounds.y - TABLE_VIEW_HEADER_H;
                if (rel >= 0) {
                    int row = tv->scroll_y + rel / TABLE_VIEW_ROW_H;
                    if (row < tv->result->num_rows) tv->hover_row = row;
                }
            }
            return false;

        case EVT_MOUSE_UP:
            if (tv->resize_col >= 0) { tv->resize_col = -1; return true; }
            return false;

        case EVT_MOUSE_WHEEL:
            if (!inside || !has_grid(tv)) return false;
            if (evt->modifiers & SV_MOD_SHIFT) tv->scroll_x -= evt->wheel_delta * 60;
            else                               tv->scroll_y -= evt->wheel_delta * 3;
            table_view_clamp_scroll(tv);
            return true;

        case EVT_MOUSE_DOWN: {
            if (!inside) {
                if (self->state == WIDGET_STATE_FOCUSED) self->state = WIDGET_STATE_NORMAL;
                return false;
            }
            self->state = WIDGET_STATE_FOCUSED;
            if (!has_grid(tv)) return true;

            int rc = resize_col_at(tv, evt->mouse_x, evt->mouse_y);
            if (rc >= 0) {
                uint64_t now = platform_time_ms();
                if (now - tv->last_header_click < 400 && tv->win) {
                    tv->col_widths[rc] = measure_col(tv, tv->win, rc); // doppio click: adatta al contenuto
                    tv->last_header_click = 0;
                    return true;
                }
                tv->last_header_click = now;
                tv->resize_col     = rc;
                tv->resize_start_x = evt->mouse_x;
                tv->resize_start_w = tv->col_widths[rc];
                return true;
            }

            int rel_y = evt->mouse_y - self->bounds.y - TABLE_VIEW_HEADER_H;
            int col   = col_at(tv, evt->mouse_x);
            if (rel_y < 0) { // click sull'header: seleziona la colonna e ordina
                tv->selected_col = col;
                if (col >= 0) {
                    if (tv->sort_col != col)  table_view_sort(tv, col, true);
                    else if (tv->sort_asc)    table_view_sort(tv, col, false);
                    else                      table_view_sort(tv, -1, true);
                }
                return true;
            }
            int clicked_row = tv->scroll_y + rel_y / TABLE_VIEW_ROW_H;
            if (clicked_row < tv->result->num_rows) {
                tv->selected_row = clicked_row;
                tv->selected_col = col;
                if (tv->on_row_select) tv->on_row_select(clicked_row, self->user_data);
                uint64_t now = platform_time_ms();
                if (tv->on_row_activate && tv->last_row_clicked == clicked_row && now - tv->last_row_click < 400) {
                    tv->last_row_click = 0;
                    tv->on_row_activate(clicked_row, self->user_data);
                    return true;
                }
                tv->last_row_click   = now;
                tv->last_row_clicked = clicked_row;
            }
            return true;
        }

        case EVT_KEY_DOWN: {
            if (self->state != WIDGET_STATE_FOCUSED || !has_grid(tv)) return false;
            int nrows = tv->result->num_rows;
            if (evt->key == KEY_LETTER && (evt->modifiers & SV_MOD_CTRL) && evt->character == 'c') {
                if (evt->modifiers & SV_MOD_SHIFT) {     // tutto il risultato come TSV
                    char* t = table_view_export(tv, false);
                    if (t && tv->win) platform_set_clipboard(tv->win, t);
                    free(t);
                } else {
                    copy_cell(tv);
                }
                return true;
            }
            if (nrows == 0) return false;
            if (tv->selected_row < 0) tv->selected_row = 0;
            switch (evt->key) {
                case KEY_UP:       tv->selected_row = SV_MAX(0, tv->selected_row - 1); break;
                case KEY_DOWN:     tv->selected_row = SV_MIN(nrows - 1, tv->selected_row + 1); break;
                case KEY_PAGEUP:   tv->selected_row = SV_MAX(0, tv->selected_row - table_view_visible_rows(tv)); break;
                case KEY_PAGEDOWN: tv->selected_row = SV_MIN(nrows - 1, tv->selected_row + table_view_visible_rows(tv)); break;
                case KEY_HOME:     tv->selected_row = 0; break;
                case KEY_END:      tv->selected_row = nrows - 1; break;
                case KEY_LEFT:     tv->selected_col = SV_MAX(0, tv->selected_col - 1); break;
                case KEY_RIGHT:    tv->selected_col = SV_MIN(num_cols(tv) - 1, tv->selected_col + 1); break;
                case KEY_ESCAPE:   tv->selected_row = tv->selected_col = -1; return true;
                case KEY_ENTER:
                    if (!tv->on_row_activate) return false;
                    tv->on_row_activate(tv->selected_row, self->user_data);
                    return true;
                default: return false;
            }
            ensure_selection_visible(tv);
            return true;
        }

        default:
            break;
    }
    return false;
}

static void table_view_destroy_fn(Widget* self) { free(((TableView*)self)->order); }

TableView* table_view_create(int x, int y, int w, int h) {
    TableView* tv = SV_ALLOC(TableView);
    if (!tv) return NULL;

    tv->base.type         = WIDGET_TABLE_VIEW;
    tv->base.state        = WIDGET_STATE_NORMAL;
    tv->base.bounds       = (Rect){x, y, w, h};
    tv->base.visible      = true;
    tv->base.enabled      = true;
    tv->base.draw         = table_view_draw;
    tv->base.handle_event = table_view_handle_event;
    tv->base.destroy      = table_view_destroy_fn;

    tv->result       = NULL;
    tv->selected_row = -1;
    tv->selected_col = -1;
    tv->hover_row    = -1;
    tv->resize_col   = -1;
    tv->sort_col     = -1;
    tv->sort_asc     = true;
    tv->rownum_w     = 44;
    tv->font_size    = CELL_FONT;

    for (int i = 0; i < TABLE_VIEW_MAX_COLS; i++)
        tv->col_widths[i] = 120;

    table_view_apply_theme(tv);

    return tv;
}

void table_view_apply_theme(TableView* tv) {
    if (!tv) return;
    tv->color_header_bg    = TH_SURFACE_2;
    tv->color_header_text  = TH_TEXT;
    tv->color_row_even     = TH_BG;
    tv->color_row_odd      = TH_ROW_ODD;
    tv->color_row_selected = TH_ACCENT_SOFT;
    tv->color_text         = TH_CELL_TEXT;
    tv->color_text_null    = TH_TEXT_FAINT;
    tv->color_border       = TH_BORDER;
}

void table_view_set_result(TableView* tv, QueryResult* result) {
    if (!tv) return;
    tv->result       = result;
    tv->selected_row = -1;
    tv->selected_col = -1;
    tv->hover_row    = -1;
    tv->resize_col   = -1;
    tv->scroll_x     = 0;
    tv->scroll_y     = 0;
    tv->msg_kind     = TABLE_MSG_NONE;
    tv->needs_autosize = true;
    tv->sort_col     = -1;
    tv->sort_asc     = true;
    free(tv->order);
    tv->order = NULL;
}

void table_view_set_message(TableView* tv, TableMessageKind kind, const char* title, const char* detail) {
    if (!tv) return;
    tv->msg_kind = kind;
    snprintf(tv->msg_title, sizeof(tv->msg_title), "%s", title ? title : "");
    snprintf(tv->msg_detail, sizeof(tv->msg_detail), "%s", detail ? detail : "");
}

void table_view_clear(TableView* tv) {
    if (!tv) return;
    free(tv->order);
    tv->order        = NULL;
    tv->sort_col     = -1;
    tv->result       = NULL;
    tv->selected_row = -1;
    tv->selected_col = -1;
    tv->msg_kind     = TABLE_MSG_NONE;
}

void table_view_scroll_to(TableView* tv, int row) {
    if (!tv) return;
    tv->scroll_y = SV_MAX(0, row);
    table_view_clamp_scroll(tv);
}

// ── ordinamento ──────────────────────────────────────────────────

static const TableView* g_sort_tv;   // contesto per qsort (la GUI è single-thread)
static int  g_sort_col;
static bool g_sort_asc, g_sort_num;

static int cmp_rows(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    const Cell* ca = &g_sort_tv->result->rows[ia].cells[g_sort_col];
    const Cell* cb = &g_sort_tv->result->rows[ib].cells[g_sort_col];
    int r;
    if (ca->is_null || cb->is_null) {
        r = (ca->is_null && cb->is_null) ? 0 : ca->is_null ? -1 : 1;   // NULL per primi in ordine crescente
    } else if (g_sort_num) {
        double x = strtod(ca->value, NULL), y = strtod(cb->value, NULL);
        r = x < y ? -1 : x > y ? 1 : 0;
    } else {
        r = strcasecmp(ca->value, cb->value);
        if (r == 0) r = strcmp(ca->value, cb->value);
    }
    if (!g_sort_asc) r = -r;
    return r ? r : ia - ib;   // a parità mantiene l'ordine originale (stabile)
}

void table_view_sort(TableView* tv, int col, bool asc) {
    if (!tv || !has_grid(tv)) return;
    QueryResult* r = tv->result;
    // la riga selezionata resta selezionata dopo l'ordinamento
    int sel_orig = (tv->selected_row >= 0 && tv->selected_row < r->num_rows)
                 ? (tv->order ? tv->order[tv->selected_row] : tv->selected_row) : -1;

    if (col < 0 || col >= num_cols(tv) || r->num_rows == 0) {
        free(tv->order);
        tv->order = NULL;
        tv->sort_col = -1;
        tv->sort_asc = true;
    } else {
        if (!tv->order) tv->order = malloc(sizeof(int) * r->num_rows);
        if (!tv->order) return;
        for (int i = 0; i < r->num_rows; i++) tv->order[i] = i;
        g_sort_tv  = tv;
        g_sort_col = col;
        g_sort_asc = asc;
        g_sort_num = tv->col_numeric[col] || column_is_numeric(r, col);
        qsort(tv->order, r->num_rows, sizeof(int), cmp_rows);
        tv->sort_col = col;
        tv->sort_asc = asc;
    }
    if (sel_orig >= 0) {
        for (int i = 0; i < r->num_rows; i++)
            if ((tv->order ? tv->order[i] : i) == sel_orig) { tv->selected_row = i; break; }
    }
}

// ── esportazione ─────────────────────────────────────────────────

typedef struct { char* b; size_t len, cap; bool ok; } TBuf;

static void tb_put(TBuf* t, const char* s, size_t n) {
    if (!t->ok) return;
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap * 2 + n + 256;
        char* nb = realloc(t->b, cap);
        if (!nb) { t->ok = false; return; }
        t->b = nb;
        t->cap = cap;
    }
    memcpy(t->b + t->len, s, n);
    t->len += n;
    t->b[t->len] = '\0';
}

static void tb_field(TBuf* t, const char* v, bool csv) {
    if (!csv) {   // TSV: tab e a capo diventano spazi
        for (const char* p = v; *p; p++) {
            char c = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
            tb_put(t, &c, 1);
        }
        return;
    }
    bool quote = strpbrk(v, ",\"\n\r") != NULL || (v[0] == ' ' || (v[0] && v[strlen(v) - 1] == ' '));
    if (!quote) { tb_put(t, v, strlen(v)); return; }
    tb_put(t, "\"", 1);
    for (const char* p = v; *p; p++) {
        if (*p == '"') tb_put(t, "\"\"", 2);
        else tb_put(t, p, 1);
    }
    tb_put(t, "\"", 1);
}

char* table_view_export(TableView* tv, bool csv) {
    if (!tv || !has_grid(tv)) return NULL;
    QueryResult* r = tv->result;
    int nc = r->num_cols;
    const char* sep = csv ? "," : "\t";
    const char* eol = csv ? "\r\n" : "\n";
    TBuf t = { NULL, 0, 0, true };
    for (int c = 0; c < nc; c++) {
        if (c) tb_put(&t, sep, 1);
        tb_field(&t, r->col_names[c] ? r->col_names[c] : "", csv);
    }
    tb_put(&t, eol, strlen(eol));
    for (int i = 0; i < r->num_rows; i++) {
        Row* row = row_at(tv, i);
        for (int c = 0; c < nc && c < row->num_cells; c++) {
            if (c) tb_put(&t, sep, 1);
            const Cell* cell = &row->cells[c];
            if (cell->is_null) { if (!csv) tb_put(&t, "NULL", 4); }
            else tb_field(&t, cell->value, csv);
        }
        tb_put(&t, eol, strlen(eol));
    }
    if (!t.ok) { free(t.b); return NULL; }
    return t.b;
}
