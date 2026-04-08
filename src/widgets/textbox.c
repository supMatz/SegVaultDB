// SCOPO: Implementazione editor SQL multilinea.

#include "textbox.h"
#include "widget.h"
#include <string.h>
#include <stdint.h>

// aggiorna cursor_line e cursor_col dal cursor_pos
static void update_cursor_linecol(TextBox* tb) {
    tb->cursor_line = 0;
    tb->cursor_col  = 0;
    for (int i = 0; i < tb->cursor_pos; i++) {
        if (tb->text[i] == '\n') {
            tb->cursor_line++;
            tb->cursor_col = 0;
        } else {
            tb->cursor_col++;
        }
    }
}

// cancella il carattere prima del cursore (backspace)
static void textbox_backspace(TextBox* tb) {
    if (tb->cursor_pos == 0) return;
    memmove(tb->text + tb->cursor_pos - 1,
            tb->text + tb->cursor_pos,
            tb->text_len - tb->cursor_pos);
    tb->cursor_pos--;
    tb->text_len--;
    tb->text[tb->text_len] = '\0';
    update_cursor_linecol(tb);
}

// cancella il carattere dopo il cursore (delete)
static void textbox_delete_char(TextBox* tb) {
    if (tb->cursor_pos >= tb->text_len) return;
    memmove(tb->text + tb->cursor_pos,
            tb->text + tb->cursor_pos + 1,
            tb->text_len - tb->cursor_pos - 1);
    tb->text_len--;
    tb->text[tb->text_len] = '\0';
}

// inserisce un singolo carattere alla posizione del cursore
static void textbox_insert_char(TextBox* tb, char c) {
    if (tb->text_len >= TEXTBOX_MAX_LEN - 1) return;
    memmove(tb->text + tb->cursor_pos + 1,
            tb->text + tb->cursor_pos,
            tb->text_len - tb->cursor_pos);
    tb->text[tb->cursor_pos] = c;
    tb->cursor_pos++;
    tb->text_len++;
    tb->text[tb->text_len] = '\0';
    update_cursor_linecol(tb);
}

// tronca `src` in `dst` in modo che la sua larghezza in pixel non superi `max_px`. Aggiunge "…" se troncato. 
// `win` serve per misurare la larghezza carattere per carattere.
static void clip_text(PlatformWindow* win, const char* src, char* dst, size_t dst_cap, int max_px, int font_size) {
    int len = (int)strlen(src);

    // misura l'intera stringa
    int full_w = platform_measure_text(win, src, font_size);
    if (full_w <= max_px) {
        // Entra tutta: copia direttamente
        snprintf(dst, dst_cap, "%s", src);
        return;
    }

    // Misura "…" una volta sola
    int ellipsis_w = platform_measure_text(win, "\xe2\x80\xa6", font_size);
    int available  = max_px - ellipsis_w;

    // Trova quanti caratteri stanno nell'area disponibile
    char tmp[4096] = {0};
    int  fit = 0;
    for (int i = 0; i < len && i < (int)sizeof(tmp) - 1; i++) {
        tmp[i] = src[i];
        tmp[i + 1] = '\0';
        if (platform_measure_text(win, tmp, font_size) > available) break;
        fit = i + 1;
    }

    // Scrivi i caratteri che stanno + "…"
    snprintf(dst, dst_cap, "%.*s\xe2\x80\xa6", fit, src);
}

