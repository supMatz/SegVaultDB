#include "schema_view.h"
#include "schema_model.h"
#include "theme.h"
#include "line_edit.h"
#include "png_write.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── misure del diagramma (unità del diagramma = pixel con zoom 100%) ──
#define BAR_H     46
#define U_HEAD    34      // UML: intestazione della classe
#define U_ROW     22      // UML: riga di un attributo
#define U_PADB    8
#define U_MARK    30      // UML: colonna dei simboli PK / FK
#define E_ENT     40      // ER: rettangolo dell'entità
#define E_LINK    60      // ER: rombo di una relazione N:M (tabella di collegamento)
#define E_DIAM    27      // ER: mezza altezza del rombo di una relazione 1:N
#define EH(t)     ((t)->link ? E_LINK : E_ENT)
#define E_ROW     20      // ER: riga di un attributo
#define F_NAME    14
#define F_ROW     13
#define F_TYPE    12
#define F_SMALL   11
#define GRID      20
#define ZOOM_MIN  0.3f
#define ZOOM_MAX  2.0f

enum { NOT_ER = 0, NOT_UML = 1 };
enum { DRAG_NONE, DRAG_MOVE, DRAG_PAN, DRAG_LINK };
enum { MENU_NONE, MENU_TABLE, MENU_REL, MENU_CANVAS };
enum { CONF_NONE, CONF_DROP_TABLE, CONF_DROP_REL };
enum { MI_EDIT = 1, MI_BROWSE, MI_LINK, MI_LINK_NM, MI_DROP_TABLE, MI_REQUIRED, MI_OPTIONAL, MI_DROP_REL, MI_NEW_HERE, MI_ARRANGE, MI_FIT };

static const char* menu_label(int id) {
    switch (id) {
        case MI_EDIT: return "Edit structure\xe2\x80\xa6";
        case MI_BROWSE: return "Browse data";
        case MI_LINK: return "Add relationship\xe2\x80\xa6";
        case MI_LINK_NM: return "Add many-to-many relationship\xe2\x80\xa6";
        case MI_DROP_TABLE: return "Drop table\xe2\x80\xa6";
        case MI_REQUIRED: return "Make mandatory (NOT NULL)";
        case MI_OPTIONAL: return "Make optional (NULL allowed)";
        case MI_DROP_REL: return "Delete relationship\xe2\x80\xa6";
        case MI_NEW_HERE: return "New table here\xe2\x80\xa6";
        case MI_ARRANGE: return "Arrange automatically";
        case MI_FIT: return "Fit to window";
        default: return "";
    }
}

struct SchemaView {
    PlatformWindow* win;
    char        data_dir[512];
    char        db[64];
    SchemaModel m;
    int*        entw;                 // ER: larghezza del rettangolo dell'entità (per tabella)
    bool        loaded, invalid, sizes_dirty, need_fit, had_positions;
    int         notation;
    float       zoom, pan_x, pan_y;
    Rect        area, canvas;
    Color       bg;

    int  sel_table, sel_rel, hover_table, hover_rel, hover_handle;   // hover_handle: -1 = tabella, >= 0 colonna, -2 nessuno
    int  drag, drag_table, drag_dx, drag_dy, drag_sx, drag_sy;
    float pan0x, pan0y;
    bool drag_moved;
    int  link_src, link_col, link_target;
    bool  link_sticky;                 // "Add relationship…": il click successivo sceglie la tabella
    bool  link_nm;                     // molti-a-molti (tabella di collegamento)
    bool  conf_cols;                   // conferma: elimina anche le colonne della relazione
    char  conf_cols_text[160];
    Rect  conf_check;
    bool  view_dirty;                  // zoom / spostamento da salvare
    uint64_t view_changed;
    int  mx, my;
    uint64_t last_click;
    int  last_click_target, last_click_x, last_click_y;

    int  menu_kind, menu_target, menu_items[8], menu_n, menu_hover;
    Rect menu_rect;
    float menu_wx, menu_wy;

    int  confirm, confirm_target;
    char confirm_text[2][200];
    Rect conf_rect, conf_ok, conf_cancel;

    char dbs[64][64];
    int  ndbs;
    bool db_menu;
    int  db_hover;
    Rect db_btn, db_menu_rect;
    LineEdit* newdb;
    LineEdit* exp_path;                // esportazione PNG
    bool  exp_open;
    int   exp_scale;
    bool  exp_light;                   // sfondo chiaro (tema chiaro) per documenti e stampa
    Rect  exp_rect, exp_ok, exp_s1, exp_s2, exp_l0, exp_l1, b_export;
    bool  exporting;                   // disegno per l'immagine: niente griglia, selezione, pallini
    bool newdb_open;
    Rect newdb_rect, newdb_ok;

    Rect b_table, b_newdb, b_er, b_uml, b_zout, b_zin, b_zlabel, b_fit, b_arrange, b_empty;

    char  table[64];
    char* sql;
    char  exec_what[200];
    char  pending_db[64];
    char  pending_table[64];
    char  focus_table[64];
    float pending_x, pending_y;
    bool  pending_place;
    char  msg[300];
    bool  msg_err;
    uint64_t msg_until;
};

static bool in_rect(Rect r, int x, int y) { return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// ── coordinate ──────────────────────────────────────────────────

static int   SX(const SchemaView* v, float wx) { return v->canvas.x + (int)lroundf((wx - v->pan_x) * v->zoom); }
static int   SY(const SchemaView* v, float wy) { return v->canvas.y + (int)lroundf((wy - v->pan_y) * v->zoom); }
static float WX(const SchemaView* v, int sx)   { return (float)(sx - v->canvas.x) / v->zoom + v->pan_x; }
static float WY(const SchemaView* v, int sy)   { return (float)(sy - v->canvas.y) / v->zoom + v->pan_y; }
static int   FS(const SchemaView* v, int base) { int s = (int)lroundf((float)base * v->zoom); return s < 6 ? 6 : s; }
static int   SL(const SchemaView* v, float len) { return (int)lroundf(len * v->zoom); }

static void set_msg(SchemaView* v, const char* text, bool err) {
    snprintf(v->msg, sizeof(v->msg), "%s", text ? text : "");
    v->msg_err = err;
    v->msg_until = platform_time_ms() + (err ? 9000 : 4000);
}

// spazio tra le tabelle disposte automaticamente (ER: rombi e cardinalità tra le entità)
static int GAP_X(const SchemaView* v) { return v->notation == NOT_ER ? 230 : 130; }
static int GAP_Y(const SchemaView* v) { return v->notation == NOT_ER ? 70 : 60; }

static void positions_path(const SchemaView* v, char* out, size_t n) {
    snprintf(out, n, "%.400s/diagram_%.63s.txt", v->data_dir[0] ? v->data_dir : ".", v->db);
}

static void save_positions(SchemaView* v) {
    if (!v->loaded) return;
    snprintf(v->m.view, sizeof(v->m.view), "v %d %.3f %.1f %.1f", v->notation, (double)v->zoom, (double)v->pan_x, (double)v->pan_y);
    v->view_dirty = false;
    char p[600];
    positions_path(v, p, sizeof(p));
    sm_save_positions(&v->m, p);
}

// ── ciclo di vita ───────────────────────────────────────────────

SchemaView* schema_view_create(PlatformWindow* win) {
    SchemaView* v = calloc(1, sizeof(SchemaView));
    if (!v) return NULL;
    v->win = win;
    v->zoom = 1.0f;
    v->pan_x = v->pan_y = -20;
    v->notation = NOT_ER;
    v->invalid = true;
    v->sel_table = v->sel_rel = v->hover_table = v->hover_rel = -1;
    v->hover_handle = -2;
    v->link_src = -1;
    v->last_click_target = -9;
    v->newdb = line_edit_create("database_name");
    if (v->newdb) { v->newdb->win = win; v->newdb->base.visible = false; }
    v->exp_path = line_edit_create("path of the .png file");
    if (v->exp_path) { v->exp_path->win = win; v->exp_path->base.visible = false; }
    v->exp_scale = 2;
    return v;
}

void schema_view_destroy(SchemaView* v) {
    if (!v) return;
    if (v->view_dirty) save_positions(v);
    sm_free(&v->m);
    free(v->entw);
    free(v->sql);
    if (v->newdb) widget_destroy((Widget*)v->newdb);
    if (v->exp_path) widget_destroy((Widget*)v->exp_path);
    free(v);
}

void schema_view_set_data_dir(SchemaView* v, const char* dir) {
    if (v) snprintf(v->data_dir, sizeof(v->data_dir), "%.500s", dir ? dir : "");
}

const char* schema_view_db(const SchemaView* v) { return v->db; }
const char* schema_view_table(const SchemaView* v) { return v->table; }
const char* schema_view_sql(const SchemaView* v) { return v->sql ? v->sql : ""; }

void schema_view_invalidate(SchemaView* v) { if (v) v->invalid = true; }

void schema_view_show_db(SchemaView* v, const char* db, const char* focus_table) {
    if (!v) return;
    if (db && db[0] && strcmp(db, v->db)) {
        snprintf(v->db, sizeof(v->db), "%.63s", db);
        v->sel_table = v->sel_rel = -1;
        v->need_fit = true;
        v->invalid = true;
    }
    if (focus_table) snprintf(v->focus_table, sizeof(v->focus_table), "%.63s", focus_table);
}

void schema_view_table_created(SchemaView* v, const char* name) {
    if (!v || !name) return;
    snprintf(v->pending_table, sizeof(v->pending_table), "%.63s", name);
    snprintf(v->focus_table, sizeof(v->focus_table), "%.63s", name);
    v->invalid = true;
}

void schema_view_table_renamed(SchemaView* v, const char* from, const char* to) {
    if (!v || !v->loaded) return;
    int i = sm_find(&v->m, from);
    if (i < 0) return;
    snprintf(v->m.t[i].name, sizeof(v->m.t[i].name), "%.63s", to);
    save_positions(v);
    v->invalid = true;
}

void schema_view_sync(SchemaView* v) {
    if (!v || !v->invalid) return;
    v->invalid = false;
    v->ndbs = 0;
    NameList* dbs = db_list_databases();
    for (int i = 0; dbs && i < dbs->count && v->ndbs < 64; i++) snprintf(v->dbs[v->ndbs++], 64, "%.63s", dbs->names[i]);
    if (dbs) db_namelist_free(dbs);
    bool exists = false;
    for (int i = 0; i < v->ndbs; i++) exists = exists || !strcmp(v->dbs[i], v->db);
    if (!exists) {
        const char* cur = db_current_database();
        snprintf(v->db, sizeof(v->db), "%.63s", cur && cur[0] ? cur : v->ndbs ? v->dbs[0] : "");
        v->need_fit = true;
        v->sel_table = v->sel_rel = -1;
    }
    char sel[64] = "";
    if (v->loaded && v->sel_table >= 0 && v->sel_table < v->m.nt) snprintf(sel, sizeof(sel), "%s", v->m.t[v->sel_table].name);
    sm_free(&v->m);
    free(v->entw);
    v->entw = NULL;
    v->loaded = v->db[0] && sm_load(&v->m, v->db);
    v->sel_rel = -1;
    v->sel_table = v->loaded && sel[0] ? sm_find(&v->m, sel) : -1;
    v->hover_table = v->hover_rel = -1;
    v->drag = DRAG_NONE;
    v->link_src = -1;
    if (!v->loaded) return;
    v->entw = calloc((size_t)(v->m.nt ? v->m.nt : 1), sizeof(int));
    char p[600];
    positions_path(v, p, sizeof(p));
    v->had_positions = sm_load_positions(&v->m, p);
    int nt;
    float z, px, py;
    if (sscanf(v->m.view, "v %d %f %f %f", &nt, &z, &px, &py) == 4 && v->need_fit) {   // la vista com'era
        v->notation = nt == NOT_UML ? NOT_UML : NOT_ER;
        v->zoom = fmaxf(ZOOM_MIN, fminf(ZOOM_MAX, z));
        v->pan_x = px;
        v->pan_y = py;
        v->need_fit = false;
    }
    v->sizes_dirty = true;
}

// ── misure dei riquadri ─────────────────────────────────────────

// attributi mostrati in ER: le chiavi esterne sono le relazioni; in una relazione N:M anche la chiave
static bool er_visible(const SmTable* t, const SmCol* c) { return t->link ? !c->pk : (!c->fk || c->pk); }

// in ER la tabella di collegamento è una relazione: un rombo tra le due entità
static bool er_link(const SchemaView* v, int i) { return v->notation == NOT_ER && v->m.t[i].link; }

static int tw(SchemaView* v, const char* s, int size, FontStyle st) { return platform_measure_text_ex(v->win, s, -1, size, st); }

// mezza larghezza di un rombo alto 2*hh che contiene al centro un testo largo text_w e alto text_h
static float diamond_hw(float text_w, float text_h, float hh) {
    float k = 1.0f - (text_h / 2 + 3) / hh;
    return (text_w / 2 + 10) / (k > 0.25f ? k : 0.25f);
}

static void measure(SchemaView* v) {
    for (int i = 0; i < v->m.nt; i++) {
        SmTable* t = &v->m.t[i];
        int name_w = tw(v, t->name, F_NAME, FONT_UI_BOLD);
        if (v->notation == NOT_UML) {
            int w = name_w + 40;
            for (int c = 0; c < t->ncols; c++) {
                int rw = U_MARK + tw(v, t->cols[c].name, F_ROW, t->cols[c].pk ? FONT_UI_BOLD : FONT_UI) + 18
                       + tw(v, t->cols[c].type, F_TYPE, FONT_MONO) + 14
                       + (t->cols[c].notnull ? 0 : tw(v, "null", F_SMALL - 1, FONT_UI) + 10);
                if (rw > w) w = rw;
            }
            t->w = SV_MAX(w, 170);
            t->h = U_HEAD + t->ncols * U_ROW + U_PADB;
        } else {
            int ew = t->link ? (int)(2 * diamond_hw((float)name_w, (float)platform_text_height(v->win, F_NAME, FONT_UI_BOLD), E_LINK / 2.0f))
                             : SV_MAX(name_w + 40, 110), w = ew, n = 0;
            for (int c = 0; c < t->ncols; c++) {
                if (!er_visible(t, &t->cols[c])) continue;
                n++;
                int rw = (t->link ? ew / 2 - 18 : 0) + 46 + tw(v, t->cols[c].name, F_ROW, FONT_UI) + 8;
                if (rw > w) w = rw;
            }
            v->entw[i] = ew;
            t->w = w;
            t->h = EH(t) + (n ? 10 + n * E_ROW : 0);
        }
    }
}

// rettangolo a cui si attaccano le relazioni (ER: solo l'entità, senza gli attributi)
static void anchor(const SchemaView* v, int i, float* x, float* y, float* w, float* h) {
    const SmTable* t = &v->m.t[i];
    *x = (float)t->x; *y = (float)t->y;
    *w = (float)(v->notation == NOT_ER ? v->entw[i] : t->w);
    *h = (float)(v->notation == NOT_ER ? EH(t) : t->h);
}

static void fit(SchemaView* v) {
    v->view_dirty = true;
    v->view_changed = platform_time_ms();
    if (!v->loaded || v->m.nt == 0 || v->canvas.w <= 0) { v->zoom = 1.0f; v->pan_x = v->pan_y = -20; return; }
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < v->m.nt; i++) {
        const SmTable* t = &v->m.t[i];
        x0 = SV_MIN(x0, t->x); y0 = SV_MIN(y0, t->y - 40);
        x1 = SV_MAX(x1, t->x + t->w + 40); y1 = SV_MAX(y1, t->y + t->h);
    }
    float bw = (float)(x1 - x0 + 80), bh = (float)(y1 - y0 + 80);
    float z = fminf((float)v->canvas.w / bw, (float)(v->canvas.h - 30) / bh);
    v->zoom = fmaxf(ZOOM_MIN, fminf(1.0f, z));
    v->pan_x = (float)x0 - 40 - ((float)v->canvas.w / v->zoom - bw) / 2;
    v->pan_y = (float)y0 - 40 - ((float)(v->canvas.h - 30) / v->zoom - bh) / 2;
}

