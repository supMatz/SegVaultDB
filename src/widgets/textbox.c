// SCOPO: Implementazione editor SQL multilinea (vedi textbox.h per le funzionalità).

#include "textbox.h"
#include "widget.h"
#include "theme.h"
#include "sql_text.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <strings.h>

#define INDENT 4

enum { EDIT_NONE, EDIT_TYPE, EDIT_DELETE, EDIT_OTHER };

// ═════════════════════════════════════════════════════════════════
//  UTF-8 e righe
// ═════════════════════════════════════════════════════════════════

static bool is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int utf8_prev(const TextBox* tb, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && is_cont(tb->text[pos])) pos--;
    return pos;
}

static int utf8_next(const TextBox* tb, int pos) {
    if (pos >= tb->text_len) return tb->text_len;
    pos++;
    while (pos < tb->text_len && is_cont(tb->text[pos])) pos++;
    return pos;
}

static int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int count_chars(const char* s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) if (!is_cont(s[i])) n++;
    return n;
}

static bool is_word_char(char c) {
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == '_' || u >= 0x80;
}

static void rebuild_lines(TextBox* tb) {
    tb->num_lines = 0;
    int need = 1;
    for (int i = 0; i < tb->text_len; i++) if (tb->text[i] == '\n') need++;
    if (need > tb->lines_cap) {
        tb->lines_cap   = need + 256;
        tb->line_starts = realloc(tb->line_starts, tb->lines_cap * sizeof(int));
    }
    tb->line_starts[tb->num_lines++] = 0;
    for (int i = 0; i < tb->text_len; i++)
        if (tb->text[i] == '\n') tb->line_starts[tb->num_lines++] = i + 1;
}

static int line_of(const TextBox* tb, int off) {
    int lo = 0, hi = tb->num_lines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (tb->line_starts[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int line_start(const TextBox* tb, int l) { return tb->line_starts[l]; }

static int line_end(const TextBox* tb, int l) {
    return (l + 1 < tb->num_lines) ? tb->line_starts[l + 1] - 1 : tb->text_len;
}

static int col_of(const TextBox* tb, int off) {
    int ls = line_start(tb, line_of(tb, off));
    return count_chars(tb->text + ls, off - ls);
}

static int offset_at_col(const TextBox* tb, int l, int col) {
    int p = line_start(tb, l), e = line_end(tb, l), c = 0;
    while (p < e && c < col) { p = utf8_next(tb, p); c++; }
    return p;
}

static int first_non_space(const TextBox* tb, int l) {
    int p = line_start(tb, l), e = line_end(tb, l);
    while (p < e && tb->text[p] == ' ') p++;
    return p;
}

static void update_cursor_linecol(TextBox* tb) {
    tb->cursor_line = line_of(tb, tb->cursor_pos);
    tb->cursor_col  = col_of(tb, tb->cursor_pos);
}

static bool has_sel(const TextBox* tb) {
    return tb->sel_anchor >= 0 && tb->sel_anchor != tb->cursor_pos;
}

static void sel_range(const TextBox* tb, int* s, int* e) {
    if (!has_sel(tb)) { *s = *e = tb->cursor_pos; return; }
    *s = SV_MIN(tb->sel_anchor, tb->cursor_pos);
    *e = SV_MAX(tb->sel_anchor, tb->cursor_pos);
}

static int visible_lines(const TextBox* tb) {
    int v = (tb->base.bounds.h - tb->pad_top) / SV_MAX(1, tb->line_height);
    return SV_MAX(1, v);
}

static int max_line_cols(const TextBox* tb) {
    int m = 0;
    for (int l = 0; l < tb->num_lines; l++) {
        int ls = line_start(tb, l);
        int c = count_chars(tb->text + ls, line_end(tb, l) - ls);
        if (c > m) m = c;
    }
    return m;
}

static int text_x0(const TextBox* tb) { return tb->base.bounds.x + tb->gutter_w + 10; }
static int text_area_w(const TextBox* tb) { return tb->base.bounds.w - tb->gutter_w - 10; }

static void clamp_scroll(TextBox* tb) {
    int max_y = SV_MAX(0, tb->num_lines - visible_lines(tb) + 1);
    tb->scroll_y = SV_CLAMP(tb->scroll_y, 0, max_y);
    int max_x = SV_MAX(0, (max_line_cols(tb) + 4) * tb->char_w - text_area_w(tb));
    tb->scroll_x = SV_CLAMP(tb->scroll_x, 0, max_x);
}

static void ensure_visible(TextBox* tb) {
    int vis = visible_lines(tb);
    if (tb->cursor_line < tb->scroll_y) tb->scroll_y = tb->cursor_line;
    else if (tb->cursor_line >= tb->scroll_y + vis) tb->scroll_y = tb->cursor_line - vis + 1;

    int cx = tb->cursor_col * tb->char_w;
    int aw = text_area_w(tb);
    if (cx - tb->scroll_x < 0)
        tb->scroll_x = SV_MAX(0, cx - tb->char_w * 8);
    else if (cx - tb->scroll_x > aw - tb->char_w * 3)
        tb->scroll_x = cx - aw + tb->char_w * 8;
    clamp_scroll(tb);
}

static void reset_blink(TextBox* tb) {
    tb->cursor_visible = true;
    tb->cursor_blink   = platform_time_ms();
}

// ═════════════════════════════════════════════════════════════════
//  Annulla / ripeti
// ═════════════════════════════════════════════════════════════════

static TextSnapshot snapshot_take(const TextBox* tb) {
    TextSnapshot s;
    s.text = malloc(tb->text_len + 1);
    if (s.text) memcpy(s.text, tb->text, tb->text_len + 1);
    s.cursor = tb->cursor_pos;
    s.anchor = tb->sel_anchor;
    return s;
}

static void clear_redo(TextBox* tb) {
    for (int i = 0; i < tb->redo_count; i++) free(tb->redo[i].text);
    tb->redo_count = 0;
}

static void stack_push(TextSnapshot* stack, int* count, TextSnapshot s) {
    if (*count == TEXTBOX_UNDO_CAP) {
        free(stack[0].text);
        memmove(stack, stack + 1, (TEXTBOX_UNDO_CAP - 1) * sizeof(TextSnapshot));
        (*count)--;
    }
    stack[(*count)++] = s;
}

static void undo_push(TextBox* tb, int kind) {
    uint64_t now = platform_time_ms();
    bool coalesce = (kind == EDIT_TYPE || kind == EDIT_DELETE) &&
                    kind == tb->last_edit_kind &&
                    now - tb->last_edit_ms < 1500 &&
                    tb->undo_count > 0;
    tb->last_edit_kind = kind;
    tb->last_edit_ms   = now;
    clear_redo(tb);
    if (coalesce) return;
    stack_push(tb->undo, &tb->undo_count, snapshot_take(tb));
}

static void restore_snapshot(TextBox* tb, TextSnapshot* s) {
    int len = (int)strlen(s->text);
    if (len + 1 > tb->text_cap) {
        tb->text_cap = len + 1024;
        tb->text = realloc(tb->text, tb->text_cap);
    }
    memcpy(tb->text, s->text, len + 1);
    tb->text_len   = len;
    rebuild_lines(tb);
    tb->cursor_pos = SV_CLAMP(s->cursor, 0, len);
    tb->sel_anchor = (s->anchor >= 0 && s->anchor <= len) ? s->anchor : -1;
    update_cursor_linecol(tb);
    tb->pref_col = tb->cursor_col;
    tb->error_line = -1;
    tb->popup_open = false;
    ensure_visible(tb);
    reset_blink(tb);
    if (tb->on_change) tb->on_change(tb->text, tb->base.user_data);
}

static void do_undo(TextBox* tb) {
    if (tb->undo_count == 0) return;
    stack_push(tb->redo, &tb->redo_count, snapshot_take(tb));
    TextSnapshot s = tb->undo[--tb->undo_count];
    restore_snapshot(tb, &s);
    free(s.text);
    tb->last_edit_kind = EDIT_NONE;
}

static void do_redo(TextBox* tb) {
    if (tb->redo_count == 0) return;
    stack_push(tb->undo, &tb->undo_count, snapshot_take(tb));
    TextSnapshot s = tb->redo[--tb->redo_count];
    restore_snapshot(tb, &s);
    free(s.text);
    tb->last_edit_kind = EDIT_NONE;
}

// ═════════════════════════════════════════════════════════════════
//  Modifica del testo (tutte le modifiche passano da replace_range)
// ═════════════════════════════════════════════════════════════════

static void replace_range(TextBox* tb, int start, int end, const char* s, int slen) {
    if (slen < 0) slen = (int)strlen(s);
    start = SV_CLAMP(start, 0, tb->text_len);
    end   = SV_CLAMP(end, start, tb->text_len);

    // s potrebbe puntare dentro tb->text: copia di sicurezza
    char* tmp = NULL;
    if (s >= tb->text && s < tb->text + tb->text_cap) {
        tmp = malloc(slen + 1);
        if (!tmp) return;
        memcpy(tmp, s, slen);
        s = tmp;
    }

    int new_len = tb->text_len - (end - start) + slen;
    if (new_len > TEXTBOX_MAX_LEN - 1) {
        slen -= new_len - (TEXTBOX_MAX_LEN - 1);
        while (slen > 0 && is_cont(s[slen])) slen--;
        if (slen < 0) { free(tmp); return; }
        new_len = tb->text_len - (end - start) + slen;
    }
    if (new_len + 1 > tb->text_cap) {
        int cap = SV_MAX(tb->text_cap * 2, new_len + 1024);
        char* nt = realloc(tb->text, cap);
        if (!nt) { free(tmp); return; }
        tb->text = nt;
        tb->text_cap = cap;
    }

    memmove(tb->text + start + slen, tb->text + end, tb->text_len - end);
    memcpy(tb->text + start, s, slen);
    tb->text_len = new_len;
    tb->text[new_len] = '\0';
    free(tmp);

    tb->cursor_pos = start + slen;
    tb->sel_anchor = -1;
    tb->popup_open = false;
    rebuild_lines(tb);
    update_cursor_linecol(tb);
    tb->pref_col   = tb->cursor_col;
    tb->error_line = -1;
    ensure_visible(tb);
    reset_blink(tb);
    if (tb->on_change) tb->on_change(tb->text, tb->base.user_data);
}

// inserisce testo "pulito" (niente \r, tab convertiti in spazi) al posto della selezione
static void insert_text(TextBox* tb, const char* s, int kind) {
    int len = (int)strlen(s);
    char* clean = malloc(len * INDENT + 1);
    if (!clean) return;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\r') continue;
        if (s[i] == '\t') { for (int k = 0; k < INDENT; k++) clean[j++] = ' '; continue; }
        clean[j++] = s[i];
    }
    clean[j] = '\0';

    int a, b;
    sel_range(tb, &a, &b);
    undo_push(tb, kind);
    replace_range(tb, a, b, clean, j);
    free(clean);
}

static void delete_range(TextBox* tb, int a, int b, int kind) {
    if (a >= b) return;
    undo_push(tb, kind);
    replace_range(tb, a, b, "", 0);
}

// ═════════════════════════════════════════════════════════════════
//  Movimento del cursore
// ═════════════════════════════════════════════════════════════════

static void move_cursor(TextBox* tb, int pos, bool select, bool keep_pref) {
    if (select) {
        if (tb->sel_anchor < 0) tb->sel_anchor = tb->cursor_pos;
    } else {
        tb->sel_anchor = -1;
    }
    tb->cursor_pos = SV_CLAMP(pos, 0, tb->text_len);
    update_cursor_linecol(tb);
    if (!keep_pref) tb->pref_col = tb->cursor_col;
    tb->last_edit_kind = EDIT_NONE;
    ensure_visible(tb);
    reset_blink(tb);
}

static int word_left(const TextBox* tb, int p) {
    if (p <= 0) return 0;
    while (p > 0 && tb->text[p - 1] == ' ') p--;
    if (p > 0 && is_word_char(tb->text[p - 1])) {
        while (p > 0 && is_word_char(tb->text[p - 1])) p--;
    } else if (p > 0) {
        p = utf8_prev(tb, p);
    }
    return p;
}

static int word_right(const TextBox* tb, int p) {
    int n = tb->text_len;
    if (p >= n) return n;
    if (is_word_char(tb->text[p])) {
        while (p < n && is_word_char(tb->text[p])) p++;
    } else if (tb->text[p] != ' ') {
        p = utf8_next(tb, p);
    }
    while (p < n && tb->text[p] == ' ') p++;
    return p;
}

static void select_word_at(TextBox* tb, int off) {
    int a = off, b = off;
    if (off < tb->text_len && is_word_char(tb->text[off])) {
        while (a > 0 && is_word_char(tb->text[a - 1])) a--;
        while (b < tb->text_len && is_word_char(tb->text[b])) b++;
    } else if (off > 0 && is_word_char(tb->text[off - 1])) {
        while (a > 0 && is_word_char(tb->text[a - 1])) a--;
    } else {
        b = utf8_next(tb, off);
    }
    tb->sel_anchor = a;
    tb->cursor_pos = b;
    update_cursor_linecol(tb);
    tb->pref_col = tb->cursor_col;
}

static void select_lines(TextBox* tb, int l0, int l1) {
    tb->sel_anchor = line_start(tb, l0);
    tb->cursor_pos = (l1 + 1 < tb->num_lines) ? line_start(tb, l1 + 1) : tb->text_len;
    update_cursor_linecol(tb);
    tb->pref_col = tb->cursor_col;
}

static int hit_offset(const TextBox* tb, int mx, int my) {
    Rect b = tb->base.bounds;
    int rel_y = my - b.y - tb->pad_top;
    int l = tb->scroll_y + (rel_y >= 0 ? rel_y / tb->line_height : -1);
    if (l < 0) return 0;
    if (l >= tb->num_lines) return tb->text_len;
    int rel_x = mx - text_x0(tb) + tb->scroll_x;
    int col = rel_x <= 0 ? 0 : (rel_x + tb->char_w / 2) / tb->char_w;
    return offset_at_col(tb, l, col);
}

// ═════════════════════════════════════════════════════════════════
//  Operazioni su blocchi di righe
// ═════════════════════════════════════════════════════════════════

// righe coinvolte dalla selezione (o la riga del cursore)
static void selected_lines(const TextBox* tb, int* l0, int* l1) {
    int a, b;
    sel_range(tb, &a, &b);
    *l0 = line_of(tb, a);
    *l1 = line_of(tb, b);
    if (*l1 > *l0 && b == line_start(tb, *l1)) (*l1)--; // selezione che finisce a inizio riga
}

// sostituisce il blocco di righe l0..l1 con new_block e lo riseleziona
static void replace_lines(TextBox* tb, int l0, int l1, const char* new_block, int len, bool reselect,
                          int cursor_delta) {
    int a = line_start(tb, l0), b = line_end(tb, l1);
    int old_cursor = tb->cursor_pos;
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, new_block, len);
    if (reselect && l1 > l0) {
        tb->sel_anchor = a;
        tb->cursor_pos = a + len;
    } else {
        tb->cursor_pos = SV_CLAMP(old_cursor + cursor_delta, a, a + len);
    }
    update_cursor_linecol(tb);
    tb->pref_col = tb->cursor_col;
    ensure_visible(tb);
}

static void indent_lines(TextBox* tb, bool outdent) {
    int l0, l1;
    selected_lines(tb, &l0, &l1);
    int a = line_start(tb, l0), b = line_end(tb, l1);
    char* out = malloc((b - a) + (l1 - l0 + 1) * INDENT + 1);
    if (!out) return;
    int j = 0, delta_first = 0;
    for (int l = l0; l <= l1; l++) {
        int ls = line_start(tb, l), le = line_end(tb, l);
        int skip = 0;
        if (outdent) {
            while (skip < INDENT && ls + skip < le && tb->text[ls + skip] == ' ') skip++;
            if (l == tb->cursor_line) delta_first = -SV_MIN(skip, tb->cursor_pos - ls);
        } else {
            if (ls < le) { for (int k = 0; k < INDENT; k++) out[j++] = ' '; }
            if (l == tb->cursor_line && ls < le) delta_first = INDENT;
        }
        memcpy(out + j, tb->text + ls + skip, le - ls - skip);
        j += le - ls - skip;
        if (l < l1) out[j++] = '\n';
    }
    replace_lines(tb, l0, l1, out, j, true, delta_first);
    free(out);
}

static void toggle_comment(TextBox* tb) {
    int l0, l1;
    selected_lines(tb, &l0, &l1);

    // tutte le righe non vuote sono già commentate?
    bool all = true;
    int min_indent = 1 << 30;
    for (int l = l0; l <= l1; l++) {
        int fns = first_non_space(tb, l), le = line_end(tb, l);
        if (fns == le) continue;
        min_indent = SV_MIN(min_indent, fns - line_start(tb, l));
        if (!(le - fns >= 2 && tb->text[fns] == '-' && tb->text[fns + 1] == '-')) all = false;
    }
    if (min_indent == (1 << 30)) return; // solo righe vuote

    int a = line_start(tb, l0), b = line_end(tb, l1);
    char* out = malloc((b - a) + (l1 - l0 + 1) * 3 + 1);
    if (!out) return;
    int j = 0, delta = 0;
    for (int l = l0; l <= l1; l++) {
        int ls = line_start(tb, l), le = line_end(tb, l), fns = first_non_space(tb, l);
        if (fns == le) {
            memcpy(out + j, tb->text + ls, le - ls); j += le - ls;
        } else if (all) {
            int rm = 2 + ((fns + 2 < le && tb->text[fns + 2] == ' ') ? 1 : 0);
            memcpy(out + j, tb->text + ls, fns - ls); j += fns - ls;
            memcpy(out + j, tb->text + fns + rm, le - fns - rm); j += le - fns - rm;
            if (l == tb->cursor_line) delta = -rm;
        } else {
            int ins = ls + min_indent;
            memcpy(out + j, tb->text + ls, ins - ls); j += ins - ls;
            memcpy(out + j, "-- ", 3); j += 3;
            memcpy(out + j, tb->text + ins, le - ins); j += le - ins;
            if (l == tb->cursor_line) delta = 3;
        }
        if (l < l1) out[j++] = '\n';
    }
    replace_lines(tb, l0, l1, out, j, true, delta);
    free(out);
}

static void duplicate(TextBox* tb) {
    if (has_sel(tb)) {
        int a, b;
        sel_range(tb, &a, &b);
        char* copy = malloc(b - a + 1);
        if (!copy) return;
        memcpy(copy, tb->text + a, b - a);
        undo_push(tb, EDIT_OTHER);
        replace_range(tb, b, b, copy, b - a);
        tb->sel_anchor = b;
        free(copy);
        return;
    }
    int l = tb->cursor_line, col = tb->cursor_col;
    int ls = line_start(tb, l), le = line_end(tb, l);
    char* copy = malloc(le - ls + 2);
    if (!copy) return;
    copy[0] = '\n';
    memcpy(copy + 1, tb->text + ls, le - ls);
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, le, le, copy, le - ls + 1);
    free(copy);
    move_cursor(tb, offset_at_col(tb, l + 1, col), false, false);
}

