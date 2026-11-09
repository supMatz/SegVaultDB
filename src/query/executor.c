#include "executor.h"
#include "platform_compat.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>

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

static void mi_invalidate_table(const char* db, const char* table);

static bool known_type(const char* t) {
    static const char* const T[] = { "INT", "INTEGER", "SMALLINT", "TINYINT", "BIGINT", "FLOAT", "DOUBLE", "REAL",
        "NUMERIC", "DECIMAL", "TIMESTAMP", "VARCHAR", "CHAR", "TEXT", "BOOL", "BOOLEAN", "DATE", "DATETIME", "BLOB" };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (!strcasecmp(t, T[i])) return true;
    return false;
}

static DataType parse_type(const char* type_name) {
    if (strcasecmp(type_name, "INT") == 0 || strcasecmp(type_name, "INTEGER") == 0 ||
        strcasecmp(type_name, "SMALLINT") == 0 || strcasecmp(type_name, "TINYINT") == 0) return SV_TYPE_INT;
    if (strcasecmp(type_name, "BIGINT") == 0) return SV_TYPE_BIGINT;
    if (strcasecmp(type_name, "FLOAT") == 0 || strcasecmp(type_name, "DOUBLE") == 0 ||
        strcasecmp(type_name, "REAL") == 0) return SV_TYPE_FLOAT;
    if (strcasecmp(type_name, "NUMERIC") == 0) return SV_TYPE_DECIMAL;
    if (strcasecmp(type_name, "TIMESTAMP") == 0) return SV_TYPE_DATETIME;
    if (strcasecmp(type_name, "VARCHAR") == 0) return SV_TYPE_VARCHAR;
    if (strcasecmp(type_name, "CHAR") == 0) return SV_TYPE_CHAR;
    if (strcasecmp(type_name, "TEXT") == 0) return SV_TYPE_TEXT;
    if (strcasecmp(type_name, "BOOL") == 0 || strcasecmp(type_name, "BOOLEAN") == 0) return SV_TYPE_BOOL;
    if (strcasecmp(type_name, "DATE") == 0) return SV_TYPE_DATE;
    if (strcasecmp(type_name, "DATETIME") == 0) return SV_TYPE_DATETIME;
    if (strcasecmp(type_name, "BLOB") == 0) return SV_TYPE_BLOB;
    if (strcasecmp(type_name, "DECIMAL") == 0) return SV_TYPE_DECIMAL;
    return SV_TYPE_VARCHAR;
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

static QueryResult* unknown_column_error(const char* col, const char* clause) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Unknown column '%.64s' in %s", col, clause);
    return result_error_msg(msg);
}

// ---- Executors ----
static QueryResult* exec_create_db(ExecCtx* ctx, ASTNode* n) {
    if (catalog_add_db(ctx->catalog, n->create_db.db_name) != SV_OK)
        return result_error_msg("Failed to create database (may already exist)");
    return result_ok_msg(0);
}

static QueryResult* exec_drop_db(ExecCtx* ctx, ASTNode* n) {
    executor_invalidate_indexes();
    if (catalog_drop_db(ctx->catalog, n->drop_db.db_name) != SV_OK)
        return result_error_msg("Database not found");
    return result_ok_msg(0);
}

static const char* add_constraint(ExecCtx* ctx, Database* db, TableSchema* ts, ConstraintDef* d, char* msg, size_t mn);
static const char* check_value(const Column* c, const char* v, char* msg, size_t n);
static const Constraint* referenced_by(Database* db, const TableSchema* ts, const TableSchema** child);

static QueryResult* exec_create_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    char msg[300];
    if (n->create_table.num_columns == 0) return result_error_msg("A table needs at least one column");
    if (n->create_table.num_columns > SV_MAX_COLUMNS) return result_error_msg("Too many columns (max 64)");
    int pks = 0, pk_col = -1;
    for (int i = 0; i < n->create_table.num_columns; i++) {
        ColumnDef* cd = &n->create_table.columns[i];
        if (!known_type(cd->type)) {
            snprintf(msg, sizeof(msg), "Unknown type '%.32s' for column '%.64s'", cd->type, cd->name);
            return result_error_msg(msg);
        }
        if (cd->primary_key) { pks++; pk_col = i; }
        for (int k = 0; k < i; k++)
            if (!strcasecmp(n->create_table.columns[k].name, cd->name)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is defined twice", cd->name);
                return result_error_msg(msg);
            }
    }
    // una sola chiave primaria: di colonna o di tabella (CONSTRAINT nome PRIMARY KEY sulla colonna
    // arriva in entrambe le forme, ed è la stessa)
    int table_pks = 0;
    bool same_as_inline = false;
    for (int i = 0; i < n->create_table.num_cons; i++) {
        ConstraintDef* d = &n->create_table.cons[i];
        if (d->kind != CONS_PRIMARY) continue;
        table_pks++;
        if (pk_col >= 0 && d->ncols == 1 && !strcasecmp(d->cols[0], n->create_table.columns[pk_col].name)) same_as_inline = true;
    }
    bool table_pk = table_pks > 0;
    if (pks > 1 || table_pks > 1 || (pks == 1 && table_pk && !same_as_inline))
        return result_error_msg("A table can have only one PRIMARY KEY");
    if (catalog_get_table(ctx->catalog, db->name, n->create_table.table_name) ||
        catalog_get_view(ctx->catalog, db->name, n->create_table.table_name)) {
        snprintf(msg, sizeof(msg), "Table or view '%.64s' already exists", n->create_table.table_name);
        return result_error_msg(msg);
    }

    TableSchema* ts = calloc(1, sizeof(TableSchema));
    if (!ts) return result_error_msg("Out of memory");
    snprintf(ts->name, sizeof(ts->name), "%s", n->create_table.table_name);

    for (int i = 0; i < n->create_table.num_columns && i < SV_MAX_COLUMNS; i++) {
        ColumnDef* cd = &n->create_table.columns[i];
        Column* col = &ts->columns[ts->num_columns++];
        memset(col, 0, sizeof(Column));
        snprintf(col->name, sizeof(col->name), "%s", cd->name);
        col->type = parse_type(cd->type);
        col->size = cd->size;
        col->nullable = cd->nullable;
        col->auto_increment = cd->auto_increment;
        if (cd->default_val[0])
            snprintf(col->default_val, sizeof(col->default_val), "%s", cd->default_val);
        if (cd->default_val[0] && strcasecmp(cd->default_val, "NULL") && check_value(col, cd->default_val, msg, sizeof(msg))) {
            free(ts);
            return result_error_msg(msg);
        }
    }

    // vincoli: prima la chiave primaria (le chiavi esterne verso la tabella stessa la usano)
    const char* fail = NULL;
    if (pks == 1 && !table_pk) {
        ConstraintDef d;
        memset(&d, 0, sizeof(d));
        d.kind = CONS_PRIMARY;
        d.ncols = 1;
        snprintf(d.cols[0], sizeof(d.cols[0]), "%s", n->create_table.columns[pk_col].name);
        snprintf(d.name, sizeof(d.name), "pk_%.58s", ts->name);
        fail = add_constraint(ctx, db, ts, &d, msg, sizeof(msg));
    }
    for (int pass = 0; pass < 2 && !fail; pass++)
        for (int i = 0; i < n->create_table.num_cons && !fail; i++) {
            ConstraintDef* d = &n->create_table.cons[i];
            if ((d->kind == CONS_PRIMARY) != (pass == 0)) continue;
            if (d->kind == CONS_PRIMARY && !d->name[0]) snprintf(d->name, sizeof(d->name), "pk_%.58s", ts->name);
            fail = add_constraint(ctx, db, ts, d, msg, sizeof(msg));
        }
    if (fail) {
        if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail);
        free(ts);
        return result_error_msg(msg);
    }

    int rc = catalog_add_table(ctx->catalog, db->name, ts);
    free(ts);
    if (rc != SV_OK) return result_error_msg("Failed to create table");

    TableSchema* saved = catalog_get_table(ctx->catalog, db->name, n->create_table.table_name);
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
    TableSchema* dts = catalog_get_table(ctx->catalog, db->name, n->drop_table.table_name);
    const TableSchema* child = NULL;
    const Constraint* ref = dts ? referenced_by(db, dts, &child) : NULL;
    if (ref) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Cannot drop table '%.64s': it is referenced by the foreign key '%.64s' of table '%.64s'",
                 dts->name, ref->name, child->name);
        return result_error_msg(msg);
    }
    mi_invalidate_table(db->name, n->drop_table.table_name);
    if (dts) heap_cache_forget(ctx->bp, dts->root_page_id);
    if (catalog_drop_table(ctx->catalog, db->name, n->drop_table.table_name) != SV_OK)
        return result_error_msg("Table not found");
    return result_ok_msg(0);
}

// ── ALTER TABLE ... COLUMN ──

static void value_copy(Value* dst, const Value* src, DataType t);

// testo SQL con gli identificatori 'from' sostituiti da 'to' (CHECK, corpi dei trigger)
static bool rename_ident_in_text(const char* text, const char* from, const char* to, char* out, size_t cap) {
    TokenList* tk = lexer_tokenize(text);
    if (!tk) return false;
    size_t len = 0;
    out[0] = '\0';
    bool ok = true;
    for (int i = 0; i < tk->count && ok; i++) {
        const Token* t = &tk->tokens[i];
        if (t->type == TOK_EOF) break;
        char piece[1100];
        if (t->type == TOK_STRING) {
            size_t k = 0;
            piece[k++] = '\'';
            for (const char* q = t->text; *q && k < sizeof(piece) - 3; q++) { if (*q == '\'' || *q == '\\') piece[k++] = *q; piece[k++] = *q; }
            piece[k++] = '\'';
            piece[k] = '\0';
        } else if (t->type == TOK_IDENTIFIER && !strcasecmp(t->text, from)) {
            snprintf(piece, sizeof(piece), "%s", to);
        } else {
            snprintf(piece, sizeof(piece), "%s", t->text);
        }
        bool tight = t->type == TOK_DOT || t->type == TOK_RPAREN || t->type == TOK_COMMA || t->type == TOK_SEMICOLON ||
                     (i > 0 && (tk->tokens[i - 1].type == TOK_DOT || tk->tokens[i - 1].type == TOK_LPAREN));
        size_t pl = strlen(piece);
        if (len + pl + 2 >= cap) { ok = false; break; }
        if (len && !tight) out[len++] = ' ';
        memcpy(out + len, piece, pl + 1);
        len += pl;
    }
    lexer_free(tk);
    return ok;
}

static bool ident_in_text(const char* text, const char* name) {
    TokenList* tk = lexer_tokenize(text);
    bool found = false;
    for (int i = 0; tk && i < tk->count && !found; i++)
        if (tk->tokens[i].type == TOK_IDENTIFIER && !strcasecmp(tk->tokens[i].text, name)) found = true;
    lexer_free(tk);
    return found;
}

// copia le righe in una nuova catena di pagine con lo schema ns (map[i] = colonna vecchia, -1 = nuova
// con il valore def). La tabella vecchia non viene toccata: se qualcosa va storto (anche un
// arresto anomalo) il catalogo punta ancora alle pagine di prima.
static const char* rewrite_rows(ExecCtx* ctx, TableSchema* ts, TableSchema* ns, const int* map, const char* def,
                                uint32_t* out_root, uint32_t* out_pages) {
    uint32_t root;
    if (bp_new_page(ctx->bp, &root) != SV_OK) return "Cannot allocate a page";
    bp_unpin(ctx->bp, root, false);
    HeapFile* nh = heap_open(ctx->bp, root, 1);
    HeapFile* oh = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    HeapScan* scan = oh ? heap_scan_open(oh, ts) : NULL;
    const char* fail = (!nh || !scan) ? "Cannot read the table" : NULL;
    Tuple* t;
    RID rid;
    while (!fail && (t = heap_scan_next(scan, &rid)) != NULL) {
        Tuple* nt = tuple_create(ns->num_columns);
        if (!nt) { tuple_free(t); fail = "Out of memory"; break; }
        for (int i = 0; i < ns->num_columns; i++) {
            if (map[i] >= 0) value_copy(&nt->values[i], &t->values[map[i]], ns->columns[i].type);
            else value_set_from_text(&nt->values[i], ns->columns[i].type, def);
        }
        RID nr;
        if (heap_insert(nh, nt, ns, &nr) != SV_OK) fail = "Cannot write the new rows (disk full?)";
        tuple_free(nt);
        tuple_free(t);
    }
    if (scan) heap_scan_close(scan);
    if (nh) { *out_root = nh->root_page_id; *out_pages = nh->num_pages; }
    free(nh);
    free(oh);
    if (fail) heap_cache_forget(ctx->bp, root);
    return fail;
}

static QueryResult* alter_column(ExecCtx* ctx, Database* db, TableSchema* ts, ASTNode* n) {
    char msg[300];
    if (ctx->tx_id && (n->alter_table.add_column || n->alter_table.drop_column[0]))
        return result_error_msg("ALTER TABLE ... ADD / DROP COLUMN is not allowed inside a transaction: COMMIT first");

    // RENAME COLUMN: solo il catalogo (e i testi che usano il nome)
    if (n->alter_table.rename_from[0]) {
        const char* from = n->alter_table.rename_from, *to = n->alter_table.rename_to;
        int ci = catalog_get_col_index(ts, from);
        if (ci < 0) { snprintf(msg, sizeof(msg), "Unknown column '%.64s'", from); return result_error_msg(msg); }
        if (strcasecmp(from, to) && catalog_get_col_index(ts, to) >= 0) { snprintf(msg, sizeof(msg), "Column '%.64s' already exists", to); return result_error_msg(msg); }
        char* buf = malloc(SV_MAX_SQL_LEN);
        if (!buf) return result_error_msg("Out of memory");
        for (int k = 0; k < ts->num_constraints; k++)
            if (ts->constraints[k].kind == CONS_CHECK && rename_ident_in_text(ts->constraints[k].check, from, to, buf, SV_CHECK_LEN))
                snprintf(ts->constraints[k].check, sizeof(ts->constraints[k].check), "%s", buf);
        for (int i = 0; i < db->num_tables; i++)
            for (int k = 0; k < db->tables[i].num_constraints; k++) {
                Constraint* c = &db->tables[i].constraints[k];
                if (c->kind != CONS_FOREIGN || strcasecmp(c->ref_table, ts->name)) continue;
                for (int q = 0; q < c->ncols; q++) if (!strcasecmp(c->ref_cols[q], from)) snprintf(c->ref_cols[q], sizeof(c->ref_cols[q]), "%s", to);
            }
        for (int i = 0; i < db->num_triggers; i++)
            if (!strcasecmp(db->triggers[i].table_name, ts->name) && rename_ident_in_text(db->triggers[i].body, from, to, buf, SV_MAX_SQL_LEN))
                snprintf(db->triggers[i].body, sizeof(db->triggers[i].body), "%s", buf);
        free(buf);
        for (int i = 0; i < ts->num_indexes; i++)
            if (!strcasecmp(ts->indexes[i].col_name, from)) snprintf(ts->indexes[i].col_name, sizeof(ts->indexes[i].col_name), "%s", to);
        snprintf(ts->columns[ci].name, sizeof(ts->columns[ci].name), "%s", to);
        mi_invalidate_table(db->name, ts->name);
        return result_ok_msg(0);
    }

    TableSchema* ns = malloc(sizeof(TableSchema));
    TableSchema* backup = malloc(sizeof(TableSchema));
    int* map = malloc(sizeof(int) * SV_MAX_COLUMNS);
    if (!ns || !backup || !map) { free(ns); free(backup); free(map); return result_error_msg("Out of memory"); }
    *ns = *ts;
    *backup = *ts;
    const char* def = NULL;
    const char* fail = NULL;
    ASTNode* add = n->alter_table.add_column;
    int dropped = -1;
    if (add) {
        ColumnDef* cd = &add->create_table.columns[0];
        if (add->create_table.num_columns != 1) fail = "Expected one column definition";
        else if (!known_type(cd->type)) { snprintf(msg, sizeof(msg), "Unknown type '%.32s'", cd->type); fail = msg; }
        else if (catalog_get_col_index(ts, cd->name) >= 0) { snprintf(msg, sizeof(msg), "Column '%.64s' already exists", cd->name); fail = msg; }
        else if (ts->num_columns >= SV_MAX_COLUMNS) fail = "Too many columns (max 64)";
        else if (cd->primary_key || cd->auto_increment) fail = "Add the column first, then ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY";
        if (!fail) {
            Column* col = &ns->columns[ns->num_columns];
            memset(col, 0, sizeof(*col));
            snprintf(col->name, sizeof(col->name), "%s", cd->name);
            col->type = parse_type(cd->type);
            col->size = cd->size;
            col->nullable = cd->nullable;
            snprintf(col->default_val, sizeof(col->default_val), "%s", cd->default_val);
            if (cd->default_val[0] && strcasecmp(cd->default_val, "NULL")) {
                def = col->default_val;
                if (check_value(col, def, msg, sizeof(msg))) fail = msg;
            }
            if (!fail && !col->nullable && !def && ts->num_rows > 0) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is NOT NULL: give it a DEFAULT for the existing rows", cd->name);
                fail = msg;
            }
            for (int i = 0; i < ts->num_columns; i++) map[i] = i;
            map[ns->num_columns] = -1;
            ns->num_columns++;
        }
    } else {
        const char* name = n->alter_table.drop_column;
        dropped = catalog_get_col_index(ts, name);
        if (dropped < 0) { snprintf(msg, sizeof(msg), "Unknown column '%.64s'", name); fail = msg; }
        else if (ts->num_columns == 1) fail = "A table needs at least one column: use DROP TABLE";
        for (int k = 0; !fail && k < ts->num_constraints; k++) {
            const Constraint* c = &ts->constraints[k];
            bool uses = c->kind == CONS_CHECK ? ident_in_text(c->check, name) : false;
            for (int q = 0; q < c->ncols; q++) if (c->cols[q] == dropped) uses = true;
            if (uses) { snprintf(msg, sizeof(msg), "Column '%.64s' is used by constraint '%.64s': drop the constraint first", name, c->name); fail = msg; }
        }
        for (int i = 0; !fail && i < db->num_tables; i++)
            for (int k = 0; !fail && k < db->tables[i].num_constraints; k++) {
                const Constraint* c = &db->tables[i].constraints[k];
                if (c->kind != CONS_FOREIGN || strcasecmp(c->ref_table, ts->name)) continue;
                for (int q = 0; q < c->ncols; q++)
                    if (!strcasecmp(c->ref_cols[q], name)) {
                        snprintf(msg, sizeof(msg), "Column '%.64s' is referenced by foreign key '%.64s' of table '%.64s'", name, c->name, db->tables[i].name);
                        fail = msg;
                    }
            }
        for (int i = 0; !fail && i < db->num_triggers; i++)
            if (!strcasecmp(db->triggers[i].table_name, ts->name) && ident_in_text(db->triggers[i].body, name)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is used by trigger '%.64s': drop the trigger first", name, db->triggers[i].name);
                fail = msg;
            }
        if (!fail) {
            for (int i = 0, k = 0; i < ts->num_columns; i++) {
                if (i == dropped) continue;
                ns->columns[k] = ts->columns[i];
                map[k++] = i;
            }
            ns->num_columns--;
            for (int k = 0; k < ns->num_constraints; k++)
                for (int q = 0; q < ns->constraints[k].ncols; q++)
                    if (ns->constraints[k].cols[q] > dropped) ns->constraints[k].cols[q]--;
            int w = 0;                                              // gli indici sulla colonna spariscono con lei
            for (int i = 0; i < ns->num_indexes; i++)
                if (strcasecmp(ns->indexes[i].col_name, name)) ns->indexes[w++] = ns->indexes[i];
            ns->num_indexes = w;
        }
    }
    uint32_t root = 0, pages = 0;
    if (!fail) fail = rewrite_rows(ctx, ts, ns, map, def, &root, &pages);
    if (!fail) {
        uint32_t old_root = ts->root_page_id;
        ns->root_page_id = root;
        ns->num_pages = pages;
        *ts = *ns;
        mi_invalidate_table(db->name, ts->name);
        // vincoli scritti con la nuova colonna (controllano le righe esistenti)
        for (int i = 0; add && !fail && i < add->create_table.num_cons; i++)
            fail = add_constraint(ctx, db, ts, &add->create_table.cons[i], msg, sizeof(msg));
        if (fail) {
            heap_cache_forget(ctx->bp, root);
            *ts = *backup;                                         // le pagine di prima sono intatte
            mi_invalidate_table(db->name, ts->name);
        } else {
            heap_cache_forget(ctx->bp, old_root);
            bp_flush_all(ctx->bp);                                 // le righe nuove su disco prima del catalogo
        }
    }
    free(ns);
    free(backup);
    free(map);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    return result_ok_msg(0);
}

static QueryResult* exec_alter_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->alter_table.table_name);
    if (!ts) return result_error_msg("Table not found");
    if (n->alter_table.add_column || n->alter_table.drop_column[0] || n->alter_table.rename_from[0])
        return alter_column(ctx, db, ts, n);
    // RENAME TO
    if (n->alter_table.new_name[0]) {
        const char* nn = n->alter_table.new_name;
        if (strcasecmp(nn, ts->name) &&
            (catalog_get_table(ctx->catalog, db->name, nn) || catalog_get_view(ctx->catalog, db->name, nn)))
            return result_error_msg("A table or view with the new name already exists");
        mi_invalidate_table(db->name, ts->name);
        for (int i = 0; i < db->num_tables; i++)                 // le chiavi esterne seguono il nuovo nome
            for (int k = 0; k < db->tables[i].num_constraints; k++)
                if (db->tables[i].constraints[k].kind == CONS_FOREIGN && !strcasecmp(db->tables[i].constraints[k].ref_table, ts->name))
                    snprintf(db->tables[i].constraints[k].ref_table, sizeof(db->tables[i].constraints[k].ref_table), "%s", nn);
        snprintf(ts->name, sizeof(ts->name), "%s", nn);
    }
    char msg[300];
    if (n->alter_table.has_add) {
        const char* fail = add_constraint(ctx, db, ts, &n->alter_table.add, msg, sizeof(msg));
        if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    }
    if (n->alter_table.drop_constraint[0]) {
        int k;
        for (k = 0; k < ts->num_constraints; k++) if (!strcasecmp(ts->constraints[k].name, n->alter_table.drop_constraint)) break;
        if (k == ts->num_constraints) {
            snprintf(msg, sizeof(msg), "Constraint '%.64s' not found in table '%.64s'", n->alter_table.drop_constraint, ts->name);
            return result_error_msg(msg);
        }
        Constraint* c = &ts->constraints[k];
        if (c->kind == CONS_PRIMARY || c->kind == CONS_UNIQUE) {
            // una chiave usata da una chiave esterna non si può togliere
            for (int i = 0; i < db->num_tables; i++)
                for (int f = 0; f < db->tables[i].num_constraints; f++) {
                    const Constraint* fk = &db->tables[i].constraints[f];
                    if (fk->kind != CONS_FOREIGN || strcasecmp(fk->ref_table, ts->name) || fk->ncols != c->ncols) continue;
                    bool same = true;
                    for (int a = 0; a < fk->ncols && same; a++) {
                        bool in = false;
                        for (int b = 0; b < c->ncols; b++) if (!strcasecmp(fk->ref_cols[a], ts->columns[c->cols[b]].name)) in = true;
                        same = in;
                    }
                    if (same && &db->tables[i] != ts) {
                        snprintf(msg, sizeof(msg), "Cannot drop '%.64s': the foreign key '%.64s' of table '%.64s' uses it",
                                 c->name, fk->name, db->tables[i].name);
                        return result_error_msg(msg);
                    }
                }
            for (int b = 0; b < c->ncols; b++) {
                if (c->kind == CONS_PRIMARY) ts->columns[c->cols[b]].primary_key = false;
                else if (c->ncols == 1) ts->columns[c->cols[b]].unique = false;
            }
        }
        memmove(c, c + 1, sizeof(Constraint) * (size_t)(ts->num_constraints - k - 1));
        ts->num_constraints--;
        mi_invalidate_table(db->name, ts->name);
    }
    return result_ok_msg(0);
}

static int utf8_len(const char* s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static bool valid_date(const char* s, const char** rest) {
    int y, m, d, k = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &y, &m, &d, &k) != 3 || k != 10) return false;
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return false;
    int md = DAYS[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    if (d > md) return false;
    if (rest) *rest = s + 10;
    return true;
}

// NULL se il testo è un valore valido per la colonna, altrimenti scrive il motivo in msg
static const char* check_value(const Column* c, const char* v, char* msg, size_t n) {
    const char* p = v;
    while (isspace((unsigned char)*p)) p++;
    switch (c->type) {
        case SV_TYPE_INT: case SV_TYPE_BIGINT: {
            char* end = NULL;
            errno = 0;
            long long x = strtoll(p, &end, 10);
            bool range = errno == ERANGE;
            if (end != p && (*end == '.' || *end == 'e' || *end == 'E')) {   // 3.0 va bene, 3.5 no
                double d = strtod(p, &end);
                if (d != (double)(long long)d) { snprintf(msg, n, "Column '%s' expects an integer, got '%.40s'", c->name, v); return msg; }
                x = (long long)d;
            }
            while (end && isspace((unsigned char)*end)) end++;
            if (end == p || !end || *end) { snprintf(msg, n, "Column '%s' expects an integer, got '%.40s'", c->name, v); return msg; }
            if (range || (c->type == SV_TYPE_INT && (x < INT_MIN || x > INT_MAX))) {
                snprintf(msg, n, "Value %.40s is out of range for column '%s' (%s)", v, c->name, datatype_name(c->type));
                return msg;
            }
            return NULL;
        }
        case SV_TYPE_FLOAT: case SV_TYPE_DECIMAL: {
            char* end = NULL;
            double d = strtod(p, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (end == p || !end || *end || d != d) { snprintf(msg, n, "Column '%s' expects a number, got '%.40s'", c->name, v); return msg; }
            return NULL;
        }
        case SV_TYPE_BOOL:
            if (!strcmp(p, "0") || !strcmp(p, "1") || !strcasecmp(p, "true") || !strcasecmp(p, "false")) return NULL;
            snprintf(msg, n, "Column '%s' expects TRUE, FALSE, 1 or 0, got '%.40s'", c->name, v);
            return msg;
        case SV_TYPE_VARCHAR: case SV_TYPE_CHAR:
            if (c->size > 0 && utf8_len(v) > c->size) {
                snprintf(msg, n, "Value too long for column '%s' (%d characters, maximum %d)", c->name, utf8_len(v), c->size);
                return msg;
            }
            return NULL;
        case SV_TYPE_DATE: {
            const char* rest = NULL;
            if (valid_date(p, &rest) && !*rest) return NULL;
            snprintf(msg, n, "Column '%s' expects a valid date YYYY-MM-DD, got '%.40s'", c->name, v);
            return msg;
        }
        case SV_TYPE_DATETIME: {
            const char* rest = NULL;
            int hh = 0, mm = 0, ss = 0, k = 0;
            if (valid_date(p, &rest) &&
                (!*rest || ((sscanf(rest, " %2d:%2d:%2d%n", &hh, &mm, &ss, &k) == 3 ||
                             (ss = 0, sscanf(rest, " %2d:%2d%n", &hh, &mm, &k) == 2)) && !rest[k] &&
                            hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60)))
                return NULL;
            snprintf(msg, n, "Column '%s' expects a date-time YYYY-MM-DD HH:MM:SS, got '%.40s'", c->name, v);
            return msg;
        }
        default:
            return NULL;
    }
}

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

// ═════════════════════════════════════════════════════════════════
//  Valori calcolati
// ═════════════════════════════════════════════════════════════════

#define EV_STR_MAX 511   // come Cell.value: un testo più lungo non entrerebbe nel risultato

typedef struct {
    bool        null;
    bool        is_num;
    bool        is_int;
    int64_t     inum;      // valore intero esatto (se is_int)
    double      num;
    const char* str;       // testo (stringhe e numeri formattati); punta a buf o a memoria della tupla
    char        buf[EV_STR_MAX + 1];
} EvalVal;

// valore tipizzato conservato (risultati, chiavi di ordinamento, funzioni finestra)
typedef struct SortKey { bool null, is_num, is_int; int64_t inum; double num; char* str; } SortKey;

static void ev_null(EvalVal* v) { v->null = true; v->is_num = v->is_int = false; v->str = ""; }

// i numeri vengono convertiti in testo solo quando serve (ev_s): confronti, aritmetica e
// filtri lavorano sui valori binari, senza snprintf per ogni riga
static void ev_int(EvalVal* v, int64_t x) {
    v->null = false; v->is_num = v->is_int = true; v->inum = x; v->num = (double)x; v->str = NULL;
}

static void ev_num(EvalVal* v, double x) {
    if (x != x || x - x != 0) { ev_null(v); return; }        // NaN / infinito: NULL
    v->null = false; v->is_num = true; v->is_int = false; v->num = x; v->str = NULL;
}

// testo del valore ("" per NULL); per i numeri viene formattato alla prima richiesta
static const char* ev_s(const EvalVal* cv) {
    EvalVal* v = (EvalVal*)cv;
    if (v->null) return "";
    if (v->str) return v->str;
    if (v->is_int) snprintf(v->buf, sizeof(v->buf), "%lld", (long long)v->inum);
    else           snprintf(v->buf, sizeof(v->buf), "%.15g", v->num);
    v->str = v->buf;
    return v->str;
}

// testo: copia nel buffer (false se più lungo di EV_STR_MAX)
static bool ev_text(EvalVal* v, const char* s, size_t n) {
    v->null = false; v->is_num = v->is_int = false;
    if (n > EV_STR_MAX) return false;
    memcpy(v->buf, s, n);
    v->buf[n] = '\0';
    v->str = v->buf;
    return true;
}

// copia sicura (il puntatore str può riferirsi al buffer interno)
static void ev_copy(EvalVal* dst, const EvalVal* src) {
    bool own = src->str == src->buf;
    *dst = *src;
    if (own) dst->str = dst->buf;
}

// testo numerico per intero? ("12", " -3 ") -> valore
static bool text_is_number(const char* s, double* out) {
    char* end = NULL;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return false;
    double d = strtod(s, &end);
    if (end == s) return false;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    *out = d;
    return true;
}

// valore numerico di qualunque cosa (testo: prefisso numerico come MySQL, altrimenti 0)
static double ev_as_num(const EvalVal* v) {
    if (v->is_num) return v->num;
    return strtod(ev_s(v), NULL);
}

// confronto: numeri fra loro, numero contro testo numerico, altrimenti testo
static int ev_cmp(const EvalVal* a, const EvalVal* b) {
    if (a->is_num && b->is_num) {
        if (a->is_int && b->is_int) return a->inum < b->inum ? -1 : a->inum > b->inum;
        return a->num < b->num ? -1 : a->num > b->num;
    }
    if (a->is_num || b->is_num) {
        double d;
        const EvalVal* txt = a->is_num ? b : a;
        if (text_is_number(ev_s(txt), &d)) {
            double x = a->is_num ? a->num : d, y = a->is_num ? d : b->num;
            return x < y ? -1 : x > y;
        }
    }
    int c = strcmp(ev_s(a), ev_s(b));
    return c < 0 ? -1 : c > 0;
}

// -1 = NULL (sconosciuto), 0 = falso, 1 = vero
static int ev_truth(const EvalVal* v) {
    if (v->null) return -1;
    return ev_as_num(v) != 0 ? 1 : 0;
}

// valore di una colonna della tupla
static void ev_from_value(EvalVal* out, const Value* v, DataType type) {
    if (v->is_null) { ev_null(out); return; }
    switch (type) {
        case SV_TYPE_INT:    ev_int(out, v->int_val); return;
        case SV_TYPE_BIGINT: ev_int(out, v->bigint_val); return;
        case SV_TYPE_BOOL:   ev_int(out, v->bool_val ? 1 : 0); return;
        case SV_TYPE_FLOAT:  ev_num(out, v->float_val); return;
        case SV_TYPE_DECIMAL: {   // salvato come testo, si comporta da numero
            out->null = false;
            out->str = v->str_val ? v->str_val : "";
            double d;
            out->is_int = false;
            out->is_num = text_is_number(out->str, &d);
            out->num = out->is_num ? d : 0;
            return;
        }
        default:
            out->null = false; out->is_num = out->is_int = false;
            out->str = v->str_val ? v->str_val : "";
            return;
    }
}

// ═════════════════════════════════════════════════════════════════
//  Mappa testo -> intero (indici, gruppi, DISTINCT)
// ═════════════════════════════════════════════════════════════════

typedef struct { char* key; int val; } SMSlot;
typedef struct { SMSlot* s; size_t cap, n; } StrMap;

static uint64_t hash_bytes(const char* k) {
    uint64_t h = 1469598103934665603ULL;          // FNV-1a
    for (; *k; k++) { h ^= (unsigned char)*k; h *= 1099511628211ULL; }
    return h;
}

static void sm_free(StrMap* m) {
    for (size_t i = 0; i < m->cap; i++) free(m->s[i].key);
    free(m->s);
    m->s = NULL; m->cap = m->n = 0;
}

static bool sm_grow(StrMap* m) {
    size_t nc = m->cap ? m->cap * 2 : 64;
    SMSlot* ns = calloc(nc, sizeof(SMSlot));
    if (!ns) return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->s[i].key) continue;
        size_t j = hash_bytes(m->s[i].key) & (nc - 1);
        while (ns[j].key) j = (j + 1) & (nc - 1);
        ns[j] = m->s[i];
    }
    free(m->s);
    m->s = ns;
    m->cap = nc;
    return true;
}

// slot della chiave; con create la inserisce (val = -1) se manca. NULL se manca memoria.
static SMSlot* sm_slot(StrMap* m, const char* key, bool create) {
    if (create && (m->n + 1) * 10 >= m->cap * 7 && !sm_grow(m)) return NULL;
    if (!m->cap) return NULL;
    size_t j = hash_bytes(key) & (m->cap - 1);
    while (m->s[j].key) {
        if (strcmp(m->s[j].key, key) == 0) return &m->s[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!create) return NULL;
    m->s[j].key = strdup(key);
    if (!m->s[j].key) return NULL;
    m->s[j].val = -1;
    m->n++;
    return &m->s[j];
}

// ═════════════════════════════════════════════════════════════════
//  Indici in memoria
//  Chiave primaria, AUTO_INCREMENT e colonne con CREATE INDEX hanno un indice
//  valore -> righe, costruito dalle righe della tabella al primo uso e mantenuto da
//  INSERT/UPDATE/DELETE. Non vive su disco: non può mai essere disallineato dai dati
//  (dopo un arresto anomalo o un ROLLBACK viene semplicemente ricostruito).
// ═════════════════════════════════════════════════════════════════

typedef struct { RID* r; int n, cap; } RidList;

typedef struct MemIndex {
    char      db[SV_MAX_NAME_LEN], table[SV_MAX_NAME_LEN];
    char      col[SV_MAX_NAME_LEN * 2];   // nome della colonna, o "a,b" per una chiave composta
    int       cols[SV_MAX_KEY_COLS];      // posizioni delle colonne della chiave
    int       ncols;
    StrMap    map;          // chiave -> posizione in lists
    RidList*  lists;
    int       nlists, caplists;
    bool      has_max;      // massimo intero (AUTO_INCREMENT)
    int64_t   max_int;
    struct MemIndex* next;
} MemIndex;

static MemIndex* g_mem_indexes;

static void mi_free(MemIndex* ix) {
    sm_free(&ix->map);
    for (int i = 0; i < ix->nlists; i++) free(ix->lists[i].r);
    free(ix->lists);
    free(ix);
}

void executor_invalidate_indexes(void) {
    heap_cache_clear();
    while (g_mem_indexes) {
        MemIndex* nx = g_mem_indexes->next;
        mi_free(g_mem_indexes);
        g_mem_indexes = nx;
    }
}

static void mi_invalidate_table(const char* db, const char* table) {
    MemIndex** pp = &g_mem_indexes;
    while (*pp) {
        MemIndex* ix = *pp;
        if (!strcasecmp(ix->db, db) && !strcasecmp(ix->table, table)) { *pp = ix->next; mi_free(ix); }
        else pp = &ix->next;
    }
}

// chiave canonica di un valore (NULL non viene indicizzato)
static void mi_key(const Value* v, DataType t, char* buf, size_t n) {
    value_to_string((Value*)v, t, buf, n);
}

// chiave della tupla sulle colonne indicate; false se una colonna è NULL (non indicizzata:
// in SQL una chiave con NULL non è mai uguale a un'altra)
static bool tuple_key(const TableSchema* ts, const Tuple* t, const int* cols, int ncols, char* buf, size_t n) {
    size_t len = 0;
    buf[0] = '\0';
    for (int k = 0; k < ncols; k++) {
        int ci = cols[k];
        if (ci < 0 || ci >= t->num_values || t->values[ci].is_null) return false;
        if (k) { if (len + 2 >= n) return false; buf[len++] = '\x1f'; buf[len] = '\0'; }
        mi_key(&t->values[ci], ts->columns[ci].type, buf + len, n - len);
        len += strlen(buf + len);
    }
    return true;
}

static bool mi_add_key(MemIndex* ix, const char* key, RID rid) {
    SMSlot* s = sm_slot(&ix->map, key, true);
    if (!s) return false;
    if (s->val < 0) {
        if (ix->nlists >= ix->caplists) {
            int nc = ix->caplists ? ix->caplists * 2 : 64;
            RidList* nl = realloc(ix->lists, sizeof(RidList) * (size_t)nc);
            if (!nl) return false;
            ix->lists = nl;
            ix->caplists = nc;
        }
        ix->lists[ix->nlists] = (RidList){ NULL, 0, 0 };
        s->val = ix->nlists++;
    }
    RidList* l = &ix->lists[s->val];
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 1;
        RID* nr = realloc(l->r, sizeof(RID) * (size_t)nc);
        if (!nr) return false;
        l->r = nr;
        l->cap = nc;
    }
    l->r[l->n++] = rid;
    return true;
}

