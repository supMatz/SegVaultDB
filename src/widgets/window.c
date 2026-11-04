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
#include <stdatomic.h>

static void refresh_tree(AppWindow* app);
static void update_session(AppWindow* app);
static bool busy_guard(AppWindow* app);
static void finish_run(AppWindow* app);
static void style_run_button(AppWindow* app);

// esecuzione in background (vedi app_window_run)
typedef struct { char* sql; bool ok; int rows; double ms; } RunLog;

typedef struct RunJob {
    char*        text;            // copia del testo eseguito
    SqlRange*    ranges;
    int          n;
    int          total_code;      // statement con codice
    RunLog*      log;             // uno per statement eseguito (per la cronologia)
    int          log_n;
    QueryResult* grid;            // ultimo risultato tabellare
    QueryResult* failed;
    int          failed_idx, failed_offset;
    int          executed, affected;
    double       total_ms;
    bool         changed;         // DDL/DML eseguito: albero e completamento da aggiornare
    bool         cancelled;       // fermato prima di uno statement
    atomic_int   done;
    atomic_int   cancel;
    atomic_int   current;         // statement in esecuzione (1-based)
} RunJob;

// ── stato / status bar ───────────────────────────────────────────

static void set_status(AppWindow* app, StatusKind kind, const char* text, const char* meta) {
    app->status_kind = kind;
    label_set_text(app->lbl_status, text ? text : "");
    Color c = TH_TEXT_MUTED;
    if (kind == STATUS_OK)    c = TH_TEXT;
    if (kind == STATUS_ERROR) c = TH_ERROR;
    label_set_color(app->lbl_status, c);
    snprintf(app->status_meta, sizeof(app->status_meta), "%.127s", meta ? meta : "");
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
    if (app->results->order) row = app->results->order[row];   // griglia ordinata
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

// mentre una query è in esecuzione il pulsante Run diventa Stop
static void on_run_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (app->job) app_window_cancel(app);
    else          app_window_run(app, RUN_SCRIPT);
}
static void on_run_current_click(void* ud) { app_window_run((AppWindow*)ud, RUN_CURRENT); }

static void on_commit_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (busy_guard(app)) return;
    bool ok = db_commit();
    update_session(app);
    set_status(app, ok ? STATUS_OK : STATUS_ERROR, ok ? "COMMIT executed" : "COMMIT failed", "");
}

static void on_rollback_click(void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (busy_guard(app)) return;
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
    if (busy_guard(app)) return;
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
    if (busy_guard((AppWindow*)ud)) return;   // il motore è occupato dal thread di lavoro
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
    if (busy_guard(app)) return;
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
        case BAR_EXPORT:  return "Export CSV";
        default:       return NULL;
    }
}

static void layout_bar(AppWindow* app) {
    for (int i = 0; i < BAR_BTN_COUNT; i++) app->bar_btn[i] = (Rect){0, 0, 0, 0};
    app->bar_input->base.visible = app->bar_replace_input->base.visible = false;
    if (!bar_visible(app)) { app->bar_rect = (Rect){0, 0, 0, 0}; return; }

    int center_w = app->win_w - app->side_w;
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
    } else if (mode == BAR_EXPORT) {
        char buf[1100];
        join_path(buf, sizeof(buf), app->data_dir[0] ? app->data_dir : ".", "result.csv");
        line_edit_set_text(app->bar_input, buf);
        app->bar_input->placeholder = "Path of the .csv file";
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
    if (mode == BAR_OPEN || mode == BAR_SAVE_AS || mode == BAR_EXPORT) app->bar_input->anchor = -1; // cursore in fondo
    else line_edit_select_all(app->bar_input);
}

static void bar_close(AppWindow* app) {
    if (!bar_visible(app)) return;
    app->bar_mode = BAR_NONE;
    app->tip_target = -1;
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
        case BAR_EXPORT: {
            char path[1024];
            expand_path(in->text, path, sizeof(path));
            if (!path[0]) { in->error = true; return; }
            char* csv = table_view_export(app->results, true);
            if (!csv) { set_status(app, STATUS_INFO, "Nothing to export: run a query first", ""); bar_close(app); return; }
            bool ok = write_text_file(path, csv);
            free(csv);
            char msg[1200];
            if (ok) {
                snprintf(msg, sizeof(msg), "Exported %d rows to %s", app->results->result->num_rows, path);
                set_status(app, STATUS_OK, msg, "");
                bar_close(app);
            } else {
                snprintf(msg, sizeof(msg), "Cannot write %s: %s", path, strerror(errno));
                set_status(app, STATUS_ERROR, msg, "");
                in->error = true;
            }
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

// ── temi ─────────────────────────────────────────────────────────

#define THEME_ROW_H 34

// ricarica i colori dei widget che li memorizzano (gli altri leggono il tema a ogni frame)
static int theme_pill_w(AppWindow* app);

static void refresh_widget_colors(AppWindow* app) {
    Button* btns[] = { app->btn_run, app->btn_run_current, app->btn_commit, app->btn_rollback,
                       app->btn_format, app->btn_refresh, app->btn_history };
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) button_set_style(btns[i], btns[i]->style);
    button_set_icon(app->btn_run_current, BUTTON_ICON_PLAY, TH_ACCENT_TEXT);
    button_set_icon(app->btn_commit, BUTTON_ICON_CHECK, TH_SUCCESS);
    button_set_icon(app->btn_rollback, BUTTON_ICON_UNDO, TH_WARNING);
    table_view_apply_theme(app->results);
    tree_view_apply_theme(app->tree);
    Scrollbar* sbs[] = { app->tree_scroll, app->editor_scroll, app->results_scroll_v, app->results_scroll_h };
    for (size_t i = 0; i < sizeof(sbs) / sizeof(sbs[0]); i++) scrollbar_apply_theme(sbs[i]);
    Color c = TH_TEXT_MUTED;
    if (app->status_kind == STATUS_OK)    c = TH_TEXT;
    if (app->status_kind == STATUS_ERROR) c = TH_ERROR;
    label_set_color(app->lbl_status, c);
    label_set_color(app->lbl_db_name, TH_TEXT);
    style_run_button(app);
}

static void set_theme(AppWindow* app, int index, bool announce) {
    if (!theme_get(index)) return;
    theme_apply(index);
    refresh_widget_colors(app);
    if (announce) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Theme: %s", theme_get(index)->name);
        set_status(app, STATUS_INFO, msg, "Ctrl+Shift+T next theme");
    }
}