// sposta le righe selezionate di una posizione in su (dir < 0) o in giù (dir > 0)
static void move_lines(TextBox* tb, int dir) {
    int l0, l1;
    selected_lines(tb, &l0, &l1);
    if ((dir < 0 && l0 == 0) || (dir > 0 && l1 >= tb->num_lines - 1)) return;

    int nb  = dir < 0 ? l0 - 1 : l1 + 1;            // riga vicina che scambia di posto
    int a   = line_start(tb, SV_MIN(l0, nb));
    int b   = line_end(tb, SV_MAX(l1, nb));
    int bs  = line_start(tb, l0), be = line_end(tb, l1);
    int ns  = line_start(tb, nb), ne = line_end(tb, nb);
    int blen = be - bs, nlen = ne - ns;

    char* out = malloc(blen + nlen + 2);
    if (!out) return;
    if (dir < 0) {
        memcpy(out, tb->text + bs, blen); out[blen] = '\n';
        memcpy(out + blen + 1, tb->text + ns, nlen);
    } else {
        memcpy(out, tb->text + ns, nlen); out[nlen] = '\n';
        memcpy(out + nlen + 1, tb->text + bs, blen);
    }
    int shift  = dir < 0 ? -(nlen + 1) : (nlen + 1);
    int cursor = tb->cursor_pos + shift;
    int anchor = tb->sel_anchor >= 0 ? tb->sel_anchor + shift : -1;

    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, out, blen + nlen + 1);
    free(out);
    tb->cursor_pos = SV_CLAMP(cursor, 0, tb->text_len);
    tb->sel_anchor = anchor >= 0 ? SV_CLAMP(anchor, 0, tb->text_len) : -1;
    update_cursor_linecol(tb);
    tb->pref_col = tb->cursor_col;
    ensure_visible(tb);
}

// elimina le righe selezionate (o quella del cursore)
static void delete_lines(TextBox* tb) {
    int l0, l1;
    selected_lines(tb, &l0, &l1);
    int col = tb->cursor_col;
    int a = line_start(tb, l0);
    int b = (l1 + 1 < tb->num_lines) ? line_start(tb, l1 + 1) : tb->text_len;
    if (l1 + 1 >= tb->num_lines && a > 0) a--; // ultima riga: rimuove il '\n' precedente
    if (a >= b) return;
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, "", 0);
    int l = SV_MIN(l0, tb->num_lines - 1);
    move_cursor(tb, offset_at_col(tb, l, col), false, false);
}

