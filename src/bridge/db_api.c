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
  __declspec(dllimport) void __stdcall InitializeCriticalSection(void* cs);
  __declspec(dllimport) void __stdcall EnterCriticalSection(void* cs);
  __declspec(dllimport) void __stdcall LeaveCriticalSection(void* cs);
  static _Alignas(16) unsigned char g_db_mutex[64];
  static void db_mutex_init(void)   { InitializeCriticalSection(g_db_mutex); }
  static void db_lock(void)         { EnterCriticalSection(g_db_mutex); }
  static void db_unlock(void)       { LeaveCriticalSection(g_db_mutex); }
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
  static pthread_mutex_t g_db_mutex;
  static void db_mutex_init(void) {
      pthread_mutexattr_t a;
      pthread_mutexattr_init(&a);
      pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&g_db_mutex, &a);
      pthread_mutexattr_destroy(&a);
  }
  static void db_lock(void)        { pthread_mutex_lock(&g_db_mutex); }
  static void db_unlock(void)      { pthread_mutex_unlock(&g_db_mutex); }
  static int  db_lock_file(int fd) { return flock(fd, LOCK_EX | LOCK_NB); }
#endif

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

    g_db.exec_ctx.catalog = g_db.catalog;
    g_db.exec_ctx.bp      = g_db.bp;
    g_db.exec_ctx.wal     = g_db.wal;
    g_db.exec_ctx.tx_id   = 0;

    g_db.initialized = true;
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

static QueryResult* db_execute_impl(const char* sql) {
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
        ASTNode* stmt = parsed->statements[0];
        // Handle transaction statements directly at bridge level
        if (stmt->type == NODE_BEGIN) {
            result = db_begin() ? result_ok(0) : result_error("Failed to begin transaction");
        } else if (stmt->type == NODE_COMMIT) {
            result = db_commit() ? result_ok(0) : result_error("Failed to commit");
        } else if (stmt->type == NODE_ROLLBACK && stmt->rollback_to.name[0] == 0) {
            result = db_rollback() ? result_ok(0) : result_error("Failed to rollback");
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
            result = executor_run(&g_db.exec_ctx, stmt);
        }
    } else {
        result = result_ok(0);
    }

    if (result) result->exec_time_ms = get_time_ms() - t_start;

    // le modifiche allo schema vanno subito su disco: dopo un arresto anomalo le tabelle
    // create non devono sparire (il catalogo è piccolo, salvarlo costa poco)
    if (result && result->success && parsed->count > 0) {
        switch (parsed->statements[0]->type) {
            case NODE_CREATE_DB: case NODE_CREATE_TABLE: case NODE_CREATE_INDEX: case NODE_CREATE_VIEW:
            case NODE_DROP_DB: case NODE_DROP_TABLE: case NODE_DROP_VIEW: case NODE_DROP_INDEX:
            case NODE_ALTER_TABLE: case NODE_TRUNCATE: case NODE_CREATE_TRIGGER: case NODE_DROP_TRIGGER:
                bp_flush_all(g_db.bp);
                catalog_save(g_db.catalog, g_db.cat_path);
                break;
            default: break;
        }
    }

    parse_result_free(parsed);
    lexer_free(tokens);
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

static bool db_begin_impl(void) {
    if (!g_db.initialized) return false;
    Transaction* tx = txm_begin(g_db.txm);
    if (!tx) return false;
    g_db.exec_ctx.tx_id = tx->tx_id;
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
    for (int i = 0; i < g_db.bp->capacity; i++) {
        if (g_db.bp->frames[i].page)
            bp_remove_page(g_db.bp, g_db.bp->frames[i].page->page_id);
    }
    txm_abort(g_db.txm, g_db.exec_ctx.tx_id);
    g_db.exec_ctx.tx_id = 0;
    executor_invalidate_indexes();   // le righe sono tornate indietro (e possono aver cambiato posizione)
    return true;
}

// Simple savepoint tracking: store name and file offset
static struct { char name[64]; off_t offset; } g_savepoints[64];
static int g_num_savepoints = 0;

