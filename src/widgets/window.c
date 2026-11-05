#include "window.h"
#include "../platform/platform.h"
#include "label.h"
#include "theme.h"
#include "sql_text.h"
#include "settings.h"
#include "result_export.h"
#include "csv_import.h"
#include <stdio.h>
#include <strings.h>
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
static void cell_view_open(AppWindow* app);
static void run_text(AppWindow* app, char* text, int base, bool from_editor, const char* label);
static ExportOptions export_options(const char* table);
static bool import_csv(AppWindow* app, const char* path);
static bool run_file(AppWindow* app, const char* path);
static bool dump_database(AppWindow* app, const char* path);

// esecuzione in background (vedi app_window_run)
typedef struct { char* sql; bool ok; int rows; double ms; } RunLog;

typedef struct RunJob {
    char*        text;            // copia del testo eseguito
    SqlRange*    ranges;
    int          n;
    int          total_code;      // statement con codice
    RunLog*      log;             // uno per statement eseguito (per la cronologia)
    int          log_n;
    QueryResult* grids[MAX_RESULT_TABS];   // risultati tabellari (gli ultimi MAX_RESULT_TABS)
    int          ngrids;
    QueryResult* failed;
    int          failed_idx, failed_offset;
    int          executed, affected;
    double       total_ms;
    bool         changed;         // DDL/DML eseguito: albero e completamento da aggiornare
    bool         cancelled;       // fermato prima di uno statement
    bool         keep_going;      // Settings: continua dopo un errore
    int          nerrors;         // statement falliti (con keep_going)
    char         label[96];       // descrizione per lo stato ("Import of clienti.csv", ...)
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

static void layout_widgets(AppWindow* app);

// imposta i risultati mostrati (uno per scheda); i precedenti vengono liberati dopo aver
// aggiornato la griglia, così la table view non punta mai a memoria liberata
static void set_result_tabs(AppWindow* app, QueryResult** rs, int n, int select) {
    QueryResult* old[MAX_RESULT_TABS];
    int nold = app->num_tabs;
    memcpy(old, app->tabs, sizeof(old));
    n = SV_CLAMP(n, 0, MAX_RESULT_TABS);
    for (int i = 0; i < n; i++) app->tabs[i] = rs[i];
    app->num_tabs = n;
    app->cur_tab  = n ? SV_CLAMP(select, 0, n - 1) : 0;
    app->last_result = n ? app->tabs[app->cur_tab] : NULL;
    app->showing_history = false;
    app->cell_open = false;
    app->ctx_kind  = 0;
    if (app->filter_input && app->filter_input->text[0]) line_edit_set_text(app->filter_input, "");
    table_view_set_filter(app->results, "", -1, false);
    table_view_set_result(app->results, app->last_result);
    for (int i = 0; i < nold; i++) {
        bool kept = false;
        for (int k = 0; k < n && !kept; k++) kept = app->tabs[k] == old[i];
        if (!kept && old[i]) db_result_free(old[i]);
    }
    layout_widgets(app);   // la barra delle schede compare solo con più risultati
}

static void show_result(AppWindow* app, QueryResult* r) {
    set_result_tabs(app, r ? &r : NULL, r ? 1 : 0, 0);
}

static void select_result_tab(AppWindow* app, int i) {
    if (i < 0 || i >= app->num_tabs || i == app->cur_tab) return;
    app->cur_tab = i;
    app->last_result = app->tabs[i];
    table_view_set_result(app->results, app->last_result);
    QueryResult* r = app->last_result;
    if (r) snprintf(app->status_meta, sizeof(app->status_meta), "%d rows  \xc2\xb7  %d cols  \xc2\xb7  %.1f ms",
                    r->num_rows, r->num_cols, r->exec_time_ms);
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

static void history_trim(AppWindow* app, int keep) {
    if (app->history_count <= keep) return;
    int drop = app->history_count - keep;
    for (int i = 0; i < drop; i++) free(app->history[i].sql);
    memmove(app->history, app->history + drop, sizeof(HistoryEntry) * keep);
    app->history_count = keep;
}

static void history_push(AppWindow* app, char* sql, const char* when, bool ok, int rows, double ms) {
    history_trim(app, SV_MIN(settings_history_limit(), HISTORY_MAX) - 1);
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
// sugli altri risultati apre il visualizzatore della cella
static void on_results_activate(int row, void* ud) {
    AppWindow* app = (AppWindow*)ud;
    if (!app->showing_history) { cell_view_open(app); return; }
    row = table_view_source_row(app->results, row);              // griglia filtrata / ordinata
    if (row < 0 || row >= app->history_count) return;
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
    scrollbar_set_range(app->results_scroll_v, grid ? table_view_row_count(tv) : 0, table_view_visible_rows(tv));
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
        case BAR_EXPORT:  return "Export results";
        case BAR_IMPORT_CSV: return "Import CSV";
        case BAR_RUN_FILE: return "Run SQL file";
        case BAR_DUMP:    return "Database dump";
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
    Rect r = {app->win_w - SCROLLBAR_W - 14 - bw, TOP_H + 8, bw, bh};
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
        app->bar_input->placeholder = "File path: .csv .tsv .json .md .html .sql choose the format";
        textbox_set_search(app->editor, NULL, false, false);
    } else if (mode == BAR_IMPORT_CSV || mode == BAR_RUN_FILE || mode == BAR_DUMP) {
        char buf[1100];
        const char* db = db_current_database();
        char name[200];
        if (mode == BAR_DUMP) snprintf(name, sizeof(name), "%.100s_dump.sql", db && db[0] ? db : "database");
        else name[0] = '\0';
        join_path(buf, sizeof(buf), app->data_dir[0] ? app->data_dir : ".", name);
        line_edit_set_text(app->bar_input, buf);
        app->bar_input->placeholder = mode == BAR_IMPORT_CSV ? "Path of the .csv file (the table takes the file name)"
                                    : mode == BAR_RUN_FILE   ? "Path of the .sql file to execute"
                                                             : "Path of the .sql dump to write";
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
    if (mode == BAR_OPEN || mode == BAR_SAVE_AS || mode == BAR_EXPORT || mode == BAR_IMPORT_CSV ||
        mode == BAR_RUN_FILE || mode == BAR_DUMP) app->bar_input->anchor = -1; // cursore in fondo
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
            ExportFormat fmt = export_format_from_path(path);
            csv_table_name_from_path(path, app->export_table, sizeof(app->export_table));
            ExportOptions o = export_options(app->export_table);
            char* out = export_result(app->results, fmt, &o);
            if (!out) { set_status(app, STATUS_INFO, "Nothing to export: run a query first", ""); bar_close(app); return; }
            bool ok = write_text_file(path, out);
            free(out);
            char msg[1200];
            if (ok) {
                snprintf(msg, sizeof(msg), "Exported %d rows as %s to %s", table_view_row_count(app->results),
                         export_format_name(fmt), path);
                set_status(app, STATUS_OK, msg, "");
                bar_close(app);
            } else {
                snprintf(msg, sizeof(msg), "Cannot write %s: %s", path, strerror(errno));
                set_status(app, STATUS_ERROR, msg, "");
                in->error = true;
            }
            break;
        }
        case BAR_IMPORT_CSV: case BAR_RUN_FILE: case BAR_DUMP: {
            char path[1024];
            expand_path(in->text, path, sizeof(path));
            size_t pl = strlen(path);
            if (!pl || path[pl - 1] == '/' || path[pl - 1] == '\\') { in->error = true; return; }
            if (app->bar_mode != BAR_DUMP && busy_guard(app)) return;
            bool ok = app->bar_mode == BAR_IMPORT_CSV ? import_csv(app, path)
                    : app->bar_mode == BAR_RUN_FILE   ? run_file(app, path)
                                                      : dump_database(app, path);
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

static void settings_store(AppWindow* app);

static void set_theme(AppWindow* app, int index, bool announce) {
    if (!theme_get(index)) return;
    theme_apply(index);
    g_settings.theme = index;
    settings_store(app);
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
    Rect b = {x_right - pw, MENUBAR_H + TOOLBAR_H / 2 - 15, pw, 30};
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

// ── griglia: cella selezionata, visualizzatore di cella, menu contestuale ──

static bool grid_ready(const AppWindow* app) {
    const TableView* tv = app->results;
    return tv->result && tv->result->num_cols > 0 && tv->msg_kind == TABLE_MSG_NONE;
}

// riga visualizzata in posizione i (tiene conto dell'ordinamento)
static Row* grid_row(AppWindow* app, int i) {
    TableView* tv = app->results;
    return &tv->result->rows[tv->order ? tv->order[i] : i];
}

static bool grid_has_row(AppWindow* app) {
    TableView* tv = app->results;
    return grid_ready(app) && tv->selected_row >= 0 && tv->selected_row < table_view_row_count(tv);
}

static bool grid_has_col(AppWindow* app) {
    TableView* tv = app->results;
    return grid_ready(app) && tv->selected_col >= 0 && tv->selected_col < tv->result->num_cols;
}

static Cell* grid_cell(AppWindow* app) {
    if (!grid_has_row(app) || !grid_has_col(app)) return NULL;
    Row* row = grid_row(app, app->results->selected_row);
    int c = app->results->selected_col;
    return c < row->num_cells ? &row->cells[c] : NULL;
}

static const char* cell_text(const Cell* c) { return c->is_null ? "NULL" : c->value; }

// buffer di testo che cresce
typedef struct { char* p; size_t n, cap; } SBuf;

static void sb_add(SBuf* b, const char* s) {
    size_t n = strlen(s);
    if (b->n + n + 1 > b->cap) {
        size_t cap = SV_MAX(b->cap * 2, b->n + n + 256);
        char* p = realloc(b->p, cap);
        if (!p) return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

// valore della cella come letterale SQL ('testo' con apici raddoppiati, numeri così come sono)
static char* sql_literal(const Cell* c) {
    if (c->is_null) return strdup("NULL");
    if (c->type == CELL_TYPE_INT || c->type == CELL_TYPE_FLOAT) return strdup(c->value);
    SBuf b = {0};
    sb_add(&b, "'");
    char one[2] = {0, 0};
    for (const char* s = c->value; *s; s++) {
        one[0] = *s;
        sb_add(&b, *s == '\'' ? "''" : one);
    }
    sb_add(&b, "'");
    return b.p;
}

static void copy_and_report(AppWindow* app, const char* text, const char* what) {
    if (!text) return;
    platform_set_clipboard(app->win, text);
    char msg[128];
    snprintf(msg, sizeof(msg), "Copied %s to the clipboard", what);
    set_status(app, STATUS_OK, msg, "");
}

// ── visualizzatore di cella ──────────────────────────────────────

#define CELL_VIEW_MAX_LINES 4096
#define CELL_VIEW_LINE_H    20

static void cell_view_open(AppWindow* app) {
    TableView* tv = app->results;
    if (!grid_has_row(app)) return;
    if (tv->selected_col < 0) tv->selected_col = 0;
    if (!grid_cell(app)) return;
    app->cell_open   = true;
    app->cell_scroll = 0;
    app->ctx_kind    = 0;
}

// spezza il testo in righe larghe al massimo max_w (a capo sugli spazi quando possibile)
static int wrap_lines(PlatformWindow* win, const char* s, int max_w, int size, FontStyle st,
                      int* starts, int* lens, int max_lines) {
    int n = 0, i = 0, len = (int)strlen(s);
    while (n < max_lines) {
        int j = i, last_space = -1;
        while (j < len && s[j] != '\n') {
            int k = j + 1;
            while (k < len && ((unsigned char)s[k] & 0xC0) == 0x80) k++;   // carattere UTF-8 intero
            if (j > i && platform_measure_text_ex(win, s + i, k - i, size, st) > max_w) break;
            if (s[j] == ' ') last_space = k;
            j = k;
        }
        starts[n] = i;
        if (j < len && s[j] != '\n') {                 // riga troppo lunga: a capo sull'ultimo spazio
            int end = last_space > i ? last_space : j;
            lens[n++] = end - i;
            i = end;
            continue;
        }
        lens[n++] = j - i;
        if (j >= len) break;
        i = j + 1;                                      // salta il '\n'
    }
    return SV_MAX(n, 1);
}

static void draw_cell_view(AppWindow* app) {
    if (!app->cell_open) return;
    Cell* cell = grid_cell(app);
    if (!cell) { app->cell_open = false; return; }
    PlatformWindow* win = app->win;
    TableView* tv = app->results;
    QueryResult* res = tv->result;
    int W = app->win_w, H = app->win_h;
    int c = tv->selected_col;

    platform_fill_rect(win, (Rect){0, 0, W, H}, ui_alpha(TH_BG, 170));
    int pw = SV_MIN(760, W - 32), ph = SV_MIN(500, H - 32);
    Rect r = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    app->cell_rect = r;
    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 4, r.w + 4, r.h + 4}, ui_alpha(TH_BG, 200), 12);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 10);
    platform_set_clip(win, r);

    // intestazione: colonna, tipo, posizione
    const char* name = res->col_names && res->col_names[c] ? res->col_names[c] : "?";
    const char* type = res->col_types && res->col_types[c] ? res->col_types[c] : "";
    if (!type[0] && !cell->is_null) {   // il motore non sempre riporta il tipo della colonna
        static const char* const NAMES[] = { [CELL_TYPE_INT] = "INT", [CELL_TYPE_FLOAT] = "FLOAT",
            [CELL_TYPE_STRING] = "TEXT", [CELL_TYPE_BOOL] = "BOOLEAN", [CELL_TYPE_DATE] = "DATE", [CELL_TYPE_BLOB] = "BLOB" };
        if ((int)cell->type >= 0 && (int)cell->type < (int)(sizeof(NAMES) / sizeof(NAMES[0])) && NAMES[cell->type])
            type = NAMES[cell->type];
    }
    char pos[96];
    snprintf(pos, sizeof(pos), "Row %d of %d  \xc2\xb7  Column %d of %d", tv->selected_row + 1, table_view_row_count(tv),
             c + 1, res->num_cols);
    int pos_w = platform_measure_text_ex(win, pos, -1, TH_FONT_SMALL + 1, FONT_UI);
    Rect hr = {r.x + 20, r.y + 14, r.w - 40, 26};
    int tw = ui_text_clipped(win, name, hr.x, ui_text_y(win, hr, 16, FONT_UI_BOLD), SV_MAX(60, hr.w - pos_w - 120),
                             TH_TEXT, 16, FONT_UI_BOLD);
    if (type[0]) {
        int bw = platform_measure_text_ex(win, type, -1, TH_FONT_SMALL, FONT_MONO) + 14;
        Rect badge = {hr.x + tw + 10, hr.y + 3, bw, 20};
        ui_round_box(win, badge, TH_ACCENT_SOFT, TH_ACCENT_SOFT, 10);
        platform_draw_text_ex(win, type, -1, (Point){badge.x + 7, ui_text_y(win, badge, TH_FONT_SMALL, FONT_MONO)},
                              TH_ACCENT_TEXT, TH_FONT_SMALL, FONT_MONO);
    }
    platform_draw_text_ex(win, pos, -1, (Point){hr.x + hr.w - pos_w, ui_text_y(win, hr, TH_FONT_SMALL + 1, FONT_UI)},
                          TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);

    // corpo: il valore, a capo automatico e scrollabile
    Rect body = {r.x + 20, r.y + 52, r.w - 40, r.h - 52 - 60};
    ui_round_box(win, body, TH_EDITOR_BG, TH_BORDER, 6);
    static int starts[CELL_VIEW_MAX_LINES], lens[CELL_VIEW_MAX_LINES];
    int inner_w = body.w - 32;
    int vis = SV_MAX(1, (body.h - 20) / CELL_VIEW_LINE_H);
    if (cell->is_null) {
        app->cell_lines = 1;
        platform_draw_text_ex(win, "NULL", -1, (Point){body.x + 14, body.y + 12}, TH_TEXT_FAINT, TH_FONT_MONO, FONT_MONO);
    } else {
        int n = wrap_lines(win, cell->value, inner_w, TH_FONT_MONO, FONT_MONO, starts, lens, CELL_VIEW_MAX_LINES);
        app->cell_lines  = n;
        app->cell_scroll = SV_CLAMP(app->cell_scroll, 0, SV_MAX(0, n - vis));
        platform_set_clip(win, (Rect){body.x + 1, body.y + 1, body.w - 2, body.h - 2});
        for (int i = app->cell_scroll; i < n && i < app->cell_scroll + vis + 1; i++) {
            int y = body.y + 10 + (i - app->cell_scroll) * CELL_VIEW_LINE_H;
            if (lens[i] > 0)
                platform_draw_text_ex(win, cell->value + starts[i], lens[i], (Point){body.x + 14, y + 2},
                                      TH_CELL_TEXT, TH_FONT_MONO, FONT_MONO);
        }
        platform_set_clip(win, r);
        if (n > vis) {   // indicatore di scroll
            int track = body.h - 12;
            int th = SV_MAX(24, track * vis / n);
            int ty = body.y + 6 + (track - th) * app->cell_scroll / SV_MAX(1, n - vis);
            platform_fill_rect_rounded(win, (Rect){body.x + body.w - 9, ty, 4, th}, TH_BORDER_STRONG, 2);
        }
    }

    // piede: statistiche e pulsanti
    Rect fr = {r.x + 20, r.y + r.h - 46, r.w - 40, 30};
    char info[128];
    if (cell->is_null) snprintf(info, sizeof(info), "NULL value  \xc2\xb7  arrows move  \xc2\xb7  Esc closes");
    else {
        int chars = 0;
        for (const char* s = cell->value; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) chars++;
        snprintf(info, sizeof(info), "%d character%s  \xc2\xb7  %d line%s  \xc2\xb7  arrows move  \xc2\xb7  Esc closes",
                 chars, chars == 1 ? "" : "s", app->cell_lines, app->cell_lines == 1 ? "" : "s");
    }
    app->cell_btn_close = (Rect){fr.x + fr.w - 76, fr.y, 76, fr.h};
    app->cell_btn_copy  = (Rect){app->cell_btn_close.x - 8 - 76, fr.y, 76, fr.h};
    ui_text_clipped(win, info, fr.x, ui_text_y(win, fr, TH_FONT_SMALL + 1, FONT_UI), app->cell_btn_copy.x - fr.x - 12,
                    TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
    draw_bar_button(app, app->cell_btn_copy, "Copy", false);
    draw_bar_button(app, app->cell_btn_close, "Close", true);
    platform_reset_clip(win);
}

static void cell_view_move(AppWindow* app, int drow, int dcol) {
    TableView* tv = app->results;
    tv->selected_row = SV_CLAMP(tv->selected_row + drow, 0, table_view_row_count(tv) - 1);
    tv->selected_col = SV_CLAMP(tv->selected_col + dcol, 0, tv->result->num_cols - 1);
    app->cell_scroll = 0;
    table_view_ensure_visible(tv);
}

// il visualizzatore è modale: consuma tutti gli eventi tranne il rilascio del mouse
static void cell_view_handle_event(AppWindow* app, sEvent* evt) {
    Cell* cell = grid_cell(app);
    if (!cell) { app->cell_open = false; return; }
    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            app->mouse_x = evt->mouse_x;
            app->mouse_y = evt->mouse_y;
            platform_set_cursor(app->win, rect_has(app->cell_btn_copy, evt->mouse_x, evt->mouse_y) ||
                                          rect_has(app->cell_btn_close, evt->mouse_x, evt->mouse_y) ? CURSOR_HAND : CURSOR_ARROW);
            break;
        case EVT_MOUSE_WHEEL:
            app->cell_scroll = SV_MAX(0, app->cell_scroll - evt->wheel_delta * 3);
            break;
        case EVT_MOUSE_DOWN:
            if (rect_has(app->cell_btn_copy, evt->mouse_x, evt->mouse_y)) copy_and_report(app, cell_text(cell), "the value");
            else if (rect_has(app->cell_btn_close, evt->mouse_x, evt->mouse_y) ||
                     !rect_has(app->cell_rect, evt->mouse_x, evt->mouse_y)) app->cell_open = false;
            break;
        case EVT_KEY_DOWN: {
            bool ctrl = evt->modifiers & SV_MOD_CTRL;
            switch (evt->key) {
                case KEY_ESCAPE: case KEY_ENTER: app->cell_open = false; break;
                case KEY_LETTER:
                    if (ctrl && evt->character == 'c') copy_and_report(app, cell_text(cell), "the value");
                    break;
                case KEY_UP:       cell_view_move(app, -1, 0); break;
                case KEY_DOWN:     cell_view_move(app, +1, 0); break;
                case KEY_LEFT:     cell_view_move(app, 0, -1); break;
                case KEY_RIGHT:    cell_view_move(app, 0, +1); break;
                case KEY_TAB:      cell_view_move(app, 0, (evt->modifiers & SV_MOD_SHIFT) ? -1 : 1); break;
                case KEY_PAGEUP:   app->cell_scroll = SV_MAX(0, app->cell_scroll - 10); break;
                case KEY_PAGEDOWN: app->cell_scroll += 10; break;
                case KEY_HOME:     app->cell_scroll = 0; break;
                case KEY_END:      app->cell_scroll = app->cell_lines; break;
                default: break;
            }
            break;
        }
        default:
            break;
    }
}

// ── filtro delle righe ───────────────────────────────────────────

// testo del filtro: "valore" cerca in tutte le colonne; "colonna: valore" solo in quella colonna;
// "colonna = valore" vuole il valore esatto (apici facoltativi, NULL per i valori nulli)
static void apply_filter_text(AppWindow* app, const char* text) {
    TableView* tv = app->results;
    int col = -1;
    bool exact = false;
    const char* val = text;
    char buf[LINE_EDIT_MAX];
    const char* sep = strpbrk(text, ":=");
    if (sep && grid_ready(app)) {
        const char* a = text;
        while (*a == ' ') a++;
        const char* b = sep;
        while (b > a && b[-1] == ' ') b--;
        for (int c = 0; c < tv->result->num_cols && col < 0; c++) {
            const char* name = tv->result->col_names[c];
            if (!name) continue;
            const char* dot = strrchr(name, '.');   // "tabella.colonna" accetta anche "colonna"
            if (((int)strlen(name) == b - a && strncasecmp(name, a, b - a) == 0) ||
                (dot && (int)strlen(dot + 1) == b - a && strncasecmp(dot + 1, a, b - a) == 0)) col = c;
        }
        if (col >= 0) {
            exact = *sep == '=';
            const char* v = sep + 1;
            while (*v == ' ') v++;
            snprintf(buf, sizeof(buf), "%s", v);
            int n = (int)strlen(buf);
            while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
            if (n >= 2 && (buf[0] == '\'' || buf[0] == '"') && buf[n - 1] == buf[0]) {
                memmove(buf, buf + 1, n - 2);
                buf[n - 2] = '\0';
            }
            val = buf;
            if (!val[0]) { table_view_set_filter(tv, "", -1, false); return; }   // "colonna:" ancora senza valore
        }
    }
    if (col < 0) table_view_set_filter(tv, text, -1, false);
    else         table_view_set_filter(tv, val, col, exact);
}

static void on_filter_change(LineEdit* le, void* ud) {
    AppWindow* app = (AppWindow*)ud;
    apply_filter_text(app, le->text);
    table_view_ensure_visible(app->results);
}

static void filter_focus(AppWindow* app) {
    if (!app->filter_input->base.visible) return;
    textbox_focus(app->editor, false);
    app->results->base.state = WIDGET_STATE_NORMAL;
    line_edit_focus(app->filter_input, true);
    line_edit_select_all(app->filter_input);
}

// true se l'evento è stato consumato dalla casella del filtro
static bool filter_handle_event(AppWindow* app, sEvent* evt) {
    LineEdit* le = app->filter_input;
    if (!le->base.visible) {
        if (line_edit_focused(le)) line_edit_focus(le, false);
        return false;
    }
    bool focused = line_edit_focused(le);
    switch (evt->type) {
        case EVT_MOUSE_DOWN:
            if (widget_contains_point((Widget*)le, evt->mouse_x, evt->mouse_y)) {
                textbox_focus(app->editor, false);
                app->results->base.state = WIDGET_STATE_NORMAL;
                widget_handle_event((Widget*)le, evt);
                return true;
            }
            if (focused) line_edit_focus(le, false);
            return false;
        case EVT_MOUSE_MOVE: case EVT_MOUSE_UP:
            return widget_handle_event((Widget*)le, evt);
        case EVT_CHAR:
            return focused && widget_handle_event((Widget*)le, evt);
        case EVT_KEY_DOWN:
            if (!focused) return false;
            if (evt->key == KEY_ESCAPE) {             // prima svuota, poi torna alla griglia
                if (le->text[0]) { line_edit_set_text(le, ""); on_filter_change(le, app); }
                else { line_edit_focus(le, false); app->results->base.state = WIDGET_STATE_FOCUSED; }
                return true;
            }
            if (evt->key == KEY_ENTER || evt->key == KEY_DOWN) {   // passa alla griglia, sulla prima riga
                line_edit_focus(le, false);
                TableView* tv = app->results;
                tv->base.state = WIDGET_STATE_FOCUSED;
                if (table_view_row_count(tv) > 0 && tv->selected_row < 0) { tv->selected_row = 0; table_view_ensure_visible(tv); }
                return true;
            }
            return widget_handle_event((Widget*)le, evt);
        default:
            return false;
    }
}

// ── menu contestuale ─────────────────────────────────────────────

typedef enum {
    CTX_SEP = 0,
    // griglia
    CTX_VIEW, CTX_COPY_VALUE, CTX_COPY_LITERAL, CTX_INSERT_VALUE, CTX_COPY_ROW, CTX_COPY_COLUMN, CTX_COPY_COL_NAME,
    CTX_SORT_ASC, CTX_SORT_DESC, CTX_SORT_CLEAR, CTX_FIT_COL, CTX_COPY_ALL, CTX_EXPORT,
    CTX_FILTER_VALUE, CTX_FILTER_ROWS, CTX_FILTER_CLEAR,
    // editor
    CTX_CUT, CTX_COPY, CTX_PASTE, CTX_SELECT_ALL, CTX_RUN, CTX_RUN_STMT, CTX_FORMAT, CTX_COMMENT,
    // File
    CTX_NEW, CTX_OPEN, CTX_SAVE, CTX_SAVE_AS, CTX_QUIT,
    // Edit
    CTX_UNDO, CTX_REDO, CTX_FIND, CTX_REPLACE, CTX_GOTO, CTX_DUP_LINE, CTX_DEL_LINE, CTX_UPPER, CTX_LOWER,
    // View
    CTX_TOGGLE_SIDEBAR, CTX_TOGGLE_FOOTER, CTX_TOGGLE_LINENUMS, CTX_TOGGLE_ROWNUMS,
    CTX_ZOOM_IN, CTX_ZOOM_OUT, CTX_ZOOM_RESET, CTX_NEXT_THEME, CTX_REFRESH_TREE, CTX_HISTORY,
    // Query
    CTX_STOP, CTX_EXPLAIN, CTX_BEGIN, CTX_COMMIT, CTX_ROLLBACK,
    CTX_SNIP_SELECT, CTX_SNIP_JOIN, CTX_SNIP_GROUP, CTX_SNIP_INSERT, CTX_SNIP_UPDATE, CTX_SNIP_DELETE, CTX_SNIP_CREATE,
    // Import / Export
    CTX_IMPORT_CSV, CTX_RUN_FILE,
    CTX_EXPORT_CSV, CTX_EXPORT_TSV, CTX_EXPORT_JSON, CTX_EXPORT_MD, CTX_EXPORT_HTML, CTX_EXPORT_SQL,
    CTX_COPY_JSON, CTX_COPY_MD, CTX_COPY_SQL, CTX_DUMP_DB,
    // Settings / Help
    CTX_SETTINGS, CTX_SETTINGS_RESET, CTX_SHORTCUTS, CTX_SQL_REFERENCE, CTX_ABOUT,
    CTX_ACTION_COUNT
} CtxAction;

typedef struct { const char* label; const char* keys; } CtxInfo;

static const CtxInfo CTX_INFO[CTX_ACTION_COUNT] = {
    [CTX_VIEW]          = { "View value",                "Enter" },
    [CTX_COPY_VALUE]    = { "Copy value",                "Ctrl+C" },
    [CTX_COPY_LITERAL]  = { "Copy as SQL literal",       NULL },
    [CTX_INSERT_VALUE]  = { "Insert value into editor",  NULL },
    [CTX_COPY_ROW]      = { "Copy row",                  NULL },
    [CTX_COPY_COLUMN]   = { "Copy column values",        NULL },
    [CTX_COPY_COL_NAME] = { "Copy column name",          NULL },
    [CTX_SORT_ASC]      = { "Sort ascending",            NULL },
    [CTX_SORT_DESC]     = { "Sort descending",           NULL },
    [CTX_SORT_CLEAR]    = { "Original order",            NULL },
    [CTX_FIT_COL]       = { "Fit column width",          NULL },
    [CTX_COPY_ALL]      = { "Copy results as TSV",       "Ctrl+Shift+C" },
    [CTX_EXPORT]        = { "Export\xe2\x80\xa6",        "Ctrl+E" },
    [CTX_FILTER_VALUE]  = { "Show only this value",      NULL },
    [CTX_FILTER_ROWS]   = { "Filter rows\xe2\x80\xa6",   "Ctrl+F" },
    [CTX_FILTER_CLEAR]  = { "Clear filter",              NULL },
    [CTX_CUT]           = { "Cut",                       "Ctrl+X" },
    [CTX_COPY]          = { "Copy",                      "Ctrl+C" },
    [CTX_PASTE]         = { "Paste",                     "Ctrl+V" },
    [CTX_SELECT_ALL]    = { "Select all",                "Ctrl+A" },
    [CTX_RUN]           = { "Run script",                "F5" },
    [CTX_RUN_STMT]      = { "Run statement",             "Ctrl+Enter" },
    [CTX_FORMAT]        = { "Format SQL",                "Ctrl+Shift+F" },
    [CTX_COMMENT]       = { "Toggle comment",            "Ctrl+/" },
    [CTX_NEW]           = { "New script",                "Ctrl+N" },
    [CTX_OPEN]          = { "Open SQL file\xe2\x80\xa6", "Ctrl+O" },
    [CTX_SAVE]          = { "Save",                      "Ctrl+S" },
    [CTX_SAVE_AS]       = { "Save as\xe2\x80\xa6",       "Ctrl+Shift+S" },
    [CTX_QUIT]          = { "Exit",                      NULL },
    [CTX_UNDO]          = { "Undo",                      "Ctrl+Z" },
    [CTX_REDO]          = { "Redo",                      "Ctrl+Y" },
    [CTX_FIND]          = { "Find\xe2\x80\xa6",          "Ctrl+F" },
    [CTX_REPLACE]       = { "Replace\xe2\x80\xa6",       "Ctrl+H" },
    [CTX_GOTO]          = { "Go to line\xe2\x80\xa6",    "Ctrl+G" },
    [CTX_DUP_LINE]      = { "Duplicate line",            "Ctrl+D" },
    [CTX_DEL_LINE]      = { "Delete line",               "Ctrl+Shift+K" },
    [CTX_UPPER]         = { "UPPERCASE",                 "Ctrl+Shift+U" },
    [CTX_LOWER]         = { "lowercase",                 "Ctrl+U" },
    [CTX_TOGGLE_SIDEBAR]  = { "Explorer sidebar",        "Ctrl+B" },
    [CTX_TOGGLE_FOOTER]   = { "Status bar",              NULL },
    [CTX_TOGGLE_LINENUMS] = { "Line numbers",            NULL },
    [CTX_TOGGLE_ROWNUMS]  = { "Row numbers in results",  NULL },
    [CTX_ZOOM_IN]       = { "Zoom in",                   "Ctrl+=" },
    [CTX_ZOOM_OUT]      = { "Zoom out",                  "Ctrl+-" },
    [CTX_ZOOM_RESET]    = { "Reset zoom",                "Ctrl+0" },
    [CTX_NEXT_THEME]    = { "Next color theme",          "Ctrl+Shift+T" },
    [CTX_REFRESH_TREE]  = { "Refresh explorer",          NULL },
    [CTX_HISTORY]       = { "Query history",             "Ctrl+Shift+H" },
    [CTX_STOP]          = { "Stop",                      "Esc" },
    [CTX_EXPLAIN]       = { "Explain statement",         "Ctrl+Shift+E" },
    [CTX_BEGIN]         = { "Begin transaction",         NULL },
    [CTX_COMMIT]        = { "Commit",                    NULL },
    [CTX_ROLLBACK]      = { "Rollback",                  NULL },
    [CTX_SNIP_SELECT]   = { "Insert SELECT template",    NULL },
    [CTX_SNIP_JOIN]     = { "Insert JOIN template",      NULL },
    [CTX_SNIP_GROUP]    = { "Insert GROUP BY template",  NULL },
    [CTX_SNIP_INSERT]   = { "Insert INSERT template",    NULL },
    [CTX_SNIP_UPDATE]   = { "Insert UPDATE template",    NULL },
    [CTX_SNIP_DELETE]   = { "Insert DELETE template",    NULL },
    [CTX_SNIP_CREATE]   = { "Insert CREATE TABLE template", NULL },
    [CTX_IMPORT_CSV]    = { "Import CSV into a table\xe2\x80\xa6", NULL },
    [CTX_RUN_FILE]      = { "Run SQL file\xe2\x80\xa6",  NULL },
    [CTX_EXPORT_CSV]    = { "Results as CSV\xe2\x80\xa6", NULL },
    [CTX_EXPORT_TSV]    = { "Results as TSV\xe2\x80\xa6", NULL },
    [CTX_EXPORT_JSON]   = { "Results as JSON\xe2\x80\xa6", NULL },
    [CTX_EXPORT_MD]     = { "Results as Markdown\xe2\x80\xa6", NULL },
    [CTX_EXPORT_HTML]   = { "Results as HTML\xe2\x80\xa6", NULL },
    [CTX_EXPORT_SQL]    = { "Results as SQL INSERTs\xe2\x80\xa6", NULL },
    [CTX_COPY_JSON]     = { "Copy results as JSON",      NULL },
    [CTX_COPY_MD]       = { "Copy results as Markdown",  NULL },
    [CTX_COPY_SQL]      = { "Copy results as SQL INSERTs", NULL },
    [CTX_DUMP_DB]       = { "Database dump (.sql)\xe2\x80\xa6", NULL },
    [CTX_SETTINGS]      = { "Settings\xe2\x80\xa6",      "Ctrl+," },
    [CTX_SETTINGS_RESET] = { "Reset all settings",       NULL },
    [CTX_SHORTCUTS]     = { "Keyboard shortcuts",        "F1" },
    [CTX_SQL_REFERENCE] = { "Supported SQL",             NULL },
    [CTX_ABOUT]         = { "About SegVault",            NULL },
};

#define CTX_ROW_H 28
#define CTX_SEP_H 9
#define CTX_W     272
#define CTX_MENU_W 300

enum { CTXK_NONE = 0, CTXK_GRID, CTXK_EDITOR, CTXK_MENU };

// ── barra dei menu ───────────────────────────────────────────────

static const int M_FILE[] = { CTX_NEW, CTX_OPEN, CTX_SAVE, CTX_SAVE_AS, CTX_SEP, CTX_RUN_FILE, CTX_SEP, CTX_QUIT };
static const int M_EDIT[] = { CTX_UNDO, CTX_REDO, CTX_SEP, CTX_CUT, CTX_COPY, CTX_PASTE, CTX_SELECT_ALL, CTX_SEP,
                              CTX_FIND, CTX_REPLACE, CTX_GOTO, CTX_SEP, CTX_DUP_LINE, CTX_DEL_LINE, CTX_COMMENT,
                              CTX_UPPER, CTX_LOWER, CTX_SEP, CTX_FORMAT };
static const int M_VIEW[] = { CTX_TOGGLE_SIDEBAR, CTX_TOGGLE_FOOTER, CTX_TOGGLE_LINENUMS, CTX_TOGGLE_ROWNUMS, CTX_SEP,
                              CTX_ZOOM_IN, CTX_ZOOM_OUT, CTX_ZOOM_RESET, CTX_SEP, CTX_NEXT_THEME, CTX_SEP,
                              CTX_HISTORY, CTX_REFRESH_TREE };
static const int M_QUERY[] = { CTX_RUN, CTX_RUN_STMT, CTX_EXPLAIN, CTX_STOP, CTX_SEP, CTX_BEGIN, CTX_COMMIT, CTX_ROLLBACK,
                               CTX_SEP, CTX_SNIP_SELECT, CTX_SNIP_JOIN, CTX_SNIP_GROUP, CTX_SNIP_INSERT, CTX_SNIP_UPDATE,
                               CTX_SNIP_DELETE, CTX_SNIP_CREATE };
static const int M_IMPORT[] = { CTX_IMPORT_CSV, CTX_SEP, CTX_OPEN, CTX_RUN_FILE };
static const int M_EXPORT[] = { CTX_EXPORT_CSV, CTX_EXPORT_TSV, CTX_EXPORT_JSON, CTX_EXPORT_MD, CTX_EXPORT_HTML,
                                CTX_EXPORT_SQL, CTX_SEP, CTX_COPY_ALL, CTX_COPY_JSON, CTX_COPY_MD, CTX_COPY_SQL, CTX_SEP,
                                CTX_DUMP_DB };
static const int M_SETTINGS[] = { CTX_SETTINGS, CTX_SEP, CTX_NEXT_THEME, CTX_TOGGLE_SIDEBAR, CTX_TOGGLE_FOOTER,
                                  CTX_TOGGLE_LINENUMS, CTX_TOGGLE_ROWNUMS, CTX_SEP, CTX_SETTINGS_RESET };
static const int M_HELP[] = { CTX_SHORTCUTS, CTX_SQL_REFERENCE, CTX_SEP, CTX_ABOUT };

typedef struct { const char* title; const int* items; int n; char key; } MenuDef;
#define MENU(t, a, k) { t, a, (int)(sizeof(a) / sizeof(a[0])), k }
static const MenuDef MENUS[] = {
    MENU("File", M_FILE, 'f'), MENU("Edit", M_EDIT, 'e'), MENU("View", M_VIEW, 'v'), MENU("Query", M_QUERY, 'q'),
    MENU("Import", M_IMPORT, 'i'), MENU("Export", M_EXPORT, 'x'), MENU("Settings", M_SETTINGS, 's'),
    MENU("Help", M_HELP, 'h'),
};
#define MENU_COUNT ((int)(sizeof(MENUS) / sizeof(MENUS[0])))

// ── stato delle voci ─────────────────────────────────────────────

static bool ctx_enabled(AppWindow* app, int a) {
    TableView* tv = app->results;
    bool idle = app->job == NULL;
    switch (a) {
        case CTX_SEP: return false;
        case CTX_VIEW: case CTX_COPY_VALUE: case CTX_COPY_LITERAL: case CTX_INSERT_VALUE:
            return grid_cell(app) != NULL;
        case CTX_COPY_ROW: return grid_has_row(app);
        case CTX_COPY_COLUMN: case CTX_COPY_COL_NAME: case CTX_FIT_COL: return grid_has_col(app);
        case CTX_SORT_ASC:  return grid_has_col(app) && !(tv->sort_col == tv->selected_col && tv->sort_asc);
        case CTX_SORT_DESC: return grid_has_col(app) && !(tv->sort_col == tv->selected_col && !tv->sort_asc);
        case CTX_SORT_CLEAR: return grid_ready(app) && tv->sort_col >= 0;
        case CTX_COPY_ALL: case CTX_EXPORT: case CTX_FILTER_ROWS:
        case CTX_EXPORT_CSV: case CTX_EXPORT_TSV: case CTX_EXPORT_JSON: case CTX_EXPORT_MD: case CTX_EXPORT_HTML:
        case CTX_EXPORT_SQL: case CTX_COPY_JSON: case CTX_COPY_MD: case CTX_COPY_SQL:
            return grid_ready(app);
        case CTX_FILTER_VALUE: return grid_cell(app) != NULL;
        case CTX_FILTER_CLEAR: return table_view_filtered(tv);
        case CTX_CUT: case CTX_COPY: return textbox_has_selection(app->editor);
        case CTX_UNDO: return app->editor->undo_count > 0;
        case CTX_REDO: return app->editor->redo_count > 0;
        case CTX_RUN: case CTX_RUN_STMT: case CTX_EXPLAIN: case CTX_RUN_FILE: case CTX_IMPORT_CSV: return idle;
        case CTX_STOP: return !idle;
        case CTX_BEGIN: return idle && !app->in_transaction;
        case CTX_COMMIT: case CTX_ROLLBACK: return idle;
        case CTX_DUMP_DB: { const char* db = db_current_database(); return idle && db && db[0]; }
        default: return true;
    }
}

// voci con una spunta (interruttori): -1 = nessuna spunta
static int ctx_checked(AppWindow* app, int a) {
    switch (a) {
        case CTX_TOGGLE_SIDEBAR:  return g_settings.show_sidebar;
        case CTX_TOGGLE_FOOTER:   return g_settings.show_footer;
        case CTX_TOGGLE_LINENUMS: return g_settings.line_numbers;
        case CTX_TOGGLE_ROWNUMS:  return g_settings.row_numbers;
        case CTX_HISTORY:         return app->showing_history;
        default: return -1;
    }
}

static const char* ctx_label(AppWindow* app, int a) {
    if (a == CTX_RUN && textbox_has_selection(app->editor)) return "Run selection";
    return CTX_INFO[a].label;
}

static void ctx_open_items(AppWindow* app, int kind, const int* items, int n, int x, int y) {
    app->ctx_count = 0;
    int h = 8;
    for (int i = 0; i < n && app->ctx_count < (int)(sizeof(app->ctx_items) / sizeof(app->ctx_items[0])); i++) {
        app->ctx_items[app->ctx_count++] = items[i];
        h += items[i] == CTX_SEP ? CTX_SEP_H : CTX_ROW_H;
    }
    app->ctx_kind  = kind;
    app->ctx_hover = -1;
    app->ctx_x = x;
    app->ctx_y = y;
    int w = kind == CTXK_MENU ? CTX_MENU_W : CTX_W;
    int rx = x + w > app->win_w - 4 ? SV_MAX(4, (kind == CTXK_MENU ? app->win_w - 4 : x) - w) : x;
    int ry = y + h > app->win_h - 4 ? SV_MAX(4, app->win_h - 4 - h) : y;
    app->ctx_rect = (Rect){rx, ry, w, h};
}

static void ctx_open(AppWindow* app, int kind, int x, int y) {
    static const int GRID[] = {
        CTX_VIEW, CTX_COPY_VALUE, CTX_COPY_LITERAL, CTX_INSERT_VALUE, CTX_SEP,
        CTX_COPY_ROW, CTX_COPY_COLUMN, CTX_COPY_COL_NAME, CTX_SEP,
        CTX_FILTER_VALUE, CTX_FILTER_ROWS, CTX_FILTER_CLEAR, CTX_SEP,
        CTX_SORT_ASC, CTX_SORT_DESC, CTX_SORT_CLEAR, CTX_FIT_COL, CTX_SEP,
        CTX_COPY_ALL, CTX_EXPORT,
    };
    static const int EDITOR[] = {
        CTX_CUT, CTX_COPY, CTX_PASTE, CTX_SELECT_ALL, CTX_SEP,
        CTX_RUN, CTX_RUN_STMT, CTX_EXPLAIN, CTX_SEP, CTX_FORMAT, CTX_COMMENT,
    };
    if (kind == CTXK_GRID) ctx_open_items(app, kind, GRID, (int)(sizeof(GRID) / sizeof(GRID[0])), x, y);
    else                   ctx_open_items(app, kind, EDITOR, (int)(sizeof(EDITOR) / sizeof(EDITOR[0])), x, y);
}

static void menu_open_at(AppWindow* app, int i) {
    if (i < 0 || i >= MENU_COUNT) return;
    app->menu_open = i;
    Rect t = app->menu_rects[i];
    ctx_open_items(app, CTXK_MENU, MENUS[i].items, MENUS[i].n, t.x, MENUBAR_H - 1);
}

static void menu_close(AppWindow* app) {
    app->menu_open = -1;
    if (app->ctx_kind == CTXK_MENU) app->ctx_kind = CTXK_NONE;
}

static Rect ctx_item_rect(AppWindow* app, int i) {
    int y = app->ctx_rect.y + 4;
    for (int k = 0; k < i; k++) y += app->ctx_items[k] == CTX_SEP ? CTX_SEP_H : CTX_ROW_H;
    int h = app->ctx_items[i] == CTX_SEP ? CTX_SEP_H : CTX_ROW_H;
    return (Rect){app->ctx_rect.x + 4, y, app->ctx_rect.w - 8, h};
}

static int ctx_item_at(AppWindow* app, int x, int y) {
    if (!rect_has(app->ctx_rect, x, y)) return -1;
    for (int i = 0; i < app->ctx_count; i++)
        if (rect_has(ctx_item_rect(app, i), x, y)) return i;
    return -1;
}

static void draw_ctx_menu(AppWindow* app) {
    if (!app->ctx_kind) return;
    PlatformWindow* win = app->win;
    Rect r = app->ctx_rect;
    bool menu = app->ctx_kind == CTXK_MENU;
    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 3, r.w + 4, r.h + 4}, ui_alpha(TH_BG, 170), 10);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 8);
    for (int i = 0; i < app->ctx_count; i++) {
        int a = app->ctx_items[i];
        Rect ir = ctx_item_rect(app, i);
        if (a == CTX_SEP) {
            platform_fill_rect(win, (Rect){ir.x + 6, ir.y + ir.h / 2, ir.w - 12, 1}, TH_BORDER);
            continue;
        }
        bool en = ctx_enabled(app, a);
        if (i == app->ctx_hover && en) platform_fill_rect_rounded(win, ir, TH_ELEVATED_HOVER, 6);
        Color tc = en ? TH_TEXT : TH_TEXT_FAINT;
        int lx = ir.x + (menu ? 30 : 12);
        int chk = ctx_checked(app, a);
        if (chk == 1) ui_icon_check(win, ir.x + 15, ir.y + ir.h / 2, 11, TH_ACCENT);
        const char* keys = CTX_INFO[a].keys;
        int kw = keys ? platform_measure_text_ex(win, keys, -1, TH_FONT_SMALL, FONT_UI) : 0;
        ui_text_clipped(win, ctx_label(app, a), lx, ui_text_y(win, ir, TH_FONT_UI, FONT_UI),
                        ir.x + ir.w - lx - 12 - kw - 12, tc, TH_FONT_UI, FONT_UI);
        if (keys)
            platform_draw_text_ex(win, keys, -1, (Point){ir.x + ir.w - 12 - kw, ui_text_y(win, ir, TH_FONT_SMALL, FONT_UI)},
                                  en ? TH_TEXT_DIM : TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    }
}

static void draw_menubar(AppWindow* app) {
    PlatformWindow* win = app->win;
    int W = app->win_w;
    if (app->ctx_kind != CTXK_MENU) app->menu_open = -1;
    platform_fill_rect(win, (Rect){0, 0, W, MENUBAR_H}, TH_SURFACE);
    platform_fill_rect(win, (Rect){0, MENUBAR_H - 1, W, 1}, TH_BORDER);
    int x = 8;
    for (int i = 0; i < MENU_COUNT; i++) {
        int tw = platform_measure_text_ex(win, MENUS[i].title, -1, TH_FONT_UI, FONT_UI);
        Rect t = {x, 3, tw + 20, MENUBAR_H - 6};
        app->menu_rects[i] = t;
        bool open = app->menu_open == i;
        bool hover = rect_has(t, app->mouse_x, app->mouse_y);
        if (open)       platform_fill_rect_rounded(win, t, TH_ACCENT_SOFT, 5);
        else if (hover) platform_fill_rect_rounded(win, t, TH_ELEVATED_HOVER, 5);
        platform_draw_text_ex(win, MENUS[i].title, -1, (Point){t.x + 10, ui_text_y(win, t, TH_FONT_UI, FONT_UI)},
                              open ? TH_ACCENT_TEXT : TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
        x += t.w + 2;
    }
    // a destra: file aperto e modifiche non salvate
    char name[300];
    snprintf(name, sizeof(name), "%.200s%s", app->file_path[0] ? base_name(app->file_path) : "untitled.sql",
             app->modified ? "  \xe2\x97\x8f" : "");
    int nw = platform_measure_text_ex(win, name, -1, TH_FONT_SMALL + 1, FONT_UI);
    if (W - 16 - nw > x + 20)
        platform_draw_text_ex(win, name, -1, (Point){W - 16 - nw, ui_text_y(win, (Rect){0, 0, 0, MENUBAR_H}, TH_FONT_SMALL + 1, FONT_UI)},
                              TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
}

// ── azioni ───────────────────────────────────────────────────────

// invia all'editor una scorciatoia Ctrl(+Shift)+tasto (taglia, copia, annulla, ...)
static void editor_key(AppWindow* app, char ch, bool shift) {
    sEvent k;
    memset(&k, 0, sizeof(k));
    k.type      = EVT_KEY_DOWN;
    k.key       = KEY_LETTER;
    k.character = (uint32_t)ch;
    k.modifiers = SV_MOD_CTRL | (shift ? SV_MOD_SHIFT : 0);
    textbox_focus(app->editor, true);
    widget_handle_event((Widget*)app->editor, &k);
}

static void insert_snippet(AppWindow* app, const char* snippet) {
    TextBox* tb = app->editor;
    textbox_focus(tb, true);
    // su una riga non vuota il modello va su una riga nuova
    int ls = tb->line_starts[tb->cursor_line];
    bool empty = true;
    for (int i = ls; i < tb->text_len && tb->text[i] != '\n'; i++) if (tb->text[i] != ' ') { empty = false; break; }
    char buf[600];
    snprintf(buf, sizeof(buf), "%s%s\n", empty ? "" : "\n", snippet);
    if (!empty) {   // fine riga
        while (tb->cursor_pos < tb->text_len && tb->text[tb->cursor_pos] != '\n') tb->cursor_pos++;
        tb->sel_anchor = -1;
    }
    textbox_insert(tb, buf);
    set_status(app, STATUS_OK, "Template inserted: replace the placeholder names", "");
}

static ExportOptions export_options(const char* table) {
    ExportOptions o;
    o.delimiter    = settings_csv_delimiter();
    o.header       = g_settings.csv_header != 0;
    o.null_as_text = g_settings.csv_null == CSVNULL_TEXT;
    o.json_pretty  = g_settings.json_pretty != 0;
    o.table        = table;
    return o;
}

static void copy_results_as(AppWindow* app, ExportFormat f) {
    ExportOptions o = export_options(app->export_table[0] ? app->export_table : "my_table");
    char* t = export_result(app->results, f, &o);
    char what[64];
    snprintf(what, sizeof(what), "%d rows as %s", table_view_row_count(app->results), export_format_name(f));
    copy_and_report(app, t, what);
    free(t);
}

static void open_export_bar(AppWindow* app, ExportFormat f) {
    bar_open(app, BAR_EXPORT, false);
    char buf[1100];
    join_path(buf, sizeof(buf), app->data_dir[0] ? app->data_dir : ".", "result.");
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof(buf) - n, "%s", export_format_ext(f));
    line_edit_set_text(app->bar_input, buf);
}

static void settings_store(AppWindow* app);
static void apply_settings(AppWindow* app);
static void settings_open_panel(AppWindow* app);

static void ctx_run(AppWindow* app, int a) {
    TableView* tv = app->results;
    if (!ctx_enabled(app, a)) return;
    switch (a) {
        case CTX_VIEW: cell_view_open(app); break;
        case CTX_COPY_VALUE: copy_and_report(app, cell_text(grid_cell(app)), "the value"); break;
        case CTX_COPY_LITERAL: {
            char* lit = sql_literal(grid_cell(app));
            copy_and_report(app, lit, "the SQL literal");
            free(lit);
            break;
        }
        case CTX_INSERT_VALUE: {
            char* lit = sql_literal(grid_cell(app));
            if (lit) {
                textbox_focus(app->editor, true);
                textbox_insert(app->editor, lit);
                set_status(app, STATUS_OK, "Value inserted into the editor", "");
            }
            free(lit);
            break;
        }
        case CTX_COPY_ROW: {
            Row* row = grid_row(app, tv->selected_row);
            SBuf b = {0};
            for (int c = 0; c < row->num_cells; c++) {
                if (c) sb_add(&b, "\t");
                sb_add(&b, cell_text(&row->cells[c]));
            }
            copy_and_report(app, b.p ? b.p : "", "the row");
            free(b.p);
            break;
        }
        case CTX_COPY_COLUMN: {
            SBuf b = {0};
            int c = tv->selected_col;
            int nr = table_view_row_count(tv);
            for (int i = 0; i < nr; i++) {
                Row* row = grid_row(app, i);
                if (i) sb_add(&b, "\n");
                if (c < row->num_cells) sb_add(&b, cell_text(&row->cells[c]));
            }
            char what[64];
            snprintf(what, sizeof(what), "%d value%s", nr, nr == 1 ? "" : "s");
            copy_and_report(app, b.p ? b.p : "", what);
            free(b.p);
            break;
        }
        case CTX_COPY_COL_NAME: copy_and_report(app, tv->result->col_names[tv->selected_col], "the column name"); break;
        case CTX_SORT_ASC:   table_view_sort(tv, tv->selected_col, true);  break;
        case CTX_SORT_DESC:  table_view_sort(tv, tv->selected_col, false); break;
        case CTX_SORT_CLEAR: table_view_sort(tv, -1, true); break;
        case CTX_FIT_COL:    table_view_fit_column(tv, tv->selected_col); break;
        case CTX_COPY_ALL:   copy_results_as(app, EXPORT_TSV); break;
        case CTX_EXPORT:     bar_open(app, BAR_EXPORT, false); break;
        case CTX_FILTER_VALUE: {
            Cell* c = grid_cell(app);
            const char* name = tv->result->col_names[tv->selected_col];
            const char* dot = name ? strrchr(name, '.') : NULL;
            char text[LINE_EDIT_MAX];
            snprintf(text, sizeof(text), "%.60s = %.400s", dot ? dot + 1 : (name ? name : ""), cell_text(c));
            line_edit_set_text(app->filter_input, text);
            apply_filter_text(app, text);
            break;
        }
        case CTX_FILTER_ROWS: filter_focus(app); break;
        case CTX_FILTER_CLEAR:
            line_edit_set_text(app->filter_input, "");
            table_view_set_filter(tv, "", -1, false);
            break;

        // editor
        case CTX_CUT:        editor_key(app, 'x', false); break;
        case CTX_COPY:       editor_key(app, 'c', false); break;
        case CTX_PASTE:      editor_key(app, 'v', false); break;
        case CTX_SELECT_ALL: editor_key(app, 'a', false); break;
        case CTX_COMMENT:    editor_key(app, '/', false); break;
        case CTX_UNDO:       editor_key(app, 'z', false); break;
        case CTX_REDO:       editor_key(app, 'y', false); break;
        case CTX_DUP_LINE:   editor_key(app, 'd', false); break;
        case CTX_DEL_LINE:   editor_key(app, 'k', true);  break;
        case CTX_UPPER:      editor_key(app, 'u', true);  break;
        case CTX_LOWER:      editor_key(app, 'u', false); break;
        case CTX_ZOOM_IN:    editor_key(app, '=', false); settings_store(app); break;
        case CTX_ZOOM_OUT:   editor_key(app, '-', false); settings_store(app); break;
        case CTX_ZOOM_RESET: editor_key(app, '0', false); settings_store(app); break;
        case CTX_FIND:       bar_open(app, BAR_FIND, false); break;
        case CTX_REPLACE:    bar_open(app, BAR_FIND, true); break;
        case CTX_GOTO:       bar_open(app, BAR_GOTO, false); break;
        case CTX_FORMAT:     format_sql(app); break;
        case CTX_RUN:        app_window_run(app, RUN_SCRIPT); break;
        case CTX_RUN_STMT:   app_window_run(app, RUN_CURRENT); break;
        case CTX_STOP:       app_window_cancel(app); break;
        case CTX_EXPLAIN: {
            char* stmt = NULL;
            int base = textbox_get_run_text(app->editor, &stmt);
            if (base < 0 || !stmt) { set_status(app, STATUS_INFO, "Put the cursor on a SELECT to explain it", ""); break; }
            const char* p = stmt;
            while (*p == ' ' || *p == '\n' || *p == '\t') p++;
            size_t n = strlen(p) + 16;
            char* q = malloc(n);
            if (q) {
                if (strncasecmp(p, "EXPLAIN", 7) == 0) snprintf(q, n, "%s", p);
                else snprintf(q, n, "EXPLAIN %s", p);
                run_text(app, q, base, true, "Explain");
            }
            free(stmt);
            break;
        }
        case CTX_BEGIN: {
            QueryResult* r = db_execute("BEGIN");
            bool ok = r && r->success;
            db_result_free(r);
            update_session(app);
            set_status(app, ok ? STATUS_OK : STATUS_ERROR, ok ? "Transaction started: COMMIT or ROLLBACK to finish"
                                                         : "BEGIN failed", "");
            break;
        }
        case CTX_COMMIT:     on_commit_click(app); break;
        case CTX_ROLLBACK:   on_rollback_click(app); break;
        case CTX_SNIP_SELECT: insert_snippet(app, "SELECT *\nFROM table_name\nWHERE column = 'value'\nORDER BY column\nLIMIT 100;"); break;
        case CTX_SNIP_JOIN:   insert_snippet(app, "SELECT a.*, b.*\nFROM table_a a\nJOIN table_b b ON a.id = b.a_id\nWHERE a.id > 0;"); break;
        case CTX_SNIP_GROUP:  insert_snippet(app, "SELECT column, COUNT(*) AS n, AVG(value) AS average\nFROM table_name\nGROUP BY column\nHAVING COUNT(*) > 1\nORDER BY n DESC;"); break;
        case CTX_SNIP_INSERT: insert_snippet(app, "INSERT INTO table_name (column1, column2)\nVALUES ('value1', 2);"); break;
        case CTX_SNIP_UPDATE: insert_snippet(app, "UPDATE table_name\nSET column = 'new value'\nWHERE id = 1;"); break;
        case CTX_SNIP_DELETE: insert_snippet(app, "DELETE FROM table_name\nWHERE id = 1;"); break;
        case CTX_SNIP_CREATE: insert_snippet(app, "CREATE TABLE table_name (\n    id INT PRIMARY KEY AUTO_INCREMENT,\n    name VARCHAR(100) NOT NULL,\n    price FLOAT,\n    created DATE\n);"); break;

        // file
        case CTX_NEW:     cmd_new(app); break;
        case CTX_OPEN:    bar_open(app, BAR_OPEN, false); break;
        case CTX_SAVE:    if (app->file_path[0]) save_to(app, app->file_path); else bar_open(app, BAR_SAVE_AS, false); break;
        case CTX_SAVE_AS: bar_open(app, BAR_SAVE_AS, false); break;
        case CTX_QUIT:    app->quit_requested = true; break;
        case CTX_RUN_FILE:   bar_open(app, BAR_RUN_FILE, false); break;
        case CTX_IMPORT_CSV: bar_open(app, BAR_IMPORT_CSV, false); break;
        case CTX_DUMP_DB:    bar_open(app, BAR_DUMP, false); break;

        // export
        case CTX_EXPORT_CSV:  open_export_bar(app, EXPORT_CSV); break;
        case CTX_EXPORT_TSV:  open_export_bar(app, EXPORT_TSV); break;
        case CTX_EXPORT_JSON: open_export_bar(app, EXPORT_JSON); break;
        case CTX_EXPORT_MD:   open_export_bar(app, EXPORT_MARKDOWN); break;
        case CTX_EXPORT_HTML: open_export_bar(app, EXPORT_HTML); break;
        case CTX_EXPORT_SQL:  open_export_bar(app, EXPORT_SQL); break;
        case CTX_COPY_JSON:   copy_results_as(app, EXPORT_JSON); break;
        case CTX_COPY_MD:     copy_results_as(app, EXPORT_MARKDOWN); break;
        case CTX_COPY_SQL:    copy_results_as(app, EXPORT_SQL); break;

        // vista
        case CTX_TOGGLE_SIDEBAR:  g_settings.show_sidebar = !g_settings.show_sidebar; apply_settings(app); settings_store(app); break;
        case CTX_TOGGLE_FOOTER:   g_settings.show_footer  = !g_settings.show_footer;  apply_settings(app); settings_store(app); break;
        case CTX_TOGGLE_LINENUMS: g_settings.line_numbers = !g_settings.line_numbers; apply_settings(app); settings_store(app); break;
        case CTX_TOGGLE_ROWNUMS:  g_settings.row_numbers  = !g_settings.row_numbers;  apply_settings(app); settings_store(app); break;
        case CTX_NEXT_THEME:      set_theme(app, (theme_current() + 1) % theme_count(), true); break;
        case CTX_REFRESH_TREE:    refresh_tree(app); set_status(app, STATUS_OK, "Explorer refreshed", ""); break;
        case CTX_HISTORY:         history_toggle(app); break;

        // settings / aiuto
        case CTX_SETTINGS:       settings_open_panel(app); break;
        case CTX_SETTINGS_RESET: {
            int sidebar = app->sidebar_w;
            settings_reset();
            app->sidebar_w = sidebar;
            apply_settings(app);
            settings_store(app);
            set_status(app, STATUS_OK, "All settings restored to their defaults", "");
            break;
        }
        case CTX_SHORTCUTS:     app->help_open = true; break;
        case CTX_SQL_REFERENCE: app->info_page = 2; app->info_scroll = 0; break;
        case CTX_ABOUT:         app->info_page = 1; app->info_scroll = 0; break;
        default: break;
    }
}

static void ctx_move_hover(AppWindow* app, int dir) {
    for (int step = 0; step < app->ctx_count; step++) {
        int i = app->ctx_hover < 0 ? (dir > 0 ? 0 : app->ctx_count - 1)
                                   : (app->ctx_hover + dir + app->ctx_count) % app->ctx_count;
        app->ctx_hover = i;
        if (ctx_enabled(app, app->ctx_items[i])) return;
    }
}

// true se l'evento è stato consumato dal menu aperto
static bool ctx_handle_event(AppWindow* app, sEvent* evt) {
    if (!app->ctx_kind) return false;
    switch (evt->type) {
        case EVT_MOUSE_MOVE: {
            app->mouse_x = evt->mouse_x;
            app->mouse_y = evt->mouse_y;
            int i = ctx_item_at(app, evt->mouse_x, evt->mouse_y);
            app->ctx_hover = i;
            bool en = i >= 0 && ctx_enabled(app, app->ctx_items[i]);
            platform_set_cursor(app->win, en ? CURSOR_HAND : CURSOR_ARROW);
            return true;
        }
        case EVT_MOUSE_DOWN: {
            int i = ctx_item_at(app, evt->mouse_x, evt->mouse_y);
            if (i >= 0) {
                if (!ctx_enabled(app, app->ctx_items[i])) return true;   // voce disabilitata: il menu resta
                int a = app->ctx_items[i];
                app->ctx_kind = CTXK_NONE;
                app->menu_open = -1;
                ctx_run(app, a);
                return true;
            }
            app->ctx_kind = CTXK_NONE;
            app->menu_open = -1;
            // un altro tasto destro riapre il menu nel nuovo punto: lascia passare l'evento
            return evt->mouse_button != SV_MOUSE_RIGHT;
        }
        case EVT_MOUSE_WHEEL: case EVT_CHAR:
            return true;
        case EVT_KEY_DOWN:
            switch (evt->key) {
                case KEY_UP:     ctx_move_hover(app, -1); break;
                case KEY_DOWN:   ctx_move_hover(app, +1); break;
                case KEY_LEFT: case KEY_RIGHT:
                    if (app->ctx_kind == CTXK_MENU && app->menu_open >= 0) {
                        int d = evt->key == KEY_LEFT ? -1 : 1;
                        menu_open_at(app, (app->menu_open + d + MENU_COUNT) % MENU_COUNT);
                        ctx_move_hover(app, +1);
                    }
                    break;
                case KEY_ENTER:
                    if (app->ctx_hover >= 0) {
                        int a = app->ctx_items[app->ctx_hover];
                        app->ctx_kind = CTXK_NONE;
                        app->menu_open = -1;
                        ctx_run(app, a);
                    }
                    break;
                case KEY_ESCAPE: app->ctx_kind = CTXK_NONE; app->menu_open = -1; break;
                default: break;
            }
            return true;
        default:
            return false;
    }
}

// barra dei menu: click sui titoli, passaggio da un menu all'altro, Alt+lettera
static bool menubar_handle_event(AppWindow* app, sEvent* evt) {
    if (evt->type == EVT_KEY_DOWN && evt->key == KEY_LETTER && (evt->modifiers & SV_MOD_ALT) &&
        !(evt->modifiers & SV_MOD_CTRL)) {
        for (int i = 0; i < MENU_COUNT; i++)
            if (MENUS[i].key == (char)evt->character) {
                menu_open_at(app, i);
                ctx_move_hover(app, +1);
                return true;
            }
        return false;
    }
    if (evt->type != EVT_MOUSE_DOWN && evt->type != EVT_MOUSE_MOVE) return false;
    if (evt->mouse_y >= MENUBAR_H) return false;
    int hit = -1;
    for (int i = 0; i < MENU_COUNT; i++) if (rect_has(app->menu_rects[i], evt->mouse_x, evt->mouse_y)) hit = i;
    if (evt->type == EVT_MOUSE_MOVE) {
        app->mouse_x = evt->mouse_x;
        app->mouse_y = evt->mouse_y;
        platform_set_cursor(app->win, hit >= 0 ? CURSOR_HAND : CURSOR_ARROW);
        if (hit >= 0 && app->menu_open >= 0 && hit != app->menu_open) menu_open_at(app, hit);   // scorre tra i menu
        return true;
    }
    if (hit < 0) { menu_close(app); return true; }
    if (app->menu_open == hit) menu_close(app);
    else menu_open_at(app, hit);
    return true;
}

// tasto destro: seleziona la cella sotto il mouse e apre il menu della griglia, oppure quello dell'editor
static bool ctx_try_open(AppWindow* app, sEvent* evt) {
    if (evt->type != EVT_MOUSE_DOWN || evt->mouse_button != SV_MOUSE_RIGHT) return false;
    int x = evt->mouse_x, y = evt->mouse_y;
    if (grid_ready(app) && widget_contains_point((Widget*)app->results, x, y)) {
        textbox_focus(app->editor, false);
        widget_handle_event((Widget*)app->results, evt);
        ctx_open(app, CTXK_GRID, x, y);
        return true;
    }
    if (widget_contains_point((Widget*)app->editor, x, y) && x < app->editor->base.bounds.x + app->editor->base.bounds.w - SCROLLBAR_W) {
        app->results->base.state = WIDGET_STATE_NORMAL;
        textbox_focus(app->editor, true);
        ctx_open(app, CTXK_EDITOR, x, y);
        return true;
    }
    return false;
}

// ── Settings ─────────────────────────────────────────────────────

static const char* const SETTINGS_SECTIONS[] = { "Appearance", "Editor", "Results", "Execution", "Export" };
#define SETTINGS_NSECT 5
#define SET_ROW_H 54

// indici (in settings_get) delle voci della sezione
static int settings_rows(int section, int* out, int max) {
    int n = 0;
    for (int i = 0; i < settings_count() && n < max; i++)
        if (!strcmp(settings_get(i)->section, SETTINGS_SECTIONS[section])) out[n++] = i;
    return n;
}

static void apply_settings(AppWindow* app) {
    Settings* s = &g_settings;
    g_ui_font = SV_CLAMP(s->ui_font_size, 11, 16);
    unsigned char rgb[3];
    bool custom = settings_accent_color(rgb);
    theme_set_accent(custom, (Color){rgb[0], rgb[1], rgb[2], 255});
    if (s->theme != theme_current()) theme_apply(s->theme);

    TextBox* tb = app->editor;
    tb->font_size          = SV_CLAMP(s->font_size, 10, 28);
    tb->show_line_nums     = s->line_numbers;
    tb->opt_highlight_line = s->highlight_line;
    tb->opt_auto_close     = s->auto_close;
    tb->opt_auto_popup     = s->autocomplete == COMPLETE_AUTO;
    tb->opt_blink          = s->cursor_blink;
    tb->opt_kw_lower       = s->keyword_case == CASE_LOWER;
    textbox_set_indent(settings_tab_size());
    g_sql_format_indent = settings_tab_size();
    g_sql_format_lower  = s->keyword_case == CASE_LOWER;

    TableView* tv = app->results;
    tv->row_h       = settings_row_height();
    tv->null_text   = settings_null_text();
    tv->zebra       = s->zebra;
    if (tv->show_rownum != (bool)s->row_numbers) {
        tv->show_rownum = s->row_numbers;
        tv->rownum_w = s->row_numbers ? 52 : 0;
    }
    table_view_clamp_scroll(tv);
    history_trim(app, SV_MIN(settings_history_limit(), HISTORY_MAX));

    refresh_widget_colors(app);
    layout_widgets(app);
}

// salva settings.ini (la dimensione del font può essere cambiata con lo zoom)
static void settings_store(AppWindow* app) {
    g_settings.font_size = app->editor->font_size;
    g_settings.theme = theme_current();
    if (app->settings_path[0]) settings_save(app->settings_path);
}

static void settings_changed(AppWindow* app) {
    apply_settings(app);
    settings_store(app);
}

static void settings_open_panel(AppWindow* app) {
    g_settings.font_size = app->editor->font_size;
    app->settings_open = true;
    app->settings_sel = 0;
    app->ctx_kind = CTXK_NONE;
    app->menu_open = -1;
}

static Rect g_set_rows[32], g_set_ctrl[32], g_set_sect[SETTINGS_NSECT], g_set_reset, g_set_done;
static int  g_set_first;   // prima riga visibile (scroll)

static void draw_switch(PlatformWindow* win, Rect r, bool on) {
    platform_fill_rect_rounded(win, r, on ? TH_ACCENT : TH_ELEVATED_HOVER, r.h / 2);
    int d = r.h - 6;
    platform_fill_rect_rounded(win, (Rect){on ? r.x + r.w - 3 - d : r.x + 3, r.y + 3, d, d},
                               on ? TH_ON_ACCENT : TH_TEXT_MUTED, d / 2);
}

static void draw_settings(AppWindow* app) {
    if (!app->settings_open) return;
    PlatformWindow* win = app->win;
    int W = app->win_w, H = app->win_h;
    platform_fill_rect(win, (Rect){0, 0, W, H}, ui_alpha(TH_BG, 190));
    int pw = SV_MIN(900, W - 32), ph = SV_MIN(640, H - 32);
    Rect r = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    app->settings_rect = r;
    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 4, r.w + 4, r.h + 4}, ui_alpha(TH_BG, 200), 12);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 10);
    platform_set_clip(win, r);

    // navigazione delle sezioni
    int nav_w = SV_MIN(190, pw / 4);
    platform_fill_rect(win, (Rect){r.x + nav_w, r.y, 1, r.h}, TH_BORDER);
    platform_draw_text_ex(win, "Settings", -1, (Point){r.x + 20, r.y + 20}, TH_TEXT, 17, FONT_UI_BOLD);
    for (int i = 0; i < SETTINGS_NSECT; i++) {
        Rect sr = {r.x + 10, r.y + 64 + i * 36, nav_w - 20, 32};
        g_set_sect[i] = sr;
        bool cur = i == app->settings_section;
        if (cur) platform_fill_rect_rounded(win, sr, TH_ACCENT_SOFT, 6);
        else if (rect_has(sr, app->mouse_x, app->mouse_y)) platform_fill_rect_rounded(win, sr, TH_ELEVATED_HOVER, 6);
        platform_draw_text_ex(win, SETTINGS_SECTIONS[i], -1, (Point){sr.x + 12, ui_text_y(win, sr, TH_FONT_UI, cur ? FONT_UI_BOLD : FONT_UI)},
                              cur ? TH_ACCENT_TEXT : TH_TEXT_MUTED, TH_FONT_UI, cur ? FONT_UI_BOLD : FONT_UI);
    }

    // righe della sezione
    int rows[32];
    int n = settings_rows(app->settings_section, rows, 32);
    int cx = r.x + nav_w + 24, cw = r.w - nav_w - 48;
    int top = r.y + 64, bottom = r.y + r.h - 60;
    int vis = SV_MAX(1, (bottom - top) / SET_ROW_H);
    app->settings_sel = SV_CLAMP(app->settings_sel, 0, n - 1);
    if (app->settings_sel < g_set_first) g_set_first = app->settings_sel;
    if (app->settings_sel >= g_set_first + vis) g_set_first = app->settings_sel - vis + 1;
    g_set_first = SV_CLAMP(g_set_first, 0, SV_MAX(0, n - vis));
    platform_draw_text_ex(win, SETTINGS_SECTIONS[app->settings_section], -1, (Point){cx, r.y + 24},
                          TH_TEXT, 15, FONT_UI_BOLD);
    for (int k = 0; k < 32; k++) g_set_rows[k] = g_set_ctrl[k] = (Rect){0, 0, 0, 0};
    for (int k = g_set_first; k < n && k < g_set_first + vis; k++) {
        const SettingDef* d = settings_get(rows[k]);
        Rect row = {cx - 10, top + (k - g_set_first) * SET_ROW_H, cw + 20, SET_ROW_H - 4};
        g_set_rows[k] = row;
        if (k == app->settings_sel) platform_fill_rect_rounded(win, row, TH_ELEVATED, 8);
        int ctrl_w = SV_MIN(240, cw / 2);
        platform_draw_text_ex(win, d->label, -1, (Point){cx, row.y + 9}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
        ui_text_clipped(win, d->help, cx, row.y + 29, cw - ctrl_w - 24, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
        char vb[32];
        if (d->kind == SET_BOOL) {
            Rect sw = {cx + cw - 46, row.y + (row.h - 24) / 2, 46, 24};
            g_set_ctrl[k] = sw;
            draw_switch(win, sw, *d->value != 0);
        } else {
            Rect box = {cx + cw - ctrl_w, row.y + (row.h - 30) / 2, ctrl_w, 30};
            g_set_ctrl[k] = box;
            ui_round_box(win, box, TH_SURFACE, TH_BORDER_STRONG, 6);
            const char* v = settings_value_text(rows[k], vb, sizeof(vb));
            bool accent = d->value == &g_settings.accent && *d->value > 0;
            int tw = platform_measure_text_ex(win, v, -1, TH_FONT_UI, FONT_UI);
            int tx = box.x + (box.w - tw) / 2 + (accent ? 9 : 0);
            if (accent) {   // campione del colore scelto
                unsigned char rgb[3];
                if (settings_accent_color(rgb)) ui_circle(win, tx - 12, box.y + box.h / 2, 6, (Color){rgb[0], rgb[1], rgb[2], 255});
            }
            platform_draw_text_ex(win, v, -1, (Point){tx, ui_text_y(win, box, TH_FONT_UI, FONT_UI)}, TH_TEXT, TH_FONT_UI, FONT_UI);
            const char* l = d->kind == SET_INT ? "\xe2\x88\x92" : "\xe2\x80\xb9";
            const char* rr = d->kind == SET_INT ? "+" : "\xe2\x80\xba";
            platform_draw_text_ex(win, l, -1, (Point){box.x + 10, ui_text_y(win, box, 15, FONT_UI_BOLD)}, TH_ACCENT_TEXT, 15, FONT_UI_BOLD);
            int rw = platform_measure_text_ex(win, rr, -1, 15, FONT_UI_BOLD);
            platform_draw_text_ex(win, rr, -1, (Point){box.x + box.w - 10 - rw, ui_text_y(win, box, 15, FONT_UI_BOLD)},
                                  TH_ACCENT_TEXT, 15, FONT_UI_BOLD);
        }
    }

    // piede
    Rect fr = {cx, r.y + r.h - 48, cw, 32};
    g_set_done  = (Rect){fr.x + fr.w - 80, fr.y, 80, fr.h};
    g_set_reset = (Rect){g_set_done.x - 10 - 120, fr.y, 120, fr.h};
    char where[1200];
    snprintf(where, sizeof(where), "Saved automatically \xc2\xb7 %s", app->settings_path[0] ? app->settings_path : "(no data directory)");
    ui_text_clipped(win, where, fr.x, ui_text_y(win, fr, TH_FONT_SMALL, FONT_UI), g_set_reset.x - fr.x - 12,
                    TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    draw_bar_button(app, g_set_reset, "Reset section", false);
    draw_bar_button(app, g_set_done, "Done", true);
    const char* hint = "\xe2\x86\x91\xe2\x86\x93 choose  \xc2\xb7  \xe2\x86\x90\xe2\x86\x92 change  \xc2\xb7  Tab section  \xc2\xb7  Esc close";
    ui_text_clipped(win, hint, r.x + 20, r.y + r.h - 34, nav_w - 24, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    platform_reset_clip(win);
}

static void settings_change(AppWindow* app, int k, int dir) {
    int rows[32];
    int n = settings_rows(app->settings_section, rows, 32);
    if (k < 0 || k >= n) return;
    settings_step(rows[k], dir);
    settings_changed(app);
}

static void settings_handle_event(AppWindow* app, sEvent* evt) {
    int rows[32];
    int n = settings_rows(app->settings_section, rows, 32);
    switch (evt->type) {
        case EVT_MOUSE_MOVE:
            app->mouse_x = evt->mouse_x;
            app->mouse_y = evt->mouse_y;
            break;
        case EVT_MOUSE_WHEEL:
            app->settings_sel = SV_CLAMP(app->settings_sel - evt->wheel_delta, 0, n - 1);
            break;
        case EVT_MOUSE_DOWN: {
            int x = evt->mouse_x, y = evt->mouse_y;
            if (!rect_has(app->settings_rect, x, y) || rect_has(g_set_done, x, y)) { app->settings_open = false; break; }
            if (rect_has(g_set_reset, x, y)) {
                for (int k = 0; k < n; k++) *settings_get(rows[k])->value = settings_get(rows[k])->def;
                settings_changed(app);
                set_status(app, STATUS_OK, "Section restored to its defaults", "");
                break;
            }
            for (int i = 0; i < SETTINGS_NSECT; i++)
                if (rect_has(g_set_sect[i], x, y)) { app->settings_section = i; app->settings_sel = 0; g_set_first = 0; return; }
            for (int k = 0; k < n && k < 32; k++) {
                if (!rect_has(g_set_rows[k], x, y)) continue;
                app->settings_sel = k;
                Rect c = g_set_ctrl[k];
                if (rect_has(c, x, y)) {
                    const SettingDef* d = settings_get(rows[k]);
                    int dir = (d->kind != SET_BOOL && x < c.x + c.w / 2) ? -1 : 1;   // metà sinistra = indietro
                    settings_change(app, k, dir);
                }
                break;
            }
            break;
        }
        case EVT_KEY_DOWN:
            switch (evt->key) {
                case KEY_ESCAPE: app->settings_open = false; break;
                case KEY_UP:     app->settings_sel = SV_MAX(0, app->settings_sel - 1); break;
                case KEY_DOWN:   app->settings_sel = SV_MIN(n - 1, app->settings_sel + 1); break;
                case KEY_LEFT:   settings_change(app, app->settings_sel, -1); break;
                case KEY_RIGHT: case KEY_ENTER: settings_change(app, app->settings_sel, +1); break;
                case KEY_TAB: {
                    int d = (evt->modifiers & SV_MOD_SHIFT) ? -1 : 1;
                    app->settings_section = (app->settings_section + d + SETTINGS_NSECT) % SETTINGS_NSECT;
                    app->settings_sel = 0;
                    g_set_first = 0;
                    break;
                }
                case KEY_LETTER:
                    if ((evt->modifiers & SV_MOD_CTRL) && evt->character == ',') app->settings_open = false;
                    break;
                default: break;
            }
            break;
        case EVT_CHAR:
            if (evt->character == ' ') settings_change(app, app->settings_sel, +1);
            break;
        default:
            break;
    }
}

// ── pannelli informativi: About e SQL supportato ────────────────

static const char* const ABOUT_LINES[] = {
    "#SegVault " SV_VERSION_STR,
    "A relational database engine written in C, with its own SQL editor.",
    "",
    "#Storage",
    "Pages, buffer pool, write-ahead log and B-tree indexes in the data directory.",
    "Catalog, tables and settings live next to each other: copy the folder to move everything.",
    "",
    "#Files in the data directory",
    "segvault.db / segvault.cat   data pages and catalog",
    "session.sql / session.ini    last script, cursor and window state",
    "settings.ini                 all preferences (Settings, Ctrl+,)",
    "history.sql                  executed queries (Query history)",
    "",
    "#Fonts",
    "Fira Sans and JetBrains Mono, SIL Open Font License (assets/fonts).",
};

static const char* const SQL_LINES[] = {
    "#Databases and tables",
    "CREATE DATABASE db;  USE db;  DROP DATABASE db;  SHOW DATABASES;  SHOW TABLES;",
    "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, price FLOAT);",
    "ALTER TABLE t ...;  DROP TABLE t;  TRUNCATE TABLE t;  DESCRIBE t;",
    "CREATE INDEX i ON t (col);  CREATE VIEW v AS SELECT ...;",
    "Types: INT BIGINT FLOAT DOUBLE DECIMAL VARCHAR(n) CHAR TEXT BOOLEAN DATE DATETIME BLOB",
    "",
    "#Reading data",
    "SELECT [DISTINCT] cols | * FROM t [alias] [JOIN t2 ON a.x = b.y] [WHERE ...]",
    "       [GROUP BY cols [HAVING ...]] [ORDER BY col [ASC|DESC]] [LIMIT n];",
    "WHERE: = != <> < > <= >=  AND OR NOT ( )  LIKE 'a%'  BETWEEN a AND b  IN (1, 2)  IS [NOT] NULL",
    "Aggregates: COUNT(*) COUNT(col) SUM AVG MIN MAX  \xc2\xb7  column and table aliases with AS",
    "JOIN: INNER, LEFT [OUTER], RIGHT  \xc2\xb7  EXPLAIN SELECT ... shows the plan",
    "",
    "#Changing data",
    "INSERT INTO t (a, b) VALUES (1, 'x');   one row per statement, up to 64 values",
    "UPDATE t SET col = value WHERE ...;     DELETE FROM t WHERE ...;",
    "Strings: 'It''s' or 'It\\'s'  \xc2\xb7  escapes \\n \\t \\\\",
    "",
    "#Transactions",
    "BEGIN;  COMMIT;  ROLLBACK;  SAVEPOINT s;  ROLLBACK TO s;",
    "",
    "#Current limits",
    "JOIN conditions must be  a = b; aggregates cannot be combined with JOIN.",
    "UPDATE sets one column per statement; no subqueries, no multi-row INSERT.",
};

static void draw_info(AppWindow* app) {
    if (!app->info_page) return;
    PlatformWindow* win = app->win;
    int W = app->win_w, H = app->win_h;
    const char* const* lines = app->info_page == 1 ? ABOUT_LINES : SQL_LINES;
    int n = app->info_page == 1 ? (int)(sizeof(ABOUT_LINES) / sizeof(ABOUT_LINES[0]))
                                : (int)(sizeof(SQL_LINES) / sizeof(SQL_LINES[0]));
    platform_fill_rect(win, (Rect){0, 0, W, H}, ui_alpha(TH_BG, 190));
    int pw = SV_MIN(860, W - 32), ph = SV_MIN(n * 22 + 110, H - 32);
    Rect r = {(W - pw) / 2, (H - ph) / 2, pw, ph};
    platform_fill_rect_rounded(win, (Rect){r.x - 2, r.y + 4, r.w + 4, r.h + 4}, ui_alpha(TH_BG, 200), 12);
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER_STRONG, 10);
    platform_set_clip(win, r);
    platform_draw_text_ex(win, app->info_page == 1 ? "About SegVault" : "Supported SQL", -1,
                          (Point){r.x + 24, r.y + 18}, TH_TEXT, 16, FONT_UI_BOLD);
    const char* close = "Esc to close \xc2\xb7 wheel to scroll";
    int cw = platform_measure_text_ex(win, close, -1, TH_FONT_SMALL + 1, FONT_UI);
    platform_draw_text_ex(win, close, -1, (Point){r.x + r.w - 24 - cw, r.y + 22}, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
    int vis = SV_MAX(1, (r.h - 80) / 22);
    app->info_scroll = SV_CLAMP(app->info_scroll, 0, SV_MAX(0, n - vis));
    int y = r.y + 60;
    for (int i = app->info_scroll; i < n && i < app->info_scroll + vis; i++, y += 22) {
        const char* l = lines[i];
        if (l[0] == '#') platform_draw_text_ex(win, l + 1, -1, (Point){r.x + 24, y + 4}, TH_ACCENT_TEXT, TH_FONT_SMALL + 1, FONT_UI_BOLD);
        else ui_text_clipped(win, l, r.x + 24, y + 3, r.w - 48, TH_TEXT_MUTED,
                             app->info_page == 2 ? TH_FONT_SMALL + 1 : TH_FONT_UI, app->info_page == 2 ? FONT_MONO : FONT_UI);
    }
    platform_reset_clip(win);
}

// ── import / export su file ─────────────────────────────────────

// legge un file intero (max 256 MB); malloc'd o NULL
static char* read_whole_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char* buf = malloc(cap);
    while (buf) {
        if (n + 1 >= cap) {
            if (cap >= ((size_t)256 << 20)) { free(buf); buf = NULL; break; }
            char* nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        size_t got = fread(buf + n, 1, cap - 1 - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static bool table_exists(const char* name) {
    const char* db = db_current_database();
    if (!db || !db[0]) return false;
    NameList* nl = db_list_tables(db);
    bool found = false;
    for (int i = 0; nl && i < nl->count && !found; i++) found = strcasecmp(nl->names[i], name) == 0;
    db_namelist_free(nl);
    return found;
}

static bool import_csv(AppWindow* app, const char* path) {
    const char* db = db_current_database();
    if (!db || !db[0]) { set_status(app, STATUS_ERROR, "Select a database first (USE name;) to import into", ""); return true; }
    char* csv = read_whole_file(path, NULL);
    if (!csv) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Cannot read %s: %s", path, strerror(errno));
        set_status(app, STATUS_ERROR, msg, "");
        return false;
    }
    char table[64];
    csv_table_name_from_path(path, table, sizeof(table));
    bool exists = table_exists(table);
    CsvImportInfo info;
    char* sql = csv_to_sql(csv, table, !exists, 0, &info);
    free(csv);
    if (!sql) {
        char msg[400];
        snprintf(msg, sizeof(msg), "CSV import: %s", info.error);
        set_status(app, STATUS_ERROR, msg, "");
        return false;
    }
    char label[128];
    snprintf(label, sizeof(label), "Import of %d rows into %s%s", info.rows, table, exists ? " (existing table)" : "");
    run_text(app, sql, 0, false, label);
    return true;
}

static bool run_file(AppWindow* app, const char* path) {
    char* sql = read_whole_file(path, NULL);
    if (!sql) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Cannot read %s: %s", path, strerror(errno));
        set_status(app, STATUS_ERROR, msg, "");
        return false;
    }
    char label[300];
    snprintf(label, sizeof(label), "Run of %s", base_name(path));
    run_text(app, sql, 0, false, label);
    return true;
}

// script SQL con struttura e dati di tutte le tabelle del database corrente
static bool dump_database(AppWindow* app, const char* path) {
    const char* db = db_current_database();
    if (!db || !db[0]) { set_status(app, STATUS_ERROR, "No database selected", ""); return true; }
    char dbname[128];
    snprintf(dbname, sizeof(dbname), "%s", db);
    SBuf b = {0};
    char when[32], line[512];
    now_str(when, sizeof(when));
    snprintf(line, sizeof(line), "-- SegVault dump of database %s, %s\n\nCREATE DATABASE %s;\nUSE %s;\n\n", dbname, when, dbname, dbname);
    sb_add(&b, line);
    NameList* nl = db_list_tables(dbname);
    int tables = 0, rows = 0;
    for (int i = 0; nl && i < nl->count; i++) {
        const char* t = nl->names[i];
        char* create = db_get_create_statement(dbname, t);
        if (create) {
            sb_add(&b, create);
            size_t cl = strlen(create);
            sb_add(&b, cl && create[cl - 1] == ';' ? "\n" : ";\n");
            db_free_string(create);
        }
        snprintf(line, sizeof(line), "SELECT * FROM %s", t);
        QueryResult* r = db_execute(line);
        if (r && r->success) {
            for (int k = 0; k < r->num_rows; k++) {
                snprintf(line, sizeof(line), "INSERT INTO %s VALUES (", t);
                sb_add(&b, line);
                for (int c = 0; c < r->rows[k].num_cells; c++) {
                    char* lit = export_sql_literal(&r->rows[k].cells[c]);
                    if (c) sb_add(&b, ", ");
                    sb_add(&b, lit ? lit : "NULL");
                    free(lit);
                }
                sb_add(&b, ");\n");
                rows++;
            }
        }
        db_result_free(r);
        sb_add(&b, "\n");
        tables++;
    }
    db_namelist_free(nl);
    bool ok = b.p && write_text_file(path, b.p);
    free(b.p);
    char msg[1300];
    if (ok) snprintf(msg, sizeof(msg), "Dump of %s: %d table%s, %d rows written to %s", dbname, tables, tables == 1 ? "" : "s", rows, path);
    else    snprintf(msg, sizeof(msg), "Cannot write %s: %s", path, strerror(errno));
    set_status(app, ok ? STATUS_OK : STATUS_ERROR, msg, "");
    return ok;
}

// ── pannello scorciatoie (F1) ───────────────────────────────────

typedef struct { const char* keys; const char* what; } Shortcut;
typedef struct { const char* title; const Shortcut* items; int count; } ShortcutGroup;

static const Shortcut SC_RUN[] = {
    { "F5", "Run script or selection" }, { "Ctrl+Enter", "Run statement under cursor" },
    { "Ctrl+Shift+F", "Format SQL" }, { "Ctrl+Shift+H", "Query history" },
    { "Ctrl+Shift+E", "Explain statement" }, { "Ctrl+Shift+T", "Next color theme" },
    { "Esc / Stop", "Cancel the running query" }, { "Ctrl+,", "Settings" }, { "Ctrl+B", "Show / hide explorer" },
    { "Alt + letter", "Open a menu (File = F, Export = X)" },
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
    { "Click header", "Sort asc / desc / off" }, { "Double-click / Enter", "View cell value" },
    { "Right-click", "Cell and editor menu" }, { "Ctrl+F (on grid)", "Filter rows" },
    { "Ctrl+C", "Copy cell (or row)" },
    { "Ctrl+Shift+C", "Copy all as TSV" }, { "Ctrl+E", "Export CSV" },
    { "Ctrl+Tab", "Next result tab" },
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
    int bh = 32, by = MENUBAR_H + (TOOLBAR_H - bh) / 2;
    Button* b[5];
    toolbar_buttons(app, b);
    for (int level = g_settings.toolbar_style == TOOLBAR_ICONS ? 2 : 0; level <= 3; level++) {
        app->compact = level;
        apply_toolbar_level(app, level);
        int x = SV_MAX(app->side_w, 200) + 14;   // a sinistra c'è il logo
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
    app->side_w = g_settings.show_sidebar
                ? SV_CLAMP(app->sidebar_w, SIDEBAR_MIN_W, SV_MAX(SIDEBAR_MIN_W, W - CENTER_MIN_W)) : 0;
    app->footer_h = g_settings.show_footer ? FOOTER_H : 0;
    app->tree->base.visible = app->tree_scroll->base.visible = app->btn_refresh->base.visible = app->side_w > 0;

    // toolbar
    int bh = 32, by = MENUBAR_H + (TOOLBAR_H - bh) / 2;
    layout_toolbar(app);
    app->lbl_db_name->base.bounds = (Rect){W - 210, by, 196, bh};

    // sidebar
    int side_top = TOP_H + SIDEBAR_HEADER_H;
    int side_h   = H - side_top - app->footer_h;
    app->tree->base.bounds        = (Rect){0, side_top, app->side_w - 1, side_h};
    app->tree_scroll->base.bounds = (Rect){app->side_w - 1 - SCROLLBAR_W, side_top, SCROLLBAR_W, side_h};
    app->btn_refresh->base.bounds = (Rect){app->side_w - 38, TOP_H + 5, 28, 28};

    // area centrale: divisa dallo splitter
    int min_y = TOP_H + 90;
    int max_y = H - app->footer_h - STATUSBAR_H - SPLITTER_H - 90;
    if (max_y < min_y) max_y = min_y;
    app->splitter_y = SV_CLAMP(app->splitter_y, min_y, max_y);

    int center_x  = app->side_w;
    int center_w  = W - app->side_w;
    int editor_h  = app->splitter_y - TOP_H;
    int status_y  = app->splitter_y + SPLITTER_H;
    int results_y = status_y + STATUSBAR_H + (app->num_tabs > 1 ? RESULT_TABS_H : 0);
    int results_h = H - app->footer_h - results_y;

    app->editor->base.bounds        = (Rect){center_x, TOP_H, center_w, editor_h};
    app->editor_scroll->base.bounds = (Rect){W - SCROLLBAR_W, TOP_H, SCROLLBAR_W, editor_h};

    app->lbl_status->base.bounds = (Rect){center_x + 112, status_y, center_w - 112 - 260, STATUSBAR_H};
    int hw = button_preferred_width(app->btn_history, app->win);
    app->btn_history->base.bounds = (Rect){W - 12 - hw, status_y + (STATUSBAR_H - 26) / 2, hw, 26};
    int fw = SV_CLAMP(center_w / 4, 150, 230);
    app->filter_input->base.bounds = (Rect){W - 12 - hw - 10 - fw, status_y + (STATUSBAR_H - 26) / 2, fw, 26};

    app->results->base.bounds          = (Rect){center_x, results_y, center_w, results_h};
    app->results_scroll_v->base.bounds = (Rect){W - SCROLLBAR_W, results_y + TABLE_VIEW_HEADER_H,
                                                SCROLLBAR_W, results_h - TABLE_VIEW_HEADER_H - SCROLLBAR_W};
    app->results_scroll_h->base.bounds = (Rect){center_x + app->results->rownum_w, H - app->footer_h - SCROLLBAR_W,
                                                center_w - app->results->rownum_w - SCROLLBAR_W, SCROLLBAR_W};
    layout_bar(app);
}

// ── API pubblica ─────────────────────────────────────────────────

bool app_window_quit_requested(AppWindow* app) { return app && app->quit_requested; }

AppWindow* app_window_create(PlatformWindow* win, int width, int height) {
    settings_init();
    AppWindow* app = SV_ALLOC(AppWindow);
    if (!app) return NULL;

    app->win    = win;
    app->win_w  = width;
    app->win_h  = height;
    app->splitter_y    = TOP_H + (int)((height - TOP_H - FOOTER_H) * 0.52f);
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

    app->filter_input = line_edit_create("Filter rows");
    app->filter_input->base.user_data = app;
    app->filter_input->on_change = on_filter_change;
    app->filter_input->win = win;
    app->filter_input->base.visible = false;
    app->menu_open = app->menu_hover = -1;
    apply_settings(app);

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

    // pannelli modali: Settings, informazioni
    if (app->settings_open && evt->type != EVT_MOUSE_UP) { settings_handle_event(app, evt); return; }
    if (app->info_page && evt->type != EVT_MOUSE_UP) {
        if (evt->type == EVT_MOUSE_WHEEL) app->info_scroll -= evt->wheel_delta * 3;
        else if (evt->type == EVT_KEY_DOWN && evt->key == KEY_DOWN) app->info_scroll++;
        else if (evt->type == EVT_KEY_DOWN && evt->key == KEY_UP) app->info_scroll--;
        else if (evt->type == EVT_KEY_DOWN || evt->type == EVT_MOUSE_DOWN) app->info_page = 0;
        if (app->info_scroll < 0) app->info_scroll = 0;
        return;
    }
    if (menubar_handle_event(app, evt)) return;

    // visualizzatore di cella (modale) e menu contestuale
    if (app->cell_open && evt->type != EVT_MOUSE_UP) {
        cell_view_handle_event(app, evt);
        return;
    }
    if (ctx_handle_event(app, evt)) return;

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

    // schede dei risultati
    if (evt->type == EVT_MOUSE_DOWN && evt->mouse_button == SV_MOUSE_LEFT) {
        for (int i = 0; i < app->num_tabs; i++)
            if (rect_has(app->tab_rects[i], evt->mouse_x, evt->mouse_y)) { select_result_tab(app, i); return; }
    }
    if (evt->type == EVT_KEY_DOWN && evt->key == KEY_TAB && (evt->modifiers & SV_MOD_CTRL) && app->num_tabs > 1) {
        int d = (evt->modifiers & SV_MOD_SHIFT) ? -1 : 1;
        select_result_tab(app, (app->cur_tab + d + app->num_tabs) % app->num_tabs);
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
                case 'f':
                    if (shift) format_sql(app);
                    else if (app->filter_input->base.visible &&
                             (app->results->base.state == WIDGET_STATE_FOCUSED || line_edit_focused(app->filter_input)))
                        filter_focus(app);
                    else bar_open(app, BAR_FIND, false);
                    return;
                case 'h': if (shift) history_toggle(app); else bar_open(app, BAR_FIND, true); return;
                case 'g': bar_open(app, BAR_GOTO, false); return;
                case 't':
                    if (shift) { set_theme(app, (theme_current() + 1) % theme_count(), true); return; }
                    break;
                case 'o': bar_open(app, BAR_OPEN, false); return;
                case ',': settings_open_panel(app); return;
                case 'b': ctx_run(app, CTX_TOGGLE_SIDEBAR); return;

                case 'n': cmd_new(app); return;
                case 'e':
                    if (shift) { ctx_run(app, CTX_EXPLAIN); return; }
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
    if (filter_handle_event(app, evt)) {
        if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN)
            platform_set_cursor(app->win, widget_contains_point((Widget*)app->filter_input, evt->mouse_x, evt->mouse_y)
                                          ? CURSOR_IBEAM : CURSOR_ARROW);
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
        app->sidebar_hover = app->side_w > 0 && abs(evt->mouse_x - app->side_w) <= 3 &&
                             evt->mouse_y > TOP_H && evt->mouse_y < app->win_h - app->footer_h;
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

    if (ctx_try_open(app, evt)) {
        platform_set_cursor(app->win, CURSOR_ARROW);
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
    c = line_edit_tick(app->filter_input, now_ms) || c;
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

    platform_fill_rect(win, (Rect){0, MENUBAR_H, W, TOOLBAR_H}, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){0, TOP_H - 1, W, 1}, TH_BORDER);

    // logo e nome
    ui_icon_logo(win, 30, (MENUBAR_H + TOOLBAR_H / 2), 26);
    int ty = ui_text_y(win, (Rect){0, MENUBAR_H, 0, TOOLBAR_H}, 16, FONT_UI_BOLD);
    int nw = platform_draw_text_ex(win, SV_NAME, -1, (Point){52, ty}, TH_TEXT, 16, FONT_UI_BOLD);
    Rect vr = {52 + nw + 8, (MENUBAR_H + TOOLBAR_H / 2) - 9, 0, 18};
    char ver[32];
    snprintf(ver, sizeof(ver), "v%s", SV_VERSION_STR);
    vr.w = platform_measure_text_ex(win, ver, -1, TH_FONT_SMALL, FONT_MONO) + 12;
    platform_fill_rect_rounded(win, vr, TH_ELEVATED, 9);
    platform_draw_text_ex(win, ver, -1, (Point){vr.x + 6, ui_text_y(win, vr, TH_FONT_SMALL, FONT_MONO)},
                          TH_TEXT_DIM, TH_FONT_SMALL, FONT_MONO);

    // separatore tra gruppo "esecuzione" e "transazione"
    Rect rc = app->btn_run_current->base.bounds;
    platform_fill_rect(win, (Rect){rc.x + rc.w + 11, MENUBAR_H + 14, 1, TOOLBAR_H - 28}, TH_BORDER_STRONG);
    Rect rb = app->btn_rollback->base.bounds;
    platform_fill_rect(win, (Rect){rb.x + rb.w + 11, MENUBAR_H + 14, 1, TOOLBAR_H - 28}, TH_BORDER_STRONG);
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
    Rect pill = {W - 16 - (tw + 46), (MENUBAR_H + TOOLBAR_H / 2) - 15, tw + 46, 30};
    ui_round_box(win, pill, TH_ELEVATED, TH_BORDER_STRONG, 15);
    ui_icon_database(win, pill.x + 18, pill.y + 15, 14, has_db ? TH_ICON_DB : TH_TEXT_FAINT);
    ui_text_clipped(win, db, pill.x + 32, ui_text_y(win, pill, TH_FONT_UI, FONT_UI_BOLD), tw,
                    has_db ? TH_TEXT : TH_TEXT_DIM, TH_FONT_UI, has_db ? FONT_UI_BOLD : FONT_UI);

    int left = pill.x;
    if (app->in_transaction) {
        const char* t = "TRANSACTION";
        bool small = app->compact >= 2;   // solo il pallino
        int w = small ? 22 : platform_measure_text_ex(win, t, -1, TH_FONT_SMALL, FONT_UI_BOLD) + 26;
        Rect b = {pill.x - w - 8, (MENUBAR_H + TOOLBAR_H / 2) - 11, w, 22};
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
    if (app->side_w <= 0) return;
    Rect hdr = {0, TOP_H, app->side_w - 1, SIDEBAR_HEADER_H};
    platform_fill_rect(win, hdr, TH_SURFACE);
    platform_draw_text_ex(win, "EXPLORER", -1,
                          (Point){18, ui_text_y(win, hdr, TH_FONT_SMALL, FONT_UI_BOLD)},
                          TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    widget_draw((Widget*)app->btn_refresh, win);
    widget_draw((Widget*)app->tree, win);
    widget_draw((Widget*)app->tree_scroll, win);

    // bordo destro della sidebar (evidenziato quando si può trascinare)
    bool active = app->sidebar_drag || app->sidebar_hover;
    platform_fill_rect(win, (Rect){app->side_w - (active ? 2 : 1), TOP_H, active ? 2 : 1,
                                   app->win_h - TOP_H - app->footer_h}, active ? TH_ACCENT : TH_BORDER);
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

    // filtro delle righe: solo con una griglia e abbastanza spazio
    LineEdit* fe = app->filter_input;
    fe->base.visible = grid_ready(app) && cw >= 600;
    if (fe->base.visible) {
        widget_draw((Widget*)fe, win);
        right = fe->base.bounds.x - 14;
    }
    char meta_buf[160];
    const char* meta = app->status_meta;
    bool filtered = table_view_filtered(app->results) && grid_ready(app);
    if (filtered) {
        snprintf(meta_buf, sizeof(meta_buf), "%d of %d rows", table_view_row_count(app->results),
                 app->results->result->num_rows);
        meta = meta_buf;
    }
    if (meta[0]) {
        int mw = platform_measure_text_ex(win, meta, -1, TH_FONT_SMALL + 1, FONT_MONO);
        int msg_w = platform_measure_text_ex(win, app->lbl_status->text, -1, TH_FONT_UI, FONT_UI);
        if (right - mw - 16 - lb->x >= SV_MIN(msg_w, 160)) {
            platform_draw_text_ex(win, meta, -1,
                                  (Point){right - mw, ui_text_y(win, st, TH_FONT_SMALL + 1, FONT_MONO)},
                                  filtered ? TH_ACCENT_TEXT : TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
            right -= mw + 16;
        }
    }
    lb->w = SV_MAX(0, right - lb->x);
    widget_draw((Widget*)app->lbl_status, win);
}

// schede dei risultati (solo quando un'esecuzione ha prodotto più griglie)
static void draw_result_tabs(AppWindow* app) {
    for (int i = 0; i < MAX_RESULT_TABS; i++) app->tab_rects[i] = (Rect){0, 0, 0, 0};
    if (app->num_tabs <= 1) return;
    PlatformWindow* win = app->win;
    int cx = app->side_w, cw = app->win_w - app->side_w;
    int y = app->splitter_y + SPLITTER_H + STATUSBAR_H;
    platform_fill_rect(win, (Rect){cx, y, cw, RESULT_TABS_H}, TH_SURFACE);
    platform_fill_rect(win, (Rect){cx, y + RESULT_TABS_H - 1, cw, 1}, TH_BORDER);
    int x = cx + 10;
    for (int i = 0; i < app->num_tabs; i++) {
        QueryResult* r = app->tabs[i];
        char label[64];
        snprintf(label, sizeof(label), "Result %d \xc2\xb7 %d row%s", i + 1, r ? r->num_rows : 0,
                 r && r->num_rows == 1 ? "" : "s");
        int tw = platform_measure_text_ex(win, label, -1, TH_FONT_SMALL + 1, FONT_UI);
        Rect t = {x, y + 4, tw + 22, RESULT_TABS_H - 8};
        if (t.x + t.w > cx + cw - 10) break;   // le schede che non entrano restano raggiungibili con Ctrl+Tab
        app->tab_rects[i] = t;
        bool active = i == app->cur_tab;
        bool hover = rect_has(t, app->mouse_x, app->mouse_y);
        if (active || hover) platform_fill_rect_rounded(win, t, active ? TH_ACCENT_SOFT : TH_HOVER, 6);
        platform_draw_text_ex(win, label, -1, (Point){t.x + 11, ui_text_y(win, t, TH_FONT_SMALL + 1, FONT_UI)},
                              active ? TH_ACCENT_TEXT : TH_TEXT_MUTED, TH_FONT_SMALL + 1, active ? FONT_UI_BOLD : FONT_UI);
        x += t.w + 6;
    }
}

static void draw_footer(AppWindow* app) {
    PlatformWindow* win = app->win;
    if (app->footer_h <= 0) return;
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
    draw_result_tabs(app);
    draw_sidebar(app);
    draw_toolbar(app);
    draw_menubar(app);
    draw_footer(app);
    textbox_draw_overlay(app->editor, win, (Rect){0, 0, app->win_w, app->win_h});
    draw_theme_menu(app);
    draw_ctx_menu(app);
    draw_tooltip(app);
    draw_cell_view(app);
    draw_settings(app);
    draw_info(app);
    draw_help(app);

    platform_present(win);
}

void app_window_resize(AppWindow* app, int new_w, int new_h) {
    if (!app || new_w <= 0 || new_h <= 0) return;
    // mantieni la proporzione dello splitter
    float ratio = (float)(app->splitter_y - TOP_H) / (float)SV_MAX(1, app->win_h - TOP_H - app->footer_h);
    app->win_w = new_w;
    app->win_h = new_h;

    app->splitter_y = TOP_H + (int)(ratio * (new_h - TOP_H - app->footer_h));
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
            bool cancelled = res && strcmp(res->error, "Query cancelled") == 0;
            j->nerrors++;
            if (!j->failed) {                 // si tiene il primo errore
                j->failed = res;
                j->failed_idx = j->executed;
                j->failed_offset = r->code_start;
            } else {
                db_result_free(res);
            }
            if (!j->keep_going || cancelled) break;
            continue;
        }
        j->total_ms += res->exec_time_ms;
        if (res->num_cols > 0) {
            if (j->ngrids == MAX_RESULT_TABS) {          // tiene gli ultimi MAX_RESULT_TABS
                db_result_free(j->grids[0]);
                memmove(j->grids, j->grids + 1, sizeof(j->grids[0]) * (MAX_RESULT_TABS - 1));
                j->ngrids--;
            }
            j->grids[j->ngrids++] = res;
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
    for (int i = 0; i < j->ngrids; i++) db_result_free(j->grids[i]);
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

// controlli di Settings > Execution: ritorna il motivo del blocco o NULL
static const char* guard_statement(const char* s, int n) {
    char first[16] = "";
    bool where = false;
    int words = 0;
    for (int i = 0; i < n; ) {
        char c = s[i];
        if (c == '-' && i + 1 < n && s[i + 1] == '-') { while (i < n && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            i++;
            while (i < n && s[i] != c) { if (s[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            if (words++ == 0) snprintf(first, sizeof(first), "%.*s", SV_MIN(j - i, 15), s + i);
            else if (j - i == 5 && !strncasecmp(s + i, "WHERE", 5)) where = true;
            i = j;
            continue;
        }
        i++;
    }
    if (g_settings.safe_updates && !where) {
        if (!strcasecmp(first, "UPDATE")) return "UPDATE without WHERE would change every row";
        if (!strcasecmp(first, "DELETE")) return "DELETE without WHERE would remove every row";
    }
    if (g_settings.confirm_drop) {
        if (!strcasecmp(first, "DROP"))     return "DROP is blocked";
        if (!strcasecmp(first, "TRUNCATE")) return "TRUNCATE is blocked";
    }
    return NULL;
}

// esegue un testo SQL nel thread di lavoro (ne prende la proprietà).
// from_editor: il testo inizia all'offset base dell'editor (gli errori evidenziano la riga);
// altrimenti (import, file eseguito) gli errori indicano solo il numero dello statement.
static void run_text(AppWindow* app, char* text, int base, bool from_editor, const char* label) {
    TextBox* tb = app->editor;

    // divisione in statement (i ';' dentro stringhe e commenti sono ignorati)
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

    // Settings > Execution: UPDATE/DELETE senza WHERE, DROP/TRUNCATE
    for (int i = 0; i < j->n; i++) {
        SqlRange* r = &ranges[i];
        if (!r->has_code) continue;
        const char* why = guard_statement(text + r->code_start, r->end - r->code_start);
        if (!why) continue;
        int line = 0;
        for (int k = 0; k < r->code_start; k++) if (text[k] == '\n') line++;
        char msg[300], detail[400];
        if (from_editor) {
            line += textbox_line_of_offset(tb, base);
            textbox_set_error_line(tb, line);
            textbox_scroll_to_line(tb, line);
            snprintf(msg, sizeof(msg), "Line %d: %s \xc2\xb7 nothing was executed", line + 1, why);
        } else {
            snprintf(msg, sizeof(msg), "%s \xc2\xb7 nothing was executed", why);
        }
        snprintf(detail, sizeof(detail), "%s. Add a WHERE clause, or turn the protection off in "
                 "Settings \xe2\x80\xba Execution (Ctrl+,).", why);
        show_result(app, NULL);
        table_view_set_message(app->results, TABLE_MSG_ERROR, "Blocked by a safety setting", detail);
        set_status(app, STATUS_ERROR, msg, "");
        job_free(j);
        return;
    }

    j->log = calloc(j->total_code, sizeof(RunLog));
    if (!j->log) { job_free(j); return; }
    j->keep_going = g_settings.on_error == ONERR_CONTINUE;
    snprintf(j->label, sizeof(j->label), "%s", label ? label : "");

    // avvio del thread di lavoro
    textbox_set_error_line(tb, -1);
    db_clear_cancel();
    app->job = j;
    app->job_base_line = from_editor ? textbox_line_of_offset(tb, base) : -1;
    app->job_started = app->job_last_status = platform_time_ms();
    app->job_thread = platform_thread_start(run_worker, j);
    if (!app->job_thread) {          // impossibile creare il thread: esecuzione diretta
        run_worker(j);
    }
    style_run_button(app);
    set_status(app, STATUS_INFO, label && label[0] ? label : "Running\xe2\x80\xa6", "");
}

void app_window_run(AppWindow* app, RunMode mode) {
    if (!app) return;
    if (busy_guard(app)) return;
    TextBox* tb = app->editor;

    // testo da eseguire e suo offset nel documento (per mappare gli errori)
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
    run_text(app, text, base, true, NULL);
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

    if (j->failed_idx > 0 && !cancelled_select && !(j->keep_going && j->executed > j->nerrors)) {
        // errore: evidenzia la riga dello statement nell'editor
        int line = SV_MAX(0, app->job_base_line);
        for (int k = 0; k < j->failed_offset && j->text[k]; k++) if (j->text[k] == '\n') line++;
        const char* err_raw = j->failed ? j->failed->error : "No response from the engine";
        // il motore numera le righe dallo statement ("... (line N)" / "... at line N"):
        // si converte nella riga dell'editor e si toglie dal messaggio
        char err[1024];
        snprintf(err, sizeof(err), "%s", err_raw);
        const char* pats[] = { " (line ", " at line " };
        for (int p = 0; p < 2; p++) {
            char* at = strstr(err, pats[p]);
            int n;
            if (at && sscanf(at + strlen(pats[p]), "%d", &n) == 1 && n >= 1) {
                line += n - 1;
                char* after = at + strlen(pats[p]);
                while (*after >= '0' && *after <= '9') after++;
                if (p == 0 && *after == ')') after++;
                memmove(at, after, strlen(after) + 1);
                break;
            }
        }
        if (app->job_base_line >= 0) {
            textbox_set_error_line(tb, line);
            textbox_scroll_to_line(tb, line);
        }

        char title[128];
        if (j->total_code > 1) snprintf(title, sizeof(title), "Statement %d of %d failed", j->failed_idx, j->total_code);
        else                   snprintf(title, sizeof(title), "Query failed");
        if (app->job_base_line >= 0) snprintf(msg, sizeof(msg), "Line %d: %s", line + 1, err);
        else snprintf(msg, sizeof(msg), "%s%sstatement %d: %s", j->label, j->label[0] ? " failed at " : "Failed at ",
                      j->failed_idx, err);
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
    } else if (j->ngrids > 0) {
        int ng = j->ngrids;
        QueryResult* grid = j->grids[ng - 1];
        set_result_tabs(app, j->grids, ng, ng - 1);   // la proprietà passa alle schede
        j->ngrids = 0;
        if (ng > 1)          snprintf(msg, sizeof(msg), "%d statements executed \xc2\xb7 %d result sets", j->executed, ng);
        else if (j->executed > 1) snprintf(msg, sizeof(msg), "%d statements executed", j->executed);
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

    if (j->keep_going && j->nerrors > 0 && j->executed > j->nerrors && !j->cancelled) {
        // modalità "continua": risultati mostrati, errori riassunti nello stato
        int line = SV_MAX(0, app->job_base_line);
        for (int k = 0; k < j->failed_offset && j->text[k]; k++) if (j->text[k] == '\n') line++;
        if (app->job_base_line >= 0) { textbox_set_error_line(tb, line); textbox_scroll_to_line(tb, line); }
        snprintf(msg, sizeof(msg), "%d of %d statements failed \xc2\xb7 first at line %d: %s", j->nerrors,
                 j->executed, line + 1, j->failed ? j->failed->error : "?");
        set_status(app, STATUS_ERROR, msg, "");
    } else if (j->label[0] && j->failed_idx == 0 && !j->cancelled) {
        snprintf(msg, sizeof(msg), "%s: done \xc2\xb7 %d statement%s", j->label, j->executed, j->executed == 1 ? "" : "s");
        set_status(app, STATUS_OK, msg, "");
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
    for (int i = 0; i < app->num_tabs; i++) db_result_free(app->tabs[i]);
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
    join_path(app->settings_path, sizeof(app->settings_path), dir, "settings.ini");
    bool have_settings = settings_load(app->settings_path);
    apply_settings(app);
    history_load(app);
    if (!g_settings.restore_session) {
        if (!have_settings) settings_store(app);
        return;
    }

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
            else if (!strncmp(line, "font=", 5) && !have_settings) tb->font_size = SV_CLAMP(atoi(line + 5), 10, 28);
            else if (!strncmp(line, "sidebar=", 8)) { app->sidebar_w = atoi(line + 8); layout_widgets(app); }
            else if (!strncmp(line, "theme=", 6) && !have_settings) {
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
    if (!have_settings) settings_store(app);   // prima esecuzione con Settings: crea il file
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
