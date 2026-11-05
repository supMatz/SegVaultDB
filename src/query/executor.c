#include "executor.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static QueryResult* result_error_msg(const char* msg) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = false;
    snprintf(r->error, sizeof(r->error), "%s", msg);
    return r;
}

static QueryResult* result_ok_msg(int affected) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->rows_affected = affected;
    return r;
}

static DataType parse_type(const char* type_name) {
    if (strcmp(type_name, "INT") == 0 || strcmp(type_name, "INTEGER") == 0 ||
        strcmp(type_name, "SMALLINT") == 0 || strcmp(type_name, "TINYINT") == 0) return SV_TYPE_INT;
    if (strcmp(type_name, "BIGINT") == 0) return SV_TYPE_BIGINT;
    if (strcmp(type_name, "FLOAT") == 0 || strcmp(type_name, "DOUBLE") == 0 ||
        strcmp(type_name, "REAL") == 0) return SV_TYPE_FLOAT;
    if (strcmp(type_name, "NUMERIC") == 0) return SV_TYPE_DECIMAL;
    if (strcmp(type_name, "TIMESTAMP") == 0) return SV_TYPE_DATETIME;
    if (strcmp(type_name, "VARCHAR") == 0) return SV_TYPE_VARCHAR;
    if (strcmp(type_name, "CHAR") == 0) return SV_TYPE_CHAR;
    if (strcmp(type_name, "TEXT") == 0) return SV_TYPE_TEXT;
    if (strcmp(type_name, "BOOL") == 0 || strcmp(type_name, "BOOLEAN") == 0) return SV_TYPE_BOOL;
    if (strcmp(type_name, "DATE") == 0) return SV_TYPE_DATE;
    if (strcmp(type_name, "DATETIME") == 0) return SV_TYPE_DATETIME;
    if (strcmp(type_name, "BLOB") == 0) return SV_TYPE_BLOB;
    if (strcmp(type_name, "DECIMAL") == 0) return SV_TYPE_DECIMAL;
    return SV_TYPE_VARCHAR;
}

// ── valutazione delle condizioni (WHERE / HAVING) ────────────────

typedef struct {
    bool        null;
    bool        is_num;
    double      num;
    int64_t     inum;      // valore intero esatto (se is_int)
    bool        is_int;
    const char* str;       // testo (stringhe o numeri formattati)
    char        buf[64];
} EvalVal;

static void eval_aggregate(ASTExpr* e, EvalVal* out);

static void eval_operand(ASTExpr* e, Tuple* t, TableSchema* schema, EvalVal* out) {
    memset(out, 0, sizeof(*out));
    out->str = "";
    if (e && e->type == EXPR_AGG) { eval_aggregate(e, out); return; }
    if (e && e->type == EXPR_COLUMN && !t) { out->null = true; return; }   // gruppo vuoto
    switch (e ? e->type : EXPR_NULL) {
        case EXPR_INT:
            out->is_num = out->is_int = true; out->inum = e->int_val; out->num = (double)e->int_val;
            snprintf(out->buf, sizeof(out->buf), "%lld", (long long)e->int_val); out->str = out->buf;
            return;
        case EXPR_FLOAT:
            out->is_num = true; out->num = e->float_val;
            snprintf(out->buf, sizeof(out->buf), "%g", e->float_val); out->str = out->buf;
            return;
        case EXPR_STRING:
            out->str = e->str_val;
            return;
        case EXPR_COLUMN: {
            int ci = catalog_get_col_index(schema, e->col_name);
            if (ci < 0 || ci >= t->num_values || t->values[ci].is_null) { out->null = true; return; }
            Value* v = &t->values[ci];
            switch (schema->columns[ci].type) {
                case SV_TYPE_INT:    out->is_num = out->is_int = true; out->inum = v->int_val; break;
                case SV_TYPE_BIGINT: out->is_num = out->is_int = true; out->inum = v->bigint_val; break;
                case SV_TYPE_BOOL:   out->is_num = out->is_int = true; out->inum = v->bool_val ? 1 : 0; break;
                case SV_TYPE_FLOAT:
                    out->is_num = true; out->num = v->float_val;
                    snprintf(out->buf, sizeof(out->buf), "%g", v->float_val); out->str = out->buf;
                    return;
                default:
                    out->str = v->str_val ? v->str_val : "";
                    return;
            }
            out->num = (double)out->inum;
            snprintf(out->buf, sizeof(out->buf), "%lld", (long long)out->inum); out->str = out->buf;
            return;
        }
        default:
            out->null = true;
            return;
    }
}

// ── aggregati: calcolati sulle righe del gruppo corrente ──

static struct {
    Tuple**      rows;
    int*         idx;
    int          n;
    TableSchema* ts;
    bool         active;
} g_agg;

static void agg_context_set(Tuple** rows, int* idx, int n, TableSchema* ts) {
    g_agg.rows = rows; g_agg.idx = idx; g_agg.n = n; g_agg.ts = ts; g_agg.active = true;
}
static void agg_context_clear(void) { g_agg.active = false; }

static int evalval_cmp(const EvalVal* a, const EvalVal* b) {
    if (a->is_num && b->is_num) {
        if (a->is_int && b->is_int) return a->inum < b->inum ? -1 : a->inum > b->inum;
        return a->num < b->num ? -1 : a->num > b->num;
    }
    return strcmp(a->str, b->str);
}

static void evalval_set_int(EvalVal* v, int64_t x) {
    memset(v, 0, sizeof(*v));
    v->is_num = v->is_int = true; v->inum = x; v->num = (double)x;
    snprintf(v->buf, sizeof(v->buf), "%lld", (long long)x); v->str = v->buf;
}
static void evalval_set_num(EvalVal* v, double x) {
    memset(v, 0, sizeof(*v));
    v->is_num = true; v->num = x;
    snprintf(v->buf, sizeof(v->buf), "%.15g", x); v->str = v->buf;
}

static void eval_aggregate(ASTExpr* e, EvalVal* out) {
    memset(out, 0, sizeof(*out));
    out->str = "";
    if (!g_agg.active) { out->null = true; return; }
    if (strcmp(e->func, "COUNT") == 0 && strcmp(e->col_name, "*") == 0) { evalval_set_int(out, g_agg.n); return; }

    ASTExpr col;
    memset(&col, 0, sizeof(col));
    col.type = EXPR_COLUMN;
    snprintf(col.col_name, sizeof(col.col_name), "%s", e->col_name);

    int64_t count = 0, isum = 0;
    double sum = 0;
    bool all_int = true;
    EvalVal best;
    bool have_best = false;
    memset(&best, 0, sizeof(best));
    for (int k = 0; k < g_agg.n; k++) {
        EvalVal v;
        eval_operand(&col, g_agg.rows[g_agg.idx[k]], g_agg.ts, &v);
        if (v.null) continue;
        count++;
        if (v.is_num) { sum += v.num; if (v.is_int) isum += v.inum; else all_int = false; }
        else all_int = false;
        bool is_min = strcmp(e->func, "MIN") == 0;
        if (!have_best || (is_min ? evalval_cmp(&v, &best) < 0 : evalval_cmp(&v, &best) > 0)) {
            bool own = v.str == v.buf;
            best = v;
            if (own) best.str = best.buf;     // la copia punta al proprio buffer
            have_best = true;
        }
    }
    if (strcmp(e->func, "COUNT") == 0) { evalval_set_int(out, count); return; }
    if (count == 0) { out->null = true; return; }   // SUM/AVG/MIN/MAX di nessun valore = NULL
    if (strcmp(e->func, "SUM") == 0) { if (all_int) evalval_set_int(out, isum); else evalval_set_num(out, sum); return; }
    if (strcmp(e->func, "AVG") == 0) { evalval_set_num(out, sum / (double)count); return; }
    *out = best;                                         // MIN / MAX
    if (best.str == best.buf) out->str = out->buf;
}

