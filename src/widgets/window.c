#include "window.h"
#include "../platform/platform.h"
#include "label.h"
#include "theme.h"
#include "sql_text.h"
#include <stdio.h>
#include <string.h>

#define MAX_STATEMENTS 512

static void refresh_tree(AppWindow* app);
static void update_session(AppWindow* app);

// ── stato / status bar ───────────────────────────────────────────

static void set_status(AppWindow* app, StatusKind kind, const char* text, const char* meta) {
    app->status_kind = kind;
    label_set_text(app->lbl_status, text ? text : "");
    Color c = TH_TEXT_MUTED;
    if (kind == STATUS_OK)    c = TH_TEXT;
    if (kind == STATUS_ERROR) c = TH_ERROR;
    label_set_color(app->lbl_status, c);
    snprintf(app->status_meta, sizeof(app->status_meta), "%s", meta ? meta : "");
}

static void update_session(AppWindow* app) {
    const char* db = db_current_database();
    label_set_text(app->lbl_db_name, db && db[0] ? db : "no database");
    SessionInfo* s = db_session_info();
    app->in_transaction = s ? s->in_transaction : false;
    if (s) db_session_free(s);
}

static void show_result(AppWindow* app, QueryResult* r) {
    // il vecchio risultato si libera solo dopo aver aggiornato la griglia
    QueryResult* old = app->last_result;
    app->last_result = r;
    table_view_set_result(app->results, r);
    if (old && old != r) db_result_free(old);
}

// ── callbacks dei bottoni ────────────────────────────────────────

static void on_run_click(void* ud)         { app_window_run((AppWindow*)ud, RUN_SCRIPT); }
static void on_run_current_click(void* ud) { app_window_run((AppWindow*)ud, RUN_CURRENT); }

static void on_commit_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    bool ok = db_commit();
    update_session(app);
    set_status(app, ok ? STATUS_OK : STATUS_ERROR, ok ? "COMMIT executed" : "COMMIT failed", "");
}

static void on_rollback_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    bool ok = db_rollback();
    update_session(app);
    refresh_tree(app);
    set_status(app, ok ? STATUS_OK : STATUS_ERROR, ok ? "ROLLBACK executed" : "ROLLBACK failed", "");
}

static void on_refresh_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    refresh_tree(app);
    set_status(app, STATUS_INFO, "Explorer refreshed", "");
}

// callback dell'editor: Ctrl+Enter esegue lo statement corrente (o la selezione)
static void on_editor_execute(const char* text, void* ud) {
    (void)text;
    app_window_run((AppWindow*)ud, RUN_CURRENT);
}

// ── scrollbar → widget ───────────────────────────────────────────

static void on_editor_scroll(int pos, void* ud)    { ((AppWindow*)ud)->editor->scroll_y = pos; }
static void on_tree_scroll(int pos, void* ud)      { ((AppWindow*)ud)->tree->scroll_y = pos; }
static void on_results_scroll_v(int pos, void* ud) { ((AppWindow*)ud)->results->scroll_y = pos; }
static void on_results_scroll_h(int pos, void* ud) { ((AppWindow*)ud)->results->scroll_x = pos; }

// widget → scrollbar (da chiamare prima di disegnare e di gestire eventi)
static void sync_scrollbars(AppWindow* app) {
    TextBox* tb = app->editor;
    scrollbar_set_range(app->editor_scroll, tb->num_lines + 1, textbox_visible_lines(tb));
    scrollbar_set_position(app->editor_scroll, tb->scroll_y);

    tree_view_clamp_scroll(app->tree);
    scrollbar_set_range(app->tree_scroll, tree_view_row_count(app->tree), tree_view_visible_rows(app->tree));
    scrollbar_set_position(app->tree_scroll, app->tree->scroll_y);

    TableView* tv = app->results;
    table_view_clamp_scroll(tv);
    bool grid = tv->msg_kind == TABLE_MSG_NONE && tv->result && tv->result->success && tv->result->num_cols > 0;
    scrollbar_set_range(app->results_scroll_v, grid ? tv->result->num_rows : 0, table_view_visible_rows(tv));
    scrollbar_set_position(app->results_scroll_v, tv->scroll_y);
    scrollbar_set_range(app->results_scroll_h, grid ? table_view_content_width(tv) : 0, table_view_viewport_width(tv));
    scrollbar_set_position(app->results_scroll_h, tv->scroll_x);
}

// ── sidebar ──────────────────────────────────────────────────────

// risale l'albero fino a trovare il database genitore
static const char* find_db_name(TreeNode* node) {
    while (node) {
        if (node->type == TREE_NODE_DATABASE) return node->label;
        node = node->parent;
    }
    return NULL;
}

