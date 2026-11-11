/* 
SCOPO: Implementazione del bridge GUI <-> DB engine.
       Mantiene lo stato globale del DB (catalog, buffer pool,
       WAL, transaction manager) e li espone alla GUI
       tramite l'API definita in db_api.h.
*/
#include "db_api.h"
#include "auth.h"
#include "../catalog/schema.h"
#include "../storage/buffer_pool.h"
#include "../query/lexer.h"
#include "../query/parser.h"
#include "../query/executor.h"
#include "../tx/transaction.h"
#include "../tx/wal.h"
#include "platform_compat.h"
#include <stdio.h>
#include <string.h>

// ── mutex del motore e lock della cartella dati ─────────────────
// Il motore non è rientrante: ogni funzione pubblica che tocca catalogo, pagine o
// transazioni passa da un mutex ricorsivo (la GUI chiama il motore da due thread).
// Il file del database viene bloccato in modo esclusivo: due processi sulla stessa
// cartella dati la corromperebbero.
#ifdef _WIN32
  // windows.h non si può includere qui (TokenType del lexer va in conflitto con quello di
  // Windows): bastano le tre funzioni della CRITICAL_SECTION, su un buffer allineato più
  // grande della struttura reale (40 byte a 64 bit)
  #include <io.h>
  #include <sys/locking.h>
  static int  db_lock_file(int fd) {
      long cur = _lseek(fd, 0, SEEK_CUR);
      _lseek(fd, 0, SEEK_SET);
      int r = _locking(fd, _LK_NBLCK, 1);   // primo byte del file
      _lseek(fd, cur, SEEK_SET);
      return r;
  }
#else
  #include <pthread.h>
  #include <sys/file.h>
  static int  db_lock_file(int fd) { return flock(fd, LOCK_EX | LOCK_NB); }
#endif

// Lock del motore: le SELECT di db_execute_readonly lo prendono in lettura (in parallelo tra loro),
// tutto il resto in scrittura (esclusivo). L'esclusivo è rientrante con un contatore per thread.
#include "../platform_lock.h"
static SvRwLock g_db_rw;
static _Thread_local int g_lock_depth;
static void db_mutex_init(void) { sv_rw_init(&g_db_rw); }
static void db_lock(void)       { if (g_lock_depth++ == 0) sv_rw_wr(&g_db_rw); }
static void db_unlock(void)     { if (--g_lock_depth == 0) sv_rw_wrunlock(&g_db_rw); }

static char g_init_error[256];
const char* db_init_error(void) { return g_init_error; }

// ── Forward declarations ─────────────────────────────────────────
static QueryResult* result_error(const char* msg);
static QueryResult* result_ok(int rows_affected);

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

static double get_time_ms(void) { return sv_get_time_ms(); }

// ── API principale ───────────────────────────────────────────────

static void wal_before_page_write(void* w) { wal_write_buffer((WAL*)w); }

static bool db_init_impl(const char* data_directory) {
    if (g_db.initialized) return true;

    strncpy(g_db.data_dir, data_directory, sizeof(g_db.data_dir)-1);
    snprintf(g_db.db_path,  sizeof(g_db.db_path),  "%s" SV_PATH_SEP "segvault.db",  data_directory);
    snprintf(g_db.cat_path, sizeof(g_db.cat_path), "%s" SV_PATH_SEP "segvault.cat", data_directory);
    snprintf(g_db.log_path, sizeof(g_db.log_path), "%s" SV_PATH_SEP "segvault.wal", data_directory);

    // Apri il file del database e bloccalo: un'altra istanza sulla stessa cartella lo corromperebbe
    g_db.db_fd = sv_open(g_db.db_path, SV_O_RDWR | SV_O_CREAT, 0644);
    if (g_db.db_fd < 0) {
        snprintf(g_init_error, sizeof(g_init_error), "cannot open %.200s", g_db.db_path);
        return false;
    }
    if (db_lock_file(g_db.db_fd) != 0) {
        snprintf(g_init_error, sizeof(g_init_error),
                 "the data directory is already in use by another SegVault instance");
        sv_close(g_db.db_fd);
        return false;
    }

    // recovery dopo un arresto anomalo (con il lock già preso)
    wal_recovery(g_db.log_path, g_db.db_path, g_db.cat_path);

    // Inizializza i componenti
    g_db.catalog = catalog_load(g_db.cat_path);
    g_db.bp      = bp_create(SV_BUFFER_POOL_CAP, g_db.db_fd);
    g_db.txm     = txm_create();
    g_db.wal     = wal_create(g_db.log_path);

    if (g_db.bp && g_db.wal) {                     // prima di scrivere una pagina, il log
        g_db.bp->before_write = wal_before_page_write;
        g_db.bp->before_write_ctx = g_db.wal;
    }

    g_db.exec_ctx.catalog = g_db.catalog;
    g_db.exec_ctx.bp      = g_db.bp;
    g_db.exec_ctx.wal     = g_db.wal;
    g_db.exec_ctx.tx_id   = 0;

    g_db.initialized = true;
    auth_init(data_directory);
    return true;
}

