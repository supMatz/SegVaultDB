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
#include "../storage/page.h"
#include "../table/overflow.h"
#include "../table/heap.h"
#include "../query/lexer.h"
#include "../query/parser.h"
#include "../query/executor.h"
#include "../tx/transaction.h"
#include "../tx/wal.h"
#include "platform_compat.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

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
  __declspec(dllimport) void __stdcall Sleep(unsigned long ms);   // (windows.h non si può includere)
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
#include "locks.h"
static SvRwLock g_db_rw;
static _Thread_local int g_lock_depth;
static void chg_hook(uint64_t tx, const char* db, const char* table, uint8_t type, RID rid, RID nrid, const uint8_t* old, size_t ol);
static void db_mutex_init(void) { sv_rw_init(&g_db_rw); lk_init(); executor_set_change_hook(chg_hook); }
static void db_lock(void)       { if (g_lock_depth++ == 0) sv_rw_wr(&g_db_rw); }
static void db_unlock(void)     { if (--g_lock_depth == 0) sv_rw_wrunlock(&g_db_rw); }

static char g_init_error[256];
const char* db_init_error(void) { return g_init_error; }

// ── Forward declarations ─────────────────────────────────────────
static QueryResult* result_error(const char* msg);
static void free_list_load(void);
static int g_open_tx;                     // transazioni aperte, di tutte le sessioni (con il lock del motore)
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
    free_list_load();
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
    if (!g_db.initialized || g_open_tx > 0) return false;     // il log serve ancora alle transazioni aperte
    bp_flush_all(g_db.bp);
    if (catalog_save(g_db.catalog, g_db.cat_path) != SV_OK) return false;
    return wal_truncate(g_db.wal) == SV_OK;
}

// ── VACUUM: pagine di overflow non più usate -> riusabili ───────

static void free_list_path(char* out, size_t n) { snprintf(out, n, "%s" SV_PATH_SEP "segvault.free", g_db.data_dir); }

static void free_list_save(void) {
    int n = bp_free_count(g_db.bp);
    uint32_t* ids = malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
    if (!ids) return;
    n = bp_free_list(g_db.bp, ids, n);
    char p[600];
    free_list_path(p, sizeof(p));
    FILE* f = fopen(p, "w");
    if (f) { for (int i = 0; i < n; i++) fprintf(f, "%u\n", ids[i]); fclose(f); }
    free(ids);
}

static void free_list_load(void) {
    char p[600];
    free_list_path(p, sizeof(p));
    FILE* f = fopen(p, "r");
    if (!f) return;
    unsigned id;
    while (fscanf(f, "%u", &id) == 1) if (id) bp_add_free(g_db.bp, id);   // validi solo se su disco sono FREE
    fclose(f);
}

static bool do_checkpoint(void);

static bool do_vacuum(int* freed) {
    *freed = 0;
    if (!do_checkpoint()) return false;                 // log vuoto: nessun record nomina le pagine
    long end = sv_lseek(g_db.db_fd, 0, SEEK_END);
    uint32_t npages = end > 0 ? (uint32_t)(end / SV_PAGE_SIZE) : 0;
    uint8_t* used = calloc((size_t)npages / 8 + 1, 1);
    if (!used) return false;
    for (int d = 0; d < g_db.catalog->num_databases; d++) {
        Database* db = &g_db.catalog->databases[d];
        for (int t = 0; t < db->num_tables; t++) {
            HeapFile* hf = heap_open(g_db.bp, db->tables[t].root_page_id, db->tables[t].num_pages);
            if (hf) heap_mark_overflow(hf, used, npages);
            free(hf);
        }
    }
    // l'elenco si ricostruisce da capo: tutte le pagine libere (anche quelle di VACUUM precedenti)
    g_db.bp->nfree = 0;
    for (uint32_t id = 1; id < npages; id++) {
        Page* p = bp_fetch(g_db.bp, id);
        if (!p) continue;
        bool overflow_unused = (p->flags & PAGE_FLAG_OVERFLOW) && !(used[id / 8] & (1u << (id % 8)));
        bool was_free = (p->flags & PAGE_FLAG_FREE) != 0;
        if (overflow_unused) {
            p->flags = PAGE_FLAG_FREE;                  // anche il "successivo" della catena azzerato
            p->next_page_id = 0;
            (*freed)++;
        }
        bp_unpin(g_db.bp, id, overflow_unused);
        if (overflow_unused || was_free) bp_add_free(g_db.bp, id);
    }
    free(used);
    bp_flush_all(g_db.bp);
    sv_fsync(g_db.db_fd);
    free_list_save();
    return true;
}

static void maybe_checkpoint(void) {
    if (g_db.initialized && g_open_tx == 0 && wal_size(g_db.wal) > SV_WAL_CHECKPOINT_BYTES) do_checkpoint();
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
    // CALL: la procedura è atomica come uno statement (se un'istruzione fallisce si annulla tutto)
    // INSERT IGNORE / REPLACE / ON DUPLICATE KEY UPDATE: più passi (INSERT, DELETE, UPDATE) per uno statement
    bool mysql_insert = (stmt->type == NODE_INSERT && (stmt->insert_stmt.mode != INS_NORMAL || stmt->insert_stmt.num_dup > 0)) ||
                        (stmt->type == NODE_UPDATE && stmt->update_stmt.multi_from) ||
                        (stmt->type == NODE_DELETE && stmt->delete_stmt.multi_from);
    if (stmt->type != NODE_CALL && !mysql_insert && (!dml || !db || db->num_triggers == 0)) return executor_run(&g_db.exec_ctx, stmt);
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

// database di uno statement con nomi qualificati (shop.prodotto): indice, o -1 con un messaggio
#define QUAL_INFOSCHEMA (-3)   // information_schema.x: tabelle virtuali del catalogo

static int qual_db_index(const ASTNode* stmt, int cur, char* err, size_t errn) {
    if (!stmt->qual_db[0]) return cur;
    if (!strcasecmp(stmt->qual_db, "information_schema")) {
        if (stmt->type != NODE_SELECT && stmt->type != NODE_EXPLAIN) { snprintf(err, errn, "information_schema is read-only"); return -1; }
        return QUAL_INFOSCHEMA;
    }
    int q = -1;
    for (int i = 0; i < g_db.catalog->num_databases; i++)
        if (!strcasecmp(g_db.catalog->databases[i].name, stmt->qual_db)) q = i;
    if (q < 0) { snprintf(err, errn, "Unknown database '%.63s'", stmt->qual_db); return -1; }
    if (stmt->qual_mixed && cur != q) {
        snprintf(err, errn, "Tables of different databases in one statement are not supported yet: "
                 "qualify every table with '%.63s.' or run USE %.63s first", stmt->qual_db, stmt->qual_db);
        return -1;
    }
    return q;
}

static QueryResult* exec_one_in(ASTNode* stmt);
static _Thread_local char g_see_user[64];
static bool see_db_of_session(const char* db) { return auth_can_see_db(g_see_user[0] ? g_see_user : g_session_user, db); }
static bool db_backup_impl(const char* dir, char* err, size_t errn);

// statement con shop.prodotto: gira nel database shop (solo per questo statement e questo thread)
static QueryResult* exec_one(ASTNode* stmt) {
    if (!stmt->qual_db[0]) return exec_one_in(stmt);
    char err[300];
    Database* cur = catalog_current_db(g_db.catalog);
    int ci = cur ? (int)(cur - g_db.catalog->databases) : -1;
    int q = qual_db_index(stmt, ci, err, sizeof(err));
    if (q == QUAL_INFOSCHEMA) {                          // tutti la leggono; ognuno vede i suoi database
        executor_set_infoschema(true);
        executor_set_db_filter(g_session_user[0] ? see_db_of_session : NULL);
        char saved[64];
        snprintf(saved, sizeof(saved), "%s", g_session_user);
        snprintf(g_see_user, sizeof(g_see_user), "%s", saved);   // il filtro usa l'utente vero
        g_session_user[0] = '\0';                        // niente controllo dei permessi sulle tabelle virtuali
        QueryResult* r = exec_one_in(stmt);
        snprintf(g_session_user, sizeof(g_session_user), "%s", saved);
        g_see_user[0] = '\0';
        executor_set_infoschema(false);
        executor_set_db_filter(NULL);
        return r;
    }
    if (q < 0) return result_error(err);
    if (q == ci) return exec_one_in(stmt);
    int prev = catalog_get_thread_db();
    catalog_set_thread_db(q);
    QueryResult* r = exec_one_in(stmt);
    catalog_set_thread_db(prev);
    return r;
}


// ── manutenzione delle tabelle (CHECK / ANALYZE / OPTIMIZE / REPAIR TABLE) ──
// Risultato come MySQL: Table | Op | Msg_type | Msg_text, una riga per messaggio.

typedef struct { QueryResult* r; int cap; } MaintOut;

static void maint_row(MaintOut* m, const char* table, const char* op, const char* type, const char* text) {
    if (!m->r) return;
    if (m->r->num_rows >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        Row* nr = realloc(m->r->rows, sizeof(Row) * (size_t)nc);
        if (!nr) return;
        m->r->rows = nr;
        m->cap = nc;
    }
    const char* v[4] = { table, op, type, text };
    if (db_row_set(&m->r->rows[m->r->num_rows], 4, v, NULL)) m->r->num_rows++;
}

// una query interna con un solo numero come risultato (-1 = errore, messaggio in err)
static long long maint_count(const char* sql, char* err, size_t en) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    long long v = -1;
    if (pr && pr->success && pr->count == 1) {
        QueryResult* r = executor_run(&g_db.exec_ctx, pr->statements[0]);
        if (r && r->success && r->num_rows > 0 && r->num_cols > 0 && !r->rows[0].cells[0].is_null) v = atoll(r->rows[0].cells[0].value);
        else if (r && r->success) v = 0;
        else snprintf(err, en, "%.250s", r ? r->error : "out of memory");
        db_result_free(r);
    } else snprintf(err, en, "%.250s", pr ? pr->error : "out of memory");
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    return v;
}