static void load_db_children(TreeNode* db_node) {
    const char* db_name = db_node->label;
    NameList* tables = db_list_tables(db_name);
    if (tables) {
        for (int i = 0; i < tables->count; i++)
            tree_node_add_child(db_node, TREE_NODE_TABLE, tables->names[i], NULL);
        db_namelist_free(tables);
    }
    NameList* views = db_list_views(db_name);
    if (views) {
        for (int i = 0; i < views->count; i++)
            tree_node_add_child(db_node, TREE_NODE_VIEW, views->names[i], NULL);
        db_namelist_free(views);
    }
    db_node->loaded = true;
}

static void load_table_children(TreeNode* tbl_node) {
    const char* db_name = find_db_name(tbl_node);
    if (!db_name) return;
    int ncols = 0;
    ColumnInfo* cols = db_list_columns(db_name, tbl_node->label, &ncols);
    if (!cols) return;
    for (int i = 0; i < ncols; i++) {
        char extra[64];
        snprintf(extra, sizeof(extra), "%s%s", cols[i].type, cols[i].primary_key ? " PK" : "");
        tree_node_add_child(tbl_node, TREE_NODE_COLUMN, cols[i].name, extra);
    }
    db_columns_free(cols, ncols);
    tbl_node->loaded = true;
}

// ricarica l'albero mantenendo espansi i nodi che lo erano
static void refresh_tree(AppWindow* app) {
    enum { MAXN = 512 };
    char (*names)[192] = calloc(MAXN, 192);
    int n = 0;
    TreeNode* root = app->tree->root;
    if (names) {
        for (int i = 0; i < root->num_children && n < MAXN; i++) {
            TreeNode* db = &root->children[i];
            if (!db->expanded) continue;
            snprintf(names[n++], 192, "%.120s", db->label);
            for (int j = 0; j < db->num_children && n < MAXN; j++)
                if (db->children[j].expanded)
                    snprintf(names[n++], 192, "%.60s/%.120s", db->label, db->children[j].label);
        }
    }

    int old_scroll = app->tree->scroll_y;
    tree_view_refresh(app->tree);
    app->tree->scroll_y = old_scroll;

    if (!names) return;
    root = app->tree->root;
    for (int i = 0; i < root->num_children; i++) {
        TreeNode* db = &root->children[i];
        bool found = false;
        for (int k = 0; k < n && !found; k++) found = strcmp(names[k], db->label) == 0;
        if (!found) continue;
        db->expanded = true;
        load_db_children(db);
        for (int j = 0; j < db->num_children; j++) {
            char key[192];
            snprintf(key, sizeof(key), "%.60s/%.120s", db->label, db->children[j].label);
            for (int k = 0; k < n; k++)
                if (strcmp(names[k], key) == 0) {
                    db->children[j].expanded = true;
                    load_table_children(&db->children[j]);
                    break;
                }
        }
    }
    free(names);
}

// click su un nodo: caricamento lazy dei figli quando viene espanso
static void on_tree_select(TreeNode* node, void* ud) {
    (void)ud;
    if (!node || !node->expanded) return;
    if (node->type == TREE_NODE_DATABASE && !node->loaded) {
        load_db_children(node);
    } else if (node->type == TREE_NODE_TABLE || node->type == TREE_NODE_VIEW) {
        // le colonne vengono ricaricate ad ogni espansione (schema sempre aggiornato)
        tree_node_clear_children(node);
        node->expanded = true;
        load_table_children(node);
    }
}

static bool use_database(AppWindow* app, const char* db) {
    const char* cur = db_current_database();
    if (cur && strcmp(cur, db) == 0) return true;
    char sql[256];
    snprintf(sql, sizeof(sql), "USE %s;", db);
    QueryResult* r = db_execute(sql);
    bool ok = r && r->success;
    if (r) db_result_free(r);
    update_session(app);
    return ok;
}

// doppio click: azione rapida sul nodo
static void on_tree_activate(TreeNode* node, void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (!node) return;

    switch (node->type) {
        case TREE_NODE_DATABASE: {
            char msg[256];
            bool ok = use_database(app, node->label);
            snprintf(msg, sizeof(msg), ok ? "Using database %s" : "Cannot use database %s", node->label);
            set_status(app, ok ? STATUS_OK : STATUS_ERROR, msg, "");
            break;
        }
        case TREE_NODE_TABLE:
        case TREE_NODE_VIEW: {
            const char* db = find_db_name(node);
            if (db) use_database(app, db);
            char sql[256];
            snprintf(sql, sizeof(sql), "SELECT * FROM %s LIMIT 200;", node->label);
            QueryResult* r = db_execute(sql);
            char msg[256], meta[128];
            if (r && r->success) {
                show_result(app, r);
                snprintf(msg, sizeof(msg), "Preview of %s", node->label);
                snprintf(meta, sizeof(meta), "%d rows  \xc2\xb7  %d columns  \xc2\xb7  %.1f ms",
                         r->num_rows, r->num_cols, r->exec_time_ms);
                set_status(app, STATUS_OK, msg, meta);
            } else {
                snprintf(msg, sizeof(msg), "Error: %.240s", r ? r->error : "no response");
                table_view_set_message(app->results, TABLE_MSG_ERROR, "Preview failed", r ? r->error : "");
                set_status(app, STATUS_ERROR, msg, "");
                if (r) db_result_free(r);
            }
            break;
        }
        case TREE_NODE_COLUMN:
            // inserisce il nome della colonna nell'editor
            textbox_insert(app->editor, node->label);
            textbox_focus(app->editor, true);
            break;
        default:
            break;
    }
}

