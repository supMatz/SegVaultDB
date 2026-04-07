#include "window.h"
#include "../platform/platform.h"
#include "label.h"
#include <stdio.h>
#include <string.h>

// ── Callbacks dei bottoni ────────────────────────────────────────

static void on_run_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    app_window_run_query(app);
}

static void on_commit_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    db_commit();
    label_set_text(app->lbl_status, "COMMIT eseguito");
    // Aggiorna il nome DB in caso di cambio
    label_set_text(app->lbl_db_name, db_current_database()
                   ? db_current_database() : "(nessun DB)");
}

static void on_rollback_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    db_rollback();
    label_set_text(app->lbl_status, "ROLLBACK eseguito");
}

// Callback dell'editor: Ctrl+Enter esegue la query
static void on_editor_execute(const char* text, void* ud) {
    (void)text;
    AppWindow* app = (AppWindow*)ud;
    app_window_run_query(app);
}

// Callback selezione nodo nell'albero
static void on_tree_select(TreeNode* node, void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (!node) return;

    // Se è una tabella: genera automaticamente SELECT * FROM tabella
    if (node->type == TREE_NODE_TABLE) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT * FROM `%s` LIMIT 100;", node->label);
        textbox_set_text(app->editor, sql);
    }
}

// -- layout helper --

// Ricalcola la posizione e dimensione di tutti i widget in base alle dimensioni correnti della finestra
static void layout_widgets(AppWindow* app) {
    int W = app->win_w;
    int H = app->win_h;

    // toolbar: striscia in alto
    int ty = 0;
    int t_btn_w = 80, t_btn_h = 28;
    app->btn_run->base.bounds      = (Rect){SIDEBAR_W + 8, ty + 6, t_btn_h, t_btn_w};
    app->btn_commit->base.bounds   = (Rect){SIDEBAR_W + 96, ty + 6, t_btn_h, t_btn_w};
    app->btn_rollback->base.bounds = (Rect){SIDEBAR_W + 184, ty + 6, t_btn_h, t_btn_w};
    app->lbl_db_name->base.bounds  = (Rect){W - 200, ty + 8, 190, 24};

    // sidebar: colonna sinistra
    int content_h = H - TOOLBAR_H;
    app->tree->base.bounds =
        (Rect){0, TOOLBAR_H, SIDEBAR_W - 12, content_h};
    app->tree_scroll->base.bounds =
        (Rect){SIDEBAR_W - 12, TOOLBAR_H, 12, content_h};

    // area centrale: divisa dallo splitter
    int center_x = SIDEBAR_W;
    int center_w = W - SIDEBAR_W;
    int editor_h = app->splitter_y - TOOLBAR_H;
    int results_y = app->splitter_y + SPLITTER_H + STATUSBAR_H;
    int results_h = H - results_y - 12; // 12 = scrollbar orizzontale

    // editor SQL
    app->editor->base.bounds = (Rect){center_x, TOOLBAR_H, center_w - 12, editor_h};
    
    app->editor_scroll->base.bounds = (Rect){W - 12, TOOLBAR_H, 12, editor_h};

    // status bar
    app->lbl_status->base.bounds =
        (Rect){center_x, app->splitter_y + SPLITTER_H,
                 center_w, STATUSBAR_H};

    // Risultati
    app->results->base.bounds =
        (Rect){center_x, results_y, center_w - 12, results_h};
    app->results_scroll_v->base.bounds =
        (Rect){W - 12, results_y, 12, results_h};
    app->results_scroll_h->base.bounds =
        (Rect){center_x, H - 12, center_w - 12, 12};
}

// -- API pubblica --