// LIKE con % (qualsiasi sequenza) e _ (un carattere), senza distinzione maiuscole/minuscole
static bool like_match(const char* s, const char* p) {
    while (*p) {
        if (*p == '%') {
            while (*p == '%') p++;
            if (!*p) return true;
            for (; *s; s++) if (like_match(s, p)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p != '_' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
        s++; p++;
    }
    return *s == '\0';
}

static bool eval_where(ASTExpr* where, Tuple* t, TableSchema* schema) {
    if (!where) return true;
    if (where->type == EXPR_UNARY) {
        if (where->op == OP_NOT) return !eval_where(where->left, t, schema);
        EvalVal v;
        eval_operand(where->left, t, schema, &v);
        return where->op == OP_IS_NULL ? v.null : !v.null;
    }
    if (where->type != EXPR_BINARY) return false;
    if (where->op == OP_AND) return eval_where(where->left, t, schema) && eval_where(where->right, t, schema);
    if (where->op == OP_OR)  return eval_where(where->left, t, schema) || eval_where(where->right, t, schema);

    EvalVal a, b;
    eval_operand(where->left, t, schema, &a);
    eval_operand(where->right, t, schema, &b);
    if (a.null || b.null) return false;   // confronti con NULL: mai veri (usare IS NULL)

    if (where->op == OP_LIKE || where->op == OP_NOT_LIKE) {
        bool m = like_match(a.str, b.str);
        return where->op == OP_LIKE ? m : !m;
    }

    int cmp;
    if (a.is_num && b.is_num) {
        if (a.is_int && b.is_int) cmp = a.inum < b.inum ? -1 : a.inum > b.inum ? 1 : 0;
        else                      cmp = a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
    } else if (a.is_num || b.is_num) {
        // numero contro testo: confronto numerico se il testo è un numero, altrimenti testuale
        const char* txt = a.is_num ? b.str : a.str;
        char* end = NULL;
        double d = strtod(txt, &end);
        if (end && end != txt && *end == '\0') {
            double x = a.is_num ? a.num : d, y = a.is_num ? d : b.num;
            cmp = x < y ? -1 : x > y ? 1 : 0;
        } else {
            cmp = strcmp(a.str, b.str);
        }
    } else {
        cmp = strcmp(a.str, b.str);
    }
    switch (where->op) {
        case OP_EQ:  return cmp == 0;
        case OP_NEQ: return cmp != 0;
        case OP_LT:  return cmp < 0;
        case OP_GT:  return cmp > 0;
        case OP_LE:  return cmp <= 0;
        case OP_GE:  return cmp >= 0;
        default:     return false;
    }
}

// prima colonna della condizione che non esiste nella tabella (NULL se tutte esistono).
// Una colonna sbagliata deve dare errore: trattarla come "vero" faceva sì che
// DELETE ... WHERE colonna_sbagliata = 1 cancellasse tutte le righe.
static const char* where_unknown_column(ASTExpr* e, TableSchema* schema) {
    if (!e) return NULL;
    if (e->type == EXPR_COLUMN || (e->type == EXPR_AGG && strcmp(e->col_name, "*") != 0))
        return catalog_get_col_index(schema, e->col_name) < 0 ? e->col_name : NULL;
    const char* l = where_unknown_column(e->left, schema);
    return l ? l : where_unknown_column(e->right, schema);
}

static QueryResult* unknown_column_error(const char* col, const char* clause) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Unknown column '%.64s' in %s", col, clause);
    return result_error_msg(msg);
}

// ---- Comparator for qsort ----
typedef struct {
    Tuple** rows;
    int*    col_map;
    int     sort_col;
    bool    order_asc;
    TableSchema* schema;
} SortCtx;

static SortCtx g_sort_ctx;

typedef struct {
    Row* rows;
    int sort_col;
    bool order_asc;
} SortRowCtx;

static SortRowCtx g_sort_rows;

static int row_compare_cells(const void* a, const void* b) {
    const Row* ra = (const Row*)a;
    const Row* rb = (const Row*)b;
    int ci = g_sort_rows.sort_col;
    if (ci < 0 || ci >= ra->num_cells || ci >= rb->num_cells) return 0;
    const Cell* ca = &ra->cells[ci];
    const Cell* cb = &rb->cells[ci];
    if (ca->is_null && cb->is_null) return 0;
    if (ca->is_null) return g_sort_rows.order_asc ? -1 : 1;
    if (cb->is_null) return g_sort_rows.order_asc ? 1 : -1;
    const char* sa = ca->value;   // array: mai NULL
    const char* sb = cb->value;
    int cmp = strcmp(sa, sb);
    // Try numeric comparison if both look like numbers
    char* ea = NULL; char* eb = NULL;
    double da = strtod(sa, &ea);
    double db = strtod(sb, &eb);
    if (ea && *ea == '\0' && eb && *eb == '\0') {
        double diff = da - db;
        cmp = (diff > 0) ? 1 : (diff < 0) ? -1 : 0;
    }
    return g_sort_rows.order_asc ? cmp : -cmp;
}

static int row_compare(const void* a, const void* b) {
    int ia = *(int*)a;
    int ib = *(int*)b;
    Tuple* ta = g_sort_ctx.rows[ia];
    Tuple* tb = g_sort_ctx.rows[ib];
    int ci = g_sort_ctx.sort_col;
    if (ci < 0 || ci >= ta->num_values || ci >= tb->num_values) return 0;
    Value* va = &ta->values[ci];
    Value* vb = &tb->values[ci];
    if (va->is_null && vb->is_null) return 0;
    if (va->is_null) return g_sort_ctx.order_asc ? -1 : 1;
    if (vb->is_null) return g_sort_ctx.order_asc ? 1 : -1;
    int cmp = 0;
    DataType t = g_sort_ctx.schema->columns[ci].type;
    if (t == SV_TYPE_INT || t == SV_TYPE_BIGINT) {
        int64_t diff = va->int_val - vb->int_val;
        cmp = (diff > 0) ? 1 : (diff < 0) ? -1 : 0;
    } else if (t == SV_TYPE_FLOAT) {
        double diff = va->float_val - vb->float_val;
        cmp = (diff > 0) ? 1 : (diff < 0) ? -1 : 0;
    } else {
        const char* sa = va->str_val ? va->str_val : "";
        const char* sb = vb->str_val ? vb->str_val : "";
        cmp = strcmp(sa, sb);
    }
    return g_sort_ctx.order_asc ? cmp : -cmp;
}

static bool rows_equal(Tuple* a, Tuple* b, int* col_map, int num_cols, TableSchema* ts) {
    if (!a || !b) return false;
    for (int j = 0; j < num_cols; j++) {
        int ci = col_map ? col_map[j] : j;
        if (ci >= a->num_values || ci >= b->num_values) return false;
        Value* va = &a->values[ci];
        Value* vb = &b->values[ci];
        if (va->is_null && vb->is_null) continue;
        if (va->is_null || vb->is_null) return false;
        DataType t = ts->columns[ci].type;
        if (t == SV_TYPE_INT || t == SV_TYPE_BIGINT) {
            if (va->int_val != vb->int_val) return false;
        } else if (t == SV_TYPE_FLOAT) {
            if (va->float_val != vb->float_val) return false;
        } else {
            const char* sa = va->str_val ? va->str_val : "";
            const char* sb = vb->str_val ? vb->str_val : "";
            if (strcmp(sa, sb) != 0) return false;
        }
    }
    return true;
}

// ---- Index helpers ----
static void build_index_for_table(ExecCtx* ctx, TableSchema* ts, int idx_i) {
    Index* idx = &ts->indexes[idx_i];
    uint32_t root_id;
    if (bp_new_page(ctx->bp, &root_id) != SV_OK) return;
    idx->root_page_id = root_id;
    BTree* bt = btree_create(ctx->bp, root_id);
    if (!bt) return;
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) { free(bt); return; }
    HeapScan* scan = heap_scan_open(hf, ts);
    if (!scan) { free(hf); free(bt); return; }
    int col_i = catalog_get_col_index(ts, idx->col_name);
    RID rid;
    Tuple* t;
    while ((t = heap_scan_next(scan, &rid)) != NULL) {
        if (col_i >= 0 && col_i < t->num_values && !t->values[col_i].is_null) {
            uint64_t key = (uint64_t)t->values[col_i].int_val;
            btree_insert(bt, key, (BTreeValue){rid.page_id, rid.slot_id});
        }
        tuple_free(t);
    }
    heap_scan_close(scan);
    free(hf);
    free(bt);
}

static bool find_index(TableSchema* ts, const char* col_name) {
    for (int i = 0; i < ts->num_indexes; i++)
        if (strcmp(ts->indexes[i].col_name, col_name) == 0 && ts->indexes[i].root_page_id > 0)
            return true;
    return false;
}

// ---- Executors ----
static QueryResult* exec_create_db(ExecCtx* ctx, ASTNode* n) {
    if (catalog_add_db(ctx->catalog, n->create_db.db_name) != SV_OK)
        return result_error_msg("Failed to create database (may already exist)");
    return result_ok_msg(0);
}

static QueryResult* exec_drop_db(ExecCtx* ctx, ASTNode* n) {
    if (catalog_drop_db(ctx->catalog, n->drop_db.db_name) != SV_OK)
        return result_error_msg("Database not found");
    return result_ok_msg(0);
}

static QueryResult* exec_create_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema ts;
    memset(&ts, 0, sizeof(ts));
    snprintf(ts.name, sizeof(ts.name), "%s", n->create_table.table_name);
    ts.num_rows = 0;
    ts.num_pages = 0;

    for (int i = 0; i < n->create_table.num_columns && i < SV_MAX_COLUMNS; i++) {
        ColumnDef* cd = &n->create_table.columns[i];
        Column* col = &ts.columns[ts.num_columns++];
        memset(col, 0, sizeof(Column));
        snprintf(col->name, sizeof(col->name), "%s", cd->name);
        col->type = parse_type(cd->type);
        col->size = cd->size;
        col->nullable = cd->nullable;
        col->primary_key = cd->primary_key;
        col->auto_increment = cd->auto_increment;
        if (cd->default_val[0])
            snprintf(col->default_val, sizeof(col->default_val), "%s", cd->default_val);
    }

    if (catalog_add_table(ctx->catalog, db->name, &ts) != SV_OK)
        return result_error_msg("Failed to create table");

    TableSchema* saved = catalog_get_table(ctx->catalog, db->name, ts.name);
    if (saved) {
        uint32_t pid;
        if (bp_new_page(ctx->bp, &pid) == SV_OK) {
            saved->root_page_id = pid;
            saved->num_pages = 1;
            bp_unpin(ctx->bp, pid, false);
        }
    }
    return result_ok_msg(0);
}

static QueryResult* exec_drop_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    if (catalog_drop_table(ctx->catalog, db->name, n->drop_table.table_name) != SV_OK)
        return result_error_msg("Table not found");
    return result_ok_msg(0);
}

static QueryResult* exec_alter_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->alter_table.table_name);
    if (!ts) return result_error_msg("Table not found");
    // RENAME TO
    if (n->alter_table.new_name[0])
        snprintf(ts->name, sizeof(ts->name), "%s", n->alter_table.new_name);
    return result_ok_msg(0);
}

// ── supporto all'INSERT: massimo di una colonna e ricerca di un valore ──