// CHECK TABLE: si leggono tutte le righe e si controllano chiave primaria, NOT NULL, UNIQUE e chiavi esterne
static void maint_check(MaintOut* m, const char* db, TableSchema* ts) {
    char q[1200], err[300], msg[400];
    int errors = 0;
    snprintf(q, sizeof(q), "SELECT COUNT(*) FROM `%s`", ts->name);
    long long rows = maint_count(q, err, sizeof(err));
    if (rows < 0) { snprintf(msg, sizeof(msg), "Rows cannot be read: %s", err); maint_row(m, ts->name, "check", "error", msg); return; }
    for (int c = 0; c < ts->num_columns; c++) {
        if (ts->columns[c].nullable && !ts->columns[c].primary_key) continue;
        snprintf(q, sizeof(q), "SELECT COUNT(*) FROM `%s` WHERE `%s` IS NULL", ts->name, ts->columns[c].name);
        long long n = maint_count(q, err, sizeof(err));
        if (n > 0) { snprintf(msg, sizeof(msg), "%lld rows have NULL in NOT NULL column %s", n, ts->columns[c].name); maint_row(m, ts->name, "check", "error", msg); errors++; }
    }
    for (int k = 0; k < ts->num_constraints; k++) {
        const Constraint* c = &ts->constraints[k];
        if (c->kind != CONS_PRIMARY && c->kind != CONS_UNIQUE && c->kind != CONS_FOREIGN) continue;
        char cols[600] = "", where[700] = "";
        size_t cl = 0, wl = 0;
        for (int j = 0; j < c->ncols; j++) {
            const char* cn = ts->columns[c->cols[j]].name;
            cl += (size_t)snprintf(cols + cl, sizeof(cols) - cl, "%s`%s`", j ? ", " : "", cn);
            wl += (size_t)snprintf(where + wl, sizeof(where) - wl, "%s`%s` IS NOT NULL", j ? " AND " : "", cn);
        }
        if (c->kind != CONS_FOREIGN) {
            snprintf(q, sizeof(q), "SELECT COUNT(*) FROM (SELECT %s FROM `%s` WHERE %s GROUP BY %s HAVING COUNT(*) > 1) AS d",
                     cols, ts->name, where, cols);
            long long n = maint_count(q, err, sizeof(err));
            if (n > 0) {
                snprintf(msg, sizeof(msg), "%lld duplicated values of the %s key (%s)", n, c->kind == CONS_PRIMARY ? "primary" : "unique", cols);
                maint_row(m, ts->name, "check", "error", msg);
                errors++;
            }
        } else {
            TableSchema* ps = catalog_get_table(g_db.catalog, db, c->ref_table);
            if (!ps) { snprintf(msg, sizeof(msg), "Foreign key %s: table %s does not exist", c->name, c->ref_table); maint_row(m, ts->name, "check", "error", msg); errors++; continue; }
            char on[900] = "";
            size_t ol = 0;
            for (int j = 0; j < c->ncols; j++) {
                const char* rn = c->ref_cols[j];
                if (!rn[0]) {                                  // la chiave primaria del genitore, j-esima colonna
                    int seen = 0;
                    for (int k2 = 0; k2 < ps->num_constraints && !rn[0]; k2++)
                        if (ps->constraints[k2].kind == CONS_PRIMARY && j < ps->constraints[k2].ncols) rn = ps->columns[ps->constraints[k2].cols[j]].name;
                    for (int k2 = 0; k2 < ps->num_columns && !rn[0]; k2++)
                        if (ps->columns[k2].primary_key && seen++ == j) rn = ps->columns[k2].name;
                }
                ol += (size_t)snprintf(on + ol, sizeof(on) - ol, "%sp.`%s` = c.`%s`", j ? " AND " : "", rn, ts->columns[c->cols[j]].name);
            }
            char cw[700] = "";
            size_t cwl = 0;
            for (int j = 0; j < c->ncols; j++)
                cwl += (size_t)snprintf(cw + cwl, sizeof(cw) - cwl, "%sc.`%s` IS NOT NULL", j ? " AND " : "", ts->columns[c->cols[j]].name);
            snprintf(q, sizeof(q), "SELECT COUNT(*) FROM `%s` c WHERE %s AND NOT EXISTS (SELECT 1 FROM `%s` p WHERE %s)",
                     ts->name, cw, ps->name, on);
            long long n = maint_count(q, err, sizeof(err));
            if (n > 0) {
                snprintf(msg, sizeof(msg), "%lld rows reference a missing row of %s (foreign key %s)", n, ps->name, c->name[0] ? c->name : cols);
                maint_row(m, ts->name, "check", "error", msg);
                errors++;
            } else if (n < 0) { snprintf(msg, sizeof(msg), "Foreign key %s not checked: %s", c->name, err); maint_row(m, ts->name, "check", "warning", msg); }
        }
    }
    if (errors) snprintf(msg, sizeof(msg), "Inconsistent: %d problem%s found", errors, errors == 1 ? "" : "s");
    else snprintf(msg, sizeof(msg), "OK (%lld rows)", rows);
    maint_row(m, ts->name, "check", errors ? "error" : "status", msg);
}