// ── layout ───────────────────────────────────────────────────────

// ricalcola la posizione e dimensione di tutti i widget in base alle dimensioni correnti della finestra
static void layout_widgets(AppWindow* app) {
    int W = app->win_w;
    int H = app->win_h;

    // toolbar
    int bh = 32, by = (TOOLBAR_H - bh) / 2;
    int x = SIDEBAR_W + 14;
    Button* btns[] = { app->btn_run, app->btn_run_current, app->btn_commit, app->btn_rollback };
    for (int i = 0; i < 4; i++) {
        int w = button_preferred_width(btns[i], app->win);
        btns[i]->base.bounds = (Rect){x, by, w, bh};
        x += w + (i == 1 ? 22 : 8); // spazio extra tra "esecuzione" e "transazione"
    }
    app->lbl_db_name->base.bounds = (Rect){W - 210, by, 196, bh};

    // sidebar
    int side_top = TOOLBAR_H + SIDEBAR_HEADER_H;
    int side_h   = H - side_top - FOOTER_H;
    app->tree->base.bounds        = (Rect){0, side_top, SIDEBAR_W - 1, side_h};
    app->tree_scroll->base.bounds = (Rect){SIDEBAR_W - 1 - SCROLLBAR_W, side_top, SCROLLBAR_W, side_h};
    app->btn_refresh->base.bounds = (Rect){SIDEBAR_W - 38, TOOLBAR_H + 5, 28, 28};

    // area centrale: divisa dallo splitter
    int min_y = TOOLBAR_H + 90;
    int max_y = H - FOOTER_H - STATUSBAR_H - SPLITTER_H - 90;
    if (max_y < min_y) max_y = min_y;
    app->splitter_y = SV_CLAMP(app->splitter_y, min_y, max_y);

    int center_x  = SIDEBAR_W;
    int center_w  = W - SIDEBAR_W;
    int editor_h  = app->splitter_y - TOOLBAR_H;
    int status_y  = app->splitter_y + SPLITTER_H;
    int results_y = status_y + STATUSBAR_H;
    int results_h = H - FOOTER_H - results_y;

    app->editor->base.bounds        = (Rect){center_x, TOOLBAR_H, center_w, editor_h};
    app->editor_scroll->base.bounds = (Rect){W - SCROLLBAR_W, TOOLBAR_H, SCROLLBAR_W, editor_h};

    app->lbl_status->base.bounds = (Rect){center_x + 112, status_y, center_w - 112 - 260, STATUSBAR_H};

    app->results->base.bounds          = (Rect){center_x, results_y, center_w, results_h};
    app->results_scroll_v->base.bounds = (Rect){W - SCROLLBAR_W, results_y + TABLE_VIEW_HEADER_H,
                                                SCROLLBAR_W, results_h - TABLE_VIEW_HEADER_H - SCROLLBAR_W};
    app->results_scroll_h->base.bounds = (Rect){center_x + app->results->rownum_w, H - FOOTER_H - SCROLLBAR_W,
                                                center_w - app->results->rownum_w - SCROLLBAR_W, SCROLLBAR_W};
}

// ── API pubblica ─────────────────────────────────────────────────