static bool mi_add_tuple(MemIndex* ix, const TableSchema* ts, const Tuple* t, RID rid) {
    char key[EV_STR_MAX + 1];
    if (!tuple_key(ts, t, ix->cols, ix->ncols, key, sizeof(key))) return true;
    if (!mi_add_key(ix, key, rid)) return false;
    if (ix->ncols == 1) {
        int ci = ix->cols[0];
        DataType ty = ts->columns[ci].type;
        if (ty == SV_TYPE_INT || ty == SV_TYPE_BIGINT) {
            int64_t x = ty == SV_TYPE_INT ? t->values[ci].int_val : t->values[ci].bigint_val;
            if (!ix->has_max || x > ix->max_int) ix->max_int = x;
            ix->has_max = true;
        }
    }
    return true;
}

static void mi_remove_tuple(MemIndex* ix, const TableSchema* ts, const Tuple* t, RID rid) {
    char key[EV_STR_MAX + 1];
    if (!tuple_key(ts, t, ix->cols, ix->ncols, key, sizeof(key))) return;
    SMSlot* s = sm_slot(&ix->map, key, false);
    if (!s || s->val < 0) return;
    RidList* l = &ix->lists[s->val];
    for (int i = 0; i < l->n; i++)
        if (l->r[i].page_id == rid.page_id && l->r[i].slot_id == rid.slot_id) { l->r[i] = l->r[--l->n]; return; }
}

static const RidList* mi_lookup(MemIndex* ix, const char* key) {
    SMSlot* s = sm_slot(&ix->map, key, false);
    return (s && s->val >= 0) ? &ix->lists[s->val] : NULL;
}

// la colonna ha un indice (chiave primaria, AUTO_INCREMENT, UNIQUE o CREATE INDEX)?
static bool col_is_indexed(const TableSchema* ts, int ci) {
    if (ts->columns[ci].primary_key || ts->columns[ci].auto_increment || ts->columns[ci].unique) return true;
    for (int i = 0; i < ts->num_indexes; i++)
        if (!strcasecmp(ts->indexes[i].col_name, ts->columns[ci].name)) return true;
    for (int i = 0; i < ts->num_constraints; i++) {
        const Constraint* c = &ts->constraints[i];
        // (le chiavi esterne hanno un indice solo per i controlli di integrità, come in PostgreSQL:
        // per cercare le righe di una colonna di chiave esterna serve CREATE INDEX)
        if (c->kind == CONS_UNIQUE && c->ncols == 1 && c->cols[0] == ci) return true;
    }
    return false;
}

// indice sulle colonne indicate (costruito leggendo la tabella se manca). NULL se manca
// memoria o la lettura è stata annullata.
static MemIndex* mi_get_cols(ExecCtx* ctx, const char* db, TableSchema* ts, const int* cols, int ncols) {
    if (ncols < 1 || ncols > SV_MAX_KEY_COLS) return NULL;
    char name[SV_MAX_NAME_LEN * 2];
    size_t len = 0;
    name[0] = '\0';
    for (int k = 0; k < ncols; k++) {
        if (cols[k] < 0 || cols[k] >= ts->num_columns) return NULL;
        int w = snprintf(name + len, sizeof(name) - len, "%s%s", k ? "," : "", ts->columns[cols[k]].name);
        if (w < 0 || (size_t)w >= sizeof(name) - len) return NULL;
        len += (size_t)w;
    }
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next)
        if (!strcasecmp(ix->db, db) && !strcasecmp(ix->table, ts->name) && !strcasecmp(ix->col, name)) return ix;
    MemIndex* ix = calloc(1, sizeof(MemIndex));
    if (!ix) return NULL;
    snprintf(ix->db, sizeof(ix->db), "%s", db);
    snprintf(ix->table, sizeof(ix->table), "%s", ts->name);
    snprintf(ix->col, sizeof(ix->col), "%s", name);
    memcpy(ix->cols, cols, sizeof(int) * (size_t)ncols);
    ix->ncols = ncols;
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    HeapScan* scan = hf ? heap_scan_open(hf, ts) : NULL;
    bool ok = scan != NULL;
    Tuple* t;
    RID rid;
    while (ok && (t = heap_scan_next(scan, &rid)) != NULL) {
        ok = mi_add_tuple(ix, ts, t, rid);
        tuple_free(t);
    }
    if (scan) heap_scan_close(scan);
    free(hf);
    if (!ok || atomic_load(&g_sv_scan_cancel)) { mi_free(ix); return NULL; }   // incompleto: scartato
    ix->next = g_mem_indexes;
    g_mem_indexes = ix;
    return ix;
}

// indice di una colonna (solo se la colonna è indicizzata)
static MemIndex* mi_get(ExecCtx* ctx, const char* db, TableSchema* ts, int ci) {
    if (ci < 0 || ci >= ts->num_columns || !col_is_indexed(ts, ci)) return NULL;
    return mi_get_cols(ctx, db, ts, &ci, 1);
}

// aggiornamento degli indici già costruiti della tabella
static void mi_on_insert(const char* db, TableSchema* ts, const Tuple* t, RID rid) {
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name)) continue;
        if (!mi_add_tuple(ix, ts, t, rid)) {
            mi_invalidate_table(db, ts->name);   // memoria esaurita: meglio ricostruire che usarlo incompleto
            return;
        }
    }
}

static void mi_on_delete(const char* db, TableSchema* ts, const Tuple* t, RID rid) {
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name)) continue;
        mi_remove_tuple(ix, ts, t, rid);
    }
}

// ═════════════════════════════════════════════════════════════════
//  Sorgenti di righe (FROM / JOIN): tabelle o viste
// ═════════════════════════════════════════════════════════════════

#define MAX_SOURCES 9            // FROM + 8 JOIN

// arena: righe lette da una tabella allocate a blocchi e liberate tutte insieme
typedef struct ArenaBlk { struct ArenaBlk* next; size_t used, cap; } ArenaBlk;
typedef struct { ArenaBlk* head; } Arena;

static void* arena_alloc(Arena* a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 - sizeof(ArenaBlk) ? n : 65536 - sizeof(ArenaBlk);
        ArenaBlk* b = malloc(sizeof(ArenaBlk) + cap);
        if (!b) return NULL;
        b->next = a->head; b->used = 0; b->cap = cap;
        a->head = b;
    }
    void* p = (char*)(a->head + 1) + a->head->used;
    a->head->used += n;
    return p;
}

static void arena_free(Arena* a) {
    while (a->head) { ArenaBlk* n = a->head->next; free(a->head); a->head = n; }
}

typedef struct {
    char         name[SV_MAX_NAME_LEN];    // nome con cui la si qualifica (alias o nome)
    char         table[SV_MAX_NAME_LEN];   // nome reale
    TableSchema* ts;                       // schema (vista: schema costruito dal risultato)
    TableSchema* own_ts;                   // da liberare (viste)
    Tuple**      rows;
    RID*         rids;
    int          n;
    bool         is_view;
    bool         loaded;
    bool         arena_rows;               // righe nell'arena (non si liberano una per una)
    Arena        arena;
} Source;

static void source_free(Source* s) {
    for (int i = 0; !s->arena_rows && i < s->n; i++) tuple_free(s->rows[i]);
    arena_free(&s->arena);
    free(s->rows);
    free(s->rids);
    free(s->own_ts);
    memset(s, 0, sizeof(*s));
}

static bool source_push(Source* s, Tuple* t, RID rid, int* cap) {
    if (s->n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        Tuple** nr = realloc(s->rows, sizeof(Tuple*) * (size_t)nc);
        if (!nr) return false;
        s->rows = nr;
        RID* nd = realloc(s->rids, sizeof(RID) * (size_t)nc);
        if (!nd) return false;
        s->rids = nd;
        *cap = nc;
    }
    s->rows[s->n] = t;
    s->rids[s->n] = rid;
    s->n++;
    return true;
}

// ═════════════════════════════════════════════════════════════════
//  Contesto di valutazione
// ═════════════════════════════════════════════════════════════════

typedef struct SelectPlan SelectPlan;

typedef struct EvalCtx {
    Source*   src;
    int       nsrc;
    Tuple**   cur;              // riga corrente di ogni sorgente (NULL = riga mancante di un outer join)
    Tuple***  grp;              // righe del gruppo corrente (aggregati); NULL fuori dai gruppi
    int       ngrp;
    ASTExpr** sel;              // espressioni della SELECT (alias in HAVING / ORDER BY)
    int       nsel;
    char      err[256];         // primo errore di valutazione
    ExecCtx*  x;                // per eseguire le subquery
    struct EvalCtx* outer;      // query esterna (colonne correlate)
    struct SortKey** win;       // valori delle funzioni finestra (per funzione, per unità)
    int       win_row;          // unità corrente (combinazione o gruppo)
} EvalCtx;

static void ctx_error(EvalCtx* c, const char* fmt, const char* a) {
    if (c->err[0]) return;
    snprintf(c->err, sizeof(c->err), fmt, a);
}

// ── binding: risolve le colonne e controlla funzioni e aggregati prima di eseguire ──

typedef enum { BIND_ROW, BIND_AGG_OK } BindMode;   // BIND_AGG_OK: SELECT, HAVING, ORDER BY

// nomi definiti con WITH visibili in una query (catena verso le query che la contengono)
typedef struct CteScope { ASTCte* ctes; int n; const struct CteScope* parent; } CteScope;

typedef struct BindCtx {
    Source*   src;
    int       nsrc;             // sorgenti visibili (le condizioni ON vedono solo quelle già unite)
    ASTExpr** sel;              // alias utilizzabili (HAVING / ORDER BY / GROUP BY)
    int       nsel;
    BindMode  mode;
    bool      in_agg;
    bool      saw_agg;
    const char* clause;
    char      err[256];
    ExecCtx*  x;                // NULL: subquery non ammesse
    struct BindCtx* outer;      // query esterna (per le subquery correlate)
    SelectPlan* plan;           // piano della query che si sta risolvendo
    int       depth;            // annidamento di viste e subquery
    const CteScope* ctes;
} BindCtx;

static SelectPlan* plan_prepare(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                char* err, size_t errn);
static void plan_free_void(void* p);
static int plan_width(const SelectPlan* p);
static int plan_using_left(const SelectPlan* p, int s, int c);
static int plan_join_type(const SelectPlan* p, int s);
static int plan_joined_copy(const SelectPlan* p, int s, int c, int* rk, int* rc);
static void plan_mark_correlated(SelectPlan* p);
static bool plan_add_window(SelectPlan* p, ASTExpr* w);

static void bind_error(BindCtx* b, const char* msg) {
    if (!b->err[0]) snprintf(b->err, sizeof(b->err), "%s", msg);
}

typedef struct { const char* name; int min_args, max_args; } FuncDef;
static const FuncDef FUNCS[] = {
    { "UPPER", 1, 1 }, { "UCASE", 1, 1 }, { "LOWER", 1, 1 }, { "LCASE", 1, 1 },
    { "LENGTH", 1, 1 }, { "CHAR_LENGTH", 1, 1 }, { "CHARACTER_LENGTH", 1, 1 },
    { "TRIM", 1, 1 }, { "LTRIM", 1, 1 }, { "RTRIM", 1, 1 },
    { "CONCAT", 1, 32 }, { "CONCAT_WS", 2, 32 }, { "SUBSTRING", 2, 3 }, { "SUBSTR", 2, 3 },
    { "LEFT", 2, 2 }, { "RIGHT", 2, 2 }, { "REPLACE", 3, 3 }, { "REVERSE", 1, 1 },
    { "ABS", 1, 1 }, { "ROUND", 1, 2 }, { "FLOOR", 1, 1 }, { "CEIL", 1, 1 }, { "CEILING", 1, 1 },
    { "MOD", 2, 2 }, { "POWER", 2, 2 }, { "POW", 2, 2 }, { "SQRT", 1, 1 }, { "SIGN", 1, 1 },
    { "COALESCE", 1, 32 }, { "IFNULL", 2, 2 }, { "NULLIF", 2, 2 }, { "IF", 3, 3 },
    { "NOW", 0, 0 }, { "CURRENT_TIMESTAMP", 0, 0 }, { "CURDATE", 0, 0 }, { "CURRENT_DATE", 0, 0 },
    { "YEAR", 1, 1 }, { "MONTH", 1, 1 }, { "DAY", 1, 1 },
};

static const FuncDef* find_func(const char* name) {
    for (size_t i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); i++)
        if (!strcmp(FUNCS[i].name, name)) return &FUNCS[i];
    return NULL;
}

// cerca la colonna nelle sorgenti visibili; 0 = non trovata, -1 = ambigua
static int find_column(BindCtx* b, const char* qual, const char* col, int* out_src, int* out_col) {
    int found = 0;
    for (int s = 0; s < b->nsrc; s++) {
        Source* src = &b->src[s];
        // con un alias la tabella si chiama solo con l'alias (come in SQL standard): così in
        // una subquery correlata "esame.corso" indica la query esterna se dentro c'è "esame e2"
        if (qual[0] && strcasecmp(qual, src->name)) continue;
        for (int c = 0; c < src->ts->num_columns; c++) {
            if (strcasecmp(src->ts->columns[c].name, col)) continue;
            if (found && !qual[0] && b->plan) {
                // colonna di NATURAL JOIN / USING: le due copie sono la stessa colonna
                int l = plan_using_left(b->plan, s, c);
                if (l == *out_src * 1024 + *out_col + 1) {
                    if (plan_join_type(b->plan, s) == JOIN_RIGHT) { *out_src = s; *out_col = c; }   // il lato che non manca
                    continue;
                }
            }
            if (found) return -1;
            found = 1;
            *out_src = s;
            *out_col = c;
        }
    }
    return found;
}

static bool qualifier_exists(BindCtx* b, const char* qual) {
    for (; b; b = b->outer)
        for (int s = 0; s < b->nsrc; s++)
            if (!strcasecmp(qual, b->src[s].name)) return true;
    return false;
}

static void bind_expr(ASTExpr* e, BindCtx* b) {
    if (!e || b->err[0]) return;
    char msg[256];
    switch (e->type) {
        case EXPR_COLUMN: {
            e->bound = 0;
            e->outer = 0;
            int s = 0, c = 0;
            int r = find_column(b, e->table_name, e->col_name, &s, &c);
            int rk, rc;
            if (r == 1 && !e->table_name[0] && b->plan && plan_joined_copy(b->plan, s, c, &rk, &rc) == JOIN_FULL) {
                // colonna di USING / NATURAL in un FULL JOIN: vale COALESCE(sinistra, destra)
                ASTExpr* l = calloc(1, sizeof(ASTExpr));
                ASTExpr* rr = calloc(1, sizeof(ASTExpr));
                ASTExpr** args = calloc(2, sizeof(ASTExpr*));
                if (!l || !rr || !args) { free(l); free(rr); free(args); bind_error(b, "Out of memory"); return; }
                l->type = rr->type = EXPR_COLUMN;
                snprintf(l->col_name, sizeof(l->col_name), "%s", e->col_name);
                snprintf(rr->col_name, sizeof(rr->col_name), "%s", e->col_name);
                snprintf(l->table_name, sizeof(l->table_name), "%s", b->src[s].name);
                snprintf(rr->table_name, sizeof(rr->table_name), "%s", b->src[rk].name);
                l->bound = s * 1024 + c + 1;
                rr->bound = rk * 1024 + rc + 1;
                args[0] = l; args[1] = rr;
                e->type = EXPR_FUNC;
                snprintf(e->func, sizeof(e->func), "COALESCE");
                e->args = args; e->nargs = 2; e->depth = 2;
                return;
            }
            if (r == 1) { e->bound = s * 1024 + c + 1; return; }
            if (r == 0) {
                // colonna di una query esterna: la subquery è correlata
                int level = 1;
                for (BindCtx* o = b->outer; o && r == 0; o = o->outer, level++) {
                    r = find_column(o, e->table_name, e->col_name, &s, &c);
                    if (r == 1) {
                        e->bound = s * 1024 + c + 1;
                        e->outer = level;
                        BindCtx* m = b;
                        for (int k = 0; k < level && m; k++, m = m->outer) if (m->plan) plan_mark_correlated(m->plan);
                        return;
                    }
                }
            }
            if (r < 0) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is ambiguous: qualify it with the table name", e->col_name);
                bind_error(b, msg);
                return;
            }
            // alias della SELECT (HAVING, ORDER BY, GROUP BY)
            if (!e->table_name[0])
                for (int k = 0; k < b->nsel; k++)
                    if (b->sel[k]->alias[0] && !strcasecmp(b->sel[k]->alias, e->col_name)) { e->bound = -(k + 1); return; }
            if (e->table_name[0] && !qualifier_exists(b, e->table_name))
                snprintf(msg, sizeof(msg), "Unknown table '%.64s' in %s", e->table_name, b->clause);
            else
                snprintf(msg, sizeof(msg), "Unknown column '%.64s' in %s", e->col_name, b->clause);
            bind_error(b, msg);
            return;
        }
        case EXPR_AGG:
            if (b->mode != BIND_AGG_OK) {
                snprintf(msg, sizeof(msg), "Aggregate functions are not allowed in %s", b->clause);
                bind_error(b, msg);
                return;
            }
            if (b->in_agg) { bind_error(b, "Aggregate functions cannot be nested"); return; }
            b->saw_agg = true;
            b->in_agg = true;
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            b->in_agg = false;
            return;
        case EXPR_FUNC: {
            const FuncDef* f = find_func(e->func);
            if (!f) {
                snprintf(msg, sizeof(msg), "Unknown function '%.32s'", e->func);
                bind_error(b, msg);
                return;
            }
            if (e->nargs < f->min_args || e->nargs > f->max_args) {
                snprintf(msg, sizeof(msg), "Wrong number of arguments for %.32s", e->func);
                bind_error(b, msg);
                return;
            }
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            return;
        }
        case EXPR_STAR:
            bind_error(b, "* is only allowed in the SELECT list and in COUNT(*)");
            return;
        case EXPR_SUBQUERY: {
            bind_expr(e->left, b);
            if (b->err[0]) return;
            if (!b->x) { bind_error(b, "Subqueries are not allowed here"); return; }
            SubQuery* q = e->subq;
            if (!q->plan) {
                char perr[300] = "";
                BindCtx* save_outer = b;
                SelectPlan* sp = plan_prepare(b->x, q->node, b->depth + 1, save_outer, b->ctes, perr, sizeof(perr));
                if (!sp) { bind_error(b, perr); return; }
                q->plan = sp;
                q->plan_free = plan_free_void;
            }
            if (e->subkind != SUBQ_EXISTS && plan_width((SelectPlan*)q->plan) != 1) {
                snprintf(msg, sizeof(msg), "The subquery in %s must return exactly one column", b->clause);
                bind_error(b, msg);
            }
            return;
        }
        case EXPR_CASE:
            bind_expr(e->left, b);
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            bind_expr(e->right, b);
            return;
        case EXPR_WINDOW: {
            static const struct { const char* n; int lo, hi; } WF[] = {
                { "ROW_NUMBER", 0, 0 }, { "RANK", 0, 0 }, { "DENSE_RANK", 0, 0 }, { "NTILE", 1, 1 },
                { "LAG", 1, 3 }, { "LEAD", 1, 3 }, { "FIRST_VALUE", 1, 1 }, { "LAST_VALUE", 1, 1 },
                { "COUNT", 0, 1 }, { "SUM", 1, 1 }, { "AVG", 1, 1 }, { "MIN", 1, 1 }, { "MAX", 1, 1 },
            };
            int k = -1;
            for (size_t i = 0; i < sizeof(WF) / sizeof(WF[0]); i++) if (!strcmp(WF[i].n, e->func)) k = (int)i;
            if (k < 0) { snprintf(msg, sizeof(msg), "'%.32s' cannot be used with OVER", e->func); bind_error(b, msg); return; }
            if (e->nargs < WF[k].lo || e->nargs > WF[k].hi) { snprintf(msg, sizeof(msg), "Wrong number of arguments for %.32s", e->func); bind_error(b, msg); return; }
            if (b->mode != BIND_AGG_OK || !b->plan || (strcmp(b->clause, "SELECT") && strcmp(b->clause, "ORDER BY"))) {
                snprintf(msg, sizeof(msg), "Window functions (OVER) are not allowed in %s", b->clause);
                bind_error(b, msg);
                return;
            }
            if (b->in_agg) { bind_error(b, "Window functions cannot be inside an aggregate"); return; }
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            for (int i = 0; i < e->win->npart; i++) bind_expr(e->win->part[i], b);
            for (int i = 0; i < e->win->nord; i++) bind_expr(e->win->ord[i], b);
            if (!b->err[0] && !plan_add_window(b->plan, e)) bind_error(b, "Too many window functions (max 16)");
            return;
        }
        default:
            bind_expr(e->left, b);
            bind_expr(e->right, b);
            return;
    }
}

static bool expr_has_agg(const ASTExpr* e) {
    if (!e) return false;
    if (e->type == EXPR_AGG) return true;
    for (int i = 0; i < e->nargs; i++) if (expr_has_agg(e->args[i])) return true;
    return expr_has_agg(e->left) || expr_has_agg(e->right);
}

// ── valutazione ──

static void eval_expr(ASTExpr* e, EvalCtx* c, EvalVal* out);
static bool eval_cond(ASTExpr* e, EvalCtx* c);
static void eval_subquery(ASTExpr* e, EvalCtx* c, EvalVal* out);
static void ev_from_oval(EvalVal* v, const SortKey* o);

static bool like_match(const char* s, const char* p);

// testo di un valore (numeri formattati); per i NULL: ""
static const char* ev_str(const EvalVal* v) { return ev_s(v); }

static int utf8_chars(const char* s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

// byte di inizio del carattere numero ci (0-based) nel testo s
static const char* utf8_at(const char* s, int ci) {
    while (*s && ci > 0) { s++; while (((unsigned char)*s & 0xC0) == 0x80) s++; ci--; }
    return s;
}

static void ev_set_text(EvalCtx* c, EvalVal* out, const char* s, size_t n) {
    if (!ev_text(out, s, n)) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); }
}

static void now_text(char* buf, size_t n, bool date_only) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, n, date_only ? "%Y-%m-%d" : "%Y-%m-%d %H:%M:%S", &tmv);
}