// -- draw --
static void textbox_draw(Widget* self, PlatformWindow* win) {
    TextBox* tb = (TextBox*)self;
    Rect b = self->bounds;

    // Sfondo
    platform_fill_rect(win, b, tb->color_bg);

    // Bordo: ciano se focused, grigio altrimenti
    platform_draw_rect(
        win, 
        b,
        self->state == WIDGET_STATE_FOCUSED ? (Color){100, 210, 255, 255} : (Color){70,  70,  80,  255},
        1);

    // Larghezza colonna numeri di riga
    int line_num_w  = tb->show_line_nums ? 48 : 0;
    int text_area_x = b.x + line_num_w + 4;
    int text_area_w = b.w - line_num_w - 8; // larghezza disponibile per il testo

    // Separatore fra numeri di riga e area testo
    if (tb->show_line_nums) {
        platform_draw_line(win,
            (Point){b.x + line_num_w, b.y},
            (Point){b.x + line_num_w, b.y + b.h},
            (Color){50, 50, 65, 255}, 1);
    }

    // righe visibili
    int first_line   = tb->scroll_y;
    int visible_lines = b.h / tb->line_height;
    int i            = 0;

    // salta le righe prima dello scroll
    int line = 0;
    while (i < tb->text_len && line < first_line) {
        if (tb->text[i] == '\n') line++;
        i++;
    }

    // disegns ogni riga visibile
    int draw_line = 0;
    int line_start = i;

    while (i <= tb->text_len && draw_line < visible_lines) {
        if (i == tb->text_len || tb->text[i] == '\n') {
            int ry = b.y + draw_line * tb->line_height + 2;

            // numero di riga
            if (tb->show_line_nums) {
                char num[16];
                snprintf(num, sizeof(num), "%d",
                         first_line + draw_line + 1);
                platform_draw_text(win, num,
                    (Point){b.x + 4, ry},
                    tb->color_line_num, tb->font_size);
            }

            // testo della riga con clipping
            int line_len = i - line_start;
            if (line_len > 0) {
                // copia la riga in un buffer temporaneo
                char raw[4096] = {0};
                int  copy_len  = line_len < 4095 ? line_len : 4095;
                memcpy(raw, tb->text + line_start, copy_len);

                // tronca se supera text_area_w
                char clipped[4096];
                clip_text(win, raw, clipped, sizeof(clipped), text_area_w, tb->font_size);

                platform_draw_text(win, clipped,(Point){text_area_x, ry},tb->color_text, tb->font_size);
            }

            draw_line++;
            line_start = i + 1;
        }
        i++;
    }

    // ── CURSORE (solo se focused e visibile per il blink) ────────
    if (self->state == WIDGET_STATE_FOCUSED && tb->cursor_visible) {
        // misura la larghezza reale dei caratteri fino al cursore
        char line_before_cursor[4096] = {0};
        int col_count = 0;
        int idx = 0;
        // trova l'inizio della riga corrente nel testo
        while (idx < tb->cursor_pos) {
            if (tb->text[idx] == '\n') col_count = 0;
            else col_count++;
            idx++;
        }
        // ora risali per trovare l'inizio della riga
        int line_start_idx = tb->cursor_pos - tb->cursor_col;
        int copy = tb->cursor_col < 4095 ? tb->cursor_col : 4095;
        memcpy(line_before_cursor, tb->text + line_start_idx, copy);
        line_before_cursor[copy] = '\0';

        int cx = text_area_x + platform_measure_text(win, line_before_cursor, tb->font_size);
        int cy = b.y + (tb->cursor_line - first_line) * tb->line_height + tb->line_height - 4;
        
        // disegna solo se il cursore è nell'area visibile
        if (cx < b.x + b.w - 4) {
            platform_fill_rect(win,
                (Rect){cx, cy + 2, 2, tb->line_height - 4},
                tb->color_cursor);
        }
    }
}

// -- Event handler --
static bool textbox_handle_event(Widget* self, sEvent* evt) {
    TextBox* tb = (TextBox*)self;

    switch (evt->type) {

        case EVT_MOUSE_DOWN:
            if (widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                self->state = WIDGET_STATE_FOCUSED;
                return true;
            }
            // click fuori: perde il focus
            if (self->state == WIDGET_STATE_FOCUSED)
                self->state = WIDGET_STATE_NORMAL;
            break;

        case EVT_CHAR:
            if (self->state != WIDGET_STATE_FOCUSED) return false;
            textbox_insert_char(tb, (char)evt->character);
            if (tb->on_change)
                tb->on_change(tb->text, self->user_data);
            return true;

        case EVT_KEY_DOWN:
            if (self->state != WIDGET_STATE_FOCUSED) return false;

            switch (evt->key) {
                case KEY_BACKSPACE:
                    textbox_backspace(tb);
                    if (tb->on_change)
                        tb->on_change(tb->text, self->user_data);
                    return true;

                case KEY_DELETE:
                    textbox_delete_char(tb);
                    if (tb->on_change)
                        tb->on_change(tb->text, self->user_data);
                    return true;

                case KEY_LEFT:
                    if (tb->cursor_pos > 0) tb->cursor_pos--;
                    update_cursor_linecol(tb);
                    return true;

                case KEY_RIGHT:
                    if (tb->cursor_pos < tb->text_len) tb->cursor_pos++;
                    update_cursor_linecol(tb);
                    return true;

                case KEY_UP:
                    if (tb->scroll_y > 0) tb->scroll_y--;
                    return true;

                case KEY_DOWN:
                    tb->scroll_y++;
                    return true;

                case KEY_ENTER:
                    // Ctrl+Enter = esegui query
                    if (evt->modifiers & SV_MOD_CTRL) {
                        if (tb->on_execute)
                            tb->on_execute(tb->text, self->user_data);
                    } else {
                        textbox_insert_char(tb, '\n');
                        if (tb->on_change)
                            tb->on_change(tb->text, self->user_data);
                    }
                    return true;

                default: break;
            }
            break;

        default: break;
    }
    return false;
}