static bool value_equal(const Value* a, const Value* b, DataType type) {
    if (a->is_null || b->is_null) return false;
    switch (type) {
        case SV_TYPE_INT:    return a->int_val == b->int_val;
        case SV_TYPE_BIGINT: return a->bigint_val == b->bigint_val;
        case SV_TYPE_FLOAT:  return a->float_val == b->float_val;
        case SV_TYPE_BOOL:   return a->bool_val == b->bool_val;
        default:             return a->str_val && b->str_val && strcmp(a->str_val, b->str_val) == 0;
    }
}

// true se una riga della tabella ha già quel valore nella colonna ci
static bool column_has_value(ExecCtx* ctx, TableSchema* ts, int ci, const Value* v) {
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return false;
    HeapScan* scan = heap_scan_open(hf, ts);
    bool found = false;
    Tuple* t;
    RID rid;
    while (scan && !found && (t = heap_scan_next(scan, &rid)) != NULL) {
        if (ci < t->num_values) found = value_equal(&t->values[ci], v, ts->columns[ci].type);
        tuple_free(t);
    }
    if (scan) heap_scan_close(scan);
    free(hf);
    return found;
}

// valore massimo (intero) della colonna ci; false se la tabella è vuota
static bool column_max(ExecCtx* ctx, TableSchema* ts, int ci, int64_t* out) {
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return false;
    HeapScan* scan = heap_scan_open(hf, ts);
    bool any = false;
    Tuple* t;
    RID rid;
    while (scan && (t = heap_scan_next(scan, &rid)) != NULL) {
        if (ci < t->num_values && !t->values[ci].is_null) {
            int64_t v = ts->columns[ci].type == SV_TYPE_BIGINT ? t->values[ci].bigint_val
                      : ts->columns[ci].type == SV_TYPE_FLOAT  ? (int64_t)t->values[ci].float_val
                      : t->values[ci].int_val;
            if (!any || v > *out) *out = v;
            any = true;
        }
        tuple_free(t);
    }
    if (scan) heap_scan_close(scan);
    free(hf);
    return any;
}

static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");

    const char* vals[64];
    char val_bufs[64][sizeof(((ASTExpr*)0)->str_val)];   // stesso spazio del valore nell'AST: niente troncamenti
    memset(val_bufs, 0, sizeof(val_bufs));
    int nv = n->insert_stmt.num_values < 64 ? n->insert_stmt.num_values : 64;

    // Track which columns are specified if column list exists
    bool col_specified[64] = {0};

    for (int i = 0; i < nv; i++) {
        if (n->insert_stmt.values[i]) {
            if (n->insert_stmt.values[i]->type == EXPR_INT)
                snprintf(val_bufs[i], sizeof(val_bufs[i]), "%lld", (long long)n->insert_stmt.values[i]->int_val);
            else if (n->insert_stmt.values[i]->type == EXPR_FLOAT)   // precisione piena (%g ne teneva 6 cifre)
                snprintf(val_bufs[i], sizeof(val_bufs[i]), "%.17g", n->insert_stmt.values[i]->float_val);
            else
                snprintf(val_bufs[i], sizeof(val_bufs[i]), "%s", n->insert_stmt.values[i]->str_val);
            vals[i] = n->insert_stmt.values[i]->type == EXPR_NULL ? "NULL" : val_bufs[i];
            if (i < n->insert_stmt.num_cols && n->insert_stmt.columns[i]) {
                int ci = catalog_get_col_index(ts, n->insert_stmt.columns[i]);
                if (ci >= 0) col_specified[ci] = true;
            }
        } else {
            vals[i] = "NULL";
        }
    }

    // numero di valori coerente con le colonne
    int expected = n->insert_stmt.num_cols > 0 ? n->insert_stmt.num_cols : ts->num_columns;
    if (nv != expected) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Column count doesn't match value count: %d column%s, %d value%s",
                 expected, expected == 1 ? "" : "s", nv, nv == 1 ? "" : "s");
        return result_error_msg(msg);
    }
    for (int i = 0; i < n->insert_stmt.num_cols; i++)
        if (catalog_get_col_index(ts, n->insert_stmt.columns[i]) < 0)
            return unknown_column_error(n->insert_stmt.columns[i], "INSERT");

    // Map positional values to columns
    const char* final_vals[64];
    for (int i = 0; i < ts->num_columns; i++) final_vals[i] = "NULL";

    if (n->insert_stmt.num_cols > 0) {
        for (int i = 0; i < n->insert_stmt.num_cols && i < nv; i++) {
            int ci = catalog_get_col_index(ts, n->insert_stmt.columns[i]);
            if (ci >= 0) final_vals[ci] = vals[i];
        }
    } else {
        for (int i = 0; i < nv && i < ts->num_columns; i++)
            final_vals[i] = vals[i];
    }

    // Auto-increment: valore massimo esistente nella colonna + 1 (in precedenza un contatore
    // statico condiviso fra tabelle e azzerato a ogni avvio generava id già usati)
    for (int i = 0; i < ts->num_columns; i++) {
        bool given = n->insert_stmt.num_cols > 0 ? col_specified[i] : strcmp(final_vals[i], "NULL") != 0;
        if (ts->columns[i].auto_increment && !given) {
            int64_t mx = 0;
            column_max(ctx, ts, i, &mx);
            char ai_buf[32];
            snprintf(ai_buf, sizeof(ai_buf), "%lld", (long long)(mx + 1));
            // Use values beyond the input to avoid overlap
            int slot = nv + i;
            if (slot < 64) {
                snprintf(val_bufs[slot], sizeof(val_bufs[slot]), "%s", ai_buf);
                final_vals[i] = val_bufs[slot];
            }
        }
    }

    Tuple* t = tuple_from_strings(final_vals, ts->num_columns, ts);
    if (!t) return result_error_msg("Failed to create tuple");

    // chiave primaria: obbligatoria e univoca
    for (int i = 0; i < ts->num_columns; i++) {
        if (!ts->columns[i].primary_key) continue;
        char msg[200];
        if (t->values[i].is_null) {
            tuple_free(t);
            snprintf(msg, sizeof(msg), "Primary key '%.64s' cannot be NULL", ts->columns[i].name);
            return result_error_msg(msg);
        }
        if (column_has_value(ctx, ts, i, &t->values[i])) {
            char vbuf[128];
            value_to_string(&t->values[i], ts->columns[i].type, vbuf, sizeof(vbuf));
            tuple_free(t);
            snprintf(msg, sizeof(msg), "Duplicate entry '%.60s' for primary key '%.64s'", vbuf, ts->columns[i].name);
            return result_error_msg(msg);
        }
    }

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) { tuple_free(t); return result_error_msg("Failed to open heap"); }

    RID rid;
    if (heap_insert(hf, t, ts, &rid) != SV_OK) {
        tuple_free(t); free(hf);
        return result_error_msg("Failed to insert row");
    }

    // Write WAL entry
    if (ctx->wal) {
        size_t serial_len;
        uint8_t* serial = tuple_serialize(t, ts, &serial_len);
        if (serial) {
            wal_write(ctx->wal, LOG_INSERT, ctx->tx_id, rid.page_id,
                      (SVRID){rid.page_id, rid.slot_id}, serial, (uint32_t)serial_len, NULL, 0);
            free(serial);
        }
    }

    // Update indexes
    for (int i = 0; i < ts->num_indexes; i++) {
        if (ts->indexes[i].root_page_id > 0) {
            int ci = catalog_get_col_index(ts, ts->indexes[i].col_name);
            if (ci >= 0 && !t->values[ci].is_null) {
                BTree* bt = btree_open(ctx->bp, ts->indexes[i].root_page_id);
                if (bt) {
                    uint64_t key = (uint64_t)t->values[ci].int_val;
                    btree_insert(bt, key, (BTreeValue){rid.page_id, rid.slot_id});
                    free(bt);
                }
            }
        }
    }

    ts->num_rows++;
    ts->num_pages = hf->num_pages;
    free(hf);
    tuple_free(t);
    return result_ok_msg(1);
}

// Forward declaration for view recursion

// ── tipi del risultato ───────────────────────────────────────────

static CellType cell_type_of(DataType t) {
    switch (t) {
        case SV_TYPE_INT: case SV_TYPE_BIGINT:     return CELL_TYPE_INT;
        case SV_TYPE_FLOAT: case SV_TYPE_DECIMAL:  return CELL_TYPE_FLOAT;
        case SV_TYPE_BOOL:                         return CELL_TYPE_BOOL;
        case SV_TYPE_DATE: case SV_TYPE_DATETIME:  return CELL_TYPE_DATE;
        case SV_TYPE_BLOB:                         return CELL_TYPE_BLOB;
        default:                                   return CELL_TYPE_STRING;
    }
}

// imposta il tipo di ogni colonna (nome per l'intestazione della griglia) e delle sue celle
// a partire dai tipi dello schema; dt[i] < 0 = colonna calcolata, si lascia com'è
static void apply_result_types(QueryResult* r, const int* dt) {
    if (!r || r->num_cols <= 0) return;
    if (!r->col_types) r->col_types = calloc((size_t)r->num_cols + 1, sizeof(char*));
    if (!r->col_types) return;
    for (int i = 0; i < r->num_cols; i++)
        if (!r->col_types[i]) r->col_types[i] = strdup(dt[i] >= 0 ? datatype_name((DataType)dt[i]) : "");
    for (int k = 0; k < r->num_rows; k++) {
        Row* row = &r->rows[k];
        for (int i = 0; i < r->num_cols && i < row->num_cells; i++) {
            Cell* c = &row->cells[i];
            if (c->is_null) c->type = CELL_TYPE_NULL;
            else if (dt[i] >= 0) c->type = cell_type_of((DataType)dt[i]);
        }
    }
}

static QueryResult* exec_select(ExecCtx* ctx, ASTNode* n);