// maiuscolo / minuscolo della selezione (o della parola sotto il cursore)
static void change_case(TextBox* tb, bool upper) {
    int a, b;
    bool had_sel = has_sel(tb);
    if (had_sel) {
        sel_range(tb, &a, &b);
    } else {
        a = b = tb->cursor_pos;
        while (a > 0 && is_word_char(tb->text[a - 1])) a--;
        while (b < tb->text_len && is_word_char(tb->text[b])) b++;
    }
    if (a >= b) return;
    char* buf = malloc(b - a);
    if (!buf) return;
    for (int i = a; i < b; i++) {
        unsigned char c = (unsigned char)tb->text[i];
        buf[i - a] = (char)(c < 0x80 ? (upper ? toupper(c) : tolower(c)) : c);
    }
    int cursor = tb->cursor_pos, anchor = tb->sel_anchor;
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, buf, b - a);
    free(buf);
    tb->cursor_pos = cursor;
    tb->sel_anchor = had_sel ? anchor : -1;
    update_cursor_linecol(tb);
}

// Ctrl+L: seleziona la riga corrente; ripetuto estende alla riga successiva
static void select_line_extend(TextBox* tb) {
    int l0, l1;
    if (has_sel(tb)) {
        selected_lines(tb, &l0, &l1);
        int s, e;
        sel_range(tb, &s, &e);
        int full_end = (l1 + 1 < tb->num_lines) ? line_start(tb, l1 + 1) : tb->text_len;
        if (s == line_start(tb, l0) && e == full_end && l1 + 1 < tb->num_lines) l1++;
    } else {
        l0 = l1 = tb->cursor_line;
    }
    select_lines(tb, l0, l1);
    ensure_visible(tb);
}

// inserisce una riga vuota sotto (o sopra) quella corrente mantenendo l'indentazione
static void open_line(TextBox* tb, bool above) {
    int l = tb->cursor_line;
    int ls = line_start(tb, l);
    int indent = first_non_space(tb, l) - ls;
    indent = SV_MIN(indent, 200);
    char buf[256];
    int n = 0;
    if (!above) buf[n++] = '\n';
    for (int i = 0; i < indent; i++) buf[n++] = ' ';
    int cursor_at = n;
    if (above) buf[n++] = '\n';
    int at = above ? ls : line_end(tb, l);
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, at, at, buf, n);
    move_cursor(tb, at + cursor_at, false, false);
}

// ═════════════════════════════════════════════════════════════════
//  Appunti
// ═════════════════════════════════════════════════════════════════

static void copy_to_clipboard(TextBox* tb, bool cut) {
    if (!tb->win) return;
    int a, b;
    if (has_sel(tb)) {
        sel_range(tb, &a, &b);
    } else {
        // nessuna selezione: copia/taglia l'intera riga
        int l = tb->cursor_line;
        a = line_start(tb, l);
        b = (l + 1 < tb->num_lines) ? line_start(tb, l + 1) : tb->text_len;
        if (a == b) return;
    }
    char* s = malloc(b - a + 2);
    if (!s) return;
    memcpy(s, tb->text + a, b - a);
    int n = b - a;
    if (!has_sel(tb) && (n == 0 || s[n - 1] != '\n')) s[n++] = '\n';
    s[n] = '\0';
    platform_set_clipboard(tb->win, s);
    free(s);
    if (cut) delete_range(tb, a, b, EDIT_OTHER);
}

static void paste_from_clipboard(TextBox* tb) {
    if (!tb->win) return;
    char* s = platform_get_clipboard(tb->win);
    if (!s) return;
    insert_text(tb, s, EDIT_OTHER);
    free(s);
}

// ═════════════════════════════════════════════════════════════════
//  Autocompletamento
// ═════════════════════════════════════════════════════════════════

#define POPUP_ROW_H 24
#define POPUP_PAD    4

static bool in_code_at(const TextBox* tb, int pos); // definita dopo il lexer
static char char_at(const TextBox* tb, int pos);

static void popup_close(TextBox* tb) {
    tb->popup_open = false;
    tb->popup_n = 0;
    tb->popup_rect = (Rect){0, 0, 0, 0};
}

static bool ieq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (!b[i] || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

static bool ieq(const char* a, const char* b) {
    int n = (int)strlen(a);
    return (int)strlen(b) == n && ieq_n(a, b, n);
}

static int cmp_items(const void* x, const void* y) {
    const CompletionItem* a = x;
    const CompletionItem* b = y;
    int la = (int)strlen(a->name), lb = (int)strlen(b->name);
    if (la != lb) return la - lb; // le parole più corte (più probabili) prima
    return strcasecmp(a->name, b->name);
}

static void popup_add(TextBox* tb, const char* name, const char* detail, const char* parent, CompletionKind k) {
    if (tb->popup_n >= TEXTBOX_POPUP_MAX) return;
    for (int i = 0; i < tb->popup_n; i++)  // niente duplicati (es. stessa colonna in più tabelle)
        if (tb->popup_items[i].kind == k && ieq(tb->popup_items[i].name, name)) return;
    CompletionItem* it = &tb->popup_items[tb->popup_n++];
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->detail, sizeof(it->detail), "%s", detail ? detail : "");
    snprintf(it->parent, sizeof(it->parent), "%s", parent ? parent : "");
    it->kind = k;
}

// la parola prima di `start` introduce un nome di tabella? (FROM, JOIN, INTO, ...)
static bool table_context(const TextBox* tb, int start) {
    int e = start;
    while (e > 0 && isspace((unsigned char)tb->text[e - 1])) e--;
    if (e > 0 && tb->text[e - 1] == ',') return false;
    int b = e;
    while (b > 0 && is_word_char(tb->text[b - 1])) b--;
    static const char* KW[] = { "FROM", "JOIN", "INTO", "UPDATE", "TABLE", "DESCRIBE", "DESC", "TRUNCATE" };
    for (size_t i = 0; i < sizeof(KW) / sizeof(KW[0]); i++)
        if ((int)strlen(KW[i]) == e - b && ieq_n(tb->text + b, KW[i], e - b)) return true;
    return false;
}

// aggiunge le voci di un tipo che iniziano con pre (schema + parole SQL), ordinate
static void popup_add_kind(TextBox* tb, CompletionKind k, const char* pre, int plen) {
    int start = tb->popup_n;
    for (int i = 0; i < tb->comp_count; i++) {
        const CompletionItem* it = &tb->comp_items[i];
        if (it->kind == k && ieq_n(it->name, pre, plen))
            popup_add(tb, it->name, it->detail, it->parent, k);
    }
    SqlWordKind wk[2] = { SQL_WORD_IDENT, SQL_WORD_IDENT };
    const char* dt = "";
    if (k == COMP_KEYWORD) { wk[0] = SQL_WORD_KEYWORD; wk[1] = SQL_WORD_LITERAL; dt = "keyword"; }
    if (k == COMP_TYPE)    { wk[0] = SQL_WORD_TYPE; dt = "type"; }
    if (k == COMP_FUNC)    { wk[0] = SQL_WORD_FUNC; dt = "function"; }
    for (int w = 0; w < 2; w++) {
        if (wk[w] == SQL_WORD_IDENT) continue;
        const char* const* list;
        int n = sql_word_list(wk[w], &list);
        for (int i = 0; i < n; i++)
            if (ieq_n(list[i], pre, plen)) popup_add(tb, list[i], dt, NULL, k);
    }
    qsort(tb->popup_items + start, tb->popup_n - start, sizeof(CompletionItem), cmp_items);
}

// ricalcola i suggerimenti per la parola sotto il cursore.
// force = apertura esplicita (Ctrl+Space o dopo un '.')
static void popup_update(TextBox* tb, bool force) {
    int cur = tb->cursor_pos;
    if (has_sel(tb) || !in_code_at(tb, cur)) { popup_close(tb); return; }
    int p = cur;
    while (p > 0 && is_word_char(tb->text[p - 1])) p--;
    int plen = cur - p;
    bool dotted = p > 0 && tb->text[p - 1] == '.';
    if (!dotted && plen == 0 && !force) { popup_close(tb); return; }
    if (!dotted && plen < 2 && !force && !tb->popup_open) { popup_close(tb); return; }
    if (plen > 60) { popup_close(tb); return; }

    if (!tb->popup_items) {
        tb->popup_items = malloc(sizeof(CompletionItem) * TEXTBOX_POPUP_MAX);
        if (!tb->popup_items) return;
    }
    char pre[64];
    memcpy(pre, tb->text + p, plen);
    pre[plen] = '\0';
    tb->popup_n = 0;

    if (dotted) {
        // tabella.colonna: colonne di quella tabella (o tutte, se è un alias sconosciuto)
        int qe = p - 1, qs = qe;
        while (qs > 0 && is_word_char(tb->text[qs - 1])) qs--;
        if (qs == qe || qe - qs >= 64) { popup_close(tb); return; }
        char qual[64];
        memcpy(qual, tb->text + qs, qe - qs);
        qual[qe - qs] = '\0';
        bool known = false;
        for (int i = 0; i < tb->comp_count && !known; i++)
            known = tb->comp_items[i].kind == COMP_COLUMN && ieq(tb->comp_items[i].parent, qual);
        int start = tb->popup_n;
        for (int i = 0; i < tb->comp_count; i++) {
            const CompletionItem* it = &tb->comp_items[i];
            if (it->kind != COMP_COLUMN || !ieq_n(it->name, pre, plen)) continue;
            if (known && !ieq(it->parent, qual)) continue;
            popup_add(tb, it->name, it->detail, it->parent, COMP_COLUMN);
        }
        qsort(tb->popup_items + start, tb->popup_n - start, sizeof(CompletionItem), cmp_items);
    } else if (table_context(tb, p)) {
        CompletionKind order[] = { COMP_TABLE, COMP_VIEW, COMP_DATABASE, COMP_KEYWORD };
        for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) popup_add_kind(tb, order[i], pre, plen);
    } else {
        CompletionKind order[] = { COMP_COLUMN, COMP_TABLE, COMP_VIEW, COMP_KEYWORD, COMP_FUNC, COMP_TYPE, COMP_DATABASE };
        for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) popup_add_kind(tb, order[i], pre, plen);
    }

    // niente da proporre, o la parola è già completa
    if (tb->popup_n == 0 || (tb->popup_n == 1 && ieq(tb->popup_items[0].name, pre) && !force)) {
        popup_close(tb);
        return;
    }
    tb->popup_open   = true;
    tb->popup_start  = p;
    tb->popup_sel    = 0;
    tb->popup_scroll = 0;
}

static void popup_move(TextBox* tb, int delta) {
    if (!tb->popup_n) return;
    int s = tb->popup_sel + delta;
    if (delta == 1 || delta == -1) s = (s + tb->popup_n) % tb->popup_n;   // ciclico con le frecce
    tb->popup_sel = SV_CLAMP(s, 0, tb->popup_n - 1);
    if (tb->popup_sel < tb->popup_scroll) tb->popup_scroll = tb->popup_sel;
    if (tb->popup_sel >= tb->popup_scroll + TEXTBOX_POPUP_ROWS) tb->popup_scroll = tb->popup_sel - TEXTBOX_POPUP_ROWS + 1;
}