static void eval_func(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    EvalVal a[3];
    int na = e->nargs < 3 ? e->nargs : 3;

    // funzioni che non valutano tutti gli argomenti subito
    if (!strcmp(f, "COALESCE")) {
        for (int i = 0; i < e->nargs; i++) {
            eval_expr(e->args[i], c, out);
            if (!out->null) return;
        }
        ev_null(out);
        return;
    }
    if (!strcmp(f, "IFNULL")) {
        eval_expr(e->args[0], c, out);
        if (out->null) eval_expr(e->args[1], c, out);
        return;
    }
    if (!strcmp(f, "IF")) {
        EvalVal cond;
        eval_expr(e->args[0], c, &cond);
        eval_expr(e->args[ev_truth(&cond) == 1 ? 1 : 2], c, out);
        return;
    }
    if (!strcmp(f, "CONCAT") || !strcmp(f, "CONCAT_WS")) {
        bool ws = f[6] == '_';
        char buf[EV_STR_MAX + 2];
        size_t len = 0;
        EvalVal sep, v;
        if (ws) { eval_expr(e->args[0], c, &sep); if (sep.null) { ev_null(out); return; } }
        bool first = true;
        for (int i = ws ? 1 : 0; i < e->nargs; i++) {
            eval_expr(e->args[i], c, &v);
            if (v.null) { if (ws) continue; ev_null(out); return; }   // CONCAT con un NULL = NULL
            const char* parts[2] = { ws && !first ? ev_s(&sep) : "", ev_s(&v) };
            for (int p = 0; p < 2; p++) {
                size_t l = strlen(parts[p]);
                if (len + l > EV_STR_MAX) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); return; }
                memcpy(buf + len, parts[p], l);
                len += l;
            }
            first = false;
        }
        ev_set_text(c, out, buf, len);
        return;
    }
    if (!strcmp(f, "NOW") || !strcmp(f, "CURRENT_TIMESTAMP") || !strcmp(f, "CURDATE") || !strcmp(f, "CURRENT_DATE")) {
        char buf[32];
        now_text(buf, sizeof(buf), !strcmp(f, "CURDATE") || !strcmp(f, "CURRENT_DATE"));
        ev_set_text(c, out, buf, strlen(buf));
        return;
    }

    for (int i = 0; i < na; i++) {
        eval_expr(e->args[i], c, &a[i]);
        if (a[i].null && strcmp(f, "NULLIF")) { ev_null(out); return; }   // NULL in ingresso -> NULL
    }
    if (!strcmp(f, "NULLIF")) {
        if (a[0].null) { ev_null(out); return; }
        if (!a[1].null && ev_cmp(&a[0], &a[1]) == 0) { ev_null(out); return; }
        ev_copy(out, &a[0]);
        return;
    }
    if (!strcmp(f, "UPPER") || !strcmp(f, "UCASE") || !strcmp(f, "LOWER") || !strcmp(f, "LCASE")) {
        bool up = f[0] == 'U';
        char buf[EV_STR_MAX + 1];
        size_t l = strlen(ev_s(&a[0]));
        for (size_t i = 0; i < l; i++) buf[i] = (char)(up ? toupper((unsigned char)ev_s(&a[0])[i]) : tolower((unsigned char)ev_s(&a[0])[i]));
        ev_set_text(c, out, buf, l);
        return;
    }
    if (!strcmp(f, "LENGTH"))        { ev_int(out, (int64_t)strlen(ev_s(&a[0]))); return; }
    if (!strncmp(f, "CHAR", 4))      { ev_int(out, utf8_chars(ev_s(&a[0]))); return; }
    if (!strcmp(f, "TRIM") || !strcmp(f, "LTRIM") || !strcmp(f, "RTRIM")) {
        const char* s = ev_s(&a[0]);
        const char* e2 = s + strlen(s);
        if (f[0] != 'R') while (*s == ' ') s++;
        if (f[0] != 'L') while (e2 > s && e2[-1] == ' ') e2--;
        ev_set_text(c, out, s, (size_t)(e2 - s));
        return;
    }
    if (!strcmp(f, "SUBSTRING") || !strcmp(f, "SUBSTR") || !strcmp(f, "LEFT") || !strcmp(f, "RIGHT")) {
        const char* s = ev_s(&a[0]);
        int chars = utf8_chars(s);
        int64_t start, count;
        if (!strcmp(f, "LEFT"))       { start = 1; count = (int64_t)ev_as_num(&a[1]); }
        else if (!strcmp(f, "RIGHT")) { count = (int64_t)ev_as_num(&a[1]); start = chars - count + 1; if (start < 1) start = 1; }
        else {
            start = (int64_t)ev_as_num(&a[1]);
            if (start < 0) start = chars + start + 1;        // posizioni negative: dalla fine
            count = e->nargs > 2 ? (int64_t)ev_as_num(&a[2]) : chars;
            if (start < 1) { ev_set_text(c, out, "", 0); return; }
        }
        if (count <= 0 || start > chars) { ev_set_text(c, out, "", 0); return; }
        const char* b0 = utf8_at(s, (int)(start - 1));
        const char* b1 = utf8_at(b0, (int)(count > chars ? chars : count));
        ev_set_text(c, out, b0, (size_t)(b1 - b0));
        return;
    }
    if (!strcmp(f, "REPLACE")) {
        const char* s = ev_s(&a[0]), *from = ev_s(&a[1]), *to = ev_s(&a[2]);
        size_t fl = strlen(from), tl = strlen(to), len = 0;
        char buf[EV_STR_MAX + 2];
        while (*s) {
            if (fl && !strncmp(s, from, fl)) {
                if (len + tl > EV_STR_MAX) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); return; }
                memcpy(buf + len, to, tl); len += tl; s += fl;
            } else {
                if (len + 1 > EV_STR_MAX) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); return; }
                buf[len++] = *s++;
            }
        }
        ev_set_text(c, out, buf, len);
        return;
    }
    if (!strcmp(f, "REVERSE")) {
        const char* s = ev_s(&a[0]);
        size_t l = strlen(s);
        char buf[EV_STR_MAX + 1];
        size_t k = 0;
        for (size_t i = l; i > 0; ) {                       // carattere UTF-8 intero
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
            memcpy(buf + k, s + j, i - j);
            k += i - j;
            i = j;
        }
        ev_set_text(c, out, buf, k);
        return;
    }
    if (!strcmp(f, "YEAR") || !strcmp(f, "MONTH") || !strcmp(f, "DAY")) {
        int y, m, d;
        if (sscanf(ev_s(&a[0]), "%d-%d-%d", &y, &m, &d) != 3) { ev_null(out); return; }
        ev_int(out, f[0] == 'Y' ? y : f[0] == 'M' ? m : d);
        return;
    }
    // numeriche
    double x = ev_as_num(&a[0]);
    if (!strcmp(f, "ABS")) {
        if (a[0].is_int && a[0].inum != INT64_MIN) ev_int(out, a[0].inum < 0 ? -a[0].inum : a[0].inum);
        else ev_num(out, fabs(x));
        return;
    }
    if (!strcmp(f, "SIGN"))  { ev_int(out, x > 0 ? 1 : x < 0 ? -1 : 0); return; }
    if (!strcmp(f, "FLOOR") || !strncmp(f, "CEIL", 4)) {
        double r = f[0] == 'F' ? floor(x) : ceil(x);
        if (r >= -9.2e18 && r <= 9.2e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return;
    }
    if (!strcmp(f, "ROUND")) {
        double dd = e->nargs > 1 ? ev_as_num(&a[1]) : 0;
        if (dd > 15) { ev_copy(out, &a[0]); if (!out->is_num) ev_num(out, x); return; }   // più cifre di quante ne ha un double
        if (dd < -308) { ev_int(out, 0); return; }
        int d = (int)dd;
        if (a[0].is_int && d >= 0) { ev_int(out, a[0].inum); return; }
        double p = pow(10.0, d);
        double r = (x < 0 ? -floor(-x * p + 0.5) : floor(x * p + 0.5)) / p;   // metà lontano da zero
        if (d <= 0 && r >= -9.2e18 && r <= 9.2e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return;
    }
    if (!strcmp(f, "MOD")) {
        if (a[0].is_int && a[1].is_int) {
            if (a[1].inum == 0) { ev_null(out); return; }
            if (a[1].inum == -1) { ev_int(out, 0); return; }
            ev_int(out, a[0].inum % a[1].inum);
            return;
        }
        double y = ev_as_num(&a[1]);
        if (y == 0) { ev_null(out); return; }
        ev_num(out, fmod(x, y));
        return;
    }
    if (!strncmp(f, "POW", 3)) { ev_num(out, pow(x, ev_as_num(&a[1]))); return; }
    if (!strcmp(f, "SQRT"))    { if (x < 0) ev_null(out); else ev_num(out, sqrt(x)); return; }
    ev_null(out);
}

static void eval_aggregate(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    if (!c->grp) { ctx_error(c, "%s", "Aggregate used outside of a group"); ev_null(out); return; }
    bool count_star = e->nargs == 0;
    if (count_star) { ev_int(out, c->ngrp); return; }

    Tuple** saved = c->cur;
    int64_t count = 0, isum = 0;
    double sum = 0;
    bool all_int = true, overflow = false, have_best = false;
    bool is_min = !strcmp(e->func, "MIN"), is_max = !strcmp(e->func, "MAX");
    EvalVal best, v;
    StrMap seen = {0};
    for (int k = 0; k < c->ngrp; k++) {
        c->cur = c->grp[k];
        eval_expr(e->args[0], c, &v);
        if (v.null) continue;
        if (e->agg_distinct) {                                   // COUNT/SUM/AVG(DISTINCT x)
            // i numeri uguali hanno la stessa chiave (5 e 5.0), i testi restano distinti dai numeri
            char key[EV_STR_MAX + 4];
            if (v.is_int) snprintf(key, sizeof(key), "n%lld", (long long)v.inum);
            else if (v.is_num && v.num == floor(v.num) && fabs(v.num) < 9e18) snprintf(key, sizeof(key), "n%lld", (long long)v.num);
            else if (v.is_num) snprintf(key, sizeof(key), "n%.17g", v.num);
            else snprintf(key, sizeof(key), "s%s", ev_s(&v));
            SMSlot* sl = sm_slot(&seen, key, true);
            if (!sl) { ctx_error(c, "%s", "Out of memory"); break; }
            if (sl->val >= 0) continue;
            sl->val = 1;
        }
        count++;
        if (v.is_num || !(is_min || is_max)) {
            double d = ev_as_num(&v);
            sum += d;
            if (v.is_int && !overflow) { if (__builtin_add_overflow(isum, v.inum, &isum)) overflow = true; }
            else all_int = false;
        }
        if ((is_min || is_max) && (!have_best || (is_min ? ev_cmp(&v, &best) < 0 : ev_cmp(&v, &best) > 0))) {
            ev_copy(&best, &v);
            if (best.str && best.str != best.buf) {   // il testo punta alla tupla: copia (la riga corrente cambia)
                size_t l = strlen(best.str);
                memcpy(best.buf, best.str, l > EV_STR_MAX ? EV_STR_MAX : l);
                best.buf[l > EV_STR_MAX ? EV_STR_MAX : l] = '\0';
                best.str = best.buf;
            }
            have_best = true;
        }
    }
    sm_free(&seen);
    c->cur = saved;
    if (!strcmp(e->func, "COUNT")) { ev_int(out, count); return; }
    if (count == 0) { ev_null(out); return; }                    // SUM/AVG/MIN/MAX di nessun valore = NULL
    if (!strcmp(e->func, "SUM")) { if (all_int && !overflow) ev_int(out, isum); else ev_num(out, sum); return; }
    if (!strcmp(e->func, "AVG")) { ev_num(out, sum / (double)count); return; }
    ev_copy(out, &best);
}

static void eval_expr(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    if (!e) { ev_null(out); return; }
    switch (e->type) {
        case EXPR_NULL:   ev_null(out); return;
        case EXPR_INT:    ev_int(out, e->int_val); return;
        case EXPR_FLOAT:  ev_num(out, e->float_val); return;
        case EXPR_STRING: out->null = false; out->is_num = out->is_int = false; out->str = e->str_val; return;
        case EXPR_COLUMN: {
            if (e->bound < 0) {                                  // alias della SELECT
                int k = -e->bound - 1;
                if (k < c->nsel) { eval_expr(c->sel[k], c, out); return; }
                ev_null(out);
                return;
            }
            if (e->bound == 0) { ctx_error(c, "Unresolved column '%s'", e->col_name); ev_null(out); return; }
            const EvalCtx* cc = c;
            for (int k = 0; k < e->outer && cc; k++) cc = cc->outer;   // colonna della query esterna
            int s = (e->bound - 1) / 1024, col = (e->bound - 1) % 1024;
            Tuple* t = (cc && cc->cur && s < cc->nsrc) ? cc->cur[s] : NULL;
            if (!t || col >= t->num_values) { ev_null(out); return; }
            ev_from_value(out, &t->values[col], cc->src[s].ts->columns[col].type);
            return;
        }
        case EXPR_SUBQUERY: eval_subquery(e, c, out); return;
        case EXPR_WINDOW:
            if (!c->win || !e->win || !c->win[e->win->slot]) { ctx_error(c, "%s", "Window function used outside of SELECT / ORDER BY"); ev_null(out); return; }
            ev_from_oval(out, &c->win[e->win->slot][c->win_row]);
            return;
        case EXPR_CASE: {
            EvalVal a, w;
            if (e->left) eval_expr(e->left, c, &a);
            for (int i = 0; i + 1 < e->nargs; i += 2) {
                bool hit;
                if (e->left) {                                   // CASE x WHEN v THEN ...: x = v
                    eval_expr(e->args[i], c, &w);
                    hit = !a.null && !w.null && ev_cmp(&a, &w) == 0;
                } else {
                    hit = eval_cond(e->args[i], c);
                }
                if (hit) { eval_expr(e->args[i + 1], c, out); return; }
            }
            if (e->right) eval_expr(e->right, c, out); else ev_null(out);
            return;
        }
        case EXPR_AGG:  eval_aggregate(e, c, out); return;
        case EXPR_FUNC: eval_func(e, c, out); return;
        case EXPR_STAR: ev_null(out); return;
        case EXPR_UNARY: {
            EvalVal a;
            eval_expr(e->left, c, &a);
            switch (e->op) {
                case OP_IS_NULL:     ev_int(out, a.null); return;
                case OP_IS_NOT_NULL: ev_int(out, !a.null); return;
                case OP_NOT: {
                    int t = ev_truth(&a);
                    if (t < 0) ev_null(out); else ev_int(out, !t);
                    return;
                }
                case OP_NEG:
                    if (a.null) { ev_null(out); return; }
                    if (a.is_int && a.inum != INT64_MIN) ev_int(out, -a.inum);
                    else ev_num(out, -ev_as_num(&a));
                    return;
                default: ev_null(out); return;
            }
        }
        case EXPR_BINARY: {
            if (e->op == OP_AND || e->op == OP_OR) {             // logica a tre valori, con cortocircuito
                EvalVal a, b;
                eval_expr(e->left, c, &a);
                int ta = ev_truth(&a);
                if (e->op == OP_AND && ta == 0) { ev_int(out, 0); return; }
                if (e->op == OP_OR && ta == 1)  { ev_int(out, 1); return; }
                eval_expr(e->right, c, &b);
                int tb = ev_truth(&b);
                if (e->op == OP_AND) { if (tb == 0) ev_int(out, 0); else if (ta < 0 || tb < 0) ev_null(out); else ev_int(out, 1); }
                else                 { if (tb == 1) ev_int(out, 1); else if (ta < 0 || tb < 0) ev_null(out); else ev_int(out, 0); }
                return;
            }
            EvalVal a, b;
            eval_expr(e->left, c, &a);
            eval_expr(e->right, c, &b);
            if (a.null || b.null) { ev_null(out); return; }
            switch (e->op) {
                case OP_EQ: ev_int(out, ev_cmp(&a, &b) == 0); return;
                case OP_NEQ: ev_int(out, ev_cmp(&a, &b) != 0); return;
                case OP_LT: ev_int(out, ev_cmp(&a, &b) < 0); return;
                case OP_GT: ev_int(out, ev_cmp(&a, &b) > 0); return;
                case OP_LE: ev_int(out, ev_cmp(&a, &b) <= 0); return;
                case OP_GE: ev_int(out, ev_cmp(&a, &b) >= 0); return;
                case OP_LIKE:     ev_int(out, like_match(ev_s(&a), ev_s(&b))); return;
                case OP_NOT_LIKE: ev_int(out, !like_match(ev_s(&a), ev_s(&b))); return;
                case OP_PLUS: case OP_MINUS: case OP_MUL: {
                    double x = ev_as_num(&a), y = ev_as_num(&b);
                    bool ints = (a.is_int || (!a.is_num && x == (double)(int64_t)x && strpbrk(ev_s(&a), ".eE") == NULL)) &&
                                (b.is_int || (!b.is_num && y == (double)(int64_t)y && strpbrk(ev_s(&b), ".eE") == NULL));
                    if (ints) {
                        int64_t xi = a.is_int ? a.inum : (int64_t)x, yi = b.is_int ? b.inum : (int64_t)y, r;
                        bool ovf = e->op == OP_PLUS  ? __builtin_add_overflow(xi, yi, &r)
                                 : e->op == OP_MINUS ? __builtin_sub_overflow(xi, yi, &r)
                                                     : __builtin_mul_overflow(xi, yi, &r);
                        if (!ovf) { ev_int(out, r); return; }
                        ctx_error(c, "%s", "Integer result out of range (64 bit)");
                        ev_null(out);
                        return;
                    }
                    ev_num(out, e->op == OP_PLUS ? x + y : e->op == OP_MINUS ? x - y : x * y);
                    return;
                }
                case OP_MOD: {                                   // come MOD(a, b)
                    if (a.is_int && b.is_int) {
                        if (b.inum == 0) { ev_null(out); return; }
                        ev_int(out, b.inum == -1 ? 0 : a.inum % b.inum);
                        return;
                    }
                    double y = ev_as_num(&b);
                    if (y == 0) { ev_null(out); return; }
                    ev_num(out, fmod(ev_as_num(&a), y));
                    return;
                }
                case OP_DIV: {
                    double y = ev_as_num(&b);
                    if (y == 0) { ev_null(out); return; }            // divisione per zero: NULL (come MySQL)
                    ev_num(out, ev_as_num(&a) / y);
                    return;
                }
                default: ev_null(out); return;
            }
        }
    }
    ev_null(out);
}

// la condizione è vera per la riga corrente? (NULL conta come falso)
static bool eval_cond(ASTExpr* e, EvalCtx* c) {
    if (!e) return true;
    EvalVal v;
    eval_expr(e, c, &v);
    return ev_truth(&v) == 1;
}

// ═════════════════════════════════════════════════════════════════
//  SELECT: sorgenti -> JOIN -> WHERE -> GROUP BY / HAVING -> proiezione
//          -> DISTINCT -> ORDER BY -> OFFSET / LIMIT
// ═════════════════════════════════════════════════════════════════

#define MAX_VIEW_DEPTH   16
#define MAX_JOIN_ROWS    10000000

static bool is_text_type(DataType t) {
    return t == SV_TYPE_VARCHAR || t == SV_TYPE_CHAR || t == SV_TYPE_TEXT || t == SV_TYPE_DATE || t == SV_TYPE_DATETIME;
}
static bool is_int_type(DataType t) { return t == SV_TYPE_INT || t == SV_TYPE_BIGINT; }
// tipi conservati come numeri nella tupla (gli altri sono testo)
static bool is_num_storage(DataType t) { return t == SV_TYPE_INT || t == SV_TYPE_BIGINT || t == SV_TYPE_FLOAT || t == SV_TYPE_BOOL; }

static Tuple* tuple_clone(const Tuple* t, const TableSchema* ts);

// chiave per l'hash join: interi -> numero, testo -> testo; false per NULL
static bool join_key(const Tuple* t, int col, DataType type, char* buf, size_t n) {
    if (!t || col >= t->num_values || t->values[col].is_null) return false;
    const Value* v = &t->values[col];
    if (type == SV_TYPE_INT)         snprintf(buf, n, "%d", v->int_val);
    else if (type == SV_TYPE_BIGINT) snprintf(buf, n, "%lld", (long long)v->bigint_val);
    else                             snprintf(buf, n, "%s", v->str_val ? v->str_val : "");
    return true;
}

// in una congiunzione di AND cerca "colonna indicizzata della sorgente 0 = costante"
static bool find_index_eq(ASTExpr* w, TableSchema* ts, int* out_ci, char* key, size_t keyn) {
    if (!w || w->type != EXPR_BINARY) return false;
    if (w->op == OP_AND) return find_index_eq(w->left, ts, out_ci, key, keyn) || find_index_eq(w->right, ts, out_ci, key, keyn);
    if (w->op != OP_EQ) return false;
    for (int side = 0; side < 2; side++) {
        ASTExpr* col = side ? w->right : w->left;
        ASTExpr* val = side ? w->left : w->right;
        if (col->type != EXPR_COLUMN || col->bound <= 0 || (col->bound - 1) / 1024 != 0) continue;
        int ci = (col->bound - 1) % 1024;
        if (!col_is_indexed(ts, ci)) continue;
        DataType t = ts->columns[ci].type;
        if (is_int_type(t) && val->type == EXPR_INT) {
            if (t == SV_TYPE_INT && (val->int_val < INT32_MIN || val->int_val > INT32_MAX)) continue;
            snprintf(key, keyn, "%lld", (long long)val->int_val);
        } else if (is_text_type(t) && val->type == EXPR_STRING) {
            snprintf(key, keyn, "%s", val->str_val);
        } else {
            continue;
        }
        *out_ci = ci;
        return true;
    }
    return false;
}

// carica le righe di una tabella (tutte, o solo quelle trovate con l'indice)
static bool source_load(ExecCtx* ctx, const char* db, Source* s, ASTExpr* where_for_index, char* plan, size_t plann) {
    if (s->loaded) return true;
    s->loaded = true;
    HeapFile* hf = heap_open(ctx->bp, s->ts->root_page_id, s->ts->num_pages);
    if (!hf) return false;
    int cap = 0, ci = -1;
    char key[EV_STR_MAX + 1];
    if (where_for_index && find_index_eq(where_for_index, s->ts, &ci, key, sizeof(key))) {
        MemIndex* ix = mi_get(ctx, db, s->ts, ci);
        if (ix) {
            if (plan) snprintf(plan, plann, "%s = %s", s->ts->columns[ci].name, key);
            const RidList* l = mi_lookup(ix, key);
            for (int i = 0; l && i < l->n; i++) {
                Tuple* t = heap_get(hf, l->r[i], s->ts);
                if (t && !source_push(s, t, l->r[i], &cap)) { tuple_free(t); free(hf); return false; }
            }
            free(hf);
            return true;
        }
    }
    HeapScan* scan = heap_scan_open(hf, s->ts);
    if (!scan) { free(hf); return false; }
    Tuple* t;
    RID rid;
    bool ok = true;
    while (ok && (t = heap_scan_next(scan, &rid)) != NULL)
        if (!source_push(s, t, rid, &cap)) { tuple_free(t); ok = false; }
    heap_scan_close(scan);
    free(hf);
    return ok;
}


// ── valori del risultato (tipizzati: i numeri restano numeri) ──

typedef SortKey OVal;

// copia di un valore calcolato (il testo viene duplicato; i numeri senza testo restano binari)
static void key_set(SortKey* k, const EvalVal* v) {
    k->null = v->null; k->is_num = v->is_num; k->is_int = v->is_int; k->inum = v->inum; k->num = v->num;
    k->str = (v->null || (v->is_num && !v->str)) ? NULL : strdup(ev_s(v));
}

static void ev_from_oval(EvalVal* v, const OVal* o) {
    if (o->null) { ev_null(v); return; }
    v->null = false; v->is_num = o->is_num; v->is_int = o->is_int; v->inum = o->inum; v->num = o->num;
    v->str = o->str ? o->str : (o->is_num ? NULL : "");
}

static int key_cmp(const SortKey* a, const SortKey* b) {
    if (a->null || b->null) return a->null == b->null ? 0 : a->null ? -1 : 1;   // NULL per primi
    if (a->is_int && b->is_int) return a->inum < b->inum ? -1 : a->inum > b->inum;
    if (a->is_num && b->is_num) return a->num < b->num ? -1 : a->num > b->num;
    if (!a->is_num && !b->is_num) { int c = strcmp(a->str ? a->str : "", b->str ? b->str : ""); return c < 0 ? -1 : c > 0; }
    EvalVal x, y;
    ev_from_oval(&x, a);
    ev_from_oval(&y, b);
    return ev_cmp(&x, &y);
}

// ── hash tipizzati (GROUP BY, hash join) ──

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

static uint64_t str_hash(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

// valori uguali per il raggruppamento hanno lo stesso hash (5 e 5.0 compresi)
static uint64_t ev_hash(const EvalVal* v) {
    if (v->null) return 0x9e3779b97f4a7c15ULL;
    if (v->is_num) {
        if (v->is_int) return mix64((uint64_t)v->inum);
        double d = v->num;
        if (d == floor(d) && fabs(d) < 9e18) return mix64((uint64_t)(int64_t)d);
        uint64_t b;
        memcpy(&b, &d, 8);
        return mix64(b ^ 0x5bd1e995ULL);
    }
    return str_hash(ev_s(v)) ^ 0x27d4eb2f165667c5ULL;
}

// stessa chiave di raggruppamento? (NULL con NULL sì, numeri per valore, testi per contenuto)
static bool ev_eq_key(const EvalVal* a, const SortKey* b) {
    if (a->null || b->null) return a->null && b->null;
    if (a->is_num != b->is_num) return false;
    if (a->is_num) return (a->is_int && b->is_int) ? a->inum == b->inum : a->num == b->num;
    return strcmp(ev_s(a), b->str ? b->str : "") == 0;
}

typedef struct { uint64_t* h; int* v; size_t cap, n; } HMap;

static void hmap_free(HMap* m) { free(m->h); free(m->v); memset(m, 0, sizeof(*m)); }

// inserisce un elemento (anche con hash già presente: le collisioni si risolvono confrontando)
static bool hmap_put(HMap* m, uint64_t h, int val) {
    if ((m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 64;
        uint64_t* nh = malloc(sizeof(uint64_t) * nc);
        int* nv = malloc(sizeof(int) * nc);
        if (!nh || !nv) { free(nh); free(nv); return false; }
        for (size_t i = 0; i < nc; i++) nv[i] = -1;
        for (size_t i = 0; i < m->cap; i++) {
            if (m->v[i] < 0) continue;
            size_t j = m->h[i] & (nc - 1);
            while (nv[j] >= 0) j = (j + 1) & (nc - 1);
            nh[j] = m->h[i]; nv[j] = m->v[i];
        }
        free(m->h); free(m->v);
        m->h = nh; m->v = nv; m->cap = nc;
    }
    size_t j = h & (m->cap - 1);
    while (m->v[j] >= 0) j = (j + 1) & (m->cap - 1);
    m->h[j] = h; m->v[j] = val;
    m->n++;
    return true;
}

// hash del valore di una colonna (interi come numero, testi come testo); false per NULL
static bool col_hash(const Tuple* t, int col, DataType type, uint64_t* h) {
    if (!t || col >= t->num_values || t->values[col].is_null) return false;
    const Value* v = &t->values[col];
    if (type == SV_TYPE_INT) *h = mix64((uint64_t)(int64_t)v->int_val);
    else if (type == SV_TYPE_BIGINT) *h = mix64((uint64_t)v->bigint_val);
    else *h = str_hash(v->str_val ? v->str_val : "");
    return true;
}

// chiave canonica (DISTINCT, UNION, INTERSECT, EXCEPT): numeri uguali hanno la stessa chiave
static size_t oval_key(const OVal* o, char* buf, size_t cap) {
    int n;
    if (o->null)        n = snprintf(buf, cap, "\x01\x1f");
    else if (o->is_int) n = snprintf(buf, cap, "n%lld\x1f", (long long)o->inum);
    else if (o->is_num && o->num == floor(o->num) && fabs(o->num) < 9e18) n = snprintf(buf, cap, "n%lld\x1f", (long long)o->num);
    else if (o->is_num) n = snprintf(buf, cap, "n%.17g\x1f", o->num);
    else                n = snprintf(buf, cap, "s%.511s\x1f", o->str ? o->str : "");
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

typedef struct { OVal* v; int nrows, ncols, cap; } ORows;

static void orows_free(ORows* r) {
    for (long i = 0; i < (long)r->nrows * r->ncols; i++) free(r->v[i].str);
    free(r->v);
    int nc = r->ncols;
    memset(r, 0, sizeof(*r));
    r->ncols = nc;
}

static OVal* orows_add(ORows* r) {
    int w = r->ncols > 0 ? r->ncols : 1;
    if (r->nrows >= r->cap) {
        int nc = r->cap ? r->cap * 2 : 64;
        OVal* nv = realloc(r->v, sizeof(OVal) * (size_t)nc * (size_t)w);
        if (!nv) return NULL;
        r->v = nv;
        r->cap = nc;
    }
    OVal* row = r->v + (size_t)r->nrows * (size_t)w;
    memset(row, 0, sizeof(OVal) * (size_t)w);
    r->nrows++;
    return row;
}

static OVal* orow(const ORows* r, int i) { return r->v + (size_t)i * (size_t)(r->ncols > 0 ? r->ncols : 1); }

static size_t row_sig(const OVal* row, int ncols, char* buf, size_t cap) {
    size_t len = 0;
    buf[0] = '\0';
    for (int j = 0; j < ncols && len + 1 < cap; j++) len += oval_key(&row[j], buf + len, cap - len);
    return len;
}

// ── colonne di output ──

typedef struct {
    ASTExpr* e;
    bool     owned;          // creata per espandere *
    char     name[160];
    int      src_type;       // tipo della colonna se è un riferimento diretto, altrimenti -1
} OutCol;

static void fill_cell(Cell* cell, const EvalVal* v, int src_type) {
    if (v->null) { cell->is_null = true; cell->type = CELL_TYPE_NULL; snprintf(cell->value, sizeof(cell->value), "NULL"); return; }
    cell->is_null = false;
    snprintf(cell->value, sizeof(cell->value), "%s", ev_s(v));
    if (src_type >= 0)  cell->type = cell_type_of((DataType)src_type);
    else if (v->is_int) cell->type = CELL_TYPE_INT;
    else if (v->is_num) cell->type = CELL_TYPE_FLOAT;
    else                cell->type = CELL_TYPE_STRING;
}

// ── vettori di espressioni ──

typedef struct { ASTExpr** v; int n, cap; } ExprVec;

static bool xv_push(ExprVec* xv, ASTExpr* e) {
    if (xv->n >= xv->cap) {
        int nc = xv->cap ? xv->cap * 2 : 4;
        ASTExpr** nv = realloc(xv->v, sizeof(ASTExpr*) * (size_t)nc);
        if (!nv) return false;
        xv->v = nv;
        xv->cap = nc;
    }
    xv->v[xv->n++] = e;
    return true;
}

static bool xv_all_true(const ExprVec* xv, EvalCtx* c) {
    for (int i = 0; i < xv->n; i++) if (!eval_cond(xv->v[i], c)) return false;
    return true;
}

// ═════════════════════════════════════════════════════════════════
//  Piano di una SELECT
//  plan_prepare apre le sorgenti, risolve i nomi e decide dove applicare ogni condizione;
//  query_exec lo esegue (una volta, o una volta per riga esterna se è una subquery correlata).
//
//  Ottimizzazioni (le stesse dei DBMS reali, visibili con EXPLAIN):
//  - selection pushdown: le condizioni della WHERE su una sola tabella vengono applicate
//    durante la lettura della tabella, prima dei join
//  - le condizioni fra due tabelle (anche di FROM a, b WHERE a.x = b.y) diventano condizioni
//    di join: con un'uguaglianza il join è un hash join invece di un prodotto cartesiano
//  - accesso con indice per "colonna indicizzata = costante"
//  - subquery non correlate eseguite una sola volta (IN con una tabella hash);
//    subquery correlate con le righe lette una volta sola e una ricerca per chiave
//  - ORDER BY + LIMIT: si ordina e si taglia prima di calcolare le colonne di output
// ═════════════════════════════════════════════════════════════════

struct SelectPlan {
    ExecCtx*  x;
    ASTNode*  n;
    int       depth;
    CteScope  scope;
    Source    src[MAX_SOURCES];
    int       nsrc;
    OutCol    out[SV_MAX_COLUMNS];
    int       nout;
    ASTExpr*  sel[SV_MAX_COLUMNS];
    bool      grouped;
    bool      correlated;        // usa colonne di una query esterna: rieseguita per ogni riga esterna
    bool      compound;          // primo ramo di UNION / INTERSECT / EXCEPT
    struct SelectPlan* next_arm; // ramo successivo
    int       corder[SV_MAX_ORDER];   // ORDER BY dell'operazione insiemistica: colonna di output
    // dove si applica ogni condizione
    ExprVec   pre_static[MAX_SOURCES];  // filtri della sorgente valutati durante la lettura
    ExprVec   pre_dyn[MAX_SOURCES];     // filtri della sorgente che usano la query esterna
    ExprVec   jcond[MAX_SOURCES];       // condizioni del join con la sorgente k
    ExprVec   resid;                    // resto della WHERE (dopo i join)
    // ricerca per chiave nella sorgente: colonna = espressione senza colonne della sorgente
    int       probe_col[MAX_SOURCES];
    ASTExpr*  probe_key[MAX_SOURCES];
    bool      probe_dyn[MAX_SOURCES];
    bool      used_index[MAX_SOURCES];
    StrMap    pmap[MAX_SOURCES];
    int*      pnext[MAX_SOURCES];
    bool      loaded;
    // risultato di una subquery non correlata
    ORows     cache;
    bool      cache_ok;
    int       cache_stop;
    StrMap    inset;                    // IN (SELECT ...): valori del risultato
    int       inset_class;              // 1 numeri, 2 testi, 3 misto
    bool      inset_null;
    bool      inset_built;
    long      execs;                    // esecuzioni (EXPLAIN delle subquery)
    // NATURAL / USING: copia di destra di una colonna unita -> colonna di sinistra (s*1024+c+1)
    int       using_left[MAX_SOURCES][SV_MAX_COLUMNS];
    // funzioni finestra della query
    ASTExpr*  wins[16];
    int       nwins;
    // righe reali per EXPLAIN ANALYZE (sommate su tutte le esecuzioni)
    long      st_scanned[MAX_SOURCES];  // righe lette dalla tabella
    long      st_loaded[MAX_SOURCES];   // righe dopo i filtri della lettura
    long      st_dyn[MAX_SOURCES];      // righe dopo i filtri correlati
    long      st_join[MAX_SOURCES];     // combinazioni dopo il join con la sorgente k
    long      st_resid, st_groups, st_having, st_out;
    double    st_ms;
    char      err[300];
};

static int plan_width(const SelectPlan* p) { return p->nout; }
static int plan_using_left(const SelectPlan* p, int s, int c) {
    return (s >= 0 && s < MAX_SOURCES && c >= 0 && c < SV_MAX_COLUMNS) ? p->using_left[s][c] : 0;
}
// la colonna (s, c) è unita (NATURAL / USING) a una colonna di una tabella successiva?
// restituisce il tipo di quel join (-1 = no)
static int plan_joined_copy(const SelectPlan* p, int s, int c, int* rk, int* rc) {
    for (int q = s + 1; q < p->nsrc; q++)
        for (int cc = 0; cc < p->src[q].ts->num_columns && cc < SV_MAX_COLUMNS; cc++)
            if (p->using_left[q][cc] == s * 1024 + c + 1) { if (rk) *rk = q; if (rc) *rc = cc; return (int)p->n->select_stmt.joins[q - 1].type; }
    return -1;
}

static int plan_join_type(const SelectPlan* p, int s) {
    return (s >= 1 && s < p->nsrc) ? (int)p->n->select_stmt.joins[s - 1].type : JOIN_INNER;
}
static void plan_mark_correlated(SelectPlan* p) { for (; p; p = p->next_arm) p->correlated = true; }
static bool plan_add_window(SelectPlan* p, ASTExpr* w) {
    for (int i = 0; i < p->nwins; i++) if (p->wins[i] == w) return true;
    if (p->nwins >= 16) return false;
    w->win->slot = p->nwins;
    p->wins[p->nwins++] = w;
    return true;
}

static void plan_free(SelectPlan* p) {
    while (p) {
        SelectPlan* nx = p->next_arm;
        for (int s = 0; s < MAX_SOURCES; s++) {
            free(p->pre_static[s].v); free(p->pre_dyn[s].v); free(p->jcond[s].v);
            sm_free(&p->pmap[s]); free(p->pnext[s]);
        }
        free(p->resid.v);
        for (int i = 0; i < p->nout; i++) if (p->out[i].owned) ast_expr_free(p->out[i].e);
        for (int s = 0; s < p->nsrc; s++) source_free(&p->src[s]);
        orows_free(&p->cache);
        sm_free(&p->inset);
        free(p);
        p = nx;
    }
}

static void plan_free_void(void* p) { plan_free((SelectPlan*)p); }

static bool query_exec(SelectPlan* p, EvalCtx* outer, int stop_after, ORows* res);
static bool arm_exec(SelectPlan* p, EvalCtx* outer, int stop_after, bool apply_order, ORows* res);
static bool rows_append(ORows* dst, ORows* src, int i);
static SelectPlan* plan_prepare_ex(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                   char* err, size_t errn, bool arm);

// tipo (nome) della colonna j del risultato: dallo schema, dalla funzione o dai valori
static const char* out_type_name(const SelectPlan* p, int j, const ORows* rows, bool for_schema) {
    const OutCol* oc = &p->out[j];
    const ASTExpr* e = oc->e;
    if (oc->src_type >= 0) return datatype_name((DataType)oc->src_type);
    if (e->type == EXPR_AGG && !strcmp(e->func, "COUNT")) return "INT";
    if (e->type == EXPR_AGG && !strcmp(e->func, "AVG")) return "FLOAT";
    if (e->type == EXPR_AGG && strcmp(e->func, "SUM") && e->nargs == 1 && e->args[0]->type == EXPR_COLUMN &&
        e->args[0]->bound > 0 && e->args[0]->outer == 0)        // MIN/MAX di una colonna
        return datatype_name(p->src[(e->args[0]->bound - 1) / 1024].ts->columns[(e->args[0]->bound - 1) % 1024].type);
    bool any = false, all_int = true, all_num = true, small = true;
    for (int i = 0; rows && i < rows->nrows; i++) {
        const OVal* o = &orow(rows, i)[j];
        if (o->null) continue;
        any = true;
        if (!o->is_int) all_int = false;
        else if (o->inum < INT32_MIN || o->inum > INT32_MAX) small = false;
        if (!o->is_num) all_num = false;
    }
    if (!any) return for_schema ? "VARCHAR" : "";
    if (all_int) return (for_schema && !small) ? "BIGINT" : "INT";
    return all_num ? "FLOAT" : "VARCHAR";
}

// righe calcolate (vista, WITH, tabella derivata) -> tuple della sorgente con lo schema ts (che diventa suo)
static bool source_fill(Source* s, TableSchema* ts, const ORows* src_rows) {
    const ORows rows = *src_rows;
    bool ok = true;
    s->is_view = true;
    s->own_ts = s->ts = ts;
    int cap = 0;
    for (int i = 0; ok && i < rows.nrows; i++) {
        Tuple* t = tuple_create(ts->num_columns);
        if (!t) { ok = false; break; }
        for (int c = 0; c < ts->num_columns; c++) {
            const OVal* o = &orow(&rows, i)[c];
            Value* v = &t->values[c];
            if (o->null) { v->is_null = true; continue; }
            switch (ts->columns[c].type) {
                case SV_TYPE_INT:    v->int_val = (int32_t)(o->is_int ? o->inum : (int64_t)o->num); break;
                case SV_TYPE_BIGINT: v->bigint_val = o->is_int ? o->inum : (int64_t)o->num; break;
                case SV_TYPE_FLOAT:  v->float_val = o->num; break;
                case SV_TYPE_BOOL:   v->bool_val = (o->is_num ? o->num : 0) != 0; break;
                default: {
                    EvalVal ev;
                    ev_from_oval(&ev, o);
                    v->str_val = strdup(ev_s(&ev));
                    v->owns_str = v->str_val != NULL;
                    if (!v->str_val) ok = false;
                    break;
                }
            }
        }
        if (!source_push(s, t, (RID){0, 0}, &cap)) { tuple_free(t); ok = false; }
    }
    return ok;
}

// sorgente calcolata (vista, WITH, tabella derivata): esegue la query e ne copia le righe
static bool source_materialize(ExecCtx* x, Source* s, ASTNode* q, int depth, const CteScope* scope,
                               const char* what, char* err, size_t errn) {
    char perr[300] = "";
    SelectPlan* sp = plan_prepare(x, q, depth + 1, NULL, scope, perr, sizeof(perr));
    ORows rows = {0};
    if (!sp || !query_exec(sp, NULL, 0, &rows)) {
        snprintf(err, errn, "%s '%.64s': %.200s", what, s->table, sp ? sp->err : perr);
        orows_free(&rows);
        plan_free(sp);
        return false;
    }
    TableSchema* ts = calloc(1, sizeof(TableSchema));
    bool ok = ts != NULL;
    if (ok) {
        snprintf(ts->name, sizeof(ts->name), "%s", s->table);
        ts->num_columns = sp->nout;
        for (int i = 0; i < sp->nout; i++) {
            const char* cn = sp->out[i].name;
            const char* dot = strrchr(cn, '.');
            snprintf(ts->columns[i].name, sizeof(ts->columns[i].name), "%s", dot ? dot + 1 : cn);
            ts->columns[i].type = parse_type(out_type_name(sp, i, &rows, true));
            ts->columns[i].nullable = true;
        }
        ts->num_rows = (uint64_t)rows.nrows;
    }
    if (ok) ok = source_fill(s, ts, &rows); else free(ts);
    orows_free(&rows);
    plan_free(sp);
    if (!ok) { snprintf(err, errn, "Out of memory while computing '%.64s'", s->table); return false; }
    s->loaded = true;
    return true;
}

// ── WITH RECURSIVE ──

#define MAX_RECURSION_STEPS 1000
#define MAX_RECURSION_ROWS  1000000

typedef struct { ORows* rows; TableSchema* ts; } WorkTable;

static bool expr_refs_table(const ASTExpr* e, const char* name);

// la query nomina la tabella (anche in un join, una tabella derivata o una subquery)?
static bool ast_refs_table(const ASTNode* n, const char* name) {
    if (!n || n->type != NODE_SELECT) return false;
    #define q (&n->select_stmt)
    if (q->from_sub ? ast_refs_table(q->from_sub, name) : !strcasecmp(q->table_name, name)) return true;
    for (int j = 0; j < q->num_joins; j++) {
        if (q->joins[j].sub ? ast_refs_table(q->joins[j].sub, name) : !strcasecmp(q->joins[j].table_name, name)) return true;
        if (expr_refs_table(q->joins[j].condition, name)) return true;
    }
    for (int i = 0; i < q->num_cols; i++) if (expr_refs_table(q->columns[i], name)) return true;
    if (expr_refs_table(q->where, name) || expr_refs_table(q->having, name)) return true;
    bool r = ast_refs_table(q->set_next, name);
    #undef q
    return r;
}

// solo questo ramo di una UNION (non i successivi)
static bool arm_refs_table(ASTNode* n, const char* name) {
    ASTNode* next = n->select_stmt.set_next;
    n->select_stmt.set_next = NULL;
    bool r = ast_refs_table(n, name);
    n->select_stmt.set_next = next;
    return r;
}

static bool expr_refs_table(const ASTExpr* e, const char* name) {
    if (!e) return false;
    if (e->type == EXPR_SUBQUERY && e->subq && ast_refs_table(e->subq->node, name)) return true;
    if (expr_refs_table(e->left, name) || expr_refs_table(e->right, name)) return true;
    for (int i = 0; i < e->nargs; i++) if (expr_refs_table(e->args[i], name)) return true;
    return false;
}

static bool orows_append_copy(ORows* dst, const OVal* row) {
    OVal* r = orows_add(dst);
    if (!r) return false;
    for (int j = 0; j < dst->ncols; j++) {
        r[j] = row[j];
        if (row[j].str && !(r[j].str = strdup(row[j].str))) return false;
    }
    return true;
}

// esegue i rami [from, to) come SELECT indipendenti e ne accoda le righe
static bool run_arms(ExecCtx* x, ASTNode** arms, int from, int to, int depth, const CteScope* scope, ORows* out,
                     SelectPlan** first_plan, char* err, size_t errn) {
    for (int k = from; k < to; k++) {
        char perr[300] = "";
        SelectPlan* p = plan_prepare_ex(x, arms[k], depth + 1, NULL, scope, perr, sizeof(perr), true);
        if (!p) { snprintf(err, errn, "%.250s", perr); return false; }
        if (out->ncols && p->nout != out->ncols) {
            snprintf(err, errn, "Each SELECT of the recursive WITH must have %d column(s)", out->ncols);
            plan_free(p);
            return false;
        }
        ORows r = {0};
        r.ncols = p->nout;
        if (!arm_exec(p, NULL, 0, false, &r)) { snprintf(err, errn, "%.250s", p->err); orows_free(&r); plan_free(p); return false; }
        if (!out->ncols) out->ncols = r.ncols;
        bool ok = true;
        for (int i = 0; ok && i < r.nrows; i++) ok = rows_append(out, &r, i);
        orows_free(&r);
        if (first_plan && !*first_plan) *first_plan = p; else plan_free(p);
        if (!ok) { snprintf(err, errn, "Out of memory"); return false; }
    }
    return true;
}

// ancora UNION [ALL] passo ricorsivo: il passo viene ripetuto sulle righe dell'iterazione
// precedente finché non ne produce di nuove (con UNION le righe già viste non contano)
static int cte_recursive(ExecCtx* x, Source* s, ASTCte* c, const CteScope* sc, int i, int depth, char* err, size_t errn) {
    CteScope self = { sc->ctes, i + 1, sc->parent };
    ASTNode* arms[64];
    int na = 0;
    for (ASTNode* a = c->node; a && na < 64; a = a->select_stmt.set_next) arms[na++] = a;
    int nanchor = 0;
    while (nanchor < na && !arm_refs_table(arms[nanchor], c->name)) nanchor++;
    if (nanchor == na) return -1;                                  // non ricorsiva: calcolo normale
    if (nanchor == 0) { snprintf(err, errn, "WITH RECURSIVE '%.64s': the first SELECT (the anchor) must not use '%.64s'", c->name, c->name); return 0; }
    bool distinct = false;
    for (int k = 0; k + 1 < na; k++) {
        if (arms[k]->select_stmt.set_op != SETOP_UNION) { snprintf(err, errn, "WITH RECURSIVE '%.64s' must combine its SELECTs with UNION or UNION ALL", c->name); return 0; }
        if (!arms[k]->select_stmt.set_all) distinct = true;
    }
    ORows all = {0}, work = {0}, step = {0};
    SelectPlan* anchor_plan = NULL;
    StrMap seen = {0};
    TableSchema* ts = NULL;
    char* sig = NULL;
    bool ok = run_arms(x, arms, 0, nanchor, depth, &self, &step, &anchor_plan, err, errn);
    size_t sigcap = 0;
    if (ok) {
        ts = calloc(1, sizeof(TableSchema));
        sigcap = (size_t)(step.ncols ? step.ncols : 1) * (EV_STR_MAX + 24) + 1;
        sig = malloc(sigcap);
        if (!ts || !sig) { ok = false; snprintf(err, errn, "Out of memory"); }
    }
    if (ok && c->ncols && c->ncols != step.ncols) {
        snprintf(err, errn, "WITH '%.64s' names %d column(s) but its query returns %d", c->name, c->ncols, step.ncols);
        ok = false;
    }
    if (ok) {
        snprintf(ts->name, sizeof(ts->name), "%s", c->name);
        ts->num_columns = step.ncols;
        for (int j = 0; j < step.ncols; j++) {
            const char* cn = c->ncols ? c->cols[j] : anchor_plan->out[j].name;
            const char* dot = strrchr(cn, '.');
            snprintf(ts->columns[j].name, sizeof(ts->columns[j].name), "%s", dot ? dot + 1 : cn);
            DataType t = parse_type(out_type_name(anchor_plan, j, &step, true));
            if (t == SV_TYPE_INT && anchor_plan->out[j].src_type < 0) t = SV_TYPE_BIGINT;   // i calcoli possono crescere
            ts->columns[j].type = t;
            ts->columns[j].nullable = true;
        }
        all.ncols = work.ncols = step.ncols;
    }
    plan_free(anchor_plan);
    int iter = 0;
    while (ok) {
        // le righe del passo che non c'erano già diventano la nuova tabella di lavoro
        orows_free(&work);
        for (int r = 0; ok && r < step.nrows; r++) {
            if (distinct) {
                row_sig(orow(&step, r), step.ncols, sig, sigcap);
                SMSlot* sl = sm_slot(&seen, sig, true);
                if (!sl) { ok = false; snprintf(err, errn, "Out of memory"); break; }
                if (sl->val >= 0) continue;
                sl->val = 1;
            }
            ok = orows_append_copy(&all, orow(&step, r)) && orows_append_copy(&work, orow(&step, r));
            if (!ok) snprintf(err, errn, "Out of memory");
        }
        orows_free(&step);
        step.ncols = all.ncols;
        if (!ok || work.nrows == 0) break;
        if (++iter > MAX_RECURSION_STEPS || all.nrows > MAX_RECURSION_ROWS) {
            snprintf(err, errn, "WITH RECURSIVE '%.64s' does not stop (over %d steps or %d rows): check the condition that ends it",
                     c->name, MAX_RECURSION_STEPS, MAX_RECURSION_ROWS);
            ok = false;
            break;
        }
        if (atomic_load(&g_sv_scan_cancel)) { snprintf(err, errn, "Query cancelled"); ok = false; break; }
        WorkTable wt = { &work, ts };
        c->work = &wt;
        ok = run_arms(x, arms, nanchor, na, depth, &self, &step, NULL, err, errn);
        c->work = NULL;
    }
    orows_free(&work);
    orows_free(&step);
    sm_free(&seen);
    free(sig);
    if (ok) {
        ts->num_rows = (uint64_t)all.nrows;
        s->is_view = true;
        ok = source_fill(s, ts, &all);
        if (!ok) snprintf(err, errn, "Out of memory");
    } else {
        free(ts);
    }
    orows_free(&all);
    return ok ? 1 : 0;
}

// apre una sorgente: nome definito con WITH, tabella, vista o tabella derivata
static bool source_open(ExecCtx* x, Database* db, Source* s, const char* table, const char* alias, ASTNode* derived,
                        int depth, const CteScope* scope, char* err, size_t errn) {
    memset(s, 0, sizeof(*s));
    snprintf(s->table, sizeof(s->table), "%s", table);
    snprintf(s->name, sizeof(s->name), "%s", alias && alias[0] ? alias : table);
    if (depth >= MAX_VIEW_DEPTH) { snprintf(err, errn, "'%.64s' is nested too deeply (or refers to itself)", table); return false; }
    if (derived) return source_materialize(x, s, derived, depth, scope, "Subquery", err, errn);
    for (const CteScope* sc = scope; sc; sc = sc->parent)
        for (int i = sc->n - 1; i >= 0; i--)
            if (!strcasecmp(sc->ctes[i].name, table)) {
                ASTCte* c = &sc->ctes[i];
                if (c->work) {                                     // passo ricorsivo: righe dell'iterazione precedente
                    WorkTable* w = c->work;
                    TableSchema* ts = malloc(sizeof(TableSchema));
                    if (!ts) { snprintf(err, errn, "Out of memory"); return false; }
                    *ts = *w->ts;
                    s->is_view = true;
                    if (!source_fill(s, ts, w->rows)) { snprintf(err, errn, "Out of memory"); return false; }
                    s->loaded = true;
                    return true;
                }
                if (c->recursive) {
                    int r = cte_recursive(x, s, c, sc, i, depth, err, errn);
                    if (r >= 0) { if (r) s->loaded = true; return r == 1; }
                }
                CteScope before = { sc->ctes, i, sc->parent };   // un nome WITH vede solo quelli precedenti
                if (!source_materialize(x, s, c->node, depth, &before, "WITH query", err, errn)) return false;
                if (c->ncols) {                                    // WITH nome (colonne) AS ...
                    if (c->ncols != s->ts->num_columns) {
                        snprintf(err, errn, "WITH '%.64s' names %d column(s) but its query returns %d", c->name, c->ncols, s->ts->num_columns);
                        return false;
                    }
                    for (int k = 0; k < c->ncols; k++) snprintf(s->ts->columns[k].name, sizeof(s->ts->columns[k].name), "%s", c->cols[k]);
                }
                return true;
            }
    if (!db) { snprintf(err, errn, "No database selected"); return false; }
    s->ts = catalog_get_table(x->catalog, db->name, table);
    if (s->ts) return true;

    ViewSchema* view = catalog_get_view(x->catalog, db->name, table);
    if (!view) { snprintf(err, errn, "Table or view '%.64s' not found", table); return false; }
    TokenList* tk = lexer_tokenize(view->definition);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    if (!pr || !pr->success || pr->count != 1 || pr->statements[0]->type != NODE_SELECT) {
        snprintf(err, errn, "View '%.64s' has an invalid definition", table);
        parse_result_free(pr);
        lexer_free(tk);
        return false;
    }
    bool ok = source_materialize(x, s, pr->statements[0], depth, NULL, "View", err, errn);
    parse_result_free(pr);
    lexer_free(tk);
    return ok;
}

// ── analisi delle condizioni ──

static void collect_conj(ASTExpr* e, ExprVec* out) {
    if (!e) return;
    if (e->type == EXPR_BINARY && e->op == OP_AND) { collect_conj(e->left, out); collect_conj(e->right, out); return; }
    xv_push(out, e);
}

// sorgenti della query usate dall'espressione; outer: usa colonne esterne; opaque: subquery,
// alias o aggregati (la condizione resta dov'è)
static void expr_refs(const ASTExpr* e, unsigned* mask, bool* outer, bool* opaque) {
    if (!e) return;
    switch (e->type) {
        case EXPR_COLUMN:
            if (e->bound < 0) *opaque = true;
            else if (e->outer > 0) *outer = true;
            else if (e->bound > 0) *mask |= 1u << ((e->bound - 1) / 1024);
            return;
        case EXPR_SUBQUERY: case EXPR_AGG: case EXPR_WINDOW:
            *opaque = true;
            return;
        default:
            break;
    }
    expr_refs(e->left, mask, outer, opaque);
    expr_refs(e->right, mask, outer, opaque);
    for (int i = 0; i < e->nargs; i++) expr_refs(e->args[i], mask, outer, opaque);
}

// la sorgente s può ricevere righe NULL da un outer join?
static bool src_nullable(const SelectPlan* p, int s) {
    const ASTNode* n = p->n;
    if (s >= 1 && (n->select_stmt.joins[s - 1].type == JOIN_LEFT || n->select_stmt.joins[s - 1].type == JOIN_FULL)) return true;
    for (int j = s + 1; j < p->nsrc; j++)
        if (n->select_stmt.joins[j - 1].type == JOIN_RIGHT || n->select_stmt.joins[j - 1].type == JOIN_FULL) return true;
    return false;
}

static int popcount_u(unsigned m) { int c = 0; while (m) { c += (int)(m & 1u); m >>= 1; } return c; }
static int highest_bit(unsigned m) { int h = -1; for (int i = 0; m; i++, m >>= 1) if (m & 1u) h = i; return h; }

// "colonna di s = espressione senza colonne di questa query" -> ricerca per chiave
static void find_probe(SelectPlan* p, int s, ExprVec* conds, bool dyn) {
    if (p->probe_col[s] >= 0) return;
    for (int i = 0; i < conds->n; i++) {
        ASTExpr* w = conds->v[i];
        if (w->type != EXPR_BINARY || w->op != OP_EQ) continue;
        for (int side = 0; side < 2; side++) {
            ASTExpr* col = side ? w->right : w->left;
            ASTExpr* key = side ? w->left : w->right;
            if (col->type != EXPR_COLUMN || col->bound <= 0 || col->outer || (col->bound - 1) / 1024 != s) continue;
            unsigned m = 0; bool o = false, op = false;
            expr_refs(key, &m, &o, &op);
            if (m || op || o != dyn) continue;
            int ci = (col->bound - 1) % 1024;
            DataType t = p->src[s].ts->columns[ci].type;
            if (!is_int_type(t) && !is_text_type(t)) continue;
            p->probe_col[s] = ci;
            p->probe_key[s] = key;
            p->probe_dyn[s] = dyn;
            return;
        }
    }
}

// chiave di ricerca dal valore calcolato; false se il tipo non è confrontabile esattamente
static bool probe_text(const EvalVal* v, DataType col, char* buf, size_t n) {
    if (v->null) return false;
    if (is_int_type(col) && v->is_int) {
        if (col == SV_TYPE_INT && (v->inum < INT32_MIN || v->inum > INT32_MAX)) return false;
        snprintf(buf, n, "%lld", (long long)v->inum);
        return true;
    }
    if (is_text_type(col) && !v->is_num) { snprintf(buf, n, "%s", ev_s(v)); return true; }
    return false;
}

static SelectPlan* plan_prepare_ex(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                   char* err, size_t errn, bool arm);

static SelectPlan* plan_prepare(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                char* err, size_t errn) {
    return plan_prepare_ex(x, n, depth, outer, ctes, err, errn, false);
}

// arm = true: solo questo ramo di UNION / INTERSECT / EXCEPT (i successivi li prepara il primo)
static SelectPlan* plan_prepare_ex(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                   char* err, size_t errn, bool arm) {
    if (!n || n->type != NODE_SELECT) { snprintf(err, errn, "Expected a SELECT"); return NULL; }
    if (depth > MAX_VIEW_DEPTH * 4) { snprintf(err, errn, "Query is nested too deeply"); return NULL; }
    SelectPlan* p = calloc(1, sizeof(SelectPlan));
    if (!p) { snprintf(err, errn, "Out of memory"); return NULL; }
    p->x = x;
    p->n = n;
    p->depth = depth;
    p->scope.ctes = n->select_stmt.ctes;
    p->scope.n = n->select_stmt.num_ctes;
    p->scope.parent = ctes;
    for (int s = 0; s < MAX_SOURCES; s++) p->probe_col[s] = -1;
    Database* db = catalog_current_db(x->catalog);
    const CteScope* scope = p->scope.n ? &p->scope : ctes;
    p->compound = !arm && n->select_stmt.set_next != NULL;
    #define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); plan_free(p); return NULL; } while (0)

    // 1. sorgenti (SELECT senza FROM: nessuna, il risultato ha una riga)
    if (n->select_stmt.table_name[0]) {
        if (!source_open(x, db, &p->src[p->nsrc++], n->select_stmt.table_name, n->select_stmt.table_alias,
                         n->select_stmt.from_sub, depth, scope, err, errn)) { plan_free(p); return NULL; }
    }
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        ASTJoin* jn = &n->select_stmt.joins[j];
        if (!source_open(x, db, &p->src[p->nsrc++], jn->table_name, jn->alias, jn->sub, depth, scope, err, errn)) {
            plan_free(p); return NULL;
        }
    }
    for (int a = 0; a < p->nsrc; a++)
        for (int b = 0; b < a; b++)
            if (!strcasecmp(p->src[a].name, p->src[b].name))
                FAIL("Table '%.64s' is used twice: give each one an alias", p->src[a].name);

    // 1b. NATURAL JOIN / USING: uguaglianze sulle colonne comuni con le tabelle precedenti
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        ASTJoin* jn = &n->select_stmt.joins[j];
        if (!jn->natural && !jn->nusing) continue;
        int k = j + 1;
        Source* R = &p->src[k];
        char names[SV_MAX_COLUMNS][64];
        int nn = 0;
        if (jn->natural) {
            for (int c = 0; c < R->ts->num_columns && nn < SV_MAX_COLUMNS; c++)
                for (int q = 0; q < k; q++)
                    if (catalog_get_col_index(p->src[q].ts, R->ts->columns[c].name) >= 0) {
                        snprintf(names[nn++], 64, "%s", R->ts->columns[c].name);
                        break;
                    }
        } else {
            for (int u = 0; u < jn->nusing; u++) snprintf(names[nn++], 64, "%s", jn->using_cols[u]);
        }
        ASTExpr* cond = NULL;
        for (int u = 0; u < nn; u++) {
            int rc = catalog_get_col_index(R->ts, names[u]);
            if (rc < 0) FAIL("Column '%.64s' of USING is not in '%.64s'", names[u], R->name);
            int ls = -1, lc = -1;
            for (int q = 0; q < k; q++) {
                int c = catalog_get_col_index(p->src[q].ts, names[u]);
                if (c < 0) continue;
                if (ls >= 0) FAIL("Column '%.64s' of the join is in more than one table before it", names[u]);
                ls = q; lc = c;
            }
            if (ls < 0) FAIL("Column '%.64s' of USING is not in the tables before the JOIN", names[u]);
            p->using_left[k][rc] = ls * 1024 + lc + 1;
            if (!jn->using_built) {
                ASTExpr* a = calloc(1, sizeof(ASTExpr));
                ASTExpr* b2 = calloc(1, sizeof(ASTExpr));
                ASTExpr* eq = calloc(1, sizeof(ASTExpr));
                if (!a || !b2 || !eq) { free(a); free(b2); free(eq); ast_expr_free(cond); FAIL("Out of memory"); }
                a->type = b2->type = EXPR_COLUMN;
                snprintf(a->table_name, sizeof(a->table_name), "%s", p->src[ls].name);
                snprintf(a->col_name, sizeof(a->col_name), "%.63s", names[u]);
                snprintf(b2->table_name, sizeof(b2->table_name), "%s", R->name);
                snprintf(b2->col_name, sizeof(b2->col_name), "%.63s", names[u]);
                eq->type = EXPR_BINARY; eq->op = OP_EQ; eq->left = a; eq->right = b2; eq->depth = 1;
                if (!cond) cond = eq;
                else {
                    ASTExpr* and = calloc(1, sizeof(ASTExpr));
                    if (!and) { ast_expr_free(eq); ast_expr_free(cond); FAIL("Out of memory"); }
                    and->type = EXPR_BINARY; and->op = OP_AND; and->left = cond; and->right = eq;
                    and->depth = 1 + (cond->depth > 1 ? cond->depth : 1);
                    cond = and;
                }
            }
        }
        if (!jn->using_built) { jn->condition = cond; jn->using_built = true; }
    }

    // 2. colonne di output (* espanso; le colonne unite da NATURAL / USING compaiono una volta)
    for (int i = 0; i < n->select_stmt.num_cols; i++) {
        ASTExpr* e = n->select_stmt.columns[i];
        if (e->type == EXPR_STAR) {
            bool any = false;
            for (int s = 0; s < p->nsrc; s++) {
                Source* sr = &p->src[s];
                if (e->table_name[0] && strcasecmp(e->table_name, sr->name) && strcasecmp(e->table_name, sr->table)) continue;
                any = true;
                for (int c = 0; c < sr->ts->num_columns; c++) {
                    if (!e->table_name[0] && p->using_left[s][c]) continue;    // copia di destra di una colonna unita
                    if (p->nout >= SV_MAX_COLUMNS) FAIL("Too many columns in the result (max %d)", SV_MAX_COLUMNS);
                    ASTExpr* col = calloc(1, sizeof(ASTExpr));
                    if (!col) FAIL("Out of memory");
                    col->type = EXPR_COLUMN;
                    snprintf(col->col_name, sizeof(col->col_name), "%s", sr->ts->columns[c].name);
                    col->bound = s * 1024 + c + 1;
                    // colonna unita con un RIGHT / FULL JOIN successivo: COALESCE(sinistra, destra)
                    int rk = -1, rcol = -1;
                    for (int q = s + 1; !e->table_name[0] && q < p->nsrc && rk < 0; q++)
                        for (int cc = 0; cc < p->src[q].ts->num_columns; cc++)
                            if (p->using_left[q][cc] == s * 1024 + c + 1 &&
                                (n->select_stmt.joins[q - 1].type == JOIN_RIGHT || n->select_stmt.joins[q - 1].type == JOIN_FULL)) { rk = q; rcol = cc; break; }
                    if (rk >= 0) {
                        ASTExpr* other = calloc(1, sizeof(ASTExpr));
                        ASTExpr* co = calloc(1, sizeof(ASTExpr));
                        ASTExpr** args = calloc(2, sizeof(ASTExpr*));
                        if (!other || !co || !args) { free(other); free(co); free(args); free(col); FAIL("Out of memory"); }
                        *other = *col;
                        other->bound = rk * 1024 + rcol + 1;
                        co->type = EXPR_FUNC;
                        snprintf(co->func, sizeof(co->func), "COALESCE");
                        co->args = args; co->nargs = 2; args[0] = col; args[1] = other;
                        co->depth = 2;
                        snprintf(co->col_name, sizeof(co->col_name), "%s", col->col_name);
                        OutCol* oc = &p->out[p->nout++];
                        oc->e = co;
                        oc->owned = true;
                        oc->src_type = (int)sr->ts->columns[c].type;
                        snprintf(oc->name, sizeof(oc->name), "%s", col->col_name);
                        continue;
                    }
                    OutCol* oc = &p->out[p->nout++];
                    oc->e = col;
                    oc->owned = true;
                    oc->src_type = (int)sr->ts->columns[c].type;
                    if (p->nsrc > 1 && (e->table_name[0] || plan_joined_copy(p, s, c, NULL, NULL) < 0))
                        snprintf(oc->name, sizeof(oc->name), "%.64s.%.64s", sr->name, col->col_name);
                    else             snprintf(oc->name, sizeof(oc->name), "%s", col->col_name);
                }
            }
            if (!any && !p->nsrc) FAIL("SELECT * needs a FROM clause");
            if (!any) FAIL("Unknown table '%.64s' in SELECT", e->table_name);
            continue;
        }
        if (p->nout >= SV_MAX_COLUMNS) FAIL("Too many columns in the result (max %d)", SV_MAX_COLUMNS);
        p->out[p->nout].e = e;
        p->out[p->nout].owned = false;
        p->out[p->nout].src_type = -1;
        p->nout++;
    }

    // 3. risoluzione dei nomi
    for (int i = 0; i < p->nout; i++) p->sel[i] = p->out[i].e;
    bool has_agg = false;
    {
        BindCtx b;
        memset(&b, 0, sizeof(b));
        b.src = p->src;
        b.x = x;
        b.outer = outer;
        b.plan = p;
        b.depth = depth;
        b.ctes = scope;
        for (int j = 0; j < n->select_stmt.num_joins; j++) {          // ON vede solo le tabelle già unite
            b.nsrc = j + 2; b.mode = BIND_ROW; b.clause = "ON";
            bind_expr(n->select_stmt.joins[j].condition, &b);
        }
        b.nsrc = p->nsrc;
        b.mode = BIND_AGG_OK; b.clause = "SELECT";
        for (int i = 0; i < p->nout; i++) if (!p->out[i].owned) bind_expr(p->out[i].e, &b);
        has_agg = b.saw_agg;
        b.mode = BIND_ROW; b.clause = "WHERE";
        bind_expr(n->select_stmt.where, &b);
        b.sel = p->sel; b.nsel = p->nout;
        b.clause = "GROUP BY";
        for (int i = 0; i < n->select_stmt.num_group; i++) {
            ASTExpr* g = n->select_stmt.group_exprs[i];
            bind_expr(g, &b);
            if (!b.err[0] && g->type == EXPR_COLUMN && g->bound < 0 && expr_has_agg(p->sel[-g->bound - 1]))
                bind_error(&b, "Cannot GROUP BY an aggregate");
        }
        b.mode = BIND_AGG_OK; b.saw_agg = false; b.clause = "HAVING";
        bind_expr(n->select_stmt.having, &b);
        b.clause = "ORDER BY";
        for (int i = 0; !p->compound && i < n->select_stmt.num_order; i++) {
            ASTExpr* o = n->select_stmt.order_exprs[i];
            if (o->type == EXPR_INT) {                                  // ORDER BY 2 = seconda colonna
                if (o->int_val < 1 || o->int_val > p->nout) { bind_error(&b, "ORDER BY position is out of range"); break; }
                continue;
            }
            // un nome che è alias di una colonna di output vale l'alias (come MySQL)
            if (o->type == EXPR_COLUMN && !o->table_name[0]) {
                int k;
                for (k = 0; k < p->nout; k++) if (p->sel[k]->alias[0] && !strcasecmp(p->sel[k]->alias, o->col_name)) break;
                if (k < p->nout) { o->bound = -(k + 1); continue; }
            }
            bind_expr(o, &b);
        }
        if (b.saw_agg) has_agg = true;
        if (b.err[0]) FAIL("%s", b.err);
    }
    for (int i = 0; i < p->nout; i++) {
        OutCol* oc = &p->out[i];
        if (oc->owned) continue;
        ASTExpr* e = oc->e;
        if (e->type == EXPR_COLUMN && e->bound > 0 && e->outer == 0) {
            int s = (e->bound - 1) / 1024, c = (e->bound - 1) % 1024;
            oc->src_type = (int)p->src[s].ts->columns[c].type;
        }
        if (e->alias[0])                                                   snprintf(oc->name, sizeof(oc->name), "%s", e->alias);
        else if (e->type == EXPR_COLUMN && p->nsrc > 1 && e->outer == 0 && e->bound > 0)
            snprintf(oc->name, sizeof(oc->name), "%.64s.%.64s", p->src[(e->bound - 1) / 1024].name, e->col_name);
        else if (e->type == EXPR_COLUMN || (e->type == EXPR_FUNC && e->col_name[0]))   // (colonna di USING in un FULL JOIN)
            snprintf(oc->name, sizeof(oc->name), "%s", e->col_name);
        else                                                               ast_expr_format(e, oc->name, sizeof(oc->name));
    }
    p->grouped = has_agg || n->select_stmt.num_group > 0 || n->select_stmt.having;

    // 4. dove applicare le condizioni
    ExprVec conj = {0};
    collect_conj(n->select_stmt.where, &conj);
    for (int i = 0; i < conj.n; i++) {
        ASTExpr* c = conj.v[i];
        unsigned m = 0; bool o = false, op = false;
        expr_refs(c, &m, &o, &op);
        bool safe = !op && m != 0;
        for (int s = 0; safe && s < p->nsrc; s++) if ((m >> s & 1u) && src_nullable(p, s)) safe = false;
        if (safe && popcount_u(m) == 1) {
            int s = highest_bit(m);
            xv_push(o ? &p->pre_dyn[s] : &p->pre_static[s], c);
            continue;
        }
        int k = highest_bit(m);
        if (safe && k >= 1 && (n->select_stmt.joins[k - 1].type == JOIN_INNER || n->select_stmt.joins[k - 1].type == JOIN_CROSS)) {
            xv_push(&p->jcond[k], c);
            continue;
        }
        xv_push(&p->resid, c);
    }
    free(conj.v);
    for (int k = 1; k < p->nsrc; k++) {
        ASTJoin* j = &n->select_stmt.joins[k - 1];
        if (j->type == JOIN_RIGHT || j->type == JOIN_FULL) { if (j->condition) xv_push(&p->jcond[k], j->condition); continue; }
        ExprVec on = {0};
        collect_conj(j->condition, &on);
        for (int i = 0; i < on.n; i++) {
            ASTExpr* c = on.v[i];
            unsigned m = 0; bool o = false, op = false;
            expr_refs(c, &m, &o, &op);
            // condizione dell'ON solo sulla tabella unita: filtra le sue righe prima del join
            bool pre = !op && m == (1u << k) && (j->type == JOIN_LEFT || !src_nullable(p, k));
            xv_push(pre ? (o ? &p->pre_dyn[k] : &p->pre_static[k]) : &p->jcond[k], c);
        }
        free(on.v);
    }
    for (int s = 0; s < p->nsrc; s++) {
        if (p->src[s].is_view) continue;
        find_probe(p, s, &p->pre_dyn[s], true);
        find_probe(p, s, &p->pre_static[s], false);
    }

    // 5. rami di UNION / INTERSECT / EXCEPT
    if (p->compound) {
        SelectPlan* tail = p;
        for (ASTNode* an = n->select_stmt.set_next; an; an = an->select_stmt.set_next) {
            SelectPlan* ap = plan_prepare_ex(x, an, depth, outer, scope, err, errn, true);
            if (!ap) { plan_free(p); return NULL; }
            tail->next_arm = ap;
            tail = ap;
            if (ap->nout != p->nout)
                FAIL("Each SELECT of %s must have the same number of columns (%d and %d)",
                     n->select_stmt.set_op == SETOP_UNION ? "a UNION" : n->select_stmt.set_op == SETOP_INTERSECT ? "an INTERSECT" : "an EXCEPT",
                     p->nout, ap->nout);
            if (ap->correlated) p->correlated = true;
        }
        for (SelectPlan* a = p->next_arm; a; a = a->next_arm) if (a->correlated) plan_mark_correlated(p);
        // ORDER BY del risultato: posizione o nome di una colonna del risultato
        for (int i = 0; i < n->select_stmt.num_order; i++) {
            ASTExpr* o = n->select_stmt.order_exprs[i];
            int k = -1;
            if (o->type == EXPR_INT && o->int_val >= 1 && o->int_val <= p->nout) k = (int)o->int_val - 1;
            else if (o->type == EXPR_COLUMN)
                for (int j = 0; j < p->nout && k < 0; j++) {
                    const char* nm = p->out[j].name;
                    const char* dot = strrchr(nm, '.');
                    if (!strcasecmp(nm, o->col_name) || (dot && !strcasecmp(dot + 1, o->col_name))) k = j;
                }
            if (k < 0) FAIL("ORDER BY of a UNION / INTERSECT / EXCEPT must name a result column or give its position");
            p->corder[i] = k;
        }
    }
    #undef FAIL
    return p;
}

// ── lettura delle sorgenti ──

static bool plan_load(SelectPlan* p) {
    if (p->loaded) return true;
    p->loaded = true;
    Database* db = catalog_current_db(p->x->catalog);
    Tuple* cur[MAX_SOURCES] = {0};
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.src = p->src; ec.nsrc = p->nsrc; ec.cur = cur; ec.x = p->x;
    for (int s = 0; s < p->nsrc; s++) {
        Source* S = &p->src[s];
        ExprVec* f = &p->pre_static[s];
        if (S->loaded) {                                       // vista / WITH / derivata: filtro sulle righe
            if (f->n) {
                int w = 0;
                for (int i = 0; i < S->n; i++) {
                    cur[s] = S->rows[i];
                    if (xv_all_true(f, &ec)) { S->rows[w] = S->rows[i]; S->rids[w] = S->rids[i]; w++; }
                    else tuple_free(S->rows[i]);
                }
                S->n = w;
                cur[s] = NULL;
            }
        } else {
            S->loaded = true;
            HeapFile* hf = heap_open(p->x->bp, S->ts->root_page_id, S->ts->num_pages);
            if (!hf) { snprintf(p->err, sizeof(p->err), "Failed to read table '%.64s'", S->table); return false; }
            int cap = 0;
            bool done = false;
            // accesso con indice: colonna indicizzata = costante
            if (p->probe_col[s] >= 0 && !p->probe_dyn[s] && db && col_is_indexed(S->ts, p->probe_col[s])) {
                EvalVal kv;
                eval_expr(p->probe_key[s], &ec, &kv);
                char key[EV_STR_MAX + 1];
                MemIndex* ix = mi_get(p->x, db->name, S->ts, p->probe_col[s]);
                if (ix) {
                    done = true;
                    p->used_index[s] = true;
                    const RidList* l = probe_text(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key)) ? mi_lookup(ix, key) : NULL;
                    for (int i = 0; l && i < l->n; i++) {
                        Tuple* t = heap_get(hf, l->r[i], S->ts);
                        if (!t) continue;
                        p->st_scanned[s]++;
                        cur[s] = t;
                        if (!xv_all_true(f, &ec)) { tuple_free(t); continue; }
                        if (!source_push(S, t, l->r[i], &cap)) { tuple_free(t); free(hf); snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
                    }
                }
            }
            if (!done) {
                // scansione: le righe vengono lette senza allocare e copiate solo se passano i filtri
                HeapScan* scan = heap_scan_open(hf, S->ts);
                Tuple* view = tuple_create(S->ts->num_columns);
                char* sbuf = malloc(SV_PAGE_SIZE + 64);
                bool ok = scan && view && sbuf;
                RID rid;
                uint16_t len;
                const uint8_t* data;
                while (ok && (data = heap_scan_next_raw(scan, &rid, &len)) != NULL) {
                    if (!tuple_view(view, data, len, S->ts, sbuf, SV_PAGE_SIZE + 64)) continue;
                    p->st_scanned[s]++;
                    if (f->n) {
                        cur[s] = view;
                        bool keep = xv_all_true(f, &ec);
                        cur[s] = NULL;
                        if (!keep) continue;
                    }
                    // copia nell'arena della sorgente: nessuna malloc per riga
                    Tuple* t = arena_alloc(&S->arena, sizeof(Tuple));
                    Value* vals = t ? arena_alloc(&S->arena, sizeof(Value) * (size_t)view->num_values) : NULL;
                    if (!vals) { ok = false; break; }
                    S->arena_rows = true;
                    t->values = vals;
                    t->num_values = view->num_values;
                    for (int c = 0; c < view->num_values; c++) {
                        vals[c] = view->values[c];
                        vals[c].owns_str = false;
                        if (!vals[c].is_null && view->values[c].str_val && !is_num_storage(S->ts->columns[c].type)) {
                            size_t l = strlen(view->values[c].str_val) + 1;
                            char* cp = arena_alloc(&S->arena, l);
                            if (!cp) { ok = false; break; }
                            memcpy(cp, view->values[c].str_val, l);
                            vals[c].str_val = cp;
                        }
                    }
                    if (!ok) break;
                    t->rid = rid;
                    if (!source_push(S, t, rid, &cap)) ok = false;
                }
                if (view) { for (int c = 0; c < view->num_values; c++) view->values[c].owns_str = false; tuple_free(view); }
                free(sbuf);
                heap_scan_close(scan);
                if (!ok) { free(hf); snprintf(p->err, sizeof(p->err), "Out of memory while reading '%.64s'", S->table); return false; }
            }
            free(hf);
            cur[s] = NULL;
        }
        if (ec.err[0]) { snprintf(p->err, sizeof(p->err), "%s", ec.err); return false; }
        if (S->is_view && !p->st_scanned[s]) p->st_scanned[s] = S->n;
        p->st_loaded[s] = S->n;
        // subquery correlata: tabella hash sulla colonna di ricerca, costruita una volta
        if (p->probe_col[s] >= 0 && p->probe_dyn[s]) {
            int col = p->probe_col[s];
            DataType kt = S->ts->columns[col].type;
            p->pnext[s] = malloc(sizeof(int) * (size_t)(S->n ? S->n : 1));
            if (!p->pnext[s]) { snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
            for (int r = S->n - 1; r >= 0; r--) {
                char key[EV_STR_MAX + 1];
                p->pnext[s][r] = -1;
                if (!join_key(S->rows[r], col, kt, key, sizeof(key))) continue;
                SMSlot* sl = sm_slot(&p->pmap[s], key, true);
                if (!sl) { snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
                p->pnext[s][r] = sl->val;
                sl->val = r;
            }
        }
    }
    if (atomic_load(&g_sv_scan_cancel)) { snprintf(p->err, sizeof(p->err), "Query cancelled"); return false; }
    return true;
}

// ── combinazioni di righe (una tupla per sorgente), in un unico array ──

typedef struct { Tuple** v; int n, cap, w; } Combos;

#define COMBO(c, i) ((c)->v + (size_t)(i) * (size_t)(c)->w)

static void combos_free(Combos* c) { free(c->v); c->v = NULL; c->n = c->cap = 0; }

static bool combos_push(Combos* c, Tuple** base, int k, Tuple* row) {
    if (c->n >= MAX_JOIN_ROWS) return false;
    if (c->n >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 256;
        Tuple** nv = realloc(c->v, sizeof(Tuple*) * (size_t)nc * (size_t)c->w);
        if (!nv) return false;
        c->v = nv;
        c->cap = nc;
    }
    Tuple** t = COMBO(c, c->n);
    if (base) memcpy(t, base, sizeof(Tuple*) * (size_t)c->w);
    else memset(t, 0, sizeof(Tuple*) * (size_t)c->w);
    if (k >= 0) t[k] = row;
    c->n++;
    return true;
}

// "colonna della sorgente k = colonna di una sorgente precedente", con tipi confrontabili per testo
static bool find_hash_cond(ExprVec* conds, int k, Source* src, int* nk_col, int* other_src, int* other_col) {
    for (int i = 0; i < conds->n; i++) {
        ASTExpr* on = conds->v[i];
        if (on->type != EXPR_BINARY || on->op != OP_EQ || on->left->type != EXPR_COLUMN || on->right->type != EXPR_COLUMN) continue;
        if (on->left->bound <= 0 || on->right->bound <= 0 || on->left->outer || on->right->outer) continue;
        int ls = (on->left->bound - 1) / 1024, lc = (on->left->bound - 1) % 1024;
        int rs = (on->right->bound - 1) / 1024, rc = (on->right->bound - 1) % 1024;
        if (rs == k && ls < k) { int ts = ls, tc = lc; ls = rs; lc = rc; rs = ts; rc = tc; }
        if (ls != k || rs >= k) continue;
        DataType a = src[ls].ts->columns[lc].type, b = src[rs].ts->columns[rc].type;
        bool same_class = (is_int_type(a) && is_int_type(b)) || (is_text_type(a) && is_text_type(b));
        if (!same_class) continue;
        *nk_col = lc; *other_src = rs; *other_col = rc;
        return true;
    }
    return false;
}

// unisce le righe rows (della sorgente k) alle combinazioni correnti
static const char* join_step(EvalCtx* ec, Combos* in, Combos* out, int k, JoinType jt, ExprVec* conds,
                             Tuple** rows, int nrows) {
    bool keep_right = jt == JOIN_RIGHT || jt == JOIN_FULL, keep_left = jt == JOIN_LEFT || jt == JOIN_FULL;
    bool* right_hit = (keep_right && nrows) ? calloc((size_t)nrows, sizeof(bool)) : NULL;
    if (keep_right && nrows && !right_hit) return "Out of memory";

    // hash join quando le condizioni contengono un'uguaglianza fra colonne confrontabili: tabella
    // hash -> prima riga, righe con lo stesso hash in catena (l'uguaglianza la verificano le condizioni)
    int kc, os, oc;
    HMap map = {0};
    int* next = NULL;
    bool hashed = find_hash_cond(conds, k, ec->src, &kc, &os, &oc);
    if (hashed) {
        next = malloc(sizeof(int) * (size_t)(nrows ? nrows : 1));
        if (!next) { free(right_hit); return "Out of memory"; }
        DataType kt = ec->src[k].ts->columns[kc].type;
        for (int r = nrows - 1; r >= 0; r--) {          // in ordine: la catena parte dalla prima riga
            uint64_t h;
            next[r] = -1;
            if (!col_hash(rows[r], kc, kt, &h)) continue;
            size_t j = map.cap ? h & (map.cap - 1) : 0;
            while (map.cap && map.v[j] >= 0 && map.h[j] != h) j = (j + 1) & (map.cap - 1);
            if (map.cap && map.v[j] >= 0) { next[r] = map.v[j]; map.v[j] = r; continue; }
            if (!hmap_put(&map, h, r)) { hmap_free(&map); free(next); free(right_hit); return "Out of memory"; }
        }
    }
    const char* fail = NULL;
    DataType ot = hashed ? ec->src[os].ts->columns[oc].type : SV_TYPE_INT;
    for (int i = 0; i < in->n && !fail; i++) {
        if ((i & 1023) == 0 && atomic_load(&g_sv_scan_cancel)) { fail = "Query cancelled"; break; }
        Tuple** base = COMBO(in, i);
        ec->cur = base;
        bool matched = false;
        int r = 0;
        if (hashed) {
            uint64_t h;
            r = -1;
            if (col_hash(base[os], oc, ot, &h) && map.cap) {
                size_t j = h & (map.cap - 1);
                while (map.v[j] >= 0 && map.h[j] != h) j = (j + 1) & (map.cap - 1);
                r = map.v[j];
            }
        }
        for (; hashed ? r >= 0 : r < nrows; r = hashed ? next[r] : r + 1) {
            base[k] = rows[r];
            bool ok = xv_all_true(conds, ec);
            base[k] = NULL;
            if (!ok) continue;
            matched = true;
            if (right_hit) right_hit[r] = true;
            if (!combos_push(out, base, k, rows[r])) { fail = out->n >= MAX_JOIN_ROWS ? "The JOIN produces too many rows (over 10 million)" : "Out of memory"; break; }
            base = COMBO(in, i);
        }
        if (!fail && !matched && keep_left && !combos_push(out, base, k, NULL)) fail = "Out of memory";
    }
    if (!fail && right_hit)
        for (int r = 0; r < nrows && !fail; r++)
            if (!right_hit[r] && !combos_push(out, NULL, k, rows[r])) fail = "Out of memory";
    hmap_free(&map);
    free(next);
    free(right_hit);
    ec->cur = NULL;
    return fail;
}

// ── ordinamento ──

typedef struct { SortKey* keys; int nkeys; const bool* desc; } SortSpec;
static SortSpec g_sort;

static int unit_cmp(const void* pa, const void* pb) {
    int ia = *(const int*)pa, ib = *(const int*)pb;
    const SortKey* a = &g_sort.keys[(size_t)ia * (size_t)g_sort.nkeys];
    const SortKey* b = &g_sort.keys[(size_t)ib * (size_t)g_sort.nkeys];
    for (int k = 0; k < g_sort.nkeys; k++) {
        int c = key_cmp(&a[k], &b[k]);
        if (c) return g_sort.desc[k] ? -c : c;
    }
    return ia < ib ? -1 : ia > ib;   // stabile
}

// ── esecuzione di un ramo ──

typedef struct {
    Combos     combos;
    Tuple**    frows[MAX_SOURCES];
    bool       fown[MAX_SOURCES];
    int        fn[MAX_SOURCES];
    Tuple***   groups_buf;
    int*       group_start;
    int*       group_size;
    int*       units;
    SortKey*   keys;
    int        nkeys_alloc;
    Tuple**    null_combo;
    SortKey*   winv[16];      // valori delle funzioni finestra, per unità
    int        nwinv;          // unità per funzione
} ArmState;

static void arm_state_free(ArmState* st, int nsrc) {
    combos_free(&st->combos);
    for (int s = 0; s < nsrc; s++) if (st->fown[s]) free(st->frows[s]);
    free(st->groups_buf); free(st->group_start); free(st->group_size); free(st->units);
    for (int i = 0; i < st->nkeys_alloc; i++) free(st->keys[i].str);
    free(st->keys);
    free(st->null_combo);
    for (int w = 0; w < 16; w++) {
        if (!st->winv[w]) continue;
        for (int i = 0; i < st->nwinv; i++) free(st->winv[w][i].str);
        free(st->winv[w]);
    }
}

static void arm_set_unit(SelectPlan* p, ArmState* st, EvalCtx* ec, int u) {
    if (p->grouped) {
        ec->grp  = st->group_size[u] ? &st->groups_buf[st->group_start[u]] : &st->null_combo;
        ec->ngrp = st->group_size[u];
        ec->cur  = st->group_size[u] ? st->groups_buf[st->group_start[u]] : st->null_combo;
    } else {
        ec->cur = COMBO(&st->combos, u);
        ec->grp = NULL;
        ec->ngrp = 0;
    }
    ec->win_row = u;
}

// ── funzioni finestra ──
// Le unità vengono ordinate per (PARTITION BY, ORDER BY). Con ORDER BY il frame va dall'inizio
// della partizione alla riga corrente compresi i suoi pari (righe con le stesse chiavi), come il
// RANGE predefinito di SQL; senza ORDER BY è l'intera partizione.

static struct { const SortKey* keys; int K, npart; const bool* desc; } g_wsort;

static int wsort_cmp(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    const SortKey* x = &g_wsort.keys[(size_t)ia * (size_t)g_wsort.K];
    const SortKey* y = &g_wsort.keys[(size_t)ib * (size_t)g_wsort.K];
    for (int k = 0; k < g_wsort.K; k++) {
        int c = key_cmp(&x[k], &y[k]);
        if (c) return (k >= g_wsort.npart && g_wsort.desc[k - g_wsort.npart]) ? -c : c;
    }
    return ia < ib ? -1 : ia > ib;
}

static bool keys_equal(const SortKey* x, const SortKey* y, int from, int to) {
    for (int k = from; k < to; k++) if (key_cmp(&x[k], &y[k])) return false;
    return true;
}

static void oval_copy(SortKey* dst, const SortKey* src) {
    free(dst->str);
    *dst = *src;
    dst->str = src->str ? strdup(src->str) : NULL;
}

typedef struct { int64_t count, isum; double dsum; bool all_int, overflow; const SortKey* best; } WinAcc;

static void wacc_add(WinAcc* a, const char* f, const SortKey* v, bool star) {
    if (star) { a->count++; return; }
    if (v->null) return;
    a->count++;
    if (!strcmp(f, "SUM") || !strcmp(f, "AVG")) {
        a->dsum += v->is_num ? v->num : 0;
        if (v->is_int && !a->overflow) { if (__builtin_add_overflow(a->isum, v->inum, &a->isum)) a->overflow = true; }
        else a->all_int = false;
    } else if (!strcmp(f, "MIN") || !strcmp(f, "MAX")) {
        int c = a->best ? key_cmp(v, a->best) : 0;
        if (!a->best || (f[1] == 'I' ? c < 0 : c > 0)) a->best = v;
    }
}

static void wacc_result(const WinAcc* a, const char* f, SortKey* out) {
    free(out->str);
    memset(out, 0, sizeof(*out));
    if (!strcmp(f, "COUNT")) { out->is_num = out->is_int = true; out->inum = a->count; out->num = (double)a->count; return; }
    if (a->count == 0) { out->null = true; return; }
    if (!strcmp(f, "SUM") && a->all_int && !a->overflow) { out->is_num = out->is_int = true; out->inum = a->isum; out->num = (double)a->isum; return; }
    if (!strcmp(f, "SUM")) { out->is_num = true; out->num = a->dsum; return; }
    if (!strcmp(f, "AVG")) { out->is_num = true; out->num = a->dsum / (double)a->count; return; }
    if (a->best) oval_copy(out, a->best);
    else out->null = true;
}

static bool window_compute(SelectPlan* p, ArmState* st, EvalCtx* ec, ASTExpr* w, const int* units, int n, SortKey* vals) {
    WindowSpec* ws = w->win;
    const char* f = w->func;
    int K = ws->npart + ws->nord;
    SortKey* keys = calloc((size_t)n * (size_t)(K ? K : 1) + 1, sizeof(SortKey));
    SortKey* argv = calloc((size_t)n * 3 + 1, sizeof(SortKey));      // argomenti 1, 2, 3 di ogni riga
    int* idx = malloc(sizeof(int) * (size_t)(n ? n : 1));
    bool ok = keys && argv && idx;
    for (int i = 0; ok && i < n; i++) {
        arm_set_unit(p, st, ec, units[i]);
        for (int k = 0; k < K; k++) {
            EvalVal v;
            eval_expr(k < ws->npart ? ws->part[k] : ws->ord[k - ws->npart], ec, &v);
            key_set(&keys[(size_t)i * (size_t)K + (size_t)k], &v);
        }
        for (int a = 0; a < w->nargs && a < 3; a++) {
            EvalVal v;
            eval_expr(w->args[a], ec, &v);
            key_set(&argv[(size_t)i * 3 + (size_t)a], &v);
        }
        idx[i] = i;
    }
    if (ok && ec->err[0]) ok = false;
    if (ok && K) {
        g_wsort.keys = keys; g_wsort.K = K; g_wsort.npart = ws->npart; g_wsort.desc = ws->desc;
        qsort(idx, (size_t)n, sizeof(int), wsort_cmp);
    }
    #define KEY(r) (&keys[(size_t)idx[r] * (size_t)K])
    #define ARG(r, a) (&argv[(size_t)idx[r] * 3 + (a)])
    #define OUT(r) (&vals[units[idx[r]]])
    bool star = !strcmp(f, "COUNT") && w->nargs == 0;
    bool aggregate = !strcmp(f, "COUNT") || !strcmp(f, "SUM") || !strcmp(f, "AVG") || !strcmp(f, "MIN") || !strcmp(f, "MAX");
    for (int start = 0; ok && start < n; ) {
        int end = start + 1;
        while (end < n && keys_equal(KEY(start), KEY(end), 0, ws->npart)) end++;
        int dense = 0;
        WinAcc acc = { 0, 0, 0, true, false, NULL };
        if (aggregate && ws->nord == 0) {                     // tutta la partizione
            for (int r = start; r < end; r++) wacc_add(&acc, f, ARG(r, 0), star);
            for (int r = start; r < end; r++) wacc_result(&acc, f, OUT(r));
        }
        for (int g = start; g < end; ) {                      // gruppi di pari
            int ge = g + 1;
            while (ge < end && keys_equal(KEY(g), KEY(ge), ws->npart, K)) ge++;
            dense++;
            if (aggregate && ws->nord) {
                for (int r = g; r < ge; r++) wacc_add(&acc, f, ARG(r, 0), star);
                for (int r = g; r < ge; r++) wacc_result(&acc, f, OUT(r));
            }
            for (int r = g; r < ge && !aggregate; r++) {
                SortKey* o = OUT(r);
                free(o->str);
                memset(o, 0, sizeof(*o));
                int64_t iv = 0;
                bool is_int = true;
                if (!strcmp(f, "ROW_NUMBER")) iv = r - start + 1;
                else if (!strcmp(f, "RANK")) iv = g - start + 1;
                else if (!strcmp(f, "DENSE_RANK")) iv = dense;
                else if (!strcmp(f, "NTILE")) {
                    const SortKey* b = ARG(start, 0);
                    int64_t buckets = b->is_num ? (int64_t)b->num : 0;
                    if (buckets < 1) { snprintf(ec->err, sizeof(ec->err), "NTILE needs a positive number of groups"); ok = false; break; }
                    int64_t cnt = end - start, size = cnt / buckets, extra = cnt % buckets, pos = r - start;
                    iv = pos < extra * (size + 1) ? pos / (size + 1) + 1 : extra + (size ? (pos - extra * (size + 1)) / size : 0) + 1;
                } else {
                    is_int = false;
                    if (!strcmp(f, "LAG") || !strcmp(f, "LEAD")) {
                        const SortKey* ov = ARG(r, 1);
                        int64_t off = w->nargs > 1 ? (ov->is_num ? (int64_t)ov->num : 0) : 1;
                        int64_t t = !strcmp(f, "LAG") ? r - off : r + off;
                        if (t >= start && t < end) oval_copy(o, ARG(t, 0));
                        else if (w->nargs > 2) oval_copy(o, ARG(r, 2));
                        else o->null = true;
                    } else if (!strcmp(f, "FIRST_VALUE")) oval_copy(o, ARG(start, 0));
                    else oval_copy(o, ARG(ws->nord ? ge - 1 : end - 1, 0));      // LAST_VALUE
                }
                if (is_int) { o->is_num = o->is_int = true; o->inum = iv; o->num = (double)iv; }
            }
            g = ge;
        }
        start = end;
    }
    #undef KEY
    #undef ARG
    #undef OUT
    for (size_t i = 0; keys && i < (size_t)n * (size_t)K; i++) free(keys[i].str);
    for (size_t i = 0; argv && i < (size_t)n * 3; i++) free(argv[i].str);
    free(keys);
    free(argv);
    free(idx);
    return ok;
}

static bool arm_exec(SelectPlan* p, EvalCtx* outer, int stop_after, bool apply_order, ORows* res) {
    ASTNode* n = p->n;
    ArmState st;
    memset(&st, 0, sizeof(st));
    p->err[0] = '\0';
    p->execs++;
    res->ncols = p->nout;
    if (!plan_load(p)) return false;

    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.src = p->src; ec.nsrc = p->nsrc; ec.sel = p->sel; ec.nsel = p->nout;
    ec.x = p->x; ec.outer = outer;
    int w = p->nsrc ? p->nsrc : 1;
    st.combos.w = w;
    Tuple* cur[MAX_SOURCES] = {0};
    #define AFAIL(...) do { snprintf(p->err, sizeof(p->err), __VA_ARGS__); arm_state_free(&st, p->nsrc); return false; } while (0)

    // 1. righe di ogni sorgente che passano i filtri dipendenti dalla query esterna
    for (int s = 0; s < p->nsrc; s++) {
        Source* S = &p->src[s];
        bool dyn = p->pre_dyn[s].n > 0;
        if (!dyn) { st.frows[s] = S->rows; st.fn[s] = S->n; continue; }
        st.frows[s] = malloc(sizeof(Tuple*) * (size_t)(S->n ? S->n : 1));
        if (!st.frows[s]) AFAIL("Out of memory");
        st.fown[s] = true;
        ec.cur = cur;
        if (p->probe_col[s] >= 0 && p->probe_dyn[s]) {
            EvalVal kv;
            eval_expr(p->probe_key[s], &ec, &kv);
            char key[EV_STR_MAX + 1];
            SMSlot* sl = probe_text(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key)) ? sm_slot(&p->pmap[s], key, false) : NULL;
            if (!probe_text(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key)) && !kv.null) {
                // tipo non confrontabile per chiave: si provano tutte le righe
                for (int r = 0; r < S->n; r++) { cur[s] = S->rows[r]; if (xv_all_true(&p->pre_dyn[s], &ec)) st.frows[s][st.fn[s]++] = S->rows[r]; }
            } else {
                for (int r = sl ? sl->val : -1; r >= 0; r = p->pnext[s][r]) {
                    cur[s] = S->rows[r];
                    if (xv_all_true(&p->pre_dyn[s], &ec)) st.frows[s][st.fn[s]++] = S->rows[r];
                }
            }
        } else {
            for (int r = 0; r < S->n; r++) { cur[s] = S->rows[r]; if (xv_all_true(&p->pre_dyn[s], &ec)) st.frows[s][st.fn[s]++] = S->rows[r]; }
        }
        cur[s] = NULL;
        ec.cur = NULL;
        p->st_dyn[s] += st.fn[s];
        if (ec.err[0]) AFAIL("%s", ec.err);
    }

    // 2. combinazioni (JOIN) e resto della WHERE
    if (p->nsrc == 0) {
        if (!combos_push(&st.combos, NULL, -1, NULL)) AFAIL("Out of memory");
    }
    for (int i = 0; p->nsrc > 0 && i < st.fn[0]; i++)
        if (!combos_push(&st.combos, NULL, 0, st.frows[0][i])) AFAIL("%s", st.combos.n >= MAX_JOIN_ROWS ? "Too many rows" : "Out of memory");
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        Combos next = { NULL, 0, 0, w };
        const char* f = join_step(&ec, &st.combos, &next, j + 1, n->select_stmt.joins[j].type, &p->jcond[j + 1],
                                  st.frows[j + 1], st.fn[j + 1]);
        combos_free(&st.combos);
        st.combos = next;
        p->st_join[j + 1] += next.n;
        if (f) AFAIL("%s", f);
        if (ec.err[0]) AFAIL("%s", ec.err);
    }
    bool quick = stop_after > 0 && !p->grouped && !n->select_stmt.distinct && (!apply_order || !n->select_stmt.num_order) &&
                 (!apply_order || !n->select_stmt.has_limit);
    if (p->resid.n || quick) {
        int kept = 0;
        for (int i = 0; i < st.combos.n; i++) {
            ec.cur = COMBO(&st.combos, i);
            if (p->resid.n && !xv_all_true(&p->resid, &ec)) continue;
            if (kept != i) memcpy(COMBO(&st.combos, kept), COMBO(&st.combos, i), sizeof(Tuple*) * (size_t)w);
            kept++;
            if (quick && kept >= stop_after) break;                 // EXISTS: basta una riga
        }
        st.combos.n = kept;
        ec.cur = NULL;
        if (ec.err[0]) AFAIL("%s", ec.err);
    }
    p->st_resid += st.combos.n;

    // 3. unità di output: combinazioni o gruppi
    st.null_combo = calloc((size_t)w, sizeof(Tuple*));
    if (!st.null_combo) AFAIL("Out of memory");
    int nunits = 0, nunits_total = 0;
    if (p->grouped) {
        int ngroups = 0;
        // gruppi con una tabella hash sui valori (tipizzati: niente conversione in testo per riga)
        int ng = n->select_stmt.num_group;
        int* group_of = malloc(sizeof(int) * (size_t)(st.combos.n ? st.combos.n : 1));
        EvalVal* kv = malloc(sizeof(EvalVal) * (size_t)(ng ? ng : 1));
        HMap gm = {0};
        SortKey* gk = NULL;                       // chiavi di ogni gruppo
        int capg = 0;
        bool ok = group_of && kv;
        for (int i = 0; ok && i < st.combos.n; i++) {
            ec.cur = COMBO(&st.combos, i);
            uint64_t h = 0x84222325cbf29ce4ULL;
            for (int g = 0; g < ng; g++) {
                eval_expr(n->select_stmt.group_exprs[g], &ec, &kv[g]);
                h = mix64(h ^ ev_hash(&kv[g]));
            }
            int found = -1;
            for (size_t j = gm.cap ? h & (gm.cap - 1) : 0; gm.cap && gm.v[j] >= 0; j = (j + 1) & (gm.cap - 1))
                if (gm.h[j] == h) {
                    bool eq = true;
                    for (int g = 0; g < ng && eq; g++) eq = ev_eq_key(&kv[g], &gk[(size_t)gm.v[j] * (size_t)ng + (size_t)g]);
                    if (eq) { found = gm.v[j]; break; }
                }
            if (found < 0) {
                if (ngroups >= capg) {
                    int nc = capg ? capg * 2 : 64;
                    SortKey* nk = realloc(gk, sizeof(SortKey) * (size_t)nc * (size_t)(ng ? ng : 1));
                    if (!nk) { ok = false; break; }
                    gk = nk;
                    capg = nc;
                }
                for (int g = 0; g < ng; g++) key_set(&gk[(size_t)ngroups * (size_t)ng + (size_t)g], &kv[g]);
                if (!hmap_put(&gm, h, ngroups)) { ok = false; break; }
                found = ngroups++;
            }
            group_of[i] = found;
        }
        for (size_t i = 0; gk && i < (size_t)ngroups * (size_t)ng; i++) free(gk[i].str);
        free(gk);
        free(kv);
        hmap_free(&gm);
        ec.cur = NULL;
        if (ok && n->select_stmt.num_group == 0 && ngroups == 0) ngroups = 1;   // solo aggregati: un gruppo anche vuoto
        st.group_start = calloc((size_t)ngroups + 1, sizeof(int));
        st.group_size  = calloc((size_t)ngroups + 1, sizeof(int));
        st.groups_buf  = malloc(sizeof(Tuple**) * (size_t)(st.combos.n ? st.combos.n : 1));
        int* fill = calloc((size_t)ngroups + 1, sizeof(int));
        if (!ok || !st.group_start || !st.group_size || !st.groups_buf || !fill) { free(group_of); free(fill); AFAIL("Out of memory"); }
        if (ec.err[0]) { free(group_of); free(fill); AFAIL("%s", ec.err); }
        for (int i = 0; i < st.combos.n; i++) st.group_size[group_of[i]]++;
        for (int g = 1; g < ngroups; g++) st.group_start[g] = st.group_start[g - 1] + st.group_size[g - 1];
        for (int i = 0; i < st.combos.n; i++) {
            int g = group_of[i];
            st.groups_buf[st.group_start[g] + fill[g]++] = COMBO(&st.combos, i);
        }
        free(fill);
        free(group_of);
        st.units = malloc(sizeof(int) * (size_t)(ngroups ? ngroups : 1));
        if (!st.units) AFAIL("Out of memory");
        for (int u = 0; u < ngroups; u++) st.units[nunits++] = u;
        nunits_total = ngroups;
        p->st_groups += ngroups;
    } else {
        st.units = malloc(sizeof(int) * (size_t)(st.combos.n ? st.combos.n : 1));
        if (!st.units) AFAIL("Out of memory");
        for (int i = 0; i < st.combos.n; i++) st.units[nunits++] = i;
        nunits_total = st.combos.n;
    }

    #define SET_UNIT(u) arm_set_unit(p, &st, &ec, (u))


    // HAVING
    if (p->grouped && n->select_stmt.having) {
        int kept = 0;
        for (int i = 0; i < nunits; i++) {
            SET_UNIT(st.units[i]);
            if (eval_cond(n->select_stmt.having, &ec)) st.units[kept++] = st.units[i];
        }
        nunits = kept;
        if (ec.err[0]) AFAIL("%s", ec.err);
    }
    p->st_having += nunits;

    // funzioni finestra: calcolate su tutte le unità rimaste, prima di DISTINCT / ORDER BY
    if (p->nwins) {
        int total = nunits_total;
        st.nwinv = total;
        for (int w = 0; w < p->nwins; w++) {
            st.winv[w] = calloc((size_t)(total ? total : 1), sizeof(SortKey));
            if (!st.winv[w]) AFAIL("Out of memory");
            if (!window_compute(p, &st, &ec, p->wins[w], st.units, nunits, st.winv[w])) AFAIL("%s", ec.err[0] ? ec.err : "Out of memory");
        }
        ec.win = st.winv;
    }

    // 4. DISTINCT, ORDER BY, OFFSET / LIMIT; le colonne si calcolano solo per le righe che restano
    int nkeys = apply_order ? n->select_stmt.num_order : 0;
    bool distinct = n->select_stmt.distinct;
    ORows all = {0};
    all.ncols = p->nout;
    int* order = malloc(sizeof(int) * (size_t)(nunits ? nunits : 1));   // indici in units (o in all con DISTINCT)
    if (!order) AFAIL("Out of memory");
    int nkeep = 0;
    if (distinct) {
        StrMap seen = {0};
        char* sig = malloc((size_t)(p->nout ? p->nout : 1) * (EV_STR_MAX + 24) + 1);
        bool ok = sig != NULL;
        int* unit_of = malloc(sizeof(int) * (size_t)(nunits ? nunits : 1));
        if (!unit_of) ok = false;
        for (int i = 0; ok && i < nunits; i++) {
            if ((i & 1023) == 0 && atomic_load(&g_sv_scan_cancel)) { ok = false; break; }
            SET_UNIT(st.units[i]);
            OVal* row = orows_add(&all);
            if (!row) { ok = false; break; }
            for (int j = 0; j < p->nout; j++) { EvalVal v; eval_expr(p->out[j].e, &ec, &v); key_set(&row[j], &v); }
            row_sig(row, p->nout, sig, (size_t)(p->nout ? p->nout : 1) * (EV_STR_MAX + 24) + 1);
            SMSlot* sl = sm_slot(&seen, sig, true);
            if (!sl) { ok = false; break; }
            if (sl->val >= 0) { for (int j = 0; j < p->nout; j++) free(row[j].str); all.nrows--; continue; }
            sl->val = all.nrows - 1;
            unit_of[all.nrows - 1] = st.units[i];
            order[nkeep++] = all.nrows - 1;
        }
        free(sig);
        sm_free(&seen);
        if (ok) { for (int i = 0; i < nkeep; i++) st.units[i] = unit_of[i]; }
        free(unit_of);
        if (!ok) { free(order); orows_free(&all); AFAIL("%s", atomic_load(&g_sv_scan_cancel) ? "Query cancelled" : "Out of memory"); }
        nunits = nkeep;
    } else {
        for (int i = 0; i < nunits; i++) order[nkeep++] = i;
    }
    if (ec.err[0]) { free(order); orows_free(&all); AFAIL("%s", ec.err); }
    if (nkeys) {
        st.keys = calloc((size_t)nunits * (size_t)nkeys + 1, sizeof(SortKey));
        if (!st.keys) { free(order); orows_free(&all); AFAIL("Out of memory"); }
        st.nkeys_alloc = nunits * nkeys;
        for (int i = 0; i < nunits; i++) {
            SET_UNIT(st.units[i]);
            for (int k = 0; k < nkeys; k++) {
                EvalVal v;
                ASTExpr* o = n->select_stmt.order_exprs[k];
                eval_expr(o->type == EXPR_INT ? p->out[o->int_val - 1].e : o, &ec, &v);
                key_set(&st.keys[(size_t)i * (size_t)nkeys + (size_t)k], &v);
            }
        }
        g_sort.keys = st.keys;
        g_sort.nkeys = nkeys;
        g_sort.desc = n->select_stmt.order_desc;
        long want = apply_order && n->select_stmt.has_limit ? (long)n->select_stmt.offset + n->select_stmt.limit : -1;
        if (want >= 0 && want < nkeep / 4) {
            // ORDER BY ... LIMIT k: le prime k righe con un heap (O(n log k)), poi si ordinano solo quelle
            int k = (int)want, hn = 0;
            for (int i = 0; i < nkeep; i++) {
                int x = order[i];
                if (hn < k) {                                        // inserisce nel max-heap
                    int j = hn++;
                    while (j > 0 && unit_cmp(&order[(j - 1) / 2], &x) < 0) { order[j] = order[(j - 1) / 2]; j = (j - 1) / 2; }
                    order[j] = x;
                } else if (k > 0 && unit_cmp(&x, &order[0]) < 0) {   // più piccolo del massimo: lo sostituisce
                    int j = 0;
                    for (;;) {
                        int l = 2 * j + 1, r = l + 1, m = j;
                        const int* cand = &x;
                        if (l < hn && unit_cmp(&order[l], cand) > 0) { m = l; cand = &order[l]; }
                        if (r < hn && unit_cmp(&order[r], cand) > 0) { m = r; }
                        if (m == j) break;
                        order[j] = order[m];
                        j = m;
                    }
                    order[j] = x;
                }
            }
            nkeep = hn;
        }
        qsort(order, (size_t)nkeep, sizeof(int), unit_cmp);
    }
    int first = 0, count = nkeep;
    if (apply_order) {
        first = n->select_stmt.offset < nkeep ? n->select_stmt.offset : nkeep;
        count = nkeep - first;
        if (n->select_stmt.has_limit && n->select_stmt.limit < count) count = n->select_stmt.limit;
    }
    if (stop_after > 0 && count > stop_after) count = stop_after;

    // 5. risultato
    for (int i = 0; i < count && !ec.err[0]; i++) {
        int k = order[first + i];
        OVal* row = orows_add(res);
        if (!row) { free(order); orows_free(&all); AFAIL("Out of memory"); }
        if (distinct) {
            memcpy(row, orow(&all, k), sizeof(OVal) * (size_t)p->nout);
            memset(orow(&all, k), 0, sizeof(OVal) * (size_t)p->nout);   // passati al risultato
        } else {
            SET_UNIT(st.units[k]);
            for (int j = 0; j < p->nout; j++) { EvalVal v; eval_expr(p->out[j].e, &ec, &v); key_set(&row[j], &v); }
        }
    }
    free(order);
    orows_free(&all);
    ec.cur = NULL;
    ec.grp = NULL;
    if (ec.err[0]) AFAIL("%s", ec.err);
    if (atomic_load(&g_sv_scan_cancel)) AFAIL("Query cancelled");
    #undef SET_UNIT
    #undef AFAIL
    p->st_out += res->nrows;
    arm_state_free(&st, p->nsrc);
    return true;
}

// ── UNION / INTERSECT / EXCEPT ──

static bool rows_append(ORows* dst, ORows* src, int i) {
    OVal* row = orows_add(dst);
    if (!row) return false;
    memcpy(row, orow(src, i), sizeof(OVal) * (size_t)dst->ncols);
    memset(orow(src, i), 0, sizeof(OVal) * (size_t)dst->ncols);
    return true;
}

// a op b -> out (a e b vengono consumati)
static bool set_combine(ORows* a, ORows* b, int op, bool all, ORows* out) {
    int nc = a->ncols;
    size_t cap = (size_t)(nc ? nc : 1) * (EV_STR_MAX + 24) + 1;
    char* sig = malloc(cap);
    StrMap inb = {0}, seen = {0};
    bool ok = sig != NULL;
    out->ncols = nc;
    if (ok && op != SETOP_UNION)
        for (int i = 0; ok && i < b->nrows; i++) {
            row_sig(orow(b, i), nc, sig, cap);
            if (!sm_slot(&inb, sig, true)) ok = false;
        }
    for (int pass = 0; ok && pass < (op == SETOP_UNION ? 2 : 1); pass++) {
        ORows* r = pass ? b : a;
        for (int i = 0; ok && i < r->nrows; i++) {
            if (op == SETOP_UNION && all) { ok = rows_append(out, r, i); continue; }
            row_sig(orow(r, i), nc, sig, cap);
            if (op == SETOP_INTERSECT && !sm_slot(&inb, sig, false)) continue;
            if (op == SETOP_EXCEPT && sm_slot(&inb, sig, false)) continue;
            SMSlot* sl = sm_slot(&seen, sig, true);
            if (!sl) { ok = false; break; }
            if (sl->val >= 0) continue;                          // duplicato
            sl->val = 1;
            ok = rows_append(out, r, i);
        }
    }
    free(sig);
    sm_free(&inb);
    sm_free(&seen);
    orows_free(a);
    orows_free(b);
    return ok;
}

static const OVal* g_cmp_rows_base;
static int g_cmp_ncols, g_cmp_nkeys;
static const int* g_cmp_cols;
static const bool* g_cmp_desc;

static int compound_cmp(const void* pa, const void* pb) {
    int ia = *(const int*)pa, ib = *(const int*)pb;
    for (int k = 0; k < g_cmp_nkeys; k++) {
        int c = key_cmp(&g_cmp_rows_base[(size_t)ia * (size_t)g_cmp_ncols + (size_t)g_cmp_cols[k]],
                        &g_cmp_rows_base[(size_t)ib * (size_t)g_cmp_ncols + (size_t)g_cmp_cols[k]]);
        if (c) return g_cmp_desc[k] ? -c : c;
    }
    return ia < ib ? -1 : ia > ib;
}

static double now_ms_exec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static bool query_exec_inner(SelectPlan* p, EvalCtx* outer, int stop_after, ORows* res);

static bool query_exec(SelectPlan* p, EvalCtx* outer, int stop_after, ORows* res) {
    double t0 = now_ms_exec();
    bool ok = query_exec_inner(p, outer, stop_after, res);
    p->st_ms += now_ms_exec() - t0;
    return ok;
}

static bool query_exec_inner(SelectPlan* p, EvalCtx* outer, int stop_after, ORows* res) {
    if (!p->compound) return arm_exec(p, outer, stop_after, true, res);
    ASTNode* n = p->n;
    // INTERSECT lega più di UNION ed EXCEPT: prima i tratti di INTERSECT, poi da sinistra a destra
    ORows acc = {0}, seg = {0};
    acc.ncols = seg.ncols = p->nout;
    int pending = SETOP_NONE;
    bool pending_all = false;
    bool ok = arm_exec(p, outer, 0, false, &seg);
    const SelectPlan* errp = p;
    SelectPlan* a = p;
    while (ok && a->next_arm) {
        int op = a->n->select_stmt.set_op;
        bool all = a->n->select_stmt.set_all;
        SelectPlan* b = a->next_arm;
        ORows r = {0};
        r.ncols = p->nout;
        if (!arm_exec(b, outer, 0, false, &r)) { ok = false; errp = b; orows_free(&r); break; }
        if (op == SETOP_INTERSECT) {
            ORows t = {0};
            ok = set_combine(&seg, &r, SETOP_INTERSECT, false, &t);
            seg = t;
        } else {
            if (pending == SETOP_NONE) { acc = seg; }
            else { ORows t = {0}; ok = set_combine(&acc, &seg, pending, pending_all, &t); acc = t; }
            pending = op;
            pending_all = all;
            seg = r;
        }
        a = b;
    }
    if (ok) {
        if (pending == SETOP_NONE) acc = seg;
        else { ORows t = {0}; ok = set_combine(&acc, &seg, pending, pending_all, &t); acc = t; }
        seg.v = NULL;
    }
    if (!ok) {
        orows_free(&acc);
        orows_free(&seg);
        if (errp != p && errp->err[0]) snprintf(p->err, sizeof(p->err), "%s", errp->err);
        else if (!p->err[0]) snprintf(p->err, sizeof(p->err), "Out of memory");
        return false;
    }
    // ORDER BY / OFFSET / LIMIT del risultato
    int nr = acc.nrows;
    int* order = malloc(sizeof(int) * (size_t)(nr ? nr : 1));
    if (!order) { orows_free(&acc); snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
    for (int i = 0; i < nr; i++) order[i] = i;
    if (n->select_stmt.num_order) {
        g_cmp_rows_base = acc.v; g_cmp_ncols = acc.ncols; g_cmp_nkeys = n->select_stmt.num_order;
        g_cmp_cols = p->corder; g_cmp_desc = n->select_stmt.order_desc;
        qsort(order, (size_t)nr, sizeof(int), compound_cmp);
    }
    int first = n->select_stmt.offset < nr ? n->select_stmt.offset : nr;
    int count = nr - first;
    if (n->select_stmt.has_limit && n->select_stmt.limit < count) count = n->select_stmt.limit;
    if (stop_after > 0 && count > stop_after) count = stop_after;
    res->ncols = acc.ncols;
    for (int i = 0; ok && i < count; i++) ok = rows_append(res, &acc, order[first + i]);
    free(order);
    orows_free(&acc);
    if (!ok) { snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
    return true;
}

// ── subquery nelle espressioni ──

// confronto a op b con la logica a tre valori: 1 vero, 0 falso, -1 sconosciuto
static int cmp_truth(OpType op, const EvalVal* a, const EvalVal* b) {
    if (a->null || b->null) return -1;
    int c = ev_cmp(a, b);
    switch (op) {
        case OP_EQ: return c == 0;  case OP_NEQ: return c != 0;
        case OP_LT: return c < 0;   case OP_GT: return c > 0;
        case OP_LE: return c <= 0;  case OP_GE: return c >= 0;
        default: return -1;
    }
}

// classe di un valore per la tabella hash di IN: 1 numero, 2 testo
static int val_class(const EvalVal* v) { return v->is_num ? 1 : 2; }

static void eval_subquery(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    SelectPlan* p = e->subq ? (SelectPlan*)e->subq->plan : NULL;
    if (!p) { ctx_error(c, "%s", "Subquery was not prepared"); ev_null(out); return; }
    int stop = e->subkind == SUBQ_EXISTS ? 1 : e->subkind == SUBQ_SCALAR ? 2 : 0;
    ORows tmp = {0};
    ORows* rows;
    bool cached = !p->correlated;
    if (cached && p->cache_ok && (p->cache_stop == 0 || (stop && stop <= p->cache_stop))) {
        rows = &p->cache;
    } else {
        ORows* dst = cached ? &p->cache : &tmp;
        if (cached) { orows_free(dst); p->inset_built = false; sm_free(&p->inset); }
        if (!query_exec(p, c, stop, dst)) {
            ctx_error(c, "%.250s", p->err[0] ? p->err : "Subquery failed");
            orows_free(&tmp);
            ev_null(out);
            return;
        }
        if (cached) { p->cache_ok = true; p->cache_stop = stop; }
        rows = dst;
    }

    switch (e->subkind) {
        case SUBQ_EXISTS:
            ev_int(out, rows->nrows > 0);
            break;
        case SUBQ_SCALAR:
            if (rows->nrows == 0) { ev_null(out); break; }
            if (rows->nrows > 1) { ctx_error(c, "%s", "Subquery used as a value returned more than one row"); ev_null(out); break; }
            ev_from_oval(out, orow(rows, 0));
            if (!cached && out->str) {                       // le righe vengono liberate: il testo va copiato
                size_t l = strlen(out->str);
                if (l > EV_STR_MAX) l = EV_STR_MAX;
                memcpy(out->buf, out->str, l);
                out->buf[l] = '\0';
                out->str = out->buf;
            }
            break;
        default: {                                            // ANY / ALL (IN, NOT IN)
            EvalVal a;
            eval_expr(e->left, c, &a);
            bool any = e->subkind == SUBQ_ANY;
            if (rows->nrows == 0) { ev_int(out, any ? 0 : 1); break; }
            if (a.null) { ev_null(out); break; }
            bool eq_set = (any && e->op == OP_EQ) || (!any && e->op == OP_NEQ);
            if (cached && eq_set) {
                // IN / NOT IN con subquery non correlata: tabella hash dei valori, costruita una volta
                if (!p->inset_built) {
                    p->inset_built = true;
                    p->inset_class = 0;
                    p->inset_null = false;
                    for (int i = 0; i < rows->nrows; i++) {
                        const OVal* o = orow(rows, i);
                        if (o->null) { p->inset_null = true; continue; }
                        int cl = o->is_num ? 1 : 2;
                        p->inset_class = p->inset_class == 0 ? cl : p->inset_class == cl ? cl : 3;
                        char key[EV_STR_MAX + 32];
                        oval_key(o, key, sizeof(key));
                        if (!sm_slot(&p->inset, key, true)) { p->inset_class = 3; break; }
                    }
                }
                if (p->inset_class == val_class(&a) || p->inset_class == 0) {
                    OVal o;
                    o.null = false; o.is_num = a.is_num; o.is_int = a.is_int; o.inum = a.inum; o.num = a.num;
                    o.str = (char*)(a.is_num ? NULL : ev_s(&a));
                    char key[EV_STR_MAX + 32];
                    oval_key(&o, key, sizeof(key));
                    bool found = p->inset_class != 0 && sm_slot(&p->inset, key, false) != NULL;
                    if (found) ev_int(out, any ? 1 : 0);
                    else if (p->inset_null) ev_null(out);
                    else ev_int(out, any ? 0 : 1);
                    break;
                }
            }
            bool saw_null = false, decided = false;
            for (int i = 0; i < rows->nrows && !decided; i++) {
                EvalVal b;
                ev_from_oval(&b, orow(rows, i));
                int t = cmp_truth(e->op, &a, &b);
                if (t < 0) saw_null = true;
                else if (any && t == 1) { ev_int(out, 1); decided = true; }
                else if (!any && t == 0) { ev_int(out, 0); decided = true; }
            }
            if (!decided) { if (saw_null) ev_null(out); else ev_int(out, any ? 0 : 1); }
            break;
        }
    }
    orows_free(&tmp);
}

// ── EXPLAIN ──
// Piano con le stime delle righe secondo le selettività di default di System R:
//   colonna = costante 1/10, confronto di intervallo 1/3, LIKE 1/10, altro 1/2;
//   equi-join R ⋈ S: |R|·|S| / max(|R|, |S|)  (come un join su chiave esterna)

static double cond_selectivity(const ASTExpr* c) {
    if (!c) return 1;
    if (c->type == EXPR_BINARY) {
        switch (c->op) {
            case OP_AND: return cond_selectivity(c->left) * cond_selectivity(c->right);
            case OP_OR: { double a = cond_selectivity(c->left), b = cond_selectivity(c->right); return a + b - a * b; }
            case OP_EQ: return 0.1;
            case OP_NEQ: return 0.9;
            case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 1.0 / 3;
            case OP_LIKE: return 0.1;
            case OP_NOT_LIKE: return 0.9;
            default: return 0.5;
        }
    }
    if (c->type == EXPR_UNARY && c->op == OP_NOT) return 1 - cond_selectivity(c->left);
    if (c->type == EXPR_UNARY && c->op == OP_IS_NULL) return 0.1;
    if (c->type == EXPR_SUBQUERY) return c->subkind == SUBQ_ALL ? 0.5 : 0.5;
    return 0.5;
}

static double xv_selectivity(const ExprVec* xv) {
    double s = 1;
    for (int i = 0; i < xv->n; i++) s *= cond_selectivity(xv->v[i]);
    return s;
}

static void xv_format(const ExprVec* xv, char* buf, size_t n) {
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < xv->n && len + 8 < n; i++) {
        char t[400];
        ast_expr_format(xv->v[i], t, sizeof(t));
        len += (size_t)snprintf(buf + len, n - len, "%s%s", i ? " AND " : "", t);
    }
}

typedef struct { QueryResult* r; bool analyze; } PlanOut;

static void plan_row_a(PlanOut* po, const char* indent, const char* op, const char* detail, double est, long actual);

static void plan_row(PlanOut* po, const char* indent, const char* op, const char* detail, double est) {
    plan_row_a(po, indent, op, detail, est, -1);
}

static void plan_row_a(PlanOut* po, const char* indent, const char* op, const char* detail, double est, long actual) {
    QueryResult* r = po->r;
    if (r->num_rows >= 256) return;
    Row* pr = &r->rows[r->num_rows++];
    pr->num_cells = r->num_cols;
    pr->cells = calloc((size_t)r->num_cols, sizeof(Cell));
    if (!pr->cells) { r->num_rows--; return; }
    snprintf(pr->cells[0].value, sizeof(pr->cells[0].value), "%d", r->num_rows);
    pr->cells[0].type = CELL_TYPE_INT;
    snprintf(pr->cells[1].value, sizeof(pr->cells[1].value), "%s%s", indent, op);
    snprintf(pr->cells[2].value, sizeof(pr->cells[2].value), "%s", detail);
    pr->cells[1].type = pr->cells[2].type = CELL_TYPE_STRING;
    if (est < 0) { pr->cells[3].is_null = true; pr->cells[3].type = CELL_TYPE_NULL; snprintf(pr->cells[3].value, sizeof(pr->cells[3].value), "NULL"); }
    else { snprintf(pr->cells[3].value, sizeof(pr->cells[3].value), "%.0f", est < 1 && est > 0 ? 1 : est); pr->cells[3].type = CELL_TYPE_INT; }
    if (r->num_cols > 4) {
        if (actual < 0) { pr->cells[4].is_null = true; pr->cells[4].type = CELL_TYPE_NULL; snprintf(pr->cells[4].value, sizeof(pr->cells[4].value), "NULL"); }
        else { snprintf(pr->cells[4].value, sizeof(pr->cells[4].value), "%ld", actual); pr->cells[4].type = CELL_TYPE_INT; }
    }
}

static void explain_subqueries(PlanOut* po, const ASTExpr* e, const char* indent, int depth);
static double explain_arm(PlanOut* po, SelectPlan* p, const char* indent, int depth);

static void explain_subqueries(PlanOut* po, const ASTExpr* e, const char* indent, int depth) {
    if (!e || depth > 8) return;
    if (e->type == EXPR_SUBQUERY && e->subq && e->subq->plan) {
        SelectPlan* sp = e->subq->plan;
        char d[600], f[200];
        ast_expr_format(e, f, sizeof(f));
        const char* how = sp->correlated ? "correlated: executed again for every row of the outer query"
                                         : (e->subkind == SUBQ_ANY && e->op == OP_EQ) || (e->subkind == SUBQ_ALL && e->op == OP_NEQ)
                                           ? "computed once, values kept in a hash table"
                                           : "computed once";
        if (po->analyze) snprintf(d, sizeof(d), "%s (%s; executed %ld time%s in %.2f ms, actual rows are totals)", f, how,
                                  sp->execs, sp->execs == 1 ? "" : "s", sp->st_ms);
        else             snprintf(d, sizeof(d), "%s (%s)", f, how);
        plan_row(po, indent, "Subquery", d, -1);
        char ind[64];
        snprintf(ind, sizeof(ind), "%s    ", indent);
        explain_arm(po, sp, ind, depth + 1);
        return;
    }
    explain_subqueries(po, e->left, indent, depth);
    explain_subqueries(po, e->right, indent, depth);
    for (int i = 0; i < e->nargs; i++) explain_subqueries(po, e->args[i], indent, depth);
}

static double explain_arm(PlanOut* po, SelectPlan* p, const char* indent, int depth) {
    ASTNode* n = p->n;
    char buf[400], det[700];
    double rows = 1;
    for (int s = 0; s < p->nsrc; s++) {
        Source* S = &p->src[s];
        double est;
        if (S->is_view) {
            est = (double)S->n;
            snprintf(det, sizeof(det), "%.64s (%d rows, computed)", S->name, S->n);
            plan_row_a(po, indent, "View", det, est, po->analyze ? p->st_scanned[s] : -1);
        } else if (p->probe_col[s] >= 0 && !p->probe_dyn[s] && col_is_indexed(S->ts, p->probe_col[s])) {
            bool unique = S->ts->columns[p->probe_col[s]].primary_key;
            est = unique ? 1 : (double)S->ts->num_rows * 0.1;
            snprintf(det, sizeof(det), "%.64s using %.64s.%.64s (in-memory index)", S->name, S->table, S->ts->columns[p->probe_col[s]].name);
            plan_row_a(po, indent, "Index lookup", det, est, po->analyze ? p->st_scanned[s] : -1);
        } else {
            est = (double)S->ts->num_rows;
            snprintf(det, sizeof(det), "%.64s (about %llu rows)", S->name, (unsigned long long)S->ts->num_rows);
            plan_row_a(po, indent, "Table scan", det, est, po->analyze ? p->st_scanned[s] : -1);
        }
        ExprVec* pf[2] = { &p->pre_static[s], &p->pre_dyn[s] };
        for (int q = 0; q < 2; q++) {
            if (!pf[q]->n) continue;
            est *= xv_selectivity(pf[q]);
            xv_format(pf[q], buf, sizeof(buf));
            if (q == 1 && p->probe_col[s] >= 0 && p->probe_dyn[s])
                snprintf(det, sizeof(det), "WHERE %s (on %.64s: lookup by %.64s in a hash table built once)", buf, S->name,
                         S->ts->columns[p->probe_col[s]].name);
            else
                snprintf(det, sizeof(det), "WHERE %s (on %.64s, before the joins)", buf, S->name);
            plan_row_a(po, indent, "Filter", det, est, !po->analyze ? -1 : q == 0 ? p->st_loaded[s] : p->st_dyn[s]);
        }
        if (s == 0) { rows = est; continue; }
        ASTJoin* j = &n->select_stmt.joins[s - 1];
        int a1, a2, a3;
        const char* kind = j->type == JOIN_LEFT ? "LEFT" : j->type == JOIN_RIGHT ? "RIGHT" : j->type == JOIN_FULL ? "FULL" :
                           j->type == JOIN_CROSS ? "CROSS" : "INNER";
        bool hashed = find_hash_cond(&p->jcond[s], s, p->src, &a1, &a2, &a3);
        if (p->jcond[s].n) xv_format(&p->jcond[s], buf, sizeof(buf)); else snprintf(buf, sizeof(buf), "every combination");
        if (j->type == JOIN_CROSS && p->jcond[s].n) kind = "INNER";
        snprintf(det, sizeof(det), "%s JOIN %.64s ON %s", kind, S->name, buf);
        double jr = hashed ? rows * est / (rows > est ? (rows > 0 ? rows : 1) : (est > 0 ? est : 1))
                           : rows * est * xv_selectivity(&p->jcond[s]);
        if (j->type == JOIN_LEFT && jr < rows) jr = rows;
        if ((j->type == JOIN_RIGHT || j->type == JOIN_FULL) && jr < est) jr = est;
        if (j->type == JOIN_FULL && jr < rows + est) jr = jr < rows ? rows + est : jr;
        rows = jr;
        plan_row_a(po, indent, hashed ? "Hash join" : "Nested loop join", det, rows, po->analyze ? p->st_join[s] : -1);
    }
    if (p->resid.n) {
        rows *= xv_selectivity(&p->resid);
        xv_format(&p->resid, buf, sizeof(buf));
        snprintf(det, sizeof(det), "WHERE %s", buf);
        plan_row_a(po, indent, "Filter", det, rows, po->analyze ? p->st_resid : -1);
    }
    if (p->grouped) {
        buf[0] = '\0';
        for (int i = 0; i < n->select_stmt.num_group; i++) {
            char g[200];
            ast_expr_format(n->select_stmt.group_exprs[i], g, sizeof(g));
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", i ? ", " : "", g);
        }
        rows = n->select_stmt.num_group ? (rows > 10 ? rows / 10 : rows) : 1;
        plan_row_a(po, indent, "Aggregate", n->select_stmt.num_group ? buf : "all rows as one group", rows, po->analyze ? p->st_groups : -1);
    }
    if (n->select_stmt.having) {
        ast_expr_format(n->select_stmt.having, buf, sizeof(buf));
        rows *= cond_selectivity(n->select_stmt.having);
        snprintf(det, sizeof(det), "HAVING %s", buf);
        plan_row_a(po, indent, "Filter groups", det, rows, po->analyze ? p->st_having : -1);
    }
    buf[0] = '\0';
    for (int i = 0; i < p->nout; i++) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", i ? ", " : "", p->out[i].name);
    for (int w = 0; w < p->nwins; w++) {
        char wf[300];
        ast_expr_format(p->wins[w], wf, sizeof(wf));
        plan_row_a(po, indent, "Window", wf, rows, po->analyze ? (p->grouped ? p->st_having : p->st_resid) : -1);
    }
    // le colonne si calcolano per tutte le righe solo con DISTINCT; altrimenti dopo ORDER BY / LIMIT
    long projected = n->select_stmt.distinct ? (p->grouped ? p->st_having : p->st_resid) : p->st_out;
    plan_row_a(po, indent, "Projection", buf, rows, po->analyze ? projected : -1);
    if (n->select_stmt.distinct) plan_row_a(po, indent, "Distinct", "remove duplicate rows", rows, po->analyze && !p->compound ? p->st_out : -1);
    // subquery usate nella query
    for (int i = 0; i < p->nout; i++) if (!p->out[i].owned) explain_subqueries(po, p->out[i].e, indent, depth);
    explain_subqueries(po, n->select_stmt.where, indent, depth);
    explain_subqueries(po, n->select_stmt.having, indent, depth);
    for (int j = 0; j < n->select_stmt.num_joins; j++) explain_subqueries(po, n->select_stmt.joins[j].condition, indent, depth);
    if (!p->compound) {
        if (n->select_stmt.num_order) {
            buf[0] = '\0';
            for (int i = 0; i < n->select_stmt.num_order; i++) {
                char o[200];
                ast_expr_format(n->select_stmt.order_exprs[i], o, sizeof(o));
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s%s", i ? ", " : "", o, n->select_stmt.order_desc[i] ? " DESC" : "");
            }
            plan_row_a(po, indent, "Sort", buf, rows, -1);
        }
        if (n->select_stmt.has_limit) {
            snprintf(det, sizeof(det), "%d rows, skipping %d", n->select_stmt.limit, n->select_stmt.offset);
            if (n->select_stmt.limit < rows) rows = n->select_stmt.limit;
            plan_row_a(po, indent, "Limit", det, rows, po->analyze ? p->st_out : -1);
        }
    }
    return rows;
}

static QueryResult* plan_explain(SelectPlan* p, bool analyze, long result_rows) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    int nc = analyze ? 5 : 4;
    r->success = true;
    r->num_cols = nc;
    r->col_names = calloc(5, sizeof(char*));
    r->col_types = calloc(5, sizeof(char*));
    r->rows = calloc(256, sizeof(Row));
    if (!r->col_names || !r->col_types || !r->rows) { db_result_free(r); return NULL; }
    const char* names[5] = { "Step", "Operation", "Details", "Est. rows", "Actual rows" };
    const char* types[5] = { "INT", "VARCHAR", "VARCHAR", "INT", "INT" };
    for (int i = 0; i < nc; i++) { r->col_names[i] = strdup(names[i]); r->col_types[i] = strdup(types[i]); }
    PlanOut po = { r, analyze };
    char tm[160];
    snprintf(tm, sizeof(tm), "%ld rows in %.3f ms", result_rows, p->st_ms);
    if (!p->compound) {
        explain_arm(&po, p, "", 0);
        if (analyze) plan_row_a(&po, "", "Result", tm, -1, result_rows);
        return r;
    }
    double total = 0;
    int k = 0;
    for (SelectPlan* a = p; a; a = a->next_arm, k++) {
        char d[64];
        snprintf(d, sizeof(d), "SELECT #%d", k + 1);
        plan_row(&po, "", "Branch", d, -1);
        total += explain_arm(&po, a, "    ", 0);
        if (a->next_arm) {
            int op = a->n->select_stmt.set_op;
            plan_row(&po, "", op == SETOP_UNION ? (a->n->select_stmt.set_all ? "Union all" : "Union") :
                              op == SETOP_INTERSECT ? "Intersect" : "Except",
                     op == SETOP_UNION && a->n->select_stmt.set_all ? "append the rows" : "hash of the rows, duplicates removed", -1);
        }
    }
    ASTNode* n = p->n;
    char buf[400];
    if (n->select_stmt.num_order) {
        buf[0] = '\0';
        for (int i = 0; i < n->select_stmt.num_order; i++)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s%s", i ? ", " : "", p->out[p->corder[i]].name,
                     n->select_stmt.order_desc[i] ? " DESC" : "");
        plan_row(&po, "", "Sort", buf, total);
    }
    if (n->select_stmt.has_limit) {
        snprintf(buf, sizeof(buf), "%d rows, skipping %d", n->select_stmt.limit, n->select_stmt.offset);
        plan_row(&po, "", "Limit", buf, n->select_stmt.limit < total ? n->select_stmt.limit : total);
    }
    if (analyze) plan_row_a(&po, "", "Result", tm, -1, result_rows);
    return r;
}