static void center_on(SchemaView* v, int i) {
    const SmTable* t = &v->m.t[i];
    v->pan_x = (float)t->x + (float)t->w / 2 - (float)v->canvas.w / 2 / v->zoom;
    v->pan_y = (float)t->y + (float)t->h / 2 - (float)v->canvas.h / 2 / v->zoom;
}

static bool table_visible(const SchemaView* v, int i) {
    const SmTable* t = &v->m.t[i];
    int x = SX(v, (float)t->x), y = SY(v, (float)t->y);
    return x >= v->canvas.x && y >= v->canvas.y && x + SL(v, (float)t->w) <= v->canvas.x + v->canvas.w &&
           y + SL(v, (float)t->h) <= v->canvas.y + v->canvas.h;
}

// sposta la tabella i (già posizionata) in un punto libero vicino, se ne copre un'altra
static void place_free(SchemaView* v, int i) {
    SmTable* t = &v->m.t[i];
    int x0 = t->x, y0 = t->y;
    for (int ring = 0; ring < 40; ring++) {
        for (int k = 0; k < 8; k++) {
            static const int DX[8] = { 0, 1, 0, -1, 1, 1, -1, -1 }, DY[8] = { 1, 0, -1, 0, 1, -1, 1, -1 };
            t->x = x0 + DX[k] * ring * 30;
            t->y = y0 + DY[k] * ring * 30;
            bool hit = false;
            for (int j = 0; j < v->m.nt && !hit; j++) {
                const SmTable* o = &v->m.t[j];
                if (j == i || !o->placed) continue;
                hit = t->x < o->x + o->w + 40 && o->x < t->x + t->w + 40 && t->y < o->y + o->h + 40 && o->y < t->y + t->h + 40;
            }
            if (!hit) return;
            if (ring == 0) break;
        }
    }
    t->x = x0; t->y = y0;
}

static void ensure_layout(SchemaView* v) {
    if (!v->loaded || !v->sizes_dirty) return;
    v->sizes_dirty = false;
    measure(v);
    bool any_unplaced = false;
    for (int i = 0; i < v->m.nt; i++) {
        SmTable* t = &v->m.t[i];
        if (t->placed) continue;
        if (v->pending_place && !strcasecmp(t->name, v->pending_table)) {
            t->x = (int)v->pending_x; t->y = (int)v->pending_y; t->placed = true;
            place_free(v, i);
        } else any_unplaced = true;
    }
    v->pending_place = false;
    if (any_unplaced) {
        sm_auto_layout(&v->m, !v->had_positions, GAP_X(v), GAP_Y(v));
        save_positions(v);
        v->had_positions = true;
    }
    if (v->need_fit) { fit(v); v->need_fit = false; }
    if (v->focus_table[0]) {
        int i = sm_find(&v->m, v->focus_table);
        if (i >= 0) { v->sel_table = i; v->sel_rel = -1; if (!table_visible(v, i)) center_on(v, i); }
        v->focus_table[0] = '\0';
    }
}

// ── geometria delle relazioni ───────────────────────────────────

typedef struct {
    float p[6][2];       // spezzata dalla figlia al genitore
    int   n;
    float mx, my;        // punto per il rombo ER
} RelGeo;

// uscita dal rettangolo partendo da (cx, cy) in direzione (dx, dy)
static void rect_exit(float x, float y, float w, float h, float cx, float cy, float dx, float dy, float* ox, float* oy) {
    float t = 1e9f;
    if (dx > 1e-6f) t = fminf(t, (x + w - cx) / dx);
    if (dx < -1e-6f) t = fminf(t, (x - cx) / dx);
    if (dy > 1e-6f) t = fminf(t, (y + h - cy) / dy);
    if (dy < -1e-6f) t = fminf(t, (y - cy) / dy);
    if (t > 1e8f) t = 0;
    *ox = cx + dx * t;
    *oy = cy + dy * t;
}

static void diamond_half(SchemaView* v, const SmRel* r, float* hw, float* hh);

static void rel_geo(const SchemaView* v, int k, RelGeo* g) {
    const SmRel* r = &v->m.r[k];
    float ax, ay, aw, ah, bx, by, bw, bh;
    anchor(v, r->child, &ax, &ay, &aw, &ah);
    anchor(v, r->parent, &bx, &by, &bw, &bh);
    if (r->child == r->parent) {     // anello sull'angolo in alto a destra
        int nth = 0;
        for (int i = 0; i < k; i++) nth += v->m.r[i].child == r->child && v->m.r[i].parent == r->child;
        float o = (float)nth * 16;
        g->n = 5;
        float y0 = ay + fminf(14, ah / 3), xr = ax + aw + 34 + o, yt = ay - 30 - o, xl = ax + aw - 30 - o;
        float pts[5][2] = { { ax + aw, y0 }, { xr, y0 }, { xr, yt }, { xl, yt }, { xl, ay } };
        memcpy(g->p, pts, sizeof(pts));
        g->mx = (xr + xl) / 2;
        g->my = yt;
        return;
    }
    // più relazioni tra le stesse tabelle: linee parallele
    int same = 0, idx = 0;
    for (int i = 0; i < v->m.nr; i++) {
        const SmRel* q = &v->m.r[i];
        bool pair = (q->child == r->child && q->parent == r->parent) || (q->child == r->parent && q->parent == r->child);
        if (!pair) continue;
        if (i < k) idx++;
        same++;
    }
    float acx = ax + aw / 2, acy = ay + ah / 2, bcx = bx + bw / 2, bcy = by + bh / 2;
    if (v->notation == NOT_ER && fabsf(bcx - acx) < fabsf(bcy - acy)) {
        // ER, una sopra l'altra: a gomito dal lato destro, così la linea non passa sugli attributi
        const SmTable *A = &v->m.t[r->child], *B = &v->m.t[r->parent];
        float hw = 0, hh;
        if (!er_link(v, r->child)) diamond_half((SchemaView*)v, r, &hw, &hh);
        float xr = (float)SV_MAX(A->x + A->w, B->x + B->w) + 30 + hw + (float)idx * 18;
        float ya = acy + ((float)idx - (float)(same - 1) / 2) * 6, yb = bcy + ((float)idx - (float)(same - 1) / 2) * 6;
        g->n = 4;
        float pts[4][2] = { { ax + aw, ya }, { xr, ya }, { xr, yb }, { bx + bw, yb } };
        memcpy(g->p, pts, sizeof(pts));
        if (er_link(v, r->child)) g->p[0][1] = acy;
        g->mx = xr;
        g->my = (ya + yb) / 2;
        return;
    }
    float dx = bcx - acx, dy = bcy - acy, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) { dx = 1; dy = 0; len = 1; }
    dx /= len; dy /= len;
    float off = ((float)idx - (float)(same - 1) / 2) * 18;
    float px = -dy * off, py = dx * off;
    float sax = acx + fmaxf(-aw / 2 + 4, fminf(aw / 2 - 4, px)), say = acy + fmaxf(-ah / 2 + 4, fminf(ah / 2 - 4, py));
    float sbx = bcx + fmaxf(-bw / 2 + 4, fminf(bw / 2 - 4, px)), sby = bcy + fmaxf(-bh / 2 + 4, fminf(bh / 2 - 4, py));
    g->n = 2;
    rect_exit(ax, ay, aw, ah, sax, say, dx, dy, &g->p[0][0], &g->p[0][1]);
    rect_exit(bx, by, bw, bh, sbx, sby, -dx, -dy, &g->p[1][0], &g->p[1][1]);
    if (er_link(v, r->child)) {      // uscita dal rombo, dal centro
        float t = 1.0f / (fabsf(dx) / (aw / 2) + fabsf(dy) / (ah / 2));
        g->p[0][0] = acx + dx * t;
        g->p[0][1] = acy + dy * t;
    }
    g->mx = (g->p[0][0] + g->p[1][0]) / 2;
    g->my = (g->p[0][1] + g->p[1][1]) / 2;
}

// nome mostrato nel rombo ER: il nome del vincolo senza il prefisso "fk_"; i nomi automatici del
// motore ("esame_1") diventano figlia_genitore ("esame_studente")
static const char* rel_label(const SchemaView* v, const SmRel* r) {
    static char buf[140];
    const char* n = r->name;
    if (!strncasecmp(n, "fk_", 3) && n[3]) n += 3;
    size_t cl = strlen(v->m.t[r->child].name);
    if (!strncasecmp(n, v->m.t[r->child].name, cl) && n[cl] == '_' && n[cl + 1]) {
        const char* d = n + cl + 1;
        while (isdigit((unsigned char)*d)) d++;
        if (!*d) {
            snprintf(buf, sizeof(buf), "%.63s_%.63s", v->m.t[r->child].name, v->m.t[r->parent].name);
            return buf;
        }
    }
    return n;
}

static void diamond_half(SchemaView* v, const SmRel* r, float* hw, float* hh) {
    *hh = E_DIAM;
    *hw = diamond_hw((float)tw(v, rel_label(v, r), F_ROW, FONT_UI_BOLD), (float)platform_text_height(v->win, F_ROW, FONT_UI_BOLD), *hh);
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = fmaxf(0, fminf(1, t));
    float qx = ax + dx * t - px, qy = ay + dy * t - py;
    return sqrtf(qx * qx + qy * qy);
}