AppWindow* app_window_create(PlatformWindow* win, int width, int height) {
    AppWindow* app = SV_ALLOC(AppWindow);
    if (!app) return NULL;

    app->win    = win;
    app->win_w  = width;
    app->win_h  = height;
    app->splitter_y    = TOOLBAR_H + (int)((height - TOOLBAR_H - FOOTER_H) * 0.52f);
    app->splitter_drag = false;
    app->last_result   = NULL;

    // -- toolbar --
    app->btn_run = button_create(0, 0, 0, 0, "Run", on_run_click, app);
    button_set_style(app->btn_run, BUTTON_STYLE_PRIMARY);
    button_set_icon(app->btn_run, BUTTON_ICON_PLAY, (Color){0, 0, 0, 0});
    button_set_hint(app->btn_run, "F5");

    app->btn_run_current = button_create(0, 0, 0, 0, "Run statement", on_run_current_click, app);
    button_set_icon(app->btn_run_current, BUTTON_ICON_PLAY, TH_ACCENT_TEXT);
    button_set_hint(app->btn_run_current, "Ctrl+Enter");

    app->btn_commit = button_create(0, 0, 0, 0, "Commit", on_commit_click, app);
    button_set_icon(app->btn_commit, BUTTON_ICON_CHECK, TH_SUCCESS);

    app->btn_rollback = button_create(0, 0, 0, 0, "Rollback", on_rollback_click, app);
    button_set_icon(app->btn_rollback, BUTTON_ICON_UNDO, TH_WARNING);

    app->lbl_db_name = label_create(0, 0, 0, 0, "", TH_FONT_UI, TH_TEXT);
    app->lbl_db_name->align = LABEL_ALIGN_RIGHT;
    app->lbl_db_name->base.visible = false; // disegnato come "pillola" dalla finestra

    // -- sidebar --
    app->tree = tree_view_create(0, 0, 0, 0);
    app->tree->base.user_data = app;
    app->tree->on_select      = on_tree_select;
    app->tree->on_activate    = on_tree_activate;
    tree_view_refresh(app->tree);

    app->tree_scroll = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);
    app->tree_scroll->base.user_data = app;
    app->tree_scroll->on_scroll = on_tree_scroll;

    app->btn_refresh = button_create(0, 0, 0, 0, "", on_refresh_click, app);
    button_set_style(app->btn_refresh, BUTTON_STYLE_GHOST);
    button_set_icon(app->btn_refresh, BUTTON_ICON_REFRESH, (Color){0, 0, 0, 0});

    // -- editor SQL --
    app->editor = textbox_create(0, 0, 0, 0);
    app->editor->base.user_data = app;
    app->editor->on_execute = on_editor_execute;
    app->editor->win = win;
    textbox_set_text(
        app->editor,
        "-- Welcome to SegVault: a relational DBMS written in pure C\n"
        "-- F5 runs the whole script (or the selection)\n"
        "-- Ctrl+Enter runs only the statement under the cursor\n"
        "\n"
        "CREATE DATABASE demo;\n"
        "USE demo;\n"
        "\n"
        "CREATE TABLE users (\n"
        "    id     INT PRIMARY KEY,\n"
        "    name   VARCHAR(100),\n"
        "    age    INT,\n"
        "    salary FLOAT,\n"
        "    active BOOLEAN\n"
        ");\n"
        "\n"
        "INSERT INTO users VALUES (1, 'Alice', 30, 45000.0, 1);\n"
        "INSERT INTO users VALUES (2, 'Bob', 25, 38000.5, 0);\n"
        "INSERT INTO users VALUES (3, 'Charlie', 41, 61200.0, 1);\n"
        "\n"
        "SELECT * FROM users WHERE age > 20 ORDER BY name;\n"
    );
    // svuota la cronologia: il testo iniziale non deve essere annullabile
    for (int i = 0; i < app->editor->undo_count; i++) free(app->editor->undo[i].text);
    app->editor->undo_count = 0;
    app->editor->cursor_pos = 0;
    app->editor->sel_anchor = -1;
    app->editor->cursor_line = app->editor->cursor_col = 0;
    app->editor->scroll_x = app->editor->scroll_y = 0;
    textbox_focus(app->editor, true);

    app->editor_scroll = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);
    app->editor_scroll->base.user_data = app;
    app->editor_scroll->on_scroll = on_editor_scroll;

    // -- status bar --
    app->lbl_status = label_create(0, 0, 0, 0, "", TH_FONT_UI, TH_TEXT_MUTED);
    set_status(app, STATUS_IDLE, "Ready", "");

    // -- risultati --
    app->results = table_view_create(0, 0, 0, 0);

    app->results_scroll_v = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);
    app->results_scroll_v->base.user_data = app;
    app->results_scroll_v->on_scroll = on_results_scroll_v;

    app->results_scroll_h = scrollbar_create(0, 0, 0, 0, SCROLLBAR_HORIZONTAL);
    app->results_scroll_h->base.user_data = app;
    app->results_scroll_h->on_scroll = on_results_scroll_h;
    app->results_scroll_h->step = 60;

    update_session(app);

    // applica il layout iniziale
    layout_widgets(app);

    return app;
}

static void update_cursor_shape(AppWindow* app, int mx, int my) {
    CursorKind k = CURSOR_ARROW;
    Rect eb = app->editor->base.bounds;
    if (app->splitter_drag || app->splitter_hover) {
        k = CURSOR_RESIZE_V;
    } else if (table_view_hit_resize(app->results, mx, my)) {
        k = CURSOR_RESIZE_H;
    } else if (app->editor->dragging ||
               (widget_contains_point((Widget*)app->editor, mx, my) &&
                mx >= eb.x + app->editor->gutter_w && mx < eb.x + eb.w - SCROLLBAR_W)) {
        k = CURSOR_IBEAM;
    } else {
        Widget* clickable[] = { (Widget*)app->btn_run, (Widget*)app->btn_run_current, (Widget*)app->btn_commit,
                                (Widget*)app->btn_rollback, (Widget*)app->btn_refresh };
        for (int i = 0; i < 5; i++)
            if (widget_contains_point(clickable[i], mx, my)) { k = CURSOR_HAND; break; }
    }
    platform_set_cursor(app->win, k);
}