// ── SELECT di primo livello: risultato per la GUI ──

static QueryResult* run_select_ex(ExecCtx* ctx, ASTNode* n, int depth, bool explain, bool analyze) {
    char err[300] = "";
    SelectPlan* p = plan_prepare(ctx, n, depth, ctx->trig_bind, NULL, err, sizeof(err));
    if (!p) return result_error_msg(err);
    if (explain) {
        long nr = -1;
        if (analyze) {                          // esegue la query: righe reali di ogni passo
            ORows rows = {0};
            if (!query_exec(p, ctx->trig_eval, 0, &rows)) {
                snprintf(err, sizeof(err), "%s", p->err[0] ? p->err : "Query failed");
                orows_free(&rows);
                plan_free(p);
                return result_error_msg(err);
            }
            nr = rows.nrows;
            orows_free(&rows);
        }
        QueryResult* r = plan_explain(p, analyze, nr);
        plan_free(p);
        return r ? r : result_error_msg("Out of memory");
    }
    ORows rows = {0};
    if (!query_exec(p, ctx->trig_eval, 0, &rows)) {
        snprintf(err, sizeof(err), "%s", p->err[0] ? p->err : "Query failed");
        orows_free(&rows);
        plan_free(p);
        return result_error_msg(err);
    }
    int nout = p->nout;
    QueryResult* r = calloc(1, sizeof(QueryResult));
    bool ok = r != NULL;
    if (ok) {
        r->success = true;
        r->num_cols = nout;
        r->col_names = calloc((size_t)nout + 1, sizeof(char*));
        r->col_types = calloc((size_t)nout + 1, sizeof(char*));
        r->rows = calloc((size_t)(rows.nrows ? rows.nrows : 1), sizeof(Row));
        ok = r->col_names && r->col_types && r->rows;
    }
    for (int j = 0; ok && j < nout; j++) {
        r->col_names[j] = strdup(p->out[j].name);
        r->col_types[j] = strdup(out_type_name(p, j, &rows, false));
    }
    for (int i = 0; ok && i < rows.nrows; i++) {
        Row* row = &r->rows[i];
        row->cells = calloc((size_t)(nout ? nout : 1), sizeof(Cell));
        if (!row->cells) { ok = false; break; }
        row->num_cells = nout;
        r->num_rows = i + 1;
        for (int j = 0; j < nout; j++) {
            EvalVal v;
            const OVal* o = &orow(&rows, i)[j];
            ev_from_oval(&v, o);
            fill_cell(&row->cells[j], &v, o->null ? -1 : p->out[j].src_type);
        }
    }
    orows_free(&rows);
    plan_free(p);
    if (!ok) { db_result_free(r); return result_error_msg("Out of memory"); }
    return r;
}