// ── annullamento ─────────────────────────────────────────────────

static atomic_int g_cancel_requested = 0;
static atomic_int g_read_running = 0;   // una SELECT è in esecuzione: si può interrompere

void db_request_cancel(void) {
    atomic_store(&g_cancel_requested, 1);
    if (atomic_load(&g_read_running)) atomic_store(&g_sv_scan_cancel, 1);
}

void db_clear_cancel(void) {
    atomic_store(&g_cancel_requested, 0);
    atomic_store(&g_sv_scan_cancel, 0);
}

bool db_cancel_requested(void) { return atomic_load(&g_cancel_requested) != 0; }

// ── checkpoint ───────────────────────────────────────────────────
// Senza transazioni aperte tutte le modifiche sono definitive: scritte le pagine e il
// catalogo su disco (fsync), il log non serve più per il recovery e viene svuotato.
// Automatico quando il log supera SV_WAL_CHECKPOINT_BYTES, oppure con CHECKPOINT.
#define SV_WAL_CHECKPOINT_BYTES (16L << 20)

static bool do_checkpoint(void) {
    if (!g_db.initialized || g_db.exec_ctx.tx_id) return false;
    bp_flush_all(g_db.bp);
    if (catalog_save(g_db.catalog, g_db.cat_path) != SV_OK) return false;
    return wal_truncate(g_db.wal) == SV_OK;
}

static void maybe_checkpoint(void) {
    if (g_db.initialized && !g_db.exec_ctx.tx_id && wal_size(g_db.wal) > SV_WAL_CHECKPOINT_BYTES) do_checkpoint();
}

static int g_num_savepoints;

// utente della sessione ("" = root, tutto permesso: la GUI). Lo imposta il server per ogni richiesta
static char g_session_user[64];

static const char* cur_db_name(void) {
    Database* d = catalog_current_db(g_db.catalog);
    return d ? d->name : "";
}

// Uno statement con trigger è atomico: se un trigger fallisce dopo che le righe sono state
// scritte, tutto lo statement viene annullato (transazione implicita, o savepoint se c'è
// già una transazione aperta). Senza trigger le modifiche sono già validate prima di scrivere.
static QueryResult* exec_atomic(ASTNode* stmt) {
    Database* db = catalog_current_db(g_db.catalog);
    bool dml = stmt->type == NODE_INSERT || stmt->type == NODE_UPDATE || stmt->type == NODE_DELETE;
    if (!dml || !db || db->num_triggers == 0) return executor_run(&g_db.exec_ctx, stmt);
    if (!g_db.exec_ctx.tx_id) {
        if (!db_begin()) return executor_run(&g_db.exec_ctx, stmt);
        QueryResult* r = executor_run(&g_db.exec_ctx, stmt);
        if (r && r->success) db_commit(); else db_rollback();
        return r;
    }
    int before = g_num_savepoints;
    bool sp = db_savepoint("__sv_statement");
    if (!sp) return result_error("Too many savepoints in this transaction (max 64): COMMIT or ROLLBACK first");
    QueryResult* r = executor_run(&g_db.exec_ctx, stmt);
    if (sp && (!r || !r->success)) db_rollback_to("__sv_statement");
    if (sp) g_num_savepoints = before;          // il savepoint interno non resta visibile
    return r;
}