void app_window_handle_event(AppWindow* app, sEvent* evt) {
    if (!app) return;

    // resize: ricalcola il layout
    if (evt->type == EVT_RESIZE) {
        app_window_resize(app, evt->new_width, evt->new_height);
        return;
    }
    if (evt->type == EVT_PAINT) return;

    sync_scrollbars(app);

    // -- scorciatoie globali --
    if (evt->type == EVT_KEY_DOWN) {
        if (evt->key == KEY_F5) { app_window_run(app, RUN_SCRIPT); return; }
        bool editor_focused = app->editor->base.state == WIDGET_STATE_FOCUSED;
        if (evt->key == KEY_ENTER && (evt->modifiers & SV_MOD_CTRL) && !editor_focused) {
            app_window_run(app, RUN_CURRENT);
            return;
        }
    }

    // -- splitter --
    int center_x = SIDEBAR_W;
    if (evt->type == EVT_MOUSE_MOVE) {
        app->mouse_x = evt->mouse_x;
        app->mouse_y = evt->mouse_y;
        int sy = app->splitter_y;
        app->splitter_hover = evt->mouse_x >= center_x &&
                              evt->mouse_y >= sy - 2 && evt->mouse_y <= sy + SPLITTER_H + 2;
    }
    if (evt->type == EVT_MOUSE_DOWN && evt->mouse_button == SV_MOUSE_LEFT && app->splitter_hover) {
        app->splitter_drag = true;
        return;
    }
    if (evt->type == EVT_MOUSE_UP && app->splitter_drag) {
        app->splitter_drag = false;
        return;
    }
    if (evt->type == EVT_MOUSE_MOVE && app->splitter_drag) {
        app->splitter_y = evt->mouse_y - SPLITTER_H / 2;
        layout_widgets(app);
        update_cursor_shape(app, evt->mouse_x, evt->mouse_y);
        return;
    }

    Widget* order[] = {
        (Widget*)app->btn_run, (Widget*)app->btn_run_current, (Widget*)app->btn_commit,
        (Widget*)app->btn_rollback, (Widget*)app->btn_refresh,
        (Widget*)app->tree_scroll, (Widget*)app->editor_scroll,
        (Widget*)app->results_scroll_v, (Widget*)app->results_scroll_h,
        (Widget*)app->tree, (Widget*)app->editor, (Widget*)app->results,
    };
    int count = (int)(sizeof(order) / sizeof(order[0]));

    if (evt->type == EVT_MOUSE_UP) {
        // il rilascio va a tutti: termina qualsiasi trascinamento in corso
        for (int i = 0; i < count; i++) widget_handle_event(order[i], evt);
    } else {
        // smista l'evento ai widget in ordine di priorità (chi è "in primo piano" riceve prima)
        for (int i = 0; i < count; i++)
            if (widget_handle_event(order[i], evt)) break;
    }

    if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_UP)
        update_cursor_shape(app, evt->mouse_x, evt->mouse_y);
}

bool app_window_tick(AppWindow* app, uint64_t now_ms) {
    if (!app) return false;
    return textbox_tick(app->editor, now_ms);
}

// ── disegno ──────────────────────────────────────────────────────