static QueryResult* run_select(ExecCtx* ctx, ASTNode* n, int depth, bool explain) {
    return run_select_ex(ctx, n, depth, explain, false);
}

// ═════════════════════════════════════════════════════════════════
//  INSERT / UPDATE / DELETE
//  Prima si valida tutto (tipi, NOT NULL, chiave primaria anche fra le righe dello stesso
//  statement), poi si scrive: uno statement che fallisce non lascia modifiche a metà.
// ═════════════════════════════════════════════════════════════════

static Tuple* tuple_clone(const Tuple* t, const TableSchema* ts) {
    Tuple* c = tuple_create(t->num_values);
    if (!c) return NULL;
    for (int i = 0; i < t->num_values; i++) {
        const Value* v = &t->values[i];
        if (v->is_null) { c->values[i].is_null = true; continue; }
        DataType ty = i < ts->num_columns ? ts->columns[i].type : SV_TYPE_VARCHAR;
        switch (ty) {
            case SV_TYPE_INT: case SV_TYPE_BIGINT: case SV_TYPE_FLOAT: case SV_TYPE_BOOL:
                c->values[i] = *v;
                c->values[i].owns_str = false;
                break;
            default:
                c->values[i].str_val = strdup(v->str_val ? v->str_val : "");
                if (!c->values[i].str_val) { tuple_free(c); return NULL; }
                c->values[i].owns_str = true;
                break;
        }
    }
    return c;
}