// esegue uno statement già analizzato (transazioni e CHECKPOINT gestiti qui)
static QueryResult* exec_one(ASTNode* stmt) {
    QueryResult* result = NULL;
    if (g_session_user[0]) {         // permessi dell'utente della sessione
        char err[300];
        if (!auth_check(g_session_user, stmt, cur_db_name(), err, sizeof(err))) return result_error(err);
    }
    if (stmt->type == NODE_BEGIN) {
        result = db_begin() ? result_ok(0) : result_error("Failed to begin transaction");
    } else if (stmt->type == NODE_COMMIT) {
        result = db_commit() ? result_ok(0) : result_error("Failed to commit");
    } else if (stmt->type == NODE_ROLLBACK && stmt->rollback_to.name[0] == 0) {
        result = db_rollback() ? result_ok(0) : result_error("Failed to rollback");
    } else if (stmt->type == NODE_CHECKPOINT) {
        result = g_db.exec_ctx.tx_id ? result_error("CHECKPOINT is not allowed inside a transaction")
               : do_checkpoint() ? result_ok(0) : result_error("Checkpoint failed");
    } else if (stmt->type == NODE_SAVEPOINT) {
        result = db_savepoint(stmt->savepoint.name) ? result_ok(0) : result_error("Failed to set savepoint");
    } else if (stmt->type == NODE_ROLLBACK_TO) {
        result = db_rollback_to(stmt->rollback_to.name) ? result_ok(0) : result_error("Failed to rollback to savepoint");
    } else if (stmt->type == NODE_SELECT) {
        // sola lettura: interrompibile a metà
        atomic_store(&g_read_running, 1);
        if (atomic_load(&g_cancel_requested)) atomic_store(&g_sv_scan_cancel, 1);
        result = atomic_load(&g_sv_scan_cancel) ? NULL : executor_run(&g_db.exec_ctx, stmt);
        atomic_store(&g_read_running, 0);
        if (atomic_load(&g_sv_scan_cancel)) {   // risultato parziale: scartato
            if (result) db_result_free(result);
            result = result_error("Query cancelled");
            atomic_store(&g_sv_scan_cancel, 0);
        }
    } else {
        result = exec_atomic(stmt);
    }

    // le modifiche allo schema vanno subito su disco: dopo un arresto anomalo le tabelle
    // create non devono sparire (il catalogo è piccolo, salvarlo costa poco)
    if (result && result->success) {
        switch (stmt->type) {
            case NODE_CREATE_DB: case NODE_CREATE_TABLE: case NODE_CREATE_INDEX: case NODE_CREATE_VIEW:
            case NODE_DROP_DB: case NODE_DROP_TABLE: case NODE_DROP_VIEW: case NODE_DROP_INDEX:
            case NODE_ALTER_TABLE: case NODE_TRUNCATE: case NODE_CREATE_TRIGGER: case NODE_DROP_TRIGGER:
                bp_flush_all(g_db.bp);
                catalog_save(g_db.catalog, g_db.cat_path);
                break;
            default: break;
        }
    }
    return result;
}

static QueryResult* db_execute_impl(const char* sql) {
    if (!g_db.initialized) return result_error("DB non inizializzato");
    if (!sql || !sql[0])   return result_error("Query vuota");

    double t_start = get_time_ms();

    // Pipeline: tokenize → parse → execute
    TokenList*   tokens = lexer_tokenize(sql);
    void* admin = NULL;               // CREATE USER, GRANT, SHOW GRANTS, ...: gestiti da auth.c
    if (tokens && auth_statement(tokens, g_session_user, cur_db_name(), &admin)) {
        lexer_free(tokens);
        QueryResult* r = admin ? (QueryResult*)admin : result_error("Out of memory");
        r->exec_time_ms = get_time_ms() - t_start;
        return r;
    }
    ParseResult* parsed = parser_parse(tokens);

    if (!parsed->success) {
        QueryResult* r = result_error(parsed->error);
        parse_result_free(parsed);
        lexer_free(tokens);
        return r;
    }

    // Esegui il primo statement (multi-statement gestito da db_execute_multi)
    QueryResult* result = parsed->count > 0 ? exec_one(parsed->statements[0]) : result_ok(0);
    if (result) result->exec_time_ms = get_time_ms() - t_start;

    parse_result_free(parsed);
    lexer_free(tokens);
    wal_write_buffer(g_db.wal);       // a fine statement il log è nel file (sopravvive a un crash del processo)
    maybe_checkpoint();
    return result;
}

static QueryResult** db_execute_multi_impl(const char* sql, int* out_count) {
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
        results[i] = exec_one(parsed->statements[i]);
        if (results[i])
            results[i]->exec_time_ms = get_time_ms() - t_start;
    }
    *out_count = parsed->count;
    wal_write_buffer(g_db.wal);
    maybe_checkpoint();

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
    for (int i = 0; r->col_tables && i < r->num_cols; i++) { free(r->col_tables[i]); free(r->col_columns[i]); }
    free(r->col_tables);
    free(r->col_columns);
    free(r->sql);
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