AppWindow* app_window_create(PlatformWindow* win, int width, int height) {
    AppWindow* app = SV_ALLOC(AppWindow);
    if (!app) return NULL;

    app->win    = win;
    app->win_w  = width;
    app->win_h  = height;
    app->splitter_y = TOOLBAR_H + (height - TOOLBAR_H) / 2;
    app->splitter_drag = false;
    app->last_result   = NULL;

    // -- toolbar --
    app->btn_run = button_create(0, 0, 0, 0, "Run Query", on_run_click, app);
    app->btn_run->color_bg      = (Color){30,  100, 50,  255};
    app->btn_run->color_hover   = (Color){40,  130, 65,  255};
    app->btn_run->color_pressed = (Color){20,  70,  35,  255};

    app->btn_commit   = button_create(0, 0, 0, 0, "Commit", on_commit_click, app);

    app->btn_rollback = button_create(0, 0, 0, 0, "Rollback", on_rollback_click, app);

    app->lbl_db_name = label_create(0, 0, 0, 0, db_current_database() ? db_current_database() : "(nessun DB)", 13, (Color){150, 200, 255, 255});
    app->lbl_db_name->align = LABEL_ALIGN_RIGHT;

    // -- sidebar --
    app->tree = tree_view_create(0, 0, 0, 0);
    app->tree->base.user_data = app;
    app->tree->on_select      = on_tree_select;
    tree_view_refresh(app->tree);

    app->tree_scroll = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);

    // -- editor SQL --
    app->editor = textbox_create(0, 0, 0, 0);
    app->editor->base.user_data = app;
    app->editor->on_execute = on_editor_execute;
    textbox_set_text(app->editor,
        "-- SegVault SQL Editor\n"
        "-- Ctrl+Enter per eseguire\n\n"
        "CREATE DATABASE test;\n");

    app->editor_scroll = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);

    // -- status bar --
    app->lbl_status = label_create(0, 0, 0, 0, "Pronto", 12, (Color){150, 150, 160, 255});

    // -- risultati --
    app->results = table_view_create(0, 0, 0, 0);
    
    app->results_scroll_v = scrollbar_create(0, 0, 0, 0,SCROLLBAR_VERTICAL);
    
    app->results_scroll_h = scrollbar_create(0, 0, 0, 0,SCROLLBAR_HORIZONTAL);

    // applica il layout iniziale
    layout_widgets(app);

    return app;
}

void app_window_handle_event(AppWindow* app, sEvent* evt) {
    if (!app) return;

    // resize: ricalcola il layout
    if (evt->type == EVT_RESIZE) {
        app_window_resize(app, evt->new_width, evt->new_height);
        return;
    }

    // splitter drag
    if (evt->type == EVT_MOUSE_DOWN) {
        int sy = app->splitter_y;
        if (evt->mouse_y >= sy && evt->mouse_y <= sy + SPLITTER_H) {
            app->splitter_drag = true;
            return;
        }
    }
    if (evt->type == EVT_MOUSE_UP)   app->splitter_drag = false;

    if (evt->type == EVT_MOUSE_MOVE && app->splitter_drag) {
        int min_y = TOOLBAR_H + 80;
        int max_y = app->win_h - STATUSBAR_H - 80;
        app->splitter_y = SV_CLAMP(evt->mouse_y, min_y, max_y);
        layout_widgets(app);
        return;
    }

    // smista l'evento ai widget in ordine di priorità
    // (chi è "in primo piano" riceve prima)
    if (widget_handle_event((Widget*)app->btn_run,       evt)) return;
    if (widget_handle_event((Widget*)app->btn_commit,    evt)) return;
    if (widget_handle_event((Widget*)app->btn_rollback,  evt)) return;
    if (widget_handle_event((Widget*)app->tree,          evt)) return;
    if (widget_handle_event((Widget*)app->tree_scroll,   evt)) return;
    if (widget_handle_event((Widget*)app->editor,        evt)) return;
    if (widget_handle_event((Widget*)app->editor_scroll, evt)) return;
    if (widget_handle_event((Widget*)app->results,       evt)) return;
    
    widget_handle_event((Widget*)app->results_scroll_v,  evt);
    widget_handle_event((Widget*)app->results_scroll_h,  evt);
}