// ── hit test ────────────────────────────────────────────────────

static int table_at(SchemaView* v, int sx, int sy) {
    if (!v->loaded || !in_rect(v->canvas, sx, sy)) return -1;
    float x = WX(v, sx), y = WY(v, sy);
    for (int i = v->m.nt - 1; i >= 0; i--) {
        const SmTable* t = &v->m.t[i];
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) return i;
    }
    return -1;
}

static int rel_at(SchemaView* v, int sx, int sy) {
    if (!v->loaded || !in_rect(v->canvas, sx, sy)) return -1;
    for (int k = 0; k < v->m.nr; k++) {
        RelGeo g;
        rel_geo(v, k, &g);
        for (int s = 0; s + 1 < g.n; s++)
            if (seg_dist((float)sx, (float)sy, (float)SX(v, g.p[s][0]), (float)SY(v, g.p[s][1]),
                         (float)SX(v, g.p[s + 1][0]), (float)SY(v, g.p[s + 1][1])) < 6) return k;
        if (v->notation == NOT_ER && !er_link(v, v->m.r[k].child)) {
            float hw, hh;
            diamond_half(v, &v->m.r[k], &hw, &hh);
            float dx = fabsf(WX(v, sx) - g.mx) / hw, dy = fabsf(WY(v, sy) - g.my) / hh;
            if (dx + dy <= 1) return k;
        }
    }
    return -1;
}

// pallini da cui si trascina una relazione: (x, y) sullo schermo
static bool handle_pos(SchemaView* v, int i, int col, int* hx, int* hy) {
    const SmTable* t = &v->m.t[i];
    if (v->notation == NOT_ER) {
        if (col >= 0) return false;
        *hx = SX(v, (float)(t->x + v->entw[i]));
        *hy = SY(v, (float)t->y + EH(t) / 2.0f);
        return true;
    }
    *hx = SX(v, (float)(t->x + t->w));
    *hy = col < 0 ? SY(v, (float)t->y + U_HEAD / 2.0f) : SY(v, (float)(t->y + U_HEAD + col * U_ROW) + U_ROW / 2.0f);
    return true;
}

static int handle_at(SchemaView* v, int i, int sx, int sy) {
    if (i < 0) return -2;
    int n = v->notation == NOT_UML ? v->m.t[i].ncols : 0;
    for (int c = -1; c < n; c++) {
        int hx, hy;
        if (!handle_pos(v, i, c, &hx, &hy)) continue;
        if ((sx - hx) * (sx - hx) + (sy - hy) * (sy - hy) <= 81) return c;
    }
    return -2;
}

// ── disegno ─────────────────────────────────────────────────────

static void text_at(SchemaView* v, const char* s, int x, int y, Color c, int base, FontStyle st) {
    platform_draw_text_ex(v->win, s, -1, (Point){x, y}, c, FS(v, base), st);
}

static void hollow_circle(SchemaView* v, int cx, int cy, int r, Color line, Color fill) {
    ui_circle(v->win, cx, cy, r, line);
    ui_circle(v->win, cx, cy, SV_MAX(1, r - SV_MAX(1, r / 3)), fill);
}

static void draw_grid(SchemaView* v) {
    if (v->zoom < 0.55f) return;
    Color dot = ui_alpha(TH_TEXT_FAINT, 70);
    float step = GRID;
    float x0 = floorf(v->pan_x / step) * step, y0 = floorf(v->pan_y / step) * step;
    float x1 = WX(v, v->canvas.x + v->canvas.w), y1 = WY(v, v->canvas.y + v->canvas.h);
    for (float y = y0; y <= y1; y += step)
        for (float x = x0; x <= x1; x += step)
            platform_fill_rect(v->win, (Rect){SX(v, x), SY(v, y), 2, 2}, dot);
}

static void draw_table_uml(SchemaView* v, int i, bool sel, bool hover, bool target) {
    PlatformWindow* win = v->win;
    const SmTable* t = &v->m.t[i];
    Rect r = { SX(v, (float)t->x), SY(v, (float)t->y), SL(v, (float)t->w), SL(v, (float)t->h) };
    int rad = SV_MAX(2, SL(v, 7));
    platform_fill_rect_rounded(win, (Rect){r.x + 2, r.y + 4, r.w, r.h}, ui_alpha(TH_BG, 120), rad);
    // selezione: un alone sottile attorno alla card, che resta scura e leggibile
    if (sel || target) platform_fill_rect_rounded(win, (Rect){r.x - 3, r.y - 3, r.w + 6, r.h + 6}, ui_alpha(TH_ACCENT, 70), rad + 3);
    ui_round_box(win, r, TH_SURFACE_2, sel || target ? TH_ACCENT : hover ? TH_TEXT_DIM : TH_BORDER_STRONG, rad);
    int hh = SL(v, U_HEAD);
    platform_fill_rect_rounded(win, (Rect){r.x + 1, r.y + 1, r.w - 2, hh}, TH_ACCENT_SOFT, rad);
    platform_fill_rect(win, (Rect){r.x + 1, r.y + hh - rad, r.w - 2, rad}, TH_ACCENT_SOFT);
    platform_fill_rect(win, (Rect){r.x + 1, r.y + hh, r.w - 2, 1}, TH_BORDER_STRONG);
    int nw = platform_measure_text_ex(win, t->name, -1, FS(v, F_NAME), FONT_UI_BOLD);
    text_at(v, t->name, r.x + (r.w - nw) / 2, ui_text_y(win, (Rect){r.x, r.y, r.w, hh}, FS(v, F_NAME), FONT_UI_BOLD),
            TH_TEXT, F_NAME, FONT_UI_BOLD);
    for (int c = 0; c < t->ncols; c++) {
        const SmCol* col = &t->cols[c];
        Rect row = { r.x, r.y + hh + SL(v, (float)(c * U_ROW)) + SL(v, 2), r.w, SL(v, U_ROW) };
        int cy = row.y + row.h / 2;
        if (col->pk) ui_icon_key(win, r.x + SL(v, 15), cy, SV_MAX(6, SL(v, 13)), col->fk ? TH_INFO : TH_WARNING);
        else if (col->fk) {
            int fw = platform_measure_text_ex(win, "FK", -1, FS(v, F_SMALL - 1), FONT_UI_BOLD);
            text_at(v, "FK", r.x + SL(v, 15) - fw / 2, ui_text_y(win, row, FS(v, F_SMALL - 1), FONT_UI_BOLD), TH_INFO, F_SMALL - 1, FONT_UI_BOLD);
        }
        FontStyle ns = col->pk ? FONT_UI_BOLD : FONT_UI;
        int x = r.x + SL(v, U_MARK);
        int w = platform_draw_text_ex(win, col->name, -1, (Point){x, ui_text_y(win, row, FS(v, F_ROW), ns)}, TH_TEXT, FS(v, F_ROW), ns);
        char type[80];
        snprintf(type, sizeof(type), ": %s", col->type);
        text_at(v, type, x + w + SL(v, 5), ui_text_y(win, row, FS(v, F_TYPE), FONT_MONO), TH_SYN_TYPE, F_TYPE, FONT_MONO);
        if (!col->notnull) {
            int qw = platform_measure_text_ex(win, "null", -1, FS(v, F_SMALL - 1), FONT_UI);
            text_at(v, "null", r.x + r.w - qw - SL(v, 8), ui_text_y(win, row, FS(v, F_SMALL - 1), FONT_UI), TH_TEXT_FAINT, F_SMALL - 1, FONT_UI);
        }
    }
}

static void draw_table_er(SchemaView* v, int i, bool sel, bool hover, bool target) {
    PlatformWindow* win = v->win;
    const SmTable* t = &v->m.t[i];
    Rect e = { SX(v, (float)t->x), SY(v, (float)t->y), SL(v, (float)v->entw[i]), SL(v, EH(t)) };
    Color border = sel || target ? TH_ACCENT : hover ? TH_TEXT : TH_TEXT_MUTED;
    int nw = platform_measure_text_ex(win, t->name, -1, FS(v, F_NAME), FONT_UI_BOLD);
    if (t->link) {   // relazione N:M: rombo con il nome
        float cx = (float)e.x + (float)e.w / 2, cy = (float)e.y + (float)e.h / 2, w = (float)e.w / 2, h = (float)e.h / 2;
        float b = sel || target ? 2.0f : 1.0f;
        float outer[8] = { cx - w - b, cy, cx, cy - h - b, cx + w + b, cy, cx, cy + h + b };
        float inner[8] = { cx - w + 1.5f, cy, cx, cy - h + 1.2f, cx + w - 1.5f, cy, cx, cy + h - 1.2f };
        ui_fill_polygon(win, outer, 4, border);
        ui_fill_polygon(win, inner, 4, sel ? TH_ACCENT_SOFT : TH_ELEVATED);
        text_at(v, t->name, (int)cx - nw / 2, ui_text_y(win, e, FS(v, F_NAME), FONT_UI_BOLD), TH_TEXT, F_NAME, FONT_UI_BOLD);
    } else {
        platform_fill_rect(win, (Rect){e.x + 2, e.y + 3, e.w, e.h}, ui_alpha(TH_BG, 120));
        platform_fill_rect(win, e, TH_ELEVATED);
        platform_draw_rect(win, e, border, sel || target ? 2 : 1);
        text_at(v, t->name, e.x + (e.w - nw) / 2, ui_text_y(win, e, FS(v, F_NAME), FONT_UI_BOLD), TH_TEXT, F_NAME, FONT_UI_BOLD);
    }
    // attributi: lecca-lecca appesi all'entità (pieno = identificatore) o alla relazione
    int n = 0;
    for (int c = 0; c < t->ncols; c++) n += er_visible(t, &t->cols[c]);
    if (!n) return;
    float sx0 = t->link ? (float)t->x + (float)v->entw[i] / 2 : (float)t->x + 18;
    int spine = SX(v, sx0);
    int y0 = e.y + e.h, last = SY(v, (float)(t->y + EH(t) + 10 + (n - 1) * E_ROW) + E_ROW / 2.0f);
    Color line = TH_TEXT_DIM;
    ui_line(win, (float)spine, (float)y0, (float)spine, (float)last, fmaxf(1, v->zoom), line);
    int k = 0;
    for (int c = 0; c < t->ncols; c++) {
        const SmCol* col = &t->cols[c];
        if (!er_visible(t, col)) continue;
        int cy = SY(v, (float)(t->y + EH(t) + 10 + k * E_ROW) + E_ROW / 2.0f);
        int cx = SX(v, sx0 + 16);
        ui_line(win, (float)spine, (float)cy, (float)cx, (float)cy, fmaxf(1, v->zoom), line);
        int rr = SV_MAX(3, SL(v, 5));
        if (col->pk) ui_circle(win, cx, cy, rr, TH_TEXT);
        else hollow_circle(v, cx, cy, rr, TH_TEXT_MUTED, v->bg);
        Rect row = { cx, cy - SL(v, E_ROW) / 2, 0, SL(v, E_ROW) };
        text_at(v, col->name, cx + SL(v, 10), ui_text_y(win, row, FS(v, F_ROW), FONT_UI),
                col->pk ? TH_TEXT : TH_TEXT_MUTED, F_ROW, FONT_UI);
        k++;
    }
}

// cardinalità vicino all'estremo (x, y) della linea, che parte nella direzione (nx, ny):
// accanto alla linea, dal lato in cui non copre il riquadro (bx, by, bw, bh) della tabella
static void card_label(SchemaView* v, float x, float y, float nx, float ny, const char* s, Color c,
                       float bx, float by, float bw, float bh) {
    float len = sqrtf(nx * nx + ny * ny);
    if (len < 1e-3f) { nx = 1; ny = 0; len = 1; }
    nx /= len; ny /= len;
    float w = (float)tw(v, s, F_SMALL, FONT_MONO), h = (float)platform_text_height(v->win, F_SMALL, FONT_MONO);
    float along = 8 + fabsf(nx) * w / 2 + fabsf(ny) * h / 2;
    float side = 6 + fabsf(ny) * w / 2 + fabsf(nx) * h / 2;
    float best_x = 0, best_y = 0;
    for (int k = 0; k < 2; k++) {
        float sg = k ? -1.0f : 1.0f;
        float cx = x + nx * along - ny * side * sg, cy = y + ny * along + nx * side * sg;
        best_x = cx; best_y = cy;
        bool hit = cx + w / 2 > bx && cx - w / 2 < bx + bw && cy + h / 2 > by && cy - h / 2 < by + bh;
        if (!hit) break;
    }
    int fw = platform_measure_text_ex(v->win, s, -1, FS(v, F_SMALL), FONT_MONO);
    int fh = platform_text_height(v->win, FS(v, F_SMALL), FONT_MONO);
    text_at(v, s, SX(v, best_x) - fw / 2, SY(v, best_y) - fh / 2, c, F_SMALL, FONT_MONO);
}