static QueryResult* exec_select(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    // Check if this is a view
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->select_stmt.table_name);
    if (!ts) {
        ViewSchema* view = catalog_get_view(ctx->catalog, db->name, n->select_stmt.table_name);
        if (view) {
            // Parse and execute the view definition
            TokenList* vtokens = lexer_tokenize(view->definition);
            if (!vtokens) return result_error_msg("Failed to tokenize view definition");
            ParseResult* vparsed = parser_parse(vtokens);
            if (!vparsed->success || vparsed->count == 0) {
                lexer_free(vtokens);
                parse_result_free(vparsed);
                return result_error_msg("Failed to parse view definition");
            }
            QueryResult* r = exec_select(ctx, vparsed->statements[0]);
            lexer_free(vtokens);
            parse_result_free(vparsed);
            return r;
        }
        return result_error_msg("Table or view not found");
    }
    const char* bad = where_unknown_column(n->select_stmt.where, ts);
    if (bad) return unknown_column_error(bad, "WHERE");

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    // Collect all rows
    Tuple** rows = NULL;
    int num_rows = 0, cap = 0;
    RID* rids = NULL;

    // Try index scan if WHERE is simple equality on indexed column
    bool use_index = false;
    BTree* idx_bt = NULL;

    // indice usato solo per "chiave_primaria_intera = costante": la chiave è univoca, quindi una
    // sola riga. La riga trovata viene verificata; se la voce è obsoleta (riga aggiornata o
    // cancellata) si ripiega sulla scansione completa.
    ASTExpr* w = n->select_stmt.where;
    int wci = (w && w->type == EXPR_BINARY && w->op == OP_EQ && w->left && w->left->type == EXPR_COLUMN &&
               w->right && w->right->type == EXPR_INT) ? catalog_get_col_index(ts, w->left->col_name) : -1;
    bool pk_int = wci >= 0 && ts->columns[wci].primary_key &&
                  (ts->columns[wci].type == SV_TYPE_INT || ts->columns[wci].type == SV_TYPE_BIGINT);
    if (pk_int) {
        const char* where_col = w->left->col_name;
        for (int i = 0; i < ts->num_indexes && !use_index; i++) {
            if (strcmp(ts->indexes[i].col_name, where_col) == 0 && ts->indexes[i].root_page_id > 0) {
                idx_bt = btree_open(ctx->bp, ts->indexes[i].root_page_id);
                if (idx_bt) {
                    BTreeValue btv;
                    if (btree_search(idx_bt, (uint64_t)n->select_stmt.where->right->int_val, &btv)) {
                        Tuple* t = heap_get(hf, (RID){btv.page_id, btv.slot_id}, ts);
                        if (t && !eval_where(w, t, ts)) { tuple_free(t); t = NULL; }
                        if (t) {
                            rows = malloc(sizeof(Tuple*));
                            rids = malloc(sizeof(RID));
                            rows[0] = t;
                            rids[0] = (RID){btv.page_id, btv.slot_id};
                            num_rows = 1;
                            use_index = true;
                        }
                    }
                    free(idx_bt);
                }
            }
        }
    }

    if (!use_index) {
        HeapScan* scan = heap_scan_open(hf, ts);
        if (!scan) { free(hf); return result_error_msg("Failed to scan"); }

        RID rid;
        Tuple* t;
        while ((t = heap_scan_next(scan, &rid)) != NULL) {
            if (num_rows >= cap) {
                cap = cap ? cap * 2 : 64;
                rows = realloc(rows, cap * sizeof(Tuple*));
                rids = realloc(rids, cap * sizeof(RID));
            }
            rows[num_rows] = t;
            rids[num_rows] = rid;
            num_rows++;
        }
        heap_scan_close(scan);
    }
    free(hf);

    // Determine columns to output
    int out_cols = n->select_stmt.num_cols;
    int* col_map = NULL;

    if (out_cols == 0 || (out_cols == 1 && n->select_stmt.columns[0]->type == EXPR_STAR)) {
        // SELECT *: mappa identità
        out_cols = ts->num_columns;
        col_map = malloc(out_cols * sizeof(int));
        for (int i = 0; i < out_cols; i++) col_map[i] = i;
    } else {
        col_map = malloc(out_cols * sizeof(int));
        for (int i = 0; i < out_cols; i++)
            col_map[i] = catalog_get_col_index(ts, n->select_stmt.columns[i]->col_name);
    }

    // Apply WHERE filter and collect indices of surviving rows
    int* valid_idx = malloc(num_rows * sizeof(int));
    int num_valid = 0;
    for (int i = 0; i < num_rows; i++) {
        if (eval_where(n->select_stmt.where, rows[i], ts)) {
            valid_idx[num_valid++] = i;
        } else {
            tuple_free(rows[i]);
        }
    }

    // ORDER BY
    if (n->select_stmt.order_col[0] && num_valid > 0) {
        int sort_col = catalog_get_col_index(ts, n->select_stmt.order_col);
        if (sort_col >= 0) {
            g_sort_ctx.rows = rows;
            g_sort_ctx.col_map = col_map;
            g_sort_ctx.sort_col = sort_col;
            g_sort_ctx.order_asc = n->select_stmt.order_asc;
            g_sort_ctx.schema = ts;
            qsort(valid_idx, num_valid, sizeof(int), row_compare);
        }
    }

    // Build result
    if (out_cols < 0 || out_cols > 256) out_cols = 0;
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = out_cols;
    r->col_names = calloc(out_cols > 0 ? out_cols : 1, sizeof(char*));
    for (int i = 0; i < out_cols; i++) {
        int ci = col_map[i];
        r->col_names[i] = (ci >= 0 && ci < ts->num_columns) ? strdup(ts->columns[ci].name) : strdup("?");
    }

    // Determine final row count (apply LIMIT and DISTINCT)
    int max_rows = (n->select_stmt.limit > 0 && n->select_stmt.limit < num_valid) ? n->select_stmt.limit : num_valid;
    r->rows = calloc(max_rows, sizeof(Row));
    int out_idx = 0;
    Tuple* prev_distinct = NULL;

    for (int vi = 0; vi < max_rows; vi++) {
        int i = valid_idx[vi];
        Tuple* tp = rows[i];

        // DISTINCT: skip if same as previous non-duplicate row
        if (n->select_stmt.distinct && prev_distinct) {
            if (rows_equal(tp, prev_distinct, col_map, out_cols, ts)) {
                tuple_free(tp);
                continue;
            }
        }
        if (prev_distinct) tuple_free(prev_distinct);
        prev_distinct = NULL;

        Row* row = &r->rows[out_idx++];
        row->num_cells = out_cols;
        row->cells = calloc(out_cols, sizeof(Cell));

        for (int j = 0; j < out_cols; j++) {
            int ci = col_map[j];
            Cell* cell = &row->cells[j];
            if (ci >= 0 && ci < tp->num_values) {
                Value* v = &tp->values[ci];
                cell->is_null = v->is_null;
                cell->type = CELL_TYPE_STRING;
                if (!v->is_null)
                    value_to_string(v, ts->columns[ci].type, cell->value, sizeof(cell->value));
                else
                    snprintf(cell->value, sizeof(cell->value), "NULL");
            } else {
                cell->is_null = true;
                snprintf(cell->value, sizeof(cell->value), "NULL");
            }
        }
        if (n->select_stmt.distinct)
            prev_distinct = tp;
        else
            tuple_free(tp);
    }
    if (prev_distinct) tuple_free(prev_distinct);

    r->num_rows = out_idx;

    {
        int* dt = calloc((size_t)out_cols + 1, sizeof(int));
        if (dt) {
            for (int i = 0; i < out_cols; i++) {
                int ci = col_map[i];
                dt[i] = (ci >= 0 && ci < ts->num_columns) ? (int)ts->columns[ci].type : -1;
            }
            apply_result_types(r, dt);
            free(dt);
        }
    }

    free(col_map);
    free(valid_idx);
    free(rows);
    free(rids);
    return r;
}

