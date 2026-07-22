#include "table_view.h"
#include <string.h>


// stima la larghezza in pixel di una stringa per un dato font. sarebbe da fare un platform_text_width() per il pixel perfect!
static int measure_text_width(const char* text, int font_size) {
    if (!text) return 0;
    return (int)(strlen(text) * font_size * 0.6f);
}

static void draw_clipped_text(PlatformWindow* win, const char* text, Point pos, Color col, int font_size, int max_width) {
    if (measure_text_width(text, font_size) <= max_width) {
        platform_draw_text(win, text, pos, col, font_size);
        return;
    }

    char buf[256];
    size_t len = strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    while (len > 3 && measure_text_width(buf, font_size) > max_width) {
        len--;
        buf[len] = '\0';
    }
    if (len > 3) {
        buf[len - 1] = '.';
        buf[len - 2] = '.';
        buf[len - 3] = '.';
    }
    platform_draw_text(win, buf, pos, col, font_size);
}

static void table_view_draw(Widget* self, PlatformWindow* win) {
    TableView* tv = (TableView*)self;
    Rect b = self->bounds; // bordi

    // sfondo generale
    platform_fill_rect(win, b, tv->color_row_even);

    if(!tv->result || !tv->result->success || tv->result->num_cols == 0) {
        // nessun risultato mostro messaggio
        platform_draw_text(win, "Nessun risultato", (Point){b.x + 10, b.y + b.h / 2 - 7}, (Color) {100, 100, 110, 255} , tv->font_size);
        return;
    }

    QueryResult* r = tv->result;

    platform_fill_rect(win, (Rect){b.x, b.y, b.w, TABLE_VIEW_HEADER_H}, tv->color_header_bg);

    int cx = b.x - tv->scroll_x;

    for(int c = 0; c < r->num_cols; c++) {
        int cw = tv->col_widths[c];

        platform_draw_text(
            win,
            r->col_names[c],
            (Point) {cx + 6, b.y + (TABLE_VIEW_HEADER_H - tv->font_size) / 2 + 1},
            tv->color_header_text,
            tv->font_size
        );

        platform_draw_line(
            win, (Point){cx + cw, b.y},
            (Point){cx + cw, b.y + TABLE_VIEW_HEADER_H},
            tv->color_border,
            1
        );
        cx += cw;
    }

    platform_draw_line(
        win,
        (Point) {b.x, b.y + TABLE_VIEW_HEADER_H},
        (Point) {b.x + b.w, b.y + TABLE_VIEW_HEADER_H},
        tv->color_border,
        1
    );

    int visible_rows = (b.h - TABLE_VIEW_HEADER_H) / TABLE_VIEW_ROW_H + 1;
    int first_row = tv->scroll_y;
    int last_row = SV_MIN(first_row + visible_rows, r->num_rows);

    for(int row = first_row; row < last_row; row++) {
        int ry = b.y + TABLE_VIEW_HEADER_H + (row - first_row) * TABLE_VIEW_ROW_H;

        Color row_bg;
        if (row == tv->selected_row)
            row_bg = tv->color_row_selected;
        else if (row % 2 == 0)
            row_bg = tv->color_row_even;
        else
            row_bg = tv->color_row_odd;

        platform_fill_rect(win, (Rect){b.x, ry, b.w, TABLE_VIEW_ROW_H}, row_bg);

        cx = b.x - tv->scroll_x;
        for (int c = 0; c < r->num_cols; c++) {
            int cw = tv->col_widths[c];
            Cell* cell = &r->rows[row].cells[c];

            Color tc = cell->is_null ? tv->color_text_null : tv->color_text;
            const char* val = cell->is_null ? "NULL" : cell->value;

            draw_clipped_text(win, val,
                (Point){cx + 6, ry + (TABLE_VIEW_ROW_H - tv->font_size) / 2},
                tc, tv->font_size,
                cw - 12 // padding sx+dx
            );

            platform_draw_line(win,
                (Point){cx + cw, ry},
                (Point){cx + cw, ry + TABLE_VIEW_ROW_H},
                tv->color_border, 1);
            cx += cw;
        }

        platform_draw_line(
            win,
            (Point){b.x, ry + TABLE_VIEW_ROW_H},
            (Point){b.x + b.w, ry + TABLE_VIEW_ROW_H},
            tv->color_border, 1
        );
    }

    platform_draw_rect(win, b, tv->color_border, 1);
}

static bool table_view_handle_event(Widget* self, sEvent* evt) {
    TableView* tv = (TableView*)self;
    if (!tv->result) return false;

    switch (evt->type) {
        case EVT_MOUSE_DOWN:
            if (!widget_contains_point(self, evt->mouse_x, evt->mouse_y))
                return false;
            {
                // calcola quale riga è stata cliccata
                int rel_y = evt->mouse_y - self->bounds.y
                            - TABLE_VIEW_HEADER_H;
                if (rel_y < 0) return false; // click sull'header
                int clicked_row = tv->scroll_y + rel_y / TABLE_VIEW_ROW_H;
                if (clicked_row < tv->result->num_rows) {
                    tv->selected_row = clicked_row;
                    if (tv->on_row_select)
                        tv->on_row_select(clicked_row, self->user_data);
                }
            }
            return true;

        default:
            break;
    }
    return false;
}

static void table_view_destroy_fn(Widget* self) { (void)self; }

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
    tv->sort_col     = -1;
    tv->sort_asc     = true;
    tv->scroll_x     = 0;
    tv->scroll_y     = 0;
    tv->font_size    = 13;

    // larghezza default colonne
    for (int i = 0; i < TABLE_VIEW_MAX_COLS; i++)
        tv->col_widths[i] = 120;

    // colori (tema Obsidian)
    tv->color_header_bg    = (Color){38,  38,  45,  255};
    tv->color_header_text  = (Color){180, 180, 200, 255};
    tv->color_row_even     = (Color){24,  24,  28,  255};
    tv->color_row_odd      = (Color){28,  28,  34,  255};
    tv->color_row_selected = (Color){40,  80,  130, 255};
    tv->color_text         = (Color){212, 212, 212, 255};
    tv->color_text_null    = (Color){100, 100, 130, 255};
    tv->color_border       = (Color){50,  50,  60,  255};

    return tv;
}

void table_view_set_result(TableView* tv, QueryResult* result) {
    if (!tv) return;
    tv->result       = result;
    tv->selected_row = -1;
    tv->scroll_x     = 0;
    tv->scroll_y     = 0;

    if (result && result->success) {
        for (int c = 0; c < result->num_cols && c < TABLE_VIEW_MAX_COLS; c++) {
            // punto di partenza: larghezza del nome colonna
            int max_w = measure_text_width(result->col_names[c], tv->font_size);

            // scansiona i valori della colonna e trova il piu' largo
            int rows_to_scan = SV_MIN(result->num_rows, TABLE_VIEW_AUTOSIZE_SCAN_LIMIT);
            for (int row = 0; row < rows_to_scan; row++) {
                Cell* cell = &result->rows[row].cells[c];
                const char* val = cell->is_null ? "NULL" : cell->value;
                int w = measure_text_width(val, tv->font_size);
                if (w > max_w) max_w = w;
            }

            // padding: 6px margine sx (come nel draw) + un po' di respiro
            tv->col_widths[c] = SV_CLAMP(max_w + 20, 80, 400);
        }
    }
}

void table_view_clear(TableView* tv) {
    if (!tv) return;
    tv->result       = NULL;
    tv->selected_row = -1;
}

void table_view_scroll_to(TableView* tv, int row) {
    if (!tv) return;
    tv->scroll_y = SV_MAX(0, row);
}
