#include "window.h"
#include "../platform/platform.h"
#include "label.h"
#include "theme.h"
#include "sql_text.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

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

// ── autocompletamento: oggetti del database corrente ─────────────

typedef struct { CompletionItem* v; int n, cap; } CompList;

static void comp_push(CompList* l, CompletionKind k, const char* name, const char* detail, const char* parent) {
    if (!name || !name[0]) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 128;
        CompletionItem* nv = realloc(l->v, sizeof(CompletionItem) * cap);
        if (!nv) return;
        l->v = nv;
        l->cap = cap;
    }
    CompletionItem* it = &l->v[l->n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->detail, sizeof(it->detail), "%s", detail ? detail : "");
    snprintf(it->parent, sizeof(it->parent), "%s", parent ? parent : "");
    it->kind = k;
}

static void update_completions(AppWindow* app) {
    CompList l = {0};
    NameList* dbs = db_list_databases();
    if (dbs) {
        for (int i = 0; i < dbs->count; i++) comp_push(&l, COMP_DATABASE, dbs->names[i], "database", NULL);
        db_namelist_free(dbs);
    }
    const char* cur = db_current_database();
    if (cur && cur[0]) {
        char db[128];
        snprintf(db, sizeof(db), "%s", cur);
        NameList* tables = db_list_tables(db);
        if (tables) {
            for (int i = 0; i < tables->count; i++) {
                comp_push(&l, COMP_TABLE, tables->names[i], "table", NULL);
                int nc = 0;
                ColumnInfo* cols = db_list_columns(db, tables->names[i], &nc);
                for (int c = 0; cols && c < nc; c++)
                    comp_push(&l, COMP_COLUMN, cols[c].name, cols[c].type, tables->names[i]);
                if (cols) db_columns_free(cols, nc);
            }
            db_namelist_free(tables);
        }
        NameList* views = db_list_views(db);
        if (views) {
            for (int i = 0; i < views->count; i++) comp_push(&l, COMP_VIEW, views->names[i], "view", NULL);
            db_namelist_free(views);
        }
    }
    textbox_set_completions(app->editor, l.v, l.n);
    free(l.v);
}

static void update_session(AppWindow* app) {
    const char* db = db_current_database();
    label_set_text(app->lbl_db_name, db && db[0] ? db : "no database");
    SessionInfo* s = db_session_info();
    app->in_transaction = s ? s->in_transaction : false;
    if (s) db_session_free(s);
    update_completions(app);
}

static void show_result(AppWindow* app, QueryResult* r) {
    // il vecchio risultato si libera solo dopo aver aggiornato la griglia
    QueryResult* old = app->last_result;
    app->last_result = r;
    app->showing_history = false;
    table_view_set_result(app->results, r);
    if (old && old != r) db_result_free(old);
}

// ── file ─────────────────────────────────────────────────────────

// legge un file di testo (max SV_MAX_SQL_LEN - 1 byte); malloc'd o NULL
static char* read_text_file(const char* path, bool* truncated) {
    if (truncated) *truncated = false;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = SV_MAX_SQL_LEN;
    char* buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, cap - 1, f);
    if (truncated && n == cap - 1 && fgetc(f) != EOF) *truncated = true;
    fclose(f);
    buf[n] = '\0';
    // salta il BOM UTF-8
    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        memmove(buf, buf + 3, n - 2);
    return buf;
}

static bool write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

// espande "~/" e toglie gli spazi ai bordi
static void expand_path(const char* in, char* out, size_t n) {
    while (*in == ' ') in++;
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\\' || in[1] == '\0') && home)
        snprintf(out, n, "%s%s", home, in + 1);
    else
        snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l > 0 && out[l - 1] == ' ') out[--l] = '\0';
}