// testo di un valore calcolato da salvare (NULL per i NULL)
static const char* ev_store_text(const EvalVal* v) { return v->null ? NULL : ev_s(v); }

static int pk_column(const TableSchema* ts) {
    for (int i = 0; i < ts->num_columns; i++) if (ts->columns[i].primary_key) return i;
    return -1;
}

static void rid_key(RID r, char* buf, size_t n) { snprintf(buf, n, "%u:%u", r.page_id, (unsigned)r.slot_id); }

static void wal_log(ExecCtx* ctx, uint8_t type, RID rid, const Tuple* newt, const Tuple* oldt, TableSchema* ts) {
    if (!ctx->wal) return;
    size_t nl = 0, ol = 0;
    uint8_t* ns = newt ? tuple_serialize((Tuple*)newt, ts, &nl) : NULL;
    uint8_t* os = oldt ? tuple_serialize((Tuple*)oldt, ts, &ol) : NULL;
    wal_write(ctx->wal, type, ctx->tx_id, rid.page_id, (SVRID){rid.page_id, rid.slot_id},
              ns, (uint32_t)nl, os, (uint32_t)ol);
    free(ns);
    free(os);
}

static void wal_log_move(ExecCtx* ctx, RID old_rid, RID new_rid, const Tuple* newt, const Tuple* oldt, TableSchema* ts) {
    if (!ctx->wal) return;
    size_t nl = 0, ol = 0;
    uint8_t* ns = tuple_serialize((Tuple*)newt, ts, &nl);
    uint8_t* os = tuple_serialize((Tuple*)oldt, ts, &ol);
    uint8_t* data = ns ? malloc(nl + 6) : NULL;
    if (data) {
        memcpy(data, &new_rid.page_id, 4);
        memcpy(data + 4, &new_rid.slot_id, 2);
        memcpy(data + 6, ns, nl);
        wal_write(ctx->wal, LOG_UPDATE_MOVE, ctx->tx_id, old_rid.page_id, (SVRID){old_rid.page_id, old_rid.slot_id},
                  data, (uint32_t)(nl + 6), os, (uint32_t)ol);
    }
    free(data);
    free(ns);
    free(os);
}

// ═════════════════════════════════════════════════════════════════
//  Vincoli di integrità
//  - chiavi (PRIMARY KEY e UNIQUE, anche composte): nessuna coppia di righe con la stessa
//    chiave; le chiavi con un NULL non si confrontano (UNIQUE ammette più NULL)
//  - CHECK: la condizione non deve essere FALSA (vera o sconosciuta va bene, come in SQL)
//  - FOREIGN KEY: ogni chiave esterna senza NULL deve esistere nella tabella riferita;
//    cancellare o modificare una riga riferita applica l'azione del vincolo:
//    NO ACTION / RESTRICT (errore), CASCADE, SET NULL, SET DEFAULT
//  Le modifiche di uno statement (comprese quelle a cascata) vengono prima calcolate tutte
//  e controllate, poi scritte: uno statement che viola un vincolo non lascia modifiche.
// ═════════════════════════════════════════════════════════════════

typedef struct { int cols[SV_MAX_KEY_COLS]; int n; bool primary; const char* name; } KeyDef;

// chiavi della tabella: la chiave primaria (vincolo o colonna del formato precedente) e le UNIQUE
static int table_keys(const TableSchema* ts, KeyDef* out, int max) {
    int k = 0;
    bool have_pk = false;
    for (int i = 0; i < ts->num_constraints && k < max; i++) {
        const Constraint* c = &ts->constraints[i];
        if (c->kind != CONS_PRIMARY && c->kind != CONS_UNIQUE) continue;
        KeyDef* d = &out[k++];
        d->n = c->ncols;
        for (int j = 0; j < c->ncols; j++) d->cols[j] = c->cols[j];
        d->primary = c->kind == CONS_PRIMARY;
        d->name = c->name;
        if (d->primary) have_pk = true;
    }
    if (!have_pk && k < max)
        for (int i = 0; i < ts->num_columns; i++)
            if (ts->columns[i].primary_key) {
                KeyDef* d = &out[k++];
                d->n = 1; d->cols[0] = i; d->primary = true; d->name = ts->columns[i].name;
                break;
            }
    return k;
}

static void key_cols_text(const TableSchema* ts, const int* cols, int n, char* buf, size_t cap) {
    size_t len = 0;
    buf[0] = '\0';
    for (int k = 0; k < n && len + 4 < cap; k++)
        len += (size_t)snprintf(buf + len, cap - len, "%s%s", k ? ", " : "", ts->columns[cols[k]].name);
}

// "Duplicate entry '5' for primary key 'id'" / "... for unique key (a, b)"
static void dup_message(const TableSchema* ts, const KeyDef* k, const char* key, char* msg, size_t n) {
    char shown[200], cols[200];
    snprintf(shown, sizeof(shown), "%.190s", key);
    for (char* p = shown; *p; p++) if (*p == '\x1f') *p = '-';
    key_cols_text(ts, k->cols, k->n, cols, sizeof(cols));
    if (k->n == 1) snprintf(msg, n, "Duplicate entry '%.100s' for %s '%.64s'", shown, k->primary ? "primary key" : "unique key", cols);
    else           snprintf(msg, n, "Duplicate entry '%.100s' for %s (%.120s)", shown, k->primary ? "primary key" : "unique key", cols);
}

// posizioni nella tabella riferita delle colonne di una chiave esterna; false se non esistono
static bool fk_ref_cols(const Constraint* f, const TableSchema* parent, int* out) {
    for (int k = 0; k < f->ncols; k++) {
        out[k] = catalog_get_col_index((TableSchema*)parent, f->ref_cols[k]);
        if (out[k] < 0) return false;
    }
    return true;
}

// ── CHECK ──

typedef struct {
    ASTExpr*          e[SV_MAX_CONSTRAINTS];
    const Constraint* c[SV_MAX_CONSTRAINTS];
    int               n;
    Source            src;
    const TableSchema* ts;
} Checks;

static void checks_free(Checks* ck) {
    for (int i = 0; i < ck->n; i++) ast_expr_free(ck->e[i]);
    ck->n = 0;
}

static ASTExpr* compile_check(const char* text, Source* src, char* msg, size_t n) {
    TokenList* tk = lexer_tokenize(text);
    if (!tk) { snprintf(msg, n, "Out of memory"); return NULL; }
    char perr[256] = "";
    ASTExpr* e = parser_parse_expr(tk, perr, sizeof(perr));
    lexer_free(tk);
    if (!e) { snprintf(msg, n, "Invalid CHECK condition: %.200s", perr); return NULL; }
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.src = src; b.nsrc = 1; b.mode = BIND_ROW; b.clause = "CHECK";
    bind_expr(e, &b);
    if (b.err[0]) { snprintf(msg, n, "%.250s", b.err); ast_expr_free(e); return NULL; }
    return e;
}

static const char* checks_prepare(Checks* ck, TableSchema* ts, char* msg, size_t n) {
    memset(ck, 0, sizeof(*ck));
    ck->ts = ts;
    snprintf(ck->src.name, sizeof(ck->src.name), "%s", ts->name);
    snprintf(ck->src.table, sizeof(ck->src.table), "%s", ts->name);
    ck->src.ts = ts;
    for (int i = 0; i < ts->num_constraints; i++) {
        const Constraint* c = &ts->constraints[i];
        if (c->kind != CONS_CHECK) continue;
        ASTExpr* e = compile_check(c->check, &ck->src, msg, n);
        if (!e) { checks_free(ck); return msg; }
        ck->e[ck->n] = e;
        ck->c[ck->n++] = c;
    }
    return NULL;
}

static const char* checks_eval(Checks* ck, Tuple* t, char* msg, size_t n) {
    if (!ck->n) return NULL;
    Tuple* cur[1] = { t };
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.src = &ck->src; ec.nsrc = 1; ec.cur = cur;
    for (int i = 0; i < ck->n; i++) {
        EvalVal v;
        eval_expr(ck->e[i], &ec, &v);
        if (ec.err[0]) { snprintf(msg, n, "%.250s", ec.err); return msg; }
        if (ev_truth(&v) == 0) {
            snprintf(msg, n, "CHECK constraint '%.64s' is violated: %.180s", ck->c[i]->name, ck->c[i]->check);
            return msg;
        }
    }
    return NULL;
}

// ── insieme delle modifiche di uno statement ──

typedef struct {
    TableSchema* ts;
    RID          rid;
    Tuple*       old;      // riga attuale (immagine per il WAL e gli indici)
    Tuple*       nt;       // nuova riga; NULL = cancellazione
    bool         owns_old;
    bool         direct;   // chiesta dallo statement (non a cascata)
} Change;

typedef struct {
    Change*   v;
    int       n, cap;
    StrMap    idx;         // "tabella:pagina:slot" -> posizione
    ExecCtx*  x;
    Database* db;
} ChangeSet;

static void row_ref(const TableSchema* ts, RID r, char* buf, size_t n) {
    snprintf(buf, n, "%p:%u:%u", (const void*)ts, r.page_id, (unsigned)r.slot_id);
}

static int cs_find(ChangeSet* cs, const TableSchema* ts, RID r) {
    char k[64];
    row_ref(ts, r, k, sizeof(k));
    SMSlot* sl = sm_slot(&cs->idx, k, false);
    return sl ? sl->val : -1;
}

static int cs_add(ChangeSet* cs, TableSchema* ts, RID r, Tuple* old, Tuple* nt, bool owns_old, bool direct) {
    if (cs->n >= cs->cap) {
        int nc = cs->cap ? cs->cap * 2 : 64;
        Change* nv = realloc(cs->v, sizeof(Change) * (size_t)nc);
        if (!nv) return -1;
        cs->v = nv;
        cs->cap = nc;
    }
    char k[64];
    row_ref(ts, r, k, sizeof(k));
    SMSlot* sl = sm_slot(&cs->idx, k, true);
    if (!sl) return -1;
    sl->val = cs->n;
    cs->v[cs->n] = (Change){ ts, r, old, nt, owns_old, direct };
    return cs->n++;
}

static void cs_free(ChangeSet* cs) {
    for (int i = 0; i < cs->n; i++) {
        if (cs->v[i].nt) tuple_free(cs->v[i].nt);
        if (cs->v[i].owns_old) tuple_free(cs->v[i].old);
    }
    free(cs->v);
    sm_free(&cs->idx);
    memset(cs, 0, sizeof(*cs));
}

static void value_copy(Value* dst, const Value* src, DataType t) {
    if (dst->owns_str) free(dst->str_val);
    *dst = *src;
    dst->owns_str = false;
    if (src->is_null) { dst->str_val = NULL; return; }
    if (t != SV_TYPE_INT && t != SV_TYPE_BIGINT && t != SV_TYPE_FLOAT && t != SV_TYPE_BOOL) {
        dst->str_val = strdup(src->str_val ? src->str_val : "");
        dst->owns_str = dst->str_val != NULL;
    }
}

// propaga le cancellazioni e le modifiche delle chiavi riferite (azioni delle FOREIGN KEY)
static const char* cs_cascade(ChangeSet* cs, char* msg, size_t mn) {
    Database* db = cs->db;
    typedef struct { TableSchema* child; RID rid; const Constraint* f; const TableSchema* parent; } Restrict;
    Restrict* rs = NULL;
    int nrs = 0, caprs = 0;
    const char* fail = NULL;
    bool again = true;
    for (int pass = 0; again && !fail; pass++) {
        if (pass >= 32) { fail = "Cascading foreign key actions do not converge"; break; }
        again = false;
        nrs = 0;
        for (int i = 0; i < cs->n && !fail; i++) {
            TableSchema* T = cs->v[i].ts;
            for (int ti = 0; ti < db->num_tables && !fail; ti++) {
                TableSchema* C = &db->tables[ti];
                for (int fi = 0; fi < C->num_constraints && !fail; fi++) {
                    const Constraint* f = &C->constraints[fi];
                    if (f->kind != CONS_FOREIGN || strcasecmp(f->ref_table, T->name)) continue;
                    int pcols[SV_MAX_KEY_COLS];
                    if (!fk_ref_cols(f, T, pcols)) continue;
                    char okey[EV_STR_MAX + 1], nkey[EV_STR_MAX + 1];
                    if (!tuple_key(T, cs->v[i].old, pcols, f->ncols, okey, sizeof(okey))) continue;
                    bool is_del = cs->v[i].nt == NULL;
                    if (!is_del && tuple_key(T, cs->v[i].nt, pcols, f->ncols, nkey, sizeof(nkey)) && !strcmp(okey, nkey)) continue;
                    int fcols[SV_MAX_KEY_COLS];
                    for (int k = 0; k < f->ncols; k++) fcols[k] = f->cols[k];
                    MemIndex* cix = mi_get_cols(cs->x, db->name, C, fcols, f->ncols);
                    if (!cix) { fail = "Out of memory (foreign key index)"; break; }
                    const RidList* l = mi_lookup(cix, okey);
                    int nl = l ? l->n : 0;
                    RID* rids = nl ? malloc(sizeof(RID) * (size_t)nl) : NULL;
                    if (nl && !rids) { fail = "Out of memory"; break; }
                    if (nl) memcpy(rids, l->r, sizeof(RID) * (size_t)nl);
                    int action = is_del ? f->on_delete : f->on_update;
                    for (int q = 0; q < nl && !fail; q++) {
                        int ex = cs_find(cs, C, rids[q]);
                        if (ex >= 0 && cs->v[ex].nt == NULL) continue;          // riga già cancellata
                        if (action == FK_NO_ACTION || action == FK_RESTRICT) {
                            if (nrs >= caprs) {
                                int nc = caprs ? caprs * 2 : 16;
                                Restrict* nr = realloc(rs, sizeof(Restrict) * (size_t)nc);
                                if (!nr) { fail = "Out of memory"; break; }
                                rs = nr;
                                caprs = nc;
                            }
                            rs[nrs++] = (Restrict){ C, rids[q], f, T };
                            continue;
                        }
                        if (action == FK_CASCADE && is_del) {                 // ON DELETE CASCADE
                            if (ex >= 0) { tuple_free(cs->v[ex].nt); cs->v[ex].nt = NULL; if (ex <= i) again = true; continue; }
                            HeapFile* hf = heap_open(cs->x->bp, C->root_page_id, C->num_pages);
                            Tuple* child = hf ? heap_get(hf, rids[q], C) : NULL;
                            free(hf);
                            if (!child) continue;
                            if (cs_add(cs, C, rids[q], child, NULL, true, false) < 0) { tuple_free(child); fail = "Out of memory"; }
                            continue;
                        }
                        // CASCADE (modifica), SET NULL, SET DEFAULT: nuova versione della riga figlia
                        Tuple* target;
                        if (ex >= 0) {
                            target = cs->v[ex].nt;
                            if (ex <= i) again = true;
                        } else {
                            HeapFile* hf = heap_open(cs->x->bp, C->root_page_id, C->num_pages);
                            Tuple* child = hf ? heap_get(hf, rids[q], C) : NULL;
                            free(hf);
                            if (!child) continue;
                            target = tuple_clone(child, C);
                            if (!target || cs_add(cs, C, rids[q], child, target, true, false) < 0) {
                                tuple_free(child); tuple_free(target); fail = "Out of memory"; break;
                            }
                        }
                        for (int k = 0; k < f->ncols; k++) {
                            int cc = f->cols[k];
                            DataType ty = C->columns[cc].type;
                            if (action == FK_CASCADE) value_copy(&target->values[cc], &cs->v[i].nt->values[pcols[k]], ty);
                            else if (action == FK_SET_NULL) value_set_from_text(&target->values[cc], ty, NULL);
                            else {
                                const char* dv = C->columns[cc].default_val;
                                value_set_from_text(&target->values[cc], ty, (dv[0] && strcasecmp(dv, "NULL")) ? dv : NULL);
                            }
                        }
                    }
                    free(rids);
                }
            }
        }
    }
    // NO ACTION / RESTRICT: la riga che riferisce deve sparire nello stesso statement
    for (int i = 0; !fail && i < nrs; i++) {
        int ex = cs_find(cs, rs[i].child, rs[i].rid);
        if (ex >= 0 && cs->v[ex].nt == NULL) continue;
        snprintf(msg, mn, "Cannot delete or change the row of '%.64s': it is referenced by table '%.64s' "
                 "(foreign key '%.64s')", rs[i].parent->name, rs[i].child->name, rs[i].f->name);
        fail = msg;
    }
    free(rs);
    return fail;
}

// esiste (alla fine dello statement) una riga di P con quella chiave sulle colonne pcols?
static int key_exists_final(ChangeSet* cs, TableSchema* P, const int* pcols, int n, const char* key) {
    MemIndex* ix = mi_get_cols(cs->x, cs->db->name, P, pcols, n);
    if (!ix) return -1;
    const RidList* l = mi_lookup(ix, key);
    for (int i = 0; l && i < l->n; i++) {
        int ex = cs_find(cs, P, l->r[i]);
        if (ex < 0) return 1;                                   // riga non toccata
        if (cs->v[ex].nt) {                                     // modificata: conta la nuova chiave
            char k2[EV_STR_MAX + 1];
            if (tuple_key(P, cs->v[ex].nt, pcols, n, k2, sizeof(k2)) && !strcmp(k2, key)) return 1;
        }
    }
    for (int i = 0; i < cs->n; i++) {                           // righe modificate che assumono la chiave
        if (cs->v[i].ts != P || !cs->v[i].nt) continue;
        char k2[EV_STR_MAX + 1];
        if (tuple_key(P, cs->v[i].nt, pcols, n, k2, sizeof(k2)) && !strcmp(k2, key)) return 1;
    }
    return 0;
}

static void fk_message(const TableSchema* C, const Constraint* f, const char* key, char* msg, size_t n) {
    char shown[200], cols[200];
    snprintf(shown, sizeof(shown), "%.190s", key);
    for (char* p = shown; *p; p++) if (*p == '\x1f') *p = ',';
    int fc[SV_MAX_KEY_COLS];
    for (int k = 0; k < f->ncols; k++) fc[k] = f->cols[k];
    key_cols_text(C, fc, f->ncols, cols, sizeof(cols));
    snprintf(msg, n, "Foreign key '%.40s' is violated: no row of '%.40s' has (%.60s) = (%.60s)", f->name, f->ref_table, cols, shown);
}

// controlla le righe modificate (NOT NULL, CHECK, chiavi, chiavi esterne)
static const char* cs_validate(ChangeSet* cs, char* msg, size_t mn) {
    Database* db = cs->db;
    const TableSchema* seen[SV_MAX_TABLES];
    int nseen = 0;
    for (int i = 0; i < cs->n; i++) {
        TableSchema* T = cs->v[i].ts;
        bool done = false;
        for (int k = 0; k < nseen; k++) if (seen[k] == T) done = true;
        if (done) continue;
        if (nseen < SV_MAX_TABLES) seen[nseen++] = T;
        Checks ck;
        if (checks_prepare(&ck, T, msg, mn)) return msg;
        KeyDef keys[SV_MAX_CONSTRAINTS + 1];
        int nk = table_keys(T, keys, SV_MAX_CONSTRAINTS + 1);
        StrMap finals[SV_MAX_CONSTRAINTS + 1];
        memset(finals, 0, sizeof(finals));
        const char* fail = NULL;
        for (int j = i; j < cs->n && !fail; j++) {
            Change* ch = &cs->v[j];
            if (ch->ts != T || !ch->nt) continue;
            for (int c = 0; c < T->num_columns && !fail; c++)
                if (ch->nt->values[c].is_null && (!T->columns[c].nullable || T->columns[c].primary_key)) {
                    snprintf(msg, mn, "Column '%.64s' cannot be NULL", T->columns[c].name);
                    fail = msg;
                }
            if (!fail) fail = checks_eval(&ck, ch->nt, msg, mn);
            // chiavi: due righe finali con la stessa chiave
            for (int k = 0; k < nk && !fail; k++) {
                char nkey[EV_STR_MAX + 1], okey[EV_STR_MAX + 1];
                if (!tuple_key(T, ch->nt, keys[k].cols, keys[k].n, nkey, sizeof(nkey))) {
                    if (keys[k].primary) { snprintf(msg, mn, "Primary key column cannot be NULL"); fail = msg; }
                    continue;
                }
                SMSlot* sl = sm_slot(&finals[k], nkey, true);
                if (!sl) { fail = "Out of memory"; break; }
                if (sl->val >= 0) { dup_message(T, &keys[k], nkey, msg, mn); fail = msg; break; }
                sl->val = j;
                bool same = tuple_key(T, ch->old, keys[k].cols, keys[k].n, okey, sizeof(okey)) && !strcmp(okey, nkey);
                if (same) continue;
                MemIndex* ix = mi_get_cols(cs->x, db->name, T, keys[k].cols, keys[k].n);
                if (!ix) { fail = "Out of memory (key index)"; break; }
                const RidList* l = mi_lookup(ix, nkey);
                for (int q = 0; l && q < l->n; q++)
                    if (cs_find(cs, T, l->r[q]) < 0) { dup_message(T, &keys[k], nkey, msg, mn); fail = msg; break; }
            }
            // chiavi esterne della riga modificata
            for (int fi = 0; fi < T->num_constraints && !fail; fi++) {
                const Constraint* f = &T->constraints[fi];
                if (f->kind != CONS_FOREIGN) continue;
                int fc[SV_MAX_KEY_COLS];
                for (int k = 0; k < f->ncols; k++) fc[k] = f->cols[k];
                char nkey[EV_STR_MAX + 1], okey[EV_STR_MAX + 1];
                if (!tuple_key(T, ch->nt, fc, f->ncols, nkey, sizeof(nkey))) continue;     // con un NULL: non controllata
                if (tuple_key(T, ch->old, fc, f->ncols, okey, sizeof(okey)) && !strcmp(okey, nkey)) continue;
                TableSchema* P = catalog_get_table(cs->x->catalog, db->name, f->ref_table);
                int pcols[SV_MAX_KEY_COLS];
                if (!P || !fk_ref_cols(f, P, pcols)) { snprintf(msg, mn, "Table '%.64s' of foreign key '%.64s' not found", f->ref_table, f->name); fail = msg; break; }
                int ok = key_exists_final(cs, P, pcols, f->ncols, nkey);
                if (ok < 0) { fail = "Out of memory"; break; }
                if (!ok) { fk_message(T, f, nkey, msg, mn); fail = msg; }
            }
        }
        for (int k = 0; k < nk; k++) sm_free(&finals[k]);
        checks_free(&ck);
        if (fail) return fail;
    }
    return NULL;
}

