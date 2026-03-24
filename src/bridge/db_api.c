/* 
SCOPO: Implementazione del bridge GUI <-> DB engine.
       Mantiene lo stato globale del DB (catalog, buffer pool,
       WAL, transaction manager) e li espone alla GUI
       tramite l'API definita in db_api.h.
*/
#include "db_api.h"
#include "../catalog/schema.h"
#include "../storage/buffer_pool.h"
#include "../query/lexer.h"
#include "../query/parser.h"
#include "../query/executor.h"
#include "../tx/transaction.h"
#include "../tx/wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// ── Stato globale del DB engine ──────────────────────────────────

static struct {
    Catalog*    catalog;
    BufferPool* bp;
    TxManager*  txm;
    WAL*        wal;
    ExecCtx     exec_ctx;
    char          data_dir[512];
    char          db_path[512];
    char          cat_path[512];
    char          log_path[512];
    int           db_fd;
    bool          initialized;
} g_db = {0};

// ── Helpers ──────────────────────────────────────────────────────

static QueryResult* make_result(void) {
    return SV_ALLOC(QueryResult);
}

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ── API principale ───────────────────────────────────────────────

bool db_init(const char* data_directory) {
    if (g_db.initialized) return true;

    strncpy(g_db.data_dir, data_directory, sizeof(g_db.data_dir)-1);
    snprintf(g_db.db_path,  sizeof(g_db.db_path),  "%s/segvault.db",  data_directory);
    snprintf(g_db.cat_path, sizeof(g_db.cat_path), "%s/segvault.cat", data_directory);
    snprintf(g_db.log_path, sizeof(g_db.log_path), "%s/segvault.wal", data_directory);

    // Recovery prima di aprire qualsiasi cosa
    wal_recovery(g_db.log_path, g_db.db_path, g_db.cat_path);

    // Apri il file del database
    g_db.db_fd = open(g_db.db_path, O_RDWR | O_CREAT, 0644);
    if (g_db.db_fd < 0) return false;

    // Inizializza i componenti
    g_db.catalog = catalog_load(g_db.cat_path);
    g_db.bp      = bp_create(SV_BUFFER_POOL_CAP, g_db.db_fd);
    g_db.txm     = txm_create();
    g_db.wal     = wal_create(g_db.log_path);

    g_db.exec_ctx.catalog = g_db.catalog;
    g_db.exec_ctx.bp      = g_db.bp;
    g_db.exec_ctx.tx_id   = 0;

    g_db.initialized = true;
    return true;
}

QueryResult* db_execute(const char* sql) {
    if (!g_db.initialized) return result_error("DB non inizializzato");
    if (!sql || !sql[0])   return result_error("Query vuota");

    double t_start = get_time_ms();

    // Pipeline: tokenize → parse → execute
    TokenList*   tokens = lexer_tokenize(sql);
    ParseResult* parsed = parser_parse(tokens);

    if (!parsed->success) {
        QueryResult* r = result_error(parsed->error);
        parse_result_free(parsed);
        lexer_free(tokens);
        return r;
    }

    // Esegui il primo statement (multi-statement gestito da db_execute_multi)
    QueryResult* result = NULL;
    if (parsed->count > 0) {
        result = executor_run(&g_db.exec_ctx, parsed->statements[0]);
    } else {
        result = result_ok(0);
    }

    if (result) result->exec_time_ms = get_time_ms() - t_start;

    parse_result_free(parsed);
    lexer_free(tokens);
    return result;
}

QueryResult** db_execute_multi(const char* sql, int* out_count) {
    *out_count = 0;
    if (!g_db.initialized) return NULL;

    double t_start = get_time_ms();

    TokenList*   tokens = lexer_tokenize(sql);
    ParseResult* parsed = parser_parse(tokens);

    if (!parsed->success || parsed->count == 0) {
        parse_result_free(parsed);
        lexer_free(tokens);
        return NULL;
    }

    QueryResult** results = SV_ALLOC_N(QueryResult*, parsed->count);
    for (int i = 0; i < parsed->count; i++) {
        results[i] = executor_run(&g_db.exec_ctx, parsed->statements[i]);
        if (results[i])
            results[i]->exec_time_ms = get_time_ms() - t_start;
    }
    *out_count = parsed->count;

    parse_result_free(parsed);
    lexer_free(tokens);
    return results;
}