// campione del tema: sfondo + accento
static void draw_theme_swatch(PlatformWindow* win, const Theme* t, int cx, int cy, int r) {
    ui_circle(win, cx, cy, r + 1, t->border_strong);
    ui_circle(win, cx, cy, r, t->surface_2);
    ui_circle(win, cx + r / 3, cy, r * 2 / 3, t->accent);
}

static void draw_theme_menu(AppWindow* app) {
    if (!app->theme_menu_open) return;
    PlatformWindow* win = app->win;
    int n = theme_count();
    int w = 230, h = n * THEME_ROW_H + 8;
    Rect b = app->theme_btn;
    Rect r = {SV_MAX(8, b.x + b.w - w), b.y + b.h + 6, w, h};
    app->theme_menu = r;
    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 3, r.w + 4, r.h + 4}, ui_alpha(TH_BG, 170), 10);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 8);
    for (int i = 0; i < n; i++) {
        const Theme* t = theme_get(i);
        Rect row = {r.x + 4, r.y + 4 + i * THEME_ROW_H, r.w - 8, THEME_ROW_H};
        if (i == app->theme_hover) platform_fill_rect_rounded(win, row, TH_ELEVATED_HOVER, 6);
        draw_theme_swatch(win, t, row.x + 18, row.y + row.h / 2, 8);
        bool cur = i == theme_current();
        platform_draw_text_ex(win, t->name, -1, (Point){row.x + 36, ui_text_y(win, row, TH_FONT_UI, cur ? FONT_UI_BOLD : FONT_UI)},
                              TH_TEXT, TH_FONT_UI, cur ? FONT_UI_BOLD : FONT_UI);
        const char* kind = t->light ? "light" : "dark";
        int kw = platform_measure_text_ex(win, kind, -1, TH_FONT_SMALL, FONT_UI);
        platform_draw_text_ex(win, kind, -1, (Point){row.x + row.w - 34 - kw, ui_text_y(win, row, TH_FONT_SMALL, FONT_UI)},
                              TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI);
        if (cur) ui_icon_check(win, row.x + row.w - 16, row.y + row.h / 2, 12, TH_ACCENT);
    }
}

// pillola nella toolbar; x_right = bordo destro disponibile. Ritorna la x sinistra.
static int draw_theme_button(AppWindow* app, int x_right) {
    PlatformWindow* win = app->win;
    const Theme* t = theme_get(theme_current());
    int pw = theme_pill_w(app);
    Rect b = {x_right - pw, TOOLBAR_H / 2 - 15, pw, 30};
    app->theme_btn = b;
    bool hover = rect_has(b, app->mouse_x, app->mouse_y) || app->theme_menu_open;
    ui_round_box(win, b, hover ? TH_ELEVATED_HOVER : TH_ELEVATED, TH_BORDER_STRONG, 15);
    draw_theme_swatch(win, t, b.x + 17, b.y + 15, 7);
    if (app->compact < 2)
        platform_draw_text_ex(win, t->name, -1, (Point){b.x + 32, ui_text_y(win, b, TH_FONT_UI, FONT_UI)}, TH_TEXT, TH_FONT_UI, FONT_UI);
    ui_icon_chevron(win, b.x + b.w - 15, b.y + 15, 9, app->theme_menu_open ? CHEVRON_UP : CHEVRON_DOWN, TH_TEXT_MUTED);
    return b.x;
}

// ritorna true se l'evento è stato consumato dal selettore temi
static bool theme_handle_event(AppWindow* app, sEvent* evt) {
    if (evt->type == EVT_MOUSE_DOWN && evt->mouse_button == SV_MOUSE_LEFT && rect_has(app->theme_btn, evt->mouse_x, evt->mouse_y)) {
        app->theme_menu_open = !app->theme_menu_open;
        app->theme_hover = theme_current();
        return true;
    }
    if (!app->theme_menu_open) return false;
    Rect r = app->theme_menu;
    int row = (evt->mouse_y - r.y - 4) / THEME_ROW_H;
    bool in = rect_has(r, evt->mouse_x, evt->mouse_y) && row >= 0 && row < theme_count();
    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            if (in) app->theme_hover = row;
            return in;
        case EVT_MOUSE_DOWN:
            if (in) set_theme(app, row, true);
            app->theme_menu_open = false;
            return true;   // il click fuori chiude il menu senza agire sotto
        case EVT_MOUSE_WHEEL:
            return true;
        case EVT_KEY_DOWN:
            switch (evt->key) {
                case KEY_UP:     app->theme_hover = (app->theme_hover + theme_count() - 1) % theme_count(); return true;
                case KEY_DOWN:   app->theme_hover = (app->theme_hover + 1) % theme_count(); return true;
                case KEY_ENTER:  set_theme(app, app->theme_hover, true); app->theme_menu_open = false; return true;
                case KEY_ESCAPE: app->theme_menu_open = false; return true;
                default: return false;
            }
        case EVT_CHAR:
            return true;
        default:
            return false;
    }
}

// ── pannello scorciatoie (F1) ───────────────────────────────────

typedef struct { const char* keys; const char* what; } Shortcut;
typedef struct { const char* title; const Shortcut* items; int count; } ShortcutGroup;

