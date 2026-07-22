#include "executor.h"
#include <string.h>
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
    if (strcmp(type_name, "INT") == 0 || strcmp(type_name, "INTEGER") == 0) return SV_TYPE_INT;
    if (strcmp(type_name, "BIGINT") == 0) return SV_TYPE_BIGINT;
    if (strcmp(type_name, "FLOAT") == 0 || strcmp(type_name, "DOUBLE") == 0) return SV_TYPE_FLOAT;
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

static bool eval_where(ASTExpr* where, Tuple* t, TableSchema* schema) {
    if (!where) return true;
    if (where->type == EXPR_BINARY && where->op == OP_EQ) {
        const char* col_name = where->left->col_name;
        int ci = catalog_get_col_index(schema, col_name);
        if (ci < 0) return true;
        Value* v = &t->values[ci];
        if (where->right->type == EXPR_INT)
            return !v->is_null && v->int_val == where->right->int_val;
        if (where->right->type == EXPR_FLOAT)
            return !v->is_null && v->float_val == where->right->float_val;
        if (where->right->type == EXPR_STRING) {
            if (v->is_null || !v->str_val) return false;
            const char* rv = where->right->str_val;
            if (rv[0] == '\'' || rv[0] == '"') { rv++; }
            char buf[512]; snprintf(buf, sizeof(buf), "%s", rv);
            int blen = strlen(buf);
            if (blen > 0 && (buf[blen-1] == '\'' || buf[blen-1] == '"')) buf[blen-1] = '\0';
            return strcmp(v->str_val, buf) == 0;
        }
    }
    if (where->type == EXPR_BINARY) {
        const char* col_name = where->left->col_name;
        int ci = catalog_get_col_index(schema, col_name);
        if (ci < 0) return true;
        Value* v = &t->values[ci];
        if (v->is_null) return false;
        int64_t val;
        if (where->right->type == EXPR_INT) val = where->right->int_val;
        else return true;
        switch (where->op) {
            case OP_LT: return v->int_val < val;
            case OP_GT: return v->int_val > val;
            case OP_LE: return v->int_val <= val;
            case OP_GE: return v->int_val >= val;
            case OP_NEQ: return v->int_val != val;
            default: return true;
        }
    }
    return true;
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

static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");

    const char* vals[64];
    char val_bufs[64][64];
    memset(val_bufs, 0, sizeof(val_bufs));
    int nv = n->insert_stmt.num_values < 64 ? n->insert_stmt.num_values : 64;

    // Track which columns are specified if column list exists
    bool col_specified[64] = {0};

    for (int i = 0; i < nv; i++) {
        if (n->insert_stmt.values[i]) {
            if (n->insert_stmt.values[i]->type == EXPR_INT)
                snprintf(val_bufs[i], sizeof(val_bufs[i]), "%lld", (long long)n->insert_stmt.values[i]->int_val);
            else if (n->insert_stmt.values[i]->type == EXPR_FLOAT)
                snprintf(val_bufs[i], sizeof(val_bufs[i]), "%g", n->insert_stmt.values[i]->float_val);
            else
                snprintf(val_bufs[i], sizeof(val_bufs[i]), "%s", n->insert_stmt.values[i]->str_val);
            vals[i] = val_bufs[i];
            if (i < n->insert_stmt.num_cols && n->insert_stmt.columns[i]) {
                int ci = catalog_get_col_index(ts, n->insert_stmt.columns[i]);
                if (ci >= 0) col_specified[ci] = true;
            }
        } else {
            vals[i] = "NULL";
        }
    }

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

    // Auto-increment: generate values for auto_increment columns with no value
    static int ai_counter = 0; // simple counter for auto_increment
    for (int i = 0; i < ts->num_columns; i++) {
        if (ts->columns[i].auto_increment && !col_specified[i]) {
            ai_counter++;
            char ai_buf[32];
            snprintf(ai_buf, sizeof(ai_buf), "%d", ai_counter);
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

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    // Collect all rows
    Tuple** rows = NULL;
    int num_rows = 0, cap = 0;
    RID* rids = NULL;

    // Try index scan if WHERE is simple equality on indexed column
    bool use_index = false;
    BTreeScan* idx_scan = NULL;
    BTree* idx_bt = NULL;

    if (n->select_stmt.where && n->select_stmt.where->type == EXPR_BINARY &&
        n->select_stmt.where->op == OP_EQ && n->select_stmt.where->right->type == EXPR_INT) {
        const char* where_col = n->select_stmt.where->left->col_name;
        for (int i = 0; i < ts->num_indexes && !use_index; i++) {
            if (strcmp(ts->indexes[i].col_name, where_col) == 0 && ts->indexes[i].root_page_id > 0) {
                idx_bt = btree_open(ctx->bp, ts->indexes[i].root_page_id);
                if (idx_bt) {
                    BTreeValue btv;
                    if (btree_search(idx_bt, (uint64_t)n->select_stmt.where->right->int_val, &btv)) {
                        Tuple* t = heap_get(hf, (RID){btv.page_id, btv.slot_id}, ts);
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
    bool output_all = false;

    if (out_cols == 0 || (out_cols == 1 && n->select_stmt.columns[0]->type == EXPR_STAR)) {
        output_all = true;
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

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    int affected = 0;
    HeapScan* scan = heap_scan_open(hf, ts);
    if (!scan) { free(hf); return result_error_msg("Failed to scan"); }

    RID rid;
    Tuple* t;
    while ((t = heap_scan_next(scan, &rid)) != NULL) {
        if (eval_where(n->update_stmt.where, t, ts)) {
            // Capture before-image for WAL
            uint8_t* old_serial = NULL;
            uint32_t old_serial_len = 0;
            if (ctx->wal) {
                size_t slen;
                old_serial = tuple_serialize(t, ts, &slen);
                old_serial_len = (uint32_t)slen;
            }

            int ci = catalog_get_col_index(ts, n->update_stmt.set_col);
            if (ci >= 0) {
                Value* v = &t->values[ci];
                v->is_null = false;
                if (n->update_stmt.set_expr->type == EXPR_INT)
                    v->int_val = (int32_t)n->update_stmt.set_expr->int_val;
                else if (n->update_stmt.set_expr->type == EXPR_FLOAT)
                    v->float_val = n->update_stmt.set_expr->float_val;
                else if (n->update_stmt.set_expr->type == EXPR_STRING) {
                    if (v->str_val) free(v->str_val);
                    v->str_val = strdup(n->update_stmt.set_expr->str_val);
                }
                heap_update(hf, rid, t, ts);
                // Write WAL entry with before/after images
                if (ctx->wal && old_serial) {
                    size_t new_len;
                    uint8_t* new_serial = tuple_serialize(t, ts, &new_len);
                    if (new_serial) {
                        wal_write(ctx->wal, LOG_UPDATE, ctx->tx_id, rid.page_id,
                                  (SVRID){rid.page_id, rid.slot_id},
                                  new_serial, (uint32_t)new_len, old_serial, old_serial_len);
                        free(new_serial);
                    }
                }
                // Update indexes
                for (int idx_i = 0; idx_i < ts->num_indexes; idx_i++) {
                    if (ts->indexes[idx_i].root_page_id > 0) {
                        BTree* bt = btree_open(ctx->bp, ts->indexes[idx_i].root_page_id);
                        if (bt) {
                            uint64_t key = (uint64_t)v->int_val;
                            btree_insert(bt, key, (BTreeValue){rid.page_id, rid.slot_id});
                            free(bt);
                        }
                    }
                }
                affected++;
            }
            free(old_serial);
        }
        tuple_free(t);
    }
    heap_scan_close(scan);
    free(hf);
    return result_ok_msg(affected);
}

static QueryResult* exec_delete(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    int affected = 0;
    HeapScan* scan = heap_scan_open(hf, ts);
    if (!scan) { free(hf); return result_error_msg("Failed to scan"); }

    RID rid;
    Tuple* t;
    while ((t = heap_scan_next(scan, &rid)) != NULL) {
        if (eval_where(n->delete_stmt.where, t, ts)) {
            // Remove from indexes
            for (int i = 0; i < ts->num_indexes; i++) {
                if (ts->indexes[i].root_page_id > 0) {
                    int ci = catalog_get_col_index(ts, ts->indexes[i].col_name);
                    if (ci >= 0 && !t->values[ci].is_null) {
                        BTree* bt = btree_open(ctx->bp, ts->indexes[i].root_page_id);
                        if (bt) {
                            btree_delete(bt, (uint64_t)t->values[ci].int_val);
                            free(bt);
                        }
                    }
                }
            }
            // Write WAL before-image
            if (ctx->wal) {
                size_t serial_len;
                uint8_t* serial = tuple_serialize(t, ts, &serial_len);
                if (serial) {
                    wal_write(ctx->wal, LOG_DELETE, ctx->tx_id, rid.page_id,
                              (SVRID){rid.page_id, rid.slot_id}, NULL, 0, serial, (uint32_t)serial_len);
                    free(serial);
                }
            }
            heap_delete(hf, rid);
            ts->num_rows--;
            affected++;
        }
        tuple_free(t);
    }
    heap_scan_close(scan);
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
    snprintf(r->rows[1].cells[2].value, sizeof(r->rows[1].cells[2].value), "");
    snprintf(r->rows[1].cells[3].value, sizeof(r->rows[1].cells[3].value), "");
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

    int max_rows = n1 * n2;
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = out_cols;
    r->col_names = calloc(out_cols + 1, sizeof(char*));

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

    bool nulls_used = false;
    for (int i = 0; i < n1; i++) {
        bool matched = false;
        for (int j = 0; j < n2; j++) {
            if (!eval_on_cond(on_cond, rows1[i], rows2[j], ts1, ts2)) continue;
            matched = true;

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
            nulls_used = true;
        }
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

    /* GROUP BY not fully implemented - returning all rows as simple select */
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    int out_cols = n->select_stmt.num_cols > 0 ? n->select_stmt.num_cols : ts->num_columns;
    r->num_cols = out_cols;
    r->col_names = calloc(out_cols, sizeof(char*));
    for (int i = 0; i < out_cols && i < ts->num_columns; i++)
        r->col_names[i] = strdup(ts->columns[i].name);
    r->rows = calloc(num_rows, sizeof(Row));
    int out = 0;
    for (int i = 0; i < num_rows; i++) {
        if (!eval_where(n->select_stmt.where, rows[i], ts)) { tuple_free(rows[i]); continue; }
        Row* row = &r->rows[out++];
        row->num_cells = out_cols;
        row->cells = calloc(out_cols, sizeof(Cell));
        for (int j = 0; j < out_cols && j < rows[i]->num_values; j++) {
            Value* v = &rows[i]->values[j];
            if (!v->is_null) value_to_string(v, ts->columns[j].type, row->cells[j].value, sizeof(row->cells[j].value));
            else snprintf(row->cells[j].value, sizeof(row->cells[j].value), "NULL");
        }
        tuple_free(rows[i]);
    }
    r->num_rows = out;
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
        case NODE_SELECT:
            if (stmt->select_stmt.num_joins > 0)
                return exec_join(ctx, stmt);
            if (stmt->select_stmt.num_group_cols > 0)
                return exec_group_by(ctx, stmt);
            return exec_select(ctx, stmt);
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