static bool db_begin_impl(void) {
    if (!g_db.initialized) return false;
    if (g_db.exec_ctx.tx_id) return false;          // transazione già aperta
    Transaction* tx = txm_begin(g_db.txm);
    if (!tx) return false;
    g_db.exec_ctx.tx_id = tx->tx_id;
    g_num_savepoints = 0;
    wal_write(g_db.wal, LOG_BEGIN, tx->tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    return true;
}

static bool db_commit_impl(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    wal_write(g_db.wal, LOG_COMMIT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    wal_flush(g_db.wal); // DEVE essere su disco prima del commit
    txm_commit(g_db.txm, g_db.exec_ctx.tx_id);
    g_db.exec_ctx.tx_id = 0;
    g_num_savepoints = 0;
    return true;
}

static bool db_rollback_impl(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    // Scrivi LOG_ABORT prima dell'undo per segnare la transazione
    wal_write(g_db.wal, LOG_ABORT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    // Flush WAL + buffer pool prima di leggere dal disco
    wal_flush(g_db.wal);
    bp_flush_all(g_db.bp);
    // Applica le before-image dal WAL per annullare
    wal_undo_tx(g_db.log_path, g_db.db_path, g_db.exec_ctx.tx_id);
    // Invalida tutte le pagine in cache per forzare la rilettura dal disco
    bp_remove_all(g_db.bp);
    txm_abort(g_db.txm, g_db.exec_ctx.tx_id);
    g_db.exec_ctx.tx_id = 0;
    g_num_savepoints = 0;
    executor_invalidate_indexes();   // le righe sono tornate indietro (e possono aver cambiato posizione)
    return true;
}

// Simple savepoint tracking: store name and file offset
static struct { char name[64]; off_t offset; } g_savepoints[64];
static int g_num_savepoints = 0;   // (dichiarato prima di exec_atomic)

static bool db_savepoint_impl(const char* name) {
    if (!g_db.initialized) return false;
    if (!g_db.exec_ctx.tx_id) return false;
    // stesso nome: sostituisce il precedente (come MySQL), così un SAVEPOINT ripetuto non riempie l'elenco
    for (int i = 0; i < g_num_savepoints; i++) {
        if (strcmp(g_savepoints[i].name, name) == 0) {
            memmove(&g_savepoints[i], &g_savepoints[i + 1], sizeof(g_savepoints[0]) * (size_t)(g_num_savepoints - i - 1));
            g_num_savepoints--;
            break;
        }
    }
    // elenco pieno: errore, mai un savepoint "riuscito" ma non registrato (exec_atomic ci conta
    // per annullare uno statement con trigger fallito)
    if (g_num_savepoints >= 64) return false;
    // Write LOG_SAVEPOINT to WAL
    wal_write(g_db.wal, LOG_SAVEPOINT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, (const uint8_t*)name, strlen(name) + 1, NULL, 0);
    wal_flush(g_db.wal);

    // Record offset
    snprintf(g_savepoints[g_num_savepoints].name, sizeof(g_savepoints[g_num_savepoints].name), "%s", name);
    g_savepoints[g_num_savepoints].offset = sv_lseek(g_db.wal->fd, 0, SEEK_CUR);
    g_num_savepoints++;
    return true;
}

static bool db_rollback_to_impl(const char* name) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    int sp_idx = -1;
    for (int i = g_num_savepoints - 1; i >= 0; i--) {
        if (strcmp(g_savepoints[i].name, name) == 0) { sp_idx = i; break; }
    }
    if (sp_idx < 0) return false;

    wal_flush(g_db.wal);
    bp_flush_all(g_db.bp);

    // annulla le voci della transazione scritte dopo il savepoint (stesso undo del ROLLBACK)
    off_t sp_off = g_savepoints[sp_idx].offset;
    wal_undo_tx_from(g_db.log_path, g_db.db_path, g_db.exec_ctx.tx_id, (long)sp_off);

    if (sv_ftruncate(g_db.wal->fd, sp_off) != 0)
        fprintf(stderr, "[SegVault] cannot shorten the transaction log after ROLLBACK TO\n");
    sv_lseek(g_db.wal->fd, 0, SEEK_END);
    g_num_savepoints = sp_idx + 1;      // il savepoint resta (come in SQL): ci si può tornare ancora

    bp_remove_all(g_db.bp);
    executor_invalidate_indexes();
    return true;
}

// ── Sidebar API ──────────────────────────────────────────────────

static NameList* db_list_databases_impl(void) {
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

static NameList* db_list_tables_impl(const char* db_name) {
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

static NameList* db_list_views_impl(const char* db_name) {
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

static NameList* db_list_procedures_impl(const char* db_name) {
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

static NameList* db_list_functions_impl(const char* db_name) {
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

static NameList* db_list_triggers_impl(const char* db_name, const char* table) {
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

static ColumnInfo* db_list_columns_impl(const char* db_name, const char* table,
                              int* out_count) {
    *out_count = 0;
    TableSchema* t = catalog_get_table(g_db.catalog, db_name, table);
    if (!t) return NULL;
    ColumnInfo* cols = SV_ALLOC_N(ColumnInfo, t->num_columns);
    for (int i = 0; i < t->num_columns; i++) {
        strncpy(cols[i].name, t->columns[i].name, 63);
        if ((t->columns[i].type == SV_TYPE_VARCHAR || t->columns[i].type == SV_TYPE_CHAR) && t->columns[i].size > 0)
            snprintf(cols[i].type, sizeof(cols[i].type), "%s(%d)", datatype_name(t->columns[i].type), t->columns[i].size);
        else
            snprintf(cols[i].type, sizeof(cols[i].type), "%s", datatype_name(t->columns[i].type));
        cols[i].nullable       = t->columns[i].nullable;
        cols[i].primary_key    = t->columns[i].primary_key;
        cols[i].auto_increment = t->columns[i].auto_increment;
        snprintf(cols[i].default_val, sizeof(cols[i].default_val), "%.127s", t->columns[i].default_val);
        cols[i].unique = t->columns[i].unique;
        for (int k = 0; k < t->num_constraints; k++) {
            const Constraint* c = &t->constraints[k];
            if (c->kind == CONS_UNIQUE && c->ncols == 1 && c->cols[0] == i) {
                cols[i].unique = true;
                snprintf(cols[i].unique_name, sizeof(cols[i].unique_name), "%.63s", c->name);
            }
        }
        for (int k = 0; k < t->num_indexes; k++)
            if (!strcasecmp(t->indexes[k].col_name, t->columns[i].name)) {
                cols[i].indexed = true;
                snprintf(cols[i].index_name, sizeof(cols[i].index_name), "%.63s", t->indexes[k].name);
            }
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

static bool db_use_impl(const char* name) {
    if (!g_db.initialized) return false;
    return catalog_use_db(g_db.catalog, name) == SV_OK;
}

static const char* db_current_database_impl(void) {
    if (!g_db.initialized) return NULL;
    Database* db = catalog_current_db(g_db.catalog);
    return db ? db->name : NULL;
}

static SessionInfo* db_session_info_impl(void) {
    SessionInfo* s = SV_ALLOC(SessionInfo);
    const char* db = db_current_database();
    if (s && db) {
        size_t n = strlen(db);
        if (n >= sizeof(s->current_db)) n = sizeof(s->current_db) - 1;
        memcpy(s->current_db, db, n);
        s->current_db[n] = '\0';
    }
    s->tx_id          = g_db.exec_ctx.tx_id;
    s->in_transaction = (s->tx_id != 0);
    s->auto_commit    = g_db.txm ? g_db.txm->autocommit : true;
    strncpy(s->server_version, SV_VERSION_STR, 31);
    return s;
}

void db_session_free(SessionInfo* s) { free(s); }

// CREATE TABLE che ricrea la struttura della tabella (dump, "Copy as SQL")
static char* db_get_create_statement_impl(const char* db_name, const char* table) {
    return executor_create_statement(g_db.catalog, db_name, table, true);
}

static char* db_get_create_statement_nofk_impl(const char* db_name, const char* table) {
    return executor_create_statement(g_db.catalog, db_name, table, false);
}

static char* db_get_foreign_keys_impl(const char* db_name, const char* table) {
    return executor_fk_statements(g_db.catalog, db_name, table);
}

void db_free_string(char* s) { free(s); }

static void db_shutdown_impl(void) {
    if (!g_db.initialized) return;
    auth_shutdown();
    g_session_user[0] = '\0';
    // una transazione lasciata aperta viene annullata (le sue pagine sono già nel buffer)
    if (g_db.exec_ctx.tx_id) db_rollback();
    bp_flush_all(g_db.bp);
    catalog_save(g_db.catalog, g_db.cat_path);
    sv_fsync(g_db.db_fd);
    wal_flush(g_db.wal);
    g_db.bp->before_write = NULL;          // il log sta per essere chiuso
    wal_destroy(g_db.wal);
    // checkpoint: tutte le modifiche sono su disco, il log non serve più.
    // Al prossimo avvio il recovery lavora solo dopo un arresto anomalo.
    remove(g_db.log_path);
    bp_destroy(g_db.bp);
    txm_destroy(g_db.txm);
    catalog_free(g_db.catalog);
    executor_invalidate_indexes();
    sv_close(g_db.db_fd);   // rilascia anche il lock del file
    g_db.initialized = false;
}

// ── Helper per l'executor ────────────────────────────────────────

static QueryResult* result_error(const char* msg) {
    QueryResult* r = make_result();
    r->success = false;
    strncpy(r->error, msg, sizeof(r->error) - 1);
    return r;
}

static QueryResult* result_ok(int rows_affected) {
    QueryResult* r = make_result();
    r->success      = true;
    r->rows_affected = rows_affected;
    return r;
}

// ── API pubblica: ogni chiamata tiene il mutex del motore ───────

QueryResult* db_execute(const char* sql) {
    db_lock();
    QueryResult* r = db_execute_impl(sql);
    db_unlock();
    return r;
}

QueryResult** db_execute_multi(const char* sql, int* out_count) {
    db_lock();
    QueryResult** r = db_execute_multi_impl(sql, out_count);
    db_unlock();
    return r;
}

bool db_begin(void) {
    db_lock();
    bool r = db_begin_impl();
    db_unlock();
    return r;
}

bool db_commit(void) {
    db_lock();
    bool r = db_commit_impl();
    db_unlock();
    return r;
}

bool db_rollback(void) {
    db_lock();
    bool r = db_rollback_impl();
    db_unlock();
    return r;
}

bool db_savepoint(const char* name) {
    db_lock();
    bool r = db_savepoint_impl(name);
    db_unlock();
    return r;
}

bool db_rollback_to(const char* name) {
    db_lock();
    bool r = db_rollback_to_impl(name);
    db_unlock();
    return r;
}

NameList* db_list_databases(void) {
    db_lock();
    NameList* r = db_list_databases_impl();
    db_unlock();
    return r;
}

NameList* db_list_tables(const char* db_name) {
    db_lock();
    NameList* r = db_list_tables_impl(db_name);
    db_unlock();
    return r;
}

NameList* db_list_views(const char* db_name) {
    db_lock();
    NameList* r = db_list_views_impl(db_name);
    db_unlock();
    return r;
}

NameList* db_list_procedures(const char* db_name) {
    db_lock();
    NameList* r = db_list_procedures_impl(db_name);
    db_unlock();
    return r;
}

NameList* db_list_functions(const char* db_name) {
    db_lock();
    NameList* r = db_list_functions_impl(db_name);
    db_unlock();
    return r;
}

NameList* db_list_triggers(const char* db_name, const char* table) {
    db_lock();
    NameList* r = db_list_triggers_impl(db_name, table);
    db_unlock();
    return r;
}

ColumnInfo* db_list_columns(const char* db_name, const char* table, int* out_count) {
    db_lock();
    ColumnInfo* r = db_list_columns_impl(db_name, table, out_count);
    db_unlock();
    return r;
}

static const char* fk_action_name(int a) {
    switch (a) {
        case FK_RESTRICT: return "RESTRICT";
        case FK_CASCADE: return "CASCADE";
        case FK_SET_NULL: return "SET NULL";
        case FK_SET_DEFAULT: return "SET DEFAULT";
        default: return "NO ACTION";
    }
}

ForeignKeyInfo* db_list_foreign_keys(const char* db_name, int* out_count) {
    *out_count = 0;
    db_lock();
    Database* db = db_name ? catalog_get_db(g_db.catalog, db_name) : NULL;
    int n = 0;
    for (int t = 0; db && t < db->num_tables; t++)
        for (int k = 0; k < db->tables[t].num_constraints; k++) n += db->tables[t].constraints[k].kind == CONS_FOREIGN;
    ForeignKeyInfo* out = n ? calloc((size_t)n, sizeof(ForeignKeyInfo)) : NULL;
    for (int t = 0; out && t < db->num_tables; t++) {
        const TableSchema* ts = &db->tables[t];
        for (int k = 0; k < ts->num_constraints; k++) {
            const Constraint* c = &ts->constraints[k];
            if (c->kind != CONS_FOREIGN) continue;
            ForeignKeyInfo* f = &out[(*out_count)++];
            snprintf(f->name, sizeof(f->name), "%.63s", c->name);
            snprintf(f->table, sizeof(f->table), "%.63s", ts->name);
            snprintf(f->ref_table, sizeof(f->ref_table), "%.63s", c->ref_table);
            f->ncols = c->ncols > 8 ? 8 : c->ncols;
            for (int i = 0; i < f->ncols; i++) {
                if (c->cols[i] < ts->num_columns) snprintf(f->cols[i], 64, "%.63s", ts->columns[c->cols[i]].name);
                snprintf(f->ref_cols[i], 64, "%.63s", c->ref_cols[i]);
            }
            snprintf(f->on_delete, sizeof(f->on_delete), "%s", fk_action_name(c->on_delete));
            snprintf(f->on_update, sizeof(f->on_update), "%s", fk_action_name(c->on_update));
        }
    }
    db_unlock();
    return out;
}

void db_foreign_keys_free(ForeignKeyInfo* fks) { free(fks); }

bool db_table_info(const char* db_name, const char* table, TableInfo* out, int* num_indexes) {
    memset(out, 0, sizeof(*out));
    db_lock();
    TableSchema* t = catalog_get_table(g_db.catalog, db_name, table);
    if (t) {
        snprintf(out->name, sizeof(out->name), "%.63s", t->name);
        snprintf(out->engine, sizeof(out->engine), "SegVault");
        snprintf(out->comment, sizeof(out->comment), "%.255s", t->comment);
        out->num_rows = t->num_rows;
        out->size_bytes = (uint64_t)t->num_pages * SV_PAGE_SIZE;
        if (num_indexes) *num_indexes = t->num_indexes;
    }
    db_unlock();
    return t != NULL;
}

// ── sessioni ────────────────────────────────────────────────────

typedef struct { char name[64]; off_t offset; } SavepointRec;

struct DbSession {
    int          cur_db;            // indice nel catalogo (-1 = nessuno)
    uint64_t     tx_id;
    SavepointRec sp[64];
    int          nsp;
    char         user[64];
};

static DbSession  g_default_session;            // stato salvato della sessione predefinita
static DbSession* g_active;                     // sessione attiva (NULL = predefinita)
static DbSession* g_tx_owner;                   // sessione con una transazione aperta (sospesa)

static void session_save(DbSession* s) {
    s->cur_db = g_db.catalog ? g_db.catalog->current_db : -1;
    s->tx_id = g_db.exec_ctx.tx_id;
    s->nsp = g_num_savepoints;
    memcpy(s->sp, g_savepoints, sizeof(s->sp));
    snprintf(s->user, sizeof(s->user), "%s", g_session_user);
}

static void session_load(const DbSession* s) {
    if (g_db.catalog) g_db.catalog->current_db = s->cur_db < g_db.catalog->num_databases ? s->cur_db : -1;
    g_db.exec_ctx.tx_id = s->tx_id;
    g_num_savepoints = s->nsp;
    memcpy(g_savepoints, s->sp, sizeof(g_savepoints));
    snprintf(g_session_user, sizeof(g_session_user), "%s", s->user);
}

DbSession* db_session_new(void) {
    DbSession* s = calloc(1, sizeof(DbSession));
    if (s) s->cur_db = -1;
    return s;
}

bool db_session_enter(DbSession* s) {
    db_lock();
    // una transazione aperta (di una sessione sospesa, o della sessione predefinita) blocca le altre
    bool busy = (g_tx_owner && g_tx_owner != s) || (s && g_db.exec_ctx.tx_id && !g_active);
    if (!busy && s && !g_active) {
        session_save(&g_default_session);
        session_load(s);
        g_active = s;
    }
    db_unlock();
    return !busy;
}

void db_session_leave(DbSession* s) {
    if (!s) return;
    db_lock();
    if (g_active == s) {
        session_save(s);
        if (s->tx_id) g_tx_owner = s;
        else if (g_tx_owner == s) g_tx_owner = NULL;
        session_load(&g_default_session);
        g_active = NULL;
    }
    db_unlock();
}

void db_session_end(DbSession* s) {
    if (!s) return;
    if (s->tx_id && db_session_enter(s)) {
        db_rollback();
        db_session_leave(s);
    }
    db_lock();
    if (g_tx_owner == s) g_tx_owner = NULL;
    db_unlock();
    free(s);
}

bool db_session_in_transaction(const DbSession* s) { return s && s->tx_id != 0; }

bool db_session_blocked(void) {
    db_lock();
    bool b = g_tx_owner != NULL;
    db_unlock();
    return b;
}

// ── letture in parallelo ────────────────────────────────────────

QueryResult* db_execute_readonly(const char* sql, const char* dbname, const char* user, int timeout_ms, bool* refused) {
    *refused = false;
    if (!g_db.initialized || !sql) { *refused = true; return NULL; }
    double t0 = get_time_ms();
    sv_rw_rd(&g_db_rw);
    // una transazione aperta (di una sessione o della GUI): niente letture accanto ai suoi dati non
    // confermati; e un thread che ha già il lock esclusivo non può prenderlo anche in lettura
    if (g_tx_owner || g_db.exec_ctx.tx_id || g_active || g_lock_depth) { sv_rw_rdunlock(&g_db_rw); *refused = true; return NULL; }
    TokenList* tokens = lexer_tokenize(sql);
    ParseResult* parsed = tokens ? parser_parse(tokens) : NULL;
    QueryResult* r = NULL;
    int idx = g_db.catalog->current_db;
    if (dbname && dbname[0]) {
        idx = -1;
        for (int i = 0; i < g_db.catalog->num_databases; i++) if (!strcasecmp(g_db.catalog->databases[i].name, dbname)) idx = i;
    }
    // solo una SELECT, in un database esistente, con un utente che esiste; il resto va per la via normale
    bool ok = parsed && parsed->success && parsed->count == 1 && parsed->statements[0]->type == NODE_SELECT &&
              (!dbname || !dbname[0] || idx >= 0) && (!user || !user[0] || auth_user_exists(user));
    if (ok) {
        char err[300];
        const char* dn = idx >= 0 ? g_db.catalog->databases[idx].name : "";
        if (user && user[0] && !auth_check(user, parsed->statements[0], dn, err, sizeof(err))) r = result_error(err);
        else {
            catalog_set_thread_db(idx);
            sv_scan_set_deadline(timeout_ms > 0 ? t0 + timeout_ms : 0);
            ExecCtx ctx = g_db.exec_ctx;              // copia: nessuna transazione, niente stato condiviso
            ctx.tx_id = 0;
            r = executor_run(&ctx, parsed->statements[0]);
            bool late = timeout_ms > 0 && get_time_ms() > t0 + timeout_ms && (!r || !r->success);
            sv_scan_set_deadline(0);
            catalog_set_thread_db(-2);
            if (late || !r) {
                if (r) db_result_free(r);
                r = result_error(late ? "Query cancelled: time limit reached" : "Out of memory");
            }
        }
        if (r) r->exec_time_ms = get_time_ms() - t0;
    } else *refused = true;
    if (parsed) parse_result_free(parsed);
    if (tokens) lexer_free(tokens);
    sv_rw_rdunlock(&g_db_rw);
    return r;
}

bool db_engine_stats(DbEngineStats* out) {
    memset(out, 0, sizeof(*out));
    db_lock();
    bool ok = g_db.initialized && g_db.bp;
    if (ok) {
        const BufferStats* st = &g_db.bp->stats;
        out->page_size = SV_PAGE_SIZE;
        out->db_pages = (long long)(sv_lseek(g_db.bp->db_fd, 0, SEEK_END) / SV_PAGE_SIZE);
        out->pool_frames = g_db.bp->capacity;
        out->pool_used = g_db.bp->num_used;
        out->pool_requests = st->requests;
        out->pool_hits = st->hits;
        out->pool_misses = st->misses;
        out->pool_evictions = st->evictions;
        out->pages_written = st->dirty_writes;
        out->wal_bytes = g_db.wal ? (long long)wal_size(g_db.wal) : 0;
        out->wal_records = g_db.wal ? (unsigned long long)g_db.wal->sequence : 0;
        out->checkpoints = g_db.wal ? (unsigned long long)g_db.wal->checkpoints : 0;
        out->databases = g_db.catalog ? g_db.catalog->num_databases : 0;
        for (int i = 0; g_db.catalog && i < g_db.catalog->num_databases; i++) out->tables += g_db.catalog->databases[i].num_tables;
        out->in_transaction = g_db.exec_ctx.tx_id != 0;
    }
    db_unlock();
    return ok;
}

bool db_set_user(const char* name) {
    db_lock();
    bool ok = !name || !name[0] || auth_user_exists(name);
    if (ok) snprintf(g_session_user, sizeof(g_session_user), "%.63s", name ? name : "");
    db_unlock();
    return ok;
}

const char* db_session_user(void) { return g_session_user; }

bool db_login(const char* name, const char* password) {
    db_lock();
    bool ok = name && name[0] && auth_login(name, password);
    db_unlock();
    return ok;
}

bool db_has_users(void) {
    db_lock();
    bool r = auth_user_count() > 0;
    db_unlock();
    return r;
}

int db_list_users(DbUserInfo* out, int max) {
    db_lock();
    int n = 0;
    for (int i = 0; i < auth_user_count() && n < max; i++) {
        const char* u = auth_user_name(i);
        snprintf(out[n].name, sizeof(out[n].name), "%s", u);
        out[n].admin = auth_is_admin(u);
        n++;
    }
    db_unlock();
    return n;
}

unsigned db_user_grant(const char* user, const char* db, const char* table) {
    db_lock();
    unsigned p = auth_grant_on(user, db, table);
    db_unlock();
    return p;
}

int db_user_table_grants(const char* user, char (*db)[64], char (*table)[64], unsigned* privs, int max) {
    db_lock();
    int n = 0;
    for (int i = 0; i < auth_grant_count(user) && n < max; i++) {
        unsigned p;
        if (auth_grant_at(user, i, db[n], table[n], &p)) { privs[n] = p; n++; }
    }
    db_unlock();
    return n;
}

bool db_use(const char* name) {
    db_lock();
    bool r = db_use_impl(name);
    db_unlock();
    return r;
}

const char* db_current_database(void) {
    db_lock();
    const char* r = db_current_database_impl();
    db_unlock();
    return r;
}

SessionInfo* db_session_info(void) {
    db_lock();
    SessionInfo* r = db_session_info_impl();
    db_unlock();
    return r;
}

char* db_get_create_statement(const char* db_name, const char* table) {
    db_lock();
    char* r = db_get_create_statement_impl(db_name, table);
    db_unlock();
    return r;
}

char* db_get_create_statement_nofk(const char* db_name, const char* table) {
    db_lock();
    char* r = db_get_create_statement_nofk_impl(db_name, table);
    db_unlock();
    return r;
}

char* db_get_foreign_keys(const char* db_name, const char* table) {
    db_lock();
    char* r = db_get_foreign_keys_impl(db_name, table);
    db_unlock();
    return r;
}

void db_shutdown(void) {
    db_lock();
    db_shutdown_impl();
    db_unlock();
}

bool db_init(const char* data_directory) {
    static int once = 0;
    if (!once) { db_mutex_init(); once = 1; }
    db_lock();
    bool r = db_init_impl(data_directory);
    db_unlock();
    return r;
}
