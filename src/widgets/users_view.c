#include "users_view.h"
#include "theme.h"
#include "line_edit.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define UV_MAX_USERS 128
#define UV_MAX_DBS   64
#define UV_MAX_TG    64
#define NPRIV        7
#define ROW_H        34

static const char* const PRIV[NPRIV] = { "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER" };

enum { MODE_VIEW, MODE_NEW, MODE_PASSWORD, MODE_DELETE };

struct UsersView {
    PlatformWindow* win;
    bool        open;
    DbUserInfo  users[UV_MAX_USERS];
    int         nusers, sel;
    char        dbs[UV_MAX_DBS + 1][64];          // [0] = "*" (tutti i database)
    int         ndbs;
    unsigned    grant[UV_MAX_DBS + 1];            // permessi dell'utente scelto su db.*
    char        tg_db[UV_MAX_TG][64], tg_table[UV_MAX_TG][64];
    unsigned    tg_privs[UV_MAX_TG];
    int         ntg;
    int         mode;
    LineEdit*   le_name, *le_pw, *le_pw2;
    char        sql[1200];
    char        last_sql[1200];
    char        msg[400];
    bool        msg_err;
    int         scroll;
    int         mx, my;
    // geometria (dal disegno)
    Rect        panel, close_btn, list_rect, new_btn, pw_btn, del_btn, admin_box, ok_btn, cancel_btn, matrix;
    int         col_x[NPRIV + 1], col_w;
    int         visible_rows;
    Rect        tg_x[UV_MAX_TG];
};