// scrive le modifiche (WAL, pagine, indici)
static int cs_apply(ChangeSet* cs, char* msg, size_t mn, const char** fail) {
    int direct = 0;
    *fail = NULL;
    for (int i = 0; i < cs->n; i++) {
        Change* ch = &cs->v[i];
        TableSchema* T = ch->ts;
        HeapFile* hf = heap_open(cs->x->bp, T->root_page_id, T->num_pages);
        if (!hf) { *fail = "Failed to open the table"; break; }
        if (!ch->nt) {
            wal_log(cs->x, LOG_DELETE, ch->rid, NULL, ch->old, T);
            if (heap_delete(hf, ch->rid) != SV_OK) { mi_invalidate_table(cs->db->name, T->name); free(hf); continue; }
            mi_on_delete(cs->db->name, T, ch->old, ch->rid);
            if (T->num_rows > 0) T->num_rows--;
        } else {
            RID nrid = ch->rid;
            if (heap_update(hf, ch->rid, ch->nt, T, &nrid) != SV_OK) {
                mi_invalidate_table(cs->db->name, T->name);
                snprintf(msg, mn, "Failed to update row %d of %d (the previous rows were updated)", i + 1, cs->n);
                *fail = msg;
                free(hf);
                break;
            }
            // la riga nuova può stare in un altro slot: un solo record con le due posizioni
            // (undo e redo lavorano per posizione; un record solo è atomico anche dopo un crash)
            wal_log_move(cs->x, ch->rid, nrid, ch->nt, ch->old, T);
            mi_on_delete(cs->db->name, T, ch->old, ch->rid);
            mi_on_insert(cs->db->name, T, ch->nt, nrid);
            T->num_pages = hf->num_pages;
        }
        free(hf);
        if (ch->direct) direct++;
    }
    return direct;
}

// righe nuove di un INSERT: CHECK, chiavi, chiavi esterne
static const char* validate_inserts(ExecCtx* ctx, Database* db, TableSchema* ts, Tuple** rows, int nrows, char* msg, size_t mn) {
    Checks ck;
    if (checks_prepare(&ck, ts, msg, mn)) return msg;
    const char* fail = NULL;
    for (int r = 0; r < nrows && !fail; r++) fail = checks_eval(&ck, rows[r], msg, mn);
    checks_free(&ck);
    if (fail) return fail;

    KeyDef keys[SV_MAX_CONSTRAINTS + 1];
    int nk = table_keys(ts, keys, SV_MAX_CONSTRAINTS + 1);
    for (int k = 0; k < nk && !fail; k++) {
        MemIndex* ix = mi_get_cols(ctx, db->name, ts, keys[k].cols, keys[k].n);
        if (!ix) return "Out of memory (key index)";
        StrMap batch = {0};
        for (int r = 0; r < nrows && !fail; r++) {
            char key[EV_STR_MAX + 1];
            if (!tuple_key(ts, rows[r], keys[k].cols, keys[k].n, key, sizeof(key))) {
                if (keys[k].primary) { snprintf(msg, mn, "Primary key column cannot be NULL"); fail = msg; }
                continue;
            }
            const RidList* l = mi_lookup(ix, key);
            SMSlot* sl = sm_slot(&batch, key, true);
            if (!sl) { fail = "Out of memory"; break; }
            if ((l && l->n > 0) || sl->val >= 0) { dup_message(ts, &keys[k], key, msg, mn); fail = msg; break; }
            sl->val = r;
        }
        sm_free(&batch);
    }
    for (int fi = 0; fi < ts->num_constraints && !fail; fi++) {
        const Constraint* f = &ts->constraints[fi];
        if (f->kind != CONS_FOREIGN) continue;
        TableSchema* P = catalog_get_table(ctx->catalog, db->name, f->ref_table);
        int pcols[SV_MAX_KEY_COLS], fc[SV_MAX_KEY_COLS];
        if (!P || !fk_ref_cols(f, P, pcols)) { snprintf(msg, mn, "Table '%.64s' of foreign key '%.64s' not found", f->ref_table, f->name); return msg; }
        for (int k = 0; k < f->ncols; k++) fc[k] = f->cols[k];
        MemIndex* pix = mi_get_cols(ctx, db->name, P, pcols, f->ncols);
        if (!pix) return "Out of memory (foreign key index)";
        StrMap own = {0};                    // tabella che riferisce sé stessa: chiavi delle righe nuove
        if (P == ts)
            for (int r = 0; r < nrows; r++) {
                char key[EV_STR_MAX + 1];
                if (tuple_key(ts, rows[r], pcols, f->ncols, key, sizeof(key)) && !sm_slot(&own, key, true)) { fail = "Out of memory"; break; }
            }
        for (int r = 0; r < nrows && !fail; r++) {
            char key[EV_STR_MAX + 1];
            if (!tuple_key(ts, rows[r], fc, f->ncols, key, sizeof(key))) continue;     // con un NULL: non controllata
            const RidList* l = mi_lookup(pix, key);
            if ((l && l->n > 0) || (P == ts && sm_slot(&own, key, false))) continue;
            fk_message(ts, f, key, msg, mn);
            fail = msg;
        }
        sm_free(&own);
    }
    return fail;
}

// ── definizione dei vincoli (CREATE TABLE, ALTER TABLE ADD CONSTRAINT) ──

static bool types_compatible(DataType a, DataType b) {
    if (a == b) return true;
    if (is_int_type(a) && is_int_type(b)) return true;
    bool ta = a == SV_TYPE_VARCHAR || a == SV_TYPE_CHAR || a == SV_TYPE_TEXT;
    bool tb = b == SV_TYPE_VARCHAR || b == SV_TYPE_CHAR || b == SV_TYPE_TEXT;
    return ta && tb;
}

static bool cons_name_used(const TableSchema* ts, const char* name) {
    for (int i = 0; i < ts->num_constraints; i++) if (!strcasecmp(ts->constraints[i].name, name)) return true;
    return false;
}

// colonne -> posizioni (con controllo di esistenza e duplicati)
static const char* resolve_cols(const TableSchema* ts, char names[][64], int n, uint8_t* out, char* msg, size_t mn) {
    if (n < 1) return "A key needs at least one column";
    for (int k = 0; k < n; k++) {
        int ci = catalog_get_col_index((TableSchema*)ts, names[k]);
        if (ci < 0) { snprintf(msg, mn, "Unknown column '%.64s' in the constraint", names[k]); return msg; }
        for (int j = 0; j < k; j++) if (out[j] == ci) { snprintf(msg, mn, "Column '%.64s' is listed twice in the constraint", names[k]); return msg; }
        out[k] = (uint8_t)ci;
    }
    return NULL;
}

// aggiunge a ts il vincolo d (parent: la tabella riferita, o ts stessa se si riferisce a sé)
static const char* add_constraint(ExecCtx* ctx, Database* db, TableSchema* ts, ConstraintDef* d, char* msg, size_t mn) {
    if (ts->num_constraints >= SV_MAX_CONSTRAINTS) return "Too many constraints on the table (max 16)";
    Constraint c;
    memset(&c, 0, sizeof(c));
    c.kind = (uint8_t)d->kind;
    const char* prefix = d->kind == CONS_PRIMARY ? "pk" : d->kind == CONS_UNIQUE ? "uq" : d->kind == CONS_FOREIGN ? "fk" : "chk";
    if (d->name[0]) snprintf(c.name, sizeof(c.name), "%s", d->name);
    else for (int k = 1; ; k++) {
        snprintf(c.name, sizeof(c.name), "%s_%.40s_%d", prefix, ts->name, k);
        if (!cons_name_used(ts, c.name)) break;
    }
    if (cons_name_used(ts, c.name)) { snprintf(msg, mn, "A constraint named '%.64s' already exists", c.name); return msg; }

    if (d->kind == CONS_CHECK) {
        Source src;
        memset(&src, 0, sizeof(src));
        snprintf(src.name, sizeof(src.name), "%s", ts->name);
        snprintf(src.table, sizeof(src.table), "%s", ts->name);
        src.ts = ts;
        ASTExpr* e = compile_check(d->check, &src, msg, mn);
        if (!e) return msg;
        bool agg = expr_has_agg(e);
        ast_expr_free(e);
        if (agg) return "Aggregate functions are not allowed in CHECK";
        snprintf(c.check, sizeof(c.check), "%s", d->check);
    } else {
        if (resolve_cols(ts, d->cols, d->ncols, c.cols, msg, mn)) return msg;
        c.ncols = (uint8_t)d->ncols;
    }
    if (d->kind == CONS_PRIMARY) {
        KeyDef keys[SV_MAX_CONSTRAINTS + 1];
        int nk = table_keys(ts, keys, SV_MAX_CONSTRAINTS + 1);
        for (int k = 0; k < nk; k++) if (keys[k].primary) return "A table can have only one PRIMARY KEY";
    }
    if (d->kind == CONS_FOREIGN) {
        bool self = !strcasecmp(d->ref_table, ts->name);
        TableSchema* P = self ? ts : catalog_get_table(ctx->catalog, db->name, d->ref_table);
        if (!P) { snprintf(msg, mn, "Referenced table '%.64s' not found", d->ref_table); return msg; }
        snprintf(c.ref_table, sizeof(c.ref_table), "%s", P->name);
        int pc[SV_MAX_KEY_COLS];
        int np = d->nref;
        if (np == 0) {                                          // REFERENCES t: la chiave primaria di t
            KeyDef keys[SV_MAX_CONSTRAINTS + 1];
            int nk = table_keys(P, keys, SV_MAX_CONSTRAINTS + 1);
            for (int k = 0; k < nk && !np; k++) if (keys[k].primary) { np = keys[k].n; memcpy(pc, keys[k].cols, sizeof(int) * (size_t)np); }
            if (!np) { snprintf(msg, mn, "Table '%.64s' has no primary key: list the referenced columns", P->name); return msg; }
        } else {
            for (int k = 0; k < np; k++) {
                pc[k] = catalog_get_col_index(P, d->ref_cols[k]);
                if (pc[k] < 0) { snprintf(msg, mn, "Unknown column '%.64s' in table '%.64s'", d->ref_cols[k], P->name); return msg; }
            }
        }
        if (np != d->ncols) { snprintf(msg, mn, "Foreign key '%.64s' has %d column(s) but references %d", c.name, d->ncols, np); return msg; }
        // le colonne riferite devono formare una chiave (PRIMARY KEY o UNIQUE) della tabella riferita
        KeyDef keys[SV_MAX_CONSTRAINTS + 1];
        int nk = table_keys(P, keys, SV_MAX_CONSTRAINTS + 1);
        bool is_key = false;
        for (int k = 0; k < nk && !is_key; k++) {
            if (keys[k].n != np) continue;
            bool all = true;
            for (int a = 0; a < np && all; a++) {
                bool in = false;
                for (int b = 0; b < np; b++) if (keys[k].cols[b] == pc[a]) in = true;
                all = in;
            }
            is_key = all;
        }
        if (!is_key) { snprintf(msg, mn, "The columns referenced by '%.64s' must be the PRIMARY KEY or a UNIQUE key of '%.64s'", c.name, P->name); return msg; }
        for (int k = 0; k < np; k++) {
            snprintf(c.ref_cols[k], sizeof(c.ref_cols[k]), "%s", P->columns[pc[k]].name);
            if (!types_compatible(ts->columns[c.cols[k]].type, P->columns[pc[k]].type)) {
                snprintf(msg, mn, "Column '%.64s' (%s) cannot reference '%.64s.%.64s' (%s): different types", ts->columns[c.cols[k]].name,
                         datatype_name(ts->columns[c.cols[k]].type), P->name, P->columns[pc[k]].name, datatype_name(P->columns[pc[k]].type));
                return msg;
            }
        }
        c.on_delete = (uint8_t)d->on_delete;
        c.on_update = (uint8_t)d->on_update;
        for (int k = 0; k < c.ncols; k++)
            if ((c.on_delete == FK_SET_NULL || c.on_update == FK_SET_NULL) && !ts->columns[c.cols[k]].nullable) {
                snprintf(msg, mn, "SET NULL is impossible: column '%.64s' is NOT NULL", ts->columns[c.cols[k]].name);
                return msg;
            }
    }

    // i dati esistenti devono già rispettare il vincolo (ALTER TABLE ADD CONSTRAINT)
    if (ts->num_rows > 0 || ts->root_page_id) {
        HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
        HeapScan* scan = hf ? heap_scan_open(hf, ts) : NULL;
        StrMap seen = {0};
        Checks ck;
        memset(&ck, 0, sizeof(ck));
        const char* fail = NULL;
        TableSchema* P = d->kind == CONS_FOREIGN ? catalog_get_table(ctx->catalog, db->name, c.ref_table) : NULL;
        int pcols[SV_MAX_KEY_COLS], cc[SV_MAX_KEY_COLS];
        for (int k = 0; k < c.ncols; k++) cc[k] = c.cols[k];
        MemIndex* pix = NULL;
        if (P && fk_ref_cols(&c, P, pcols)) pix = mi_get_cols(ctx, db->name, P, pcols, c.ncols);
        if (d->kind == CONS_CHECK) {
            ck.ts = ts;
            snprintf(ck.src.name, sizeof(ck.src.name), "%s", ts->name);
            snprintf(ck.src.table, sizeof(ck.src.table), "%s", ts->name);
            ck.src.ts = ts;
            ck.e[0] = compile_check(c.check, &ck.src, msg, mn);
            ck.c[0] = &c;
            ck.n = ck.e[0] ? 1 : 0;
            if (!ck.n) fail = msg;
        }
        Tuple* t;
        RID rid;
        while (!fail && scan && (t = heap_scan_next(scan, &rid)) != NULL) {
            char key[EV_STR_MAX + 1];
            if (d->kind == CONS_CHECK) fail = checks_eval(&ck, t, msg, mn);
            else if (d->kind == CONS_FOREIGN) {
                if (tuple_key(ts, t, cc, c.ncols, key, sizeof(key)) && (!pix || !mi_lookup(pix, key) || !mi_lookup(pix, key)->n)) {
                    snprintf(msg, mn, "Existing rows violate the foreign key '%.64s' (for example %.100s)", c.name, key);
                    fail = msg;
                }
            } else {
                if (!tuple_key(ts, t, cc, c.ncols, key, sizeof(key))) {
                    if (d->kind == CONS_PRIMARY) fail = "Existing rows have NULL in the primary key columns";
                } else {
                    SMSlot* sl = sm_slot(&seen, key, true);
                    if (!sl) fail = "Out of memory";
                    else if (sl->val >= 0) { snprintf(msg, mn, "Existing rows have duplicate values for the new key (%.100s)", key); fail = msg; }
                    else sl->val = 1;
                }
            }
            tuple_free(t);
        }
        if (scan) heap_scan_close(scan);
        free(hf);
        sm_free(&seen);
        checks_free(&ck);
        if (fail) return fail;
    }

    ts->constraints[ts->num_constraints++] = c;
    if (d->kind == CONS_PRIMARY)
        for (int k = 0; k < c.ncols; k++) { ts->columns[c.cols[k]].primary_key = true; ts->columns[c.cols[k]].nullable = false; }
    if (d->kind == CONS_UNIQUE && c.ncols == 1) ts->columns[c.cols[0]].unique = true;
    mi_invalidate_table(db->name, ts->name);
    return NULL;
}

// la tabella ts è riferita da una chiave esterna di un'altra tabella? (nome del vincolo)
static const Constraint* referenced_by(Database* db, const TableSchema* ts, const TableSchema** child) {
    for (int i = 0; i < db->num_tables; i++) {
        const TableSchema* C = &db->tables[i];
        if (C == ts) continue;
        for (int k = 0; k < C->num_constraints; k++)
            if (C->constraints[k].kind == CONS_FOREIGN && !strcasecmp(C->constraints[k].ref_table, ts->name)) {
                if (child) *child = C;
                return &C->constraints[k];
            }
    }
    return NULL;
}

// ═════════════════════════════════════════════════════════════════
//  Trigger (basi di dati attive): regole evento-condizione-azione
//  CREATE TRIGGER nome {BEFORE | AFTER} {INSERT | UPDATE [OF colonne] | DELETE} ON tabella
//      [FOR EACH ROW | FOR EACH STATEMENT] [WHEN (condizione)] corpo
//  - FOR EACH ROW: eseguito per ogni riga, con NEW (riga nuova) e OLD (riga precedente)
//  - BEFORE: prima dei controlli dei vincoli; può cambiare la riga con SET NEW.col = ...
//    o rifiutarla con SIGNAL 'messaggio'
//  - AFTER: dopo la modifica; le sue istruzioni possono attivare altri trigger
//  Se un trigger fallisce lo statement che lo ha attivato viene annullato per intero.
// ═════════════════════════════════════════════════════════════════

#define MAX_TRIGGER_DEPTH 16

typedef struct {
    Source  src[2];       // NEW, OLD
    Tuple*  cur[2];
    BindCtx b;
    EvalCtx e;
} TrigScope;

static void trig_scope_init(TrigScope* sc, ExecCtx* ctx, TableSchema* ts, Tuple* newt, Tuple* oldt) {
    memset(sc, 0, sizeof(*sc));
    const char* names[2] = { "NEW", "OLD" };
    for (int i = 0; i < 2; i++) {
        snprintf(sc->src[i].name, sizeof(sc->src[i].name), "%s", names[i]);
        snprintf(sc->src[i].table, sizeof(sc->src[i].table), "%s", names[i]);
        sc->src[i].ts = ts;
    }
    sc->cur[0] = newt;
    sc->cur[1] = oldt;
    sc->b.src = sc->src; sc->b.nsrc = 2; sc->b.mode = BIND_ROW; sc->b.x = ctx; sc->b.clause = "the trigger";
    sc->e.src = sc->src; sc->e.nsrc = 2; sc->e.cur = sc->cur; sc->e.x = ctx;
}

static bool trig_has_set(TrigStmt** v, int n) {
    for (int i = 0; i < n; i++) {
        if (v[i]->kind == TS_SET) return true;
        if (v[i]->kind == TS_IF && (trig_has_set(v[i]->then_v, v[i]->nthen) || trig_has_set(v[i]->else_v, v[i]->nelse))) return true;
    }
    return false;
}

// controlla le espressioni del corpo (WHEN, SET, IF, SIGNAL) contro NEW / OLD
static const char* trig_bind_all(TrigScope* sc, TableSchema* ts, TrigStmt** v, int n, char* msg, size_t mn) {
    for (int i = 0; i < n; i++) {
        TrigStmt* t = v[i];
        sc->b.err[0] = '\0';
        if (t->kind == TS_SET) {
            if (catalog_get_col_index(ts, t->col) < 0) { snprintf(msg, mn, "Unknown column 'NEW.%.64s'", t->col); return msg; }
            bind_expr(t->expr, &sc->b);
        } else if (t->kind == TS_SIGNAL) {
            bind_expr(t->expr, &sc->b);
        } else if (t->kind == TS_IF) {
            bind_expr(t->cond, &sc->b);
            if (!sc->b.err[0] && trig_bind_all(sc, ts, t->then_v, t->nthen, msg, mn)) return msg;
            if (!sc->b.err[0] && trig_bind_all(sc, ts, t->else_v, t->nelse, msg, mn)) return msg;
        }
        if (sc->b.err[0]) { snprintf(msg, mn, "%.250s", sc->b.err); return msg; }
    }
    return NULL;
}

static const char* trig_run(ExecCtx* ctx, TrigScope* sc, TableSchema* ts, TrigStmt** v, int n, Tuple* newt,
                            char* msg, size_t mn) {
    for (int i = 0; i < n; i++) {
        TrigStmt* t = v[i];
        sc->e.err[0] = '\0';
        switch (t->kind) {
            case TS_SQL: {
                QueryResult* r = executor_run(ctx, t->sql);
                if (!r || !r->success) {
                    snprintf(msg, mn, "%.280s", r ? r->error : "statement failed");
                    db_result_free(r);
                    return msg;
                }
                db_result_free(r);
                break;
            }
            case TS_SET: {
                int ci = catalog_get_col_index(ts, t->col);
                if (ci < 0 || !newt) { snprintf(msg, mn, "Cannot set NEW.%.64s here", t->col); return msg; }
                EvalVal v2;
                eval_expr(t->expr, &sc->e, &v2);
                if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
                char text[EV_STR_MAX + 1];
                const char* tx = NULL;
                if (!v2.null) { snprintf(text, sizeof(text), "%s", ev_s(&v2)); tx = text; }
                if (tx && check_value(&ts->columns[ci], tx, msg, mn)) return msg;
                value_set_from_text(&newt->values[ci], ts->columns[ci].type, tx);
                break;
            }
            case TS_SIGNAL: {
                EvalVal v2;
                eval_expr(t->expr, &sc->e, &v2);
                snprintf(msg, mn, "%.280s", v2.null ? "Error raised by a trigger" : ev_s(&v2));
                return msg;
            }
            case TS_IF: {
                bool c = eval_cond(t->cond, &sc->e);
                if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
                const char* f = c ? trig_run(ctx, sc, ts, t->then_v, t->nthen, newt, msg, mn)
                                  : trig_run(ctx, sc, ts, t->else_v, t->nelse, newt, msg, mn);
                if (f) return f;
                break;
            }
        }
    }
    return NULL;
}

// esegue i trigger della tabella per l'evento; NULL se tutto va bene, altrimenti il messaggio
static const char* fire_triggers(ExecCtx* ctx, Database* db, TableSchema* ts, const char* event, bool before, bool row,
                                 Tuple* oldt, Tuple* newt, const int* set_ci, int nset, char* msg, size_t mn) {
    if (!db || !db->num_triggers) return NULL;
    for (int i = 0; i < db->num_triggers; i++) {
        TriggerSchema* tg = &db->triggers[i];
        if (tg->before != before || strcasecmp(tg->table_name, ts->name) || strcasecmp(tg->event, event)) continue;
        TokenList* tk = lexer_tokenize(tg->body);
        TrigProgram* prog = tk ? parser_parse_trigger(tk) : NULL;
        if (!prog || !prog->success) {
            snprintf(msg, mn, "Trigger '%.64s' cannot be read: %.180s", tg->name, prog ? prog->error : "out of memory");
            trig_program_free(prog);
            lexer_free(tk);
            return msg;
        }
        bool skip = prog->for_each_row != row;
        if (!skip && prog->n_of > 0 && set_ci) {                  // UPDATE OF colonne
            skip = true;
            for (int k = 0; k < prog->n_of && skip; k++)
                for (int j = 0; j < nset; j++)
                    if (!strcasecmp(ts->columns[set_ci[j]].name, prog->of_cols[k])) skip = false;
        }
        const char* fail = NULL;
        if (!skip) {
            if (ctx->trig_depth >= MAX_TRIGGER_DEPTH) fail = "Triggers are nested too deeply (a trigger keeps activating itself?)";
            TrigScope* sc = fail ? NULL : malloc(sizeof(TrigScope));
            if (!fail && !sc) fail = "Out of memory";
            if (sc) {
                trig_scope_init(sc, ctx, ts, row ? newt : NULL, row ? oldt : NULL);
                bool go = true;
                if (prog->when) {
                    bind_expr(prog->when, &sc->b);
                    if (sc->b.err[0]) { snprintf(msg, mn, "%.250s", sc->b.err); fail = msg; }
                    else go = eval_cond(prog->when, &sc->e);
                    if (!fail && sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); fail = msg; }
                }
                if (!fail && go) fail = trig_bind_all(sc, ts, prog->v, prog->n, msg, mn);
                if (!fail && go) {
                    struct BindCtx* sb = ctx->trig_bind;
                    struct EvalCtx* se = ctx->trig_eval;
                    ctx->trig_bind = &sc->b;
                    ctx->trig_eval = &sc->e;
                    ctx->trig_depth++;
                    fail = trig_run(ctx, sc, ts, prog->v, prog->n, before ? newt : NULL, msg, mn);
                    ctx->trig_depth--;
                    ctx->trig_bind = sb;
                    ctx->trig_eval = se;
                }
                free(sc);
            }
        }
        trig_program_free(prog);
        lexer_free(tk);
        if (fail) {
            if (!strncmp(fail, "Trigger '", 9)) { if (fail != msg) snprintf(msg, mn, "%s", fail); return msg; }   // dal trigger più interno
            char inner[300];
            snprintf(inner, sizeof(inner), "%s", fail);
            snprintf(msg, mn, "Trigger '%.64s': %.220s", tg->name, inner);
            return msg;
        }
        // la tabella può essere cambiata (indirizzo) solo con DDL, non ammessi nei trigger
    }
    return NULL;
}

static QueryResult* exec_create_trigger(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    char msg[300];
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->create_trigger.table_name);
    if (!ts) { snprintf(msg, sizeof(msg), "Table '%.64s' not found (triggers are defined on tables)", n->create_trigger.table_name); return result_error_msg(msg); }
    for (int i = 0; i < db->num_triggers; i++)
        if (!strcasecmp(db->triggers[i].name, n->create_trigger.name)) {
            snprintf(msg, sizeof(msg), "Trigger '%.64s' already exists", n->create_trigger.name);
            return result_error_msg(msg);
        }
    if (db->num_triggers >= 64) return result_error_msg("Too many triggers (max 64 per database)");
    TokenList* tk = lexer_tokenize(n->create_trigger.body);
    TrigProgram* prog = tk ? parser_parse_trigger(tk) : NULL;
    const char* fail = NULL;
    if (!prog || !prog->success) { snprintf(msg, sizeof(msg), "Invalid trigger: %.250s", prog ? prog->error : "out of memory"); fail = msg; }
    if (!fail && trig_has_set(prog->v, prog->n) && (!n->create_trigger.before || !prog->for_each_row))
        fail = "SET NEW.column is only allowed in BEFORE ... FOR EACH ROW triggers";
    if (!fail && trig_has_set(prog->v, prog->n) && !strcasecmp(n->create_trigger.event, "DELETE"))
        fail = "A DELETE trigger has no NEW row to change";
    for (int k = 0; !fail && prog && k < prog->n_of; k++)
        if (catalog_get_col_index(ts, prog->of_cols[k]) < 0) { snprintf(msg, sizeof(msg), "Unknown column '%.64s' after OF", prog->of_cols[k]); fail = msg; }
    if (!fail) {
        TrigScope* sc = malloc(sizeof(TrigScope));
        if (!sc) fail = "Out of memory";
        else {
            trig_scope_init(sc, ctx, ts, NULL, NULL);
            if (prog->when) { bind_expr(prog->when, &sc->b); if (sc->b.err[0]) { snprintf(msg, sizeof(msg), "%.250s", sc->b.err); fail = msg; } }
            if (!fail) fail = trig_bind_all(sc, ts, prog->v, prog->n, msg, sizeof(msg));
            free(sc);
        }
    }
    trig_program_free(prog);
    lexer_free(tk);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    TriggerSchema* tg = &db->triggers[db->num_triggers++];
    memset(tg, 0, sizeof(*tg));
    snprintf(tg->name, sizeof(tg->name), "%s", n->create_trigger.name);
    snprintf(tg->table_name, sizeof(tg->table_name), "%s", ts->name);
    tg->before = n->create_trigger.before;
    snprintf(tg->event, sizeof(tg->event), "%s", n->create_trigger.event);
    snprintf(tg->body, sizeof(tg->body), "%s", n->create_trigger.body);
    return result_ok_msg(0);
}

static QueryResult* exec_drop_trigger(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    for (int i = 0; i < db->num_triggers; i++)
        if (!strcasecmp(db->triggers[i].name, n->drop_trigger.name)) {
            memmove(&db->triggers[i], &db->triggers[i + 1], sizeof(TriggerSchema) * (size_t)(db->num_triggers - i - 1));
            db->num_triggers--;
            return result_ok_msg(0);
        }
    char msg[200];
    snprintf(msg, sizeof(msg), "Trigger '%.64s' not found", n->drop_trigger.name);
    return result_error_msg(msg);
}

// trigger per riga delle modifiche fatte dalle azioni delle chiavi esterne (CASCADE, SET NULL, ...)
static const char* fire_cascaded(ExecCtx* ctx, Database* db, ChangeSet* cs, bool before, char* msg, size_t mn) {
    if (!db->num_triggers) return NULL;
    for (int i = 0; i < cs->n; i++) {
        Change* ch = &cs->v[i];
        if (ch->direct) continue;
        int changed[SV_MAX_COLUMNS], nch = 0;
        for (int c = 0; ch->nt && c < ch->ts->num_columns; c++) {
            char a[EV_STR_MAX + 1], b[EV_STR_MAX + 1];
            const Value* x = &ch->old->values[c];
            const Value* y = &ch->nt->values[c];
            bool diff = x->is_null != y->is_null;
            if (!diff && !x->is_null) {
                mi_key(x, ch->ts->columns[c].type, a, sizeof(a));
                mi_key(y, ch->ts->columns[c].type, b, sizeof(b));
                diff = strcmp(a, b) != 0;
            }
            if (diff) changed[nch++] = c;
        }
        const char* f = fire_triggers(ctx, db, ch->ts, ch->nt ? "UPDATE" : "DELETE", before, true, ch->old, ch->nt,
                                      ch->nt ? changed : NULL, nch, msg, mn);
        if (f) return f;
    }
    return NULL;
}

static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->insert_stmt.table_name))
            return result_error_msg("Cannot INSERT into a view");
        return result_error_msg("Table not found");
    }
    char msg[300];
    int nc = ts->num_columns;

    // colonne indicate
    int map[64];
    int ncols = n->insert_stmt.num_cols > 0 ? n->insert_stmt.num_cols : nc;
    for (int i = 0; i < ncols; i++) {
        if (n->insert_stmt.num_cols == 0) { map[i] = i; continue; }
        int ci = catalog_get_col_index(ts, n->insert_stmt.columns[i]);
        if (ci < 0) return unknown_column_error(n->insert_stmt.columns[i], "INSERT");
        for (int k = 0; k < i; k++)
            if (map[k] == ci) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is listed twice", ts->columns[ci].name);
                return result_error_msg(msg);
            }
        map[i] = ci;
    }
    // INSERT ... SELECT: prima si calcola tutta la SELECT (che può leggere la stessa tabella)
    ORows srows = {0};
    bool from_select = n->insert_stmt.select != NULL;
    if (from_select) {
        char perr[300] = "";
        SelectPlan* sp = plan_prepare(ctx, n->insert_stmt.select, 0, ctx->trig_bind, NULL, perr, sizeof(perr));
        if (!sp) return result_error_msg(perr);
        if (sp->nout != ncols) {
            snprintf(msg, sizeof(msg), "Column count doesn't match: %d column%s, the SELECT returns %d",
                     ncols, ncols == 1 ? "" : "s", sp->nout);
            plan_free(sp);
            return result_error_msg(msg);
        }
        if (!query_exec(sp, ctx->trig_eval, 0, &srows)) {
            snprintf(msg, sizeof(msg), "%s", sp->err);
            plan_free(sp);
            orows_free(&srows);
            return result_error_msg(msg);
        }
        plan_free(sp);
    } else if (n->insert_stmt.num_values != ncols) {
        snprintf(msg, sizeof(msg), "Column count doesn't match value count: %d column%s, %d value%s",
                 ncols, ncols == 1 ? "" : "s", n->insert_stmt.num_values, n->insert_stmt.num_values == 1 ? "" : "s");
        return result_error_msg(msg);
    }
    int nrows = from_select ? srows.nrows : n->insert_stmt.num_rows;

    // i valori sono espressioni senza colonne (numeri, testi, funzioni come NOW(), subquery)
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.mode = BIND_ROW;
    b.clause = "VALUES";
    b.x = ctx;
    b.outer = ctx->trig_bind;
    for (int i = 0; !from_select && i < nrows * ncols; i++) bind_expr(n->insert_stmt.values[i], &b);
    if (b.err[0]) return result_error_msg(b.err);

    int ai = -1;
    for (int i = 0; i < nc; i++) if (ts->columns[i].auto_increment) { ai = i; break; }
    int64_t auto_next = 0;
    if (ai >= 0) {
        MemIndex* ix = mi_get(ctx, db->name, ts, ai);
        if (!ix) { orows_free(&srows); return result_error_msg("Out of memory (AUTO_INCREMENT index)"); }
        auto_next = ix->has_max ? ix->max_int : 0;
    }

    Tuple** tuples = calloc((size_t)(nrows ? nrows : 1), sizeof(Tuple*));
    if (!tuples) { orows_free(&srows); return result_error_msg("Out of memory"); }
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.x = ctx;
    ec.outer = ctx->trig_eval;
    const char* fail = NULL;
    static char vals[64][EV_STR_MAX + 1];   // testi della riga corrente
    char ai_buf[32];

    for (int r = 0; r < nrows && !fail; r++) {
        const char* text[64];
        bool given[64] = {0};
        for (int i = 0; i < nc; i++) text[i] = NULL;
        for (int k = 0; k < ncols; k++) {
            EvalVal v;
            if (from_select) ev_from_oval(&v, &orow(&srows, r)[k]);
            else eval_expr(n->insert_stmt.values[r * ncols + k], &ec, &v);
            if (ec.err[0]) { fail = ec.err; break; }
            int ci = map[k];
            given[ci] = true;
            if (v.null) { text[ci] = NULL; continue; }
            snprintf(vals[ci], sizeof(vals[ci]), "%s", ev_s(&v));
            text[ci] = vals[ci];
        }
        if (fail) break;
        for (int i = 0; i < nc; i++) {
            Column* c = &ts->columns[i];
            if (!given[i] && c->default_val[0] && strcasecmp(c->default_val, "NULL") != 0) text[i] = c->default_val;
            if (i == ai) {
                if (!text[i]) {                                   // valore generato (anche per un NULL esplicito)
                    if (auto_next == INT64_MAX) { fail = "AUTO_INCREMENT value out of range"; break; }
                    snprintf(ai_buf, sizeof(ai_buf), "%lld", (long long)++auto_next);
                    text[i] = ai_buf;
                } else {
                    char* end = NULL;
                    long long x = strtoll(text[i], &end, 10);
                    if (end != text[i] && x > auto_next) auto_next = x;
                }
            }
            if (!text[i]) {
                if (!c->nullable || c->primary_key) {
                    snprintf(msg, sizeof(msg), c->primary_key ? "Primary key '%.64s' cannot be NULL" : "Column '%.64s' cannot be NULL", c->name);
                    fail = msg;
                    break;
                }
                continue;
            }
            if (check_value(c, text[i], msg, sizeof(msg))) { fail = msg; break; }
        }
        if (fail) break;
        tuples[r] = tuple_from_strings(text, nc, ts);
        if (!tuples[r]) { fail = "Out of memory"; break; }
    }
    // trigger BEFORE (possono cambiare NEW o rifiutare la riga), poi i vincoli: CHECK,
    // PRIMARY KEY / UNIQUE (anche fra le righe dello statement), FOREIGN KEY
    if (!fail) fail = fire_triggers(ctx, db, ts, "INSERT", true, false, NULL, NULL, NULL, 0, msg, sizeof(msg));
    for (int r = 0; r < nrows && !fail; r++)
        fail = fire_triggers(ctx, db, ts, "INSERT", true, true, NULL, tuples[r], NULL, 0, msg, sizeof(msg));
    for (int r = 0; r < nrows && !fail; r++)                      // un trigger può aver tolto un valore obbligatorio
        for (int c = 0; c < nc && !fail; c++)
            if (tuples[r]->values[c].is_null && (!ts->columns[c].nullable || ts->columns[c].primary_key)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' cannot be NULL", ts->columns[c].name);
                fail = msg;
            }
    if (!fail) fail = validate_inserts(ctx, db, ts, tuples, nrows, msg, sizeof(msg));
    if (fail) {
        for (int r = 0; r < nrows; r++) if (tuples[r]) tuple_free(tuples[r]);
        free(tuples);
        QueryResult* er = result_error_msg(fail);
        orows_free(&srows);
        return er;
    }
    orows_free(&srows);

    // scrittura
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    int done = 0;
    if (hf) {
        for (int r = 0; r < nrows; r++) {
            RID rid;
            if (heap_insert(hf, tuples[r], ts, &rid) != SV_OK) break;
            wal_log(ctx, LOG_INSERT, rid, tuples[r], NULL, ts);
            mi_on_insert(db->name, ts, tuples[r], rid);
            ts->num_rows++;
            done++;
        }
        ts->num_pages = hf->num_pages;
        free(hf);
    }
    const char* tfail = NULL;
    for (int r = 0; r < done && !tfail; r++)                      // trigger AFTER
        tfail = fire_triggers(ctx, db, ts, "INSERT", false, true, NULL, tuples[r], NULL, 0, msg, sizeof(msg));
    if (!tfail && done == nrows) tfail = fire_triggers(ctx, db, ts, "INSERT", false, false, NULL, NULL, NULL, 0, msg, sizeof(msg));
    for (int r = 0; r < nrows; r++) tuple_free(tuples[r]);
    free(tuples);
    if (tfail) return result_error_msg(msg);
    if (done < nrows) {
        mi_invalidate_table(db->name, ts->name);
        snprintf(msg, sizeof(msg), "Failed to insert row %d of %d (the previous rows were inserted)", done + 1, nrows);
        return result_error_msg(msg);
    }
    return result_ok_msg(done);
}