static const char* base_name(const char* p) {
    const char* b = p;
    for (const char* s = p; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static void join_path(char* out, size_t n, const char* dir, const char* name) {
    size_t l = strlen(dir);
    bool sep = l > 0 && (dir[l - 1] == '/' || dir[l - 1] == '\\');
    snprintf(out, n, "%s%s%s", dir, (sep || l == 0) ? "" : "/", name);
}

static void now_str(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    if (tm) strftime(buf, n, "%Y-%m-%d %H:%M:%S", tm);
    else snprintf(buf, n, "-");
}

// sostituisce il testo dell'editor (annullabile con Ctrl+Z) e porta il cursore in cima
static void editor_load_text(AppWindow* app, const char* text) {
    TextBox* tb = app->editor;
    textbox_replace_text(tb, 0, tb->text_len, text, false);
    tb->cursor_pos = 0;
    tb->sel_anchor = -1;
    tb->cursor_line = tb->cursor_col = tb->pref_col = 0;
    tb->scroll_x = tb->scroll_y = 0;
    textbox_set_error_line(tb, -1);
    app->modified = false;
}

static bool save_to(AppWindow* app, const char* path) {
    char msg[1200];
    if (!write_text_file(path, textbox_get_text(app->editor))) {
        snprintf(msg, sizeof(msg), "Cannot save %s: %s", path, strerror(errno));
        set_status(app, STATUS_ERROR, msg, "");
        return false;
    }
    if (path != app->file_path) snprintf(app->file_path, sizeof(app->file_path), "%s", path);
    app->modified = false;
    snprintf(msg, sizeof(msg), "Saved %s", path);
    set_status(app, STATUS_OK, msg, "");
    return true;
}

static bool open_from(AppWindow* app, const char* path) {
    char msg[1200];
    bool trunc = false;
    char* text = read_text_file(path, &trunc);
    if (!text) {
        snprintf(msg, sizeof(msg), "Cannot open %s: %s", path, strerror(errno));
        set_status(app, STATUS_ERROR, msg, "");
        return false;
    }
    editor_load_text(app, text);
    free(text);
    snprintf(app->file_path, sizeof(app->file_path), "%s", path);
    snprintf(msg, sizeof(msg), trunc ? "Opened %s (truncated to 64 KB)" : "Opened %s", path);
    set_status(app, trunc ? STATUS_INFO : STATUS_OK, msg, "Ctrl+Z restores the previous text");
    return true;
}

static void cmd_new(AppWindow* app) {
    editor_load_text(app, "");
    app->file_path[0] = '\0';
    set_status(app, STATUS_INFO, "New script", "Ctrl+Z restores the previous text");
    textbox_focus(app->editor, true);
}

// ── cronologia delle query ───────────────────────────────────────

#define HISTORY_FILE  "history.sql"
#define HISTORY_MARK  "-- @run "

static void history_push(AppWindow* app, char* sql, const char* when, bool ok, int rows, double ms) {
    if (app->history_count == HISTORY_MAX) {
        free(app->history[0].sql);
        memmove(app->history, app->history + 1, sizeof(HistoryEntry) * (HISTORY_MAX - 1));
        app->history_count--;
    }
    HistoryEntry* e = &app->history[app->history_count++];
    e->sql = sql;
    snprintf(e->when, sizeof(e->when), "%s", when);
    e->ok = ok;
    e->rows = rows;
    e->ms = ms;
}

static void history_add(AppWindow* app, const char* sql, bool ok, int rows, double ms) {
    // testo senza spazi ai bordi
    while (*sql && isspace((unsigned char)*sql)) sql++;
    size_t n = strlen(sql);
    while (n > 0 && isspace((unsigned char)sql[n - 1])) n--;
    if (n == 0) return;
    char* copy = malloc(n + 1);
    if (!copy) return;
    memcpy(copy, sql, n);
    copy[n] = '\0';
    char when[20];
    now_str(when, sizeof(when));
    history_push(app, copy, when, ok, rows, ms);

    if (!app->data_dir[0]) return;
    char path[1100];
    join_path(path, sizeof(path), app->data_dir, HISTORY_FILE);
    FILE* f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, HISTORY_MARK "%s | %s | %d | %.2f\n%s\n", when, ok ? "ok" : "error", rows, ms, copy);
    fclose(f);
}

static void history_load(AppWindow* app) {
    char path[1100];
    join_path(path, sizeof(path), app->data_dir, HISTORY_FILE);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    // legge solo la coda del file: bastano le ultime HISTORY_MAX voci
    long start = size > 512 * 1024 ? size - 512 * 1024 : 0;
    fseek(f, start, SEEK_SET);
    char* buf = malloc(size - start + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, size - start, f);
    fclose(f);
    buf[n] = '\0';

    size_t ml = strlen(HISTORY_MARK);
    char* p = strstr(buf, HISTORY_MARK);
    while (p) {
        char* hdr = p + ml;
        char* body = strchr(hdr, '\n');
        if (!body) break;
        *body++ = '\0';
        char* next = strstr(body, "\n" HISTORY_MARK);
        char* end = next ? next : body + strlen(body);
        while (end > body && isspace((unsigned char)end[-1])) end--;

        char when[20] = "", status[16] = "";
        int rows = 0;
        double ms = 0;
        if (sscanf(hdr, "%19[^|] | %15[^ |] | %d | %lf", when, status, &rows, &ms) >= 2 && end > body) {
            size_t wl = strlen(when);
            while (wl > 0 && when[wl - 1] == ' ') when[--wl] = '\0';
            char* sql = malloc(end - body + 1);
            if (sql) {
                memcpy(sql, body, end - body);
                sql[end - body] = '\0';
                history_push(app, sql, when, strcmp(status, "ok") == 0, rows, ms);
            }
        }
        p = next ? next + 1 : NULL;
    }
    free(buf);
}

// risultato sintetico (stesse regole di allocazione di db_result_free)
static QueryResult* make_grid(const char* const* cols, const char* const* types, int ncols, int nrows) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    r->success   = true;
    r->col_names = calloc(ncols, sizeof(char*));
    r->col_types = calloc(ncols, sizeof(char*));
    r->rows      = calloc(nrows > 0 ? nrows : 1, sizeof(Row));
    if (!r->col_names || !r->col_types || !r->rows) { db_result_free(r); return NULL; }
    r->num_cols = ncols;
    for (int c = 0; c < ncols; c++) {
        r->col_names[c] = strdup(cols[c]);
        r->col_types[c] = strdup(types[c]);
    }
    for (int i = 0; i < nrows; i++) {
        r->rows[i].cells = calloc(ncols, sizeof(Cell));
        if (!r->rows[i].cells) { db_result_free(r); return NULL; }
        r->rows[i].num_cells = ncols;
        r->num_rows++;
    }
    return r;
}

static void history_show(AppWindow* app) {
    static const char* const cols[]  = { "time", "status", "rows", "ms", "statement" };
    static const char* const types[] = { "", "", "", "", "" };
    int n = app->history_count;
    QueryResult* r = make_grid(cols, types, 5, n);
    if (!r) return;
    for (int i = 0; i < n; i++) {
        const HistoryEntry* e = &app->history[n - 1 - i];   // più recente in alto
        Cell* c = r->rows[i].cells;
        snprintf(c[0].value, sizeof(c[0].value), "%s", e->when);
        snprintf(c[1].value, sizeof(c[1].value), "%s", e->ok ? "ok" : "error");
        snprintf(c[2].value, sizeof(c[2].value), "%d", e->rows);
        snprintf(c[3].value, sizeof(c[3].value), "%.1f", e->ms);
        c[0].type = c[1].type = c[4].type = CELL_TYPE_STRING;
        c[2].type = CELL_TYPE_INT;
        c[3].type = CELL_TYPE_FLOAT;
        // statement su una riga sola
        int k = 0;
        bool sp = false;
        for (const char* s = e->sql; *s && k < (int)sizeof(c[4].value) - 4; s++) {
            if (isspace((unsigned char)*s)) { if (!sp && k) c[4].value[k++] = ' '; sp = true; }
            else { c[4].value[k++] = *s; sp = false; }
        }
        c[4].value[k] = '\0';
    }
    show_result(app, r);
    app->showing_history = true;
    char meta[64];
    snprintf(meta, sizeof(meta), "%d entr%s", n, n == 1 ? "y" : "ies");
    set_status(app, STATUS_INFO, n ? "Query history \xc2\xb7 double-click or Enter inserts the statement into the editor"
                                   : "Query history is empty", meta);
}

