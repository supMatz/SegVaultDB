#include "grid_edit.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── testo che cresce ────────────────────────────────────────────

typedef struct { char* p; size_t n, cap; bool oom; } GBuf;

static void gb_add(GBuf* b, const char* s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char* p = realloc(b->p, cap);
        if (!p) { b->oom = true; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void gb_str(GBuf* b, const char* s) { gb_add(b, s, strlen(s)); }

static char* gb_done(GBuf* b) {
    if (b->oom) { free(b->p); return NULL; }
    return b->p ? b->p : strdup("");
}

// nome tra backtick: funziona anche con parole riservate
static void gb_ident(GBuf* b, const char* name) {
    gb_str(b, "`");
    gb_str(b, name);
    gb_str(b, "`");
}

// 'testo' con apici raddoppiati e backslash raddoppiati (il lexer tratta \ come escape)
static void gb_quoted(GBuf* b, const char* s) {
    gb_str(b, "'");
    const char* run = s;
    for (; *s; s++) {
        if (*s != '\'' && *s != '\\') continue;
        gb_add(b, run, (size_t)(s - run));
        gb_str(b, *s == '\'' ? "''" : "\\\\");
        run = s + 1;
    }
    gb_str(b, run);
    gb_str(b, "'");
}

// ── analisi ─────────────────────────────────────────────────────

void grid_edit_analyze(GridEdit* ge, const QueryResult* r) {
    memset(ge, 0, sizeof(*ge));
    ge->r = r;
    ge->row_table = -1;
    for (int c = 0; c < 64; c++) ge->col_table[c] = -1;
    if (!r || !r->success || r->num_cols == 0) { snprintf(ge->why, sizeof(ge->why), "No result to edit"); return; }
    if (!r->col_tables) {
        snprintf(ge->why, sizeof(ge->why), "Read-only: the rows come from GROUP BY, DISTINCT, UNION or a view");
        return;
    }
    if (r->num_cols > 64) { snprintf(ge->why, sizeof(ge->why), "Read-only: too many columns"); return; }
    snprintf(ge->db, sizeof(ge->db), "%s", r->source_db);

    for (int c = 0; c < r->num_cols; c++) {
        const char* tn = r->col_tables[c];
        if (!tn) continue;
        int t = -1;
        for (int k = 0; k < ge->nt; k++) if (!strcmp(ge->t[k].name, tn)) t = k;
        if (t < 0) {
            if (ge->nt == GE_MAX_TABLES) continue;
            t = ge->nt++;
            snprintf(ge->t[t].name, sizeof(ge->t[t].name), "%s", tn);
            ge->t[t].auto_col = -1;
        }
        ge->col_table[c] = t;
    }
    if (ge->nt == 0) { snprintf(ge->why, sizeof(ge->why), "Read-only: no column comes directly from a table"); return; }

    // chiave primaria di ogni tabella: tutte le sue colonne devono essere nel risultato
    for (int t = 0; t < ge->nt; t++) {
        GeTable* T = &ge->t[t];
        int n = 0;
        ColumnInfo* cols = db_list_columns(ge->db, T->name, &n);
        if (!cols) continue;
        bool missing = false;
        for (int i = 0; i < n; i++) {
            int rc = -1;
            for (int c = 0; c < r->num_cols && rc < 0; c++)
                if (ge->col_table[c] == t && r->col_columns[c] && !strcasecmp(r->col_columns[c], cols[i].name)) rc = c;
            if (rc >= 0) {
                snprintf(ge->col_type[rc], sizeof(ge->col_type[rc]), "%s", cols[i].type);
                ge->col_nullable[rc] = cols[i].nullable;
                snprintf(ge->col_default[rc], sizeof(ge->col_default[rc]), "%s", cols[i].default_val);
                if (cols[i].auto_increment) T->auto_col = rc;
            }
            if (!cols[i].primary_key) continue;
            if (rc < 0 || T->nkey == GE_MAX_KEY) missing = true;
            else T->key[T->nkey++] = rc;
        }
        T->key_ok = T->nkey > 0 && !missing;
        db_columns_free(cols, n);
    }
    if (ge->nt == 1) ge->row_table = 0;

    if (!grid_edit_any(ge)) {
        const GeTable* T = &ge->t[0];
        if (T->nkey == 0) snprintf(ge->why, sizeof(ge->why), "Read-only: table '%.60s' has no primary key", T->name);
        else snprintf(ge->why, sizeof(ge->why), "Read-only: select the primary key of '%.60s' to edit its rows", T->name);
    }
}

bool grid_edit_any(const GridEdit* ge) {
    for (int t = 0; t < ge->nt; t++) if (ge->t[t].key_ok) return true;
    return false;
}

bool grid_edit_cell_ok(const GridEdit* ge, int col) {
    if (!ge->r || col < 0 || col >= ge->r->num_cols || col >= 64) return false;
    int t = ge->col_table[col];
    return t >= 0 && ge->t[t].key_ok;
}

bool grid_edit_rows_ok(const GridEdit* ge) {
    return ge->row_table >= 0 && ge->t[ge->row_table].key_ok;
}

// ── letterali ───────────────────────────────────────────────────

static bool is_int_text(const char* s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-' || *s == '+') s++;
    if (!isdigit((unsigned char)*s)) return false;
    while (isdigit((unsigned char)*s)) s++;
    while (isspace((unsigned char)*s)) s++;
    return *s == '\0';
}

static bool is_num_text(const char* s) {
    char* end;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return false;
    strtod(s, &end);
    if (end == s) return false;
    while (isspace((unsigned char)*end)) end++;
    return *end == '\0' && !strpbrk(s, "xXnNiI");     // niente esadecimali, nan, inf
}

static void gb_value(GBuf* b, const char* type, const char* v) {
    char t[16];
    snprintf(t, sizeof(t), "%s", type ? type : "");
    for (char* p = t; *p; p++) *p = (char)toupper((unsigned char)*p);
    while (isspace((unsigned char)*v) && (is_num_text(v) || is_int_text(v))) v++;
    if ((!strncmp(t, "INT", 3) || !strncmp(t, "BIGINT", 6)) && is_int_text(v)) { gb_str(b, v); return; }
    if ((!strncmp(t, "FLOAT", 5) || !strncmp(t, "DOUBLE", 6) || !strncmp(t, "DECIMAL", 7)) && is_num_text(v)) { gb_str(b, v); return; }
    if (!strncmp(t, "BOOL", 4)) {
        if (!strcasecmp(v, "true") || !strcmp(v, "1")) { gb_str(b, "TRUE"); return; }
        if (!strcasecmp(v, "false") || !strcmp(v, "0")) { gb_str(b, "FALSE"); return; }
    }
    gb_quoted(b, v);                    // testo, date, o un valore non valido (l'errore lo dà il motore)
}

char* grid_edit_literal(const GridEdit* ge, int col, const char* value) {
    GBuf b = {0};
    gb_value(&b, col >= 0 && col < 64 ? ge->col_type[col] : "", value ? value : "");
    return gb_done(&b);
}

char* grid_edit_cell_literal(const Cell* c) {
    GBuf b = {0};
    if (c->is_null) gb_str(&b, "NULL");
    else if ((c->type == CELL_TYPE_INT || c->type == CELL_TYPE_FLOAT) && is_num_text(c->value)) gb_str(&b, c->value);
    else gb_quoted(&b, c->value);
    return gb_done(&b);
}

// WHERE chiave = valori della riga
static bool gb_where_key(GBuf* b, const GridEdit* ge, int t, const Row* row) {
    const GeTable* T = &ge->t[t];
    gb_str(b, " WHERE ");
    for (int k = 0; k < T->nkey; k++) {
        int c = T->key[k];
        if (c >= row->num_cells || row->cells[c].is_null) return false;
        if (k) gb_str(b, " AND ");
        gb_ident(b, ge->r->col_columns[c]);
        gb_str(b, " = ");
        char* lit = grid_edit_cell_literal(&row->cells[c]);
        if (!lit) return false;
        gb_str(b, lit);
        free(lit);
    }
    return true;
}

// ── SQL ─────────────────────────────────────────────────────────

char* grid_edit_update_sql(const GridEdit* ge, const Row* row, int col, const char* value, bool set_null,
                           char* err, size_t errn) {
    if (!grid_edit_cell_ok(ge, col)) { snprintf(err, errn, "%s", ge->why[0] ? ge->why : "This column cannot be edited"); return NULL; }
    int t = ge->col_table[col];
    GBuf b = {0};
    gb_str(&b, "UPDATE ");
    gb_ident(&b, ge->t[t].name);
    gb_str(&b, " SET ");
    gb_ident(&b, ge->r->col_columns[col]);
    gb_str(&b, " = ");
    if (set_null) gb_str(&b, "NULL");
    else gb_value(&b, ge->col_type[col], value ? value : "");
    if (!gb_where_key(&b, ge, t, row)) { free(b.p); snprintf(err, errn, "The row has no primary key value"); return NULL; }
    return gb_done(&b);
}

char* grid_edit_delete_sql(const GridEdit* ge, const Row* row, char* err, size_t errn) {
    if (!grid_edit_rows_ok(ge)) { snprintf(err, errn, "%s", ge->why[0] ? ge->why : "Rows of this result cannot be deleted"); return NULL; }
    GBuf b = {0};
    gb_str(&b, "DELETE FROM ");
    gb_ident(&b, ge->t[ge->row_table].name);
    if (!gb_where_key(&b, ge, ge->row_table, row)) { free(b.p); snprintf(err, errn, "The row has no primary key value"); return NULL; }
    return gb_done(&b);
}

char* grid_edit_insert_sql(const GridEdit* ge, const char* const* values, const bool* nulls, const bool* set,
                           char* err, size_t errn) {
    if (!grid_edit_rows_ok(ge)) { snprintf(err, errn, "%s", ge->why[0] ? ge->why : "Rows cannot be added to this result"); return NULL; }
    int t = ge->row_table, n = 0;
    GBuf b = {0};
    gb_str(&b, "INSERT INTO ");
    gb_ident(&b, ge->t[t].name);
    for (int c = 0; c < ge->r->num_cols && c < 64; c++) {
        if (!set[c] || ge->col_table[c] != t) continue;
        gb_str(&b, n++ ? ", " : " (");
        gb_ident(&b, ge->r->col_columns[c]);
    }
    if (n == 0) {                        // solo valori di default: si scrive la prima colonna come DEFAULT
        free(b.p);
        snprintf(err, errn, "Type at least one value for the new row");
        return NULL;
    }
    gb_str(&b, ") VALUES (");
    n = 0;
    for (int c = 0; c < ge->r->num_cols && c < 64; c++) {
        if (!set[c] || ge->col_table[c] != t) continue;
        if (n++) gb_str(&b, ", ");
        if (nulls[c]) gb_str(&b, "NULL");
        else gb_value(&b, ge->col_type[c], values[c] ? values[c] : "");
    }
    gb_str(&b, ")");
    return gb_done(&b);
}

char* grid_edit_fetch_sql(const GridEdit* ge, int t, const char* const* key_vals) {
    if (t < 0 || t >= ge->nt || !ge->t[t].key_ok) return NULL;
    GBuf b = {0};
    gb_str(&b, "SELECT ");
    int n = 0;
    for (int c = 0; c < ge->r->num_cols && c < 64; c++) {
        if (ge->col_table[c] != t) continue;
        if (n++) gb_str(&b, ", ");
        gb_ident(&b, ge->r->col_columns[c]);
    }
    gb_str(&b, " FROM ");
    gb_ident(&b, ge->t[t].name);
    gb_str(&b, " WHERE ");
    for (int k = 0; k < ge->t[t].nkey; k++) {
        if (k) gb_str(&b, " AND ");
        gb_ident(&b, ge->r->col_columns[ge->t[t].key[k]]);
        gb_str(&b, " = ");
        gb_str(&b, key_vals[k]);
    }
    return gb_done(&b);
}