void db_result_free(QueryResult* r) {
    if (!r) return;
    for (int i = 0; i < r->num_cols; i++) {
        free(r->col_names[i]);
        if (r->col_types) free(r->col_types[i]);
    }
    free(r->col_names);
    free(r->col_types);
    for (int i = 0; i < r->num_rows; i++)
        free(r->rows[i].cells);
    free(r->rows);
    free(r);
}

void db_result_free_multi(QueryResult** results, int count) {
    for (int i = 0; i < count; i++) db_result_free(results[i]);
    free(results);
}

// ── Transazioni ──────────────────────────────────────────────────

bool db_begin(void) {
    if (!g_db.initialized) return false;
    Transaction* tx = txm_begin(g_db.txm);
    if (!tx) return false;
    g_db.exec_ctx.tx_id = tx->tx_id;
    wal_write(g_db.wal, LOG_BEGIN, tx->tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    return true;
}

bool db_commit(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    wal_write(g_db.wal, LOG_COMMIT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    wal_flush(g_db.wal); // DEVE essere su disco prima del commit
    txm_commit(g_db.txm, g_db.exec_ctx.tx_id);
    g_db.exec_ctx.tx_id = 0;
    return true;
}

bool db_rollback(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    // TODO: applicare le before-image dal WAL per annullare
    wal_write(g_db.wal, LOG_ABORT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    txm_abort(g_db.txm, g_db.exec_ctx.tx_id);
    g_db.exec_ctx.tx_id = 0;
    return true;
}

bool db_savepoint(const char* name) {
    (void)name; // TODO
    return g_db.initialized;
}

bool db_rollback_to(const char* name) {
    (void)name; // TODO
    return g_db.initialized;
}

// ── Sidebar API ──────────────────────────────────────────────────

NameList* db_list_databases(void) {
    if (!g_db.initialized || !g_db.catalog) return NULL;
    Catalog* c  = g_db.catalog;
    NameList*  nl = SV_ALLOC(NameList);
    nl->count     = c->num_databases;
    nl->names     = SV_ALLOC_N(char*, nl->count);
    nl->extras    = NULL;
    for (int i = 0; i < nl->count; i++) {
        nl->names[i] = strdup(c->databases[i].name);
    }
    return nl;
}

NameList* db_list_tables(const char* db_name) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_get_db(g_db.catalog, db_name);
    if (!db) return NULL;
    NameList* nl = SV_ALLOC(NameList);
    nl->count    = db->num_tables;
    nl->names    = SV_ALLOC_N(char*, nl->count);
    nl->extras   = NULL;
    for (int i = 0; i < nl->count; i++)
        nl->names[i] = strdup(db->tables[i].name);
    return nl;
}

NameList* db_list_views(const char* db_name) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_get_db(g_db.catalog, db_name);
    if (!db) return NULL;
    NameList* nl = SV_ALLOC(NameList);
    nl->count    = db->num_views;
    nl->names    = SV_ALLOC_N(char*, nl->count);
    nl->extras   = NULL;
    for (int i = 0; i < nl->count; i++)
        nl->names[i] = strdup(db->views[i].name);
    return nl;
}

NameList* db_list_procedures(const char* db_name) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_get_db(g_db.catalog, db_name);
    if (!db) return NULL;
    NameList* nl = SV_ALLOC(NameList);
    nl->names    = SV_ALLOC_N(char*, db->num_routines);
    nl->count    = 0;
    nl->extras   = NULL;
    for (int i = 0; i < db->num_routines; i++)
        if (!db->routines[i].is_function)
            nl->names[nl->count++] = strdup(db->routines[i].name);
    return nl;
}