static void popup_accept(TextBox* tb) {
    if (!tb->popup_open || tb->popup_sel >= tb->popup_n) return;
    const CompletionItem* it = &tb->popup_items[tb->popup_sel];
    char buf[80];
    int n = snprintf(buf, sizeof(buf), "%s", it->name);

    // parole SQL: rispetta lo stile dell'utente (tutto minuscolo -> minuscolo)
    if (it->kind == COMP_KEYWORD || it->kind == COMP_TYPE || it->kind == COMP_FUNC) {
        bool lower = false, upper = false;
        for (int i = tb->popup_start; i < tb->cursor_pos; i++) {
            if (islower((unsigned char)tb->text[i])) lower = true;
            if (isupper((unsigned char)tb->text[i])) upper = true;
        }
        if (lower && !upper) for (int i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    }
    bool call = it->kind == COMP_FUNC && char_at(tb, tb->cursor_pos) != '(';
    if (call && n + 2 < (int)sizeof(buf)) { buf[n++] = '('; buf[n++] = ')'; buf[n] = '\0'; }

    int start = tb->popup_start, end = tb->cursor_pos;
    popup_close(tb);
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, start, end, buf, n);
    if (call) move_cursor(tb, tb->cursor_pos - 1, false, false);
    tb->last_edit_kind = EDIT_NONE;
}

// tasti con il popup aperto; true se consumato
static bool popup_handle_key(TextBox* tb, sEvent* evt) {
    if (!tb->popup_open) return false;
    bool mods = evt->modifiers & (SV_MOD_CTRL | SV_MOD_ALT | SV_MOD_SHIFT);
    switch (evt->key) {
        case KEY_UP:       if (mods) break; popup_move(tb, -1); return true;
        case KEY_DOWN:     if (mods) break; popup_move(tb, +1); return true;
        case KEY_PAGEUP:   popup_move(tb, -TEXTBOX_POPUP_ROWS); return true;
        case KEY_PAGEDOWN: popup_move(tb, +TEXTBOX_POPUP_ROWS); return true;
        case KEY_ENTER:
        case KEY_TAB:      if (mods) break; popup_accept(tb); return true;
        case KEY_ESCAPE:   popup_close(tb); return true;
        default: break;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════
//  Input
// ═════════════════════════════════════════════════════════════════

static char char_at(const TextBox* tb, int pos) {
    return (pos >= 0 && pos < tb->text_len) ? tb->text[pos] : '\0';
}

static void wrap_selection(TextBox* tb, const char* open, const char* close) {
    int a, b;
    sel_range(tb, &a, &b);
    int ol = (int)strlen(open), cl = (int)strlen(close);
    char* buf = malloc(b - a + ol + cl + 1);
    if (!buf) return;
    memcpy(buf, open, ol);
    memcpy(buf + ol, tb->text + a, b - a);
    memcpy(buf + ol + (b - a), close, cl);
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, buf, b - a + ol + cl);
    tb->sel_anchor = a + ol;
    tb->cursor_pos = b + ol;
    update_cursor_linecol(tb);
    free(buf);
}

static void insert_pair(TextBox* tb, const char* pair) {
    undo_push(tb, EDIT_OTHER);
    int a, b;
    sel_range(tb, &a, &b);
    replace_range(tb, a, b, pair, 2);
    move_cursor(tb, tb->cursor_pos - 1, false, false);
}

static void handle_char(TextBox* tb, uint32_t cp) {
    char next = char_at(tb, tb->cursor_pos);
    char prev = char_at(tb, tb->cursor_pos - 1);
    bool sel  = has_sel(tb);

    // sovrascrittura della chiusura già inserita automaticamente
    if (!sel && (cp == ')' || cp == ']') && next == (char)cp) {
        move_cursor(tb, tb->cursor_pos + 1, false, false);
        return;
    }
    if (cp == '\'' || cp == '"' || cp == '`') {
        char q[3] = {(char)cp, (char)cp, 0};
        if (!sel && next == (char)cp) { move_cursor(tb, tb->cursor_pos + 1, false, false); return; }
        if (sel) { wrap_selection(tb, q + 1, q + 1); return; }
        if (!is_word_char(prev) && !is_word_char(next)) { insert_pair(tb, q); return; }
    }
    if (cp == '(') {
        if (sel) { wrap_selection(tb, "(", ")"); return; }
        if (next == '\0' || next == ' ' || next == '\n' || next == ')' || next == ',' || next == ';') {
            insert_pair(tb, "()");
            return;
        }
    }

    char buf[5] = {0};
    utf8_encode(cp, buf);
    insert_text(tb, buf, EDIT_TYPE);
    if (cp == ' ') tb->last_edit_kind = EDIT_NONE; // ogni parola è un passo di annulla
}

static void handle_enter(TextBox* tb) {
    int l = tb->cursor_line;
    int ls = line_start(tb, l);
    int indent = 0;
    while (ls + indent < tb->cursor_pos && tb->text[ls + indent] == ' ') indent++;

    // carattere non-spazio prima del cursore
    int p = tb->cursor_pos;
    while (p > ls && tb->text[p - 1] == ' ') p--;
    bool after_open = p > ls && tb->text[p - 1] == '(';
    bool before_close = char_at(tb, tb->cursor_pos) == ')';

    char buf[512];
    int n = 0;
    buf[n++] = '\n';
    int ind = SV_MIN(indent + (after_open ? INDENT : 0), 200);
    for (int i = 0; i < ind; i++) buf[n++] = ' ';
    int cursor_at = n;
    if (after_open && before_close) {
        buf[n++] = '\n';
        for (int i = 0; i < SV_MIN(indent, 200); i++) buf[n++] = ' ';
    }
    buf[n] = '\0';

    int a, b;
    sel_range(tb, &a, &b);
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, buf, n);
    move_cursor(tb, a + cursor_at, false, false);
}

static void handle_backspace(TextBox* tb, bool ctrl) {
    if (has_sel(tb)) {
        int a, b;
        sel_range(tb, &a, &b);
        delete_range(tb, a, b, EDIT_OTHER);
        return;
    }
    int pos = tb->cursor_pos;
    if (pos == 0) return;
    if (ctrl) { delete_range(tb, word_left(tb, pos), pos, EDIT_OTHER); return; }

    char prev = char_at(tb, pos - 1), next = char_at(tb, pos);
    // coppia vuota "()" o "''": cancella entrambi
    if ((prev == '(' && next == ')') || ((prev == '\'' || prev == '"' || prev == '`') && next == prev)) {
        delete_range(tb, pos - 1, pos + 1, EDIT_DELETE);
        return;
    }
    // indentazione: torna al multiplo di INDENT precedente
    int ls = line_start(tb, tb->cursor_line);
    bool only_spaces = true;
    for (int i = ls; i < pos; i++) if (tb->text[i] != ' ') { only_spaces = false; break; }
    if (only_spaces && pos - ls > 0) {
        int col = pos - ls;
        int target = ((col - 1) / INDENT) * INDENT;
        delete_range(tb, ls + target, pos, EDIT_DELETE);
        return;
    }
    delete_range(tb, utf8_prev(tb, pos), pos, EDIT_DELETE);
}

static void handle_delete(TextBox* tb, bool ctrl) {
    if (has_sel(tb)) {
        int a, b;
        sel_range(tb, &a, &b);
        delete_range(tb, a, b, EDIT_OTHER);
        return;
    }
    int pos = tb->cursor_pos;
    if (pos >= tb->text_len) return;
    int end = ctrl ? word_right(tb, pos) : utf8_next(tb, pos);
    delete_range(tb, pos, end, EDIT_DELETE);
}

static void zoom(TextBox* tb, int delta) {
    if (delta == 0) tb->font_size = TH_FONT_MONO;
    else tb->font_size = SV_CLAMP(tb->font_size + delta, 10, 28);
}

static bool handle_key_inner(TextBox* tb, sEvent* evt);

static bool handle_key(TextBox* tb, sEvent* evt) {
    if (popup_handle_key(tb, evt)) return true;
    if (evt->key == KEY_LETTER && (evt->modifiers & SV_MOD_CTRL) && evt->character == ' ') {
        popup_update(tb, true);   // Ctrl+Space
        return true;
    }
    bool was_open = tb->popup_open;
    bool handled = handle_key_inner(tb, evt);
    if (was_open) {
        bool plain = !(evt->modifiers & (SV_MOD_CTRL | SV_MOD_ALT));
        if (plain && (evt->key == KEY_BACKSPACE || evt->key == KEY_DELETE ||
                      evt->key == KEY_LEFT || evt->key == KEY_RIGHT)) {
            tb->popup_open = true;  // replace_range lo chiude: riapre filtrando di nuovo
            popup_update(tb, false);
        } else if (evt->key != KEY_NONE) {
            popup_close(tb);
        }
    }
    return handled;
}

