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
        if (where->right->type == EXPR_STRING) {
            if (v->is_null || !v->str_val) return false;
            // Strip quotes
            const char* rv = where->right->str_val;
            if (rv[0] == '\'' || rv[0] == '"') { rv++; }
            char buf[512]; snprintf(buf, sizeof(buf), "%s", rv);
            int blen = strlen(buf);
            if (blen > 0 && (buf[blen-1] == '\'' || buf[blen-1] == '"')) buf[blen-1] = '\0';
            return strcmp(v->str_val, buf) == 0;
        }
    }
    return true;
}

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

    return result_ok_msg(0);
}

static QueryResult* exec_drop_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    if (catalog_drop_table(ctx->catalog, db->name, n->drop_table.table_name) != SV_OK)
        return result_error_msg("Table not found");
    return result_ok_msg(0);
}

static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");

    // Build value array from parsed values
    const char* vals[64];
    int nv = n->insert_stmt.num_values < 64 ? n->insert_stmt.num_values : 64;
    for (int i = 0; i < nv; i++) {
        if (n->insert_stmt.values[i]) {
            static char buf[64];
            if (n->insert_stmt.values[i]->type == EXPR_INT)
                snprintf(buf, sizeof(buf), "%lld", (long long)n->insert_stmt.values[i]->int_val);
            else if (n->insert_stmt.values[i]->type == EXPR_FLOAT)
                snprintf(buf, sizeof(buf), "%g", n->insert_stmt.values[i]->float_val);
            else
                snprintf(buf, sizeof(buf), "%s", n->insert_stmt.values[i]->str_val);
            vals[i] = buf;
        } else {
            vals[i] = "NULL";
        }
    }
    for (int i = nv; i < ts->num_columns; i++) vals[i] = "NULL";

    Tuple* t = tuple_from_strings(vals, ts->num_columns, ts);
    if (!t) return result_error_msg("Failed to create tuple");

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) { tuple_free(t); return result_error_msg("Failed to open heap"); }

    RID rid;
    if (heap_insert(hf, t, ts, &rid) != SV_OK) {
        tuple_free(t); free(hf);
        return result_error_msg("Failed to insert row");
    }

    ts->num_rows++;
    ts->num_pages = hf->num_pages;
    free(hf);
    tuple_free(t);
    return result_ok_msg(1);
}

static QueryResult* exec_select(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->select_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");

    // Collect all rows first, then filter
    Tuple** rows = NULL;
    int num_rows = 0, cap = 0;
    RID* rids = NULL;

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

    // Build result
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = out_cols;
    r->num_rows = 0;
    r->col_names = calloc(out_cols, sizeof(char*));
    for (int i = 0; i < out_cols; i++) {
        int ci = col_map[i];
        if (ci >= 0 && ci < ts->num_columns)
            r->col_names[i] = strdup(ts->columns[ci].name);
        else
            r->col_names[i] = strdup("?");
    }

    // Allocate rows (max num_rows)
    r->rows = calloc(num_rows, sizeof(Row));
    int out_idx = 0;

    for (int i = 0; i < num_rows; i++) {
        Tuple* tp = rows[i];
        if (!eval_where(n->select_stmt.where, tp, ts)) {
            tuple_free(tp);
            continue;
        }

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
        tuple_free(tp);
    }

    r->num_rows = out_idx;
    r->num_cols = out_cols;

    free(col_map);
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
            int ci = catalog_get_col_index(ts, n->update_stmt.set_col);
            if (ci >= 0) {
                Value* v = &t->values[ci];
                v->is_null = false;
                if (n->update_stmt.set_expr->type == EXPR_INT)
                    v->int_val = (int32_t)n->update_stmt.set_expr->int_val;
                else if (n->update_stmt.set_expr->type == EXPR_STRING) {
                    free(v->str_val);
                    v->str_val = strdup(n->update_stmt.set_expr->str_val);
                }
                heap_update(hf, rid, t, ts);
                affected++;
            }
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
        // Show databases
        NameList* nl = NULL;
        // Build list of databases directly
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
        // Show tables
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

    return result_ok_msg(0);
}

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return result_error_msg("Invalid executor context");

    switch (stmt->type) {
        case NODE_CREATE_DB:  return exec_create_db(ctx, stmt);
        case NODE_DROP_DB:    return exec_drop_db(ctx, stmt);
        case NODE_CREATE_TABLE: return exec_create_table(ctx, stmt);
        case NODE_DROP_TABLE: return exec_drop_table(ctx, stmt);
        case NODE_INSERT:     return exec_insert(ctx, stmt);
        case NODE_SELECT:     return exec_select(ctx, stmt);
        case NODE_UPDATE:     return exec_update(ctx, stmt);
        case NODE_DELETE:     return exec_delete(ctx, stmt);
        case NODE_USE:        return exec_use(ctx, stmt);
        case NODE_SHOW:       return exec_show(ctx, stmt);
        case NODE_DESCRIBE:   return exec_describe(ctx, stmt);
        case NODE_CREATE_INDEX: return exec_create_index(ctx, stmt);
        case NODE_BEGIN:
        case NODE_COMMIT:
        case NODE_ROLLBACK:
            return result_ok_msg(0);
        default:
            return result_error_msg("Unsupported statement type");
    }
}