static void textbox_destroy_fn(Widget* self) {
    TextBox* tb = (TextBox*)self;
    free(tb->text);
}

// ── API pubblica ─────────────────────────────────────────────────

TextBox* textbox_create(int x, int y, int w, int h) {
    TextBox* tb = SV_ALLOC(TextBox);
    if (!tb) return NULL;

    tb->base.type         = WIDGET_TEXTBOX;
    tb->base.state        = WIDGET_STATE_NORMAL;
    tb->base.bounds       = (Rect){x, y, w, h};
    tb->base.visible      = true;
    tb->base.enabled      = true;
    tb->base.draw         = textbox_draw;
    tb->base.handle_event = textbox_handle_event;
    tb->base.destroy      = textbox_destroy_fn;

    tb->text_cap       = 4096;
    tb->text           = (char*)calloc(tb->text_cap, 1);
    tb->text_len       = 0;
    tb->cursor_pos     = 0;
    tb->cursor_line    = 0;
    tb->cursor_col     = 0;
    tb->cursor_visible = true;
    tb->cursor_blink   = 0;
    tb->sel_start      = -1;
    tb->sel_end        = -1;
    tb->scroll_x       = 0;
    tb->scroll_y       = 0;
    tb->font_size      = 14;
    tb->line_height    = 20;
    tb->show_line_nums = true;

    tb->color_bg       = (Color){20,  20,  24,  255};
    tb->color_text     = (Color){212, 212, 212, 255};
    tb->color_cursor   = (Color){100, 210, 255, 255};
    tb->color_select   = (Color){50,  100, 180, 100};
    tb->color_line_num = (Color){80,  80,  100, 255};
    tb->color_keyword  = (Color){86,  156, 214, 255};
    tb->color_string   = (Color){206, 145, 120, 255};
    tb->color_number   = (Color){181, 206, 168, 255};
    tb->color_comment  = (Color){106, 153, 85,  255};

    return tb;
}

void textbox_set_text(TextBox* tb, const char* text) {
    if (!tb || !text) return;
    int len = (int)strlen(text);
    if (len >= tb->text_cap) {
        tb->text_cap = len + 1024;
        tb->text     = realloc(tb->text, tb->text_cap);
    }
    memcpy(tb->text, text, len + 1);
    tb->text_len   = len;
    tb->cursor_pos = len;
    update_cursor_linecol(tb);
}

const char* textbox_get_text(TextBox* tb) {
    return tb ? tb->text : "";
}

void textbox_clear(TextBox* tb) {
    if (!tb) return;
    memset(tb->text, 0, tb->text_cap);
    tb->text_len    = 0;
    tb->cursor_pos  = 0;
    tb->cursor_line = 0;
    tb->cursor_col  = 0;
    tb->scroll_y    = 0;
}

void textbox_insert(TextBox* tb, const char* text) {
    if (!tb || !text) return;
    for (int i = 0; text[i]; i++)
        textbox_insert_char(tb, text[i]);
}

void textbox_tick(TextBox* tb, uint64_t now_ms) {
    if (!tb) return;
    if (now_ms - tb->cursor_blink > 500) {
        tb->cursor_visible = !tb->cursor_visible;
        tb->cursor_blink   = now_ms;
    }
}