static void history_toggle(AppWindow* app) {
    if (app->showing_history) {
        show_result(app, NULL);
        set_status(app, STATUS_IDLE, "Ready", "");
    } else {
        history_show(app);
    }
}

static void on_history_click(void* ud) { history_toggle((AppWindow*)ud); }

// doppio click su una riga della cronologia: inserisce lo statement nell'editor
static void on_results_activate(int row, void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (!app->showing_history || row < 0 || row >= app->history_count) return;
    const HistoryEntry* e = &app->history[app->history_count - 1 - row];
    size_t n = strlen(e->sql);
    char* text = malloc(n + 3);
    if (!text) return;
    memcpy(text, e->sql, n);
    if (n == 0 || e->sql[n - 1] != ';') text[n++] = ';';
    text[n] = '\0';
    TextBox* tb = app->editor;
    // accoda in fondo allo script (separato da una riga vuota) e lo seleziona:
    // inserirlo al cursore rischierebbe di spezzare lo statement corrente
    int end = tb->text_len;
    while (end > 0 && (tb->text[end - 1] == ' ' || tb->text[end - 1] == '\n')) end--;
    char* full = malloc(n + 4);
    if (!full) { free(text); return; }
    snprintf(full, n + 4, "%s%s", end > 0 ? "\n\n" : "", text);
    textbox_replace_text(tb, end, tb->text_len, full, false);
    int stmt_start = end + (end > 0 ? 2 : 0);
    tb->sel_anchor = stmt_start;
    textbox_scroll_to_line(tb, textbox_line_of_offset(tb, stmt_start));
    free(full);
    free(text);
    textbox_focus(tb, true);
    set_status(app, STATUS_OK, "Statement inserted into the editor", "");
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

// formatta la selezione (estesa a righe intere) o l'intero script
static void format_sql(AppWindow* app) {
    TextBox* tb = app->editor;
    int a = 0, b = tb->text_len;
    bool sel = textbox_has_selection(tb);
    if (sel) {
        textbox_get_selection_range(tb, &a, &b);
        while (a > 0 && tb->text[a - 1] != '\n') a--;
    }
    char* out = sql_format(tb->text + a, b - a);
    if (!out) { set_status(app, STATUS_ERROR, "Format failed (out of memory)", ""); return; }
    if ((int)strlen(out) == b - a && memcmp(out, tb->text + a, b - a) == 0) {
        set_status(app, STATUS_INFO, "Already formatted", "");
    } else {
        textbox_replace_text(tb, a, b, out, sel);
        set_status(app, STATUS_OK, sel ? "Selection formatted" : "Script formatted", "Ctrl+Z to undo");
    }
    free(out);
    textbox_focus(tb, true);
}

static void on_format_click(void* ud) { format_sql((AppWindow*)ud); }

static void on_refresh_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    refresh_tree(app);
    set_status(app, STATUS_INFO, "Explorer refreshed", "");
}

// callback dell'editor: Ctrl+Enter esegue lo statement corrente (o la selezione)
static void on_editor_change(const char* text, void* ud) {
    (void)text;
    ((AppWindow*)ud)->modified = true;
}

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

// ── barra comandi: trova / sostituisci / vai alla riga ───────────

#define BAR_ROW_H  32
#define BAR_BTN_W  26

static bool bar_visible(const AppWindow* app) { return app->bar_mode != BAR_NONE; }

static const char* bar_label(const AppWindow* app) {
    switch (app->bar_mode) {
        case BAR_GOTO:    return "Go to line";
        case BAR_OPEN:    return "Open file";
        case BAR_SAVE_AS: return "Save as";
        default:       return NULL;
    }
}

static void layout_bar(AppWindow* app) {
    for (int i = 0; i < BAR_BTN_COUNT; i++) app->bar_btn[i] = (Rect){0, 0, 0, 0};
    app->bar_input->base.visible = app->bar_replace_input->base.visible = false;
    if (!bar_visible(app)) { app->bar_rect = (Rect){0, 0, 0, 0}; return; }

    int center_w = app->win_w - SIDEBAR_W;
    int bw = SV_MIN(560, center_w - 40);
    bool two_rows = app->bar_mode == BAR_FIND && app->bar_replace;
    int bh = (two_rows ? 2 : 1) * BAR_ROW_H + 12 + (two_rows ? 4 : 0);
    Rect r = {app->win_w - SCROLLBAR_W - 14 - bw, TOOLBAR_H + 8, bw, bh};
    app->bar_rect = r;

    int y1 = r.y + 6, y2 = y1 + BAR_ROW_H + 4;
    int ih = BAR_ROW_H - 4;
    int right = r.x + r.w - 6;
    Rect* b = app->bar_btn;

    b[BAR_BTN_CLOSE] = (Rect){right - BAR_BTN_W, y1 + 2, BAR_BTN_W, ih};
    int in_x = r.x + 8;
    int in_r;
    if (app->bar_mode == BAR_FIND) {
        b[BAR_BTN_TOGGLE_REPLACE] = (Rect){r.x + 6, y1 + 2, 20, two_rows ? ih * 2 + 4 + 4 : ih};
        in_x = r.x + 30;
        b[BAR_BTN_NEXT] = (Rect){b[BAR_BTN_CLOSE].x - 2 - BAR_BTN_W, y1 + 2, BAR_BTN_W, ih};
        b[BAR_BTN_PREV] = (Rect){b[BAR_BTN_NEXT].x - 2 - BAR_BTN_W, y1 + 2, BAR_BTN_W, ih};
        int info_w = 84;
        b[BAR_BTN_WORD] = (Rect){b[BAR_BTN_PREV].x - 6 - info_w - BAR_BTN_W, y1 + 2, BAR_BTN_W, ih};
        b[BAR_BTN_CASE] = (Rect){b[BAR_BTN_WORD].x - 2 - BAR_BTN_W, y1 + 2, BAR_BTN_W, ih};
        in_r = b[BAR_BTN_CASE].x - 6;
        if (two_rows) {
            int aw = 44, rw = 72;
            b[BAR_BTN_REPLACE_ALL] = (Rect){right - aw, y2 + 2, aw, ih};
            b[BAR_BTN_REPLACE]     = (Rect){b[BAR_BTN_REPLACE_ALL].x - 4 - rw, y2 + 2, rw, ih};
            app->bar_replace_input->base.bounds  = (Rect){in_x, y2 + 2, in_r - in_x, ih};
            app->bar_replace_input->base.visible = true;
        }
    } else {
        in_x = r.x + 12 + app->bar_label_w + 10;
        in_r = b[BAR_BTN_CLOSE].x - 96;
    }
    app->bar_input->base.bounds  = (Rect){in_x, y1 + 2, SV_MAX(60, in_r - in_x), ih};
    app->bar_input->base.visible = true;
}