static QueryResult* exec_table_maint(ASTNode* stmt) {
    Database* d = catalog_current_db(g_db.catalog);
    if (!d) return result_error("No database selected");
    char dbname[64];
    snprintf(dbname, sizeof(dbname), "%s", d->name);
    const char* op = stmt->maint.op;
    char opl[12];
    snprintf(opl, sizeof(opl), "%s", op);
    for (char* c = opl; *c; c++) *c = (char)tolower((unsigned char)*c);
    bool optimize = !strcmp(op, "OPTIMIZE");
    if ((optimize) && g_open_tx > 0) return result_error("OPTIMIZE TABLE is not allowed while a transaction is open");
    MaintOut m = { calloc(1, sizeof(QueryResult)), 0 };
    if (!m.r) return result_error("Out of memory");
    static const char* const NAMES[4] = { "Table", "Op", "Msg_type", "Msg_text" };
    m.r->success = true;
    m.r->num_cols = 4;
    m.r->col_names = calloc(4, sizeof(char*));
    m.r->col_types = calloc(4, sizeof(char*));
    for (int i = 0; m.r->col_names && m.r->col_types && i < 4; i++) { m.r->col_names[i] = strdup(NAMES[i]); m.r->col_types[i] = strdup("VARCHAR"); }
    bool vacuumed = false;
    for (int i = 0; i < stmt->maint.ntables; i++) {
        const char* tn = stmt->maint.tables[i];
        TableSchema* ts = catalog_get_table(g_db.catalog, dbname, tn);
        if (!ts) { maint_row(&m, tn, opl, "Error", "Table doesn't exist"); continue; }
        char msg[200], err[300], q[200];
        if (!strcmp(op, "CHECK")) { maint_check(&m, dbname, ts); continue; }
        // ANALYZE / REPAIR / OPTIMIZE: conteggio esatto delle righe (statistiche dell'ottimizzatore) e indici ricostruiti
        snprintf(q, sizeof(q), "SELECT COUNT(*) FROM `%s`", ts->name);
        long long rows = maint_count(q, err, sizeof(err));
        if (rows < 0) { snprintf(msg, sizeof(msg), "%.180s", err); maint_row(&m, tn, opl, "error", msg); continue; }
        ts = catalog_get_table(g_db.catalog, dbname, tn);
        if (ts) ts->num_rows = (uint64_t)rows;
        if (strcmp(op, "ANALYZE")) executor_invalidate_indexes();          // ricostruiti al primo uso
        if (optimize && !vacuumed) {
            int freed = 0;
            vacuumed = do_vacuum(&freed);
            snprintf(msg, sizeof(msg), vacuumed ? "OK (%lld rows; %d unused pages reclaimed)" : "VACUUM failed", rows, freed);
            maint_row(&m, tn, opl, vacuumed ? "status" : "error", msg);
            continue;
        }
        snprintf(msg, sizeof(msg), "OK (%lld rows)", rows);
        maint_row(&m, tn, opl, "status", msg);
    }
    catalog_save(g_db.catalog, g_db.cat_path);                             // statistiche aggiornate
    return m.r;
}

// istruzioni dentro una procedura: con i permessi dell'utente che la chiama
static bool proc_auth(const ASTNode* st, char* err, size_t errn) {
    return auth_check(g_session_user, st, cur_db_name(), err, errn);
}

// esegue uno statement già analizzato (transazioni e CHECKPOINT gestiti qui)
static QueryResult* exec_one_in(ASTNode* stmt) {
    QueryResult* result = NULL;
    g_db.exec_ctx.auth_hook = g_session_user[0] ? proc_auth : NULL;
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
    } else if (stmt->type == NODE_TABLE_MAINT) {
        result = exec_table_maint(stmt);
    } else if (stmt->type == NODE_BACKUP) {
        char err[600];
        result = db_backup_impl(stmt->backup.path, err, sizeof(err)) ? result_ok(0) : result_error(err);
    } else if (stmt->type == NODE_VACUUM) {
        int freed = 0;
        result = g_open_tx > 0 ? result_error("VACUUM is not allowed while a transaction is open")
               : do_vacuum(&freed) ? result_ok(freed) : result_error("VACUUM failed");
    } else if (stmt->type == NODE_CHECKPOINT) {
        result = g_open_tx > 0 ? result_error(g_db.exec_ctx.tx_id ? "CHECKPOINT is not allowed inside a transaction"
                                                                  : "CHECKPOINT is not allowed while a transaction is open")
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

    // SHOW DATABASES: un utente vede solo i database in cui ha qualche permesso
    if (result && result->success && g_session_user[0] && stmt->type == NODE_SHOW &&
        !strcmp(stmt->select_stmt.table_name, "DATABASES")) {
        int k = 0;
        for (int i = 0; i < result->num_rows; i++) {
            const Cell* c = &result->rows[i].cells[0];
            if (!c->is_null && auth_can_see_db(g_session_user, c->value)) result->rows[k++] = result->rows[i];
            else free(result->rows[i].cells);
        }
        result->num_rows = k;
    }

    // le modifiche allo schema vanno subito su disco: dopo un arresto anomalo le tabelle
    // create non devono sparire (il catalogo è piccolo, salvarlo costa poco)
    if (result && result->success) {
        switch (stmt->type) {
            case NODE_CREATE_DB: case NODE_CREATE_TABLE: case NODE_CREATE_INDEX: case NODE_CREATE_VIEW:
            case NODE_DROP_DB: case NODE_DROP_TABLE: case NODE_DROP_VIEW: case NODE_DROP_INDEX:
            case NODE_ALTER_TABLE: case NODE_TRUNCATE: case NODE_CREATE_TRIGGER: case NODE_DROP_TRIGGER:
            case NODE_CREATE_PROCEDURE: case NODE_DROP_PROCEDURE:
                bp_flush_all(g_db.bp);
                catalog_save(g_db.catalog, g_db.cat_path);
                break;
            default: break;
        }
    }
    return result;
}


typedef struct { char name[64]; off_t offset; uint64_t chg_seq; } SavepointRec;

struct DbSession {
    int          cur_db;            // indice nel catalogo (-1 = nessuno)
    uint64_t     tx_id;
    SavepointRec sp[64];
    int          nsp;
    char         user[64];
};


// ── letture della versione confermata (MVCC) ────────────────────
// Ogni modifica di una transazione aperta resta qui (riga di prima, posizione) finché la transazione
// finisce. Una SELECT che troverebbe una tabella in scrittura altrui non aspetta: legge la versione
// confermata (heap_snapshot), ricostruita da queste voci. Le scritture restano serializzate dai lock.

typedef struct {
    uint64_t tx, seq;
    char     db[64], table[64];
    uint8_t  type;                 // LOG_INSERT / LOG_DELETE / LOG_UPDATE / LOG_UPDATE_MOVE; 0 = non registrata (memoria)
    RID      rid, nrid;
    uint8_t* old;
    size_t   ol;
} TxChange;

