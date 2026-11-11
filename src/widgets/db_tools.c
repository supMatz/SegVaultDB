#include "db_tools.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── risultati costruiti qui ─────────────────────────────────────

static QueryResult* new_result(int ncols, const char* const* names, const char* const* types) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    r->col_names = calloc((size_t)ncols, sizeof(char*));
    r->col_types = calloc((size_t)ncols, sizeof(char*));
    if (!r->col_names || !r->col_types) { free(r->col_names); free(r->col_types); free(r); return NULL; }
    r->num_cols = ncols;
    for (int i = 0; i < ncols; i++) {
        r->col_names[i] = strdup(names[i]);
        r->col_types[i] = strdup(types ? types[i] : "TEXT");
    }
    r->success = true;
    return r;
}

static bool add_row(QueryResult* r, int* cap, const char* const* vals, const CellType* types) {
    if (r->num_rows >= *cap) {
        int nc = *cap ? *cap * 2 : 32;
        Row* rows = realloc(r->rows, sizeof(Row) * (size_t)nc);
        if (!rows) return false;
        r->rows = rows;
        *cap = nc;
    }
    if (!db_row_set(&r->rows[r->num_rows], r->num_cols, vals, types)) return false;
    r->num_rows++;
    return true;
}

// ── ricerca ─────────────────────────────────────────────────────