static void draw_toolbar(AppWindow* app) {
    PlatformWindow* win = app->win;
    int W = app->win_w;

    platform_fill_rect(win, (Rect){0, 0, W, TOOLBAR_H}, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){0, TOOLBAR_H - 1, W, 1}, TH_BORDER);

    // logo e nome
    ui_icon_logo(win, 30, TOOLBAR_H / 2, 26);
    int ty = ui_text_y(win, (Rect){0, 0, 0, TOOLBAR_H}, 16, FONT_UI_BOLD);
    int nw = platform_draw_text_ex(win, SV_NAME, -1, (Point){52, ty}, TH_TEXT, 16, FONT_UI_BOLD);
    Rect vr = {52 + nw + 8, TOOLBAR_H / 2 - 9, 0, 18};
    char ver[32];
    snprintf(ver, sizeof(ver), "v%s", SV_VERSION_STR);
    vr.w = platform_measure_text_ex(win, ver, -1, TH_FONT_SMALL, FONT_MONO) + 12;
    platform_fill_rect_rounded(win, vr, TH_ELEVATED, 9);
    platform_draw_text_ex(win, ver, -1, (Point){vr.x + 6, ui_text_y(win, vr, TH_FONT_SMALL, FONT_MONO)},
                          TH_TEXT_DIM, TH_FONT_SMALL, FONT_MONO);

    // separatore tra gruppo "esecuzione" e "transazione"
    Rect rc = app->btn_run_current->base.bounds;
    platform_fill_rect(win, (Rect){rc.x + rc.w + 11, 14, 1, TOOLBAR_H - 28}, TH_BORDER_STRONG);

    widget_draw((Widget*)app->btn_run, win);
    widget_draw((Widget*)app->btn_run_current, win);
    widget_draw((Widget*)app->btn_commit, win);
    widget_draw((Widget*)app->btn_rollback, win);

    // pillola del database corrente
    const char* db = app->lbl_db_name->text;
    bool has_db = strcmp(db, "no database") != 0;
    int tw = platform_measure_text_ex(win, db, -1, TH_FONT_UI, has_db ? FONT_UI_BOLD : FONT_UI);
    tw = SV_MIN(tw, 220);
    Rect pill = {W - 16 - (tw + 46), TOOLBAR_H / 2 - 15, tw + 46, 30};
    ui_round_box(win, pill, TH_ELEVATED, TH_BORDER_STRONG, 15);
    ui_icon_database(win, pill.x + 18, pill.y + 15, 14, has_db ? TH_ICON_DB : TH_TEXT_FAINT);
    ui_text_clipped(win, db, pill.x + 32, ui_text_y(win, pill, TH_FONT_UI, FONT_UI_BOLD), tw,
                    has_db ? TH_TEXT : TH_TEXT_DIM, TH_FONT_UI, has_db ? FONT_UI_BOLD : FONT_UI);

    if (app->in_transaction) {
        const char* t = "TRANSACTION";
        int w = platform_measure_text_ex(win, t, -1, TH_FONT_SMALL, FONT_UI_BOLD) + 26;
        Rect b = {pill.x - w - 8, TOOLBAR_H / 2 - 11, w, 22};
        platform_fill_rect_rounded(win, b, TH_WARNING_SOFT, 11);
        ui_circle(win, b.x + 11, b.y + 11, 3, TH_WARNING);
        platform_draw_text_ex(win, t, -1, (Point){b.x + 19, ui_text_y(win, b, TH_FONT_SMALL, FONT_UI_BOLD)},
                              TH_WARNING, TH_FONT_SMALL, FONT_UI_BOLD);
    }
}

