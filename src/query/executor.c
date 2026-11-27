#include "common.h"
#include "sv_alloc.h"
#include "sv_strbuf.h"
#include "executor.h"
#include "u8case.h"
#include "../platform_lock.h"
#include "../table/overflow.h"
#include "../table/binesc.h"
#include "sqljson.h"
#include "regex.h"
#include "stats.h"
#include "decimal.h"
#include "../bridge/auth.h"
#include "platform_compat.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
static int new_root_page(ExecCtx* ctx, uint32_t* pid);     // radice nuova registrata nel log (P1.11)
static int tree_for(const TableSchema* ts, int col);        // albero su disco valido della colonna (P1.6)
#include <strings.h>
#include <limits.h>
#include <errno.h>
#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
static void sv_win_sleep(unsigned long ms) { Sleep(ms); }
#endif

static QueryResult* result_error_msg(const char* msg) {
    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
    r->success = false;
    snprintf(r->error, sizeof(r->error), "%s", msg);
    return r;
}

static QueryResult* result_ok_msg(int affected) {
    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
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

// LIKE con % (qualsiasi sequenza) e _ (un carattere), senza distinzione maiuscole/minuscole;
// esc (0 = nessuno): il carattere che lo segue nel pattern vale per sé (ESCAPE '!': '100!%').
// Iterativo: a un errore si torna solo all'ultimo % (basta: % prende qualunque sequenza), tempo O(|s|·|p|).
// Prima una ricorsione per ogni %: '%a%a%a…%b' su 40 'a' non finiva mai, 50 000 % facevano traboccare lo stack
static bool like_match_cs(const char* s, const char* p, char esc, bool cs) {
    const char *star_p = NULL, *star_s = NULL;
    for (;;) {
        if (*p == '%') {
            while (*p == '%') p++;
            if (!*p) return true;
            star_p = p;
            star_s = s;
            continue;
        }
        if (!*s) break;
        bool lit = esc && *p == esc && p[1];
        const char* pc = lit ? p + 1 : p;
        if (*p && !lit && *p == '_') { s += u8_char_len(s); p++; continue; }   // un carattere, non un byte
        int np, ns;
        uint32_t cp = u8_get(pc, &np), cs_ = u8_get(s, &ns);
        if (*p && (cs ? cp == cs_ : u8_ci_cp(cp) == u8_ci_cp(cs_))) {   // 'À' = 'a' come le collation _ci; cs: BINARY / _bin
            s += ns;
            p = pc + np;
            continue;
        }
        if (!star_p) return false;
        p = star_p;
        star_s += u8_char_len(star_s);
        s = star_s;
    }
    while (*p == '%') p++;
    return !*p;
}

static bool like_match_esc(const char* s, const char* p, char esc) { return like_match_cs(s, p, esc, false); }
// LIKE: come MySQL e PostgreSQL, \\ protegge % e _ (like_match_cs con esc = '\\')

static QueryResult* unknown_column_error(const char* col, const char* clause) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Unknown column '%.64s' in %s", col, clause);
    return result_error_msg(msg);
}

// nome non ammesso per un oggetto nuovo (sv_name_ok); il nome non viene ripetuto (può avere caratteri di controllo)
static QueryResult* bad_name_error(const char* what) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Invalid %s name: it cannot be empty or contain a backtick (`) or a control character", what);
    return result_error_msg(msg);
}

// ---- Executors ----
static QueryResult* exec_create_db(ExecCtx* ctx, ASTNode* n) {
    if (!sv_name_ok(n->create_db.db_name)) return bad_name_error("database");
    if (catalog_add_db(ctx->catalog, n->create_db.db_name) != SV_OK)
        return result_error_msg("Failed to create database (may already exist)");
    return result_ok_msg(0);
}

void executor_drop_db_sequences(const char* db);
static QueryResult* exec_drop_db(ExecCtx* ctx, ASTNode* n) {
    executor_invalidate_indexes();
    if (catalog_drop_db(ctx->catalog, n->drop_db.db_name) != SV_OK)
        return result_error_msg("Database not found");
    executor_drop_db_sequences(n->drop_db.db_name);
    return result_ok_msg(0);
}

static const char* add_constraint(ExecCtx* ctx, Database* db, TableSchema* ts, ConstraintDef* d, char* msg, size_t mn);
static QueryResult* exec_drop_table(ExecCtx* ctx, ASTNode* n);
static const char* check_value(const Column* c, const char* v, char* msg, size_t n);

// DEFAULT CURRENT_TIMESTAMP / NOW() / LOCALTIMESTAMP / CURRENT_DATE: valore calcolato a ogni INSERT
static bool is_default_expr(const ASTExpr* e) { return e && e->type == EXPR_FUNC && !strcmp(e->func, "__DEFAULT"); }

static bool default_is_now(const char* d) {
    return !strcasecmp(d, "CURRENT_TIMESTAMP") || !strcasecmp(d, "NOW") || !strcasecmp(d, "LOCALTIMESTAMP") ||
           !strcasecmp(d, "LOCALTIME") || !strcasecmp(d, "CURRENT_DATE") || !strcasecmp(d, "CURDATE") ||
           !strcasecmp(d, "UTC_TIMESTAMP");
}

// ora attuale come testo per una colonna DATE / DATETIME (UTC_TIMESTAMP: in UTC)
static void col_now_text(const char* fn, DataType t, char* out, size_t n) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    if (fn && !strcasecmp(fn, "UTC_TIMESTAMP")) gmtime_s(&tmv, &now); else localtime_s(&tmv, &now);
#else
    if (fn && !strcasecmp(fn, "UTC_TIMESTAMP")) gmtime_r(&now, &tmv); else localtime_r(&now, &tmv);
#endif
    strftime(out, n, t == SV_TYPE_DATE ? "%Y-%m-%d" : "%Y-%m-%d %H:%M:%S", &tmv);
}

// numero decimale arrotondato a scale cifre (metà lontano da zero, come MySQL), senza passare dal double
static bool decimal_round(const char* in, int scale, char* out, size_t n) {
    // (scala dalla definizione della colonna: DECIMAL(10, 200) riempiva fp[80] oltre la fine; MySQL arriva a 30)
    if (scale < 0 || scale > 60) return false;
    while (isspace((unsigned char)*in)) in++;
    bool neg = *in == '-';
    if (*in == '-' || *in == '+') in++;
    char ip[80], fp[80];
    size_t il = 0, fl = 0;
    while (isdigit((unsigned char)*in) && il < sizeof(ip) - 2) ip[il++] = *in++;
    if (*in == '.') { in++; while (isdigit((unsigned char)*in) && fl < sizeof(fp) - 2) fp[fl++] = *in++; }
    while (isspace((unsigned char)*in)) in++;
    if (*in || (il == 0 && fl == 0)) return false;                 // esponenti e altro: lasciati come sono
    if (il == 0) ip[il++] = '0';
    while (fl < (size_t)scale) fp[fl++] = '0';
    bool up = fl > (size_t)scale && fp[scale] >= '5';
    char digits[170];
    size_t dl = 0;
    for (size_t i = 0; i < il; i++) digits[dl++] = ip[i];
    for (int i = 0; i < scale; i++) digits[dl++] = fp[i];
    for (size_t i = dl; up && i-- > 0; ) {                         // riporto
        if (digits[i] == '9') { digits[i] = '0'; if (i == 0) { memmove(digits + 1, digits, dl++); digits[0] = '1'; il++; up = false; } }
        else { digits[i]++; up = false; }
    }
    size_t lead = 0;                                               // zeri iniziali inutili
    while (lead + 1 < dl - (size_t)scale && digits[lead] == '0') lead++;
    bool zero = true;
    for (size_t i = lead; i < dl; i++) if (digits[i] != '0') zero = false;
    size_t o = 0;
    if (neg && !zero && o + 1 < n) out[o++] = '-';
    for (size_t i = lead; i < dl - (size_t)scale && o + 1 < n; i++) out[o++] = digits[i];
    if (scale > 0 && o + 1 < n) out[o++] = '.';
    for (size_t i = dl - (size_t)scale; i < dl && o + 1 < n; i++) out[o++] = digits[i];
    out[o] = '\0';
    return true;
}

// valore di una colonna da un testo, con le regole della colonna (DECIMAL(p, s): s decimali)
static void column_set_from_text(Value* v, const Column* c, const char* text) {
    char buf[200];
    if (text && c->type == SV_TYPE_DECIMAL && c->decimal_prec > 0 && decimal_round(text, c->decimal_prec - 1, buf, sizeof(buf))) text = buf;
    value_set_from_text(v, c->type, text);
}
static const Constraint* referenced_by(Database* db, const TableSchema* ts, const TableSchema** child);

static void rt_temp_created(const char* db, const char* table);
static bool rt_is_temp(const char* db, const char* table);
static QueryResult* exec_create_table_impl(ExecCtx* ctx, ASTNode* n);
static const char* gen_compute(ExecCtx* ctx, TableSchema* ts, Tuple* t, char* msg, size_t mn);
static bool has_generated(const TableSchema* ts);
static QueryResult* exec_create_table_as(ExecCtx* ctx, ASTNode* n);
static QueryResult* exec_create_table_like(ExecCtx* ctx, ASTNode* n);
static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n);
static QueryResult* run_select(ExecCtx* ctx, ASTNode* n, int depth, bool explain);

// CREATE [TEMPORARY] TABLE: una tabella TEMPORARY creata in una routine sparisce alla fine della CALL
// più esterna (fuori dalle routine è una tabella normale, da eliminare con DROP)
static QueryResult* exec_create_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    bool temp = n->create_table.temporary && ctx->routine;
    if (temp && rt_is_temp(db->name, n->create_table.table_name) && catalog_get_table(ctx->catalog, db->name, n->create_table.table_name)) {
        ASTNode* dn = sv_calloc(1, sizeof(ASTNode));
        if (!dn) return result_error_msg("Out of memory");
        dn->type = NODE_DROP_TABLE;
        snprintf(dn->drop_table.table_name, sizeof(dn->drop_table.table_name), "%s", n->create_table.table_name);
        db_result_free(exec_drop_table(ctx, dn));
        free(dn);
    }
    QueryResult* r = n->create_table.as_select ? exec_create_table_as(ctx, n)
                   : n->create_table.like_table[0] ? exec_create_table_like(ctx, n)
                   : exec_create_table_impl(ctx, n);
    if (temp && r && r->success) rt_temp_created(db->name, n->create_table.table_name);
    return r;
}

// CREATE TABLE t AS SELECT ...: colonne con i nomi e i tipi del risultato, poi le righe
static QueryResult* exec_create_table_as(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    char msg[300];
    if (catalog_get_table(ctx->catalog, db->name, n->create_table.table_name) || catalog_get_view(ctx->catalog, db->name, n->create_table.table_name)) {
        if (n->if_exists) return result_ok_msg(0);
        snprintf(msg, sizeof(msg), "Table or view '%.64s' already exists", n->create_table.table_name);
        return result_error_msg(msg);
    }
    ASTNode* q = n->create_table.as_select;
    QueryResult* r = run_select(ctx, q, 0, false);
    if (!r || !r->success) return r ? r : result_error_msg("Out of memory");
    ASTNode* cn = sv_calloc(1, sizeof(ASTNode));
    if (!cn) { db_result_free(r); return result_error_msg("Out of memory"); }
    cn->type = NODE_CREATE_TABLE;
    snprintf(cn->create_table.table_name, sizeof(cn->create_table.table_name), "%s", n->create_table.table_name);
    for (int i = 0; i < r->num_cols && i < SV_MAX_COLUMNS; i++) {
        ColumnDef* d = ast_create_new_col(cn);
        if (!d) break;
        cn->create_table.num_columns++;
        const char* nm = r->col_names[i] ? r->col_names[i] : "";
        const char* dot = strrchr(nm, '.');                      // "t.col" -> "col"
        snprintf(d->name, sizeof(d->name), "%s", dot && dot[1] ? dot + 1 : nm[0] ? nm : "col");
        const char* ty = r->col_types && r->col_types[i] && r->col_types[i][0] ? r->col_types[i] : "TEXT";
        // testi senza lunghezza nota: TEXT; tipi sconosciuti (JSON, ...): TEXT
        if (!strcasecmp(ty, "VARCHAR") || !strcasecmp(ty, "CHAR") || !known_type(ty)) ty = "TEXT";
        snprintf(d->type, sizeof(d->type), "%s", ty);
        d->nullable = true;
    }
    db_result_free(r);
    QueryResult* cr = exec_create_table_impl(ctx, cn);
    free(cn->create_table.columns);
    free(cn);
    if (!cr || !cr->success) return cr;
    db_result_free(cr);
    ASTNode* ins = sv_calloc(1, sizeof(ASTNode));
    if (!ins) return result_error_msg("Out of memory");
    ins->type = NODE_INSERT;
    snprintf(ins->insert_stmt.table_name, sizeof(ins->insert_stmt.table_name), "%s", n->create_table.table_name);
    ins->insert_stmt.select = q;
    QueryResult* ir = exec_insert(ctx, ins);
    ins->insert_stmt.select = NULL;
    free(ins);
    if (!ir || !ir->success) {                                   // niente tabella a metà
        ASTNode* dn = sv_calloc(1, sizeof(ASTNode));
        if (dn) {
            dn->type = NODE_DROP_TABLE;
            snprintf(dn->drop_table.table_name, sizeof(dn->drop_table.table_name), "%s", n->create_table.table_name);
            db_result_free(exec_drop_table(ctx, dn));
            free(dn);
        }
    }
    return ir;
}

// CREATE TABLE t LIKE s: la stessa struttura (colonne, chiavi, indici, CHECK), senza righe né chiavi esterne
static QueryResult* exec_create_table_like(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    char msg[300];
    if (!executor_priv_ok(ctx, db->name, n->create_table.like_table, AUTH_SELECT, msg, sizeof(msg))) return result_error_msg(msg);
    if (!catalog_get_table(ctx->catalog, db->name, n->create_table.like_table)) {
        snprintf(msg, sizeof(msg), "Table '%.64s' not found", n->create_table.like_table);
        return result_error_msg(msg);
    }
    char* sql = executor_create_statement(ctx->catalog, db->name, n->create_table.like_table, false);
    if (!sql) return result_error_msg("Out of memory");
    char* paren = strchr(sql, '(');
    size_t need = strlen(sql) + 200;
    char* text = paren ? sv_malloc(need) : NULL;
    if (!text) { free(sql); return result_error_msg("Out of memory"); }
    snprintf(text, need, "CREATE TABLE %s`%s` %s", n->if_exists ? "IF NOT EXISTS " : "", n->create_table.table_name, paren);
    free(sql);
    TokenList* tk = lexer_tokenize(text);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    QueryResult* r;
    if (!pr || !pr->success || pr->count != 1) r = result_error_msg(pr ? pr->error : "Out of memory");
    else r = exec_create_table(ctx, pr->statements[0]);
    parse_result_free(pr);
    lexer_free(tk);
    free(text);
    return r;
}

static QueryResult* exec_create_table_impl(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    char msg[300];
    if (n->create_table.num_columns == 0) return result_error_msg("A table needs at least one column");
    if (n->create_table.num_columns > SV_MAX_COLUMNS) return result_error_msg("Too many columns (max 1024)");
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

    TableSchema* ts = schema_new_aux(n->create_table.num_columns, 0, SV_MAX_CONSTRAINTS);   // i vincoli si aggiungono qui
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
        snprintf(col->sqltype, sizeof(col->sqltype), "%s", cd->sqltype);
        col->flags = cd->flags;
        if (cd->comment[0] && cd->comment[0] != '\x01') snprintf(col->comment, sizeof(col->comment), "%s", cd->comment);
        // DECIMAL(p, s): s decimali (decimal_prec = s + 1; 0 = come scritto, tabelle di prima)
        if (col->type == SV_TYPE_DECIMAL) col->decimal_prec = cd->has_scale ? cd->scale + 1 : cd->size > 0 ? 1 : 0;
        if (cd->default_val[0])
            snprintf(col->default_val, sizeof(col->default_val), "%s", cd->default_val);
        col->has_default = cd->has_default && strcasecmp(cd->default_val, "NULL");       // anche DEFAULT ''
        if (col->has_default && !default_is_now(cd->default_val) &&
            check_value(col, cd->default_val, msg, sizeof(msg))) {
            free(ts);
            return result_error_msg(msg);
        }
        // ENUM('a','b'): un CHECK con i valori ammessi
        if (cd->comment[0] == '\x01') {
            ConstraintDef* d = ast_cons_add(n);                  // (prima, a lista piena, l'ENUM restava senza controllo)
            if (!d) { free(ts); snprintf(msg, sizeof(msg), "Too many constraints (max %d, ENUM columns count as one each)", SV_MAX_CONS_DEFS); return result_error_msg(msg); }
            d->kind = CONS_CHECK;
            snprintf(d->name, sizeof(d->name), "%.50s_chk_enum", cd->name);
            snprintf(d->check, sizeof(d->check), "`%.64s` IN %.420s", cd->name, cd->comment + 1);
        }
    }
    if (n->create_table.collation[0]) snprintf(ts->collation, sizeof(ts->collation), "%s", n->create_table.collation);
    if (n->create_table.comment[0]) snprintf(ts->comment, sizeof(ts->comment), "%s", n->create_table.comment);
    if (n->create_table.auto_increment > 0) ts->auto_increment = (uint64_t)n->create_table.auto_increment;

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

    if (has_generated(ts)) {                                     // colonne calcolate: espressioni valide sulle colonne
        Tuple* probe = tuple_create(ts->num_columns);
        for (int i = 0; probe && i < probe->num_values; i++) probe->values[i].is_null = true;
        const char* gf = probe ? gen_compute(ctx, ts, probe, msg, sizeof(msg)) : "Out of memory";
        if (probe) tuple_free(probe);
        if (gf && strstr(gf, "cannot be NULL")) gf = NULL;    // su una riga vuota il valore può mancare
        if (gf) { if (gf != msg) snprintf(msg, sizeof(msg), "%s", gf); free(ts); return result_error_msg(msg); }
    }
    for (int k = 0; k < n->create_table.num_keys; k++)
        if (!sv_name_ok_opt(n->create_table.keys[k].name)) { free(ts); return bad_name_error("index"); }
    int rc = catalog_add_table(ctx->catalog, db->name, ts);
    free(ts);
    if (rc == SV_BAD_NAME) return bad_name_error("table, column or constraint");
    if (rc != SV_OK) return result_error_msg("Failed to create table");

    TableSchema* saved = catalog_get_table(ctx->catalog, db->name, n->create_table.table_name);
    if (saved) {
        uint32_t pid;
        if (new_root_page(ctx, &pid) == SV_OK) {
            saved->root_page_id = pid;
            saved->num_pages = 1;
        }
        // KEY / INDEX / FULLTEXT di MySQL: indici (si cerca con la prima colonna)
        for (int k = 0; k < n->create_table.num_keys && saved->num_indexes < SV_MAX_INDEXES; k++) {
            const TableKeyDef* kd = &n->create_table.keys[k];
            bool ok = kd->ncols > 0;
            for (int c = 0; c < kd->ncols && ok; c++) ok = catalog_get_col_index(saved, kd->cols[c]) >= 0;
            if (!ok) {
                snprintf(msg, sizeof(msg), "Unknown column in KEY '%.64s'", kd->name);
                catalog_drop_table(ctx->catalog, db->name, saved->name);
                return result_error_msg(msg);
            }
            Index* ix = schema_add_index(saved);
            if (!ix) { catalog_drop_table(ctx->catalog, db->name, saved->name); return result_error_msg("Out of memory"); }
            if (kd->name[0]) snprintf(ix->name, sizeof(ix->name), "%s", kd->name);
            else snprintf(ix->name, sizeof(ix->name), "%.63s", kd->cols[0]);
            for (int j = 0; j < saved->num_indexes - 1; j++)             // nomi automatici distinti (col, col_2, ...)
                if (!strcasecmp(saved->indexes[j].name, ix->name)) { snprintf(ix->name, sizeof(ix->name), "%.57s_%d", kd->cols[0], (k + 2) % 100000); break; }
            snprintf(ix->col_name, sizeof(ix->col_name), "%s", kd->cols[0]);
            snprintf(ix->type, sizeof(ix->type), "BTREE");
            ix->ncols = (uint8_t)kd->ncols;
            ix->fulltext = kd->fulltext;
            for (int c = 0; c < kd->ncols; c++) snprintf(ix->cols[c], sizeof(ix->cols[c]), "%s", kd->cols[c]);
        }
    }
    return result_ok_msg(0);
}

// pagina radice nuova di una tabella, registrata nel log (LOG_LINK con pagina precedente 0): dopo una caduta il
// recupero la ricrea vuota anche se non era mai arrivata su disco (P1.11: il DDL non scrive più tutte le pagine)
static int new_root_page(ExecCtx* ctx, uint32_t* pid) {
    if (bp_new_page(ctx->bp, pid) != SV_OK) return SV_FULL;
    if (ctx->bp->on_link) ctx->bp->on_link(ctx->bp->on_link_ctx, 0, *pid);
    bp_unpin(ctx->bp, *pid, true);
    return SV_OK;
}

static void (*g_drop_hook)(uint32_t root);
void executor_set_drop_hook(void (*hook)(uint32_t root)) { g_drop_hook = hook; }

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
    int npages = 0;
    uint32_t droot = dts ? dts->root_page_id : 0;
    if (dts && g_drop_hook) heap_cache_forget(ctx->bp, droot);
    else if (dts) {                              // le pagine della tabella diventano spazio abbandonato (VACUUM automatico)
        heap_cache_forget(ctx->bp, dts->root_page_id);
        for (uint32_t pid = dts->root_page_id, guard = 0; pid && guard < (1u << 24); guard++) {
            Page* pg = bp_fetch(ctx->bp, pid);
            if (!pg) break;
            uint32_t nx = page_next(pg);
            bp_unpin(ctx->bp, pid, false);
            npages++;
            pid = nx;
        }
    }
    if (catalog_drop_table(ctx->catalog, db->name, n->drop_table.table_name) != SV_OK)
        return result_error_msg("Table not found");
    if (droot && g_drop_hook) g_drop_hook(droot);    // il motore libera le pagine quando il DROP è su disco (P1.5)
    else atomic_fetch_add(&g_sv_abandoned_pages, npages);
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
// conv >= 0: la colonna conv cambia tipo e ogni valore passa dal testo (con i controlli del nuovo tipo);
// cmsg riceve l'errore di conversione
static const char* rewrite_rows(ExecCtx* ctx, TableSchema* ts, TableSchema* ns, const int* map, const char* def,
                                int conv, char* cmsg, size_t cmsgn, uint32_t* out_root, uint32_t* out_pages) {
    uint32_t root;
    if (new_root_page(ctx, &root) != SV_OK) return "Cannot allocate a page";
    HeapFile* nh = heap_open(ctx->bp, root, 1);
    HeapFile* oh = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    HeapScan* scan = oh ? heap_scan_open(oh, ts) : NULL;
    const char* fail = (!nh || !scan) ? "Cannot read the table" : NULL;
    Tuple* t;
    RID rid;
    while (!fail && (t = heap_scan_next(scan, &rid)) != NULL) {
        Tuple* nt = tuple_create(ns->num_columns);
        if (!nt) { tuple_free(t); fail = "Out of memory"; break; }
        for (int i = 0; i < ns->num_columns && !fail; i++) {
            if (i == conv && !t->values[map[i]].is_null) {
                char txt[4096], why[200];
                value_to_string(&t->values[map[i]], ts->columns[map[i]].type, txt, sizeof(txt));
                if (check_value(&ns->columns[i], txt, why, sizeof(why))) {
                    snprintf(cmsg, cmsgn, "Cannot change the type of '%.64s': %s", ns->columns[i].name, why);
                    fail = cmsg;
                } else column_set_from_text(&nt->values[i], &ns->columns[i], txt);
            }
            else if (map[i] >= 0) value_copy(&nt->values[i], &t->values[map[i]], ns->columns[i].type);
            else column_set_from_text(&nt->values[i], &ns->columns[i], def);
        }
        if (!fail && has_generated(ns)) fail = gen_compute(ctx, ns, nt, cmsg, cmsgn);   // colonne calcolate
        if (fail) { tuple_free(nt); tuple_free(t); break; }
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

// VACUUM FULL (P1.5): le righe della tabella in pagine nuove (compatte, nelle pagine libere più basse); la tabella
// vecchia resta com'è finché il chiamante non salva il catalogo con la radice nuova
const char* executor_rebuild_table(ExecCtx* ctx, TableSchema* ts, uint32_t* out_root, uint32_t* out_pages) {
    int* map = sv_malloc(sizeof(int) * (size_t)(ts->num_columns > 0 ? ts->num_columns : 1));
    if (!map) return "Out of memory";
    for (int i = 0; i < ts->num_columns; i++) map[i] = i;
    char msg[300] = "";
    const char* f = rewrite_rows(ctx, ts, ts, map, "", -1, msg, sizeof(msg), out_root, out_pages);
    free(map);
    return f;
}

// la colonna ci fa parte di una chiave esterna (della tabella o di un'altra che la riferisce)?
static const char* col_in_fk(Database* db, const TableSchema* ts, int ci, char* msg, size_t n) {
    for (int k = 0; k < ts->num_constraints; k++) {
        const Constraint* c = &ts->constraints[k];
        if (c->kind != CONS_FOREIGN) continue;
        for (int q = 0; q < c->ncols; q++)
            if (c->cols[q] == ci) { snprintf(msg, n, "Column '%.64s' is part of foreign key '%.64s': drop it first", ts->columns[ci].name, c->name); return msg; }
    }
    for (int i = 0; i < db->num_tables; i++)
        for (int k = 0; k < db->tables[i]->num_constraints; k++) {
            const Constraint* c = &db->tables[i]->constraints[k];
            if (c->kind != CONS_FOREIGN || strcasecmp(c->ref_table, ts->name)) continue;
            for (int q = 0; q < c->ncols; q++)
                if (!strcasecmp(c->ref_cols[q], ts->columns[ci].name)) {
                    snprintf(msg, n, "Column '%.64s' is referenced by foreign key '%.64s' of table '%.64s': drop it first",
                             ts->columns[ci].name, c->name, db->tables[i]->name);
                    return msg;
                }
        }
    return NULL;
}

// ALTER TABLE t ALTER COLUMN c TYPE ... | SET / DROP DEFAULT | SET / DROP NOT NULL
static QueryResult* alter_column_attr(ExecCtx* ctx, Database* db, TableSchema* ts, ASTNode* n) {
    char msg[300];
    const char* name = n->alter_table.alter_col;
    int ci = catalog_get_col_index(ts, name);
    if (ci < 0) { snprintf(msg, sizeof(msg), "Unknown column '%.64s'", name); return result_error_msg(msg); }
    Column* col = &ts->columns[ci];
    const ColumnDef* d = &n->alter_table.alter_def;
    bool in_pk = col->primary_key;
    for (int k = 0; k < ts->num_constraints; k++)
        if (ts->constraints[k].kind == CONS_PRIMARY)
            for (int q = 0; q < ts->constraints[k].ncols; q++) if (ts->constraints[k].cols[q] == ci) in_pk = true;
    switch (n->alter_table.alter_kind) {
        case ALT_SET_DEFAULT: {
            const char* v = d->default_val;
            if (v[0] && strcasecmp(v, "NULL")) {
                if (check_value(col, v, msg, sizeof(msg))) return result_error_msg(msg);
            } else if (!col->nullable) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is NOT NULL: its DEFAULT cannot be NULL", name);
                return result_error_msg(msg);
            }
            if (!v[0] && !col->nullable) {}                       // DEFAULT '' va bene anche per NOT NULL
            snprintf(col->default_val, sizeof(col->default_val), "%s", strcasecmp(v, "NULL") ? v : "");
            col->has_default = strcasecmp(v, "NULL") != 0;
            return result_ok_msg(0);
        }
        case ALT_DROP_DEFAULT:
            col->default_val[0] = '\0';
            col->has_default = false;
            return result_ok_msg(0);
        case ALT_DROP_NOT_NULL:
            if (in_pk) { snprintf(msg, sizeof(msg), "Column '%.64s' is part of the primary key: it cannot be NULL", name); return result_error_msg(msg); }
            col->nullable = true;
            return result_ok_msg(0);
        case ALT_SET_NOT_NULL: {
            // le righe esistenti devono già rispettare il vincolo
            HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
            HeapScan* scan = hf ? heap_scan_open(hf, ts) : NULL;
            if (!scan) { free(hf); return result_error_msg("Cannot read the table"); }
            long nulls = 0;
            Tuple* t;
            RID rid;
            while ((t = heap_scan_next(scan, &rid)) != NULL) {
                if (ci < t->num_values && t->values[ci].is_null) nulls++;
                tuple_free(t);
            }
            heap_scan_close(scan);
            free(hf);
            if (nulls) {
                snprintf(msg, sizeof(msg), "Column '%.64s' contains NULL in %ld row%s: update them before SET NOT NULL", name, nulls, nulls == 1 ? "" : "s");
                return result_error_msg(msg);
            }
            col->nullable = false;
            return result_ok_msg(0);
        }
        default: break;
    }
    // TYPE: le righe vengono riscritte con il valore convertito
    if (ctx->tx_id) return result_error_msg("ALTER TABLE ... ALTER COLUMN ... TYPE is not allowed inside a transaction: COMMIT first");
    if (!known_type(d->type)) { snprintf(msg, sizeof(msg), "Unknown type '%.32s'", d->type); return result_error_msg(msg); }
    if (col_in_fk(db, ts, ci, msg, sizeof(msg))) return result_error_msg(msg);
    Column nc = *col;
    nc.type = parse_type(d->type);
    nc.size = d->size;
    if (col->auto_increment && nc.type != SV_TYPE_INT && nc.type != SV_TYPE_BIGINT) {
        snprintf(msg, sizeof(msg), "Column '%.64s' is AUTO_INCREMENT: it must stay an integer", name);
        return result_error_msg(msg);
    }
    if (nc.default_val[0] && strcasecmp(nc.default_val, "NULL") && check_value(&nc, nc.default_val, msg, sizeof(msg))) {
        char m2[400];
        snprintf(m2, sizeof(m2), "DEFAULT does not fit the new type (%s): change it first", msg);
        return result_error_msg(m2);
    }
    TableSchema* ns = schema_dup(ts, 0);
    int* map = sv_malloc(sizeof(int) * SV_MAX_COLUMNS);
    if (!ns || !map) { free(ns); free(map); return result_error_msg("Out of memory"); }
    ns->columns[ci] = nc;
    for (int i = 0; i < ts->num_columns; i++) map[i] = i;
    uint32_t root = 0, pages = 0, old_root = ts->root_page_id;
    const char* fail = rewrite_rows(ctx, ts, ns, map, NULL, ci, msg, sizeof(msg), &root, &pages);
    if (!fail) {
        ns->root_page_id = root;
        ns->num_pages = pages;
        schema_assign(ts, ns);
        mi_invalidate_table(db->name, ts->name);
        heap_cache_forget(ctx->bp, old_root);
        bp_flush_all(ctx->bp);                                     // le righe nuove su disco prima del catalogo
    }
    free(ns);
    free(map);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    return result_ok_msg(0);
}

static QueryResult* alter_column(ExecCtx* ctx, Database* db, TableSchema* ts, ASTNode* n) {
    char msg[300];
    if (ctx->tx_id && (n->alter_table.add_column || n->alter_table.drop_column[0]))
        return result_error_msg("ALTER TABLE ... ADD / DROP COLUMN is not allowed inside a transaction: COMMIT first");

    // RENAME COLUMN: solo il catalogo (e i testi che usano il nome)
    if (n->alter_table.rename_from[0]) {
        const char* from = n->alter_table.rename_from, *to = n->alter_table.rename_to;
        if (!sv_name_ok(to)) return bad_name_error("column");
        int ci = catalog_get_col_index(ts, from);
        if (ci < 0) { snprintf(msg, sizeof(msg), "Unknown column '%.64s'", from); return result_error_msg(msg); }
        if (strcasecmp(from, to) && catalog_get_col_index(ts, to) >= 0) { snprintf(msg, sizeof(msg), "Column '%.64s' already exists", to); return result_error_msg(msg); }
        char* buf = sv_malloc(SV_MAX_SQL_LEN);
        if (!buf) return result_error_msg("Out of memory");
        for (int k = 0; k < ts->num_constraints; k++)
            if (ts->constraints[k].kind == CONS_CHECK && rename_ident_in_text(ts->constraints[k].check, from, to, buf, SV_CHECK_LEN))
                snprintf(ts->constraints[k].check, sizeof(ts->constraints[k].check), "%s", buf);
        for (int i = 0; i < db->num_tables; i++)
            for (int k = 0; k < db->tables[i]->num_constraints; k++) {
                Constraint* c = &db->tables[i]->constraints[k];
                if (c->kind != CONS_FOREIGN || strcasecmp(c->ref_table, ts->name)) continue;
                for (int q = 0; q < c->ncols; q++) if (!strcasecmp(c->ref_cols[q], from)) snprintf(c->ref_cols[q], sizeof(c->ref_cols[q]), "%s", to);
            }
        for (int i = 0; i < db->num_triggers; i++)
            if (!strcasecmp(db->triggers[i].table_name, ts->name) && rename_ident_in_text(db->triggers[i].body, from, to, buf, SV_MAX_SQL_LEN))
                snprintf(db->triggers[i].body, sizeof(db->triggers[i].body), "%s", buf);
        for (int i = 0; i < ts->num_columns; i++)                // espressioni delle colonne calcolate
            if ((ts->columns[i].flags & COLF_GENERATED) && rename_ident_in_text(ts->columns[i].comment, from, to, buf, SV_MAX_SQL_LEN))
                snprintf(ts->columns[i].comment, sizeof(ts->columns[i].comment), "%s", buf);
        free(buf);
        for (int i = 0; i < ts->num_indexes; i++)
            if (!strcasecmp(ts->indexes[i].col_name, from)) snprintf(ts->indexes[i].col_name, sizeof(ts->indexes[i].col_name), "%s", to);
        snprintf(ts->columns[ci].name, sizeof(ts->columns[ci].name), "%s", to);
        mi_invalidate_table(db->name, ts->name);
        return result_ok_msg(0);
    }

    TableSchema* ns = schema_dup(ts, 1);                      // spazio per una colonna aggiunta
    TableSchema* backup = schema_dup(ts, 0);
    int* map = sv_malloc(sizeof(int) * SV_MAX_COLUMNS);
    if (!ns || !backup || !map) { free(ns); free(backup); free(map); return result_error_msg("Out of memory"); }
    const char* def = NULL;
    const char* fail = NULL;
    ASTNode* add = n->alter_table.add_column;
    int dropped = -1;
    if (add) {
        ColumnDef* cd = &add->create_table.columns[0];
        if (add->create_table.num_columns != 1) fail = "Expected one column definition";
        else if (!sv_name_ok(cd->name)) fail = "Invalid column name: it cannot be empty or contain a backtick (`) or a control character";
        else if (!known_type(cd->type)) { snprintf(msg, sizeof(msg), "Unknown type '%.32s'", cd->type); fail = msg; }
        else if (catalog_get_col_index(ts, cd->name) >= 0) { snprintf(msg, sizeof(msg), "Column '%.64s' already exists", cd->name); fail = msg; }
        else if (ts->num_columns >= SV_MAX_COLUMNS) fail = "Too many columns (max 1024)";
        else if (cd->primary_key || cd->auto_increment) fail = "Add the column first, then ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY";
        if (!fail) {
            Column* col = &ns->columns[ns->num_columns];
            memset(col, 0, sizeof(*col));
            snprintf(col->name, sizeof(col->name), "%s", cd->name);
            col->type = parse_type(cd->type);
            col->size = cd->size;
            col->nullable = cd->nullable;
            snprintf(col->default_val, sizeof(col->default_val), "%s", cd->default_val);
            col->has_default = cd->has_default && strcasecmp(cd->default_val, "NULL");
            col->sqltype[0] = '\0';
            snprintf(col->sqltype, sizeof(col->sqltype), "%s", cd->sqltype);
            col->flags = cd->flags;
            if (cd->comment[0] && cd->comment[0] != '\x01') snprintf(col->comment, sizeof(col->comment), "%s", cd->comment);
            if (col->type == SV_TYPE_DECIMAL) col->decimal_prec = cd->has_scale ? cd->scale + 1 : cd->size > 0 ? 1 : 0;
            if (col->has_default && !default_is_now(cd->default_val)) {
                def = col->default_val;
                if (check_value(col, def, msg, sizeof(msg))) fail = msg;
            }
            if (!fail && (col->flags & COLF_GENERATED)) {        // espressione controllata su una riga vuota
                Tuple* probe = tuple_create(ns->num_columns + 1);
                for (int i = 0; probe && i < probe->num_values; i++) probe->values[i].is_null = true;
                ns->num_columns++;
                const char* gf = probe ? gen_compute(ctx, ns, probe, msg, sizeof(msg)) : "Out of memory";
                ns->num_columns--;
                if (probe) tuple_free(probe);
                if (gf && strstr(gf, "cannot be NULL")) gf = NULL;   // su una riga vuota il valore può mancare
                if (gf) fail = gf;
            }
            if (!fail && !col->nullable && !def && ts->num_rows > 0 && !(col->flags & COLF_GENERATED)) {
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
            for (int k = 0; !fail && k < db->tables[i]->num_constraints; k++) {
                const Constraint* c = &db->tables[i]->constraints[k];
                if (c->kind != CONS_FOREIGN || strcasecmp(c->ref_table, ts->name)) continue;
                for (int q = 0; q < c->ncols; q++)
                    if (!strcasecmp(c->ref_cols[q], name)) {
                        snprintf(msg, sizeof(msg), "Column '%.64s' is referenced by foreign key '%.64s' of table '%.64s'", name, c->name, db->tables[i]->name);
                        fail = msg;
                    }
            }
        for (int i = 0; !fail && i < ts->num_columns; i++)
            if (i != dropped && (ts->columns[i].flags & COLF_GENERATED) && ident_in_text(ts->columns[i].comment, name)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is used by generated column '%.64s'", name, ts->columns[i].name);
                fail = msg;
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
    if (!fail) fail = rewrite_rows(ctx, ts, ns, map, def, -1, msg, sizeof(msg), &root, &pages);
    if (!fail) {
        uint32_t old_root = ts->root_page_id;
        ns->root_page_id = root;
        ns->num_pages = pages;
        if (!schema_assign(ts, ns)) fail = "Out of memory";
        mi_invalidate_table(db->name, ts->name);
        // vincoli scritti con la nuova colonna (controllano le righe esistenti)
        for (int i = 0; add && !fail && i < add->create_table.num_cons; i++)
            fail = add_constraint(ctx, db, ts, &add->create_table.cons[i], msg, sizeof(msg));
        if (fail) {
            heap_cache_forget(ctx->bp, root);
            schema_assign(ts, backup);                             // le pagine di prima sono intatte
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
    if (n->alter_table.alter_kind) return alter_column_attr(ctx, db, ts, n);
    // RENAME TO
    if (n->alter_table.new_name[0]) {
        const char* nn = n->alter_table.new_name;
        if (!sv_name_ok(nn)) return bad_name_error("table");
        if (strcasecmp(nn, ts->name) &&
            (catalog_get_table(ctx->catalog, db->name, nn) || catalog_get_view(ctx->catalog, db->name, nn)))
            return result_error_msg("A table or view with the new name already exists");
        mi_invalidate_table(db->name, ts->name);
        for (int i = 0; i < db->num_tables; i++)                 // le chiavi esterne seguono il nuovo nome
            for (int k = 0; k < db->tables[i]->num_constraints; k++)
                if (db->tables[i]->constraints[k].kind == CONS_FOREIGN && !strcasecmp(db->tables[i]->constraints[k].ref_table, ts->name))
                    snprintf(db->tables[i]->constraints[k].ref_table, sizeof(db->tables[i]->constraints[k].ref_table), "%s", nn);
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
                for (int f = 0; f < db->tables[i]->num_constraints; f++) {
                    const Constraint* fk = &db->tables[i]->constraints[f];
                    if (fk->kind != CONS_FOREIGN || strcasecmp(fk->ref_table, ts->name) || fk->ncols != c->ncols) continue;
                    bool same = true;
                    for (int a = 0; a < fk->ncols && same; a++) {
                        bool in = false;
                        for (int b = 0; b < c->ncols; b++) if (!strcasecmp(fk->ref_cols[a], ts->columns[c->cols[b]].name)) in = true;
                        same = in;
                    }
                    if (same && db->tables[i] != ts) {
                        snprintf(msg, sizeof(msg), "Cannot drop '%.64s': the foreign key '%.64s' of table '%.64s' uses it",
                                 c->name, fk->name, db->tables[i]->name);
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
    if (y == 0 && m == 0 && d == 0) { if (rest) *rest = s + 10; return true; }   // data zero di MySQL (0000-00-00)
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return false;
    int md = DAYS[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    if (d > md) return false;
    if (rest) *rest = s + 10;
    return true;
}

// NULL se il testo è un valore valido per la colonna, altrimenti scrive il motivo in msg
// fine di un orario: niente, o i decimali dei secondi (".123", fino a 6 cifre: datetime(3) di MySQL)
static bool frac_secs_ok(const char* p) {
    if (!*p) return true;
    if (*p != '.') return false;
    int d = 0;
    for (p++; *p >= '0' && *p <= '9'; p++) d++;
    return !*p && d >= 1 && d <= 6;
}

// intervallo dei tipi interi più stretti di INT, dal tipo dichiarato (TINYINT / SMALLINT / MEDIUMINT si conservano
// come INT): prima TINYINT accettava 200 e SMALLINT 100000. false = nessun limite oltre quello di INT
static bool narrow_int_bounds(const Column* c, long long* lo, long long* hi) {
    static const struct { const char* name; long long lo, hi, uhi; } T[] = {
        { "tinyint", -128, 127, 255 }, { "smallint", -32768, 32767, 65535 }, { "mediumint", -8388608, 8388607, 16777215 } };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        size_t l = strlen(T[i].name);
        if (strncasecmp(c->sqltype, T[i].name, l) || isalnum((unsigned char)c->sqltype[l])) continue;
        bool un = (c->flags & COLF_UNSIGNED) != 0;
        *lo = un ? 0 : T[i].lo;
        *hi = un ? T[i].uhi : T[i].hi;
        return true;
    }
    return false;
}

static const char* check_value(const Column* c, const char* v, char* msg, size_t n) {
    const char* p = v;
    while (isspace((unsigned char)*p)) p++;
    switch (c->type) {
        case SV_TYPE_INT: case SV_TYPE_BIGINT: {
            char* end = NULL;
            errno = 0;
            long long x = strtoll(p, &end, 10);
            bool range = errno == ERANGE;
            // BIGINT UNSIGNED: fino a 2^64 - 1 (P1.9; il valore si conserva nei 64 bit, letto senza segno)
            if (range && x > 0 && c->type == SV_TYPE_BIGINT && (c->flags & COLF_UNSIGNED) && *p != '-') {
                char* e2 = NULL;
                errno = 0;
                (void)strtoull(p, &e2, 10);
                while (e2 && isspace((unsigned char)*e2)) e2++;
                if (errno != ERANGE && e2 && !*e2) return NULL;
            }
            if (end != p && (*end == '.' || *end == 'e' || *end == 'E')) {   // 3.0 va bene, 3.5 no
                double d = strtod(p, &end);
                if (d != (double)(long long)d) { snprintf(msg, n, "Column '%s' expects an integer, got '%.40s'", c->name, v); return msg; }
                x = (long long)d;
            }
            while (end && isspace((unsigned char)*end)) end++;
            if (end == p || !end || *end) { snprintf(msg, n, "Column '%s' expects an integer, got '%.40s'", c->name, v); return msg; }
            if (range || (c->type == SV_TYPE_INT && (x < INT_MIN || x > INT_MAX)) || (x < 0 && (c->flags & COLF_UNSIGNED))) {
                snprintf(msg, n, "Value %.40s is out of range for column '%s' (%s)", v, c->name, datatype_name(c->type));
                return msg;
            }
            long long lo, hi;
            if (c->type == SV_TYPE_INT && narrow_int_bounds(c, &lo, &hi) && (x < lo || x > hi)) {
                snprintf(msg, n, "Value %.40s is out of range for column '%s' (%s)", v, c->name, c->sqltype);
                return msg;
            }
            return NULL;
        }
        case SV_TYPE_FLOAT: case SV_TYPE_DECIMAL: {
            char* end = NULL;
            double d = strtod(p, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (end == p || !end || *end || d != d) { snprintf(msg, n, "Column '%s' expects a number, got '%.40s'", c->name, v); return msg; }
            // DECIMAL(p, s): al più p - s cifre intere dopo l'arrotondamento a s decimali (prima DECIMAL(4,1) prendeva
            // 999.95 e salvava 1000.0, oltre la precisione dichiarata)
            if (c->type == SV_TYPE_DECIMAL && c->size > 0 && c->decimal_prec > 0 && c->size >= c->decimal_prec - 1) {
                int s = c->decimal_prec - 1, ip = c->size - s;
                double lim = pow(10.0, ip) - 0.5 * pow(10.0, -s);
                if (ip < 300 && fabs(d) >= lim) {
                    snprintf(msg, n, "Value %.40s is out of range for column '%s' (%s)", v, c->name, c->sqltype);
                    return msg;
                }
            }
            return NULL;
        }
        case SV_TYPE_BOOL:
            if (!strcmp(p, "0") || !strcmp(p, "1") || !strcasecmp(p, "true") || !strcasecmp(p, "false")) return NULL;
            snprintf(msg, n, "Column '%s' expects TRUE, FALSE, 1 or 0, got '%.40s'", c->name, v);
            return msg;
        case SV_TYPE_VARCHAR: case SV_TYPE_CHAR:
            if (c->type == SV_TYPE_CHAR && c->size > 0 && utf8_len(v) > c->size) {   // CHAR: gli spazi finali non contano
                size_t l = strlen(v);
                while (l > 0 && v[l - 1] == ' ') l--;
                int chars = 0;
                for (size_t i = 0; i < l; i++) if (((unsigned char)v[i] & 0xC0) != 0x80) chars++;
                if (chars <= c->size) return NULL;
            }
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
                             (ss = 0, sscanf(rest, " %2d:%2d%n", &hh, &mm, &k) == 2)) && frac_secs_ok(rest + k) &&
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

#define SUBK_UDF 100      // EXPR_FUNC con subkind SUBK_UDF: funzione definita dall'utente (CREATE FUNCTION)

// righe scritte da INSERT / UPDATE / DELETE ... RETURNING (solo quelle dello statement, non dei trigger)
typedef struct { bool on; int depth; char table[SV_MAX_NAME_LEN]; Tuple** v; int n, cap; } RetCapture;
// descrizione di una SELECT (PREPARE del protocollo MySQL): colonne senza leggere righe né valutare funzioni
static _Thread_local bool g_describe;
void executor_set_describe(bool on) { g_describe = on; }
static _Thread_local RetCapture g_cap;
static Tuple* tuple_clone(const Tuple* t, const TableSchema* ts);
static void cap_add(const ExecCtx* ctx, const TableSchema* ts, const Tuple* t) {
    if (!g_cap.on || ctx->trig_depth != g_cap.depth || strcasecmp(ts->name, g_cap.table) || !t) return;
    if (g_cap.n == g_cap.cap) {
        int nc = g_cap.cap ? g_cap.cap * 2 : 16;
        Tuple** nv = sv_realloc(g_cap.v, sizeof(Tuple*) * (size_t)nc);
        if (!nv) return;
        g_cap.v = nv; g_cap.cap = nc;
    }
    Tuple* c = tuple_clone(t, ts);
    if (c) g_cap.v[g_cap.n++] = c;
}
static Tuple* rt_scope_row(void* routine, int s, const TableSchema** ts);   // riga s (NEW / OLD / variabili) della routine
static _Thread_local int64_t g_last_insert_id;
#define EV_STR_MAX 511   // come Cell.value: un testo più lungo non entrerebbe nel risultato

typedef struct {
    bool        null;
    bool        is_num;
    bool        is_int;
    int64_t     inum;      // valore intero esatto (se is_int)
    double      num;
    const char* str;       // testo (stringhe e numeri formattati); punta a buf o a memoria della tupla
    bool        ci;        // testo di una colonna con collation _ci: confronti senza maiuscole (MySQL)
    bool        is_dec;    // decimale esatto (P1.9): str = il testo esatto, num = l'approssimazione double
    char        buf[EV_STR_MAX + 1];
} EvalVal;

// valore tipizzato conservato (risultati, chiavi di ordinamento, funzioni finestra)
typedef struct SortKey { bool null, is_num, is_int, ci, is_dec; int64_t inum; double num; char* str; } SortKey;

// colonna di testo di una tabella con collation *_ci (creata da un'applicazione MySQL): i confronti,
// gli indici, GROUP BY e ORDER BY ignorano maiuscole e minuscole, come in MySQL
static bool col_is_ci(const TableSchema* ts, int c) {
    if (!ts || c < 0 || c >= ts->num_columns) return false;
    DataType t = ts->columns[c].type;
    if (t != SV_TYPE_VARCHAR && t != SV_TYPE_CHAR && t != SV_TYPE_TEXT) return false;
    if (ts->columns[c].flags & COLF_BIN) return false;          // COLLATE sulla colonna: prevale sulla tabella
    if (ts->columns[c].flags & COLF_CI) return true;
    size_t l = strlen(ts->collation);
    return l > 3 && !strcmp(ts->collation + l - 3, "_ci");
}

// confronto di testi senza maiuscole, anche per lettere accentate, greche e cirilliche (u8case.h)
static int ci_cmp(const char* a, const char* b) { return u8_ci_cmp(a, b); }

// copia in minuscolo (malloc); NULL se manca memoria
static char* ci_fold(const char* s) {
    size_t l = strlen(s);
    char* o = sv_malloc(l + 1);
    if (!o) return NULL;
    u8_ci_fold(o, s);                                   // senza maiuscole e senza accenti (collation _ci)
    return o;
}

static void ev_null(EvalVal* v) { v->null = true; v->is_num = v->is_int = false; v->str = ""; v->ci = false; v->is_dec = false; }

// i numeri vengono convertiti in testo solo quando serve (ev_s): confronti, aritmetica e
// filtri lavorano sui valori binari, senza snprintf per ogni riga
static void ev_int(EvalVal* v, int64_t x) {
    v->null = false; v->is_num = v->is_int = true; v->inum = x; v->num = (double)x; v->str = NULL; v->ci = false; v->is_dec = false;
}

static void ev_num(EvalVal* v, double x) {
    if (x != x || x - x != 0) { ev_null(v); return; }        // NaN / infinito: NULL
    v->null = false; v->is_num = true; v->is_int = false; v->num = x; v->str = NULL; v->ci = false; v->is_dec = false;
}

// decimale esatto (P1.9): text deve restare valido finché si usa il valore (letterale dell'albero, colonna
// della tupla) oppure copy = true (risultato calcolato: nel buffer). Testo che non è un decimale: come numero
static void ev_dec_text(EvalVal* v, const char* text, bool copy) {
    Dec d;
    if (!dec_parse(text, &d)) { v->null = false; v->is_int = false; v->is_dec = false; v->ci = false;
                                v->is_num = true; v->num = strtod(text, NULL); v->str = NULL; return; }
    v->null = false; v->is_num = true; v->is_int = false; v->is_dec = true; v->ci = false;
    v->num = dec_to_double(&d);
    if (copy) { snprintf(v->buf, sizeof(v->buf), "%s", text); v->str = v->buf; }
    else v->str = text;
}

static void ev_dec(EvalVal* v, const Dec* d) {
    v->null = false; v->is_num = true; v->is_int = false; v->is_dec = true; v->ci = false;
    dec_format(d, v->buf, sizeof(v->buf));
    v->str = v->buf;
    v->num = dec_to_double(d);
}

// il valore come decimale esatto: un decimale o un intero (non un double né un testo)
static bool ev_get_dec(const EvalVal* v, Dec* d) {
    if (v->null || !v->is_num) return false;
    if (v->is_int) { dec_from_i64(v->inum, d); return true; }
    return v->is_dec && v->str && dec_parse(v->str, d);
}

// aritmetica decimale: almeno un decimale e l'altro decimale o intero (con un double si resta nel double, come MySQL)
static bool ev_dec_pair(const EvalVal* a, const EvalVal* b, Dec* x, Dec* y) {
    if (!(a->is_dec || b->is_dec)) return false;
    return ev_get_dec(a, x) && ev_get_dec(b, y);
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
    v->null = false; v->is_num = v->is_int = false; v->is_dec = false;
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
        Dec x, y;
        if (ev_dec_pair(a, b, &x, &y)) return dec_cmp(&x, &y);       // 0.10 + 0.20 = 0.30: esatto
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
    int c = (a->ci || b->ci) ? ci_cmp(ev_s(a), ev_s(b)) : strcmp(ev_s(a), ev_s(b));
    return c < 0 ? -1 : c > 0;
}

// -1 = NULL (sconosciuto), 0 = falso, 1 = vero
static int ev_truth(const EvalVal* v) {
    if (v->null) return -1;
    return ev_as_num(v) != 0 ? 1 : 0;
}

static void ev_from_value(EvalVal* out, const Value* v, DataType type);

// valore di una colonna con le sue opzioni: BIGINT UNSIGNED oltre 2^63 (bit negativi) è il suo valore esatto
static void ev_from_col(EvalVal* out, const Value* v, const Column* col) {
    if (col->type == SV_TYPE_BIGINT && (col->flags & COLF_UNSIGNED) && !v->is_null && v->bigint_val < 0) {
        Dec d;
        dec_from_u64((uint64_t)v->bigint_val, &d);
        ev_dec(out, &d);
        return;
    }
    ev_from_value(out, v, col->type);
}

// valore di una colonna della tupla
static void ev_from_value(EvalVal* out, const Value* v, DataType type) {
    out->ci = false;
    if (v->is_null) { ev_null(out); return; }
    switch (type) {
        case SV_TYPE_INT:    ev_int(out, v->int_val); return;
        case SV_TYPE_BIGINT: ev_int(out, v->bigint_val); return;
        case SV_TYPE_BOOL:   ev_int(out, v->bool_val ? 1 : 0); return;
        case SV_TYPE_FLOAT:  ev_num(out, v->float_val); return;
        case SV_TYPE_DECIMAL: {   // salvato come testo: decimale esatto (testi di versioni precedenti: come numero)
            out->null = false;
            out->str = v->str_val ? v->str_val : "";
            Dec dd;
            if (dec_parse(out->str, &dd)) { out->is_dec = true; out->is_num = true; out->is_int = false; out->num = dec_to_double(&dd); return; }
            double d;
            out->is_int = false;
            out->is_dec = false;
            out->is_num = text_is_number(out->str, &d);
            out->num = out->is_num ? d : 0;
            return;
        }
        default:
            out->null = false; out->is_num = out->is_int = false; out->is_dec = false;
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
    SMSlot* ns = sv_calloc(nc, sizeof(SMSlot));
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
    m->s[j].key = sv_strdup(key);
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

// ── chiavi di testo ─────────────────────────────────────────────
// Un testo corto (fino a KEY_INLINE_MAX byte, senza i separatori \x1e e \x1f) è la chiave di
// se stesso. Uno più lungo diventa "prefisso\x1e<hash a 128 bit>:<lunghezza>": la chiave resta
// corta (gli indici e i controlli di unicità non troncano più), le due forme non coincidono mai
// (solo la seconda contiene \x1e) e testi diversi non collidono. Il seme è casuale a ogni avvio:
// le chiavi vivono solo in memoria, e nessuno può costruire apposta due testi con la stessa chiave.
#define KEY_INLINE_MAX 200
#define KEY_BUF        4096     // chiave di 16 colonne (SV_MAX_KEY_COLS) da KEY_INLINE_MAX + separatore ciascuna

static uint64_t g_key_seed[2];

static uint64_t kmix64(uint64_t x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// al primo uso, anche da più letture in parallelo: il seme si pubblica una volta sola, completo
static atomic_int g_key_seed_ready;
static SvRwLock g_key_seed_lock = SV_RWLOCK_INIT;

static void key_seed_init(void) {
    if (atomic_load_explicit(&g_key_seed_ready, memory_order_acquire)) return;
    sv_rw_wr(&g_key_seed_lock);
    if (!atomic_load_explicit(&g_key_seed_ready, memory_order_relaxed)) {
        uint64_t seed[2] = { 0, 0 };
        uint64_t s = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)&g_key_seed << 13) ^ ((uint64_t)clock() << 32);
        FILE* f = fopen("/dev/urandom", "rb" SV_FE);
        if (f) { if (fread(seed, 1, sizeof(seed), f) != sizeof(seed)) seed[0] = seed[1] = 0; fclose(f); }
        g_key_seed[0] = kmix64(seed[0] ^ s) | 1;
        g_key_seed[1] = kmix64(seed[1] ^ s ^ 0x9E3779B97F4A7C15ULL) | 1;
        atomic_store_explicit(&g_key_seed_ready, 1, memory_order_release);
    }
    sv_rw_wrunlock(&g_key_seed_lock);
}

static uint64_t key_hash(const char* s, size_t n, uint64_t seed) {
    uint64_t h = seed ^ kmix64(n);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, s + i, 8); h = kmix64(h ^ w) + 0x9E3779B97F4A7C15ULL; }
    uint64_t t = 0;
    memcpy(&t, s + i, n - i);
    return kmix64(h ^ t ^ ((uint64_t)(n - i) << 56));
}

static bool key_is_long(const char* s, size_t n) {
    return n > KEY_INLINE_MAX || memchr(s, '\x1e', n) || memchr(s, '\x1f', n);
}

// scrive la chiave del testo s in buf (cap >= KEY_INLINE_MAX + 1); true se in forma lunga
static bool key_text(char* buf, size_t cap, const char* s) {
    size_t n = strlen(s);
    if (!key_is_long(s, n) && n < cap) { memcpy(buf, s, n + 1); return false; }
    key_seed_init();
    size_t pre = n < 64 ? n : 64;
    if (pre + 60 > cap) pre = cap > 60 ? cap - 60 : 0;
    for (size_t k = 0; k < pre; k++) buf[k] = (s[k] == '\x1e' || s[k] == '\x1f') ? '?' : s[k];
    snprintf(buf + pre, cap - pre, "\x1e%016llx%016llx:%llu", (unsigned long long)key_hash(s, n, g_key_seed[0]),
             (unsigned long long)key_hash(s, n, g_key_seed[1]), (unsigned long long)n);
    return true;
}

typedef struct MemIndex {
    char      db[SV_MAX_NAME_LEN], table[SV_MAX_NAME_LEN];
    char      col[SV_MAX_NAME_LEN * SV_MAX_KEY_COLS];   // nome della colonna, o "a,b" per una chiave composta
    int       cols[SV_MAX_KEY_COLS];      // posizioni delle colonne della chiave
    int       ncols;
    StrMap    map;          // chiave -> posizione in lists
    RidList*  lists;
    int       nlists, caplists;
    bool      has_max;      // massimo intero (AUTO_INCREMENT)
    int64_t   max_int;
    // vista ordinata delle chiavi (scansioni per intervallo): costruita al primo uso,
    // da rifare solo quando compare una chiave nuova
    char**    lkeys;        // testo della chiave di ogni lista
    int*      ord;          // liste in ordine di chiave
    int64_t*  okey;         // chiave numerica di ogni lista (colonne intere)
    int       nord;
    bool      ord_ok;
    bool      has_long;     // qualche chiave è in forma lunga: niente scansioni per intervallo (l'ordine non vale)
    // indice pigro (P1.15): base_root != 0 = le chiavi non in map stanno nell'albero su disco base_root (scritto
    // all'ultimo checkpoint); in map ci sono solo le chiavi lette o cambiate da allora, con la lista intera dei RID
    // (una lista vuota nasconde la chiave della base). Niente lettura della tabella per costruirlo
    uint32_t  base_root;
    BufferPool* bp;
    uint64_t  built_seq;    // sequenza delle modifiche (db_api) quando è stato costruito: un ROLLBACK che annulla
                            // modifiche più recenti lo scarta (le sue chiavi le includono senza diario)
    struct MemIndex* next;
} MemIndex;

// Protezione: la lista si legge / allunga con g_mi_lock; gli indici cambiano (insert / delete / free)
// solo con il motore in esclusiva (db_lock), quando nessuna lettura parallela è in corso.
static MemIndex* g_mem_indexes;
static MemIndex* g_mi_zombies;          // tolti dalla lista con letture in parallelo possibili: liberati in esclusiva

static void ij_forget_index(const MemIndex* ix);
static uint64_t (*g_ij_seq)(void);       // (diario degli indici, più sotto)

static void mi_free(MemIndex* ix) {
    ij_forget_index(ix);
    sm_free(&ix->map);
    for (int i = 0; i < ix->nlists; i++) { free(ix->lists[i].r); free(ix->lkeys[i]); }
    free(ix->lists);
    free(ix->lkeys);
    free(ix->ord);
    free(ix->okey);
    free(ix);
}

static void mi_free_zombies(void) {
    while (g_mi_zombies) { MemIndex* nx = g_mi_zombies->next; mi_free(g_mi_zombies); g_mi_zombies = nx; }
}

void executor_invalidate_indexes(void) {
    heap_cache_clear();
    mi_free_zombies();
    while (g_mem_indexes) {
        MemIndex* nx = g_mem_indexes->next;
        mi_free(g_mem_indexes);
        g_mem_indexes = nx;
    }
}

static void mi_invalidate_table(const char* db, const char* table);
void executor_invalidate_table(BufferPool* bp, const char* db, const char* table, uint32_t root_page_id) {
    mi_invalidate_table(db, table);
    heap_cache_forget(bp, root_page_id);
}

static void mi_invalidate_table(const char* db, const char* table) {
    mi_free_zombies();                                 // (motore in esclusiva: nessuna lettura in corso)
    MemIndex** pp = &g_mem_indexes;
    while (*pp) {
        MemIndex* ix = *pp;
        if (!strcasecmp(ix->db, db) && !strcasecmp(ix->table, table)) { *pp = ix->next; mi_free(ix); }
        else pp = &ix->next;
    }
}

// chiave canonica di un valore (NULL non viene indicizzato)
static void mi_key(const Value* v, DataType t, char* buf, size_t n, bool ci) {
    if (t != SV_TYPE_INT && t != SV_TYPE_BIGINT && t != SV_TYPE_FLOAT && t != SV_TYPE_BOOL) {   // testo
        const char* s = v->str_val ? v->str_val : "";
        char* f = ci ? ci_fold(s) : NULL;
        key_text(buf, n, f ? f : s);
        free(f);
    } else value_to_string((Value*)v, t, buf, n);
}

// chiave della tupla sulle colonne indicate; false se una colonna è NULL (non indicizzata:
// in SQL una chiave con NULL non è mai uguale a un'altra)
static bool tuple_key(const TableSchema* ts, const Tuple* t, const int* cols, int ncols, char* buf, size_t n) {
    size_t len = 0;
    buf[0] = '\0';
    for (int k = 0; k < ncols; k++) {
        int ci = cols[k];
        if (ci < 0 || ci >= t->num_values || t->values[ci].is_null) return false;
        if (k) { if (len + KEY_INLINE_MAX + 2 >= n) return false; buf[len++] = '\x1f'; buf[len] = '\0'; }
        mi_key(&t->values[ci], ts->columns[ci].type, buf + len, n - len, col_is_ci(ts, ci));
        len += strlen(buf + len);
    }
    return true;
}

static bool mi_add_key(MemIndex* ix, const char* key, RID rid) {
    if (strchr(key, '\x1e')) ix->has_long = true;
    SMSlot* s = sm_slot(&ix->map, key, true);
    if (!s) return false;
    if (s->val < 0) {
        if (ix->nlists >= ix->caplists) {
            int nc = ix->caplists ? ix->caplists * 2 : 64;
            RidList* nl = sv_realloc(ix->lists, sizeof(RidList) * (size_t)nc);
            if (!nl) return false;
            ix->lists = nl;
            char** nk = sv_realloc(ix->lkeys, sizeof(char*) * (size_t)nc);
            if (!nk) return false;
            ix->lkeys = nk;
            ix->caplists = nc;
        }
        char* kc = sv_strdup(key);
        if (!kc) return false;
        ix->lists[ix->nlists] = (RidList){ NULL, 0, 0 };
        ix->lkeys[ix->nlists] = kc;
        s->val = ix->nlists++;
        ix->ord_ok = false;
    }
    RidList* l = &ix->lists[s->val];
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 1;
        RID* nr = sv_realloc(l->r, sizeof(RID) * (size_t)nc);
        if (!nr) return false;
        l->r = nr;
        l->cap = nc;
    }
    l->r[l->n++] = rid;
    return true;
}

static bool mi_fault_in(MemIndex* ix, const char* key);

static bool mi_add_tuple(MemIndex* ix, const TableSchema* ts, const Tuple* t, RID rid) {
    char key[KEY_BUF];
    if (!tuple_key(ts, t, ix->cols, ix->ncols, key, sizeof(key))) return true;
    if (ix->base_root && !mi_fault_in(ix, key)) return false;
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
    char key[KEY_BUF];
    if (!tuple_key(ts, t, ix->cols, ix->ncols, key, sizeof(key))) return;
    if (ix->base_root && !mi_fault_in(ix, key)) return;
    SMSlot* s = sm_slot(&ix->map, key, false);
    if (!s || s->val < 0) return;
    RidList* l = &ix->lists[s->val];
    for (int i = 0; i < l->n; i++)
        if (l->r[i].page_id == rid.page_id && l->r[i].slot_id == rid.slot_id) { l->r[i] = l->r[--l->n]; return; }
}

// alberi su disco trovati illeggibili (pagina danneggiata): non si usano più fino al riavvio (P1.15)
#define BAD_ROOTS 64
static atomic_uint g_bad_roots[BAD_ROOTS];
static atomic_int g_nbad_roots;
static void tree_mark_bad(uint32_t root) {
    int k = atomic_fetch_add(&g_nbad_roots, 1);
    if (k < BAD_ROOTS) atomic_store(&g_bad_roots[k], root);
}
static bool tree_is_bad(uint32_t root) {
    int n = atomic_load(&g_nbad_roots);
    for (int i = 0; i < n && i < BAD_ROOTS; i++) if (atomic_load(&g_bad_roots[i]) == root) return true;
    return false;
}

// risultati delle ricerche nella base di un indice pigro: non si scrivono nell'indice (le letture in parallelo lo
// condividono), restano validi per le MI_SCRATCH ricerche successive dello stesso thread
#define MI_SCRATCH 8
static _Thread_local RidList g_mi_scratch[MI_SCRATCH];
static _Thread_local int g_mi_scratch_next;

static const RidList* mi_lookup(MemIndex* ix, const char* key) {
    SMSlot* s = sm_slot(&ix->map, key, false);
    if (s && s->val >= 0) return &ix->lists[s->val];
    if (!ix->base_root) return NULL;
    RID* r = NULL;
    int n = 0;
    int f = bpt_lookup(ix->bp, ix->base_root, key, &r, &n, NULL);
    if (f < 0) {                                   // lo statement fallisce; il prossimo ricostruisce l'indice dalla tabella
        tree_mark_bad(ix->base_root);
        heap_damage_note(ix->base_root, "an unreadable on-disk index (rebuilt from the table by the next statement)");
        return NULL;
    }
    if (f == 0) return NULL;
    RidList* out = &g_mi_scratch[g_mi_scratch_next];
    g_mi_scratch_next = (g_mi_scratch_next + 1) % MI_SCRATCH;
    free(out->r);
    *out = (RidList){ r, n, n };
    return out;
}

// prima di cambiare la chiave key di un indice pigro: la sua lista dalla base in map (motore in esclusiva)
static bool mi_fault_in(MemIndex* ix, const char* key) {
    if (!ix->base_root) return true;
    SMSlot* s = sm_slot(&ix->map, key, false);
    if (s && s->val >= 0) return true;
    RID* r = NULL;
    int n = 0;
    int f = bpt_lookup(ix->bp, ix->base_root, key, &r, &n, NULL);
    if (f < 0) {
        tree_mark_bad(ix->base_root);
        heap_damage_note(ix->base_root, "an unreadable on-disk index (rebuilt from the table by the next statement)");
        return false;
    }
    bool ok = mi_add_key(ix, key, (RID){0, 0}) ;                   // crea la voce (con un RID provvisorio)
    if (ok) {
        RidList* l = &ix->lists[sm_slot(&ix->map, key, false)->val];
        l->n = 0;                                                  // via il provvisorio
        for (int i = 0; i < n && ok; i++) ok = mi_add_key(ix, key, r[i]);
    }
    free(r);
    return ok;
}

// ── vista ordinata dell'indice: scansione per intervallo ──
//  Un indice hash trova solo chiavi uguali. Per "colonna < costante" o BETWEEN serve
//  l'ordine delle chiavi, come in un B+-albero: qui le chiavi distinte vengono ordinate
//  una volta (e di nuovo solo se ne arrivano di nuove) e l'intervallo si trova con una
//  ricerca binaria. I RID trovati vengono poi ordinati per pagina, così ogni pagina
//  della tabella viene letta una volta sola e nell'ordine del file.

static _Thread_local const MemIndex* g_ord_ix;

static int ord_cmp(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    if (g_ord_ix->okey) { int64_t p = g_ord_ix->okey[x], q = g_ord_ix->okey[y]; return p < q ? -1 : p > q; }
    return strcmp(g_ord_ix->lkeys[x], g_ord_ix->lkeys[y]);
}

// (chiamata con g_mi_lock in scrittura: più letture parallele possono arrivare qui insieme)
static bool mi_ordered(MemIndex* ix, bool numeric) {
    if (ix->ord_ok) return true;
    free(ix->ord);
    free(ix->okey);
    ix->okey = NULL;
    ix->ord = sv_malloc(sizeof(int) * (size_t)(ix->nlists ? ix->nlists : 1));
    if (!ix->ord) return false;
    if (numeric) {
        ix->okey = sv_malloc(sizeof(int64_t) * (size_t)(ix->nlists ? ix->nlists : 1));
        if (!ix->okey) { free(ix->ord); ix->ord = NULL; return false; }
        for (int i = 0; i < ix->nlists; i++) ix->okey[i] = strtoll(ix->lkeys[i], NULL, 10);
    }
    for (int i = 0; i < ix->nlists; i++) ix->ord[i] = i;
    g_ord_ix = ix;
    qsort(ix->ord, (size_t)ix->nlists, sizeof(int), ord_cmp);
    ix->nord = ix->nlists;
    ix->ord_ok = true;
    return true;
}

// confronto fra la chiave della lista li e un estremo dell'intervallo
static int ord_key_cmp(const MemIndex* ix, int li, const EvalVal* b) {
    if (ix->okey) {
        int64_t k = ix->okey[li];
        if (b->is_int) return k < b->inum ? -1 : k > b->inum;
        double d = (double)k;
        return d < b->num ? -1 : d > b->num;
    }
    int c = strcmp(ix->lkeys[li], ev_s(b));
    return c < 0 ? -1 : c > 0;
}

static int rid_cmp(const void* a, const void* b) {
    const RID* x = a; const RID* y = b;
    if (x->page_id != y->page_id) return x->page_id < y->page_id ? -1 : 1;
    return x->slot_id < y->slot_id ? -1 : x->slot_id > y->slot_id;
}

// RID delle righe con chiave in [lo, hi] (estremo NULL = illimitato), in ordine fisico.
// Gli estremi sono sempre inclusi: chi chiama riapplica le condizioni esatte.
static SvRwLock g_mi_lock;
static bool mi_range_locked(MemIndex* ix, bool numeric, const EvalVal* lo, const EvalVal* hi, RID** out, int* nout);

static bool mi_range(MemIndex* ix, bool numeric, const EvalVal* lo, const EvalVal* hi, RID** out, int* nout) {
    *out = NULL;
    *nout = 0;
    if (ix->base_root) return false;                            // indice pigro: l'intervallo legge la tabella
    if (!numeric && ix->has_long) return false;                 // testi lunghi: l'ordine delle chiavi non vale
    // la vista ordinata si costruisce al primo uso: con più letture in parallelo, una alla volta
    sv_rw_wr(&g_mi_lock);
    bool ok = mi_range_locked(ix, numeric, lo, hi, out, nout);
    sv_rw_wrunlock(&g_mi_lock);
    return ok;
}

static bool mi_range_locked(MemIndex* ix, bool numeric, const EvalVal* lo, const EvalVal* hi, RID** out, int* nout) {
    if (!mi_ordered(ix, numeric)) return false;
    int a = 0, b = ix->nord;
    if (lo) {                                   // prima chiave >= lo
        while (a < b) { int m = a + (b - a) / 2; if (ord_key_cmp(ix, ix->ord[m], lo) < 0) a = m + 1; else b = m; }
    }
    int end = ix->nord;
    if (hi) {                                   // prima chiave > hi
        int l = a, r = ix->nord;
        while (l < r) { int m = l + (r - l) / 2; if (ord_key_cmp(ix, ix->ord[m], hi) <= 0) l = m + 1; else r = m; }
        end = l;
    }
    size_t n = 0;
    for (int i = a; i < end; i++) n += (size_t)ix->lists[ix->ord[i]].n;
    RID* r = sv_malloc(sizeof(RID) * (n ? n : 1));
    if (!r) return false;
    n = 0;
    for (int i = a; i < end; i++) {
        const RidList* l = &ix->lists[ix->ord[i]];
        memcpy(r + n, l->r, sizeof(RID) * (size_t)l->n);
        n += (size_t)l->n;
    }
    qsort(r, n, sizeof(RID), rid_cmp);
    *out = r;
    *nout = (int)n;
    return true;
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
static MemIndex* mi_get_cols_locked(ExecCtx* ctx, const char* db, TableSchema* ts, const int* cols, int ncols);

// letture in parallelo: la ricerca e la costruzione dell'elenco degli indici sono serializzate;
// un indice costruito è poi solo letto (le scritture hanno il motore in esclusiva)
static SvRwLock g_mi_lock = SV_RWLOCK_INIT;

static MemIndex* mi_get_cols(ExecCtx* ctx, const char* db, TableSchema* ts, const int* cols, int ncols) {
    // versione confermata (MVCC): gli indici descrivono le righe attuali, si legge la tabella
    if (heap_snapshot_active(ts->root_page_id)) return NULL;
    sv_rw_wr(&g_mi_lock);
    MemIndex* ix = mi_get_cols_locked(ctx, db, ts, cols, ncols);
    sv_rw_wrunlock(&g_mi_lock);
    return ix;
}

static MemIndex* mi_get_cols_locked(ExecCtx* ctx, const char* db, TableSchema* ts, const int* cols, int ncols) {
    if (ncols < 1 || ncols > SV_MAX_KEY_COLS) return NULL;
    char name[SV_MAX_NAME_LEN * SV_MAX_KEY_COLS];
    size_t len = 0;
    name[0] = '\0';
    for (int k = 0; k < ncols; k++) {
        if (cols[k] < 0 || cols[k] >= ts->num_columns) return NULL;
        int w = snprintf(name + len, sizeof(name) - len, "%s%s", k ? "," : "", ts->columns[cols[k]].name);
        if (w < 0 || (size_t)w >= sizeof(name) - len) return NULL;
        len += (size_t)w;
    }
    for (MemIndex** pp = &g_mem_indexes; *pp; pp = &(*pp)->next) {
        MemIndex* ix = *pp;
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name) || strcasecmp(ix->col, name)) continue;
        if (ix->base_root && tree_is_bad(ix->base_root)) {          // ricostruito qui sotto; liberato più tardi (una
            *pp = ix->next;                                        // lettura in parallelo può ancora usarlo)
            ix->next = g_mi_zombies;
            g_mi_zombies = ix;
            break;
        }
        return ix;
    }
    MemIndex* ix = sv_calloc(1, sizeof(MemIndex));
    if (!ix) return NULL;
    snprintf(ix->db, sizeof(ix->db), "%s", db);
    snprintf(ix->table, sizeof(ix->table), "%s", ts->name);
    snprintf(ix->col, sizeof(ix->col), "%s", name);
    memcpy(ix->cols, cols, sizeof(int) * (size_t)ncols);
    ix->ncols = ncols;
    ix->bp = ctx->bp;
    ix->built_seq = g_ij_seq ? g_ij_seq() : 0;
    int ti = ncols == 1 ? tree_for(ts, cols[0]) : -1;
    if (ti >= 0) {                                              // P1.15: l'albero su disco fa da base, niente lettura
        ix->base_root = ts->tree_root[ti];
        ix->has_max = ts->tree_hasmax[ti] != 0;
        ix->max_int = ts->tree_max[ti];
        ix->next = g_mem_indexes;
        g_mem_indexes = ix;
        return ix;
    }
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
    if (!ok || sv_scan_cancelled() || heap_damage(NULL, 0)) { mi_free(ix); return NULL; }   // incompleto: scartato
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
// ── diario degli indici in memoria (ROLLBACK senza ricostruirli) ─────────────────────────────────
// Prima un ROLLBACK (anche di un solo statement fallito) buttava gli indici in memoria delle tabelle toccate e il
// primo statement dopo li ricostruiva leggendo tutta la tabella: BEGIN; INSERT 10 righe; ROLLBACK su una tabella di
// 100 000 righe = 100 000 righe rilette (1 300 rollback/s contro 89 000 di SQLite). Ora ogni chiave aggiunta / tolta
// dentro una transazione va nel diario con la sequenza della modifica di db_api; l'annullamento fa il contrario
// delle voci più recenti del punto a cui si torna. Un indice costruito dopo quel punto (contiene righe annullate
// senza diario) viene scartato come prima; senza memoria per il diario la transazione torna al vecchio modo.
typedef struct { uint64_t tx, seq; MemIndex* ix; char* key; RID rid; bool add; } IjEnt;
static IjEnt*   g_ij;
static int      g_nij, g_capij;
static uint64_t (*g_ij_tx)(void);        // transazione corrente (0 = nessuna)
#define IJ_MAX_LOST 16
static uint64_t g_ij_lost[IJ_MAX_LOST];  // transazioni con il diario incompleto
static int      g_nij_lost;
static bool     g_ij_lost_all;           // troppe: tutte

void executor_set_tx_source(uint64_t (*seq)(void), uint64_t (*tx)(void)) { g_ij_seq = seq; g_ij_tx = tx; }

static bool ij_lost(uint64_t tx) {
    if (g_ij_lost_all) return true;
    for (int i = 0; i < g_nij_lost; i++) if (g_ij_lost[i] == tx) return true;
    return false;
}

static void ij_mark_lost(uint64_t tx) {
    if (ij_lost(tx)) return;
    if (g_nij_lost < IJ_MAX_LOST) g_ij_lost[g_nij_lost++] = tx;
    else g_ij_lost_all = true;
}

static void ij_note(MemIndex* ix, const TableSchema* ts, const Tuple* t, RID rid, bool add) {
    uint64_t tx = g_ij_tx ? g_ij_tx() : 0;
    if (!tx || ij_lost(tx)) return;
    char key[KEY_BUF];
    if (!tuple_key(ts, t, ix->cols, ix->ncols, key, sizeof(key))) return;    // NULL: nella chiave non c'è
    if (g_nij == g_capij) {
        int nc = g_capij ? g_capij * 2 : 256;
        IjEnt* g = nc <= (1 << 22) ? sv_try_realloc(g_ij, sizeof(IjEnt) * (size_t)nc) : NULL;
        if (!g) { ij_mark_lost(tx); return; }
        g_ij = g;
        g_capij = nc;
    }
    char* kc = sv_try_malloc(strlen(key) + 1);
    if (!kc) { ij_mark_lost(tx); return; }
    strcpy(kc, key);
    g_ij[g_nij++] = (IjEnt){ tx, g_ij_seq ? g_ij_seq() : 0, ix, kc, rid, add };
}

static void ij_forget_index(const MemIndex* ix) {
    for (int i = 0; i < g_nij; i++) if (g_ij[i].ix == ix) g_ij[i].ix = NULL;
}

// voci della transazione tx dopo la sequenza after: via (COMMIT: after = 0; ROLLBACK [TO]: dopo l'annullamento)
void executor_index_journal_drop(uint64_t tx, uint64_t after) {
    int k = 0;
    for (int i = 0; i < g_nij; i++) {
        if (g_ij[i].tx == tx && g_ij[i].seq > after) { free(g_ij[i].key); continue; }
        g_ij[k++] = g_ij[i];
    }
    g_nij = k;
    if (after == 0) {
        for (int i = 0; i < g_nij_lost; i++) if (g_ij_lost[i] == tx) { g_ij_lost[i] = g_ij_lost[--g_nij_lost]; break; }
        if (g_nij == 0) { g_ij_lost_all = false; g_nij_lost = 0; }
    }
    if (g_nij == 0 && g_capij > 4096) { free(g_ij); g_ij = NULL; g_capij = 0; }
}

// dopo l'annullamento delle modifiche di tx più recenti di after sulla tabella: indici riportati indietro con il
// diario (o scartati se non si può), mappa dello spazio libero da rifare
void executor_index_undo(BufferPool* bp, const char* db, const char* table, uint32_t root_page_id, uint64_t tx, uint64_t after) {
    if (ij_lost(tx)) { executor_invalidate_table(bp, db, table, root_page_id); return; }
    mi_free_zombies();
    MemIndex** pp = &g_mem_indexes;
    while (*pp) {                                      // costruiti dopo il punto di ritorno: scartati
        MemIndex* ix = *pp;
        if (!strcasecmp(ix->db, db) && !strcasecmp(ix->table, table) && ix->built_seq > after) { *pp = ix->next; mi_free(ix); }
        else pp = &ix->next;
    }
    for (int i = g_nij - 1; i >= 0; i--) {
        IjEnt* e = &g_ij[i];
        if (e->tx != tx || e->seq <= after || !e->ix) continue;
        MemIndex* ix = e->ix;
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, table)) continue;
        if (e->add) {                                  // chiave aggiunta: via il suo RID
            SMSlot* s = sm_slot(&ix->map, e->key, false);
            if (!s || s->val < 0) continue;
            RidList* l = &ix->lists[s->val];
            for (int k = 0; k < l->n; k++)
                if (l->r[k].page_id == e->rid.page_id && l->r[k].slot_id == e->rid.slot_id) { l->r[k] = l->r[--l->n]; break; }
        } else if ((ix->base_root && !mi_fault_in(ix, e->key)) || !mi_add_key(ix, e->key, e->rid)) {
            executor_invalidate_table(bp, db, table, root_page_id);    // (memoria / albero illeggibile)
            return;
        }
    }
    heap_cache_forget(bp, root_page_id);
}

static void mi_on_insert(const char* db, TableSchema* ts, const Tuple* t, RID rid) {
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name)) continue;
        if (!mi_add_tuple(ix, ts, t, rid)) {
            mi_invalidate_table(db, ts->name);   // memoria esaurita: meglio ricostruire che usarlo incompleto
            return;
        }
        ij_note(ix, ts, t, rid, true);
    }
}

static void mi_on_delete(const char* db, TableSchema* ts, const Tuple* t, RID rid) {
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name)) continue;
        mi_remove_tuple(ix, ts, t, rid);
        ij_note(ix, ts, t, rid, false);
    }
}

// ── alberi su disco (P1.6, passo 1) ─────────────────────────────
// Al checkpoint finale (chiusura) e con CHECKPOINT, ogni indice in memoria di una colonna di una tabella con almeno
// TREE_MIN_ROWS righe viene scritto come B+tree di sola lettura (src/index/bptree.c) e la sua radice va nel catalogo
// (TableSchema.tree_*). Dopo un riavvio una ricerca per chiave legge radice e foglia (2-3 pagine) invece di
// costruire l'indice leggendo tutta la tabella. La prima scrittura sulla tabella azzera i suoi alberi e il motore
// lo registra nel log prima della riga (g_tree_hook); un albero vale solo se la tabella ha ancora la radice, e la
// colonna il tipo e la collation, di quando è stato scritto (ALTER / TRUNCATE riscrivono la tabella). Le pagine
// degli alberi abbandonati si liberano al checkpoint successivo (executor_trees_garbage), o con VACUUM.
#define TREE_MIN_ROWS 64

static void (*g_tree_hook)(void);
void executor_set_tree_hook(void (*hook)(void)) { g_tree_hook = hook; }

static uint32_t* g_tree_garbage;
static int g_ntree_garbage, g_captree_garbage;
static int g_trees_live;                    // tabelle del catalogo con alberi (0: nessun controllo in più)
static _Thread_local unsigned g_stmt_gen;   // statement corrente (controllo del catalogo una volta per statement)
static _Thread_local const TableSchema* g_tw_ts;
static _Thread_local unsigned g_tw_gen;

static void tree_garbage_add(uint32_t root) {
    if (!root) return;
    if (g_ntree_garbage == g_captree_garbage) {
        int nc = g_captree_garbage ? g_captree_garbage * 2 : 16;
        uint32_t* nv = sv_try_realloc(g_tree_garbage, sizeof(uint32_t) * (size_t)nc);
        if (!nv) return;                                    // pagine perse fino a un VACUUM
        g_tree_garbage = nv;
        g_captree_garbage = nc;
    }
    g_tree_garbage[g_ntree_garbage++] = root;
}

static void trees_drop(TableSchema* ts) {
    if (!ts || !ts->ntrees) return;
    for (int i = 0; i < ts->ntrees; i++) tree_garbage_add(ts->tree_root[i]);
    ts->ntrees = 0;
    if (g_trees_live > 0) g_trees_live--;
}

// radici degli alberi abbandonati (la lista passa a chi chiama, da liberare dopo un salvataggio del catalogo).
// Quelle ancora base di un indice pigro in memoria (P1.15) restano in lista: le sue pagine servono ancora
int executor_trees_garbage(uint32_t** roots) {
    uint32_t* out = sv_try_malloc(sizeof(uint32_t) * (size_t)(g_ntree_garbage ? g_ntree_garbage : 1));
    if (!out) { *roots = NULL; return 0; }
    int n = 0, k = 0;
    for (int i = 0; i < g_ntree_garbage; i++) {
        bool used = false;
        for (MemIndex* ix = g_mem_indexes; ix && !used; ix = ix->next) used = ix->base_root == g_tree_garbage[i];
        if (used) g_tree_garbage[k++] = g_tree_garbage[i];
        else out[n++] = g_tree_garbage[i];
    }
    g_ntree_garbage = k;
    *roots = out;
    return n;
}

// basi degli indici pigri in memoria (VACUUM non deve liberarne le pagine): n radici in *roots (malloc'd)
int executor_lazy_roots(uint32_t** roots) {
    int n = 0;
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) if (ix->base_root) n++;
    *roots = sv_try_malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
    if (!*roots) return 0;
    n = 0;
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) if (ix->base_root) (*roots)[n++] = ix->base_root;
    return n;
}

// indice pigro -> completo: le chiavi della base che non sono in map (P1.15). false = albero illeggibile o memoria
static bool lazy_fill_cb(void* ctx, const char* key, const RID* rids, int n) {
    MemIndex* ix = ctx;
    SMSlot* s = sm_slot(&ix->map, key, false);
    if (s && s->val >= 0) return true;                       // cambiata da allora: vale quella in memoria
    for (int i = 0; i < n; i++) if (!mi_add_key(ix, key, rids[i])) return false;
    return true;
}
static bool mi_materialize(MemIndex* ix) {
    if (!ix->base_root) return true;
    if (!bpt_iterate(ix->bp, ix->base_root, lazy_fill_cb, ix)) return false;
    ix->base_root = 0;
    return true;
}

void executor_trees_drop_all(Catalog* c) {
    for (int d = 0; c && d < c->num_databases; d++)
        for (int t = 0; t < c->databases[d].num_tables; t++) trees_drop(c->databases[d].tables[t]);
    g_trees_live = 0;
}

void executor_trees_count(Catalog* c) {
    g_trees_live = 0;
    for (int d = 0; c && d < c->num_databases; d++)
        for (int t = 0; t < c->databases[d].num_tables; t++) if (c->databases[d].tables[t]->ntrees) g_trees_live++;
}

// prima di registrare una modifica della tabella: i suoi alberi non valgono più
static MemIndex* mi_get_cols(ExecCtx* ctx, const char* db, TableSchema* ts, const int* cols, int ncols);
static bool mi_exists(const char* db, const char* table, const char* col);

// prima di abbandonare gli alberi di una tabella: un indice in memoria "pigro" per ognuno che ancora non ce l'ha
// (base = l'albero, in memoria solo le chiavi cambiate). Senza, la prima scrittura dopo un riavvio faceva costruire
// ogni altro indice della tabella leggendola tutta (P1.15: 1 678 pagine per 3 scritture e 5 ricerche su 20 000
// righe). Le pagine dell'albero restano finché un indice le usa (executor_trees_garbage)
static void trees_adopt(ExecCtx* ctx, TableSchema* ts) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return;
    for (int i = 0; i < ts->ntrees; i++) {
        int col = ts->tree_col[i];
        if (col < 0 || col >= ts->num_columns || tree_for(ts, col) < 0) continue;
        if (mi_exists(db->name, ts->name, ts->columns[col].name)) continue;
        (void)mi_get_cols(ctx, db->name, ts, &col, 1);          // pigro: nessuna lettura della tabella
    }
}

static void trees_on_write(ExecCtx* ctx, TableSchema* ts) {
    bool any = false;
    if (ts->ntrees) { trees_adopt(ctx, ts); trees_drop(ts); any = true; }
    if (g_trees_live > 0 && (g_tw_ts != ts || g_tw_gen != g_stmt_gen)) {      // ts copia di lavoro: quella del catalogo
        g_tw_ts = ts;
        g_tw_gen = g_stmt_gen;
        Database* db = catalog_current_db(ctx->catalog);
        TableSchema* c = db ? catalog_get_table(ctx->catalog, db->name, ts->name) : NULL;
        if (c && c != ts && c->ntrees) { trees_adopt(ctx, c); trees_drop(c); any = true; }
    }
    if (any && g_tree_hook) g_tree_hook();
}

// segno della piegatura delle chiavi in un albero su disco: 0 = testo così com'è, 2 = minuscolo UTF-8 (u8case.h).
// Gli alberi delle versioni precedenti hanno 1 (minuscolo solo ASCII): non coincide più e si ricostruiscono
static uint8_t tree_ci_mark(const TableSchema* ts, int col) { return col_is_ci(ts, col) ? 2 : 0; }

// albero valido della colonna (-1 = nessuno)
static int tree_for(const TableSchema* ts, int col) {
    for (int i = 0; i < ts->ntrees; i++)
        if (ts->tree_col[i] == col && ts->tree_root[i] && ts->tree_tabroot[i] == ts->root_page_id && !tree_is_bad(ts->tree_root[i]) &&
            ts->tree_type[i] == (uint8_t)ts->columns[col].type && ts->tree_ci[i] == tree_ci_mark(ts, col)) return i;
    return -1;
}

static bool mi_exists(const char* db, const char* table, const char* col) {
    sv_rw_rd(&g_mi_lock);
    bool f = false;
    for (MemIndex* ix = g_mem_indexes; ix && !f; ix = ix->next)
        f = !strcasecmp(ix->db, db) && !strcasecmp(ix->table, table) && !strcasecmp(ix->col, col);
    sv_rw_rdunlock(&g_mi_lock);
    return f;
}

// ricerca nell'albero su disco della colonna, se c'è e l'indice in memoria non è già costruito: 1 trovata / 0 assente
// (*r malloc'd), -1 = nessun albero utilizzabile (si usa l'indice in memoria)
static int tree_probe(ExecCtx* x, const char* db, TableSchema* ts, int col, const char* key, RID** r, int* n) {
    *r = NULL;
    *n = 0;
    int ti = tree_for(ts, col);
    if (ti < 0 || mi_exists(db, ts->name, ts->columns[col].name)) return -1;
    if (heap_snapshot_active(ts->root_page_id)) return -1;
    int f = bpt_lookup(x->bp, ts->tree_root[ti], key, r, n, NULL);
    if (f < 0) tree_mark_bad(ts->tree_root[ti]);               // illeggibile: da qui l'indice si costruisce dalla tabella
    return f < 0 ? -1 : f;
}

typedef struct { const char* key; int list; } TreeKey;
static int treekey_cmp(const void* a, const void* b) { return strcmp(((const TreeKey*)a)->key, ((const TreeKey*)b)->key); }

// scrive gli alberi degli indici in memoria che non ne hanno uno valido (nessuna transazione aperta: gli indici
// descrivono le righe confermate). Ritorna gli alberi scritti
int executor_trees_write(ExecCtx* ctx, uint64_t lsn) {
    int written = 0;
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (ix->ncols != 1 || ix->has_long) continue;
        TableSchema* ts = catalog_get_table(ctx->catalog, ix->db, ix->table);
        if (!ts || ts->num_rows < TREE_MIN_ROWS) continue;
        int col = ix->cols[0];
        if (col < 0 || col >= ts->num_columns || tree_for(ts, col) >= 0) continue;
        if (ix->base_root && !mi_materialize(ix)) continue;     // pigro con modifiche: base + modifiche
        TreeKey* tk = sv_try_malloc(sizeof(TreeKey) * (size_t)(ix->nlists ? ix->nlists : 1));
        BptEntry* e = sv_try_malloc(sizeof(BptEntry) * (size_t)(ix->nlists ? ix->nlists : 1));
        if (!tk || !e) { free(tk); free(e); continue; }
        int n = 0;
        for (int i = 0; i < ix->nlists; i++) if (ix->lists[i].n > 0) tk[n++] = (TreeKey){ ix->lkeys[i], i };
        qsort(tk, (size_t)n, sizeof(TreeKey), treekey_cmp);
        for (int i = 0; i < n; i++) e[i] = (BptEntry){ tk[i].key, ix->lists[tk[i].list].r, ix->lists[tk[i].list].n };
        uint32_t root = 0;
        int rc = bpt_build(ctx->bp, e, n, lsn, &root, NULL);
        free(tk);
        free(e);
        if (rc != SV_OK) continue;                          // chiave lunga o troppi doppioni: niente albero
        int slot = -1;
        for (int i = 0; i < ts->ntrees; i++) if (ts->tree_col[i] == col) slot = i;
        if (slot >= 0) tree_garbage_add(ts->tree_root[slot]);   // albero vecchio (tabella riscritta)
        else if (ts->ntrees < SV_MAX_TREES) { slot = ts->ntrees++; if (slot == 0) g_trees_live++; }
        else { tree_garbage_add(root); continue; }
        ts->tree_col[slot] = (uint16_t)col;
        ts->tree_type[slot] = (uint8_t)ts->columns[col].type;
        ts->tree_ci[slot] = tree_ci_mark(ts, col);
        ts->tree_root[slot] = root;
        ts->tree_tabroot[slot] = ts->root_page_id;
        ts->tree_hasmax[slot] = ix->has_max;
        ts->tree_max[slot] = ix->max_int;
        written++;
    }
    return written;
}

// ═════════════════════════════════════════════════════════════════
//  Sorgenti di righe (FROM / JOIN): tabelle o viste
// ═════════════════════════════════════════════════════════════════

#define MAX_SOURCES 61           // FROM + 60 JOIN, come MySQL (maschere delle sorgenti a 64 bit)
// colonna legata (ASTExpr.bound): sorgente s e colonna c codificate come s * BOUND_STRIDE + c + 1 (0 = non legata,
// negativo = alias della SELECT). Prima il passo era il letterale 1024, uguale per coincidenza a SV_MAX_COLUMNS (CR-B3)
#define BOUND_STRIDE      SV_MAX_COLUMNS
#define BOUND_MAKE(s, c)  ((s) * BOUND_STRIDE + (c) + 1)
#define BOUND_SRC(b)      (((b) - 1) / BOUND_STRIDE)
#define BOUND_COL(b)      (((b) - 1) % BOUND_STRIDE)
_Static_assert((long long)(MAX_SOURCES + 64) * BOUND_STRIDE < 2147483647LL, "ASTExpr.bound: sorgenti x colonne oltre un int");

// arena: righe lette da una tabella allocate a blocchi e liberate tutte insieme
typedef struct ArenaBlk { struct ArenaBlk* next; size_t used, cap; } ArenaBlk;
typedef struct { ArenaBlk* head; } Arena;

static void* arena_alloc(Arena* a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 - sizeof(ArenaBlk) ? n : 65536 - sizeof(ArenaBlk);
        ArenaBlk* b = sv_malloc(sizeof(ArenaBlk) + cap);
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
    char         db[SV_MAX_NAME_LEN];      // database della tabella ("" = il corrente)
    // sorgente laterale (LATERAL (subquery), JSON_TABLE, GENERATE_SERIES): righe ricalcolate per ogni
    // combinazione delle sorgenti a sinistra, tenute in pool fino alla prossima esecuzione del piano
    bool         lateral;
    struct SelectPlan* lat_plan;
    struct BindCtx* lat_bind;
    const struct TableFunc* tf;
    bool         lat_typed;                // tipi delle colonne della subquery già decisi dai valori
    Tuple**      pool;
    int          npool, cappool;
    // riferimenti alle colonne nella query (P1.14), contati dal binder (255 = molti, non si scende più): la scansione
    // copia nell'arena solo i testi delle colonne che servono dopo i filtri di lettura
    uint8_t      needc[SV_MAX_COLUMNS];
    // lettura a blocchi (P1.14, risultati in streaming): la scansione resta aperta fra un blocco e l'altro
    HeapScan*    bscan;
    HeapFile*    bhf;
    bool         bmore;                    // la scansione non è finita: c'è un altro blocco
} Source;

static void src_need(Source* S, int c) { if (S && c >= 0 && c < SV_MAX_COLUMNS && S->needc[c] < 255) S->needc[c]++; }

// i riferimenti di un filtro di lettura (già valutato sulla riga prima di copiarla) non tengono la colonna. Si
// scende solo in left / right / args: un riferimento in un'altra parte dell'albero resta contato (si copia)
static void need_minus(uint8_t* need, int s, const struct ASTExpr* e) {
    if (!e || e->type == EXPR_SUBQUERY) return;
    if (e->type == EXPR_COLUMN && e->bound > 0 && e->outer == 0 && BOUND_SRC(e->bound) == s) {
        int c = BOUND_COL(e->bound);
        if (need[c] > 0 && need[c] < 255) need[c]--;
        return;
    }
    need_minus(need, s, e->left);
    need_minus(need, s, e->right);
    for (int i = 0; i < e->nargs; i++) need_minus(need, s, e->args[i]);
}

static void plan_free(struct SelectPlan* p);
static void source_free(Source* s) {
    if (s->bscan) heap_scan_close(s->bscan);
    free(s->bhf);
    for (int i = 0; i < s->npool; i++) tuple_free(s->pool[i]);
    free(s->pool);
    if (s->lat_plan) plan_free(s->lat_plan);
    free(s->lat_bind);
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
        Tuple** nr = sv_realloc(s->rows, sizeof(Tuple*) * (size_t)nc);
        if (!nr) return false;
        s->rows = nr;
        RID* nd = sv_realloc(s->rids, sizeof(RID) * (size_t)nc);
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
    bool      rt_scope;         // ambiente di una routine / trigger: i suoi valori non cambiano durante una query
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
    { "LEFT", 2, 2 }, { "RIGHT", 2, 2 }, { "REPLACE", 3, 3 }, { "INSERT", 4, 4 }, { "REVERSE", 1, 1 },
    { "ABS", 1, 1 }, { "ROUND", 1, 2 }, { "FLOOR", 1, 1 }, { "CEIL", 1, 1 }, { "CEILING", 1, 1 },
    { "MOD", 2, 2 }, { "POWER", 2, 2 }, { "POW", 2, 2 }, { "SQRT", 1, 1 }, { "SIGN", 1, 1 },
    { "COALESCE", 1, 32 }, { "IFNULL", 2, 2 }, { "NULLIF", 2, 2 }, { "IF", 3, 3 },
    { "NOW", 0, 0 }, { "CURRENT_TIMESTAMP", 0, 0 }, { "CURDATE", 0, 0 }, { "CURRENT_DATE", 0, 0 },
    { "YEAR", 1, 1 }, { "MONTH", 1, 1 }, { "DAY", 1, 1 },
    { "JSON_EXTRACT", 2, 2 }, { "JSON_UNQUOTE", 1, 1 }, { "JSON_VALID", 1, 1 }, { "JSON_TYPE", 1, 1 },
    { "JSON_LENGTH", 1, 2 }, { "JSON_OBJECT", 0, 32 }, { "JSON_ARRAY", 0, 32 },
    { "MATCH_AGAINST", 3, 18 },
    { "DATABASE", 0, 0 }, { "SCHEMA", 0, 0 }, { "VERSION", 0, 0 }, { "LAST_INSERT_ID", 0, 1 },
    // MySQL
    { "LOCATE", 2, 3 }, { "INSTR", 2, 2 }, { "LPAD", 3, 3 }, { "RPAD", 3, 3 }, { "REPEAT", 2, 2 }, { "SPACE", 1, 1 },
    { "FIELD", 2, 32 }, { "FIND_IN_SET", 2, 2 }, { "ELT", 2, 32 }, { "SUBSTRING_INDEX", 3, 3 }, { "ASCII", 1, 1 }, { "ORD", 1, 1 },
    { "CHAR", 1, 32 }, { "HEX", 1, 1 }, { "UNHEX", 1, 1 }, { "MD5", 1, 1 }, { "SHA1", 1, 1 }, { "SHA", 1, 1 }, { "SHA2", 2, 2 },
    { "TO_BASE64", 1, 1 }, { "FROM_BASE64", 1, 1 }, { "QUOTE", 1, 1 }, { "STRCMP", 2, 2 }, { "FORMAT", 2, 3 },
    { "BIT_LENGTH", 1, 1 }, { "OCTET_LENGTH", 1, 1 }, { "CHARSET", 1, 1 }, { "COLLATION", 1, 1 },
    { "REGEXP_LIKE", 2, 3 }, { "REGEXP_REPLACE", 3, 6 }, { "REGEXP_SUBSTR", 2, 5 }, { "REGEXP_INSTR", 2, 6 }, { "TRIM_EX", 3, 3 },
    { "BIN", 1, 1 }, { "OCT", 1, 1 }, { "CRC32", 1, 1 }, { "INET_ATON", 1, 1 }, { "INET_NTOA", 1, 1 },
    { "RAND", 0, 1 }, { "PI", 0, 0 }, { "EXP", 1, 1 }, { "LN", 1, 1 }, { "LOG", 1, 2 }, { "LOG10", 1, 1 }, { "LOG2", 1, 1 },
    { "TRUNCATE", 2, 2 }, { "GREATEST", 2, 32 }, { "LEAST", 2, 32 }, { "RADIANS", 1, 1 }, { "DEGREES", 1, 1 },
    { "SIN", 1, 1 }, { "COS", 1, 1 }, { "TAN", 1, 1 }, { "COT", 1, 1 }, { "ASIN", 1, 1 }, { "ACOS", 1, 1 }, { "ATAN", 1, 2 },
    { "ATAN2", 2, 2 }, { "INTDIV", 2, 2 },
    { "DATE_FORMAT", 2, 2 }, { "TIME_FORMAT", 2, 2 }, { "STR_TO_DATE", 2, 2 }, { "UNIX_TIMESTAMP", 0, 1 }, { "FROM_UNIXTIME", 1, 2 },
    { "DATE", 1, 1 }, { "TIME", 1, 1 }, { "HOUR", 1, 1 }, { "MINUTE", 1, 1 }, { "SECOND", 1, 1 }, { "MICROSECOND", 1, 1 },
    { "DAYOFWEEK", 1, 1 }, { "DAYOFMONTH", 1, 1 }, { "DAYOFYEAR", 1, 1 }, { "WEEKDAY", 1, 1 }, { "WEEK", 1, 2 },
    { "WEEKOFYEAR", 1, 1 }, { "YEARWEEK", 1, 2 }, { "QUARTER", 1, 1 }, { "MONTHNAME", 1, 1 }, { "DAYNAME", 1, 1 },
    { "LAST_DAY", 1, 1 }, { "DATEDIFF", 2, 2 }, { "TIMEDIFF", 2, 2 }, { "TIMESTAMPDIFF", 3, 3 }, { "DATE_ADD", 2, 2 },
    { "DATE_SUB", 2, 2 }, { "ADDDATE", 2, 2 }, { "SUBDATE", 2, 2 }, { "__INTERVAL", 2, 2 }, { "EXTRACT", 2, 2 },
    { "MAKEDATE", 2, 2 }, { "MAKETIME", 3, 3 }, { "TO_DAYS", 1, 1 }, { "FROM_DAYS", 1, 1 }, { "TO_SECONDS", 1, 1 },
    { "SEC_TO_TIME", 1, 1 }, { "TIME_TO_SEC", 1, 1 }, { "CONVERT_TZ", 3, 3 }, { "CAST", 2, 2 },
    { "UTC_TIMESTAMP", 0, 0 }, { "UTC_DATE", 0, 0 }, { "UTC_TIME", 0, 0 }, { "CURTIME", 0, 0 }, { "CURRENT_TIME", 0, 0 },
    { "SYSDATE", 0, 0 }, { "LOCALTIME", 0, 0 }, { "LOCALTIMESTAMP", 0, 0 },
    { "FOUND_ROWS", 0, 0 }, { "ROW_COUNT", 0, 0 }, { "CONNECTION_ID", 0, 0 }, { "USER", 0, 0 }, { "CURRENT_USER", 0, 0 },
    { "SESSION_USER", 0, 0 }, { "SYSTEM_USER", 0, 0 }, { "UUID", 0, 0 }, { "UUID_SHORT", 0, 0 }, { "SLEEP", 1, 1 },
    { "GET_LOCK", 2, 2 }, { "RELEASE_LOCK", 1, 1 }, { "IS_FREE_LOCK", 1, 1 }, { "IS_USED_LOCK", 1, 1 }, { "RELEASE_ALL_LOCKS", 0, 0 },
    { "ANY_VALUE", 1, 1 }, { "ISNULL", 1, 1 }, { "VALUES", 1, 1 },
    // operatori e variabili (dal parser)
    { "__UVAR", 1, 1 }, { "__SYSVAR", 1, 1 }, { "__UASSIGN", 2, 2 }, { "__BITNOT", 1, 1 }, { "__COLLATE", 2, 2 }, { "__DEFAULT", 0, 0 }, { "__BITAND", 2, 2 },
    { "__BITOR", 2, 2 }, { "__BITXOR", 2, 2 }, { "__SHL", 2, 2 }, { "__SHR", 2, 2 }, { "__NSEQ", 2, 2 }, { "__XOR", 2, 2 },
    { "__PIPES", 2, 2 }, { "__ISTRUE", 1, 1 }, { "__ISFALSE", 1, 1 },
    // JSON (modifica e ricerca), testo, date
    { "JSON_SET", 3, 31 }, { "JSON_INSERT", 3, 31 }, { "JSON_REPLACE", 3, 31 }, { "JSON_REMOVE", 2, 32 },
    { "JSON_ARRAY_APPEND", 3, 31 }, { "JSON_KEYS", 1, 2 }, { "JSON_CONTAINS", 2, 3 }, { "JSON_CONTAINS_PATH", 3, 32 },
    { "JSON_SEARCH", 3, 3 }, { "JSON_PRETTY", 1, 1 }, { "JSON_DEPTH", 1, 1 }, { "JSON_MERGE_PATCH", 2, 32 },
    { "JSON_MERGE_PRESERVE", 2, 32 }, { "JSON_MERGE", 2, 32 }, { "JSON_ARRAY_INSERT", 3, 31 },
    { "JSON_QUOTE", 1, 1 }, { "JSON_STORAGE_SIZE", 1, 1 },
    { "INITCAP", 1, 1 }, { "SPLIT_PART", 3, 3 }, { "TRANSLATE", 3, 3 }, { "CHR", 1, 1 }, { "CONV", 3, 3 },
    { "SOUNDEX", 1, 1 }, { "OVERLAY", 3, 4 }, { "DATE_TRUNC", 2, 2 }, { "DATE_PART", 2, 2 }, { "STARTS_WITH", 2, 2 },
    { "REGEXP_COUNT", 2, 2 }, { "MAKE_SET", 2, 32 }, { "EXPORT_SET", 3, 5 },
    { "NEXTVAL", 1, 1 }, { "CURRVAL", 1, 1 }, { "SETVAL", 2, 3 }, { "LASTVAL", 0, 0 },
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
                if (l == BOUND_MAKE(*out_src, *out_col)) {
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

static void temporal_literal(const ASTExpr* col, ASTExpr* lit, BindCtx* b);

static void bind_expr(ASTExpr* e, BindCtx* b) {
    if (!e || b->err[0]) return;
    char msg[256];
    switch (e->type) {
        case EXPR_COLUMN: {
            e->bound = 0;
            e->outer = 0;
            e->rt_const = false;
            int s = 0, c = 0;
            int r = find_column(b, e->table_name, e->col_name, &s, &c);
            int rk, rc;
            if (r == 1 && !e->table_name[0] && b->plan && plan_joined_copy(b->plan, s, c, &rk, &rc) == JOIN_FULL) {
                // colonna di USING / NATURAL in un FULL JOIN: vale COALESCE(sinistra, destra)
                ASTExpr* l = sv_calloc(1, sizeof(ASTExpr));
                ASTExpr* rr = sv_calloc(1, sizeof(ASTExpr));
                ASTExpr** args = sv_calloc(2, sizeof(ASTExpr*));
                if (!l || !rr || !args) { free(l); free(rr); free(args); bind_error(b, "Out of memory"); return; }
                l->type = rr->type = EXPR_COLUMN;
                snprintf(l->col_name, sizeof(l->col_name), "%s", e->col_name);
                snprintf(rr->col_name, sizeof(rr->col_name), "%s", e->col_name);
                snprintf(l->table_name, sizeof(l->table_name), "%s", b->src[s].name);
                snprintf(rr->table_name, sizeof(rr->table_name), "%s", b->src[rk].name);
                l->bound = BOUND_MAKE(s, c);
                rr->bound = BOUND_MAKE(rk, rc);
                src_need(&b->src[s], c);
                src_need(&b->src[rk], rc);
                args[0] = l; args[1] = rr;
                e->type = EXPR_FUNC;
                snprintf(e->func, sizeof(e->func), "COALESCE");
                e->args = args; e->nargs = 2; e->depth = 2;
                return;
            }
            if (r == 1) { e->bound = BOUND_MAKE(s, c); src_need(&b->src[s], c); return; }
            if (r == 0) {
                // colonna di una query esterna: la subquery è correlata
                int level = 1;
                for (BindCtx* o = b->outer; o && r == 0; o = o->outer, level++) {
                    r = find_column(o, e->table_name, e->col_name, &s, &c);
                    if (r == 1) {
                        e->bound = BOUND_MAKE(s, c);
                        e->outer = level;
                        src_need(&o->src[s], c);
                        // variabile della routine nella query principale di un'istruzione (ripianificata a ogni
                        // esecuzione): una costante, così vale per gli indici. Nelle subquery (piani tenuti
                        // nell'AST e riusati nei cicli) resta un riferimento correlato
                        if (o->rt_scope && level == 1 && b->depth == 0) { e->rt_const = true; return; }
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
            // DEFAULT come valore di INSERT ... VALUES e di UPDATE ... SET col = DEFAULT (una colonna vera chiamata
            // `default` resta una colonna: qui si arriva solo se non c'è)
            if (!e->table_name[0] && !strcasecmp(e->col_name, "DEFAULT") && b->clause &&
                (!strcmp(b->clause, "VALUES") || !strcmp(b->clause, "SET"))) {
                e->type = EXPR_FUNC;
                snprintf(e->func, sizeof(e->func), "__DEFAULT");
                e->col_name[0] = '\0';
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
                // funzione definita dall'utente (CREATE FUNCTION) nel database corrente
                Database* udb = b->x ? catalog_current_db(b->x->catalog) : NULL;
                bool udf = false;
                for (int i = 0; udb && i < udb->num_routines && !udf; i++)
                    udf = udb->routines[i].is_function && !strcasecmp(udb->routines[i].name, e->func);
                if (udf) {
                    e->subkind = SUBK_UDF;
                    for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
                    return;
                }
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
                { "PERCENT_RANK", 0, 0 }, { "CUME_DIST", 0, 0 }, { "NTH_VALUE", 2, 2 },
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
            if (e->win->frame == FRAME_RANGE && e->win->nord != 1 &&
                (e->win->fs == FB_PREC || e->win->fs == FB_FOLL || e->win->fe == FB_PREC || e->win->fe == FB_FOLL)) {
                bind_error(b, "A RANGE frame with n PRECEDING / FOLLOWING needs exactly one ORDER BY expression");
                return;
            }
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            for (int i = 0; i < e->win->npart; i++) bind_expr(e->win->part[i], b);
            for (int i = 0; i < e->win->nord; i++) bind_expr(e->win->ord[i], b);
            if (!b->err[0] && !plan_add_window(b->plan, e)) bind_error(b, "Too many window functions in one query (max 64)");
            return;
        }
        default:
            bind_expr(e->left, b);
            bind_expr(e->right, b);
            if (e->type == EXPR_BINARY && e->op <= OP_GE && !b->err[0]) {   // = <> < > <= >=
                temporal_literal(e->left, e->right, b);
                temporal_literal(e->right, e->left, b);
            }
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

// nome della funzione SQL f uguale al letterale lit? Prima la lunghezza (costante) e il primo carattere, poi memcmp:
// le catene di centinaia di confronti per riga diventano quasi solo confronti fra interi (CR-M2)
#define FIS(lit) (flen == sizeof(lit) - 1 && f[0] == (lit)[0] && !memcmp(f, (lit), sizeof(lit) - 1))
static void eval_expr(ASTExpr* e, EvalCtx* c, EvalVal* out);
static bool eval_cond(ASTExpr* e, EvalCtx* c);
static void eval_subquery(ASTExpr* e, EvalCtx* c, EvalVal* out);
static void ev_from_oval(EvalVal* v, const SortKey* o);


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

// Testi calcolati più lunghi del buffer di EvalVal (CONCAT, REPLACE, ...): copiati in un'arena
// che vive quanto lo statement (liberata quando executor_run torna al livello più esterno).
#define EV_TEXT_MAX SV_MAX_TEXT_BYTES
// per-thread: preparazione all'esecuzione in parallelo (ogni thread ha la sua arena e la sua profondità)
static _Thread_local Arena g_text_arena;
static _Thread_local int   g_run_depth;

// copia s nell'arena (o nel buffer se ci sta); false se troppo lungo o senza memoria
static bool ev_text_long(EvalVal* out, const char* s, size_t n) {
    if (n <= EV_STR_MAX) return ev_text(out, s, n);
    if (n > EV_TEXT_MAX) return false;
    char* p = arena_alloc(&g_text_arena, n + 1);
    if (!p) return false;
    memcpy(p, s, n);
    p[n] = '\0';
    out->null = false; out->is_num = out->is_int = false; out->is_dec = false;
    out->str = p;
    return true;
}

static void ev_set_text(EvalCtx* c, EvalVal* out, const char* s, size_t n) {
    if (!ev_text_long(out, s, n)) {
        ctx_error(c, "%s", n > EV_TEXT_MAX ? "String result longer than 16 MB" : "Out of memory");
        ev_null(out);
    }
}

// ── variabili di sessione (@x) ───────────────────────────────────
// Ogni sessione ha le sue (db_api le scambia con executor_set_uservars); i nomi non distinguono
// maiuscole e minuscole, come in MySQL. Una variabile mai assegnata vale NULL.
typedef struct { char name[64]; bool null, is_num, is_int; int64_t inum; double num; char* text; } UVar;   // text: anche dei DECIMAL
typedef struct { char name[64]; char* sql; } UPrep;
struct UserVars { UVar* v; int n, cap; UPrep* prep; int nprep; };
// Protezione: stato della sessione attiva, cambiato da db_api sotto db_lock; le letture parallele
// (db_execute_readonly) rifiutano le @variabili, quindi non lo toccano.
static UserVars* g_uvars;

UserVars* executor_uservars(void) { return g_uvars; }
void executor_set_uservars(UserVars* v) { g_uvars = v; }
void executor_uservars_free(UserVars* v) {
    if (!v) return;
    for (int i = 0; i < v->n; i++) free(v->v[i].text);
    free(v->v);
    for (int i = 0; i < v->nprep; i++) free(v->prep[i].sql);
    free(v->prep);
    if (g_uvars == v) g_uvars = NULL;           // prima di free: dopo, il valore di v è indeterminato (CR-B4)
    free(v);
}

static UVar* uservar_find(const char* name, bool create) {
    if (!g_uvars) {
        if (!create) return NULL;
        g_uvars = sv_calloc(1, sizeof(UserVars));
        if (!g_uvars) return NULL;
    }
    for (int i = 0; i < g_uvars->n; i++) if (!strcasecmp(g_uvars->v[i].name, name)) return &g_uvars->v[i];
    if (!create) return NULL;
    if (g_uvars->n == g_uvars->cap) {
        int nc = g_uvars->cap ? g_uvars->cap * 2 : 16;
        UVar* nv = sv_realloc(g_uvars->v, sizeof(UVar) * (size_t)nc);
        if (!nv) return NULL;
        g_uvars->v = nv;
        g_uvars->cap = nc;
    }
    UVar* u = &g_uvars->v[g_uvars->n++];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->null = true;
    return u;
}

static void uservar_set(const char* name, const EvalVal* v) {
    UVar* u = uservar_find(name, true);
    if (!u) return;
    char* nt = NULL;
    // testo (anche di un numero DECIMAL, che tiene i suoi decimali: 115.00)
    if (!v->null && (!v->is_num || (v->str && !v->is_int))) { nt = sv_strdup(ev_s(v)); if (!nt) return; }
    free(u->text);
    u->text = nt;
    u->null = v->null;
    u->is_num = !v->null && v->is_num;
    u->is_int = u->is_num && v->is_int;
    u->inum = v->inum;
    u->num = v->num;
}

// PREPARE nome FROM '...': testo delle istruzioni preparate della sessione
bool executor_prepare_store(const char* name, const char* sql) {
    if (!g_uvars && !(g_uvars = sv_calloc(1, sizeof(UserVars)))) return false;
    char* dup = sv_strdup(sql);
    if (!dup) return false;
    for (int i = 0; i < g_uvars->nprep; i++)
        if (!strcasecmp(g_uvars->prep[i].name, name)) { free(g_uvars->prep[i].sql); g_uvars->prep[i].sql = dup; return true; }
    UPrep* np = sv_realloc(g_uvars->prep, sizeof(UPrep) * (size_t)(g_uvars->nprep + 1));
    if (!np) { free(dup); return false; }
    g_uvars->prep = np;
    snprintf(np[g_uvars->nprep].name, sizeof(np[0].name), "%s", name);
    np[g_uvars->nprep++].sql = dup;
    return true;
}
const char* executor_prepare_get(const char* name) {
    for (int i = 0; g_uvars && i < g_uvars->nprep; i++) if (!strcasecmp(g_uvars->prep[i].name, name)) return g_uvars->prep[i].sql;
    return NULL;
}
bool executor_prepare_drop(const char* name) {
    for (int i = 0; g_uvars && i < g_uvars->nprep; i++)
        if (!strcasecmp(g_uvars->prep[i].name, name)) {
            free(g_uvars->prep[i].sql);
            g_uvars->prep[i] = g_uvars->prep[--g_uvars->nprep];
            return true;
        }
    return false;
}
// valore di @nome come letterale SQL (NULL, numero, 'testo' con apici e backslash raddoppiati); malloc'd
char* executor_uservar_literal(const char* name) {
    UVar* u = uservar_find(name, false);
    if (!u || u->null) return sv_strdup("NULL");
    char b[64];
    if (u->is_num && !u->text) { if (u->is_int) snprintf(b, sizeof(b), "%lld", (long long)u->inum); else snprintf(b, sizeof(b), "%.17g", u->num); return sv_strdup(b); }
    if (u->is_num) return sv_strdup(u->text);
    const char* t = u->text ? u->text : "";
    char* o = sv_malloc(strlen(t) * 2 + 3);
    if (!o) return NULL;
    size_t k = 0;
    o[k++] = '\'';
    for (; *t; t++) { if (*t == '\'' || *t == '\\') o[k++] = *t; o[k++] = *t; }
    o[k++] = '\'';
    o[k] = '\0';
    return o;
}

static void uservar_get(const char* name, EvalVal* out) {
    UVar* u = uservar_find(name, false);
    if (!u || u->null) { ev_null(out); return; }
    if (u->is_int) { ev_int(out, u->inum); return; }
    if (u->is_num && !u->text) { ev_num(out, u->num); return; }
    if (u->is_num) { out->null = false; out->is_num = true; out->is_int = false; out->num = u->num; out->ci = false; out->str = u->text;
                     Dec ud; out->is_dec = u->text && dec_parse(u->text, &ud); return; }   // @v = 0.10: resta esatto
    out->null = false; out->is_num = out->is_int = false; out->ci = false; out->is_dec = false;
    out->str = u->text ? u->text : "";
}

// variabili di sistema (@@x): i valori che il server MySQL di SegVault annuncia
static const char* const SYSVARS[][2] = {
        { "version", "8.0.35-SegVault" }, { "version_comment", "SegVault" }, { "version_compile_os", "SegVault" },
        { "max_allowed_packet", "67108864" }, { "sql_mode", "" }, { "autocommit", "1" },
        { "character_set_client", "utf8mb4" }, { "character_set_connection", "utf8mb4" }, { "character_set_results", "utf8mb4" },
        { "character_set_server", "utf8mb4" }, { "character_set_database", "utf8mb4" }, { "collation_connection", "utf8mb4_general_ci" },
        { "collation_server", "utf8mb4_general_ci" }, { "collation_database", "utf8mb4_general_ci" }, { "wait_timeout", "28800" },
        { "interactive_timeout", "28800" }, { "lower_case_table_names", "1" }, { "transaction_isolation", "SERIALIZABLE" },
        { "tx_isolation", "SERIALIZABLE" }, { "transaction_read_only", "0" }, { "time_zone", "SYSTEM" }, { "system_time_zone", "UTC" },
        { "auto_increment_increment", "1" }, { "auto_increment_offset", "1" }, { "max_connections", "64" },
        { "default_storage_engine", "SegVault" }, { "storage_engine", "SegVault" }, { "sql_auto_is_null", "0" },
        { "sql_select_limit", "18446744073709551615" }, { "group_concat_max_len", "1024" }, { "max_sp_recursion_depth", "16" },
        { "hostname", "localhost" }, { "port", "3306" }, { "license", "MIT" }, { "warning_count", "0" }, { "error_count", "0" },
        { "foreign_key_checks", "ON" }, { "unique_checks", "ON" }, { "sql_safe_updates", "OFF" }, { "innodb_strict_mode", "ON" },
        { "explicit_defaults_for_timestamp", "ON" }, { "default_week_format", "0" }, { "div_precision_increment", "4" },
        { NULL, NULL },
};
static const char* sysvar_value(const char* name) {
    for (size_t i = 0; SYSVARS[i][0]; i++) if (!strcasecmp(SYSVARS[i][0], name)) return SYSVARS[i][1];
    return NULL;
}

// operandi e risultati delle operazioni bit a bit: interi a 64 bit senza segno, come MySQL
static uint64_t bit_operand(const EvalVal* v) {
    if (v->is_int) return (uint64_t)v->inum;
    double d = ev_as_num(v);
    if (d < 0) return (uint64_t)(int64_t)llround(d);
    return d >= 18446744073709551615.0 ? UINT64_MAX : (uint64_t)llround(d);
}
static void ev_unsigned(EvalCtx* c, EvalVal* out, uint64_t r) {
    if (r <= (uint64_t)INT64_MAX) { ev_int(out, (int64_t)r); return; }
    char b[24];
    snprintf(b, sizeof(b), "%llu", (unsigned long long)r);
    ev_set_text(c, out, b, strlen(b));
    out->is_num = true;
    out->is_dec = true;                 // intero senza segno oltre 2^63: esatto come decimale
    out->num = (double)r;
}

// operatori di MySQL e variabili, scritti dal parser come funzioni interne "__..."
// collation esplicita di un operando (x COLLATE nome, BINARY x): 1 = distingue maiuscole e minuscole (binary, _bin,
// _cs), 2 = no (_ci), 0 = nessuna. Come in MySQL prevale su quella della colonna
static int coll_kind(const ASTExpr* x) {
    if (!x || x->type != EXPR_FUNC || strcmp(x->func, "__COLLATE") || x->nargs != 2 || !x->args[1]->str_val) return 0;
    const char* n = x->args[1]->str_val;
    size_t l = strlen(n);
    if (l >= 3 && !strcasecmp(n + l - 3, "_ci")) return 2;
    return 1;
}

static bool eval_internal(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    const size_t flen = strlen(f);                       // per FIS: confronti per lunghezza prima (CR-M2)
    if (f[0] != '_' || f[1] != '_') return false;
    if (FIS("__DEFAULT")) { ev_null(out); return true; }   // DEFAULT fuori da VALUES / SET col = DEFAULT
    if (FIS("__UVAR"))   { uservar_get(e->args[0]->str_val, out); return true; }
    if (FIS("__SYSVAR")) {
        if (!strcasecmp(e->args[0]->str_val, "foreign_key_checks")) { ev_int(out, executor_fk_checks()); return true; }
        if (!strcasecmp(e->args[0]->str_val, "cache_size")) { ev_int(out, db_cache_size()); return true; }
        if (!strcasecmp(e->args[0]->str_val, "synchronous")) {
            out->null = false; out->is_num = out->is_int = false; out->ci = false; out->is_dec = false; out->str = db_synchronous();
            return true;
        }
        if (!strcasecmp(e->args[0]->str_val, "identity") || !strcasecmp(e->args[0]->str_val, "last_insert_id")) { ev_int(out, g_last_insert_id); return true; }
        const char* v = sysvar_value(e->args[0]->str_val);
        if (!v) { ctx_error(c, "Unknown system variable '%s'", e->args[0]->str_val); ev_null(out); return true; }
        double d;
        if (v[0] && text_is_number(v, &d) && strlen(v) < 18) ev_int(out, (int64_t)d);
        else { out->null = false; out->is_num = out->is_int = false; out->ci = false; out->is_dec = false; out->str = v; }
        return true;
    }
    if (FIS("__UASSIGN")) {
        EvalVal v;
        eval_expr(e->args[1], c, &v);
        uservar_set(e->args[0]->str_val, &v);
        uservar_get(e->args[0]->str_val, out);
        return true;
    }
    EvalVal a, b;
    eval_expr(e->args[0], c, &a);
    if (FIS("__ISTRUE") || FIS("__ISFALSE")) {   // x IS TRUE / IS FALSE: mai NULL
        int t = ev_truth(&a);
        ev_int(out, f[4] == 'T' ? t == 1 : t == 0);
        return true;
    }
    if (FIS("__BITNOT")) { if (a.null) ev_null(out); else ev_unsigned(c, out, ~bit_operand(&a)); return true; }
    if (FIS("__COLLATE")) {                             // x COLLATE nome, BINARY x: il valore, con la collation scelta
        ev_copy(out, &a);
        if (!a.null && !a.is_num) out->ci = coll_kind(e) == 2;
        return true;
    }
    eval_expr(e->args[1], c, &b);
    if (FIS("__NSEQ")) {                                // a <=> b: NULL <=> NULL è vero
        if (a.null || b.null) ev_int(out, a.null && b.null);
        else ev_int(out, ev_cmp(&a, &b) == 0);
        return true;
    }
    if (FIS("__PIPES")) {
        if (executor_mysql_mode()) {                           // MySQL: || è OR
            int x = ev_truth(&a), y = ev_truth(&b);
            if (x == 1 || y == 1) ev_int(out, 1);
            else if (x < 0 || y < 0) ev_null(out);
            else ev_int(out, 0);
            return true;
        }
        if (a.null || b.null) { ev_null(out); return true; }   // standard: concatenazione
        const char* x = ev_s(&a);
        size_t lx = strlen(x);
        char* buf = sv_malloc(lx + strlen(ev_s(&b)) + 1);
        if (!buf) { ctx_error(c, "%s", "Out of memory"); ev_null(out); return true; }
        memcpy(buf, x, lx);
        strcpy(buf + lx, ev_s(&b));
        ev_set_text(c, out, buf, strlen(buf));
        free(buf);
        return true;
    }
    if (FIS("__XOR")) {
        int x = ev_truth(&a), y = ev_truth(&b);
        if (x < 0 || y < 0) ev_null(out); else ev_int(out, x != y);
        return true;
    }
    if (a.null || b.null) { ev_null(out); return true; }
    uint64_t x = bit_operand(&a), y = bit_operand(&b), r;
    if (FIS("__BITAND"))      r = x & y;
    else if (FIS("__BITOR"))  r = x | y;
    else if (FIS("__BITXOR")) r = x ^ y;
    else if (FIS("__SHL"))    r = y >= 64 ? 0 : x << y;
    else if (FIS("__SHR"))    r = y >= 64 ? 0 : x >> y;
    else return false;
    ev_unsigned(c, out, r);
    return true;
}

static void rt_eval_function(ASTExpr* e, EvalCtx* c, EvalVal* out);

// ── sequenze (CREATE SEQUENCE, NEXTVAL / CURRVAL / SETVAL): come in PostgreSQL non sono transazionali;
//    stato in un file di testo accanto al catalogo, riscritto a ogni cambio ──
typedef struct {
    char db[64], name[64]; int64_t value, inc, minv, maxv, start; bool cycle, called;
    // riserva (P3.9): NEXTVAL salva "res" (fino a 32 incrementi avanti) e poi dà i valori fino a lì senza scrivere il
    // file; dopo una caduta si riparte da res, quindi nessun valore già dato si ripete (resta un salto, come la
    // cache delle sequenze di PostgreSQL). Alla chiusura pulita si salva il valore esatto
    int64_t res; bool has_res;
} Seq;
// Protezione: solo con il motore in esclusiva; db_execute_readonly rifiuta NEXTVAL / SETVAL.
static Seq*  g_seq;
static int   g_nseq, g_capseq;
static char  g_seq_path[1100];

// nome nel file: %XX per spazi, controlli, '%' (prima un nome con spazi non si rileggeva più)
static void seq_put_name(FILE* f, const char* n) {
    for (const unsigned char* p = (const unsigned char*)n; *p; p++)
        if (*p <= ' ' || *p == '%' || *p == 0x7f) fprintf(f, "%%%02X", *p); else fputc(*p, f);
}
static void seq_get_name(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(h, NULL, 16);
            r += 2;
        } else *w++ = *r;
    }
    *w = '\0';
}

// stato delle sequenze nel file: temporaneo scritto e sincronizzato, poi sostituzione atomica (prima remove + rename
// senza fsync: una caduta in mezzo perdeva il file e le sequenze ripartivano dall'inizio). false = non salvato
static bool seq_save(void) {
    if (!g_seq_path[0]) return true;
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_seq_path);
    FILE* f = sv_fopen_private(tmp, "w");
    if (!f) return false;
    bool ok = true;
    for (int i = 0; i < g_nseq && ok; i++) {
        Seq* q = &g_seq[i];
        seq_put_name(f, q->db);
        fputc(' ', f);
        seq_put_name(f, q->name);
        int64_t v = q->has_res ? q->res : q->value;
        bool called = q->has_res ? true : q->called;
        ok = fprintf(f, " %lld %lld %lld %lld %lld %d %d\n", (long long)v, (long long)q->inc,
                     (long long)q->minv, (long long)q->maxv, (long long)q->start, q->cycle, called) > 0;
    }
    ok = ok && fflush(f) == 0 && sv_fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || sv_replace_file(tmp, g_seq_path) != 0) { remove(tmp); return false; }
    return true;
}

// chiusura pulita: valori esatti, senza la riserva
void executor_seq_flush(void) {
    bool any = false;
    for (int i = 0; i < g_nseq; i++) if (g_seq[i].has_res) { g_seq[i].has_res = false; any = true; }
    if (any) seq_save();
}

void executor_set_seq_file(const char* path) {
    free(g_seq);
    g_seq = NULL;
    g_nseq = g_capseq = 0;
    snprintf(g_seq_path, sizeof(g_seq_path), "%s", path ? path : "");
    FILE* f = path ? fopen(path, "r" SV_FE) : NULL;
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        Seq q;
        memset(&q, 0, sizeof(q));
        long long v, inc, mn, mx, st;
        int cy, ca;
        if (sscanf(line, "%63s %63s %lld %lld %lld %lld %lld %d %d", q.db, q.name, &v, &inc, &mn, &mx, &st, &cy, &ca) != 9) continue;
        seq_get_name(q.db);
        seq_get_name(q.name);
        q.value = v; q.inc = inc; q.minv = mn; q.maxv = mx; q.start = st; q.cycle = cy; q.called = ca;
        if (g_nseq == g_capseq) {
            int nc = g_capseq ? g_capseq * 2 : 16;
            Seq* nv = sv_realloc(g_seq, sizeof(Seq) * (size_t)nc);
            if (!nv) break;
            g_seq = nv; g_capseq = nc;
        }
        g_seq[g_nseq++] = q;
    }
    fclose(f);
}

static Seq* seq_find(const char* db, const char* name) {
    for (int i = 0; i < g_nseq; i++) if (!strcasecmp(g_seq[i].db, db) && !strcasecmp(g_seq[i].name, name)) return &g_seq[i];
    return NULL;
}

// "s" nel database corrente o "db.s"
static Seq* seq_lookup(ExecCtx* x, const char* ref, char* err, size_t en) {
    char db[64] = "", name[64];
    const char* dot = strchr(ref, '.');
    if (dot) { snprintf(db, sizeof(db), "%.*s", (int)(dot - ref) > 63 ? 63 : (int)(dot - ref), ref); snprintf(name, sizeof(name), "%.63s", dot + 1); }
    else {
        Database* d = x ? catalog_current_db(x->catalog) : NULL;
        if (!d) { snprintf(err, en, "No database selected"); return NULL; }
        snprintf(db, sizeof(db), "%s", d->name);
        snprintf(name, sizeof(name), "%.63s", ref);
    }
    Seq* q = seq_find(db, name);
    if (!q) snprintf(err, en, "Sequence '%.64s' does not exist", ref);
    return q;
}

void executor_drop_db_sequences(const char* db) {
    int k = 0;
    for (int i = 0; i < g_nseq; i++) if (strcasecmp(g_seq[i].db, db)) g_seq[k++] = g_seq[i];
    if (k != g_nseq) { g_nseq = k; seq_save(); }
}

static void seq_remember(const Seq* q) {                       // CURRVAL / LASTVAL della sessione
    char key[160];
    snprintf(key, sizeof(key), "\x01seq.%s.%s", q->db, q->name);
    EvalVal v;
    ev_int(&v, q->value);
    uservar_set(key, &v);
    uservar_set("\x01seq.last", &v);
}

// NEXTVAL / CURRVAL / SETVAL / LASTVAL
static bool eval_sequence(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    const size_t flen = strlen(f);                       // per FIS: confronti per lunghezza prima (CR-M2)
    if ((!FIS("NEXTVAL")) && (!FIS("CURRVAL")) && (!FIS("SETVAL")) && (!FIS("LASTVAL"))) return false;
    if (g_describe) { ev_null(out); return true; }      // PREPARE: solo la forma del risultato, nessun effetto
    if (FIS("LASTVAL")) {
        uservar_get("\x01seq.last", out);
        if (out->null) ctx_error(c, "%s", "LASTVAL is not yet defined in this session");
        return true;
    }
    EvalVal a;
    eval_expr(e->args[0], c, &a);
    if (a.null) { ev_null(out); return true; }
    char ref[130], err[200];
    snprintf(ref, sizeof(ref), "%.129s", ev_s(&a));
    Seq* q = seq_lookup(c->x, ref, err, sizeof(err));
    if (!q) { ctx_error(c, "%s", err); ev_null(out); return true; }
    if (FIS("CURRVAL")) {
        char key[160];
        snprintf(key, sizeof(key), "\x01seq.%s.%s", q->db, q->name);
        uservar_get(key, out);
        if (out->null) ctx_error(c, "currval of sequence '%s' is not yet defined in this session", q->name);
        return true;
    }
    if (FIS("SETVAL")) {
        EvalVal v, called;
        eval_expr(e->args[1], c, &v);
        if (e->nargs > 2) eval_expr(e->args[2], c, &called); else ev_int(&called, 1);
        if (v.null) { ev_null(out); return true; }
        int64_t x = (int64_t)ev_as_num(&v);
        if (x < q->minv || x > q->maxv) { ctx_error(c, "setval: value %.40s is out of bounds for the sequence", ev_s(&v)); ev_null(out); return true; }
        q->value = x;
        q->called = ev_truth(&called) == 1;
        q->has_res = false;
        if (!seq_save()) { ctx_error(c, "setval: cannot save the state of sequence '%s' (disk?)", q->name); ev_null(out); return true; }
        if (q->called) seq_remember(q);
        ev_int(out, x);
        return true;
    }
    int64_t next;                                              // NEXTVAL
    if (!q->called) next = q->value;
    else if (__builtin_add_overflow(q->value, q->inc, &next) || next > q->maxv || next < q->minv) {
        if (!q->cycle) { ctx_error(c, "nextval: reached the limit of sequence '%s'", q->name); ev_null(out); return true; }
        next = q->inc > 0 ? q->minv : q->maxv;
    }
    bool wrapped = q->called && q->value != next && (q->inc > 0 ? next < q->value : next > q->value);
    if (!q->has_res || wrapped || (q->inc > 0 ? next > q->res : next < q->res)) {   // riserva finita: un'altra
        Seq keep = *q;
        int64_t res;
        if (__builtin_mul_overflow(q->inc, (int64_t)31, &res) || __builtin_add_overflow(next, res, &res) ||
            res > q->maxv || res < q->minv) res = q->inc > 0 ? q->maxv : q->minv;
        q->value = next;
        q->called = true;
        q->res = res;
        q->has_res = true;
        if (!seq_save()) { *q = keep; ctx_error(c, "nextval: cannot save the state of sequence '%s' (disk?)", q->name); ev_null(out); return true; }
    }
    q->value = next;
    q->called = true;
    seq_remember(q);
    ev_int(out, next);
    return true;
}

static QueryResult* exec_sequence(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    char msg[200];
    Seq* q = seq_find(db->name, n->seq.name);
    if (n->seq.op == 1) {                                      // DROP
        if (!q) {
            if (n->if_exists) return result_ok_msg(0);
            snprintf(msg, sizeof(msg), "Sequence '%.64s' does not exist", n->seq.name);
            return result_error_msg(msg);
        }
        *q = g_seq[--g_nseq];
        seq_save();
        return result_ok_msg(0);
    }
    if (n->seq.op == 0) {                                      // CREATE
        if (!sv_name_ok(n->seq.name)) return bad_name_error("sequence");
        if (q) {
            if (n->if_exists) return result_ok_msg(0);
            snprintf(msg, sizeof(msg), "Sequence '%.64s' already exists", n->seq.name);
            return result_error_msg(msg);
        }
        if (catalog_get_table(ctx->catalog, db->name, n->seq.name)) { snprintf(msg, sizeof(msg), "A table named '%.64s' exists", n->seq.name); return result_error_msg(msg); }
        if (g_nseq == g_capseq) {
            int nc = g_capseq ? g_capseq * 2 : 16;
            Seq* nv = sv_realloc(g_seq, sizeof(Seq) * (size_t)nc);
            if (!nv) return result_error_msg("Out of memory");
            g_seq = nv; g_capseq = nc;
        }
        q = &g_seq[g_nseq++];
        memset(q, 0, sizeof(*q));
        snprintf(q->db, sizeof(q->db), "%s", db->name);
        snprintf(q->name, sizeof(q->name), "%s", n->seq.name);
        q->inc = 1;
        q->minv = 1;
        q->maxv = INT64_MAX;
    } else if (!q) {
        snprintf(msg, sizeof(msg), "Sequence '%.64s' does not exist", n->seq.name);
        return result_error_msg(msg);
    }
    Seq old = *q;
    if (n->seq.has_inc) { if (n->seq.inc == 0) { if (n->seq.op == 0) g_nseq--; else *q = old; return result_error_msg("INCREMENT must not be zero"); } q->inc = n->seq.inc; }
    if (n->seq.op == 0 && q->inc < 0) { q->minv = INT64_MIN; q->maxv = -1; }
    if (n->seq.has_min) q->minv = n->seq.minv;
    if (n->seq.has_max) q->maxv = n->seq.maxv;
    if (n->seq.has_cycle) q->cycle = n->seq.cycle;
    if (n->seq.has_start) q->start = n->seq.start;
    else if (n->seq.op == 0) q->start = q->inc > 0 ? q->minv : q->maxv;
    if (n->seq.op == 0) { q->value = q->start; q->called = false; }
    if (n->seq.restart) { q->value = n->seq.has_restart_with ? n->seq.restart_with : q->start; q->called = false; }
    q->has_res = false;                                      // ALTER: la riserva va rifatta con i nuovi limiti
    if (q->minv > q->maxv || q->start < q->minv || q->start > q->maxv) {
        if (n->seq.op == 0) g_nseq--; else *q = old;
        return result_error_msg("START value must be between MINVALUE and MAXVALUE");
    }
    if (!seq_save()) return result_error_msg("Cannot save the sequences (disk?)");
    return result_ok_msg(0);
}

// buffer di testo che cresce (risultati delle funzioni di testo): il buffer comune, limitato a EV_TEXT_MAX
typedef SvStrBuf TextBuf;
static void tb_put(TextBuf* t, const char* s, size_t n) {
    if (!t->max) t->max = EV_TEXT_MAX;
    svsb_put(t, s, n);
}
static void tb_result(EvalCtx* c, EvalVal* out, TextBuf* t) {
    if (t->oom) { ctx_error(c, "%s", t->toolong ? "String result longer than 16 MB" : "Out of memory"); ev_null(out); }
    else ev_set_text(c, out, t->p ? t->p : "", t->n);
    svsb_free(t);
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

// ── ricerca full-text (senza indice: si legge ogni riga) ─────────
// Parole: lettere, cifre, '_' e i caratteri UTF-8 non ASCII; senza distinzione maiuscole/minuscole.
static bool ft_wordch(unsigned char ch) { return isalnum(ch) || ch == '_' || ch >= 0x80; }

// occorrenze della parola w (wl byte, minuscola) nei testi; prefix: anche come inizio di una parola
static int ft_count(const char* const* docs, int nd, const char* w, size_t wl, bool prefix) {
    int n = 0;
    for (int d = 0; d < nd; d++) {
        const unsigned char* p = (const unsigned char*)docs[d];
        while (p && *p) {
            while (*p && !ft_wordch(*p)) p++;
            const unsigned char* b = p;
            while (*p && ft_wordch(*p)) p++;
            size_t l = (size_t)(p - b);
            if (l && (l == wl || (prefix && l > wl))) {
                size_t k = 0;
                while (k < wl && tolower(b[k]) == (unsigned char)w[k]) k++;
                if (k == wl) n++;
            }
        }
    }
    return n;
}

// la frase (minuscola) compare in uno dei testi?
static bool ft_phrase(const char* const* docs, int nd, const char* ph, size_t pl) {
    if (!pl) return false;
    for (int d = 0; d < nd; d++)
        for (const char* p = docs[d]; p && *p; p++) {
            size_t k = 0;
            while (k < pl && p[k] && tolower((unsigned char)p[k]) == (unsigned char)ph[k]) k++;
            if (k == pl) return true;
        }
    return false;
}

// punteggio: somma di 1 + ln(occorrenze) per ogni parola trovata. BOOLEAN MODE: +parola obbligatoria,
// -parola esclusa, parola* prefisso, "frase esatta"; senza + basta una parola trovata
static double ft_score(const char* const* docs, int nd, char* q, bool boolean) {
    for (char* p = q; *p; p++) *p = (char)tolower((unsigned char)*p);
    double score = 0;
    bool any_req = false, matched = false;
    char* p = q;
    while (*p) {
        while (*p && !ft_wordch((unsigned char)*p) && *p != '"' && !(boolean && (*p == '+' || *p == '-'))) p++;
        if (!*p) break;
        char op = 0;
        if (boolean && (*p == '+' || *p == '-')) { op = *p++; while (*p == '~' || *p == '<' || *p == '>') p++; }
        int cnt;
        if (*p == '"') {                                    // frase
            char* b = ++p;
            while (*p && *p != '"') p++;
            cnt = ft_phrase(docs, nd, b, (size_t)(p - b)) ? 1 : 0;
            if (*p) p++;
        } else {
            char* b = p;
            while (*p && ft_wordch((unsigned char)*p)) p++;
            size_t wl = (size_t)(p - b);
            bool prefix = boolean && *p == '*';
            if (prefix) p++;
            if (!wl) continue;
            cnt = ft_count(docs, nd, b, wl, prefix);
        }
        if (op == '-') { if (cnt) return 0; continue; }
        if (op == '+') { any_req = true; if (!cnt) return 0; }
        if (cnt) { matched = true; score += 1.0 + log((double)cnt); }
    }
    if (!matched && !any_req) return 0;
    return round(score * 10000.0) / 10000.0;
}

// ultimo AUTO_INCREMENT generato da questo thread (LAST_INSERT_ID(); una connessione MySQL è un thread)


// ═════════════════════════════════════════════════════════════════
//  Funzioni di MySQL (stringhe, numeri, date, informazioni)
// ═════════════════════════════════════════════════════════════════

static _Thread_local long long g_found_rows;
static _Thread_local long long g_select_total;    // righe dell'ultima query prima di OFFSET / LIMIT     // FOUND_ROWS(): righe dell'ultima SELECT (senza LIMIT con SQL_CALC_FOUND_ROWS)
static _Thread_local long long g_row_count = -1; // ROW_COUNT(): righe modificate dall'ultimo statement

// ── date ──
typedef struct { int y, mo, d, h, mi, s; long us; bool has_date, has_time; } MyDT;

static long long days_from_civil(long long y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, int* y, int* m, int* d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static int days_in_month(int y, int m) {
    static const int D[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 31;
    return D[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
}

// 'YYYY-MM-DD[ HH:MM[:SS[.f]]]', 'YYYY/MM/DD', 'YYYYMMDD[HHMMSS]', 'HH:MM:SS' (solo ora)
static bool mydt_parse(const char* s, MyDT* t) {
    memset(t, 0, sizeof(*t));
    while (isspace((unsigned char)*s)) s++;
    int k = 0, y, mo, d;
    if (sscanf(s, "%d%*[-/.]%d%*[-/.]%d%n", &y, &mo, &d, &k) == 3 && k >= 8) {
        t->y = y; t->mo = mo; t->d = d; t->has_date = true;
        s += k;
    } else {
        int n = 0;
        while (isdigit((unsigned char)s[n])) n++;
        if (n == 8 || n == 14) {
            char b[16];
            memcpy(b, s, (size_t)n); b[n] = '\0';
            t->y = (b[0] - '0') * 1000 + (b[1] - '0') * 100 + (b[2] - '0') * 10 + (b[3] - '0');
            t->mo = (b[4] - '0') * 10 + (b[5] - '0');
            t->d = (b[6] - '0') * 10 + (b[7] - '0');
            t->has_date = true;
            if (n == 14) {
                t->h = (b[8] - '0') * 10 + (b[9] - '0'); t->mi = (b[10] - '0') * 10 + (b[11] - '0'); t->s = (b[12] - '0') * 10 + (b[13] - '0');
                t->has_time = true;
            }
            s += n;
        } else {
            int h, mi, sec = 0;
            if (sscanf(s, "%d:%d%n", &h, &mi, &k) == 2) {
                t->h = h; t->mi = mi; s += k;
                if (*s == ':') { if (sscanf(s + 1, "%d%n", &sec, &k) == 1) { t->s = sec; s += 1 + k; } }
                if (*s == '.') { s++; long us = 0; int dg = 0; while (isdigit((unsigned char)*s) && dg < 6) { us = us * 10 + (*s++ - '0'); dg++; } while (dg++ < 6) us *= 10; t->us = us; }
                t->has_time = true;
                return h >= -838 && h <= 838 && mi >= 0 && mi < 60 && t->s >= 0 && t->s < 60;
            }
            return false;
        }
    }
    if (*s == ' ' || *s == 'T') {
        int h, mi, sec = 0;
        if (sscanf(s + 1, "%d:%d%n", &h, &mi, &k) == 2) {
            s += 1 + k;
            if (*s == ':' && sscanf(s + 1, "%d%n", &sec, &k) == 1) s += 1 + k;
            t->h = h; t->mi = mi; t->s = sec; t->has_time = true;
            if (*s == '.') { s++; long us = 0; int dg = 0; while (isdigit((unsigned char)*s) && dg < 6) { us = us * 10 + (*s++ - '0'); dg++; } while (dg++ < 6) us *= 10; t->us = us; }
        }
    }
    if (t->y == 0 && t->mo == 0 && t->d == 0) return true;          // data zero di MySQL
    return t->mo >= 1 && t->mo <= 12 && t->d >= 1 && t->d <= days_in_month(t->y, t->mo) &&
           t->h >= 0 && t->h < 24 && t->mi >= 0 && t->mi < 60 && t->s >= 0 && t->s < 60;
}

static long long mydt_secs(const MyDT* t) {
    return days_from_civil(t->y, t->mo, t->d) * 86400LL + t->h * 3600LL + t->mi * 60LL + t->s;
}

static void mydt_from_secs(long long secs, MyDT* t) {
    long long days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    long long rem = secs - days * 86400;
    civil_from_days(days, &t->y, &t->mo, &t->d);
    t->h = (int)(rem / 3600); t->mi = (int)(rem / 60 % 60); t->s = (int)(rem % 60);
    t->has_date = true;
}

static void mydt_text(const MyDT* t, bool with_time, char* out, size_t n) {
    if (!with_time) snprintf(out, n, "%04d-%02d-%02d", t->y, t->mo, t->d);
    else if (t->us) snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%06ld", t->y, t->mo, t->d, t->h, t->mi, t->s, t->us);
    else snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", t->y, t->mo, t->d, t->h, t->mi, t->s);
}

// colonna DATE / DATETIME confrontata con una stringa costante: la costante nella forma della colonna, come la
// conversione di MySQL ('2024-02-01' = '2024-02-01 00:00:00' per un DATETIME, '2024-01-15 00:00:00' = '2024-01-15'
// per un DATE). Prima i due testi si confrontavano così com'erano: niente uguaglianze, bordi sbagliati con < e >.
// Riscrivere la costante vale anche per le ricerche con gli indici
static void temporal_literal(const ASTExpr* col, ASTExpr* lit, BindCtx* b) {
    if (!col || !lit || col->type != EXPR_COLUMN || col->bound <= 0 || col->rt_const || lit->type != EXPR_STRING || !lit->str_val) return;
    const BindCtx* bb = b;
    for (int k = 0; k < col->outer && bb; k++) bb = bb->outer;
    int s = BOUND_SRC(col->bound), c = BOUND_COL(col->bound);
    if (!bb || s >= bb->nsrc || !bb->src[s].ts || c >= bb->src[s].ts->num_columns) return;
    DataType t = bb->src[s].ts->columns[c].type;
    if (t != SV_TYPE_DATE && t != SV_TYPE_DATETIME) return;
    MyDT d;
    if (!mydt_parse(lit->str_val, &d) || !d.has_date) return;
    char out[40];
    if (t == SV_TYPE_DATE) {
        if (d.has_time && (d.h || d.mi || d.s || d.us)) return;   // un orario vero: il confronto fra testi è già giusto
        mydt_text(&d, false, out, sizeof(out));
    } else {
        if (!d.has_time) { d.h = d.mi = d.s = 0; d.us = 0; d.has_time = true; }
        mydt_text(&d, true, out, sizeof(out));
    }
    if (!strcmp(out, lit->str_val)) return;
    char* nv = sv_strdup(out);
    if (!nv) return;
    free(lit->str_val);
    lit->str_val = nv;
}


static void local_now(MyDT* t, bool utc) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    if (utc) gmtime_s(&tmv, &now); else localtime_s(&tmv, &now);
#else
    if (utc) gmtime_r(&now, &tmv); else localtime_r(&now, &tmv);
#endif
    memset(t, 0, sizeof(*t));
    t->y = tmv.tm_year + 1900; t->mo = tmv.tm_mon + 1; t->d = tmv.tm_mday;
    t->h = tmv.tm_hour; t->mi = tmv.tm_min; t->s = tmv.tm_sec;
    t->has_date = t->has_time = true;
}

// differenza fra l'ora locale e UTC in secondi (per UNIX_TIMESTAMP / FROM_UNIXTIME)
static long long tz_offset(time_t at) {
    struct tm l, g;
#ifdef _WIN32
    localtime_s(&l, &at); gmtime_s(&g, &at);
#else
    localtime_r(&at, &l); gmtime_r(&at, &g);
#endif
    long long dl = days_from_civil(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday) * 86400LL + l.tm_hour * 3600 + l.tm_min * 60 + l.tm_sec;
    long long dg = days_from_civil(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday) * 86400LL + g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec;
    return dl - dg;
}

static const char* const MONTH_NAMES[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                           "September", "October", "November", "December" };
static const char* const DAY_NAMES[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

static int weekday_mon0(const MyDT* t) {          // 0 = lunedì
    long long z = days_from_civil(t->y, t->mo, t->d);
    long long w = (z + 3) % 7;                    // 1970-01-01 era giovedì
    return (int)(w < 0 ? w + 7 : w);
}

static int day_of_year(const MyDT* t) { return (int)(days_from_civil(t->y, t->mo, t->d) - days_from_civil(t->y, 1, 1)) + 1; }

// WEEK(d, mode) di MySQL: modi 0..7 (primo giorno, intervallo 0-53 o 1-53, regola della prima settimana)
static int mysql_week(const MyDT* t, int mode, int* year_out) {
    bool monday_first = mode & 1;
    bool range1_53 = mode & 2;
    bool first_week_4days = (mode == 1 || mode == 3 || mode == 4 || mode == 6);
    if (mode == 4 || mode == 6) monday_first = false; else if (mode == 5 || mode == 7) monday_first = true;
    if (mode == 1 || mode == 3) monday_first = true;
    int y = t->y;
    long long day = days_from_civil(t->y, t->mo, t->d);
    long long jan1 = days_from_civil(y, 1, 1);
    int wd_jan1 = (int)(((jan1 + 3) % 7 + 7) % 7);                     // 0 = lunedì
    int first_day_off = monday_first ? wd_jan1 : (wd_jan1 + 1) % 7;    // giorni dall'inizio della settimana
    long long week_start;
    if (first_week_4days) week_start = first_day_off <= 3 ? jan1 - first_day_off : jan1 + (7 - first_day_off);
    else week_start = first_day_off == 0 ? jan1 : jan1 + (7 - first_day_off);
    int week;
    if (day < week_start) {
        if (!range1_53) { if (year_out) *year_out = y; return 0; }
        MyDT prev = { y - 1, 12, 31, 0, 0, 0, 0, true, false };
        return mysql_week(&prev, mode, year_out);
    }
    week = (int)((day - week_start) / 7) + 1;
    if (year_out) *year_out = y;
    if (range1_53 && week >= 52) {                                     // forse è la settimana 1 dell'anno dopo
        long long njan1 = days_from_civil(y + 1, 1, 1);
        int nwd = (int)(((njan1 + 3) % 7 + 7) % 7);
        int noff = monday_first ? nwd : (nwd + 1) % 7;
        long long nstart = first_week_4days ? (noff <= 3 ? njan1 - noff : njan1 + (7 - noff)) : (noff == 0 ? njan1 : njan1 + (7 - noff));
        if (day >= nstart) { if (year_out) *year_out = y + 1; return 1; }
    }
    return week;
}

static void date_format_into(const MyDT* t, const char* fmt, TextBuf* tb) {
    char b[64];
    for (const char* p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) { tb_put(tb, p, 1); continue; }
        p++;
        b[0] = '\0';
        int h12 = t->h % 12 == 0 ? 12 : t->h % 12;
        switch (*p) {
            case 'Y': snprintf(b, sizeof(b), "%04d", t->y); break;
            case 'y': snprintf(b, sizeof(b), "%02d", t->y % 100); break;
            case 'm': snprintf(b, sizeof(b), "%02d", t->mo); break;
            case 'c': snprintf(b, sizeof(b), "%d", t->mo); break;
            case 'd': snprintf(b, sizeof(b), "%02d", t->d); break;
            case 'e': snprintf(b, sizeof(b), "%d", t->d); break;
            case 'D': snprintf(b, sizeof(b), "%d%s", t->d, (t->d % 100 >= 11 && t->d % 100 <= 13) ? "th" : t->d % 10 == 1 ? "st" : t->d % 10 == 2 ? "nd" : t->d % 10 == 3 ? "rd" : "th"); break;
            case 'H': snprintf(b, sizeof(b), "%02d", t->h); break;
            case 'k': snprintf(b, sizeof(b), "%d", t->h); break;
            case 'h': case 'I': snprintf(b, sizeof(b), "%02d", h12); break;
            case 'l': snprintf(b, sizeof(b), "%d", h12); break;
            case 'i': snprintf(b, sizeof(b), "%02d", t->mi); break;
            case 's': case 'S': snprintf(b, sizeof(b), "%02d", t->s); break;
            case 'f': snprintf(b, sizeof(b), "%06ld", t->us); break;
            case 'p': snprintf(b, sizeof(b), "%s", t->h < 12 ? "AM" : "PM"); break;
            case 'T': snprintf(b, sizeof(b), "%02d:%02d:%02d", t->h, t->mi, t->s); break;
            case 'r': snprintf(b, sizeof(b), "%02d:%02d:%02d %s", h12, t->mi, t->s, t->h < 12 ? "AM" : "PM"); break;
            case 'M': snprintf(b, sizeof(b), "%s", t->mo >= 1 && t->mo <= 12 ? MONTH_NAMES[t->mo - 1] : ""); break;
            case 'b': snprintf(b, sizeof(b), "%.3s", t->mo >= 1 && t->mo <= 12 ? MONTH_NAMES[t->mo - 1] : ""); break;
            case 'W': snprintf(b, sizeof(b), "%s", DAY_NAMES[weekday_mon0(t)]); break;
            case 'a': snprintf(b, sizeof(b), "%.3s", DAY_NAMES[weekday_mon0(t)]); break;
            case 'w': snprintf(b, sizeof(b), "%d", (weekday_mon0(t) + 1) % 7); break;
            case 'j': snprintf(b, sizeof(b), "%03d", day_of_year(t)); break;
            case 'U': snprintf(b, sizeof(b), "%02d", mysql_week(t, 0, NULL)); break;
            case 'u': snprintf(b, sizeof(b), "%02d", mysql_week(t, 1, NULL)); break;
            case 'V': snprintf(b, sizeof(b), "%02d", mysql_week(t, 2, NULL)); break;
            case 'v': snprintf(b, sizeof(b), "%02d", mysql_week(t, 3, NULL)); break;
            case 'X': { int yy; mysql_week(t, 2, &yy); snprintf(b, sizeof(b), "%04d", yy); break; }
            case 'x': { int yy; mysql_week(t, 3, &yy); snprintf(b, sizeof(b), "%04d", yy); break; }
            default: b[0] = *p; b[1] = '\0';
        }
        tb_put(tb, b, strlen(b));
    }
}

// STR_TO_DATE: i formati più usati; false se il testo non corrisponde
static bool str_to_date(const char* s, const char* fmt, MyDT* t) {
    memset(t, 0, sizeof(*t));
    bool pm = false, h12 = false;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            if (isspace((unsigned char)*p)) { while (isspace((unsigned char)*s)) s++; continue; }
            if (*s != *p) return false;
            s++;
            continue;
        }
        p++;
        int v = 0, n = 0;
        int maxd = (*p == 'Y') ? 4 : (*p == 'f') ? 6 : (*p == 'j') ? 3 : 2;
        switch (*p) {
            case 'Y': case 'y': case 'm': case 'c': case 'd': case 'e': case 'H': case 'k': case 'h': case 'I': case 'l':
            case 'i': case 's': case 'S': case 'f': case 'j':
                while (n < maxd && isdigit((unsigned char)*s)) { v = v * 10 + (*s++ - '0'); n++; }
                if (!n) return false;
                if (*p == 'Y') t->y = v; else if (*p == 'y') t->y = v < 70 ? 2000 + v : 1900 + v;
                else if (*p == 'm' || *p == 'c') t->mo = v; else if (*p == 'd' || *p == 'e') t->d = v;
                else if (*p == 'H' || *p == 'k') t->h = v; else if (*p == 'i') t->mi = v; else if (*p == 's' || *p == 'S') t->s = v;
                else if (*p == 'f') { while (n++ < 6) v *= 10; t->us = v; }
                else if (*p == 'j') { MyDT x; mydt_from_secs((days_from_civil(t->y, 1, 1) + v - 1) * 86400LL, &x); t->mo = x.mo; t->d = x.d; }
                else { t->h = v; h12 = true; }
                break;
            case 'p':
                if (!strncasecmp(s, "PM", 2)) pm = true; else if (strncasecmp(s, "AM", 2)) return false;
                s += 2;
                break;
            case 'M': case 'b': {
                bool ok = false;
                for (int m = 0; m < 12 && !ok; m++) {
                    size_t l = *p == 'b' ? 3 : strlen(MONTH_NAMES[m]);
                    if (!strncasecmp(s, MONTH_NAMES[m], l)) { t->mo = m + 1; s += l; ok = true; }
                }
                if (!ok) return false;
                break;
            }
            case 'W': case 'a':
                while (isalpha((unsigned char)*s)) s++;
                break;
            case 'T': if (sscanf(s, "%d:%d:%d%n", &t->h, &t->mi, &t->s, &n) != 3) return false; s += n; break;
            case '%': if (*s++ != '%') return false; break;
            default: return false;
        }
    }
    if (h12) { if (t->h == 12) t->h = 0; if (pm) t->h += 12; }
    t->has_date = true;
    t->has_time = t->h || t->mi || t->s || strchr(fmt, 'H') || strchr(fmt, 'T') || strchr(fmt, 'i');
    return t->mo >= 0 && t->mo <= 12 && t->d >= 0 && t->d <= 31 && t->h < 24 && t->mi < 60 && t->s < 60;
}

// aggiunge un INTERVAL (unità di MySQL, anche composte come DAY_HOUR '1 2'); false = valore non valido
static bool mydt_add(MyDT* t, const char* unit, const char* val, int sign) {
    long long parts[5] = {0};
    int np = 0;
    const char* p = val;
    bool neg = false;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') { neg = true; p++; }
    while (*p && np < 5) {
        if (isdigit((unsigned char)*p)) {
            // oltre 10^12 nessuna unità resta dentro gli anni 0-9999: valore non valido (prima: overflow, UB)
            long long v = 0;
            while (isdigit((unsigned char)*p)) {
                if (v > 1000000000000LL) return false;
                v = v * 10 + (*p++ - '0');
            }
            parts[np++] = v;
        } else if (*p == '.' && !strcmp(unit, "SECOND")) {      // INTERVAL 1.5 SECOND
            p++;
            long us = 0; int dg = 0;
            while (isdigit((unsigned char)*p) && dg < 6) { us = us * 10 + (*p++ - '0'); dg++; }
            while (dg++ < 6) us *= 10;
            parts[np++] = us;
        } else p++;
    }
    if (np == 0) return false;
    long long sg = (neg ? -1 : 1) * sign;
    long long secs = 0, months = 0;
    #define LAST(k) (np >= (k) ? parts[np - (k)] : 0)
    if (!strcmp(unit, "MICROSECOND")) secs = 0;
    else if (!strcmp(unit, "SECOND")) secs = parts[0];
    else if (!strcmp(unit, "MINUTE")) secs = parts[0] * 60;
    else if (!strcmp(unit, "HOUR")) secs = parts[0] * 3600;
    else if (!strcmp(unit, "DAY")) secs = parts[0] * 86400;
    else if (!strcmp(unit, "WEEK")) secs = parts[0] * 7 * 86400;
    else if (!strcmp(unit, "MONTH")) months = parts[0];
    else if (!strcmp(unit, "QUARTER")) months = parts[0] * 3;
    else if (!strcmp(unit, "YEAR")) months = parts[0] * 12;
    else if (!strcmp(unit, "YEAR_MONTH")) months = LAST(2) * 12 + LAST(1);
    else if (!strcmp(unit, "DAY_HOUR")) secs = LAST(2) * 86400 + LAST(1) * 3600;
    else if (!strcmp(unit, "DAY_MINUTE")) secs = LAST(3) * 86400 + LAST(2) * 3600 + LAST(1) * 60;
    else if (!strcmp(unit, "DAY_SECOND")) secs = LAST(4) * 86400 + LAST(3) * 3600 + LAST(2) * 60 + LAST(1);
    else if (!strcmp(unit, "HOUR_MINUTE")) secs = LAST(2) * 3600 + LAST(1) * 60;
    else if (!strcmp(unit, "HOUR_SECOND")) secs = LAST(3) * 3600 + LAST(2) * 60 + LAST(1);
    else if (!strcmp(unit, "MINUTE_SECOND")) secs = LAST(2) * 60 + LAST(1);
    else return false;
    #undef LAST
    if (months) {
        long long total = (long long)t->y * 12 + (t->mo - 1) + sg * months;
        t->y = (int)(total >= 0 ? total / 12 : -((-total + 11) / 12));
        t->mo = (int)(total - (long long)t->y * 12) + 1;
        int dim = days_in_month(t->y, t->mo);
        if (t->d > dim) t->d = dim;                             // 31 gennaio + 1 mese = 28/29 febbraio
    }
    if (secs) {
        MyDT r;
        mydt_from_secs(mydt_secs(t) + sg * secs, &r);
        r.us = t->us; r.has_time = t->has_time;
        *t = r;
    }
    return t->y >= 0 && t->y <= 9999;
}

// ── hash e codifiche ──
static void md5_bytes(const uint8_t* msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t* m = sv_calloc(total, 1);
    if (!m) { memset(out, 0, 16); return; }
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + i * 4] | (uint32_t)m[off + i * 4 + 1] << 8 | (uint32_t)m[off + i * 4 + 2] << 16 | (uint32_t)m[off + i * 4 + 3] << 24;
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t tmp = d; d = c; c = b;
            uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    free(m);
    uint32_t h[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (8 * j));
}

static uint32_t crc32_bytes(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

static void hex_into(TextBuf* tb, const uint8_t* p, size_t n) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { char b[2] = { H[p[i] >> 4], H[p[i] & 15] }; tb_put(tb, b, 2); }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// ── valutazione ──
// true se f è una funzione di questo blocco (e out è il risultato)
// una ricerca si è fermata al limite di passi: errore dello statement, come ER_REGEXP_TIME_OUT di MySQL, invece di
// un "nessuna corrispondenza" falso ripetuto per ogni riga
static bool regex_limit_error(EvalCtx* c, EvalVal* out) {
    if (!sv_regex_take_limit()) return false;
    const char* why = sv_thread_cancelled() ? "Query cancelled"
                    : "Regular expression too expensive for this value (backtracking limit): simplify the pattern";
    if (!sv_thread_cancelled()) sv_scan_abort(why);    // le righe restanti non cambierebbero l'esito
    ctx_error(c, "%s", why);
    ev_null(out);
    return true;
}

static bool eval_mysql(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    const size_t flen = strlen(f);                       // per FIS: confronti per lunghezza prima (CR-M2)
    // senza argomenti
    if (FIS("FOUND_ROWS")) { ev_int(out, g_found_rows); return true; }
    if (FIS("ROW_COUNT")) { ev_int(out, g_row_count); return true; }
    if (FIS("CONNECTION_ID")) { ev_int(out, (int64_t)(((uintptr_t)&g_found_rows) >> 4) & 0x7fffffff); return true; }
    if (FIS("USER") || FIS("CURRENT_USER") || FIS("SESSION_USER") || FIS("SYSTEM_USER")) {
        ev_set_text(c, out, "root@localhost", 14);
        return true;
    }
    if (FIS("PI")) { ev_num(out, 3.141592653589793); return true; }
    if (FIS("RAND") && e->nargs == 0) { ev_num(out, (double)rand() / ((double)RAND_MAX + 1.0)); return true; }
    if (FIS("UUID")) {
        char b[40];
        unsigned r[8];
        for (int i = 0; i < 8; i++) r[i] = (unsigned)rand() & 0xffff;
        snprintf(b, sizeof(b), "%04x%04x-%04x-%04x-%04x-%04x%04x%04x", r[0], r[1], r[2], (r[3] & 0x0fff) | 0x1000, (r[4] & 0x3fff) | 0x8000, r[5], r[6], r[7]);
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("UUID_SHORT")) { ev_int(out, ((int64_t)time(NULL) << 24) | (rand() & 0xffffff)); return true; }
    if (FIS("RELEASE_ALL_LOCKS")) { ev_int(out, 0); return true; }
    if (FIS("UTC_TIMESTAMP") || FIS("UTC_DATE") || FIS("UTC_TIME") || FIS("SYSDATE") ||
        FIS("LOCALTIME") || FIS("LOCALTIMESTAMP") || FIS("CURTIME") || FIS("CURRENT_TIME")) {
        MyDT t;
        local_now(&t, !strncmp(f, "UTC_", 4));
        char b[32];
        if (FIS("UTC_DATE")) mydt_text(&t, false, b, sizeof(b));
        else if (FIS("UTC_TIME") || FIS("CURTIME") || FIS("CURRENT_TIME")) snprintf(b, sizeof(b), "%02d:%02d:%02d", t.h, t.mi, t.s);
        else mydt_text(&t, true, b, sizeof(b));
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("UNIX_TIMESTAMP") && e->nargs == 0) { ev_int(out, (int64_t)time(NULL)); return true; }

    // funzioni con argomenti (valutati qui, fino a 32)
    static const char* const MINE[] = {
        "LOCATE", "INSTR", "LPAD", "RPAD", "REPEAT", "SPACE", "FIELD", "FIND_IN_SET", "ELT", "SUBSTRING_INDEX", "ASCII", "ORD",
        "CHAR", "HEX", "UNHEX", "MD5", "SHA1", "SHA", "SHA2", "TO_BASE64", "FROM_BASE64", "QUOTE", "STRCMP", "FORMAT",
        "BIT_LENGTH", "OCTET_LENGTH", "CHARSET", "COLLATION", "REGEXP_LIKE", "REGEXP_REPLACE", "REGEXP_SUBSTR", "REGEXP_INSTR",
        "TRIM_EX", "BIN", "OCT", "CRC32", "INET_ATON", "INET_NTOA", "RAND", "EXP", "LN", "LOG", "LOG10", "LOG2", "TRUNCATE",
        "GREATEST", "LEAST", "RADIANS", "DEGREES", "SIN", "COS", "TAN", "COT", "ASIN", "ACOS", "ATAN", "ATAN2", "INTDIV",
        "DATE_FORMAT", "TIME_FORMAT", "STR_TO_DATE", "UNIX_TIMESTAMP", "FROM_UNIXTIME", "DATE", "TIME", "HOUR", "MINUTE",
        "SECOND", "MICROSECOND", "DAYOFWEEK", "DAYOFMONTH", "DAYOFYEAR", "WEEKDAY", "WEEK", "WEEKOFYEAR", "YEARWEEK",
        "QUARTER", "MONTHNAME", "DAYNAME", "LAST_DAY", "DATEDIFF", "TIMEDIFF", "TIMESTAMPDIFF", "DATE_ADD", "DATE_SUB",
        "ADDDATE", "SUBDATE", "__INTERVAL", "EXTRACT", "MAKEDATE", "MAKETIME", "TO_DAYS", "FROM_DAYS", "TO_SECONDS",
        "SEC_TO_TIME", "TIME_TO_SEC", "CONVERT_TZ", "CAST", "SLEEP", "GET_LOCK", "RELEASE_LOCK", "IS_FREE_LOCK",
        "IS_USED_LOCK", "ANY_VALUE", "ISNULL", "VALUES", "LAST_INSERT_ID", "CHAR_LENGTH_BYTES", "INSERT" };
    if (e->fmine == 0) {                                   // dipende solo dal nome: calcolato una volta per nodo
        bool mine = false;
        for (size_t i = 0; i < sizeof(MINE) / sizeof(MINE[0]) && !mine; i++) mine = !strcmp(f, MINE[i]);
        e->fmine = mine ? 1 : -1;
    }
    if (e->fmine < 0) return false;

    // DATE_ADD(d, INTERVAL n unità): l'INTERVAL non è un valore, si legge qui
    if ((FIS("DATE_ADD") || FIS("DATE_SUB") || FIS("ADDDATE") || FIS("SUBDATE")) && e->nargs == 2) {
        int sign = (FIS("DATE_SUB") || FIS("SUBDATE")) ? -1 : 1;
        EvalVal dv, iv;
        eval_expr(e->args[0], c, &dv);
        ASTExpr* ie = e->args[1];
        bool interval = ie->type == EXPR_FUNC && !strcmp(ie->func, "__INTERVAL");
        eval_expr(interval ? ie->args[0] : ie, c, &iv);
        if (dv.null || iv.null) { ev_null(out); return true; }
        MyDT t;
        if (!mydt_parse(ev_s(&dv), &t) || !t.has_date) { ev_null(out); return true; }
        const char* unit = interval && ie->args[1]->str_val ? ie->args[1]->str_val : "DAY";   // ADDDATE(d, n): giorni
        char amt[64], b2[40];
        snprintf(amt, sizeof(amt), "%.63s", ev_s(&iv));
        bool date_only = !t.has_time && (!strcmp(unit, "DAY") || !strcmp(unit, "WEEK") || !strcmp(unit, "MONTH") ||
                                         !strcmp(unit, "QUARTER") || !strcmp(unit, "YEAR") || !strcmp(unit, "YEAR_MONTH"));
        if (!mydt_add(&t, unit, amt, sign)) { ev_null(out); return true; }
        mydt_text(&t, !date_only, b2, sizeof(b2));
        ev_set_text(c, out, b2, strlen(b2));
        return true;
    }

    int na = e->nargs < 32 ? e->nargs : 32;
    EvalVal a[na > 0 ? na : 1];                                // solo gli argomenti che ci sono (stack dei thread)
    // ELT, FIELD, GREATEST, LEAST, ISNULL, CHAR, CONCAT-like: i NULL hanno regole proprie
    bool null_ok = FIS("FIELD") || FIS("ELT") || FIS("ISNULL") || FIS("CHAR") || FIS("VALUES") ||
                   FIS("ANY_VALUE") || FIS("LAST_INSERT_ID") || FIS("QUOTE") || FIS("CAST");
    for (int i = 0; i < na; i++) {
        eval_expr(e->args[i], c, &a[i]);
        if (a[i].null && !null_ok) { ev_null(out); return true; }
    }
    #define S(i) ev_s(&a[i])
    #define N(i) ev_as_num(&a[i])
    #define I(i) ((long long)ev_as_num(&a[i]))
    char b[128];
    TextBuf tb = {0};

    if (FIS("ISNULL")) { ev_int(out, a[0].null ? 1 : 0); return true; }
    if (FIS("ANY_VALUE") || FIS("VALUES")) { if (a[0].null) ev_null(out); else ev_copy(out, &a[0]); return true; }
    if (FIS("LAST_INSERT_ID")) {                        // LAST_INSERT_ID(x): lo imposta e lo restituisce
        if (na == 1 && !a[0].null) g_last_insert_id = (int64_t)N(0);
        ev_int(out, g_last_insert_id);
        return true;
    }
    if (FIS("QUOTE")) {
        if (a[0].null) { ev_set_text(c, out, "NULL", 4); return true; }
        tb_put(&tb, "'", 1);
        for (const char* p = S(0); *p; p++) { if (*p == '\'' || *p == '\\') tb_put(&tb, "\\", 1); tb_put(&tb, p, 1); }
        tb_put(&tb, "'", 1);
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("LOCATE") || FIS("INSTR")) {         // LOCATE(sub, s [, pos]), INSTR(s, sub): posizione (1..), 0 = no
        const char* sub = FIS("INSTR") ? S(1) : S(0);
        const char* s = FIS("INSTR") ? S(0) : S(1);
        long long from = na > 2 ? I(2) : 1;
        if (from < 1) { ev_int(out, 0); return true; }
        const char* start = utf8_at(s, (int)(from - 1));
        const char* hit = NULL;
        size_t sl = strlen(sub);
        for (const char* p = start; *p && !hit; p++) if (!strncasecmp(p, sub, sl)) hit = p;
        if (!sl) hit = start;
        if (!hit) { ev_int(out, 0); return true; }
        int pos = 1;
        for (const char* p = s; p < hit; p++) if (((unsigned char)*p & 0xC0) != 0x80) pos++;
        ev_int(out, pos);
        return true;
    }
    if (FIS("LPAD") || FIS("RPAD")) {
        const char* s = S(0);
        long long len = I(1);
        const char* pad = S(2);
        int sl = utf8_len(s);
        if (len < 0) { ev_null(out); return true; }
        if (sl >= len) { const char* end = utf8_at(s, (int)len); ev_set_text(c, out, s, (size_t)(end - s)); return true; }
        if (!*pad) { ev_null(out); return true; }
        long long need = len - sl;
        TextBuf padb = {0};
        for (long long k = 0; k < need; ) {
            const char* p = pad;
            while (*p && k < need) { const char* q = utf8_at(p, 1); tb_put(&padb, p, (size_t)(q - p)); p = q; k++; }
        }
        if (f[0] == 'L') { tb_put(&tb, padb.p ? padb.p : "", padb.n); tb_put(&tb, s, strlen(s)); }
        else { tb_put(&tb, s, strlen(s)); tb_put(&tb, padb.p ? padb.p : "", padb.n); }
        free(padb.p);
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("INSERT")) {                               // INSERT(str, pos, len, new) come MySQL
        const char* s = S(0);
        int chars = utf8_chars(s);
        double dp = N(1), dl = N(2);
        if (dp < 1 || dp > chars) { ev_set_text(c, out, s, strlen(s)); return true; }   // posizione fuori: intatta
        int64_t pos = (int64_t)dp;
        int64_t len = dl < 0 || dl > chars ? chars : (int64_t)dl;                     // negativa o oltre: fino in fondo
        if (len > chars - pos + 1) len = chars - pos + 1;
        const char* b0 = utf8_at(s, (int)(pos - 1));
        const char* b1 = utf8_at(b0, (int)len);
        tb_put(&tb, s, (size_t)(b0 - s));
        tb_put(&tb, S(3), strlen(S(3)));
        tb_put(&tb, b1, strlen(b1));
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("REPEAT") || FIS("SPACE")) {
        bool sp = f[0] == 'S';
        long long n = sp ? I(0) : I(1);
        const char* s = sp ? " " : S(0);
        size_t l = strlen(s);
        if (n <= 0 || !l) { ev_set_text(c, out, "", 0); return true; }
        if ((size_t)n * l > SV_MAX_TEXT_BYTES) { ctx_error(c, "%s", "Result of REPEAT is too long"); ev_null(out); return true; }
        for (long long i = 0; i < n; i++) tb_put(&tb, s, l);
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("FIELD")) {                                  // FIELD(x, a, b, ...): posizione di x (0 = assente)
        if (a[0].null) { ev_int(out, 0); return true; }
        for (int i = 1; i < na; i++) if (!a[i].null && ev_cmp(&a[0], &a[i]) == 0) { ev_int(out, i); return true; }
        ev_int(out, 0);
        return true;
    }
    if (FIS("ELT")) {
        long long k = a[0].null ? 0 : I(0);
        if (k < 1 || k >= na || a[k].null) { ev_null(out); return true; }
        ev_copy(out, &a[k]);
        return true;
    }
    if (FIS("FIND_IN_SET")) {                            // FIND_IN_SET('b', 'a,b,c') = 2
        const char* x = S(0);
        const char* list = S(1);
        size_t xl = strlen(x);
        int pos = 1;
        for (const char* p = list; ; pos++) {
            const char* comma = strchr(p, ',');
            size_t l = comma ? (size_t)(comma - p) : strlen(p);
            if (l == xl && !strncasecmp(p, x, l)) { ev_int(out, *list ? pos : 0); return true; }
            if (!comma) break;
            p = comma + 1;
        }
        ev_int(out, 0);
        return true;
    }
    if (FIS("SUBSTRING_INDEX")) {                        // SUBSTRING_INDEX('a.b.c', '.', 2) = 'a.b'
        const char* s = S(0);
        const char* d = S(1);
        long long n = I(2);
        size_t dl = strlen(d), sl = strlen(s);
        if (!dl || n == 0) { ev_set_text(c, out, "", 0); return true; }
        if (n > 0) {
            const char* p = s;
            for (long long k = 0; k < n; k++) {
                const char* hit = strstr(p, d);
                if (!hit) { ev_set_text(c, out, s, sl); return true; }
                if (k == n - 1) { ev_set_text(c, out, s, (size_t)(hit - s)); return true; }
                p = hit + dl;
            }
        } else {
            long long want = -n, found = 0;
            for (size_t i = sl; i >= dl && i-- > 0; ) {
                if (i + dl <= sl && !strncmp(s + i, d, dl)) {
                    if (++found == want) { ev_set_text(c, out, s + i + dl, sl - i - dl); return true; }
                }
                if (i == 0) break;
            }
            ev_set_text(c, out, s, sl);
            return true;
        }
        ev_set_text(c, out, s, sl);
        return true;
    }
    if (FIS("ASCII")) { ev_int(out, (unsigned char)S(0)[0]); return true; }
    if (FIS("ORD")) {                                   // MySQL: i byte del primo carattere come numero (ORD('à') = 50080)
        const unsigned char* u = (const unsigned char*)S(0);
        int n = !u[0] ? 0 : u[0] < 0x80 ? 1 : u[0] >= 0xF0 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC0 ? 2 : 1;
        int64_t v = 0;
        for (int i = 0; i < n && u[i]; i++) v = v * 256 + u[i];
        ev_int(out, v);
        return true;
    }
    if (FIS("CHAR")) {                                   // byte veri, anche CHAR(0) (binesc.h)
        for (int i = 0; i < na; i++) if (!a[i].null) { long long v = I(i); char e2[2]; tb_put(&tb, e2, sv_bin_put((uint8_t)(v & 0xff), e2)); }
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("HEX")) {
        if (a[0].is_num) { snprintf(b, sizeof(b), "%llX", (unsigned long long)I(0)); ev_set_text(c, out, b, strlen(b)); return true; }
        const char* s = S(0);
        static const char H[] = "0123456789ABCDEF";
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            unsigned char by = *p;
            if (by == SV_BIN_ESC && (p[1] == 0x80 || p[1] == 0x81)) { by = p[1] == 0x80 ? 0 : SV_BIN_ESC; p++; }   // byte vero
            char h[2] = { H[by >> 4], H[by & 15] };
            tb_put(&tb, h, 2);
        }
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("UNHEX")) {
        const char* s = S(0);
        size_t l = strlen(s);
        if (l % 2) { ev_null(out); return true; }
        for (size_t i = 0; i < l; i += 2) {
            int hi = isdigit((unsigned char)s[i]) ? s[i] - '0' : isxdigit((unsigned char)s[i]) ? (tolower((unsigned char)s[i]) - 'a' + 10) : -1;
            int lo = isdigit((unsigned char)s[i + 1]) ? s[i + 1] - '0' : isxdigit((unsigned char)s[i + 1]) ? (tolower((unsigned char)s[i + 1]) - 'a' + 10) : -1;
            if (hi < 0 || lo < 0) { free(tb.p); ev_null(out); return true; }
            char e2[2];
            tb_put(&tb, e2, sv_bin_put((uint8_t)(hi * 16 + lo), e2));
        }
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("MD5") || FIS("SHA1") || FIS("SHA") || FIS("SHA2")) {
        const char* s0 = S(0);
        size_t sl0 = strlen(s0);
        char* s = sv_malloc(sl0 + 1);                              // i byte veri (binesc.h)
        if (!s) { ev_null(out); return true; }
        size_t sl = sv_bin_decode(s0, sl0, s);
        uint8_t h[32];
        size_t hl;
        if (f[0] == 'M') { md5_bytes((const uint8_t*)s, sl, h); hl = 16; }
        else if (FIS("SHA2")) {
            long long bits = I(1);
            if (bits != 256 && bits != 0) { free(s); ev_null(out); return true; }
            auth_sha256(s, sl, h); hl = 32;
        } else { auth_sha1(s, sl, h); hl = 20; }
        free(s);
        hex_into(&tb, h, hl);
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("CRC32")) {
        const char* s0 = S(0);
        size_t l0 = strlen(s0);
        char* s = sv_malloc(l0 + 1);
        if (!s) { ev_null(out); return true; }
        ev_int(out, crc32_bytes((const uint8_t*)s, sv_bin_decode(s0, l0, s)));
        free(s);
        return true;
    }
    if (FIS("TO_BASE64")) {
        const char* s0 = S(0);
        size_t l0 = strlen(s0);
        unsigned char* s = sv_malloc(l0 + 1);
        if (!s) { ev_null(out); return true; }
        size_t l = sv_bin_decode(s0, l0, (char*)s);
        for (size_t i = 0; i < l; i += 3) {
            uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < l ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < l ? s[i + 2] : 0);
            char q[4] = { B64[v >> 18], B64[(v >> 12) & 63], i + 1 < l ? B64[(v >> 6) & 63] : '=', i + 2 < l ? B64[v & 63] : '=' };
            tb_put(&tb, q, 4);
        }
        free(s);
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("FROM_BASE64")) {
        const char* s = S(0);
        uint32_t acc = 0;
        int bits = 0;
        for (const char* p = s; *p; p++) {
            const char* k = strchr(B64, *p);
            if (*p == '=' || isspace((unsigned char)*p)) continue;
            if (!k || !*p) { free(tb.p); ev_null(out); return true; }
            acc = acc << 6 | (uint32_t)(k - B64);
            bits += 6;
            if (bits >= 8) { bits -= 8; char e2[2]; tb_put(&tb, e2, sv_bin_put((uint8_t)((acc >> bits) & 0xff), e2)); }
        }
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("STRCMP")) { int r = strcmp(S(0), S(1)); ev_int(out, r < 0 ? -1 : r > 0); return true; }
    if (FIS("FORMAT")) {                                 // FORMAT(1234567.891, 2) = '1,234,567.89'
        long long d = I(1);
        if (d < 0) d = 0;
        if (d > 30) d = 30;
        // arrotondamento decimale, a metà lontano dallo zero come MySQL: con printf su un double FORMAT(0.5, 0) = '0',
        // FORMAT(2.5, 0) = '2', FORMAT(1.005, 2) = '1.00' (MySQL: 1, 3, 1.01)
        char num[DEC_LIMBS * 9 + 80];
        Dec dx, dr;
        bool okd = ev_get_dec(&a[0], &dx);
        if (!okd) { char tmp[64]; snprintf(tmp, sizeof(tmp), "%.15g", N(0)); okd = dec_parse(tmp, &dx); }
        if (okd && dec_round(&dx, (int)d, &dr)) dec_format(&dr, num, sizeof(num));
        else snprintf(num, sizeof(num), "%.*f", (int)d, N(0));
        char* dot = strchr(num, '.');
        size_t il = dot ? (size_t)(dot - num) : strlen(num);
        size_t start = num[0] == '-' ? 1 : 0;
        if (start) tb_put(&tb, "-", 1);
        for (size_t i = start; i < il; i++) {
            tb_put(&tb, &num[i], 1);
            size_t left = il - i - 1;
            if (left && left % 3 == 0) tb_put(&tb, ",", 1);
        }
        if (dot) tb_put(&tb, dot, strlen(dot));
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("BIT_LENGTH")) { ev_int(out, (int64_t)sv_bin_len(S(0)) * 8); return true; }
    if (FIS("OCTET_LENGTH")) { ev_int(out, (int64_t)sv_bin_len(S(0))); return true; }
    if (FIS("CHARSET")) { ev_set_text(c, out, "utf8mb4", 7); return true; }
    if (FIS("COLLATION")) { ev_set_text(c, out, "utf8mb4_general_ci", 18); return true; }
    if (FIS("BIN") || FIS("OCT")) {
        unsigned long long v = (unsigned long long)I(0);
        int base = f[0] == 'B' ? 2 : 8;
        char tmp[70];
        int k = 0;
        do { tmp[k++] = (char)('0' + (int)(v % (unsigned)base)); v /= (unsigned)base; } while (v && k < 69);
        for (int i = 0; i < k; i++) b[i] = tmp[k - 1 - i];
        b[k] = '\0';
        ev_set_text(c, out, b, (size_t)k);
        return true;
    }
    if (FIS("INET_ATON")) {
        unsigned x[4];
        if (sscanf(S(0), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]) != 4 || x[0] > 255 || x[1] > 255 || x[2] > 255 || x[3] > 255) { ev_null(out); return true; }
        ev_int(out, (int64_t)x[0] << 24 | x[1] << 16 | x[2] << 8 | x[3]);
        return true;
    }
    if (FIS("INET_NTOA")) {
        long long v = I(0);
        if (v < 0 || v > 4294967295LL) { ev_null(out); return true; }
        snprintf(b, sizeof(b), "%lld.%lld.%lld.%lld", v >> 24 & 255, v >> 16 & 255, v >> 8 & 255, v & 255);
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("TRIM_EX")) {                               // TRIM(LEADING|TRAILING|BOTH x FROM s)
        const char* s = S(0);
        const char* r = S(1);
        const char* mode = S(2);
        size_t rl = strlen(r), sl = strlen(s);
        const char* p = s;
        const char* end = s + sl;
        if (rl && strcmp(mode, "TRAILING")) while ((size_t)(end - p) >= rl && !strncmp(p, r, rl)) p += rl;
        if (rl && strcmp(mode, "LEADING")) while ((size_t)(end - p) >= rl && !strncmp(end - rl, r, rl)) end -= rl;
        ev_set_text(c, out, p, (size_t)(end - p));
        return true;
    }
    if (!strncmp(f, "REGEXP_", 7)) {
        bool icase = true;
        int mode_arg = FIS("REGEXP_LIKE") ? 2 : FIS("REGEXP_REPLACE") ? 5 : FIS("REGEXP_SUBSTR") ? 4 : 5;
        if (na > mode_arg && strchr(S(mode_arg), 'c')) icase = false;
        char err[160];
        bool owned;
        SvRegex* re = sv_regex_cached(S(1), icase, err, sizeof(err), &owned);   // pattern costante: compilato una volta
        if (!re) { ctx_error(c, "%s", err); ev_null(out); return true; }
        const char* s = S(0);
        sv_regex_take_limit();
        if (FIS("REGEXP_LIKE")) {
            bool m = sv_regex_match(re, s);
            if (owned) sv_regex_free(re);
            if (regex_limit_error(c, out)) return true;
            ev_int(out, m ? 1 : 0);
            return true;
        }
        long long pos = na > 2 && (!FIS("REGEXP_REPLACE")) ? I(2) : (na > 3 && FIS("REGEXP_REPLACE") ? I(3) : 1);
        long long occ = FIS("REGEXP_REPLACE") ? (na > 4 ? I(4) : 0) : (na > 3 ? I(3) : 1);
        if (pos < 1) pos = 1;
        size_t from = (size_t)(utf8_at(s, (int)(pos - 1)) - s);
        size_t st, en;
        if (FIS("REGEXP_REPLACE")) {                     // tutte (occ = 0) o la n-esima occorrenza
            const char* rep = S(2);
            tb_put(&tb, s, from);
            size_t at = from;
            long long k = 0;
            while (at <= strlen(s) && sv_regex_search(re, s, at, &st, &en)) {
                k++;
                tb_put(&tb, s + at, st - at);
                if (occ == 0 || k == occ) tb_put(&tb, rep, strlen(rep)); else tb_put(&tb, s + st, en - st);
                if (en == st) { if (s[en]) tb_put(&tb, s + en, 1); at = en + 1; } else at = en;
                if (occ && k == occ) break;
            }
            if (at < strlen(s)) tb_put(&tb, s + at, strlen(s) - at);
            if (owned) sv_regex_free(re);
            if (regex_limit_error(c, out)) { free(tb.p); return true; }
            tb_result(c, out, &tb);
            return true;
        }
        size_t at = from;
        bool found = false;
        for (long long k = 0; k < (occ < 1 ? 1 : occ); k++) {
            found = sv_regex_search(re, s, at, &st, &en);
            if (!found) break;
            at = en > st ? en : en + 1;
        }
        if (owned) sv_regex_free(re);
        if (regex_limit_error(c, out)) return true;
        if (FIS("REGEXP_SUBSTR")) { if (found) ev_set_text(c, out, s + st, en - st); else ev_null(out); return true; }
        if (!found) { ev_int(out, 0); return true; }
        int p1 = 1;
        for (size_t i = 0; i < st; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) p1++;
        ev_int(out, p1);
        return true;
    }
    // numeri
    if (FIS("RAND")) { srand((unsigned)I(0)); ev_num(out, (double)rand() / ((double)RAND_MAX + 1.0)); return true; }
    if (FIS("INTDIV")) { double y = N(1); if (y == 0) { ev_null(out); return true; } ev_int(out, (int64_t)(N(0) / y)); return true; }
    if (FIS("GREATEST") || FIS("LEAST")) {
        bool g = f[0] == 'G';
        int best = 0;
        for (int i = 1; i < na; i++) { int r = ev_cmp(&a[i], &a[best]); if (g ? r > 0 : r < 0) best = i; }
        ev_copy(out, &a[best]);
        return true;
    }
    if (FIS("TRUNCATE")) {
        long long d = I(1);
        if (a[0].is_dec) {                                        // decimale: esatto, con d cifre (P1.9)
            Dec xd, r;
            int dd = d > DEC_MAX_SCALE ? DEC_MAX_SCALE : d < -60 ? -60 : (int)d;
            if (ev_get_dec(&a[0], &xd) && dec_trunc(&xd, dd, &r)) { ev_dec(out, &r); return true; }
        }
        double x = N(0), p = pow(10.0, (double)d);
        double r = x >= 0 ? floor(x * p) / p : ceil(x * p) / p;
        if (d <= 0 && fabs(r) < 9e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return true;
    }
    {
        static const struct { const char* n; int k; } MATH[] = {
            { "EXP", 1 }, { "LN", 2 }, { "LOG10", 3 }, { "LOG2", 4 }, { "RADIANS", 5 }, { "DEGREES", 6 }, { "SIN", 7 }, { "COS", 8 },
            { "TAN", 9 }, { "COT", 10 }, { "ASIN", 11 }, { "ACOS", 12 } };
        for (size_t i = 0; i < sizeof(MATH) / sizeof(MATH[0]); i++)
            if (!strcmp(f, MATH[i].n)) {
                double x = N(0), r = 0;
                switch (MATH[i].k) {
                    case 1: r = exp(x); break;
                    case 2: if (x <= 0) { ev_null(out); return true; } r = log(x); break;
                    case 3: if (x <= 0) { ev_null(out); return true; } r = log10(x); break;
                    case 4: if (x <= 0) { ev_null(out); return true; } r = log2(x); break;
                    case 5: r = x * 3.141592653589793 / 180; break;
                    case 6: r = x * 180 / 3.141592653589793; break;
                    case 7: r = sin(x); break;
                    case 8: r = cos(x); break;
                    case 9: r = tan(x); break;
                    case 10: r = 1 / tan(x); break;
                    case 11: if (x < -1 || x > 1) { ev_null(out); return true; } r = asin(x); break;
                    case 12: if (x < -1 || x > 1) { ev_null(out); return true; } r = acos(x); break;
                }
                ev_num(out, r);
                return true;
            }
    }
    if (FIS("LOG")) {                                    // LOG(x) = ln, LOG(b, x)
        if (na == 1) { if (N(0) <= 0) ev_null(out); else ev_num(out, log(N(0))); return true; }
        if (N(0) <= 0 || N(0) == 1 || N(1) <= 0) { ev_null(out); return true; }
        ev_num(out, log(N(1)) / log(N(0)));
        return true;
    }
    if (FIS("ATAN") || FIS("ATAN2")) { ev_num(out, na == 2 ? atan2(N(0), N(1)) : atan(N(0))); return true; }
    // CAST(x AS tipo)
    if (FIS("CAST")) {
        if (a[0].null) { ev_null(out); return true; }
        const char* ty = S(1);
        if (!strncmp(ty, "SIGNED", 6) || !strncmp(ty, "UNSIGNED", 8) || !strncmp(ty, "INT", 3) || !strcmp(ty, "YEAR")) {
            bool uns = !strncmp(ty, "UNSIGNED", 8);
            int64_t x;
            if (a[0].is_int) x = a[0].inum;
            else if (a[0].is_num) {
                double v = N(0);
                if (uns && v >= 9223372036854775808.0) { ev_unsigned(c, out, v >= 18446744073709551615.0 ? UINT64_MAX : (uint64_t)v); return true; }
                v = v < 0 ? ceil(v - 0.5) : floor(v + 0.5);
                x = v >= 9223372036854775807.0 ? INT64_MAX : v <= -9223372036854775808.0 ? INT64_MIN : (int64_t)v;
            } else {                                            // testo: come MySQL, la parte intera iniziale
                const char* t = S(0);
                while (*t == ' ') t++;
                if (uns && *t != '-') { ev_unsigned(c, out, (uint64_t)strtoull(t, NULL, 10)); return true; }
                x = (int64_t)strtoll(t, NULL, 10);
            }
            // UNSIGNED come MySQL: un negativo vale modulo 2^64 (CAST(-1 AS UNSIGNED) = 18446744073709551615)
            if (uns) ev_unsigned(c, out, (uint64_t)x); else ev_int(out, x);
            return true;
        }
        if (!strncmp(ty, "DECIMAL", 7) || !strncmp(ty, "NUMERIC", 7)) {
            int p = 10, sc = 0;
            sscanf(ty, "%*[A-Z] ( %d , %d", &p, &sc);
            sscanf(ty, "%*[A-Z](%d,%d", &p, &sc);
            char buf[120];
            snprintf(buf, sizeof(buf), "%.17g", N(0));
            char rnd[160];
            bool exact = decimal_round(buf, sc, rnd, sizeof(rnd));
            if (exact) ev_set_text(c, out, rnd, strlen(rnd)); else ev_num(out, N(0));
            out->is_num = true;
            out->is_dec = exact;
            out->num = N(0);
            return true;
        }
        if (!strcmp(ty, "DOUBLE") || !strcmp(ty, "FLOAT") || !strcmp(ty, "REAL")) { ev_num(out, N(0)); return true; }
        if (!strcmp(ty, "DATE") || !strcmp(ty, "DATETIME") || !strncmp(ty, "DATETIME(", 9) || !strcmp(ty, "TIME")) {
            MyDT t;
            if (!mydt_parse(S(0), &t)) { ev_null(out); return true; }
            if (!strcmp(ty, "TIME")) snprintf(b, sizeof(b), "%02d:%02d:%02d", t.h, t.mi, t.s);
            else mydt_text(&t, strcmp(ty, "DATE") != 0, b, sizeof(b));
            ev_set_text(c, out, b, strlen(b));
            return true;
        }
        if (!strcmp(ty, "JSON")) {                          // come MySQL: testo JSON valido, in forma normalizzata
            if (a[0].is_num) { ev_set_text(c, out, S(0), strlen(S(0))); return true; }
            if (!sj_valid(S(0))) { ctx_error(c, "%s", "Invalid JSON text in argument 1 to function cast_as_json"); ev_null(out); return true; }
            char* nj = sj_normalize(S(0), strlen(S(0)));
            if (!nj) { ctx_error(c, "%s", "Out of memory"); ev_null(out); return true; }
            ev_set_text(c, out, nj, strlen(nj));
            free(nj);
            return true;
        }
        // CHAR[(n)], BINARY, NCHAR: testo
        const char* s = S(0);
        int n = -1;
        if (sscanf(ty, "%*[A-Z](%d)", &n) == 1 && n >= 0) { const char* end = utf8_at(s, n); ev_set_text(c, out, s, (size_t)(end - s)); return true; }
        ev_set_text(c, out, s, strlen(s));
        return true;
    }
    // date
    if (FIS("UNIX_TIMESTAMP")) {
        MyDT t;
        if (!mydt_parse(S(0), &t) || !t.has_date) { ev_int(out, 0); return true; }
        long long local = mydt_secs(&t);
        ev_int(out, local - tz_offset((time_t)local));
        return true;
    }
    if (FIS("FROM_UNIXTIME")) {
        long long secs = I(0);
        MyDT t;
        mydt_from_secs(secs + tz_offset((time_t)secs), &t);
        t.has_time = true;
        if (na > 1) { date_format_into(&t, S(1), &tb); tb_result(c, out, &tb); return true; }
        mydt_text(&t, true, b, sizeof(b));
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("STR_TO_DATE")) {
        MyDT t;
        if (!str_to_date(S(0), S(1), &t)) { ev_null(out); return true; }
        mydt_text(&t, t.has_time, b, sizeof(b));
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("SEC_TO_TIME")) {
        long long v = I(0);
        bool neg = v < 0;
        if (neg) v = -v;
        snprintf(b, sizeof(b), "%s%02lld:%02lld:%02lld", neg ? "-" : "", v / 3600, v / 60 % 60, v % 60);
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("MAKEDATE")) {
        long long y = I(0), doy = I(1);
        if (doy < 1) { ev_null(out); return true; }
        MyDT t;
        mydt_from_secs((days_from_civil(y, 1, 1) + doy - 1) * 86400LL, &t);
        mydt_text(&t, false, b, sizeof(b));
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("MAKETIME")) { snprintf(b, sizeof(b), "%02lld:%02lld:%02lld", I(0), I(1), I(2)); ev_set_text(c, out, b, strlen(b)); return true; }
    if (FIS("FROM_DAYS")) {
        MyDT t;
        mydt_from_secs((I(0) - 719528) * 86400LL, &t);
        mydt_text(&t, false, b, sizeof(b));
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("CONVERT_TZ")) { ev_copy(out, &a[0]); return true; }   // fusi orari: niente tabelle, il valore resta
    if (FIS("__INTERVAL")) { ctx_error(c, "%s", "INTERVAL can only be used with DATE_ADD / DATE_SUB or + / -"); ev_null(out); return true; }
    if (FIS("DATE_ADD") || FIS("DATE_SUB") || FIS("ADDDATE") || FIS("SUBDATE")) {
        int sign = (FIS("DATE_SUB") || FIS("SUBDATE")) ? -1 : 1;
        MyDT t;
        if (!mydt_parse(S(0), &t) || !t.has_date) { ev_null(out); return true; }
        const char* unit = "DAY";
        EvalVal iv;
        const char* amount;
        ASTExpr* ie = e->args[1];
        if (ie->type == EXPR_FUNC && !strcmp(ie->func, "__INTERVAL")) {
            eval_expr(ie->args[0], c, &iv);
            if (iv.null) { ev_null(out); return true; }
            amount = ev_s(&iv);
            unit = ie->args[1]->str_val;
        } else amount = S(1);                                  // ADDDATE(d, n): giorni
        char amt[64];
        snprintf(amt, sizeof(amt), "%s", amount);
        bool date_only = !t.has_time && (!strcmp(unit, "DAY") || !strcmp(unit, "WEEK") || !strcmp(unit, "MONTH") ||
                                         !strcmp(unit, "QUARTER") || !strcmp(unit, "YEAR") || !strcmp(unit, "YEAR_MONTH"));
        if (!mydt_add(&t, unit, amt, sign)) { ev_null(out); return true; }
        mydt_text(&t, !date_only, b, sizeof(b));
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("DATEDIFF")) {
        MyDT x, y;
        if (!mydt_parse(S(0), &x) || !mydt_parse(S(1), &y)) { ev_null(out); return true; }
        ev_int(out, days_from_civil(x.y, x.mo, x.d) - days_from_civil(y.y, y.mo, y.d));
        return true;
    }
    if (FIS("TIMEDIFF")) {
        MyDT x, y;
        if (!mydt_parse(S(0), &x) || !mydt_parse(S(1), &y)) { ev_null(out); return true; }
        long long d = (x.has_date ? mydt_secs(&x) : x.h * 3600LL + x.mi * 60 + x.s) - (y.has_date ? mydt_secs(&y) : y.h * 3600LL + y.mi * 60 + y.s);
        bool neg = d < 0;
        if (neg) d = -d;
        snprintf(b, sizeof(b), "%s%02lld:%02lld:%02lld", neg ? "-" : "", d / 3600, d / 60 % 60, d % 60);
        ev_set_text(c, out, b, strlen(b));
        return true;
    }
    if (FIS("TIMESTAMPDIFF")) {                          // (a, b, unità) dopo il parser
        MyDT x, y;
        if (!mydt_parse(S(0), &x) || !mydt_parse(S(1), &y)) { ev_null(out); return true; }
        const char* u = S(2);
        long long sx = mydt_secs(&x), sy = mydt_secs(&y);
        if (!strcmp(u, "MONTH") || !strcmp(u, "QUARTER") || !strcmp(u, "YEAR")) {
            long long m = ((long long)y.y * 12 + y.mo) - ((long long)x.y * 12 + x.mo);
            // mese incompleto: non conta
            long long rest_y = (long long)y.d * 86400 + y.h * 3600 + y.mi * 60 + y.s, rest_x = (long long)x.d * 86400 + x.h * 3600 + x.mi * 60 + x.s;
            if (m > 0 && rest_y < rest_x) m--;
            else if (m < 0 && rest_y > rest_x) m++;
            ev_int(out, !strcmp(u, "MONTH") ? m : !strcmp(u, "QUARTER") ? m / 3 : m / 12);
            return true;
        }
        long long d = sy - sx;
        long long div = !strcmp(u, "SECOND") ? 1 : !strcmp(u, "MINUTE") ? 60 : !strcmp(u, "HOUR") ? 3600 : !strcmp(u, "DAY") ? 86400 :
                        !strcmp(u, "WEEK") ? 604800 : !strcmp(u, "MICROSECOND") ? 0 : -1;
        if (div < 0) { ctx_error(c, "Unknown unit %s in TIMESTAMPDIFF", u); ev_null(out); return true; }
        ev_int(out, div == 0 ? d * 1000000 : d / div);
        return true;
    }
    if (FIS("SLEEP")) {
        double s = N(0);
        bool interrupted = false;
        if (s > 0) {
            long ms = (long)(s * 1000);
            if (ms > 60000) ms = 60000;
            // a fette di 50 ms: KILL QUERY e --timeout la interrompono (prima no: SLEEP(60) su molte righe teneva il
            // motore per ore). Come MySQL, una SLEEP interrotta vale 1
            while (ms > 0) {
                if (sv_cancel_now()) { interrupted = true; break; }
                long d = ms < 50 ? ms : 50;
#ifdef _WIN32
                sv_win_sleep((unsigned long)d);
#else
                struct timespec ts = { 0, d * 1000000L };
                nanosleep(&ts, NULL);
#endif
                ms -= d;
            }
        }
        if (interrupted) { ctx_error(c, "%s", "Query cancelled"); ev_null(out); return true; }
        ev_int(out, 0);
        return true;
    }
    if (FIS("GET_LOCK") || FIS("RELEASE_LOCK") || FIS("IS_FREE_LOCK")) { ev_int(out, 1); return true; }
    if (FIS("IS_USED_LOCK")) { ev_null(out); return true; }
    if (FIS("TIME_TO_SEC")) {
        MyDT t;
        if (!mydt_parse(S(0), &t)) { ev_null(out); return true; }
        ev_int(out, t.h * 3600LL + t.mi * 60 + t.s);
        return true;
    }
    // parti di una data
    MyDT t;
    if (!mydt_parse(S(f[0] == 'E' && FIS("EXTRACT") ? 1 : 0), &t)) { ev_null(out); return true; }
    if (FIS("DATE")) { if (!t.has_date) { ev_null(out); return true; } mydt_text(&t, false, b, sizeof(b)); ev_set_text(c, out, b, strlen(b)); return true; }
    if (FIS("TIME")) { snprintf(b, sizeof(b), "%02d:%02d:%02d", t.h, t.mi, t.s); ev_set_text(c, out, b, strlen(b)); return true; }
    if (FIS("HOUR")) { ev_int(out, t.h); return true; }
    if (FIS("MINUTE")) { ev_int(out, t.mi); return true; }
    if (FIS("SECOND")) { ev_int(out, t.s); return true; }
    if (FIS("MICROSECOND")) { ev_int(out, t.us); return true; }
    if (!t.has_date) { ev_null(out); return true; }
    if (FIS("DAYOFWEEK")) { ev_int(out, (weekday_mon0(&t) + 1) % 7 + 1); return true; }
    if (FIS("WEEKDAY")) { ev_int(out, weekday_mon0(&t)); return true; }
    if (FIS("DAYOFMONTH")) { ev_int(out, t.d); return true; }
    if (FIS("DAYOFYEAR")) { ev_int(out, day_of_year(&t)); return true; }
    if (FIS("WEEK")) { ev_int(out, mysql_week(&t, na > 1 ? (int)I(1) & 7 : 0, NULL)); return true; }
    if (FIS("WEEKOFYEAR")) { ev_int(out, mysql_week(&t, 3, NULL)); return true; }
    if (FIS("YEARWEEK")) { int yy; int w = mysql_week(&t, na > 1 ? ((int)I(1) & 7) | 2 : 2, &yy); ev_int(out, yy * 100 + w); return true; }
    if (FIS("QUARTER")) { ev_int(out, (t.mo + 2) / 3); return true; }
    if (FIS("MONTHNAME")) { const char* m = t.mo >= 1 && t.mo <= 12 ? MONTH_NAMES[t.mo - 1] : ""; ev_set_text(c, out, m, strlen(m)); return true; }
    if (FIS("DAYNAME")) { const char* d = DAY_NAMES[weekday_mon0(&t)]; ev_set_text(c, out, d, strlen(d)); return true; }
    if (FIS("LAST_DAY")) { t.d = days_in_month(t.y, t.mo); mydt_text(&t, false, b, sizeof(b)); ev_set_text(c, out, b, strlen(b)); return true; }
    if (FIS("TO_DAYS")) { ev_int(out, days_from_civil(t.y, t.mo, t.d) + 719528); return true; }
    if (FIS("TO_SECONDS")) { ev_int(out, (days_from_civil(t.y, t.mo, t.d) + 719528) * 86400LL + t.h * 3600 + t.mi * 60 + t.s); return true; }
    if (FIS("DATE_FORMAT") || FIS("TIME_FORMAT")) { date_format_into(&t, S(1), &tb); tb_result(c, out, &tb); return true; }
    if (FIS("EXTRACT")) {
        const char* u = S(0);
        long long v = !strcmp(u, "YEAR") ? t.y : !strcmp(u, "MONTH") ? t.mo : !strcmp(u, "DAY") ? t.d : !strcmp(u, "HOUR") ? t.h :
                      !strcmp(u, "MINUTE") ? t.mi : !strcmp(u, "SECOND") ? t.s : !strcmp(u, "MICROSECOND") ? t.us :
                      !strcmp(u, "QUARTER") ? (t.mo + 2) / 3 : !strcmp(u, "WEEK") ? mysql_week(&t, 0, NULL) :
                      !strcmp(u, "YEAR_MONTH") ? t.y * 100LL + t.mo : !strcmp(u, "DAY_HOUR") ? t.d * 100LL + t.h :
                      !strcmp(u, "HOUR_MINUTE") ? t.h * 100LL + t.mi : !strcmp(u, "DAY_MINUTE") ? (t.d * 100LL + t.h) * 100 + t.mi :
                      !strcmp(u, "MINUTE_SECOND") ? t.mi * 100LL + t.s : !strcmp(u, "HOUR_SECOND") ? (t.h * 100LL + t.mi) * 100 + t.s : -1;
        if (v < 0) { ctx_error(c, "Unknown unit %s in EXTRACT", u); ev_null(out); return true; }
        ev_int(out, v);
        return true;
    }
    #undef S
    #undef N
    #undef I
    free(tb.p);
    return false;
}

// GROUP_CONCAT: argomenti = valori, chiavi di ORDER BY, separatore; col_name = "#gc nv no flags"
typedef struct { char* text; EvalVal* keys; int nk; const char* flags; } GcItem;
static int gc_cmp(const void* x, const void* y) {
    const GcItem* a = x;
    const GcItem* b = y;
    for (int k = 0; k < a->nk; k++) {
        int r;
        if (a->keys[k].null || b->keys[k].null) r = a->keys[k].null - b->keys[k].null;
        else r = ev_cmp(&a->keys[k], &b->keys[k]);
        if (r) return a->flags[k] == 'D' ? -r : r;
    }
    return 0;
}

static void eval_group_concat(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    int nv = 1, no = 0;
    char flags[40] = "";
    sscanf(e->col_name, "#gc %d %d %39s", &nv, &no, flags);
    const char* sep = e->args[e->nargs - 1]->str_val ? e->args[e->nargs - 1]->str_val : ",";
    Tuple** saved = c->cur;
    GcItem* items = sv_calloc((size_t)(c->ngrp > 0 ? c->ngrp : 1), sizeof(GcItem));
    EvalVal* keys = no ? sv_calloc((size_t)(c->ngrp > 0 ? c->ngrp : 1) * (size_t)no, sizeof(EvalVal)) : NULL;
    int n = 0;
    StrMap seen = {0};
    bool oom = !items || (no && !keys);
    for (int k = 0; k < c->ngrp && !oom; k++) {
        c->cur = c->grp[k];
        TextBuf tb = {0};
        bool null = false;
        for (int i = 0; i < nv && !null; i++) {
            EvalVal v;
            eval_expr(e->args[i], c, &v);
            if (v.null) null = true; else { const char* s = ev_s(&v); tb_put(&tb, s, strlen(s)); }
        }
        if (null) { free(tb.p); continue; }                    // una riga con un NULL non conta (come MySQL)
        tb_put(&tb, "", 1);
        if (tb.oom) { free(tb.p); oom = true; break; }
        if (e->agg_distinct) {
            char key[KEY_BUF];
            key_text(key, sizeof(key), tb.p);
            SMSlot* sl = sm_slot(&seen, key, true);
            if (!sl) { free(tb.p); oom = true; break; }
            if (sl->val >= 0) { free(tb.p); continue; }
            sl->val = 1;
        }
        items[n].text = tb.p;
        items[n].nk = no;
        items[n].flags = flags;
        items[n].keys = keys ? &keys[(size_t)n * (size_t)no] : NULL;
        for (int j = 0; j < no; j++) {
            EvalVal* kv = &items[n].keys[j];
            eval_expr(e->args[nv + j], c, kv);
            if (!kv->null && !kv->is_num) {                    // il testo punta alla riga: copia
                char* dup = sv_strdup(ev_s(kv));
                if (!dup) { oom = true; break; }
                kv->str = dup;
            }
        }
        n++;
    }
    c->cur = saved;
    sm_free(&seen);
    if (!oom && no) qsort(items, (size_t)n, sizeof(GcItem), gc_cmp);
    TextBuf res = {0};
    size_t limit = 1024 * 1024;                                // group_concat_max_len (MySQL: 1024; qui più generoso)
    for (int i = 0; i < n && !oom; i++) {
        if (i) tb_put(&res, sep, strlen(sep));
        tb_put(&res, items[i].text, strlen(items[i].text));
        if (res.n > limit) { res.n = limit; break; }
    }
    for (int i = 0; i < n; i++) {
        free(items[i].text);
        for (int j = 0; j < no; j++) if (!items[i].keys[j].null && !items[i].keys[j].is_num) free((char*)items[i].keys[j].str);
    }
    free(items);
    free(keys);
    if (oom) { free(res.p); ctx_error(c, "%s", "Out of memory in GROUP_CONCAT"); ev_null(out); return; }
    if (n == 0) { free(res.p); ev_null(out); return; }
    tb_result(c, out, &res);
}

// valore SQL come testo JSON: numeri così come sono, NULL -> null, oggetti / array JSON così come sono, testo tra virgolette
static char* json_of(const EvalVal* v) {
    if (v->null) return sv_strdup("null");
    const char* s = ev_s(v);
    if (v->is_num) return sv_strdup(s);
    if ((*s == '{' || *s == '[') && sj_valid(s)) return sv_strdup(s);
    return sj_quote(s);
}

// funzioni JSON di modifica / ricerca e funzioni di testo e date di PostgreSQL / MySQL
static bool eval_more(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    const size_t flen = strlen(f);                       // per FIS: confronti per lunghezza prima (CR-M2)
    bool jmod = FIS("JSON_SET") || FIS("JSON_INSERT") || FIS("JSON_REPLACE") || FIS("JSON_REMOVE") ||
                FIS("JSON_ARRAY_APPEND") || FIS("JSON_ARRAY_INSERT");
    if (jmod) {
        EvalVal d;
        eval_expr(e->args[0], c, &d);
        if (d.null) { ev_null(out); return true; }
        char* doc = sv_strdup(ev_s(&d));
        int op = FIS("JSON_SET") ? 0 : FIS("JSON_INSERT") ? 1 : FIS("JSON_REPLACE") ? 2 : FIS("JSON_REMOVE") ? 3 :
                 FIS("JSON_ARRAY_APPEND") ? 4 : 5;
        int step = op == 3 ? 1 : 2;
        if (op != 3 && e->nargs % 2 == 0) { free(doc); ctx_error(c, "Wrong number of arguments for %s", f); ev_null(out); return true; }
        for (int i = 1; doc && i < e->nargs; i += step) {
            EvalVal pv, vv;
            eval_expr(e->args[i], c, &pv);
            if (pv.null) { free(doc); doc = NULL; break; }
            char* val = NULL;
            if (op != 3) { eval_expr(e->args[i + 1], c, &vv); val = json_of(&vv); }
            char* nd = sj_modify(doc, ev_s(&pv), val ? val : "", op);
            free(val);
            free(doc);
            doc = nd;
            if (!doc) { ctx_error(c, "Invalid JSON document or path in %s", f); ev_null(out); return true; }
        }
        if (!doc) { ev_null(out); return true; }
        char* nd = sj_normalize(doc, strlen(doc));
        free(doc);
        if (!nd) { ctx_error(c, "%s", "Out of memory"); ev_null(out); return true; }
        ev_set_text(c, out, nd, strlen(nd));
        free(nd);
        return true;
    }
    if (!strncmp(f, "JSON_", 5)) {
        bool known = FIS("JSON_KEYS") || FIS("JSON_CONTAINS") || FIS("JSON_CONTAINS_PATH") || FIS("JSON_SEARCH") ||
                     FIS("JSON_PRETTY") || FIS("JSON_DEPTH") || FIS("JSON_MERGE_PATCH") || FIS("JSON_QUOTE") ||
                     FIS("JSON_STORAGE_SIZE") || FIS("JSON_MERGE_PRESERVE") || FIS("JSON_MERGE");
        if (!known) return false;
        EvalVal d;
        eval_expr(e->args[0], c, &d);
        if (FIS("JSON_QUOTE")) {
            if (d.null) { ev_null(out); return true; }
            char* q = sj_quote(ev_s(&d));
            if (q) { ev_set_text(c, out, q, strlen(q)); free(q); } else ev_null(out);
            return true;
        }
        if (d.null) { ev_null(out); return true; }
        const char* doc = ev_s(&d);
        if (FIS("JSON_STORAGE_SIZE")) { ev_int(out, (int64_t)strlen(doc)); return true; }
        if (!sj_valid(doc)) { ctx_error(c, "Invalid JSON text in argument 1 to function %s", f); ev_null(out); return true; }
        char* docc = sv_strdup(doc);                         // il testo può puntare a un buffer che le prossime valutazioni cambiano
        if (!docc) { ev_null(out); return true; }
        char* r = NULL;
        if (FIS("JSON_KEYS") || FIS("JSON_DEPTH") || FIS("JSON_PRETTY")) {
            const char* v = docc; size_t vl = strlen(docc);
            if (e->nargs > 1) {
                EvalVal pv; eval_expr(e->args[1], c, &pv);
                if (pv.null || !sj_extract(docc, ev_s(&pv), &v, &vl)) { free(docc); ev_null(out); return true; }
            }
            if (FIS("JSON_DEPTH")) { ev_int(out, sj_depth(v)); free(docc); return true; }
            if (FIS("JSON_PRETTY")) r = sj_pretty(docc);
            else {
                char* tmp = sv_malloc(vl + 1);                       // (strndup non c'è su MinGW)
                if (tmp) { memcpy(tmp, v, vl); tmp[vl] = '\0'; }
                r = tmp ? sj_keys(tmp, vl) : NULL;
                free(tmp);
            }
        } else if (FIS("JSON_CONTAINS")) {
            EvalVal cv; eval_expr(e->args[1], c, &cv);
            if (cv.null) { free(docc); ev_null(out); return true; }
            char* cand = sv_strdup(ev_s(&cv));
            const char* v = docc; size_t vl = strlen(docc);
            bool ok = cand && sj_valid(cand);
            if (ok && e->nargs > 2) {
                EvalVal pv; eval_expr(e->args[2], c, &pv);
                if (pv.null || !sj_extract(docc, ev_s(&pv), &v, &vl)) { free(cand); free(docc); ev_null(out); return true; }
            }
            if (!ok) { free(cand); free(docc); ctx_error(c, "%s", "Invalid JSON text in argument 2 to function JSON_CONTAINS"); ev_null(out); return true; }
            ev_int(out, sj_contains(v, vl, cand, strlen(cand)));
            free(cand); free(docc);
            return true;
        } else if (FIS("JSON_CONTAINS_PATH")) {
            EvalVal mv; eval_expr(e->args[1], c, &mv);
            bool all = !mv.null && !strcasecmp(ev_s(&mv), "all");
            int hits = 0, n = 0;
            for (int i = 2; i < e->nargs; i++) {
                EvalVal pv; eval_expr(e->args[i], c, &pv);
                const char* v; size_t vl;
                n++;
                if (!pv.null && sj_extract(docc, ev_s(&pv), &v, &vl)) hits++;
            }
            ev_int(out, all ? hits == n : hits > 0);
            free(docc);
            return true;
        } else if (FIS("JSON_SEARCH")) {
            EvalVal mv, sv; eval_expr(e->args[1], c, &mv);
            bool all = !mv.null && !strcasecmp(ev_s(&mv), "all");
            eval_expr(e->args[2], c, &sv);
            if (sv.null) { free(docc); ev_null(out); return true; }
            char* pat = sv_strdup(ev_s(&sv));
            r = pat ? sj_search(docc, all, pat) : NULL;
            free(pat);
        } else {                                              // JSON_MERGE_PATCH / JSON_MERGE_PRESERVE (a, b, ...)
            bool patch = FIS("JSON_MERGE_PATCH");
            r = docc; docc = NULL;
            for (int i = 1; r && i < e->nargs; i++) {
                EvalVal pv; eval_expr(e->args[i], c, &pv);
                if (pv.null) { free(r); r = NULL; break; }
                if (!sj_valid(ev_s(&pv))) { free(r); ctx_error(c, "Invalid JSON text in argument to function %s", f); ev_null(out); return true; }
                char* nr = patch ? sj_merge_patch(r, ev_s(&pv)) : sj_merge_preserve(r, ev_s(&pv));
                free(r);
                r = nr;
            }
        }
        free(docc);
        if (!r) { ev_null(out); return true; }
        if (!strncmp(f, "JSON_MERGE", 10)) { char* nr = sj_normalize(r, strlen(r)); free(r); r = nr; if (!r) { ev_null(out); return true; } }
        ev_set_text(c, out, r, strlen(r));
        free(r);
        return true;
    }
    static const char* const MORE[] = { "INITCAP", "SPLIT_PART", "TRANSLATE", "CHR", "CONV", "SOUNDEX", "OVERLAY", "DATE_TRUNC",
                                        "DATE_PART", "STARTS_WITH", "REGEXP_COUNT", "MAKE_SET", "EXPORT_SET" };
    bool mine = false;
    for (size_t i = 0; i < sizeof(MORE) / sizeof(MORE[0]) && !mine; i++) mine = f[0] == MORE[i][0] && !strcmp(f, MORE[i]);
    if (!mine) return false;
    int na = e->nargs < 8 ? e->nargs : 8;
    EvalVal a[8];
    for (int i = 0; i < na; i++) eval_expr(e->args[i], c, &a[i]);
    if (FIS("MAKE_SET")) {                             // MAKE_SET(bit, s1, s2, ...): le stringhe dei bit accesi
        if (a[0].null) { ev_null(out); return true; }
        uint64_t bits = bit_operand(&a[0]);
        TextBuf tb = {0};
        int n = 0;
        for (int i = 1; i < e->nargs && i <= 64; i++) {
            if (!(bits >> (i - 1) & 1)) continue;
            EvalVal v; eval_expr(e->args[i], c, &v);
            if (v.null) continue;
            if (n++) tb_put(&tb, ",", 1);
            tb_put(&tb, ev_s(&v), strlen(ev_s(&v)));
        }
        if (!tb.p) { ev_set_text(c, out, "", 0); return true; }
        tb_result(c, out, &tb);
        return true;
    }
    for (int i = 0; i < na; i++) if (a[i].null) { ev_null(out); return true; }
    char b[256];
    if (FIS("INITCAP")) {                              // iniziali maiuscole
        const char* sv = ev_s(&a[0]);
        size_t n = strlen(sv);
        char* t = sv_malloc(n + 1);
        if (!t) { ev_null(out); return true; }
        bool start = true;
        for (size_t i = 0; i <= n; i++) {
            unsigned char ch = (unsigned char)sv[i];
            t[i] = (char)(ch >= 0x80 ? ch : start ? toupper(ch) : tolower(ch));
            start = !(isalnum(ch) || ch >= 0x80);
        }
        ev_set_text(c, out, t, n);
        free(t);
        return true;
    }
    if (FIS("SPLIT_PART")) {                           // n-esimo pezzo (da 1; negativo: dalla fine)
        const char* sv = ev_s(&a[0]);
        char* sc = sv_strdup(sv);
        char* dl = sv_strdup(ev_s(&a[1]));
        long n = (long)ev_as_num(&a[2]);
        if (!sc || !dl || n == 0) { free(sc); free(dl); if (n == 0) ctx_error(c, "%s", "SPLIT_PART field position must not be zero"); ev_null(out); return true; }
        size_t dlen = strlen(dl);
        const char* parts[1024];
        size_t lens[1024];
        int np = 0;
        const char* p = sc;
        for (;;) {
            const char* q = dlen ? strstr(p, dl) : NULL;
            if (np < 1024) { parts[np] = p; lens[np] = q ? (size_t)(q - p) : strlen(p); np++; }
            if (!q) break;
            p = q + dlen;
        }
        long k = n > 0 ? n - 1 : np + n;
        if (k >= 0 && k < np) ev_set_text(c, out, parts[k], lens[k]); else ev_set_text(c, out, "", 0);
        free(sc); free(dl);
        return true;
    }
    if (FIS("TRANSLATE")) {                            // caratteri di from sostituiti da quelli di to (tolti se to è più corto)
        char* sv = sv_strdup(ev_s(&a[0]));
        char* fr = sv_strdup(ev_s(&a[1]));
        char* to = sv_strdup(ev_s(&a[2]));
        if (!sv || !fr || !to) { free(sv); free(fr); free(to); ev_null(out); return true; }
        size_t tl = strlen(to), k = 0;
        for (size_t i = 0; sv[i]; i++) {
            char* hit = strchr(fr, sv[i]);
            if (!hit) { sv[k++] = sv[i]; continue; }
            size_t pos = (size_t)(hit - fr);
            if (pos < tl) sv[k++] = to[pos];
        }
        sv[k] = '\0';
        ev_set_text(c, out, sv, k);
        free(sv); free(fr); free(to);
        return true;
    }
    if (FIS("CHR")) {                                  // carattere Unicode del codice
        long cp = (long)ev_as_num(&a[0]);
        size_t k = 0;
        if (cp < 0 || cp > 0x10FFFF) { ev_null(out); return true; }
        if (cp < 0x80) b[k++] = (char)cp;
        else if (cp < 0x800) { b[k++] = (char)(0xC0 | (cp >> 6)); b[k++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { b[k++] = (char)(0xE0 | (cp >> 12)); b[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[k++] = (char)(0x80 | (cp & 0x3F)); }
        else { b[k++] = (char)(0xF0 | (cp >> 18)); b[k++] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[k++] = (char)(0x80 | (cp & 0x3F)); }
        ev_set_text(c, out, b, k);
        return true;
    }
    if (FIS("CONV")) {                                 // CONV(n, da_base, a_base), basi 2..36
        int fb = (int)ev_as_num(&a[1]), tbase = (int)ev_as_num(&a[2]);
        if (fb < 0) fb = -fb;
        bool neg_out = tbase < 0;
        if (tbase < 0) tbase = -tbase;
        if (fb < 2 || fb > 36 || tbase < 2 || tbase > 36) { ev_null(out); return true; }
        const char* sv = ev_s(&a[0]);
        bool neg = *sv == '-';
        if (neg) sv++;
        uint64_t v = 0;
        for (; *sv; sv++) {
            int d = isdigit((unsigned char)*sv) ? *sv - '0' : isalpha((unsigned char)*sv) ? toupper((unsigned char)*sv) - 'A' + 10 : 99;
            if (d >= fb) break;
            v = v * (uint64_t)fb + (uint64_t)d;
        }
        if (neg && !neg_out) v = (uint64_t)(-(int64_t)v);
        char tmp[80];
        int k = 0;
        do { int d = (int)(v % (uint64_t)tbase); tmp[k++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v /= (uint64_t)tbase; } while (v && k < 79);
        int o = 0;
        if (neg && neg_out) b[o++] = '-';
        while (k) b[o++] = tmp[--k];
        ev_set_text(c, out, b, (size_t)o);
        return true;
    }
    if (FIS("SOUNDEX")) {
        static const char* const CODE = "01230120022455012623010202";   // A..Z
        const char* sv = ev_s(&a[0]);
        int k = 0;
        char last = 0;
        for (; *sv && k < 64; sv++) {
            if (!isalpha((unsigned char)*sv)) continue;
            char u = (char)toupper((unsigned char)*sv);
            char d = CODE[u - 'A'];
            if (k == 0) { b[k++] = u; last = d; continue; }
            if (d != '0' && d != last) b[k++] = d;
            if (u != 'H' && u != 'W') last = d;
        }
        while (k > 0 && k < 4) b[k++] = '0';
        ev_set_text(c, out, b, (size_t)k);
        return true;
    }
    if (FIS("OVERLAY")) {                              // OVERLAY(s PLACING r FROM n [FOR m])
        const char* sv = ev_s(&a[0]);
        size_t sl = strlen(sv);
        char* sc = sv_strdup(sv);
        char* r = sv_strdup(ev_s(&a[1]));
        long from = (long)ev_as_num(&a[2]);
        long len = na > 3 ? (long)ev_as_num(&a[3]) : (long)(r ? strlen(r) : 0);
        if (!sc || !r || from < 1) { free(sc); free(r); ev_null(out); return true; }
        size_t st = (size_t)(from - 1) < sl ? (size_t)(from - 1) : sl;
        size_t en = len < 0 ? st : st + (size_t)len < sl ? st + (size_t)len : sl;
        TextBuf tb = {0};
        tb_put(&tb, sc, st); tb_put(&tb, r, strlen(r)); tb_put(&tb, sc + en, sl - en);
        free(sc); free(r);
        if (!tb.p) { ev_set_text(c, out, "", 0); return true; }
        tb_result(c, out, &tb);
        return true;
    }
    if (FIS("STARTS_WITH")) {
        const char* x = ev_s(&a[0]);
        char* y = sv_strdup(ev_s(&a[1]));
        ev_int(out, y && !strncmp(x, y, strlen(y)));
        free(y);
        return true;
    }
    if (FIS("REGEXP_COUNT")) {                         // occorrenze del modello
        char* sv = sv_strdup(ev_s(&a[0]));
        char* pat = sv_strdup(ev_s(&a[1]));
        long n = 0;
        char rerr[200];
        bool owned = false;
        SvRegex* re = sv && pat ? sv_regex_cached(pat, false, rerr, sizeof(rerr), &owned) : NULL;
        if (sv && pat && !re) { free(sv); free(pat); ctx_error(c, "%s", "Invalid regular expression"); ev_null(out); return true; }
        size_t from = 0, so, eo, sl = sv ? strlen(sv) : 0;
        sv_regex_take_limit();
        while (re && from <= sl && sv_regex_search(re, sv, from, &so, &eo)) { n++; from = eo > so ? eo : so + 1; }
        if (owned) sv_regex_free(re);
        free(sv); free(pat);
        if (regex_limit_error(c, out)) return true;
        ev_int(out, n);
        return true;
    }
    if (FIS("EXPORT_SET")) {                           // EXPORT_SET(bit, on, off [, sep [, n]])
        uint64_t bits = bit_operand(&a[0]);
        char* on = sv_strdup(ev_s(&a[1]));
        char* off = sv_strdup(ev_s(&a[2]));
        char* sep = sv_strdup(na > 3 ? ev_s(&a[3]) : ",");
        int nb = na > 4 ? (int)ev_as_num(&a[4]) : 64;
        if (nb < 0 || nb > 64) nb = 64;
        TextBuf tb = {0};
        for (int i = 0; on && off && sep && i < nb; i++) {
            if (i) tb_put(&tb, sep, strlen(sep));
            const char* w = (bits >> i & 1) ? on : off;
            tb_put(&tb, w, strlen(w));
        }
        free(on); free(off); free(sep);
        if (!tb.p) { ev_set_text(c, out, "", 0); return true; }
        tb_result(c, out, &tb);
        return true;
    }
    // DATE_TRUNC('month', d) (PostgreSQL) e DATE_PART('year', d) = EXTRACT(YEAR FROM d)
    MyDT t;
    char unit[32];
    snprintf(unit, sizeof(unit), "%s", ev_s(&a[0]));
    for (char* q = unit; *q; q++) *q = (char)toupper((unsigned char)*q);
    if (!mydt_parse(ev_s(&a[1]), &t) || !t.has_date) { ev_null(out); return true; }
    if (FIS("DATE_PART")) {
        long v;
        if (!strcmp(unit, "YEAR")) v = t.y; else if (!strcmp(unit, "MONTH")) v = t.mo; else if (!strcmp(unit, "DAY")) v = t.d;
        else if (!strcmp(unit, "HOUR")) v = t.h; else if (!strcmp(unit, "MINUTE")) v = t.mi; else if (!strcmp(unit, "SECOND")) v = t.s;
        else if (!strcmp(unit, "QUARTER")) v = (t.mo - 1) / 3 + 1;
        else if (!strcmp(unit, "DOW")) { struct tm tmv = { 0 }; tmv.tm_year = t.y - 1900; tmv.tm_mon = t.mo - 1; tmv.tm_mday = t.d; tmv.tm_hour = 12; mktime(&tmv); v = tmv.tm_wday; }
        else if (!strcmp(unit, "DOY")) { struct tm tmv = { 0 }; tmv.tm_year = t.y - 1900; tmv.tm_mon = t.mo - 1; tmv.tm_mday = t.d; tmv.tm_hour = 12; mktime(&tmv); v = tmv.tm_yday + 1; }
        else { ctx_error(c, "Unknown DATE_PART unit '%s'", unit); ev_null(out); return true; }
        ev_int(out, v);
        return true;
    }
    bool keep_time = true;
    if (!strcmp(unit, "YEAR")) { t.mo = 1; t.d = 1; t.h = t.mi = t.s = 0; }
    else if (!strcmp(unit, "QUARTER")) { t.mo = (t.mo - 1) / 3 * 3 + 1; t.d = 1; t.h = t.mi = t.s = 0; }
    else if (!strcmp(unit, "MONTH")) { t.d = 1; t.h = t.mi = t.s = 0; }
    else if (!strcmp(unit, "WEEK")) {                          // al lunedì
        struct tm tmv = { 0 }; tmv.tm_year = t.y - 1900; tmv.tm_mon = t.mo - 1; tmv.tm_mday = t.d; tmv.tm_hour = 12; mktime(&tmv);
        int back = (tmv.tm_wday + 6) % 7;
        tmv.tm_mday -= back; mktime(&tmv);
        t.y = tmv.tm_year + 1900; t.mo = tmv.tm_mon + 1; t.d = tmv.tm_mday; t.h = t.mi = t.s = 0;
    }
    else if (!strcmp(unit, "DAY")) { t.h = t.mi = t.s = 0; }
    else if (!strcmp(unit, "HOUR")) { t.mi = t.s = 0; }
    else if (!strcmp(unit, "MINUTE")) { t.s = 0; }
    else if (!strcmp(unit, "SECOND")) {}
    else { ctx_error(c, "Unknown DATE_TRUNC unit '%s'", unit); ev_null(out); return true; }
    t.has_time = keep_time;
    t.us = 0;
    mydt_text(&t, true, b, sizeof(b));
    ev_set_text(c, out, b, strlen(b));
    return true;
}

static void eval_func(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    const size_t flen = strlen(f);                       // per FIS: confronti per lunghezza prima (CR-M2)
    EvalVal a[3];
    int na = e->nargs < 3 ? e->nargs : 3;

    if (e->subkind == SUBK_UDF) { if (g_describe) { ev_null(out); return; } rt_eval_function(e, c, out); return; }
    if (eval_internal(e, c, out)) return;
    if (eval_sequence(e, c, out)) return;
    if (eval_more(e, c, out)) return;

    if (eval_mysql(e, c, out)) return;

    // MATCH(colonne) AGAINST('parole'): punteggio di pertinenza (0 = nessuna corrispondenza)
    if (FIS("MATCH_AGAINST")) {
        int nc = e->nargs - 2;
        EvalVal q, m;
        eval_expr(e->args[nc], c, &q);
        eval_expr(e->args[nc + 1], c, &m);
        if (q.null) { ev_null(out); return; }
        char* qs = sv_strdup(ev_s(&q));
        char** docs = sv_calloc((size_t)nc, sizeof(char*));
        bool oom = !qs || !docs;
        for (int i = 0; !oom && i < nc; i++) {
            EvalVal v;
            eval_expr(e->args[i], c, &v);
            if (!v.null && !(docs[i] = sv_strdup(ev_s(&v)))) oom = true;
        }
        if (oom) ctx_error(c, "%s", "Out of memory");
        double sc = oom ? 0 : ft_score((const char* const*)docs, nc, qs, m.is_int && m.inum == 1);
        for (int i = 0; docs && i < nc; i++) free(docs[i]);
        free(docs);
        free(qs);
        ev_num(out, sc);
        return;
    }

    // JSON_OBJECT / JSON_ARRAY: tutti gli argomenti (anche oltre 3); numeri così come sono, NULL -> null
    if (FIS("JSON_OBJECT") || FIS("JSON_ARRAY")) {
        bool obj = f[5] == 'O';
        if (obj && e->nargs % 2) { ctx_error(c, "%s", "JSON_OBJECT needs key, value pairs"); ev_null(out); return; }
        TextBuf tb = {0};
        tb_put(&tb, obj ? "{" : "[", 1);
        for (int i = 0; i < e->nargs; i++) {
            EvalVal v;
            eval_expr(e->args[i], c, &v);
            if (i) tb_put(&tb, obj && i % 2 ? ": " : ", ", 2);
            bool key = obj && i % 2 == 0;
            if (v.null) { if (key) { free(tb.p); ctx_error(c, "%s", "JSON_OBJECT key is NULL"); ev_null(out); return; } tb_put(&tb, "null", 4); continue; }
            const char* s = ev_s(&v);
            if (!key && (v.is_num || v.is_int)) { tb_put(&tb, s, strlen(s)); continue; }
            if (!key && sj_valid(s) && (*s == '{' || *s == '[')) { tb_put(&tb, s, strlen(s)); continue; }   // JSON annidato
            char* q = sj_quote(s);
            if (q) { tb_put(&tb, q, strlen(q)); free(q); }
        }
        tb_put(&tb, obj ? "}" : "]", 1);
        tb_result(c, out, &tb);
        return;
    }

    // funzioni che non valutano tutti gli argomenti subito
    if (FIS("COALESCE")) {
        for (int i = 0; i < e->nargs; i++) {
            eval_expr(e->args[i], c, out);
            if (!out->null) return;
        }
        ev_null(out);
        return;
    }
    if (FIS("IFNULL")) {
        eval_expr(e->args[0], c, out);
        if (out->null) eval_expr(e->args[1], c, out);
        return;
    }
    if (FIS("IF")) {
        EvalVal cond;
        eval_expr(e->args[0], c, &cond);
        eval_expr(e->args[ev_truth(&cond) == 1 ? 1 : 2], c, out);
        return;
    }
    if (FIS("CONCAT") || FIS("CONCAT_WS")) {
        bool ws = f[6] == '_';
        TextBuf tb = {0};
        EvalVal sep, v;
        if (ws) { eval_expr(e->args[0], c, &sep); if (sep.null) { ev_null(out); return; } }
        bool first = true;
        for (int i = ws ? 1 : 0; i < e->nargs; i++) {
            eval_expr(e->args[i], c, &v);
            if (v.null) { if (ws) continue; free(tb.p); ev_null(out); return; }   // CONCAT con un NULL = NULL
            const char* parts[2] = { ws && !first ? ev_s(&sep) : "", ev_s(&v) };
            for (int p = 0; p < 2; p++) tb_put(&tb, parts[p], strlen(parts[p]));
            first = false;
        }
        tb_result(c, out, &tb);
        return;
    }
    if (FIS("DATABASE") || FIS("SCHEMA")) {          // database corrente (NULL se nessuno)
        Database* db = c->x ? catalog_current_db(c->x->catalog) : NULL;
        if (db) ev_set_text(c, out, db->name, strlen(db->name)); else ev_null(out);
        return;
    }
    if (FIS("VERSION")) { ev_set_text(c, out, "8.0.35-SegVault", 15); return; }
    if (FIS("LAST_INSERT_ID")) { ev_int(out, g_last_insert_id); return; }
    if (FIS("NOW") || FIS("CURRENT_TIMESTAMP") || FIS("CURDATE") || FIS("CURRENT_DATE")) {
        char buf[32];
        now_text(buf, sizeof(buf), FIS("CURDATE") || FIS("CURRENT_DATE"));
        ev_set_text(c, out, buf, strlen(buf));
        return;
    }

    for (int i = 0; i < na; i++) {
        eval_expr(e->args[i], c, &a[i]);
        if (a[i].null && (!FIS("NULLIF"))) { ev_null(out); return; }   // NULL in ingresso -> NULL
    }
    if (FIS("NULLIF")) {
        if (a[0].null) { ev_null(out); return; }
        if (!a[1].null && ev_cmp(&a[0], &a[1]) == 0) { ev_null(out); return; }
        ev_copy(out, &a[0]);
        return;
    }
    if (FIS("UPPER") || FIS("UCASE") || FIS("LOWER") || FIS("LCASE")) {
        bool up = f[0] == 'U';
        const char* src = ev_s(&a[0]);
        size_t l = strlen(src);
        char* buf = sv_malloc(l + 1);
        if (!buf) { ctx_error(c, "%s", "Out of memory"); ev_null(out); return; }
        memcpy(buf, src, l);
        u8_case_inplace(buf, l, up);                    // UPPER('città') = 'CITTÀ' (prima solo ASCII)
        ev_set_text(c, out, buf, l);
        free(buf);
        return;
    }
    if (FIS("LENGTH"))        { ev_int(out, (int64_t)sv_bin_len(ev_s(&a[0]))); return; }   // byte veri
    if (!strncmp(f, "JSON_", 5)) {
        const char* doc = ev_s(&a[0]);
        if (FIS("JSON_VALID")) { ev_int(out, sj_valid(doc)); return; }
        if (FIS("JSON_UNQUOTE")) {
            char* u = sj_unquote(doc, strlen(doc));
            if (!u) { ev_null(out); return; }
            ev_set_text(c, out, u, strlen(u));
            free(u);
            return;
        }
        const char* vs = doc;
        size_t vl = strlen(doc);
        if (FIS("JSON_EXTRACT") || (FIS("JSON_LENGTH") && na == 2)) {
            if (!sj_extract(doc, ev_s(&a[1]), &vs, &vl)) { ev_null(out); return; }   // percorso assente: NULL
        } else if (!sj_valid(doc)) { ctx_error(c, "Invalid JSON text in argument 1 to %s", f); ev_null(out); return; }
        if (FIS("JSON_EXTRACT")) {
            if (*vs != '{' && *vs != '[') { ev_set_text(c, out, vs, vl); return; }
            char* nv = sj_normalize(vs, vl);                 // oggetti e array nella forma di MySQL
            if (!nv) { ev_null(out); return; }
            ev_set_text(c, out, nv, strlen(nv));
            free(nv);
            return;
        }
        if (FIS("JSON_TYPE")) { const char* t = sj_type(vs, vl); ev_set_text(c, out, t, strlen(t)); return; }
        ev_int(out, sj_length(vs, vl));
        return;
    }
    if (!strncmp(f, "CHAR", 4))      { ev_int(out, utf8_chars(ev_s(&a[0]))); return; }
    if (FIS("TRIM") || FIS("LTRIM") || FIS("RTRIM")) {
        const char* s = ev_s(&a[0]);
        const char* e2 = s + strlen(s);
        if (f[0] != 'R') while (*s == ' ') s++;
        if (f[0] != 'L') while (e2 > s && e2[-1] == ' ') e2--;
        ev_set_text(c, out, s, (size_t)(e2 - s));
        return;
    }
    if (FIS("SUBSTRING") || FIS("SUBSTR") || FIS("LEFT") || FIS("RIGHT")) {
        const char* s = ev_s(&a[0]);
        int chars = utf8_chars(s);
        int64_t start, count;
        if (FIS("LEFT"))       { start = 1; count = (int64_t)ev_as_num(&a[1]); }
        else if (FIS("RIGHT")) { count = (int64_t)ev_as_num(&a[1]); start = chars - count + 1; if (start < 1) start = 1; }
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
    if (FIS("REPLACE")) {
        const char* s = ev_s(&a[0]), *from = ev_s(&a[1]), *to = ev_s(&a[2]);
        size_t fl = strlen(from), tl = strlen(to);
        TextBuf tb = {0};
        while (*s && !tb.oom) {
            if (fl && !strncmp(s, from, fl)) { tb_put(&tb, to, tl); s += fl; }
            else {
                const char* nx = fl ? strstr(s, from) : NULL;         // copia in blocco fino alla prossima occorrenza
                size_t run = nx ? (size_t)(nx - s) : strlen(s);
                tb_put(&tb, s, run);
                s += run;
            }
        }
        tb_result(c, out, &tb);
        return;
    }
    if (FIS("REVERSE")) {
        const char* s = ev_s(&a[0]);
        size_t l = strlen(s);
        char* buf = sv_malloc(l + 1);
        if (!buf) { ctx_error(c, "%s", "Out of memory"); ev_null(out); return; }
        size_t k = 0;
        for (size_t i = l; i > 0; ) {                       // carattere UTF-8 intero
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
            memcpy(buf + k, s + j, i - j);
            k += i - j;
            i = j;
        }
        ev_set_text(c, out, buf, k);
        free(buf);
        return;
    }
    if (FIS("YEAR") || FIS("MONTH") || FIS("DAY")) {
        int y, m, d;
        if (sscanf(ev_s(&a[0]), "%d-%d-%d", &y, &m, &d) != 3) { ev_null(out); return; }
        ev_int(out, f[0] == 'Y' ? y : f[0] == 'M' ? m : d);
        return;
    }
    // numeriche
    double x = ev_as_num(&a[0]);
    if (FIS("ABS")) {
        if (a[0].is_int && a[0].inum != INT64_MIN) ev_int(out, a[0].inum < 0 ? -a[0].inum : a[0].inum);
        else ev_num(out, fabs(x));
        return;
    }
    if (FIS("SIGN"))  { ev_int(out, x > 0 ? 1 : x < 0 ? -1 : 0); return; }
    if (FIS("FLOOR") || !strncmp(f, "CEIL", 4)) {
        double r = f[0] == 'F' ? floor(x) : ceil(x);
        if (r >= -9.2e18 && r <= 9.2e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return;
    }
    if (FIS("ROUND")) {
        double dd = e->nargs > 1 ? ev_as_num(&a[1]) : 0;
        if (a[0].is_dec) {            // decimale: esatto, con d cifre (ROUND(150.000, 2) = 150.00, come MySQL)
            Dec xd, r;
            int d = dd > DEC_MAX_SCALE ? DEC_MAX_SCALE : dd < -60 ? -60 : (int)dd;
            if (ev_get_dec(&a[0], &xd) && dec_round(&xd, d, &r)) { ev_dec(out, &r); return; }
        }
        if (dd > 15) { ev_copy(out, &a[0]); if (!out->is_num) ev_num(out, x); return; }   // più cifre di quante ne ha un double
        if (dd < -308) { ev_int(out, 0); return; }
        int d = (int)dd;
        if (a[0].is_int && d >= 0) { ev_int(out, a[0].inum); return; }
        double p = pow(10.0, d);
        double r = (x < 0 ? -floor(-x * p + 0.5) : floor(x * p + 0.5)) / p;   // metà lontano da zero
        if (d <= 0 && r >= -9.2e18 && r <= 9.2e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return;
    }
    if (FIS("MOD")) {
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
    if (FIS("SQRT"))    { if (x < 0) ev_null(out); else ev_num(out, sqrt(x)); return; }
    ev_null(out);
}

// aggregati statistici, bit a bit, booleani e JSON
static bool eval_agg_extra(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    const size_t flen = strlen(f);                       // per FIS: confronti per lunghezza prima (CR-M2)
    bool stat = !strncmp(f, "STD", 3) || !strncmp(f, "VAR", 3);
    bool bit = !strncmp(f, "BIT_", 4);
    bool boo = !strncmp(f, "BOOL_", 5) || FIS("EVERY");
    bool jarr = FIS("JSON_ARRAYAGG"), jobj = FIS("JSON_OBJECTAGG");
    if (!stat && !bit && !boo && !jarr && !jobj) return false;
    Tuple** saved = c->cur;
    int64_t n = 0;
    double mean = 0, m2 = 0;                                   // Welford: media e somma dei quadrati degli scarti
    uint64_t acc = FIS("BIT_AND") ? UINT64_MAX : 0;
    int truth = -1;
    TextBuf tb = {0};
    if (jarr || jobj) tb_put(&tb, jarr ? "[" : "{", 1);
    StrMap seen = {0};
    for (int k = 0; k < c->ngrp && !c->err[0]; k++) {
        c->cur = c->grp[k];
        EvalVal v;
        eval_expr(e->args[0], c, &v);
        if (jarr || jobj) {
            EvalVal val;
            if (jobj) {
                if (v.null) { ctx_error(c, "%s", "JSON_OBJECTAGG key is NULL"); break; }
                eval_expr(e->args[1], c, &val);
                char* q = sj_quote(ev_s(&v));
                if (n) tb_put(&tb, ", ", 2);
                if (q) { tb_put(&tb, q, strlen(q)); free(q); }
                tb_put(&tb, ": ", 2);
            } else {
                ev_copy(&val, &v);
                if (n) tb_put(&tb, ", ", 2);
            }
            if (val.null) tb_put(&tb, "null", 4);
            else {
                const char* sv = ev_s(&val);
                if (val.is_num || (sj_valid(sv) && (*sv == '{' || *sv == '['))) tb_put(&tb, sv, strlen(sv));
                else { char* q = sj_quote(sv); if (q) { tb_put(&tb, q, strlen(q)); free(q); } }
            }
            n++;
            continue;
        }
        if (v.null) continue;
        if (e->agg_distinct) {
            char key[KEY_BUF];
            if (v.is_num) snprintf(key, sizeof(key), "n%.17g", ev_as_num(&v));
            else { key[0] = 's'; key_text(key + 1, sizeof(key) - 1, ev_s(&v)); }
            SMSlot* sl = sm_slot(&seen, key, true);
            if (!sl || sl->val >= 0) continue;
            sl->val = 1;
        }
        n++;
        if (stat) {
            double x = ev_as_num(&v), d = x - mean;
            mean += d / (double)n;
            m2 += d * (x - mean);
        } else if (bit) {
            uint64_t x = bit_operand(&v);
            if (FIS("BIT_AND")) acc &= x; else if (FIS("BIT_OR")) acc |= x; else acc ^= x;
        } else {
            int t = ev_truth(&v);
            if (truth < 0) truth = t;
            else if (FIS("BOOL_OR")) truth = truth || t;
            else truth = truth && t;
        }
    }
    c->cur = saved;
    sm_free(&seen);
    if (jarr || jobj) {
        tb_put(&tb, jarr ? "]" : "}", 1);
        if (n == 0) { free(tb.p); ev_null(out); return true; }
        tb_result(c, out, &tb);
        return true;
    }
    if (bit) { ev_unsigned(c, out, n || (!FIS("BIT_AND")) ? acc : UINT64_MAX); return true; }
    if (boo) { if (truth < 0) ev_null(out); else ev_int(out, truth); return true; }
    // varianza / deviazione: _POP (e STD, STDDEV, VARIANCE come MySQL) divide per n, _SAMP per n - 1
    bool samp = strstr(f, "SAMP") != NULL;
    if (n == 0 || (samp && n < 2)) { ev_null(out); return true; }
    double var = m2 / (double)(samp ? n - 1 : n);
    ev_num(out, !strncmp(f, "VAR", 3) ? var : sqrt(var));
    return true;
}

// chiave di DISTINCT negli aggregati: i numeri uguali hanno la stessa chiave (5 e 5.0), i testi restano distinti dai
// numeri; colonna _ci: 'Città' e 'CITTA' sono lo stesso valore
static void distinct_key(EvalVal* v, char* key, size_t kn) {
    Dec kd;
    if (v->is_int) snprintf(key, kn, "n%lld", (long long)v->inum);
    else if (v->is_dec && ev_get_dec(v, &kd)) { key[0] = 'n'; dec_key(&kd, key + 1, kn - 1); }
    else if (v->is_num && v->num == floor(v->num) && fabs(v->num) < 9e18) snprintf(key, kn, "n%lld", (long long)v->num);
    else if (v->is_num) snprintf(key, kn, "n%.17g", v->num);
    else {
        char* fk = v->ci ? ci_fold(ev_s(v)) : NULL;
        key[0] = 's';
        key_text(key + 1, kn - 1, fk ? fk : ev_s(v));
        free(fk);
    }
}

static void eval_aggregate(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    if (!c->grp) { ctx_error(c, "%s", "Aggregate used outside of a group"); ev_null(out); return; }
    if (!strcmp(e->func, "GROUP_CONCAT")) { eval_group_concat(e, c, out); return; }
    if (eval_agg_extra(e, c, out)) return;
    bool count_star = e->nargs == 0;
    if (count_star) { ev_int(out, c->ngrp); return; }

    Tuple** saved = c->cur;
    int64_t count = 0, isum = 0;
    double sum = 0;
    bool all_int = true, overflow = false, have_best = false;
    Dec dsum;                                     // somma esatta se i valori sono decimali e interi (P1.9)
    dec_from_i64(0, &dsum);
    bool any_dec = false, dec_ok = true;
    int max_scale = 0;
    bool is_min = !strcmp(e->func, "MIN"), is_max = !strcmp(e->func, "MAX");
    EvalVal best, v;
    StrMap seen = {0};
    for (int k = 0; k < c->ngrp; k++) {
        c->cur = c->grp[k];
        if (e->nargs > 1) {                                      // COUNT(DISTINCT a, b, ...): righe con un NULL escluse
            SvStrBuf kb = {0};
            bool any_null = false;
            for (int i = 0; i < e->nargs && !any_null; i++) {
                eval_expr(e->args[i], c, &v);
                if (v.null) { any_null = true; break; }
                char key[KEY_BUF];
                distinct_key(&v, key, sizeof(key));
                svsb_printf(&kb, "%u:%s", (unsigned)strlen(key), key);   // lunghezza in testa: chiave composta univoca
            }
            if (any_null || kb.oom) { bool oom = kb.oom; svsb_free(&kb); if (oom) { ctx_error(c, "%s", "Out of memory"); break; } continue; }
            SMSlot* sl = sm_slot(&seen, kb.p, true);
            svsb_free(&kb);
            if (!sl) { ctx_error(c, "%s", "Out of memory"); break; }
            if (sl->val >= 0) continue;
            sl->val = 1;
            count++;
            continue;
        }
        eval_expr(e->args[0], c, &v);
        if (v.null) continue;
        if (e->agg_distinct) {                                   // COUNT/SUM/AVG(DISTINCT x)
            char key[KEY_BUF];
            distinct_key(&v, key, sizeof(key));
            SMSlot* sl = sm_slot(&seen, key, true);
            if (!sl) { ctx_error(c, "%s", "Out of memory"); break; }
            if (sl->val >= 0) continue;
            sl->val = 1;
        }
        count++;
        if (v.is_num || !(is_min || is_max)) {
            double d = ev_as_num(&v);
            sum += d;
            if (dec_ok && !(is_min || is_max)) {
                Dec vd;
                if (ev_get_dec(&v, &vd)) {
                    if (v.is_dec) any_dec = true;
                    if (vd.scale > max_scale) max_scale = vd.scale;
                    if (!dec_add(&dsum, &vd, &dsum)) dec_ok = false;
                } else dec_ok = false;                       // un double o un testo: la somma resta double
            }
            if (v.is_int && !overflow) { if (__builtin_add_overflow(isum, v.inum, &isum)) overflow = true; }
            else all_int = false;
        }
        if ((is_min || is_max) && (!have_best || (is_min ? ev_cmp(&v, &best) < 0 : ev_cmp(&v, &best) > 0))) {
            ev_copy(&best, &v);
            if (best.str && best.str != best.buf && !best.is_num) {   // il testo punta alla tupla: copia (la riga corrente cambia)
                if (!ev_text_long(&best, best.str, strlen(best.str))) { ctx_error(c, "%s", "Out of memory"); break; }
            }
            have_best = true;
        }
    }
    sm_free(&seen);
    c->cur = saved;
    if (!strcmp(e->func, "COUNT")) { ev_int(out, count); return; }
    if (count == 0) { ev_null(out); return; }                    // SUM/AVG/MIN/MAX di nessun valore = NULL
    if (!strcmp(e->func, "SUM")) {
        if (all_int && !overflow) ev_int(out, isum);
        else if (any_dec && dec_ok) ev_dec(out, &dsum);          // SUM di DECIMAL: esatto, con la scala della colonna
        else ev_num(out, sum);
        return;
    }
    if (!strcmp(e->func, "AVG")) {
        Dec cnt, q;
        dec_from_i64(count, &cnt);
        // DECIMAL e anche interi: AVG(int) = DECIMAL con scala 4 in MySQL (7.3333, non 7.33333333333333)
        if ((any_dec || (all_int && !overflow)) && dec_ok && dec_div(&dsum, &cnt, max_scale + DEC_DIV_INC, &q)) { ev_dec(out, &q); return; }   // scala + 4, come MySQL
        ev_num(out, sum / (double)count);
        return;
    }
    ev_copy(out, &best);
}

static void eval_expr(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    out->ci = false;                       // lo imposta solo la lettura di una colonna _ci (e chi la copia)
    if (!e) { ev_null(out); return; }
    switch (e->type) {
        case EXPR_NULL:   ev_null(out); return;
        case EXPR_INT:    ev_int(out, e->int_val); return;
        case EXPR_FLOAT:  if (e->col_name[0]) ev_dec_text(out, e->col_name, false); else ev_num(out, e->float_val); return;
        case EXPR_STRING: out->null = false; out->is_num = out->is_int = false; out->is_dec = false; out->str = e->str_val ? e->str_val : ""; return;
        case EXPR_COLUMN: {
            if (e->bound < 0) {                                  // alias della SELECT
                int k = -e->bound - 1;
                if (k < c->nsel) { eval_expr(c->sel[k], c, out); return; }
                ev_null(out);
                return;
            }
            if (e->bound == 0) { ctx_error(c, "Unresolved column '%s'", e->col_name); ev_null(out); return; }
            if (e->rt_const) {                                   // variabile della routine in esecuzione
                int s = BOUND_SRC(e->bound), col = BOUND_COL(e->bound);
                const TableSchema* rts = NULL;
                Tuple* t = c->x ? rt_scope_row(c->x->routine, s, &rts) : NULL;
                if (!t || !rts || col >= t->num_values || col >= rts->num_columns) { ev_null(out); return; }
                ev_from_col(out, &t->values[col], &rts->columns[col]);
                return;
            }
            const EvalCtx* cc = c;
            for (int k = 0; k < e->outer && cc; k++) cc = cc->outer;   // colonna della query esterna
            int s = BOUND_SRC(e->bound), col = BOUND_COL(e->bound);
            Tuple* t = (cc && cc->cur && s < cc->nsrc) ? cc->cur[s] : NULL;
            if (!t || col >= t->num_values) { ev_null(out); return; }
            ev_from_col(out, &t->values[col], &cc->src[s].ts->columns[col]);
            out->ci = col_is_ci(cc->src[s].ts, col);
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
                    else {
                        Dec d;
                        if (a.is_dec && ev_get_dec(&a, &d)) { if (d.n) d.neg = !d.neg; ev_dec(out, &d); }
                        else ev_num(out, -ev_as_num(&a));
                    }
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
            if ((e->op == OP_LIKE || e->op == OP_NOT_LIKE) && e->right && e->right->type == EXPR_BINARY && e->right->op == OP_ESCAPE) {
                EvalVal x;
                eval_expr(e->left, c, &a);
                eval_expr(e->right->left, c, &b);
                eval_expr(e->right->right, c, &x);
                if (a.null || b.null || x.null) { ev_null(out); return; }
                const char* es = ev_s(&x);
                if (strlen(es) != 1) { ev_null(out); return; }     // ESCAPE vuoto o lungo: nessun risultato
                bool m = like_match_cs(ev_s(&a), ev_s(&b), es[0], (coll_kind(e->left) | coll_kind(e->right->left)) == 1);
                ev_int(out, e->op == OP_LIKE ? m : !m);
                return;
            }
            eval_expr(e->left, c, &a);
            eval_expr(e->right, c, &b);
            if (a.null || b.null) { ev_null(out); return; }
            int ck = coll_kind(e->left) | coll_kind(e->right);     // collation esplicita: vale per entrambi gli operandi
            if (ck == 1) a.ci = b.ci = false;
            else if (ck == 2) { if (!a.is_num) a.ci = true; if (!b.is_num) b.ci = true; }
            switch (e->op) {
                case OP_EQ: ev_int(out, ev_cmp(&a, &b) == 0); return;
                case OP_NEQ: ev_int(out, ev_cmp(&a, &b) != 0); return;
                case OP_LT: ev_int(out, ev_cmp(&a, &b) < 0); return;
                case OP_GT: ev_int(out, ev_cmp(&a, &b) > 0); return;
                case OP_LE: ev_int(out, ev_cmp(&a, &b) <= 0); return;
                case OP_GE: ev_int(out, ev_cmp(&a, &b) >= 0); return;
                case OP_LIKE:     ev_int(out, like_match_cs(ev_s(&a), ev_s(&b), '\\', ck == 1)); return;
                case OP_NOT_LIKE: ev_int(out, !like_match_cs(ev_s(&a), ev_s(&b), '\\', ck == 1)); return;
                case OP_PLUS: case OP_MINUS: case OP_MUL: {
                    Dec dx, dy, dr;
                    if (ev_dec_pair(&a, &b, &dx, &dy)) {             // decimali esatti (scala: max per + -, somma per *)
                        bool ok = e->op == OP_PLUS ? dec_add(&dx, &dy, &dr) : e->op == OP_MINUS ? dec_sub(&dx, &dy, &dr) : dec_mul(&dx, &dy, &dr);
                        if (ok) { ev_dec(out, &dr); return; }
                        ctx_error(c, "%s", "DECIMAL value is out of range (more than 81 digits)");
                        ev_null(out);
                        return;
                    }
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
                    Dec dx, dy, dr;
                    if (ev_dec_pair(&a, &b, &dx, &dy)) {
                        if (dec_is_zero(&dy) || !dec_mod(&dx, &dy, &dr)) { ev_null(out); return; }
                        ev_dec(out, &dr);
                        return;
                    }
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
                    Dec dx, dy, dr;
                    // DECIMAL, e anche intero / intero: in MySQL 7 / 2 = 3.5000 (DECIMAL con scala 0 + 4), non 3.5
                    if (ev_dec_pair(&a, &b, &dx, &dy) || (a.is_int && b.is_int && ev_get_dec(&a, &dx) && ev_get_dec(&b, &dy))) {   // scala del dividendo + 4 (div_precision_increment)
                        if (dec_is_zero(&dy) || !dec_div(&dx, &dy, dx.scale + DEC_DIV_INC, &dr)) { ev_null(out); return; }
                        ev_dec(out, &dr);
                        return;
                    }
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
static bool join_key(const Tuple* t, int col, DataType type, char* buf, size_t n, bool ci) {
    if (!t || col >= t->num_values || t->values[col].is_null) return false;
    const Value* v = &t->values[col];
    if (type == SV_TYPE_INT)         snprintf(buf, n, "%d", v->int_val);
    else if (type == SV_TYPE_BIGINT) snprintf(buf, n, "%lld", (long long)v->bigint_val);
    else {
        char* f = ci ? ci_fold(v->str_val ? v->str_val : "") : NULL;
        key_text(buf, n, f ? f : (v->str_val ? v->str_val : ""));
        free(f);
    }
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
        if (col->type != EXPR_COLUMN || col->bound <= 0 || BOUND_SRC(col->bound) != 0) continue;
        int ci = BOUND_COL(col->bound);
        if (!col_is_indexed(ts, ci)) continue;
        DataType t = ts->columns[ci].type;
        double d;
        if (is_int_type(t) && val->type == EXPR_INT) {
            if (t == SV_TYPE_INT && (val->int_val < INT32_MIN || val->int_val > INT32_MAX)) continue;
            snprintf(key, keyn, "%lld", (long long)val->int_val);
        } else if (is_int_type(t) && val->type == EXPR_STRING && val->str_val && text_is_number(val->str_val, &d) &&
                   d == floor(d) && fabs(d) <= 2147483647.0) {                // WHERE id = '5'
            snprintf(key, keyn, "%lld", (long long)d);
        } else if (is_text_type(t) && val->type == EXPR_STRING) {
            char* f = col_is_ci(ts, ci) ? ci_fold(val->str_val ? val->str_val : "") : NULL;
            key_text(key, keyn, f ? f : (val->str_val ? val->str_val : ""));
            free(f);
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
    if (g_describe) return true;                               // descrizione: tabella vuota, niente letto
    HeapFile* hf = heap_open(ctx->bp, s->ts->root_page_id, s->ts->num_pages);
    if (!hf) return false;
    int cap = 0, ci = -1;
    char key[KEY_BUF];
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
    // annullata a metà (KILL QUERY, limite di tempo): mai righe in meno, la lettura fallisce
    // (un UPDATE / DELETE si ferma così prima di scrivere)
    if (ok && sv_scan_cancelled()) ok = false;
    return ok;
}


// ── valori del risultato (tipizzati: i numeri restano numeri) ──

typedef SortKey OVal;

// copia di un valore calcolato (il testo viene duplicato; i numeri senza testo restano binari)
static void key_set(SortKey* k, const EvalVal* v) {
    k->null = v->null; k->is_num = v->is_num; k->is_int = v->is_int; k->inum = v->inum; k->num = v->num; k->ci = v->ci && !v->is_num;
    k->is_dec = !v->null && v->is_dec;
    k->str = (v->null || (v->is_num && !v->str)) ? NULL : sv_strdup(ev_s(v));
}

static void ev_from_oval(EvalVal* v, const OVal* o) {
    if (o->null) { ev_null(v); return; }
    v->null = false; v->is_num = o->is_num; v->is_int = o->is_int; v->inum = o->inum; v->num = o->num;
    v->str = o->str ? o->str : (o->is_num ? NULL : "");
    v->ci = o->ci;
    v->is_dec = o->is_dec && v->str;
}

static int key_cmp(const SortKey* a, const SortKey* b) {
    if (a->null || b->null) return a->null == b->null ? 0 : a->null ? -1 : 1;   // NULL per primi
    if (a->is_int && b->is_int) return a->inum < b->inum ? -1 : a->inum > b->inum;
    if (a->is_num && b->is_num && !a->is_dec && !b->is_dec) return a->num < b->num ? -1 : a->num > b->num;
    if (!a->is_num && !b->is_num) {
        int c = (a->ci || b->ci) ? ci_cmp(a->str ? a->str : "", b->str ? b->str : "") : strcmp(a->str ? a->str : "", b->str ? b->str : "");
        return c < 0 ? -1 : c > 0;
    }
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

// valori uguali per il raggruppamento hanno lo stesso hash (5 e 5.0 compresi)
static uint64_t ev_hash(const EvalVal* v) {
    if (v->null) return 0x9e3779b97f4a7c15ULL;
    if (v->is_num) {
        if (v->is_int) return mix64((uint64_t)v->inum);
        Dec dd;
        int64_t iv;
        if (v->is_dec && ev_get_dec(v, &dd) && dec_to_i64(&dd, &iv)) {   // 5.00 come 5; altrimenti per il double
            Dec r;
            if (dec_round(&dd, 0, &r) && dec_cmp(&r, &dd) == 0) return mix64((uint64_t)iv);
        }
        double d = v->num;
        if (d == floor(d) && fabs(d) < 9e18) return mix64((uint64_t)(int64_t)d);
        uint64_t b;
        memcpy(&b, &d, 8);
        return mix64(b ^ 0x5bd1e995ULL);
    }
    // sempre senza maiuscole: 'Anna' e 'anna' finiscono nello stesso secchio, poi decide l'uguaglianza
    // (con o senza collation _ci): così i due lati di un confronto hanno sempre lo stesso hash
    return u8_ci_hash(ev_s(v)) ^ 0x27d4eb2f165667c5ULL;
}

// stessa chiave di raggruppamento? (NULL con NULL sì, numeri per valore, testi per contenuto)
static bool ev_eq_key(const EvalVal* a, const SortKey* b) {
    if (a->null || b->null) return a->null && b->null;
    if (a->is_num != b->is_num) return false;
    if (a->is_num && (a->is_dec || b->is_dec)) { EvalVal y; ev_from_oval(&y, b); Dec p, q; if (ev_dec_pair(a, &y, &p, &q)) return dec_cmp(&p, &q) == 0; }
    if (a->is_num) return (a->is_int && b->is_int) ? a->inum == b->inum : a->num == b->num;
    if (a->ci || b->ci) return ci_cmp(ev_s(a), b->str ? b->str : "") == 0;
    return strcmp(ev_s(a), b->str ? b->str : "") == 0;
}

typedef struct { uint64_t* h; int* v; size_t cap, n; } HMap;

static void hmap_free(HMap* m) { free(m->h); free(m->v); memset(m, 0, sizeof(*m)); }

// inserisce un elemento (anche con hash già presente: le collisioni si risolvono confrontando)
static bool hmap_put(HMap* m, uint64_t h, int val) {
    if ((m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 64;
        uint64_t* nh = sv_malloc(sizeof(uint64_t) * nc);
        int* nv = sv_malloc(sizeof(int) * nc);
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
static bool col_hash(const Tuple* t, int col, DataType type, uint64_t* h, bool ci) {
    if (!t || col >= t->num_values || t->values[col].is_null) return false;
    const Value* v = &t->values[col];
    if (type == SV_TYPE_INT) *h = mix64((uint64_t)(int64_t)v->int_val);
    else if (type == SV_TYPE_BIGINT) *h = mix64((uint64_t)v->bigint_val);
    else {                                                     // senza maiuscole: vedi ev_hash
        (void)ci;
        *h = u8_ci_hash(v->str_val ? v->str_val : "");
    }
    return true;
}

// chiave canonica (DISTINCT, UNION, INTERSECT, EXCEPT): numeri uguali hanno la stessa chiave
static size_t oval_key(const OVal* o, char* buf, size_t cap) {
    int n;
    if (o->null)        n = snprintf(buf, cap, "\x01\x1f");
    else if (o->is_int) n = snprintf(buf, cap, "n%lld\x1f", (long long)o->inum);
    else if (o->is_dec && o->str) {                     // decimale: forma canonica (1.50 = 1.5, 5.0 = 5)
        Dec dd;
        char kt[DEC_LIMBS * 9 + 80];
        if (dec_parse(o->str, &dd)) { dec_key(&dd, kt, sizeof(kt)); n = snprintf(buf, cap, "n%s\x1f", kt); }
        else n = snprintf(buf, cap, "n%.17g\x1f", o->num);
    }
    else if (o->is_num && o->num == floor(o->num) && fabs(o->num) < 9e18) n = snprintf(buf, cap, "n%lld\x1f", (long long)o->num);
    else if (o->is_num) n = snprintf(buf, cap, "n%.17g\x1f", o->num);
    else {
        if (cap < KEY_INLINE_MAX + 3) { buf[0] = '\0'; return 0; }
        buf[0] = 's';
        char* f = o->ci ? ci_fold(o->str ? o->str : "") : NULL;
        key_text(buf + 1, cap - 2, f ? f : (o->str ? o->str : ""));
        free(f);
        n = (int)strlen(buf);
        buf[n++] = '\x1f';
        buf[n] = '\0';
    }
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
        OVal* nv = sv_realloc(r->v, sizeof(OVal) * (size_t)nc * (size_t)w);
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

// stessa espressione (dopo il binding): stesse colonne, operatori, funzioni e costanti
static bool expr_same(const ASTExpr* a, const ASTExpr* b) {
    if (!a || !b) return a == b;
    if (a->type == EXPR_COLUMN && b->type == EXPR_COLUMN)
        return a->bound != 0 && a->bound == b->bound && a->outer == b->outer;
    if (a->type != b->type || a->op != b->op || a->nargs != b->nargs || a->agg_distinct != b->agg_distinct ||
        strcasecmp(a->func, b->func)) return false;
    switch (a->type) {
        case EXPR_INT:    if (a->int_val != b->int_val) return false; break;
        case EXPR_FLOAT:  if (a->float_val != b->float_val) return false; break;
        case EXPR_STRING: if (strcmp(a->str_val ? a->str_val : "", b->str_val ? b->str_val : "")) return false; break;
        case EXPR_SUBQUERY: case EXPR_WINDOW: {          // stesso testo
            char x[512], y[512];
            ast_expr_format(a, x, sizeof(x));
            ast_expr_format(b, y, sizeof(y));
            return !strcasecmp(x, y);
        }
        default: break;
    }
    if (!expr_same(a->left, b->left) || !expr_same(a->right, b->right)) return false;
    for (int i = 0; i < a->nargs; i++) if (!expr_same(a->args[i], b->args[i])) return false;
    return true;
}

// ── colonne di output ──

typedef struct {
    ASTExpr* e;
    bool     owned;          // creata per espandere *
    char     name[160];
    int      src_type;       // tipo della colonna se è un riferimento diretto, altrimenti -1
} OutCol;

// ── vettori di espressioni ──

typedef struct { ASTExpr** v; int n, cap; } ExprVec;

static bool xv_push(ExprVec* xv, ASTExpr* e) {
    if (xv->n >= xv->cap) {
        int nc = xv->cap ? xv->cap * 2 : 4;
        ASTExpr** nv = sv_realloc(xv->v, sizeof(ASTExpr*) * (size_t)nc);
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
    // array per sorgente (P3.12): cap_src elementi in un solo blocco (srcblk), quanti le tabelle della query
    void*     srcblk;
    int       cap_src;
    Source* src;
    int       nsrc;
    OutCol*   out;               // nout colonne di output (lista dinamica: fino a SV_MAX_COLUMNS, P1.11)
    int       nout, cap_out;
    ASTExpr** sel;               // le loro espressioni (stessa capacità di out)
    bool      grouped;
    bool      correlated;        // usa colonne di una query esterna: rieseguita per ogni riga esterna
    bool      compound;          // primo ramo di UNION / INTERSECT / EXCEPT
    struct SelectPlan* next_arm; // ramo successivo
    int*      corder;                 // ORDER BY dell'operazione insiemistica: colonna di output (num_order)
    // dove si applica ogni condizione
    ExprVec* pre_static;  // filtri della sorgente valutati durante la lettura
    ExprVec* pre_dyn;     // filtri della sorgente che usano la query esterna
    ExprVec* jcond;       // condizioni del join con la sorgente k
    ExprVec   resid;                    // resto della WHERE (dopo i join)
    // ricerca per chiave nella sorgente: colonna = espressione senza colonne della sorgente
    int* probe_col;
    ASTExpr** probe_key;
    bool* probe_dyn;
    bool* used_index;
    bool* used_tree;     // la ricerca per chiave ha usato l'albero su disco (P1.6)
    int       batch;                      // righe per blocco della lettura (0 = tutte insieme)
    // scansione per intervallo con l'indice: colonna >, >=, <, <=, BETWEEN costanti
    int* range_col;
    ASTExpr** range_lo;
    ASTExpr** range_hi;
    long* range_rows;   // righe trovate con l'indice (-1 = indice non usato)
    long* probe_rows;   // righe trovate dalla ricerca per chiave nell'indice (-1 = non fatta)
    StrMap* pmap;
    int** pnext;
    bool      loaded;
    // risultato di una subquery non correlata
    ORows     cache;
    bool      cache_ok;
    int       cache_stop;
    StrMap    inset;                    // IN (SELECT ...): valori del risultato
    int       inset_class;              // 1 numeri, 2 testi, 3 misto
    bool      inset_null;
    bool      inset_ci;                // i valori della subquery vengono da una colonna _ci
    bool      inset_built;
    long      execs;                    // esecuzioni (EXPLAIN delle subquery)
    // NATURAL / USING: copia di destra di una colonna unita -> colonna di sinistra (BOUND_MAKE(s, c))
    int** using_left;   // per sorgente, colonne della sorgente (NULL = nessuna colonna unita)
    int* using_n;
    // funzioni finestra della query
    ASTExpr*  wins[64];           // funzioni finestra della query (P3.12: erano 16)
    int       nwins;
    // righe reali per EXPLAIN ANALYZE (sommate su tutte le esecuzioni)
    long* st_scanned;  // righe lette dalla tabella
    long* st_loaded;   // righe dopo i filtri della lettura
    long* st_dyn;      // righe dopo i filtri correlati
    long* st_join;     // combinazioni dopo il join con la sorgente k
    long      st_resid, st_groups, st_having, st_out;
    double    st_ms;
    char      err[300];
};

static int plan_width(const SelectPlan* p) { return p->nout; }
static int plan_using_left(const SelectPlan* p, int s, int c) {
    return (s >= 0 && s < p->cap_src && c >= 0 && c < p->using_n[s] && p->using_left[s]) ? p->using_left[s][c] : 0;
}

static bool plan_using_set(SelectPlan* p, int s, int c, int v) {
    if (s < 0 || s >= p->cap_src || c < 0 || c >= SV_MAX_COLUMNS) return false;
    if (c >= p->using_n[s]) {
        int nn = c + 1 > 16 ? c + 1 : 16;
        int* nv = sv_try_realloc(p->using_left[s], sizeof(int) * (size_t)nn);
        if (!nv) return false;
        memset(nv + p->using_n[s], 0, sizeof(int) * (size_t)(nn - p->using_n[s]));
        p->using_left[s] = nv;
        p->using_n[s] = nn;
    }
    p->using_left[s][c] = v;
    return true;
}

// una colonna di output in più (azzerata); NULL oltre SV_MAX_COLUMNS o senza memoria
static OutCol* plan_out_new(SelectPlan* p) {
    if (p->nout >= SV_MAX_COLUMNS) return NULL;
    if (p->nout >= p->cap_out) {
        int nc = p->cap_out ? p->cap_out * 2 : 16;
        OutCol* no = sv_try_realloc(p->out, sizeof(OutCol) * (size_t)nc);
        if (!no) return NULL;
        p->out = no;
        ASTExpr** ns = sv_try_realloc(p->sel, sizeof(ASTExpr*) * (size_t)nc);
        if (!ns) return NULL;
        p->sel = ns;
        p->cap_out = nc;
    }
    OutCol* oc = &p->out[p->nout++];
    memset(oc, 0, sizeof(*oc));
    return oc;
}
// la colonna (s, c) è unita (NATURAL / USING) a una colonna di una tabella successiva?
// restituisce il tipo di quel join (-1 = no)
static int plan_joined_copy(const SelectPlan* p, int s, int c, int* rk, int* rc) {
    for (int q = s + 1; q < p->nsrc; q++)
        for (int cc = 0; cc < p->src[q].ts->num_columns && cc < SV_MAX_COLUMNS; cc++)
            if (plan_using_left(p, q, cc) == BOUND_MAKE(s, c)) { if (rk) *rk = q; if (rc) *rc = cc; return (int)p->n->select_stmt.joins[q - 1].type; }
    return -1;
}

static int plan_join_type(const SelectPlan* p, int s) {
    return (s >= 1 && s < p->nsrc) ? (int)p->n->select_stmt.joins[s - 1].type : JOIN_INNER;
}
static void plan_mark_correlated(SelectPlan* p) { for (; p; p = p->next_arm) p->correlated = true; }
static bool plan_add_window(SelectPlan* p, ASTExpr* w) {
    for (int i = 0; i < p->nwins; i++) if (p->wins[i] == w) return true;
    if (p->nwins >= 64) return false;
    w->win->slot = p->nwins;
    p->wins[p->nwins++] = w;
    return true;
}


#define PLAN_SRC_FIELDS(X) X(Source, src) X(ExprVec, pre_static) X(ExprVec, pre_dyn) X(ExprVec, jcond) X(int, probe_col) X(ASTExpr*, probe_key) X(bool, probe_dyn) X(bool, used_index) X(bool, used_tree) X(int, range_col) X(ASTExpr*, range_lo) X(ASTExpr*, range_hi) X(long, range_rows) X(long, probe_rows) X(StrMap, pmap) X(int*, pnext) X(int*, using_left) X(int, using_n) X(long, st_scanned) X(long, st_loaded) X(long, st_dyn) X(long, st_join)

// gli array per sorgente del piano, per ns sorgenti, in un solo blocco azzerato
static bool plan_alloc_src(SelectPlan* p, int ns) {
    if (ns < 1) ns = 1;
    size_t off = 0;
#define X(T, f) size_t o_##f = off = (off + 15) & ~(size_t)15; off += sizeof(T) * (size_t)ns;
    PLAN_SRC_FIELDS(X)
#undef X
    char* b = sv_calloc(1, off);
    if (!b) return false;
#define X(T, f) p->f = (T*)(void*)(b + o_##f);
    PLAN_SRC_FIELDS(X)
#undef X
    p->srcblk = b;
    p->cap_src = ns;
    for (int s = 0; s < ns; s++) { p->probe_col[s] = -1; p->range_col[s] = -1; p->range_rows[s] = -1; p->probe_rows[s] = -1; }
    return true;
}

static void plan_free(SelectPlan* p) {
    while (p) {
        SelectPlan* nx = p->next_arm;
        for (int s = 0; s < p->cap_src; s++) free(p->using_left[s]);
        for (int s = 0; s < p->cap_src; s++) {
            free(p->pre_static[s].v); free(p->pre_dyn[s].v); free(p->jcond[s].v);
            sm_free(&p->pmap[s]); free(p->pnext[s]);
        }
        free(p->resid.v);
        for (int i = 0; i < p->nout; i++) if (p->out[i].owned) ast_expr_free(p->out[i].e);
        free(p->out);
        free(p->sel);
        for (int s = 0; s < p->nsrc; s++) source_free(&p->src[s]);
        orows_free(&p->cache);
        sm_free(&p->inset);
        free(p->srcblk);
        free(p->corder);
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
    if (e->type == EXPR_AGG && (!strncmp(e->func, "STD", 3) || !strncmp(e->func, "VAR", 3))) return "FLOAT";
    if (e->type == EXPR_AGG && (!strncmp(e->func, "BIT_", 4) || !strncmp(e->func, "BOOL_", 5) || !strcmp(e->func, "EVERY"))) return "BIGINT";
    if (e->type == EXPR_AGG && !strncmp(e->func, "JSON_", 5)) return "JSON";
    if (e->type == EXPR_AGG && (!strcmp(e->func, "MIN") || !strcmp(e->func, "MAX")) && e->nargs == 1 && e->args[0]->type == EXPR_COLUMN &&
        e->args[0]->bound > 0 && e->args[0]->outer == 0)        // MIN/MAX di una colonna
        return datatype_name(p->src[BOUND_SRC(e->args[0]->bound)].ts->columns[BOUND_COL(e->args[0]->bound)].type);
    bool any = false, all_int = true, all_num = true, small = true, any_dec = false, any_dbl = false;
    for (int i = 0; rows && i < rows->nrows; i++) {
        const OVal* o = &orow(rows, i)[j];
        if (o->null) continue;
        any = true;
        if (!o->is_int) all_int = false;
        else if (o->inum < INT32_MIN || o->inum > INT32_MAX) small = false;
        if (!o->is_num) all_num = false;
        else if (o->is_dec) any_dec = true;
        else if (!o->is_int) any_dbl = true;
    }
    if (!any) return for_schema ? "VARCHAR" : (e->type == EXPR_AGG && !strcmp(e->func, "AVG") ? "FLOAT" : "");
    if (all_int) return (for_schema && !small) ? "BIGINT" : "INT";
    // decimali esatti (SUM / AVG di DECIMAL, 0.10 + 0.20, ...): DECIMAL, così il protocollo MySQL li manda come
    // NEWDECIMAL (testo esatto) e non come double
    if (all_num && any_dec && !any_dbl) return "DECIMAL";
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
                    v->str_val = sv_strdup(ev_s(&ev));
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
    TableSchema* ts = schema_new(sp->nout);
    bool ok = ts != NULL;
    if (ok) {
        snprintf(ts->name, sizeof(ts->name), "%s", s->table);
        ts->num_columns = sp->nout;
        for (int i = 0; i < sp->nout; i++) {
            const char* cn = sp->out[i].name;
            const char* dot = strrchr(cn, '.');
            snprintf(ts->columns[i].name, sizeof(ts->columns[i].name), "%.63s", dot ? dot + 1 : cn);
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
        if (row[j].str && !(r[j].str = sv_strdup(row[j].str))) return false;
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
    // tutti i SELECT dell'unione (P3.11: con arms[64] quelli dopo il 64° venivano ignorati senza errore)
    int na = 0;
    for (ASTNode* a = c->node; a; a = a->select_stmt.set_next) na++;
    ASTNode** arms = sv_malloc(sizeof(ASTNode*) * (size_t)(na ? na : 1));
    if (!arms) { snprintf(err, errn, "Out of memory"); return 0; }
    na = 0;
    for (ASTNode* a = c->node; a; a = a->select_stmt.set_next) arms[na++] = a;
    int nanchor = 0;
    while (nanchor < na && !arm_refs_table(arms[nanchor], c->name)) nanchor++;
    if (nanchor == na) { free(arms); return -1; }                  // non ricorsiva: calcolo normale
    if (nanchor == 0) { snprintf(err, errn, "WITH RECURSIVE '%.64s': the first SELECT (the anchor) must not use '%.64s'", c->name, c->name); free(arms); return 0; }
    bool distinct = false;
    for (int k = 0; k + 1 < na; k++) {
        if (arms[k]->select_stmt.set_op != SETOP_UNION) { snprintf(err, errn, "WITH RECURSIVE '%.64s' must combine its SELECTs with UNION or UNION ALL", c->name); free(arms); return 0; }
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
        ts = schema_new(step.ncols);
        sigcap = (size_t)(step.ncols ? step.ncols : 1) * (EV_STR_MAX + 24) + 1;
        sig = sv_malloc(sigcap);
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
            snprintf(ts->columns[j].name, sizeof(ts->columns[j].name), "%.63s", dot ? dot + 1 : cn);
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
        if (sv_scan_cancelled()) { snprintf(err, errn, "Query cancelled"); ok = false; break; }
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
    free(arms);
    return ok ? 1 : 0;
}

// apre una sorgente: nome definito con WITH, tabella, vista o tabella derivata
// ═════════════════════════════════════════════════════════════════
//  information_schema: tabelle virtuali costruite dal catalogo (come in MySQL)
// ═════════════════════════════════════════════════════════════════

static _Thread_local bool g_infoschema;            // lo statement nomina information_schema.x
static _Thread_local bool (*g_see_db)(const char* db);   // permessi dell'utente (NULL = vede tutto)

void executor_set_infoschema(bool on) { g_infoschema = on; }
static _Thread_local bool g_mysql_mode;
void executor_set_mysql_mode(bool on) { g_mysql_mode = on; }
bool executor_mysql_mode(void) { return g_mysql_mode; }
void executor_set_db_filter(bool (*see)(const char* db)) { g_see_db = see; }

typedef struct { Source* s; int cap; Tuple* t; int col; bool ok; } ISB;

// schema da "NOME:s,NOME:i,..." (s = testo, i = intero)
static TableSchema* is_schema(const char* name, const char* spec) {
    int cap = 1;
    for (const char* q = spec; *q; q++) cap += *q == ',';
    TableSchema* ts = schema_new(cap);
    if (!ts) return NULL;
    snprintf(ts->name, sizeof(ts->name), "%s", name);
    const char* p = spec;
    while (*p && ts->num_columns < ts->cap_columns) {
        const char* c = strchr(p, ':');
        if (!c) break;
        Column* col = &ts->columns[ts->num_columns++];
        snprintf(col->name, sizeof(col->name), "%.*s", (int)(c - p), p);
        col->type = c[1] == 'i' ? SV_TYPE_BIGINT : SV_TYPE_VARCHAR;
        col->nullable = true;
        p = c + 2;
        if (*p == ',') p++;
    }
    return ts;
}

static void isb_row(ISB* b) {
    b->t = tuple_create(b->s->ts->num_columns);
    b->col = 0;
    if (!b->t) b->ok = false;
}
static Value* isb_next(ISB* b) { return b->t && b->col < b->s->ts->num_columns ? &b->t->values[b->col++] : NULL; }
static void isb_s(ISB* b, const char* v) {
    Value* x = isb_next(b);
    if (!x) return;
    if (!v) { x->is_null = true; return; }
    x->str_val = sv_strdup(v);
    x->owns_str = x->str_val != NULL;
    if (!x->str_val) b->ok = false;
}
static void isb_i(ISB* b, long long v) { Value* x = isb_next(b); if (x) x->bigint_val = v; }
static void isb_null(ISB* b) { Value* x = isb_next(b); if (x) x->is_null = true; }
static void isb_end(ISB* b) {
    if (!b->t) return;
    while (b->col < b->s->ts->num_columns) isb_null(b);
    if (!source_push(b->s, b->t, (RID){0, 0}, &b->cap)) { tuple_free(b->t); b->ok = false; }
    b->t = NULL;
}

static bool is_see(const Database* d) { return !g_see_db || g_see_db(d->name); }

// tipo SQL come lo mostra MySQL ("bigint(20) unsigned", "varchar(255)", "decimal(10,2)")
static void is_coltype(const Column* c, char* out, size_t n) {
    if (c->sqltype[0]) { snprintf(out, n, "%s", c->sqltype); return; }
    switch (c->type) {
        case SV_TYPE_INT:      snprintf(out, n, "int"); break;
        case SV_TYPE_BIGINT:   snprintf(out, n, "bigint"); break;
        case SV_TYPE_FLOAT:    snprintf(out, n, "double"); break;
        case SV_TYPE_VARCHAR:  if (c->size > 0) snprintf(out, n, "varchar(%d)", c->size); else snprintf(out, n, "text"); break;
        case SV_TYPE_CHAR:     snprintf(out, n, "char(%d)", c->size > 0 ? c->size : 1); break;
        case SV_TYPE_TEXT:     snprintf(out, n, "text"); break;
        case SV_TYPE_BOOL:     snprintf(out, n, "tinyint(1)"); break;
        case SV_TYPE_DATE:     snprintf(out, n, "date"); break;
        case SV_TYPE_DATETIME: snprintf(out, n, "datetime"); break;
        case SV_TYPE_BLOB:     snprintf(out, n, "blob"); break;
        case SV_TYPE_DECIMAL:
            if (c->decimal_prec > 0) snprintf(out, n, "decimal(%d,%d)", c->size > 0 ? c->size : 10, c->decimal_prec - 1);
            else snprintf(out, n, "decimal(65,30)");
            break;
        default: snprintf(out, n, "text");
    }
}

// DATA_TYPE: il nome del tipo senza dimensioni né attributi
static void is_datatype(const Column* c, char* out, size_t n) {
    char t[64];
    is_coltype(c, t, sizeof(t));
    size_t l = strcspn(t, "( ");
    snprintf(out, n, "%.*s", (int)l, t);
}

static bool is_text_col(const Column* c) {
    return c->type == SV_TYPE_VARCHAR || c->type == SV_TYPE_CHAR || c->type == SV_TYPE_TEXT;
}

static long long is_char_max(const Column* c) {
    if (c->type == SV_TYPE_VARCHAR || c->type == SV_TYPE_CHAR) return c->size > 0 ? c->size : 65535;
    if (c->type == SV_TYPE_TEXT) {
        const char* t = c->sqltype;
        if (!strncmp(t, "tinytext", 8)) return 255;
        if (!strncmp(t, "mediumtext", 10)) return 16777215;
        if (!strncmp(t, "longtext", 8) || !strncmp(t, "json", 4)) return 4294967295LL;
        return 65535;
    }
    return -1;
}

// PRI / UNI / MUL come la colonna Key di SHOW COLUMNS
static const char* is_colkey(const TableSchema* ts, int ci) {
    const char* name = ts->columns[ci].name;
    if (ts->columns[ci].primary_key) return "PRI";
    for (int k = 0; k < ts->num_constraints; k++) {
        const Constraint* c = &ts->constraints[k];
        if (c->kind == CONS_PRIMARY && c->ncols > 0 && c->cols[0] == ci) return "PRI";
    }
    for (int k = 0; k < ts->num_constraints; k++) {
        const Constraint* c = &ts->constraints[k];
        if (c->kind == CONS_UNIQUE && c->ncols == 1 && c->cols[0] == ci) return "UNI";
    }
    if (ts->columns[ci].unique) return "UNI";
    for (int k = 0; k < ts->num_constraints; k++) {
        const Constraint* c = &ts->constraints[k];
        if ((c->kind == CONS_UNIQUE || c->kind == CONS_FOREIGN) && c->ncols > 0 && c->cols[0] == ci) return "MUL";
    }
    for (int i = 0; i < ts->num_indexes; i++) if (!strcasecmp(ts->indexes[i].col_name, name)) return "MUL";
    return "";
}

static const char* is_fk_rule(int a) {
    switch (a) {
        case FK_CASCADE: return "CASCADE";
        case FK_SET_NULL: return "SET NULL";
        case FK_SET_DEFAULT: return "SET DEFAULT";
        case FK_RESTRICT: return "RESTRICT";
        default: return "NO ACTION";
    }
}

// collation dichiarata; senza, i testi si confrontano byte per byte: utf8mb4_bin (prima information_schema diceva
// utf8mb4_general_ci, cioè senza maiuscole, ma i confronti le distinguevano)
static const char* is_collation(const TableSchema* ts) { return ts->collation[0] ? ts->collation : "utf8mb4_bin"; }
static const char* is_col_collation(const TableSchema* ts, const Column* col) {   // COLLATE della colonna, se c'è
    bool tci = strlen(ts->collation) > 3 && !strcmp(ts->collation + strlen(ts->collation) - 3, "_ci");
    if ((col->flags & COLF_CI) && !tci) return "utf8mb4_general_ci";
    if ((col->flags & COLF_BIN) && tci) return "utf8mb4_bin";
    return is_collation(ts);
}

bool executor_priv_ok(const ExecCtx* x, const char* db, const char* table, unsigned priv, char* err, size_t errn) {
    if (!x || !x->user || !x->user[0] || x->priv_off > 0) return true;
    if (auth_privs(x->user, db, table) & priv) return true;
    snprintf(err, errn, "Access denied for user '%.63s': %s on %.63s.%.63s", x->user, auth_priv_name(priv), db ? db : "", table);
    return false;
}

// qualche permesso sulla tabella (DESCRIBE, SHOW CREATE, information_schema la mostrano)
static bool priv_any(const ExecCtx* x, const char* db, const char* table) {
    return !x || !x->user || !x->user[0] || x->priv_off > 0 || auth_privs(x->user, db, table) != 0;
}

static bool infoschema_open(ExecCtx* x, Source* s, const char* table, char* err, size_t errn) {
    Catalog* cat = x->catalog;
    char up[SV_MAX_NAME_LEN];
    snprintf(up, sizeof(up), "%s", table);
    for (char* q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
    static const struct { const char* name; const char* spec; } T[] = {
        { "SCHEMATA", "CATALOG_NAME:s,SCHEMA_NAME:s,DEFAULT_CHARACTER_SET_NAME:s,DEFAULT_COLLATION_NAME:s,SQL_PATH:s,DEFAULT_ENCRYPTION:s" },
        { "TABLES", "TABLE_CATALOG:s,TABLE_SCHEMA:s,TABLE_NAME:s,TABLE_TYPE:s,ENGINE:s,VERSION:i,ROW_FORMAT:s,TABLE_ROWS:i,"
                    "AVG_ROW_LENGTH:i,DATA_LENGTH:i,MAX_DATA_LENGTH:i,INDEX_LENGTH:i,DATA_FREE:i,AUTO_INCREMENT:i,CREATE_TIME:s,"
                    "UPDATE_TIME:s,CHECK_TIME:s,TABLE_COLLATION:s,CHECKSUM:i,CREATE_OPTIONS:s,TABLE_COMMENT:s" },
        { "COLUMNS", "TABLE_CATALOG:s,TABLE_SCHEMA:s,TABLE_NAME:s,COLUMN_NAME:s,ORDINAL_POSITION:i,COLUMN_DEFAULT:s,IS_NULLABLE:s,"
                     "DATA_TYPE:s,CHARACTER_MAXIMUM_LENGTH:i,CHARACTER_OCTET_LENGTH:i,NUMERIC_PRECISION:i,NUMERIC_SCALE:i,"
                     "DATETIME_PRECISION:i,CHARACTER_SET_NAME:s,COLLATION_NAME:s,COLUMN_TYPE:s,COLUMN_KEY:s,EXTRA:s,PRIVILEGES:s,"
                     "COLUMN_COMMENT:s,GENERATION_EXPRESSION:s,SRS_ID:i" },
        { "STATISTICS", "TABLE_CATALOG:s,TABLE_SCHEMA:s,TABLE_NAME:s,NON_UNIQUE:i,INDEX_SCHEMA:s,INDEX_NAME:s,SEQ_IN_INDEX:i,"
                        "COLUMN_NAME:s,COLLATION:s,CARDINALITY:i,SUB_PART:i,PACKED:s,NULLABLE:s,INDEX_TYPE:s,COMMENT:s,"
                        "INDEX_COMMENT:s,IS_VISIBLE:s,EXPRESSION:s" },
        { "TABLE_CONSTRAINTS", "CONSTRAINT_CATALOG:s,CONSTRAINT_SCHEMA:s,CONSTRAINT_NAME:s,TABLE_SCHEMA:s,TABLE_NAME:s,CONSTRAINT_TYPE:s,ENFORCED:s" },
        { "KEY_COLUMN_USAGE", "CONSTRAINT_CATALOG:s,CONSTRAINT_SCHEMA:s,CONSTRAINT_NAME:s,TABLE_CATALOG:s,TABLE_SCHEMA:s,TABLE_NAME:s,"
                              "COLUMN_NAME:s,ORDINAL_POSITION:i,POSITION_IN_UNIQUE_CONSTRAINT:i,REFERENCED_TABLE_SCHEMA:s,"
                              "REFERENCED_TABLE_NAME:s,REFERENCED_COLUMN_NAME:s" },
        { "REFERENTIAL_CONSTRAINTS", "CONSTRAINT_CATALOG:s,CONSTRAINT_SCHEMA:s,CONSTRAINT_NAME:s,UNIQUE_CONSTRAINT_CATALOG:s,"
                                     "UNIQUE_CONSTRAINT_SCHEMA:s,UNIQUE_CONSTRAINT_NAME:s,MATCH_OPTION:s,UPDATE_RULE:s,DELETE_RULE:s,"
                                     "TABLE_NAME:s,REFERENCED_TABLE_NAME:s" },
        { "CHECK_CONSTRAINTS", "CONSTRAINT_CATALOG:s,CONSTRAINT_SCHEMA:s,CONSTRAINT_NAME:s,CHECK_CLAUSE:s" },
        { "VIEWS", "TABLE_CATALOG:s,TABLE_SCHEMA:s,TABLE_NAME:s,VIEW_DEFINITION:s,CHECK_OPTION:s,IS_UPDATABLE:s,DEFINER:s,"
                   "SECURITY_TYPE:s,CHARACTER_SET_CLIENT:s,COLLATION_CONNECTION:s" },
        { "TRIGGERS", "TRIGGER_CATALOG:s,TRIGGER_SCHEMA:s,TRIGGER_NAME:s,EVENT_MANIPULATION:s,EVENT_OBJECT_CATALOG:s,"
                      "EVENT_OBJECT_SCHEMA:s,EVENT_OBJECT_TABLE:s,ACTION_ORDER:i,ACTION_CONDITION:s,ACTION_STATEMENT:s,"
                      "ACTION_ORIENTATION:s,ACTION_TIMING:s,CREATED:s,SQL_MODE:s,DEFINER:s" },
        { "ROUTINES", "SPECIFIC_NAME:s,ROUTINE_CATALOG:s,ROUTINE_SCHEMA:s,ROUTINE_NAME:s,ROUTINE_TYPE:s,DATA_TYPE:s,ROUTINE_BODY:s,"
                      "ROUTINE_DEFINITION:s,IS_DETERMINISTIC:s,SQL_DATA_ACCESS:s,SECURITY_TYPE:s,CREATED:s,LAST_ALTERED:s,"
                      "SQL_MODE:s,ROUTINE_COMMENT:s,DEFINER:s" },
        { "ENGINES", "ENGINE:s,SUPPORT:s,COMMENT:s,TRANSACTIONS:s,XA:s,SAVEPOINTS:s" },
        { "CHARACTER_SETS", "CHARACTER_SET_NAME:s,DEFAULT_COLLATE_NAME:s,DESCRIPTION:s,MAXLEN:i" },
        { "COLLATIONS", "COLLATION_NAME:s,CHARACTER_SET_NAME:s,ID:i,IS_DEFAULT:s,IS_COMPILED:s,SORTLEN:i,PAD_ATTRIBUTE:s" },
        { "PROCESSLIST", "ID:i,USER:s,HOST:s,DB:s,COMMAND:s,TIME:i,STATE:s,INFO:s" },
        { "PLUGINS", "PLUGIN_NAME:s,PLUGIN_VERSION:s,PLUGIN_STATUS:s,PLUGIN_TYPE:s" },
        { "USER_PRIVILEGES", "GRANTEE:s,TABLE_CATALOG:s,PRIVILEGE_TYPE:s,IS_GRANTABLE:s" },
        { "PARAMETERS", "SPECIFIC_CATALOG:s,SPECIFIC_SCHEMA:s,SPECIFIC_NAME:s,ORDINAL_POSITION:i,PARAMETER_MODE:s,PARAMETER_NAME:s,DATA_TYPE:s" },
        { "EVENTS", "EVENT_CATALOG:s,EVENT_SCHEMA:s,EVENT_NAME:s" },
        { "PARTITIONS", "TABLE_CATALOG:s,TABLE_SCHEMA:s,TABLE_NAME:s,PARTITION_NAME:s,TABLE_ROWS:i" },
        { "SESSION_VARIABLES", "VARIABLE_NAME:s,VARIABLE_VALUE:s" },
        { "SEQUENCES", "SEQUENCE_CATALOG:s,SEQUENCE_SCHEMA:s,SEQUENCE_NAME:s,DATA_TYPE:s,START_VALUE:i,MINIMUM_VALUE:i,MAXIMUM_VALUE:i,"
                       "INCREMENT:i,CYCLE_OPTION:s,CURRENT_VALUE:i" },
        { "GLOBAL_VARIABLES", "VARIABLE_NAME:s,VARIABLE_VALUE:s" },
    };
    int ti = -1;
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (!strcmp(up, T[i].name)) ti = (int)i;
    if (ti < 0) { snprintf(err, errn, "Unknown table '%.64s' in information_schema", table); return false; }
    TableSchema* ts = is_schema(T[ti].name, T[ti].spec);
    if (!ts) { snprintf(err, errn, "Out of memory"); return false; }
    s->is_view = true;
    s->own_ts = s->ts = ts;
    ISB b = { s, 0, NULL, 0, true };
    char buf[128], buf2[64];

    for (int d = 0; d < cat->num_databases && b.ok; d++) {
        Database* db = &cat->databases[d];
        if (!is_see(db)) continue;
        const char* sch = db->name;
        if (ti == 0) {                                               // SCHEMATA
            isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, "utf8mb4"); isb_s(&b, "utf8mb4_general_ci"); isb_null(&b); isb_s(&b, "NO"); isb_end(&b);
            continue;
        }
        for (int t = 0; t < db->num_tables && b.ok; t++) {
            TableSchema* tb = db->tables[t];
            if (!priv_any(x, sch, tb->name)) continue;              // solo le tabelle su cui l'utente ha permessi
            switch (ti) {
                case 1: {                                            // TABLES
                    isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, tb->name); isb_s(&b, "BASE TABLE");
                    isb_s(&b, "InnoDB"); isb_i(&b, 10); isb_s(&b, "Dynamic"); isb_i(&b, (long long)tb->num_rows);
                    isb_i(&b, tb->num_rows ? (long long)tb->num_pages * SV_PAGE_SIZE / (long long)tb->num_rows : 0);
                    isb_i(&b, (long long)tb->num_pages * SV_PAGE_SIZE); isb_i(&b, 0); isb_i(&b, 0); isb_i(&b, 0);
                    long long ai = -1;
                    for (int c = 0; c < tb->num_columns; c++)
                        if (tb->columns[c].auto_increment) {
                            MemIndex* ix = mi_get(x, db->name, tb, c);
                            ai = (ix && ix->has_max ? ix->max_int : 0) + 1;
                            if (tb->auto_increment > 0 && ai < (long long)tb->auto_increment) ai = (long long)tb->auto_increment;
                        }
                    if (ai > 0) isb_i(&b, ai); else isb_null(&b);
                    isb_null(&b); isb_null(&b); isb_null(&b); isb_s(&b, is_collation(tb)); isb_null(&b); isb_s(&b, "");
                    isb_s(&b, tb->comment);
                    isb_end(&b);
                    break;
                }
                case 2:                                              // COLUMNS
                    for (int c = 0; c < tb->num_columns; c++) {
                        const Column* col = &tb->columns[c];
                        isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, tb->name); isb_s(&b, col->name); isb_i(&b, c + 1);
                        if ((col->default_val[0] || col->has_default) && strcasecmp(col->default_val, "NULL")) isb_s(&b, col->default_val); else isb_null(&b);
                        isb_s(&b, col->nullable && !col->primary_key ? "YES" : "NO");
                        is_datatype(col, buf2, sizeof(buf2)); isb_s(&b, buf2);
                        long long cm = is_char_max(col);
                        if (cm >= 0) { isb_i(&b, cm); isb_i(&b, cm * 4 > 4294967295LL ? 4294967295LL : cm * 4); } else { isb_null(&b); isb_null(&b); }
                        if (col->type == SV_TYPE_INT) { isb_i(&b, 10); isb_i(&b, 0); }
                        else if (col->type == SV_TYPE_BIGINT) { isb_i(&b, 19); isb_i(&b, 0); }
                        else if (col->type == SV_TYPE_FLOAT) { isb_i(&b, 22); isb_null(&b); }
                        else if (col->type == SV_TYPE_DECIMAL) { isb_i(&b, col->size > 0 ? col->size : 65); isb_i(&b, col->decimal_prec > 0 ? col->decimal_prec - 1 : 30); }
                        else if (col->type == SV_TYPE_BOOL) { isb_i(&b, 3); isb_i(&b, 0); }
                        else { isb_null(&b); isb_null(&b); }
                        if (col->type == SV_TYPE_DATETIME) isb_i(&b, 0); else isb_null(&b);
                        if (is_text_col(col)) { isb_s(&b, "utf8mb4"); isb_s(&b, is_col_collation(tb, col)); } else { isb_null(&b); isb_null(&b); }
                        is_coltype(col, buf, sizeof(buf)); isb_s(&b, buf);
                        isb_s(&b, is_colkey(tb, c));
                        bool gen = (col->flags & COLF_GENERATED) != 0;
                        snprintf(buf, sizeof(buf), "%s%s%s%s", col->auto_increment ? "auto_increment" : "",
                                 default_is_now(col->default_val) ? "DEFAULT_GENERATED" : "",
                                 (col->flags & COLF_ON_UPDATE_NOW) ? (default_is_now(col->default_val) ? " on update CURRENT_TIMESTAMP" : "on update CURRENT_TIMESTAMP") : "",
                                 gen ? "STORED GENERATED" : "");
                        isb_s(&b, buf);
                        isb_s(&b, "select,insert,update,references"); isb_s(&b, gen ? "" : col->comment); isb_s(&b, gen ? col->comment : ""); isb_null(&b);
                        isb_end(&b);
                    }
                    break;
                case 3: {                                            // STATISTICS
                    // chiave primaria e UNIQUE (vincoli), poi gli indici
                    for (int k = 0; k < tb->num_constraints; k++) {
                        const Constraint* c = &tb->constraints[k];
                        if (c->kind != CONS_PRIMARY && c->kind != CONS_UNIQUE) continue;
                        for (int j = 0; j < c->ncols; j++) {
                            const Column* col = &tb->columns[c->cols[j]];
                            isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, tb->name); isb_i(&b, 0); isb_s(&b, sch);
                            isb_s(&b, c->kind == CONS_PRIMARY ? "PRIMARY" : c->name); isb_i(&b, j + 1); isb_s(&b, col->name);
                            isb_s(&b, "A"); isb_i(&b, (long long)tb->num_rows); isb_null(&b); isb_null(&b);
                            isb_s(&b, col->nullable && c->kind != CONS_PRIMARY ? "YES" : ""); isb_s(&b, "BTREE");
                            isb_s(&b, ""); isb_s(&b, ""); isb_s(&b, "YES"); isb_null(&b);
                            isb_end(&b);
                        }
                    }
                    for (int i = 0; i < tb->num_indexes; i++) {
                        const Index* ix = &tb->indexes[i];
                        int nc = ix->ncols > 0 ? ix->ncols : 1;
                        for (int j = 0; j < nc; j++) {
                            const char* cn = ix->ncols > 0 ? ix->cols[j] : ix->col_name;
                            int ci = catalog_get_col_index(tb, cn);
                            isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, tb->name); isb_i(&b, ix->unique ? 0 : 1); isb_s(&b, sch);
                            isb_s(&b, ix->name); isb_i(&b, j + 1); isb_s(&b, cn); isb_s(&b, ix->fulltext ? NULL : "A");
                            isb_i(&b, (long long)tb->num_rows); isb_null(&b); isb_null(&b);
                            isb_s(&b, ci >= 0 && tb->columns[ci].nullable ? "YES" : ""); isb_s(&b, ix->fulltext ? "FULLTEXT" : "BTREE");
                            isb_s(&b, ""); isb_s(&b, ""); isb_s(&b, "YES"); isb_null(&b);
                            isb_end(&b);
                        }
                    }
                    break;
                }
                case 4:                                              // TABLE_CONSTRAINTS
                    for (int k = 0; k < tb->num_constraints; k++) {
                        const Constraint* c = &tb->constraints[k];
                        const char* ty = c->kind == CONS_PRIMARY ? "PRIMARY KEY" : c->kind == CONS_UNIQUE ? "UNIQUE" :
                                         c->kind == CONS_FOREIGN ? "FOREIGN KEY" : "CHECK";
                        isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, c->kind == CONS_PRIMARY ? "PRIMARY" : c->name);
                        isb_s(&b, sch); isb_s(&b, tb->name); isb_s(&b, ty); isb_s(&b, "YES"); isb_end(&b);
                    }
                    break;
                case 5:                                              // KEY_COLUMN_USAGE
                    for (int k = 0; k < tb->num_constraints; k++) {
                        const Constraint* c = &tb->constraints[k];
                        if (c->kind == CONS_CHECK) continue;
                        for (int j = 0; j < c->ncols; j++) {
                            isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, c->kind == CONS_PRIMARY ? "PRIMARY" : c->name);
                            isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, tb->name); isb_s(&b, tb->columns[c->cols[j]].name); isb_i(&b, j + 1);
                            if (c->kind == CONS_FOREIGN) {
                                isb_i(&b, j + 1); isb_s(&b, sch); isb_s(&b, c->ref_table);
                                const char* rc = c->ref_cols[j];
                                if (!rc[0]) {                       // la chiave primaria della tabella riferita
                                    TableSchema* rt = catalog_get_table(cat, sch, c->ref_table);
                                    for (int q = 0; rt && q < rt->num_constraints && !rc[0]; q++)
                                        if (rt->constraints[q].kind == CONS_PRIMARY && j < rt->constraints[q].ncols) rc = rt->columns[rt->constraints[q].cols[j]].name;
                                }
                                isb_s(&b, rc);
                            } else { isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); }
                            isb_end(&b);
                        }
                    }
                    break;
                case 6:                                              // REFERENTIAL_CONSTRAINTS
                    for (int k = 0; k < tb->num_constraints; k++) {
                        const Constraint* c = &tb->constraints[k];
                        if (c->kind != CONS_FOREIGN) continue;
                        isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, c->name); isb_s(&b, "def"); isb_s(&b, sch);
                        isb_s(&b, "PRIMARY"); isb_s(&b, "NONE"); isb_s(&b, is_fk_rule(c->on_update)); isb_s(&b, is_fk_rule(c->on_delete));
                        isb_s(&b, tb->name); isb_s(&b, c->ref_table); isb_end(&b);
                    }
                    break;
                case 7:                                              // CHECK_CONSTRAINTS
                    for (int k = 0; k < tb->num_constraints; k++) {
                        const Constraint* c = &tb->constraints[k];
                        if (c->kind != CONS_CHECK) continue;
                        isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, c->name); isb_s(&b, c->check); isb_end(&b);
                    }
                    break;
                default: break;
            }
        }
        if (ti == 1)                                                 // TABLES: anche le viste
            for (int v = 0; v < db->num_views && b.ok; v++) {
                if (!priv_any(x, sch, db->views[v].name)) continue;
                isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, db->views[v].name); isb_s(&b, "VIEW");
                isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b);
                isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b); isb_null(&b);
                isb_s(&b, "VIEW"); isb_end(&b);
            }
        if (ti == 8)                                                 // VIEWS
            for (int v = 0; v < db->num_views && b.ok; v++) {
                if (!priv_any(x, sch, db->views[v].name)) continue;
                isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, db->views[v].name); isb_s(&b, db->views[v].definition);
                isb_s(&b, "NONE"); isb_s(&b, "NO"); isb_s(&b, "root@localhost"); isb_s(&b, "DEFINER"); isb_s(&b, "utf8mb4");
                isb_s(&b, "utf8mb4_general_ci"); isb_end(&b);
            }
        if (ti == 9)                                                 // TRIGGERS
            for (int g = 0; g < db->num_triggers && b.ok; g++) {
                const TriggerSchema* tg = &db->triggers[g];
                if (!priv_any(x, sch, tg->table_name)) continue;
                isb_row(&b); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, tg->name); isb_s(&b, tg->event); isb_s(&b, "def");
                isb_s(&b, sch); isb_s(&b, tg->table_name); isb_i(&b, g + 1); isb_null(&b); isb_s(&b, tg->body); isb_s(&b, "ROW");
                isb_s(&b, tg->before ? "BEFORE" : "AFTER"); isb_null(&b); isb_s(&b, ""); isb_s(&b, "root@localhost"); isb_end(&b);
            }
        if (ti == 10)                                                // ROUTINES
            for (int r = 0; r < db->num_routines && b.ok; r++) {
                const RoutineSchema* rt = &db->routines[r];
                isb_row(&b); isb_s(&b, rt->name); isb_s(&b, "def"); isb_s(&b, sch); isb_s(&b, rt->name);
                isb_s(&b, rt->is_function ? "FUNCTION" : "PROCEDURE"); isb_s(&b, rt->is_function ? rt->return_type : "");
                isb_s(&b, "SQL"); isb_s(&b, rt->body); isb_s(&b, "NO"); isb_s(&b, "CONTAINS SQL"); isb_s(&b, "DEFINER");
                isb_null(&b); isb_null(&b); isb_s(&b, ""); isb_s(&b, ""); isb_s(&b, "root@localhost"); isb_end(&b);
            }
    }
    if (ti == 11) {
        isb_row(&b); isb_s(&b, "InnoDB"); isb_s(&b, "DEFAULT"); isb_s(&b, "SegVault storage engine (transactions, row data in pages, WAL)");
        isb_s(&b, "YES"); isb_s(&b, "NO"); isb_s(&b, "YES"); isb_end(&b);
    }
    if (ti == 12) {
        static const char* const CS[][3] = { { "utf8mb4", "utf8mb4_general_ci", "UTF-8 Unicode" }, { "utf8mb3", "utf8mb3_general_ci", "UTF-8 Unicode" },
                                             { "latin1", "latin1_swedish_ci", "cp1252 West European" }, { "ascii", "ascii_general_ci", "US ASCII" },
                                             { "binary", "binary", "Binary pseudo charset" } };
        static const int ML[] = { 4, 3, 1, 1, 1 };
        for (int i = 0; i < 5; i++) { isb_row(&b); isb_s(&b, CS[i][0]); isb_s(&b, CS[i][1]); isb_s(&b, CS[i][2]); isb_i(&b, ML[i]); isb_end(&b); }
    }
    if (ti == 13) {
        static const char* const CO[][2] = { { "utf8mb4_general_ci", "utf8mb4" }, { "utf8mb4_unicode_ci", "utf8mb4" }, { "utf8mb4_unicode_520_ci", "utf8mb4" },
                                             { "utf8mb4_0900_ai_ci", "utf8mb4" }, { "utf8mb4_bin", "utf8mb4" }, { "utf8mb3_general_ci", "utf8mb3" },
                                             { "latin1_swedish_ci", "latin1" }, { "ascii_general_ci", "ascii" }, { "binary", "binary" } };
        static const int ID[] = { 45, 224, 246, 255, 46, 33, 8, 11, 63 };
        for (int i = 0; i < 9; i++) {
            isb_row(&b); isb_s(&b, CO[i][0]); isb_s(&b, CO[i][1]); isb_i(&b, ID[i]);
            isb_s(&b, (i == 0 || i == 5 || i == 6 || i == 7 || i == 8) ? "Yes" : ""); isb_s(&b, "Yes"); isb_i(&b, 1); isb_s(&b, "PAD SPACE"); isb_end(&b);
        }
    }
    if (!strcmp(T[ti].name, "SEQUENCES"))
        for (int i = 0; i < g_nseq && b.ok; i++) {
            const Seq* q = &g_seq[i];
            if (g_see_db && !g_see_db(q->db)) continue;
            isb_row(&b); isb_s(&b, "def"); isb_s(&b, q->db); isb_s(&b, q->name); isb_s(&b, "bigint"); isb_i(&b, q->start);
            isb_i(&b, q->minv); isb_i(&b, q->maxv); isb_i(&b, q->inc); isb_s(&b, q->cycle ? "YES" : "NO");
            if (q->called) isb_i(&b, q->value); else isb_null(&b);
            isb_end(&b);
        }
    if (!strcmp(T[ti].name, "SESSION_VARIABLES") || !strcmp(T[ti].name, "GLOBAL_VARIABLES"))
        for (int i = 0; SYSVARS[i][0] && b.ok; i++) { isb_row(&b); isb_s(&b, SYSVARS[i][0]); isb_s(&b, SYSVARS[i][1]); isb_end(&b); }
    if (!b.ok) { snprintf(err, errn, "Out of memory while reading information_schema"); return false; }
    ts->num_rows = (uint64_t)s->n;
    s->loaded = true;
    return true;
}

static bool source_open(ExecCtx* x, Database* db, Source* s, const char* table, const char* alias, ASTNode* derived,
                        int depth, const CteScope* scope, char* err, size_t errn, const char* tdb) {
    memset(s, 0, sizeof(*s));
    if (tdb && tdb[0] && !derived) {                       // db.tabella: quel database (anche diverso dal corrente)
        db = catalog_get_db(x->catalog, tdb);
        if (!db) { snprintf(err, errn, "Unknown database '%.63s'", tdb); return false; }
        snprintf(s->db, sizeof(s->db), "%s", db->name);
    }
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
                    TableSchema* ts = schema_dup(w->ts, 0);
                    if (!ts) { snprintf(err, errn, "Out of memory"); return false; }
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
    if (g_infoschema) return infoschema_open(x, s, table, err, errn);
    if (!db) { snprintf(err, errn, "No database selected"); return false; }
    s->ts = catalog_get_table(x->catalog, db->name, table);
    if (s->ts) {
        if (!executor_priv_ok(x, db->name, s->ts->name, AUTH_SELECT, err, errn)) { s->ts = NULL; return false; }
        return true;
    }

    ViewSchema* view = catalog_get_view(x->catalog, db->name, table);
    if (!view) { snprintf(err, errn, "Table or view '%.64s' not found", table); return false; }
    if (!executor_priv_ok(x, db->name, view->name, AUTH_SELECT, err, errn)) return false;
    TokenList* tk = lexer_tokenize(view->definition);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    if (!pr || !pr->success || pr->count != 1 || pr->statements[0]->type != NODE_SELECT) {
        snprintf(err, errn, "View '%.64s' has an invalid definition", table);
        parse_result_free(pr);
        lexer_free(tk);
        return false;
    }
    x->priv_off++;                                  // il corpo: con i permessi controllati a CREATE VIEW
    bool ok = source_materialize(x, s, pr->statements[0], depth, NULL, "View", err, errn);
    x->priv_off--;
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
static bool expr_has_udf(const ASTExpr* e) {
    if (!e) return false;
    if (e->type == EXPR_FUNC && e->subkind == SUBK_UDF) return true;
    if (expr_has_udf(e->left) || expr_has_udf(e->right)) return true;
    for (int i = 0; i < e->nargs; i++) if (expr_has_udf(e->args[i])) return true;
    return false;
}

static void expr_refs(const ASTExpr* e, uint64_t* mask, bool* outer, bool* opaque) {
    if (!e) return;
    switch (e->type) {
        case EXPR_COLUMN:
            if (e->bound < 0) *opaque = true;
            else if (e->rt_const) {}                          // variabile di routine: una costante
            else if (e->outer > 0) *outer = true;
            else if (e->bound > 0) *mask |= (uint64_t)1 << (BOUND_SRC(e->bound));   // fino a 64 sorgenti
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

static int popcount_u(uint64_t m) { int c = 0; while (m) { c += (int)(m & 1u); m >>= 1; } return c; }
static int highest_bit(uint64_t m) { int h = -1; for (int i = 0; m; i++, m >>= 1) if (m & 1u) h = i; return h; }

// "colonna di s = espressione senza colonne di questa query" -> ricerca per chiave
static void find_probe(SelectPlan* p, int s, ExprVec* conds, bool dyn) {
    if (p->probe_col[s] >= 0) return;
    for (int i = 0; i < conds->n; i++) {
        ASTExpr* w = conds->v[i];
        if (w->type != EXPR_BINARY || w->op != OP_EQ) continue;
        for (int side = 0; side < 2; side++) {
            ASTExpr* col = side ? w->right : w->left;
            ASTExpr* key = side ? w->left : w->right;
            if (col->type != EXPR_COLUMN || col->bound <= 0 || col->outer || BOUND_SRC(col->bound) != s) continue;
            uint64_t m = 0; bool o = false, op = false;
            expr_refs(key, &m, &o, &op);
            if (m || op || o != dyn) continue;
            int ci = BOUND_COL(col->bound);
            DataType t = p->src[s].ts->columns[ci].type;
            if (!is_int_type(t) && !is_text_type(t)) continue;
            p->probe_col[s] = ci;
            p->probe_key[s] = key;
            p->probe_dyn[s] = dyn;
            return;
        }
    }
}

// "colonna indicizzata di s  <, <=, >, >=  espressione costante" (BETWEEN è già diventato
// due confronti): estremi per una scansione dell'indice ordinato
static void find_range(SelectPlan* p, int s, ExprVec* conds) {
    const TableSchema* ts = p->src[s].ts;
    for (int i = 0; i < conds->n; i++) {
        ASTExpr* w = conds->v[i];
        if (w->type != EXPR_BINARY || (w->op != OP_LT && w->op != OP_LE && w->op != OP_GT && w->op != OP_GE)) continue;
        for (int side = 0; side < 2; side++) {
            ASTExpr* col = side ? w->right : w->left;
            ASTExpr* key = side ? w->left : w->right;
            if (col->type != EXPR_COLUMN || col->bound <= 0 || col->outer || BOUND_SRC(col->bound) != s) continue;
            uint64_t m = 0; bool o = false, op = false;
            expr_refs(key, &m, &o, &op);
            if (m || o || op) continue;
            int ci = BOUND_COL(col->bound);
            if (p->range_col[s] >= 0 && p->range_col[s] != ci) continue;
            DataType t = ts->columns[ci].type;
            if ((!is_int_type(t) && !is_text_type(t)) || !col_is_indexed(ts, ci)) continue;
            // colonna < chiave (o chiave > colonna): estremo superiore
            bool upper = (w->op == OP_LT || w->op == OP_LE) != (side == 1);
            ASTExpr** slot = upper ? &p->range_hi[s] : &p->range_lo[s];
            if (*slot) continue;
            *slot = key;
            p->range_col[s] = ci;
        }
    }
}

// valuta gli estremi dell'intervallo e prende i RID dall'indice; false = indice non utilizzabile
// (tipo dell'estremo non confrontabile con quello della colonna, memoria, lettura annullata)
static bool range_rids(SelectPlan* p, int s, EvalCtx* ec, RID** rids, int* n) {
    Source* S = &p->src[s];
    Database* db = S->db[0] ? catalog_get_db(p->x->catalog, S->db) : catalog_current_db(p->x->catalog);
    int ci = p->range_col[s];
    if (!db || ci < 0) return false;
    bool numeric = is_int_type(S->ts->columns[ci].type);
    EvalVal lo, hi;
    ASTExpr* bx[2] = { p->range_lo[s], p->range_hi[s] };
    EvalVal* bv[2] = { &lo, &hi };
    char keep[2][EV_STR_MAX + 1];
    for (int k = 0; k < 2; k++) {
        if (!bx[k]) continue;
        eval_expr(bx[k], ec, bv[k]);
        if (bv[k]->null) { *rids = NULL; *n = 0; return true; }       // confronto con NULL: nessuna riga
        if (numeric ? !bv[k]->is_num : bv[k]->is_num) return false;
        if (!bv[k]->is_num) {                                          // il testo deve sopravvivere alla prossima eval
            const char* bt = ev_s(bv[k]);
            if (key_is_long(bt, strlen(bt))) return false;               // chiave in forma lunga: niente intervallo
            snprintf(keep[k], sizeof(keep[k]), "%s", bt);
            if (col_is_ci(S->ts, ci)) u8_ci_fold(keep[k], keep[k]);   // stessa piegatura delle chiavi dell'indice
            bv[k]->str = keep[k];
        }
    }
    MemIndex* ix = mi_get(p->x, db->name, S->ts, ci);
    return ix && mi_range(ix, numeric, bx[0] ? &lo : NULL, bx[1] ? &hi : NULL, rids, n);
}

// conviene leggere le righe con l'indice? Ogni riga trovata costa un accesso alla sua
// pagina; la scansione legge tutte le pagine in sequenza. Sopra 1/4 della tabella vince la scansione.
static bool range_worth(const Source* S, int n) {
    return (uint64_t)n * 4 <= S->ts->num_rows + 4;
}

// chiave di ricerca dal valore calcolato; false se il tipo non è confrontabile esattamente
static bool probe_text_ci(const EvalVal* v, DataType col, char* buf, size_t n, bool ci);
static bool probe_text_ci(const EvalVal* v, DataType col, char* buf, size_t n, bool ci) {
    if (v->null) return false;
    if (is_int_type(col)) {
        // colonna intera: anche 1.0 e '1' (MySQL: WHERE ID = '1') trovano la chiave 1
        long long x;
        double d;
        if (v->is_int) x = v->inum;
        else if (v->is_num) { if (v->num != floor(v->num) || fabs(v->num) > 9e18) return false; x = (long long)v->num; }
        else if (text_is_number(ev_s(v), &d) && d == floor(d) && fabs(d) <= 9e18) x = (long long)d;
        else return false;
        if (col == SV_TYPE_INT && (x < INT32_MIN || x > INT32_MAX)) return false;
        snprintf(buf, n, "%lld", x);
        return true;
    }
    if (is_text_type(col) && !v->is_num) {
        char* f = ci ? ci_fold(ev_s(v)) : NULL;
        key_text(buf, n, f ? f : ev_s(v));
        free(f);
        return true;
    }
    return false;
}

static SelectPlan* plan_prepare_ex(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                   char* err, size_t errn, bool arm);

static SelectPlan* plan_prepare(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                char* err, size_t errn) {
    return plan_prepare_ex(x, n, depth, outer, ctes, err, errn, false);
}
static bool lateral_open(SelectPlan* p, int k, ASTNode* sub, const TableFunc* tf, const char* alias, BindCtx* outer,
                         const CteScope* scope, char* err, size_t errn);

// sorgente laterale k: (subquery) o funzione-tabella, preparata con le sorgenti 0..k-1 visibili
static bool lateral_open(SelectPlan* p, int k, ASTNode* sub, const TableFunc* tf, const char* alias, BindCtx* outer,
                         const CteScope* scope, char* err, size_t errn) {
    char al[SV_MAX_NAME_LEN];
    snprintf(al, sizeof(al), "%s", alias && alias[0] ? alias : tf ? "table_function" : "lateral");
    Source* s = &p->src[k];
    memset(s, 0, sizeof(*s));
    memcpy(s->name, al, sizeof(s->name));
    memcpy(s->table, al, sizeof(s->table));
    s->lateral = true;
    s->is_view = true;
    s->loaded = true;
    s->tf = tf;
    TableSchema* ts = schema_new(SV_MAX_COLUMNS);     // le colonne si sanno solo dopo il piano della sottoquery
    BindCtx* lb = sv_calloc(1, sizeof(BindCtx));
    if (!ts || !lb) { free(ts); free(lb); snprintf(err, errn, "Out of memory"); return false; }
    s->own_ts = s->ts = ts;
    s->lat_bind = lb;
    snprintf(ts->name, sizeof(ts->name), "%s", s->name);
    lb->src = p->src; lb->nsrc = k; lb->mode = BIND_ROW; lb->x = p->x; lb->outer = outer; lb->clause = "LATERAL";
    lb->depth = p->depth + 1; lb->ctes = scope;
    if (sub) {
        char perr[300] = "";
        s->lat_plan = plan_prepare(p->x, sub, p->depth + 1, lb, scope, perr, sizeof(perr));
        if (!s->lat_plan) { snprintf(err, errn, "LATERAL '%.64s': %.200s", s->name, perr); return false; }
        ts->num_columns = s->lat_plan->nout;
        for (int i = 0; i < s->lat_plan->nout; i++) {
            const char* cn = s->lat_plan->out[i].name;
            const char* dot = strrchr(cn, '.');
            snprintf(ts->columns[i].name, sizeof(ts->columns[i].name), "%.63s", dot ? dot + 1 : cn);
            int st = s->lat_plan->out[i].src_type;
            ts->columns[i].type = st >= 0 ? (DataType)st : SV_TYPE_TEXT;   // le altre: decise dai primi valori
            ts->columns[i].nullable = true;
        }
        s->lat_typed = false;
        return true;
    }
    for (int i = 0; i < tf->nargs; i++) bind_expr(tf->args[i], lb);
    if (lb->err[0]) { snprintf(err, errn, "%.250s", lb->err); return false; }
    if (!strcmp(tf->func, "JSON_TABLE")) {
        if (tf->nout > SV_MAX_COLUMNS) { snprintf(err, errn, "Too many JSON_TABLE columns"); return false; }
        for (int i = 0; i < tf->ncols; i++) {
            const TFCol* tc = &tf->cols[i];
            if (tc->kind == TFC_NESTED) continue;
            Column* c = &ts->columns[ts->num_columns++];
            snprintf(c->name, sizeof(c->name), "%s", tc->out < tf->ncolnames ? tf->colnames[tc->out] : tc->name);
            if (tc->kind == TFC_PATH && !known_type(tc->def.type)) { snprintf(err, errn, "Unknown type '%.32s'", tc->def.type); return false; }
            c->type = tc->kind == TFC_PATH ? parse_type(tc->def.type) : SV_TYPE_BIGINT;
            c->size = tc->def.size;
            if (c->type == SV_TYPE_DECIMAL) c->decimal_prec = tc->def.has_scale ? tc->def.scale + 1 : 0;
            c->nullable = true;
        }
    } else {                                                   // GENERATE_SERIES: una colonna
        Column* c = &ts->columns[ts->num_columns++];
        snprintf(c->name, sizeof(c->name), "%s", tf->ncolnames ? tf->colnames[0] : "generate_series");
        c->type = SV_TYPE_BIGINT;
        c->nullable = true;
    }
    s->lat_typed = true;
    return true;
}

// arm = true: solo questo ramo di UNION / INTERSECT / EXCEPT (i successivi li prepara il primo)
// ── ordine dei join (P1.13) ─────────────────────────────────────
// Un join solo INNER / CROSS fra 3-8 tabelle vere (niente USING / NATURAL / LATERAL / tabelle derivate / CTE)
// si può eseguire in qualunque ordine: le condizioni ON passano nel WHERE (equivalente per un join interno) e
// le tabelle si mettono nell'ordine che il costo stimato preferisce. Greedy: prima la tabella con meno righe
// dopo i suoi filtri, poi ogni volta quella unita da un'uguaglianza che dà il risultato intermedio più piccolo
// (righe dalle statistiche di ANALYZE, altrimenti num_rows e le selettività di default). Lo stesso join scritto
// in un altro ordine finisce nello stesso ordine. SELECT * si espande prima nell'ordine scritto: le colonne del
// risultato (ordine e nomi "tabella.colonna") non cambiano.

typedef struct { const char* name; const char* table; const char* db; TableSchema* ts; } JoSrc;
static bool lit_number(const ASTExpr* e, double* v);

// sorgente di una colonna scritta nel testo (per alias, nome della tabella o, senza, unica tabella che la ha)
static int jo_col_src(const JoSrc* S, int n, const ASTExpr* e) {
    if (!e || e->type != EXPR_COLUMN) return -1;
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (e->table_name[0] && strcasecmp(e->table_name, S[i].name) && strcasecmp(e->table_name, S[i].table)) continue;
        if (catalog_get_col_index(S[i].ts, e->col_name) < 0) continue;
        if (found >= 0) return -2;                        // ambigua
        found = i;
    }
    return found;
}

// sorgenti usate da un'espressione (bit), -1 se c'è qualcosa che non si sa attribuire (subquery, colonna esterna)
static int64_t jo_refs(const JoSrc* S, int n, const ASTExpr* e, int d) {
    if (!e) return 0;
    if (d > 64 || e->type == EXPR_SUBQUERY || e->type == EXPR_WINDOW || e->type == EXPR_AGG) return -1;
    int64_t m = 0;
    if (e->type == EXPR_COLUMN) { int k = jo_col_src(S, n, e); return k < 0 ? -1 : (int64_t)1 << k; }
    int64_t a = jo_refs(S, n, e->left, d + 1), b = jo_refs(S, n, e->right, d + 1);
    if (a < 0 || b < 0) return -1;
    m = a | b;
    for (int i = 0; i < e->nargs; i++) { int64_t c = jo_refs(S, n, e->args[i], d + 1); if (c < 0) return -1; m |= c; }
    return m;
}

static const ColStats* jo_stats(const JoSrc* S, int n, const ASTExpr* e) {
    int k = jo_col_src(S, n, e);
    return k >= 0 ? stats_col(S[k].db, S[k].table, e->col_name) : NULL;
}

// selettività di una condizione su una sola tabella
static double jo_sel(const JoSrc* S, int n, const ASTExpr* c) {
    if (!c) return 1;
    if (c->type == EXPR_BINARY && c->op == OP_AND) return jo_sel(S, n, c->left) * jo_sel(S, n, c->right);
    if (c->type == EXPR_BINARY && c->op == OP_OR) { double a = jo_sel(S, n, c->left), b = jo_sel(S, n, c->right); return a + b - a * b; }
    if (c->type != EXPR_BINARY) return 0.5;
    const ColStats* cs = jo_stats(S, n, c->left);
    const ASTExpr* other = c->right;
    int op = c->op;
    if (!cs) {
        cs = jo_stats(S, n, c->right);
        other = c->left;
        if (op == OP_LT) op = OP_GT; else if (op == OP_GT) op = OP_LT; else if (op == OP_LE) op = OP_GE; else if (op == OP_GE) op = OP_LE;
    }
    double v = 0;
    bool have = lit_number(other, &v);
    switch (op) {
        case OP_EQ: return cs ? stats_sel_eq(cs, have, v) : 0.1;
        case OP_NEQ: return 0.9;
        case OP_LT: return cs && have ? stats_sel_range(cs, '<', v) : 1.0 / 3;
        case OP_LE: return cs && have ? stats_sel_range(cs, 'l', v) : 1.0 / 3;
        case OP_GT: return cs && have ? stats_sel_range(cs, '>', v) : 1.0 / 3;
        case OP_GE: return cs && have ? stats_sel_range(cs, 'g', v) : 1.0 / 3;
        case OP_LIKE: return 0.1;
        default: return 0.5;
    }
}

static ASTExpr* jo_and(ASTExpr* a, ASTExpr* b) {
    if (!a) return b;
    if (!b) return a;
    ASTExpr* e = sv_calloc(1, sizeof(ASTExpr));
    e->type = EXPR_BINARY;
    e->op = OP_AND;
    e->left = a;
    e->right = b;
    e->depth = 1 + (a->depth > b->depth ? a->depth : b->depth);
    return e;
}

static void jo_conj(ASTExpr* e, ASTExpr** out, int* n, int max) {
    if (!e) return;
    if (e->type == EXPR_BINARY && e->op == OP_AND) { jo_conj(e->left, out, n, max); jo_conj(e->right, out, n, max); return; }
    if (*n < max) out[(*n)++] = e;
}

#define JO_MAX 16                                // tabelle che il riordino considera (P3.12: erano 8)
static void join_reorder(ExecCtx* x, ASTNode* n, const CteScope* scope) {
    int nj = n->select_stmt.num_joins, ns = nj + 1;
    if (nj < 2 || ns > JO_MAX || scope || n->select_stmt.set_next || n->select_stmt.from_sub || n->select_stmt.from_tf ||
        !n->select_stmt.table_name[0]) return;
    Database* cur = catalog_current_db(x->catalog);
    JoSrc S[JO_MAX];
    for (int i = 0; i < ns; i++) {
        const char *tn, *al, *tdb;
        if (i == 0) { tn = n->select_stmt.table_name; al = n->select_stmt.table_alias; tdb = n->select_stmt.table_db; }
        else {
            ASTJoin* j = &n->select_stmt.joins[i - 1];
            if ((j->type != JOIN_INNER && j->type != JOIN_CROSS) || j->natural || j->nusing || j->lateral || j->tf || j->sub) return;
            tn = j->table_name; al = j->alias; tdb = j->table_db;
        }
        const char* db = tdb[0] ? tdb : cur ? cur->name : NULL;
        TableSchema* ts = db ? catalog_get_table(x->catalog, db, tn) : NULL;
        if (!ts) return;                                   // vista o tabella che non c'è: come scritto
        S[i] = (JoSrc){ al[0] ? al : tn, tn, db, ts };
    }
    // condizioni: WHERE + tutte le ON
    ASTExpr* conj[128];
    int nc = 0;
    jo_conj(n->select_stmt.where, conj, &nc, 128);
    for (int j = 0; j < nj; j++) jo_conj(n->select_stmt.joins[j].condition, conj, &nc, 128);
    if (nc >= 128) return;
    int64_t refs[128];
    for (int i = 0; i < nc; i++) { refs[i] = jo_refs(S, ns, conj[i], 0); }
    // righe di ogni tabella dopo le sue condizioni
    double base[JO_MAX];
    for (int i = 0; i < ns; i++) {
        const TableStats* st = stats_get(S[i].db, S[i].table);
        base[i] = st ? st->rows : (double)S[i].ts->num_rows;
        if (base[i] < 1) base[i] = 1;
        for (int c = 0; c < nc; c++) if (refs[c] == ((int64_t)1 << i)) base[i] *= jo_sel(S, ns, conj[c]);
        if (base[i] < 1) base[i] = 1;
    }
    // costo di un ordine: somma dei risultati intermedi
    #define JO_COST(ord, outc) do { \
        int64_t in = (int64_t)1 << (ord)[0]; double r = base[(ord)[0]], tot = 0; \
        for (int k = 1; k < ns; k++) { \
            int t = (ord)[k]; double est = r * base[t]; \
            for (int c = 0; c < nc; c++) { \
                if (refs[c] <= 0 || !(refs[c] & ((int64_t)1 << t)) || (refs[c] & ~(in | ((int64_t)1 << t)))) continue; \
                if (refs[c] == ((int64_t)1 << t)) continue; \
                const ASTExpr* e = conj[c]; double sel = 0.5; \
                if (e->type == EXPR_BINARY && e->op == OP_EQ) { \
                    const ColStats* a = jo_stats(S, ns, e->left), *b = jo_stats(S, ns, e->right); \
                    int sa = jo_col_src(S, ns, e->left), sb = jo_col_src(S, ns, e->right); \
                    double da = a ? a->ndv : sa >= 0 ? base[sa] : 0, db2 = b ? b->ndv : sb >= 0 ? base[sb] : 0; \
                    double d = da > db2 ? da : db2; sel = d > 0 ? 1 / d : 1; \
                } \
                est *= sel; \
            } \
            if (est < 1) est = 1; \
            r = est; tot += r; in |= (int64_t)1 << t; \
        } \
        (outc) = tot; \
    } while (0)
    int orig[JO_MAX], best[JO_MAX];
    for (int i = 0; i < ns; i++) orig[i] = i;
    // greedy: la più piccola, poi la prossima che (unita) dà il risultato più piccolo; parità: nome
    bool used[JO_MAX] = {0};
    int first = 0;
    for (int i = 1; i < ns; i++)
        if (base[i] < base[first] || (base[i] == base[first] && strcasecmp(S[i].name, S[first].name) < 0)) first = i;
    best[0] = first; used[first] = true;
    for (int k = 1; k < ns; k++) {
        int pick = -1;
        double pick_cost = 0;
        for (int t = 0; t < ns; t++) {
            if (used[t]) continue;
            int trial[JO_MAX];
            memcpy(trial, best, sizeof(int) * (size_t)k);
            trial[k] = t;
            // costo del prefisso con t aggiunta (le altre ancora fuori)
            int64_t in = 0; double r = 0, tot = 0;
            for (int q = 0; q <= k; q++) {
                int u = trial[q];
                if (q == 0) { r = base[u]; in = (int64_t)1 << u; continue; }
                double est = r * base[u];
                bool joined = false;
                for (int c = 0; c < nc; c++) {
                    if (refs[c] <= 0 || refs[c] == ((int64_t)1 << u) || !(refs[c] & ((int64_t)1 << u)) || (refs[c] & ~(in | ((int64_t)1 << u)))) continue;
                    const ASTExpr* e = conj[c];
                    double sel = 0.5;
                    if (e->type == EXPR_BINARY && e->op == OP_EQ) {
                        const ColStats* a = jo_stats(S, ns, e->left), *b = jo_stats(S, ns, e->right);
                        int sa = jo_col_src(S, ns, e->left), sb = jo_col_src(S, ns, e->right);
                        double da = a ? a->ndv : sa >= 0 ? base[sa] : 0, db2 = b ? b->ndv : sb >= 0 ? base[sb] : 0;
                        double d = da > db2 ? da : db2;
                        sel = d > 0 ? 1 / d : 1;
                        joined = true;
                    }
                    est *= sel;
                }
                if (!joined && q == k) est *= 1e6;         // prodotto cartesiano: solo se non c'è altro
                if (est < 1) est = 1;
                r = est; tot += r; in |= (int64_t)1 << u;
            }
            if (pick < 0 || tot < pick_cost || (tot == pick_cost && strcasecmp(S[t].name, S[pick].name) < 0)) { pick = t; pick_cost = tot; }
        }
        best[k] = pick;
        used[pick] = true;
    }
    double c_orig, c_best;
    JO_COST(orig, c_orig);
    JO_COST(best, c_best);
    #undef JO_COST
    bool same = true;
    for (int i = 0; i < ns; i++) if (best[i] != i) same = false;
    if (same || c_best > c_orig) return;               // già nel miglior ordine (o il greedy non migliora)
    // SELECT *: espanso nell'ordine scritto (stesse colonne, stessi nomi "tabella.colonna")
    int nexp = 0;
    for (int i = 0; i < n->select_stmt.num_cols; i++) {
        ASTExpr* e = n->select_stmt.columns[i];
        if (e->type != EXPR_STAR) { nexp++; continue; }
        for (int k = 0; k < ns; k++)
            if (!e->table_name[0] || !strcasecmp(e->table_name, S[k].name) || !strcasecmp(e->table_name, S[k].table)) nexp += S[k].ts->num_columns;
    }
    // (P3.12 / bug: con cols[64] si saltava il riordino oltre 64 colonne, e la memcpy sotto scriveva oltre la fine
    // di select_stmt.columns, grande quanto l'elenco scritto — "*" = 8 posti: heap overflow trovato con ASan)
    if (nexp > SV_MAX_COLUMNS) return;
    ASTExpr** cols = sv_malloc(sizeof(ASTExpr*) * (size_t)(nexp ? nexp : 1));
    if (!cols) return;
    int nn = 0;
    for (int i = 0; i < n->select_stmt.num_cols; i++) {
        ASTExpr* e = n->select_stmt.columns[i];
        if (e->type != EXPR_STAR) { cols[nn++] = e; continue; }
        for (int k = 0; k < ns; k++) {
            if (e->table_name[0] && strcasecmp(e->table_name, S[k].name) && strcasecmp(e->table_name, S[k].table)) continue;
            for (int c = 0; c < S[k].ts->num_columns; c++) {
                ASTExpr* col = sv_calloc(1, sizeof(ASTExpr));
                col->type = EXPR_COLUMN;
                col->depth = 1;
                snprintf(col->table_name, sizeof(col->table_name), "%s", S[k].name);
                snprintf(col->col_name, sizeof(col->col_name), "%s", S[k].ts->columns[c].name);
                cols[nn++] = col;
            }
        }
        ast_expr_free(e);
    }
    free(n->select_stmt.columns);                      // la lista espansa prende il posto di quella scritta
    n->select_stmt.columns = cols;
    n->select_stmt.num_cols = nn;
    n->select_stmt.cap_cols = nexp ? nexp : 1;
    // le ON nel WHERE, le tabelle nel nuovo ordine (join interni senza condizione propria)
    ASTExpr* where = n->select_stmt.where;
    for (int j = 0; j < nj; j++) { where = jo_and(where, n->select_stmt.joins[j].condition); n->select_stmt.joins[j].condition = NULL; }
    n->select_stmt.where = where;
    char tn[JO_MAX][64], al[JO_MAX][64], tdb[JO_MAX][64];
    for (int i = 0; i < ns; i++) {
        const char* a = i == 0 ? n->select_stmt.table_alias : n->select_stmt.joins[i - 1].alias;
        const char* d = i == 0 ? n->select_stmt.table_db : n->select_stmt.joins[i - 1].table_db;
        snprintf(tn[i], 64, "%s", S[i].table);
        snprintf(al[i], 64, "%s", a);
        snprintf(tdb[i], 64, "%s", d);
    }
    for (int k = 0; k < ns; k++) {
        int i = best[k];
        if (k == 0) {
            snprintf(n->select_stmt.table_name, sizeof(n->select_stmt.table_name), "%s", tn[i]);
            snprintf(n->select_stmt.table_alias, sizeof(n->select_stmt.table_alias), "%s", al[i]);
            snprintf(n->select_stmt.table_db, sizeof(n->select_stmt.table_db), "%s", tdb[i]);
        } else {
            ASTJoin* j = &n->select_stmt.joins[k - 1];
            j->type = JOIN_INNER;
            j->condition = NULL;
            snprintf(j->table_name, sizeof(j->table_name), "%s", tn[i]);
            snprintf(j->alias, sizeof(j->alias), "%s", al[i]);
            snprintf(j->table_db, sizeof(j->table_db), "%s", tdb[i]);
        }
    }
}

static SelectPlan* plan_prepare_ex(ExecCtx* x, ASTNode* n, int depth, BindCtx* outer, const CteScope* ctes,
                                   char* err, size_t errn, bool arm) {
    if (!n || n->type != NODE_SELECT) { snprintf(err, errn, "Expected a SELECT"); return NULL; }
    if (depth > MAX_VIEW_DEPTH * 4) { snprintf(err, errn, "Query is nested too deeply"); return NULL; }
    SelectPlan* p = sv_calloc(1, sizeof(SelectPlan));
    if (!p) { snprintf(err, errn, "Out of memory"); return NULL; }
    p->x = x;
    p->n = n;
    p->depth = depth;
    p->scope.ctes = n->select_stmt.ctes;
    p->scope.n = n->select_stmt.num_ctes;
    p->scope.parent = ctes;
    Database* db = catalog_current_db(x->catalog);
    const CteScope* scope = p->scope.n ? &p->scope : ctes;
    p->compound = !arm && n->select_stmt.set_next != NULL;
    #define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); plan_free(p); return NULL; } while (0)

    if (!p->compound && !arm) join_reorder(x, n, scope);   // join interni: l'ordine che costa meno (P1.13)
    if (n->select_stmt.num_joins + 1 > MAX_SOURCES) FAIL("Too many tables in one SELECT (max %d)", MAX_SOURCES);
    if (!plan_alloc_src(p, n->select_stmt.num_joins + 1)) FAIL("Out of memory");

    // 1. sorgenti (SELECT senza FROM: nessuna, il risultato ha una riga)
    if (n->select_stmt.table_name[0]) {
        bool ok = n->select_stmt.from_tf
            ? lateral_open(p, p->nsrc, NULL, n->select_stmt.from_tf, n->select_stmt.table_alias, outer, scope, err, errn)
            : source_open(x, db, &p->src[p->nsrc], n->select_stmt.table_name, n->select_stmt.table_alias,
                          n->select_stmt.from_sub, depth, scope, err, errn, n->select_stmt.table_db);
        p->nsrc++;
        if (!ok) { plan_free(p); return NULL; }
    }
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        ASTJoin* jn = &n->select_stmt.joins[j];
        bool ok = jn->lateral ? lateral_open(p, p->nsrc, jn->sub, jn->tf, jn->alias, outer, scope, err, errn)
                              : source_open(x, db, &p->src[p->nsrc], jn->table_name, jn->alias, jn->sub, depth, scope, err, errn, jn->table_db);
        p->nsrc++;
        if (!ok) { plan_free(p); return NULL; }
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
        // puntatori ai nomi (schema / AST, stabili qui): prima copie da 64 byte, 64 KB di stack in una funzione
        // ricorsiva (CR-M1)
        const char* names[SV_MAX_COLUMNS];
        int nn = 0;
        if (jn->natural) {
            for (int c = 0; c < R->ts->num_columns && nn < SV_MAX_COLUMNS; c++)
                for (int q = 0; q < k; q++)
                    if (catalog_get_col_index(p->src[q].ts, R->ts->columns[c].name) >= 0) {
                        names[nn++] = R->ts->columns[c].name;
                        break;
                    }
        } else {
            for (int u = 0; u < jn->nusing && nn < SV_MAX_COLUMNS; u++) names[nn++] = jn->using_cols[u];
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
            if (!plan_using_set(p, k, rc, BOUND_MAKE(ls, lc))) FAIL("Out of memory");
            if (!jn->using_built) {
                ASTExpr* a = sv_calloc(1, sizeof(ASTExpr));
                ASTExpr* b2 = sv_calloc(1, sizeof(ASTExpr));
                ASTExpr* eq = sv_calloc(1, sizeof(ASTExpr));
                if (!a || !b2 || !eq) { free(a); free(b2); free(eq); ast_expr_free(cond); FAIL("Out of memory"); }
                a->type = b2->type = EXPR_COLUMN;
                snprintf(a->table_name, sizeof(a->table_name), "%s", p->src[ls].name);
                snprintf(a->col_name, sizeof(a->col_name), "%.63s", names[u]);
                snprintf(b2->table_name, sizeof(b2->table_name), "%s", R->name);
                snprintf(b2->col_name, sizeof(b2->col_name), "%.63s", names[u]);
                eq->type = EXPR_BINARY; eq->op = OP_EQ; eq->left = a; eq->right = b2; eq->depth = 1;
                if (!cond) cond = eq;
                else {
                    ASTExpr* and = sv_calloc(1, sizeof(ASTExpr));
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
                    if (!e->table_name[0] && plan_using_left(p, s, c)) continue;    // copia di destra di una colonna unita
                    if (p->nout >= SV_MAX_COLUMNS) FAIL("Too many columns in the result (max %d)", SV_MAX_COLUMNS);
                    ASTExpr* col = sv_calloc(1, sizeof(ASTExpr));
                    if (!col) FAIL("Out of memory");
                    col->type = EXPR_COLUMN;
                    snprintf(col->col_name, sizeof(col->col_name), "%s", sr->ts->columns[c].name);
                    col->bound = BOUND_MAKE(s, c);
                    src_need(sr, c);
                    // colonna unita con un RIGHT / FULL JOIN successivo: COALESCE(sinistra, destra)
                    int rk = -1, rcol = -1;
                    for (int q = s + 1; !e->table_name[0] && q < p->nsrc && rk < 0; q++)
                        for (int cc = 0; cc < p->src[q].ts->num_columns; cc++)
                            if (plan_using_left(p, q, cc) == BOUND_MAKE(s, c) &&
                                (n->select_stmt.joins[q - 1].type == JOIN_RIGHT || n->select_stmt.joins[q - 1].type == JOIN_FULL)) { rk = q; rcol = cc; break; }
                    if (rk >= 0) {
                        ASTExpr* other = sv_calloc(1, sizeof(ASTExpr));
                        ASTExpr* co = sv_calloc(1, sizeof(ASTExpr));
                        ASTExpr** args = sv_calloc(2, sizeof(ASTExpr*));
                        if (!other || !co || !args) { free(other); free(co); free(args); free(col); FAIL("Out of memory"); }
                        *other = *col;
                        other->bound = BOUND_MAKE(rk, rcol);
                        src_need(&p->src[rk], rcol);
                        co->type = EXPR_FUNC;
                        snprintf(co->func, sizeof(co->func), "COALESCE");
                        co->args = args; co->nargs = 2; args[0] = col; args[1] = other;
                        co->depth = 2;
                        snprintf(co->col_name, sizeof(co->col_name), "%s", col->col_name);
                        OutCol* oc = plan_out_new(p);
                        if (!oc) { ast_expr_free(co); FAIL("Too many columns in the result (max %d)", SV_MAX_COLUMNS); }
                        oc->e = co;
                        oc->owned = true;
                        oc->src_type = (int)sr->ts->columns[c].type;
                        snprintf(oc->name, sizeof(oc->name), "%s", col->col_name);
                        continue;
                    }
                    OutCol* oc = plan_out_new(p);
                    if (!oc) { ast_expr_free(col); FAIL("Too many columns in the result (max %d)", SV_MAX_COLUMNS); }
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
        OutCol* oc = plan_out_new(p);
        if (!oc) FAIL("Too many columns in the result (max %d)", SV_MAX_COLUMNS);
        oc->e = e;
        oc->owned = false;
        oc->src_type = -1;
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
        // SELECT DISTINCT: l'ordine deve dipendere solo dalle colonne restituite (come MySQL 5.7+ e
        // PostgreSQL); con un'altra espressione ogni riga distinta ne avrebbe più valori e l'ordine
        // sarebbe arbitrario
        for (int i = 0; n->select_stmt.distinct && !p->compound && !g_mysql_mode && i < n->select_stmt.num_order; i++) {
            ASTExpr* o = n->select_stmt.order_exprs[i];
            if (o->type == EXPR_INT || (o->type == EXPR_COLUMN && o->bound < 0)) continue;
            bool found = false;
            for (int k = 0; k < p->nout && !found; k++) found = expr_same(o, p->out[k].e);
            if (!found) {
                char t[128];
                ast_expr_format(o, t, sizeof(t));
                FAIL("ORDER BY %.100s must also appear in the SELECT list when DISTINCT is used", t);
            }
        }
    }
    for (int i = 0; i < p->nout; i++) {
        OutCol* oc = &p->out[i];
        if (oc->owned) continue;
        ASTExpr* e = oc->e;
        if (e->type == EXPR_COLUMN && e->bound > 0 && e->outer == 0) {
            int s = BOUND_SRC(e->bound), c = BOUND_COL(e->bound);
            oc->src_type = (int)p->src[s].ts->columns[c].type;
        }
        if (e->alias[0])                                                   snprintf(oc->name, sizeof(oc->name), "%s", e->alias);
        else if (e->type == EXPR_COLUMN && p->nsrc > 1 && e->outer == 0 && e->bound > 0)
            snprintf(oc->name, sizeof(oc->name), "%.64s.%.64s", p->src[BOUND_SRC(e->bound)].name, e->col_name);
        else if (e->type == EXPR_COLUMN || (e->type == EXPR_FUNC && e->col_name[0]))   // (colonna di USING in un FULL JOIN)
            snprintf(oc->name, sizeof(oc->name), "%s", e->col_name);
        else                                                               ast_expr_format(e, oc->name, sizeof(oc->name));
    }
    p->grouped = has_agg || n->select_stmt.num_group > 0 || n->select_stmt.having;

    // 4. dove applicare le condizioni (quelle che chiamano funzioni dell'utente per ultime: costano
    // un'esecuzione della funzione per riga, meglio dopo i filtri e i join che riducono le righe)
    ExprVec conj = {0};
    collect_conj(n->select_stmt.where, &conj);
    ExprVec costly = {0};
    for (int i = 0; i < conj.n; i++) {
        ASTExpr* c = conj.v[i];
        if (expr_has_udf(c)) { xv_push(&costly, c); continue; }
        uint64_t m = 0; bool o = false, op = false;
        expr_refs(c, &m, &o, &op);
        bool safe = !op && m != 0;
        for (int s = 0; safe && s < p->nsrc; s++) if ((m >> s & 1u) && src_nullable(p, s)) safe = false;
        bool lat = false;                                   // righe calcolate nel join: condizione dopo
        for (int s = 0; s < p->nsrc; s++) if ((m >> s & 1u) && p->src[s].lateral) lat = true;
        if (lat) { xv_push(&p->resid, c); continue; }
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
    for (int i = 0; i < costly.n; i++) xv_push(&p->resid, costly.v[i]);
    free(costly.v);
    free(conj.v);
    for (int k = 1; k < p->nsrc; k++) {
        ASTJoin* j = &n->select_stmt.joins[k - 1];
        if (j->type == JOIN_RIGHT || j->type == JOIN_FULL) { if (j->condition) xv_push(&p->jcond[k], j->condition); continue; }
        ExprVec on = {0};
        collect_conj(j->condition, &on);
        for (int i = 0; i < on.n; i++) {
            ASTExpr* c = on.v[i];
            uint64_t m = 0; bool o = false, op = false;
            expr_refs(c, &m, &o, &op);
            // condizione dell'ON solo sulla tabella unita: filtra le sue righe prima del join
            bool pre = !op && m == ((uint64_t)1 << k) && (j->type == JOIN_LEFT || !src_nullable(p, k)) && !p->src[k].lateral;
            xv_push(pre ? (o ? &p->pre_dyn[k] : &p->pre_static[k]) : &p->jcond[k], c);
        }
        free(on.v);
    }
    for (int s = 0; s < p->nsrc; s++) {
        if (p->src[s].is_view) continue;
        find_probe(p, s, &p->pre_dyn[s], true);
        find_probe(p, s, &p->pre_static[s], false);
        if (p->probe_col[s] < 0 || p->probe_dyn[s]) find_range(p, s, &p->pre_static[s]);
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
        if (n->select_stmt.num_order && !(p->corder = sv_malloc(sizeof(int) * (size_t)n->select_stmt.num_order))) FAIL("Out of memory");
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

// righe della scansione nella sorgente s (filtri di lettura applicati, testi non usati non copiati), al più
// scan_max (< 0 = tutte); *ended = scansione finita. false = memoria esaurita
static bool scan_rows(SelectPlan* p, int s, HeapScan* scan, long scan_max, EvalCtx* ec, Tuple** cur, int* cap, bool* ended) {
    Source* S = &p->src[s];
    ExprVec* f = &p->pre_static[s];
                // colonne che servono dopo la lettura: tutti i riferimenti meno quelli dei filtri di lettura
                uint8_t* used = sv_malloc(SV_MAX_COLUMNS);
                memcpy(used, S->needc, SV_MAX_COLUMNS);
                for (int i = 0; i < f->n; i++) need_minus(used, s, f->v[i]);
                // scansione: le righe vengono lette senza allocare e copiate solo se passano i filtri
                Tuple* view = tuple_create(S->ts->num_columns);
                size_t scap = SV_PAGE_SIZE + 64;
                char* sbuf = sv_malloc(scap);
                bool ok = view && sbuf;
                *ended = false;
                RID rid;
                size_t len;
                const uint8_t* data;
                while (ok && !(scan_max >= 0 && S->n >= scan_max)) {
                    if ((data = heap_scan_next_raw(scan, &rid, &len)) == NULL) { *ended = true; break; }
                    if (len + 64 > scap) {              // riga grande (pagine di overflow): buffer più grande
                        char* nb = sv_realloc(sbuf, len + 64);
                        if (!nb) { ok = false; break; }
                        sbuf = nb;
                        scap = len + 64;
                    }
                    if (!tuple_view(view, data, len, S->ts, sbuf, scap)) continue;
                    p->st_scanned[s]++;
                    if (f->n) {
                        cur[s] = view;
                        bool keep = xv_all_true(f, ec);
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
                        if (!vals[c].is_null && view->values[c].str_val && !is_num_storage(S->ts->columns[c].type) && !used[c]) {
                            vals[c].str_val = NULL;            // testo che la query non usa: non copiato (P1.14)
                            vals[c].is_null = true;
                            continue;
                        }
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
                    if (!source_push(S, t, rid, cap)) ok = false;
                }
                if (view) { for (int c = 0; c < view->num_values; c++) view->values[c].owns_str = false; tuple_free(view); }
                free(sbuf);
                free(used);
                return ok;
}

static bool plan_load(SelectPlan* p) {
    if (p->loaded) return true;
    p->loaded = true;
    // colonne lette per indice, non attraverso un'espressione: chiave di ricerca, intervallo, NATURAL / USING
    for (int s = 0; s < p->nsrc; s++) {
        src_need(&p->src[s], p->probe_col[s]);
        src_need(&p->src[s], p->range_col[s]);
        for (int c = 0; c < p->using_n[s]; c++) {
            int v = p->using_left[s] ? p->using_left[s][c] : 0;
            if (v > 0) { src_need(&p->src[s], c); if (BOUND_SRC(v) < p->nsrc) src_need(&p->src[BOUND_SRC(v)], BOUND_COL(v)); }
        }
    }
    // LIMIT senza ORDER BY, raggruppamenti, DISTINCT, finestre, join né condizioni dopo la lettura: la lettura della
    // tabella si ferma a OFFSET + LIMIT righe (prima: letta e copiata tutta, poi tagliata)
    long scan_max = -1;
    {
        const ASTNode* n = p->n;
        if (p->nsrc == 1 && n->select_stmt.has_limit && n->select_stmt.limit >= 0 && n->select_stmt.offset >= 0 &&
            !n->select_stmt.calc_found && !p->grouped && !n->select_stmt.num_order && !n->select_stmt.distinct && !p->nwins &&
            !p->resid.n && !p->pre_dyn[0].n && !p->compound && !p->next_arm && !p->correlated && !p->src[0].lateral)
            scan_max = (long)n->select_stmt.offset + n->select_stmt.limit;
        if (p->batch > 0) scan_max = p->batch;            // a blocchi (mai insieme a LIMIT: vedi plan_streamable)
    }
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
        } else if (S->bscan) {                                // lettura a blocchi: il blocco successivo
            S->loaded = true;
            int cap = 0;
            bool ended = true;
            bool ok = scan_rows(p, s, S->bscan, p->batch, &ec, cur, &cap, &ended);
            S->bmore = ok && !ended;
            if (!S->bmore) { heap_scan_close(S->bscan); free(S->bhf); S->bscan = NULL; S->bhf = NULL; }
            if (!ok) { snprintf(p->err, sizeof(p->err), "Out of memory while reading '%.64s'", S->table); return false; }
            cur[s] = NULL;
        } else {
            S->loaded = true;
            // aperta prima senza la mappa delle pagine (che legge tutta la catena): una ricerca per chiave legge solo
            // le pagine delle righe trovate; per l'intervallo e la scansione si riapre completa qui sotto
            HeapFile* hf = heap_open_read(p->x->bp, S->ts->root_page_id, S->ts->num_pages);
            if (!hf) { snprintf(p->err, sizeof(p->err), "Failed to read table '%.64s'", S->table); return false; }
            int cap = 0;
            bool done = false;
            // accesso con indice: colonna indicizzata = costante
            if (p->probe_col[s] >= 0 && !p->probe_dyn[s] && db && col_is_indexed(S->ts, p->probe_col[s])) {
                EvalVal kv;
                eval_expr(p->probe_key[s], &ec, &kv);
                char key[KEY_BUF];
                bool keyed = probe_text_ci(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key), col_is_ci(S->ts, p->probe_col[s]));
                // albero su disco (P1.6): radice e foglia invece di costruire l'indice leggendo tutta la tabella
                bool tree_answered = false;          // l'albero ha risposto (anche "troppe righe": si legge tutto)
                if (keyed) {
                    RID* tr = NULL;
                    int tn = 0;
                    int tf = tree_probe(p->x, S->db[0] ? S->db : db->name, S->ts, p->probe_col[s], key, &tr, &tn);
                    if (tf >= 0) { p->probe_rows[s] = tn; tree_answered = true; }
                    if (tf >= 0 && range_worth(S, tn)) {
                        done = true;
                        p->used_index[s] = true;
                        p->used_tree[s] = true;
                        for (int i = 0; i < tn; i++) {
                            Tuple* t = heap_get(hf, tr[i], S->ts);
                            if (!t) continue;
                            p->st_scanned[s]++;
                            cur[s] = t;
                            if (!xv_all_true(f, &ec)) { tuple_free(t); continue; }
                            if (!source_push(S, t, tr[i], &cap)) { tuple_free(t); free(tr); free(hf); snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
                        }
                        cur[s] = NULL;
                    }
                    free(tr);
                }
                // un valore che non diventa una chiave della colonna (testo contro numero, ...): si legge tutto
                MemIndex* ix = !done && !tree_answered && (keyed || kv.null) ? mi_get(p->x, S->db[0] ? S->db : db->name, S->ts, p->probe_col[s]) : NULL;
                const RidList* l = ix && keyed ? mi_lookup(ix, key) : NULL;
                if (ix) p->probe_rows[s] = l ? l->n : 0;
                // un valore frequente (più di un quarto delle righe, come per gli intervalli): leggere tutte le
                // pagine in ordine costa meno di saltare da una riga all'altra (P1.13)
                if (ix && l && !range_worth(S, l->n)) ix = NULL;
                if (ix) {
                    done = true;
                    p->used_index[s] = true;
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
                free(hf);
                hf = heap_open(p->x->bp, S->ts->root_page_id, S->ts->num_pages);
                if (!hf) { snprintf(p->err, sizeof(p->err), "Failed to read table '%.64s'", S->table); return false; }
            }
            // scansione per intervallo con l'indice ordinato
            if (!done && p->range_col[s] >= 0) {
                RID* rids = NULL;
                int nr = 0;
                if (range_rids(p, s, &ec, &rids, &nr)) {
                    p->range_rows[s] = nr;
                    if (range_worth(S, nr)) {
                        done = true;
                        p->used_index[s] = true;
                        for (int i = 0; i < nr; i++) {
                            Tuple* t = heap_get(hf, rids[i], S->ts);
                            if (!t) continue;
                            p->st_scanned[s]++;
                            cur[s] = t;
                            if (!xv_all_true(f, &ec)) { tuple_free(t); continue; }
                            if (!source_push(S, t, rids[i], &cap)) {
                                tuple_free(t); free(rids); free(hf);
                                snprintf(p->err, sizeof(p->err), "Out of memory");
                                return false;
                            }
                        }
                        cur[s] = NULL;
                    }
                    free(rids);
                }
            }
            if (!done) {
                HeapScan* scan = heap_scan_open(hf, S->ts);
                bool ended = true;
                bool ok = scan && scan_rows(p, s, scan, scan_max, &ec, cur, &cap, &ended);
                if (ok && p->batch > 0 && !ended) {           // lettura a blocchi: la scansione continua dopo
                    S->bscan = scan;
                    S->bhf = hf;
                    S->bmore = true;
                    hf = NULL;
                } else if (scan) heap_scan_close(scan);
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
            p->pnext[s] = sv_malloc(sizeof(int) * (size_t)(S->n ? S->n : 1));
            if (!p->pnext[s]) { snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
            for (int r = S->n - 1; r >= 0; r--) {
                char key[KEY_BUF];
                p->pnext[s][r] = -1;
                if (!join_key(S->rows[r], col, kt, key, sizeof(key), col_is_ci(S->ts, col))) continue;
                SMSlot* sl = sm_slot(&p->pmap[s], key, true);
                if (!sl) { snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
                p->pnext[s][r] = sl->val;
                sl->val = r;
            }
        }
    }
    if (sv_scan_cancelled()) { snprintf(p->err, sizeof(p->err), "Query cancelled"); return false; }
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
        Tuple** nv = sv_realloc(c->v, sizeof(Tuple*) * (size_t)nc * (size_t)c->w);
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
        int ls = BOUND_SRC(on->left->bound), lc = BOUND_COL(on->left->bound);
        int rs = BOUND_SRC(on->right->bound), rc = BOUND_COL(on->right->bound);
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
    bool* right_hit = (keep_right && nrows) ? sv_calloc((size_t)nrows, sizeof(bool)) : NULL;
    if (keep_right && nrows && !right_hit) return "Out of memory";

    // hash join quando le condizioni contengono un'uguaglianza fra colonne confrontabili: tabella
    // hash -> prima riga, righe con lo stesso hash in catena (l'uguaglianza la verificano le condizioni)
    int kc, os, oc;
    HMap map = {0};
    int* next = NULL;
    bool hci = false;
    bool hashed = find_hash_cond(conds, k, ec->src, &kc, &os, &oc);
    if (hashed) {
        next = sv_malloc(sizeof(int) * (size_t)(nrows ? nrows : 1));
        if (!next) { free(right_hit); return "Out of memory"; }
        DataType kt = ec->src[k].ts->columns[kc].type;
        hci = col_is_ci(ec->src[k].ts, kc) || col_is_ci(ec->src[os].ts, oc);
        for (int r = nrows - 1; r >= 0; r--) {          // in ordine: la catena parte dalla prima riga
            uint64_t h;
            next[r] = -1;
            if (!col_hash(rows[r], kc, kt, &h, hci)) continue;
            size_t j = map.cap ? h & (map.cap - 1) : 0;
            while (map.cap && map.v[j] >= 0 && map.h[j] != h) j = (j + 1) & (map.cap - 1);
            if (map.cap && map.v[j] >= 0) { next[r] = map.v[j]; map.v[j] = r; continue; }
            if (!hmap_put(&map, h, r)) { hmap_free(&map); free(next); free(right_hit); return "Out of memory"; }
        }
    }
    const char* fail = NULL;
    DataType ot = hashed ? ec->src[os].ts->columns[oc].type : SV_TYPE_INT;
    for (int i = 0; i < in->n && !fail; i++) {
        if ((i & 1023) == 0 && sv_scan_cancelled()) { fail = "Query cancelled"; break; }
        Tuple** base = COMBO(in, i);
        ec->cur = base;
        bool matched = false;
        int r = 0;
        if (hashed) {
            uint64_t h;
            r = -1;
            if (col_hash(base[os], oc, ot, &h, hci) && map.cap) {
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

// ── sorgenti laterali ──

static const char* strdup_static(const char* m) {             // messaggio che resta valido dopo la funzione
    static _Thread_local char buf[300];
    snprintf(buf, sizeof(buf), "%s", m);
    return buf;
}

static bool pool_add(Source* S, Tuple* t) {
    if (S->npool == S->cappool) {
        int nc = S->cappool ? S->cappool * 2 : 64;
        Tuple** nv = sv_realloc(S->pool, sizeof(Tuple*) * (size_t)nc);
        if (!nv) return false;
        S->pool = nv; S->cappool = nc;
    }
    S->pool[S->npool++] = t;
    return true;
}

static void pool_reset(Source* S) {
    for (int i = 0; i < S->npool; i++) tuple_free(S->pool[i]);
    S->npool = 0;
}

// valore di testo (NULL = SQL NULL) nella colonna c della tupla
static void lat_set(Tuple* t, const TableSchema* ts, int c, const char* text) {
    column_set_from_text(&t->values[c], &ts->columns[c], text);
}

// testo di un valore JSON (stringhe senza virgolette, null -> NULL); malloc'd o NULL
static char* json_scalar_text(const char* v, size_t len, bool* is_null) {
    *is_null = false;
    if (len == 4 && !strncmp(v, "null", 4)) { *is_null = true; return NULL; }
    return sj_unquote(v, len);
}

// JSON_TABLE: elementi indicati da un percorso (prefisso[*]resto: ogni elemento dell'array; senza [*]: il
// valore, se c'è). Array malloc'd di testi malloc'd; *n = -1 se manca la memoria
static char** jt_elems(const char* doc, const char* path, long* n) {
    *n = 0;
    const char* star = strstr(path, "[*]");
    char prefix[512], rest[512] = "";
    if (star) {
        snprintf(prefix, sizeof(prefix), "%.*s", (int)(star - path), path);
        snprintf(rest, sizeof(rest), "%s", star + 3);
    } else snprintf(prefix, sizeof(prefix), "%s", path);
    const char* arr; size_t al;
    if (!sj_extract(doc, prefix, &arr, &al)) return NULL;
    long nel = star ? (*arr == '[' ? sj_length(arr, al) : 0) : 1;
    if (nel > 10000000) nel = 10000000;
    char** out = sv_malloc(sizeof(char*) * (size_t)(nel ? nel : 1));
    char* arrc = sv_malloc(al + 1);
    if (!out || !arrc) { free(out); free(arrc); *n = -1; return NULL; }
    memcpy(arrc, arr, al); arrc[al] = '\0';
    long k = 0;
    for (long i = 0; i < nel; i++) {
        const char* ev = arrc; size_t el = al;
        if (star) {
            char ip[600];
            snprintf(ip, sizeof(ip), "$[%ld]%s", i, rest);
            if (!sj_extract(arrc, ip, &ev, &el)) continue;
        }
        char* e = sv_malloc(el + 1);
        if (!e) { for (long j = 0; j < k; j++) free(out[j]); free(out); free(arrc); *n = -1; return NULL; }
        memcpy(e, ev, el); e[el] = '\0';
        out[k++] = e;
    }
    free(arrc);
    *n = k;
    return out;
}

static void jt_free_elems(char** el, long n) {
    for (long i = 0; i < n; i++) free(el[i]);
    free(el);
}

typedef struct {
    Source*      S;
    TableSchema* ts;
    char**       vals;          // testo di ogni colonna prodotta (NULL = SQL NULL)
    char*        err;
    size_t       errn;
} JtGen;

// azzera le colonne prodotte dalle voci [a, b)
static void jt_clear(JtGen* g, int a, int b) {
    for (int i = a; i < b; i++) {
        const TFCol* c = &g->S->tf->cols[i];
        if (c->kind != TFC_NESTED) { free(g->vals[c->out]); g->vals[c->out] = NULL; }
    }
}

static const char* jt_emit(JtGen* g) {
    Tuple* t = tuple_create(g->ts->num_columns);
    if (!t || !pool_add(g->S, t)) { tuple_free(t); return "Out of memory"; }
    for (int c = 0; c < g->ts->num_columns; c++) {
        if (!g->vals[c]) t->values[c].is_null = true;
        else lat_set(t, g->ts, c, g->vals[c]);
    }
    return NULL;
}

// il testo è accettabile per il tipo della colonna? (numeri e booleani)
static bool jt_fits(DataType ty, const char* v, bool composite) {
    if (composite) return ty == SV_TYPE_TEXT || ty == SV_TYPE_VARCHAR || ty == SV_TYPE_CHAR || ty == SV_TYPE_BLOB;
    if (ty == SV_TYPE_INT || ty == SV_TYPE_BIGINT || ty == SV_TYPE_FLOAT || ty == SV_TYPE_DECIMAL) {
        char* e; strtod(v, &e);
        while (*e == ' ') e++;
        return e != v && !*e;
    }
    if (ty == SV_TYPE_BOOL) return !strcmp(v, "true") || !strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "1");
    return true;
}

// valore di una colonna PATH per l'elemento elem, con ON EMPTY / ON ERROR
static const char* jt_value(JtGen* g, const TFCol* c, const char* elem) {
    char** dst = &g->vals[c->out];
    const char* vs; size_t vl;
    if (!sj_extract(elem, c->path, &vs, &vl)) {
        if (c->on_empty == TFE_ERROR) { snprintf(g->err, g->errn, "Missing value for JSON_TABLE column '%.64s'", c->name); return g->err; }
        if (c->on_empty == TFE_DEFAULT && !(*dst = sv_strdup(c->def_empty))) return "Out of memory";
        return NULL;
    }
    bool isnull;
    char* txt = json_scalar_text(vs, vl, &isnull);
    if (isnull) return NULL;
    if (!txt) return "Out of memory";
    DataType ty = g->ts->columns[c->out].type;
    if (!jt_fits(ty, txt, *vs == '{' || *vs == '[')) {
        free(txt);
        if (c->on_error == TFE_ERROR) { snprintf(g->err, g->errn, "Can't store the value of JSON_TABLE column '%.64s'", c->name); return g->err; }
        if (c->on_error == TFE_DEFAULT && !(*dst = sv_strdup(c->def_error))) return "Out of memory";
        return NULL;
    }
    if (ty == SV_TYPE_BOOL && (!strcmp(txt, "true") || !strcmp(txt, "false"))) { txt[0] = txt[0] == 't' ? '1' : '0'; txt[1] = '\0'; }
    *dst = txt;
    return NULL;
}

// righe delle voci [a, b) per l'elemento elem (ord: la sua posizione, per FOR ORDINALITY). I NESTED PATH
// fratelli si uniscono (le righe di uno hanno NULL nelle colonne degli altri); se nessuno ha elementi
// resta una riga con NULL
static const char* jt_gen(JtGen* g, int a, int b, const char* elem, long ord) {
    const TFCol* cols = g->S->tf->cols;
    bool nested = false;
    for (int i = a; i < b; i++) {
        const TFCol* c = &cols[i];
        if (c->kind == TFC_NESTED) { nested = true; i = c->nend - 1; continue; }
        char** dst = &g->vals[c->out];
        free(*dst); *dst = NULL;
        if (c->kind == TFC_ORDINALITY) {
            char nb[32]; snprintf(nb, sizeof(nb), "%ld", ord);
            if (!(*dst = sv_strdup(nb))) return "Out of memory";
        } else if (c->kind == TFC_EXISTS) {
            const char* vs; size_t vl;
            if (!(*dst = sv_strdup(sj_extract(elem, c->path, &vs, &vl) ? "1" : "0"))) return "Out of memory";
        } else {
            const char* f = jt_value(g, c, elem);
            if (f) return f;
        }
    }
    if (!nested) return jt_emit(g);
    long total = 0;
    for (int i = a; i < b; i++) {
        const TFCol* c = &cols[i];
        if (c->kind != TFC_NESTED) continue;
        long n;
        char** el = jt_elems(elem, c->path, &n);
        if (n < 0) return "Out of memory";
        const char* f = NULL;
        for (long j = 0; !f && j < n; j++) {
            if (g->S->npool > 10000000) f = "JSON_TABLE produces too many rows (max 10 million)";
            else f = jt_gen(g, i + 1, c->nend, el[j], j + 1);
            jt_clear(g, i + 1, c->nend);
        }
        jt_free_elems(el, n);
        if (f) return f;
        total += n;
        i = c->nend - 1;
    }
    return total ? NULL : jt_emit(g);
}

// righe della sorgente laterale k per la combinazione base (sorgenti 0..k-1); *rows: array malloc'd
static const char* lateral_rows(SelectPlan* p, int k, Tuple** base, EvalCtx* outer, Tuple*** rows, int* nrows, char* err, size_t errn) {
    Source* S = &p->src[k];
    TableSchema* ts = S->ts;
    *rows = NULL;
    *nrows = 0;
    int start = S->npool;
    EvalCtx lec;
    memset(&lec, 0, sizeof(lec));
    lec.src = p->src; lec.nsrc = k; lec.cur = base; lec.x = p->x; lec.outer = outer;
    if (S->lat_plan) {                                          // LATERAL (subquery)
        ORows res = {0};
        if (!query_exec(S->lat_plan, &lec, 0, &res)) { snprintf(err, errn, "LATERAL '%.64s': %.200s", S->name, S->lat_plan->err); orows_free(&res); return err; }
        if (!S->lat_typed && res.nrows > 0) {                   // tipi delle colonne calcolate: dai primi valori
            for (int c = 0; c < ts->num_columns; c++) {
                if (S->lat_plan->out[c].src_type >= 0) continue;
                DataType t = parse_type(out_type_name(S->lat_plan, c, &res, true));
                ts->columns[c].type = t == SV_TYPE_INT ? SV_TYPE_BIGINT : t;
            }
            S->lat_typed = true;
        }
        for (int i = 0; i < res.nrows; i++) {
            Tuple* t = tuple_create(ts->num_columns);
            if (!t || !pool_add(S, t)) { tuple_free(t); orows_free(&res); return "Out of memory"; }
            for (int c = 0; c < ts->num_columns; c++) {
                const OVal* o = &orow(&res, i)[c];
                if (o->null) { t->values[c].is_null = true; continue; }
                EvalVal ev;
                ev_from_oval(&ev, o);
                lat_set(t, ts, c, ev_s(&ev));
            }
        }
        orows_free(&res);
    } else if (!strcmp(S->tf->func, "GENERATE_SERIES")) {
        EvalVal a[3];
        for (int i = 0; i < 3; i++) { if (i < S->tf->nargs) eval_expr(S->tf->args[i], &lec, &a[i]); else ev_int(&a[i], 1); }
        if (lec.err[0]) { snprintf(err, errn, "%.250s", lec.err); return err; }
        if (!a[0].null && !a[1].null && !a[2].null) {
            int64_t from = (int64_t)ev_as_num(&a[0]), to = (int64_t)ev_as_num(&a[1]), step = (int64_t)ev_as_num(&a[2]);
            if (step == 0) { snprintf(err, errn, "GENERATE_SERIES step cannot be zero"); return err; }
            int64_t count = step > 0 ? (to >= from ? (to - from) / step + 1 : 0) : (from >= to ? (from - to) / -step + 1 : 0);
            if (count > 10000000) { snprintf(err, errn, "GENERATE_SERIES produces too many rows (max 10 million)"); return err; }
            for (int64_t i = 0, v = from; i < count; i++, v += step) {
                Tuple* t = tuple_create(1);
                if (!t || !pool_add(S, t)) { tuple_free(t); return "Out of memory"; }
                t->values[0].bigint_val = v;
            }
        }
    } else {                                                    // JSON_TABLE(doc, 'percorso' COLUMNS (...))
        EvalVal dv, pv;
        eval_expr(S->tf->args[0], &lec, &dv);
        eval_expr(S->tf->args[1], &lec, &pv);
        if (lec.err[0]) { snprintf(err, errn, "%.250s", lec.err); return err; }
        if (!dv.null && !pv.null) {
            char* doc = sv_strdup(ev_s(&dv));
            char* path = sv_strdup(ev_s(&pv));
            if (!doc || !path) { free(doc); free(path); return "Out of memory"; }
            if (!sj_valid(doc)) { free(doc); free(path); snprintf(err, errn, "JSON_TABLE: the document is not valid JSON"); return err; }
            JtGen g = { S, ts, NULL, err, errn };
            g.vals = sv_calloc((size_t)(S->tf->nout ? S->tf->nout : 1), sizeof(char*));
            const char* f = g.vals ? NULL : "Out of memory";
            long nel = 0;
            char** el = f ? NULL : jt_elems(doc, path, &nel);
            if (!f && nel < 0) f = "Out of memory";
            for (long i = 0; !f && i < nel; i++) {
                jt_clear(&g, 0, S->tf->ncols);
                f = jt_gen(&g, 0, S->tf->ncols, el[i], i + 1);
            }
            jt_free_elems(el, nel);
            if (g.vals) { jt_clear(&g, 0, S->tf->ncols); free(g.vals); }
            if (f) { free(doc); free(path); return f; }
            free(doc);
            free(path);
        }
    }
    int n = S->npool - start;
    *rows = sv_malloc(sizeof(Tuple*) * (size_t)(n ? n : 1));
    if (!*rows) return "Out of memory";
    for (int i = 0; i < n; i++) (*rows)[i] = S->pool[start + i];
    *nrows = n;
    return NULL;
}

// join con una sorgente laterale: le sue righe si calcolano per ogni combinazione a sinistra
static const char* lateral_join_step(SelectPlan* p, EvalCtx* ec, Combos* in, Combos* out, int k, JoinType jt, ExprVec* conds) {
    bool keep_left = jt == JOIN_LEFT;
    char err[300];
    for (int i = 0; i < in->n; i++) {
        if ((i & 255) == 0 && sv_scan_cancelled()) return "Query cancelled";
        Tuple** base = COMBO(in, i);
        Tuple** rows = NULL;
        int nr = 0;
        const char* f = lateral_rows(p, k, base, ec->outer, &rows, &nr, err, sizeof(err));
        if (f) { free(rows); return f == err ? strdup_static(f) : f; }
        bool matched = false;
        for (int r = 0; r < nr; r++) {
            base = COMBO(in, i);
            ec->cur = base;
            base[k] = rows[r];
            bool ok = xv_all_true(conds, ec);
            base[k] = NULL;
            if (!ok) continue;
            matched = true;
            if (!combos_push(out, base, k, rows[r])) { free(rows); return out->n >= MAX_JOIN_ROWS ? "The JOIN produces too many rows (over 10 million)" : "Out of memory"; }
        }
        free(rows);
        if (!matched && keep_left && !combos_push(out, COMBO(in, i), k, NULL)) return "Out of memory";
    }
    ec->cur = NULL;
    return NULL;
}

// ── ordinamento ──

typedef struct { SortKey* keys; int nkeys; const bool* desc; const char* nulls; } SortSpec;
static _Thread_local SortSpec g_sort;

static int unit_cmp(const void* pa, const void* pb) {
    int ia = *(const int*)pa, ib = *(const int*)pb;
    const SortKey* a = &g_sort.keys[(size_t)ia * (size_t)g_sort.nkeys];
    const SortKey* b = &g_sort.keys[(size_t)ib * (size_t)g_sort.nkeys];
    for (int k = 0; k < g_sort.nkeys; k++) {
        char nl = g_sort.nulls ? g_sort.nulls[k] : 0;
        if (nl && a[k].null != b[k].null) return (a[k].null == (nl == 'F')) ? -1 : 1;   // NULLS FIRST / LAST
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

// (_Thread_local: qsort gira nel thread che ordina, e più SELECT in parallelo ordinano insieme)
static _Thread_local struct { const SortKey* keys; int K, npart; const bool* desc; } g_wsort;

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
    dst->str = src->str ? sv_strdup(src->str) : NULL;
}

typedef struct { int64_t count, isum; double dsum; bool all_int, overflow; const SortKey* best;
                 Dec ddec; bool any_dec, dec_bad; int scale; } WinAcc;

// decimale esatto di un valore conservato (decimale o intero)
static bool key_get_dec(const SortKey* v, Dec* d) {
    if (v->null || !v->is_num) return false;
    if (v->is_int) { dec_from_i64(v->inum, d); return true; }
    return v->is_dec && v->str && dec_parse(v->str, d);
}

static void wacc_add(WinAcc* a, const char* f, const SortKey* v, bool star) {
    if (star) { a->count++; return; }
    if (v->null) return;
    a->count++;
    if (!strcmp(f, "SUM") || !strcmp(f, "AVG")) {
        Dec vd;
        if (a->count == 1) dec_from_i64(0, &a->ddec);
        if (!a->dec_bad && key_get_dec(v, &vd)) {
            if (v->is_dec) a->any_dec = true;
            if (vd.scale > a->scale) a->scale = vd.scale;
            if (!dec_add(&a->ddec, &vd, &a->ddec)) a->dec_bad = true;
        } else a->dec_bad = true;
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
    out->is_dec = false;
    if (!strcmp(f, "COUNT")) { out->is_num = out->is_int = true; out->inum = a->count; out->num = (double)a->count; return; }
    if (a->count == 0) { out->null = true; return; }
    if (!strcmp(f, "SUM") && a->all_int && !a->overflow) { out->is_num = out->is_int = true; out->inum = a->isum; out->num = (double)a->isum; return; }
    if (((a->any_dec && !strcmp(f, "SUM")) || ((a->any_dec || (a->all_int && !a->overflow)) && !strcmp(f, "AVG"))) && !a->dec_bad) {   // decimali (e AVG di interi): esatti
        Dec r = a->ddec, cnt;
        dec_from_i64(a->count, &cnt);
        if (!strcmp(f, "SUM") || dec_div(&a->ddec, &cnt, a->scale + DEC_DIV_INC, &r)) {
            char t[DEC_LIMBS * 9 + 80];
            dec_format(&r, t, sizeof(t));
            out->is_num = true; out->is_dec = true; out->num = dec_to_double(&r); out->str = sv_strdup(t);
            return;
        }
    }
    if (!strcmp(f, "SUM")) { out->is_num = true; out->num = a->dsum; return; }
    if (!strcmp(f, "AVG")) { out->is_num = true; out->num = a->dsum / (double)a->count; return; }
    if (a->best) oval_copy(out, a->best);
    else out->null = true;
}

// valore numerico della chiave di ORDER BY per i frame RANGE con distanza (date: secondi); false se NULL
static bool range_key(const SortKey* k, bool time, double* out) {
    if (k->null) return false;
    if (!time) {
        if (k->is_num) { *out = k->num; return true; }
        return k->str && text_is_number(k->str, out);
    }
    MyDT t;
    if (!k->str || !mydt_parse(k->str, &t) || !t.has_date) return false;
    int64_t y = t.y - (t.mo <= 2), era = (y >= 0 ? y : y - 399) / 400;   // giorni dal 1970-01-01 (calendario civile)
    int64_t yoe = y - era * 400, mp = (t.mo + 9) % 12, doy = (153 * mp + 2) / 5 + t.d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy, days = era * 146097 + doe - 719468;
    *out = (double)(days * 86400 + t.h * 3600 + t.mi * 60 + t.s);
    return true;
}

// Funzione finestra con un frame esplicito su una partizione [start, end) (posizioni in idx).
// Gli estremi del frame [lo, hi] non diminuiscono mai passando alla riga successiva: il
// frame "scorre" e ogni riga entra ed esce una volta sola (somme e conteggi aggiornati,
// MIN / MAX con una coda monotona), quindi il costo è lineare anche con frame grandi.
static bool window_frame(const WindowSpec* ws, const char* f, bool star, int start, int end, const int* idx,
                         const SortKey* keys, int K, const SortKey* argv, const int* units, SortKey* vals, int* dq) {
    #define KEY(r) (&keys[(size_t)idx[r] * (size_t)K])
    #define ARG(r) (&argv[(size_t)idx[r] * 3])
    bool is_min = !strcmp(f, "MIN"), is_max = !strcmp(f, "MAX");
    int64_t count = 0, isum = 0;
    double dsum = 0;
    int nonint = 0;
    bool overflow = false;
    int qh = 0, qt = 0;                         // coda monotona di MIN / MAX: dq[qh..qt)
    int cl = start, ch = start - 1;             // frame attuale [cl, ch]
    int pg = start, pge = start;                // gruppo di pari della riga (RANGE)
    // RANGE n PRECEDING / FOLLOWING: estremi per valore della chiave (che cresce con r, anche con DESC
    // contando il segno); le righe con la chiave NULL formano un gruppo a parte
    bool by_value = ws->frame == FRAME_RANGE && (ws->fs == FB_PREC || ws->fs == FB_FOLL || ws->fe == FB_PREC || ws->fe == FB_FOLL);
    double sgn = ws->nord > 0 && ws->desc[0] ? -1.0 : 1.0;
    int va = start, vb = start;                 // righe con la chiave non NULL: [va, vb)
    if (by_value) {
        double x;
        while (va < end && !range_key(&KEY(va)[ws->npart], ws->range_time, &x)) va++;
        vb = va;
        while (vb < end && range_key(&KEY(vb)[ws->npart], ws->range_time, &x)) vb++;
    }
    int lp = va, hp = va - 1;                   // estremi che avanzano
    #define RV(i) (range_key(&KEY(i)[ws->npart], ws->range_time, &rvtmp), sgn * rvtmp)
    double rvtmp = 0;
    for (int r = start; r < end; r++) {
        if (r >= pge) {
            pg = r;
            pge = r + 1;
            while (pge < end && keys_equal(KEY(pg), KEY(pge), ws->npart, K)) pge++;
        }
        int64_t lo, hi;
        if (by_value) {
            double cv;
            if (r < va || r >= vb || !range_key(&KEY(r)[ws->npart], ws->range_time, &cv)) { lo = pg; hi = pge - 1; }
            else {
                cv *= sgn;
                if (ws->fs == FB_UNB_PREC) lo = start;
                else if (ws->fs == FB_CURRENT) lo = pg;
                else {
                    double target = ws->fs == FB_PREC ? cv - (double)ws->fs_off : cv + (double)ws->fs_off;
                    while (lp < vb && RV(lp) < target) lp++;
                    lo = lp;
                }
                if (ws->fe == FB_UNB_FOLL) hi = end - 1;
                else if (ws->fe == FB_CURRENT) hi = pge - 1;
                else {
                    double target = ws->fe == FB_PREC ? cv - (double)ws->fe_off : cv + (double)ws->fe_off;
                    while (hp + 1 < vb && RV(hp + 1) <= target) hp++;
                    hi = hp;
                }
            }
            goto have_bounds;
        }
        switch (ws->fs) {
            case FB_UNB_PREC: lo = start; break;
            case FB_PREC:     lo = (int64_t)r - ws->fs_off; break;
            case FB_FOLL:     lo = (int64_t)r + ws->fs_off; break;
            default:          lo = ws->frame == FRAME_RANGE ? pg : r; break;
        }
        switch (ws->fe) {
            case FB_UNB_FOLL: hi = end - 1; break;
            case FB_PREC:     hi = (int64_t)r - ws->fe_off; break;
            case FB_FOLL:     hi = (int64_t)r + ws->fe_off; break;
            default:          hi = ws->frame == FRAME_RANGE ? pge - 1 : r; break;
        }
    have_bounds:
        if (lo < start) lo = start;
        if (hi > end - 1) hi = end - 1;
        if (lo > end) lo = end;
        if (hi < start - 1) hi = start - 1;
        while (ch < hi) {                       // righe che entrano
            const SortKey* v = ARG(++ch);
            if (star) { count++; continue; }
            if (v->null) continue;
            count++;
            if (v->is_num) dsum += v->num;
            if (v->is_int) { if (!overflow && __builtin_add_overflow(isum, v->inum, &isum)) overflow = true; }
            else nonint++;
            if (is_min || is_max) {
                while (qt > qh) {
                    int c = key_cmp(ARG(dq[qt - 1]), v);
                    if (is_min ? c > 0 : c < 0) qt--; else break;
                }
                dq[qt++] = ch;
            }
        }
        while (cl < lo) {                       // righe che escono
            if (cl <= ch) {
                const SortKey* v = ARG(cl);
                if (star) count--;
                else if (!v->null) {
                    count--;
                    if (v->is_num) dsum -= v->num;
                    if (v->is_int) { if (!overflow && __builtin_sub_overflow(isum, v->inum, &isum)) overflow = true; }
                    else nonint--;
                    if (qt > qh && dq[qh] == cl) qh++;
                }
            }
            cl++;
        }
        if (ch < cl - 1) ch = cl - 1;           // frame vuoto oltre la riga attuale
        SortKey* o = &vals[units[idx[r]]];
        free(o->str);
        memset(o, 0, sizeof(*o));
        bool empty = lo > hi;
        o->is_dec = false;
        if (!strcmp(f, "COUNT")) { o->is_num = o->is_int = true; o->inum = count; o->num = (double)count; }
        else if (!strcmp(f, "FIRST_VALUE")) { if (empty) o->null = true; else oval_copy(o, ARG(lo)); }
        else if (!strcmp(f, "LAST_VALUE"))  { if (empty) o->null = true; else oval_copy(o, ARG(hi)); }
        else if (count == 0) o->null = true;
        else if (is_min || is_max) oval_copy(o, ARG(dq[qh]));
        else if (!strcmp(f, "SUM") && !nonint && !overflow) { o->is_num = o->is_int = true; o->inum = isum; o->num = (double)isum; }
        else if (!strcmp(f, "SUM")) { o->is_num = true; o->num = dsum; }
        else { o->is_num = true; o->num = dsum / (double)count; }   // AVG
    }
    #undef RV
    #undef KEY
    #undef ARG
    return true;
}

static bool window_compute(SelectPlan* p, ArmState* st, EvalCtx* ec, ASTExpr* w, const int* units, int n, SortKey* vals) {
    WindowSpec* ws = w->win;
    const char* f = w->func;
    int K = ws->npart + ws->nord;
    SortKey* keys = sv_calloc((size_t)n * (size_t)(K ? K : 1) + 1, sizeof(SortKey));
    SortKey* argv = sv_calloc((size_t)n * 3 + 1, sizeof(SortKey));      // argomenti 1, 2, 3 di ogni riga
    int* idx = sv_malloc(sizeof(int) * (size_t)(n ? n : 1));
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
    // frame esplicito (ROWS / RANGE BETWEEN ...): aggregati e FIRST_VALUE / LAST_VALUE
    bool framed = ws->frame && (aggregate || !strcmp(f, "FIRST_VALUE") || !strcmp(f, "LAST_VALUE"));
    int* dq = framed ? sv_malloc(sizeof(int) * (size_t)(n ? n : 1)) : NULL;
    if (framed && !dq) ok = false;
    for (int start = 0; ok && start < n; ) {
        int end = start + 1;
        while (end < n && keys_equal(KEY(start), KEY(end), 0, ws->npart)) end++;
        if (framed) {
            if (!window_frame(ws, f, star, start, end, idx, keys, K, argv, units, vals, dq)) { ok = false; break; }
            start = end;
            continue;
        }
        int dense = 0;
        WinAcc acc;
        memset(&acc, 0, sizeof(acc));
        acc.all_int = true;
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
                    else if (!strcmp(f, "PERCENT_RANK")) { o->is_num = true; o->num = end - start > 1 ? (double)(g - start) / (double)(end - start - 1) : 0; }
                    else if (!strcmp(f, "CUME_DIST"))    { o->is_num = true; o->num = (double)(ge - start) / (double)(end - start); }
                    else if (!strcmp(f, "NTH_VALUE")) {      // n-esima riga della finestra (fino ai pari della riga corrente)
                        const SortKey* nv = ARG(r, 1);
                        int64_t nth = nv->is_num ? (int64_t)nv->num : 0;
                        int last = ws->nord ? ge - 1 : end - 1;
                        if (nth < 1) { snprintf(ec->err, sizeof(ec->err), "NTH_VALUE needs a positive row number"); ok = false; break; }
                        if (start + nth - 1 <= last) oval_copy(o, ARG(start + nth - 1, 0)); else o->null = true;
                    }
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
    free(dq);
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
    // annullamento: controllato a ogni esecuzione, così si fermano anche le subquery
    // correlate (eseguite una volta per riga esterna)
    if (sv_scan_cancelled()) AFAIL("Query cancelled");

    // 1. righe di ogni sorgente che passano i filtri dipendenti dalla query esterna
    for (int s = 0; s < p->nsrc; s++) {
        Source* S = &p->src[s];
        bool dyn = p->pre_dyn[s].n > 0;
        if (!dyn) { st.frows[s] = S->rows; st.fn[s] = S->n; continue; }
        st.frows[s] = sv_malloc(sizeof(Tuple*) * (size_t)(S->n ? S->n : 1));
        if (!st.frows[s]) AFAIL("Out of memory");
        st.fown[s] = true;
        ec.cur = cur;
        if (p->probe_col[s] >= 0 && p->probe_dyn[s]) {
            EvalVal kv;
            eval_expr(p->probe_key[s], &ec, &kv);
            char key[KEY_BUF];
            bool pci = col_is_ci(S->ts, p->probe_col[s]);
            SMSlot* sl = probe_text_ci(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key), pci) ? sm_slot(&p->pmap[s], key, false) : NULL;
            if (!probe_text_ci(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key), pci) && !kv.null) {
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
    for (int s = 0; s < p->nsrc; s++) if (p->src[s].lateral) pool_reset(&p->src[s]);   // righe dell'esecuzione precedente
    if (p->nsrc == 0 && !g_describe) {                         // (descrizione: nessuna riga, niente da valutare)
        if (!combos_push(&st.combos, NULL, -1, NULL)) AFAIL("Out of memory");
    }
    if (g_describe) {}
    else if (p->nsrc > 0 && p->src[0].lateral) {               // FROM GENERATE_SERIES(...) / JSON_TABLE(...)
        Tuple** lrows = NULL;
        int lnr = 0;
        char lerr[300];
        Tuple* none[MAX_SOURCES] = {0};
        const char* f = lateral_rows(p, 0, none, outer, &lrows, &lnr, lerr, sizeof(lerr));
        if (f) { free(lrows); AFAIL("%s", f); }
        for (int i = 0; i < lnr; i++)
            if (!combos_push(&st.combos, NULL, 0, lrows[i])) { free(lrows); AFAIL("%s", st.combos.n >= MAX_JOIN_ROWS ? "Too many rows" : "Out of memory"); }
        free(lrows);
    } else
    for (int i = 0; p->nsrc > 0 && i < st.fn[0]; i++)
        if (!combos_push(&st.combos, NULL, 0, st.frows[0][i])) AFAIL("%s", st.combos.n >= MAX_JOIN_ROWS ? "Too many rows" : "Out of memory");
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        Combos next = { NULL, 0, 0, w };
        const char* f = p->src[j + 1].lateral
            ? lateral_join_step(p, &ec, &st.combos, &next, j + 1, n->select_stmt.joins[j].type, &p->jcond[j + 1])
            : join_step(&ec, &st.combos, &next, j + 1, n->select_stmt.joins[j].type, &p->jcond[j + 1],
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
    st.null_combo = sv_calloc((size_t)w, sizeof(Tuple*));
    if (!st.null_combo) AFAIL("Out of memory");
    int nunits = 0, nunits_total = 0;
    if (p->grouped) {
        int ngroups = 0;
        // gruppi con una tabella hash sui valori (tipizzati: niente conversione in testo per riga)
        int ng = n->select_stmt.num_group;
        int* group_of = sv_malloc(sizeof(int) * (size_t)(st.combos.n ? st.combos.n : 1));
        EvalVal* kv = sv_malloc(sizeof(EvalVal) * (size_t)(ng ? ng : 1));
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
                    SortKey* nk = sv_realloc(gk, sizeof(SortKey) * (size_t)nc * (size_t)(ng ? ng : 1));
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
        st.group_start = sv_calloc((size_t)ngroups + 1, sizeof(int));
        st.group_size  = sv_calloc((size_t)ngroups + 1, sizeof(int));
        st.groups_buf  = sv_malloc(sizeof(Tuple**) * (size_t)(st.combos.n ? st.combos.n : 1));
        int* fill = sv_calloc((size_t)ngroups + 1, sizeof(int));
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
        st.units = sv_malloc(sizeof(int) * (size_t)(ngroups ? ngroups : 1));
        if (!st.units) AFAIL("Out of memory");
        for (int u = 0; u < ngroups; u++) st.units[nunits++] = u;
        nunits_total = ngroups;
        p->st_groups += ngroups;
    } else {
        st.units = sv_malloc(sizeof(int) * (size_t)(st.combos.n ? st.combos.n : 1));
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
        for (int wi = 0; wi < p->nwins; wi++) {
            st.winv[wi] = sv_calloc((size_t)(total ? total : 1), sizeof(SortKey));
            if (!st.winv[wi]) AFAIL("Out of memory");
            if (!window_compute(p, &st, &ec, p->wins[wi], st.units, nunits, st.winv[wi])) AFAIL("%s", ec.err[0] ? ec.err : "Out of memory");
        }
        ec.win = st.winv;
    }

    // 4. DISTINCT, ORDER BY, OFFSET / LIMIT; le colonne si calcolano solo per le righe che restano
    int nkeys = apply_order ? n->select_stmt.num_order : 0;
    bool distinct = n->select_stmt.distinct;
    ORows all = {0};
    all.ncols = p->nout;
    int* order = sv_malloc(sizeof(int) * (size_t)(nunits ? nunits : 1));   // indici in units (o in all con DISTINCT)
    if (!order) AFAIL("Out of memory");
    int nkeep = 0;
    if (distinct) {
        StrMap seen = {0};
        char* sig = sv_malloc((size_t)(p->nout ? p->nout : 1) * (EV_STR_MAX + 24) + 1);
        bool ok = sig != NULL;
        int* unit_of = sv_malloc(sizeof(int) * (size_t)(nunits ? nunits : 1));
        if (!unit_of) ok = false;
        for (int i = 0; ok && i < nunits; i++) {
            if ((i & 1023) == 0 && sv_scan_cancelled()) { ok = false; break; }
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
        if (!ok) { free(order); orows_free(&all); AFAIL("%s", sv_scan_cancelled() ? "Query cancelled" : "Out of memory"); }
        nunits = nkeep;
    } else {
        for (int i = 0; i < nunits; i++) order[nkeep++] = i;
    }
    if (ec.err[0]) { free(order); orows_free(&all); AFAIL("%s", ec.err); }
    if (nkeys) {
        st.keys = sv_calloc((size_t)nunits * (size_t)nkeys + 1, sizeof(SortKey));
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
        g_sort.nulls = n->select_stmt.order_nulls;
        long want = apply_order && n->select_stmt.has_limit && !n->select_stmt.calc_found ? (long)n->select_stmt.offset + n->select_stmt.limit : -1;
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
        g_select_total = nkeep;                                  // righe prima di LIMIT (SQL_CALC_FOUND_ROWS)
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
    if (sv_scan_cancelled()) AFAIL("Query cancelled");
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
    char* sig = sv_malloc(cap);
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

static _Thread_local const OVal* g_cmp_rows_base;
static _Thread_local int g_cmp_ncols, g_cmp_nkeys;
static _Thread_local const int* g_cmp_cols;
static _Thread_local const bool* g_cmp_desc;

static int compound_cmp(const void* pa, const void* pb) {
    int ia = *(const int*)pa, ib = *(const int*)pb;
    for (int k = 0; k < g_cmp_nkeys; k++) {
        int c = key_cmp(&g_cmp_rows_base[(size_t)ia * (size_t)g_cmp_ncols + (size_t)g_cmp_cols[k]],
                        &g_cmp_rows_base[(size_t)ib * (size_t)g_cmp_ncols + (size_t)g_cmp_cols[k]]);
        if (c) return g_cmp_desc[k] ? -c : c;
    }
    return ia < ib ? -1 : ia > ib;
}

static double now_ms_exec(void) { return sv_get_time_ms(); }   // portabile (MinGW senza clock_gettime)

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
    int* order = sv_malloc(sizeof(int) * (size_t)(nr ? nr : 1));
    if (!order) { orows_free(&acc); snprintf(p->err, sizeof(p->err), "Out of memory"); return false; }
    for (int i = 0; i < nr; i++) order[i] = i;
    if (n->select_stmt.num_order) {
        g_cmp_rows_base = acc.v; g_cmp_ncols = acc.ncols; g_cmp_nkeys = n->select_stmt.num_order;
        g_cmp_cols = p->corder; g_cmp_desc = n->select_stmt.order_desc;
        qsort(order, (size_t)nr, sizeof(int), compound_cmp);
    }
    int first = n->select_stmt.offset < nr ? n->select_stmt.offset : nr;
    int count = nr - first;
    g_select_total = nr;
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
            if (!cached && !out->null && out->str) {         // le righe vengono liberate: il testo va copiato (un NULL resta NULL)
                bool num = out->is_num, isint = out->is_int, dc = out->is_dec;
                if (!ev_text_long(out, out->str, strlen(out->str))) { ctx_error(c, "%s", "Out of memory"); ev_null(out); }
                else { out->is_num = num; out->is_int = isint; out->is_dec = dc; }
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
                    p->inset_ci = false;
                    for (int i = 0; i < rows->nrows; i++) {
                        const OVal* o = orow(rows, i);
                        if (o->null) { p->inset_null = true; continue; }
                        if (o->ci) p->inset_ci = true;
                        int cl = o->is_num ? 1 : 2;
                        p->inset_class = p->inset_class == 0 ? cl : p->inset_class == cl ? cl : 3;
                        char key[KEY_BUF];
                        oval_key(o, key, sizeof(key));
                        if (!sm_slot(&p->inset, key, true)) { p->inset_class = 3; break; }
                    }
                }
                // collation diverse fra il valore e la subquery: confronto riga per riga (sotto)
                if ((p->inset_class == val_class(&a) || p->inset_class == 0) && (a.ci && !a.is_num) == p->inset_ci) {
                    OVal o;
                    memset(&o, 0, sizeof(o));
                    o.null = false; o.is_num = a.is_num; o.is_int = a.is_int; o.inum = a.inum; o.num = a.num; o.ci = a.ci && !a.is_num;
                    o.is_dec = a.is_dec;
                    o.str = (char*)(a.is_num && !a.is_dec ? NULL : ev_s(&a));
                    char key[KEY_BUF];
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
// Stime delle righe: dalle statistiche di ANALYZE TABLE quando ci sono (valori distinti, NULL, istogramma:
// src/query/stats.h), altrimenti le selettività di default di System R (colonna = costante 1/10, intervallo
// 1/3, LIKE 1/10, altro 1/2). Equi-join R ⋈ S: |R|·|S| / max(distinti delle due colonne), o senza statistiche
// |R|·|S| / max(|R|, |S|) (come un join su chiave esterna).

// statistiche della colonna di una sorgente (NULL: vista, colonna esterna, mai analizzata)
static const ColStats* src_col_stats(const SelectPlan* p, const ASTExpr* e) {
    if (!p || !e || e->type != EXPR_COLUMN || e->bound <= 0 || e->outer) return NULL;
    int s = BOUND_SRC(e->bound), c = BOUND_COL(e->bound);
    if (s >= p->nsrc || p->src[s].is_view || !p->src[s].ts || c >= p->src[s].ts->num_columns) return NULL;
    const Source* S = &p->src[s];
    const char* db = S->db;
    if (!db[0]) { Database* d = catalog_current_db(p->x->catalog); db = d ? d->name : ""; }
    return stats_col(db, S->table, S->ts->columns[c].name);
}

static bool lit_number(const ASTExpr* e, double* v) {
    if (!e) return false;
    if (e->type == EXPR_INT) { *v = (double)e->int_val; return true; }
    if (e->type == EXPR_FLOAT) { *v = e->float_val; return true; }
    if (e->type == EXPR_UNARY && e->op == OP_NEG && lit_number(e->left, v)) { *v = -*v; return true; }
    return false;
}

static double cond_selectivity(const SelectPlan* p, const ASTExpr* c) {
    if (!c) return 1;
    if (c->type == EXPR_BINARY) {
        if (c->op == OP_AND) return cond_selectivity(p, c->left) * cond_selectivity(p, c->right);
        if (c->op == OP_OR) { double a = cond_selectivity(p, c->left), b = cond_selectivity(p, c->right); return a + b - a * b; }
        // colonna op costante (in un verso o nell'altro), o colonna = colonna
        const ColStats* ls = src_col_stats(p, c->left), *rs = src_col_stats(p, c->right);
        const ColStats* cs = ls ? ls : rs;
        const ASTExpr* other = ls ? c->right : c->left;
        double v = 0;
        bool have = lit_number(other, &v);
        int op = c->op;
        if (rs && !ls) {                                   // costante a sinistra: si gira il confronto
            if (op == OP_LT) op = OP_GT; else if (op == OP_GT) op = OP_LT;
            else if (op == OP_LE) op = OP_GE; else if (op == OP_GE) op = OP_LE;
        }
        switch (op) {
            case OP_EQ:
                if (ls && rs) { double d = ls->ndv > rs->ndv ? ls->ndv : rs->ndv; return d > 0 ? 1 / d : 1; }
                return cs ? stats_sel_eq(cs, have, v) : 0.1;
            case OP_NEQ: return cs && !(ls && rs) ? 1 - cs->null_frac - stats_sel_eq(cs, have, v) : 0.9;
            case OP_LT: return cs && have && !(ls && rs) ? stats_sel_range(cs, '<', v) : 1.0 / 3;
            case OP_LE: return cs && have && !(ls && rs) ? stats_sel_range(cs, 'l', v) : 1.0 / 3;
            case OP_GT: return cs && have && !(ls && rs) ? stats_sel_range(cs, '>', v) : 1.0 / 3;
            case OP_GE: return cs && have && !(ls && rs) ? stats_sel_range(cs, 'g', v) : 1.0 / 3;
            case OP_LIKE: return 0.1;
            case OP_NOT_LIKE: return 0.9;
            default: return 0.5;
        }
    }
    if (c->type == EXPR_UNARY && c->op == OP_NOT) return 1 - cond_selectivity(p, c->left);
    if (c->type == EXPR_UNARY && c->op == OP_IS_NULL) { const ColStats* cs = src_col_stats(p, c->left); return cs ? cs->null_frac : 0.1; }
    if (c->type == EXPR_SUBQUERY) return 0.5;
    return 0.5;
}

static double xv_selectivity(const SelectPlan* p, const ExprVec* xv) {
    double s = 1;
    for (int i = 0; i < xv->n; i++) s *= cond_selectivity(p, xv->v[i]);
    return s;
}

// equi-join con le statistiche: |R|·|S| / max(distinti); -1 se la condizione non è una colonna = colonna analizzata
static double join_card(const SelectPlan* p, const ExprVec* jc, double left, double right) {
    for (int i = 0; i < jc->n; i++) {
        const ASTExpr* c = jc->v[i];
        if (c->type != EXPR_BINARY || c->op != OP_EQ) continue;
        const ColStats* a = src_col_stats(p, c->left), *b = src_col_stats(p, c->right);
        if (!a || !b) continue;
        double d = a->ndv > b->ndv ? a->ndv : b->ndv;
        double r = left * right / (d > 0 ? d : 1);
        for (int k = 0; k < jc->n; k++) if (k != i) r *= cond_selectivity(p, jc->v[k]);
        return r;
    }
    return -1;
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
    char c0[16], c1[512], c2[512], c3[32], c4[32];
    snprintf(c0, sizeof(c0), "%d", r->num_rows);
    snprintf(c1, sizeof(c1), "%s%s", indent, op);
    snprintf(c2, sizeof(c2), "%s", detail);
    snprintf(c3, sizeof(c3), "%.0f", est < 1 && est > 0 ? 1 : est);
    snprintf(c4, sizeof(c4), "%ld", actual);
    const char* vals[5] = { c0, c1, c2, est < 0 ? NULL : c3, actual < 0 ? NULL : c4 };
    static const CellType types[5] = { CELL_TYPE_INT, CELL_TYPE_STRING, CELL_TYPE_STRING, CELL_TYPE_INT, CELL_TYPE_INT };
    if (!db_row_set(pr, r->num_cols < 5 ? r->num_cols : 5, vals, types)) { r->num_rows--; return; }
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

// riga del piano per una sorgente con un intervallo su una colonna indicizzata: con EXPLAIN
// l'indice viene interrogato per sapere quante righe trova, come farà l'esecuzione
static bool range_explain(PlanOut* po, SelectPlan* p, int s, const char* indent, double* est) {
    Source* S = &p->src[s];
    long found = p->range_rows[s];
    if (found < 0) {
        EvalCtx ec;
        Tuple* cur[MAX_SOURCES] = {0};
        memset(&ec, 0, sizeof(ec));
        ec.src = p->src; ec.nsrc = p->nsrc; ec.cur = cur; ec.x = p->x;
        RID* rids = NULL;
        int n = 0;
        if (!range_rids(p, s, &ec, &rids, &n)) return false;
        free(rids);
        found = n;
    }
    const char* col = S->ts->columns[p->range_col[s]].name;
    char det[400];
    if (range_worth(S, (int)found)) {
        *est = (double)found;
        snprintf(det, sizeof(det), "%.64s using %.64s.%.64s (ordered in-memory index: %ld row%s in the range, read in page order)",
                 S->name, S->table, col, found, found == 1 ? "" : "s");
        plan_row_a(po, indent, "Index range scan", det, *est, po->analyze ? p->st_scanned[s] : -1);
    } else {
        *est = (double)S->ts->num_rows;
        snprintf(det, sizeof(det), "%.64s (about %llu rows; the index on %.64s finds %ld, too many: reading every page in order is cheaper)",
                 S->name, (unsigned long long)S->ts->num_rows, col, found);
        plan_row_a(po, indent, "Table scan", det, *est, po->analyze ? p->st_scanned[s] : -1);
    }
    return true;
}

// righe che la ricerca per chiave di s troverebbe nell'indice (-1 se non si può sapere), senza eseguire
static long probe_count(SelectPlan* p, int s) {
    Source* S = &p->src[s];
    Database* db = catalog_current_db(p->x->catalog);
    if (!db) return -1;
    EvalCtx ec;
    Tuple* cur[MAX_SOURCES] = {0};
    memset(&ec, 0, sizeof(ec));
    ec.src = p->src; ec.nsrc = p->nsrc; ec.cur = cur; ec.x = p->x;
    EvalVal kv;
    eval_expr(p->probe_key[s], &ec, &kv);
    char key[KEY_BUF];
    if (!probe_text_ci(&kv, S->ts->columns[p->probe_col[s]].type, key, sizeof(key), col_is_ci(S->ts, p->probe_col[s]))) return -1;
    RID* tr = NULL;
    int tn = 0;
    int tf = tree_probe(p->x, S->db[0] ? S->db : db->name, S->ts, p->probe_col[s], key, &tr, &tn);
    free(tr);
    if (tf >= 0) { p->used_tree[s] = true; return tn; }
    MemIndex* ix = mi_get(p->x, S->db[0] ? S->db : db->name, S->ts, p->probe_col[s]);
    if (!ix) return -1;
    const RidList* l = mi_lookup(ix, key);
    return l ? l->n : 0;
}

// la condizione w è già nel conteggio dell'indice di s (la chiave della ricerca o un estremo dell'intervallo)?
static bool index_counted(const SelectPlan* p, int s, const ASTExpr* w) {
    if (w->type != EXPR_BINARY) return false;
    const ASTExpr* k[3] = { p->probe_col[s] >= 0 && !p->probe_dyn[s] ? p->probe_key[s] : NULL, p->range_lo[s], p->range_hi[s] };
    for (int i = 0; i < 3; i++) if (k[i] && (w->left == k[i] || w->right == k[i])) return true;
    return false;
}

static double explain_arm(PlanOut* po, SelectPlan* p, const char* indent, int depth) {
    ASTNode* n = p->n;
    char buf[400], det[700];
    double rows = 1;
    bool est_probe_done[MAX_SOURCES] = {0};
    for (int s = 0; s < p->nsrc; s++) {
        Source* S = &p->src[s];
        double est;
        if (S->is_view) {
            est = (double)S->n;
            snprintf(det, sizeof(det), "%.64s (%d rows, computed)", S->name, S->n);
            plan_row_a(po, indent, "View", det, est, po->analyze ? p->st_scanned[s] : -1);
        } else if (p->probe_col[s] >= 0 && !p->probe_dyn[s] && col_is_indexed(S->ts, p->probe_col[s])) {
            // come l'esecuzione: l'indice dice quante righe trova la chiave, e con troppe si legge tutto
            long found = p->probe_rows[s] >= 0 ? p->probe_rows[s] : probe_count(p, s);
            const char* cn = S->ts->columns[p->probe_col[s]].name;
            if (found >= 0 && !range_worth(S, (int)found)) {
                est = (double)S->ts->num_rows;
                snprintf(det, sizeof(det), "%.64s (about %llu rows; the index on %.64s finds %ld, too many: reading every page in order is cheaper)",
                         S->name, (unsigned long long)S->ts->num_rows, cn, found);
                plan_row_a(po, indent, "Table scan", det, est, po->analyze ? p->st_scanned[s] : -1);
                est = (double)found;                       // dopo il filtro restano le righe che l'indice ha contato
            } else {
                est = found >= 0 ? (double)found : S->ts->columns[p->probe_col[s]].primary_key ? 1 : (double)S->ts->num_rows * 0.1;
                snprintf(det, sizeof(det), "%.64s using %.64s.%.64s (%s)", S->name, S->table, cn,
                         p->used_tree[s] ? "B+tree on disk" : "in-memory index");
                plan_row_a(po, indent, "Index lookup", det, est, po->analyze ? p->st_scanned[s] : -1);
            }
            // la condizione della ricerca è già nel conteggio: il filtro sotto non la ricalcola
            est_probe_done[s] = true;
        } else if (p->range_col[s] >= 0 && range_explain(po, p, s, indent, &est)) {
            // scansione per intervallo (righe e scelta dell'accesso in range_explain): con l'indice, le condizioni
            // dell'intervallo sono già nel conteggio
            if (est < (double)S->ts->num_rows || range_worth(S, (int)est)) est_probe_done[s] = true;
        } else {
            est = (double)S->ts->num_rows;
            snprintf(det, sizeof(det), "%.64s (about %llu rows)", S->name, (unsigned long long)S->ts->num_rows);
            plan_row_a(po, indent, "Table scan", det, est, po->analyze ? p->st_scanned[s] : -1);
        }
        ExprVec* pf[2] = { &p->pre_static[s], &p->pre_dyn[s] };
        for (int q = 0; q < 2; q++) {
            if (!pf[q]->n) continue;
            if (q == 0 && est_probe_done[s]) {                    // l'indice le ha già contate: solo le altre condizioni
                for (int i = 0; i < pf[q]->n; i++) if (!index_counted(p, s, pf[q]->v[i])) est *= cond_selectivity(p, pf[q]->v[i]);
            } else est *= xv_selectivity(p, pf[q]);
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
        double jr = join_card(p, &p->jcond[s], rows, est);
        if (jr < 0) jr = hashed ? rows * est / (rows > est ? (rows > 0 ? rows : 1) : (est > 0 ? est : 1))
                                : rows * est * xv_selectivity(p, &p->jcond[s]);
        if (j->type == JOIN_LEFT && jr < rows) jr = rows;
        if ((j->type == JOIN_RIGHT || j->type == JOIN_FULL) && jr < est) jr = est;
        if (j->type == JOIN_FULL && jr < rows + est) jr = jr < rows ? rows + est : jr;
        rows = jr;
        plan_row_a(po, indent, hashed ? "Hash join" : "Nested loop join", det, rows, po->analyze ? p->st_join[s] : -1);
    }
    if (p->resid.n) {
        rows *= xv_selectivity(p, &p->resid);
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
        rows *= cond_selectivity(p, n->select_stmt.having);
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
    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    int nc = analyze ? 5 : 4;
    r->success = true;
    r->num_cols = nc;
    r->col_names = sv_calloc(5, sizeof(char*));
    r->col_types = sv_calloc(5, sizeof(char*));
    r->rows = sv_calloc(256, sizeof(Row));
    if (!r->col_names || !r->col_types || !r->rows) { db_result_free(r); return NULL; }
    const char* names[5] = { "Step", "Operation", "Details", "Est. rows", "Actual rows" };
    const char* types[5] = { "INT", "VARCHAR", "VARCHAR", "INT", "INT" };
    for (int i = 0; i < nc; i++) { r->col_names[i] = sv_strdup(names[i]); r->col_types[i] = sv_strdup(types[i]); }
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

// righe calcolate -> QueryResult (nomi, tipi, origine delle colonne, testi); NULL senza memoria
static QueryResult* rows_result(ExecCtx* ctx, SelectPlan* p, ASTNode* n, int depth, ORows* rows) {
    int nout = p->nout;
    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
    bool ok = r != NULL;
    if (ok) {
        r->success = true;
        r->num_cols = nout;
        r->col_names = sv_calloc((size_t)nout + 1, sizeof(char*));
        r->col_types = sv_calloc((size_t)nout + 1, sizeof(char*));
        r->rows = sv_calloc((size_t)(rows->nrows ? rows->nrows : 1), sizeof(Row));
        ok = r->col_names && r->col_types && r->rows;
    }
    // origine delle colonne: solo se ogni riga del risultato è una riga di una tabella
    if (ok && depth == 0 && !p->grouped && !p->compound && !n->select_stmt.distinct && nout > 0) {
        Database* cdb = catalog_current_db(ctx->catalog);
        r->col_tables = sv_calloc((size_t)nout, sizeof(char*));
        r->col_columns = sv_calloc((size_t)nout, sizeof(char*));
        if (!r->col_tables || !r->col_columns) ok = false;
        if (cdb) snprintf(r->source_db, sizeof(r->source_db), "%s", cdb->name);
        for (int j = 0; ok && j < nout; j++) {
            const ASTExpr* e = p->out[j].e;
            if (!e || e->type != EXPR_COLUMN || e->bound <= 0 || e->outer) continue;
            int s = BOUND_SRC(e->bound), c = BOUND_COL(e->bound);
            if (s < 0 || s >= p->nsrc || p->src[s].is_view || !p->src[s].ts || c >= p->src[s].ts->num_columns) continue;
            r->col_tables[j] = sv_strdup(p->src[s].table);
            r->col_columns[j] = sv_strdup(p->src[s].ts->columns[c].name);
        }
    }
    for (int j = 0; ok && j < nout; j++) {
        r->col_names[j] = sv_strdup(p->out[j].name);
        r->col_types[j] = sv_strdup(out_type_name(p, j, rows, false));
    }
    // ogni riga è un solo blocco: le celle seguite dai testi, scritti prima in un buffer di appoggio
    size_t tcap = 4096, *toff = sv_malloc(sizeof(size_t) * (size_t)(nout ? nout : 1));
    char* tbuf = sv_malloc(tcap);
    CellType* ttype = sv_malloc(sizeof(CellType) * (size_t)(nout ? nout : 1));
    if (!toff || !tbuf || !ttype) ok = false;
    for (int i = 0; ok && i < rows->nrows; i++) {
        size_t tlen = 0;
        for (int j = 0; ok && j < nout; j++) {
            EvalVal v;
            const OVal* o = &orow(rows, i)[j];
            ev_from_oval(&v, o);
            const char* s = v.null ? "NULL" : ev_s(&v);
            size_t l = strlen(s);
            if (tlen + l + 1 > tcap) {
                while (tlen + l + 1 > tcap) tcap *= 2;
                char* nb = sv_realloc(tbuf, tcap);
                if (!nb) { ok = false; break; }
                tbuf = nb;
            }
            memcpy(tbuf + tlen, s, l + 1);
            toff[j] = tlen;
            tlen += l + 1;
            int st = o->null ? -1 : p->out[j].src_type;
            ttype[j] = v.null ? CELL_TYPE_NULL : st >= 0 ? cell_type_of((DataType)st)
                     : v.is_int ? CELL_TYPE_INT : v.is_num ? CELL_TYPE_FLOAT : CELL_TYPE_STRING;
        }
        if (!ok) break;
        Row* row = &r->rows[i];
        size_t head = sizeof(Cell) * (size_t)(nout ? nout : 1);
        char* block = sv_malloc(head + tlen);
        if (!block) { ok = false; break; }
        memcpy(block + head, tbuf, tlen);
        row->cells = (Cell*)block;
        row->num_cells = nout;
        r->num_rows = i + 1;
        for (int j = 0; j < nout; j++) {
            row->cells[j].value = block + head + toff[j];
            row->cells[j].type = ttype[j];
            row->cells[j].is_null = ttype[j] == CELL_TYPE_NULL;
        }
    }
    free(toff); free(tbuf); free(ttype);
    if (!ok) { db_result_free(r); return NULL; }
    return r;
}

// ── COUNT(*) in memoria costante (P1.14) ──
// SELECT COUNT(*) FROM t [WHERE filtri della tabella]: le righe si contano durante la scansione, sulla vista della
// pagina, senza copiarle (prima: ogni riga che passava il filtro copiata e tenuta fino alla fine)
static bool plan_count_only(const SelectPlan* p) {
    const ASTNode* n = p->n;
    if (p->nsrc != 1 || p->src[0].is_view || p->src[0].lateral || p->compound || p->next_arm || p->correlated) return false;
    if (!p->grouped || n->select_stmt.num_group || n->select_stmt.having || p->nwins || n->select_stmt.distinct) return false;
    if (p->nout != 1 || p->resid.n || p->pre_dyn[0].n || p->probe_col[0] >= 0 || p->range_col[0] >= 0) return false;
    if (n->select_stmt.num_order || n->select_stmt.calc_found) return false;
    if (n->select_stmt.has_limit && (n->select_stmt.limit < 1 || n->select_stmt.offset != 0)) return false;
    const ASTExpr* e = p->sel[0];
    return e && e->type == EXPR_AGG && !strcmp(e->func, "COUNT") && !e->agg_distinct && !strcmp(e->col_name, "*");
}

static bool count_scan(SelectPlan* p, long long* out) {
    Source* S = &p->src[0];
    ExprVec* f = &p->pre_static[0];
    HeapFile* hf = heap_open(p->x->bp, S->ts->root_page_id, S->ts->num_pages);
    HeapScan* scan = hf ? heap_scan_open(hf, S->ts) : NULL;
    Tuple* view = f->n ? tuple_create(S->ts->num_columns) : NULL;
    size_t scap = SV_PAGE_SIZE + 64;
    char* sbuf = f->n ? sv_malloc(scap) : NULL;
    bool ok = scan && (!f->n || (view && sbuf));
    Tuple* cur[MAX_SOURCES] = {0};
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.src = p->src; ec.nsrc = p->nsrc; ec.cur = cur; ec.x = p->x;
    long long n = 0;
    RID rid;
    size_t len;
    const uint8_t* data;
    while (ok && (data = heap_scan_next_raw(scan, &rid, &len)) != NULL) {
        p->st_scanned[0]++;
        if (f->n) {
            if (len + 64 > scap) {
                char* nb = sv_realloc(sbuf, len + 64);
                if (!nb) { ok = false; break; }
                sbuf = nb;
                scap = len + 64;
            }
            if (!tuple_view(view, data, len, S->ts, sbuf, scap)) continue;
            cur[0] = view;
            bool keep = xv_all_true(f, &ec);
            cur[0] = NULL;
            if (!keep) continue;
        }
        n++;
    }
    if (view) { for (int c = 0; c < view->num_values; c++) view->values[c].owns_str = false; tuple_free(view); }
    free(sbuf);
    if (scan) heap_scan_close(scan);
    free(hf);
    if (ec.err[0]) { snprintf(p->err, sizeof(p->err), "%s", ec.err); return false; }
    if (sv_scan_cancelled()) { snprintf(p->err, sizeof(p->err), "Query cancelled"); return false; }
    if (!ok) { snprintf(p->err, sizeof(p->err), "Out of memory while reading '%.64s'", S->table); return false; }
    p->st_loaded[0] = n;
    *out = n;
    return true;
}

// ── risultati in streaming (P1.14) ──
// Una SELECT di primo livello su una tabella sola, senza ordinamento, raggruppamenti, DISTINCT, finestre, LIMIT né
// correlazioni, si esegue a blocchi di STREAM_BATCH righe lette: ogni blocco passa per la stessa pipeline e va al
// sink, poi le sue righe si liberano. La memoria resta quella di un blocco qualunque sia la tabella.
#define STREAM_BATCH 2048

static _Thread_local ExecRowSink* g_stream_sink;
void executor_set_stream_sink(ExecRowSink* sink) { g_stream_sink = sink; }

static bool plan_streamable(const SelectPlan* p) {
    const ASTNode* n = p->n;
    return p->nsrc == 1 && !p->src[0].is_view && !p->src[0].lateral && !p->grouped && !p->compound && !p->next_arm &&
           !p->correlated && !n->select_stmt.num_order && !n->select_stmt.distinct && !p->nwins &&
           !n->select_stmt.has_limit && !n->select_stmt.calc_found && !p->pre_dyn[0].n;
}

static QueryResult* stream_select(ExecCtx* ctx, SelectPlan* p, ASTNode* n, ExecRowSink* sink) {
    p->batch = STREAM_BATCH;
    long long total = 0;
    int ncols = p->nout;
    bool first = true;
    char err[300] = "";
    for (;;) {
        ORows rows = {0};
        if (!query_exec(p, ctx->trig_eval, 0, &rows)) {
            snprintf(err, sizeof(err), "%s", p->err[0] ? p->err : "Query failed");
            orows_free(&rows);
            break;
        }
        QueryResult* r = rows_result(ctx, p, n, 0, &rows);
        orows_free(&rows);
        if (!r) { snprintf(err, sizeof(err), "Out of memory"); break; }
        bool go = (!first || sink->begin(sink->ud, r)) && sink->rows(sink->ud, r);
        first = false;
        total += r->num_rows;
        db_result_free(r);
        if (!go) { snprintf(err, sizeof(err), "The result could not be sent"); break; }
        Source* S = &p->src[0];
        if (!S->bmore) break;
        arena_free(&S->arena);                      // il blocco successivo: le righe di questo non servono più
        free(S->rows);
        free(S->rids);
        S->rows = NULL;
        S->rids = NULL;
        S->n = 0;
        S->arena_rows = false;
        S->loaded = false;
        p->loaded = false;
    }
    plan_free(p);
    if (err[0]) {
        QueryResult* e = result_error_msg(err);
        if (e && !first) e->streamed = true;        // righe già mandate: chi chiama chiude il resultset con l'errore
        return e;
    }
    QueryResult* res = sv_calloc(1, sizeof(QueryResult));
    if (!res) return result_error_msg("Out of memory");
    res->success = true;
    res->streamed = true;
    res->streamed_rows = total;                     // colonne e righe già consegnate al sink (num_cols resta 0)
    (void)ncols;
    return res;
}

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
    ExecRowSink* sink = depth == 0 ? g_stream_sink : NULL;
    g_stream_sink = NULL;                               // solo per lo statement principale
    if (sink && plan_streamable(p)) return stream_select(ctx, p, n, sink);
    if (plan_count_only(p)) {
        long long cnt = 0;
        if (!count_scan(p, &cnt)) {
            snprintf(err, sizeof(err), "%s", p->err[0] ? p->err : "Query failed");
            plan_free(p);
            return result_error_msg(err);
        }
        ORows crow = {0};
        crow.ncols = 1;
        OVal* o = orows_add(&crow);
        EvalVal cv;
        ev_int(&cv, cnt);
        if (o) key_set(o, &cv);
        QueryResult* cr = o ? rows_result(ctx, p, n, depth, &crow) : NULL;
        orows_free(&crow);
        plan_free(p);
        return cr ? cr : result_error_msg("Out of memory");
    }
    ORows rows = {0};
    if (!query_exec(p, ctx->trig_eval, 0, &rows)) {
        snprintf(err, sizeof(err), "%s", p->err[0] ? p->err : "Query failed");
        orows_free(&rows);
        plan_free(p);
        return result_error_msg(err);
    }
    QueryResult* r = rows_result(ctx, p, n, depth, &rows);
    orows_free(&rows);
    plan_free(p);
    if (!r) return result_error_msg("Out of memory");
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
                c->values[i].str_val = sv_strdup(v->str_val ? v->str_val : "");
                if (!c->values[i].str_val) { tuple_free(c); return NULL; }
                c->values[i].owns_str = true;
                break;
        }
    }
    return c;
}

// testo di un valore calcolato da salvare (NULL per i NULL)
static const char* ev_store_text(const EvalVal* v) { return v->null ? NULL : ev_s(v); }

// Protezione: scritto e letto solo con il motore in esclusiva (le letture parallele non scrivono).
static bool g_fk_checks = true;
void executor_set_fk_checks(bool on) { g_fk_checks = on; }
bool executor_fk_checks(void) { return g_fk_checks; }

// impostato una volta da db_init, poi solo letto
static ExecChangeHook g_change_hook;
void executor_set_change_hook(ExecChangeHook hook) { g_change_hook = hook; }

static void change_notify(ExecCtx* ctx, uint8_t type, RID rid, RID nrid, const uint8_t* old, size_t ol, TableSchema* ts) {
    if (!g_change_hook || !ctx->tx_id) return;
    Database* db = catalog_current_db(ctx->catalog);
    g_change_hook(ctx->tx_id, db ? db->name : "", ts->name, type, rid, nrid, old, ol);
}

static void wal_log(ExecCtx* ctx, uint8_t type, RID rid, const Tuple* newt, const Tuple* oldt, TableSchema* ts) {
    trees_on_write(ctx, ts);                       // gli alberi su disco della tabella non valgono più (P1.6)
    if (!ctx->wal) return;
    size_t nl = 0, ol = 0;
    uint8_t* ns = newt ? tuple_serialize((Tuple*)newt, ts, &nl) : NULL;
    uint8_t* os = oldt ? tuple_serialize((Tuple*)oldt, ts, &ol) : NULL;
    change_notify(ctx, type, rid, rid, os, ol, ts);
    if (wal_write(ctx->wal, type, ctx->tx_id, rid.page_id, (SVRID){rid.page_id, rid.slot_id},
                  ns, (uint32_t)nl, os, (uint32_t)ol) == SV_OK)
        bp_note_lsn(ctx->bp, rid.page_id, ctx->wal->last_lsn);      // LSN della pagina (redo, doppia scrittura)
    free(ns);
    free(os);
}

static void wal_log_move(ExecCtx* ctx, RID old_rid, RID new_rid, const Tuple* newt, const Tuple* oldt, TableSchema* ts) {
    trees_on_write(ctx, ts);
    if (!ctx->wal) return;
    size_t nl = 0, ol = 0;
    uint8_t* ns = tuple_serialize((Tuple*)newt, ts, &nl);
    uint8_t* os = tuple_serialize((Tuple*)oldt, ts, &ol);
    change_notify(ctx, LOG_UPDATE_MOVE, old_rid, new_rid, os, ol, ts);
    uint8_t* data = ns ? sv_malloc(nl + 6) : NULL;
    if (data) {
        memcpy(data, &new_rid.page_id, 4);
        memcpy(data + 4, &new_rid.slot_id, 2);
        memcpy(data + 6, ns, nl);
        if (wal_write(ctx->wal, LOG_UPDATE_MOVE, ctx->tx_id, old_rid.page_id, (SVRID){old_rid.page_id, old_rid.slot_id},
                      data, (uint32_t)(nl + 6), os, (uint32_t)ol) == SV_OK) {
            bp_note_lsn(ctx->bp, old_rid.page_id, ctx->wal->last_lsn);
            bp_note_lsn(ctx->bp, new_rid.page_id, ctx->wal->last_lsn);
        }
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
        Change* nv = sv_realloc(cs->v, sizeof(Change) * (size_t)nc);
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
        dst->str_val = sv_strdup(src->str_val ? src->str_val : "");
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
                TableSchema* C = db->tables[ti];
                for (int fi = 0; fi < C->num_constraints && !fail; fi++) {
                    const Constraint* f = &C->constraints[fi];
                    if (f->kind != CONS_FOREIGN || strcasecmp(f->ref_table, T->name)) continue;
                    int pcols[SV_MAX_KEY_COLS];
                    if (!fk_ref_cols(f, T, pcols)) continue;
                    char okey[KEY_BUF], nkey[KEY_BUF];
                    if (!tuple_key(T, cs->v[i].old, pcols, f->ncols, okey, sizeof(okey))) continue;
                    bool is_del = cs->v[i].nt == NULL;
                    if (!is_del && tuple_key(T, cs->v[i].nt, pcols, f->ncols, nkey, sizeof(nkey)) && !strcmp(okey, nkey)) continue;
                    int fcols[SV_MAX_KEY_COLS];
                    for (int k = 0; k < f->ncols; k++) fcols[k] = f->cols[k];
                    MemIndex* cix = mi_get_cols(cs->x, db->name, C, fcols, f->ncols);
                    if (!cix) { fail = "Out of memory (foreign key index)"; break; }
                    const RidList* l = mi_lookup(cix, okey);
                    int nl = l ? l->n : 0;
                    RID* rids = nl ? sv_malloc(sizeof(RID) * (size_t)nl) : NULL;
                    if (nl && !rids) { fail = "Out of memory"; break; }
                    if (nl) memcpy(rids, l->r, sizeof(RID) * (size_t)nl);
                    int action = is_del ? f->on_delete : f->on_update;
                    for (int q = 0; q < nl && !fail; q++) {
                        int ex = cs_find(cs, C, rids[q]);
                        if (ex >= 0 && cs->v[ex].nt == NULL) continue;          // riga già cancellata
                        if (action == FK_NO_ACTION || action == FK_RESTRICT) {
                            if (nrs >= caprs) {
                                int nc = caprs ? caprs * 2 : 16;
                                Restrict* nr = sv_realloc(rs, sizeof(Restrict) * (size_t)nc);
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
            char k2[KEY_BUF];
            if (tuple_key(P, cs->v[ex].nt, pcols, n, k2, sizeof(k2)) && !strcmp(k2, key)) return 1;
        }
    }
    for (int i = 0; i < cs->n; i++) {                           // righe modificate che assumono la chiave
        if (cs->v[i].ts != P || !cs->v[i].nt) continue;
        char k2[KEY_BUF];
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
// le righe grandi vanno in pagine di overflow (overflow.h) fino a OV_MAX_ROW: controllo prima di
// scrivere, così lo statement fallisce senza lasciare modifiche a metà
#define SV_MAX_ROW_BYTES OV_MAX_ROW
static const char* row_fits(const TableSchema* ts, const Tuple* t, char* msg, size_t mn) {
    bool too_long = false;
    size_t sz = tuple_serialized_size(t, ts, &too_long);
    if (too_long) { snprintf(msg, mn, "A text value in table '%.64s' is longer than %d MB", ts->name, (int)(SV_MAX_TEXT_BYTES >> 20)); return msg; }
    if (sz > SV_MAX_ROW_BYTES) {
        snprintf(msg, mn, "Row too large for table '%.64s' (%llu bytes, at most %d MB per row)", ts->name, (unsigned long long)sz, (int)(SV_MAX_ROW_BYTES >> 20));
        return msg;
    }
    for (int c = 0; c < ts->num_columns && c < t->num_values; c++) {          // UNSIGNED: niente negativi
        const Column* col = &ts->columns[c];
        const Value* v = &t->values[c];
        long long lo, hi;                                // TINYINT / SMALLINT / MEDIUMINT: su ogni percorso di scrittura
        if (!v->is_null && col->type == SV_TYPE_INT && narrow_int_bounds(col, &lo, &hi) && (v->int_val < lo || v->int_val > hi)) {
            snprintf(msg, mn, "Value %lld is out of range for column '%.64s' (%.40s)", (long long)v->int_val, col->name, col->sqltype);
            return msg;                                  // (prima INSERT ... VALUES (200) entrava in un TINYINT)
        }
        // INT UNSIGNED si conserva come BIGINT: al più 2^32 - 1 (prima 4294967296 entrava)
        if (!v->is_null && col->type == SV_TYPE_BIGINT && (col->flags & COLF_UNSIGNED) &&
            (!strncasecmp(col->sqltype, "int", 3) && !isalnum((unsigned char)col->sqltype[3]) ? true :
             !strncasecmp(col->sqltype, "integer", 7) && !isalnum((unsigned char)col->sqltype[7])) &&
            (uint64_t)v->bigint_val > 4294967295ULL) {
            snprintf(msg, mn, "Value %llu is out of range for column '%.64s' (%.40s)", (unsigned long long)(uint64_t)v->bigint_val, col->name, col->sqltype);
            return msg;
        }
        if (!(col->flags & COLF_UNSIGNED) || v->is_null) continue;
        // BIGINT UNSIGNED: i 64 bit sono il valore senza segno (oltre 2^63 i bit sono "negativi"); il testo
        // negativo è già stato rifiutato da check_value
        if (col->type == SV_TYPE_BIGINT) continue;
        double x = col->type == SV_TYPE_INT ? v->int_val :
                   col->type == SV_TYPE_FLOAT ? v->float_val : col->type == SV_TYPE_DECIMAL && v->str_val ? strtod(v->str_val, NULL) : 0;
        if (x < 0) { snprintf(msg, mn, "Value %g is out of range for column '%.64s' (UNSIGNED)", x, col->name); return msg; }
    }
    return NULL;
}

static const char* cs_validate(ChangeSet* cs, char* msg, size_t mn) {
    Database* db = cs->db;
    const TableSchema* seen[64];                     // tabelle già controllate (oltre 64: ricontrollate, stesso esito)
    int nseen = 0;
    for (int i = 0; i < cs->n; i++) {
        TableSchema* T = cs->v[i].ts;
        bool done = false;
        for (int k = 0; k < nseen; k++) if (seen[k] == T) done = true;
        if (done) continue;
        if (nseen < 64) seen[nseen++] = T;
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
            fail = row_fits(T, ch->nt, msg, mn);
            for (int c = 0; c < T->num_columns && !fail; c++)
                if (ch->nt->values[c].is_null && (!T->columns[c].nullable || T->columns[c].primary_key)) {
                    snprintf(msg, mn, "Column '%.64s' cannot be NULL", T->columns[c].name);
                    fail = msg;
                }
            if (!fail) fail = checks_eval(&ck, ch->nt, msg, mn);
            // chiavi: due righe finali con la stessa chiave
            for (int k = 0; k < nk && !fail; k++) {
                char nkey[KEY_BUF], okey[KEY_BUF];
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
            // chiavi esterne della riga modificata (SET FOREIGN_KEY_CHECKS = 0: non controllate, come MySQL)
            for (int fi = 0; g_fk_checks && fi < T->num_constraints && !fail; fi++) {
                const Constraint* f = &T->constraints[fi];
                if (f->kind != CONS_FOREIGN) continue;
                int fc[SV_MAX_KEY_COLS];
                for (int k = 0; k < f->ncols; k++) fc[k] = f->cols[k];
                char nkey[KEY_BUF], okey[KEY_BUF];
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
            // stesso slot (UPDATE sul posto, P1.5): LOG_UPDATE; altrimenti un solo record con le due posizioni
            // (undo e redo lavorano per posizione; un record solo è atomico anche dopo un crash)
            if (nrid.page_id == ch->rid.page_id && nrid.slot_id == ch->rid.slot_id) wal_log(cs->x, LOG_UPDATE, nrid, ch->nt, ch->old, T);
            else wal_log_move(cs->x, ch->rid, nrid, ch->nt, ch->old, T);
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
// ultimo conflitto di chiave di un INSERT (per INSERT IGNORE / REPLACE / ON DUPLICATE KEY UPDATE):
// colonne della chiave e valori della riga proposta (testo; NULL = SQL NULL)
static _Thread_local struct {
    bool  set;
    int   nkey, keycols[SV_MAX_KEY_COLS];
    int   ncols;
    char* text[SV_MAX_COLUMNS];
} g_dup;

static void dup_clear(void) {
    for (int i = 0; i < g_dup.ncols; i++) { free(g_dup.text[i]); g_dup.text[i] = NULL; }
    g_dup.ncols = 0;
    g_dup.set = false;
}

static void dup_record(const TableSchema* ts, const Tuple* row, const int* cols, int n) {
    dup_clear();
    g_dup.set = true;
    g_dup.nkey = n;
    for (int k = 0; k < n; k++) g_dup.keycols[k] = cols[k];
    g_dup.ncols = ts->num_columns;
    for (int c = 0; c < ts->num_columns; c++) {
        if (row->values[c].is_null) continue;
        EvalVal ev;
        ev_from_value(&ev, &row->values[c], ts->columns[c].type);
        g_dup.text[c] = sv_strdup(ev_s(&ev));
    }
}

static const char* validate_inserts(ExecCtx* ctx, Database* db, TableSchema* ts, Tuple** rows, int nrows, char* msg, size_t mn) {
    Checks ck;
    if (checks_prepare(&ck, ts, msg, mn)) return msg;
    const char* fail = NULL;
    for (int r = 0; r < nrows && !fail; r++) fail = row_fits(ts, rows[r], msg, mn);
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
            char key[KEY_BUF];
            if (!tuple_key(ts, rows[r], keys[k].cols, keys[k].n, key, sizeof(key))) {
                if (keys[k].primary) { snprintf(msg, mn, "Primary key column cannot be NULL"); fail = msg; }
                continue;
            }
            const RidList* l = mi_lookup(ix, key);
            SMSlot* sl = sm_slot(&batch, key, true);
            if (!sl) { fail = "Out of memory"; break; }
            if ((l && l->n > 0) || sl->val >= 0) {
                dup_message(ts, &keys[k], key, msg, mn);
                dup_record(ts, rows[r], keys[k].cols, keys[k].n);
                fail = msg;
                break;
            }
            sl->val = r;
        }
        sm_free(&batch);
    }
    for (int fi = 0; g_fk_checks && fi < ts->num_constraints && !fail; fi++) {   // FOREIGN_KEY_CHECKS = 0: niente
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
                char key[KEY_BUF];
                if (tuple_key(ts, rows[r], pcols, f->ncols, key, sizeof(key)) && !sm_slot(&own, key, true)) { fail = "Out of memory"; break; }
            }
        for (int r = 0; r < nrows && !fail; r++) {
            char key[KEY_BUF];
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
        if (ci > 255) {                  // le posizioni dei vincoli sono byte (catalogo v6): mai troncate
            snprintf(msg, mn, "Column '%.64s' is column %d: keys and constraints can use only the first 256 columns of a table", names[k], ci + 1);
            return msg;
        }
        out[k] = (uint8_t)ci;
    }
    return NULL;
}

// aggiunge a ts il vincolo d (parent: la tabella riferita, o ts stessa se si riferisce a sé)
static const char* add_constraint(ExecCtx* ctx, Database* db, TableSchema* ts, ConstraintDef* d, char* msg, size_t mn) {
    if (ts->num_constraints >= SV_MAX_CONSTRAINTS) return "Too many constraints on the table (max 64)";
    if (!sv_name_ok_opt(d->name)) return "Invalid constraint name: it cannot contain a backtick (`) or a control character";
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
        if (!P && (g_fk_checks || d->nref != d->ncols)) { snprintf(msg, mn, "Referenced table '%.64s' not found", d->ref_table); return msg; }
        if (!P) {
            // SET FOREIGN_KEY_CHECKS = 0 (dump di MySQL): la tabella riferita viene creata più avanti; il
            // vincolo resta per nome e vale quando c'è (come in MySQL)
            snprintf(c.ref_table, sizeof(c.ref_table), "%s", d->ref_table);
            for (int k = 0; k < d->ncols; k++) snprintf(c.ref_cols[k], sizeof(c.ref_cols[k]), "%s", d->ref_cols[k]);
            c.on_delete = (uint8_t)d->on_delete;
            c.on_update = (uint8_t)d->on_update;
        } else {
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
    }

    // i dati esistenti devono già rispettare il vincolo (ALTER TABLE ADD CONSTRAINT); con
    // FOREIGN_KEY_CHECKS = 0 le chiavi esterne non si verificano (come in MySQL)
    if ((ts->num_rows > 0 || ts->root_page_id) && !(d->kind == CONS_FOREIGN && !g_fk_checks)) {
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
            char key[KEY_BUF];
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

    Constraint* slot = schema_add_constraint(ts);
    if (!slot) return "Too many constraints on the table (max 64) or out of memory";
    *slot = c;
    if (d->kind == CONS_PRIMARY)
        for (int k = 0; k < c.ncols; k++) { ts->columns[c.cols[k]].primary_key = true; ts->columns[c.cols[k]].nullable = false; }
    if (d->kind == CONS_UNIQUE && c.ncols == 1) ts->columns[c.cols[0]].unique = true;
    mi_invalidate_table(db->name, ts->name);
    return NULL;
}

// la tabella ts è riferita da una chiave esterna di un'altra tabella? (nome del vincolo)
static const Constraint* referenced_by(Database* db, const TableSchema* ts, const TableSchema** child) {
    for (int i = 0; i < db->num_tables; i++) {
        const TableSchema* C = db->tables[i];
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
#define RT_MAX_HANDLERS   32
#define RT_NOT_FOUND      "No data - zero rows fetched, selected, or processed"

enum { CTL_NONE, CTL_LEAVE, CTL_ITERATE, CTL_RETURN, CTL_EXIT };

typedef struct { char name[64]; ASTNode* sql; QueryResult* res; int pos; bool open; } RtCursor;
typedef struct { TrigStmt* h; int level; } RtHandler;

// Ambiente di un trigger o di una routine. Le variabili locali (parametri e DECLARE) sono le colonne
// di una riga virtuale (sorgente senza nome): le espressioni e le query del corpo le vedono come
// NEW / OLD, con i tipi dichiarati.
typedef struct TrigScope {
    Source  src[3];       // NEW, OLD, variabili
    Tuple*  cur[3];
    BindCtx b;
    EvalCtx e;
    bool    proc;         // corpo di una procedura: tiene l'ultimo risultato e controlla i permessi
    bool    is_function;  // funzione: niente risultati, RETURN obbligatorio
    QueryResult* last;    // ultimo risultato con colonne (SELECT) della procedura
    int     affected;
    TableSchema* vts;     // variabili
    Tuple*  vrow;
    RtCursor* curs;
    int     ncurs;
    RtHandler hs[RT_MAX_HANDLERS];
    int     nh;
    int     level;        // profondità dei blocchi BEGIN ... END
    int     ctl;          // CTL_*: LEAVE / ITERATE / RETURN / uscita da un handler EXIT
    char    ctl_label[64];
    int     exit_level;
    EvalVal ret;
    bool    has_ret;
    char    last_err[300];  // per RESIGNAL dentro un handler
} TrigScope;

static Tuple* rt_scope_row(void* routine, int s, const TableSchema** ts) {
    TrigScope* sc = routine;
    if (!sc || s < 0 || s > 2) return NULL;
    *ts = sc->src[s].ts;
    return sc->cur[s];
}

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
    sc->vts = schema_new(0);                           // variabili: aggiunte da rt_add_var (la lista cresce)
    if (sc->vts) sc->vts->own_columns = 1;
    sc->src[2].ts = sc->vts;
    sc->src[2].name[0] = '\0';                         // non qualificabile: v, non x.v
    sc->b.src = sc->src; sc->b.nsrc = sc->vts ? 3 : 2; sc->b.mode = BIND_ROW; sc->b.x = ctx; sc->b.clause = "the trigger";
    sc->b.rt_scope = true;
    sc->e.src = sc->src; sc->e.nsrc = sc->vts ? 3 : 2; sc->e.cur = sc->cur; sc->e.x = ctx;
}

static void trig_scope_free(TrigScope* sc) {
    if (!sc) return;
    if (sc->vrow) tuple_free(sc->vrow);
    for (int i = 0; i < sc->ncurs; i++) db_result_free(sc->curs[i].res);
    free(sc->curs);
    schema_release(sc->vts);
    free(sc->vts);
    if (sc->last) db_result_free(sc->last);
    sc->vrow = NULL; sc->curs = NULL; sc->vts = NULL; sc->last = NULL;
}

// una variabile in più (parametro o DECLARE); un nome già visto è la stessa variabile
static bool rt_add_var(TrigScope* sc, const char* name, const ColumnDef* d, char* msg, size_t mn) {
    if (!sc->vts) { snprintf(msg, mn, "Out of memory"); return false; }
    if (catalog_get_col_index(sc->vts, name) >= 0) return true;
    if (sc->vts->num_columns >= SV_MAX_COLUMNS) { snprintf(msg, mn, "Too many variables in a routine (max %d)", SV_MAX_COLUMNS); return false; }
    if (d && d->type[0] && !known_type(d->type)) { snprintf(msg, mn, "Unknown type '%.32s' for variable '%.64s'", d->type, name); return false; }
    Column* c = schema_add_column(sc->vts);
    if (!c) { snprintf(msg, mn, "Out of memory"); return false; }
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->type = d && d->type[0] ? parse_type(d->type) : SV_TYPE_TEXT;
    c->size = d ? d->size : 0;
    c->nullable = true;
    if (d) snprintf(c->sqltype, sizeof(c->sqltype), "%s", d->sqltype);
    if (c->type == SV_TYPE_DECIMAL && d) c->decimal_prec = d->has_scale ? d->scale + 1 : d->size > 0 ? 1 : 0;
    return true;
}

// le DECLARE di tutto il corpo (anche dei blocchi annidati), prima di eseguire
static bool rt_collect_vars(TrigScope* sc, TrigStmt** v, int n, char* msg, size_t mn) {
    for (int i = 0; i < n; i++) {
        TrigStmt* t = v[i];
        if (t->kind == TS_DECLARE)
            for (int k = 0; k < t->nnames; k++) if (!rt_add_var(sc, t->names[k], t->vdef, msg, mn)) return false;
        if (!rt_collect_vars(sc, t->then_v, t->nthen, msg, mn) || !rt_collect_vars(sc, t->else_v, t->nelse, msg, mn)) return false;
    }
    return true;
}

// riga dei valori (tutti NULL) dopo aver raccolto le variabili
static bool rt_vars_ready(TrigScope* sc) {
    if (!sc->vts) return false;
    sc->vrow = tuple_create(sc->vts->num_columns > 0 ? sc->vts->num_columns : 1);
    if (!sc->vrow) return false;
    for (int i = 0; i < sc->vrow->num_values; i++) sc->vrow->values[i].is_null = true;
    sc->cur[2] = sc->vrow;
    return true;
}

// valore di una variabile locale (testo NULL = SQL NULL)
static bool rt_set_var(TrigScope* sc, const char* name, const EvalVal* v) {
    int ci = sc && sc->vts ? catalog_get_col_index(sc->vts, name) : -1;
    if (ci < 0 || !sc->vrow) return false;
    Column* c = &sc->vts->columns[ci];
    char nb[64];
    const char* tx = v->null ? NULL : ev_s(v);
    if (!v->null && v->is_num && !v->is_int && (c->type == SV_TYPE_INT || c->type == SV_TYPE_BIGINT)) {
        snprintf(nb, sizeof(nb), "%lld", (long long)llround(v->num));   // intero: arrotondato come MySQL
        tx = nb;
    }
    if (c->type == SV_TYPE_BOOL && tx && v->is_num) { snprintf(nb, sizeof(nb), "%d", v->num != 0); tx = nb; }
    column_set_from_text(&sc->vrow->values[ci], c, tx);
    return true;
}

static void uservar_set(const char* name, const EvalVal* v);

// destinazione di SET / INTO / FETCH: @x, variabile locale, NEW.colonna
static const char* rt_assign(ExecCtx* ctx, TrigScope* sc, TableSchema* ts, Tuple* newt, const char* name, const EvalVal* v,
                             char* msg, size_t mn) {
    (void)ctx;
    if (name[0] == '@') { uservar_set(name + 1, v); return NULL; }
    if (!strncasecmp(name, "NEW.", 4)) {
        int ci = ts ? catalog_get_col_index(ts, name + 4) : -1;
        if (ci < 0 || !newt) { snprintf(msg, mn, "Cannot set %.64s here", name); return msg; }
        const char* tx = v->null ? NULL : ev_s(v);
        if (tx && check_value(&ts->columns[ci], tx, msg, mn)) return msg;
        column_set_from_text(&newt->values[ci], &ts->columns[ci], tx);
        return NULL;
    }
    if (!rt_set_var(sc, name, v)) { snprintf(msg, mn, "Undeclared variable: %.64s", name); return msg; }
    return NULL;
}

// CREATE / DROP TABLE nel corpo (non ammessi nei trigger e nelle funzioni)
static bool trig_has_ddl(TrigStmt** v, int n) {
    for (int i = 0; i < n; i++) {
        if (v[i]->kind == TS_SQL && v[i]->sql && (v[i]->sql->type == NODE_CREATE_TABLE || v[i]->sql->type == NODE_DROP_TABLE)) return true;
        if (trig_has_ddl(v[i]->then_v, v[i]->nthen) || trig_has_ddl(v[i]->else_v, v[i]->nelse)) return true;
    }
    return false;
}

static bool trig_has_set(TrigStmt** v, int n) {
    for (int i = 0; i < n; i++) {
        if (v[i]->kind == TS_SET) return true;
        if (v[i]->kind == TS_ASSIGN)
            for (int k = 0; k < v[i]->nnames; k++) if (!strncasecmp(v[i]->names[k], "NEW.", 4)) return true;
        if (trig_has_set(v[i]->then_v, v[i]->nthen) || trig_has_set(v[i]->else_v, v[i]->nelse)) return true;
    }
    return false;
}

// controlla le espressioni del corpo (WHEN, SET, IF, cicli, RETURN, ...) contro NEW / OLD / variabili
static const char* trig_bind_all(TrigScope* sc, TableSchema* ts, TrigStmt** v, int n, char* msg, size_t mn) {
    for (int i = 0; i < n; i++) {
        TrigStmt* t = v[i];
        sc->b.err[0] = '\0';
        switch (t->kind) {
            case TS_SET:
                if (catalog_get_col_index(ts, t->col) < 0) { snprintf(msg, mn, "Unknown column 'NEW.%.64s'", t->col); return msg; }
                bind_expr(t->expr, &sc->b);
                break;
            case TS_ASSIGN:
                for (int k = 0; k < t->nnames; k++) {
                    const char* nm = t->names[k];
                    if (nm[0] != '@' && strncasecmp(nm, "NEW.", 4) && catalog_get_col_index(sc->vts, nm) < 0) {
                        snprintf(msg, mn, "Undeclared variable: %.64s", nm);
                        return msg;
                    }
                    if (!strncasecmp(nm, "NEW.", 4) && catalog_get_col_index(ts, nm + 4) < 0) { snprintf(msg, mn, "Unknown column '%.64s'", nm); return msg; }
                    bind_expr(t->exprs[k], &sc->b);
                }
                break;
            case TS_FETCH:
                for (int k = 0; k < t->nnames; k++)
                    if (t->names[k][0] != '@' && strncasecmp(t->names[k], "NEW.", 4) && catalog_get_col_index(sc->vts, t->names[k]) < 0) {
                        snprintf(msg, mn, "Undeclared variable: %.64s", t->names[k]);
                        return msg;
                    }
                break;
            case TS_SIGNAL: case TS_RETURN: case TS_DECLARE:
                bind_expr(t->expr, &sc->b);
                break;
            case TS_IF: case TS_WHILE: case TS_REPEAT:
                bind_expr(t->cond, &sc->b);
                break;
            default: break;
        }
        if (sc->b.err[0]) { snprintf(msg, mn, "%.250s", sc->b.err); return msg; }
        if (t->kind == TS_RETURN && !sc->is_function) { snprintf(msg, mn, "RETURN is only allowed in a FUNCTION"); return msg; }
        if (trig_bind_all(sc, ts, t->then_v, t->nthen, msg, mn)) return msg;
        if (trig_bind_all(sc, ts, t->else_v, t->nelse, msg, mn)) return msg;
    }
    return NULL;
}

static RtCursor* rt_cursor(TrigScope* sc, const char* name) {
    for (int i = 0; i < sc->ncurs; i++) if (!strcasecmp(sc->curs[i].name, name)) return &sc->curs[i];
    return NULL;
}

// handler per l'errore (o la condizione NOT FOUND), dal più interno; -1 se nessuno
static int rt_find_handler(TrigScope* sc, const char* err) {
    bool nf = strstr(err, RT_NOT_FOUND) != NULL;
    for (int i = sc->nh - 1; i >= 0; i--) {
        TrigStmt* h = sc->hs[i].h;
        for (int k = 0; k < h->nconds; k++) {
            const char* c = h->conds[k];
            if (!strcmp(c, "NOTFOUND") ? nf : !strcmp(c, "SQLWARNING") ? false : !nf) return i;
        }
    }
    return -1;
}

static const char* trig_run(ExecCtx* ctx, TrigScope* sc, TableSchema* ts, TrigStmt** v, int n, Tuple* newt,
                            char* msg, size_t mn);

// un'istruzione
static const char* rt_exec(ExecCtx* ctx, TrigScope* sc, TableSchema* ts, TrigStmt* t, Tuple* newt, char* msg, size_t mn) {
    sc->e.err[0] = '\0';
    switch (t->kind) {
        case TS_NOOP: case TS_CURSOR: case TS_HANDLER: return NULL;   // registrati da trig_run
        case TS_SQL: {
            if (sc->proc && ctx->auth_hook && !ctx->auth_hook(t->sql, msg, mn)) return msg;
            ctx->rt_notfound = false;
            QueryResult* r = executor_run(ctx, t->sql);
            if (!r || !r->success) {
                snprintf(msg, mn, "%.280s", r ? r->error : "statement failed");
                db_result_free(r);
                return msg;
            }
            if (sc->is_function && r->num_cols > 0) {
                db_result_free(r);
                snprintf(msg, mn, "Not allowed to return a result set from a function (use SELECT ... INTO)");
                return msg;
            }
            if (sc->proc) {
                if (t->sql->type != NODE_SELECT) sc->affected += r->rows_affected;
                if (r->num_cols > 0) {                       // il risultato di CALL è l'ultima SELECT
                    if (sc->last) db_result_free(sc->last);
                    sc->last = r;
                    r = NULL;
                }
            }
            db_result_free(r);
            if (ctx->rt_notfound) { ctx->rt_notfound = false; snprintf(msg, mn, "%s", RT_NOT_FOUND); return msg; }
            return NULL;
        }
        case TS_SET: {
            int ci = catalog_get_col_index(ts, t->col);
            if (ci < 0 || !newt) { snprintf(msg, mn, "Cannot set NEW.%.64s here", t->col); return msg; }
            EvalVal v2;
            eval_expr(t->expr, &sc->e, &v2);
            if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
            const char* tx = v2.null ? NULL : ev_s(&v2);     // v2 vive fino a value_set_from_text (che copia)
            if (tx && check_value(&ts->columns[ci], tx, msg, mn)) return msg;
            column_set_from_text(&newt->values[ci], &ts->columns[ci], tx);
            return NULL;
        }
        case TS_ASSIGN:
            for (int k = 0; k < t->nnames; k++) {
                EvalVal v2;
                eval_expr(t->exprs[k], &sc->e, &v2);
                if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
                const char* f = rt_assign(ctx, sc, ts, newt, t->names[k], &v2, msg, mn);
                if (f) return f;
            }
            return NULL;
        case TS_DECLARE: {
            EvalVal v2;
            if (t->expr) eval_expr(t->expr, &sc->e, &v2);
            else ev_null(&v2);
            if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
            for (int k = 0; k < t->nnames; k++) rt_set_var(sc, t->names[k], &v2);
            return NULL;
        }
        case TS_SIGNAL: {
            if (!t->expr) {                                    // RESIGNAL
                snprintf(msg, mn, "%.280s", sc->last_err[0] ? sc->last_err : "RESIGNAL outside of a handler");
                return msg;
            }
            EvalVal v2;
            eval_expr(t->expr, &sc->e, &v2);
            snprintf(msg, mn, "%.280s", v2.null ? "Error raised by a trigger" : ev_s(&v2));
            return msg;
        }
        case TS_RETURN: {
            EvalVal v2;
            eval_expr(t->expr, &sc->e, &v2);
            if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
            ev_copy(&sc->ret, &v2);
            if (!sc->ret.null && !sc->ret.is_num && sc->ret.str && sc->ret.str != sc->ret.buf) {   // testo che non vive oltre: copia
                size_t l = strlen(sc->ret.str);
                if (l > EV_STR_MAX) l = EV_STR_MAX;
                char tmp[EV_STR_MAX + 1];
                memcpy(tmp, sc->ret.str, l); tmp[l] = '\0';
                ev_text(&sc->ret, tmp, l);
            }
            sc->has_ret = true;
            sc->ctl = CTL_RETURN;
            return NULL;
        }
        case TS_IF: {
            bool c = eval_cond(t->cond, &sc->e);
            if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
            return c ? trig_run(ctx, sc, ts, t->then_v, t->nthen, newt, msg, mn)
                     : trig_run(ctx, sc, ts, t->else_v, t->nelse, newt, msg, mn);
        }
        case TS_BLOCK: {
            sc->level++;
            int my = sc->level;
            const char* f = trig_run(ctx, sc, ts, t->then_v, t->nthen, newt, msg, mn);
            sc->level--;
            if (f) return f;
            if (sc->ctl == CTL_LEAVE && t->label[0] && !strcasecmp(sc->ctl_label, t->label)) sc->ctl = CTL_NONE;
            if (sc->ctl == CTL_EXIT && sc->exit_level == my) sc->ctl = CTL_NONE;
            return NULL;
        }
        case TS_WHILE: case TS_REPEAT: case TS_LOOP: {
            for (long long iter = 0;; iter++) {
                if ((iter & 255) == 0 && sv_scan_cancelled()) { snprintf(msg, mn, "Query cancelled"); return msg; }
                if (t->kind == TS_WHILE) {
                    bool c = eval_cond(t->cond, &sc->e);
                    if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
                    if (!c) break;
                }
                const char* f = trig_run(ctx, sc, ts, t->then_v, t->nthen, newt, msg, mn);
                if (f) return f;
                if (sc->ctl == CTL_ITERATE && !strcasecmp(sc->ctl_label, t->label)) { sc->ctl = CTL_NONE; continue; }
                if (sc->ctl == CTL_LEAVE && !strcasecmp(sc->ctl_label, t->label)) { sc->ctl = CTL_NONE; break; }
                if (sc->ctl) break;                            // RETURN, uscita verso un'etichetta esterna
                if (t->kind == TS_REPEAT) {
                    bool c = eval_cond(t->cond, &sc->e);
                    if (sc->e.err[0]) { snprintf(msg, mn, "%.250s", sc->e.err); return msg; }
                    if (c) break;
                }
            }
            return NULL;
        }
        case TS_LEAVE: case TS_ITERATE:
            sc->ctl = t->kind == TS_LEAVE ? CTL_LEAVE : CTL_ITERATE;
            snprintf(sc->ctl_label, sizeof(sc->ctl_label), "%s", t->label);
            return NULL;
        case TS_OPEN: {
            RtCursor* c = rt_cursor(sc, t->label);
            if (!c) { snprintf(msg, mn, "Undefined CURSOR: %.64s", t->label); return msg; }
            if (c->open) { snprintf(msg, mn, "Cursor is already open: %.64s", t->label); return msg; }
            if (sc->proc && ctx->auth_hook && !ctx->auth_hook(c->sql, msg, mn)) return msg;
            QueryResult* r = executor_run(ctx, c->sql);
            if (!r || !r->success) { snprintf(msg, mn, "%.280s", r ? r->error : "statement failed"); db_result_free(r); return msg; }
            c->res = r; c->pos = 0; c->open = true;
            return NULL;
        }
        case TS_FETCH: {
            RtCursor* c = rt_cursor(sc, t->label);
            if (!c) { snprintf(msg, mn, "Undefined CURSOR: %.64s", t->label); return msg; }
            if (!c->open) { snprintf(msg, mn, "Cursor is not open: %.64s", t->label); return msg; }
            if (t->nnames != c->res->num_cols) { snprintf(msg, mn, "Incorrect number of FETCH variables"); return msg; }
            if (c->pos >= c->res->num_rows) { snprintf(msg, mn, "%s", RT_NOT_FOUND); return msg; }
            for (int k = 0; k < t->nnames; k++) {
                const Cell* cell = &c->res->rows[c->pos].cells[k];
                EvalVal v2;
                if (cell->is_null) ev_null(&v2);
                else { v2.null = false; v2.is_num = v2.is_int = false; v2.ci = false; v2.is_dec = false; v2.str = cell->value ? cell->value : ""; }
                const char* f = rt_assign(ctx, sc, ts, newt, t->names[k], &v2, msg, mn);
                if (f) return f;
            }
            c->pos++;
            return NULL;
        }
        case TS_CLOSE: {
            RtCursor* c = rt_cursor(sc, t->label);
            if (!c) { snprintf(msg, mn, "Undefined CURSOR: %.64s", t->label); return msg; }
            if (!c->open) { snprintf(msg, mn, "Cursor is not open: %.64s", t->label); return msg; }
            db_result_free(c->res);
            c->res = NULL; c->open = false;
            return NULL;
        }
    }
    return NULL;
}

// esegue una lista di istruzioni: dichiarazioni di cursori e handler, errori passati agli handler,
// LEAVE / ITERATE / RETURN propagati con sc->ctl
static const char* trig_run(ExecCtx* ctx, TrigScope* sc, TableSchema* ts, TrigStmt** v, int n, Tuple* newt,
                            char* msg, size_t mn) {
    int hs_base = sc->nh;
    const char* fail = NULL;
    for (int i = 0; i < n && !fail; i++) {
        TrigStmt* t = v[i];
        if (t->kind == TS_HANDLER) {
            if (sc->nh >= RT_MAX_HANDLERS) { snprintf(msg, mn, "Too many handlers"); fail = msg; break; }
            sc->hs[sc->nh].h = t;
            sc->hs[sc->nh].level = sc->level;
            sc->nh++;
            continue;
        }
        if (t->kind == TS_CURSOR) {
            RtCursor* c = rt_cursor(sc, t->label);
            if (!c) {
                RtCursor* nc = sv_realloc(sc->curs, sizeof(RtCursor) * (size_t)(sc->ncurs + 1));
                if (!nc) { snprintf(msg, mn, "Out of memory"); fail = msg; break; }
                sc->curs = nc;
                c = &sc->curs[sc->ncurs++];
                memset(c, 0, sizeof(*c));
                snprintf(c->name, sizeof(c->name), "%s", t->label);
            }
            c->sql = t->sql;
            continue;
        }
        const char* f = rt_exec(ctx, sc, ts, t, newt, msg, mn);
        if (f) {
            int h = rt_find_handler(sc, f);
            if (h < 0) { fail = f; break; }
            RtHandler hd = sc->hs[h];
            char saved[300];
            snprintf(saved, sizeof(saved), "%s", sc->last_err);
            snprintf(sc->last_err, sizeof(sc->last_err), "%.290s", f);
            int nh_save = sc->nh;
            sc->nh = h;                                        // dentro l'handler valgono solo quelli più esterni
            const char* f2 = trig_run(ctx, sc, ts, hd.h->then_v, hd.h->nthen, newt, msg, mn);
            sc->nh = nh_save;
            snprintf(sc->last_err, sizeof(sc->last_err), "%s", saved);
            if (f2) { fail = f2; break; }
            if (hd.h->exit_handler && !sc->ctl) { sc->ctl = CTL_EXIT; sc->exit_level = hd.level; }
        }
        if (sc->ctl) break;
    }
    sc->nh = hs_base;
    return fail;
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
            TrigScope* sc = fail ? NULL : sv_malloc(sizeof(TrigScope));
            if (!fail && !sc) fail = "Out of memory";
            if (sc) {
                trig_scope_init(sc, ctx, ts, row ? newt : NULL, row ? oldt : NULL);
                if (!rt_collect_vars(sc, prog->v, prog->n, msg, mn)) fail = msg;
                else if (!rt_vars_ready(sc)) fail = "Out of memory";
                bool go = !fail;
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
                    void* sr = ctx->routine;
                    ctx->trig_bind = &sc->b;
                    ctx->trig_eval = &sc->e;
                    ctx->routine = sc;
                    ctx->trig_depth++;
                    fail = trig_run(ctx, sc, ts, prog->v, prog->n, before ? newt : NULL, msg, mn);
                    ctx->trig_depth--;
                    ctx->trig_bind = sb;
                    ctx->trig_eval = se;
                    ctx->routine = sr;
                }
                trig_scope_free(sc);
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
    TokenList* tk = lexer_tokenize(n->create_trigger.body);
    TrigProgram* prog = tk ? parser_parse_trigger(tk) : NULL;
    const char* fail = NULL;
    if (!prog || !prog->success) { snprintf(msg, sizeof(msg), "Invalid trigger: %.250s", prog ? prog->error : "out of memory"); fail = msg; }
    if (!fail && trig_has_ddl(prog->v, prog->n)) fail = "A trigger cannot create or drop tables";
    if (!fail && trig_has_set(prog->v, prog->n) && (!n->create_trigger.before || !prog->for_each_row))
        fail = "SET NEW.column is only allowed in BEFORE ... FOR EACH ROW triggers";
    if (!fail && trig_has_set(prog->v, prog->n) && !strcasecmp(n->create_trigger.event, "DELETE"))
        fail = "A DELETE trigger has no NEW row to change";
    for (int k = 0; !fail && prog && k < prog->n_of; k++)
        if (catalog_get_col_index(ts, prog->of_cols[k]) < 0) { snprintf(msg, sizeof(msg), "Unknown column '%.64s' after OF", prog->of_cols[k]); fail = msg; }
    if (!fail) {
        TrigScope* sc = sv_malloc(sizeof(TrigScope));
        if (!sc) fail = "Out of memory";
        else {
            trig_scope_init(sc, ctx, ts, NULL, NULL);
            if (!rt_collect_vars(sc, prog->v, prog->n, msg, sizeof(msg))) fail = msg;
            if (!fail && prog->when) { bind_expr(prog->when, &sc->b); if (sc->b.err[0]) { snprintf(msg, sizeof(msg), "%.250s", sc->b.err); fail = msg; } }
            if (!fail) fail = trig_bind_all(sc, ts, prog->v, prog->n, msg, sizeof(msg));
            trig_scope_free(sc);
            free(sc);
        }
    }
    trig_program_free(prog);
    lexer_free(tk);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    if (!sv_name_ok(n->create_trigger.name)) return bad_name_error("trigger");
    TriggerSchema* tg = catalog_new_trigger(db);            // lista dinamica (P1.11: prima 64 al massimo)
    if (!tg) return result_error_msg("Out of memory");
    snprintf(tg->name, sizeof(tg->name), "%s", n->create_trigger.name);
    snprintf(tg->table_name, sizeof(tg->table_name), "%s", ts->name);
    tg->before = n->create_trigger.before;
    snprintf(tg->event, sizeof(tg->event), "%s", n->create_trigger.event);
    snprintf(tg->body, sizeof(tg->body), "%s", n->create_trigger.body);
    return result_ok_msg(0);
}

// ═════════════════════════════════════════════════════════════════
//  Stored procedure e funzioni (come MySQL):
//    CREATE PROCEDURE nome([IN | OUT | INOUT] p tipo, ...) corpo        CALL nome(valori, @out, ...)
//    CREATE FUNCTION nome(p tipo, ...) RETURNS tipo [DETERMINISTIC ...] corpo     SELECT nome(x)
//  Il corpo è un'istruzione o BEGIN ... END con variabili, cicli, cursori e handler (vedi parser.h).
//  I parametri e le DECLARE sono variabili tipizzate; il risultato di CALL è quello dell'ultima SELECT.
// ═════════════════════════════════════════════════════════════════

static TableSchema g_no_table;                 // NEW / OLD di una routine: nessuna colonna

static RoutineSchema* find_routine(Database* db, const char* name, bool is_function) {
    for (int i = 0; db && i < db->num_routines; i++)
        if (db->routines[i].is_function == is_function && !strcasecmp(db->routines[i].name, name)) return &db->routines[i];
    return NULL;
}

static RoutineSchema* find_procedure(Database* db, const char* name) { return find_routine(db, name, false); }

static bool trig_has_return(TrigStmt** v, int n) {
    for (int i = 0; i < n; i++)
        if (v[i]->kind == TS_RETURN || trig_has_return(v[i]->then_v, v[i]->nthen) || trig_has_return(v[i]->else_v, v[i]->nelse)) return true;
    return false;
}

static RoutineDef* routine_parse(const RoutineSchema* rt, TokenList** tk_out, char* msg, size_t mn) {
    TokenList* tk = lexer_tokenize(rt->body);
    if (!tk) { snprintf(msg, mn, "Out of memory"); return NULL; }
    RoutineDef* d = parser_parse_routine(tk, rt->is_function);
    if (!d || !d->success) {
        snprintf(msg, mn, "%s '%.64s' cannot be read: %.180s", rt->is_function ? "Function" : "Procedure", rt->name, d ? d->error : "out of memory");
        routine_def_free(d);
        lexer_free(tk);
        return NULL;
    }
    *tk_out = tk;
    return d;
}

// prepara l'ambiente (parametri con i valori, DECLARE) ed esegue il corpo. Ritorna NULL o l'errore;
// sc resta da leggere (OUT, risultato, RETURN) e da liberare con trig_scope_free
static const char* routine_run(ExecCtx* ctx, const RoutineSchema* rt, RoutineDef* d, const EvalVal* argv, TrigScope* sc,
                               char* msg, size_t mn) {
    trig_scope_init(sc, ctx, &g_no_table, NULL, NULL);       // sempre: chi chiama libera sc
    if (ctx->trig_depth >= MAX_TRIGGER_DEPTH)
        return rt->is_function ? "Functions are nested too deeply (a function keeps calling itself?)"
                               : "Procedures are nested too deeply (a procedure keeps calling itself?)";
    sc->proc = true;
    sc->is_function = rt->is_function;
    sc->b.clause = rt->is_function ? "the function" : "the procedure";
    for (int k = 0; k < d->nparams; k++) if (!rt_add_var(sc, d->params[k].name, &d->params[k].def, msg, mn)) return msg;
    if (!rt_collect_vars(sc, d->prog->v, d->prog->n, msg, mn)) return msg;
    if (!rt_vars_ready(sc)) return "Out of memory";
    for (int k = 0; k < d->nparams; k++)
        if (d->params[k].mode != PARAM_OUT) rt_set_var(sc, d->params[k].name, &argv[k]);
    const char* fail = trig_bind_all(sc, &g_no_table, d->prog->v, d->prog->n, msg, mn);
    if (fail) return fail;
    struct BindCtx* sb = ctx->trig_bind;
    struct EvalCtx* se = ctx->trig_eval;
    void* sr = ctx->routine;
    ctx->trig_bind = &sc->b;
    ctx->trig_eval = &sc->e;
    ctx->routine = sc;
    ctx->trig_depth++;
    fail = trig_run(ctx, sc, &g_no_table, d->prog->v, d->prog->n, NULL, msg, mn);
    ctx->trig_depth--;
    ctx->trig_bind = sb;
    ctx->trig_eval = se;
    ctx->routine = sr;
    if (!fail && rt->is_function && !sc->has_ret) { snprintf(msg, mn, "No RETURN found in function"); fail = msg; }
    if (!fail && sc->ctl == CTL_LEAVE && sc->ctl_label[0]) {}   // LEAVE dell'etichetta del corpo
    return fail;
}

// tabelle TEMPORARY create dentro le routine: eliminate alla fine della CALL più esterna
static _Thread_local char (*g_rt_temps)[2][64];
static _Thread_local int g_rt_ntemps;

static QueryResult* exec_drop_table(ExecCtx* ctx, ASTNode* n);

static void rt_temps_cleanup(ExecCtx* ctx) {
    if (!g_rt_ntemps) return;
    ASTNode* dn = sv_calloc(1, sizeof(ASTNode));
    int saved = ctx->catalog->current_db;
    for (int i = 0; dn && i < g_rt_ntemps; i++) {
        for (int d = 0; d < ctx->catalog->num_databases; d++)
            if (!strcasecmp(ctx->catalog->databases[d].name, g_rt_temps[i][0])) { ctx->catalog->current_db = d; break; }
        if (!catalog_get_table(ctx->catalog, g_rt_temps[i][0], g_rt_temps[i][1])) continue;
        memset(dn, 0, sizeof(*dn));
        dn->type = NODE_DROP_TABLE;
        snprintf(dn->drop_table.table_name, sizeof(dn->drop_table.table_name), "%s", g_rt_temps[i][1]);
        db_result_free(exec_drop_table(ctx, dn));
    }
    ctx->catalog->current_db = saved;
    free(dn);
    free(g_rt_temps);
    g_rt_temps = NULL;
    g_rt_ntemps = 0;
}

static void rt_temp_created(const char* db, const char* table) {
    char (*nt)[2][64] = sv_realloc(g_rt_temps, sizeof(*g_rt_temps) * (size_t)(g_rt_ntemps + 1));
    if (!nt) return;
    g_rt_temps = nt;
    snprintf(g_rt_temps[g_rt_ntemps][0], 64, "%s", db);
    snprintf(g_rt_temps[g_rt_ntemps][1], 64, "%s", table);
    g_rt_ntemps++;
}

static bool rt_is_temp(const char* db, const char* table) {
    for (int i = 0; i < g_rt_ntemps; i++)
        if (!strcasecmp(g_rt_temps[i][0], db) && !strcasecmp(g_rt_temps[i][1], table)) return true;
    return false;
}

static QueryResult* exec_create_procedure(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    bool is_function = n->create_trigger.before;
    const char* what = is_function ? "Function" : "Procedure";
    char msg[300];
    if (find_routine(db, n->create_trigger.name, is_function)) {
        if (n->if_exists) return result_ok_msg(0);
        snprintf(msg, sizeof(msg), "%s '%.64s' already exists", what, n->create_trigger.name);
        return result_error_msg(msg);
    }
    RoutineSchema tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.name, sizeof(tmp.name), "%s", n->create_trigger.name);
    tmp.is_function = is_function;
    snprintf(tmp.body, sizeof(tmp.body), "%s", n->create_trigger.body);
    TokenList* tk = NULL;
    RoutineDef* d = routine_parse(&tmp, &tk, msg, sizeof(msg));
    if (!d) return result_error_msg(msg);
    const char* fail = NULL;
    if (is_function && !trig_has_return(d->prog->v, d->prog->n)) fail = "No RETURN found in function";
    if (!fail && is_function && trig_has_ddl(d->prog->v, d->prog->n)) fail = "A function cannot create or drop tables";
    if (!fail && is_function && !known_type(d->ret.type)) { snprintf(msg, sizeof(msg), "Unknown type '%.32s' after RETURNS", d->ret.type); fail = msg; }
    for (int k = 0; !fail && k < d->nparams; k++)
        if (!known_type(d->params[k].def.type)) { snprintf(msg, sizeof(msg), "Unknown type '%.32s' for parameter '%.64s'", d->params[k].def.type, d->params[k].name); fail = msg; }
    if (!fail) {                                         // variabili e dichiarazioni coerenti
        TrigScope* sc = sv_malloc(sizeof(TrigScope));
        if (!sc) fail = "Out of memory";
        else {
            trig_scope_init(sc, ctx, &g_no_table, NULL, NULL);
            for (int k = 0; !fail && k < d->nparams; k++) if (!rt_add_var(sc, d->params[k].name, &d->params[k].def, msg, sizeof(msg))) fail = msg;
            if (!fail && !rt_collect_vars(sc, d->prog->v, d->prog->n, msg, sizeof(msg))) fail = msg;
            trig_scope_free(sc);
            free(sc);
        }
    }
    char rtype[64] = "";
    if (is_function) snprintf(rtype, sizeof(rtype), "%s", d->ret.sqltype[0] ? d->ret.sqltype : d->ret.type);
    routine_def_free(d);
    lexer_free(tk);
    if (fail) {
        if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail);
        char full[400];
        snprintf(full, sizeof(full), "Invalid %s: %.300s", is_function ? "function" : "procedure", msg);
        return result_error_msg(full);
    }
    if (!sv_name_ok(tmp.name)) return bad_name_error(is_function ? "function" : "procedure");
    RoutineSchema* rt = catalog_new_routine(db);            // lista dinamica (P1.11: prima 64 al massimo)
    if (!rt) return result_error_msg("Out of memory");
    *rt = tmp;
    snprintf(rt->return_type, sizeof(rt->return_type), "%s", rtype);
    return result_ok_msg(0);
}

static QueryResult* exec_drop_procedure(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    bool is_function = n->drop_trigger.is_function;
    RoutineSchema* rt = find_routine(db, n->drop_trigger.name, is_function);
    char msg[200];
    if (!rt) { snprintf(msg, sizeof(msg), "%s '%.64s' not found", is_function ? "Function" : "Procedure", n->drop_trigger.name); return result_error_msg(msg); }
    int i = (int)(rt - db->routines);
    memmove(&db->routines[i], &db->routines[i + 1], sizeof(RoutineSchema) * (size_t)(db->num_routines - i - 1));
    db->num_routines--;
    return result_ok_msg(0);
}

// valore di un parametro / variabile della routine terminata
static void rt_var_value(TrigScope* sc, const char* name, EvalVal* out) {
    int ci = catalog_get_col_index(sc->vts, name);
    if (ci < 0 || !sc->vrow) { ev_null(out); return; }
    ev_from_col(out, &sc->vrow->values[ci], &sc->vts->columns[ci]);
}

static QueryResult* exec_call(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    char msg[400], inner[300];
    RoutineSchema* rt = find_procedure(db, n->call.name);
    if (!rt) {
        snprintf(msg, sizeof(msg), find_routine(db, n->call.name, true) ? "'%.64s' is a function: use SELECT %.64s(...)" : "Procedure '%.64s' not found",
                 n->call.name, n->call.name);
        return result_error_msg(msg);
    }
    bool outermost = ctx->trig_depth == 0;
    char pname[64];
    snprintf(pname, sizeof(pname), "%s", rt->name);
    TokenList* tk = NULL;
    RoutineDef* d = routine_parse(rt, &tk, msg, sizeof(msg));
    if (!d) return result_error_msg(msg);
    const char* fail = NULL;
    if (d->nparams != n->call.nargs) {
        snprintf(inner, sizeof(inner), "expects %d argument%s, got %d", d->nparams, d->nparams == 1 ? "" : "s", n->call.nargs);
        fail = inner;
    }
    // argomenti IN / INOUT: valutati una volta (vedono le variabili della routine che chiama); OUT e INOUT
    // devono essere variabili (@x o locali)
    EvalVal* av = sv_calloc(32, sizeof(EvalVal));
    if (!fail && !av) fail = "Out of memory";
    TrigScope* caller = ctx->routine;
    if (!fail) {
        BindCtx b;
        memset(&b, 0, sizeof(b));
        b.mode = BIND_ROW; b.clause = "CALL"; b.x = ctx; b.outer = ctx->trig_bind;
        EvalCtx ec;
        memset(&ec, 0, sizeof(ec));
        ec.x = ctx; ec.outer = ctx->trig_eval;
        for (int k = 0; k < d->nparams && !fail; k++) {
            ASTExpr* a = n->call.args[k];
            if (d->params[k].mode != PARAM_IN) {
                bool uv = a->type == EXPR_FUNC && !strcmp(a->func, "__UVAR");
                bool lv = a->type == EXPR_COLUMN && !a->table_name[0] && caller && catalog_get_col_index(caller->vts, a->col_name) >= 0;
                if (!uv && !lv) {
                    snprintf(inner, sizeof(inner), "OUT or INOUT argument %d for routine %.64s is not a variable", k + 1, pname);
                    fail = inner;
                    break;
                }
            }
            if (d->params[k].mode == PARAM_OUT) { ev_null(&av[k]); continue; }
            bind_expr(a, &b);
            if (b.err[0]) { snprintf(inner, sizeof(inner), "%.250s", b.err); fail = inner; break; }
            EvalVal v;
            eval_expr(a, &ec, &v);
            if (ec.err[0]) { snprintf(inner, sizeof(inner), "%.250s", ec.err); fail = inner; break; }
            if (v.null) ev_null(&av[k]);
            else if (v.is_num) ev_copy(&av[k], &v);
            else { const char* sv = ev_s(&v); size_t l = strlen(sv); if (l > EV_STR_MAX) l = EV_STR_MAX; ev_text(&av[k], sv, l); }
        }
    }
    TrigScope* sc = NULL;
    if (!fail) {
        sc = sv_calloc(1, sizeof(TrigScope));
        if (!sc) fail = "Out of memory";
        else fail = routine_run(ctx, rt, d, av, sc, inner, sizeof(inner));
    }
    // OUT / INOUT: i valori finali tornano alle variabili di chi chiama
    for (int k = 0; !fail && sc && k < d->nparams; k++) {
        if (d->params[k].mode == PARAM_IN) continue;
        EvalVal v;
        rt_var_value(sc, d->params[k].name, &v);
        ASTExpr* a = n->call.args[k];
        if (a->type == EXPR_FUNC) uservar_set(a->args[0]->str_val, &v);
        else rt_set_var(caller, a->col_name, &v);
    }
    QueryResult* r;
    if (fail) {
        if (!strncmp(fail, "Procedure '", 11) || !strncmp(fail, "Function '", 10)) snprintf(msg, sizeof(msg), "%s", fail);   // dalla routine più interna
        else snprintf(msg, sizeof(msg), "Procedure '%.64s': %.300s", pname, fail);
        r = result_error_msg(msg);
    } else if (sc->last) { r = sc->last; sc->last = NULL; }
    else r = result_ok_msg(sc->affected);
    if (sc) { trig_scope_free(sc); free(sc); }
    free(av);
    routine_def_free(d);
    lexer_free(tk);
    if (outermost) rt_temps_cleanup(ctx);
    return r;
}

// funzione definita dall'utente in un'espressione: argomenti valutati nella riga corrente
static void rt_eval_function(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    ExecCtx* ctx = c->x;
    Database* db = ctx ? catalog_current_db(ctx->catalog) : NULL;
    RoutineSchema* rt = find_routine(db, e->func, true);
    char msg[300];
    if (!rt) { ctx_error(c, "Function '%s' not found", e->func); ev_null(out); return; }
    TokenList* tk = NULL;
    RoutineDef* d = routine_parse(rt, &tk, msg, sizeof(msg));
    if (!d) { ctx_error(c, "%.250s", msg); ev_null(out); return; }
    if (d->nparams != e->nargs) {
        snprintf(msg, sizeof(msg), "Function '%.64s' expects %d argument%s, got %d", rt->name, d->nparams, d->nparams == 1 ? "" : "s", e->nargs);
        ctx_error(c, "%.250s", msg);
        routine_def_free(d); lexer_free(tk); ev_null(out);
        return;
    }
    EvalVal* av = sv_calloc((size_t)(e->nargs > 0 ? e->nargs : 1), sizeof(EvalVal));
    TrigScope* sc = av ? sv_calloc(1, sizeof(TrigScope)) : NULL;
    const char* fail = sc ? NULL : "Out of memory";
    for (int k = 0; !fail && k < e->nargs; k++) {
        EvalVal v;
        eval_expr(e->args[k], c, &v);
        if (c->err[0]) { fail = c->err; break; }
        if (v.null) ev_null(&av[k]);
        else if (v.is_num) ev_copy(&av[k], &v);
        else { const char* sv = ev_s(&v); size_t l = strlen(sv); if (l > EV_STR_MAX) l = EV_STR_MAX; ev_text(&av[k], sv, l); }
    }
    char name[64];
    snprintf(name, sizeof(name), "%s", rt->name);
    bool ctx_err = fail != NULL;
    if (!fail) {
        // la funzione esegue le sue query con il proprio ambiente: lo stato della query che la usa si salva
        bool nf = ctx->rt_notfound;
        fail = routine_run(ctx, rt, d, av, sc, msg, sizeof(msg));
        ctx->rt_notfound = nf;
    }
    if (fail) {
        if (!ctx_err) {
            if (!strncmp(fail, "Function '", 10) || !strncmp(fail, "Procedure '", 11)) ctx_error(c, "%.250s", fail);
            else { char full[400]; snprintf(full, sizeof(full), "Function '%.64s': %.300s", name, fail); ctx_error(c, "%.250s", full); }
        }
        ev_null(out);
    } else {
        // RETURN convertito nel tipo dichiarato (RETURNS DECIMAL(5,2), INT, ...)
        Column col;
        memset(&col, 0, sizeof(col));
        col.type = parse_type(d->ret.type);
        col.size = d->ret.size;
        if (col.type == SV_TYPE_DECIMAL) col.decimal_prec = d->ret.has_scale ? d->ret.scale + 1 : d->ret.size > 0 ? 1 : 0;
        if (sc->ret.null) ev_null(out);
        else {
            EvalVal r = sc->ret;
            if (r.is_num && !r.is_int && (col.type == SV_TYPE_INT || col.type == SV_TYPE_BIGINT)) ev_int(out, llround(r.num));
            else if (r.is_num && (col.type == SV_TYPE_INT || col.type == SV_TYPE_BIGINT || col.type == SV_TYPE_FLOAT)) ev_copy(out, &r);
            else {
                Value tv;
                memset(&tv, 0, sizeof(tv));
                column_set_from_text(&tv, &col, ev_s(&sc->ret));
                EvalVal tmp;
                ev_from_value(&tmp, &tv, col.type);
                if (tmp.null) ev_null(out);
                else if (tmp.str && tmp.str != tmp.buf) {          // testo nella Value temporanea: copiato
                    const char* tx = tmp.str;
                    ev_set_text(c, out, tx, strlen(tx));
                    out->is_num = tmp.is_num; out->is_int = tmp.is_int; out->num = tmp.num; out->inum = tmp.inum; out->is_dec = tmp.is_dec;
                } else ev_copy(out, &tmp);
                if (tv.owns_str) free(tv.str_val);
            }
        }
    }
    if (sc) { trig_scope_free(sc); free(sc); }
    free(av);
    routine_def_free(d);
    lexer_free(tk);
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
            char a[KEY_BUF], b[KEY_BUF];
            const Value* x = &ch->old->values[c];
            const Value* y = &ch->nt->values[c];
            bool diff = x->is_null != y->is_null;
            if (!diff && !x->is_null) {
                mi_key(x, ch->ts->columns[c].type, a, sizeof(a), col_is_ci(ch->ts, c));
                mi_key(y, ch->ts->columns[c].type, b, sizeof(b), col_is_ci(ch->ts, c));
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

// api_endpoints.definer: chi ha scritto la riga, sempre l'utente della sessione ("" = root / token /
// nessun utente), dopo i trigger BEFORE: il server esegue l'endpoint con i suoi permessi (P0.2)
static void definer_force(const ExecCtx* ctx, TableSchema* ts, Tuple* t) {
    if (strcasecmp(ts->name, AUTH_ENDPOINTS_TABLE)) return;
    int ci = catalog_get_col_index(ts, "definer");
    if (ci < 0 || ci >= t->num_values) return;
    column_set_from_text(&t->values[ci], &ts->columns[ci], ctx->user ? ctx->user : "");
}

static bool has_generated(const TableSchema* ts) {
    for (int i = 0; i < ts->num_columns; i++) if (ts->columns[i].flags & COLF_GENERATED) return true;
    return false;
}

// colonne calcolate della riga t (in ordine: una può usare quelle prima); NULL o il messaggio
static const char* gen_compute(ExecCtx* ctx, TableSchema* ts, Tuple* t, char* msg, size_t mn) {
    Source s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", ts->name);
    snprintf(s.table, sizeof(s.table), "%s", ts->name);
    s.ts = ts;
    for (int i = 0; i < ts->num_columns; i++) {
        Column* c = &ts->columns[i];
        if (!(c->flags & COLF_GENERATED)) continue;
        TokenList* tk = lexer_tokenize(c->comment);
        char err[256] = "";
        ASTExpr* e = tk ? parser_parse_expr(tk, err, sizeof(err)) : NULL;
        if (!e) { if (tk) lexer_free(tk); snprintf(msg, mn, "Generated column '%.64s': %.200s", c->name, err[0] ? err : "out of memory"); return msg; }
        BindCtx b;
        memset(&b, 0, sizeof(b));
        b.src = &s; b.nsrc = 1; b.mode = BIND_ROW; b.x = NULL; b.clause = "a generated column";
        bind_expr(e, &b);
        if (b.err[0]) { snprintf(msg, mn, "Generated column '%.64s': %.200s", c->name, b.err); ast_expr_free(e); lexer_free(tk); return msg; }
        EvalCtx ec;
        memset(&ec, 0, sizeof(ec));
        Tuple* cur[1] = { t };
        ec.src = &s; ec.nsrc = 1; ec.cur = cur; ec.x = ctx;
        EvalVal v;
        eval_expr(e, &ec, &v);
        const char* fail = NULL;
        if (ec.err[0]) { snprintf(msg, mn, "Generated column '%.64s': %.200s", c->name, ec.err); fail = msg; }
        else {
            const char* tx = v.null ? NULL : ev_s(&v);
            char nb[64];
            if (tx && v.is_num && !v.is_int && (c->type == SV_TYPE_INT || c->type == SV_TYPE_BIGINT)) { snprintf(nb, sizeof(nb), "%lld", (long long)llround(v.num)); tx = nb; }
            if (!tx && !c->nullable) { snprintf(msg, mn, "Column '%.64s' cannot be NULL", c->name); fail = msg; }
            else if (tx && check_value(c, tx, msg, mn)) fail = msg;
            else column_set_from_text(&t->values[i], c, tx);
        }
        ast_expr_free(e);
        lexer_free(tk);
        if (fail) return fail;
    }
    return NULL;
}

// buffer per colonna di exec_insert (P1.11: erano array di 64 sullo stack, una tabella più larga li sforava),
// allocati dentro e liberati dal chiamante: la funzione ha molte uscite
typedef struct {
    int* map; char (*vals)[EV_STR_MAX + 1]; char (*now_buf)[24];
    const char** text; bool* given; bool* direct; Value* dv;
} InsScratch;

static QueryResult* exec_insert_in(ExecCtx* ctx, ASTNode* n, InsScratch* S);

static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n) {
    InsScratch S;
    memset(&S, 0, sizeof(S));
    QueryResult* r = exec_insert_in(ctx, n, &S);
    free(S.map); free(S.vals); free(S.now_buf); free(S.text); free(S.given); free(S.direct); free(S.dv);
    return r;
}

static QueryResult* exec_insert_in(ExecCtx* ctx, ASTNode* n, InsScratch* S) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->insert_stmt.table_name))
            return result_error_msg("Cannot INSERT into a view");
        return result_error_msg("Table not found");
    }
    char msg[300];
    if (!executor_priv_ok(ctx, db->name, ts->name, AUTH_INSERT, msg, sizeof(msg))) return result_error_msg(msg);
    int nc = ts->num_columns;

    // colonne indicate
    // VALUES () / DEFAULT VALUES: nessun valore, ogni colonna prende il suo default
    bool all_default = n->insert_stmt.num_cols == 0 && n->insert_stmt.num_values == 0 && !n->insert_stmt.select;
    int ncols = n->insert_stmt.num_cols > 0 ? n->insert_stmt.num_cols : all_default ? 0 : nc;
    size_t ncap = (size_t)(nc > 0 ? nc : 1);
    int* map = S->map = sv_malloc(sizeof(int) * (size_t)(ncols > 0 ? ncols : 1));   // colonne indicate
    char (*now_buf)[24] = S->now_buf = sv_malloc(24 * ncap);   // DEFAULT CURRENT_TIMESTAMP della riga in costruzione
    char (*vals)[EV_STR_MAX + 1] = S->vals = sv_malloc((EV_STR_MAX + 1) * ncap);   // testi della riga corrente
    const char** text = S->text = sv_malloc(sizeof(char*) * ncap);
    bool* given = S->given = sv_malloc(ncap);
    bool* direct = S->direct = sv_malloc(ncap);           // numero già pronto (niente conversione in testo e ritorno)
    Value* dv = S->dv = sv_malloc(sizeof(Value) * ncap);
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
    int64_t last_generated = 0;     // PRIMO valore AUTO_INCREMENT generato dallo statement: LAST_INSERT_ID() di MySQL
                                    // (prima l'ultimo: con INSERT … VALUES (…),(…) gli id calcolati dalle app erano sbagliati)
    if (ai >= 0) {
        MemIndex* ix = mi_get(ctx, db->name, ts, ai);
        if (!ix) { orows_free(&srows); return result_error_msg("Out of memory (AUTO_INCREMENT index)"); }
        auto_next = ix->has_max ? ix->max_int : 0;
        if (ts->auto_increment > 0 && auto_next < (int64_t)ts->auto_increment - 1) auto_next = (int64_t)ts->auto_increment - 1;
    }

    Tuple** tuples = sv_calloc((size_t)(nrows ? nrows : 1), sizeof(Tuple*));
    if (!tuples) { orows_free(&srows); return result_error_msg("Out of memory"); }
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.x = ctx;
    ec.outer = ctx->trig_eval;
    const char* fail = NULL;
    char ai_buf[32];

    for (int r = 0; r < nrows && !fail; r++) {
        static _Thread_local char fail_buf[200];
        memset(given, 0, ncap);
        memset(direct, 0, ncap);
        for (int i = 0; i < nc; i++) text[i] = NULL;
        for (int k = 0; k < ncols; k++) {
            EvalVal v;
            if (!from_select && is_default_expr(n->insert_stmt.values[r * ncols + k])) continue;   // DEFAULT: come omessa
            if (from_select) ev_from_oval(&v, &orow(&srows, r)[k]);
            else eval_expr(n->insert_stmt.values[r * ncols + k], &ec, &v);
            if (ec.err[0]) { fail = ec.err; break; }
            int ci = map[k];
            given[ci] = true;
            if (v.null) { text[ci] = NULL; continue; }
            DataType ty = ts->columns[ci].type;
            if (v.is_int && (ty == SV_TYPE_BIGINT || (ty == SV_TYPE_INT && v.inum >= INT32_MIN && v.inum <= INT32_MAX))) {
                if (v.inum < 0 && ty == SV_TYPE_BIGINT && (ts->columns[ci].flags & COLF_UNSIGNED)) {   // i bit negativi sono
                    snprintf(fail_buf, sizeof(fail_buf), "Value %lld is out of range for column '%.64s' (UNSIGNED)",   // valori oltre 2^63
                             (long long)v.inum, ts->columns[ci].name);
                    fail = fail_buf;
                    break;
                }
                memset(&dv[ci], 0, sizeof(Value));
                if (ty == SV_TYPE_INT) dv[ci].int_val = (int32_t)v.inum; else dv[ci].bigint_val = v.inum;
                direct[ci] = true;
                text[ci] = "";                    // (non NULL)
                continue;
            }
            if (v.is_num && ty == SV_TYPE_FLOAT) {
                memset(&dv[ci], 0, sizeof(Value));
                dv[ci].float_val = v.num;
                direct[ci] = true;
                text[ci] = "";
                continue;
            }
            // il testo del buffer di v va copiato (v cambia alla prossima colonna); quello lungo sta già
            // nella memoria del letterale, della riga letta o dell'arena e resta valido per lo statement
            const char* sv = ev_s(&v);
            if (sv == v.buf) { memcpy(vals[ci], sv, strlen(sv) + 1); text[ci] = vals[ci]; }
            else text[ci] = sv;
        }
        if (fail) break;
        for (int i = 0; i < nc; i++) {
            Column* c = &ts->columns[i];
            if (c->flags & COLF_GENERATED) {                            // calcolata dopo
                if (given[i] && text[i]) {
                    snprintf(msg, sizeof(msg), "The value specified for generated column '%.64s' in table '%.64s' is not allowed", c->name, ts->name);
                    fail = msg;
                    break;
                }
                text[i] = NULL; direct[i] = false;
                continue;
            }
            if (!given[i] && (c->default_val[0] || c->has_default) && strcasecmp(c->default_val, "NULL") != 0) {
                if (default_is_now(c->default_val)) { col_now_text(c->default_val, c->type, now_buf[i], sizeof(now_buf[0])); text[i] = now_buf[i]; }
                else text[i] = c->default_val;
            }
            if (!given[i] && direct[i]) direct[i] = false;
            if (i == ai && direct[i]) {
                int64_t x = c->type == SV_TYPE_INT ? dv[i].int_val : dv[i].bigint_val;
                if (x > auto_next) auto_next = x;
            } else if (i == ai) {
                if (!text[i]) {                                   // valore generato (anche per un NULL esplicito)
                    if (auto_next == INT64_MAX) { fail = "AUTO_INCREMENT value out of range"; break; }
                    snprintf(ai_buf, sizeof(ai_buf), "%lld", (long long)++auto_next);
                    if (!last_generated) last_generated = auto_next;
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
            if (!direct[i] && check_value(c, text[i], msg, sizeof(msg))) { fail = msg; break; }
        }
        if (fail) break;
        tuples[r] = tuple_create(nc);
        if (!tuples[r]) { fail = "Out of memory"; break; }
        for (int i = 0; i < nc; i++) {
            if (direct[i]) tuples[r]->values[i] = dv[i];
            else column_set_from_text(&tuples[r]->values[i], &ts->columns[i], text[i]);
        }
        if (has_generated(ts)) { fail = gen_compute(ctx, ts, tuples[r], msg, sizeof(msg)); if (fail) break; }
    }
    // trigger BEFORE (possono cambiare NEW o rifiutare la riga), poi i vincoli: CHECK,
    // PRIMARY KEY / UNIQUE (anche fra le righe dello statement), FOREIGN KEY
    if (!fail) fail = fire_triggers(ctx, db, ts, "INSERT", true, false, NULL, NULL, NULL, 0, msg, sizeof(msg));
    for (int r = 0; r < nrows && !fail; r++)
        fail = fire_triggers(ctx, db, ts, "INSERT", true, true, NULL, tuples[r], NULL, 0, msg, sizeof(msg));
    for (int r = 0; r < nrows && !fail; r++) definer_force(ctx, ts, tuples[r]);
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
    for (int r = 0; r < done; r++) cap_add(ctx, ts, tuples[r]);   // RETURNING
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
    QueryResult* ok = result_ok_msg(done);
    if (ok && last_generated > 0) ok->last_insert_id = (long long)last_generated;
    if (last_generated > 0) g_last_insert_id = last_generated;
    return ok;
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
            int ci = BOUND_COL(col->bound);
            uint64_t m = 0; bool o = false, op = false;
            expr_refs(key, &m, &o, &op);
            if (m || op || !col_is_indexed(s->ts, ci) || key->type == EXPR_INT || key->type == EXPR_STRING) continue;
            EvalVal kv;
            eval_expr(key, ec, &kv);
            char kt[KEY_BUF];
            if (!probe_text_ci(&kv, s->ts->columns[ci].type, kt, sizeof(kt), col_is_ci(s->ts, ci)) && !kv.null) continue;
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
    if (!source_load(ctx, db->name, s, where, NULL, 0)) return sv_scan_cancelled() ? "Query cancelled" : "Failed to read the table";
    {   // righe mancanti per una pagina danneggiata: niente UPDATE / DELETE a metà
        static _Thread_local char dm[200];
        if (heap_damage(dm, sizeof(dm))) return dm;
    }
    int* idx = sv_malloc(sizeof(int) * (size_t)(s->n ? s->n : 1));
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

// set_ci: spazio per num_sets indici di colonna (lo dà exec_update: la lista SET non ha più un tetto fisso)
static QueryResult* exec_update_cols(ExecCtx* ctx, ASTNode* n, int* set_ci) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->update_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->update_stmt.table_name)) return result_error_msg("Cannot UPDATE a view");
        return result_error_msg("Table not found");
    }
    char msg[300];
    if (!executor_priv_ok(ctx, db->name, ts->name, AUTH_UPDATE, msg, sizeof(msg))) return result_error_msg(msg);
    for (int k = 0; k < n->update_stmt.num_sets; k++) {
        set_ci[k] = catalog_get_col_index(ts, n->update_stmt.set_cols[k]);
        if (set_ci[k] < 0) return unknown_column_error(n->update_stmt.set_cols[k], "SET");
        const ASTExpr* sx = n->update_stmt.set_exprs[k];
        bool to_default = is_default_expr(sx) || (sx->type == EXPR_COLUMN && !sx->table_name[0] && !strcasecmp(sx->col_name, "DEFAULT"));
        if ((ts->columns[set_ci[k]].flags & COLF_GENERATED) && !to_default) {
            snprintf(msg, sizeof(msg), "The value specified for generated column '%.64s' in table '%.64s' is not allowed", ts->columns[set_ci[k]].name, ts->name);
            return result_error_msg(msg);
        }
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
    Tuple** nt = sv_calloc((size_t)(nm ? nm : 1), sizeof(Tuple*));
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
            const char* text;
            char dnow[40];
            if (is_default_expr(n->update_stmt.set_exprs[k])) {             // SET col = DEFAULT
                if (c->flags & COLF_GENERATED) continue;                       // la ricalcola gen_compute
                if ((!c->default_val[0] && !c->has_default) || !strcasecmp(c->default_val, "NULL")) text = NULL;
                else if (default_is_now(c->default_val)) { col_now_text(c->default_val, c->type, dnow, sizeof(dnow)); text = dnow; }
                else text = c->default_val;
            } else {
                eval_expr(n->update_stmt.set_exprs[k], &ec, &v);
                if (ec.err[0]) { fail = ec.err; break; }
                text = ev_store_text(&v);
            }
            if (!text && (!c->nullable || c->primary_key)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' cannot be NULL", c->name);
                fail = msg;
                break;
            }
            if (text && check_value(c, text, msg, sizeof(msg))) { fail = msg; break; }
            column_set_from_text(&nt[i]->values[ci], c, text);
        }
        if (!fail && has_generated(ts)) fail = gen_compute(ctx, ts, nt[i], msg, sizeof(msg));
    }
    ec.cur = NULL;

    // trigger BEFORE, modifiche (con le azioni a cascata delle chiavi esterne), controllo dei
    // vincoli, scrittura, trigger AFTER
    int affected = 0;
    if (!fail) fail = fire_triggers(ctx, db, ts, "UPDATE", true, false, NULL, NULL, set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
    for (int i = 0; i < nm && !fail; i++)
        fail = fire_triggers(ctx, db, ts, "UPDATE", true, true, s.rows[idx[i]], nt[i], set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
    for (int i = 0; i < nm && !fail; i++) definer_force(ctx, ts, nt[i]);
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
        if (!fail && heap_damage(msg, sizeof(msg))) fail = msg;
        if (!fail && heap_damage(msg, sizeof(msg))) fail = msg;
    if (!fail) affected = cs_apply(&cs, msg, sizeof(msg), &fail);
        for (int i = 0; i < cs.n && !fail; i++)
            if (cs.v[i].direct)
                fail = fire_triggers(ctx, db, ts, "UPDATE", false, true, cs.v[i].old, cs.v[i].nt, set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
        if (!fail) fail = fire_cascaded(ctx, db, &cs, false, msg, sizeof(msg));
        if (!fail) fail = fire_triggers(ctx, db, ts, "UPDATE", false, false, NULL, NULL, set_ci, n->update_stmt.num_sets, msg, sizeof(msg));
        for (int i = 0; i < cs.n && !fail; i++) if (cs.v[i].direct && cs.v[i].ts == ts) cap_add(ctx, ts, cs.v[i].nt);   // RETURNING
        cs_free(&cs);
    }
    for (int i = 0; i < nm; i++) if (nt[i]) tuple_free(nt[i]);
    free(nt);
    free(idx);
    source_free(&s);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    return result_ok_msg(affected);
}

static QueryResult* exec_update(ExecCtx* ctx, ASTNode* n) {
    int* set_ci = sv_malloc(sizeof(int) * ((size_t)n->update_stmt.num_sets + 1));
    if (!set_ci) return result_error_msg("Out of memory");
    QueryResult* r = exec_update_cols(ctx, n, set_ci);
    free(set_ci);
    return r;
}

static QueryResult* exec_delete(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->delete_stmt.table_name)) return result_error_msg("Cannot DELETE from a view");
        return result_error_msg("Table not found");
    }
    {
        char pm[300];
        if (!executor_priv_ok(ctx, db->name, ts->name, AUTH_DELETE, pm, sizeof(pm))) return result_error_msg(pm);
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
    for (int i = 0; i < cs.n && !fail; i++) if (cs.v[i].direct && cs.v[i].ts == ts) cap_add(ctx, ts, cs.v[i].old);   // RETURNING
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

typedef SvStrBuf TBuf;                                       // testo dei CREATE TABLE (buffer comune, CR-B2)
#define tb_add svsb_printf

// i nomi sempre tra backtick, come MySQL: prima `order INT` o `my col INT` rendevano il testo non rieseguibile
// (dump che non si ripristinavano, CREATE TABLE ... LIKE che falliva)
static void tb_qcols(TBuf* b, const TableSchema* ts, const uint8_t* cols, int n) {
    for (int k = 0; k < n; k++) tb_add(b, "%s`%s`", k ? ", " : "", ts->columns[cols[k]].name);
}
static void tb_cols(TBuf* b, const TableSchema* ts, const uint8_t* cols, int n) {   // elenco da mostrare (SHOW CONSTRAINTS)
    for (int k = 0; k < n; k++) tb_add(b, "%s%s", k ? ", " : "", ts->columns[cols[k]].name);
}

// 'testo' con apici e backslash raddoppiati (il lexer tratta \ come escape)
static void tb_quoted(TBuf* b, const char* s) {
    svsb_putc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'' || *s == '\\') svsb_putc(b, *s);
        svsb_putc(b, *s);
    }
    svsb_putc(b, '\'');
}

static void tb_fk(TBuf* b, const TableSchema* ts, const Constraint* c) {
    tb_add(b, "CONSTRAINT `%s` FOREIGN KEY (", c->name);
    tb_qcols(b, ts, c->cols, c->ncols);
    tb_add(b, ") REFERENCES `%s` (", c->ref_table);
    for (int k = 0; k < c->ncols; k++) tb_add(b, "%s`%s`", k ? ", " : "", c->ref_cols[k]);
    tb_add(b, ")");
    if (c->on_delete != FK_NO_ACTION) tb_add(b, " ON DELETE %s", fk_action_name(c->on_delete));
    if (c->on_update != FK_NO_ACTION) tb_add(b, " ON UPDATE %s", fk_action_name(c->on_update));
}

// CREATE TABLE con colonne e vincoli; with_fk = false: senza le chiavi esterne (un dump le
// aggiunge alla fine con executor_fk_statements, quando tutte le tabelle hanno i dati)
char* executor_create_statement(Catalog* cat, const char* db, const char* table, bool with_fk) {
    TableSchema* t = catalog_get_table(cat, db, table);
    if (!t) return NULL;
    TBuf b = {0};
    char pkname[SV_MAX_NAME_LEN + 8];
    snprintf(pkname, sizeof(pkname), "pk_%.58s", t->name);
    const Constraint* inline_pk = NULL;
    for (int i = 0; i < t->num_constraints; i++)
        if (t->constraints[i].kind == CONS_PRIMARY && t->constraints[i].ncols == 1 && !strcasecmp(t->constraints[i].name, pkname))
            inline_pk = &t->constraints[i];
    bool has_pk_cons = false;
    for (int i = 0; i < t->num_constraints; i++) if (t->constraints[i].kind == CONS_PRIMARY) has_pk_cons = true;
    tb_add(&b, "CREATE TABLE `%s` (", t->name);
    for (int i = 0; i < t->num_columns; i++) {
        const Column* c = &t->columns[i];
        char type[48];
        if ((c->type == SV_TYPE_VARCHAR || c->type == SV_TYPE_CHAR) && c->size > 0)
            snprintf(type, sizeof(type), "%s(%d)", datatype_name(c->type), c->size);
        else if (c->type == SV_TYPE_DECIMAL && c->size > 0 && c->decimal_prec > 0)    // DECIMAL(p, s)
            snprintf(type, sizeof(type), "DECIMAL(%d,%d)", c->size, c->decimal_prec - 1);
        else if (c->type == SV_TYPE_DECIMAL && c->size > 0)
            snprintf(type, sizeof(type), "DECIMAL(%d)", c->size);
        else
            snprintf(type, sizeof(type), "%s", datatype_name(c->type));
        // tipi numerici come sono stati dichiarati: TINYINT, INT UNSIGNED, DECIMAL(5,2) UNSIGNED (prima diventavano
        // INT / BIGINT e un dump o CREATE TABLE ... LIKE perdeva limiti e UNSIGNED)
        bool numeric = c->type == SV_TYPE_INT || c->type == SV_TYPE_BIGINT || c->type == SV_TYPE_FLOAT || c->type == SV_TYPE_DECIMAL;
        if (numeric && c->sqltype[0] && strncasecmp(c->sqltype, "serial", 6) && strlen(c->sqltype) < sizeof(type)) {
            size_t k = 0;
            for (; c->sqltype[k]; k++) type[k] = (char)toupper((unsigned char)c->sqltype[k]);
            type[k] = '\0';
        } else if (numeric && !strncasecmp(c->sqltype, "serial", 6)) snprintf(type, sizeof(type), "BIGINT UNSIGNED");
        if (is_text_col(c) && (c->flags & (COLF_CI | COLF_BIN)))
            snprintf(type + strlen(type), sizeof(type) - strlen(type), " COLLATE %s", (c->flags & COLF_CI) ? "utf8mb4_general_ci" : "utf8mb4_bin");
        bool pk_here = (inline_pk && inline_pk->cols[0] == i) || (!has_pk_cons && c->primary_key);
        tb_add(&b, "%s\n    `%s` %s%s%s%s", i ? "," : "", c->name, type, pk_here ? " PRIMARY KEY" : "",
               c->auto_increment ? " AUTO_INCREMENT" : "", (!c->nullable && !c->primary_key) ? " NOT NULL" : "");
        if (c->flags & COLF_GENERATED) tb_add(&b, " GENERATED ALWAYS AS (%s) STORED", c->comment);
        else if (c->default_val[0] || c->has_default) {
            bool num = c->type == SV_TYPE_INT || c->type == SV_TYPE_BIGINT || c->type == SV_TYPE_FLOAT ||
                       c->type == SV_TYPE_DECIMAL || c->type == SV_TYPE_BOOL || !strcasecmp(c->default_val, "NULL");
            if (num || default_is_now(c->default_val)) tb_add(&b, " DEFAULT %s", c->default_val);
            else { svsb_puts(&b, " DEFAULT "); tb_quoted(&b, c->default_val); }
        }
        if (c->flags & COLF_ON_UPDATE_NOW) svsb_puts(&b, " ON UPDATE CURRENT_TIMESTAMP");   // prima perso
        if (!(c->flags & COLF_GENERATED) && c->comment[0] && c->comment[0] != '\x01') { svsb_puts(&b, " COMMENT "); tb_quoted(&b, c->comment); }
    }
    for (int i = 0; i < t->num_constraints; i++) {
        const Constraint* c = &t->constraints[i];
        if (c == inline_pk || (c->kind == CONS_FOREIGN && !with_fk)) continue;
        tb_add(&b, ",\n    ");
        switch (c->kind) {
            case CONS_PRIMARY: tb_add(&b, "CONSTRAINT `%s` PRIMARY KEY (", c->name); tb_qcols(&b, t, c->cols, c->ncols); tb_add(&b, ")"); break;
            case CONS_UNIQUE:  tb_add(&b, "CONSTRAINT `%s` UNIQUE (", c->name); tb_qcols(&b, t, c->cols, c->ncols); tb_add(&b, ")"); break;
            case CONS_CHECK:   tb_add(&b, "CONSTRAINT `%s` CHECK (%s)", c->name, c->check); break;
            case CONS_FOREIGN: tb_fk(&b, t, c); break;
        }
    }
    // indici (prima persi: un dump ripristinato o CREATE TABLE ... LIKE non li aveva)
    for (int i = 0; i < t->num_indexes; i++) {
        const Index* ix = &t->indexes[i];
        tb_add(&b, ",\n    %sKEY `%s` (", ix->fulltext ? "FULLTEXT " : ix->unique ? "UNIQUE " : "", ix->name);
        int nc = ix->ncols > 0 ? ix->ncols : 1;
        for (int k = 0; k < nc; k++) tb_add(&b, "%s`%s`", k ? ", " : "", ix->ncols > 0 ? ix->cols[k] : ix->col_name);
        svsb_putc(&b, ')');
    }
    svsb_puts(&b, "\n)");
    if (t->collation[0] && sv_name_ok(t->collation) && !strpbrk(t->collation, " ;'")) tb_add(&b, " COLLATE %s", t->collation);
    if (t->comment[0]) { svsb_puts(&b, " COMMENT "); tb_quoted(&b, t->comment); }
    svsb_putc(&b, ';');
    return svsb_take(&b);
}

// ALTER TABLE ... ADD CONSTRAINT per le chiavi esterne della tabella ("" se non ne ha)
char* executor_fk_statements(Catalog* cat, const char* db, const char* table) {
    TableSchema* t = catalog_get_table(cat, db, table);
    if (!t) return NULL;
    TBuf b = {0};
    tb_add(&b, "%s", "");
    for (int i = 0; i < t->num_constraints; i++) {
        if (t->constraints[i].kind != CONS_FOREIGN) continue;
        tb_add(&b, "ALTER TABLE `%s` ADD ", t->name);
        tb_fk(&b, t, &t->constraints[i]);
        tb_add(&b, ";\n");
    }
    return svsb_take(&b);
}

// risultato con colonne di testo
static QueryResult* text_result(int ncols, const char* const* names) {
    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    r->success = true;
    r->num_cols = ncols;
    r->col_names = sv_calloc((size_t)ncols, sizeof(char*));
    r->col_types = sv_calloc((size_t)ncols, sizeof(char*));
    if (!r->col_names || !r->col_types) { db_result_free(r); return NULL; }
    for (int i = 0; i < ncols; i++) { r->col_names[i] = sv_strdup(names[i]); r->col_types[i] = sv_strdup("VARCHAR"); }
    return r;
}

static bool text_row(QueryResult* r, int* cap, const char* const* vals) {
    if (r->num_rows >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        Row* nr = sv_realloc(r->rows, sizeof(Row) * (size_t)nc);
        if (!nr) return false;
        r->rows = nr;
        *cap = nc;
    }
    if (!db_row_set(&r->rows[r->num_rows], r->num_cols, vals, NULL)) return false;
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
        BufferStats stv;
        bp_stats(ctx->bp, &stv);
        BufferStats* st = &stv;
        int mi = 0;
        for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) mi++;
        long long pages = (long long)(sv_lseek(ctx->bp->db_fd, 0, SEEK_END) / SV_PAGE_SIZE);
        struct { const char* k; char v[64]; } rows[16];
        int nr = 0;
        #define STAT(name, ...) do { rows[nr].k = name; snprintf(rows[nr].v, sizeof(rows[nr].v), __VA_ARGS__); nr++; } while (0)
        STAT("page_size", "%d bytes", SV_PAGE_SIZE);
        STAT("database_pages", "%lld", pages);
        STAT("buffer_pool_frames", "%d (%d in use)", ctx->bp->capacity, ctx->bp->num_used);
        STAT("buffer_pool_policy", "CLOCK (second chance)");
        STAT("buffer_requests", "%llu", (unsigned long long)st->requests);
        STAT("buffer_hits", "%llu", (unsigned long long)st->hits);
        STAT("buffer_misses", "%llu (pages read from disk)", (unsigned long long)st->misses);
        STAT("buffer_hit_ratio", "%.2f%%", st->requests ? 100.0 * (double)st->hits / (double)st->requests : 100.0);
        STAT("buffer_evictions", "%llu", (unsigned long long)st->evictions);
        STAT("pages_written", "%llu", (unsigned long long)st->dirty_writes);
        STAT("pages_allocated", "%llu", (unsigned long long)st->new_pages);
        STAT("wal_size", "%lld bytes", ctx->wal ? (long long)wal_bytes(ctx->wal) : 0LL);
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
            if (!priv_any(ctx, db->name, tg->table_name)) continue;
            const char* v[5] = { tg->name, tg->before ? "BEFORE" : "AFTER", tg->event, tg->table_name, tg->body };
            if (!text_row(r, &cap, v)) { db_result_free(r); return result_error_msg("Out of memory"); }
        }
        return r;
    }
    if (!strcmp(kind, "CREATE VIEW")) {
        ViewSchema* vs = catalog_get_view(ctx->catalog, db->name, n->select_stmt.table_alias);
        if (!vs) { snprintf(a, sizeof(a), "View '%.64s' not found", n->select_stmt.table_alias); return result_error_msg(a); }
        if (!priv_any(ctx, db->name, vs->name)) { snprintf(a, sizeof(a), "Access denied for user '%.63s' to view '%.64s'", ctx->user, vs->name); return result_error_msg(a); }
        static const char* const NAMES[] = { "View", "Create View" };
        QueryResult* r = text_result(2, NAMES);
        size_t dl = strlen(vs->definition) + 100;
        char* ddl = sv_malloc(dl);
        if (ddl) snprintf(ddl, dl, "CREATE VIEW %s AS %s", vs->name, vs->definition);
        const char* v[2] = { vs->name, ddl ? ddl : "" };
        if (!r || !text_row(r, &cap, v)) { free(ddl); db_result_free(r); return result_error_msg("Out of memory"); }
        free(ddl);
        return r;
    }
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->select_stmt.table_alias);
    if (!ts) { snprintf(a, sizeof(a), "Table '%.64s' not found", n->select_stmt.table_alias); return result_error_msg(a); }
    if (!priv_any(ctx, db->name, ts->name)) { snprintf(a, sizeof(a), "Access denied for user '%.63s' to table '%.64s'", ctx->user, ts->name); return result_error_msg(a); }
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
            TBuf cb = {0};
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
    bool full = !strcmp(n->select_stmt.table_name, "FULL TABLES");
    if (full || !strcmp(n->select_stmt.table_name, "TABLES")) {     // come MySQL: anche le viste
        Database* db = catalog_current_db(ctx->catalog);
        if (!db) return result_error_msg("No database selected");
        char head[80];                                             // come MySQL: Tables_in_<database>
        snprintf(head, sizeof(head), "Tables_in_%.63s", db->name);
        const char* NAMES[] = { head, "Table_type" };
        QueryResult* r = text_result(full ? 2 : 1, NAMES);
        int cap = 0;
        for (int i = 0; r && i < db->num_tables + db->num_views; i++) {
            bool view = i >= db->num_tables;
            const char* v[2] = { view ? db->views[i - db->num_tables].name : db->tables[i]->name, view ? "VIEW" : "BASE TABLE" };
            if (n->select_stmt.has_show_like && !like_match_esc(v[0], n->select_stmt.show_like, '\\')) continue;
            if (!priv_any(ctx, db->name, v[0])) continue;              // come MySQL: solo le tabelle con qualche permesso
            if (!text_row(r, &cap, v)) { db_result_free(r); r = NULL; }
        }
        return r ? r : result_error_msg("Out of memory");
    }
    if (strcmp(n->select_stmt.table_name, "TABLES") && strcmp(n->select_stmt.table_name, "DATABASES"))
        return exec_show_extra(ctx, n);
    bool tables = strcmp(n->select_stmt.table_name, "TABLES") == 0;
    Database* db = catalog_current_db(ctx->catalog);
    if (tables && !db) return result_error_msg("No database selected");
    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
    r->success = true;

    if (!tables) {
        r->num_cols = 1;
        r->col_names = sv_calloc(1, sizeof(char*));
        r->col_names[0] = sv_strdup("Database");
        r->rows = sv_calloc((size_t)ctx->catalog->num_databases + 1, sizeof(Row));
        r->num_rows = 0;
        for (int i = 0; i < ctx->catalog->num_databases; i++) {
            const char* v = ctx->catalog->databases[i].name;
            if (n->select_stmt.has_show_like && !like_match_esc(v, n->select_stmt.show_like, '\\')) continue;
            db_row_set(&r->rows[r->num_rows++], 1, &v, NULL);
        }
    } else {
        r->num_cols = 1;
        r->col_names = sv_calloc(1, sizeof(char*));
        r->col_names[0] = sv_strdup("Tables");
        r->num_rows = db->num_tables;
        r->rows = sv_calloc(r->num_rows, sizeof(Row));
        for (int i = 0; i < r->num_rows; i++) {
            const char* v = db->tables[i]->name;
            db_row_set(&r->rows[i], 1, &v, NULL);
        }
    }
    return r;
}

static QueryResult* exec_describe(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    const char* tname = n->select_stmt.table_name;
    if (!tname[0]) return result_error_msg("No table specified");
    if (!priv_any(ctx, db->name, tname)) {
        char pm[300];
        snprintf(pm, sizeof(pm), "Access denied for user '%.63s' to table '%.64s'", ctx->user, tname);
        return result_error_msg(pm);
    }

    int num_cols;
    ColumnInfo* cols = db_list_columns(db->name, tname, &num_cols);
    if (!cols && catalog_get_view(ctx->catalog, db->name, tname)) {   // vista: le colonne del suo risultato
        char q[200];
        snprintf(q, sizeof(q), "SELECT * FROM `%.64s` LIMIT 0", tname);
        TokenList* tk = lexer_tokenize(q);
        ParseResult* pr = tk ? parser_parse(tk) : NULL;
        QueryResult* vr = pr && pr->success && pr->count == 1 ? run_select_ex(ctx, pr->statements[0], 0, false, false) : NULL;
        if (pr) parse_result_free(pr);
        if (tk) lexer_free(tk);
        if (!vr || !vr->success) return vr ? vr : result_error_msg("Out of memory");
        static const char* const NAMES[] = { "Field", "Type", "Null", "Key" };
        QueryResult* r = text_result(4, NAMES);
        int cap = 0;
        for (int i = 0; r && i < vr->num_cols; i++) {
            const char* v[4] = { vr->col_names[i], vr->col_types && vr->col_types[i] ? vr->col_types[i] : "", "YES", "" };
            if (!text_row(r, &cap, v)) { db_result_free(r); r = NULL; }
        }
        db_result_free(vr);
        return r ? r : result_error_msg("Out of memory");
    }
    if (!cols) return result_error_msg("Table not found");

    QueryResult* r = sv_calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = 4;
    r->col_names = sv_calloc(4, sizeof(char*));
    r->col_names[0] = sv_strdup("Field");
    r->col_names[1] = sv_strdup("Type");
    r->col_names[2] = sv_strdup("Null");
    r->col_names[3] = sv_strdup("Key");
    r->num_rows = num_cols;
    r->rows = sv_calloc(num_cols, sizeof(Row));
    for (int i = 0; i < num_cols; i++) {
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
        const char* vals[4] = { cols[i].name, cols[i].type, cols[i].nullable ? "YES" : "NO", key };
        db_row_set(&r->rows[i], 4, vals, NULL);
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
    if (ts->num_indexes >= SV_MAX_INDEXES) return result_error_msg("Too many indexes on this table (max 64)");
    if (!sv_name_ok_opt(n->create_index.index_name)) return bad_name_error("index");

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
    // indice creato con CREATE UNIQUE INDEX: è un vincolo UNIQUE con quel nome
    for (int k = 0; k < ts->num_constraints; k++)
        if (ts->constraints[k].kind == CONS_UNIQUE && !strcasecmp(ts->constraints[k].name, n->drop_index.index_name)) {
            ASTNode alt;
            memset(&alt, 0, sizeof(alt));
            alt.type = NODE_ALTER_TABLE;
            snprintf(alt.alter_table.table_name, sizeof(alt.alter_table.table_name), "%s", ts->name);
            snprintf(alt.alter_table.drop_constraint, sizeof(alt.alter_table.drop_constraint), "%s", n->drop_index.index_name);
            return exec_alter_table(ctx, &alt);
        }
    return result_error_msg("Index not found");
}

// la definizione di una vista funziona? (piano della SELECT, senza eseguirla); NULL o il messaggio
static const char* view_check(ExecCtx* ctx, const char* def, char* msg, size_t mn) {
    TokenList* tk = lexer_tokenize(def);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    const char* fail = NULL;
    if (!pr || !pr->success || pr->count != 1 || pr->statements[0]->type != NODE_SELECT) {
        snprintf(msg, mn, "Invalid view: %.250s", pr && !pr->success ? pr->error : "not a single SELECT");
        fail = msg;
    } else {
        QueryResult* r = run_select_ex(ctx, pr->statements[0], 0, true, false);
        if (!r || !r->success) { snprintf(msg, mn, "Invalid view: %.250s", r ? r->error : "out of memory"); fail = msg; }
        db_result_free(r);
    }
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    return fail;
}

// CREATE [OR REPLACE] VIEW / ALTER VIEW. Una vista sostituita resta al suo posto nel catalogo
static QueryResult* exec_create_view(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    const char* name = n->create_view.view_name;
    char msg[400];
    if (!sv_name_ok(name)) return bad_name_error("view");
    if (catalog_get_table(ctx->catalog, db->name, name)) {
        snprintf(msg, sizeof(msg), "Table '%.64s' already exists", name);
        return result_error_msg(msg);
    }
    ViewSchema* old = catalog_get_view(ctx->catalog, db->name, name);
    if (old && !n->create_view.replace && !n->create_view.alter) {
        snprintf(msg, sizeof(msg), "View '%.64s' already exists (use CREATE OR REPLACE VIEW)", name);
        return result_error_msg(msg);
    }
    if (!old && n->create_view.alter) { snprintf(msg, sizeof(msg), "View '%.64s' not found", name); return result_error_msg(msg); }
    // durante il controllo la vista non esiste: una definizione che la usa (anche passando da
    // un'altra vista) viene rifiutata invece di creare un ciclo
    char saved[SV_MAX_NAME_LEN];
    if (old) { memcpy(saved, old->name, sizeof(saved)); old->name[0] = '\0'; }
    const char* fail = view_check(ctx, n->create_view.definition, msg, sizeof(msg));
    if (old) memcpy(old->name, saved, sizeof(saved));
    if (fail) return result_error_msg(fail);
    if (old) {
        snprintf(old->definition, sizeof(old->definition), "%s", n->create_view.definition);
        return result_ok_msg(0);
    }
    ViewSchema* vs = sv_calloc(1, sizeof(ViewSchema));
    if (!vs) return result_error_msg("Out of memory");
    snprintf(vs->name, sizeof(vs->name), "%s", name);
    snprintf(vs->definition, sizeof(vs->definition), "%s", n->create_view.definition);
    int rc = catalog_add_view(ctx->catalog, db->name, vs);
    free(vs);
    if (rc != SV_OK) return result_error_msg("Failed to create view (too many views?)");
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
    // P1.11: una pagina radice nuova e vuota (nel log), la catena vecchia liberata come quella di un DROP TABLE
    // quando il record del catalogo è su disco. Prima: ogni riga cancellata senza record nel log (dopo una caduta
    // tornavano) e un passaggio su tutta la tabella
    uint32_t root, old_root = ts->root_page_id;
    if (new_root_page(ctx, &root) != SV_OK) return result_error_msg("Cannot allocate a page");
    int affected = ts->num_rows > (uint64_t)INT32_MAX ? INT32_MAX : (int)ts->num_rows;
    ts->root_page_id = root;
    ts->num_pages = 1;
    ts->num_rows = 0;
    mi_invalidate_table(db->name, ts->name);
    heap_cache_forget(ctx->bp, old_root);
    if (old_root && g_drop_hook) g_drop_hook(old_root);
    return result_ok_msg(affected);
}

// EXPLAIN FORMAT=JSON: la tabella del piano come {"query_plan": [ {step, depth, operation, details, estimated_rows,
// actual_rows}, ... ]} in una sola riga (depth = rientro: i passi di una subquery o di un ramo sono più a destra)
static void js_str(char** b, size_t* l, size_t* cap, const char* s) {
    size_t need = *l + strlen(s) * 6 + 8;
    if (need > *cap) { *cap = need * 2; *b = sv_realloc(*b, *cap); }
    char* o = *b + *l;
    *o++ = '"';
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"' || *p == '\\') { *o++ = '\\'; *o++ = (char)*p; }
        else if (*p < 0x20) o += sprintf(o, "\\u%04x", *p);
        else *o++ = (char)*p;
    }
    *o++ = '"';
    *l = (size_t)(o - *b);
    (*b)[*l] = '\0';
}
static void js_raw(char** b, size_t* l, size_t* cap, const char* s) {
    size_t n = strlen(s);
    if (*l + n + 1 > *cap) { *cap = (*l + n + 1) * 2; *b = sv_realloc(*b, *cap); }
    memcpy(*b + *l, s, n + 1);
    *l += n;
}

static QueryResult* explain_json(QueryResult* t) {
    if (!t || !t->success) return t;
    size_t l = 0, cap = 1024;
    char* b = sv_malloc(cap);
    b[0] = '\0';
    js_raw(&b, &l, &cap, "{\"query_plan\": [");
    for (int i = 0; i < t->num_rows; i++) {
        const Row* r = &t->rows[i];
        const char* op = r->cells[1].value ? r->cells[1].value : "";
        int depth = 0;
        while (op[0] == ' ') { op++; depth++; }
        char num[96];
        snprintf(num, sizeof(num), "%s{\"step\": %s, \"depth\": %d, \"operation\": ", i ? ", " : "", r->cells[0].value, depth / 4);
        js_raw(&b, &l, &cap, num);
        js_str(&b, &l, &cap, op);
        js_raw(&b, &l, &cap, ", \"details\": ");
        js_str(&b, &l, &cap, r->cells[2].value ? r->cells[2].value : "");
        snprintf(num, sizeof(num), ", \"estimated_rows\": %s", r->cells[3].is_null ? "null" : r->cells[3].value);
        js_raw(&b, &l, &cap, num);
        if (t->num_cols > 4) {
            snprintf(num, sizeof(num), ", \"actual_rows\": %s", r->cells[4].is_null ? "null" : r->cells[4].value);
            js_raw(&b, &l, &cap, num);
        }
        js_raw(&b, &l, &cap, "}");
    }
    js_raw(&b, &l, &cap, "]}");
    QueryResult* out = sv_calloc(1, sizeof(QueryResult));
    out->success = true;
    out->num_cols = 1;
    out->col_names = sv_calloc(1, sizeof(char*));
    out->col_types = sv_calloc(1, sizeof(char*));
    out->col_names[0] = sv_strdup("EXPLAIN");
    out->col_types[0] = sv_strdup("JSON");
    out->rows = sv_calloc(1, sizeof(Row));
    const char* vals[1] = { b };
    static const CellType ty[1] = { CELL_TYPE_STRING };
    if (db_row_set(&out->rows[0], 1, vals, ty)) out->num_rows = 1;
    free(b);
    db_result_free(t);
    return out;
}

static QueryResult* exec_explain(ExecCtx* ctx, ASTNode* n) {
    ASTNode* stmt = (ASTNode*)n->explain.stmt;
    if (!stmt || stmt->type != NODE_SELECT) return result_error_msg("EXPLAIN works with SELECT statements");
    QueryResult* r = run_select_ex(ctx, stmt, 0, true, n->explain.analyze);
    return n->explain.json ? explain_json(r) : r;
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
        case NODE_DROP_PROCEDURE: return db && !find_procedure(db, n->drop_trigger.name);
        case NODE_CREATE_PROCEDURE: {
            if (!db) return false;
            for (int i = 0; i < db->num_routines; i++) if (!strcasecmp(db->routines[i].name, n->create_trigger.name)) return true;
            return false;
        }
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

static QueryResult* executor_run_impl(ExecCtx* ctx, ASTNode* stmt);

// i testi calcolati restano validi per tutto lo statement (anche dentro i trigger, che
// rientrano qui): l'arena si libera solo all'uscita dal livello più esterno


// ── UPDATE / DELETE con più tabelle, alias, ORDER BY / LIMIT (MySQL) ──
// 1. per ogni tabella bersaglio una SELECT sulla stessa FROM / WHERE / ORDER BY / LIMIT raccoglie la
//    chiave (primaria, o tutte le colonne) delle righe da toccare e, per UPDATE, i nuovi valori;
// 2. poi le righe vengono cancellate / aggiornate per chiave con statement semplici (vincoli, trigger,
//    chiavi esterne come sempre). Tutte le SELECT vengono prima di ogni modifica, come in MySQL.

static void sql_lit(TextBuf* tb, const char* v) {
    if (!v) { tb_put(tb, "NULL", 4); return; }
    tb_put(tb, "'", 1);
    for (const char* p = v; *p; p++) { if (*p == '\'' || *p == '\\') tb_put(tb, p, 1); tb_put(tb, p, 1); }
    tb_put(tb, "'", 1);
}

static void sql_ident(TextBuf* tb, const char* name) {
    tb_put(tb, "`", 1);
    for (const char* p = name; *p; p++) { if (*p == '`') tb_put(tb, "`", 1); tb_put(tb, p, 1); }
    tb_put(tb, "`", 1);
}

static bool is_from_keyword(const char* w) {
    static const char* const K[] = { "ON", "USING", "JOIN", "INNER", "LEFT", "RIGHT", "CROSS", "NATURAL", "STRAIGHT_JOIN",
                                     "OUTER", "WHERE", "FULL", "SET", "ORDER", "LIMIT", "GROUP" };
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) if (!strcasecmp(w, K[i])) return true;
    return false;
}

// coppie alias -> tabella della clausola FROM (testo); un nome senza alias è alias di sé stesso
static int from_aliases(const char* from, char out[][2][SV_MAX_NAME_LEN], int max) {
    TokenList* tk = lexer_tokenize(from);
    if (!tk) return 0;
    int n = 0, depth = 0;
    bool expect_table = true;
    for (int i = 0; i < tk->count && n < max; i++) {
        Token* t = &tk->tokens[i];
        if (t->type == TOK_EOF) break;
        if (t->type == TOK_LPAREN) { depth++; continue; }
        if (t->type == TOK_RPAREN) { depth--; continue; }
        if (depth > 0) continue;
        if (t->type == TOK_COMMA || t->type == TOK_JOIN) { expect_table = true; continue; }
        if (!expect_table || t->type != TOK_IDENTIFIER || is_from_keyword(t->text)) continue;
        expect_table = false;
        int j = i;
        if (j + 2 < tk->count && tk->tokens[j + 1].type == TOK_DOT && tk->tokens[j + 2].type == TOK_IDENTIFIER) j += 2;   // db.tabella
        snprintf(out[n][1], SV_MAX_NAME_LEN, "%s", tk->tokens[j].text);
        snprintf(out[n][0], SV_MAX_NAME_LEN, "%s", tk->tokens[j].text);
        int k = j + 1;
        if (k < tk->count && tk->tokens[k].type == TOK_AS) k++;
        if (k < tk->count && tk->tokens[k].type == TOK_IDENTIFIER && !is_from_keyword(tk->tokens[k].text)) {
            snprintf(out[n][0], SV_MAX_NAME_LEN, "%s", tk->tokens[k].text);
            j = k;
        }
        n++;
        i = j;
    }
    lexer_free(tk);
    return n;
}

static QueryResult* run_sql_select(ExecCtx* ctx, const char* sql) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    QueryResult* r;
    if (!pr || !pr->success || pr->count != 1) r = result_error_msg(pr && pr->error[0] ? pr->error : "Out of memory");
    else r = run_select_ex(ctx, pr->statements[0], 0, false, false);
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    return r;
}

static QueryResult* exec_update(ExecCtx* ctx, ASTNode* n);
static QueryResult* exec_delete(ExecCtx* ctx, ASTNode* n);

static QueryResult* run_sql_dml(ExecCtx* ctx, const char* sql) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    QueryResult* r;
    if (!pr || !pr->success || pr->count != 1) r = result_error_msg(pr && pr->error[0] ? pr->error : "Out of memory");
    else r = pr->statements[0]->type == NODE_DELETE ? exec_delete(ctx, pr->statements[0]) : exec_update(ctx, pr->statements[0]);
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    return r;
}

typedef struct {
    char         alias[SV_MAX_NAME_LEN];
    char         table[SV_MAX_NAME_LEN];
    int          idcols[SV_MAX_COLUMNS];   // colonne che identificano la riga
    int          nid;
    int*         sets;                     // UPDATE: indici in n->update_stmt (colonne di questa tabella); num_sets posti
    int          nsets;
    QueryResult* rows;
} DmlTarget;

// setbuf: 8 * num_sets interi, una fetta per tabella bersaglio (exec_dml_multi)
static QueryResult* exec_dml_multi_impl(ExecCtx* ctx, ASTNode* n, int* setbuf) {
    bool del = n->type == NODE_DELETE;
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    const char* from = del ? n->delete_stmt.multi_from : n->update_stmt.multi_from;
    const char* where = del ? n->delete_stmt.multi_where : n->update_stmt.multi_where;
    const char* ol = del ? n->delete_stmt.order_limit : n->update_stmt.order_limit;
    char al[16][2][SV_MAX_NAME_LEN];
    int nal = from_aliases(from, al, 16);
    if (nal == 0) return result_error_msg("No table in the FROM clause");
    if (ol && nal > 1) return result_error_msg("ORDER BY / LIMIT cannot be used with more than one table (as in MySQL)");
    DmlTarget tg[8];
    int ntg = 0;
    char msg[400];
    // tabelle bersaglio
    if (del) {
        // (P3.12: oltre l'8ª tabella le altre erano ignorate senza errore: le loro righe restavano)
        if (n->delete_stmt.num_targets > 8) return result_error_msg("Too many tables in DELETE (max 8)");
        for (int i = 0; i < n->delete_stmt.num_targets; i++) {
            memset(&tg[ntg], 0, sizeof(tg[ntg]));
            snprintf(tg[ntg].alias, sizeof(tg[ntg].alias), "%s", n->delete_stmt.targets[i]);
            ntg++;
        }
    } else {
        for (int k = 0; k < n->update_stmt.num_sets; k++) {
            const char* q = n->update_stmt.set_tables[k][0] ? n->update_stmt.set_tables[k] : al[0][0];
            if (!q[0] || nal == 1) q = al[0][0];
            int f = -1;
            for (int i = 0; i < ntg; i++) if (!strcasecmp(tg[i].alias, q)) f = i;
            if (f < 0) {
                if (ntg >= 8) return result_error_msg("Too many tables in UPDATE (max 8)");
                memset(&tg[ntg], 0, sizeof(tg[ntg]));
                snprintf(tg[ntg].alias, sizeof(tg[ntg].alias), "%s", q);
                tg[ntg].sets = setbuf + (size_t)ntg * (size_t)n->update_stmt.num_sets;
                f = ntg++;
            }
            tg[f].sets[tg[f].nsets++] = k;
        }
    }
    for (int i = 0; i < ntg; i++) {
        int a = -1;
        for (int k = 0; k < nal; k++) if (!strcasecmp(al[k][0], tg[i].alias)) a = k;
        if (a < 0) for (int k = 0; k < nal; k++) if (!strcasecmp(al[k][1], tg[i].alias)) a = k;
        if (a < 0) { snprintf(msg, sizeof(msg), "Unknown table '%.64s' in %s", tg[i].alias, del ? "DELETE" : "UPDATE"); return result_error_msg(msg); }
        snprintf(tg[i].alias, sizeof(tg[i].alias), "%s", al[a][0]);
        snprintf(tg[i].table, sizeof(tg[i].table), "%s", al[a][1]);
        TableSchema* ts = catalog_get_table(ctx->catalog, db->name, tg[i].table);
        if (!ts) { snprintf(msg, sizeof(msg), "Table '%.64s' not found", tg[i].table); return result_error_msg(msg); }
        if (!executor_priv_ok(ctx, db->name, ts->name, del ? AUTH_DELETE : AUTH_UPDATE, msg, sizeof(msg))) return result_error_msg(msg);
        for (int k = 0; k < ts->num_constraints && !tg[i].nid; k++)
            if (ts->constraints[k].kind == CONS_PRIMARY)
                for (int j = 0; j < ts->constraints[k].ncols; j++) tg[i].idcols[tg[i].nid++] = ts->constraints[k].cols[j];
        if (!tg[i].nid) for (int c = 0; c < ts->num_columns; c++) if (ts->columns[c].primary_key) tg[i].idcols[tg[i].nid++] = c;
        if (!tg[i].nid) for (int c = 0; c < ts->num_columns; c++) tg[i].idcols[tg[i].nid++] = c;
        for (int s = 0; s < tg[i].nsets; s++)
            if (catalog_get_col_index(ts, n->update_stmt.set_cols[tg[i].sets[s]]) < 0)
                return unknown_column_error(n->update_stmt.set_cols[tg[i].sets[s]], "UPDATE");
    }
    // 1. le righe da toccare (tutte prima di cambiare qualcosa)
    QueryResult* fail = NULL;
    for (int i = 0; i < ntg && !fail; i++) {
        TableSchema* ts = catalog_get_table(ctx->catalog, db->name, tg[i].table);
        TextBuf q = {0};
        tb_put(&q, "SELECT ", 7);
        for (int k = 0; k < tg[i].nid; k++) {
            if (k) tb_put(&q, ", ", 2);
            sql_ident(&q, tg[i].alias);
            tb_put(&q, ".", 1);
            sql_ident(&q, ts->columns[tg[i].idcols[k]].name);
        }
        for (int s = 0; s < tg[i].nsets; s++) {
            const char* t = n->update_stmt.set_texts[tg[i].sets[s]];
            tb_put(&q, ", (", 3);
            tb_put(&q, t ? t : "NULL", strlen(t ? t : "NULL"));
            tb_put(&q, ")", 1);
        }
        tb_put(&q, " FROM ", 6);
        tb_put(&q, from, strlen(from));
        if (where) { tb_put(&q, " WHERE ", 7); tb_put(&q, where, strlen(where)); }
        if (ol) { tb_put(&q, " ", 1); tb_put(&q, ol, strlen(ol)); }
        tb_put(&q, "", 1);
        if (q.oom) { free(q.p); fail = result_error_msg("Out of memory"); break; }
        tg[i].rows = run_sql_select(ctx, q.p);
        free(q.p);
        if (!tg[i].rows || !tg[i].rows->success) { fail = tg[i].rows; tg[i].rows = NULL; }
    }
    // 2. le modifiche, per chiave
    long long affected = 0;
    for (int i = 0; i < ntg && !fail; i++) {
        TableSchema* ts = catalog_get_table(ctx->catalog, db->name, tg[i].table);
        QueryResult* r = tg[i].rows;
        StrMap seen = {0};
        bool single = tg[i].nid == 1;
        TextBuf batch = {0};
        int inb = 0;
        for (int row = 0; row <= r->num_rows && !fail; row++) {
            bool flush = row == r->num_rows || (del && single && inb >= 500);
            if (flush && inb) {                                    // DELETE ... WHERE id IN (...)
                tb_put(&batch, ")", 1);
                tb_put(&batch, "", 1);
                QueryResult* d = batch.oom ? result_error_msg("Out of memory") : run_sql_dml(ctx, batch.p);
                free(batch.p);
                memset(&batch, 0, sizeof(batch));
                inb = 0;
                if (!d || !d->success) { fail = d ? d : result_error_msg("Out of memory"); break; }
                affected += d->rows_affected;
                db_result_free(d);
            }
            if (row == r->num_rows) break;
            const Row* rw = &r->rows[row];
            // una riga legata a più righe delle altre tabelle si tocca una volta sola
            TextBuf key = {0};
            for (int k = 0; k < tg[i].nid; k++) {
                const Cell* cl = &rw->cells[k];
                tb_put(&key, cl->is_null ? "\x01" : cl->value, strlen(cl->is_null ? "\x01" : cl->value));
                tb_put(&key, "\x02", 1);
            }
            tb_put(&key, "", 1);
            char kb[KEY_BUF];
            key_text(kb, sizeof(kb), key.p ? key.p : "");
            free(key.p);
            SMSlot* sl = sm_slot(&seen, kb, true);
            if (!sl) { fail = result_error_msg("Out of memory"); break; }
            if (sl->val >= 0) continue;
            sl->val = 1;
            if (del && single) {
                if (!inb) { tb_put(&batch, "DELETE FROM ", 12); sql_ident(&batch, ts->name); tb_put(&batch, " WHERE ", 7); sql_ident(&batch, ts->columns[tg[i].idcols[0]].name); tb_put(&batch, " IN (", 5); }
                else tb_put(&batch, ", ", 2);
                sql_lit(&batch, rw->cells[0].is_null ? NULL : rw->cells[0].value);
                inb++;
                continue;
            }
            TextBuf st = {0};
            if (del) { tb_put(&st, "DELETE FROM ", 12); sql_ident(&st, ts->name); }
            else {
                tb_put(&st, "UPDATE ", 7);
                sql_ident(&st, ts->name);
                tb_put(&st, " SET ", 5);
                for (int s = 0; s < tg[i].nsets; s++) {
                    if (s) tb_put(&st, ", ", 2);
                    sql_ident(&st, n->update_stmt.set_cols[tg[i].sets[s]]);
                    tb_put(&st, " = ", 3);
                    const Cell* cl = &rw->cells[tg[i].nid + s];
                    sql_lit(&st, cl->is_null ? NULL : cl->value);
                }
            }
            tb_put(&st, " WHERE ", 7);
            for (int k = 0; k < tg[i].nid; k++) {
                if (k) tb_put(&st, " AND ", 5);
                sql_ident(&st, ts->columns[tg[i].idcols[k]].name);
                const Cell* cl = &rw->cells[k];
                if (cl->is_null) tb_put(&st, " IS NULL", 8);
                else { tb_put(&st, " = ", 3); sql_lit(&st, cl->value); }
            }
            tb_put(&st, "", 1);
            QueryResult* d = st.oom ? result_error_msg("Out of memory") : run_sql_dml(ctx, st.p);
            free(st.p);
            if (!d || !d->success) { fail = d ? d : result_error_msg("Out of memory"); break; }
            affected += d->rows_affected;
            db_result_free(d);
            ts = catalog_get_table(ctx->catalog, db->name, tg[i].table);
        }
        free(batch.p);
        sm_free(&seen);
    }
    for (int i = 0; i < ntg; i++) db_result_free(tg[i].rows);
    if (fail) return fail;
    snprintf(msg, sizeof(msg), "%lld", affected);
    return result_ok_msg((int)affected);
}

static QueryResult* exec_dml_multi(ExecCtx* ctx, ASTNode* n) {
    size_t ns = n->type == NODE_DELETE ? 0 : (size_t)n->update_stmt.num_sets;
    int* setbuf = sv_calloc(8 * ns + 1, sizeof(int));
    if (!setbuf) return result_error_msg("Out of memory");
    QueryResult* r = exec_dml_multi_impl(ctx, n, setbuf);
    free(setbuf);
    return r;
}

// ── INSERT IGNORE / REPLACE / ON DUPLICATE KEY UPDATE (MySQL) ──
// Le righe vanno una alla volta come INSERT normali (trigger, vincoli, AUTO_INCREMENT come sempre);
// una riga respinta per una chiave duplicata viene saltata (IGNORE), sostituisce le righe in
// conflitto (REPLACE) o aggiorna la riga esistente (ON DUPLICATE KEY UPDATE, con VALUES(col)).

static ASTExpr* lit_expr(const char* text) {
    ASTExpr* e = sv_calloc(1, sizeof(ASTExpr));
    if (!e) return NULL;
    if (!text) { e->type = EXPR_NULL; return e; }
    e->type = EXPR_STRING;
    e->str_val = sv_strdup(text);
    if (!e->str_val) { free(e); return NULL; }
    return e;
}

// VALUES(col) e alias.col -> il valore proposto per col
static ASTExpr* dup_subst(ASTExpr* e, const TableSchema* ts, const char* alias) {
    if (!e) return NULL;
    if (e->type == EXPR_FUNC && !strcmp(e->func, "VALUES") && e->nargs == 1 && e->args[0]->type == EXPR_COLUMN) {
        int ci = catalog_get_col_index((TableSchema*)ts, e->args[0]->col_name);
        if (ci >= 0) { ASTExpr* l = lit_expr(ci < g_dup.ncols ? g_dup.text[ci] : NULL); ast_expr_free(e); return l; }
    }
    if (e->type == EXPR_COLUMN && alias[0] && !strcasecmp(e->table_name, alias)) {
        int ci = catalog_get_col_index((TableSchema*)ts, e->col_name);
        if (ci >= 0) { ASTExpr* l = lit_expr(ci < g_dup.ncols ? g_dup.text[ci] : NULL); ast_expr_free(e); return l; }
    }
    e->left = dup_subst(e->left, ts, alias);
    e->right = dup_subst(e->right, ts, alias);
    for (int i = 0; i < e->nargs; i++) e->args[i] = dup_subst(e->args[i], ts, alias);
    return e;
}

// WHERE chiave = valori della riga in conflitto
static ASTExpr* dup_where(const TableSchema* ts, char* err, size_t errn) {
    char sql[4096];
    size_t o = 0;
    for (int k = 0; k < g_dup.nkey && o < sizeof(sql) - 200; k++) {
        int ci = g_dup.keycols[k];
        const char* v = ci < g_dup.ncols ? g_dup.text[ci] : NULL;
        o += (size_t)snprintf(sql + o, sizeof(sql) - o, "%s`%s` = '", k ? " AND " : "", ts->columns[ci].name);
        for (const char* q = v ? v : ""; *q && o < sizeof(sql) - 8; q++) {
            if (*q == '\'' || *q == '\\') sql[o++] = *q;
            sql[o++] = *q;
        }
        o += (size_t)snprintf(sql + o, sizeof(sql) - o, "'");
    }
    TokenList* tk = lexer_tokenize(sql);
    ASTExpr* e = tk ? parser_parse_expr(tk, err, errn) : NULL;
    if (tk) lexer_free(tk);
    return e;
}

static QueryResult* exec_insert_mysql(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) return exec_insert(ctx, n);                         // il messaggio giusto lo dà exec_insert
    {   // REPLACE cancella le righe in conflitto, ON DUPLICATE KEY UPDATE le aggiorna
        char pm[300];
        if (!executor_priv_ok(ctx, db->name, ts->name, AUTH_INSERT, pm, sizeof(pm)) ||
            (n->insert_stmt.mode == INS_REPLACE && !executor_priv_ok(ctx, db->name, ts->name, AUTH_DELETE, pm, sizeof(pm))) ||
            (n->insert_stmt.num_dup > 0 && !executor_priv_ok(ctx, db->name, ts->name, AUTH_UPDATE, pm, sizeof(pm))))
            return result_error_msg(pm);
    }
    char tsname[SV_MAX_NAME_LEN];
    snprintf(tsname, sizeof(tsname), "%s", ts->name);
    // righe: i VALUES, o il risultato della SELECT trasformato in letterali
    ASTExpr** vals = n->insert_stmt.values;
    int nrows = n->insert_stmt.num_rows, nv = n->insert_stmt.num_values;
    ASTExpr** owned = NULL;
    if (n->insert_stmt.select) {
        QueryResult* sr = run_select_ex(ctx, n->insert_stmt.select, 0, false, false);
        if (!sr || !sr->success) return sr ? sr : result_error_msg("Out of memory");
        nrows = sr->num_rows;
        nv = sr->num_cols;
        owned = sv_calloc((size_t)(nrows * nv) + 1, sizeof(ASTExpr*));
        for (int r = 0; owned && r < nrows; r++)
            for (int c = 0; c < nv; c++) {
                const Cell* cell = &sr->rows[r].cells[c];
                owned[r * nv + c] = lit_expr(cell->is_null ? NULL : cell->value);
            }
        db_result_free(sr);
        if (!owned) return result_error_msg("Out of memory");
        vals = owned;
    }
    long long affected = 0;
    int64_t last_id = 0;
    QueryResult* fail = NULL;
    for (int i = 0; i < nrows && !fail; i++) {
        ASTNode one = *n;                                        // copia superficiale: niente da liberare
        one.insert_stmt.select = NULL;
        one.insert_stmt.values = vals + (size_t)i * (size_t)nv;
        one.insert_stmt.num_rows = 1;
        one.insert_stmt.num_values = nv;
        one.insert_stmt.mode = INS_NORMAL;
        one.insert_stmt.num_dup = 0;
        for (int attempt = 0; attempt < SV_MAX_CONSTRAINTS + 2 && !fail; attempt++) {
            dup_clear();
            QueryResult* r = exec_insert(ctx, &one);
            if (r && r->success) {
                affected += r->rows_affected;
                if (r->last_insert_id && !last_id) last_id = r->last_insert_id;   // il primo, come MySQL
                db_result_free(r);
                break;
            }
            if (!r || !g_dup.set || (n->insert_stmt.mode == INS_NORMAL && n->insert_stmt.num_dup == 0)) { fail = r ? r : result_error_msg("Out of memory"); break; }
            db_result_free(r);
            if (n->insert_stmt.mode == INS_IGNORE && n->insert_stmt.num_dup == 0) break;      // riga saltata
            char err[300] = "";
            ASTExpr* where = dup_where(ts, err, sizeof(err));
            if (!where) { fail = result_error_msg(err[0] ? err : "Out of memory"); break; }
            ASTNode op;
            memset(&op, 0, sizeof(op));
            if (n->insert_stmt.mode == INS_REPLACE) {            // via le righe in conflitto, poi di nuovo l'INSERT
                op.type = NODE_DELETE;
                snprintf(op.delete_stmt.table_name, sizeof(op.delete_stmt.table_name), "%s", tsname);
                op.delete_stmt.where = where;
                QueryResult* d = exec_delete(ctx, &op);
                ast_expr_free(where);
                if (!d || !d->success) { fail = d ? d : result_error_msg("Out of memory"); break; }
                affected += d->rows_affected;
                db_result_free(d);
                continue;
            }
            op.type = NODE_UPDATE;                               // ON DUPLICATE KEY UPDATE
            snprintf(op.update_stmt.table_name, sizeof(op.update_stmt.table_name), "%s", tsname);
            op.update_stmt.where = where;
            op.update_stmt.set_cols = n->insert_stmt.dup_cols;  // gli stessi nomi (non si liberano qui)
            op.update_stmt.set_exprs = sv_calloc((size_t)n->insert_stmt.num_dup + 1, sizeof(ASTExpr*));
            if (!op.update_stmt.set_exprs) { ast_expr_free(where); fail = result_error_msg("Out of memory"); break; }
            op.update_stmt.num_sets = n->insert_stmt.num_dup;
            for (int k = 0; k < n->insert_stmt.num_dup; k++)
                op.update_stmt.set_exprs[k] = dup_subst(ast_expr_clone(n->insert_stmt.dup_exprs[k]), ts, n->insert_stmt.row_alias);
            QueryResult* u = exec_update(ctx, &op);
            for (int k = 0; k < op.update_stmt.num_sets; k++) ast_expr_free(op.update_stmt.set_exprs[k]);
            free(op.update_stmt.set_exprs);
            ast_expr_free(where);
            if (!u || !u->success) { fail = u ? u : result_error_msg("Out of memory"); break; }
            affected += u->rows_affected > 0 ? 2 : 0;            // come MySQL: 2 = riga esistente aggiornata
            db_result_free(u);
            break;
        }
        ts = catalog_get_table(ctx->catalog, db->name, tsname);
        if (!ts) break;
    }
    dup_clear();
    if (owned) { for (int i = 0; i < nrows * nv; i++) ast_expr_free(owned[i]); free(owned); }
    if (fail) return fail;
    QueryResult* ok = result_ok_msg((int)affected);
    if (ok && last_id > 0) ok->last_insert_id = (long long)last_id;
    if (last_id > 0) g_last_insert_id = last_id;              // ogni riga l'aveva impostato al suo id
    return ok;
}

// SELECT ... INTO v1, @v2: una riga al massimo; nessuna riga = condizione NOT FOUND (le variabili restano come sono)
static QueryResult* exec_select_into(ExecCtx* ctx, ASTNode* n) {
    int ni = n->select_stmt.num_into;
    n->select_stmt.num_into = 0;
    QueryResult* r = run_select(ctx, n, 0, false);
    n->select_stmt.num_into = ni;
    if (!r || !r->success) return r;
    char msg[300];
    if (r->num_cols != ni) {
        db_result_free(r);
        return result_error_msg("The used SELECT statements have a different number of columns than INTO variables");
    }
    if (r->num_rows > 1) { db_result_free(r); return result_error_msg("Result consisted of more than one row"); }
    if (r->num_rows == 0) {
        db_result_free(r);
        ctx->rt_notfound = true;
        return result_ok_msg(0);
    }
    TrigScope* sc = ctx->routine;
    for (int k = 0; k < ni; k++) {
        const Cell* cell = &r->rows[0].cells[k];
        EvalVal v;
        if (cell->is_null) ev_null(&v);
        else {
            v.null = false; v.ci = false; v.is_num = v.is_int = false; v.is_dec = false;
            v.str = cell->value ? cell->value : "";
            double d;
            const char* ct = r->col_types ? r->col_types[k] : NULL;   // colonne numeriche: valori numerici (per @x)
            if (ct && (strstr(ct, "INT") || strstr(ct, "FLOAT") || strstr(ct, "DOUBLE") || strstr(ct, "DECIMAL")) && text_is_number(v.str, &d)) {
                if (strchr(v.str, '.') || strchr(v.str, 'e') || strchr(v.str, 'E')) ev_num(&v, d);
                else ev_int(&v, strtoll(cell->value, NULL, 10));
            }
        }
        const char* name = n->select_stmt.into[k];
        if (name[0] == '@') { uservar_set(name + 1, &v); continue; }
        if (!sc || !rt_set_var(sc, name, &v)) {
            snprintf(msg, sizeof(msg), "Undeclared variable: %.64s", name);
            db_result_free(r);
            return result_error_msg(msg);
        }
    }
    db_result_free(r);
    return result_ok_msg(1);
}

// SET @a = espr, @b := espr (variabili di sessione)
QueryResult* executor_set_session_vars(ExecCtx* ctx, ASTNode* stmt) {
    for (int k = 0; k < stmt->set_n; k++) {
        BindCtx b;
        memset(&b, 0, sizeof(b));
        b.mode = BIND_ROW; b.clause = "SET"; b.x = ctx; b.outer = ctx->trig_bind;
        bind_expr(stmt->set_exprs[k], &b);
        if (b.err[0]) return result_error_msg(b.err);
        EvalCtx ec;
        memset(&ec, 0, sizeof(ec));
        ec.x = ctx; ec.outer = ctx->trig_eval;
        EvalVal v;
        eval_expr(stmt->set_exprs[k], &ec, &v);
        if (ec.err[0]) return result_error_msg(ec.err);
        uservar_set(stmt->set_names[k], &v);
    }
    return result_ok_msg(0);
}

static QueryResult* executor_run_impl(ExecCtx* ctx, ASTNode* stmt);
static QueryResult* exec_merge(ExecCtx* ctx, ASTNode* n);

// testo SQL di un solo statement, eseguito qui (MERGE)
static QueryResult* run_sql_text(ExecCtx* ctx, const char* sql) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    QueryResult* r;
    if (!pr || !pr->success || pr->count != 1) {
        char m[400];
        snprintf(m, sizeof(m), "MERGE: %.300s", pr ? pr->error : "out of memory");
        r = result_error_msg(m);
    } else r = executor_run(ctx, pr->statements[0]);
    parse_result_free(pr);
    lexer_free(tk);
    return r;
}

// valore come letterale SQL (apici e backslash raddoppiati)
static void sql_literal(TextBuf* tb, const Cell* c) {
    if (c->is_null) { tb_put(tb, "NULL", 4); return; }
    tb_put(tb, "'", 1);
    for (const char* p = c->value ? c->value : ""; *p; p++) {
        if (*p == '\'' || *p == '\\') tb_put(tb, p, 1);
        tb_put(tb, p, 1);
    }
    tb_put(tb, "'", 1);
}

// MERGE INTO t USING s ON cond WHEN ...: le righe di s senza corrispondenza si calcolano prima (sul contenuto
// iniziale di t), poi UPDATE / DELETE delle righe con corrispondenza, poi gli INSERT
static QueryResult* exec_merge(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    const char* T = n->merge.target;
    const char* ta = n->merge.talias;
    size_t cap = SV_MAX_SQL_LEN;
    char* q = sv_malloc(cap);
    if (!q) return result_error_msg("Out of memory");
    long long affected = 0;
    QueryResult* fail = NULL;
    // 1. righe nuove (NOT MATCHED): valori calcolati adesso
    QueryResult** ins = sv_calloc((size_t)n->merge.nwhen + 1, sizeof(QueryResult*));   // una per clausola WHEN
    if (!ins) { free(q); return result_error_msg("Out of memory"); }
    for (int w = 0; w < n->merge.nwhen && !fail; w++) {
        MergeWhen* c = &n->merge.when[w];
        if (c->kind != 2) continue;
        TextBuf tb = {0};
        tb_put(&tb, "SELECT ", 7);
        for (int k = 0; k < c->n; k++) { if (k) tb_put(&tb, ", ", 2); tb_put(&tb, c->vals[k], strlen(c->vals[k])); }
        snprintf(q, cap, " FROM %s WHERE NOT EXISTS (SELECT 1 FROM `%s` AS `%s` WHERE %s)%s%s%s",
                 n->merge.source, T, ta, n->merge.on, c->cond ? " AND (" : "", c->cond ? c->cond : "", c->cond ? ")" : "");
        tb_put(&tb, q, strlen(q));
        if (tb.oom || !tb.p) { free(tb.p); fail = result_error_msg("Out of memory"); break; }
        ins[w] = run_sql_text(ctx, tb.p);
        free(tb.p);
        if (!ins[w] || !ins[w]->success) { fail = ins[w]; ins[w] = NULL; }
    }
    // 2. righe con corrispondenza: la prima clausola WHEN MATCHED che vale
    TextBuf prev = {0};                                   // condizioni delle clausole precedenti
    for (int w = 0; w < n->merge.nwhen && !fail; w++) {
        MergeWhen* c = &n->merge.when[w];
        if (c->kind == 2) continue;
        char extra[SV_MAX_SQL_LEN / 4];
        snprintf(extra, sizeof(extra), "%s%s%s%s%s%s", c->cond ? " AND (" : "", c->cond ? c->cond : "", c->cond ? ")" : "",
                 prev.p ? " AND NOT (" : "", prev.p ? prev.p : "", prev.p ? ")" : "");
        TextBuf tb = {0};
        if (c->kind == 1) {
            snprintf(q, cap, "DELETE FROM `%s` AS `%s` WHERE EXISTS (SELECT 1 FROM %s WHERE (%s)%s)", T, ta, n->merge.source, n->merge.on, extra);
            tb_put(&tb, q, strlen(q));
        } else {
            snprintf(q, cap, "UPDATE `%s` AS `%s` SET ", T, ta);
            tb_put(&tb, q, strlen(q));
            for (int k = 0; k < c->n; k++) {
                snprintf(q, cap, "%s`%s` = (SELECT %s FROM %s WHERE (%s)%s LIMIT 1)", k ? ", " : "", c->cols[k], c->vals[k],
                         n->merge.source, n->merge.on, extra);
                tb_put(&tb, q, strlen(q));
            }
            snprintf(q, cap, " WHERE EXISTS (SELECT 1 FROM %s WHERE (%s)%s)", n->merge.source, n->merge.on, extra);
            tb_put(&tb, q, strlen(q));
        }
        if (tb.oom || !tb.p) { free(tb.p); fail = result_error_msg("Out of memory"); break; }
        QueryResult* r = run_sql_text(ctx, tb.p);
        free(tb.p);
        if (!r || !r->success) { fail = r; break; }
        affected += r->rows_affected;
        db_result_free(r);
        // le clausole dopo valgono solo per le righe che questa non ha preso
        char one[SV_MAX_SQL_LEN / 4];
        snprintf(one, sizeof(one), "EXISTS (SELECT 1 FROM %s WHERE (%s)%s%s%s)", n->merge.source, n->merge.on,
                 c->cond ? " AND (" : "", c->cond ? c->cond : "", c->cond ? ")" : "");
        if (prev.p) tb_put(&prev, " OR ", 4);
        tb_put(&prev, one, strlen(one));
    }
    free(prev.p);
    // 3. INSERT delle righe senza corrispondenza
    for (int w = 0; w < n->merge.nwhen && !fail; w++) {
        MergeWhen* c = &n->merge.when[w];
        if (c->kind != 2 || !ins[w] || ins[w]->num_rows == 0) continue;
        TextBuf tb = {0};
        snprintf(q, cap, "INSERT INTO `%s` ", T);
        tb_put(&tb, q, strlen(q));
        if (c->has_cols) {
            tb_put(&tb, "(", 1);
            for (int k = 0; k < c->ncols; k++) { if (k) tb_put(&tb, ", ", 2); tb_put(&tb, "`", 1); tb_put(&tb, c->cols[k], strlen(c->cols[k])); tb_put(&tb, "`", 1); }
            tb_put(&tb, ") ", 2);
        }
        tb_put(&tb, "VALUES ", 7);
        for (int r = 0; r < ins[w]->num_rows; r++) {
            tb_put(&tb, r ? ", (" : "(", r ? 3 : 1);
            for (int k = 0; k < ins[w]->num_cols; k++) { if (k) tb_put(&tb, ", ", 2); sql_literal(&tb, &ins[w]->rows[r].cells[k]); }
            tb_put(&tb, ")", 1);
        }
        if (tb.oom || !tb.p) { free(tb.p); fail = result_error_msg("Out of memory"); break; }
        QueryResult* r = run_sql_text(ctx, tb.p);
        free(tb.p);
        if (!r || !r->success) { fail = r; break; }
        affected += r->rows_affected;
        db_result_free(r);
    }
    for (int w = 0; w < n->merge.nwhen; w++) db_result_free(ins[w]);
    free(ins);
    free(q);
    if (fail) return fail;
    return result_ok_msg((int)affected);
}

// INSERT / UPDATE / DELETE ... RETURNING: lo statement, poi le espressioni sulle righe scritte (nuove; vecchie per DELETE)
static QueryResult* exec_returning(ExecCtx* ctx, ASTNode* stmt) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    const char* tname = stmt->type == NODE_INSERT ? stmt->insert_stmt.table_name
                      : stmt->type == NODE_UPDATE ? stmt->update_stmt.table_name : stmt->delete_stmt.table_name;
    if (stmt->type == NODE_UPDATE && stmt->update_stmt.multi_from) return result_error_msg("RETURNING is not supported with UPDATE ... ORDER BY / LIMIT or joins");
    if (stmt->type == NODE_DELETE && stmt->delete_stmt.multi_from) return result_error_msg("RETURNING is not supported with DELETE ... ORDER BY / LIMIT or joins");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, tname);
    if (!ts) return executor_run_impl(ctx, stmt);              // il messaggio d'errore giusto
    RetCapture saved = g_cap;
    memset(&g_cap, 0, sizeof(g_cap));
    g_cap.on = true;
    g_cap.depth = ctx->trig_depth;
    snprintf(g_cap.table, sizeof(g_cap.table), "%s", ts->name);
    int nret = stmt->nret;
    stmt->nret = 0;                                             // lo statement normale
    QueryResult* r = executor_run_impl(ctx, stmt);
    stmt->nret = nret;
    RetCapture cap = g_cap;
    g_cap = saved;
    QueryResult* out = NULL;
    if (r && r->success) {
        ts = catalog_get_table(ctx->catalog, db->name, tname);
        Source s;
        memset(&s, 0, sizeof(s));
        snprintf(s.name, sizeof(s.name), "%s", ts->name);
        snprintf(s.table, sizeof(s.table), "%s", ts->name);
        s.ts = ts;
        BindCtx b;
        memset(&b, 0, sizeof(b));
        b.src = &s; b.nsrc = 1; b.mode = BIND_ROW; b.x = ctx; b.outer = ctx->trig_bind; b.clause = "RETURNING";
        // colonne: * diventa tutte le colonne della tabella. Gli array per colonna stanno nell'heap: sullo stack
        // erano 168 KB (CR-M1)
        struct RetCols {
            ASTExpr* cols[SV_MAX_COLUMNS];
            char names[SV_MAX_COLUMNS][128];
            ASTExpr* owned[SV_MAX_COLUMNS];
            const char* np[SV_MAX_COLUMNS];
            bool all_int[SV_MAX_COLUMNS], all_num[SV_MAX_COLUMNS];
            char* vals[SV_MAX_COLUMNS];
        }* rc = sv_calloc(1, sizeof(struct RetCols));
        ASTExpr** cols = rc->cols;
        char (*names)[128] = rc->names;
        ASTExpr** owned = rc->owned;
        int nc = 0;
        int nown = 0;
        for (int k = 0; k < nret && nc < SV_MAX_COLUMNS; k++) {
            ASTExpr* e = stmt->returning[k];
            if (e->type == EXPR_STAR) {
                for (int ci = 0; ci < ts->num_columns && nc < SV_MAX_COLUMNS; ci++) {
                    ASTExpr* c = sv_calloc(1, sizeof(ASTExpr));
                    if (!c) break;
                    c->type = EXPR_COLUMN;
                    snprintf(c->col_name, sizeof(c->col_name), "%s", ts->columns[ci].name);
                    owned[nown++] = c;
                    snprintf(names[nc], sizeof(names[nc]), "%s", ts->columns[ci].name);
                    cols[nc++] = c;
                }
                continue;
            }
            if (e->alias[0]) snprintf(names[nc], sizeof(names[nc]), "%s", e->alias);
            else if (e->type == EXPR_COLUMN) snprintf(names[nc], sizeof(names[nc]), "%s", e->col_name);
            else ast_expr_format(e, names[nc], sizeof(names[nc]));
            cols[nc++] = e;
        }
        for (int k = 0; k < nc; k++) bind_expr(cols[k], &b);
        if (b.err[0]) out = result_error_msg(b.err);
        else {
            const char** np = rc->np;
            for (int k = 0; k < nc; k++) np[k] = names[k];
            out = text_result(nc, np);
            EvalCtx ec;
            memset(&ec, 0, sizeof(ec));
            Tuple* cur[1];
            ec.src = &s; ec.nsrc = 1; ec.cur = cur; ec.x = ctx; ec.outer = ctx->trig_eval;
            int cap_rows = 0;
            bool* all_int = rc->all_int;
            bool* all_num = rc->all_num;
            for (int k = 0; k < nc; k++) all_int[k] = all_num[k] = true;
            for (int i = 0; out && i < cap.n; i++) {
                cur[0] = cap.v[i];
                char** vals = rc->vals;
                memset(vals, 0, sizeof(rc->vals));
                for (int k = 0; k < nc; k++) {
                    EvalVal v;
                    eval_expr(cols[k], &ec, &v);
                    if (!v.null && !v.is_int) all_int[k] = false;
                    if (!v.null && !v.is_num) all_num[k] = false;
                    vals[k] = v.null ? NULL : sv_strdup(ev_s(&v));
                }
                if (ec.err[0]) { for (int k = 0; k < nc; k++) free(vals[k]); db_result_free(out); out = result_error_msg(ec.err); break; }
                text_row(out, &cap_rows, (const char* const*)vals);
                for (int k = 0; k < nc; k++) free(vals[k]);
            }
            for (int k = 0; out && out->success && k < nc; k++) {      // tipi: delle colonne, o dai valori
                const char* ty = cols[k]->type == EXPR_COLUMN && cols[k]->bound > 0 && cols[k]->outer == 0
                                 ? datatype_name(ts->columns[BOUND_COL(cols[k]->bound)].type)
                                 : cap.n && all_int[k] ? "BIGINT" : cap.n && all_num[k] ? "FLOAT" : "VARCHAR";
                free(out->col_types[k]);
                out->col_types[k] = sv_strdup(ty);
            }
            if (out && out->success) out->rows_affected = r->rows_affected;
        }
        for (int k = 0; k < nown; k++) ast_expr_free(owned[k]);
        free(rc);
        db_result_free(r);
    } else out = r;
    for (int i = 0; i < cap.n; i++) tuple_free(cap.v[i]);
    free(cap.v);
    return out;
}

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt) {
    if (g_run_depth == 0) heap_damage_clear();
    sv_scan_abort(NULL);
    g_stmt_gen++;
    g_run_depth++;
    g_select_total = -1;
    QueryResult* r = stmt->nret > 0 ? exec_returning(ctx, stmt) : executor_run_impl(ctx, stmt);
    if (--g_run_depth == 0) {
        arena_free(&g_text_arena);
        char dm[200];
        if (heap_damage(dm, sizeof(dm)) && (!r || r->success)) {   // pagina danneggiata: mai un risultato parziale
            if (r) db_result_free(r);
            r = result_error_msg(dm);
        }
        const char* why = sv_scan_abort_why();          // scansione fermata da un errore certo: il suo messaggio
        if (why && r && !r->success && !strncmp(r->error, "Query cancelled", 15)) snprintf(r->error, sizeof(r->error), "%s", why);
        sv_scan_abort(NULL);                            // finito lo statement: le altre scansioni del thread ripartono
        // FOUND_ROWS() e ROW_COUNT() riguardano lo statement di primo livello (come in MySQL)
        if (r && r->success && stmt->type == NODE_SELECT)
            g_found_rows = stmt->select_stmt.calc_found && g_select_total >= 0 ? g_select_total : r->num_rows;
        if (r && r->success) {
            bool dml = stmt->type == NODE_INSERT || stmt->type == NODE_UPDATE || stmt->type == NODE_DELETE;
            g_row_count = dml ? r->rows_affected : stmt->type == NODE_SELECT ? -1 : 0;
        }
    }
    return r;
}

static QueryResult* executor_run_impl(ExecCtx* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return result_error_msg("Invalid executor context");
    if (stmt->if_exists && if_exists_noop(ctx, stmt)) return result_ok_msg(0);

    switch (stmt->type) {
        case NODE_CREATE_DB:    return exec_create_db(ctx, stmt);
        case NODE_DROP_DB:      return exec_drop_db(ctx, stmt);
        case NODE_CREATE_TABLE: return exec_create_table(ctx, stmt);
        case NODE_DROP_TABLE: {
            // in una routine si eliminano solo le tabelle TEMPORARY che le routine hanno creato
            Database* cdb = catalog_current_db(ctx->catalog);
            if (ctx->routine && !(cdb && rt_is_temp(cdb->name, stmt->drop_table.table_name))) {
                if (stmt->if_exists && cdb && !catalog_get_table(ctx->catalog, cdb->name, stmt->drop_table.table_name)) return result_ok_msg(0);
                return result_error_msg("Only TEMPORARY tables created by the routine can be dropped inside a routine");
            }
            if (stmt->drop_table.nmore == 0) return exec_drop_table(ctx, stmt);
            // DROP TABLE a, b, c: una dopo l'altra (con IF EXISTS quelle che mancano si saltano)
            QueryResult* r = exec_drop_table(ctx, stmt);
            for (int k = 0; k < stmt->drop_table.nmore && r && r->success; k++) {
                db_result_free(r);
                ASTNode* dn = sv_calloc(1, sizeof(ASTNode));
                if (!dn) return result_error_msg("Out of memory");
                dn->type = NODE_DROP_TABLE;
                dn->if_exists = stmt->if_exists;
                snprintf(dn->drop_table.table_name, sizeof(dn->drop_table.table_name), "%s", stmt->drop_table.more[k]);
                if (stmt->if_exists && cdb && !catalog_get_table(ctx->catalog, cdb->name, dn->drop_table.table_name)) r = result_ok_msg(0);
                else r = exec_drop_table(ctx, dn);
                free(dn);
            }
            return r;
        }
        case NODE_ALTER_TABLE: {
            // ALTER TABLE t op1, op2, ...: in ordine, fino al primo errore
            QueryResult* r = stmt->alter_table.noop ? result_ok_msg(0) : exec_alter_table(ctx, stmt);
            long long affected = r && r->success ? r->rows_affected : 0;
            for (ASTNode* c = stmt->chain; c && r && r->success; c = c->chain) {
                db_result_free(r);
                r = c->type == NODE_ALTER_TABLE && c->alter_table.noop ? result_ok_msg(0)
                  : c->type == NODE_ALTER_TABLE ? exec_alter_table(ctx, c) : executor_run_impl(ctx, c);
                if (r && r->success) affected += r->rows_affected;
            }
            if (r && r->success && stmt->chain) r->rows_affected = affected;
            return r;
        }
        case NODE_INSERT: {
            if (stmt->insert_stmt.mode != INS_NORMAL || stmt->insert_stmt.num_dup > 0) return exec_insert_mysql(ctx, stmt);
            QueryResult* r = exec_insert(ctx, stmt);
            dup_clear();           // il conflitto registrato serve solo a exec_insert_mysql: niente resti nel thread
            return r;
        }
        case NODE_SELECT:       return stmt->select_stmt.num_into ? exec_select_into(ctx, stmt) : run_select(ctx, stmt, 0, false);
        case NODE_UPDATE:       return stmt->update_stmt.multi_from ? exec_dml_multi(ctx, stmt) : exec_update(ctx, stmt);
        case NODE_DELETE:       return stmt->delete_stmt.multi_from ? exec_dml_multi(ctx, stmt) : exec_delete(ctx, stmt);
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
        case NODE_CREATE_PROCEDURE: return exec_create_procedure(ctx, stmt);
        case NODE_DROP_PROCEDURE: return exec_drop_procedure(ctx, stmt);
        case NODE_CALL: return exec_call(ctx, stmt);
        case NODE_MERGE: return exec_merge(ctx, stmt);
        case NODE_SEQUENCE: return exec_sequence(ctx, stmt);
        case NODE_CHECKPOINT:   return result_ok_msg(0);
        case NODE_VACUUM:       return result_ok_msg(0);
        case NODE_DROP_TRIGGER: return exec_drop_trigger(ctx, stmt);
        default:
            return result_error_msg("Unsupported statement type");
    }
}