static void bar_apply_search(AppWindow* app, bool select_first) {
    LineEdit* in = app->bar_input;
    textbox_set_search(app->editor, in->text, app->find_case, app->find_word);
    if (!in->text[0]) { in->error = false; return; }
    bool found = select_first ? textbox_find_next(app->editor, false, true) : textbox_search_count(app->editor, NULL) > 0;
    in->error = !found;
}

static void bar_open(AppWindow* app, BarMode mode, bool replace) {
    bool was_find = app->bar_mode == BAR_FIND;
    app->bar_mode = mode;
    if (mode == BAR_FIND) app->bar_replace = replace || (was_find && app->bar_replace);
    const char* lbl = bar_label(app);
    app->bar_label_w = lbl ? platform_measure_text_ex(app->win, lbl, -1, TH_FONT_UI, FONT_UI_BOLD) : 0;

    if (mode == BAR_FIND) {
        // precompila con la selezione o la parola sotto il cursore
        char* w = textbox_word_or_selection(app->editor);
        if (w && w[0]) line_edit_set_text(app->bar_input, w);
        free(w);
        app->bar_input->placeholder = "Find";
        bar_apply_search(app, false);
    } else if (mode == BAR_GOTO) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", app->editor->cursor_line + 1);
        line_edit_set_text(app->bar_input, buf);
        app->bar_input->placeholder = "Line number";
        textbox_set_search(app->editor, NULL, false, false);
    } else if (mode == BAR_OPEN || mode == BAR_SAVE_AS) {
        char buf[1100];
        if (app->file_path[0] && mode == BAR_SAVE_AS) {
            snprintf(buf, sizeof(buf), "%s", app->file_path);
        } else if (app->file_path[0]) {
            snprintf(buf, sizeof(buf), "%s", app->file_path);
            char* b = (char*)base_name(buf);
            *b = '\0';                               // solo la cartella
        } else {
            join_path(buf, sizeof(buf), app->data_dir[0] ? app->data_dir : ".",
                      mode == BAR_SAVE_AS ? "script.sql" : "");
        }
        line_edit_set_text(app->bar_input, buf);
        app->bar_input->placeholder = "Path of the .sql file";
        textbox_set_search(app->editor, NULL, false, false);
    }
    layout_bar(app);
    textbox_focus(app->editor, false);
    line_edit_focus(app->bar_replace_input, false);
    line_edit_focus(app->bar_input, true);
    if (mode == BAR_OPEN || mode == BAR_SAVE_AS) app->bar_input->anchor = -1;   // cursore in fondo al percorso
    else line_edit_select_all(app->bar_input);
}

static void bar_close(AppWindow* app) {
    if (!bar_visible(app)) return;
    app->bar_mode = BAR_NONE;
    textbox_set_search(app->editor, NULL, false, false);
    line_edit_focus(app->bar_input, false);
    line_edit_focus(app->bar_replace_input, false);
    app->bar_input->error = false;
    layout_bar(app);
    textbox_focus(app->editor, true);
}

static void bar_find(AppWindow* app, bool backwards) {
    if (!app->bar_input->text[0]) return;
    textbox_set_search(app->editor, app->bar_input->text, app->find_case, app->find_word);
    app->bar_input->error = !textbox_find_next(app->editor, backwards, false);
}

static void bar_replace_one(AppWindow* app) {
    if (!app->bar_input->text[0]) return;
    bar_apply_search(app, false);
    textbox_replace_current(app->editor, app->bar_replace_input->text);
}

static void bar_replace_all(AppWindow* app) {
    if (!app->bar_input->text[0]) return;
    bar_apply_search(app, false);
    int n = textbox_replace_all(app->editor, app->bar_replace_input->text);
    char msg[96];
    snprintf(msg, sizeof(msg), n ? "Replaced %d occurrence%s" : "No occurrences to replace", n, n == 1 ? "" : "s");
    set_status(app, n ? STATUS_OK : STATUS_INFO, msg, "");
}