void app_window_draw(AppWindow* app) {
    if (!app) return;
    PlatformWindow* win = app->win;

    // sfondo generale
    platform_clear(win, (Color){18, 18, 18, 255});

    // toolbar background
    platform_fill_rect(
        win,
        (Rect){0, 0, TOOLBAR_H, app->win_w},
        (Color){28, 28, 34, 255}
    );
    
    platform_draw_line(
        win,
        (Point){0, TOOLBAR_H},
        (Point){app->win_w, TOOLBAR_H},
        (Color){50, 50, 60, 255}, 1
    );

    // separatore sidebar
    platform_draw_line(
        win,
        (Point){SIDEBAR_W, TOOLBAR_H},
        (Point){SIDEBAR_W, app->win_h},
        (Color){50, 50, 60, 255}, 1
    );

    // splitter
    platform_fill_rect(win,
        (Rect){SIDEBAR_W, app->splitter_y, SPLITTER_H, (app->win_w - SIDEBAR_W) },
        (Color){38, 38, 48, 255}
    );

    // status bar background
    platform_fill_rect(win,
        (Rect){SIDEBAR_W, app->splitter_y + SPLITTER_H, STATUSBAR_H, (app->win_w - SIDEBAR_W)},
        (Color){28, 28, 34, 255}
    );

    // disegna tutti i widget
    widget_draw((Widget*)app->tree,           win);
    widget_draw((Widget*)app->tree_scroll,    win);
    widget_draw((Widget*)app->editor,         win);
    widget_draw((Widget*)app->editor_scroll,  win);
    widget_draw((Widget*)app->results,        win);
    widget_draw((Widget*)app->results_scroll_v, win);
    widget_draw((Widget*)app->results_scroll_h, win);
    widget_draw((Widget*)app->btn_run,        win);
    widget_draw((Widget*)app->btn_commit,     win);
    widget_draw((Widget*)app->btn_rollback,   win);
    widget_draw((Widget*)app->lbl_db_name,    win);
    widget_draw((Widget*)app->lbl_status,     win);

    platform_present(win);
}

void app_window_resize(AppWindow* app, int new_w, int new_h) {
    if (!app) return;
    // mantieni la proporzione dello splitter
    float ratio = (float)(app->splitter_y - TOOLBAR_H) / (float)(app->win_h - TOOLBAR_H);
    app->win_w = new_w;
    app->win_h = new_h;

    app->splitter_y = TOOLBAR_H + (int)(ratio * (new_h - TOOLBAR_H));
    layout_widgets(app);
}

void app_window_run_query(AppWindow* app) {
    if (!app) return;

    const char* sql = textbox_get_text(app->editor);
    if (!sql || sql[0] == '\0') return;

    // liberazione risultato precedente
    if (app->last_result) {
        db_result_free(app->last_result);
        app->last_result = NULL;
    }

    // esecuzione query tramite il bridge
    QueryResult* result = db_execute(sql);
    app->last_result    = result;

    // aggiornamento griglia dei risultati
    table_view_set_result(app->results, result);

    // aggiornamento status bar
    char status[256];
    if (!result) {
        snprintf(status, sizeof(status), "Errore: risposta NULL");
    } else if (!result->success) {
        snprintf(status, sizeof(status), "Errore: %s", result->error);
        label_set_color(app->lbl_status, (Color){220, 80, 80, 255});
    } else if (result->num_cols > 0) {
        snprintf(status, sizeof(status),
                 "%d righe  |  %.1f ms  |  %d colonne",
                 result->num_rows, result->exec_time_ms, result->num_cols);
        label_set_color(app->lbl_status, (Color){150, 150, 160, 255});
    } else {
        snprintf(status, sizeof(status),
                 "%d righe modificate  |  %.1f ms",
                 result->rows_affected, result->exec_time_ms);
        label_set_color(app->lbl_status, (Color){80, 200, 120, 255});
    }
    label_set_text(app->lbl_status, status);

    // aggiornamento nome DB (potrebbe essere cambiato con USE)
    const char* db = db_current_database();
    label_set_text(app->lbl_db_name, db ? db : "(nessun DB)");

    // ricarica l'albero se il DDL ha modificato la struttura
    if (result && result->success && result->num_cols == 0)
        tree_view_refresh(app->tree);
}

void app_window_destroy(AppWindow* app) {
    if (!app) return;
    if (app->last_result) db_result_free(app->last_result);
    widget_destroy((Widget*)app->btn_run);
    widget_destroy((Widget*)app->btn_commit);
    widget_destroy((Widget*)app->btn_rollback);
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