static QueryResult* exec_update(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->update_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");
    if (catalog_get_col_index(ts, n->update_stmt.set_col) < 0) return unknown_column_error(n->update_stmt.set_col, "SET");
    const char* bad = where_unknown_column(n->update_stmt.where, ts);
    if (bad) return unknown_column_error(bad, "WHERE");

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    // First pass: collect matching RIDs (avoid heap_update infinite loop)
    RID* match_rids = NULL;
    int num_match = 0, cap_match = 0;
    {
        HeapScan* scan = heap_scan_open(hf, ts);
        if (!scan) { free(hf); return result_error_msg("Failed to scan"); }
        RID rid;
        Tuple* t;
        while ((t = heap_scan_next(scan, &rid)) != NULL) {
            if (eval_where(n->update_stmt.where, t, ts)) {
                if (num_match >= cap_match) {
                    cap_match = cap_match ? cap_match * 2 : 64;
                    match_rids = realloc(match_rids, cap_match * sizeof(RID));
                }
                match_rids[num_match++] = rid;
            }
            tuple_free(t);
        }
        heap_scan_close(scan);
    }

    // Second pass: update each matched row
    int affected = 0;
    for (int i = 0; i < num_match; i++) {
        Tuple* t = heap_get(hf, match_rids[i], ts);
        if (!t) continue;

        uint8_t* old_serial = NULL;
        uint32_t old_serial_len = 0;
        if (ctx->wal) {
            size_t slen;
            old_serial = tuple_serialize(t, ts, &slen);
            old_serial_len = (uint32_t)slen;
        }

        int ci = catalog_get_col_index(ts, n->update_stmt.set_col);
        if (ci >= 0) {
            // il valore viene convertito secondo il tipo della colonna, non del letterale
            ASTExpr* se = n->update_stmt.set_expr;
            char text[sizeof(se->str_val)];
            const char* src = text;
            if (se->type == EXPR_INT)        snprintf(text, sizeof(text), "%lld", (long long)se->int_val);
            else if (se->type == EXPR_FLOAT) snprintf(text, sizeof(text), "%.17g", se->float_val);
            else if (se->type == EXPR_NULL)  src = NULL;
            else                             src = se->str_val;
            value_set_from_text(&t->values[ci], ts->columns[ci].type, src);
            RID new_rid = match_rids[i];
            heap_update(hf, match_rids[i], t, ts, &new_rid);
            if (ctx->wal && old_serial) {
                size_t new_len;
                uint8_t* new_serial = tuple_serialize(t, ts, &new_len);
                if (new_serial) {
                    wal_write(ctx->wal, LOG_UPDATE, ctx->tx_id, match_rids[i].page_id,
                              (SVRID){match_rids[i].page_id, match_rids[i].slot_id},
                              new_serial, (uint32_t)new_len, old_serial, old_serial_len);
                    free(new_serial);
                }
            }
            // indici: chiave = valore della colonna dell'indice, puntata alla nuova posizione della riga
            for (int idx_i = 0; idx_i < ts->num_indexes; idx_i++) {
                if (ts->indexes[idx_i].root_page_id == 0) continue;
                int ic = catalog_get_col_index(ts, ts->indexes[idx_i].col_name);
                if (ic < 0 || t->values[ic].is_null) continue;
                DataType it = ts->columns[ic].type;
                if (it != SV_TYPE_INT && it != SV_TYPE_BIGINT) continue;
                BTree* bt = btree_open(ctx->bp, ts->indexes[idx_i].root_page_id);
                if (bt) {
                    uint64_t key = it == SV_TYPE_INT ? (uint64_t)t->values[ic].int_val : (uint64_t)t->values[ic].bigint_val;
                    btree_insert(bt, key, (BTreeValue){new_rid.page_id, new_rid.slot_id});
                    free(bt);
                }
            }
            affected++;
        }
        free(old_serial);
        tuple_free(t);
    }

    free(match_rids);
    free(hf);
    return result_ok_msg(affected);
}

static QueryResult* exec_delete(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");
    const char* bad = where_unknown_column(n->delete_stmt.where, ts);
    if (bad) return unknown_column_error(bad, "WHERE");

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    // First pass: collect matching RIDs
    RID* match_rids = NULL;
    int num_match = 0, cap_match = 0;
    {
        HeapScan* scan = heap_scan_open(hf, ts);
        if (!scan) { free(hf); return result_error_msg("Failed to scan"); }
        RID rid;
        Tuple* t;
        while ((t = heap_scan_next(scan, &rid)) != NULL) {
            if (eval_where(n->delete_stmt.where, t, ts)) {
                if (num_match >= cap_match) {
                    cap_match = cap_match ? cap_match * 2 : 64;
                    match_rids = realloc(match_rids, cap_match * sizeof(RID));
                }
                match_rids[num_match++] = rid;
            }
            tuple_free(t);
        }
        heap_scan_close(scan);
    }

    // Second pass: delete each matched row
    int affected = 0;
    for (int i = 0; i < num_match; i++) {
        Tuple* t = heap_get(hf, match_rids[i], ts);
        if (!t) continue;

        for (int idx_i = 0; idx_i < ts->num_indexes; idx_i++) {
            if (ts->indexes[idx_i].root_page_id > 0) {
                int ci = catalog_get_col_index(ts, ts->indexes[idx_i].col_name);
                if (ci >= 0 && !t->values[ci].is_null) {
                    BTree* bt = btree_open(ctx->bp, ts->indexes[idx_i].root_page_id);
                    if (bt) {
                        btree_delete(bt, (uint64_t)t->values[ci].int_val);
                        free(bt);
                    }
                }
            }
        }
        if (ctx->wal) {
            size_t serial_len;
            uint8_t* serial = tuple_serialize(t, ts, &serial_len);
            if (serial) {
                wal_write(ctx->wal, LOG_DELETE, ctx->tx_id, match_rids[i].page_id,
                          (SVRID){match_rids[i].page_id, match_rids[i].slot_id}, NULL, 0, serial, (uint32_t)serial_len);
                free(serial);
            }
        }
        heap_delete(hf, match_rids[i]);
        ts->num_rows--;
        affected++;
        tuple_free(t);
    }

    free(match_rids);
    free(hf);
    return result_ok_msg(affected);
}

static QueryResult* exec_use(ExecCtx* ctx, ASTNode* n) {
    if (catalog_use_db(ctx->catalog, n->use_stmt.db_name) != SV_OK)
        return result_error_msg("Database not found");
    return result_ok_msg(0);
}

static QueryResult* exec_show(ExecCtx* ctx, ASTNode* n) {
    (void)n;
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;

    Database* db = catalog_current_db(ctx->catalog);
    if (!db) {
        r->num_cols = 1;
        r->col_names = calloc(1, sizeof(char*));
        r->col_names[0] = strdup("Database");
        r->num_rows = ctx->catalog->num_databases;
        r->rows = calloc(r->num_rows, sizeof(Row));
        for (int i = 0; i < r->num_rows; i++) {
            r->rows[i].num_cells = 1;
            r->rows[i].cells = calloc(1, sizeof(Cell));
            snprintf(r->rows[i].cells[0].value, sizeof(r->rows[i].cells[0].value),
                     "%s", ctx->catalog->databases[i].name);
        }
    } else {
        r->num_cols = 1;
        r->col_names = calloc(1, sizeof(char*));
        r->col_names[0] = strdup("Tables");
        r->num_rows = db->num_tables;
        r->rows = calloc(r->num_rows, sizeof(Row));
        for (int i = 0; i < r->num_rows; i++) {
            r->rows[i].num_cells = 1;
            r->rows[i].cells = calloc(1, sizeof(Cell));
            snprintf(r->rows[i].cells[0].value, sizeof(r->rows[i].cells[0].value),
                     "%s", db->tables[i].name);
        }
    }
    return r;
}

static QueryResult* exec_describe(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    const char* tname = n->select_stmt.table_name;
    if (!tname[0]) return result_error_msg("No table specified");

    int num_cols;
    ColumnInfo* cols = db_list_columns(db->name, tname, &num_cols);
    if (!cols) return result_error_msg("Table not found");

    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = 4;
    r->col_names = calloc(4, sizeof(char*));
    r->col_names[0] = strdup("Field");
    r->col_names[1] = strdup("Type");
    r->col_names[2] = strdup("Null");
    r->col_names[3] = strdup("Key");
    r->num_rows = num_cols;
    r->rows = calloc(num_cols, sizeof(Row));
    for (int i = 0; i < num_cols; i++) {
        r->rows[i].num_cells = 4;
        r->rows[i].cells = calloc(4, sizeof(Cell));
        snprintf(r->rows[i].cells[0].value, sizeof(r->rows[i].cells[0].value), "%s", cols[i].name);
        snprintf(r->rows[i].cells[1].value, sizeof(r->rows[i].cells[1].value), "%s", cols[i].type);
        snprintf(r->rows[i].cells[2].value, sizeof(r->rows[i].cells[2].value), "%s", cols[i].nullable ? "YES" : "NO");
        snprintf(r->rows[i].cells[3].value, sizeof(r->rows[i].cells[3].value), "%s", cols[i].primary_key ? "PRI" : "");
    }
    db_columns_free(cols, num_cols);
    return r;
}

static QueryResult* exec_create_index(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->create_index.table_name);
    if (!ts) return result_error_msg("Table not found");

    Index idx;
    memset(&idx, 0, sizeof(idx));
    snprintf(idx.name, sizeof(idx.name), "%s", n->create_index.index_name);
    snprintf(idx.col_name, sizeof(idx.col_name), "%s", n->create_index.col_name);
    snprintf(idx.type, sizeof(idx.type), "BTREE");

    if (catalog_add_index(ctx->catalog, db->name, n->create_index.table_name, &idx) != SV_OK)
        return result_error_msg("Failed to create index");

    // Build the B+Tree and populate it with existing rows
    TableSchema* updated_ts = catalog_get_table(ctx->catalog, db->name, n->create_index.table_name);
    if (updated_ts) {
        build_index_for_table(ctx, updated_ts, updated_ts->num_indexes - 1);
    }

    return result_ok_msg(0);
}

static QueryResult* exec_drop_index(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->drop_index.table_name);
    if (!ts) return result_error_msg("Table not found");
    for (int i = 0; i < ts->num_indexes; i++) {
        if (strcmp(ts->indexes[i].name, n->drop_index.index_name) == 0) {
            memmove(&ts->indexes[i], &ts->indexes[i+1], (ts->num_indexes - i - 1) * sizeof(Index));
            ts->num_indexes--;
            return result_ok_msg(0);
        }
    }
    return result_error_msg("Index not found");
}

static QueryResult* exec_create_view(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    ViewSchema vs;
    memset(&vs, 0, sizeof(vs));
    snprintf(vs.name, sizeof(vs.name), "%s", n->create_view.view_name);
    snprintf(vs.definition, sizeof(vs.definition), "%s", n->create_view.definition);
    if (catalog_add_view(ctx->catalog, db->name, &vs) != SV_OK)
        return result_error_msg("Failed to create view");
    return result_ok_msg(0);
}

static QueryResult* exec_drop_view(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    if (catalog_drop_view(ctx->catalog, db->name, n->drop_view.view_name) != SV_OK)
        return result_error_msg("View not found");
    return result_ok_msg(0);
}