// Invio nel campo principale: dipende dalla modalità
static void bar_submit(AppWindow* app, bool shift, bool ctrl) {
    LineEdit* in = app->bar_input;
    switch (app->bar_mode) {
        case BAR_FIND:
            if (line_edit_focused(app->bar_replace_input)) {
                if (ctrl) bar_replace_all(app); else bar_replace_one(app);
            } else {
                bar_find(app, shift);
            }
            break;
        case BAR_GOTO: {
            char* end = NULL;
            long l = strtol(in->text, &end, 10);
            while (end && *end == ' ') end++;
            if (!in->text[0] || (end && *end) || l < 1) { in->error = true; return; }
            textbox_goto_line(app->editor, (int)l - 1);
            bar_close(app);
            break;
        }
        case BAR_OPEN:
        case BAR_SAVE_AS: {
            char path[1024];
            expand_path(in->text, path, sizeof(path));
            size_t pl = strlen(path);
            if (!pl || path[pl - 1] == '/' || path[pl - 1] == '\\') { in->error = true; return; }
            bool ok = app->bar_mode == BAR_OPEN ? open_from(app, path) : save_to(app, path);
            if (ok) bar_close(app); else in->error = true;
            break;
        }
        default: break;
    }
}

static void on_bar_input_change(LineEdit* le, void* ud) {
    AppWindow* app = (AppWindow*)ud;
    (void)le;
    if (app->bar_mode == BAR_FIND) bar_apply_search(app, true);
}

static void bar_click(AppWindow* app, BarButton btn) {
    switch (btn) {
        case BAR_BTN_TOGGLE_REPLACE:
            app->bar_replace = !app->bar_replace;
            layout_bar(app);
            break;
        case BAR_BTN_CASE: app->find_case = !app->find_case; bar_apply_search(app, false); break;
        case BAR_BTN_WORD: app->find_word = !app->find_word; bar_apply_search(app, false); break;
        case BAR_BTN_PREV: bar_find(app, true);  break;
        case BAR_BTN_NEXT: bar_find(app, false); break;
        case BAR_BTN_CLOSE: bar_close(app); break;
        case BAR_BTN_REPLACE: bar_replace_one(app); break;
        case BAR_BTN_REPLACE_ALL: bar_replace_all(app); break;
        default: break;
    }
}