NameList* db_list_functions(const char* db_name) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_get_db(g_db.catalog, db_name);
    if (!db) return NULL;
    NameList* nl = SV_ALLOC(NameList);
    nl->names    = SV_ALLOC_N(char*, db->num_routines);
    nl->count    = 0;
    nl->extras   = NULL;
    for (int i = 0; i < db->num_routines; i++)
        if (db->routines[i].is_function)
            nl->names[nl->count++] = strdup(db->routines[i].name);
    return nl;
}

NameList* db_list_triggers(const char* db_name, const char* table) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_get_db(g_db.catalog, db_name);
    if (!db) return NULL;
    NameList* nl = SV_ALLOC(NameList);
    nl->names    = SV_ALLOC_N(char*, db->num_triggers);
    nl->count    = 0;
    nl->extras   = NULL;
    for (int i = 0; i < db->num_triggers; i++) {
        if (!table || strcmp(db->triggers[i].table_name, table) == 0)
            nl->names[nl->count++] = strdup(db->triggers[i].name);
    }
    return nl;
}

ColumnInfo* db_list_columns(const char* db_name, const char* table,
                              int* out_count) {
    *out_count = 0;
    TableSchema* t = catalog_get_table(g_db.catalog, db_name, table);
    if (!t) return NULL;
    ColumnInfo* cols = SV_ALLOC_N(ColumnInfo, t->num_columns);
    for (int i = 0; i < t->num_columns; i++) {
        strncpy(cols[i].name, t->columns[i].name, 63);
        snprintf(cols[i].type, sizeof(cols[i].type), "%s",
                 datatype_name(t->columns[i].type));
        cols[i].nullable       = t->columns[i].nullable;
        cols[i].primary_key    = t->columns[i].primary_key;
        cols[i].auto_increment = t->columns[i].auto_increment;
    }
    *out_count = t->num_columns;
    return cols;
}

void db_columns_free(ColumnInfo* cols, int count) {
    (void)count; free(cols);
}

void db_namelist_free(NameList* nl) {
    if (!nl) return;
    for (int i = 0; i < nl->count; i++) free(nl->names[i]);
    free(nl->names);
    free(nl->extras);
    free(nl);
}

bool db_use(const char* name) {
    if (!g_db.initialized) return false;
    return catalog_use_db(g_db.catalog, name) == SV_OK;
}

const char* db_current_database(void) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_current_db(g_db.catalog);
    return db ? db->name : NULL;
}

SessionInfo* db_session_info(void) {
    SessionInfo* s = SV_ALLOC(SessionInfo);
    const char* db = db_current_database();
    if (db) strncpy(s->current_db, db, 63);
    s->tx_id          = g_db.exec_ctx.tx_id;
    s->in_transaction = (s->tx_id != 0);
    s->autocommit     = g_db.txm ? g_db.txm->autocommit : true;
    strncpy(s->server_version, SV_VERSION_STR, 31);
    return s;
}

void db_session_free(SessionInfo* s) { free(s); }

void db_free_string(char* s) { free(s); }

void db_shutdown(void) {
    if (!g_db.initialized) return;
    bp_flush_all(g_db.bp);
    catalog_save(g_db.catalog, g_db.cat_path);
    wal_flush(g_db.wal);
    wal_destroy(g_db.wal);
    bp_destroy(g_db.bp);
    txm_destroy(g_db.txm);
    catalog_free(g_db.catalog);
    close(g_db.db_fd);
    g_db.initialized = false;
}

// ── Helper per l'executor ────────────────────────────────────────

QueryResult* result_error(const char* msg) {
    QueryResult* r = make_result();
    r->success = false;
    strncpy(r->error, msg, sizeof(r->error) - 1);
    return r;
}

QueryResult* result_ok(int rows_affected) {
    QueryResult* r = make_result();
    r->success      = true;
    r->rows_affected = rows_affected;
    return r;
}