static QueryResult* exec_truncate(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");
    // Delete all rows
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");
    int affected = 0;
    HeapScan* scan = heap_scan_open(hf, ts);
    if (!scan) { free(hf); return result_error_msg("Failed to scan"); }
    RID rid;
    Tuple* t;
    while ((t = heap_scan_next(scan, &rid)) != NULL) {
        heap_delete(hf, rid);
        affected++;
        tuple_free(t);
    }
    heap_scan_close(scan);
    ts->num_rows = 0;
    free(hf);
    return result_ok_msg(affected);
}

static QueryResult* exec_explain(ExecCtx* ctx, ASTNode* n) {
    (void)n;
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    // Build a textual explain result
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = 4;
    r->col_names = calloc(4, sizeof(char*));
    r->col_names[0] = strdup("Operation");
    r->col_names[1] = strdup("Target");
    r->col_names[2] = strdup("Condition");
    r->col_names[3] = strdup("Estimated Rows");
    r->num_rows = 2;
    r->rows = calloc(2, sizeof(Row));

    // Node 0: Seq Scan
    r->rows[0].num_cells = 4;
    r->rows[0].cells = calloc(4, sizeof(Cell));
    snprintf(r->rows[0].cells[0].value, sizeof(r->rows[0].cells[0].value), "Seq Scan");
    snprintf(r->rows[0].cells[1].value, sizeof(r->rows[0].cells[1].value), "table");
    snprintf(r->rows[0].cells[2].value, sizeof(r->rows[0].cells[2].value), "WHERE");
    snprintf(r->rows[0].cells[3].value, sizeof(r->rows[0].cells[3].value), "~all");

    // Node 1: Output
    r->rows[1].num_cells = 4;
    r->rows[1].cells = calloc(4, sizeof(Cell));
    snprintf(r->rows[1].cells[0].value, sizeof(r->rows[1].cells[0].value), "Projection");
    snprintf(r->rows[1].cells[1].value, sizeof(r->rows[1].cells[1].value), "columns");
    return r;
}

static int find_join_col(ASTExpr* col, TableSchema* ts1, TableSchema* ts2) {
    if (col->table_name[0]) {
        if (strcmp(col->table_name, ts1->name) == 0)
            return catalog_get_col_index(ts1, col->col_name);
        int ci = catalog_get_col_index(ts2, col->col_name);
        return (ci >= 0) ? ci + ts1->num_columns : -1;
    }
    int ci = catalog_get_col_index(ts1, col->col_name);
    if (ci >= 0) return ci;
    ci = catalog_get_col_index(ts2, col->col_name);
    return (ci >= 0) ? ci + ts1->num_columns : -1;
}

static bool eval_on_cond(ASTExpr* cond, Tuple* t1, Tuple* t2,
                          TableSchema* ts1, TableSchema* ts2) {
    if (!cond) return true;
    int li = find_join_col(cond->left, ts1, ts2);
    int ri = find_join_col(cond->right, ts1, ts2);
    if (li < 0 || ri < 0) return true;
    Value* lv = (li < ts1->num_columns) ? &t1->values[li] : &t2->values[li - ts1->num_columns];
    Value* rv = (ri < ts1->num_columns) ? &t1->values[ri] : &t2->values[ri - ts1->num_columns];
    if (lv->is_null || rv->is_null) return false;
    char lbuf[64], rbuf[64];
    DataType lt = (li < ts1->num_columns) ? ts1->columns[li].type : ts2->columns[li - ts1->num_columns].type;
    DataType rt = (ri < ts1->num_columns) ? ts1->columns[ri].type : ts2->columns[ri - ts1->num_columns].type;
    value_to_string(lv, lt, lbuf, sizeof(lbuf));
    value_to_string(rv, rt, rbuf, sizeof(rbuf));
    return strcmp(lbuf, rbuf) == 0;
}