static bool rect_has(Rect r, int x, int y) {
    return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ritorna true se l'evento è stato consumato dalla barra
static bool bar_handle_event(AppWindow* app, sEvent* evt) {
    if (!bar_visible(app)) return false;
    LineEdit* in = app->bar_input;
    LineEdit* rp = app->bar_replace_input;
    bool focused = line_edit_focused(in) || line_edit_focused(rp);

    switch (evt->type) {
        case EVT_MOUSE_DOWN: {
            bool inside = rect_has(app->bar_rect, evt->mouse_x, evt->mouse_y);
            // i campi gestiscono da soli acquisizione/perdita del focus
            bool hit_in = widget_handle_event((Widget*)in, evt);
            bool hit_rp = rp->base.visible && widget_handle_event((Widget*)rp, evt);
            if (!inside) return false;
            if (hit_in || hit_rp) { textbox_focus(app->editor, false); return true; }
            for (int i = 0; i < BAR_BTN_COUNT; i++)
                if (rect_has(app->bar_btn[i], evt->mouse_x, evt->mouse_y)) { bar_click(app, (BarButton)i); break; }
            return true;
        }
        case EVT_MOUSE_MOVE:
        case EVT_MOUSE_UP: {
            bool a = widget_handle_event((Widget*)in, evt);
            bool b = rp->base.visible && widget_handle_event((Widget*)rp, evt);
            return a || b;
        }
        case EVT_MOUSE_WHEEL:
            return rect_has(app->bar_rect, evt->mouse_x, evt->mouse_y);

        case EVT_KEY_DOWN: {
            bool shift = evt->modifiers & SV_MOD_SHIFT;
            bool ctrl  = evt->modifiers & SV_MOD_CTRL;
            bool alt   = evt->modifiers & SV_MOD_ALT;
            if (evt->key == KEY_ESCAPE && (focused || app->editor->base.state == WIDGET_STATE_FOCUSED)) {
                bar_close(app);
                return true;
            }
            if (!focused) return false;
            if (evt->key == KEY_ENTER) { bar_submit(app, shift, ctrl); return true; }
            if (evt->key == KEY_TAB && app->bar_mode == BAR_FIND && app->bar_replace) {
                bool to_rp = line_edit_focused(in);
                line_edit_focus(in, !to_rp);
                line_edit_focus(rp, to_rp);
                line_edit_select_all(to_rp ? rp : in);
                return true;
            }
            if (app->bar_mode == BAR_FIND && evt->key == KEY_LETTER && alt) {
                if (evt->character == 'c') { bar_click(app, BAR_BTN_CASE); return true; }
                if (evt->character == 'w') { bar_click(app, BAR_BTN_WORD); return true; }
                if (evt->character == 'r') { bar_replace_one(app); return true; }
                if (evt->character == 'a') { bar_replace_all(app); return true; }
            }
            if (app->bar_mode == BAR_FIND && (evt->key == KEY_UP || evt->key == KEY_DOWN)) {
                bar_find(app, evt->key == KEY_UP);
                return true;
            }
            LineEdit* target = line_edit_focused(rp) ? rp : in;
            widget_handle_event((Widget*)target, evt);
            return true; // con il focus nella barra, i tasti non arrivano all'editor
        }
        case EVT_CHAR:
            if (!focused) return false;
            widget_handle_event((Widget*)(line_edit_focused(rp) ? rp : in), evt);
            return true;
        default:
            return false;
    }
}

static void draw_bar_button(AppWindow* app, Rect r, const char* text, bool active) {
    PlatformWindow* win = app->win;
    bool hover = rect_has(r, app->mouse_x, app->mouse_y);
    Color bg = active ? TH_ACCENT_SOFT : hover ? TH_ELEVATED_HOVER : (text && strlen(text) > 2) ? TH_ELEVATED
                                                                         : (Color){0, 0, 0, 0};
    if (bg.a) ui_round_box(win, r, bg, active ? TH_ACCENT : bg, 4);
    if (text) {
        int tw = platform_measure_text_ex(win, text, -1, TH_FONT_SMALL + 1, FONT_UI_BOLD);
        platform_draw_text_ex(win, text, -1, (Point){r.x + (r.w - tw) / 2, ui_text_y(win, r, TH_FONT_SMALL + 1, FONT_UI_BOLD)},
                              active ? TH_ACCENT_TEXT : TH_TEXT_MUTED, TH_FONT_SMALL + 1, FONT_UI_BOLD);
    }
}

static void draw_bar(AppWindow* app) {
    if (!bar_visible(app)) return;
    PlatformWindow* win = app->win;
    Rect r = app->bar_rect;
    Rect* b = app->bar_btn;

    // ombra + pannello
    platform_fill_rect_rounded(win, (Rect){r.x - 1, r.y + 2, r.w + 2, r.h + 2}, ui_alpha(TH_BG, 160), 8);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 7);

    if (app->bar_mode == BAR_FIND) {
        Rect t = b[BAR_BTN_TOGGLE_REPLACE];
        bool hv = rect_has(t, app->mouse_x, app->mouse_y);
        if (hv) platform_fill_rect_rounded(win, t, TH_ELEVATED_HOVER, 4);
        ui_icon_chevron(win, t.x + t.w / 2, t.y + BAR_ROW_H / 2 - 2, 10,
                        app->bar_replace ? CHEVRON_DOWN : CHEVRON_RIGHT, TH_TEXT_MUTED);

        draw_bar_button(app, b[BAR_BTN_CASE], "Aa", app->find_case);
        draw_bar_button(app, b[BAR_BTN_WORD], "W", app->find_word);

        // contatore risultati
        char info[64] = "";
        if (app->bar_input->text[0]) {
            int cur = 0, n = textbox_search_count(app->editor, &cur);
            if (n == 0)      snprintf(info, sizeof(info), "No results");
            else if (cur)    snprintf(info, sizeof(info), "%d of %d", cur, n);
            else             snprintf(info, sizeof(info), "%d found", n);
        }
        int ix = b[BAR_BTN_WORD].x + BAR_BTN_W + 8;
        Rect ir = {ix, b[BAR_BTN_WORD].y, b[BAR_BTN_PREV].x - ix - 4, b[BAR_BTN_WORD].h};
        if (info[0])
            ui_text_clipped(win, info, ir.x, ui_text_y(win, ir, TH_FONT_SMALL + 1, FONT_UI), ir.w,
                            app->bar_input->error ? TH_ERROR : TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);

        for (int k = 0; k < 2; k++) {
            Rect br = b[k ? BAR_BTN_NEXT : BAR_BTN_PREV];
            draw_bar_button(app, br, NULL, false);
            ui_icon_chevron(win, br.x + br.w / 2, br.y + br.h / 2, 10, k ? CHEVRON_DOWN : CHEVRON_UP, TH_TEXT_MUTED);
        }
        if (app->bar_replace) {
            draw_bar_button(app, b[BAR_BTN_REPLACE], "Replace", false);
            draw_bar_button(app, b[BAR_BTN_REPLACE_ALL], "All", false);
            widget_draw((Widget*)app->bar_replace_input, win);
        }
    } else {
        const char* lbl = bar_label(app);
        if (lbl) {
            Rect lr = {r.x + 12, r.y + 6, app->bar_label_w, BAR_ROW_H};
            platform_draw_text_ex(win, lbl, -1, (Point){lr.x, ui_text_y(win, lr, TH_FONT_UI, FONT_UI_BOLD)},
                                  TH_TEXT_MUTED, TH_FONT_UI, FONT_UI_BOLD);
        }
        Rect ib = app->bar_input->base.bounds;
        const char* hint = app->bar_input->error ? "Invalid" : "Enter to confirm";
        Rect hr = {ib.x + ib.w + 8, ib.y, b[BAR_BTN_CLOSE].x - (ib.x + ib.w + 8) - 4, ib.h};
        ui_text_clipped(win, hint, hr.x, ui_text_y(win, hr, TH_FONT_SMALL + 1, FONT_UI), hr.w,
                        app->bar_input->error ? TH_ERROR : TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
    }

    widget_draw((Widget*)app->bar_input, win);
    Rect c = b[BAR_BTN_CLOSE];
    draw_bar_button(app, c, NULL, false);
    ui_icon_cross(win, c.x + c.w / 2, c.y + c.h / 2, 9, TH_TEXT_MUTED);
}

// ── layout ───────────────────────────────────────────────────────

// ricalcola la posizione e dimensione di tutti i widget in base alle dimensioni correnti della finestra
static void layout_widgets(AppWindow* app) {
    int W = app->win_w;
    int H = app->win_h;

    // toolbar
    int bh = 32, by = (TOOLBAR_H - bh) / 2;
    int x = SIDEBAR_W + 14;
    Button* btns[] = { app->btn_run, app->btn_run_current, app->btn_commit, app->btn_rollback, app->btn_format };
    for (int i = 0; i < 5; i++) {
        int w = button_preferred_width(btns[i], app->win);
        btns[i]->base.bounds = (Rect){x, by, w, bh};
        x += w + ((i == 1 || i == 3) ? 22 : 8); // spazio extra tra i gruppi
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
    int hw = button_preferred_width(app->btn_history, app->win);
    app->btn_history->base.bounds = (Rect){W - 12 - hw, status_y + (STATUSBAR_H - 26) / 2, hw, 26};

    app->results->base.bounds          = (Rect){center_x, results_y, center_w, results_h};
    app->results_scroll_v->base.bounds = (Rect){W - SCROLLBAR_W, results_y + TABLE_VIEW_HEADER_H,
                                                SCROLLBAR_W, results_h - TABLE_VIEW_HEADER_H - SCROLLBAR_W};
    app->results_scroll_h->base.bounds = (Rect){center_x + app->results->rownum_w, H - FOOTER_H - SCROLLBAR_W,
                                                center_w - app->results->rownum_w - SCROLLBAR_W, SCROLLBAR_W};
    layout_bar(app);
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

    app->btn_format = button_create(0, 0, 0, 0, "Format", on_format_click, app);
    button_set_hint(app->btn_format, "Ctrl+Shift+F");

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
    app->editor->on_change = on_editor_change;
    app->modified = false;

    app->editor_scroll = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);
    app->editor_scroll->base.user_data = app;
    app->editor_scroll->on_scroll = on_editor_scroll;

    // -- status bar --
    app->lbl_status = label_create(0, 0, 0, 0, "", TH_FONT_UI, TH_TEXT_MUTED);
    set_status(app, STATUS_IDLE, "Ready", "");

    // -- risultati --
    app->results = table_view_create(0, 0, 0, 0);
    app->results->base.user_data = app;
    app->results->on_row_activate = on_results_activate;

    app->btn_history = button_create(0, 0, 0, 0, "History", on_history_click, app);
    button_set_style(app->btn_history, BUTTON_STYLE_GHOST);

    app->results_scroll_v = scrollbar_create(0, 0, 0, 0, SCROLLBAR_VERTICAL);
    app->results_scroll_v->base.user_data = app;
    app->results_scroll_v->on_scroll = on_results_scroll_v;

    app->results_scroll_h = scrollbar_create(0, 0, 0, 0, SCROLLBAR_HORIZONTAL);
    app->results_scroll_h->base.user_data = app;
    app->results_scroll_h->on_scroll = on_results_scroll_h;
    app->results_scroll_h->step = 60;

    // -- barra comandi --
    app->bar_input = line_edit_create("Find");
    app->bar_input->base.user_data = app;
    app->bar_input->on_change = on_bar_input_change;
    app->bar_input->win = win;
    app->bar_replace_input = line_edit_create("Replace");
    app->bar_replace_input->win = win;
    app->bar_mode = BAR_NONE;

    update_session(app);

    // applica il layout iniziale
    layout_widgets(app);

    return app;
}

static void update_cursor_shape(AppWindow* app, int mx, int my) {
    CursorKind k = CURSOR_ARROW;
    Rect eb = app->editor->base.bounds;
    if (bar_visible(app) && rect_has(app->bar_rect, mx, my)) {
        k = CURSOR_ARROW;
        if (widget_contains_point((Widget*)app->bar_input, mx, my) ||
            (app->bar_replace_input->base.visible && widget_contains_point((Widget*)app->bar_replace_input, mx, my)))
            k = CURSOR_IBEAM;
        for (int i = 0; i < BAR_BTN_COUNT; i++) if (rect_has(app->bar_btn[i], mx, my)) k = CURSOR_HAND;
    } else if (app->splitter_drag || app->splitter_hover) {
        k = CURSOR_RESIZE_V;
    } else if (table_view_hit_resize(app->results, mx, my)) {
        k = CURSOR_RESIZE_H;
    } else if (app->editor->dragging ||
               (widget_contains_point((Widget*)app->editor, mx, my) &&
                mx >= eb.x + app->editor->gutter_w && mx < eb.x + eb.w - SCROLLBAR_W)) {
        k = CURSOR_IBEAM;
    } else {
        Widget* clickable[] = { (Widget*)app->btn_run, (Widget*)app->btn_run_current, (Widget*)app->btn_commit,
                                (Widget*)app->btn_rollback, (Widget*)app->btn_refresh, (Widget*)app->btn_format,
                                (Widget*)app->btn_history };
        for (int i = 0; i < 7; i++)
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
        bool ctrl  = evt->modifiers & SV_MOD_CTRL;
        bool shift = evt->modifiers & SV_MOD_SHIFT;
        if (evt->key == KEY_F5) { app_window_run(app, RUN_SCRIPT); return; }
        if (evt->key == KEY_LETTER && ctrl && !(evt->modifiers & SV_MOD_ALT)) {
            switch (evt->character) {
                case 'f': if (shift) format_sql(app); else bar_open(app, BAR_FIND, false); return;
                case 'h': if (shift) history_toggle(app); else bar_open(app, BAR_FIND, true); return;
                case 'g': bar_open(app, BAR_GOTO, false); return;
                case 'o': bar_open(app, BAR_OPEN, false); return;
                case 'n': cmd_new(app); return;
                case 's':
                    if (shift || !app->file_path[0]) bar_open(app, BAR_SAVE_AS, false);
                    else save_to(app, app->file_path);
                    return;
                default: break;
            }
        }
        if (evt->key == KEY_F3) {
            if (app->bar_input->text[0]) {
                bar_find(app, shift);
                if (!bar_visible(app)) textbox_set_search(app->editor, NULL, false, false);
            } else {
                bar_open(app, BAR_FIND, false);
            }
            return;
        }
    }
    // popup di autocompletamento: ha la precedenza sul resto (può sporgere sui risultati)
    if ((evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_WHEEL) &&
        textbox_popup_contains(app->editor, evt->mouse_x, evt->mouse_y)) {
        widget_handle_event((Widget*)app->editor, evt);
        platform_set_cursor(app->win, CURSOR_HAND);
        return;
    }
    if (evt->type == EVT_MOUSE_DOWN) app->editor->popup_open = false;

    if (bar_handle_event(app, evt)) {
        if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN) {
            app->mouse_x = evt->mouse_x;
            app->mouse_y = evt->mouse_y;
            update_cursor_shape(app, evt->mouse_x, evt->mouse_y);
        }
        return;
    }
    if (evt->type == EVT_KEY_DOWN) {
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
        (Widget*)app->btn_rollback, (Widget*)app->btn_refresh, (Widget*)app->btn_format,
        (Widget*)app->btn_history,
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
    bool a = textbox_tick(app->editor, now_ms);
    bool b = line_edit_tick(app->bar_input, now_ms);
    bool c = line_edit_tick(app->bar_replace_input, now_ms);
    return a || b || c;
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
    Rect rb = app->btn_rollback->base.bounds;
    platform_fill_rect(win, (Rect){rb.x + rb.w + 11, 14, 1, TOOLBAR_H - 28}, TH_BORDER_STRONG);
    widget_draw((Widget*)app->btn_format, win);

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

    widget_draw((Widget*)app->btn_history, win);
    if (app->showing_history) {
        Rect hb = app->btn_history->base.bounds;
        platform_fill_rect(win, (Rect){hb.x + 8, hb.y + hb.h, hb.w - 16, 2}, TH_ACCENT);
    }
    if (app->status_meta[0]) {
        int mw = platform_measure_text_ex(win, app->status_meta, -1, TH_FONT_SMALL + 1, FONT_MONO);
        int mx = app->btn_history->base.bounds.x - 14 - mw;
        platform_draw_text_ex(win, app->status_meta, -1,
                              (Point){mx, ui_text_y(win, st, TH_FONT_SMALL + 1, FONT_MONO)},
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

    // file corrente (● = modifiche non salvate)
    const char* fname = app->file_path[0] ? base_name(app->file_path) : "untitled.sql";
    x += platform_draw_text_ex(win, fname, -1, (Point){x, ty}, app->file_path[0] ? TH_TEXT_MUTED : TH_TEXT_DIM, fs, FONT_UI);
    if (app->modified) {
        ui_circle(win, x + 8, f.y + FOOTER_H / 2, 3, TH_ACCENT_TEXT);
        x += 14;
    }
    x += 18;

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
    draw_bar(app);

    draw_splitter_and_status(app);
    draw_sidebar(app);
    draw_toolbar(app);
    draw_footer(app);
    textbox_draw_overlay(app->editor, win, (Rect){0, 0, app->win_w, app->win_h});

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
        history_add(app, stmt, res && res->success,
                    res ? (res->num_cols > 0 ? res->num_rows : res->rows_affected) : 0,
                    res ? res->exec_time_ms : 0);
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
    widget_destroy((Widget*)app->btn_format);
    widget_destroy((Widget*)app->btn_history);
    for (int i = 0; i < app->history_count; i++) free(app->history[i].sql);
    widget_destroy((Widget*)app->lbl_db_name);
    widget_destroy((Widget*)app->tree);
    widget_destroy((Widget*)app->tree_scroll);
    widget_destroy((Widget*)app->editor);
    widget_destroy((Widget*)app->editor_scroll);
    widget_destroy((Widget*)app->lbl_status);
    widget_destroy((Widget*)app->results);
    widget_destroy((Widget*)app->results_scroll_v);
    widget_destroy((Widget*)app->results_scroll_h);
    widget_destroy((Widget*)app->bar_input);
    widget_destroy((Widget*)app->bar_replace_input);
    free(app);
}

// ── sessione ─────────────────────────────────────────────────────

#define SESSION_TEXT "session.sql"
#define SESSION_META "session.ini"

void app_window_set_data_dir(AppWindow* app, const char* dir) {
    if (!app || !dir) return;
    snprintf(app->data_dir, sizeof(app->data_dir), "%s", dir);
    history_load(app);

    char path[1100];
    join_path(path, sizeof(path), dir, SESSION_TEXT);
    char* text = read_text_file(path, NULL);
    if (!text) return;   // prima esecuzione: resta lo script di benvenuto

    TextBox* tb = app->editor;
    textbox_replace_text(tb, 0, tb->text_len, text, false);
    free(text);
    // il testo ripristinato non deve essere annullabile
    for (int i = 0; i < tb->undo_count; i++) free(tb->undo[i].text);
    tb->undo_count = 0;

    int cursor = 0, modified = 0;
    join_path(path, sizeof(path), dir, SESSION_META);
    FILE* f = fopen(path, "rb");
    if (f) {
        char line[1200];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!strncmp(line, "file=", 5))     snprintf(app->file_path, sizeof(app->file_path), "%.1023s", line + 5);
            else if (!strncmp(line, "cursor=", 7))   cursor = atoi(line + 7);
            else if (!strncmp(line, "modified=", 9)) modified = atoi(line + 9);
            else if (!strncmp(line, "font=", 5))     tb->font_size = SV_CLAMP(atoi(line + 5), 10, 28);
        }
        fclose(f);
    }
    tb->cursor_pos = SV_CLAMP(cursor, 0, tb->text_len);
    tb->sel_anchor = -1;
    tb->cursor_line = textbox_line_of_offset(tb, tb->cursor_pos);
    tb->cursor_col = 0;
    for (int i = tb->line_starts[tb->cursor_line]; i < tb->cursor_pos; i++)
        if (((unsigned char)tb->text[i] & 0xC0) != 0x80) tb->cursor_col++;
    tb->pref_col = tb->cursor_col;
    tb->scroll_x = 0;
    tb->scroll_y = SV_MAX(0, tb->cursor_line - 5);
    app->modified = modified != 0;
    set_status(app, STATUS_IDLE, "Previous session restored", "");
}

void app_window_save_session(AppWindow* app) {
    if (!app || !app->data_dir[0]) return;
    char path[1100];
    join_path(path, sizeof(path), app->data_dir, SESSION_TEXT);
    write_text_file(path, textbox_get_text(app->editor));
    join_path(path, sizeof(path), app->data_dir, SESSION_META);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "file=%s\ncursor=%d\nmodified=%d\nfont=%d\n",
            app->file_path, app->editor->cursor_pos, app->modified ? 1 : 0, app->editor->font_size);
    fclose(f);
}