static void draw_rel(SchemaView* v, int k) {
    const SmRel* r = &v->m.r[k];
    RelGeo g;
    rel_geo(v, k, &g);
    bool sel = k == v->sel_rel, hov = k == v->hover_rel;
    Color c = sel ? TH_ACCENT : hov ? TH_TEXT : TH_TEXT_DIM;
    float th = fmaxf(1.0f, v->zoom * (sel ? 2.2f : 1.4f));
    for (int s = 0; s + 1 < g.n; s++)
        ui_line(v->win, (float)SX(v, g.p[s][0]), (float)SY(v, g.p[s][1]), (float)SX(v, g.p[s + 1][0]), (float)SY(v, g.p[s + 1][1]), th, c);
    float ax = g.p[0][0], ay = g.p[0][1], bx = g.p[g.n - 1][0], by = g.p[g.n - 1][1];
    float adx = g.p[1][0] - ax, ady = g.p[1][1] - ay, bdx = g.p[g.n - 2][0] - bx, bdy = g.p[g.n - 2][1] - by;
    if (g.n == 4) {   // a gomito: le cardinalità accanto al tratto verticale, vicino agli angoli
        ax = g.p[1][0]; ay = g.p[1][1]; adx = 0; ady = g.p[2][1] - ay;
        bx = g.p[2][0]; by = g.p[2][1]; bdx = 0; bdy = g.p[1][1] - by;
    }
    Color lc = sel ? TH_ACCENT_TEXT : TH_TEXT_MUTED;
    float cax, cay, caw, cah, pbx, pby, pbw, pbh;
    anchor(v, r->child, &cax, &cay, &caw, &cah);
    anchor(v, r->parent, &pbx, &pby, &pbw, &pbh);
    if (v->notation == NOT_UML) {
        card_label(v, ax, ay, adx, ady, r->one_to_one ? "0..1" : "*", lc, cax, cay, caw, cah);
        card_label(v, bx, by, bdx, bdy, r->optional ? "0..1" : "1", lc, pbx, pby, pbw, pbh);
        // freccia aperta verso la tabella riferita (navigabilità della chiave esterna)
        float len = sqrtf(bdx * bdx + bdy * bdy);
        if (len > 1) {
            float ux = bdx / len, uy = bdy / len, s = 9;
            float tx = (float)SX(v, bx), ty = (float)SY(v, by);
            float l1x = (float)SX(v, bx + ux * s - uy * s * 0.55f), l1y = (float)SY(v, by + uy * s + ux * s * 0.55f);
            float l2x = (float)SX(v, bx + ux * s + uy * s * 0.55f), l2y = (float)SY(v, by + uy * s - ux * s * 0.55f);
            ui_line(v->win, tx, ty, l1x, l1y, th, c);
            ui_line(v->win, tx, ty, l2x, l2y, th, c);
        }
    } else if (er_link(v, r->child)) {
        card_label(v, bx, by, bdx, bdy, r->one_to_one ? "(0,1)" : "(0,N)", lc, pbx, pby, pbw, pbh);
    } else {
        card_label(v, ax, ay, adx, ady, r->optional ? "(0,1)" : "(1,1)", lc, cax, cay, caw, cah);
        card_label(v, bx, by, bdx, bdy, r->one_to_one ? "(0,1)" : "(0,N)", lc, pbx, pby, pbw, pbh);
        float hw, hh;
        diamond_half(v, r, &hw, &hh);
        float cx = (float)SX(v, g.mx), cy = (float)SY(v, g.my), w = hw * v->zoom, h = hh * v->zoom;
        float outer[8] = { cx - w - 1, cy, cx, cy - h - 1, cx + w + 1, cy, cx, cy + h + 1 };
        float inner[8] = { cx - w + 1.5f, cy, cx, cy - h + 1.2f, cx + w - 1.5f, cy, cx, cy + h - 1.2f };
        ui_fill_polygon(v->win, outer, 4, sel ? TH_ACCENT : hov ? TH_TEXT : TH_TEXT_MUTED);
        ui_fill_polygon(v->win, inner, 4, sel ? TH_ACCENT_SOFT : TH_SURFACE);
        const char* lbl = rel_label(v, r);
        int lw = platform_measure_text_ex(v->win, lbl, -1, FS(v, F_ROW), FONT_UI_BOLD);
        int lh = platform_text_height(v->win, FS(v, F_ROW), FONT_UI_BOLD);
        text_at(v, lbl, (int)cx - lw / 2, (int)cy - lh / 2, TH_TEXT, F_ROW, FONT_UI_BOLD);
    }
}

static void draw_button(SchemaView* v, Rect r, const char* label, bool primary, bool active) {
    PlatformWindow* win = v->win;
    bool hov = in_rect(r, v->mx, v->my);
    Color fill = primary ? (hov ? TH_ACCENT_HOVER : TH_ACCENT) : active ? TH_ACCENT_SOFT : hov ? TH_ELEVATED_HOVER : TH_ELEVATED;
    if (primary) platform_fill_rect_rounded(win, r, fill, 6);
    else ui_round_box(win, r, fill, active ? TH_ACCENT : TH_BORDER, 6);
    FontStyle fs = primary || active ? FONT_UI_BOLD : FONT_UI;
    int w = platform_measure_text_ex(win, label, -1, TH_FONT_UI, fs);
    platform_draw_text_ex(win, label, -1, (Point){r.x + (r.w - w) / 2, ui_text_y(win, r, TH_FONT_UI, fs)},
                          primary ? TH_ON_ACCENT : active ? TH_ACCENT_TEXT : TH_TEXT, TH_FONT_UI, fs);
}

static int btn_w(SchemaView* v, const char* label) { return platform_measure_text_ex(v->win, label, -1, TH_FONT_UI, FONT_UI_BOLD) + 26; }

static void draw_bar(SchemaView* v) {
    PlatformWindow* win = v->win;
    Rect bar = { v->area.x, v->area.y, v->area.w, BAR_H };
    platform_fill_rect(win, bar, TH_SURFACE);
    platform_fill_rect(win, (Rect){bar.x, bar.y + bar.h - 1, bar.w, 1}, TH_BORDER);
    int y = bar.y + 7, h = 32, x = bar.x + 12;
    // database
    const char* dbn = v->db[0] ? v->db : "no database";
    int dw = platform_measure_text_ex(win, dbn, -1, TH_FONT_UI, FONT_UI_BOLD);
    v->db_btn = (Rect){ x, y, SV_MIN(dw + 54, 260), h };
    bool dh = in_rect(v->db_btn, v->mx, v->my) || v->db_menu;
    ui_round_box(win, v->db_btn, dh ? TH_ELEVATED_HOVER : TH_ELEVATED, v->db_menu ? TH_ACCENT : TH_BORDER_STRONG, 16);
    ui_icon_database(win, x + 17, y + 16, 14, v->db[0] ? TH_ICON_DB : TH_TEXT_FAINT);
    ui_text_clipped(win, dbn, x + 31, ui_text_y(win, v->db_btn, TH_FONT_UI, FONT_UI_BOLD), v->db_btn.w - 54,
                    v->db[0] ? TH_TEXT : TH_TEXT_DIM, TH_FONT_UI, FONT_UI_BOLD);
    ui_icon_chevron(win, v->db_btn.x + v->db_btn.w - 15, y + 16, 8, CHEVRON_DOWN, TH_TEXT_DIM);
    x += v->db_btn.w + 10;
    v->b_table = (Rect){ x, y, btn_w(v, "+ Table"), h };
    v->b_table.w = v->db[0] ? v->b_table.w : 0;
    if (v->b_table.w) { draw_button(v, v->b_table, "+ Table", true, false); x += v->b_table.w + 8; }
    v->b_newdb = (Rect){ x, y, btn_w(v, "+ Database"), h };
    draw_button(v, v->b_newdb, "+ Database", false, false);
    x += v->b_newdb.w + 8;
    v->b_export = (Rect){ x, y, btn_w(v, "Export PNG"), h };
    v->b_export.w = v->loaded && v->m.nt ? v->b_export.w : 0;
    if (v->b_export.w) draw_button(v, v->b_export, "Export PNG", false, v->exp_open);

    // a destra: notazione, zoom, adatta, disponi
    int rx = bar.x + bar.w - 12;
    v->b_arrange = (Rect){ rx - btn_w(v, "Arrange"), y, btn_w(v, "Arrange"), h }; rx = v->b_arrange.x - 6;
    v->b_fit = (Rect){ rx - btn_w(v, "Fit"), y, btn_w(v, "Fit"), h }; rx = v->b_fit.x - 10;
    v->b_zin = (Rect){ rx - 32, y, 32, h }; rx -= 32;
    v->b_zlabel = (Rect){ rx - 52, y, 52, h }; rx -= 52;
    v->b_zout = (Rect){ rx - 32, y, 32, h }; rx -= 32 + 14;
    int ew = btn_w(v, "UML");
    v->b_uml = (Rect){ rx - ew, y, ew, h }; rx -= ew;
    v->b_er = (Rect){ rx - ew, y, ew, h };
    bool room = v->b_er.x > SV_MAX(v->b_newdb.x + v->b_newdb.w, v->b_export.x + v->b_export.w) + 20;
    if (!room) { v->b_er.w = v->b_uml.w = v->b_zin.w = v->b_zout.w = v->b_zlabel.w = 0; }
    if (room) {
        draw_button(v, v->b_er, "ER", false, v->notation == NOT_ER);
        draw_button(v, v->b_uml, "UML", false, v->notation == NOT_UML);
        draw_button(v, v->b_zout, "\xe2\x88\x92", false, false);
        draw_button(v, v->b_zin, "+", false, false);
        char z[16];
        snprintf(z, sizeof(z), "%d%%", (int)lroundf(v->zoom * 100));
        int zw = platform_measure_text_ex(win, z, -1, TH_FONT_SMALL + 1, FONT_MONO);
        platform_draw_text_ex(win, z, -1, (Point){v->b_zlabel.x + (v->b_zlabel.w - zw) / 2, ui_text_y(win, v->b_zlabel, TH_FONT_SMALL + 1, FONT_MONO)},
                              TH_TEXT_MUTED, TH_FONT_SMALL + 1, FONT_MONO);
    }
    draw_button(v, v->b_fit, "Fit", false, false);
    draw_button(v, v->b_arrange, "Arrange", false, false);
}

static void draw_empty(SchemaView* v, const char* title, const char* sub, const char* button) {
    PlatformWindow* win = v->win;
    int cx = v->canvas.x + v->canvas.w / 2, cy = v->canvas.y + v->canvas.h / 2 - 40;
    ui_icon_table(win, cx, cy - 30, 34, TH_TEXT_FAINT);
    int w = platform_measure_text_ex(win, title, -1, 16, FONT_UI_BOLD);
    platform_draw_text_ex(win, title, -1, (Point){cx - w / 2, cy}, TH_TEXT, 16, FONT_UI_BOLD);
    w = platform_measure_text_ex(win, sub, -1, TH_FONT_UI, FONT_UI);
    platform_draw_text_ex(win, sub, -1, (Point){cx - w / 2, cy + 28}, TH_TEXT_DIM, TH_FONT_UI, FONT_UI);
    v->b_empty = (Rect){0, 0, 0, 0};
    if (button) {
        int bw = btn_w(v, button);
        v->b_empty = (Rect){ cx - bw / 2, cy + 60, bw, 34 };
        draw_button(v, v->b_empty, button, true, false);
    }
}

static void draw_menu(SchemaView* v) {
    if (v->menu_kind == MENU_NONE) return;
    PlatformWindow* win = v->win;
    Rect m = v->menu_rect;
    platform_fill_rect_rounded(win, (Rect){m.x - 2, m.y + 3, m.w + 4, m.h + 4}, ui_alpha(TH_BG, 170), 10);
    ui_round_box(win, m, TH_SURFACE_2, TH_BORDER_STRONG, 8);
    for (int i = 0; i < v->menu_n; i++) {
        Rect it = { m.x + 4, m.y + 4 + i * 28, m.w - 8, 28 };
        bool danger = v->menu_items[i] == MI_DROP_TABLE || v->menu_items[i] == MI_DROP_REL;
        if (i == v->menu_hover) platform_fill_rect_rounded(win, it, danger ? TH_ERROR_SOFT : TH_HOVER, 5);
        platform_draw_text_ex(win, menu_label(v->menu_items[i]), -1, (Point){it.x + 10, ui_text_y(win, it, TH_FONT_UI, FONT_UI)},
                              danger ? TH_ERROR_TEXT : TH_TEXT, TH_FONT_UI, FONT_UI);
    }
}