static TxChange* g_chg;
static int       g_nchg, g_capchg;
static uint64_t  g_chg_seq;

static void chg_hook(uint64_t tx, const char* db, const char* table, uint8_t type, RID rid, RID nrid, const uint8_t* old, size_t ol) {
    if (g_nchg == g_capchg) {
        int nc = g_capchg ? g_capchg * 2 : 256;
        TxChange* nv = realloc(g_chg, sizeof(TxChange) * (size_t)nc);
        if (!nv) { if (g_nchg) g_chg[g_nchg - 1].type = 0; return; }     // la transazione non si potrà leggere confermata
        g_chg = nv;
        g_capchg = nc;
    }
    TxChange* c = &g_chg[g_nchg];
    memset(c, 0, sizeof(*c));
    c->tx = tx;
    c->seq = ++g_chg_seq;
    snprintf(c->db, sizeof(c->db), "%s", db);
    snprintf(c->table, sizeof(c->table), "%s", table);
    c->type = type;
    c->rid = rid;
    c->nrid = nrid;
    if (old && ol) {
        c->old = malloc(ol);
        if (c->old) { memcpy(c->old, old, ol); c->ol = ol; }
        else c->type = 0;
    }
    g_nchg++;
}

// via le voci della transazione tx dopo il numero after (0 = tutte): COMMIT, ROLLBACK, ROLLBACK TO
static void chg_drop(uint64_t tx, uint64_t after) {
    int k = 0;
    for (int i = 0; i < g_nchg; i++) {
        if (g_chg[i].tx == tx && g_chg[i].seq > after) { free(g_chg[i].old); continue; }
        g_chg[k++] = g_chg[i];
    }
    g_nchg = k;
}

typedef struct { RID rid; const uint8_t* old; size_t ol; bool fresh; } SnapRow;

static int snaprow_cmp(const void* a, const void* b) {
    const SnapRow* x = a; const SnapRow* y = b;
    if (x->rid.page_id != y->rid.page_id) return x->rid.page_id < y->rid.page_id ? -1 : 1;
    return x->rid.slot_id < y->rid.slot_id ? -1 : x->rid.slot_id > y->rid.slot_id;
}

static HeapSnapshot g_snaps[16];
static int g_nsnaps;

static void snapshot_clear(void) {
    heap_snapshot_set(NULL, 0);
    for (int i = 0; i < g_nsnaps; i++) {
        free(g_snaps[i].hide); free(g_snaps[i].repl_rid); free(g_snaps[i].repl); free(g_snaps[i].repl_len);
        free(g_snaps[i].extra); free(g_snaps[i].extra_len);
    }
    memset(g_snaps, 0, sizeof(g_snaps));
    g_nsnaps = 0;
}

// versione confermata della tabella (db, table) rispetto alla transazione tx
static bool snapshot_build(uint64_t tx, int dbi, const char* table, HeapSnapshot* out) {
    if (dbi < 0 || dbi >= g_db.catalog->num_databases || g_nsnaps >= 16) return false;
    Database* d = &g_db.catalog->databases[dbi];
    TableSchema* ts = catalog_get_table(g_db.catalog, d->name, table);
    if (!ts) return false;
    memset(out, 0, sizeof(*out));
    out->root = ts->root_page_id;
    int cap = 0, n = 0, ecap = 0;
    SnapRow* m = NULL;              // posizione attuale -> origine (riga confermata o nuova)
    for (int i = 0; i < g_nchg; i++) {
        const TxChange* c = &g_chg[i];
        if (c->tx != tx || strcasecmp(c->db, d->name) || strcasecmp(c->table, table)) continue;
        if (!c->type) { free(m); return false; }
        if (n + 1 > cap) {
            cap = cap ? cap * 2 : 64;
            SnapRow* nm = realloc(m, sizeof(SnapRow) * (size_t)cap);
            if (!nm) { free(m); return false; }
            m = nm;
        }
        int at = -1;
        RID key = c->rid;
        for (int k = 0; k < n; k++) if (m[k].rid.page_id == key.page_id && m[k].rid.slot_id == key.slot_id) { at = k; break; }
        SnapRow origin = { key, c->old, c->ol, false };
        if (at >= 0) origin = m[at];
        switch (c->type) {
        case LOG_INSERT:
            if (at >= 0) m[at] = (SnapRow){ key, NULL, 0, true };
            else m[n++] = (SnapRow){ key, NULL, 0, true };
            break;
        case LOG_UPDATE:
            if (at < 0) m[n++] = origin;                         // prima modifica: la versione confermata è la vecchia
            break;
        case LOG_UPDATE_MOVE:
            if (at >= 0) m[at] = m[--n];                         // la vecchia posizione non c'è più
            origin.rid = c->nrid;
            m[n++] = origin;
            break;
        case LOG_DELETE:
            if (at >= 0) m[at] = m[--n];
            if (!origin.fresh && origin.old) {                   // riga confermata cancellata: torna in fondo
                if (out->nextra + 1 > ecap) {
                    ecap = ecap ? ecap * 2 : 16;
                    uint8_t** ne = realloc(out->extra, sizeof(uint8_t*) * (size_t)ecap);
                    size_t* nl = realloc(out->extra_len, sizeof(size_t) * (size_t)ecap);
                    if (ne) out->extra = ne;
                    if (nl) out->extra_len = nl;
                    if (!ne || !nl) { free(m); return false; }
                }
                out->extra[out->nextra] = (uint8_t*)origin.old;
                out->extra_len[out->nextra++] = origin.ol;
            }
            break;
        }
    }
    if (n) qsort(m, (size_t)n, sizeof(SnapRow), snaprow_cmp);
    out->hide = malloc(sizeof(RID) * (size_t)(n + 1));
    out->repl_rid = malloc(sizeof(RID) * (size_t)(n + 1));
    out->repl = malloc(sizeof(uint8_t*) * (size_t)(n + 1));
    out->repl_len = malloc(sizeof(size_t) * (size_t)(n + 1));
    if (!out->hide || !out->repl_rid || !out->repl || !out->repl_len) { free(m); return false; }
    for (int k = 0; k < n; k++) {
        if (m[k].fresh) out->hide[out->nhide++] = m[k].rid;
        else if (m[k].old) {
            out->repl_rid[out->nrepl] = m[k].rid;
            out->repl[out->nrepl] = (uint8_t*)m[k].old;
            out->repl_len[out->nrepl++] = m[k].ol;
        }
    }
    free(m);
    return true;
}

// SELECT ... FOR UPDATE / FOR SHARE / LOCK IN SHARE MODE: aspetta come in MySQL (niente versione confermata)
static bool locking_read(const TokenList* tl) {
    for (int i = 0; tl && i + 1 < tl->count; i++) {
        const Token* t = &tl->tokens[i];
        if (t->type == TOK_FOR && (tl->tokens[i + 1].type == TOK_UPDATE ||
                                   (tl->tokens[i + 1].text && !strcasecmp(tl->tokens[i + 1].text, "SHARE")))) return true;
        if (t->text && !strcasecmp(t->text, "LOCK") && tl->tokens[i + 1].type == TOK_IN) return true;
    }
    return false;
}

// ── lock delle tabelle per statement (transazioni concorrenti, vedi locks.h) ──

static int g_lock_wait_ms = 30000;
void db_set_lock_wait_timeout(int ms) { g_lock_wait_ms = ms > 0 ? ms : 30000; }
#define MAX_STMT_LOCKS 96