static bool handle_key_inner(TextBox* tb, sEvent* evt) {
    bool ctrl  = evt->modifiers & SV_MOD_CTRL;
    bool shift = evt->modifiers & SV_MOD_SHIFT;
    bool alt   = evt->modifiers & SV_MOD_ALT;
    int  vis   = visible_lines(tb);

    switch (evt->key) {
        case KEY_ENTER:
            if (ctrl && shift) {
                open_line(tb, true);              // Ctrl+Shift+Enter: nuova riga sopra
            } else if (ctrl) {
                if (tb->on_execute) tb->on_execute(tb->text, tb->base.user_data);
            } else if (shift) {
                open_line(tb, false);             // Shift+Enter: nuova riga sotto
            } else {
                handle_enter(tb);
            }
            return true;

        case KEY_BACKSPACE: handle_backspace(tb, ctrl); return true;
        case KEY_DELETE:    handle_delete(tb, ctrl);    return true;

        case KEY_LEFT: {
            int a, b;
            sel_range(tb, &a, &b);
            if (has_sel(tb) && !shift) { move_cursor(tb, a, false, false); return true; }
            int p = ctrl ? word_left(tb, tb->cursor_pos) : utf8_prev(tb, tb->cursor_pos);
            move_cursor(tb, p, shift, false);
            return true;
        }
        case KEY_RIGHT: {
            int a, b;
            sel_range(tb, &a, &b);
            if (has_sel(tb) && !shift) { move_cursor(tb, b, false, false); return true; }
            int p = ctrl ? word_right(tb, tb->cursor_pos) : utf8_next(tb, tb->cursor_pos);
            move_cursor(tb, p, shift, false);
            return true;
        }
        case KEY_UP:
            if (alt) { move_lines(tb, -1); return true; }
            if (ctrl) { tb->scroll_y--; clamp_scroll(tb); return true; }
            if (tb->cursor_line == 0) move_cursor(tb, 0, shift, false);
            else move_cursor(tb, offset_at_col(tb, tb->cursor_line - 1, tb->pref_col), shift, true);
            return true;

        case KEY_DOWN:
            if (alt) { move_lines(tb, +1); return true; }
            if (ctrl) { tb->scroll_y++; clamp_scroll(tb); return true; }
            if (tb->cursor_line >= tb->num_lines - 1) move_cursor(tb, tb->text_len, shift, false);
            else move_cursor(tb, offset_at_col(tb, tb->cursor_line + 1, tb->pref_col), shift, true);
            return true;

        case KEY_PAGEUP: {
            int l = SV_MAX(0, tb->cursor_line - vis);
            tb->scroll_y -= vis;
            move_cursor(tb, offset_at_col(tb, l, tb->pref_col), shift, true);
            return true;
        }
        case KEY_PAGEDOWN: {
            int l = SV_MIN(tb->num_lines - 1, tb->cursor_line + vis);
            tb->scroll_y += vis;
            clamp_scroll(tb);
            move_cursor(tb, offset_at_col(tb, l, tb->pref_col), shift, true);
            return true;
        }
        case KEY_HOME: {
            if (ctrl) { move_cursor(tb, 0, shift, false); return true; }
            // "smart home": primo carattere non spazio, poi inizio riga
            int fns = first_non_space(tb, tb->cursor_line);
            int ls  = line_start(tb, tb->cursor_line);
            move_cursor(tb, tb->cursor_pos == fns ? ls : fns, shift, false);
            return true;
        }
        case KEY_END:
            if (ctrl) move_cursor(tb, tb->text_len, shift, false);
            else      move_cursor(tb, line_end(tb, tb->cursor_line), shift, false);
            return true;

        case KEY_TAB: {
            if (shift) { indent_lines(tb, true); return true; }
            if (has_sel(tb)) {
                int a, b;
                sel_range(tb, &a, &b);
                if (line_of(tb, a) != line_of(tb, b)) { indent_lines(tb, false); return true; }
            }
            int spaces = INDENT - (tb->cursor_col % INDENT);
            char buf[INDENT + 1] = "    ";
            buf[spaces] = '\0';
            insert_text(tb, buf, EDIT_OTHER);
            return true;
        }
        case KEY_ESCAPE:
            if (has_sel(tb)) { tb->sel_anchor = -1; return true; }
            return false;

        case KEY_LETTER:
            if (!ctrl) return false;
            switch (evt->character) {
                case 'a':
                    tb->sel_anchor = 0;
                    tb->cursor_pos = tb->text_len;
                    update_cursor_linecol(tb);
                    return true;
                case 'c': copy_to_clipboard(tb, false); return true;
                case 'x': copy_to_clipboard(tb, true);  return true;
                case 'v': paste_from_clipboard(tb);     return true;
                case 'z': if (shift) do_redo(tb); else do_undo(tb); return true;
                case 'y': do_redo(tb); return true;
                case 'd': duplicate(tb); return true;
                case 'k': if (shift) { delete_lines(tb); return true; } return false;
                case 'u': change_case(tb, shift); return true;
                case 'l': select_line_extend(tb); return true;
                case '/': toggle_comment(tb); return true;
                case '7': if (shift) { toggle_comment(tb); return true; } return false; // Ctrl+/ su tastiera italiana
                case '=': case '+': zoom(tb, +1); return true;
                case '-': zoom(tb, -1); return true;
                case '0': zoom(tb, 0);  return true;
                default: return false;
            }

        default:
            return false;
    }
}