static QueryResult* exec_join(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    char tables[2][64];
    snprintf(tables[0], sizeof(tables[0]), "%s", n->select_stmt.table_name);
    if (n->select_stmt.num_joins > 0)
        snprintf(tables[1], sizeof(tables[1]), "%s", n->select_stmt.joins[0].table_name);
    else
        return result_error_msg("JOIN requires two tables");

    TableSchema* ts1 = catalog_get_table(ctx->catalog, db->name, tables[0]);
    TableSchema* ts2 = catalog_get_table(ctx->catalog, db->name, tables[1]);
    if (!ts1 || !ts2) return result_error_msg("Table not found in JOIN");

    HeapFile* hf1 = heap_open(ctx->bp, ts1->root_page_id, ts1->num_pages);
    HeapFile* hf2 = heap_open(ctx->bp, ts2->root_page_id, ts2->num_pages);
    if (!hf1 || !hf2) { free(hf1); free(hf2); return result_error_msg("Failed to open heap"); }

    Tuple** rows1 = NULL; int n1 = 0, c1 = 0;
    Tuple** rows2 = NULL; int n2 = 0, c2 = 0;

    HeapScan* s1 = heap_scan_open(hf1, ts1);
    if (s1) { RID r; Tuple* t; while ((t = heap_scan_next(s1, &r)) != NULL) { if (n1 >= c1) { c1 = c1 ? c1*2 : 64; rows1 = realloc(rows1, c1*sizeof(Tuple*)); } rows1[n1++] = t; } heap_scan_close(s1); }
    HeapScan* s2 = heap_scan_open(hf2, ts2);
    if (s2) { RID r; Tuple* t; while ((t = heap_scan_next(s2, &r)) != NULL) { if (n2 >= c2) { c2 = c2 ? c2*2 : 64; rows2 = realloc(rows2, c2*sizeof(Tuple*)); } rows2[n2++] = t; } heap_scan_close(s2); }

    free(hf1); free(hf2);

    int total_cols = ts1->num_columns + ts2->num_columns;

    // Determine output columns and map
    int out_cols = n->select_stmt.num_cols;
    int* col_map = NULL;
    bool output_all = false;

    if (out_cols == 0 || (out_cols == 1 && n->select_stmt.columns[0]->type == EXPR_STAR && !n->select_stmt.columns[0]->table_name[0])) {
        output_all = true;
        out_cols = total_cols;
    } else if (out_cols == 1 && n->select_stmt.columns[0]->type == EXPR_STAR && n->select_stmt.columns[0]->table_name[0]) {
        output_all = true;
        out_cols = (strcmp(n->select_stmt.columns[0]->table_name, ts1->name) == 0) ? ts1->num_columns : ts2->num_columns;
    } else {
        col_map = malloc(out_cols * sizeof(int));
        for (int i = 0; i < out_cols; i++) {
            if (n->select_stmt.columns[i]->type == EXPR_STAR)
                col_map[i] = -1;
            else
                col_map[i] = find_join_col(n->select_stmt.columns[i], ts1, ts2);
        }
    }

    ASTExpr* on_cond = (n->select_stmt.num_joins > 0) ? n->select_stmt.joins[0].condition : NULL;

    if (out_cols < 0) out_cols = 0;
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = out_cols;
    r->col_names = calloc((size_t)out_cols + 1, sizeof(char*));

    for (int i = 0; i < out_cols; i++) {
        int ci = output_all ? i : (col_map ? col_map[i] : -1);
        if (ci < 0) { r->col_names[i] = strdup("?"); continue; }
        char buf[128];
        if (ci < ts1->num_columns)
            snprintf(buf, sizeof(buf), "%s.%s", ts1->name, ts1->columns[ci].name);
        else
            snprintf(buf, sizeof(buf), "%s.%s", ts2->name, ts2->columns[ci - ts1->num_columns].name);
        r->col_names[i] = strdup(buf);
    }

    JoinType join_type = (n->select_stmt.num_joins > 0) ? n->select_stmt.joins[0].type : JOIN_INNER;
    int max_alloc = (join_type == JOIN_LEFT) ? n1 * (n2 > 0 ? n2 : 1) : n1 * n2;
    if (max_alloc < 1) max_alloc = 1;
    r->rows = calloc(max_alloc, sizeof(Row));
    int out = 0;

    bool* right_matched = join_type == JOIN_RIGHT ? calloc(n2, sizeof(bool)) : NULL;
    for (int i = 0; i < n1; i++) {
        if (atomic_load(&g_sv_scan_cancel)) break;   // SELECT annullata dall'utente
        bool matched = false;
        for (int j = 0; j < n2; j++) {
            if (!eval_on_cond(on_cond, rows1[i], rows2[j], ts1, ts2)) continue;
            matched = true;
            if (right_matched) right_matched[j] = true;

            Row* row = &r->rows[out++];
            row->num_cells = total_cols;
            row->cells = calloc(total_cols, sizeof(Cell));
            int ci = 0;
            for (int k = 0; k < ts1->num_columns; k++, ci++) {
                Value* v = &rows1[i]->values[k];
                row->cells[ci].is_null = v->is_null;
                if (!v->is_null) value_to_string(v, ts1->columns[k].type, row->cells[ci].value, sizeof(row->cells[ci].value));
                else snprintf(row->cells[ci].value, sizeof(row->cells[ci].value), "NULL");
            }
            for (int k = 0; k < ts2->num_columns; k++, ci++) {
                Value* v = &rows2[j]->values[k];
                row->cells[ci].is_null = v->is_null;
                if (!v->is_null) value_to_string(v, ts2->columns[k].type, row->cells[ci].value, sizeof(row->cells[ci].value));
                else snprintf(row->cells[ci].value, sizeof(row->cells[ci].value), "NULL");
            }
        }
        if (!matched && join_type == JOIN_LEFT) {
            Row* row = &r->rows[out++];
            row->num_cells = total_cols;
            row->cells = calloc(total_cols, sizeof(Cell));
            int ci = 0;
            for (int k = 0; k < ts1->num_columns; k++, ci++) {
                Value* v = &rows1[i]->values[k];
                row->cells[ci].is_null = v->is_null;
                if (!v->is_null) value_to_string(v, ts1->columns[k].type, row->cells[ci].value, sizeof(row->cells[ci].value));
                else snprintf(row->cells[ci].value, sizeof(row->cells[ci].value), "NULL");
            }
            for (int k = 0; k < ts2->num_columns; k++, ci++) {
                row->cells[ci].is_null = true;
                snprintf(row->cells[ci].value, sizeof(row->cells[ci].value), "NULL");
            }
        }
    }
    // RIGHT JOIN: add unmatched right rows with NULL left columns
    if (right_matched) {
        for (int j = 0; j < n2; j++) {
            if (right_matched[j]) continue;
            Row* row = &r->rows[out++];
            row->num_cells = total_cols;
            row->cells = calloc(total_cols, sizeof(Cell));
            int ci = 0;
            for (int k = 0; k < ts1->num_columns; k++, ci++) {
                row->cells[ci].is_null = true;
                snprintf(row->cells[ci].value, sizeof(row->cells[ci].value), "NULL");
            }
            for (int k = 0; k < ts2->num_columns; k++, ci++) {
                Value* v = &rows2[j]->values[k];
                row->cells[ci].is_null = v->is_null;
                if (!v->is_null) value_to_string(v, ts2->columns[k].type, row->cells[ci].value, sizeof(row->cells[ci].value));
                else snprintf(row->cells[ci].value, sizeof(row->cells[ci].value), "NULL");
            }
        }
        free(right_matched);
    }
    r->num_rows = out;

    // Apply WHERE clause
    if (n->select_stmt.where) {
        int wi = 0;
        for (int i = 0; i < out; i++) {
            Row* row = &r->rows[i];
            bool keep = true;
            ASTExpr* w = n->select_stmt.where;
            if (w->type == EXPR_BINARY) {
                int li = find_join_col(w->left, ts1, ts2);
                if (li >= 0 && li < row->num_cells) {
                    if (w->right->type == EXPR_INT) {
                        if (row->cells[li].is_null) { keep = false; }
                        else {
                            int64_t cv = atoll(row->cells[li].value);
                            switch (w->op) {
                                case OP_EQ: keep = (cv == w->right->int_val); break;
                                case OP_NEQ: keep = (cv != w->right->int_val); break;
                                case OP_LT: keep = (cv < w->right->int_val); break;
                                case OP_GT: keep = (cv > w->right->int_val); break;
                                case OP_LE: keep = (cv <= w->right->int_val); break;
                                case OP_GE: keep = (cv >= w->right->int_val); break;
                                default: keep = true;
                            }
                        }
                    } else if (w->right->type == EXPR_FLOAT) {
                        if (row->cells[li].is_null) { keep = false; }
                        else {
                            double cv = atof(row->cells[li].value);
                            switch (w->op) {
                                case OP_EQ: keep = (cv == w->right->float_val); break;
                                case OP_NEQ: keep = (cv != w->right->float_val); break;
                                case OP_LT: keep = (cv < w->right->float_val); break;
                                case OP_GT: keep = (cv > w->right->float_val); break;
                                case OP_LE: keep = (cv <= w->right->float_val); break;
                                case OP_GE: keep = (cv >= w->right->float_val); break;
                                default: keep = true;
                            }
                        }
                    } else if (w->right->type == EXPR_STRING) {
                        if (row->cells[li].is_null) { keep = false; }
                        else {
                            const char* rv = w->right->str_val;
                            if (rv[0] == '\'' || rv[0] == '"') rv++;
                            char buf[512]; snprintf(buf, sizeof(buf), "%s", rv);
                            int blen = strlen(buf);
                            if (blen > 0 && (buf[blen-1] == '\'' || buf[blen-1] == '"')) buf[blen-1] = '\0';
                            if (w->op == OP_EQ) keep = (strcmp(row->cells[li].value, buf) == 0);
                        }
                    } else if (w->right->type == EXPR_COLUMN) {
                        int ri = find_join_col(w->right, ts1, ts2);
                        if (ri >= 0 && ri < row->num_cells) {
                            if (row->cells[li].is_null || row->cells[ri].is_null) keep = false;
                            else if (w->op == OP_EQ) keep = (strcmp(row->cells[li].value, row->cells[ri].value) == 0);
                        }
                    }
                }
            }
            if (keep) {
                if (wi != i) r->rows[wi] = r->rows[i];
                wi++;
            } else {
                free(row->cells);
            }
        }
        r->num_rows = wi;
    }

    // Apply column projection
    if (!output_all && col_map) {
        for (int i = 0; i < r->num_rows; i++) {
            Row* row = &r->rows[i];
            Cell* new_cells = calloc(out_cols, sizeof(Cell));
            for (int j = 0; j < out_cols; j++) {
                int ci = col_map[j];
                if (ci >= 0 && ci < row->num_cells)
                    new_cells[j] = row->cells[ci];
            }
            free(row->cells);
            row->cells = new_cells;
            row->num_cells = out_cols;
        }
    }

    // DISTINCT for JOIN
    if (n->select_stmt.distinct && r->num_rows > 1) {
        int wi = 0;
        for (int i = 0; i < r->num_rows; i++) {
            bool dup = false;
            for (int j = 0; j < wi; j++) {
                bool same = true;
                for (int c = 0; c < out_cols; c++) {
                    if (strcmp(r->rows[i].cells[c].value, r->rows[j].cells[c].value) != 0 ||
                        r->rows[i].cells[c].is_null != r->rows[j].cells[c].is_null) {
                        same = false; break;
                    }
                }
                if (same) { dup = true; break; }
            }
            if (!dup) {
                if (wi != i) r->rows[wi] = r->rows[i];
                wi++;
            } else {
                free(r->rows[i].cells);
            }
        }
        r->num_rows = wi;
    }

    // ORDER BY for JOIN
    if (n->select_stmt.order_col[0] && r->num_rows > 0) {
        int sort_col = -1;
        for (int i = 0; i < out_cols; i++) {
            const char* dot = strchr(r->col_names[i], '.');
            if (strcmp(r->col_names[i], n->select_stmt.order_col) == 0 ||
                (dot && strcmp(dot + 1, n->select_stmt.order_col) == 0)) {
                sort_col = i;
                break;
            }
        }
        if (sort_col >= 0) {
            g_sort_rows.rows = r->rows;
            g_sort_rows.sort_col = sort_col;
            g_sort_rows.order_asc = n->select_stmt.order_asc;
            qsort(r->rows, r->num_rows, sizeof(Row), row_compare_cells);
        }
    }

    // LIMIT for JOIN
    if (n->select_stmt.limit > 0 && r->num_rows > n->select_stmt.limit) {
        for (int i = n->select_stmt.limit; i < r->num_rows; i++)
            free(r->rows[i].cells);
        r->num_rows = n->select_stmt.limit;
        r->rows = realloc(r->rows, r->num_rows * sizeof(Row));
    }

    {
        int* dt = calloc((size_t)out_cols + 1, sizeof(int));
        if (dt) {
            for (int i = 0; i < out_cols; i++) {
                int ci = output_all ? i : (col_map ? col_map[i] : -1);
                dt[i] = ci < 0 ? -1
                      : ci < ts1->num_columns ? (int)ts1->columns[ci].type
                      : ci - ts1->num_columns < ts2->num_columns ? (int)ts2->columns[ci - ts1->num_columns].type : -1;
            }
            apply_result_types(r, dt);
            free(dt);
        }
    }

    for (int i = 0; i < n1; i++) tuple_free(rows1[i]);
    for (int i = 0; i < n2; i++) tuple_free(rows2[i]);
    free(rows1); free(rows2);
    free(col_map);
    return r;
}