static _Thread_local void (*g_wait_hook)(void*);
static _Thread_local void* g_wait_ud;

void db_set_lock_wait_hook(void (*hook)(void* ud), void* ud) { g_wait_hook = hook; g_wait_ud = ud; }

typedef struct {
    LkReq r[MAX_STMT_LOCKS];
    int   n;
    bool  overflow;               // troppe tabelle: si usa il lock globale esclusivo
} LockSet;

static void ls_add(LockSet* ls, int db, const char* name, uint8_t mode) {
    char low[64];
    size_t l = strlen(name);
    if (l >= sizeof(low)) l = sizeof(low) - 1;
    for (size_t i = 0; i < l; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[l] = '\0';
    for (int i = 0; i < ls->n; i++)
        if (ls->r[i].db == db && !strcmp(ls->r[i].table, low)) { if (mode > ls->r[i].mode) ls->r[i].mode = mode; return; }
    if (ls->n >= MAX_STMT_LOCKS) { ls->overflow = true; return; }
    ls->r[ls->n].db = db;
    memcpy(ls->r[ls->n].table, low, l + 1);
    ls->r[ls->n].mode = mode;
    ls->n++;
}

static uint8_t ls_mode(const LockSet* ls, int db, const char* name) {
    for (int i = 0; i < ls->n; i++) if (ls->r[i].db == db && !strcasecmp(ls->r[i].table, name)) return ls->r[i].mode;
    return 0;
}

static void ls_scan_text(LockSet* ls, int dbi, const char* sql, uint8_t mode, int depth);

// una tabella (o vista, o procedura) con quello che può toccare: trigger, chiavi esterne, definizioni
static void ls_name(LockSet* ls, int dbi, const char* name, uint8_t mode, int depth) {
    if (dbi < 0 || dbi >= g_db.catalog->num_databases || depth > 8 || !name[0]) return;
    Database* d = &g_db.catalog->databases[dbi];
    for (int t = 0; t < d->num_tables; t++) {
        if (strcasecmp(d->tables[t].name, name)) continue;
        if (ls_mode(ls, dbi, name) >= mode) return;            // già visto (anche nei cicli di riferimenti)
        ls_add(ls, dbi, name, mode);
        if (mode != LK_X) return;
        for (int g = 0; g < d->num_triggers; g++)               // i trigger possono scrivere ovunque
            if (!strcasecmp(d->triggers[g].table_name, name)) ls_scan_text(ls, dbi, d->triggers[g].body, LK_X, depth + 1);
        const TableSchema* ts = &d->tables[t];
        for (int c = 0; c < ts->num_constraints; c++)           // tabelle riferite: controllate (lettura)
            if (ts->constraints[c].kind == CONS_FOREIGN) ls_name(ls, dbi, ts->constraints[c].ref_table, LK_S, depth + 1);
        for (int u = 0; u < d->num_tables; u++)                 // tabelle che la riferiscono: CASCADE / SET NULL
            for (int c = 0; c < d->tables[u].num_constraints; c++)
                if (d->tables[u].constraints[c].kind == CONS_FOREIGN && !strcasecmp(d->tables[u].constraints[c].ref_table, name))
                    ls_name(ls, dbi, d->tables[u].name, LK_X, depth + 1);
        return;
    }
    for (int v = 0; v < d->num_views; v++)
        if (!strcasecmp(d->views[v].name, name)) { ls_scan_text(ls, dbi, d->views[v].definition, mode, depth + 1); return; }
    for (int r = 0; r < d->num_routines; r++)
        if (!strcasecmp(d->routines[r].name, name)) { ls_scan_text(ls, dbi, d->routines[r].body, LK_X, depth + 1); return; }
}

static int db_index_of(const char* name) {
    for (int i = 0; i < g_db.catalog->num_databases; i++) if (!strcasecmp(g_db.catalog->databases[i].name, name)) return i;
    return -1;
}

// ogni parola del testo che è il nome di una tabella / vista / procedura (sovrastima: sempre sicura)
static void ls_scan_tokens(LockSet* ls, int dbi, const TokenList* tl, uint8_t mode, int depth) {
    for (int i = 0; tl && i < tl->count; i++) {
        const Token* t = &tl->tokens[i];
        if (t->type == TOK_STRING || t->type == TOK_INTEGER || t->type == TOK_FLOAT || !t->text || !t->text[0]) continue;
        if (!isalpha((unsigned char)t->text[0]) && t->text[0] != '_' && (unsigned char)t->text[0] < 0x80) continue;
        if (i + 2 < tl->count && tl->tokens[i + 1].type == TOK_DOT && tl->tokens[i + 2].text) {   // db.tabella
            int q = db_index_of(t->text);
            if (q >= 0) { ls_name(ls, q, tl->tokens[i + 2].text, mode, depth); i += 2; continue; }
        }
        ls_name(ls, dbi, t->text, mode, depth);
    }
}

static void ls_scan_text(LockSet* ls, int dbi, const char* sql, uint8_t mode, int depth) {
    if (!sql || !sql[0] || depth > 8) return;
    TokenList* tl = lexer_tokenize(sql);
    ls_scan_tokens(ls, dbi, tl, mode, depth);
    if (tl) lexer_free(tl);
}

// 0 = nessun lock (controllo delle transazioni, USE), 1 = lettura, 2 = scrittura, 3 = schema / manutenzione
static int stmt_lock_kind(const ASTNode* st) {
    switch (st->type) {
    case NODE_BEGIN: case NODE_COMMIT: case NODE_ROLLBACK: case NODE_SAVEPOINT: case NODE_ROLLBACK_TO: case NODE_USE:
        return 0;
    case NODE_SELECT: case NODE_SHOW: case NODE_DESCRIBE: case NODE_EXPLAIN:
        return 1;
    case NODE_INSERT: case NODE_UPDATE: case NODE_DELETE: case NODE_CALL:
        return 2;
    default:
        return 3;
    }
}

// lock di uno statement: le tabelle del suo testo, più il lock globale (S per tutti, X per lo schema)
static void stmt_locks(const ASTNode* st, const TokenList* tl, LockSet* ls, int dbi) {
    ls->n = 0;
    ls->overflow = false;
    int kind = stmt_lock_kind(st);
    if (kind == 0) return;
    if (kind != 3) ls_scan_tokens(ls, dbi, tl, kind == 2 ? LK_X : LK_S, 0);
    if (kind == 3 || ls->overflow) ls->n = 0;
    LkReq* g = &ls->r[ls->n < MAX_STMT_LOCKS ? ls->n : MAX_STMT_LOCKS - 1];
    g->db = -1;
    snprintf(g->table, sizeof(g->table), "*");
    g->mode = kind == 3 || ls->overflow ? LK_X : LK_S;
    if (ls->n < MAX_STMT_LOCKS) ls->n++;
}

static const void* lock_owner(void);
static void session_save(DbSession* s);
static void session_load(const DbSession* s);
static DbSession  g_default_session;
static DbSession* g_active;

// prende i lock; 0 = presi, 1 = deadlock, 2 = tempo scaduto. Mentre aspetta lascia il motore
// (la sessione esce, il lock del motore si libera, l'hook del thread lascia passare gli altri)
static uint64_t owner_tx(const void* owner) {
    if (owner == (const void*)&g_default_session) return g_active ? g_default_session.tx_id : g_db.exec_ctx.tx_id;
    if (owner == (const void*)g_active) return g_db.exec_ctx.tx_id;
    return ((const DbSession*)owner)->tx_id;
}

// lettura senza attesa: le tabelle in scrittura altrui si leggono nella versione confermata, le altre
// con il lock S solito. true = fatto (heap_snapshot impostato per questo thread)
static bool try_snapshot(const LockSet* ls) {
    const void* owner = lock_owner();
    LkReq rest[MAX_STMT_LOCKS];
    int nr = 0;
    snapshot_clear();
    for (int i = 0; i < ls->n; i++) {
        const LkReq* q = &ls->r[i];
        const void* xo = q->mode == LK_S && q->db >= 0 ? lk_x_holder(q->db, q->table, owner) : NULL;
        if (xo) {
            uint64_t tx = owner_tx(xo);
            if (!tx || !snapshot_build(tx, q->db, q->table, &g_snaps[g_nsnaps])) { snapshot_clear(); return false; }
            g_nsnaps++;
        } else rest[nr++] = *q;
    }
    if (!g_nsnaps) return false;
    if (lk_acquire(owner, rest, nr) != LK_GRANTED) { lk_cancel_wait(owner); snapshot_clear(); return false; }
    heap_snapshot_set(g_snaps, g_nsnaps);
    return true;
}

static int acquire_locks(const LockSet* ls, bool allow_snapshot) {
    if (!ls->n) return 0;
    double t0 = get_time_ms();
    for (;;) {
        const void* owner = lock_owner();
        int r = lk_acquire(owner, ls->r, ls->n);
        if (r == LK_GRANTED) return 0;
        if (r == LK_DEADLOCK) return 1;
        if (allow_snapshot && try_snapshot(ls)) return 3;     // SELECT: la versione confermata, subito
        if (get_time_ms() - t0 > g_lock_wait_ms || g_lock_depth != 1) { lk_cancel_wait(owner); return 2; }
        DbSession* s = g_active;
        if (s) { session_save(s); session_load(&g_default_session); g_active = NULL; }
        g_lock_depth = 0;
        sv_rw_wrunlock(&g_db_rw);
        if (g_wait_hook) g_wait_hook(g_wait_ud);
        else {
#ifdef _WIN32
            Sleep(3);
#else
            struct timespec ts = { 0, 3000000 };
            nanosleep(&ts, NULL);
#endif
        }
        sv_rw_wr(&g_db_rw);
        g_lock_depth = 1;
        if (s) {
            while (g_active) {                      // un'altra sessione è dentro: si aspetta che esca
                g_lock_depth = 0;
                sv_rw_wrunlock(&g_db_rw);
                if (g_wait_hook) g_wait_hook(g_wait_ud);
                else {
#ifdef _WIN32
                    Sleep(1);
#else
                    struct timespec ts1 = { 0, 1000000 };
                    nanosleep(&ts1, NULL);
#endif
                }
                sv_rw_wr(&g_db_rw);
                g_lock_depth = 1;
            }
            session_save(&g_default_session);
            session_load(s);
            g_active = s;
        }
    }
}

static bool db_rollback_impl(void);

// errore di lock per lo statement; un deadlock annulla la transazione (come MySQL)
static QueryResult* lock_error(int why) {
    if (why == 1) {
        if (g_db.exec_ctx.tx_id) db_rollback_impl();
        return result_error("Deadlock found when trying to get lock; try restarting transaction");
    }
    return result_error("Lock wait timeout exceeded; try restarting transaction (a table is in use by another session's open transaction)");
}

// a fine statement, senza transazione aperta, i lock si liberano subito
static void release_if_autocommit(void) {
    if (!g_db.exec_ctx.tx_id) lk_release_all(lock_owner());
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

    // Esegui il primo statement (multi-statement gestito da db_execute_multi), con i suoi lock
    QueryResult* result;
    if (parsed->count > 0) {
        LockSet* ls = malloc(sizeof(LockSet));
        if (!ls) result = result_error("Out of memory");
        else {
            stmt_locks(parsed->statements[0], tokens, ls, g_db.catalog->current_db);
            bool snap = stmt_lock_kind(parsed->statements[0]) == 1 && !locking_read(tokens);
            int why = acquire_locks(ls, snap);
            result = why == 1 || why == 2 ? lock_error(why) : exec_one(parsed->statements[0]);
            if (why == 3) snapshot_clear();
            release_if_autocommit();
            free(ls);
        }
    } else result = result_ok(0);
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
    LockSet* ls = malloc(sizeof(LockSet));
    for (int i = 0; i < parsed->count; i++) {
        // i lock di ogni statement dal testo intero (sovrastima: più tabelle, mai meno)
        int why = 0;
        if (ls) {
            stmt_locks(parsed->statements[i], tokens, ls, g_db.catalog->current_db);
            why = acquire_locks(ls, stmt_lock_kind(parsed->statements[i]) == 1 && !locking_read(tokens));
        }
        results[i] = !ls ? result_error("Out of memory") : why == 1 || why == 2 ? lock_error(why) : exec_one(parsed->statements[i]);
        if (why == 3) snapshot_clear();
        release_if_autocommit();
        if (results[i])
            results[i]->exec_time_ms = get_time_ms() - t_start;
    }
    free(ls);
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
    g_open_tx++;
    wal_write(g_db.wal, LOG_BEGIN, tx->tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    return true;
}

static const void* lock_owner(void);

static bool db_commit_impl(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    wal_write(g_db.wal, LOG_COMMIT, g_db.exec_ctx.tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    wal_flush(g_db.wal); // DEVE essere su disco prima del commit
    txm_commit(g_db.txm, g_db.exec_ctx.tx_id);
    chg_drop(g_db.exec_ctx.tx_id, 0);
    g_db.exec_ctx.tx_id = 0;
    g_num_savepoints = 0;
    if (g_open_tx > 0) g_open_tx--;
    lk_release_all(lock_owner());          // fine della transazione: i lock delle tabelle si liberano
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
    chg_drop(g_db.exec_ctx.tx_id, 0);
    g_db.exec_ctx.tx_id = 0;
    g_num_savepoints = 0;
    if (g_open_tx > 0) g_open_tx--;
    executor_invalidate_indexes();   // le righe sono tornate indietro (e possono aver cambiato posizione)
    lk_release_all(lock_owner());
    return true;
}

// Simple savepoint tracking: store name and file offset
static SavepointRec g_savepoints[64];
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
    g_savepoints[g_num_savepoints].chg_seq = g_chg_seq;
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
    chg_drop(g_db.exec_ctx.tx_id, g_savepoints[sp_idx].chg_seq);
    wal_undo_tx_from(g_db.log_path, g_db.db_path, g_db.exec_ctx.tx_id, (long)sp_off);

    // il log non si accorcia (dopo il savepoint ci sono anche le voci delle altre transazioni): una
    // voce ROLLBACK_TO dice a ROLLBACK e al recupero che quelle voci sono già state annullate
    int64_t sp64 = (int64_t)sp_off;
    wal_write(g_db.wal, LOG_ROLLBACK_TO, g_db.exec_ctx.tx_id, 0, (SVRID){0,0}, (const uint8_t*)&sp64, 8, NULL, 0);
    wal_flush(g_db.wal);
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
    free_list_save();
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



// g_default_session: stato salvato della sessione predefinita; g_active: sessione attiva (NULL = predefinita)

// proprietario dei lock delle tabelle: la sessione attiva (o quella predefinita)
static const void* lock_owner(void) { return g_active ? (const void*)g_active : (const void*)&g_default_session; }

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

DbSession* db_session_new_temp(void) {
    DbSession* s = calloc(1, sizeof(DbSession));
    if (!s) return NULL;
    db_lock();
    s->cur_db = g_active ? g_default_session.cur_db : (g_db.catalog ? g_db.catalog->current_db : -1);
    db_unlock();
    return s;
}

bool db_session_enter(DbSession* s) {
    db_lock();
    // più sessioni possono avere una transazione aperta: le tabelle le proteggono i lock (locks.h)
    bool busy = g_active && g_active != s;
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
    lk_release_all(s);                          // anche i lock di letture senza transazione
    free(s);
}

bool db_session_in_transaction(const DbSession* s) { return s && s->tx_id != 0; }

bool db_session_blocked(void) { return false; }   // le transazioni non si escludono più a vicenda

// ── letture in parallelo ────────────────────────────────────────

QueryResult* db_execute_readonly(const char* sql, const char* dbname, const char* user, int timeout_ms, bool* refused) {
    *refused = false;
    if (!g_db.initialized || !sql) { *refused = true; return NULL; }
    double t0 = get_time_ms();
    sv_rw_rd(&g_db_rw);
    // una transazione aperta (di una sessione o della GUI): niente letture accanto ai suoi dati non
    // confermati; e un thread che ha già il lock esclusivo non può prenderlo anche in lettura
    if (g_lock_depth) { sv_rw_rdunlock(&g_db_rw); *refused = true; return NULL; }
    TokenList* tokens = lexer_tokenize(sql);
    ParseResult* parsed = tokens ? parser_parse(tokens) : NULL;
    QueryResult* r = NULL;
    // database della richiesta, altrimenti quello della sessione predefinita (non di una sessione attiva)
    int idx = g_active ? g_default_session.cur_db : g_db.catalog->current_db;
    if (dbname && dbname[0]) {
        idx = -1;
        for (int i = 0; i < g_db.catalog->num_databases; i++) if (!strcasecmp(g_db.catalog->databases[i].name, dbname)) idx = i;
    }
    // solo una SELECT, in un database esistente, con un utente che esiste; il resto va per la via normale
    bool ok = parsed && parsed->success && parsed->count == 1 && parsed->statements[0]->type == NODE_SELECT &&
              (!dbname || !dbname[0] || idx >= 0) && (!user || !user[0] || auth_user_exists(user));
    char err[300];
    bool infos = false;
    if (ok && parsed->statements[0]->qual_db[0]) {           // shop.prodotto: il database dello statement
        int q = qual_db_index(parsed->statements[0], idx, err, sizeof(err));
        if (q == QUAL_INFOSCHEMA) infos = true;
        else if (q < 0) ok = false;                          // l'errore lo dà la via normale
        else idx = q;
    }
    // le tabelle lette non devono essere in scrittura in una transazione aperta (niente letture sporche):
    // lock S presi senza aspettare, altrimenti la via normale (che aspetta)
    static _Thread_local char ro_owner;
    LockSet* rls = NULL;
    if (ok) {
        rls = malloc(sizeof(LockSet));
        if (rls) stmt_locks(parsed->statements[0], tokens, rls, idx);   // nomi non qualificati: il database della richiesta
        if (!rls || !lk_try(&ro_owner, rls->r, rls->n)) { ok = false; *refused = true; }
    }
    if (ok) {
        const char* dn = idx >= 0 ? g_db.catalog->databases[idx].name : "";
        if (infos) {
            executor_set_infoschema(true);
            snprintf(g_see_user, sizeof(g_see_user), "%s", user ? user : "");
            executor_set_db_filter(user && user[0] ? see_db_of_session : NULL);
        }
        if (!infos && user && user[0] && !auth_check(user, parsed->statements[0], dn, err, sizeof(err))) r = result_error(err);
        else {
            catalog_set_thread_db(idx);
            sv_scan_set_deadline(timeout_ms > 0 ? t0 + timeout_ms : 0);
            ExecCtx ctx = g_db.exec_ctx;              // copia: nessuna transazione, niente stato condiviso
            ctx.tx_id = 0;
            r = executor_run(&ctx, parsed->statements[0]);
            bool late = timeout_ms > 0 && get_time_ms() > t0 + timeout_ms && (!r || !r->success);
            sv_scan_set_deadline(0);
            catalog_set_thread_db(-2);
            executor_set_infoschema(false);
            executor_set_db_filter(NULL);
            g_see_user[0] = '\0';
            if (late || !r) {
                if (r) db_result_free(r);
                r = result_error(late ? "Query cancelled: time limit reached" : "Out of memory");
            }
        }
        if (r) r->exec_time_ms = get_time_ms() - t0;
    } else *refused = true;
    if (rls) { lk_release_all(&ro_owner); free(rls); }
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

int db_login_native(const char* user, const uint8_t scramble[20], const uint8_t* resp, size_t rlen) {
    db_lock();
    int r = user && user[0] ? auth_login_native(user, scramble, resp, rlen) : 0;
    db_unlock();
    return r;
}

bool db_user_can_see(const char* user, const char* db, const char* table) {
    if (!user || !user[0]) return true;
    db_lock();
    bool r = table ? auth_privs(user, db, table) != 0 : auth_can_see_db(user, db);
    db_unlock();
    return r;
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

// viste, trigger e procedure del database come istruzioni SQL (per il dump)
static void sb_cat(char** p, size_t* n, size_t* cap, const char* a) {
    size_t l = strlen(a);
    if (!*p) return;
    if (*n + l + 1 > *cap) {
        size_t nc = (*n + l + 1) * 2;
        char* q = realloc(*p, nc);
        if (!q) { free(*p); *p = NULL; return; }
        *p = q; *cap = nc;
    }
    memcpy(*p + *n, a, l + 1);
    *n += l;
}

// il testo SQL contiene la parola name (non dentro un'altra parola; senza distinzione maiuscole/minuscole)?
static bool sql_mentions(const char* sql, const char* name) {
    size_t nl = strlen(name);
    for (const char* p = sql; nl && *p; p++) {
        if (strncasecmp(p, name, nl)) continue;
        bool before = p > sql && (isalnum((unsigned char)p[-1]) || p[-1] == '_');
        bool after = isalnum((unsigned char)p[nl]) || p[nl] == '_';
        if (!before && !after) return true;
    }
    return false;
}

static char* db_get_objects_sql_impl(const char* db_name, int what) {
    Database* db = g_db.initialized ? catalog_get_db(g_db.catalog, db_name) : NULL;
    if (!db) return NULL;
    size_t n = 0, cap = 4096;
    char* out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    char head[400];
    // viste in ordine di dipendenza: una vista va dopo quelle che nomina (ciascuna viene controllata
    // quando si ricrea); con un ciclo apparente si prosegue nell'ordine del catalogo
    bool* done = what == 0 ? calloc((size_t)db->num_views + 1, 1) : NULL;
    for (int left = what == 0 ? db->num_views : 0; left > 0; ) {
        int pick = -1;
        for (int i = 0; i < db->num_views && pick < 0; i++) {
            if (done[i]) continue;
            bool ready = true;
            for (int j = 0; j < db->num_views && ready; j++)
                if (j != i && !done[j] && sql_mentions(db->views[i].definition, db->views[j].name)) ready = false;
            if (ready) pick = i;
        }
        for (int i = 0; i < db->num_views && pick < 0; i++) if (!done[i]) pick = i;
        done[pick] = true;
        left--;
        snprintf(head, sizeof(head), "CREATE VIEW %.64s AS ", db->views[pick].name);
        sb_cat(&out, &n, &cap, head);
        sb_cat(&out, &n, &cap, db->views[pick].definition);
        sb_cat(&out, &n, &cap, ";\n");
    }
    free(done);
    for (int i = 0; what == 1 && i < db->num_triggers; i++) {
        TriggerSchema* t = &db->triggers[i];
        snprintf(head, sizeof(head), "CREATE TRIGGER %.64s %s %.15s ON %.64s ", t->name, t->before ? "BEFORE" : "AFTER", t->event, t->table_name);
        sb_cat(&out, &n, &cap, head);
        sb_cat(&out, &n, &cap, t->body);
        sb_cat(&out, &n, &cap, ";\n");
    }
    for (int i = 0; what == 2 && i < db->num_routines; i++) {
        if (db->routines[i].is_function) continue;
        snprintf(head, sizeof(head), "CREATE PROCEDURE %.64s", db->routines[i].name);
        sb_cat(&out, &n, &cap, head);
        sb_cat(&out, &n, &cap, db->routines[i].body);
        sb_cat(&out, &n, &cap, ";\n");
    }
    return out;
}

char* db_get_objects_sql(const char* db_name, int what) {
    db_lock();
    char* r = db_get_objects_sql_impl(db_name, what);
    db_unlock();
    return r;
}


// ── BACKUP / ripristino ─────────────────────────────────────────
// Un backup è una cartella di dati completa (si apre direttamente con segvault o segvault-server):
// dopo un checkpoint si copiano il file delle pagine, il catalogo, la lista delle pagine libere e
// gli utenti; il log resta vuoto. Il lock esclusivo del motore ferma tutto durante la copia.
static const char* const BK_FILES[4] = { "segvault.db", "segvault.cat", "segvault.free", "users.dat" };

static bool bk_copy(const char* from, const char* to, bool must_exist, char* err, size_t errn) {
    int in = sv_open(from, SV_O_RDONLY, 0);
    if (in < 0) {
        if (!must_exist) { remove(to); return true; }
        snprintf(err, errn, "cannot read %.400s", from);
        return false;
    }
    int out = sv_open(to, SV_O_WRONLY | SV_O_CREAT | SV_O_TRUNC, 0644);
    if (out < 0) { sv_close(in); snprintf(err, errn, "cannot write %.400s", to); return false; }
    char* buf = malloc(1 << 20);
    bool ok = buf != NULL;
    while (ok) {
        int n = (int)sv_read(in, buf, 1u << 20);
        if (n < 0) ok = false;
        if (n <= 0) break;
        ok = sv_write_full(out, buf, (size_t)n);
    }
    if (ok) ok = sv_fsync(out) == 0;
    free(buf);
    sv_close(in);
    if (sv_close(out) != 0) ok = false;
    if (!ok) snprintf(err, errn, "copy to %.400s failed", to);
    return ok;
}

static bool bk_has_data(const char* dir) {
    char p[700];
    snprintf(p, sizeof(p), "%.600s" SV_PATH_SEP "segvault.db", dir);
    int fd = sv_open(p, SV_O_RDONLY, 0);
    if (fd < 0) return false;
    sv_close(fd);
    return true;
}

static bool bk_copy_dir(const char* from, const char* to, char* err, size_t errn) {
    for (int i = 0; i < 4; i++) {
        char a[700], b[700];
        snprintf(a, sizeof(a), "%.600s" SV_PATH_SEP "%s", from, BK_FILES[i]);
        snprintf(b, sizeof(b), "%.600s" SV_PATH_SEP "%s", to, BK_FILES[i]);
        if (!bk_copy(a, b, i < 2, err, errn)) return false;
    }
    char w[700];
    snprintf(w, sizeof(w), "%.600s" SV_PATH_SEP "segvault.wal", to);
    remove(w);                                            // log vuoto: niente da rifare
    return true;
}

static bool db_backup_impl(const char* dir, char* err, size_t errn) {
    if (!g_db.initialized) { snprintf(err, errn, "Database not initialized"); return false; }
    if (!dir || !dir[0]) { snprintf(err, errn, "Missing backup folder"); return false; }
    if (g_open_tx > 0) { snprintf(err, errn, "BACKUP is not allowed while a transaction is open"); return false; }
    if (!strcmp(dir, g_db.data_dir)) { snprintf(err, errn, "The backup folder must not be the data folder"); return false; }
    char part[600];                                      // crea anche le cartelle intermedie
    snprintf(part, sizeof(part), "%s", dir);
    for (char* q = part + 1; *q; q++)
        if (*q == '/' || *q == '\\') { char c = *q; *q = '\0'; sv_mkdir(part); *q = c; }
    sv_mkdir(dir);                                        // può esistere già
    if (bk_has_data(dir)) { snprintf(err, errn, "%.300s already contains a database: choose an empty folder", dir); return false; }
    if (!do_checkpoint()) { snprintf(err, errn, "Checkpoint failed"); return false; }
    free_list_save();
    sv_fsync(g_db.db_fd);
    return bk_copy_dir(g_db.data_dir, dir, err, errn);
}

static bool db_init_impl(const char* data_directory);

static bool db_restore_impl(const char* dir, char* err, size_t errn) {
    if (!g_db.initialized) { snprintf(err, errn, "Database not initialized"); return false; }
    if (g_open_tx > 0) { snprintf(err, errn, "Restore is not allowed while a transaction is open"); return false; }
    if (!dir || !bk_has_data(dir)) { snprintf(err, errn, "%.300s is not a SegVault backup (segvault.db missing)", dir ? dir : ""); return false; }
    if (!strcmp(dir, g_db.data_dir)) { snprintf(err, errn, "That is the data folder itself"); return false; }
    // i dati attuali restano in <dati>/before-restore: un ripristino sbagliato si può annullare
    char keep[600];
    snprintf(keep, sizeof(keep), "%.560s" SV_PATH_SEP "before-restore", g_db.data_dir);
    sv_mkdir(keep);
    char dd[512];
    snprintf(dd, sizeof(dd), "%s", g_db.data_dir);
    if (!do_checkpoint()) { snprintf(err, errn, "Checkpoint failed"); return false; }
    free_list_save();
    sv_fsync(g_db.db_fd);
    if (!bk_copy_dir(dd, keep, err, errn)) return false;
    db_shutdown_impl();
    bool ok = bk_copy_dir(dir, dd, err, errn);
    if (!ok) {                                            // rimette i dati di prima
        char e2[300];
        bk_copy_dir(keep, dd, e2, sizeof(e2));
    }
    if (!db_init_impl(dd)) { snprintf(err, errn, "Restored files cannot be opened: %.200s (previous data in %.200s)", g_init_error, keep); return false; }
    return ok;
}

bool db_backup(const char* dir, char* err, size_t errn) {
    db_lock();
    bool r = db_backup_impl(dir, err, errn);
    db_unlock();
    return r;
}

bool db_restore(const char* dir, char* err, size_t errn) {
    db_lock();
    bool r = db_restore_impl(dir, err, errn);
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

void db_set_mysql_mode(bool on) { executor_set_mysql_mode(on); }