static void draw_db_menu(SchemaView* v) {
    if (!v->db_menu) return;
    PlatformWindow* win = v->win;
    int n = v->ndbs + 1;
    Rect m = { v->db_btn.x, v->db_btn.y + v->db_btn.h + 4, SV_MAX(v->db_btn.w, 220), n * 28 + 8 + 9 };
    v->db_menu_rect = m;
    platform_fill_rect_rounded(win, (Rect){m.x - 2, m.y + 3, m.w + 4, m.h + 4}, ui_alpha(TH_BG, 170), 10);
    ui_round_box(win, m, TH_SURFACE_2, TH_BORDER_STRONG, 8);
    for (int i = 0; i < n; i++) {
        int yy = m.y + 4 + i * 28 + (i == v->ndbs ? 9 : 0);
        Rect it = { m.x + 4, yy, m.w - 8, 28 };
        if (i == v->ndbs) platform_fill_rect(win, (Rect){m.x + 8, yy - 5, m.w - 16, 1}, TH_BORDER);
        bool cur = i < v->ndbs && !strcmp(v->dbs[i], v->db);
        if (i == v->db_hover || cur) platform_fill_rect_rounded(win, it, cur ? TH_ACCENT_SOFT : TH_HOVER, 5);
        const char* s = i < v->ndbs ? v->dbs[i] : "New database\xe2\x80\xa6";
        if (i < v->ndbs) ui_icon_database(win, it.x + 14, it.y + 14, 12, TH_ICON_DB);
        platform_draw_text_ex(win, s, -1, (Point){it.x + (i < v->ndbs ? 28 : 10), ui_text_y(win, it, TH_FONT_UI, FONT_UI)},
                              cur ? TH_ACCENT_TEXT : i < v->ndbs ? TH_TEXT : TH_ACCENT_TEXT, TH_FONT_UI, FONT_UI);
    }
}