static void draw_sidebar(AppWindow* app) {
    PlatformWindow* win = app->win;
    Rect hdr = {0, TOOLBAR_H, SIDEBAR_W - 1, SIDEBAR_HEADER_H};
    platform_fill_rect(win, hdr, TH_SURFACE);
    platform_draw_text_ex(win, "EXPLORER", -1,
                          (Point){18, ui_text_y(win, hdr, TH_FONT_SMALL, FONT_UI_BOLD)},
                          TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    widget_draw((Widget*)app->btn_refresh, win);
    widget_draw((Widget*)app->tree, win);
    widget_draw((Widget*)app->tree_scroll, win);

    // bordo destro della sidebar
    platform_fill_rect(win, (Rect){SIDEBAR_W - 1, TOOLBAR_H, 1, app->win_h - TOOLBAR_H - FOOTER_H}, TH_BORDER);
}

static void draw_splitter_and_status(AppWindow* app) {
    PlatformWindow* win = app->win;
    int cx = SIDEBAR_W, cw = app->win_w - SIDEBAR_W;
    int sy = app->splitter_y;

    // splitter con "maniglia"
    bool active = app->splitter_drag || app->splitter_hover;
    platform_fill_rect(win, (Rect){cx, sy, cw, SPLITTER_H}, TH_SURFACE);
    platform_fill_rect(win, (Rect){cx, sy, cw, 1}, TH_BORDER);
    if (active) platform_fill_rect(win, (Rect){cx, sy + SPLITTER_H / 2 - 1, cw, 2}, ui_alpha(TH_ACCENT, 160));
    int gx = cx + cw / 2;
    for (int i = -1; i <= 1; i++)
        ui_circle(win, gx + i * 8, sy + SPLITTER_H / 2, 2, active ? TH_ACCENT_TEXT : TH_TEXT_FAINT);

    // intestazione risultati
    Rect st = {cx, sy + SPLITTER_H, cw, STATUSBAR_H};
    platform_fill_rect(win, st, TH_SURFACE);
    platform_fill_rect(win, (Rect){cx, st.y + st.h - 1, cw, 1}, TH_BORDER);
    platform_draw_text_ex(win, "RESULTS", -1, (Point){cx + 18, ui_text_y(win, st, TH_FONT_SMALL, FONT_UI_BOLD)},
                          TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);

    Color dot = TH_TEXT_FAINT;
    if (app->status_kind == STATUS_OK)    dot = TH_SUCCESS;
    if (app->status_kind == STATUS_ERROR) dot = TH_ERROR;
    if (app->status_kind == STATUS_INFO)  dot = TH_INFO;
    ui_circle(win, cx + 100, st.y + st.h / 2, 4, dot);
    widget_draw((Widget*)app->lbl_status, win);

    if (app->status_meta[0]) {
        int mw = platform_measure_text_ex(win, app->status_meta, -1, TH_FONT_SMALL + 1, FONT_MONO);
        platform_draw_text_ex(win, app->status_meta, -1,
                              (Point){app->win_w - 18 - mw, ui_text_y(win, st, TH_FONT_SMALL + 1, FONT_MONO)},
                              TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
    }
}

static void draw_footer(AppWindow* app) {
    PlatformWindow* win = app->win;
    int W = app->win_w, H = app->win_h;
    Rect f = {0, H - FOOTER_H, W, FOOTER_H};
    platform_fill_rect(win, f, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){0, f.y, W, 1}, TH_BORDER);

    int fs = TH_FONT_SMALL + 1;
    int ty = ui_text_y(win, f, fs, FONT_UI);
    int x = 16;

    ui_circle(win, x + 3, f.y + FOOTER_H / 2, 3, TH_SUCCESS);
    x += 12;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s engine %s", SV_NAME, SV_VERSION_STR);
    x += platform_draw_text_ex(win, buf, -1, (Point){x, ty}, TH_TEXT_DIM, fs, FONT_UI) + 18;

    ui_icon_database(win, x + 6, f.y + FOOTER_H / 2, 11, TH_TEXT_DIM);
    x += 16;
    x += platform_draw_text_ex(win, app->lbl_db_name->text, -1, (Point){x, ty}, TH_TEXT_MUTED, fs, FONT_UI) + 18;

    if (app->in_transaction) {
        ui_circle(win, x + 3, f.y + FOOTER_H / 2, 3, TH_WARNING);
        x += 12;
        platform_draw_text_ex(win, "Transaction open", -1, (Point){x, ty}, TH_WARNING, fs, FONT_UI);
    }

    // destra: posizione del cursore
    TextBox* tb = app->editor;
    int sa, se;
    textbox_get_selection_range(tb, &sa, &se);
    if (se > sa)
        snprintf(buf, sizeof(buf), "Ln %d, Col %d (%d selected)    %d lines    UTF-8    SQL",
                 tb->cursor_line + 1, tb->cursor_col + 1, se - sa, tb->num_lines);
    else
        snprintf(buf, sizeof(buf), "Ln %d, Col %d    %d lines    UTF-8    SQL",
                 tb->cursor_line + 1, tb->cursor_col + 1, tb->num_lines);
    int w = platform_measure_text_ex(win, buf, -1, fs, FONT_UI);
    platform_draw_text_ex(win, buf, -1, (Point){W - 16 - w, ty}, TH_TEXT_DIM, fs, FONT_UI);
}

void app_window_draw(AppWindow* app) {
    if (!app) return;
    PlatformWindow* win = app->win;

    sync_scrollbars(app);

    // sfondo generale
    platform_clear(win, TH_BG);

    widget_draw((Widget*)app->editor,           win);
    widget_draw((Widget*)app->editor_scroll,    win);
    widget_draw((Widget*)app->results,          win);
    widget_draw((Widget*)app->results_scroll_v, win);
    widget_draw((Widget*)app->results_scroll_h, win);

    draw_splitter_and_status(app);
    draw_sidebar(app);
    draw_toolbar(app);
    draw_footer(app);

    platform_present(win);
}

void app_window_resize(AppWindow* app, int new_w, int new_h) {
    if (!app || new_w <= 0 || new_h <= 0) return;
    // mantieni la proporzione dello splitter
    float ratio = (float)(app->splitter_y - TOOLBAR_H) / (float)SV_MAX(1, app->win_h - TOOLBAR_H - FOOTER_H);
    app->win_w = new_w;
    app->win_h = new_h;

    app->splitter_y = TOOLBAR_H + (int)(ratio * (new_h - TOOLBAR_H - FOOTER_H));
    layout_widgets(app);
}

// ── esecuzione delle query ───────────────────────────────────────

void app_window_run_query(AppWindow* app) {
    app_window_run(app, RUN_SCRIPT);
}

void app_window_run(AppWindow* app, RunMode mode) {
    if (!app) return;
    TextBox* tb = app->editor;

    // 1. testo da eseguire e suo offset nel documento (per mappare gli errori)
    char* owned = NULL;
    const char* text;
    int base = 0;
    if (mode == RUN_CURRENT || textbox_has_selection(tb)) {
        base = textbox_get_run_text(tb, &owned);
        if (base < 0 || !owned) {
            set_status(app, STATUS_INFO, "Nothing to run: the cursor is not on a statement", "");
            return;
        }
        text = owned;
    } else {
        text = textbox_get_text(tb);
    }
    int len = (int)strlen(text);

    // 2. divisione in statement (i ';' dentro stringhe e commenti sono ignorati)
    SqlRange* ranges = malloc(sizeof(SqlRange) * MAX_STATEMENTS);
    if (!ranges) { free(owned); return; }
    int n = sql_split(text, len, ranges, MAX_STATEMENTS);

    int executed = 0, total_code = 0, affected = 0;
    double total_ms = 0;
    QueryResult* grid = NULL;          // ultimo risultato tabellare
    QueryResult* failed = NULL;
    int failed_idx = -1, failed_offset = 0;
    bool changed = false;

    for (int i = 0; i < n; i++) if (ranges[i].has_code) total_code++;

    for (int i = 0; i < n; i++) {
        SqlRange* r = &ranges[i];
        if (!r->has_code) continue;
        int slen = r->end_semi - r->code_start;
        char* stmt = malloc(slen + 1);
        if (!stmt) break;
        memcpy(stmt, text + r->code_start, slen);
        stmt[slen] = '\0';

        QueryResult* res = db_execute(stmt);
        free(stmt);
        executed++;

        if (!res || !res->success) {
            failed = res;
            failed_idx = executed;
            failed_offset = r->code_start;
            break;
        }
        total_ms += res->exec_time_ms;
        if (res->num_cols > 0) {
            if (grid) db_result_free(grid);
            grid = res;
        } else {
            affected += res->rows_affected;
            changed = true;
            db_result_free(res);
        }
    }
    free(ranges);

    char msg[1200], meta[160];
    textbox_set_error_line(tb, -1);

    if (failed_idx > 0) {
        // errore: evidenzia la riga dello statement nell'editor
        int line = textbox_line_of_offset(tb, base + failed_offset);
        textbox_set_error_line(tb, line);
        textbox_scroll_to_line(tb, line);
        const char* err = failed ? failed->error : "No response from the engine";

        char title[128];
        if (total_code > 1) snprintf(title, sizeof(title), "Statement %d of %d failed", failed_idx, total_code);
        else                snprintf(title, sizeof(title), "Query failed");
        snprintf(msg, sizeof(msg), "Line %d: %s", line + 1, err);
        snprintf(meta, sizeof(meta), "%d of %d executed", failed_idx - 1, total_code);

        if (grid) db_result_free(grid);
        show_result(app, NULL);
        table_view_set_message(app->results, TABLE_MSG_ERROR, title, err);
        set_status(app, STATUS_ERROR, msg, meta);
        if (failed) db_result_free(failed);
    } else if (executed == 0) {
        set_status(app, STATUS_INFO, "Nothing to run", "");
    } else if (grid) {
        show_result(app, grid);
        if (executed > 1) snprintf(msg, sizeof(msg), "%d statements executed, showing last result", executed);
        else              snprintf(msg, sizeof(msg), "%d row%s returned", grid->num_rows, grid->num_rows == 1 ? "" : "s");
        snprintf(meta, sizeof(meta), "%d rows  \xc2\xb7  %d cols  \xc2\xb7  %.1f ms",
                 grid->num_rows, grid->num_cols, total_ms);
        set_status(app, STATUS_OK, msg, meta);
    } else {
        show_result(app, NULL);
        char detail[160];
        snprintf(detail, sizeof(detail), "%d row%s affected  \xc2\xb7  %.1f ms",
                 affected, affected == 1 ? "" : "s", total_ms);
        if (executed > 1) snprintf(msg, sizeof(msg), "%d statements executed", executed);
        else              snprintf(msg, sizeof(msg), "Query OK");
        table_view_set_message(app->results, TABLE_MSG_OK, msg, detail);
        snprintf(meta, sizeof(meta), "%.1f ms", total_ms);
        set_status(app, STATUS_OK, msg, meta);
    }

    free(owned);

    // aggiornamento nome DB (potrebbe essere cambiato con USE) e albero se il DDL ha modificato la struttura
    update_session(app);
    if (changed) refresh_tree(app);
}

void app_window_destroy(AppWindow* app) {
    if (!app) return;
    if (app->last_result) db_result_free(app->last_result);
    widget_destroy((Widget*)app->btn_run);
    widget_destroy((Widget*)app->btn_run_current);
    widget_destroy((Widget*)app->btn_commit);
    widget_destroy((Widget*)app->btn_rollback);
    widget_destroy((Widget*)app->btn_refresh);
    widget_destroy((Widget*)app->lbl_db_name);
    widget_destroy((Widget*)app->tree);
    widget_destroy((Widget*)app->tree_scroll);
    widget_destroy((Widget*)app->editor);
    widget_destroy((Widget*)app->editor_scroll);
    widget_destroy((Widget*)app->lbl_status);
    widget_destroy((Widget*)app->results);
    widget_destroy((Widget*)app->results_scroll_v);
    widget_destroy((Widget*)app->results_scroll_h);
    free(app);
}