static bool db_savepoint_impl(const char* name) {
    if (!g_db.initialized) return false;
    if (!g_db.exec_ctx.tx_id) return false;
    // Write LOG_SAVEPOINT to WAL
    wal_write(g_db.wal, LOG_SAVEPOINT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, (const uint8_t*)name, strlen(name) + 1, NULL, 0);
    wal_flush(g_db.wal);

    // Record offset
    if (g_num_savepoints < 64) {
        snprintf(g_savepoints[g_num_savepoints].name, sizeof(g_savepoints[g_num_savepoints].name), "%s", name);
        g_savepoints[g_num_savepoints].offset = sv_lseek(g_db.wal->fd, 0, SEEK_CUR);
        g_num_savepoints++;
    }
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

    // Undo only entries after the savepoint offset for current tx
    off_t sp_off = g_savepoints[sp_idx].offset;
    int log_fd = sv_open(g_db.log_path, SV_O_RDONLY, 0);
    if (log_fd >= 0) {
        sv_lseek(log_fd, sp_off, SEEK_SET);
        #define MAX_UNDO 4096
        typedef struct { uint8_t type; uint32_t page_id; uint16_t slot_id;
                         uint32_t data_len; uint32_t old_data_len;
                         uint8_t* data; uint8_t* old_data; } UndoEntry;
        UndoEntry undo[MAX_UNDO];
        int n_undo = 0;

        uint8_t hdr[23];
        while (sv_read(log_fd, hdr, 23) == 23 && n_undo < MAX_UNDO) {
            uint64_t entry_tx_id;
            memcpy(&entry_tx_id, hdr + 1, 8);
            uint32_t page_id; memcpy(&page_id, hdr + 9, 4);
            uint16_t slot_id; memcpy(&slot_id, hdr + 13, 2);
            uint32_t dlen;    memcpy(&dlen, hdr + 15, 4);
            uint32_t oldlen;  memcpy(&oldlen, hdr + 19, 4);

            if (entry_tx_id == g_db.exec_ctx.tx_id && dlen > 0 && hdr[0] != LOG_BEGIN &&
                hdr[0] != LOG_COMMIT && hdr[0] != LOG_ABORT &&
                hdr[0] != LOG_SAVEPOINT && hdr[0] != LOG_ROLLBACK_TO) {
                UndoEntry* e = &undo[n_undo++];
                e->type = hdr[0]; e->page_id = page_id; e->slot_id = slot_id;
                e->data_len = dlen; e->old_data_len = oldlen;
                e->data = dlen > 0 ? malloc(dlen) : NULL;
                e->old_data = oldlen > 0 ? malloc(oldlen) : NULL;
                sv_read(log_fd, e->data, dlen);
                sv_read(log_fd, e->old_data, oldlen);
            } else {
                if (dlen > 0) sv_lseek(log_fd, dlen, SEEK_CUR);
                if (oldlen > 0) sv_lseek(log_fd, oldlen, SEEK_CUR);
            }
        }
        sv_close(log_fd);

        int db_fd = sv_open(g_db.db_path, SV_O_RDWR, 0);
        if (db_fd >= 0) {
            for (int i = n_undo - 1; i >= 0; i--) {
                UndoEntry* e = &undo[i];
                Page* p = page_read(e->page_id, db_fd);
                if (!p) { free(e->data); free(e->old_data); continue; }
                switch (e->type) {
                    case LOG_INSERT:
                        page_delete_slot(p, e->slot_id);
                        break;
                    case LOG_DELETE:
                        if (e->old_data && e->old_data_len > 0)
                            page_insert(p, e->old_data, e->old_data_len);
                        break;
                    case LOG_UPDATE:
                        page_delete_slot(p, e->slot_id);
                        if (e->old_data && e->old_data_len > 0)
                            page_insert(p, e->old_data, e->old_data_len);
                        break;
                }
                page_write(p, db_fd);
                page_free(p);
                free(e->data);
                free(e->old_data);
            }
            sv_close(db_fd);
        }
    }

    sv_ftruncate(g_db.wal->fd, sp_off);
    sv_lseek(g_db.wal->fd, 0, SEEK_END);
    g_num_savepoints = sp_idx;

    for (int i = 0; i < g_db.bp->capacity; i++) {
        if (g_db.bp->frames[i].page)
            bp_remove_page(g_db.bp, g_db.bp->frames[i].page->page_id);
    }
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
    TableSchema* t = catalog_get_table(g_db.catalog, db_name, table);
    if (!t) return NULL;
    size_t cap = 256 + (size_t)t->num_columns * 400;
    char* out = malloc(cap);
    if (!out) return NULL;
    size_t n = (size_t)snprintf(out, cap, "CREATE TABLE %s (", t->name);
    for (int i = 0; i < t->num_columns && n < cap; i++) {
        const Column* c = &t->columns[i];
        char type[48];
        if ((c->type == SV_TYPE_VARCHAR || c->type == SV_TYPE_CHAR) && c->size > 0)
            snprintf(type, sizeof(type), "%s(%d)", datatype_name(c->type), c->size);
        else
            snprintf(type, sizeof(type), "%s", datatype_name(c->type));
        n += (size_t)snprintf(out + n, cap - n, "%s\n    %s %s%s%s%s", i ? "," : "", c->name, type,
                              c->primary_key ? " PRIMARY KEY" : "",
                              c->auto_increment ? " AUTO_INCREMENT" : "",
                              (!c->nullable && !c->primary_key) ? " NOT NULL" : "");
        if (c->default_val[0] && n < cap) {
            bool num = c->type == SV_TYPE_INT || c->type == SV_TYPE_BIGINT || c->type == SV_TYPE_FLOAT ||
                       c->type == SV_TYPE_DECIMAL;
            n += (size_t)snprintf(out + n, cap - n, num ? " DEFAULT %s" : " DEFAULT '%s'", c->default_val);
        }
    }
    if (n < cap) snprintf(out + n, cap - n, "\n);");
    return out;
}

void db_free_string(char* s) { free(s); }

static void db_shutdown_impl(void) {
    if (!g_db.initialized) return;
    // una transazione lasciata aperta viene annullata (le sue pagine sono già nel buffer)
    if (g_db.exec_ctx.tx_id) db_rollback();
    bp_flush_all(g_db.bp);
    catalog_save(g_db.catalog, g_db.cat_path);
    sv_fsync(g_db.db_fd);
    wal_flush(g_db.wal);
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
