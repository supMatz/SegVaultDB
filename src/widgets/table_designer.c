#include "table_designer.h"
#include "table_design.h"
#include "theme.h"
#include "line_edit.h"
#include "../../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TD_ROW_H   34
#define TD_HEAD_H  52
#define TD_FOOT_H  60

// colonne del modulo: nome, tipo, lunghezza, default, 5 caselle, elimina
enum { F_NAME, F_TYPE, F_SIZE, F_DEF, F_PK, F_NN, F_UQ, F_IX, F_AI, F_DEL, F_COUNT };
#define NCHECK 5
static const char* const FIELD_TITLE[F_COUNT] = { "NAME", "TYPE", "LENGTH", "DEFAULT", "PRIMARY", "NOT NULL", "UNIQUE", "INDEX", "AUTO INC", "" };

struct TableDesigner {
    PlatformWindow* win;
    bool        open;
    bool        is_new;
    char        db[64];
    DesignTable orig, cur;
    LineEdit*   name_le;
    LineEdit*   le[DESIGN_MAX_COLS][3];       // nome, lunghezza, default
    char*       sql;                          // ultimo SQL generato
    char        msg[400];                     // errore di validazione o dell'esecuzione
    bool        msg_exec;                     // msg viene dall'esecuzione (resta finché non si modifica)
    int         scroll;                       // prima colonna visibile
    int         type_menu;                    // colonna con il menu dei tipi aperto (-1 = nessuna)
    Rect        type_menu_rect;
    int         mx, my;
    // geometria (dal disegno)
    Rect        panel, rows_area, close_btn, add_btn, apply_btn, cancel_btn, editor_btn;
    int         fx[F_COUNT + 1];              // x delle colonne del modulo
    int         visible_rows;
};