// righe della tabella che soddisfano la WHERE (con l'indice quando possibile: anche per
// "colonna indicizzata = espressione senza colonne della tabella", es. NEW.id in un trigger)
static const char* select_matches(ExecCtx* ctx, Database* db, Source* s, ASTExpr* where, EvalCtx* ec,
                                  int** out_idx, int* out_n) {
    ExprVec conj = {0};
    collect_conj(where, &conj);
    for (int i = 0; i < conj.n && !s->loaded; i++) {
        ASTExpr* w = conj.v[i];
        if (w->type != EXPR_BINARY || w->op != OP_EQ) continue;
        for (int side = 0; side < 2 && !s->loaded; side++) {
            ASTExpr* col = side ? w->right : w->left;
            ASTExpr* key = side ? w->left : w->right;
            if (col->type != EXPR_COLUMN || col->bound <= 0 || col->outer) continue;
            int ci = (col->bound - 1) % 1024;
            unsigned m = 0; bool o = false, op = false;
            expr_refs(key, &m, &o, &op);
            if (m || op || !col_is_indexed(s->ts, ci) || key->type == EXPR_INT || key->type == EXPR_STRING) continue;
            EvalVal kv;
            eval_expr(key, ec, &kv);
            char kt[EV_STR_MAX + 1];
            if (!probe_text(&kv, s->ts->columns[ci].type, kt, sizeof(kt)) && !kv.null) continue;
            MemIndex* ix = mi_get(ctx, db->name, s->ts, ci);
            if (!ix) continue;
            s->loaded = true;
            HeapFile* hf = heap_open(ctx->bp, s->ts->root_page_id, s->ts->num_pages);
            if (!hf) { free(conj.v); return "Failed to read the table"; }
            int cap = 0;
            const RidList* l = kv.null ? NULL : mi_lookup(ix, kt);
            for (int r = 0; l && r < l->n; r++) {
                Tuple* t = heap_get(hf, l->r[r], s->ts);
                if (t && !source_push(s, t, l->r[r], &cap)) { tuple_free(t); free(hf); free(conj.v); return "Out of memory"; }
            }
            free(hf);
        }
    }
    free(conj.v);
    if (!source_load(ctx, db->name, s, where, NULL, 0)) return "Failed to read the table";
    int* idx = malloc(sizeof(int) * (size_t)(s->n ? s->n : 1));
    if (!idx) return "Out of memory";
    int n = 0;
    Tuple* cur[1];
    ec->src = s;
    ec->nsrc = 1;
    ec->cur = cur;
    for (int i = 0; i < s->n; i++) {
        cur[0] = s->rows[i];
        if (eval_cond(where, ec)) idx[n++] = i;
    }
    ec->cur = NULL;
    if (ec->err[0]) { free(idx); return ec->err; }
    *out_idx = idx;
    *out_n = n;
    return NULL;
}

static QueryResult* exec_update(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->update_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->update_stmt.table_name)) return result_error_msg("Cannot UPDATE a view");
        return result_error_msg("Table not found");
    }
    char msg[300];
    int set_ci[SV_MAX_SETS];
    for (int k = 0; k < n->update_stmt.num_sets; k++) {
        set_ci[k] = catalog_get_col_index(ts, n->update_stmt.set_cols[k]);
        if (set_ci[k] < 0) return unknown_column_error(n->update_stmt.set_cols[k], "SET");
    }
    Source s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", ts->name);
    snprintf(s.table, sizeof(s.table), "%s", ts->name);
    s.ts = ts;
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.src = &s; b.nsrc = 1; b.mode = BIND_ROW; b.x = ctx; b.outer = ctx->trig_bind;
    b.clause = "SET";
    for (int k = 0; k < n->update_stmt.num_sets; k++) bind_expr(n->update_stmt.set_exprs[k], &b);
    b.clause = "WHERE";
    bind_expr(n->update_stmt.where, &b);
    if (b.err[0]) return result_error_msg(b.err);

    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.x = ctx;
    ec.outer = ctx->trig_eval;
    int* idx = NULL;
    int nm = 0;
    const char* fail = select_matches(ctx, db, &s, n->update_stmt.where, &ec, &idx, &nm);
    if (fail) { snprintf(msg, sizeof(msg), "%s", fail); source_free(&s); return result_error_msg(msg); }

    // nuove righe: tutte le espressioni usano i valori precedenti della riga
    Tuple** nt = calloc((size_t)(nm ? nm : 1), sizeof(Tuple*));
    if (!nt) { free(idx); source_free(&s); return result_error_msg("Out of memory"); }
    Tuple* cur[1];
    ec.src = &s; ec.nsrc = 1; ec.cur = cur;
    for (int i = 0; i < nm && !fail; i++) {
        Tuple* old = s.rows[idx[i]];
        cur[0] = old;
        nt[i] = tuple_clone(old, ts);
        if (!nt[i]) { fail = "Out of memory"; break; }
        for (int k = 0; k < n->update_stmt.num_sets; k++) {
            int ci = set_ci[k];
            Column* c = &ts->columns[ci];
            EvalVal v;
            eval_expr(n->update_stmt.set_exprs[k], &ec, &v);
            if (ec.err[0]) { fail = ec.err; break; }
            const char* text = ev_store_text(&v);
            if (!text && (!c->nullable || c->primary_key)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' cannot be NULL", c->name);
                fail = msg;
                break;
            }
            if (text && check_value(c, text, msg, sizeof(msg))) { fail = msg; break; }
            value_set_from_text(&nt[i]->values[ci], c->type, text);
        }
    }
    ec.cur = NULL;

    // trigger BEFORE, modifiche (con le azioni a cascata delle chiavi esterne), controllo dei
    // vincoli, scrittura, trigger AFTER
    int affected = 0;
    if (!fail) fail = fire_triggers(ctx, db, ts, "UPDATE", true, false, NULL, NULL, set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
    for (int i = 0; i < nm && !fail; i++)
        fail = fire_triggers(ctx, db, ts, "UPDATE", true, true, s.rows[idx[i]], nt[i], set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
    if (!fail) {
        ChangeSet cs;
        memset(&cs, 0, sizeof(cs));
        cs.x = ctx;
        cs.db = db;
        for (int i = 0; i < nm && !fail; i++) {
            if (cs_add(&cs, ts, s.rids[idx[i]], s.rows[idx[i]], nt[i], false, true) < 0) fail = "Out of memory";
            else nt[i] = NULL;                                   // ora appartiene all'insieme delle modifiche
        }
        if (!fail) fail = cs_cascade(&cs, msg, sizeof(msg));
        if (!fail) fail = fire_cascaded(ctx, db, &cs, true, msg, sizeof(msg));
        if (!fail) fail = cs_validate(&cs, msg, sizeof(msg));
        if (!fail) affected = cs_apply(&cs, msg, sizeof(msg), &fail);
        for (int i = 0; i < cs.n && !fail; i++)
            if (cs.v[i].direct)
                fail = fire_triggers(ctx, db, ts, "UPDATE", false, true, cs.v[i].old, cs.v[i].nt, set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
        if (!fail) fail = fire_cascaded(ctx, db, &cs, false, msg, sizeof(msg));
        if (!fail) fail = fire_triggers(ctx, db, ts, "UPDATE", false, false, NULL, NULL, set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
        cs_free(&cs);
    }
    for (int i = 0; i < nm; i++) if (nt[i]) tuple_free(nt[i]);
    free(nt);
    free(idx);
    source_free(&s);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    return result_ok_msg(affected);
}

static QueryResult* exec_delete(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->delete_stmt.table_name)) return result_error_msg("Cannot DELETE from a view");
        return result_error_msg("Table not found");
    }
    Source s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", ts->name);
    snprintf(s.table, sizeof(s.table), "%s", ts->name);
    s.ts = ts;
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.src = &s; b.nsrc = 1; b.mode = BIND_ROW; b.clause = "WHERE"; b.x = ctx; b.outer = ctx->trig_bind;
    bind_expr(n->delete_stmt.where, &b);
    if (b.err[0]) return result_error_msg(b.err);

    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.x = ctx;
    ec.outer = ctx->trig_eval;
    int* idx = NULL;
    int nm = 0;
    char msg[300];
    const char* fail = select_matches(ctx, db, &s, n->delete_stmt.where, &ec, &idx, &nm);
    if (fail) { snprintf(msg, sizeof(msg), "%s", fail); source_free(&s); return result_error_msg(msg); }

    int affected = 0;
    ChangeSet cs;
    memset(&cs, 0, sizeof(cs));
    cs.x = ctx;
    cs.db = db;
    if (!fail) fail = fire_triggers(ctx, db, ts, "DELETE", true, false, NULL, NULL, NULL, 0, msg, sizeof(msg));
    for (int i = 0; i < nm && !fail; i++)
        fail = fire_triggers(ctx, db, ts, "DELETE", true, true, s.rows[idx[i]], NULL, NULL, 0, msg, sizeof(msg));
    for (int i = 0; i < nm && !fail; i++)
        if (cs_add(&cs, ts, s.rids[idx[i]], s.rows[idx[i]], NULL, false, true) < 0) fail = "Out of memory";
    if (!fail) fail = cs_cascade(&cs, msg, sizeof(msg));
    if (!fail) fail = fire_cascaded(ctx, db, &cs, true, msg, sizeof(msg));
    if (!fail) fail = cs_validate(&cs, msg, sizeof(msg));
    if (!fail) affected = cs_apply(&cs, msg, sizeof(msg), &fail);
    for (int i = 0; i < cs.n && !fail; i++)
        if (cs.v[i].direct) fail = fire_triggers(ctx, db, ts, "DELETE", false, true, cs.v[i].old, NULL, NULL, 0, msg, sizeof(msg));
    if (!fail) fail = fire_cascaded(ctx, db, &cs, false, msg, sizeof(msg));
    if (!fail) fail = fire_triggers(ctx, db, ts, "DELETE", false, false, NULL, NULL, NULL, 0, msg, sizeof(msg));
    cs_free(&cs);
    free(idx);
    source_free(&s);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    return result_ok_msg(affected);
}

static QueryResult* exec_use(ExecCtx* ctx, ASTNode* n) {
    if (catalog_use_db(ctx->catalog, n->use_stmt.db_name) != SV_OK)
        return result_error_msg("Database not found");
    return result_ok_msg(0);
}

// ═════════════════════════════════════════════════════════════════
//  Script DDL di una tabella e SHOW
// ═════════════════════════════════════════════════════════════════

static const char* fk_action_name(int a) {
    switch (a) {
        case FK_CASCADE: return "CASCADE";  case FK_SET_NULL: return "SET NULL";
        case FK_SET_DEFAULT: return "SET DEFAULT"; case FK_RESTRICT: return "RESTRICT";
        default: return "NO ACTION";
    }
}

typedef struct { char* p; size_t n, cap; bool ok; } TBuf;

static void tb_add(TBuf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void tb_add(TBuf* b, const char* fmt, ...) {
    if (!b->ok) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (w < 0) { b->ok = false; return; }
        if (b->p && b->n + (size_t)w < b->cap) { b->n += (size_t)w; return; }
        size_t nc = (b->cap ? b->cap : 256) * 2 + (size_t)w;
        char* np = realloc(b->p, nc);
        if (!np) { b->ok = false; return; }
        b->p = np;
        b->cap = nc;
    }
}

static void tb_cols(TBuf* b, const TableSchema* ts, const uint8_t* cols, int n) {
    for (int k = 0; k < n; k++) tb_add(b, "%s%s", k ? ", " : "", ts->columns[cols[k]].name);
}

static void tb_fk(TBuf* b, const TableSchema* ts, const Constraint* c) {
    tb_add(b, "CONSTRAINT %s FOREIGN KEY (", c->name);
    tb_cols(b, ts, c->cols, c->ncols);
    tb_add(b, ") REFERENCES %s (", c->ref_table);
    for (int k = 0; k < c->ncols; k++) tb_add(b, "%s%s", k ? ", " : "", c->ref_cols[k]);
    tb_add(b, ")");
    if (c->on_delete != FK_NO_ACTION) tb_add(b, " ON DELETE %s", fk_action_name(c->on_delete));
    if (c->on_update != FK_NO_ACTION) tb_add(b, " ON UPDATE %s", fk_action_name(c->on_update));
}

// CREATE TABLE con colonne e vincoli; with_fk = false: senza le chiavi esterne (un dump le
// aggiunge alla fine con executor_fk_statements, quando tutte le tabelle hanno i dati)
char* executor_create_statement(Catalog* cat, const char* db, const char* table, bool with_fk) {
    TableSchema* t = catalog_get_table(cat, db, table);
    if (!t) return NULL;
    TBuf b = { NULL, 0, 0, true };
    char pkname[SV_MAX_NAME_LEN + 8];
    snprintf(pkname, sizeof(pkname), "pk_%.58s", t->name);
    const Constraint* inline_pk = NULL;
    for (int i = 0; i < t->num_constraints; i++)
        if (t->constraints[i].kind == CONS_PRIMARY && t->constraints[i].ncols == 1 && !strcasecmp(t->constraints[i].name, pkname))
            inline_pk = &t->constraints[i];
    bool has_pk_cons = false;
    for (int i = 0; i < t->num_constraints; i++) if (t->constraints[i].kind == CONS_PRIMARY) has_pk_cons = true;
    tb_add(&b, "CREATE TABLE %s (", t->name);
    for (int i = 0; i < t->num_columns; i++) {
        const Column* c = &t->columns[i];
        char type[48];
        if ((c->type == SV_TYPE_VARCHAR || c->type == SV_TYPE_CHAR) && c->size > 0)
            snprintf(type, sizeof(type), "%s(%d)", datatype_name(c->type), c->size);
        else
            snprintf(type, sizeof(type), "%s", datatype_name(c->type));
        bool pk_here = (inline_pk && inline_pk->cols[0] == i) || (!has_pk_cons && c->primary_key);
        tb_add(&b, "%s\n    %s %s%s%s%s", i ? "," : "", c->name, type, pk_here ? " PRIMARY KEY" : "",
               c->auto_increment ? " AUTO_INCREMENT" : "", (!c->nullable && !c->primary_key) ? " NOT NULL" : "");
        if (c->default_val[0]) {
            bool num = c->type == SV_TYPE_INT || c->type == SV_TYPE_BIGINT || c->type == SV_TYPE_FLOAT ||
                       c->type == SV_TYPE_DECIMAL || c->type == SV_TYPE_BOOL || !strcasecmp(c->default_val, "NULL");
            tb_add(&b, num ? " DEFAULT %s" : " DEFAULT '%s'", c->default_val);
        }
    }
    for (int i = 0; i < t->num_constraints; i++) {
        const Constraint* c = &t->constraints[i];
        if (c == inline_pk || (c->kind == CONS_FOREIGN && !with_fk)) continue;
        tb_add(&b, ",\n    ");
        switch (c->kind) {
            case CONS_PRIMARY: tb_add(&b, "CONSTRAINT %s PRIMARY KEY (", c->name); tb_cols(&b, t, c->cols, c->ncols); tb_add(&b, ")"); break;
            case CONS_UNIQUE:  tb_add(&b, "CONSTRAINT %s UNIQUE (", c->name); tb_cols(&b, t, c->cols, c->ncols); tb_add(&b, ")"); break;
            case CONS_CHECK:   tb_add(&b, "CONSTRAINT %s CHECK (%s)", c->name, c->check); break;
            case CONS_FOREIGN: tb_fk(&b, t, c); break;
        }
    }
    tb_add(&b, "\n);");
    if (!b.ok) { free(b.p); return NULL; }
    return b.p;
}

// ALTER TABLE ... ADD CONSTRAINT per le chiavi esterne della tabella ("" se non ne ha)
char* executor_fk_statements(Catalog* cat, const char* db, const char* table) {
    TableSchema* t = catalog_get_table(cat, db, table);
    if (!t) return NULL;
    TBuf b = { NULL, 0, 0, true };
    tb_add(&b, "%s", "");
    for (int i = 0; i < t->num_constraints; i++) {
        if (t->constraints[i].kind != CONS_FOREIGN) continue;
        tb_add(&b, "ALTER TABLE %s ADD ", t->name);
        tb_fk(&b, t, &t->constraints[i]);
        tb_add(&b, ";\n");
    }
    if (!b.ok) { free(b.p); return NULL; }
    return b.p;
}

// risultato con colonne di testo
static QueryResult* text_result(int ncols, const char* const* names) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    r->success = true;
    r->num_cols = ncols;
    r->col_names = calloc((size_t)ncols, sizeof(char*));
    r->col_types = calloc((size_t)ncols, sizeof(char*));
    if (!r->col_names || !r->col_types) { db_result_free(r); return NULL; }
    for (int i = 0; i < ncols; i++) { r->col_names[i] = strdup(names[i]); r->col_types[i] = strdup("VARCHAR"); }
    return r;
}

static bool text_row(QueryResult* r, int* cap, const char* const* vals) {
    if (r->num_rows >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        Row* nr = realloc(r->rows, sizeof(Row) * (size_t)nc);
        if (!nr) return false;
        r->rows = nr;
        *cap = nc;
    }
    Row* row = &r->rows[r->num_rows];
    row->cells = calloc((size_t)r->num_cols, sizeof(Cell));
    if (!row->cells) return false;
    row->num_cells = r->num_cols;
    for (int i = 0; i < r->num_cols; i++) {
        Cell* c = &row->cells[i];
        if (!vals[i]) { c->is_null = true; c->type = CELL_TYPE_NULL; snprintf(c->value, sizeof(c->value), "NULL"); continue; }
        snprintf(c->value, sizeof(c->value), "%s", vals[i]);
        c->type = CELL_TYPE_STRING;
    }
    r->num_rows++;
    return true;
}

static QueryResult* exec_show_extra(ExecCtx* ctx, ASTNode* n) {
    const char* kind = n->select_stmt.table_name;
    Database* db = catalog_current_db(ctx->catalog);
    int cap = 0;
    char a[600], b2[600], c3[600];
    if (!strcmp(kind, "STATUS")) {
        static const char* const NAMES[] = { "Variable", "Value" };
        QueryResult* r = text_result(2, NAMES);
        if (!r) return result_error_msg("Out of memory");
        BufferStats* st = &ctx->bp->stats;
        int mi = 0;
        for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) mi++;
        long pages = sv_lseek(ctx->bp->db_fd, 0, SEEK_END) / SV_PAGE_SIZE;
        struct { const char* k; char v[64]; } rows[16];
        int nr = 0;
        #define STAT(name, ...) do { rows[nr].k = name; snprintf(rows[nr].v, sizeof(rows[nr].v), __VA_ARGS__); nr++; } while (0)
        STAT("page_size", "%d bytes", SV_PAGE_SIZE);
        STAT("database_pages", "%ld", pages);
        STAT("buffer_pool_frames", "%d (%d in use)", ctx->bp->capacity, ctx->bp->num_used);
        STAT("buffer_pool_policy", "CLOCK (second chance)");
        STAT("buffer_requests", "%llu", (unsigned long long)st->requests);
        STAT("buffer_hits", "%llu", (unsigned long long)st->hits);
        STAT("buffer_misses", "%llu (pages read from disk)", (unsigned long long)st->misses);
        STAT("buffer_hit_ratio", "%.2f%%", st->requests ? 100.0 * (double)st->hits / (double)st->requests : 100.0);
        STAT("buffer_evictions", "%llu", (unsigned long long)st->evictions);
        STAT("pages_written", "%llu", (unsigned long long)st->dirty_writes);
        STAT("pages_allocated", "%llu", (unsigned long long)st->new_pages);
        STAT("wal_size", "%ld bytes", ctx->wal ? wal_size(ctx->wal) : 0L);
        STAT("wal_records", "%llu", ctx->wal ? (unsigned long long)ctx->wal->sequence : 0ULL);
        STAT("checkpoints", "%llu", ctx->wal ? (unsigned long long)ctx->wal->checkpoints : 0ULL);
        STAT("in_memory_indexes", "%d", mi);
        if (ctx->tx_id) STAT("transaction", "%llu (open)", (unsigned long long)ctx->tx_id);
        else            STAT("transaction", "none (autocommit)");
        #undef STAT
        for (int i = 0; i < nr; i++) {
            const char* v[2] = { rows[i].k, rows[i].v };
            if (!text_row(r, &cap, v)) { db_result_free(r); return result_error_msg("Out of memory"); }
        }
        return r;
    }
    if (!db) return result_error_msg("No database selected");
    if (!strcmp(kind, "TRIGGERS")) {
        static const char* const NAMES[] = { "Trigger", "Timing", "Event", "Table", "Definition" };
        QueryResult* r = text_result(5, NAMES);
        if (!r) return result_error_msg("Out of memory");
        for (int i = 0; i < db->num_triggers; i++) {
            TriggerSchema* tg = &db->triggers[i];
            if (n->select_stmt.table_alias[0] && strcasecmp(n->select_stmt.table_alias, tg->table_name)) continue;
            const char* v[5] = { tg->name, tg->before ? "BEFORE" : "AFTER", tg->event, tg->table_name, tg->body };
            if (!text_row(r, &cap, v)) { db_result_free(r); return result_error_msg("Out of memory"); }
        }
        return r;
    }
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->select_stmt.table_alias);
    if (!ts) { snprintf(a, sizeof(a), "Table '%.64s' not found", n->select_stmt.table_alias); return result_error_msg(a); }
    if (!strcmp(kind, "CREATE")) {
        static const char* const NAMES[] = { "Table", "Create Table" };
        QueryResult* r = text_result(2, NAMES);
        char* ddl = executor_create_statement(ctx->catalog, db->name, ts->name, true);
        const char* v[2] = { ts->name, ddl ? ddl : "" };
        if (!r || !text_row(r, &cap, v)) { free(ddl); db_result_free(r); return result_error_msg("Out of memory"); }
        free(ddl);
        return r;
    }
    if (!strcmp(kind, "CONSTRAINTS")) {
        static const char* const NAMES[] = { "Name", "Type", "Columns", "References", "On delete", "On update", "Check" };
        QueryResult* r = text_result(7, NAMES);
        if (!r) return result_error_msg("Out of memory");
        bool pk_cons = false;
        for (int i = 0; i < ts->num_constraints; i++) if (ts->constraints[i].kind == CONS_PRIMARY) pk_cons = true;
        for (int i = 0; !pk_cons && i < ts->num_columns; i++)
            if (ts->columns[i].primary_key) {                    // formato precedente: colonna PRIMARY KEY
                const char* v[7] = { "PRIMARY", "PRIMARY KEY", ts->columns[i].name, NULL, NULL, NULL, NULL };
                text_row(r, &cap, v);
                break;
            }
        for (int i = 0; i < ts->num_constraints; i++) {
            const Constraint* c = &ts->constraints[i];
            TBuf cb = { NULL, 0, 0, true };
            tb_cols(&cb, ts, c->cols, c->ncols);
            snprintf(a, sizeof(a), "%s", cb.p ? cb.p : "");
            free(cb.p);
            b2[0] = '\0';
            if (c->kind == CONS_FOREIGN) {
                size_t len = (size_t)snprintf(b2, sizeof(b2), "%.64s (", c->ref_table);
                for (int k = 0; k < c->ncols && len < sizeof(b2); k++) len += (size_t)snprintf(b2 + len, sizeof(b2) - len, "%s%.64s", k ? ", " : "", c->ref_cols[k]);
                if (len < sizeof(b2)) snprintf(b2 + len, sizeof(b2) - len, ")");
            }
            snprintf(c3, sizeof(c3), "%s", c->check);
            const char* type = c->kind == CONS_PRIMARY ? "PRIMARY KEY" : c->kind == CONS_UNIQUE ? "UNIQUE" :
                               c->kind == CONS_FOREIGN ? "FOREIGN KEY" : "CHECK";
            bool fk = c->kind == CONS_FOREIGN;
            const char* v[7] = { c->name, type, c->kind == CONS_CHECK ? NULL : a, fk ? b2 : NULL,
                                 fk ? fk_action_name(c->on_delete) : NULL, fk ? fk_action_name(c->on_update) : NULL,
                                 c->kind == CONS_CHECK ? c3 : NULL };
            if (!text_row(r, &cap, v)) { db_result_free(r); return result_error_msg("Out of memory"); }
        }
        return r;
    }
    // INDEXES: chiavi, indici dichiarati e chiavi esterne, con lo stato dell'indice in memoria
    static const char* const NAMES[] = { "Name", "Columns", "Kind", "Built", "Distinct keys" };
    QueryResult* r = text_result(5, NAMES);
    if (!r) return result_error_msg("Out of memory");
    struct { char name[SV_MAX_NAME_LEN]; int cols[SV_MAX_KEY_COLS]; int n; const char* kind; } ix[SV_MAX_CONSTRAINTS * 2 + SV_MAX_INDEXES + 1];
    int nix = 0;
    KeyDef keys[SV_MAX_CONSTRAINTS + 1];
    int nk = table_keys(ts, keys, SV_MAX_CONSTRAINTS + 1);
    for (int k = 0; k < nk; k++) {
        snprintf(ix[nix].name, sizeof(ix[nix].name), "%s", keys[k].name);
        memcpy(ix[nix].cols, keys[k].cols, sizeof(int) * (size_t)keys[k].n);
        ix[nix].n = keys[k].n;
        ix[nix].kind = keys[k].primary ? "PRIMARY KEY (hash)" : "UNIQUE (hash)";
        nix++;
    }
    for (int i = 0; i < ts->num_indexes; i++) {
        int ci = catalog_get_col_index(ts, ts->indexes[i].col_name);
        if (ci < 0) continue;
        snprintf(ix[nix].name, sizeof(ix[nix].name), "%s", ts->indexes[i].name);
        ix[nix].cols[0] = ci; ix[nix].n = 1; ix[nix].kind = "INDEX (hash)";
        nix++;
    }
    for (int i = 0; i < ts->num_constraints; i++) {
        const Constraint* c = &ts->constraints[i];
        if (c->kind != CONS_FOREIGN) continue;
        snprintf(ix[nix].name, sizeof(ix[nix].name), "%s", c->name);
        for (int k = 0; k < c->ncols; k++) ix[nix].cols[k] = c->cols[k];
        ix[nix].n = c->ncols; ix[nix].kind = "FOREIGN KEY checks (hash)";
        nix++;
    }
    for (int i = 0; i < ts->num_columns; i++)
        if (ts->columns[i].auto_increment) {
            snprintf(ix[nix].name, sizeof(ix[nix].name), "auto_increment");
            ix[nix].cols[0] = i; ix[nix].n = 1; ix[nix].kind = "AUTO_INCREMENT (hash)";
            nix++;
            break;
        }
    for (int i = 0; i < nix; i++) {
        size_t len = 0;
        a[0] = '\0';
        for (int k = 0; k < ix[i].n && len < sizeof(a); k++)
            len += (size_t)snprintf(a + len, sizeof(a) - len, "%s%s", k ? ", " : "", ts->columns[ix[i].cols[k]].name);
        // indice costruito? (nome come in mi_get_cols)
        char nm[SV_MAX_NAME_LEN * 2];
        size_t nl = 0;
        nm[0] = '\0';
        for (int k = 0; k < ix[i].n && nl < sizeof(nm); k++)
            nl += (size_t)snprintf(nm + nl, sizeof(nm) - nl, "%s%s", k ? "," : "", ts->columns[ix[i].cols[k]].name);
        MemIndex* mi = NULL;
        for (MemIndex* m = g_mem_indexes; m; m = m->next)
            if (!strcasecmp(m->db, db->name) && !strcasecmp(m->table, ts->name) && !strcasecmp(m->col, nm)) mi = m;
        snprintf(b2, sizeof(b2), "%d", mi ? (int)mi->map.n : 0);
        const char* v[5] = { ix[i].name, a, ix[i].kind, mi ? "yes" : "no (built on first use)", mi ? b2 : NULL };
        if (!text_row(r, &cap, v)) { db_result_free(r); return result_error_msg("Out of memory"); }
    }
    return r;
}

static QueryResult* exec_show(ExecCtx* ctx, ASTNode* n) {
    if (strcmp(n->select_stmt.table_name, "TABLES") && strcmp(n->select_stmt.table_name, "DATABASES"))
        return exec_show_extra(ctx, n);
    bool tables = strcmp(n->select_stmt.table_name, "TABLES") == 0;
    Database* db = catalog_current_db(ctx->catalog);
    if (tables && !db) return result_error_msg("No database selected");
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;

    if (!tables) {
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
        const char* key = cols[i].primary_key ? "PRI" : "";
        TableSchema* dts = catalog_get_table(ctx->catalog, db->name, tname);
        int ci = dts ? catalog_get_col_index(dts, cols[i].name) : -1;
        for (int k = 0; !key[0] && dts && ci >= 0 && k < dts->num_constraints; k++) {
            const Constraint* c = &dts->constraints[k];
            if (c->kind == CONS_UNIQUE && c->ncols == 1 && c->cols[0] == ci) key = "UNI";
        }
        for (int k = 0; !key[0] && dts && ci >= 0 && k < dts->num_constraints; k++) {
            const Constraint* c = &dts->constraints[k];
            for (int j = 0; (c->kind == CONS_FOREIGN || c->kind == CONS_UNIQUE) && j < c->ncols; j++) if (c->cols[j] == ci) key = "MUL";
        }
        snprintf(r->rows[i].cells[3].value, sizeof(r->rows[i].cells[3].value), "%s", key);
    }
    db_columns_free(cols, num_cols);
    return r;
}

static QueryResult* exec_create_index(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->create_index.table_name);
    if (!ts) return result_error_msg("Table not found");

    if (catalog_get_col_index(ts, n->create_index.col_name) < 0)
        return unknown_column_error(n->create_index.col_name, "CREATE INDEX");
    for (int i = 0; i < ts->num_indexes; i++)
        if (!strcasecmp(ts->indexes[i].name, n->create_index.index_name))
            return result_error_msg("An index with this name already exists on the table");
    if (ts->num_indexes >= SV_MAX_INDEXES) return result_error_msg("Too many indexes on this table (max 16)");

    Index idx;
    memset(&idx, 0, sizeof(idx));
    snprintf(idx.name, sizeof(idx.name), "%s", n->create_index.index_name);
    snprintf(idx.col_name, sizeof(idx.col_name), "%s", n->create_index.col_name);
    snprintf(idx.type, sizeof(idx.type), "BTREE");

    if (catalog_add_index(ctx->catalog, db->name, n->create_index.table_name, &idx) != SV_OK)
        return result_error_msg("Failed to create index");

    // l'indice in memoria viene costruito dalle righe al primo utilizzo
    mi_invalidate_table(db->name, ts->name);

    return result_ok_msg(0);
}

static QueryResult* exec_drop_index(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->drop_index.table_name);
    if (!ts) return result_error_msg("Table not found");
    for (int i = 0; i < ts->num_indexes; i++) {
        if (strcasecmp(ts->indexes[i].name, n->drop_index.index_name) == 0) {
            mi_invalidate_table(db->name, ts->name);
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
    if (catalog_get_table(ctx->catalog, db->name, vs.name) || catalog_get_view(ctx->catalog, db->name, vs.name)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Table or view '%.64s' already exists", vs.name);
        return result_error_msg(msg);
    }
    if (catalog_add_view(ctx->catalog, db->name, &vs) != SV_OK)
        return result_error_msg("Failed to create view (too many views?)");
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
    const TableSchema* child = NULL;
    const Constraint* ref = referenced_by(db, ts, &child);
    if (ref) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Cannot truncate '%.64s': it is referenced by the foreign key '%.64s' of table '%.64s' (use DELETE)",
                 ts->name, ref->name, child->name);
        return result_error_msg(msg);
    }
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
    mi_invalidate_table(db->name, ts->name);
    return result_ok_msg(affected);
}

static QueryResult* exec_explain(ExecCtx* ctx, ASTNode* n) {
    ASTNode* stmt = (ASTNode*)n->explain.stmt;
    if (!stmt || stmt->type != NODE_SELECT) return result_error_msg("EXPLAIN works with SELECT statements");
    return run_select_ex(ctx, stmt, 0, true, n->explain.analyze);
}

// IF EXISTS / IF NOT EXISTS: l'oggetto manca (DROP) o c'è già (CREATE) -> nulla da fare
static bool if_exists_noop(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    const char* dbn = db ? db->name : "";
    switch (n->type) {
        case NODE_DROP_DB:      return !catalog_get_db(ctx->catalog, n->drop_db.db_name);
        case NODE_CREATE_DB:    return catalog_get_db(ctx->catalog, n->create_db.db_name) != NULL;
        case NODE_DROP_TABLE:   return db && !catalog_get_table(ctx->catalog, dbn, n->drop_table.table_name);
        case NODE_CREATE_TABLE: return db && catalog_get_table(ctx->catalog, dbn, n->create_table.table_name);
        case NODE_DROP_VIEW:    return db && !catalog_get_view(ctx->catalog, dbn, n->drop_view.view_name);
        case NODE_CREATE_VIEW:  return db && catalog_get_view(ctx->catalog, dbn, n->create_view.view_name);
        case NODE_DROP_TRIGGER: case NODE_CREATE_TRIGGER: {
            if (!db) return false;
            const char* nm = n->type == NODE_DROP_TRIGGER ? n->drop_trigger.name : n->create_trigger.name;
            bool found = false;
            for (int i = 0; i < db->num_triggers; i++) if (!strcasecmp(db->triggers[i].name, nm)) found = true;
            return n->type == NODE_DROP_TRIGGER ? !found : found;
        }
        case NODE_DROP_INDEX: case NODE_CREATE_INDEX: {
            if (!db) return false;
            const char* tn = n->type == NODE_DROP_INDEX ? n->drop_index.table_name : n->create_index.table_name;
            const char* in = n->type == NODE_DROP_INDEX ? n->drop_index.index_name : n->create_index.index_name;
            TableSchema* ts = catalog_get_table(ctx->catalog, dbn, tn);
            bool found = false;
            for (int i = 0; ts && i < ts->num_indexes; i++) if (!strcasecmp(ts->indexes[i].name, in)) found = true;
            return n->type == NODE_DROP_INDEX ? !found : found;
        }
        default: return false;
    }
}

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return result_error_msg("Invalid executor context");
    if (stmt->if_exists && if_exists_noop(ctx, stmt)) return result_ok_msg(0);

    switch (stmt->type) {
        case NODE_CREATE_DB:    return exec_create_db(ctx, stmt);
        case NODE_DROP_DB:      return exec_drop_db(ctx, stmt);
        case NODE_CREATE_TABLE: return exec_create_table(ctx, stmt);
        case NODE_DROP_TABLE:   return exec_drop_table(ctx, stmt);
        case NODE_ALTER_TABLE:  return exec_alter_table(ctx, stmt);
        case NODE_INSERT:       return exec_insert(ctx, stmt);
        case NODE_SELECT:       return run_select(ctx, stmt, 0, false);
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
        case NODE_CREATE_TRIGGER: return exec_create_trigger(ctx, stmt);
        case NODE_CHECKPOINT:   return result_ok_msg(0);
        case NODE_DROP_TRIGGER: return exec_drop_trigger(ctx, stmt);
        default:
            return result_error_msg("Unsupported statement type");
    }
}