static const Shortcut SC_RUN[] = {
    { "F5", "Run script or selection" }, { "Ctrl+Enter", "Run statement under cursor" },
    { "Ctrl+Shift+F", "Format SQL" }, { "Ctrl+Shift+H", "Query history" },
    { "Ctrl+Shift+T", "Next color theme" }, { "Esc / Stop", "Cancel the running query" },
};
static const Shortcut SC_FILE[] = {
    { "Ctrl+N", "New script" }, { "Ctrl+O", "Open file" },
    { "Ctrl+S", "Save" }, { "Ctrl+Shift+S", "Save as" },
};
static const Shortcut SC_FIND[] = {
    { "Ctrl+F", "Find" }, { "Ctrl+H", "Find and replace" }, { "F3 / Shift+F3", "Next / previous match" },
    { "Alt+C / Alt+W", "Match case / whole word" }, { "Ctrl+G", "Go to line" },
};
static const Shortcut SC_EDIT[] = {
    { "Ctrl+Space", "Autocomplete" }, { "Alt+Up / Down", "Move lines" }, { "Ctrl+D", "Duplicate line" },
    { "Ctrl+Shift+K", "Delete line" }, { "Ctrl+L", "Select line" }, { "Ctrl+/", "Toggle comment" },
    { "Ctrl+Shift+U / U", "Upper / lower case" }, { "Shift+Enter", "New line below" },
    { "Tab / Shift+Tab", "Indent / outdent" }, { "Ctrl+Z / Ctrl+Y", "Undo / redo" },
    { "Ctrl + wheel, Ctrl+0", "Zoom / reset" },
};
static const Shortcut SC_GRID[] = {
    { "Click header", "Sort asc / desc / off" }, { "Ctrl+C", "Copy cell (or row)" },
    { "Ctrl+Shift+C", "Copy all as TSV" }, { "Ctrl+E", "Export CSV" },
    { "Double-click border", "Fit column width" },
};

#define SC_GROUP(t, a) { t, a, (int)(sizeof(a) / sizeof(a[0])) }
static const ShortcutGroup SC_GROUPS[] = {
    SC_GROUP("RUN", SC_RUN), SC_GROUP("FILES", SC_FILE), SC_GROUP("FIND", SC_FIND),
    SC_GROUP("EDITING", SC_EDIT), SC_GROUP("RESULTS", SC_GRID),
};