// sottostringa senza distinzione tra maiuscole e minuscole (strcasestr non c'è su Windows)
static bool ci_contains(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

// testo tra apici per LIKE: apici raddoppiati, backslash raddoppiato
static void like_literal(const char* needle, char* out, size_t n) {
    size_t k = 0;
    if (k + 2 < n) { out[k++] = '\''; out[k++] = '%'; }
    for (const char* s = needle; *s && k + 4 < n; s++) {
        char c = (char)tolower((unsigned char)*s);
        if (c == '\'' || c == '\\') out[k++] = c;
        out[k++] = c;
    }
    if (k + 3 <= n) { out[k++] = '%'; out[k++] = '\''; }
    out[k] = '\0';
}

char* dbt_search_select(const char* db, const char* table, const char* needle) {
    int n = 0;
    ColumnInfo* cols = db_list_columns(db, table, &n);
    if (!cols || n == 0) { db_columns_free(cols, n); return NULL; }
    char lit[600];
    like_literal(needle, lit, sizeof(lit));
    size_t cap = 64 + strlen(table) + (size_t)n * (strlen(lit) + 100), len = 0;
    char* s = malloc(cap);
    if (!s) { db_columns_free(cols, n); return NULL; }
    len += (size_t)snprintf(s + len, cap - len, "SELECT * FROM `%s` WHERE ", table);
    for (int i = 0; i < n; i++)
        len += (size_t)snprintf(s + len, cap - len, "%sLOWER(`%s`) LIKE %s", i ? " OR " : "", cols[i].name, lit);
    db_columns_free(cols, n);
    return s;
}

QueryResult* dbt_search(const char* db, const char* needle, int limit, int* tables_hit, bool* truncated) {
    static const char* const NAMES[4] = { "Table", "Column", "Value", "Row" };
    if (tables_hit) *tables_hit = 0;
    if (truncated) *truncated = false;
    if (limit <= 0) limit = 1000;
    QueryResult* out = new_result(4, NAMES, NULL);
    NameList* tables = db_list_tables(db);
    if (!out || !tables || !needle || !needle[0]) { if (tables) db_namelist_free(tables); return out; }
    int cap = 0;
    for (int t = 0; t < tables->count && out->num_rows < limit; t++) {
        char* sql = dbt_search_select(db, tables->names[t], needle);
        if (!sql) continue;
        QueryResult* r = db_execute(sql);
        free(sql);
        if (!r || !r->success || r->num_rows == 0) { db_result_free(r); continue; }
        if (tables_hit) (*tables_hit)++;
        // chiave della riga: le colonne della chiave primaria ("id=3"), altrimenti il numero della riga
        int n = 0;
        ColumnInfo* cols = db_list_columns(db, tables->names[t], &n);
        int pk[8], npk = 0;
        for (int c = 0; c < r->num_cols && npk < 8; c++)
            for (int k = 0; k < n; k++)
                if (cols[k].primary_key && !strcasecmp(cols[k].name, r->col_names[c])) pk[npk++] = c;
        db_columns_free(cols, n);
        for (int i = 0; i < r->num_rows && out->num_rows < limit; i++) {
            char key[256];
            size_t kl = 0;
            key[0] = '\0';
            for (int k = 0; k < npk; k++)
                kl += (size_t)snprintf(key + kl, sizeof(key) - kl < 2 ? 0 : sizeof(key) - kl, "%s%s=%.60s", k ? ", " : "",
                                       r->col_names[pk[k]], r->rows[i].cells[pk[k]].value);
            if (!npk) snprintf(key, sizeof(key), "row %d", i + 1);
            for (int c = 0; c < r->num_cols && out->num_rows < limit; c++) {
                const Cell* cell = &r->rows[i].cells[c];
                if (cell->is_null || !ci_contains(cell->value, needle)) continue;
                const char* v[4] = { tables->names[t], r->col_names[c], cell->value, key };
                add_row(out, &cap, v, NULL);
            }
        }
        if (out->num_rows >= limit && truncated) *truncated = true;
        db_result_free(r);
    }
    db_namelist_free(tables);
    return out;
}

// ── panoramica ──────────────────────────────────────────────────

static void human_size(uint64_t b, char* out, size_t n) {
    if (b < 1024) snprintf(out, n, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) snprintf(out, n, "%.1f KB", (double)b / 1024);
    else snprintf(out, n, "%.1f MB", (double)b / (1024.0 * 1024.0));
}

QueryResult* dbt_overview(const char* db) {
    static const char* const NAMES[8] = { "Table", "Rows", "Columns", "Primary key", "Indexes", "Foreign keys", "Referenced by", "Size" };
    static const char* const TYPES[8] = { "TEXT", "INT", "INT", "TEXT", "INT", "INT", "TEXT", "TEXT" };
    QueryResult* out = new_result(8, NAMES, TYPES);
    NameList* tables = db_list_tables(db);
    if (!out || !tables) { if (tables) db_namelist_free(tables); return out; }
    int nf = 0;
    ForeignKeyInfo* fks = db_list_foreign_keys(db, &nf);
    int cap = 0;
    uint64_t tot_rows = 0, tot_size = 0;
    for (int t = 0; t < tables->count; t++) {
        const char* name = tables->names[t];
        char sql[200], rows[32] = "?", ncols[16], pk[200] = "", nidx[16] = "0", fko[16], refs[300] = "", size[32] = "";
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM `%.120s`", name);
        QueryResult* c = db_execute(sql);
        if (c && c->success && c->num_rows) { snprintf(rows, sizeof(rows), "%s", c->rows[0].cells[0].value); tot_rows += strtoull(rows, NULL, 10); }
        db_result_free(c);
        int n = 0;
        ColumnInfo* cols = db_list_columns(db, name, &n);
        snprintf(ncols, sizeof(ncols), "%d", n);
        size_t pl = 0;
        for (int k = 0; k < n; k++)
            if (cols[k].primary_key && pl + 2 < sizeof(pk))
                pl += (size_t)snprintf(pk + pl, sizeof(pk) - pl, "%s%.60s", pl ? ", " : "", cols[k].name);
        db_columns_free(cols, n);
        TableInfo ti;
        int ni = 0;
        if (db_table_info(db, name, &ti, &ni)) {
            snprintf(nidx, sizeof(nidx), "%d", ni);
            human_size(ti.size_bytes, size, sizeof(size));
            tot_size += ti.size_bytes;
        }
        int nout = 0;
        size_t rl = 0;
        for (int k = 0; k < nf; k++) {
            if (!strcasecmp(fks[k].table, name)) nout++;
            if (!strcasecmp(fks[k].ref_table, name) && rl + 2 < sizeof(refs)) {
                bool dup = false;     // una tabella sola volta
                for (int j = 0; j < k && !dup; j++) dup = !strcasecmp(fks[j].ref_table, name) && !strcasecmp(fks[j].table, fks[k].table);
                if (!dup) rl += (size_t)snprintf(refs + rl, sizeof(refs) - rl, "%s%.60s", rl ? ", " : "", fks[k].table);
            }
        }
        snprintf(fko, sizeof(fko), "%d", nout);
        const char* v[8] = { name, rows, ncols, pk[0] ? pk : NULL, nidx, fko, refs, size };
        static const CellType T[8] = { CELL_TYPE_STRING, CELL_TYPE_INT, CELL_TYPE_INT, CELL_TYPE_STRING, CELL_TYPE_INT,
                                       CELL_TYPE_INT, CELL_TYPE_STRING, CELL_TYPE_STRING };
        add_row(out, &cap, v, T);
    }
    // riga dei totali
    char trows[32], tsize[32], tcount[48];
    snprintf(trows, sizeof(trows), "%llu", (unsigned long long)tot_rows);
    human_size(tot_size, tsize, sizeof(tsize));
    snprintf(tcount, sizeof(tcount), "%d tables", tables->count);
    const char* v[8] = { tcount, trows, "", "", "", "", "", tsize };
    static const CellType T[8] = { CELL_TYPE_STRING, CELL_TYPE_INT, CELL_TYPE_INT, CELL_TYPE_STRING, CELL_TYPE_INT,
                                   CELL_TYPE_INT, CELL_TYPE_STRING, CELL_TYPE_STRING };
    if (tables->count) add_row(out, &cap, v, T);
    db_foreign_keys_free(fks);
    db_namelist_free(tables);
    return out;
}