static bool textbox_handle_event(Widget* self, sEvent* evt) {
    TextBox* tb = (TextBox*)self;
    bool focused = self->state == WIDGET_STATE_FOCUSED;

    // popup di completamento (può sporgere fuori dall'editor)
    if (tb->popup_open && (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_WHEEL || evt->type == EVT_MOUSE_MOVE)) {
        Rect pr = tb->popup_rect;
        bool in = pr.w > 0 && evt->mouse_x >= pr.x && evt->mouse_x < pr.x + pr.w &&
                  evt->mouse_y >= pr.y && evt->mouse_y < pr.y + pr.h;
        if (in) {
            int row = tb->popup_scroll + (evt->mouse_y - pr.y - POPUP_PAD) / POPUP_ROW_H;
            row = SV_CLAMP(row, 0, tb->popup_n - 1);
            if (evt->type == EVT_MOUSE_WHEEL) {
                int max_s = SV_MAX(0, tb->popup_n - TEXTBOX_POPUP_ROWS);
                tb->popup_scroll = SV_CLAMP(tb->popup_scroll - evt->wheel_delta, 0, max_s);
            } else if (evt->type == EVT_MOUSE_MOVE) {
                if (tb->dragging) return false;
                tb->popup_sel = row;
            } else {
                tb->popup_sel = row;
                popup_accept(tb);
                self->state = WIDGET_STATE_FOCUSED;
            }
            return true;
        }
        if (evt->type == EVT_MOUSE_DOWN) popup_close(tb);
    }

    switch (evt->type) {
        case EVT_MOUSE_DOWN: {
            if (!widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                if (focused) self->state = WIDGET_STATE_NORMAL; // click fuori: perde il focus
                return false;
            }
            self->state = WIDGET_STATE_FOCUSED;
            reset_blink(tb);
            if (evt->mouse_button != SV_MOUSE_LEFT) return true;

            uint64_t now = platform_time_ms();
            if (now - tb->last_click_ms < 400 &&
                abs(evt->mouse_x - tb->last_click_x) < 5 && abs(evt->mouse_y - tb->last_click_y) < 5)
                tb->click_count = tb->click_count % 3 + 1;
            else
                tb->click_count = 1;
            tb->last_click_ms = now;
            tb->last_click_x  = evt->mouse_x;
            tb->last_click_y  = evt->mouse_y;

            int off = hit_offset(tb, evt->mouse_x, evt->mouse_y);
            bool in_gutter = evt->mouse_x < self->bounds.x + tb->gutter_w;

            if (in_gutter) {
                int l = line_of(tb, off);
                select_lines(tb, l, l);
            } else if (tb->click_count == 2) {
                select_word_at(tb, off);
            } else if (tb->click_count == 3) {
                int l = line_of(tb, off);
                select_lines(tb, l, l);
            } else if (evt->modifiers & SV_MOD_SHIFT) {
                move_cursor(tb, off, true, false);
            } else {
                move_cursor(tb, off, false, false);
                tb->sel_anchor = tb->cursor_pos;
            }
            tb->dragging = tb->click_count == 1;
            tb->last_edit_kind = EDIT_NONE;
            return true;
        }

        case EVT_MOUSE_MOVE:
            if (tb->dragging) {
                Rect b = self->bounds;
                // auto-scroll trascinando oltre i bordi
                if (evt->mouse_y < b.y) tb->scroll_y--;
                else if (evt->mouse_y > b.y + b.h) tb->scroll_y++;
                clamp_scroll(tb);
                int off = hit_offset(tb, evt->mouse_x, evt->mouse_y);
                if (tb->sel_anchor < 0) tb->sel_anchor = tb->cursor_pos;
                tb->cursor_pos = off;
                update_cursor_linecol(tb);
                tb->pref_col = tb->cursor_col;
                ensure_visible(tb);
                reset_blink(tb);
                return true;
            }
            return false;

        case EVT_MOUSE_UP:
            if (tb->dragging) { tb->dragging = false; return true; }
            return false;

        case EVT_MOUSE_WHEEL:
            if (!widget_contains_point(self, evt->mouse_x, evt->mouse_y)) return false;
            if (evt->modifiers & SV_MOD_CTRL)       zoom(tb, evt->wheel_delta > 0 ? 1 : -1);
            else if (evt->modifiers & SV_MOD_SHIFT) tb->scroll_x -= evt->wheel_delta * tb->char_w * 6;
            else                                    tb->scroll_y -= evt->wheel_delta * 3;
            clamp_scroll(tb);
            return true;

        case EVT_CHAR: {
            if (!focused) return false;
            uint32_t cp = evt->character;
            handle_char(tb, cp);
            if (cp == '_' || (cp < 0x80 && isalnum((int)cp))) popup_update(tb, false);
            else if (cp == '.') popup_update(tb, true);
            else popup_close(tb);
            return true;
        }

        case EVT_KEY_DOWN:
            if (!focused) return false;
            return handle_key(tb, evt);

        default: break;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════
//  Disegno con syntax highlighting
// ═════════════════════════════════════════════════════════════════

static bool search_match_at(const TextBox* tb, int i);

typedef enum { LX_NORMAL, LX_BLOCK_COMMENT, LX_STRING_SQ, LX_STRING_DQ, LX_STRING_BT } LexState;

// stato del lexer all'inizio dell'offset `upto` (commenti /* */ e stringhe su più righe)
static LexState lex_state_at(const TextBox* tb, int upto) {
    LexState st = LX_NORMAL;
    const char* s = tb->text;
    for (int i = 0; i < upto; i++) {
        char c = s[i];
        switch (st) {
            case LX_NORMAL:
                if (c == '-' && i + 1 < upto && s[i + 1] == '-') {
                    while (i < upto && s[i] != '\n') i++;
                } else if (c == '/' && i + 1 < upto && s[i + 1] == '*') {
                    st = LX_BLOCK_COMMENT; i++;
                } else if (c == '\'') st = LX_STRING_SQ;
                else if (c == '"')    st = LX_STRING_DQ;
                else if (c == '`')    st = LX_STRING_BT;
                break;
            case LX_BLOCK_COMMENT:
                if (c == '*' && i + 1 < upto && s[i + 1] == '/') { st = LX_NORMAL; i++; }
                break;
            case LX_STRING_SQ: if (c == '\\') i++; else if (c == '\'') st = LX_NORMAL; break;
            case LX_STRING_DQ: if (c == '\\') i++; else if (c == '"')  st = LX_NORMAL; break;
            case LX_STRING_BT: if (c == '`') st = LX_NORMAL; break;
        }
    }
    return st;
}

// true se pos non è dentro una stringa o un commento
static bool in_code_at(const TextBox* tb, int pos) {
    if (lex_state_at(tb, pos) != LX_NORMAL) return false;
    // commento "--" sulla stessa riga prima di pos
    int ls = line_start(tb, line_of(tb, pos));
    LexState st = lex_state_at(tb, ls);
    if (st != LX_NORMAL) return false;
    for (int i = ls; i + 1 < pos; i++) {
        char c = tb->text[i];
        if (c == '\'' || c == '"' || c == '`') {           // salta la stringa
            int j = i + 1;
            while (j < pos && tb->text[j] != c) j++;
            i = j;
        } else if (c == '-' && tb->text[i + 1] == '-') {
            return false;
        }
    }
    return true;
}

typedef struct {
    PlatformWindow* win;
    TextBox* tb;
    int x0, y, col;
    int clip_l, clip_r;
} SpanCtx;

static void draw_span(SpanCtx* c, int from, int to, Color color, FontStyle style) {
    if (to <= from) return;
    const char* s = c->tb->text + from;
    int n = to - from;
    int chars = count_chars(s, n);
    int x = c->x0 + c->col * c->tb->char_w;
    int w = chars * c->tb->char_w;
    if (x + w >= c->clip_l && x <= c->clip_r)
        platform_draw_text_ex(c->win, s, n, (Point){x, c->y}, color, c->tb->font_size, style);
    c->col += chars;
}

// disegna una riga colorando i token; ritorna lo stato del lexer alla fine della riga
// keyword dopo le quali un nome seguito da '(' è una tabella, non una funzione
static bool names_object(const char* w, int len) {
    static const char* KW[] = { "TABLE", "INTO", "VIEW", "INDEX", "ON", "REFERENCES", "EXISTS",
                                "UPDATE", "FROM", "JOIN", "TRIGGER" };
    for (size_t k = 0; k < sizeof(KW) / sizeof(KW[0]); k++) {
        int kl = (int)strlen(KW[k]);
        if (kl != len) continue;
        int j = 0;
        while (j < len && toupper((unsigned char)w[j]) == KW[k][j]) j++;
        if (j == len) return true;
    }
    return false;
}

static LexState draw_line_tokens(SpanCtx* c, int ls, int le, LexState st) {
    const char* s = c->tb->text;
    int i = ls;
    int prev_w = -1, prev_len = 0;   // parola precedente sulla riga
    while (i < le) {
        if (st == LX_BLOCK_COMMENT) {
            int j = i;
            while (j < le && !(s[j] == '*' && j + 1 < le && s[j + 1] == '/')) j++;
            if (j < le) { j += 2; st = LX_NORMAL; }
            draw_span(c, i, j, TH_SYN_COMMENT, FONT_MONO);
            i = j;
            continue;
        }
        if (st == LX_STRING_SQ || st == LX_STRING_DQ || st == LX_STRING_BT) {
            char q = st == LX_STRING_SQ ? '\'' : st == LX_STRING_DQ ? '"' : '`';
            int j = i;
            while (j < le && s[j] != q) { if (s[j] == '\\' && q != '`') j++; j++; }
            if (j < le) { j++; st = LX_NORMAL; }
            j = SV_MIN(j, le);
            draw_span(c, i, j, st == LX_STRING_BT || q == '`' ? TH_SYN_IDENT : TH_SYN_STRING, FONT_MONO);
            i = j;
            continue;
        }

        char ch = s[i];
        if (ch == ' ') {
            int j = i;
            while (j < le && s[j] == ' ') j++;
            c->col += j - i;
            i = j;
        } else if (ch == '-' && i + 1 < le && s[i + 1] == '-') {
            draw_span(c, i, le, TH_SYN_COMMENT, FONT_MONO);
            i = le;
        } else if (ch == '/' && i + 1 < le && s[i + 1] == '*') {
            st = LX_BLOCK_COMMENT;
            draw_span(c, i, i + 2, TH_SYN_COMMENT, FONT_MONO);
            i += 2;
        } else if (ch == '\'') { st = LX_STRING_SQ; draw_span(c, i, i + 1, TH_SYN_STRING, FONT_MONO); i++; }
        else if (ch == '"')    { st = LX_STRING_DQ; draw_span(c, i, i + 1, TH_SYN_STRING, FONT_MONO); i++; }
        else if (ch == '`')    { st = LX_STRING_BT; draw_span(c, i, i + 1, TH_SYN_IDENT,  FONT_MONO); i++; }
        else if (isdigit((unsigned char)ch) && (i == ls || !is_word_char(s[i - 1]))) {
            int j = i;
            while (j < le && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            draw_span(c, i, j, TH_SYN_NUMBER, FONT_MONO);
            i = j;
        } else if (is_word_char(ch)) {
            int j = i;
            while (j < le && is_word_char(s[j])) j++;
            SqlWordKind k = sql_classify_word(s + i, j - i);
            int p = j;
            while (p < le && s[p] == ' ') p++;
            bool call = p < le && s[p] == '(';
            bool after_dot = i > ls && s[i - 1] == '.';
            Color col; FontStyle fs = FONT_MONO;
            if (after_dot)                     col = TH_SYN_IDENT;
            else if (k == SQL_WORD_KEYWORD)  { col = TH_SYN_KEYWORD; fs = FONT_MONO_BOLD; }
            else if (k == SQL_WORD_TYPE)       col = TH_SYN_TYPE;
            else if (k == SQL_WORD_LITERAL)    col = TH_SYN_NUMBER;
            else if (k == SQL_WORD_FUNC || (call && !(prev_w >= 0 && names_object(s + prev_w, prev_len))))
                                               col = TH_SYN_FUNC;
            else                               col = TH_SYN_IDENT;
            draw_span(c, i, j, col, fs);
            prev_w = i; prev_len = j - i;
            i = j;
        } else if (strchr("=<>!+-*/%|&^~?", ch)) {
            draw_span(c, i, i + 1, TH_SYN_OPERATOR, FONT_MONO);
            i++;
        } else {
            int j = utf8_next(c->tb, i);
            draw_span(c, i, SV_MIN(j, le), TH_SYN_PUNCT, FONT_MONO);
            i = j;
        }
    }
    return st;
}

// trova la parentesi corrispondente a quella in pos (o -1)
static int find_match(const TextBox* tb, int pos) {
    char c = char_at(tb, pos);
    const char* open = "([", *close = ")]";
    const char* o = strchr(open, c);
    const char* cl = strchr(close, c);
    if (!c || (!o && !cl)) return -1;
    char want = o ? close[o - open] : open[cl - close];
    int dir = o ? 1 : -1, depth = 0;
    int limit = 20000;
    for (int i = pos; i >= 0 && i < tb->text_len && limit-- > 0; i += dir) {
        if (tb->text[i] == c) depth++;
        else if (tb->text[i] == want && --depth == 0) return i;
    }
    return -1;
}

static void textbox_draw(Widget* self, PlatformWindow* win) {
    TextBox* tb = (TextBox*)self;
    Rect b = self->bounds;
    bool focused = self->state == WIDGET_STATE_FOCUSED;
    tb->win = win;

    // -- metriche --
    tb->char_w      = SV_MAX(1, platform_measure_text_ex(win, "MMMMMMMMMM", 10, tb->font_size, FONT_MONO) / 10);
    tb->line_height = (tb->font_size * 16 + 5) / 10 + 2;
    int text_h      = platform_text_height(win, tb->font_size, FONT_MONO);
    int digits = 1;
    for (int n = tb->num_lines; n >= 10; n /= 10) digits++;
    digits = SV_MAX(digits, 3);
    int gutter_size = SV_MAX(10, tb->font_size - 1);
    int gutter_cw = SV_MAX(1, platform_measure_text_ex(win, "0000000000", 10, gutter_size, FONT_MONO) / 10);
    tb->gutter_w = tb->show_line_nums ? digits * gutter_cw + 28 : 0;
    clamp_scroll(tb);

    platform_fill_rect(win, b, TH_EDITOR_BG);
    platform_set_clip(win, b);

    int vis   = visible_lines(tb);
    int first = tb->scroll_y;
    int last  = SV_MIN(tb->num_lines, first + vis + 1);
    int x0    = text_x0(tb) - tb->scroll_x;
    int tx_l  = b.x + tb->gutter_w;   // bordo sinistro area testo

    int sa, se;
    sel_range(tb, &sa, &se);
    bool sel = has_sel(tb);

    // statement corrente (quello che Ctrl+Enter eseguirebbe)
    int st_l0 = -1, st_l1 = -1;
    if (!sel) {
        SqlRange r;
        if (sql_statement_at(tb->text, tb->text_len, tb->cursor_pos, &r)) {
            st_l0 = line_of(tb, r.code_start);
            st_l1 = line_of(tb, SV_MAX(r.code_start, r.end_semi - 1));
        }
    } else {
        st_l0 = line_of(tb, sa);
        st_l1 = line_of(tb, se > sa ? se - 1 : se);
    }

    // parentesi corrispondenti (prima o dopo il cursore)
    int br_a = -1, br_b = -1;
    if (focused) {
        int m = find_match(tb, tb->cursor_pos);
        if (m >= 0) { br_a = tb->cursor_pos; br_b = m; }
        else if ((m = find_match(tb, tb->cursor_pos - 1)) >= 0) { br_a = tb->cursor_pos - 1; br_b = m; }
    }

    // occorrenze della parola sotto il cursore (o della parola selezionata)
    int occ_a = -1, occ_len = 0;
    if (focused) {
        if (sel) {
            bool word = se - sa >= 2 && se - sa <= 64 && line_of(tb, sa) == line_of(tb, se);
            for (int i = sa; word && i < se; i++) if (!is_word_char(tb->text[i])) word = false;
            if (word) { occ_a = sa; occ_len = se - sa; }
        } else {
            int a = tb->cursor_pos, e = tb->cursor_pos;
            while (a > 0 && is_word_char(tb->text[a - 1])) a--;
            while (e < tb->text_len && is_word_char(tb->text[e])) e++;
            if (e - a >= 2 && e - a <= 64 && sql_classify_word(tb->text + a, e - a) == SQL_WORD_IDENT) {
                occ_a = a; occ_len = e - a;
            }
        }
    }

    LexState st = lex_state_at(tb, line_start(tb, first));

    for (int l = first; l < last; l++) {
        int y  = b.y + tb->pad_top + (l - first) * tb->line_height;
        int ty = y + (tb->line_height - text_h) / 2;
        int ls = line_start(tb, l), le = line_end(tb, l);

        // sfondo riga corrente / errore
        if (l == tb->error_line) {
            platform_fill_rect(win, (Rect){b.x, y, b.w, tb->line_height}, TH_ERROR_SOFT);
        } else if (focused && !sel && l == tb->cursor_line) {
            platform_fill_rect(win, (Rect){b.x, y, b.w, tb->line_height}, TH_EDITOR_LINE);
        }

        platform_set_clip(win, (Rect){tx_l, b.y, b.w - tb->gutter_w, b.h});

        // risultati della ricerca
        int slen = tb->search[0] ? (int)strlen(tb->search) : 0;
        if (slen) {
            for (int i = ls; i + slen <= le; i++) {
                if (!search_match_at(tb, i)) continue;
                bool cur = sel && i == sa && se - sa == slen;
                int c0 = count_chars(tb->text + ls, i - ls);
                Rect mr = {x0 + c0 * tb->char_w, y + 1, count_chars(tb->text + i, slen) * tb->char_w, tb->line_height - 2};
                if (cur) ui_round_box(win, mr, TH_EDITOR_FIND_CUR, TH_WARNING, 3);
                else     platform_fill_rect_rounded(win, mr, TH_EDITOR_FIND, 3);
                i += slen - 1;
            }
        }

        // selezione
        bool sel_is_match = slen && sel && se - sa == slen && search_match_at(tb, sa);
        if (sel && !sel_is_match && sa <= le && se > ls) {
            int a = SV_MAX(sa, ls), e = SV_MIN(se, le);
            int c0 = count_chars(tb->text + ls, a - ls);
            int c1 = c0 + count_chars(tb->text + a, e - a);
            int w  = (c1 - c0) * tb->char_w + (se > le ? tb->char_w / 2 : 0);
            platform_fill_rect_rounded(win, (Rect){x0 + c0 * tb->char_w, y, w, tb->line_height},
                                       TH_EDITOR_SELECT, 3);
        }

        // altre occorrenze della parola
        if (occ_a >= 0) {
            for (int i = ls; i + occ_len <= le; i++) {
                if (i == occ_a || (i > 0 && is_word_char(tb->text[i - 1]))) continue;
                if (i + occ_len < tb->text_len && is_word_char(tb->text[i + occ_len])) continue;
                int k = 0;
                while (k < occ_len && tolower((unsigned char)tb->text[i + k]) ==
                                      tolower((unsigned char)tb->text[occ_a + k])) k++;
                if (k < occ_len) continue;
                int c0 = count_chars(tb->text + ls, i - ls);
                platform_fill_rect_rounded(win, (Rect){x0 + c0 * tb->char_w, y + 1,
                                                       count_chars(tb->text + i, occ_len) * tb->char_w,
                                                       tb->line_height - 2}, TH_EDITOR_OCCUR, 3);
                i += occ_len - 1;
            }
        }

        // parentesi
        for (int k = 0; k < 2; k++) {
            int bp = k == 0 ? br_a : br_b;
            if (bp >= ls && bp < le) {
                int c0 = count_chars(tb->text + ls, bp - ls);
                ui_round_box(win, (Rect){x0 + c0 * tb->char_w - 1, y + 2, tb->char_w + 2, tb->line_height - 4},
                             TH_EDITOR_MATCH, TH_BORDER_STRONG, 3);
            }
        }

        // testo
        SpanCtx ctx = { win, tb, x0, ty, 0, tx_l, b.x + b.w };
        st = draw_line_tokens(&ctx, ls, le, st);
        if (le < tb->text_len && st == LX_NORMAL) { /* fine riga */ }

        platform_set_clip(win, b);

        // gutter
        if (tb->show_line_nums) {
            char num[16];
            snprintf(num, sizeof(num), "%d", l + 1);
            int nw = platform_measure_text_ex(win, num, -1, gutter_size, FONT_MONO);
            Color nc = (l == tb->cursor_line) ? TH_GUTTER_ACTIVE : TH_GUTTER_TEXT;
            if (l == tb->error_line) nc = TH_ERROR;
            int gy = y + (tb->line_height - platform_text_height(win, gutter_size, FONT_MONO)) / 2;
            platform_draw_text_ex(win, num, -1, (Point){b.x + tb->gutter_w - 14 - nw, gy}, nc, gutter_size, FONT_MONO);

            // barra dello statement corrente
            if (l >= st_l0 && l <= st_l1) {
                Color bc = focused ? TH_ACCENT : ui_alpha(TH_ACCENT, 90);
                int top = (l == st_l0) ? y + 2 : y;
                int bot = (l == st_l1) ? y + tb->line_height - 2 : y + tb->line_height;
                platform_fill_rect(win, (Rect){b.x + tb->gutter_w - 6, top, 2, bot - top}, bc);
            }
            if (l == tb->error_line)
                ui_circle(win, b.x + 8, y + tb->line_height / 2, 3, TH_ERROR);
        }
    }

    // placeholder
    if (tb->text_len == 0 && tb->placeholder) {
        int y = b.y + tb->pad_top + (tb->line_height - text_h) / 2;
        platform_draw_text_ex(win, tb->placeholder, -1, (Point){text_x0(tb), y}, TH_TEXT_FAINT,
                              tb->font_size, FONT_MONO);
    }

    // dissolvenza sul bordo inferiore (la riga tagliata a metà non "sporca" lo splitter)
    for (int k = 0; k < 4; k++)
        platform_fill_rect(win, (Rect){b.x, b.y + b.h - 8 + k * 2, b.w, 2},
                           ui_alpha(TH_EDITOR_BG, (uint8_t)(90 + k * 55)));

    // cursore
    if (focused && tb->cursor_visible &&
        tb->cursor_line >= first && tb->cursor_line < first + vis + 1) {
        int cx = x0 + tb->cursor_col * tb->char_w;
        int cy = b.y + tb->pad_top + (tb->cursor_line - first) * tb->line_height;
        if (cx >= tx_l) {
            platform_fill_rect(win, (Rect){cx, cy + 2, 2, tb->line_height - 4}, TH_CARET);
        }
    }

    platform_reset_clip(win);
}

static void textbox_destroy_fn(Widget* self) {
    TextBox* tb = (TextBox*)self;
    free(tb->text);
    free(tb->line_starts);
    for (int i = 0; i < tb->undo_count; i++) free(tb->undo[i].text);
    for (int i = 0; i < tb->redo_count; i++) free(tb->redo[i].text);
    free(tb->comp_items);
    free(tb->popup_items);
}

// ═════════════════════════════════════════════════════════════════
//  API pubblica
// ═════════════════════════════════════════════════════════════════

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

    tb->text_cap   = 4096;
    tb->text       = (char*)calloc(tb->text_cap, 1);
    tb->sel_anchor = -1;
    tb->cursor_visible = true;
    tb->font_size   = TH_FONT_MONO;
    tb->line_height = 22;
    tb->char_w      = 8;
    tb->gutter_w    = 44;
    tb->pad_top     = 8;
    tb->show_line_nums = true;
    tb->error_line  = -1;
    tb->placeholder = "-- Write SQL here. Ctrl+Enter runs the statement under the cursor, F5 runs everything";
    rebuild_lines(tb);

    return tb;
}

void textbox_set_text(TextBox* tb, const char* text) {
    if (!tb || !text) return;
    if (tb->text_len > 0) undo_push(tb, EDIT_OTHER);
    tb->sel_anchor = -1;
    replace_range(tb, 0, tb->text_len, text, -1);
    tb->last_edit_kind = EDIT_NONE;
}

const char* textbox_get_text(TextBox* tb) {
    return tb ? tb->text : "";
}

void textbox_clear(TextBox* tb) {
    if (!tb || tb->text_len == 0) return;
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, 0, tb->text_len, "", 0);
    tb->scroll_x = tb->scroll_y = 0;
}

void textbox_insert(TextBox* tb, const char* text) {
    if (!tb || !text) return;
    insert_text(tb, text, EDIT_OTHER);
    tb->last_edit_kind = EDIT_NONE;
}

void textbox_replace_text(TextBox* tb, int start, int end, const char* text, bool select) {
    if (!tb || !text) return;
    start = SV_CLAMP(start, 0, tb->text_len);
    end   = SV_CLAMP(end, start, tb->text_len);
    int top = tb->scroll_y;
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, start, end, text, -1);
    if (select) {
        tb->sel_anchor = start;
        update_cursor_linecol(tb);
    }
    tb->scroll_y = top;   // mantiene la vista (ensure_visible la corregge se serve)
    ensure_visible(tb);
    tb->last_edit_kind = EDIT_NONE;
}