static QueryResult* exec_group_by(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->select_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");
    const char* bad = where_unknown_column(n->select_stmt.where, ts);
    if (bad) return unknown_column_error(bad, "WHERE");
    bad = where_unknown_column(n->select_stmt.having, ts);
    if (bad) return unknown_column_error(bad, "HAVING");
    for (int i = 0; i < n->select_stmt.num_cols; i++) {
        ASTExpr* c = n->select_stmt.columns[i];
        if (c->type == EXPR_STAR) continue;
        bad = where_unknown_column(c, ts);
        if (bad) return unknown_column_error(bad, "SELECT");
    }

    // Collect all rows
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    Tuple** rows = NULL;
    int num_rows = 0, cap = 0;
    HeapScan* scan = heap_scan_open(hf, ts);
    if (!scan) { free(hf); return result_error_msg("Failed to scan"); }
    RID rid; Tuple* t;
    while ((t = heap_scan_next(scan, &rid)) != NULL) {
        if (num_rows >= cap) { cap = cap ? cap*2 : 64; rows = realloc(rows, cap*sizeof(Tuple*)); }
        rows[num_rows++] = t;
    }
    heap_scan_close(scan); free(hf);


    // Apply WHERE and convert Tuples to Rows
    Tuple** filtered = NULL;
    int nf = 0;
    for (int i = 0; i < num_rows; i++) {
        if (!eval_where(n->select_stmt.where, rows[i], ts)) { tuple_free(rows[i]); continue; }
        filtered = realloc(filtered, (nf + 1) * sizeof(Tuple*));
        filtered[nf++] = rows[i];
    }

    // Gruppi: righe con gli stessi valori nelle colonne di GROUP BY.
    // Senza GROUP BY (solo aggregati) esiste un unico gruppo con tutte le righe, anche se vuoto.
    int gcols[64], ngc = 0;
    for (int k = 0; k < n->select_stmt.num_group_cols && k < 64; k++) {
        const char* gc_name = n->select_stmt.group_cols[k];
        const char* dot = strchr(gc_name, '.');
        if (dot) gc_name = dot + 1;
        int col_idx = catalog_get_col_index(ts, gc_name);
        if (col_idx < 0) {
            for (int q = 0; q < nf; q++) tuple_free(filtered[q]);
            free(filtered); free(rows);
            return unknown_column_error(gc_name, "GROUP BY");
        }
        gcols[ngc++] = col_idx;
    }

    int ng = 0;
    int* group_size = NULL;
    int** group_rows = NULL;
    if (ngc == 0) {
        ng = 1;
        group_size = calloc(1, sizeof(int));
        group_rows = calloc(1, sizeof(int*));
        group_rows[0] = nf ? malloc(nf * sizeof(int)) : NULL;
        for (int q = 0; q < nf; q++) group_rows[0][q] = q;
        group_size[0] = nf;
    }
    for (int i = 0; ngc > 0 && i < nf; i++) {
        int gi = -1;
        for (int g = 0; g < ng && gi < 0; g++) {
            Tuple* first = filtered[group_rows[g][0]];
            bool same = true;
            for (int k = 0; k < ngc && same; k++) {
                Value* va = &first->values[gcols[k]];
                Value* vb = &filtered[i]->values[gcols[k]];
                same = (va->is_null && vb->is_null) || value_equal(va, vb, ts->columns[gcols[k]].type);
            }
            if (same) gi = g;
        }
        if (gi < 0) {
            gi = ng++;
            group_size = realloc(group_size, ng * sizeof(int));
            group_rows = realloc(group_rows, ng * sizeof(int*));
            group_size[gi] = 0;
            group_rows[gi] = NULL;
        }
        group_size[gi]++;
        group_rows[gi] = realloc(group_rows[gi], group_size[gi] * sizeof(int));
        group_rows[gi][group_size[gi] - 1] = i;
    }

    // HAVING: valutato sul gruppo (gli aggregati sono calcolati sulle righe del gruppo)
    if (n->select_stmt.having) {
        int wg = 0;
        for (int g = 0; g < ng; g++) {
            agg_context_set(filtered, group_rows[g], group_size[g], ts);
            Tuple* first_t = group_size[g] > 0 ? filtered[group_rows[g][0]] : NULL;
            bool keep = eval_where(n->select_stmt.having, first_t, ts);
            agg_context_clear();
            if (keep) {
                group_rows[wg] = group_rows[g];
                group_size[wg] = group_size[g];
                wg++;
            } else {
                free(group_rows[g]);
            }
        }
        ng = wg;
    }

    // colonne di output: SELECT * = tutte le colonne della tabella
    bool star = n->select_stmt.num_cols == 0 ||
                (n->select_stmt.num_cols == 1 && n->select_stmt.columns[0]->type == EXPR_STAR);
    int out_cols = star ? ts->num_columns : n->select_stmt.num_cols;

    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = out_cols;
    r->col_names = calloc((size_t)out_cols + 1, sizeof(char*));
    r->col_types = calloc((size_t)out_cols + 1, sizeof(char*));
    for (int i = 0; i < out_cols; i++) {
        char name[160];
        ASTExpr* c = star ? NULL : n->select_stmt.columns[i];
        if (star)                 snprintf(name, sizeof(name), "%s", ts->columns[i].name);
        else if (c->alias[0])     snprintf(name, sizeof(name), "%s", c->alias);
        else if (c->type == EXPR_AGG) snprintf(name, sizeof(name), "%s(%s)", c->func, c->col_name);
        else                      snprintf(name, sizeof(name), "%s", c->col_name);
        r->col_names[i] = strdup(name);
        int ci = star ? i : (c->type == EXPR_COLUMN ? catalog_get_col_index(ts, c->col_name) : -1);
        const char* tn = ci >= 0 ? datatype_name(ts->columns[ci].type) : "";
        if (c && c->type == EXPR_AGG) {
            int ai = catalog_get_col_index(ts, c->col_name);
            if (!strcasecmp(c->func, "COUNT"))    tn = "INT";
            else if (!strcasecmp(c->func, "AVG")) tn = "FLOAT";
            else if (ai >= 0)                     tn = datatype_name(ts->columns[ai].type);
        }
        r->col_types[i] = strdup(tn);
    }

    r->rows = calloc(ng > 0 ? ng : 1, sizeof(Row));
    r->num_rows = ng;
    for (int g = 0; g < ng; g++) {
        Row* row = &r->rows[g];
        row->num_cells = out_cols;
        row->cells = calloc(out_cols, sizeof(Cell));
        Tuple* first = group_size[g] > 0 ? filtered[group_rows[g][0]] : NULL;
        agg_context_set(filtered, group_rows[g], group_size[g], ts);
        for (int j = 0; j < out_cols; j++) {
            Cell* cell = &row->cells[j];
            ASTExpr* c = star ? NULL : n->select_stmt.columns[j];
            if (c && c->type == EXPR_AGG) {
                EvalVal v;
                eval_operand(c, first, ts, &v);
                cell->is_null = v.null;
                cell->type = v.is_num ? (v.is_int ? CELL_TYPE_INT : CELL_TYPE_FLOAT) : CELL_TYPE_STRING;
                snprintf(cell->value, sizeof(cell->value), "%s", v.null ? "NULL" : v.str);
                continue;
            }
            int ci = star ? j : catalog_get_col_index(ts, c->col_name);
            if (ci < 0 || !first || first->values[ci].is_null) {
                cell->is_null = true;
                snprintf(cell->value, sizeof(cell->value), "NULL");
                continue;
            }
            value_to_string(&first->values[ci], ts->columns[ci].type, cell->value, sizeof(cell->value));
            cell->type = cell_type_of(ts->columns[ci].type);
        }
        agg_context_clear();
    }

    // ORDER BY su una colonna di output (nome, alias o aggregato), poi LIMIT
    if (n->select_stmt.order_col[0]) {
        const char* oc = n->select_stmt.order_col;
        const char* dot = strchr(oc, '.');
        int sort_col = -1;
        for (int i = 0; i < out_cols && sort_col < 0; i++)
            if (strcasecmp(r->col_names[i], oc) == 0 || (dot && strcasecmp(r->col_names[i], dot + 1) == 0)) sort_col = i;
        if (sort_col >= 0 && r->num_rows > 1) {
            g_sort_rows.rows = r->rows;
            g_sort_rows.sort_col = sort_col;
            g_sort_rows.order_asc = n->select_stmt.order_asc;
            qsort(r->rows, r->num_rows, sizeof(Row), row_compare_cells);
        }
    }
    if (n->select_stmt.limit > 0 && r->num_rows > n->select_stmt.limit) {
        for (int i = n->select_stmt.limit; i < r->num_rows; i++) free(r->rows[i].cells);
        r->num_rows = n->select_stmt.limit;
    }

    // Cleanup
    for (int i = 0; i < nf; i++) tuple_free(filtered[i]);
    for (int i = 0; i < ng; i++) free(group_rows[i]);
    free(group_rows); free(group_size); free(filtered);
    free(rows);
    return r;
}

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return result_error_msg("Invalid executor context");

    switch (stmt->type) {
        case NODE_CREATE_DB:    return exec_create_db(ctx, stmt);
        case NODE_DROP_DB:      return exec_drop_db(ctx, stmt);
        case NODE_CREATE_TABLE: return exec_create_table(ctx, stmt);
        case NODE_DROP_TABLE:   return exec_drop_table(ctx, stmt);
        case NODE_ALTER_TABLE:  return exec_alter_table(ctx, stmt);
        case NODE_INSERT:       return exec_insert(ctx, stmt);
        case NODE_SELECT: {
            bool has_agg = false;
            for (int i = 0; i < stmt->select_stmt.num_cols; i++)
                if (stmt->select_stmt.columns[i]->type == EXPR_AGG) has_agg = true;
            if (stmt->select_stmt.num_joins > 0) {
                if (has_agg || stmt->select_stmt.num_group_cols > 0)
                    return result_error_msg("Aggregates and GROUP BY are not supported together with JOIN yet");
                return exec_join(ctx, stmt);
            }
            if (stmt->select_stmt.num_group_cols > 0 || has_agg)
                return exec_group_by(ctx, stmt);
            return exec_select(ctx, stmt);
        }
        case NODE_UPDATE:       return exec_update(ctx, stmt);
        case NODE_DELETE:       return exec_delete(ctx, stmt);
        case NODE_USE:          return exec_use(ctx, stmt);
        case NODE_SHOW:         return exec_show(ctx, stmt);
        case NODE_DESCRIBE:     return exec_describe(ctx, stmt);
        case NODE_CREATE_INDEX: return exec_create_index(ctx, stmt);
        case NODE_DROP_INDEX:   return exec_drop_index(ctx, stmt);
        case NODE_CREATE_VIEW:  return exec_create_view(ctx, stmt);
        case NODE_DROP_VIEW:    return exec_drop_view(ctx, stmt);
        case NODE_EXPLAIN:      return exec_explain(ctx, stmt);
        case NODE_TRUNCATE:     return exec_truncate(ctx, stmt);
        case NODE_BEGIN:
        case NODE_COMMIT:
        case NODE_ROLLBACK:
            return result_ok_msg(0);
        case NODE_SAVEPOINT:    return result_ok_msg(0);
        case NODE_ROLLBACK_TO:  return result_ok_msg(0);
        case NODE_CREATE_TRIGGER: return result_ok_msg(0);
        case NODE_DROP_TRIGGER: return result_ok_msg(0);
        default:
            return result_error_msg("Unsupported statement type");
    }
}