static bool in_rect(Rect r, int x, int y) { return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static LineEdit* mk(UsersView* v, const char* ph, bool secret) {
    LineEdit* le = line_edit_create(ph);
    if (le) { le->win = v->win; le->secret = secret; le->base.visible = false; }
    return le;
}

UsersView* uv_create(PlatformWindow* win) {
    UsersView* v = calloc(1, sizeof(UsersView));
    if (!v) return NULL;
    v->win = win;
    v->le_name = mk(v, "user_name", false);
    v->le_pw = mk(v, "password (at least 4 characters)", true);
    v->le_pw2 = mk(v, "repeat the password", true);
    return v;
}

void uv_destroy(UsersView* v) {
    if (!v) return;
    if (v->le_name) widget_destroy((Widget*)v->le_name);
    if (v->le_pw) widget_destroy((Widget*)v->le_pw);
    if (v->le_pw2) widget_destroy((Widget*)v->le_pw2);
    free(v);
}

static const char* sel_name(const UsersView* v) { return v->sel >= 0 && v->sel < v->nusers ? v->users[v->sel].name : NULL; }

static void load(UsersView* v, const char* keep) {
    v->nusers = db_list_users(v->users, UV_MAX_USERS);
    v->sel = v->nusers ? 0 : -1;
    for (int i = 0; keep && i < v->nusers; i++) if (!strcasecmp(v->users[i].name, keep)) v->sel = i;
    v->ndbs = 1;
    snprintf(v->dbs[0], 64, "*");
    NameList* nl = db_list_databases();
    for (int i = 0; nl && i < nl->count && v->ndbs <= UV_MAX_DBS; i++) snprintf(v->dbs[v->ndbs++], 64, "%.63s", nl->names[i]);
    if (nl) db_namelist_free(nl);
    v->ntg = 0;
    const char* u = sel_name(v);
    for (int d = 0; d < v->ndbs; d++) v->grant[d] = u ? db_user_grant(u, v->dbs[d], "*") : 0;
    if (u) {
        char dbs[UV_MAX_TG][64], tabs[UV_MAX_TG][64];
        unsigned pr[UV_MAX_TG];
        int n = db_user_table_grants(u, dbs, tabs, pr, UV_MAX_TG);
        for (int i = 0; i < n; i++) {
            if (!strcmp(tabs[i], "*")) continue;
            snprintf(v->tg_db[v->ntg], 64, "%.63s", dbs[i]);
            snprintf(v->tg_table[v->ntg], 64, "%.63s", tabs[i]);
            v->tg_privs[v->ntg++] = pr[i];
        }
    }
}

static void reset_fields(UsersView* v) {
    line_edit_set_text(v->le_name, "");
    line_edit_set_text(v->le_pw, "");
    line_edit_set_text(v->le_pw2, "");
    line_edit_focus(v->le_name, false);
    line_edit_focus(v->le_pw, false);
    line_edit_focus(v->le_pw2, false);
}

static void set_mode(UsersView* v, int mode) {
    v->mode = mode;
    reset_fields(v);
    if (mode == MODE_NEW) line_edit_focus(v->le_name, true);
    if (mode == MODE_PASSWORD) line_edit_focus(v->le_pw, true);
}

void uv_open(UsersView* v) {
    if (!v) return;
    load(v, NULL);
    v->open = true;
    v->msg[0] = v->last_sql[0] = '\0';
    v->scroll = 0;
    set_mode(v, v->nusers ? MODE_VIEW : MODE_NEW);
}

bool uv_is_open(const UsersView* v) { return v && v->open; }
void uv_close(UsersView* v) { if (v) { v->open = false; reset_fields(v); } }
const char* uv_sql(const UsersView* v) { return v->sql; }

void uv_exec_done(UsersView* v, bool ok, const char* message) {
    char keep[64] = "";
    if (sel_name(v)) snprintf(keep, sizeof(keep), "%s", sel_name(v));
    if (ok && v->mode == MODE_NEW) snprintf(keep, sizeof(keep), "%.63s", v->le_name->text);
    // l'istruzione eseguita si mostra senza la password, che non resta in memoria
    snprintf(v->last_sql, sizeof(v->last_sql), "%s", v->sql);
    char* idb = strstr(v->last_sql, "IDENTIFIED BY ");
    if (idb) snprintf(idb + 14, sizeof(v->last_sql) - (size_t)(idb + 14 - v->last_sql), "'****'");
    memset(v->sql, 0, sizeof(v->sql));
    if (ok) {
        snprintf(v->msg, sizeof(v->msg), "%s", message && message[0] ? message : "Saved");
        v->msg_err = false;
        load(v, keep[0] ? keep : NULL);
        if (v->mode != MODE_VIEW) set_mode(v, MODE_VIEW);
    } else {
        snprintf(v->msg, sizeof(v->msg), "%s", message && message[0] ? message : "The change was refused");
        v->msg_err = true;
        load(v, keep[0] ? keep : NULL);
    }
}

// ── disegno ─────────────────────────────────────────────────────

static void button(UsersView* v, Rect r, const char* label, int kind) {   // 0 = normale, 1 = primario, 2 = pericolo
    PlatformWindow* win = v->win;
    bool hov = in_rect(r, v->mx, v->my);
    if (kind == 1) platform_fill_rect_rounded(win, r, hov ? TH_ACCENT_HOVER : TH_ACCENT, 6);
    else if (kind == 2) platform_fill_rect_rounded(win, r, hov ? ui_mix(TH_ERROR, TH_TEXT, 0.15f) : TH_ERROR, 6);
    else ui_round_box(win, r, hov ? TH_ELEVATED_HOVER : TH_ELEVATED, TH_BORDER, 6);
    FontStyle fs = kind ? FONT_UI_BOLD : FONT_UI;
    int w = platform_measure_text_ex(win, label, -1, TH_FONT_UI, fs);
    platform_draw_text_ex(win, label, -1, (Point){r.x + (r.w - w) / 2, ui_text_y(win, r, TH_FONT_UI, fs)},
                          kind ? TH_ON_ACCENT : TH_TEXT, TH_FONT_UI, fs);
}

static int bw(UsersView* v, const char* label) { return platform_measure_text_ex(v->win, label, -1, TH_FONT_UI, FONT_UI_BOLD) + 28; }

static void checkbox(PlatformWindow* win, int cx, int cy, bool on, bool enabled, bool hover) {
    Rect b = { cx - 8, cy - 8, 16, 16 };
    Color bd = !enabled ? TH_BORDER : hover ? TH_ACCENT : TH_BORDER_STRONG;
    ui_round_box(win, b, on ? (enabled ? TH_ACCENT : TH_ELEVATED) : TH_SURFACE, on && enabled ? TH_ACCENT : bd, 4);
    if (on) ui_icon_check(win, cx, cy, 11, enabled ? TH_ON_ACCENT : TH_TEXT_DIM);
}

static void field(UsersView* v, LineEdit* le, const char* label, int x, int y, int w) {
    platform_draw_text_ex(v->win, label, -1, (Point){x, y}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    le->base.bounds = (Rect){ x, y + 18, w, 32 };
    le->base.visible = true;
    widget_draw((Widget*)le, v->win);
}

static void privs_text(unsigned p, char* out, size_t n) {
    if ((p & DB_PRIV_ALL) == DB_PRIV_ALL) { snprintf(out, n, "ALL"); return; }
    size_t l = 0;
    out[0] = '\0';
    for (int i = 0; i < NPRIV; i++)
        if (p & (1u << i)) l += (size_t)snprintf(out + l, l < n ? n - l : 0, "%s%s", l ? ", " : "", PRIV[i]);
}

void uv_draw(UsersView* v, int W, int H) {
    if (!v || !v->open) return;
    PlatformWindow* win = v->win;
    platform_fill_rect(win, (Rect){0, 0, W, H}, ui_alpha(TH_BG, 170));
    int pw = SV_MIN(1000, W - 40), ph = SV_MIN(660, H - 40);
    Rect P = { (W - pw) / 2, (H - ph) / 2, pw, ph };
    v->panel = P;
    ui_round_box(win, P, TH_SURFACE, TH_BORDER_STRONG, 10);
    platform_draw_text_ex(win, "Users & permissions", -1, (Point){P.x + 24, P.y + 18}, TH_TEXT, 16, FONT_UI_BOLD);
    ui_text_clipped(win, "Who can log in to the API server and what each user can do. The desktop app always works as the administrator.",
                    P.x + 24, P.y + 44, pw - 80, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
    v->close_btn = (Rect){ P.x + P.w - 44, P.y + 12, 30, 30 };
    if (in_rect(v->close_btn, v->mx, v->my)) platform_fill_rect_rounded(win, v->close_btn, TH_HOVER, 6);
    ui_icon_cross(win, v->close_btn.x + 15, v->close_btn.y + 15, 10, TH_TEXT_MUTED);
    for (int k = 0; k < 3; k++) { LineEdit* le = k == 0 ? v->le_name : k == 1 ? v->le_pw : v->le_pw2; le->base.visible = false; }

    // ── elenco degli utenti ──
    int top = P.y + 76, foot = P.y + P.h - 56;
    Rect L = { P.x + 16, top, 240, foot - top - 50 };
    v->list_rect = L;
    ui_round_box(win, L, TH_EDITOR_BG, TH_BORDER, 8);
    for (int i = 0; i < v->nusers && 8 + (i + 1) * 36 <= L.h; i++) {
        Rect r = { L.x + 4, L.y + 4 + i * 36, L.w - 8, 34 };
        bool sel = i == v->sel && v->mode != MODE_NEW, hov = in_rect(r, v->mx, v->my);
        if (sel || hov) platform_fill_rect_rounded(win, r, sel ? TH_ACCENT_SOFT : TH_HOVER, 6);
        ui_circle(win, r.x + 16, r.y + 17, 9, v->users[i].admin ? TH_WARNING : TH_ICON_DB);
        char ini[2] = { (char)toupper((unsigned char)v->users[i].name[0]), 0 };
        int iw = platform_measure_text_ex(win, ini, -1, TH_FONT_SMALL, FONT_UI_BOLD);
        platform_draw_text_ex(win, ini, -1, (Point){r.x + 16 - iw / 2, ui_text_y(win, (Rect){0, r.y + 8, 0, 18}, TH_FONT_SMALL, FONT_UI_BOLD)},
                              TH_ON_ACCENT, TH_FONT_SMALL, FONT_UI_BOLD);
        int tw = ui_text_clipped(win, v->users[i].name, r.x + 34, ui_text_y(win, r, TH_FONT_UI, FONT_UI), r.w - 100,
                                 sel ? TH_ACCENT_TEXT : TH_TEXT, TH_FONT_UI, sel ? FONT_UI_BOLD : FONT_UI);
        if (v->users[i].admin)
            platform_draw_text_ex(win, "admin", -1, (Point){r.x + 34 + tw + 8, ui_text_y(win, r, TH_FONT_SMALL, FONT_UI_BOLD)},
                                  TH_WARNING, TH_FONT_SMALL, FONT_UI_BOLD);
    }
    if (!v->nusers)
        ui_text_clipped(win, "No users yet", L.x + 14, L.y + 14, L.w - 28, TH_TEXT_FAINT, TH_FONT_UI, FONT_UI);
    v->new_btn = (Rect){ L.x, L.y + L.h + 10, L.w, 34 };
    button(v, v->new_btn, "+ New user", v->mode == MODE_NEW ? 0 : 1);

    // ── dettaglio ──
    int x = L.x + L.w + 24, w = P.x + P.w - 24 - x, y = top;
    v->pw_btn = v->del_btn = v->admin_box = v->ok_btn = v->cancel_btn = v->matrix = (Rect){0, 0, 0, 0};
    v->visible_rows = 0;
    for (int i = 0; i < UV_MAX_TG; i++) v->tg_x[i] = (Rect){0, 0, 0, 0};
    const char* u = sel_name(v);
    if (v->mode == MODE_NEW) {
        platform_draw_text_ex(win, "New user", -1, (Point){x, y}, TH_TEXT, 15, FONT_UI_BOLD);
        y += 34;
        int fw = SV_MIN(360, w);
        field(v, v->le_name, "USER NAME", x, y, fw); y += 62;
        field(v, v->le_pw, "PASSWORD", x, y, fw); y += 62;
        field(v, v->le_pw2, "REPEAT THE PASSWORD", x, y, fw); y += 66;
        v->ok_btn = (Rect){ x, y, bw(v, "Create user"), 34 };
        button(v, v->ok_btn, "Create user", 1);
        if (v->nusers) { v->cancel_btn = (Rect){ v->ok_btn.x + v->ok_btn.w + 10, y, 96, 34 }; button(v, v->cancel_btn, "Cancel", 0); }
        y += 50;
        ui_text_clipped(win, "A new user can do nothing until you give it permissions.", x, y, w, TH_TEXT_FAINT, TH_FONT_SMALL + 1, FONT_UI);
    } else if (u) {
        platform_draw_text_ex(win, u, -1, (Point){x, y + 4}, TH_TEXT, 18, FONT_UI_BOLD);
        int rx = x + w;
        v->del_btn = (Rect){ rx - bw(v, "Delete user"), y, bw(v, "Delete user"), 32 };
        v->pw_btn = (Rect){ v->del_btn.x - 10 - bw(v, "Change password"), y, bw(v, "Change password"), 32 };
        button(v, v->pw_btn, "Change password", 0);
        button(v, v->del_btn, "Delete user", 0);
        y += 46;
        if (v->mode == MODE_PASSWORD) {
            int fw = SV_MIN(300, (w - 20) / 2);
            field(v, v->le_pw, "NEW PASSWORD", x, y, fw);
            field(v, v->le_pw2, "REPEAT", x + fw + 12, y, fw);
            v->ok_btn = (Rect){ x, y + 60, 90, 32 };
            v->cancel_btn = (Rect){ x + 100, y + 60, 90, 32 };
            button(v, v->ok_btn, "Save", 1);
            button(v, v->cancel_btn, "Cancel", 0);
            y += 106;
        } else if (v->mode == MODE_DELETE) {
            Rect box = { x, y, w, 52 };
            ui_round_box(win, box, TH_ERROR_SOFT, TH_ERROR, 8);
            char t[160];
            snprintf(t, sizeof(t), "Delete %.60s? Apps logging in as this user stop working.", u);
            ui_text_clipped(win, t, x + 14, ui_text_y(win, box, TH_FONT_UI, FONT_UI), w - 230, TH_ERROR_TEXT, TH_FONT_UI, FONT_UI);
            v->ok_btn = (Rect){ x + w - 10 - 90, y + 10, 90, 32 };
            v->cancel_btn = (Rect){ v->ok_btn.x - 10 - 90, y + 10, 90, 32 };
            button(v, v->ok_btn, "Delete", 2);
            button(v, v->cancel_btn, "Cancel", 0);
            y += 66;
        }
        bool admin = v->users[v->sel].admin;
        v->admin_box = (Rect){ x - 4, y, w + 4, 30 };
        bool ah = in_rect(v->admin_box, v->mx, v->my);
        checkbox(win, x + 8, y + 15, admin, true, ah);
        platform_draw_text_ex(win, "Administrator", -1, (Point){x + 26, ui_text_y(win, (Rect){0, y, 0, 30}, TH_FONT_UI, FONT_UI_BOLD)}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
        ui_text_clipped(win, "every privilege on every database, manages users", x + 132, ui_text_y(win, (Rect){0, y, 0, 30}, TH_FONT_SMALL + 1, FONT_UI),
                        w - 140, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
        y += 42;

        // matrice database x privilegio
        int name_w = SV_MAX(140, w - (NPRIV + 1) * 70);
        v->col_w = SV_MIN(70, (w - name_w) / (NPRIV + 1));
        for (int c = 0; c <= NPRIV; c++) v->col_x[c] = x + name_w + c * v->col_w;
        platform_draw_text_ex(win, "DATABASE", -1, (Point){x, y}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
        for (int c = 0; c <= NPRIV; c++) {
            const char* t = c < NPRIV ? PRIV[c] : "ALL";
            int tw = platform_measure_text_ex(win, t, -1, TH_FONT_SMALL, FONT_UI_BOLD);
            platform_draw_text_ex(win, t, -1, (Point){v->col_x[c] + (v->col_w - tw) / 2, y}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
        }
        y += 20;
        int tg_h = v->ntg ? 30 + SV_MIN(v->ntg, 4) * 26 : 0;
        Rect M = { x - 8, y, w + 8, foot - y - tg_h - 8 };
        v->matrix = M;
        v->visible_rows = SV_MAX(1, M.h / ROW_H);
        v->scroll = SV_CLAMP(v->scroll, 0, SV_MAX(0, v->ndbs - v->visible_rows));
        platform_set_clip(win, M);
        for (int r = 0; r + v->scroll < v->ndbs && r < v->visible_rows; r++) {
            int d = r + v->scroll, ry = y + r * ROW_H;
            if (r % 2) platform_fill_rect(win, (Rect){M.x, ry, M.w, ROW_H}, TH_ROW_ODD);
            const char* label = d == 0 ? "All databases  (*.*)" : v->dbs[d];
            ui_text_clipped(win, label, x, ui_text_y(win, (Rect){0, ry, 0, ROW_H}, TH_FONT_UI, d == 0 ? FONT_UI_BOLD : FONT_UI),
                            name_w - 10, TH_TEXT, TH_FONT_UI, d == 0 ? FONT_UI_BOLD : FONT_UI);
            unsigned own = v->grant[d], inh = admin ? DB_PRIV_ALL : d ? v->grant[0] : 0;
            for (int c = 0; c <= NPRIV; c++) {
                unsigned bit = c < NPRIV ? 1u << c : DB_PRIV_ALL;
                bool on_own = (own & bit) == bit, on_inh = (inh & bit) == bit;
                int cx = v->col_x[c] + v->col_w / 2, cy = ry + ROW_H / 2;
                bool hov = !admin && in_rect((Rect){cx - 14, ry, 28, ROW_H}, v->mx, v->my);
                checkbox(win, cx, cy, on_own || on_inh, !admin && !(on_inh && !on_own), hov);
            }
        }
        platform_reset_clip(win);
        if (v->ndbs > v->visible_rows) {
            char more[64];
            snprintf(more, sizeof(more), "%d of %d databases \xc2\xb7 scroll", v->visible_rows, v->ndbs);
            platform_draw_text_ex(win, more, -1, (Point){x, M.y + M.h - 14}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
        }
        // permessi sulle singole tabelle (dati con GRANT ... ON db.tabella)
        if (v->ntg) {
            int ty = foot - tg_h;
            platform_draw_text_ex(win, "TABLE PERMISSIONS", -1, (Point){x, ty}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
            ty += 22;
            for (int i = 0; i < v->ntg && i < 4; i++) {
                char p[120], line[300];
                privs_text(v->tg_privs[i], p, sizeof(p));
                snprintf(line, sizeof(line), "%.60s.%.60s   %s", v->tg_db[i], v->tg_table[i], p);
                ui_text_clipped(win, line, x, ty + 4, w - 40, TH_TEXT_MUTED, TH_FONT_SMALL + 1, FONT_MONO);
                v->tg_x[i] = (Rect){ x + w - 26, ty, 24, 22 };
                bool hov = in_rect(v->tg_x[i], v->mx, v->my);
                if (hov) platform_fill_rect_rounded(win, v->tg_x[i], TH_ERROR_SOFT, 5);
                ui_icon_cross(win, v->tg_x[i].x + 12, v->tg_x[i].y + 11, 8, hov ? TH_ERROR : TH_TEXT_DIM);
                ty += 26;
            }
        }
    } else {
        ui_text_clipped(win, "Create the first user with + New user.", x, y + 6, w, TH_TEXT_DIM, TH_FONT_UI, FONT_UI);
    }

    // ── esito e SQL eseguito ──
    platform_fill_rect(win, (Rect){P.x + 1, foot, P.w - 2, 1}, TH_BORDER);
    int fy = foot + 10;
    if (v->msg[0])
        ui_text_clipped(win, v->msg, P.x + 24, fy, pw - 48, v->msg_err ? TH_ERROR_TEXT : TH_SUCCESS, TH_FONT_UI, FONT_UI);
    else
        ui_text_clipped(win, "Every change is an SQL statement: CREATE USER, GRANT, REVOKE, ALTER USER, DROP USER.", P.x + 24, fy, pw - 48,
                        TH_TEXT_FAINT, TH_FONT_SMALL + 1, FONT_UI);
    if (v->last_sql[0]) {
        char one[300];
        snprintf(one, sizeof(one), "%.290s", v->last_sql);
        for (char* p = one; *p; p++) if (*p == '\n') *p = ' ';
        ui_text_clipped(win, one, P.x + 24, fy + 22, pw - 48, TH_SYN_KEYWORD, TH_FONT_SMALL + 1, FONT_MONO);
    }
}

// ── eventi ──────────────────────────────────────────────────────

static LineEdit* focused(UsersView* v) {
    LineEdit* all[3] = { v->le_name, v->le_pw, v->le_pw2 };
    for (int i = 0; i < 3; i++) if (all[i]->base.visible && line_edit_focused(all[i])) return all[i];
    return NULL;
}

static void focus(UsersView* v, LineEdit* le) {
    LineEdit* all[3] = { v->le_name, v->le_pw, v->le_pw2 };
    for (int i = 0; i < 3; i++) line_edit_focus(all[i], all[i] == le);
}

static UvAction error(UsersView* v, const char* m) {
    snprintf(v->msg, sizeof(v->msg), "%s", m);
    v->msg_err = true;
    return UV_REDRAW;
}

static bool valid_name(const char* s) {
    if (!s[0] || strlen(s) > 63) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') return false;
    return true;
}

// password tra apici per SQL: apici e backslash raddoppiati
static void quote_pw(const char* pw, char* out, size_t n) {
    size_t k = 0;
    out[k++] = '\'';
    for (; *pw && k + 3 < n; pw++) {
        if (*pw == '\'' || *pw == '\\') out[k++] = *pw;
        out[k++] = *pw;
    }
    out[k++] = '\'';
    out[k] = '\0';
}

static UvAction submit(UsersView* v) {
    if (v->mode == MODE_NEW || v->mode == MODE_PASSWORD) {
        const char* name = v->mode == MODE_NEW ? v->le_name->text : sel_name(v);
        if (!name || !valid_name(name)) return error(v, "User name: letters, digits, _ . - (up to 63)");
        if (v->mode == MODE_NEW)
            for (int i = 0; i < v->nusers; i++) if (!strcasecmp(v->users[i].name, name)) return error(v, "A user with this name already exists");
        if (strlen(v->le_pw->text) < 4) return error(v, "The password needs at least 4 characters");
        if (strcmp(v->le_pw->text, v->le_pw2->text)) return error(v, "The two passwords are different");
        char q[1100];
        quote_pw(v->le_pw->text, q, sizeof(q));
        snprintf(v->sql, sizeof(v->sql), "%s USER %s IDENTIFIED BY %.1100s", v->mode == MODE_NEW ? "CREATE" : "ALTER", name, q);
        return UV_EXEC;
    }
    if (v->mode == MODE_DELETE && sel_name(v)) {
        snprintf(v->sql, sizeof(v->sql), "DROP USER %s", sel_name(v));
        return UV_EXEC;
    }
    return UV_NONE;
}

UvAction uv_event(UsersView* v, sEvent* evt) {
    if (!v || !v->open) return UV_NONE;
    if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_UP) { v->mx = evt->mouse_x; v->my = evt->mouse_y; }
    LineEdit* f = focused(v);
    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            if (f) widget_handle_event((Widget*)f, evt);
            platform_set_cursor(v->win, CURSOR_ARROW);
            return UV_REDRAW;
        case EVT_MOUSE_UP:
            if (f) widget_handle_event((Widget*)f, evt);
            return UV_REDRAW;
        case EVT_MOUSE_WHEEL:
            if (in_rect(v->matrix, evt->mouse_x, evt->mouse_y)) v->scroll -= evt->wheel_delta;
            if (v->scroll < 0) v->scroll = 0;
            return UV_REDRAW;
        case EVT_CHAR:
            if (f) widget_handle_event((Widget*)f, evt);
            return UV_REDRAW;
        case EVT_KEY_DOWN:
            if (evt->key == KEY_ESCAPE) {
                if (v->mode != MODE_VIEW && v->nusers) { set_mode(v, MODE_VIEW); return UV_REDRAW; }
                return UV_CLOSE;
            }
            if (evt->key == KEY_ENTER) return submit(v);
            if (evt->key == KEY_TAB && f) {
                LineEdit* order[3] = { v->le_name, v->le_pw, v->le_pw2 };
                int n = 3, at = 0;
                for (int i = 0; i < n; i++) if (order[i] == f) at = i;
                for (int k = 1; k <= n; k++) {
                    LineEdit* nx = order[(at + ((evt->modifiers & SV_MOD_SHIFT) ? n - k : k)) % n];
                    if (nx->base.visible) { focus(v, nx); line_edit_select_all(nx); break; }
                }
                return UV_REDRAW;
            }
            if (evt->key == KEY_UP || evt->key == KEY_DOWN) {
                if (!f && v->nusers && v->mode == MODE_VIEW) {
                    int s = SV_CLAMP(v->sel + (evt->key == KEY_DOWN ? 1 : -1), 0, v->nusers - 1);
                    if (s != v->sel) { char k[64]; snprintf(k, sizeof(k), "%s", v->users[s].name); load(v, k); }
                }
                return UV_REDRAW;
            }
            if (f) widget_handle_event((Widget*)f, evt);
            return UV_REDRAW;
        case EVT_MOUSE_DOWN: break;
        default: return UV_NONE;
    }
    if (evt->mouse_button != SV_MOUSE_LEFT) return UV_REDRAW;
    int x = v->mx, y = v->my;
    if (in_rect(v->close_btn, x, y)) return UV_CLOSE;
    if (!in_rect(v->panel, x, y)) return UV_CLOSE;
    // campi di testo
    LineEdit* all[3] = { v->le_name, v->le_pw, v->le_pw2 };
    for (int i = 0; i < 3; i++)
        if (all[i]->base.visible && widget_contains_point((Widget*)all[i], x, y)) {
            focus(v, all[i]);
            widget_handle_event((Widget*)all[i], evt);
            return UV_REDRAW;
        }
    focus(v, NULL);
    if (in_rect(v->new_btn, x, y)) { set_mode(v, MODE_NEW); v->msg[0] = '\0'; return UV_REDRAW; }
    if (in_rect(v->ok_btn, x, y)) return submit(v);
    if (in_rect(v->cancel_btn, x, y)) { set_mode(v, MODE_VIEW); return UV_REDRAW; }
    if (in_rect(v->pw_btn, x, y)) { set_mode(v, MODE_PASSWORD); return UV_REDRAW; }
    if (in_rect(v->del_btn, x, y)) { set_mode(v, MODE_DELETE); return UV_REDRAW; }
    // elenco
    if (in_rect(v->list_rect, x, y)) {
        int i = (y - v->list_rect.y - 4) / 36;
        if (i >= 0 && i < v->nusers) {
            char k[64];
            snprintf(k, sizeof(k), "%s", v->users[i].name);
            load(v, k);
            set_mode(v, MODE_VIEW);
            v->msg[0] = '\0';
        }
        return UV_REDRAW;
    }
    const char* u = sel_name(v);
    if (!u || v->mode == MODE_NEW) return UV_REDRAW;
    bool admin = v->users[v->sel].admin;
    if (in_rect(v->admin_box, x, y)) {
        snprintf(v->sql, sizeof(v->sql), admin ? "REVOKE GRANT OPTION ON *.* FROM %s" : "GRANT ALL ON *.* TO %s WITH GRANT OPTION", u);
        return UV_EXEC;
    }
    for (int i = 0; i < v->ntg && i < 4; i++)
        if (in_rect(v->tg_x[i], x, y)) {
            snprintf(v->sql, sizeof(v->sql), "REVOKE ALL ON %s.%s FROM %s", v->tg_db[i], v->tg_table[i], u);
            return UV_EXEC;
        }
    if (!admin && in_rect(v->matrix, x, y)) {
        int r = (y - v->matrix.y) / ROW_H, d = r + v->scroll;
        if (r < 0 || r >= v->visible_rows || d >= v->ndbs) return UV_REDRAW;
        for (int c = 0; c <= NPRIV; c++) {
            int cx = v->col_x[c] + v->col_w / 2;
            if (x < cx - 14 || x >= cx + 14) continue;
            unsigned bit = c < NPRIV ? 1u << c : DB_PRIV_ALL;
            bool on = (v->grant[d] & bit) == bit;
            unsigned inh = d ? v->grant[0] : 0;
            if ((inh & bit) == bit && !on) return error(v, "Given on all databases: change it in the first row");
            char target[80];
            snprintf(target, sizeof(target), "%s.*", v->dbs[d]);
            const char* pn = c < NPRIV ? PRIV[c] : "ALL";
            if (on) snprintf(v->sql, sizeof(v->sql), "REVOKE %s ON %s FROM %s", pn, target, u);
            else snprintf(v->sql, sizeof(v->sql), "GRANT %s ON %s TO %s", pn, target, u);
            return UV_EXEC;
        }
    }
    return UV_REDRAW;
}

bool uv_tick(UsersView* v, uint64_t now_ms) {
    if (!v || !v->open) return false;
    LineEdit* f = focused(v);
    return f && line_edit_tick(f, now_ms);
}