bool textbox_tick(TextBox* tb, uint64_t now_ms) {
    if (!tb || tb->base.state != WIDGET_STATE_FOCUSED) return false;
    if (now_ms - tb->cursor_blink >= 530) {
        tb->cursor_visible = !tb->cursor_visible;
        tb->cursor_blink   = now_ms;
        return true;
    }
    return false;
}

void textbox_focus(TextBox* tb, bool focused) {
    if (!tb) return;
    tb->base.state = focused ? WIDGET_STATE_FOCUSED : WIDGET_STATE_NORMAL;
    reset_blink(tb);
}

bool textbox_has_selection(TextBox* tb) { return tb && has_sel(tb); }

void textbox_get_selection_range(TextBox* tb, int* start, int* end) {
    sel_range(tb, start, end);
}

char* textbox_get_selection(TextBox* tb) {
    if (!tb || !has_sel(tb)) return NULL;
    int a, b;
    sel_range(tb, &a, &b);
    char* s = malloc(b - a + 1);
    if (!s) return NULL;
    memcpy(s, tb->text + a, b - a);
    s[b - a] = '\0';
    return s;
}

int textbox_get_run_text(TextBox* tb, char** out) {
    *out = NULL;
    if (!tb) return -1;
    int a, b;
    if (has_sel(tb)) {
        sel_range(tb, &a, &b);
    } else {
        SqlRange r;
        if (!sql_statement_at(tb->text, tb->text_len, tb->cursor_pos, &r)) return -1;
        a = r.code_start;
        b = r.end_semi;
    }
    char* s = malloc(b - a + 1);
    if (!s) return -1;
    memcpy(s, tb->text + a, b - a);
    s[b - a] = '\0';
    *out = s;
    return a;
}

int textbox_line_of_offset(TextBox* tb, int offset) {
    return line_of(tb, SV_CLAMP(offset, 0, tb->text_len));
}

int textbox_visible_lines(TextBox* tb) { return visible_lines(tb); }

void textbox_set_error_line(TextBox* tb, int line) {
    if (!tb) return;
    tb->error_line = (line >= 0 && line < tb->num_lines) ? line : -1;
}

void textbox_scroll_to_line(TextBox* tb, int line) {
    if (!tb) return;
    int vis = visible_lines(tb);
    if (line < tb->scroll_y || line >= tb->scroll_y + vis)
        tb->scroll_y = SV_MAX(0, line - vis / 3);
    clamp_scroll(tb);
}

// ═════════════════════════════════════════════════════════════════
//  Ricerca e sostituzione
// ═════════════════════════════════════════════════════════════════

// true se la ricerca corrente corrisponde a partire dall'offset i
static bool search_match_at(const TextBox* tb, int i) {
    int n = (int)strlen(tb->search);
    if (n == 0 || i < 0 || i + n > tb->text_len) return false;
    for (int k = 0; k < n; k++) {
        unsigned char a = (unsigned char)tb->text[i + k], b = (unsigned char)tb->search[k];
        if (a != b && (tb->search_case || a >= 0x80 || tolower(a) != tolower(b))) return false;
    }
    if (tb->search_word) {
        if (i > 0 && is_word_char(tb->text[i - 1])) return false;
        if (i + n < tb->text_len && is_word_char(tb->text[i + n])) return false;
    }
    return true;
}

void textbox_set_search(TextBox* tb, const char* query, bool match_case, bool whole_word) {
    if (!tb) return;
    snprintf(tb->search, sizeof(tb->search), "%s", query ? query : "");
    // la ricerca è su una sola riga: tronca a capo eventuali
    char* nl = strchr(tb->search, '\n');
    if (nl) *nl = '\0';
    tb->search_case = match_case;
    tb->search_word = whole_word;
}