static void draw_help(AppWindow* app) {
    if (!app->help_open) return;
    PlatformWindow* win = app->win;
    int W = app->win_w, H = app->win_h;
    platform_fill_rect(win, (Rect){0, 0, W, H}, ui_alpha(TH_BG, 200));

    int col_w = 340, row_h = 22, gap = 28;
    int ncols = W >= 2 * col_w + 120 ? 2 : 1;
    int ngroups = (int)(sizeof(SC_GROUPS) / sizeof(SC_GROUPS[0]));
    // colonna sinistra: RUN, FILES, FIND · destra: EDITING, RESULTS
    int split = ncols == 2 ? 3 : ngroups;
    int hcol[2] = {0, 0};
    for (int g = 0; g < ngroups; g++) hcol[g < split ? 0 : 1] += 24 + SC_GROUPS[g].count * row_h + 12;
    int pw = ncols * col_w + (ncols - 1) * gap + 48;
    int ph = SV_MAX(hcol[0], hcol[1]) + 58 + 10;
    Rect r = {(W - pw) / 2, SV_MAX(12, (H - ph) / 2), pw, SV_MIN(ph, H - 24)};
    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 4, r.w + 4, r.h + 4}, ui_alpha(TH_BG, 200), 12);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 10);
    platform_set_clip(win, r);

    platform_draw_text_ex(win, "Keyboard shortcuts", -1, (Point){r.x + 24, r.y + 18}, TH_TEXT, 16, FONT_UI_BOLD);
    const char* close = "F1 or Esc to close";
    int cw = platform_measure_text_ex(win, close, -1, TH_FONT_SMALL + 1, FONT_UI);
    platform_draw_text_ex(win, close, -1, (Point){r.x + r.w - 24 - cw, r.y + 22}, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);

    int y0 = r.y + 58;
    int y[2] = {y0, y0};
    for (int g = 0; g < ngroups; g++) {
        int c = g < split ? 0 : 1;
        int x = r.x + 24 + c * (col_w + gap);
        const ShortcutGroup* grp = &SC_GROUPS[g];
        platform_draw_text_ex(win, grp->title, -1, (Point){x, y[c]}, TH_ACCENT_TEXT, TH_FONT_SMALL, FONT_UI_BOLD);
        y[c] += 24;
        for (int i = 0; i < grp->count; i++) {
            Rect kr = {x, y[c], 150, row_h};
            ui_text_clipped(win, grp->items[i].keys, kr.x, ui_text_y(win, kr, TH_FONT_SMALL + 1, FONT_MONO), 150,
                            TH_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
            ui_text_clipped(win, grp->items[i].what, x + 158, ui_text_y(win, kr, TH_FONT_UI, FONT_UI), col_w - 158,
                            TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
            y[c] += row_h;
        }
        y[c] += 12;
    }
    platform_reset_clip(win);
}

// ── layout ───────────────────────────────────────────────────────

// ── toolbar adattiva ─────────────────────────────────────────────

// testo dei pulsanti per livello di compattezza: completo, senza scorciatoia, solo icona
typedef struct { const char* label; const char* hint; bool has_icon; } ToolbarText;

static const ToolbarText TOOLBAR_TEXT[] = {
    { "Run", "F5", true }, { "Run statement", "Ctrl+Enter", true }, { "Commit", NULL, true },
    { "Rollback", NULL, true }, { "Format", "Ctrl+Shift+F", false },
};

static void toolbar_buttons(AppWindow* app, Button** out) {
    out[0] = app->btn_run; out[1] = app->btn_run_current; out[2] = app->btn_commit;
    out[3] = app->btn_rollback; out[4] = app->btn_format;
}

static void apply_toolbar_level(AppWindow* app, int level) {
    Button* b[5];
    toolbar_buttons(app, b);
    for (int i = 0; i < 5; i++) {
        const ToolbarText* t = &TOOLBAR_TEXT[i];
        const char* label = t->label;
        const char* hint  = t->hint;
        if (i == 0 && app->job) { label = "Stop"; hint = "Esc"; }
        if (level >= 2 && t->has_icon) label = "";
        if (level >= 3 && !t->has_icon) label = "Fmt";
        button_set_label(b[i], label);
        button_set_hint(b[i], level == 0 ? hint : NULL);
    }
}

// larghezza del testo nella pillola del database
static int db_pill_text_w(AppWindow* app) {
    const char* db = app->lbl_db_name->text;
    bool has_db = strcmp(db, "no database") != 0;
    int cap = app->compact >= 3 ? 80 : app->compact >= 1 ? 140 : 220;
    return SV_MIN(platform_measure_text_ex(app->win, db, -1, TH_FONT_UI, has_db ? FONT_UI_BOLD : FONT_UI), cap);
}

static int theme_pill_w(AppWindow* app) {
    if (app->compact >= 2) return 44;   // solo campione + freccia
    const Theme* t = theme_get(theme_current());
    return platform_measure_text_ex(app->win, t->name, -1, TH_FONT_UI, FONT_UI) + 58;
}

// spazio occupato a destra (tema, transazione, database) con il livello corrente
static int toolbar_right_w(AppWindow* app) {
    int w = 16 + db_pill_text_w(app) + 46 + 8 + theme_pill_w(app) + 12;
    if (app->in_transaction) w += (app->compact >= 2 ? 22 : 104) + 8;
    return w;
}

static void layout_toolbar(AppWindow* app) {
    int bh = 32, by = (TOOLBAR_H - bh) / 2;
    Button* b[5];
    toolbar_buttons(app, b);
    for (int level = 0; level <= 3; level++) {
        app->compact = level;
        apply_toolbar_level(app, level);
        int x = app->side_w + 14;
        for (int i = 0; i < 5; i++) {
            int w = button_preferred_width(b[i], app->win);
            if (level >= 2 && TOOLBAR_TEXT[i].has_icon) w = bh + 2;   // pulsante quadrato con sola icona
            b[i]->base.bounds = (Rect){x, by, w, bh};
            x += w + ((i == 1 || i == 3) ? 22 : 8); // spazio extra tra i gruppi
        }
        if (x + toolbar_right_w(app) <= app->win_w) break;
    }
}

// ── tooltip ──────────────────────────────────────────────────────

#define TIP_DELAY_MS 450
enum { TIP_RUN, TIP_RUN_CURRENT, TIP_COMMIT, TIP_ROLLBACK, TIP_FORMAT, TIP_REFRESH, TIP_THEME, TIP_COUNT };

static const char* TIP_TEXT[TIP_COUNT] = {
    "Run script or selection  \xc2\xb7  F5", "Run statement under cursor  \xc2\xb7  Ctrl+Enter",
    "Commit transaction", "Rollback transaction", "Format SQL  \xc2\xb7  Ctrl+Shift+F",
    "Refresh explorer", "Color theme  \xc2\xb7  Ctrl+Shift+T",
};

static Rect tip_rect_of(AppWindow* app, int t) {
    switch (t) {
        case TIP_RUN:         return app->btn_run->base.bounds;
        case TIP_RUN_CURRENT: return app->btn_run_current->base.bounds;
        case TIP_COMMIT:      return app->btn_commit->base.bounds;
        case TIP_ROLLBACK:    return app->btn_rollback->base.bounds;
        case TIP_FORMAT:      return app->btn_format->base.bounds;
        case TIP_REFRESH:     return app->btn_refresh->base.bounds;
        default:              return app->theme_btn;
    }
}

// i pulsanti con testo e scorciatoia visibili non hanno bisogno del tooltip
static bool tip_needed(AppWindow* app, int t) {
    if (t <= TIP_FORMAT) return app->compact >= 1;
    return true;
}

static void tip_update(AppWindow* app, int mx, int my) {
    int target = -1;
    if (!app->theme_menu_open && !app->help_open)
        for (int t = 0; t < TIP_COUNT && target < 0; t++)
            if (tip_needed(app, t) && rect_has(tip_rect_of(app, t), mx, my)) target = t;
    if (target != app->tip_target) {
        app->tip_target  = target;
        app->tip_since   = platform_time_ms();
        app->tip_visible = false;
    }
}

static void draw_tooltip(AppWindow* app) {
    if (!app->tip_visible || app->tip_target < 0) return;
    PlatformWindow* win = app->win;
    const char* text = TIP_TEXT[app->tip_target];
    if (app->tip_target == TIP_RUN && app->job) text = "Stop the running query  \xc2\xb7  Esc";
    Rect a = tip_rect_of(app, app->tip_target);
    int tw = platform_measure_text_ex(win, text, -1, TH_FONT_SMALL + 1, FONT_UI);
    Rect r = {a.x + a.w / 2 - (tw + 20) / 2, a.y + a.h + 6, tw + 20, 26};
    r.x = SV_CLAMP(r.x, 6, app->win_w - r.w - 6);
    platform_fill_rect_rounded(win, (Rect){r.x - 1, r.y + 2, r.w + 2, r.h + 2}, ui_alpha(TH_BG, 150), 7);
    ui_round_box(win, r, TH_ELEVATED, TH_BORDER_STRONG, 6);
    platform_draw_text_ex(win, text, -1, (Point){r.x + 10, ui_text_y(win, r, TH_FONT_SMALL + 1, FONT_UI)},
                          TH_TEXT, TH_FONT_SMALL + 1, FONT_UI);
}

// ricalcola la posizione e dimensione di tutti i widget in base alle dimensioni correnti della finestra
static void layout_widgets(AppWindow* app) {
    int W = app->win_w;
    int H = app->win_h;

    // sidebar: larghezza scelta dall'utente, ridotta se la finestra è stretta
    if (app->sidebar_w <= 0) app->sidebar_w = SIDEBAR_W;
    app->side_w = SV_CLAMP(app->sidebar_w, SIDEBAR_MIN_W, SV_MAX(SIDEBAR_MIN_W, W - CENTER_MIN_W));

    // toolbar
    int bh = 32, by = (TOOLBAR_H - bh) / 2;
    layout_toolbar(app);
    app->lbl_db_name->base.bounds = (Rect){W - 210, by, 196, bh};

    // sidebar
    int side_top = TOOLBAR_H + SIDEBAR_HEADER_H;
    int side_h   = H - side_top - FOOTER_H;
    app->tree->base.bounds        = (Rect){0, side_top, app->side_w - 1, side_h};
    app->tree_scroll->base.bounds = (Rect){app->side_w - 1 - SCROLLBAR_W, side_top, SCROLLBAR_W, side_h};
    app->btn_refresh->base.bounds = (Rect){app->side_w - 38, TOOLBAR_H + 5, 28, 28};

    // area centrale: divisa dallo splitter
    int min_y = TOOLBAR_H + 90;
    int max_y = H - FOOTER_H - STATUSBAR_H - SPLITTER_H - 90;
    if (max_y < min_y) max_y = min_y;
    app->splitter_y = SV_CLAMP(app->splitter_y, min_y, max_y);

    int center_x  = app->side_w;
    int center_w  = W - app->side_w;
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
    app->lbl_status->clip = true;   // testo lungo troncato con "…"
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
    } else if (app->sidebar_drag || app->sidebar_hover) {
        k = CURSOR_RESIZE_H;
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
        if (rect_has(app->theme_btn, mx, my)) k = CURSOR_HAND;
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

    // pannello scorciatoie aperto: qualsiasi tasto o click lo chiude
    if (app->help_open) {
        if (evt->type == EVT_KEY_DOWN || evt->type == EVT_MOUSE_DOWN) app->help_open = false;
        if (evt->type != EVT_MOUSE_MOVE) return;
    }

    if (evt->type == EVT_MOUSE_MOVE) {
        app->mouse_x = evt->mouse_x;
        app->mouse_y = evt->mouse_y;
        tip_update(app, evt->mouse_x, evt->mouse_y);
    }
    if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_KEY_DOWN) {   // qualsiasi azione nasconde il tooltip
        app->tip_visible = false;
        app->tip_since = platform_time_ms() + 100000;
    }
    if (theme_handle_event(app, evt)) {
        if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN)
            platform_set_cursor(app->win, rect_has(app->theme_menu, evt->mouse_x, evt->mouse_y) ||
                                          rect_has(app->theme_btn, evt->mouse_x, evt->mouse_y) ? CURSOR_HAND : CURSOR_ARROW);
        return;
    }

    // -- scorciatoie globali --
    if (evt->type == EVT_KEY_DOWN && evt->key == KEY_F1) {
        app->help_open = true;
        return;
    }
    if (evt->type == EVT_KEY_DOWN) {
        bool ctrl  = evt->modifiers & SV_MOD_CTRL;
        bool shift = evt->modifiers & SV_MOD_SHIFT;
        // Esc ferma la query in esecuzione (se non serve a chiudere popup, barra o selezione)
        if (evt->key == KEY_ESCAPE && app->job && !bar_visible(app) && !app->editor->popup_open &&
            !textbox_has_selection(app->editor)) {
            app_window_cancel(app);
            return;
        }
        if (evt->key == KEY_F5) { app_window_run(app, RUN_SCRIPT); return; }
        if (evt->key == KEY_LETTER && ctrl && !(evt->modifiers & SV_MOD_ALT)) {
            switch (evt->character) {
                case 'f': if (shift) format_sql(app); else bar_open(app, BAR_FIND, false); return;
                case 'h': if (shift) history_toggle(app); else bar_open(app, BAR_FIND, true); return;
                case 'g': bar_open(app, BAR_GOTO, false); return;
                case 't':
                    if (shift) { set_theme(app, (theme_current() + 1) % theme_count(), true); return; }
                    break;
                case 'o': bar_open(app, BAR_OPEN, false); return;
                case 'n': cmd_new(app); return;
                case 'e':
                    if (app->results->result && app->results->result->num_cols > 0 &&
                        app->results->msg_kind == TABLE_MSG_NONE) bar_open(app, BAR_EXPORT, false);
                    else set_status(app, STATUS_INFO, "Nothing to export: run a query first", "");
                    return;
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

    // -- bordo della sidebar (trascinabile) --
    if (evt->type == EVT_MOUSE_MOVE) {
        app->sidebar_hover = abs(evt->mouse_x - app->side_w) <= 3 &&
                             evt->mouse_y > TOOLBAR_H && evt->mouse_y < app->win_h - FOOTER_H;
        if (app->sidebar_drag) {
            app->sidebar_w = SV_CLAMP(evt->mouse_x, SIDEBAR_MIN_W, SV_MAX(SIDEBAR_MIN_W, app->win_w - CENTER_MIN_W));
            layout_widgets(app);
            platform_set_cursor(app->win, CURSOR_RESIZE_H);
            return;
        }
    }
    if (evt->type == EVT_MOUSE_DOWN && evt->mouse_button == SV_MOUSE_LEFT && app->sidebar_hover) {
        app->sidebar_drag = true;
        return;
    }
    if (evt->type == EVT_MOUSE_UP && app->sidebar_drag) {
        app->sidebar_drag = false;
        return;
    }

    // -- splitter --
    int center_x = app->side_w;
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
    // esecuzione in background: completamento e stato con il tempo trascorso
    if (app->job) {
        if (atomic_load(&app->job->done)) {
            finish_run(app);
            return true;
        }
        if (now_ms - app->job_last_status >= 100) {
            app->job_last_status = now_ms;
            char msg[96], meta[48];
            int cur = atomic_load(&app->job->current);
            if (atomic_load(&app->job->cancel))  snprintf(msg, sizeof(msg), "Cancelling\xe2\x80\xa6");
            else if (app->job->total_code > 1)   snprintf(msg, sizeof(msg), "Running statement %d of %d\xe2\x80\xa6",
                                                          SV_MAX(cur, 1), app->job->total_code);
            else                                 snprintf(msg, sizeof(msg), "Running\xe2\x80\xa6");
            snprintf(meta, sizeof(meta), "%.1f s", (now_ms - app->job_started) / 1000.0);
            set_status(app, STATUS_INFO, msg, meta);
            return true;
        }
    }
    bool d = false;
    if (app->tip_target >= 0 && !app->tip_visible && now_ms >= app->tip_since + TIP_DELAY_MS) {
        app->tip_visible = true;
        d = true;
    }
    return a || b || c || d;
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
    tw = db_pill_text_w(app);
    Rect pill = {W - 16 - (tw + 46), TOOLBAR_H / 2 - 15, tw + 46, 30};
    ui_round_box(win, pill, TH_ELEVATED, TH_BORDER_STRONG, 15);
    ui_icon_database(win, pill.x + 18, pill.y + 15, 14, has_db ? TH_ICON_DB : TH_TEXT_FAINT);
    ui_text_clipped(win, db, pill.x + 32, ui_text_y(win, pill, TH_FONT_UI, FONT_UI_BOLD), tw,
                    has_db ? TH_TEXT : TH_TEXT_DIM, TH_FONT_UI, has_db ? FONT_UI_BOLD : FONT_UI);

    int left = pill.x;
    if (app->in_transaction) {
        const char* t = "TRANSACTION";
        bool small = app->compact >= 2;   // solo il pallino
        int w = small ? 22 : platform_measure_text_ex(win, t, -1, TH_FONT_SMALL, FONT_UI_BOLD) + 26;
        Rect b = {pill.x - w - 8, TOOLBAR_H / 2 - 11, w, 22};
        left = b.x;
        platform_fill_rect_rounded(win, b, TH_WARNING_SOFT, 11);
        ui_circle(win, b.x + 11, b.y + 11, 3, TH_WARNING);
        if (!small)
            platform_draw_text_ex(win, t, -1, (Point){b.x + 19, ui_text_y(win, b, TH_FONT_SMALL, FONT_UI_BOLD)},
                                  TH_WARNING, TH_FONT_SMALL, FONT_UI_BOLD);
    }
    draw_theme_button(app, left - 8);
}

static void draw_sidebar(AppWindow* app) {
    PlatformWindow* win = app->win;
    Rect hdr = {0, TOOLBAR_H, app->side_w - 1, SIDEBAR_HEADER_H};
    platform_fill_rect(win, hdr, TH_SURFACE);
    platform_draw_text_ex(win, "EXPLORER", -1,
                          (Point){18, ui_text_y(win, hdr, TH_FONT_SMALL, FONT_UI_BOLD)},
                          TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    widget_draw((Widget*)app->btn_refresh, win);
    widget_draw((Widget*)app->tree, win);
    widget_draw((Widget*)app->tree_scroll, win);

    // bordo destro della sidebar (evidenziato quando si può trascinare)
    bool active = app->sidebar_drag || app->sidebar_hover;
    platform_fill_rect(win, (Rect){app->side_w - (active ? 2 : 1), TOOLBAR_H, active ? 2 : 1,
                                   app->win_h - TOOLBAR_H - FOOTER_H}, active ? TH_ACCENT : TH_BORDER);
}

static void draw_splitter_and_status(AppWindow* app) {
    PlatformWindow* win = app->win;
    int cx = app->side_w, cw = app->win_w - app->side_w;
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

    widget_draw((Widget*)app->btn_history, win);
    if (app->showing_history) {
        Rect hb = app->btn_history->base.bounds;
        platform_fill_rect(win, (Rect){hb.x + 8, hb.y + hb.h, hb.w - 16, 2}, TH_ACCENT);
    }

    // il messaggio ha la precedenza sui dettagli (righe, tempo): se manca spazio i dettagli spariscono
    Rect* lb = &app->lbl_status->base.bounds;
    int right = app->btn_history->base.bounds.x - 14;
    if (app->status_meta[0]) {
        int mw = platform_measure_text_ex(win, app->status_meta, -1, TH_FONT_SMALL + 1, FONT_MONO);
        int msg_w = platform_measure_text_ex(win, app->lbl_status->text, -1, TH_FONT_UI, FONT_UI);
        if (right - mw - 16 - lb->x >= SV_MIN(msg_w, 160)) {
            platform_draw_text_ex(win, app->status_meta, -1,
                                  (Point){right - mw, ui_text_y(win, st, TH_FONT_SMALL + 1, FONT_MONO)},
                                  TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
            right -= mw + 16;
        }
    }
    lb->w = SV_MAX(0, right - lb->x);
    widget_draw((Widget*)app->lbl_status, win);
}

static void draw_footer(AppWindow* app) {
    PlatformWindow* win = app->win;
    int W = app->win_w, H = app->win_h;
    Rect f = {0, H - FOOTER_H, W, FOOTER_H};
    platform_fill_rect(win, f, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){0, f.y, W, 1}, TH_BORDER);

    int fs = TH_FONT_SMALL + 1;
    int ty = ui_text_y(win, f, fs, FONT_UI);
    int cy = f.y + FOOTER_H / 2;
    char buf[256];

    // -- destra: la variante più ricca che lascia almeno ~45% della larghezza alla parte sinistra --
    TextBox* tb = app->editor;
    int sa, se;
    textbox_get_selection_range(tb, &sa, &se);
    char sel[48] = "";
    if (se > sa) snprintf(sel, sizeof(sel), " (%d selected)", se - sa);
    char variants[3][160];
    snprintf(variants[0], sizeof(variants[0]), "Ln %d, Col %d%s    %d lines    UTF-8    SQL",
             tb->cursor_line + 1, tb->cursor_col + 1, sel, tb->num_lines);
    snprintf(variants[1], sizeof(variants[1]), "Ln %d, Col %d%s", tb->cursor_line + 1, tb->cursor_col + 1, sel);
    snprintf(variants[2], sizeof(variants[2]), "%d:%d", tb->cursor_line + 1, tb->cursor_col + 1);
    const char* help = W >= 700 ? "F1 Shortcuts" : "F1";
    int hw = platform_measure_text_ex(win, help, -1, fs, FONT_UI_BOLD);
    int budget = W - 32 - hw - 22 - W * 45 / 100;
    int v = 0;
    while (v < 2 && platform_measure_text_ex(win, variants[v], -1, fs, FONT_UI) > budget) v++;
    int rw = platform_measure_text_ex(win, variants[v], -1, fs, FONT_UI);
    platform_draw_text_ex(win, help, -1, (Point){W - 16 - hw, ty}, TH_ACCENT_TEXT, fs, FONT_UI_BOLD);
    int right_x = W - 16 - hw - 22 - rw;
    platform_draw_text_ex(win, variants[v], -1, (Point){right_x, ty}, TH_TEXT_DIM, fs, FONT_UI);
    int limit = right_x - 18;   // la parte sinistra non deve superare questo punto

    // -- sinistra, in ordine di importanza: database, file, transazione; il nome del motore solo se c'è posto --
    const char* db = app->lbl_db_name->text;
    const char* fname = app->file_path[0] ? base_name(app->file_path) : "untitled.sql";
    snprintf(buf, sizeof(buf), "%s engine %s", SV_NAME, SV_VERSION_STR);
    int engine_w = 12 + platform_measure_text_ex(win, buf, -1, fs, FONT_UI) + 18;
    int db_w   = 16 + platform_measure_text_ex(win, db, -1, fs, FONT_UI) + 18;
    int file_w = platform_measure_text_ex(win, fname, -1, fs, FONT_UI) + (app->modified ? 14 : 0) + 18;
    int tx_w   = app->in_transaction ? 12 + platform_measure_text_ex(win, "Transaction open", -1, fs, FONT_UI) : 0;
    bool show_engine = 16 + engine_w + db_w + file_w + tx_w <= limit;

    int x = 16;
    if (show_engine) {
        ui_circle(win, x + 3, cy, 3, TH_SUCCESS);
        x += 12;
        x += platform_draw_text_ex(win, buf, -1, (Point){x, ty}, TH_TEXT_DIM, fs, FONT_UI) + 18;
    }
    ui_icon_database(win, x + 6, cy, 11, TH_TEXT_DIM);
    x += 16;
    x += ui_text_clipped(win, db, x, ty, SV_MAX(0, SV_MIN(160, limit - x)), TH_TEXT_MUTED, fs, FONT_UI) + 18;

    // file corrente (● = modifiche non salvate)
    int room = limit - x - (app->modified ? 14 : 0) - (app->in_transaction ? 30 : 0);
    if (room > 30) {
        x += ui_text_clipped(win, fname, x, ty, room, app->file_path[0] ? TH_TEXT_MUTED : TH_TEXT_DIM, fs, FONT_UI);
        if (app->modified) { ui_circle(win, x + 8, cy, 3, TH_ACCENT_TEXT); x += 14; }
        x += 18;
    }
    if (app->in_transaction && x + 12 < limit) {
        ui_circle(win, x + 3, cy, 3, TH_WARNING);
        x += 12;
        ui_text_clipped(win, "Transaction open", x, ty, SV_MAX(0, limit - x), TH_WARNING, fs, FONT_UI);
    }
}

void app_window_draw(AppWindow* app) {
    if (!app) return;
    PlatformWindow* win = app->win;

    sync_scrollbars(app);
    layout_toolbar(app);   // nome del DB, transazione e tema cambiano lo spazio disponibile

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
    draw_theme_menu(app);
    draw_tooltip(app);
    draw_help(app);

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

// ── esecuzione in background ─────────────────────────────────────
// Il worker esegue gli statement e accumula i risultati in RunJob senza toccare
// lo stato della GUI; il thread principale (app_window_tick) li mostra a fine corsa.
// Durante l'esecuzione la GUI non chiama il motore (explorer, commit/rollback bloccati).


static void run_worker(void* arg) {
    RunJob* j = (RunJob*)arg;
    for (int i = 0; i < j->n; i++) {
        SqlRange* r = &j->ranges[i];
        if (!r->has_code) continue;
        if (atomic_load(&j->cancel)) { j->cancelled = true; break; }
        int slen = r->end_semi - r->code_start;
        char* stmt = malloc(slen + 1);
        if (!stmt) break;
        memcpy(stmt, j->text + r->code_start, slen);
        stmt[slen] = '\0';
        atomic_store(&j->current, j->executed + 1);

        QueryResult* res = db_execute(stmt);
        j->executed++;
        j->log[j->log_n++] = (RunLog){ stmt, res && res->success,
                                       res ? (res->num_cols > 0 ? res->num_rows : res->rows_affected) : 0,
                                       res ? res->exec_time_ms : 0 };
        if (!res || !res->success) {
            j->failed = res;
            j->failed_idx = j->executed;
            j->failed_offset = r->code_start;
            break;
        }
        j->total_ms += res->exec_time_ms;
        if (res->num_cols > 0) {
            if (j->grid) db_result_free(j->grid);
            j->grid = res;
        } else {
            j->affected += res->rows_affected;
            j->changed = true;
            db_result_free(res);
        }
    }
    atomic_store(&j->done, 1);
}

static void job_free(RunJob* j) {
    if (!j) return;
    for (int i = 0; i < j->log_n; i++) free(j->log[i].sql);
    free(j->log);
    free(j->ranges);
    free(j->text);
    if (j->grid) db_result_free(j->grid);
    if (j->failed) db_result_free(j->failed);
    free(j);
}

// aspetto dei comandi mentre una query è in esecuzione
static void style_run_button(AppWindow* app) {
    Button* b = app->btn_run;
    button_set_style(b, BUTTON_STYLE_PRIMARY);
    if (app->job) {
        button_set_icon(b, BUTTON_ICON_STOP, (Color){0, 0, 0, 0});
        b->color_bg = b->color_border = TH_ERROR;
        b->color_hover   = ui_mix(TH_ERROR, (Color){255, 255, 255, 255}, 0.15f);
        b->color_pressed = ui_mix(TH_ERROR, (Color){0, 0, 0, 255}, 0.15f);
        b->color_text    = (Color){255, 255, 255, 255};
    } else {
        button_set_icon(b, BUTTON_ICON_PLAY, (Color){0, 0, 0, 0});
    }
    bool idle = app->job == NULL;
    button_set_enabled(app->btn_run_current, idle);
    button_set_enabled(app->btn_commit, idle);
    button_set_enabled(app->btn_rollback, idle);
    button_set_enabled(app->btn_refresh, idle);
}

bool app_window_busy(AppWindow* app) { return app && app->job; }

// messaggio standard quando si prova a usare il motore durante un'esecuzione
static bool busy_guard(AppWindow* app) {
    if (!app->job) return false;
    set_status(app, STATUS_INFO, "A query is running \xc2\xb7 press Stop (or Esc) to cancel it", "");
    return true;
}

void app_window_cancel(AppWindow* app) {
    if (!app || !app->job) return;
    atomic_store(&app->job->cancel, 1);
    db_request_cancel();
    set_status(app, STATUS_INFO, "Cancelling\xe2\x80\xa6", "SELECT stops now, other statements finish first");
}

void app_window_run(AppWindow* app, RunMode mode) {
    if (!app) return;
    if (busy_guard(app)) return;
    TextBox* tb = app->editor;

    // 1. testo da eseguire e suo offset nel documento (per mappare gli errori)
    char* text = NULL;
    int base = 0;
    if (mode == RUN_CURRENT || textbox_has_selection(tb)) {
        base = textbox_get_run_text(tb, &text);
        if (base < 0 || !text) {
            set_status(app, STATUS_INFO, "Nothing to run: the cursor is not on a statement", "");
            return;
        }
    } else {
        text = strdup(textbox_get_text(tb));
        if (!text) return;
    }

    // 2. divisione in statement (i ';' dentro stringhe e commenti sono ignorati)
    // al massimo un intervallo per ';' più uno finale: nessun limite fisso al numero di statement
    int max_ranges = 2;
    for (const char* c = text; *c; c++) if (*c == ';') max_ranges++;
    RunJob* j = calloc(1, sizeof(RunJob));
    SqlRange* ranges = malloc(sizeof(SqlRange) * max_ranges);
    if (!j || !ranges) { free(j); free(ranges); free(text); return; }
    j->text = text;
    j->ranges = ranges;
    j->n = sql_split(text, (int)strlen(text), ranges, max_ranges);
    for (int i = 0; i < j->n; i++) if (ranges[i].has_code) j->total_code++;
    if (j->total_code == 0) {
        job_free(j);
        set_status(app, STATUS_INFO, "Nothing to run", "");
        return;
    }
    j->log = calloc(j->total_code, sizeof(RunLog));
    if (!j->log) { job_free(j); return; }

    // 3. avvio del thread di lavoro
    textbox_set_error_line(tb, -1);
    db_clear_cancel();
    app->job = j;
    app->job_base_line = textbox_line_of_offset(tb, base);
    app->job_started = app->job_last_status = platform_time_ms();
    app->job_thread = platform_thread_start(run_worker, j);
    if (!app->job_thread) {          // impossibile creare il thread: esecuzione diretta
        run_worker(j);
    }
    style_run_button(app);
    set_status(app, STATUS_INFO, "Running\xe2\x80\xa6", "");
}

// a esecuzione terminata (thread principale): cronologia, risultati, errori
static void finish_run(AppWindow* app) {
    RunJob* j = app->job;
    if (app->job_thread) platform_thread_join(app->job_thread);
    app->job_thread = NULL;
    app->job = NULL;
    style_run_button(app);

    TextBox* tb = app->editor;
    for (int i = 0; i < j->log_n; i++) history_add(app, j->log[i].sql, j->log[i].ok, j->log[i].rows, j->log[i].ms);

    double wall = (double)(platform_time_ms() - app->job_started);
    char msg[1200], meta[160];
    // annullata davvero solo se uno statement è stato interrotto o saltato: se lo Stop arriva
    // quando tutto è già finito, si mostrano normalmente i risultati
    bool cancelled_select = j->failed && strcmp(j->failed->error, "Query cancelled") == 0;

    if (j->failed_idx > 0 && !cancelled_select) {
        // errore: evidenzia la riga dello statement nell'editor
        int line = app->job_base_line;
        for (int k = 0; k < j->failed_offset && j->text[k]; k++) if (j->text[k] == '\n') line++;
        textbox_set_error_line(tb, line);
        textbox_scroll_to_line(tb, line);
        const char* err = j->failed ? j->failed->error : "No response from the engine";

        char title[128];
        if (j->total_code > 1) snprintf(title, sizeof(title), "Statement %d of %d failed", j->failed_idx, j->total_code);
        else                   snprintf(title, sizeof(title), "Query failed");
        snprintf(msg, sizeof(msg), "Line %d: %s", line + 1, err);
        snprintf(meta, sizeof(meta), "%d of %d executed", j->failed_idx - 1, j->total_code);
        show_result(app, NULL);
        table_view_set_message(app->results, TABLE_MSG_ERROR, title, err);
        set_status(app, STATUS_ERROR, msg, meta);
    } else if (j->cancelled || cancelled_select) {
        int done_ok = j->executed - (cancelled_select ? 1 : 0);
        snprintf(msg, sizeof(msg), "Cancelled after %d of %d statement%s", done_ok, j->total_code,
                 j->total_code == 1 ? "" : "s");
        snprintf(meta, sizeof(meta), "%.1f s", wall / 1000.0);
        show_result(app, NULL);
        table_view_set_message(app->results, TABLE_MSG_ERROR, "Execution cancelled",
                               "Statements already completed were applied; the rest was not executed.");
        set_status(app, STATUS_INFO, msg, meta);
    } else if (j->grid) {
        QueryResult* grid = j->grid;
        j->grid = NULL;
        show_result(app, grid);
        if (j->executed > 1) snprintf(msg, sizeof(msg), "%d statements executed, showing last result", j->executed);
        else                 snprintf(msg, sizeof(msg), "%d row%s returned", grid->num_rows, grid->num_rows == 1 ? "" : "s");
        snprintf(meta, sizeof(meta), "%d rows  \xc2\xb7  %d cols  \xc2\xb7  %.1f ms",
                 grid->num_rows, grid->num_cols, j->total_ms);
        set_status(app, STATUS_OK, msg, meta);
    } else {
        show_result(app, NULL);
        char detail[160];
        snprintf(detail, sizeof(detail), "%d row%s affected  \xc2\xb7  %.1f ms",
                 j->affected, j->affected == 1 ? "" : "s", j->total_ms);
        if (j->executed > 1) snprintf(msg, sizeof(msg), "%d statements executed", j->executed);
        else                 snprintf(msg, sizeof(msg), "Query OK");
        table_view_set_message(app->results, TABLE_MSG_OK, msg, detail);
        snprintf(meta, sizeof(meta), "%.1f ms", j->total_ms);
        set_status(app, STATUS_OK, msg, meta);
    }

    bool changed = j->changed;
    job_free(j);
    db_clear_cancel();

    // aggiornamento nome DB (potrebbe essere cambiato con USE) e albero se il DDL ha modificato la struttura
    update_session(app);
    if (changed) refresh_tree(app);
}

void app_window_destroy(AppWindow* app) {
    if (!app) return;
    if (app->job) {   // chiusura durante un'esecuzione: annulla e attende il thread prima di chiudere il motore
        atomic_store(&app->job->cancel, 1);
        db_request_cancel();
        if (app->job_thread) platform_thread_join(app->job_thread);
        app->job_thread = NULL;
        finish_run(app);
    }
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
            else if (!strncmp(line, "sidebar=", 8)) { app->sidebar_w = atoi(line + 8); layout_widgets(app); }
            else if (!strncmp(line, "theme=", 6)) {
                int ti = theme_find(line + 6);
                if (ti >= 0) set_theme(app, ti, false);
            }
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
    fprintf(f, "file=%s\ncursor=%d\nmodified=%d\nfont=%d\ntheme=%s\nsidebar=%d\n",
            app->file_path, app->editor->cursor_pos, app->modified ? 1 : 0, app->editor->font_size,
            theme_get(theme_current())->name, app->sidebar_w);
    fclose(f);
}