static void draw_newdb(SchemaView* v) {
    if (!v->newdb_open) return;
    PlatformWindow* win = v->win;
    Rect p = { v->b_newdb.x, v->b_newdb.y + v->b_newdb.h + 6, 340, 96 };
    p.x = SV_MIN(p.x, v->area.x + v->area.w - p.w - 8);
    v->newdb_rect = p;
    platform_fill_rect_rounded(win, (Rect){p.x - 2, p.y + 3, p.w + 4, p.h + 4}, ui_alpha(TH_BG, 170), 10);
    ui_round_box(win, p, TH_SURFACE_2, TH_BORDER_STRONG, 8);
    platform_draw_text_ex(win, "NEW DATABASE", -1, (Point){p.x + 14, p.y + 12}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    v->newdb->base.bounds = (Rect){ p.x + 14, p.y + 34, p.w - 28 - 90, 32 };
    v->newdb->base.visible = true;
    widget_draw((Widget*)v->newdb, win);
    v->newdb_ok = (Rect){ p.x + p.w - 14 - 80, p.y + 34, 80, 32 };
    draw_button(v, v->newdb_ok, "Create", true, false);
    platform_draw_text_ex(win, "Letters, digits and _  \xc2\xb7  Enter to create", -1, (Point){p.x + 14, p.y + 72},
                          TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
}

static void draw_confirm(SchemaView* v) {
    if (v->confirm == CONF_NONE) return;
    PlatformWindow* win = v->win;
    platform_fill_rect(win, v->area, ui_alpha(TH_BG, 150));
    int w = 480, h = v->confirm == CONF_DROP_REL && v->conf_cols_text[0] ? 180 : 150;
    Rect r = { v->area.x + (v->area.w - w) / 2, v->area.y + (v->area.h - h) / 2, w, h };
    v->conf_rect = r;
    ui_round_box(win, r, TH_SURFACE, TH_BORDER_STRONG, 10);
    platform_draw_text_ex(win, v->confirm_text[0], -1, (Point){r.x + 22, r.y + 20}, TH_TEXT, 15, FONT_UI_BOLD);
    ui_text_clipped(win, v->confirm_text[1], r.x + 22, r.y + 50, w - 44, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    v->conf_check = (Rect){0, 0, 0, 0};
    if (v->confirm == CONF_DROP_REL && v->conf_cols_text[0]) {
        Rect cb = { r.x + 22, r.y + 80, 16, 16 };
        int tw2 = platform_measure_text_ex(win, v->conf_cols_text, -1, TH_FONT_UI, FONT_UI);
        v->conf_check = (Rect){ cb.x - 4, cb.y - 4, SV_MIN(tw2 + 34, w - 40), 24 };
        bool hov = in_rect(v->conf_check, v->mx, v->my);
        ui_round_box(win, cb, v->conf_cols ? TH_ACCENT : TH_SURFACE, v->conf_cols || hov ? TH_ACCENT : TH_BORDER_STRONG, 4);
        if (v->conf_cols) ui_icon_check(win, cb.x + 8, cb.y + 8, 11, TH_ON_ACCENT);
        ui_text_clipped(win, v->conf_cols_text, cb.x + 26, ui_text_y(win, (Rect){cb.x, cb.y - 2, 0, 20}, TH_FONT_UI, FONT_UI),
                        w - 44 - 26, TH_TEXT, TH_FONT_UI, FONT_UI);
    }
    v->conf_ok = (Rect){ r.x + w - 22 - 100, r.y + h - 22 - 34, 100, 34 };
    v->conf_cancel = (Rect){ v->conf_ok.x - 10 - 90, v->conf_ok.y, 90, 34 };
    draw_button(v, v->conf_cancel, "Cancel", false, false);
    bool hov = in_rect(v->conf_ok, v->mx, v->my);
    platform_fill_rect_rounded(win, v->conf_ok, hov ? ui_mix(TH_ERROR, TH_TEXT, 0.15f) : TH_ERROR, 6);
    const char* l = v->confirm == CONF_DROP_TABLE ? "Drop table" : "Delete";
    int lw = platform_measure_text_ex(win, l, -1, TH_FONT_UI, FONT_UI_BOLD);
    platform_draw_text_ex(win, l, -1, (Point){v->conf_ok.x + (v->conf_ok.w - lw) / 2, ui_text_y(win, v->conf_ok, TH_FONT_UI, FONT_UI_BOLD)},
                          TH_ON_ACCENT, TH_FONT_UI, FONT_UI_BOLD);
}

// relazioni (sotto) e tabelle
static void draw_diagram(SchemaView* v) {
    for (int k = 0; k < v->m.nr; k++) if (k != v->sel_rel) draw_rel(v, k);
    if (v->sel_rel >= 0 && v->sel_rel < v->m.nr) draw_rel(v, v->sel_rel);
    for (int i = 0; i < v->m.nt; i++) {
        bool sel = i == v->sel_table, hov = i == v->hover_table && v->drag == DRAG_NONE;
        bool target = v->drag == DRAG_LINK && i == v->link_target;
        if (v->notation == NOT_UML) draw_table_uml(v, i, sel, hov, target);
        else draw_table_er(v, i, sel, hov, target);
    }
}

// ── esportazione PNG ────────────────────────────────────────────

// rettangolo del diagramma (coordinate del diagramma): tabelle, linee, rombi ed etichette
static void diagram_bounds(SchemaView* v, float* x0, float* y0, float* x1, float* y1) {
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (int i = 0; i < v->m.nt; i++) {
        const SmTable* t = &v->m.t[i];
        *x0 = fminf(*x0, (float)t->x); *y0 = fminf(*y0, (float)t->y);
        *x1 = fmaxf(*x1, (float)(t->x + t->w)); *y1 = fmaxf(*y1, (float)(t->y + t->h));
    }
    for (int k = 0; k < v->m.nr; k++) {
        RelGeo g;
        rel_geo(v, k, &g);
        for (int s = 0; s < g.n; s++) {
            *x0 = fminf(*x0, g.p[s][0]); *y0 = fminf(*y0, g.p[s][1]);
            *x1 = fmaxf(*x1, g.p[s][0]); *y1 = fmaxf(*y1, g.p[s][1]);
        }
        if (v->notation == NOT_ER && !er_link(v, v->m.r[k].child)) {
            float hw, hh;
            diamond_half(v, &v->m.r[k], &hw, &hh);
            *x0 = fminf(*x0, g.mx - hw); *x1 = fmaxf(*x1, g.mx + hw);
            *y0 = fminf(*y0, g.my - hh); *y1 = fmaxf(*y1, g.my + hh);
        }
    }
    *x0 -= 50; *y0 -= 50; *x1 += 50; *y1 += 50;     // margine (anche per le cardinalità)
}

// disegna il diagramma a riquadri grandi quanto l'area visibile, legge i pixel e li ricompone
static bool export_png(SchemaView* v, const char* path, int scale, char* err, size_t errn, int* ow, int* oh) {
    if (!v->loaded || v->m.nt == 0) { snprintf(err, errn, "The diagram is empty"); return false; }
    if (v->canvas.w < 50 || v->canvas.h < 50) { snprintf(err, errn, "The window is too small"); return false; }
    float x0, y0, x1, y1;
    diagram_bounds(v, &x0, &y0, &x1, &y1);
    int W = (int)ceilf((x1 - x0) * (float)scale), H = (int)ceilf((y1 - y0) * (float)scale);
    if (W > 16000 || H > 16000 || (long long)W * H > 60000000LL) {
        snprintf(err, errn, "The image would be %dx%d pixels: too large%s", W, H, scale > 1 ? ", try 1x" : "");
        return false;
    }
    uint32_t* img = malloc(sizeof(uint32_t) * (size_t)W * (size_t)H);
    uint32_t* tile = malloc(sizeof(uint32_t) * (size_t)v->canvas.w * (size_t)v->canvas.h);
    if (!img || !tile) { free(img); free(tile); snprintf(err, errn, "Out of memory"); return false; }
    // stato della vista: salvato e ripristinato
    float zoom = v->zoom, px = v->pan_x, py = v->pan_y;
    int st = v->sel_table, sr = v->sel_rel, ht = v->hover_table, hr = v->hover_rel, dr = v->drag;
    v->zoom = (float)scale;
    v->sel_table = v->sel_rel = v->hover_table = v->hover_rel = -1;
    v->drag = DRAG_NONE;
    v->exporting = true;
    int old_theme = theme_current();
    Color old_bg = v->bg;
    if (v->exp_light && !theme_get(old_theme)->light)
        for (int t = 0; t < theme_count(); t++) if (theme_get(t)->light) { theme_apply(t); break; }
    v->bg = TH_EDITOR_BG;
    int cw = v->canvas.w, ch = v->canvas.h;
    bool ok = true;
    for (int ty = 0; ty * ch < H && ok; ty++)
        for (int tx = 0; tx * cw < W && ok; tx++) {
            v->pan_x = x0 + (float)(tx * cw) / v->zoom;
            v->pan_y = y0 + (float)(ty * ch) / v->zoom;
            platform_set_clip(v->win, v->canvas);
            platform_fill_rect(v->win, v->canvas, v->bg);
            draw_diagram(v);
            platform_reset_clip(v->win);
            ok = platform_read_pixels(v->win, v->canvas, tile);
            int w = SV_MIN(cw, W - tx * cw), h = SV_MIN(ch, H - ty * ch);
            for (int y = 0; ok && y < h; y++)
                memcpy(&img[(size_t)(ty * ch + y) * (size_t)W + (size_t)(tx * cw)], &tile[(size_t)y * (size_t)cw], sizeof(uint32_t) * (size_t)w);
        }
    v->zoom = zoom; v->pan_x = px; v->pan_y = py;
    v->sel_table = st; v->sel_rel = sr; v->hover_table = ht; v->hover_rel = hr; v->drag = dr;
    v->exporting = false;
    if (theme_current() != old_theme) theme_apply(old_theme);
    v->bg = old_bg;
    free(tile);
    if (!ok) { free(img); snprintf(err, errn, "Cannot read the drawn image"); return false; }
    ok = png_write_file(path, W, H, img);
    free(img);
    if (!ok) { snprintf(err, errn, "Cannot write %.200s", path); return false; }
    *ow = W; *oh = H;
    return true;
}

static void draw_export(SchemaView* v) {
    if (!v->exp_open) return;
    PlatformWindow* win = v->win;
    Rect p = { v->b_export.x, v->b_export.y + v->b_export.h + 6, 520, 166 };
    p.x = SV_MAX(v->area.x + 8, SV_MIN(p.x, v->area.x + v->area.w - p.w - 8));
    v->exp_rect = p;
    platform_fill_rect_rounded(win, (Rect){p.x - 2, p.y + 3, p.w + 4, p.h + 4}, ui_alpha(TH_BG, 170), 10);
    ui_round_box(win, p, TH_SURFACE_2, TH_BORDER_STRONG, 8);
    platform_draw_text_ex(win, "EXPORT THE DIAGRAM AS A PNG IMAGE", -1, (Point){p.x + 14, p.y + 12}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    v->exp_path->base.bounds = (Rect){ p.x + 14, p.y + 34, p.w - 28, 32 };
    v->exp_path->base.visible = true;
    widget_draw((Widget*)v->exp_path, win);
    int y = p.y + 78;
    platform_draw_text_ex(win, "Resolution", -1, (Point){p.x + 14, ui_text_y(win, (Rect){0, y, 0, 32}, TH_FONT_UI, FONT_UI)}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    v->exp_s1 = (Rect){ p.x + 100, y, 60, 32 };
    v->exp_s2 = (Rect){ p.x + 164, y, 104, 32 };
    draw_button(v, v->exp_s1, "1\xc3\x97", false, v->exp_scale == 1);
    draw_button(v, v->exp_s2, "2\xc3\x97 sharp", false, v->exp_scale == 2);
    y += 40;
    platform_draw_text_ex(win, "Background", -1, (Point){p.x + 14, ui_text_y(win, (Rect){0, y, 0, 32}, TH_FONT_UI, FONT_UI)}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    v->exp_l0 = (Rect){ p.x + 100, y, 104, 32 };
    v->exp_l1 = (Rect){ p.x + 208, y, 130, 32 };
    draw_button(v, v->exp_l0, "Like the app", false, !v->exp_light);
    draw_button(v, v->exp_l1, "Light (print)", false, v->exp_light);
    v->exp_ok = (Rect){ p.x + p.w - 14 - 110, y, 110, 32 };
    draw_button(v, v->exp_ok, "Save PNG", true, false);
}

static void open_export(SchemaView* v) {
    char path[700];
    snprintf(path, sizeof(path), "%.500s/%.63s_%s.png", v->data_dir[0] ? v->data_dir : ".", v->db, v->notation == NOT_ER ? "er" : "uml");
    line_edit_set_text(v->exp_path, path);
    line_edit_focus(v->exp_path, true);
    v->exp_open = true;
    v->db_menu = false;
}

static void do_export(SchemaView* v) {
    char err[300];
    int w = 0, h = 0;
    v->exp_open = false;
    line_edit_focus(v->exp_path, false);
    if (!v->exp_path->text[0]) { set_msg(v, "Write the path of the .png file", true); return; }
    if (export_png(v, v->exp_path->text, v->exp_scale, err, sizeof(err), &w, &h)) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Diagram saved: %.200s (%dx%d)", v->exp_path->text, w, h);
        set_msg(v, msg, false);
    } else set_msg(v, err, true);
}

void schema_view_draw(SchemaView* v, Rect r) {
    if (!v) return;
    PlatformWindow* win = v->win;
    v->area = r;
    v->canvas = (Rect){ r.x, r.y + BAR_H, r.w, r.h - BAR_H };
    v->bg = TH_EDITOR_BG;
    ensure_layout(v);
    platform_fill_rect(win, v->canvas, v->bg);
    platform_set_clip(win, v->canvas);
    v->b_empty = (Rect){0, 0, 0, 0};
    if (!v->ndbs) {
        draw_empty(v, "No databases yet", "Create one to start designing its tables", "Create a database");
    } else if (!v->loaded) {
        draw_empty(v, "Choose a database", "Pick one from the menu above", NULL);
    } else if (v->m.nt == 0) {
        draw_grid(v);
        draw_empty(v, "No tables yet", "Create the first table, or double-click anywhere on the canvas", "+ New table");
    } else {
        draw_grid(v);
        draw_diagram(v);
        // pallini per trascinare le relazioni (sulla tabella sotto il mouse)
        int ht = v->drag == DRAG_NONE ? v->hover_table : v->drag == DRAG_LINK ? v->link_src : -1;
        if (ht >= 0 && ht < v->m.nt) {
            int n = v->notation == NOT_UML ? v->m.t[ht].ncols : 0;
            for (int c = -1; c < n; c++) {
                int hx, hy;
                if (!handle_pos(v, ht, c, &hx, &hy)) continue;
                bool on = (v->drag == DRAG_LINK && c == v->link_col) || (v->drag == DRAG_NONE && c == v->hover_handle);
                int rad = c < 0 ? 6 : 4;
                ui_circle(win, hx, hy, rad + 2, v->bg);
                ui_circle(win, hx, hy, rad, on ? TH_ACCENT : TH_TEXT_DIM);
                if (c < 0) {
                    platform_fill_rect(win, (Rect){hx - 3, hy, 7, 1}, TH_ON_ACCENT);
                    platform_fill_rect(win, (Rect){hx, hy - 3, 1, 7}, TH_ON_ACCENT);
                }
            }
        }
        if (v->drag == DRAG_LINK && v->link_src >= 0) {
            int hx, hy;
            if (!handle_pos(v, v->link_src, v->link_col, &hx, &hy)) handle_pos(v, v->link_src, -1, &hx, &hy);
            float dx = (float)(v->mx - hx), dy = (float)(v->my - hy), len = sqrtf(dx * dx + dy * dy);
            for (float s = 0; s < len; s += 12) {
                float e = fminf(len, s + 7);
                ui_line(win, hx + dx * s / len, hy + dy * s / len, hx + dx * e / len, hy + dy * e / len, 2, TH_ACCENT);
            }
        }
        // suggerimento in basso
        const char* hint = v->drag == DRAG_LINK
            ? (v->link_nm ? "Click the other table of the many-to-many relationship  \xc2\xb7  Esc to cancel"
                          : "Release on the table it refers to  \xc2\xb7  hold Shift for many-to-many (link table)  \xc2\xb7  Esc to cancel")
            : v->notation == NOT_UML
              ? "Drag a table to move it  \xc2\xb7  drag from a + handle to another table to link them (on a column: that column becomes the key)  \xc2\xb7  double-click to edit  \xc2\xb7  right-click for more"
              : "Drag a table to move it  \xc2\xb7  drag from the + handle to the table it refers to  \xc2\xb7  double-click to edit  \xc2\xb7  right-click for more";
        int hh = platform_text_height(win, TH_FONT_SMALL, FONT_UI);
        ui_text_clipped(win, hint, v->canvas.x + 14, v->canvas.y + v->canvas.h - hh - 10, v->canvas.w - 28, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    }
    // messaggio (esito dell'ultima azione)
    if (v->msg[0] && platform_time_ms() < v->msg_until) {
        int mw = SV_MIN(platform_measure_text_ex(win, v->msg, -1, TH_FONT_UI, FONT_UI) + 30, v->canvas.w - 40);
        Rect m = { v->canvas.x + (v->canvas.w - mw) / 2, v->canvas.y + 12, mw, 34 };
        ui_round_box(win, m, v->msg_err ? TH_ERROR_SOFT : TH_SUCCESS_SOFT, v->msg_err ? TH_ERROR : TH_SUCCESS, 8);
        ui_text_clipped(win, v->msg, m.x + 15, ui_text_y(win, m, TH_FONT_UI, FONT_UI), m.w - 30,
                        v->msg_err ? TH_ERROR_TEXT : TH_TEXT, TH_FONT_UI, FONT_UI);
    }
    platform_reset_clip(win);
    draw_bar(v);
    draw_menu(v);
    draw_db_menu(v);
    draw_newdb(v);
    draw_export(v);
    draw_confirm(v);
}

// ── azioni ──────────────────────────────────────────────────────

static SchemaViewAction exec(SchemaView* v, char* sql, const char* what) {
    if (!sql) return SCV_REDRAW;
    free(v->sql);
    v->sql = sql;
    snprintf(v->exec_what, sizeof(v->exec_what), "%.199s", what);
    return SCV_EXEC;
}

void schema_view_exec_done(SchemaView* v, bool ok, const char* message) {
    if (!v) return;
    if (ok) {
        set_msg(v, v->exec_what, false);
        if (v->pending_db[0]) {
            snprintf(v->db, sizeof(v->db), "%s", v->pending_db);
            v->need_fit = true;
            v->sel_table = v->sel_rel = -1;
        }
    } else set_msg(v, message && message[0] ? message : "The database refused the change", true);
    v->pending_db[0] = '\0';
    v->invalid = true;
}

static SchemaViewAction new_table_at(SchemaView* v, float wx, float wy) {
    v->pending_x = floorf(wx / GRID) * GRID;
    v->pending_y = floorf(wy / GRID) * GRID;
    v->pending_place = true;
    return SCV_NEW_TABLE;
}

static SchemaViewAction new_table_default(SchemaView* v) {
    // in alto a sinistra della parte visibile, su uno spazio libero
    float x = WX(v, v->canvas.x + 60), y = WY(v, v->canvas.y + 60);
    for (int tries = 0; tries < 40; tries++) {
        bool hit = false;
        for (int i = 0; i < v->m.nt && !hit; i++) {
            const SmTable* t = &v->m.t[i];
            hit = x < t->x + t->w + 30 && t->x < x + 200 && y < t->y + t->h + 30 && t->y < y + 140;
        }
        if (!hit) break;
        x += 230;
        if (x > WX(v, v->canvas.x + v->canvas.w - 200)) { x = WX(v, v->canvas.x + 60); y += 170; }
    }
    return new_table_at(v, x, y);
}

static void confirm_drop_table(SchemaView* v, int i) {
    v->confirm = CONF_DROP_TABLE;
    v->confirm_target = i;
    int refs = 0;
    for (int k = 0; k < v->m.nr; k++) refs += v->m.r[k].parent == i && v->m.r[k].child != i;
    snprintf(v->confirm_text[0], sizeof(v->confirm_text[0]), "Drop table %.60s?", v->m.t[i].name);
    if (refs) snprintf(v->confirm_text[1], sizeof(v->confirm_text[1]), "Its rows are deleted and %d relationship%s pointing to it removed.", refs, refs == 1 ? "" : "s");
    else snprintf(v->confirm_text[1], sizeof(v->confirm_text[1]), "The table and all its rows are deleted.");
}

static void confirm_drop_rel(SchemaView* v, int k) {
    v->confirm = CONF_DROP_REL;
    v->confirm_target = k;
    const SmRel* r = &v->m.r[k];
    snprintf(v->confirm_text[0], sizeof(v->confirm_text[0]), "Delete relationship %.60s?", r->name);
    // colonne che possono andare via con la relazione
    char cols[120] = "";
    size_t n = 0;
    for (int i = 0; i < r->ncols && n < sizeof(cols) - 1; i++)
        if (sm_droppable_column(&v->m, k, i))
            n += (size_t)snprintf(cols + n, sizeof(cols) - n, "%s%.40s", n ? ", " : "", r->cols[i]);
    v->conf_cols = false;
    v->conf_cols_text[0] = '\0';
    if (n) snprintf(v->conf_cols_text, sizeof(v->conf_cols_text), "Also drop the column%s %.100s and its data",
                    strchr(cols, ',') ? "s" : "", cols);
    snprintf(v->confirm_text[1], sizeof(v->confirm_text[1]), "The foreign key of %.60s is removed.", v->m.t[r->child].name);
}

static void open_menu(SchemaView* v, int kind, int target, int x, int y) {
    v->menu_kind = kind;
    v->menu_target = target;
    v->menu_n = 0;
    v->menu_hover = -1;
    v->menu_wx = WX(v, x);
    v->menu_wy = WY(v, y);
    if (kind == MENU_TABLE) {
        int items[] = { MI_EDIT, MI_BROWSE, MI_LINK, MI_LINK_NM, MI_DROP_TABLE };
        memcpy(v->menu_items, items, sizeof(items)); v->menu_n = 5;
    } else if (kind == MENU_REL) {
        v->menu_items[v->menu_n++] = v->m.r[target].optional ? MI_REQUIRED : MI_OPTIONAL;
        v->menu_items[v->menu_n++] = MI_DROP_REL;
    } else {
        int items[] = { MI_NEW_HERE, MI_ARRANGE, MI_FIT };
        memcpy(v->menu_items, items, sizeof(items)); v->menu_n = 3;
    }
    int w = 60;
    for (int i = 0; i < v->menu_n; i++) w = SV_MAX(w, platform_measure_text_ex(v->win, menu_label(v->menu_items[i]), -1, TH_FONT_UI, FONT_UI) + 30);
    Rect m = { x, y, w, v->menu_n * 28 + 8 };
    if (m.x + m.w > v->area.x + v->area.w - 4) m.x = x - m.w;
    if (m.y + m.h > v->area.y + v->area.h - 4) m.y = SV_MAX(v->area.y, y - m.h);
    v->menu_rect = m;
}

static SchemaViewAction run_menu(SchemaView* v, int id) {
    int t = v->menu_target;
    v->menu_kind = MENU_NONE;
    switch (id) {
        case MI_EDIT: case MI_BROWSE:
            snprintf(v->table, sizeof(v->table), "%s", v->m.t[t].name);
            return id == MI_EDIT ? SCV_EDIT_TABLE : SCV_BROWSE_TABLE;
        case MI_LINK: case MI_LINK_NM:
            v->drag = DRAG_LINK; v->link_src = t; v->link_col = -1; v->link_target = -1; v->link_sticky = true;
            v->link_nm = id == MI_LINK_NM;
            return SCV_REDRAW;
        case MI_DROP_TABLE: confirm_drop_table(v, t); return SCV_REDRAW;
        case MI_REQUIRED: case MI_OPTIONAL:
            return exec(v, sm_relation_required_sql(&v->m, t, id == MI_REQUIRED),
                        id == MI_REQUIRED ? "Relationship is now mandatory" : "Relationship is now optional");
        case MI_DROP_REL: confirm_drop_rel(v, t); return SCV_REDRAW;
        case MI_NEW_HERE: return new_table_at(v, v->menu_wx, v->menu_wy);
        case MI_ARRANGE:
            sm_auto_layout(&v->m, true, GAP_X(v), GAP_Y(v)); save_positions(v); fit(v); return SCV_REDRAW;
        case MI_FIT: fit(v); return SCV_REDRAW;
        default: return SCV_REDRAW;
    }
}

static SchemaViewAction finish_link(SchemaView* v, int target) {
    int src = v->link_src, col = v->link_col;
    v->drag = DRAG_NONE;
    v->link_src = -1;
    v->link_sticky = false;
    bool nm = v->link_nm;
    v->link_nm = false;
    if (target < 0) return SCV_REDRAW;
    char err[300];
    if (nm) {
        char link[64];
        char* sql = sm_many_to_many_sql(&v->m, src, target, link, sizeof(link), err, sizeof(err));
        if (!sql) { set_msg(v, err, true); return SCV_REDRAW; }
        // la tabella di collegamento va tra le due
        const SmTable *a = &v->m.t[src], *b = &v->m.t[target];
        snprintf(v->pending_table, sizeof(v->pending_table), "%s", link);
        float acx = (float)a->x + (float)a->w / 2, acy = (float)a->y + (float)a->h / 2;
        float bcx = (float)b->x + (float)b->w / 2, bcy = (float)b->y + (float)b->h / 2;
        if (fabsf(acx - bcx) >= fabsf(acy - bcy)) {    // affiancate: in mezzo
            v->pending_x = (acx + bcx) / 2 - 80;
            v->pending_y = (acy + bcy) / 2 - 20;
        } else {                                       // una sopra l'altra: a destra, a metà altezza
            v->pending_x = (float)SV_MAX(a->x + a->w, b->x + b->w) + 140;
            v->pending_y = (acy + bcy) / 2 - 20;
        }
        v->pending_x = floorf(v->pending_x / GRID) * GRID;
        v->pending_y = floorf(v->pending_y / GRID) * GRID;
        if (src == target) { v->pending_x = (float)(a->x + a->w + 120); v->pending_y = (float)a->y; }
        v->pending_place = true;
        char what[200];
        snprintf(what, sizeof(what), "Many-to-many: link table %.60s created", link);
        v->sel_table = -1;
        return exec(v, sql, what);
    }
    const char* cname = col >= 0 && col < v->m.t[src].ncols ? v->m.t[src].cols[col].name : "";
    char* sql = sm_relation_sql(&v->m, src, cname, target, err, sizeof(err));
    if (!sql) { set_msg(v, err, true); return SCV_REDRAW; }
    char what[160];
    snprintf(what, sizeof(what), "Relationship created: %.60s \xe2\x86\x92 %.60s", v->m.t[src].name, v->m.t[target].name);
    v->sel_table = -1;
    return exec(v, sql, what);
}

static void set_zoom(SchemaView* v, float z, int ax, int ay) {
    z = fmaxf(ZOOM_MIN, fminf(ZOOM_MAX, z));
    v->view_dirty = true;
    v->view_changed = platform_time_ms();
    float wx = WX(v, ax), wy = WY(v, ay);
    v->zoom = z;
    v->pan_x = wx - (float)(ax - v->canvas.x) / z;
    v->pan_y = wy - (float)(ay - v->canvas.y) / z;
}

static bool valid_ident(const char* s) {
    if (!s[0] || strlen(s) > 63 || isdigit((unsigned char)s[0])) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return false;
    return true;
}

static SchemaViewAction create_db(SchemaView* v) {
    const char* name = v->newdb->text;
    if (!valid_ident(name)) { set_msg(v, "Database name: letters, digits and _ (not starting with a digit)", true); return SCV_REDRAW; }
    for (int i = 0; i < v->ndbs; i++)
        if (!strcasecmp(v->dbs[i], name)) { set_msg(v, "A database with this name already exists", true); return SCV_REDRAW; }
    size_t n = strlen(name) + 32;
    char* sql = malloc(n);
    if (!sql) return SCV_REDRAW;
    snprintf(sql, n, "CREATE DATABASE %s", name);
    snprintf(v->pending_db, sizeof(v->pending_db), "%s", name);
    v->newdb_open = false;
    line_edit_focus(v->newdb, false);
    char what[120];
    snprintf(what, sizeof(what), "Database %.60s created: add its first table", name);
    return exec(v, sql, what);
}

static void open_newdb(SchemaView* v) {
    v->db_menu = false;
    v->newdb_open = true;
    line_edit_set_text(v->newdb, "");
    line_edit_focus(v->newdb, true);
}

static void update_hover(SchemaView* v) {
    v->hover_table = table_at(v, v->mx, v->my);
    v->hover_handle = -2;
    // i pallini sporgono dal bordo: cercati anche appena fuori dalla tabella
    int cand[2] = { v->hover_table, v->sel_table };
    for (int k = 0; k < 2 && v->hover_handle == -2; k++) {
        if (cand[k] < 0 || cand[k] >= v->m.nt) continue;
        int h = handle_at(v, cand[k], v->mx, v->my);
        if (h != -2) { v->hover_table = cand[k]; v->hover_handle = h; }
    }
    if (v->hover_handle == -2 && v->hover_table < 0) {
        for (int i = v->m.nt - 1; i >= 0 && v->hover_handle == -2; i--) {
            int h = handle_at(v, i, v->mx, v->my);
            if (h != -2) { v->hover_table = i; v->hover_handle = h; }
        }
    }
    v->hover_rel = v->hover_table < 0 ? rel_at(v, v->mx, v->my) : -1;
}

SchemaViewAction schema_view_event(SchemaView* v, sEvent* evt) {
    if (!v) return SCV_NONE;
    if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_UP) { v->mx = evt->mouse_x; v->my = evt->mouse_y; }

    // ── finestra di conferma ──
    if (v->confirm != CONF_NONE) {
        bool ok = false, cancel = false;
        if (evt->type == EVT_KEY_DOWN) { ok = evt->key == KEY_ENTER; cancel = evt->key == KEY_ESCAPE; }
        if (evt->type == EVT_MOUSE_DOWN && evt->mouse_button == SV_MOUSE_LEFT) {
            ok = in_rect(v->conf_ok, v->mx, v->my);
            cancel = in_rect(v->conf_cancel, v->mx, v->my) || !in_rect(v->conf_rect, v->mx, v->my);
        }
        if (evt->type == EVT_MOUSE_DOWN && in_rect(v->conf_check, v->mx, v->my)) { v->conf_cols = !v->conf_cols; return SCV_REDRAW; }
        if (cancel) { v->confirm = CONF_NONE; return SCV_REDRAW; }
        if (ok) {
            int kind = v->confirm, t = v->confirm_target;
            v->confirm = CONF_NONE;
            char what[120];
            if (kind == CONF_DROP_TABLE && t < v->m.nt) {
                snprintf(what, sizeof(what), "Table %.60s dropped", v->m.t[t].name);
                v->sel_table = -1;
                return exec(v, sm_drop_table_sql(&v->m, t), what);
            }
            if (kind == CONF_DROP_REL && t < v->m.nr) {
                snprintf(what, sizeof(what), "Relationship %.60s deleted", v->m.r[t].name);
                v->sel_rel = -1;
                return exec(v, sm_drop_relation_sql(&v->m, t, v->conf_cols), what);
            }
        }
        return SCV_REDRAW;
    }

    // ── esportazione ──
    if (v->exp_open) {
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ESCAPE) { v->exp_open = false; line_edit_focus(v->exp_path, false); return SCV_REDRAW; }
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ENTER) { do_export(v); return SCV_REDRAW; }
        if (evt->type == EVT_MOUSE_DOWN) {
            if (in_rect(v->exp_ok, v->mx, v->my)) { do_export(v); return SCV_REDRAW; }
            if (in_rect(v->exp_s1, v->mx, v->my)) { v->exp_scale = 1; return SCV_REDRAW; }
            if (in_rect(v->exp_s2, v->mx, v->my)) { v->exp_scale = 2; return SCV_REDRAW; }
            if (in_rect(v->exp_l0, v->mx, v->my)) { v->exp_light = false; return SCV_REDRAW; }
            if (in_rect(v->exp_l1, v->mx, v->my)) { v->exp_light = true; return SCV_REDRAW; }
            if (!in_rect(v->exp_rect, v->mx, v->my)) { v->exp_open = false; line_edit_focus(v->exp_path, false); }
            else widget_handle_event((Widget*)v->exp_path, evt);
            return SCV_REDRAW;
        }
        if (evt->type == EVT_CHAR || evt->type == EVT_KEY_DOWN || evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_UP)
            widget_handle_event((Widget*)v->exp_path, evt);
        return SCV_REDRAW;
    }

    // ── nuovo database ──
    if (v->newdb_open) {
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ESCAPE) { v->newdb_open = false; line_edit_focus(v->newdb, false); return SCV_REDRAW; }
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ENTER) return create_db(v);
        if (evt->type == EVT_MOUSE_DOWN) {
            if (in_rect(v->newdb_ok, v->mx, v->my)) return create_db(v);
            if (!in_rect(v->newdb_rect, v->mx, v->my)) { v->newdb_open = false; line_edit_focus(v->newdb, false); }
            else widget_handle_event((Widget*)v->newdb, evt);
            return SCV_REDRAW;
        }
        if (evt->type == EVT_CHAR || evt->type == EVT_KEY_DOWN || evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_UP)
            widget_handle_event((Widget*)v->newdb, evt);
        return SCV_REDRAW;
    }

    // ── menu dei database ──
    if (v->db_menu) {
        int n = v->ndbs + 1, hit = -1;
        for (int i = 0; i < n; i++) {
            Rect it = { v->db_menu_rect.x + 4, v->db_menu_rect.y + 4 + i * 28 + (i == v->ndbs ? 9 : 0), v->db_menu_rect.w - 8, 28 };
            if (in_rect(it, v->mx, v->my)) hit = i;
        }
        if (evt->type == EVT_MOUSE_MOVE) { v->db_hover = hit; return SCV_REDRAW; }
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ESCAPE) { v->db_menu = false; return SCV_REDRAW; }
        if (evt->type == EVT_MOUSE_DOWN) {
            v->db_menu = false;
            if (hit == v->ndbs) open_newdb(v);
            else if (hit >= 0) schema_view_show_db(v, v->dbs[hit], NULL);
            return SCV_REDRAW;
        }
        return evt->type == EVT_MOUSE_UP ? SCV_NONE : SCV_REDRAW;
    }

    // ── menu contestuale ──
    if (v->menu_kind != MENU_NONE) {
        int hit = -1;
        for (int i = 0; i < v->menu_n; i++)
            if (in_rect((Rect){v->menu_rect.x + 4, v->menu_rect.y + 4 + i * 28, v->menu_rect.w - 8, 28}, v->mx, v->my)) hit = i;
        if (evt->type == EVT_MOUSE_MOVE) { v->menu_hover = hit; return SCV_REDRAW; }
        if (evt->type == EVT_KEY_DOWN) {
            if (evt->key == KEY_ESCAPE) v->menu_kind = MENU_NONE;
            else if (evt->key == KEY_DOWN) v->menu_hover = (v->menu_hover + 1) % v->menu_n;
            else if (evt->key == KEY_UP) v->menu_hover = (v->menu_hover - 1 + v->menu_n) % v->menu_n;
            else if (evt->key == KEY_ENTER && v->menu_hover >= 0) return run_menu(v, v->menu_items[v->menu_hover]);
            return SCV_REDRAW;
        }
        if (evt->type == EVT_MOUSE_DOWN) {
            if (hit >= 0) return run_menu(v, v->menu_items[hit]);
            v->menu_kind = MENU_NONE;
            if (evt->mouse_button != SV_MOUSE_RIGHT) return SCV_REDRAW;   // un altro tasto destro riapre il menu qui
        } else return SCV_REDRAW;
    }

    switch (evt->type) {
        case EVT_MOUSE_MOVE: {
            if (v->drag == DRAG_MOVE && v->drag_table >= 0 && v->drag_table < v->m.nt) {
                SmTable* t = &v->m.t[v->drag_table];
                float x = WX(v, v->mx) - (float)v->drag_dx, y = WY(v, v->my) - (float)v->drag_dy;
                t->x = (int)(roundf(x / 10) * 10);
                t->y = (int)(roundf(y / 10) * 10);
                if (abs(v->mx - v->drag_sx) + abs(v->my - v->drag_sy) > 3) v->drag_moved = true;
                platform_set_cursor(v->win, CURSOR_HAND);
                return SCV_REDRAW;
            }
            if (v->drag == DRAG_PAN) {
                v->pan_x = v->pan0x - (float)(v->mx - v->drag_sx) / v->zoom;
                v->pan_y = v->pan0y - (float)(v->my - v->drag_sy) / v->zoom;
                return SCV_REDRAW;
            }
            if (v->drag == DRAG_LINK) {
                v->link_target = table_at(v, v->mx, v->my);
                return SCV_REDRAW;
            }
            update_hover(v);
            bool clickable = v->hover_table >= 0 || v->hover_rel >= 0;
            Rect btns[] = { v->db_btn, v->b_table, v->b_newdb, v->b_er, v->b_uml, v->b_zout, v->b_zin, v->b_fit, v->b_arrange, v->b_empty };
            for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) clickable = clickable || in_rect(btns[i], v->mx, v->my);
            platform_set_cursor(v->win, clickable ? CURSOR_HAND : CURSOR_ARROW);
            return SCV_REDRAW;
        }
        case EVT_MOUSE_UP: {
            int d = v->drag;
            if (d == DRAG_MOVE) {
                v->drag = DRAG_NONE;
                if (v->drag_moved) save_positions(v);
                return SCV_REDRAW;
            }
            if (d == DRAG_PAN) { v->drag = DRAG_NONE; save_positions(v); return SCV_REDRAW; }
            if (d == DRAG_LINK && !v->link_sticky) {
                int t = table_at(v, v->mx, v->my);
                // rilasciato sulla stessa tabella senza muoversi: resta in attesa del click sulla destinazione
                if (t == v->link_src && abs(v->mx - v->drag_sx) + abs(v->my - v->drag_sy) < 6) { v->link_sticky = true; return SCV_REDRAW; }
                if (evt->modifiers & SV_MOD_SHIFT) v->link_nm = true;
                return finish_link(v, t);
            }
            return SCV_NONE;
        }
        case EVT_MOUSE_WHEEL: {
            if (!in_rect(v->canvas, evt->mouse_x, evt->mouse_y)) return SCV_NONE;
            v->view_dirty = true;
            v->view_changed = platform_time_ms();
            if (evt->modifiers & SV_MOD_CTRL) { set_zoom(v, v->zoom * (evt->wheel_delta > 0 ? 1.1f : 1 / 1.1f), evt->mouse_x, evt->mouse_y); return SCV_REDRAW; }
            float step = 60 / v->zoom * (float)evt->wheel_delta;
            if (evt->modifiers & SV_MOD_SHIFT) v->pan_x -= step;
            else v->pan_y -= step;
            return SCV_REDRAW;
        }
        case EVT_KEY_DOWN: {
            if (evt->key == KEY_ESCAPE) {
                if (v->drag == DRAG_LINK) { v->drag = DRAG_NONE; v->link_src = -1; v->link_sticky = false; v->link_nm = false; }
                else { v->sel_table = v->sel_rel = -1; }
                return SCV_REDRAW;
            }
            if (evt->key == KEY_DELETE || evt->key == KEY_BACKSPACE) {
                if (v->sel_table >= 0 && v->sel_table < v->m.nt) confirm_drop_table(v, v->sel_table);
                else if (v->sel_rel >= 0 && v->sel_rel < v->m.nr) confirm_drop_rel(v, v->sel_rel);
                return SCV_REDRAW;
            }
            if ((evt->key == KEY_ENTER || evt->key == KEY_F2) && v->sel_table >= 0 && v->sel_table < v->m.nt) {
                snprintf(v->table, sizeof(v->table), "%s", v->m.t[v->sel_table].name);
                return SCV_EDIT_TABLE;
            }
            if (evt->key == KEY_LETTER && (evt->modifiers & SV_MOD_CTRL)) {
                int cx = v->canvas.x + v->canvas.w / 2, cy = v->canvas.y + v->canvas.h / 2;
                if (evt->character == '=' || evt->character == '+') { set_zoom(v, v->zoom * 1.2f, cx, cy); return SCV_REDRAW; }
                if (evt->character == '-') { set_zoom(v, v->zoom / 1.2f, cx, cy); return SCV_REDRAW; }
                if (evt->character == '0') { set_zoom(v, 1.0f, cx, cy); return SCV_REDRAW; }
                return SCV_NONE;
            }
            int dx = evt->key == KEY_LEFT ? -1 : evt->key == KEY_RIGHT ? 1 : 0, dy = evt->key == KEY_UP ? -1 : evt->key == KEY_DOWN ? 1 : 0;
            if ((dx || dy) && v->sel_table >= 0 && v->sel_table < v->m.nt) {
                int s = (evt->modifiers & SV_MOD_SHIFT) ? 50 : 10;
                v->m.t[v->sel_table].x += dx * s;
                v->m.t[v->sel_table].y += dy * s;
                save_positions(v);
                return SCV_REDRAW;
            }
            if (dx || dy) { v->pan_x += (float)(dx * 60) / v->zoom; v->pan_y += (float)(dy * 60) / v->zoom; return SCV_REDRAW; }
            return SCV_NONE;
        }
        case EVT_MOUSE_DOWN:
            break;
        default:
            return SCV_NONE;
    }

    // ── click ──
    int x = v->mx, y = v->my;
    if (evt->mouse_button == SV_MOUSE_LEFT && y < v->canvas.y) {
        if (in_rect(v->db_btn, x, y)) { v->db_menu = !v->db_menu; v->db_hover = -1; return SCV_REDRAW; }
        if (in_rect(v->b_table, x, y)) return new_table_default(v);
        if (in_rect(v->b_newdb, x, y)) { open_newdb(v); return SCV_REDRAW; }
        if (in_rect(v->b_export, x, y)) { open_export(v); return SCV_REDRAW; }
        if (in_rect(v->b_er, x, y) || in_rect(v->b_uml, x, y)) {
            int n = in_rect(v->b_er, x, y) ? NOT_ER : NOT_UML;
            if (n != v->notation) { v->notation = n; v->sizes_dirty = true; save_positions(v); }
            return SCV_REDRAW;
        }
        int cx = v->canvas.x + v->canvas.w / 2, cy = v->canvas.y + v->canvas.h / 2;
        if (in_rect(v->b_zin, x, y)) { set_zoom(v, v->zoom * 1.2f, cx, cy); return SCV_REDRAW; }
        if (in_rect(v->b_zout, x, y)) { set_zoom(v, v->zoom / 1.2f, cx, cy); return SCV_REDRAW; }
        if (in_rect(v->b_zlabel, x, y)) { set_zoom(v, 1.0f, cx, cy); return SCV_REDRAW; }
        if (in_rect(v->b_fit, x, y)) { fit(v); return SCV_REDRAW; }
        if (in_rect(v->b_arrange, x, y) && v->loaded) { sm_auto_layout(&v->m, true, GAP_X(v), GAP_Y(v)); save_positions(v); fit(v); return SCV_REDRAW; }
        return SCV_REDRAW;
    }
    if (!in_rect(v->canvas, x, y)) return SCV_NONE;
    if (in_rect(v->b_empty, x, y) && evt->mouse_button == SV_MOUSE_LEFT) {
        if (!v->ndbs) { open_newdb(v); return SCV_REDRAW; }
        if (v->loaded) return new_table_default(v);
        return SCV_REDRAW;
    }
    if (evt->mouse_button == SV_MOUSE_MIDDLE) {
        v->drag = DRAG_PAN; v->drag_sx = x; v->drag_sy = y; v->pan0x = v->pan_x; v->pan0y = v->pan_y;
        return SCV_REDRAW;
    }
    if (!v->loaded) return SCV_REDRAW;
    update_hover(v);
    int t = v->hover_table, k = v->hover_rel;

    if (v->drag == DRAG_LINK && v->link_sticky) {
        if (evt->modifiers & SV_MOD_SHIFT) v->link_nm = true;
        return finish_link(v, t);
    }

    if (evt->mouse_button == SV_MOUSE_RIGHT) {
        if (t >= 0) { v->sel_table = t; v->sel_rel = -1; open_menu(v, MENU_TABLE, t, x, y); }
        else if (k >= 0) { v->sel_rel = k; v->sel_table = -1; open_menu(v, MENU_REL, k, x, y); }
        else open_menu(v, MENU_CANVAS, -1, x, y);
        return SCV_REDRAW;
    }
    if (evt->mouse_button != SV_MOUSE_LEFT) return SCV_REDRAW;

    uint64_t now = platform_time_ms();
    int target = t >= 0 ? t : k >= 0 ? 1000 + k : -1;
    bool dbl = target == v->last_click_target && now - v->last_click < 400 && abs(x - v->last_click_x) < 6 && abs(y - v->last_click_y) < 6;
    v->last_click = dbl ? 0 : now;
    v->last_click_target = target;
    v->last_click_x = x;
    v->last_click_y = y;

    if (t >= 0 && v->hover_handle != -2) {         // relazione: dal pallino a un'altra tabella
        v->drag = DRAG_LINK; v->link_src = t; v->link_col = v->hover_handle; v->link_target = -1; v->link_sticky = false; v->link_nm = false;
        v->drag_sx = x; v->drag_sy = y;
        return SCV_REDRAW;
    }
    if (t >= 0) {
        v->sel_table = t; v->sel_rel = -1;
        if (dbl) { snprintf(v->table, sizeof(v->table), "%s", v->m.t[t].name); return SCV_EDIT_TABLE; }
        // in primo piano: in fondo all'array (disegnata per ultima)
        if (t != v->m.nt - 1) {
            SmTable tmp = v->m.t[t];
            int ew = v->entw[t];
            memmove(&v->m.t[t], &v->m.t[t + 1], sizeof(SmTable) * (size_t)(v->m.nt - t - 1));
            memmove(&v->entw[t], &v->entw[t + 1], sizeof(int) * (size_t)(v->m.nt - t - 1));
            v->m.t[v->m.nt - 1] = tmp;
            v->entw[v->m.nt - 1] = ew;
            for (int r = 0; r < v->m.nr; r++) {
                SmRel* q = &v->m.r[r];
                q->child = q->child == t ? v->m.nt - 1 : q->child > t ? q->child - 1 : q->child;
                q->parent = q->parent == t ? v->m.nt - 1 : q->parent > t ? q->parent - 1 : q->parent;
            }
            t = v->m.nt - 1;
            v->sel_table = t;
            v->last_click_target = t;
        }
        v->drag = DRAG_MOVE; v->drag_table = t; v->drag_moved = false;
        v->drag_dx = (int)(WX(v, x) - (float)v->m.t[t].x);
        v->drag_dy = (int)(WY(v, y) - (float)v->m.t[t].y);
        v->drag_sx = x; v->drag_sy = y;
        return SCV_REDRAW;
    }
    if (k >= 0) { v->sel_rel = k; v->sel_table = -1; return SCV_REDRAW; }
    v->sel_table = v->sel_rel = -1;
    if (dbl) return new_table_at(v, WX(v, x), WY(v, y));
    v->drag = DRAG_PAN; v->drag_sx = x; v->drag_sy = y; v->pan0x = v->pan_x; v->pan0y = v->pan_y;
    return SCV_REDRAW;
}

bool schema_view_tick(SchemaView* v, uint64_t now_ms) {
    if (!v) return false;
    bool r = false;
    if (v->newdb_open) r = line_edit_tick(v->newdb, now_ms);
    if (v->exp_open) r = line_edit_tick(v->exp_path, now_ms) || r;
    if (v->msg[0] && now_ms >= v->msg_until) { v->msg[0] = '\0'; r = true; }
    if (v->view_dirty && now_ms - v->view_changed > 800) save_positions(v);
    return r;
}