static void select_match(TextBox* tb, int at) {
    int n = (int)strlen(tb->search);
    tb->sel_anchor = at;
    tb->cursor_pos = at + n;
    update_cursor_linecol(tb);
    tb->pref_col = tb->cursor_col;
    tb->last_edit_kind = EDIT_NONE;
    // centra il risultato se è fuori vista
    int vis = visible_lines(tb);
    if (tb->cursor_line < tb->scroll_y || tb->cursor_line >= tb->scroll_y + vis)
        tb->scroll_y = SV_MAX(0, tb->cursor_line - vis / 3);
    ensure_visible(tb);
    reset_blink(tb);
}

bool textbox_find_next(TextBox* tb, bool backwards, bool from_selection_start) {
    if (!tb || !tb->search[0]) return false;
    int a, b;
    sel_range(tb, &a, &b);
    int n = tb->text_len;
    if (!backwards) {
        int start = from_selection_start ? a : b;
        for (int k = 0; k <= n; k++) {
            int i = (start + k) % (n + 1);
            if (search_match_at(tb, i)) { select_match(tb, i); return true; }
        }
    } else {
        int start = a - 1;
        for (int k = 0; k <= n; k++) {
            int i = ((start - k) % (n + 1) + (n + 1)) % (n + 1);
            if (search_match_at(tb, i)) { select_match(tb, i); return true; }
        }
    }
    return false;
}

int textbox_search_count(TextBox* tb, int* current) {
    if (current) *current = 0;
    if (!tb || !tb->search[0]) return 0;
    int n = (int)strlen(tb->search), count = 0;
    int a, b;
    sel_range(tb, &a, &b);
    for (int i = 0; i + n <= tb->text_len; i++) {
        if (!search_match_at(tb, i)) continue;
        count++;
        if (current && i == a && b - a == n) *current = count;
        i += n - 1;
    }
    return count;
}

bool textbox_replace_current(TextBox* tb, const char* repl) {
    if (!tb || !tb->search[0]) return false;
    int a, b;
    sel_range(tb, &a, &b);
    int n = (int)strlen(tb->search);
    if (!(b - a == n && search_match_at(tb, a))) return textbox_find_next(tb, false, true);
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, a, b, repl ? repl : "", -1);
    textbox_find_next(tb, false, false);
    return true;
}

int textbox_replace_all(TextBox* tb, const char* repl) {
    if (!tb || !tb->search[0]) return 0;
    if (!repl) repl = "";
    int n = (int)strlen(tb->search), rl = (int)strlen(repl);
    int count = 0;
    for (int i = 0; i + n <= tb->text_len; i++)
        if (search_match_at(tb, i)) { count++; i += n - 1; }
    if (count == 0) return 0;

    int out_len = tb->text_len + count * (rl - n);
    char* out = malloc(out_len + 1);
    if (!out) return 0;
    int j = 0, cursor = tb->cursor_pos, new_cursor = -1;
    for (int i = 0; i < tb->text_len; ) {
        if (i == cursor && new_cursor < 0) new_cursor = j;
        if (i + n <= tb->text_len && search_match_at(tb, i)) {
            memcpy(out + j, repl, rl); j += rl; i += n;
        } else {
            out[j++] = tb->text[i++];
        }
    }
    if (new_cursor < 0) new_cursor = j;
    undo_push(tb, EDIT_OTHER);
    replace_range(tb, 0, tb->text_len, out, j);
    free(out);
    move_cursor(tb, new_cursor, false, false);
    return count;
}

char* textbox_word_or_selection(TextBox* tb) {
    if (!tb) return NULL;
    int a, b;
    if (has_sel(tb)) {
        sel_range(tb, &a, &b);
        if (line_of(tb, a) != line_of(tb, b) || b - a > 200) return NULL;
    } else {
        a = b = tb->cursor_pos;
        while (a > 0 && is_word_char(tb->text[a - 1])) a--;
        while (b < tb->text_len && is_word_char(tb->text[b])) b++;
        if (a == b) return NULL;
    }
    char* s = malloc(b - a + 1);
    if (!s) return NULL;
    memcpy(s, tb->text + a, b - a);
    s[b - a] = '\0';
    return s;
}

void textbox_goto_line(TextBox* tb, int line) {
    if (!tb) return;
    line = SV_CLAMP(line, 0, tb->num_lines - 1);
    move_cursor(tb, first_non_space(tb, line), false, false);
    int vis = visible_lines(tb);
    tb->scroll_y = SV_MAX(0, line - vis / 3);
    clamp_scroll(tb);
}

// ═════════════════════════════════════════════════════════════════
//  Autocompletamento: API e disegno del popup
// ═════════════════════════════════════════════════════════════════

void textbox_set_completions(TextBox* tb, const CompletionItem* items, int count) {
    if (!tb) return;
    free(tb->comp_items);
    tb->comp_items = NULL;
    tb->comp_count = 0;
    if (count <= 0 || !items) return;
    tb->comp_items = malloc(sizeof(CompletionItem) * count);
    if (!tb->comp_items) return;
    memcpy(tb->comp_items, items, sizeof(CompletionItem) * count);
    tb->comp_count = count;
}

bool textbox_popup_contains(TextBox* tb, int x, int y) {
    if (!tb || !tb->popup_open) return false;
    Rect r = tb->popup_rect;
    return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static Color kind_color(CompletionKind k) {
    switch (k) {
        case COMP_KEYWORD:  return TH_SYN_KEYWORD;
        case COMP_TYPE:     return TH_SYN_TYPE;
        case COMP_FUNC:     return TH_SYN_FUNC;
        case COMP_DATABASE: return TH_ICON_DB;
        case COMP_TABLE:    return TH_ICON_TABLE;
        case COMP_VIEW:     return TH_ICON_VIEW;
        default:            return TH_ICON_COLUMN;
    }
}

static const char* kind_letter(CompletionKind k) {
    switch (k) {
        case COMP_KEYWORD:  return "K";
        case COMP_TYPE:     return "T";
        case COMP_FUNC:     return "f";
        case COMP_DATABASE: return "D";
        case COMP_TABLE:    return "#";
        case COMP_VIEW:     return "V";
        default:            return "c";
    }
}

void textbox_draw_overlay(TextBox* tb, PlatformWindow* win, Rect area) {
    if (!tb) return;
    if (!tb->popup_open || tb->popup_n == 0 || tb->base.state != WIDGET_STATE_FOCUSED) {
        tb->popup_rect = (Rect){0, 0, 0, 0};
        return;
    }
    int fs = SV_MAX(11, tb->font_size - 1);
    int rows = SV_MIN(tb->popup_n, TEXTBOX_POPUP_ROWS);
    int name_w = 0, det_w = 0;
    for (int i = 0; i < tb->popup_n; i++) {
        name_w = SV_MAX(name_w, platform_measure_text_ex(win, tb->popup_items[i].name, -1, fs, FONT_MONO));
        det_w  = SV_MAX(det_w,  platform_measure_text_ex(win, tb->popup_items[i].detail, -1, TH_FONT_SMALL, FONT_UI));
    }
    int w = SV_CLAMP(34 + name_w + 24 + det_w + 14, 220, 520);
    int h = rows * POPUP_ROW_H + POPUP_PAD * 2;

    int line = line_of(tb, tb->popup_start);
    int col  = col_of(tb, tb->popup_start);
    Rect b = tb->base.bounds;
    int line_top = b.y + tb->pad_top + (line - tb->scroll_y) * tb->line_height;
    int x = text_x0(tb) - tb->scroll_x + col * tb->char_w - 30;
    int y = line_top + tb->line_height + 2;
    if (y + h > area.y + area.h - 4) y = line_top - h - 2;    // non c'è spazio sotto: sopra la riga
    x = SV_CLAMP(x, area.x + 4, area.x + area.w - w - 4);
    Rect r = {x, y, w, h};
    tb->popup_rect = r;

    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 3, r.w + 4, r.h + 3}, ui_alpha(TH_BG, 170), 9);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 7);
    platform_set_clip(win, r);

    int plen = tb->cursor_pos - tb->popup_start;
    for (int k = 0; k < rows; k++) {
        int i = tb->popup_scroll + k;
        if (i >= tb->popup_n) break;
        const CompletionItem* it = &tb->popup_items[i];
        Rect row = {r.x + POPUP_PAD, r.y + POPUP_PAD + k * POPUP_ROW_H, r.w - POPUP_PAD * 2, POPUP_ROW_H};
        if (i == tb->popup_sel) platform_fill_rect_rounded(win, row, TH_ACCENT_SOFT, 5);

        // badge del tipo
        Color kc = kind_color(it->kind);
        Rect badge = {row.x + 6, row.y + (POPUP_ROW_H - 16) / 2, 16, 16};
        platform_fill_rect_rounded(win, badge, ui_alpha(kc, 40), 4);
        const char* kl = kind_letter(it->kind);
        int kw = platform_measure_text_ex(win, kl, -1, TH_FONT_SMALL, FONT_MONO_BOLD);
        platform_draw_text_ex(win, kl, -1, (Point){badge.x + (16 - kw) / 2, ui_text_y(win, badge, TH_FONT_SMALL, FONT_MONO_BOLD)},
                              kc, TH_FONT_SMALL, FONT_MONO_BOLD);

        // nome: il prefisso digitato in evidenza
        int nx = row.x + 30, ny = ui_text_y(win, row, fs, FONT_MONO);
        int pl = SV_MIN(plen, (int)strlen(it->name));
        int pw = platform_draw_text_ex(win, it->name, pl, (Point){nx, ny}, TH_ACCENT_TEXT, fs, FONT_MONO_BOLD);
        platform_draw_text_ex(win, it->name + pl, -1, (Point){nx + pw, ny},
                              i == tb->popup_sel ? TH_TEXT : TH_TEXT_MUTED, fs, FONT_MONO);

        // dettaglio a destra (tipo colonna, tabella, "keyword"...)
        char det[140];
        if (it->kind == COMP_COLUMN && it->parent[0]) snprintf(det, sizeof(det), "%s \xc2\xb7 %s", it->detail, it->parent);
        else snprintf(det, sizeof(det), "%s", it->detail);
        int dw = platform_measure_text_ex(win, det, -1, TH_FONT_SMALL, FONT_UI);
        int dx = row.x + row.w - 8 - dw;
        if (dx > nx + name_w + 12 || dx > nx + platform_measure_text_ex(win, it->name, -1, fs, FONT_MONO) + 12)
            platform_draw_text_ex(win, det, -1, (Point){dx, ui_text_y(win, row, TH_FONT_SMALL, FONT_UI)},
                                  TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI);
    }

    // indicatore di scroll
    if (tb->popup_n > rows) {
        int track = r.h - 8;
        int th = SV_MAX(16, track * rows / tb->popup_n);
        int ty = r.y + 4 + (track - th) * tb->popup_scroll / SV_MAX(1, tb->popup_n - rows);
        platform_fill_rect_rounded(win, (Rect){r.x + r.w - 5, ty, 3, th}, TH_BORDER_STRONG, 2);
    }
    platform_reset_clip(win);
}