static bool in_rect(Rect r, int x, int y) { return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static LineEdit* make_le(TableDesigner* td, const char* ph) {
    LineEdit* le = line_edit_create(ph);
    if (le) { le->win = td->win; le->base.visible = false; }
    return le;
}

TableDesigner* td_create(PlatformWindow* win) {
    TableDesigner* td = calloc(1, sizeof(TableDesigner));
    if (!td) return NULL;
    td->win = win;
    td->name_le = make_le(td, "table_name");
    td->type_menu = -1;
    return td;
}

static void free_rows(TableDesigner* td) {
    for (int i = 0; i < DESIGN_MAX_COLS; i++)
        for (int k = 0; k < 3; k++) if (td->le[i][k]) { widget_destroy((Widget*)td->le[i][k]); td->le[i][k] = NULL; }
}

void td_destroy(TableDesigner* td) {
    if (!td) return;
    free_rows(td);
    if (td->name_le) widget_destroy((Widget*)td->name_le);
    free(td->sql);
    free(td);
}

// campi di testo della riga i dal modello
static void row_widgets(TableDesigner* td, int i) {
    static const char* const PH[3] = { "column", "", "none" };
    for (int k = 0; k < 3; k++) if (!td->le[i][k]) td->le[i][k] = make_le(td, PH[k]);
    const DesignCol* c = &td->cur.cols[i];
    line_edit_set_text(td->le[i][0], c->name);
    line_edit_set_text(td->le[i][1], c->size);
    line_edit_set_text(td->le[i][2], c->def);
}

bool td_open(TableDesigner* td, const char* db, const char* table) {
    if (!td || !db || !db[0]) return false;
    free_rows(td);
    snprintf(td->db, sizeof(td->db), "%s", db);
    td->is_new = table == NULL;
    if (td->is_new) design_new(&td->cur);
    else if (!design_load(&td->cur, db, table)) return false;
    td->orig = td->cur;
    for (int i = 0; i < td->cur.ncols; i++) row_widgets(td, i);
    line_edit_set_text(td->name_le, td->cur.name);
    line_edit_focus(td->name_le, td->is_new);
    td->open = true;
    td->scroll = 0;
    td->type_menu = -1;
    td->msg[0] = '\0';
    td->msg_exec = false;
    return true;
}

bool td_is_open(const TableDesigner* td) { return td && td->open; }
void td_close(TableDesigner* td) { if (td) { td->open = false; td->type_menu = -1; } }
bool td_is_new(const TableDesigner* td) { return td->is_new; }
const char* td_db(const TableDesigner* td) { return td->db; }
const char* td_table(const TableDesigner* td) { return td->cur.name; }
const char* td_orig_table(const TableDesigner* td) { return td->orig.name; }

void td_set_error(TableDesigner* td, const char* msg) {
    snprintf(td->msg, sizeof(td->msg), "%s", msg ? msg : "");
    td->msg_exec = msg && msg[0];
}

// testi dei campi -> modello, poi SQL
static void sync(TableDesigner* td) {
    snprintf(td->cur.name, sizeof(td->cur.name), "%.63s", td->name_le->text);
    for (int i = 0; i < td->cur.ncols; i++) {
        DesignCol* c = &td->cur.cols[i];
        if (!td->le[i][0]) continue;
        snprintf(c->name, sizeof(c->name), "%.63s", td->le[i][0]->text);
        snprintf(c->size, sizeof(c->size), "%.15s", td->le[i][1]->text);
        snprintf(c->def, sizeof(c->def), "%.127s", td->le[i][2]->text);
    }
    free(td->sql);
    char err[400];
    td->sql = design_sql(td->is_new ? NULL : &td->orig, &td->cur, err, sizeof(err));
    if (!td->msg_exec) snprintf(td->msg, sizeof(td->msg), "%s", td->sql ? "" : err);
}

const char* td_sql(TableDesigner* td) { sync(td); return td->sql; }

static LineEdit* focused_le(TableDesigner* td) {
    if (line_edit_focused(td->name_le)) return td->name_le;
    for (int i = 0; i < td->cur.ncols; i++)
        for (int k = 0; k < 3; k++) if (td->le[i][k] && line_edit_focused(td->le[i][k])) return td->le[i][k];
    return NULL;
}

static void focus_le(TableDesigner* td, LineEdit* le) {
    LineEdit* f = focused_le(td);
    if (f == le) return;
    if (f) line_edit_focus(f, false);
    if (le) line_edit_focus(le, true);
}

// ── disegno ─────────────────────────────────────────────────────

static void button(PlatformWindow* win, Rect r, const char* label, bool primary, bool hover, bool enabled) {
    Color fill = !enabled ? TH_SURFACE_2 : primary ? (hover ? TH_ACCENT_HOVER : TH_ACCENT) : (hover ? TH_ELEVATED_HOVER : TH_ELEVATED);
    if (primary && enabled) platform_fill_rect_rounded(win, r, fill, 6);
    else ui_round_box(win, r, fill, TH_BORDER, 6);
    FontStyle fs = primary ? FONT_UI_BOLD : FONT_UI;
    int tw = platform_measure_text_ex(win, label, -1, TH_FONT_UI, fs);
    platform_draw_text_ex(win, label, -1, (Point){r.x + (r.w - tw) / 2, ui_text_y(win, r, TH_FONT_UI, fs)},
                          !enabled ? TH_TEXT_FAINT : primary ? TH_ON_ACCENT : TH_TEXT, TH_FONT_UI, fs);
}

static void checkbox(PlatformWindow* win, int cx, int cy, bool on, bool enabled, bool hover) {
    Rect b = { cx - 8, cy - 8, 16, 16 };
    Color bd = !enabled ? TH_BORDER : hover ? TH_ACCENT : TH_BORDER_STRONG;
    ui_round_box(win, b, on ? (enabled ? TH_ACCENT : TH_ELEVATED) : TH_SURFACE, on && enabled ? TH_ACCENT : bd, 4);
    if (on) ui_icon_check(win, cx, cy, 11, enabled ? TH_ON_ACCENT : TH_TEXT_DIM);
}

// righe di testo dell'SQL (a capo sui \n) nel riquadro r
static void draw_sql(PlatformWindow* win, Rect r, const char* sql) {
    int fs = TH_FONT_SMALL + 1, lh = platform_text_height(win, fs, FONT_MONO) + 3, y = r.y + 8;
    platform_set_clip(win, r);
    for (const char* p = sql; *p && y + lh <= r.y + r.h; ) {
        const char* e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        char line[512];
        snprintf(line, sizeof(line), "%.*s", n > 500 ? 500 : n, p);
        ui_text_clipped(win, line, r.x + 12, y, r.w - 24, TH_SYN_KEYWORD, fs, FONT_MONO);
        y += lh;
        p += n + (e ? 1 : 0);
    }
    platform_reset_clip(win);
}

void td_draw(TableDesigner* td, int W, int H) {
    if (!td || !td->open) return;
    PlatformWindow* win = td->win;
    sync(td);
    platform_fill_rect(win, (Rect){0, 0, W, H}, ui_alpha(TH_BG, 170));
    int pw = SV_MIN(1040, W - 40), ph = SV_MIN(700, H - 40);
    Rect P = { (W - pw) / 2, (H - ph) / 2, pw, ph };
    td->panel = P;
    ui_round_box(win, P, TH_SURFACE, TH_BORDER_STRONG, 10);

    // intestazione
    char title[160];
    if (td->is_new) snprintf(title, sizeof(title), "New table in %s", td->db);
    else snprintf(title, sizeof(title), "Structure of %s.%s", td->db, td->orig.name);
    platform_draw_text_ex(win, title, -1, (Point){P.x + 24, P.y + 18}, TH_TEXT, 16, FONT_UI_BOLD);
    td->close_btn = (Rect){ P.x + P.w - 44, P.y + 12, 30, 30 };
    if (in_rect(td->close_btn, td->mx, td->my)) platform_fill_rect_rounded(win, td->close_btn, TH_HOVER, 6);
    ui_icon_cross(win, td->close_btn.x + 15, td->close_btn.y + 15, 10, TH_TEXT_MUTED);

    // nome
    int x0 = P.x + 24, y = P.y + TD_HEAD_H + 6;
    platform_draw_text_ex(win, "TABLE NAME", -1, (Point){x0, y}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    td->name_le->base.bounds = (Rect){ x0, y + 18, SV_MIN(320, pw - 48), 30 };
    td->name_le->base.visible = true;
    widget_draw((Widget*)td->name_le, win);
    y += 18 + 30 + 18;

    // colonne del modulo
    int inner = pw - 48;
    int wfix[F_COUNT] = { 0, 130, 80, 0, 70, 70, 70, 70, 70, 34 };
    int rest = inner - (F_COUNT - 1) * 10;
    for (int f = 0; f < F_COUNT; f++) rest -= wfix[f];
    wfix[F_NAME] = SV_MAX(120, rest * 45 / 100);
    wfix[F_DEF] = SV_MAX(100, rest - wfix[F_NAME]);
    td->fx[0] = x0;
    for (int f = 0; f < F_COUNT; f++) td->fx[f + 1] = td->fx[f] + wfix[f] + 10;
    for (int f = 0; f < F_COUNT; f++) {
        int tw = platform_measure_text_ex(win, FIELD_TITLE[f], -1, TH_FONT_SMALL, FONT_UI_BOLD);
        int tx = f >= F_PK ? td->fx[f] + (wfix[f] - tw) / 2 : td->fx[f];
        platform_draw_text_ex(win, FIELD_TITLE[f], -1, (Point){tx, y}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    }
    y += 20;
    int list_bottom = P.y + P.h - TD_FOOT_H - 150;
    td->rows_area = (Rect){ P.x + 1, y, pw - 2, list_bottom - y };
    td->visible_rows = SV_MAX(1, td->rows_area.h / TD_ROW_H);
    td->scroll = SV_CLAMP(td->scroll, 0, SV_MAX(0, td->cur.ncols + 1 - td->visible_rows));
    int ntypes;
    const char* const* types = design_types(&ntypes);
    for (int i = 0; i < DESIGN_MAX_COLS; i++) for (int k = 0; k < 3; k++) if (td->le[i][k]) td->le[i][k]->base.visible = false;
    int row = 0;
    for (int i = td->scroll; i < td->cur.ncols && row < td->visible_rows; i++, row++) {
        DesignCol* c = &td->cur.cols[i];
        int ry = y + row * TD_ROW_H, cy = ry + TD_ROW_H / 2 - 2;
        bool existing_key = !td->is_new && c->orig >= 0 && (c->pk || c->autoinc);
        if (c->orig < 0 && !td->is_new) platform_fill_rect(win, (Rect){P.x + 8, ry + 2, 3, TD_ROW_H - 8}, TH_ACCENT);   // colonna nuova
        LineEdit* nm = td->le[i][0], *sz = td->le[i][1], *df = td->le[i][2];
        nm->base.bounds = (Rect){ td->fx[F_NAME], ry + 2, wfix[F_NAME], 28 };
        nm->base.visible = true;
        widget_draw((Widget*)nm, win);
        Rect tr = { td->fx[F_TYPE], ry + 2, wfix[F_TYPE], 28 };
        bool th = in_rect(tr, td->mx, td->my);
        ui_round_box(win, tr, th ? TH_ELEVATED_HOVER : TH_EDITOR_BG, td->type_menu == i ? TH_ACCENT : TH_BORDER, 6);
        platform_draw_text_ex(win, types[c->type], -1, (Point){tr.x + 10, ui_text_y(win, tr, TH_FONT_SMALL + 1, FONT_MONO)},
                              TH_SYN_TYPE, TH_FONT_SMALL + 1, FONT_MONO);
        ui_icon_chevron(win, tr.x + tr.w - 14, tr.y + 14, 8, CHEVRON_DOWN, TH_TEXT_DIM);
        if (design_type_sized(c->type)) {
            sz->base.bounds = (Rect){ td->fx[F_SIZE], ry + 2, wfix[F_SIZE], 28 };
            sz->base.visible = true;
            widget_draw((Widget*)sz, win);
        }
        if (!c->autoinc) {
            df->base.bounds = (Rect){ td->fx[F_DEF], ry + 2, wfix[F_DEF], 28 };
            df->base.visible = true;
            widget_draw((Widget*)df, win);
        } else platform_draw_text_ex(win, "generated", -1, (Point){td->fx[F_DEF] + 8, ui_text_y(win, tr, TH_FONT_SMALL, FONT_UI)},
                                     TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
        bool vals[NCHECK] = { c->pk, c->notnull || c->pk, c->unique || c->pk, c->index || c->pk, c->autoinc };
        for (int k = 0; k < NCHECK; k++) {
            int f = F_PK + k;
            bool enabled = !existing_key && !(k == 0 && !td->is_new) && !(k == 4 && !td->is_new) && !((k == 1 || k == 2 || k == 3) && c->pk);
            int cx = td->fx[f] + wfix[f] / 2;
            checkbox(win, cx, cy, vals[k], enabled, enabled && in_rect((Rect){cx - 14, ry, 28, TD_ROW_H}, td->mx, td->my));
        }
        Rect del = { td->fx[F_DEL] + 4, ry + 4, 26, 26 };
        if (!existing_key) {
            if (in_rect(del, td->mx, td->my)) platform_fill_rect_rounded(win, del, TH_ERROR_SOFT, 6);
            ui_icon_cross(win, del.x + 13, del.y + 13, 8, in_rect(del, td->mx, td->my) ? TH_ERROR : TH_TEXT_DIM);
        }
    }
    // + Add column
    int ay = y + row * TD_ROW_H + 4;
    const char* al = "+ Add column";
    int aw = platform_measure_text_ex(win, al, -1, TH_FONT_UI, FONT_UI) + 20;
    td->add_btn = (Rect){0, 0, 0, 0};
    if (td->cur.ncols < DESIGN_MAX_COLS && ay + 28 <= list_bottom) {
        td->add_btn = (Rect){ x0 - 6, ay, aw, 28 };
        if (in_rect(td->add_btn, td->mx, td->my)) platform_fill_rect_rounded(win, td->add_btn, TH_HOVER, 6);
        platform_draw_text_ex(win, al, -1, (Point){x0 + 4, ui_text_y(win, td->add_btn, TH_FONT_UI, FONT_UI)}, TH_ACCENT_TEXT, TH_FONT_UI, FONT_UI);
    }

    // SQL generato (o il motivo per cui non si può)
    Rect sr = { x0, list_bottom + 10, inner, 130 };
    platform_draw_text_ex(win, td->is_new ? "SQL" : "SQL TO APPLY", -1, (Point){sr.x, sr.y - 2}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    sr.y += 16; sr.h -= 16;
    ui_round_box(win, sr, TH_EDITOR_BG, TH_BORDER, 6);
    if (td->msg[0]) {
        bool soft = !td->msg_exec && !strcmp(td->msg, "No changes");
        ui_text_clipped(win, td->msg, sr.x + 12, sr.y + 10, sr.w - 24, soft ? TH_TEXT_DIM : TH_ERROR_TEXT, TH_FONT_UI, FONT_UI);
        if (td->sql && td->msg_exec) draw_sql(win, (Rect){sr.x, sr.y + 22, sr.w, sr.h - 22}, td->sql);
    } else if (td->sql) draw_sql(win, sr, td->sql);

    // pulsanti
    int by = P.y + P.h - TD_FOOT_H + 14;
    const char* ap = td->is_new ? "Create table" : "Apply changes";
    int apw = platform_measure_text_ex(win, ap, -1, TH_FONT_UI, FONT_UI_BOLD) + 32;
    td->apply_btn = (Rect){ P.x + P.w - 24 - apw, by, apw, 34 };
    td->cancel_btn = (Rect){ td->apply_btn.x - 10 - 96, by, 96, 34 };
    td->editor_btn = (Rect){ x0, by, platform_measure_text_ex(win, "Open in editor", -1, TH_FONT_UI, FONT_UI) + 28, 34 };
    button(win, td->apply_btn, ap, true, in_rect(td->apply_btn, td->mx, td->my), td->sql != NULL);
    button(win, td->cancel_btn, "Cancel", false, in_rect(td->cancel_btn, td->mx, td->my), true);
    button(win, td->editor_btn, "Open in editor", false, in_rect(td->editor_btn, td->mx, td->my), td->sql != NULL);

    // menu dei tipi
    td->type_menu_rect = (Rect){0, 0, 0, 0};
    if (td->type_menu >= td->scroll && td->type_menu < td->scroll + td->visible_rows) {
        int ry = y + (td->type_menu - td->scroll) * TD_ROW_H + 32;
        Rect m = { td->fx[F_TYPE], ry, wfix[F_TYPE], ntypes * 26 + 8 };
        if (m.y + m.h > H - 8) m.y = ry - 32 - m.h - 2;
        td->type_menu_rect = m;
        ui_round_box(win, m, TH_ELEVATED, TH_BORDER_STRONG, 8);
        for (int t = 0; t < ntypes; t++) {
            Rect it = { m.x + 4, m.y + 4 + t * 26, m.w - 8, 26 };
            bool cur = td->cur.cols[td->type_menu].type == t, hov = in_rect(it, td->mx, td->my);
            if (cur || hov) platform_fill_rect_rounded(win, it, cur ? TH_ACCENT_SOFT : TH_HOVER, 5);
            platform_draw_text_ex(win, types[t], -1, (Point){it.x + 8, ui_text_y(win, it, TH_FONT_SMALL + 1, FONT_MONO)},
                                  cur ? TH_ACCENT_TEXT : TH_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
        }
    }
}

// ── eventi ──────────────────────────────────────────────────────

static void remove_row(TableDesigner* td, int i) {
    for (int k = 0; k < 3; k++) if (td->le[i][k]) widget_destroy((Widget*)td->le[i][k]);
    memmove(&td->cur.cols[i], &td->cur.cols[i + 1], sizeof(DesignCol) * (size_t)(td->cur.ncols - i - 1));
    memmove(&td->le[i], &td->le[i + 1], sizeof(td->le[0]) * (size_t)(td->cur.ncols - i - 1));
    td->cur.ncols--;
    for (int k = 0; k < 3; k++) td->le[td->cur.ncols][k] = NULL;
}

// Tab: campo di testo successivo (nome tabella -> nome, lunghezza, default di ogni colonna)
static void focus_next(TableDesigner* td, int dir) {
    LineEdit* order[1 + DESIGN_MAX_COLS * 3];
    int n = 0;
    order[n++] = td->name_le;
    for (int i = 0; i < td->cur.ncols; i++) {
        order[n++] = td->le[i][0];
        if (design_type_sized(td->cur.cols[i].type)) order[n++] = td->le[i][1];
        if (!td->cur.cols[i].autoinc) order[n++] = td->le[i][2];
    }
    LineEdit* f = focused_le(td);
    int at = 0;
    for (int k = 0; k < n; k++) if (order[k] == f) at = k;
    int next = (at + dir + n) % n;
    focus_le(td, order[next]);
    line_edit_select_all(order[next]);
    for (int i = 0; i < td->cur.ncols; i++)               // la riga del campo deve essere visibile
        for (int k = 0; k < 3; k++) if (td->le[i][k] == order[next]) {
            if (i < td->scroll) td->scroll = i;
            if (i >= td->scroll + td->visible_rows) td->scroll = i - td->visible_rows + 1;
        }
}

TdAction td_event(TableDesigner* td, sEvent* evt) {
    if (!td || !td->open) return TD_NONE;
    LineEdit* f = focused_le(td);
    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            td->mx = evt->mouse_x; td->my = evt->mouse_y;
            if (f) widget_handle_event((Widget*)f, evt);
            return TD_REDRAW;
        case EVT_MOUSE_UP:
            if (f) widget_handle_event((Widget*)f, evt);
            return TD_REDRAW;
        case EVT_MOUSE_WHEEL:
            if (in_rect(td->rows_area, evt->mouse_x, evt->mouse_y)) td->scroll -= evt->wheel_delta;
            if (td->scroll < 0) td->scroll = 0;
            td->type_menu = -1;
            return TD_REDRAW;
        case EVT_CHAR:
            if (f) { widget_handle_event((Widget*)f, evt); td->msg_exec = false; }
            return TD_REDRAW;
        case EVT_KEY_DOWN:
            if (evt->key == KEY_ESCAPE) {
                if (td->type_menu >= 0) { td->type_menu = -1; return TD_REDRAW; }
                return TD_CLOSE;
            }
            if (evt->key == KEY_TAB) { focus_next(td, (evt->modifiers & SV_MOD_SHIFT) ? -1 : 1); return TD_REDRAW; }
            if (evt->key == KEY_ENTER && (evt->modifiers & SV_MOD_CTRL)) { sync(td); return td->sql ? TD_APPLY : TD_REDRAW; }
            if (f) { widget_handle_event((Widget*)f, evt); td->msg_exec = false; }
            return TD_REDRAW;
        case EVT_MOUSE_DOWN:
            break;
        default:
            return TD_NONE;
    }
    if (evt->mouse_button != SV_MOUSE_LEFT) return TD_REDRAW;
    int x = evt->mouse_x, y = evt->mouse_y;
    td->msg_exec = false;

    // menu dei tipi aperto: scelta o chiusura
    if (td->type_menu >= 0) {
        int col = td->type_menu;
        td->type_menu = -1;
        if (in_rect(td->type_menu_rect, x, y)) {
            int t = (y - td->type_menu_rect.y - 4) / 26, ntypes;
            design_types(&ntypes);
            if (t >= 0 && t < ntypes) {
                DesignCol* c = &td->cur.cols[col];
                c->type = t;
                if (design_type_sized(t) && !td->le[col][1]->text[0]) line_edit_set_text(td->le[col][1], "100");
                if (c->autoinc && t != 0 && t != 1) c->autoinc = false;      // AUTO_INCREMENT solo con INT / BIGINT
            }
            return TD_REDRAW;
        }
    }
    if (!in_rect(td->panel, x, y)) return TD_REDRAW;
    if (in_rect(td->close_btn, x, y) || in_rect(td->cancel_btn, x, y)) return TD_CLOSE;
    if (in_rect(td->apply_btn, x, y)) { sync(td); return td->sql ? TD_APPLY : TD_REDRAW; }
    if (in_rect(td->editor_btn, x, y)) { sync(td); return td->sql ? TD_TO_EDITOR : TD_REDRAW; }
    if (in_rect(td->add_btn, x, y)) {
        sync(td);
        design_add_column(&td->cur);
        row_widgets(td, td->cur.ncols - 1);
        focus_le(td, td->le[td->cur.ncols - 1][0]);
        if (td->cur.ncols - 1 >= td->scroll + td->visible_rows) td->scroll = td->cur.ncols - td->visible_rows;
        return TD_REDRAW;
    }
    // campi di testo
    LineEdit* hit_le = NULL;
    if (widget_contains_point((Widget*)td->name_le, x, y)) hit_le = td->name_le;
    for (int i = 0; i < td->cur.ncols && !hit_le; i++)
        for (int k = 0; k < 3; k++)
            if (td->le[i][k] && td->le[i][k]->base.visible && widget_contains_point((Widget*)td->le[i][k], x, y)) hit_le = td->le[i][k];
    if (hit_le) {
        focus_le(td, hit_le);
        widget_handle_event((Widget*)hit_le, evt);
        return TD_REDRAW;
    }
    focus_le(td, NULL);
    // righe: tipo, caselle, elimina
    if (!in_rect(td->rows_area, x, y)) return TD_REDRAW;
    int i = td->scroll + (y - td->rows_area.y) / TD_ROW_H;
    if (i < 0 || i >= td->cur.ncols) return TD_REDRAW;
    sync(td);
    DesignCol* c = &td->cur.cols[i];
    bool existing_key = !td->is_new && c->orig >= 0 && (c->pk || c->autoinc);
    if (x >= td->fx[F_TYPE] && x < td->fx[F_TYPE + 1] - 10) { td->type_menu = i; return TD_REDRAW; }
    for (int k = 0; k < NCHECK; k++) {
        int fx = td->fx[F_PK + k], fw = td->fx[F_PK + k + 1] - 10 - fx;
        if (x < fx || x >= fx + fw || existing_key) continue;
        switch (k) {
            case 0: if (td->is_new) { c->pk = !c->pk; if (!c->pk) c->autoinc = false; } break;
            case 1: if (!c->pk) c->notnull = !c->notnull; break;
            case 2: if (!c->pk) c->unique = !c->unique; break;
            case 3: if (!c->pk) c->index = !c->index; break;
            case 4:
                if (td->is_new) {
                    c->autoinc = !c->autoinc;
                    if (c->autoinc) { c->pk = true; if (c->type != 0 && c->type != 1) c->type = 0; line_edit_set_text(td->le[i][2], ""); }
                }
                break;
        }
        return TD_REDRAW;
    }
    if (x >= td->fx[F_DEL] && x < td->fx[F_DEL] + 34 && !existing_key) remove_row(td, i);
    return TD_REDRAW;
}

bool td_tick(TableDesigner* td, uint64_t now_ms) {
    if (!td || !td->open) return false;
    LineEdit* f = focused_le(td);
    return f && line_edit_tick(f, now_ms);
}
