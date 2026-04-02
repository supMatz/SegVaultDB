#include "table_view.h"
#include <string.h>

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

    // header
    Rect header = {b.x, b.y, TABLE_VIEW_HEADER_H, b.w} ;
    platform_fill_rect(win, header, tv->color_header_bg);

    int x = b.x - tv->scroll_x, y = b.y, w = b.w, h = b.h;

    for(int c = 0; c < r->num_cols; c++) {
        int cw = tv->col_widths[c];
        
        // testo colonna
        platform_draw_text(
            win, 
            r->col_names[c], 
            (Point) {x + 6, y + (TABLE_VIEW_HEADER_H - tv->font_size) / 2}, 
            tv->color_header_text,
            1
        );       

        // separatore verticale
        platform_draw_line(
            win, (Point){x + cw, y}, 
            (Point){x + cw, y + TABLE_VIEW_HEADER_H}, 
            tv->color_border, 
            1
        );
        x += cw;
    }

    // bordo inferire header
    platform_draw_line(
        win, 
        (Point) {x, y + TABLE_VIEW_HEADER_H},
        (Point) {x + w, y + TABLE_VIEW_HEADER_H},
        tv->color_border,
        1
    );

    // disegno le righe
    int visible_rows = (h - TABLE_VIEW_HEADER_H) / TABLE_VIEW_ROW_H + 1;
    int first_row = tv->scroll_y;
    int last_row = SV_MIN(first_row + visible_rows, r->num_rows);

    for(int row = first_row; row < last_row; row++) {
        int ry = y + TABLE_VIEW_ROW_H + (row - first_row) * TABLE_VIEW_ROW_H;

        // colore alternato riga pari/dispari
        Color row_bg;
        if (row == tv->selected_row)
            row_bg = tv->color_row_selected;
        else if (row % 2 == 0)
            row_bg = tv->color_row_even;
        else
            row_bg = tv->color_row_odd;
 
        platform_fill_rect(win, (Rect){b.x, ry, b.w, TABLE_VIEW_ROW_H},row_bg);
 
        // celle
        x = b.x - tv->scroll_x;
        for (int c = 0; c < r->num_cols; c++) {
            int cw = tv->col_widths[c];
            Cell* cell = &r->rows[row].cells[c];
 
            Color tc = cell->is_null ? tv->color_text_null : tv->color_text;
            const char* val = cell->is_null ? "NULL" : cell->value;
 
            platform_draw_text(win, val,
                (Point){x + 6,ry + (TABLE_VIEW_ROW_H - tv->font_size) / 2},
                tc, tv->font_size
            );
 
            // separatore verticale
            platform_draw_line(win,
                (Point){x + cw, ry},
                (Point){x + cw, ry + TABLE_VIEW_ROW_H},
                tv->color_border, 1);
            x += cw;
        }
 
        // separatore orizzontale riga
        platform_draw_line(win,
            (Point){b.x, ry + TABLE_VIEW_ROW_H},
            (Point){b.x + b.w, ry + TABLE_VIEW_ROW_H},
            tv->color_border, 1);
    }
 
    // bordo esterno
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
 
    // auto-dimensiona colonne in base al nome della colonna
    if (result && result->success) {
        for (int c = 0; c < result->num_cols && c < TABLE_VIEW_MAX_COLS; c++) {
            int name_len = strlen(result->col_names[c]);
            // msinimo 80px, massimo 300px
            tv->col_widths[c] = SV_CLAMP(name_len * 9 + 20, 80, 300);
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