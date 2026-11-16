/* 
SCOPO: Implementazione del bridge GUI <-> DB engine.
       Mantiene lo stato globale del DB (catalog, buffer pool,
       WAL, transaction manager) e li espone alla GUI
       tramite l'API definita in db_api.h.
*/
#include "db_api.h"
#include "../table/binesc.h"
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
#include <errno.h>

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
#include "../storage/page_migrate.h"
#include "../query/stats.h"
#include "sv_alloc.h"
static SvRwLock g_db_rw;
static _Thread_local int g_lock_depth;
static void chg_hook(uint64_t tx, const char* db, const char* table, uint8_t type, RID rid, RID nrid, const uint8_t* old, size_t ol);
static void on_chain_abandoned(uint32_t first, uint32_t total);
static void on_table_dropped(uint32_t root);
static void pf_stmt_end(void);
static void pf_process(void);
static void pf_ddl_saved(void);
static void db_mutex_init(void) {
    sv_rw_init(&g_db_rw);
    lk_init();
    executor_set_change_hook(chg_hook);
    heap_set_abandon_hook(on_chain_abandoned);     // spazio liberato subito (P1.5)
    executor_set_drop_hook(on_table_dropped);
}
static void db_lock(void)       { if (g_lock_depth++ == 0) sv_rw_wr(&g_db_rw); }
static void db_unlock(void)     { if (--g_lock_depth == 0) sv_rw_wrunlock(&g_db_rw); }

static char g_init_error[400];
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

// regola del WAL + doppia scrittura: prima di scrivere pagine modificate nel file dei dati, le loro immagini nel
// log e il log su disco (un fsync per gruppo). Una scrittura interrotta a metà di una pagina (CRC sbagliato al
// riavvio) si ripara dall'immagine
static bool wal_before_page_write(void* w, Page* const* pages, int n) {
    for (int i = 0; i < n; i++) if (wal_log_page_image((WAL*)w, pages[i]) != SV_OK) return false;
    return wal_flush((WAL*)w) == SV_OK;
}
static void wal_on_link(void* w, uint32_t prev, uint32_t page) {
    if (wal_write((WAL*)w, LOG_LINK, 0, page, (SVRID){ page, 0 }, (const uint8_t*)&prev, 4, NULL, 0) != SV_OK) return;
    bp_note_lsn(g_db.bp, page, ((WAL*)w)->last_lsn);
    if (prev) bp_note_lsn(g_db.bp, prev, ((WAL*)w)->last_lsn);
}

// ── errore di I/O: il motore si ferma ───────────────────────────
// Dopo una scrittura o un fsync non riuscito sul log o sul file dei dati non c'è più modo di sapere
// che cosa è su disco: ogni statement viene rifiutato con lo stesso messaggio finché non si riavvia
// (il recupero all'avvio rimette il database nell'ultimo stato confermato).
static char g_io_msg[300];

// ── durabilità delle conferme (SET synchronous = OFF | NORMAL | FULL) ──
//  FULL:   ogni scrittura confermata (anche in autocommit) è su disco prima della risposta.
//  NORMAL: COMMIT esplicito su disco subito; le scritture in autocommit al più entro SYNC_NORMAL_MS (al
//          prossimo statement o a db_sync_tick): un'interruzione di corrente può perdere quegli ultimi ms.
//  OFF:    nessun fsync del log (solo checkpoint e arresto): sopravvive al crash del processo, non alla corrente.
#define SYNC_NORMAL_MS 100
static int    g_sync_mode = 1;
static double g_last_sync;

static const char* const SYNC_NAMES[] = { "OFF", "NORMAL", "FULL" };

// ── cache delle pagine: 1/8 della RAM, fra 4 MB e 512 MB (prima: 4 MB fissi, ogni lavoro più grande
// rileggeva continuamente il disco). --cache-size / SET GLOBAL cache_size = MB ──
static long g_cache_mb;                     // 0 = automatica
#ifdef _WIN32
typedef struct { unsigned long len, load; unsigned long long total, avail, tpf, apf, tv, av, aev; } SvMemStatus;
__declspec(dllimport) int __stdcall GlobalMemoryStatusEx(SvMemStatus*);
#endif
static long auto_cache_mb(void) {
    unsigned long long ram = 0;
#ifdef _WIN32
    SvMemStatus ms;
    memset(&ms, 0, sizeof(ms));
    ms.len = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) ram = ms.total;
#else
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) ram = (unsigned long long)pages * (unsigned long long)psz;
#endif
    long mb = ram ? (long)(ram / 8 / (1024 * 1024)) : 4;
    return mb < 4 ? 4 : mb > 512 ? 512 : mb;
}
static int cache_frames(long mb) { long f = mb * 1024 * 1024 / SV_PAGE_SIZE; return f < 16 ? 16 : f > (1 << 20) ? (1 << 20) : (int)f; }
static uint64_t g_engine_gen;       // aumenta a ogni db_init: le transazioni di prima sono finite

static bool io_failed(void) {
    return g_db.initialized && ((g_db.wal && g_db.wal->failed) || (g_db.bp && g_db.bp->io_error));
}

static const char* io_failed_msg(void) {
    if (!g_io_msg[0])
        snprintf(g_io_msg, sizeof(g_io_msg), "SegVault stopped writing after an I/O error on the %s (disk full or failing?): "
                 "fix the disk and restart; the last committed state is recovered at startup",
                 g_db.wal && g_db.wal->failed ? "transaction log" : "data file");
    return g_io_msg;
}

static bool db_init_impl(const char* data_directory) {
    if (g_db.initialized) return true;

    strncpy(g_db.data_dir, data_directory, sizeof(g_db.data_dir)-1);
    snprintf(g_db.db_path,  sizeof(g_db.db_path),  "%s" SV_PATH_SEP "segvault.db",  data_directory);
    snprintf(g_db.cat_path, sizeof(g_db.cat_path), "%s" SV_PATH_SEP "segvault.cat", data_directory);
    snprintf(g_db.log_path, sizeof(g_db.log_path), "%s" SV_PATH_SEP "segvault.wal", data_directory);

    // Apri il file del database e bloccalo: un'altra istanza sulla stessa cartella lo corromperebbe
    g_db.db_fd = sv_open(g_db.db_path, SV_O_RDWR | SV_O_CREAT, 0600);
#ifndef _WIN32
    if (g_db.db_fd >= 0) fchmod(g_db.db_fd, 0600);       // anche un file creato da una versione precedente (0644)
#else
    // la cartella dei dati e i file già presenti: solo l'utente corrente (e SYSTEM); i file nuovi ereditano
    sv_win_private(data_directory, 1);
    {
        char up[1100];
        snprintf(up, sizeof(up), "%s" SV_PATH_SEP "users.dat", data_directory);
        const char* files[] = { g_db.db_path, g_db.cat_path, g_db.log_path, up };
        for (int i = 0; i < 4; i++) if (_access(files[i], 0) == 0) sv_win_private(files[i], 0);
    }
#endif
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

    // formato del file dei dati: un file della 1.3 (formato 1) diventa formato 2 (LSN e CRC in ogni pagina);
    // una conversione interrotta riprende dalla copia segvault.db.v1
    {
        char merr[300] = "";
        int lf = sv_open(g_db.log_path, SV_O_RDONLY, 0);
        bool log_present = lf >= 0 && sv_lseek(lf, 0, SEEK_END) > 0;
        if (lf >= 0) sv_close(lf);
        if (page_migrate(g_db.db_fd, g_db.db_path, log_present, merr, sizeof(merr)) != SV_OK) {
            snprintf(g_init_error, sizeof(g_init_error), "cannot upgrade the data file: %.300s", merr[0] ? merr : "I/O error");
            sv_close(g_db.db_fd);
            return false;
        }
    }

    // recovery dopo un arresto anomalo (con il lock già preso); se non riesce il motore non parte:
    // il log resta com'è e nessuna scrittura nuova può peggiorare le cose
    {
        char rerr[400] = "";
        if (wal_recovery(g_db.log_path, g_db.db_path, g_db.cat_path, rerr, sizeof(rerr)) != SV_OK) {
            snprintf(g_init_error, sizeof(g_init_error), "crash recovery failed: %.300s", rerr[0] ? rerr : "I/O error");
            sv_close(g_db.db_fd);
            return false;
        }
        // il log nuovo riparte oltre ogni LSN già scritto nelle pagine (intestazione del file, fine del log recuperato)
        uint64_t hl = 1;
        page_header_read(g_db.db_fd, &hl);
        int64_t start = (int64_t)hl > wal_recovery_end_lsn() ? (int64_t)hl : wal_recovery_end_lsn();
        wal_set_initial_lsn(start > 0 ? start : 1);
    }

    // Inizializza i componenti
    g_db.catalog = catalog_load(g_db.cat_path);
    {                                                     // sequenze (CREATE SEQUENCE)
        char sp[600];
        snprintf(sp, sizeof(sp), "%s" SV_PATH_SEP "sequences.txt", data_directory);
        executor_set_seq_file(sp);
    }
    g_db.bp      = bp_create(cache_frames(g_cache_mb > 0 ? g_cache_mb : auto_cache_mb()), g_db.db_fd);
    g_db.txm     = txm_create();
    g_db.wal     = wal_create(g_db.log_path);

    if (!g_db.catalog || !g_db.bp || !g_db.txm || !g_db.wal) {
        snprintf(g_init_error, sizeof(g_init_error), "%s", !g_db.catalog ? (catalog_load_error()[0] ? catalog_load_error() : "cannot load the catalog")
                 : !g_db.wal ? "cannot open the transaction log" : "out of memory");
        if (g_db.bp) { bp_destroy(g_db.bp); g_db.bp = NULL; }
        if (g_db.wal) { wal_destroy(g_db.wal); g_db.wal = NULL; }
        free(g_db.txm); g_db.txm = NULL;
        free(g_db.catalog); g_db.catalog = NULL;
        sv_close(g_db.db_fd);
        return false;
    }
    g_db.bp->before_write = wal_before_page_write;    // prima di scrivere una pagina, il log
    g_db.bp->before_write_ctx = g_db.wal;
    g_db.bp->on_link = wal_on_link;
    g_db.bp->on_link_ctx = g_db.wal;
    g_io_msg[0] = '\0';

    g_db.exec_ctx.catalog = g_db.catalog;
    g_db.exec_ctx.bp      = g_db.bp;
    g_db.exec_ctx.wal     = g_db.wal;
    g_db.exec_ctx.tx_id   = 0;
    g_engine_gen++;
    {                                           // statistiche dell'ottimizzatore (ANALYZE): danneggiate = nessuna
        char sp[1100];
        snprintf(sp, sizeof(sp), "%s" SV_PATH_SEP "segvault.stats", data_directory);
        stats_load(sp);
    }

    g_db.initialized = true;
    if (!auth_init(data_directory)) {          // utenti illeggibili: mai "nessun utente" (= tutti root)
        snprintf(g_init_error, sizeof(g_init_error), "%s", auth_init_error());
        g_db.initialized = false;
        bp_destroy(g_db.bp); g_db.bp = NULL;
        wal_destroy(g_db.wal); g_db.wal = NULL;
        txm_destroy(g_db.txm); g_db.txm = NULL;
        catalog_free(g_db.catalog); g_db.catalog = NULL;
        sv_close(g_db.db_fd);
        return false;
    }
    free_list_load();
    return true;
}

// ── annullamento ─────────────────────────────────────────────────

static atomic_int g_cancel_requested = 0;
static atomic_int g_read_running = 0;   // uno statement è in esecuzione: si può interrompere

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

static int64_t g_ckpt_kept;      // byte rimasti nel log dopo l'ultimo checkpoint (record delle transazioni aperte)

// Checkpoint: pagine e catalogo su disco, poi il log si accorcia. Senza transazioni aperte si svuota; con
// transazioni aperte (P1.1) resta solo ciò che serve per annullarle (i loro record), così una transazione
// dimenticata aperta non fa più crescere il log all'infinito né blocca il checkpoint
static bool do_checkpoint(void) {
    if (!g_db.initialized || io_failed()) return false;
    if (wal_flush(g_db.wal) != SV_OK || bp_flush_all(g_db.bp) != SV_OK) return false;
    if (catalog_save(g_db.catalog, g_db.cat_path) != SV_OK) return false;
    bp_checkpoint_done(g_db.bp);          // prima di accorciare il log: da qui le pagine vecchie vogliono l'immagine
    uint64_t act[SV_MAX_TRANSACTIONS];
    int n = 0;
    for (int i = 0; i < g_db.txm->num_transactions; i++)
        if (g_db.txm->transactions[i].active) act[n++] = g_db.txm->transactions[i].tx_id;
    bool ok = wal_checkpoint_keep(g_db.wal, act, n) == SV_OK;
    if (ok) g_ckpt_kept = wal_bytes(g_db.wal);
    if (ok) page_header_write(g_db.db_fd, (uint64_t)wal_size(g_db.wal));   // da dove ripartirebbe un log nuovo
    return ok;
}

// ── VACUUM: pagine di overflow non più usate -> riusabili ───────

static void free_list_path(char* out, size_t n) { snprintf(out, n, "%s" SV_PATH_SEP "segvault.free", g_db.data_dir); }

static void free_list_save(void) {
    int n = bp_free_count(g_db.bp);
    uint32_t* ids = sv_malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
    if (!ids) return;
    n = bp_free_list(g_db.bp, ids, n);
    char p[600];
    free_list_path(p, sizeof(p));
    FILE* f = sv_fopen_private(p, "w");
    if (f) { for (int i = 0; i < n; i++) fprintf(f, "%u\n", ids[i]); fclose(f); }
    free(ids);
}

static void free_list_load(void) {
    char p[600];
    free_list_path(p, sizeof(p));
    FILE* f = fopen(p, "r" SV_FE);
    if (!f) return;
    unsigned id;
    while (fscanf(f, "%u", &id) == 1) if (id) bp_add_free(g_db.bp, id);   // validi solo se su disco sono FREE
    fclose(f);
}

static bool do_checkpoint(void);
static bool do_vacuum(int* freed);
static void free_heap(uint32_t root, uint64_t lsn);

// VACUUM FULL (P1.5): ogni tabella riscritta in pagine compatte (prima le pagine libere più basse, due giri: il
// secondo riempie i buchi lasciati dal primo), le pagine vecchie liberate, le libere in fondo al file tolte. Ritorna
// in *freed le pagine tolte dal file. Ogni tabella è un passo a sé: catalogo salvato con la radice nuova, poi le
// pagine vecchie libere (un arresto a metà lascia tabelle già compattate e altre com'erano)
static bool do_vacuum_full(int* freed) {
    int f0 = 0;
    if (!do_vacuum(&f0)) return false;
    for (int pass = 0; pass < 2; pass++) {
        bp_free_lowest_first(g_db.bp);
        for (int d = 0; d < g_db.catalog->num_databases; d++) {
            Database* db = &g_db.catalog->databases[d];
            int prev = g_db.catalog->current_db;
            g_db.catalog->current_db = d;
            for (int t = 0; t < db->num_tables; t++) {
                TableSchema* ts = db->tables[t];
                uint32_t old_root = ts->root_page_id, root = 0, pages = 0;
                if (!old_root) continue;
                const char* fail = executor_rebuild_table(&g_db.exec_ctx, ts, &root, &pages);
                if (fail) { g_db.catalog->current_db = prev; fprintf(stderr, "[SegVault] VACUUM FULL: %s (table %s)\n", fail, ts->name); return false; }
                ts->root_page_id = root;
                ts->num_pages = pages;
                if (bp_flush_all(g_db.bp) != SV_OK || catalog_save(g_db.catalog, g_db.cat_path) != SV_OK) {
                    g_db.catalog->current_db = prev;
                    return false;
                }
                executor_invalidate_table(g_db.bp, db->name, ts->name, old_root);
                free_heap(old_root, (uint64_t)wal_size(g_db.wal));
                bp_free_lowest_first(g_db.bp);
            }
            g_db.catalog->current_db = prev;
        }
    }
    if (bp_flush_all(g_db.bp) != SV_OK) return false;
    int cut = bp_truncate_free_tail(g_db.bp);
    if (cut < 0) return false;
    free_list_save();
    *freed = cut;
    return do_checkpoint();
}

static bool do_vacuum(int* freed) {
    *freed = 0;
    if (!do_checkpoint()) return false;                 // log vuoto: nessun record nomina le pagine
    int64_t end = sv_lseek(g_db.db_fd, 0, SEEK_END);
    uint32_t npages = end > 0 ? (uint32_t)(end / SV_PAGE_SIZE) : 0;
    heap_damage_clear();
    uint8_t* used = sv_calloc((size_t)npages / 8 + 1, 1);
    uint8_t* heapu = sv_calloc((size_t)npages / 8 + 1, 1);       // pagine raggiungibili dalla catena di una tabella
    if (!used || !heapu) { free(used); free(heapu); return false; }
    heap_pages_bitmap = heapu;
    for (int d = 0; d < g_db.catalog->num_databases; d++) {
        Database* db = &g_db.catalog->databases[d];
        for (int t = 0; t < db->num_tables; t++) {
            HeapFile* hf = heap_open(g_db.bp, db->tables[t]->root_page_id, db->tables[t]->num_pages);
            if (hf) heap_mark_overflow(hf, used, npages);
            free(hf);
        }
    }
    heap_pages_bitmap = NULL;
    // una catena interrotta (pagina danneggiata) farebbe sembrare libere le pagine dopo: niente VACUUM
    if (heap_damage(NULL, 0)) {
        heap_damage_clear();
        free(used);
        free(heapu);
        fprintf(stderr, "[SegVault] VACUUM skipped: a table has a damaged page (run CHECK TABLE)\n");
        return false;
    }
    // l'elenco si ricostruisce da capo: tutte le pagine libere (anche quelle di VACUUM precedenti)
    g_db.bp->nfree = 0;
    for (uint32_t id = 1; id < npages; id++) {
        Page* p = bp_fetch(g_db.bp, id);
        if (!p) continue;
        bool overflow_unused = (p->flags & PAGE_FLAG_OVERFLOW) && !(used[id / 8] & (1u << (id % 8)));
        // pagina di heap che nessuna tabella raggiunge più (tabella eliminata o troncata): anche lei riusabile
        bool heap_unused = !(p->flags & (PAGE_FLAG_OVERFLOW | PAGE_FLAG_FREE)) && (p->flags & PAGE_FLAG_CHAINED) &&
                           !(heapu[id / 8] & (1u << (id % 8)));
        overflow_unused = overflow_unused || heap_unused;
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
    free(heapu);
    atomic_store(&g_sv_abandoned_pages, 0);
    bp_flush_all(g_db.bp);
    sv_fsync(g_db.db_fd);
    free_list_save();
    return true;
}

// VACUUM automatico: con più di 4 MB abbandonati (catene di righe grandi sostituite, tabelle eliminate) e
// nessuna transazione aperta. Prima lo spazio restava perso fino a un VACUUM a mano
#define SV_AUTO_VACUUM_PAGES 1024
static bool do_vacuum(int* freed);

static long long g_ckpt_bytes = SV_WAL_CHECKPOINT_BYTES;
void db_set_checkpoint_bytes(long long bytes) { g_ckpt_bytes = bytes > 0 ? bytes : SV_WAL_CHECKPOINT_BYTES; }

static void maybe_checkpoint(void) {
    if (!g_db.initialized) return;
    if (g_open_tx == 0 && atomic_load(&g_sv_abandoned_pages) > SV_AUTO_VACUUM_PAGES) { int fr = 0; do_vacuum(&fr); return; }
    // anche con transazioni aperte; ma se i loro record da soli sono tanti, non a ogni statement: solo quando
    // il log è almeno il doppio di quanto era rimasto dopo l'ultimo checkpoint
    int64_t b = wal_bytes(g_db.wal);
    if (b > g_ckpt_bytes && b > 2 * g_ckpt_kept) do_checkpoint();
}

static int g_num_savepoints;
static int64_t g_tx_start;      // transazione attiva: posizione del suo BEGIN nel log
static bool g_tx_ddl;           // ... e se ha cambiato lo schema (o eseguito procedure)
static bool g_chg_lost;         // una modifica non è stata registrata (memoria): ROLLBACK invalida tutto

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
                        (stmt->type == NODE_DELETE && stmt->delete_stmt.multi_from) ||
                        (dml && stmt->returning) ||          // RETURNING si calcola dopo la scrittura: se fallisce, niente righe
                        stmt->type == NODE_MERGE;            // MERGE: più statement interni
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
static void tx_end(bool committed);
static char g_tx_err[400];
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
        Row* nr = sv_realloc(m->r->rows, sizeof(Row) * (size_t)nc);
        if (!nr) return;
        m->r->rows = nr;
        m->cap = nc;
    }
    const char* v[4] = { table, op, type, text };
    if (db_row_set(&m->r->rows[m->r->num_rows], 4, v, NULL)) m->r->num_rows++;
}

// una query interna con un solo numero come risultato (-1 = errore, messaggio in err)
// query interna (permessi già controllati): il risultato, NULL + err se fallisce
static QueryResult* maint_query(const char* sql, char* err, size_t en) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    QueryResult* r = NULL;
    if (pr && pr->success && pr->count == 1) {
        g_db.exec_ctx.priv_off++;
        r = executor_run(&g_db.exec_ctx, pr->statements[0]);
        g_db.exec_ctx.priv_off--;
        if (r && !r->success) { snprintf(err, en, "%.250s", r->error); db_result_free(r); r = NULL; }
        else if (!r) snprintf(err, en, "out of memory");
    } else snprintf(err, en, "%.250s", pr ? pr->error : "out of memory");
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    return r;
}

// ANALYZE: statistiche delle colonne per l'ottimizzatore (P1.13; vedi src/query/stats.h)
static bool analyze_columns(const char* db, const TableSchema* ts, long long rows, char* err, size_t en) {
    TableStats t;
    memset(&t, 0, sizeof(t));
    snprintf(t.db, sizeof(t.db), "%s", db ? db : "");
    snprintf(t.table, sizeof(t.table), "%s", ts->name);
    t.rows = (double)rows;
    t.ncols = ts->num_columns;
    t.cols = sv_calloc((size_t)(ts->num_columns ? ts->num_columns : 1), sizeof(ColStats));
    bool ok = true;
    for (int c = 0; ok && c < ts->num_columns; c++) {
        const Column* cd = &ts->columns[c];
        ColStats* cs = &t.cols[c];
        snprintf(cs->name, sizeof(cs->name), "%s", cd->name);
        if (cd->type == SV_TYPE_BLOB || cd->type == SV_TYPE_TEXT) {   // testi lunghi: solo i NULL
            char q[400];
            snprintf(q, sizeof(q), "SELECT COUNT(*) FROM `%s` WHERE `%s` IS NULL", ts->name, cd->name);
            QueryResult* r = maint_query(q, err, en);
            if (!r) { ok = false; break; }
            double nulls = r->num_rows ? atof(r->rows[0].cells[0].value) : 0;
            db_result_free(r);
            cs->null_frac = rows > 0 ? nulls / (double)rows : 0;
            cs->ndv = rows > 0 ? (double)rows * (1 - cs->null_frac) : 0;
            continue;
        }
        char q[400];
        snprintf(q, sizeof(q), "SELECT COUNT(DISTINCT `%s`), COUNT(*) - COUNT(`%s`) FROM `%s`", cd->name, cd->name, ts->name);
        QueryResult* r = maint_query(q, err, en);
        if (!r) { ok = false; break; }
        if (r->num_rows) {
            cs->ndv = r->rows[0].cells[0].is_null ? 0 : atof(r->rows[0].cells[0].value);
            double nulls = r->rows[0].cells[1].is_null ? 0 : atof(r->rows[0].cells[1].value);
            cs->null_frac = rows > 0 ? nulls / (double)rows : 0;
        }
        db_result_free(r);
        bool num = cd->type == SV_TYPE_INT || cd->type == SV_TYPE_BIGINT || cd->type == SV_TYPE_FLOAT ||
                   cd->type == SV_TYPE_DECIMAL || cd->type == SV_TYPE_BOOL;
        if (!num || rows <= 0) continue;
        // istogramma equi-depth: i valori ordinati, presi alle posizioni k / STATS_BUCKETS
        snprintf(q, sizeof(q), "SELECT `%s` FROM `%s` WHERE `%s` IS NOT NULL ORDER BY `%s`", cd->name, ts->name, cd->name, cd->name);
        r = maint_query(q, err, en);
        if (!r) { ok = false; break; }
        if (r->num_rows > 0) {
            cs->numeric = true;
            for (int k = 0; k <= STATS_BUCKETS; k++) {
                long i = (long)((double)(r->num_rows - 1) * k / STATS_BUCKETS);
                cs->hist[k] = atof(r->rows[i].cells[0].value);
            }
            cs->min = cs->hist[0];
            cs->max = cs->hist[STATS_BUCKETS];
        }
        db_result_free(r);
    }
    if (ok) stats_put(&t);
    free(t.cols);
    return ok;
}

static long long maint_count(const char* sql, char* err, size_t en) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    long long v = -1;
    if (pr && pr->success && pr->count == 1) {
        g_db.exec_ctx.priv_off++;                         // query interna: i permessi li ha già controllati auth_check
        QueryResult* r = executor_run(&g_db.exec_ctx, pr->statements[0]);
        g_db.exec_ctx.priv_off--;
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
    MaintOut m = { sv_calloc(1, sizeof(QueryResult)), 0 };
    if (!m.r) return result_error("Out of memory");
    static const char* const NAMES[4] = { "Table", "Op", "Msg_type", "Msg_text" };
    m.r->success = true;
    m.r->num_cols = 4;
    m.r->col_names = sv_calloc(4, sizeof(char*));
    m.r->col_types = sv_calloc(4, sizeof(char*));
    for (int i = 0; m.r->col_names && m.r->col_types && i < 4; i++) { m.r->col_names[i] = sv_strdup(NAMES[i]); m.r->col_types[i] = sv_strdup("VARCHAR"); }
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
        else if (ts && !analyze_columns(dbname, ts, rows, err, sizeof(err))) {
            snprintf(msg, sizeof(msg), "column statistics: %.170s", err);
            maint_row(&m, tn, opl, "error", msg);
            continue;
        }
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
    if (!strcmp(op, "ANALYZE")) {
        char sp[1100];
        snprintf(sp, sizeof(sp), "%s" SV_PATH_SEP "segvault.stats", g_db.data_dir);
        stats_save(sp);
    }
    return m.r;
}

// istruzioni dentro una procedura: con i permessi dell'utente che la chiama
static bool proc_auth(const ASTNode* st, char* err, size_t errn) {
    return auth_check(g_session_user, st, cur_db_name(), err, errn);
}

// esegue uno statement già analizzato (transazioni e CHECKPOINT gestiti qui)
static QueryResult* exec_one_in(ASTNode* stmt) {
    QueryResult* result = NULL;
    // dopo un errore di I/O: nessuno statement (nemmeno le letture, che vedrebbero pagine non salvate);
    // ROLLBACK libera comunque la transazione in memoria
    if (io_failed()) {
        if ((stmt->type == NODE_ROLLBACK || stmt->type == NODE_COMMIT) && g_db.exec_ctx.tx_id) { snprintf(g_tx_err, sizeof(g_tx_err), "%s", io_failed_msg()); tx_end(false); }
        return result_error(io_failed_msg());
    }
    g_db.exec_ctx.auth_hook = g_session_user[0] ? proc_auth : NULL;
    // permessi sulle tabelle, controllati dall'executor dove le apre (con information_schema
    // g_session_user è vuoto: l'utente vero è in g_see_user)
    g_db.exec_ctx.user = g_session_user[0] ? g_session_user : g_see_user[0] ? g_see_user : NULL;
    g_db.exec_ctx.priv_off = 0;
    if (g_session_user[0]) {         // permessi dell'utente della sessione
        char err[300];
        if (!auth_check(g_session_user, stmt, cur_db_name(), err, sizeof(err))) return result_error(err);
        // shop.prodotto JOIN uni.esame: gli stessi permessi su ogni database nominato
        for (int i = 0; i < stmt->nqual_dbs; i++)
            if (!auth_check(g_session_user, stmt, stmt->qual_dbs[i], err, sizeof(err))) return result_error(err);
    }
    if (stmt->type == NODE_SET) {                 // variabili di sessione di MySQL: accettate
        // prima le assegnazioni (SET @OLD_FOREIGN_KEY_CHECKS = @@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS = 0
        // dei dump: @OLD deve avere il valore di prima), poi il flag
        result = stmt->set_n > 0 ? executor_set_session_vars(&g_db.exec_ctx, stmt) : result_ok(0);   // SET @x = ...
        if (stmt->set_cache_mb != -1 && result && result->success) {          // SET GLOBAL cache_size = MB
            const char* why = NULL;
            if (stmt->set_cache_mb < 1 || stmt->set_cache_mb > 65536) why = "cache_size is a number of megabytes (1 to 65536)";
            else if (g_session_user[0] && !auth_is_admin(g_session_user)) why = "Access denied: only administrators can change cache_size";
            else if (g_open_tx > 0) why = "cache_size cannot change while a transaction is open";
            else if (!bp_resize(g_db.bp, cache_frames(stmt->set_cache_mb))) why = "cannot resize the page cache now";
            else { g_cache_mb = stmt->set_cache_mb; executor_invalidate_indexes(); }
            if (why) { db_result_free(result); result = result_error(why); }
        }
        if (stmt->set_sync == -2 && result && result->success) {
            db_result_free(result);
            result = result_error("synchronous must be OFF, NORMAL or FULL");
        } else if (stmt->set_sync >= 0 && result && result->success) {
            if (g_session_user[0] && !auth_is_admin(g_session_user)) {   // vale per tutti: solo un amministratore
                db_result_free(result);
                result = result_error("Access denied: only administrators can change synchronous");
            } else g_sync_mode = stmt->set_sync;
        }
        if (stmt->set_fk_checks >= 0 && result && result->success) executor_set_fk_checks(stmt->set_fk_checks != 0);
    } else if (stmt->type == NODE_BEGIN) {
        result = db_begin() ? result_ok(0) : result_error("Failed to begin transaction");
    } else if (stmt->type == NODE_COMMIT) {
        // senza transazione aperta è un OK, come in MySQL (i dump finiscono spesso con COMMIT)
        result = !g_db.exec_ctx.tx_id || db_commit() ? result_ok(0) : result_error(g_tx_err[0] ? g_tx_err : "Failed to commit");
    } else if (stmt->type == NODE_ROLLBACK && stmt->rollback_to.name[0] == 0) {
        result = !g_db.exec_ctx.tx_id || db_rollback() ? result_ok(0) : result_error(g_tx_err[0] ? g_tx_err : "Failed to rollback");
    } else if (stmt->type == NODE_TABLE_MAINT) {
        result = exec_table_maint(stmt);
    } else if (stmt->type == NODE_BACKUP) {
        char err[600];
        result = db_backup_impl(stmt->backup.path, err, sizeof(err)) ? result_ok(0) : result_error(err);
    } else if (stmt->type == NODE_VACUUM) {
        int freed = 0;
        bool full = stmt->delete_stmt.num_targets == 1;
        result = g_open_tx > 0 ? result_error("VACUUM is not allowed while a transaction is open")
               : (full ? do_vacuum_full(&freed) : do_vacuum(&freed)) ? result_ok(freed) : result_error("VACUUM failed");
    } else if (stmt->type == NODE_CHECKPOINT) {
        result = do_checkpoint() ? result_ok(0) : result_error("Checkpoint failed");   // anche con transazioni aperte (P1.1)
    } else if (stmt->type == NODE_SAVEPOINT) {
        result = db_savepoint(stmt->savepoint.name) ? result_ok(0) : result_error("Failed to set savepoint");
    } else if (stmt->type == NODE_ROLLBACK_TO) {
        result = db_rollback_to(stmt->rollback_to.name) ? result_ok(0) : result_error(io_failed() ? io_failed_msg() : "Failed to rollback to savepoint");
    } else if (stmt->type == NODE_SELECT) {
        // sola lettura: interrompibile a metà
        atomic_store(&g_read_running, 1);
        if (atomic_load(&g_cancel_requested)) atomic_store(&g_sv_scan_cancel, 1);
        result = atomic_load(&g_sv_scan_cancel) ? NULL : executor_run(&g_db.exec_ctx, stmt);
        atomic_store(&g_read_running, 0);
        if (atomic_load(&g_sv_scan_cancel) || sv_scan_cancelled()) {   // risultato parziale: scartato
            if (result) db_result_free(result);
            result = result_error("Query cancelled");
            atomic_store(&g_sv_scan_cancel, 0);
        }
    } else {
        // interrompibile anche una scrittura (--timeout, KILL QUERY): si ferma nella fase di lettura, prima di
        // scrivere (la scansione annullata fa fallire lo statement)
        atomic_store(&g_read_running, 1);
        if (atomic_load(&g_cancel_requested)) atomic_store(&g_sv_scan_cancel, 1);
        result = exec_atomic(stmt);
        atomic_store(&g_read_running, 0);
    }
    // DDL (o procedure, che possono contenerne) dentro una transazione: il suo ROLLBACK invalida tutta la cache
    if (g_db.exec_ctx.tx_id && stmt->type != NODE_SELECT && stmt->type != NODE_INSERT && stmt->type != NODE_UPDATE &&
        stmt->type != NODE_DELETE && stmt->type != NODE_MERGE && stmt->type != NODE_SET && stmt->type != NODE_SHOW &&
        stmt->type != NODE_DESCRIBE && stmt->type != NODE_EXPLAIN && stmt->type != NODE_SAVEPOINT &&
        stmt->type != NODE_ROLLBACK_TO && stmt->type != NODE_BEGIN && stmt->type != NODE_USE) g_tx_ddl = true;
    // scrittura in autocommit: su disco secondo synchronous
    if (result && result->success && !g_db.exec_ctx.tx_id && g_db.wal && g_db.wal->unsynced && !io_failed() &&
        stmt->type != NODE_SELECT && stmt->type != NODE_SHOW && stmt->type != NODE_DESCRIBE && stmt->type != NODE_EXPLAIN) {
        double now = get_time_ms();
        if (g_sync_mode == 2 || (g_sync_mode == 1 && now - g_last_sync >= SYNC_NORMAL_MS)) {
            if (wal_flush(g_db.wal) == SV_OK) g_last_sync = now;
        } else if (g_sync_mode >= 1) wal_write_buffer(g_db.wal);
    }
    if (io_failed() && result && result->success) {   // lo statement ha incontrato l'errore: non è salvato
        db_result_free(result);
        result = result_error(io_failed_msg());
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
                if (catalog_save(g_db.catalog, g_db.cat_path) == SV_OK) pf_ddl_saved();
                break;
            default: break;
        }
    }
    return result;
}


typedef struct { char name[64]; int64_t offset; uint64_t chg_seq; } SavepointRec;

struct DbSession {
    bool         fk_off;            // SET FOREIGN_KEY_CHECKS = 0 in questa sessione
    UserVars*    uvars;             // variabili @x della sessione
    int          cur_db;            // indice nel catalogo (-1 = nessuno)
    uint64_t     tx_id;
    SavepointRec sp[64];
    int          nsp;
    char         user[64];
    uint64_t     gen;               // avvio del motore a cui appartiene tx_id (dopo un riavvio non vale più)
    int64_t      tx_start;          // posizione del BEGIN nel log (l'annullamento parte da lì)
    bool         tx_ddl;            // la transazione ha cambiato lo schema: ROLLBACK invalida tutto
};



// ── letture della versione confermata (MVCC) ────────────────────
// Ogni modifica di una transazione aperta resta qui (riga di prima, posizione) finché la transazione
// finisce. Una SELECT che troverebbe una tabella in scrittura altrui non aspetta: legge la versione
// confermata (heap_snapshot), ricostruita da queste voci. Le scritture restano serializzate dai lock.

// (database, tabella) delle modifiche: un id piccolo invece di due nomi copiati in ogni voce (184 -> 56 byte
// per riga modificata); l'elenco degli id si svuota quando non ci sono più modifiche in sospeso
typedef struct { char db[64], table[64]; } ChgTable;
static ChgTable* g_ctab;
static int       g_nctab, g_capctab;
static int       g_ctab_last = -1;

// id della tabella; create: aggiunto se manca. -1 = non c'è (o memoria esaurita)
static int ctab_id(const char* db, const char* table, bool create) {
    if (g_ctab_last >= 0 && g_ctab_last < g_nctab && !strcasecmp(g_ctab[g_ctab_last].table, table) && !strcasecmp(g_ctab[g_ctab_last].db, db))
        return g_ctab_last;
    for (int i = 0; i < g_nctab; i++)
        if (!strcasecmp(g_ctab[i].table, table) && !strcasecmp(g_ctab[i].db, db)) return g_ctab_last = i;
    if (!create || g_nctab >= 65535) return -1;
    if (g_nctab == g_capctab) {
        int nc = g_capctab ? g_capctab * 2 : 16;
        ChgTable* nv = sv_realloc(g_ctab, sizeof(ChgTable) * (size_t)nc);
        if (!nv) return -1;
        g_ctab = nv;
        g_capctab = nc;
    }
    snprintf(g_ctab[g_nctab].db, sizeof(g_ctab[0].db), "%s", db);
    snprintf(g_ctab[g_nctab].table, sizeof(g_ctab[0].table), "%s", table);
    return g_ctab_last = g_nctab++;
}

typedef struct {
    uint64_t tx, seq;
    uint8_t* old;
    uint32_t ol;
    uint16_t tid;                  // ctab_id
    uint8_t  type;                 // LOG_INSERT / LOG_DELETE / LOG_UPDATE / LOG_UPDATE_MOVE; 0 = non registrata (memoria)
    RID      rid, nrid;
} TxChange;

static TxChange* g_chg;
static int       g_nchg, g_capchg;
static uint64_t  g_chg_seq;

static void chg_hook(uint64_t tx, const char* db, const char* table, uint8_t type, RID rid, RID nrid, const uint8_t* old, size_t ol) {
    if (g_nchg == g_capchg) {
        int nc = g_capchg ? g_capchg * 2 : 256;
        TxChange* nv = sv_realloc(g_chg, sizeof(TxChange) * (size_t)nc);
        if (!nv) { if (g_nchg) g_chg[g_nchg - 1].type = 0; g_chg_lost = true; return; }   // la transazione non si potrà leggere confermata
        g_chg = nv;
        g_capchg = nc;
    }
    TxChange* c = &g_chg[g_nchg];
    memset(c, 0, sizeof(*c));
    c->tx = tx;
    c->seq = ++g_chg_seq;
    int tid = ctab_id(db, table, true);
    c->tid = (uint16_t)(tid < 0 ? 0 : tid);
    c->type = tid < 0 ? 0 : type;
    if (tid < 0) g_chg_lost = true;
    c->rid = rid;
    c->nrid = nrid;
    if (old && ol) {
        c->old = sv_malloc(ol);
        if (c->old) { memcpy(c->old, old, ol); c->ol = (uint32_t)ol; }
        else { c->type = 0; g_chg_lost = true; }
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
    if (g_nchg == 0) { g_nctab = 0; g_ctab_last = -1; }   // nessuna modifica in sospeso: id da capo
}

typedef struct { RID rid; const uint8_t* old; size_t ol; bool fresh, dead; } SnapRow;

static int snaprow_cmp(const void* a, const void* b) {
    const SnapRow* x = a; const SnapRow* y = b;
    if (x->rid.page_id != y->rid.page_id) return x->rid.page_id < y->rid.page_id ? -1 : 1;
    return x->rid.slot_id < y->rid.slot_id ? -1 : x->rid.slot_id > y->rid.slot_id;
}

// RID -> posizione in SnapRow (indirizzamento aperto; -1 libero): la ricostruzione della versione confermata
// era quadratica (ricerca lineare per ogni modifica), con 100 000 righe modificate ~5·10^9 confronti
typedef struct { int* h; size_t cap; } RidMap;
static size_t rid_hash(RID r, size_t cap) {
    uint64_t k = ((uint64_t)r.page_id << 16 | r.slot_id) * 0x9E3779B97F4A7C15ULL;
    return (size_t)(k >> 20) & (cap - 1);
}
static bool ridmap_grow(RidMap* mp, const SnapRow* m, int n) {
    size_t nc = mp->cap ? mp->cap * 2 : 256;
    while (nc < (size_t)n * 2 + 2) nc *= 2;
    int* nh = sv_malloc(sizeof(int) * nc);
    if (!nh) return false;
    for (size_t i = 0; i < nc; i++) nh[i] = -1;
    for (int k = 0; k < n; k++) {
        if (m[k].dead) continue;
        size_t p = rid_hash(m[k].rid, nc);
        while (nh[p] >= 0) p = (p + 1) & (nc - 1);
        nh[p] = k;
    }
    free(mp->h);
    mp->h = nh;
    mp->cap = nc;
    return true;
}
// voce viva con quel RID, o -1; slot: dove inserirla (se manca)
static int ridmap_find(const RidMap* mp, const SnapRow* m, RID r, size_t* slot) {
    size_t p = rid_hash(r, mp->cap);
    while (mp->h[p] >= 0) {
        const SnapRow* e = &m[mp->h[p]];
        if (!e->dead && e->rid.page_id == r.page_id && e->rid.slot_id == r.slot_id) { *slot = p; return mp->h[p]; }
        p = (p + 1) & (mp->cap - 1);
    }
    *slot = p;
    return -1;
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
    SnapRow* m = NULL;              // posizione attuale -> origine (riga confermata o nuova); dead = sostituita
    RidMap mp = { NULL, 0 };
    int tid = ctab_id(d->name, table, false);
    bool fail = false;
    for (int i = 0; tid >= 0 && i < g_nchg && !fail; i++) {
        const TxChange* c = &g_chg[i];
        if (c->tx != tx || c->tid != (uint16_t)tid) continue;
        if (!c->type) { fail = true; break; }
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 64;
            SnapRow* nm = sv_realloc(m, sizeof(SnapRow) * (size_t)cap);
            if (!nm) { fail = true; break; }
            m = nm;
        }
        if ((size_t)(n + 2) * 2 > mp.cap && !ridmap_grow(&mp, m, n)) { fail = true; break; }
        size_t slot;
        RID key = c->rid;
        int at = ridmap_find(&mp, m, key, &slot);
        SnapRow origin = { key, c->old, c->ol, false, false };
        if (at >= 0) origin = m[at];
        switch (c->type) {
        case LOG_INSERT:
            if (at >= 0) m[at] = (SnapRow){ key, NULL, 0, true, false };
            else { m[n] = (SnapRow){ key, NULL, 0, true, false }; mp.h[slot] = n++; }
            break;
        case LOG_UPDATE:
            if (at < 0) { m[n] = origin; mp.h[slot] = n++; }        // prima modifica: la versione confermata è la vecchia
            break;
        case LOG_UPDATE_MOVE: {
            if (at >= 0) m[at].dead = true;                        // la vecchia posizione non c'è più
            origin.rid = c->nrid;
            origin.dead = false;
            size_t s2;
            int at2 = ridmap_find(&mp, m, origin.rid, &s2);
            if (at2 >= 0) m[at2] = origin;
            else { m[n] = origin; mp.h[s2] = n++; }
            break;
        }
        case LOG_DELETE:
            if (at >= 0) m[at].dead = true;
            if (!origin.fresh && origin.old) {                     // riga confermata cancellata: torna in fondo
                if (out->nextra + 1 > ecap) {
                    ecap = ecap ? ecap * 2 : 16;
                    uint8_t** ne = sv_realloc(out->extra, sizeof(uint8_t*) * (size_t)ecap);
                    size_t* nl = sv_realloc(out->extra_len, sizeof(size_t) * (size_t)ecap);
                    if (ne) out->extra = ne;
                    if (nl) out->extra_len = nl;
                    if (!ne || !nl) { fail = true; break; }
                }
                out->extra[out->nextra] = (uint8_t*)origin.old;
                out->extra_len[out->nextra++] = origin.ol;
            }
            break;
        }
    }
    free(mp.h);
    if (fail) { free(m); return false; }
    {   // via le voci sostituite
        int k = 0;
        for (int i = 0; i < n; i++) if (!m[i].dead) m[k++] = m[i];
        n = k;
    }
    if (n) qsort(m, (size_t)n, sizeof(SnapRow), snaprow_cmp);
    out->hide = sv_malloc(sizeof(RID) * (size_t)(n + 1));
    out->repl_rid = sv_malloc(sizeof(RID) * (size_t)(n + 1));
    out->repl = sv_malloc(sizeof(uint8_t*) * (size_t)(n + 1));
    out->repl_len = sv_malloc(sizeof(size_t) * (size_t)(n + 1));
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

static _Thread_local void (*g_wait_leave)(void*);
static _Thread_local void (*g_wait_enter)(void*);
static _Thread_local void* g_wait_ud;

void db_set_lock_wait_hooks(void (*leave)(void* ud), void (*enter)(void* ud), void* ud) {
    g_wait_leave = leave;
    g_wait_enter = enter;
    g_wait_ud = ud;
}

// attesa senza polling: fuori dal motore (e dal turno del server) finché un lock viene rilasciato o una
// sessione esce; al massimo 200 ms per ricontrollare tempo scaduto e annullamento
static void wait_lock_change(unsigned long long seq, double deadline) {
    if (g_wait_leave) g_wait_leave(g_wait_ud);
    double left = deadline - get_time_ms();
    int ms = left > 200 ? 200 : left < 1 ? 1 : (int)left;
    lk_wait_change(seq, ms);
    if (g_wait_enter) g_wait_enter(g_wait_ud);
}

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
        if (strcasecmp(d->tables[t]->name, name)) continue;
        if (ls_mode(ls, dbi, name) >= mode) return;            // già visto (anche nei cicli di riferimenti)
        ls_add(ls, dbi, name, mode);
        if (mode != LK_X) return;
        for (int g = 0; g < d->num_triggers; g++)               // i trigger possono scrivere ovunque
            if (!strcasecmp(d->triggers[g].table_name, name)) ls_scan_text(ls, dbi, d->triggers[g].body, LK_X, depth + 1);
        const TableSchema* ts = d->tables[t];
        for (int c = 0; c < ts->num_constraints; c++)           // tabelle riferite: controllate (lettura)
            if (ts->constraints[c].kind == CONS_FOREIGN) ls_name(ls, dbi, ts->constraints[c].ref_table, LK_S, depth + 1);
        for (int u = 0; u < d->num_tables; u++)                 // tabelle che la riferiscono: CASCADE / SET NULL
            for (int c = 0; c < d->tables[u]->num_constraints; c++)
                if (d->tables[u]->constraints[c].kind == CONS_FOREIGN && !strcasecmp(d->tables[u]->constraints[c].ref_table, name))
                    ls_name(ls, dbi, d->tables[u]->name, LK_X, depth + 1);
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
    case NODE_BEGIN: case NODE_COMMIT: case NODE_ROLLBACK: case NODE_SAVEPOINT: case NODE_ROLLBACK_TO: case NODE_USE: case NODE_SET:
        return 0;
    case NODE_SELECT: case NODE_SHOW: case NODE_DESCRIBE: case NODE_EXPLAIN:
        return 1;
    case NODE_INSERT: case NODE_UPDATE: case NODE_DELETE: case NODE_CALL:
        return 2;
    default:
        return 3;
    }
}

// CALL di una procedura che crea tabelle TEMPORARY (anche in quelle che chiama): cambia lo schema
static bool call_changes_schema(const ASTNode* st, int dbi) {
    if (st->type != NODE_CALL || dbi < 0 || dbi >= g_db.catalog->num_databases) return false;
    const Database* d = &g_db.catalog->databases[dbi];
    for (int r = 0; r < d->num_routines; r++)
        for (const char* p = d->routines[r].body; *p; p++)
            if ((*p == 'T' || *p == 't') && !strncasecmp(p, "TEMPORARY", 9)) return true;
    return false;
}

// lock di uno statement: le tabelle del suo testo, più il lock globale (S per tutti, X per lo schema)
static void stmt_locks(const ASTNode* st, const TokenList* tl, LockSet* ls, int dbi) {
    ls->n = 0;
    ls->overflow = false;
    int kind = stmt_lock_kind(st);
    if (kind == 2 && call_changes_schema(st, dbi)) kind = 3;
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
        unsigned long long seq = lk_seq();              // prima del tentativo: un rilascio dopo non si perde
        int r = lk_acquire(owner, ls->r, ls->n);
        if (r == LK_GRANTED) return 0;
        if (r == LK_DEADLOCK) return 1;
        if (allow_snapshot && try_snapshot(ls)) return 3;     // SELECT: la versione confermata, subito
        if (sv_thread_cancelled()) { lk_cancel_wait(owner); return 4; }   // KILL QUERY / Annulla della GUI
        if (get_time_ms() - t0 > g_lock_wait_ms || g_lock_depth != 1) { lk_cancel_wait(owner); return 2; }
        DbSession* s = g_active;
        if (s) { session_save(s); session_load(&g_default_session); g_active = NULL; }
        g_lock_depth = 0;
        if (s) lk_notify();                         // la sessione è uscita: chi aspettava di entrare entra
        sv_rw_wrunlock(&g_db_rw);
        wait_lock_change(seq, t0 + g_lock_wait_ms);
        sv_rw_wr(&g_db_rw);
        g_lock_depth = 1;
        if (s) {
            while (g_active) {                      // un'altra sessione è dentro: si aspetta che esca
                unsigned long long seq2 = lk_sess_seq();
                g_lock_depth = 0;
                sv_rw_wrunlock(&g_db_rw);
                if (g_wait_leave) g_wait_leave(g_wait_ud);
                lk_wait_sess(seq2, 200);
                if (g_wait_enter) g_wait_enter(g_wait_ud);
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
    if (why == 4) return result_error("Query cancelled while waiting for a lock held by another session");
    return result_error("Lock wait timeout exceeded; try restarting transaction (a table is in use by another session's open transaction)");
}

// a fine statement, senza transazione aperta, i lock si liberano subito
static void release_if_autocommit(void) {
    if (!g_db.exec_ctx.tx_id) lk_release_all(lock_owner());
}

// SELECT semplice (non FOR UPDATE / FOR SHARE) in una transazione: i lock S che ha preso non restano
// fino a COMMIT. Così due sessioni che leggono e poi scrivono la stessa tabella non finiscono in un
// deadlock di conversione S -> X; la lettura vede l'ultima versione confermata (READ COMMITTED)
static void release_plain_read(const LockSet* ls, const uint8_t* prior) {
    if (ls && prior && g_db.exec_ctx.tx_id) lk_release_reads(lock_owner(), ls->r, ls->n, prior);
}

static QueryResult* db_execute_impl(const char* sql);

// '?' fuori da stringhe, identificatori `...` e commenti
static const char* next_placeholder(const char* p) {
    for (; *p; p++) {
        if (*p == '\'' || *p == '"' || *p == '`') {
            char q = *p;
            for (p++; *p && *p != q; p++) if (*p == '\\' && q != '`' && p[1]) p++;
            if (!*p) return NULL;
            continue;
        }
        if (*p == '-' && p[1] == '-') { while (*p && *p != '\n') p++; if (!*p) return NULL; continue; }
        if (*p == '#') { while (*p && *p != '\n') p++; if (!*p) return NULL; continue; }
        if (*p == '/' && p[1] == '*') { const char* e = strstr(p + 2, "*/"); if (!e) return NULL; p = e + 1; continue; }
        if (*p == '?') return p;
    }
    return NULL;
}

// PREPARE nome FROM 'sql' | @var; EXECUTE nome [USING @a, ...]; {DEALLOCATE | DROP} PREPARE nome.
// NULL se lo statement non è uno di questi
static QueryResult* prepared_statement(const TokenList* tk) {
    const Token* t = tk->tokens;
    int n = tk->count;
    #define TW(i, w) ((i) < n && t[i].text && t[i].type != TOK_STRING && !strcasecmp(t[i].text, w))
    if (TW(0, "PREPARE") && n >= 4 && t[1].type == TOK_IDENTIFIER && TW(2, "FROM")) {
        char* text = NULL;
        if (t[3].type == TOK_STRING) text = sv_strdup(t[3].text);
        else if (t[3].type == TOK_IDENTIFIER && t[3].text[0] == '@') {
            text = executor_uservar_literal(t[3].text + 1);
            if (text && text[0] == '\'') {                        // il testo della variabile, senza apici
                TokenList* lt = lexer_tokenize(text);
                char* raw = lt && lt->count > 0 && lt->tokens[0].type == TOK_STRING ? sv_strdup(lt->tokens[0].text) : NULL;
                if (lt) lexer_free(lt);
                free(text);
                text = raw;
            } else { free(text); text = NULL; }
        }
        if (!text) return result_error("PREPARE needs FROM 'statement' or FROM @variable with the statement text");
        bool ok = executor_prepare_store(t[1].text, text);
        free(text);
        return ok ? result_ok(0) : result_error("Out of memory");
    }
    if ((TW(0, "DEALLOCATE") || (t[0].type == TOK_DROP)) && TW(1, "PREPARE") && n >= 3) {
        if (!executor_prepare_drop(t[2].text)) {
            char m[200]; snprintf(m, sizeof(m), "Unknown prepared statement handler (%.64s) given to DEALLOCATE PREPARE", t[2].text);
            return result_error(m);
        }
        return result_ok(0);
    }
    if (TW(0, "EXECUTE") && n >= 2 && t[1].type == TOK_IDENTIFIER) {
        const char* sql = executor_prepare_get(t[1].text);
        if (!sql) { char m[200]; snprintf(m, sizeof(m), "Unknown prepared statement handler (%.64s) given to EXECUTE", t[1].text); return result_error(m); }
        char* args[64];
        int na = 0;
        if (TW(2, "USING")) {
            for (int i = 3; i < n && t[i].type != TOK_EOF && t[i].type != TOK_SEMICOLON && na < 64; i++) {
                if (t[i].type == TOK_COMMA) continue;
                if (t[i].type != TOK_IDENTIFIER || t[i].text[0] != '@') { for (int k = 0; k < na; k++) free(args[k]); return result_error("EXECUTE ... USING takes @variables"); }
                args[na++] = executor_uservar_literal(t[i].text + 1);
            }
        }
        int need = 0;
        for (const char* p = next_placeholder(sql); p; p = next_placeholder(p + 1)) need++;
        if (need != na) {
            for (int k = 0; k < na; k++) free(args[k]);
            return result_error("Incorrect arguments to EXECUTE: the number of ? and of USING variables differ");
        }
        size_t len = strlen(sql) + 1;
        for (int k = 0; k < na; k++) len += args[k] ? strlen(args[k]) : 4;
        char* full = sv_malloc(len);
        if (!full) { for (int k = 0; k < na; k++) free(args[k]); return result_error("Out of memory"); }
        size_t o = 0;
        const char* from = sql;
        int k = 0;
        for (const char* p = next_placeholder(sql); p; p = next_placeholder(p + 1)) {
            memcpy(full + o, from, (size_t)(p - from)); o += (size_t)(p - from);
            const char* v = args[k] ? args[k] : "NULL";
            memcpy(full + o, v, strlen(v)); o += strlen(v);
            from = p + 1;
            k++;
        }
        strcpy(full + o, from);
        for (int i = 0; i < na; i++) free(args[i]);
        QueryResult* r = db_execute_impl(full);
        free(full);
        return r;
    }
    #undef TW
    return NULL;
}

// ── generazione dello schema (P2.2) ─────────────────────────────
// Cambia dopo ogni statement che non è una lettura o una modifica di righe (DDL, USE, CALL, ROLLBACK, ...):
// gli alberi dei prepared statement analizzati prima si rifanno (l'executor vi lascia risoluzioni che
// dipendono dallo schema, come le colonne di USING). Prudente: più invalidazioni del necessario, mai meno.
static uint64_t g_schema_gen = 1;
static void schema_maybe_changed(const ASTNode* st) {
    if (!st || (st->type != NODE_SELECT && st->type != NODE_INSERT && st->type != NODE_UPDATE && st->type != NODE_DELETE))
        g_schema_gen++;
}

static QueryResult* db_execute_impl(const char* sql) {
    if (!g_db.initialized) return result_error("DB non inizializzato");
    if (!sql || !sql[0])   return result_error("Query vuota");

    double t_start = get_time_ms();

    // Pipeline: tokenize → parse → execute
    TokenList*   tokens = lexer_tokenize(sql);
    QueryResult* prep = tokens ? prepared_statement(tokens) : NULL;
    if (prep) { lexer_free(tokens); prep->exec_time_ms = get_time_ms() - t_start; return prep; }
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
        LockSet* ls = sv_malloc(sizeof(LockSet));
        if (!ls) result = result_error("Out of memory");
        else {
            stmt_locks(parsed->statements[0], tokens, ls, g_db.catalog->current_db);
            bool snap = stmt_lock_kind(parsed->statements[0]) == 1 && !locking_read(tokens);
            uint8_t prior[MAX_STMT_LOCKS];
            if (snap) lk_modes(lock_owner(), ls->r, ls->n, prior);
            int why = acquire_locks(ls, snap);
            result = why == 1 || why == 2 || why == 4 ? lock_error(why) : exec_one(parsed->statements[0]);
            if (why == 3) snapshot_clear();
            if (snap) release_plain_read(ls, prior);
            release_if_autocommit();
            free(ls);
        }
        schema_maybe_changed(parsed->statements[0]);
    } else result = result_ok(0);
    if (result) result->exec_time_ms = get_time_ms() - t_start;

    parse_result_free(parsed);
    lexer_free(tokens);
    wal_write_buffer(g_db.wal);       // a fine statement il log è nel file (sopravvive a un crash del processo)
    pf_stmt_end();
    pf_process();
    maybe_checkpoint();
    return result;
}

static QueryResult** db_execute_multi_impl(const char* sql, int* out_count) {
    *out_count = 0;
    if (!g_db.initialized) return NULL;

    double t_start = get_time_ms();

    TokenList*   tokens = lexer_tokenize(sql);
    QueryResult* prep = tokens ? prepared_statement(tokens) : NULL;    // PREPARE / EXECUTE / DEALLOCATE (uno solo)
    if (prep) {
        lexer_free(tokens);
        QueryResult** one = SV_ALLOC_N(QueryResult*, 1);
        if (!one) { db_result_free(prep); return NULL; }
        one[0] = prep;
        *out_count = 1;
        return one;
    }
    ParseResult* parsed = parser_parse(tokens);

    if (!parsed->success || parsed->count == 0) {
        parse_result_free(parsed);
        lexer_free(tokens);
        return NULL;
    }

    QueryResult** results = SV_ALLOC_N(QueryResult*, parsed->count);
    LockSet* ls = sv_malloc(sizeof(LockSet));
    for (int i = 0; i < parsed->count; i++) {
        // i lock di ogni statement dal testo intero (sovrastima: più tabelle, mai meno)
        int why = 0;
        bool plain = false;
        uint8_t prior[MAX_STMT_LOCKS];
        if (ls) {
            stmt_locks(parsed->statements[i], tokens, ls, g_db.catalog->current_db);
            plain = stmt_lock_kind(parsed->statements[i]) == 1 && !locking_read(tokens);
            if (plain) lk_modes(lock_owner(), ls->r, ls->n, prior);
            why = acquire_locks(ls, plain);
        }
        results[i] = !ls ? result_error("Out of memory") : why == 1 || why == 2 || why == 4 ? lock_error(why) : exec_one(parsed->statements[i]);
        if (why == 3) snapshot_clear();
        if (plain) release_plain_read(ls, prior);
        release_if_autocommit();
        schema_maybe_changed(parsed->statements[i]);
        if (results[i])
            results[i]->exec_time_ms = get_time_ms() - t_start;
    }
    free(ls);
    *out_count = parsed->count;
    wal_write_buffer(g_db.wal);
    pf_stmt_end();
    pf_process();
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
    g_tx_start = wal_size(g_db.wal);              // il primo record della transazione (BEGIN) è qui
    g_tx_ddl = false;
    wal_write(g_db.wal, LOG_BEGIN, tx->tx_id, 0,
              (SVRID){0,0}, NULL, 0, NULL, 0);
    return true;
}

static const void* lock_owner(void);

// dopo l'annullamento delle modifiche di tx (quelle dopo la sequenza after): via dalla cache solo le pagine
// riscritte e, delle tabelle toccate, gli indici in memoria e la mappa dello spazio libero. Con DDL nella
// transazione o modifiche non registrate si invalida tutto, come prima
// righe di ogni tabella dopo l'annullamento (P1.13: num_rows esatto, non più gonfiato da un ROLLBACK di
// INSERT o calato da uno di DELETE): ogni modifica annullata conta al contrario
static void rows_after_undo(uint64_t tx, uint64_t after) {
    for (int i = 0; i < g_nchg; i++) {
        const TxChange* c = &g_chg[i];
        if (c->tx != tx || c->seq <= after || c->tid >= g_nctab) continue;
        if (c->type != LOG_INSERT && c->type != LOG_DELETE) continue;
        TableSchema* ts = catalog_get_table(g_db.catalog, g_ctab[c->tid].db, g_ctab[c->tid].table);
        if (!ts) continue;
        if (c->type == LOG_INSERT) { if (ts->num_rows > 0) ts->num_rows--; }
        else ts->num_rows++;
    }
}

static void invalidate_after_undo(uint64_t tx, uint64_t after, const uint32_t* pages, int npages, bool undo_ok) {
    if (undo_ok && !g_tx_ddl && !g_chg_lost) rows_after_undo(tx, after);
    if (!undo_ok || g_tx_ddl || g_chg_lost || !pages) {
        bp_remove_all(g_db.bp);
        executor_invalidate_indexes();
        return;
    }
    for (int i = 0; i < npages; i++) bp_remove_page(g_db.bp, pages[i]);
    uint8_t* seen = sv_calloc((size_t)g_nctab + 1, 1);           // una volta per tabella
    if (!seen) { bp_remove_all(g_db.bp); executor_invalidate_indexes(); return; }
    for (int i = 0; i < g_nchg; i++) {
        const TxChange* c = &g_chg[i];
        if (c->tx != tx || c->seq <= after) continue;
        if (c->tid >= g_nctab || seen[c->tid]) continue;
        seen[c->tid] = 1;
        const ChgTable* ct = &g_ctab[c->tid];
        TableSchema* ts = catalog_get_table(g_db.catalog, ct->db, ct->table);
        if (ts) executor_invalidate_table(g_db.bp, ct->db, ct->table, ts->root_page_id);
        else { executor_invalidate_indexes(); break; }
    }
    free(seen);
}

// dopo un annullamento sulle pagine della cache (P1.1): le pagine sono già giuste in memoria, si buttano solo le
// strutture derivate (indici in memoria, mappa dello spazio libero) delle tabelle toccate; con DDL nella transazione,
// modifiche non registrate o un annullamento fallito, quelle di tutte le tabelle
static void invalidate_after_cache_undo(uint64_t tx, uint64_t after, bool undo_ok) {
    if (undo_ok && !g_tx_ddl && !g_chg_lost) rows_after_undo(tx, after);
    if (!undo_ok || g_tx_ddl || g_chg_lost) { executor_invalidate_indexes(); return; }
    uint8_t* seen = sv_calloc((size_t)g_nctab + 1, 1);
    if (!seen) { executor_invalidate_indexes(); return; }
    for (int i = 0; i < g_nchg; i++) {
        const TxChange* c = &g_chg[i];
        if (c->tx != tx || c->seq <= after || c->tid >= g_nctab || seen[c->tid]) continue;
        seen[c->tid] = 1;
        const ChgTable* ct = &g_ctab[c->tid];
        TableSchema* ts = catalog_get_table(g_db.catalog, ct->db, ct->table);
        if (ts) executor_invalidate_table(g_db.bp, ct->db, ct->table, ts->root_page_id);
        else { executor_invalidate_indexes(); break; }
    }
    free(seen);
}

// ── spazio liberato subito (P1.5) ────────────────────────────────
// Una catena di overflow sostituita o eliminata, e le pagine di una tabella eliminata, tornano nella lista delle
// pagine libere appena la modifica che le ha abbandonate è durevole nel log (prima: solo un VACUUM, che vuole
// zero transazioni aperte). Fino ad allora un arresto potrebbe far tornare la riga di prima, che le usa ancora.
// tx != 0: aspetta la fine della transazione (COMMIT o ROLLBACK: dopo l'annullamento le righe rimesse hanno
// catene nuove); ready = LSN da cui la si può liberare (-1 = non ancora deciso); heap: radice di una tabella
typedef struct { uint64_t tx; int64_t ready; uint32_t first, total; uint8_t heap; } PendFree;
static PendFree* g_pf;
static int g_npf, g_cappf;
static unsigned long long g_pf_pages;          // pagine liberate così (SHOW STATUS)

static void pf_add(uint64_t tx, uint32_t first, uint32_t total, uint8_t heap) {
    if (!first) return;
    if (g_npf == g_cappf) {
        int nc = g_cappf ? g_cappf * 2 : 64;
        PendFree* nv = sv_try_realloc(g_pf, sizeof(PendFree) * (size_t)nc);
        if (!nv) { atomic_fetch_add(&g_sv_abandoned_pages, (int)(total / SV_PAGE_SIZE + 1)); return; }   // VACUUM poi
        g_pf = nv;
        g_cappf = nc;
    }
    g_pf[g_npf++] = (PendFree){ tx, -1, first, total, heap };
}

static void on_chain_abandoned(uint32_t first, uint32_t total) { pf_add(g_db.exec_ctx.tx_id, first, total, 0); }
static void on_table_dropped(uint32_t root) { pf_add(0, root, 0, 1); }

static void pf_tx_end(uint64_t tx) {
    int64_t at = wal_size(g_db.wal);
    for (int i = 0; i < g_npf; i++) if (tx && g_pf[i].tx == tx) { g_pf[i].tx = 0; g_pf[i].ready = at; }
}

static void pf_stmt_end(void) {                // statement in autocommit concluso: le sue catene, dal suo ultimo record
    int64_t at = wal_size(g_db.wal);
    for (int i = 0; i < g_npf; i++) if (!g_pf[i].tx && g_pf[i].ready < 0 && !g_pf[i].heap) g_pf[i].ready = at;
}

static void pf_ddl_saved(void) {               // DROP TABLE su disco (catalogo salvato con fsync): subito
    for (int i = 0; i < g_npf; i++) if (g_pf[i].heap && g_pf[i].ready < 0) g_pf[i].ready = 0;
}

static void free_chain(uint32_t first, uint32_t total, uint64_t lsn) {
    uint32_t guard = total / (SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE - 2) + 2;
    for (uint32_t pid = first; pid && guard--; ) {
        Page* p = bp_fetch(g_db.bp, pid);
        if (!p) break;
        bool ov = (p->flags & PAGE_FLAG_OVERFLOW) != 0;
        uint32_t nx = page_next(p);
        bp_unpin(g_db.bp, pid, false);
        if (!ov || bp_free_page_now(g_db.bp, pid, lsn) != SV_OK) break;
        g_pf_pages++;
        pid = nx;
    }
}

static void free_heap(uint32_t root, uint64_t lsn) {
    uint32_t* ids = NULL;
    int n = 0, cap = 0;
    for (uint32_t pid = root, guard = 0; pid && guard < (1u << 24); guard++) {
        Page* p = bp_fetch(g_db.bp, pid);
        if (!p) break;
        if (p->flags & (PAGE_FLAG_FREE | PAGE_FLAG_OVERFLOW)) { bp_unpin(g_db.bp, pid, false); break; }
        for (uint16_t s = 0; s < p->num_slots; s++) {          // le righe grandi: anche le loro catene
            uint16_t l = 0;
            const void* d = page_get_slot(p, s, &l);
            uint32_t t = 0, f = 0;
            if (d && ov_is_stub(d, l, &t, &f)) {
                bp_unpin(g_db.bp, pid, false);
                free_chain(f, t, lsn);
                p = bp_fetch(g_db.bp, pid);
                if (!p) break;
            }
        }
        if (!p) break;
        uint32_t nx = page_next(p);
        bp_unpin(g_db.bp, pid, false);
        if (n == cap) {
            int nc = cap ? cap * 2 : 64;
            uint32_t* nv = sv_try_realloc(ids, sizeof(uint32_t) * (size_t)nc);
            if (!nv) break;
            ids = nv;
            cap = nc;
        }
        ids[n++] = pid;
        pid = nx;
    }
    for (int i = 0; i < n; i++) if (bp_free_page_now(g_db.bp, ids[i], lsn) == SV_OK) g_pf_pages++;
    free(ids);
}

// libera ciò che è pronto: la modifica che l'ha abbandonato è nel log su disco. Se aspettano almeno 64 KB (righe
// grandi sostituite) e il log non è ancora su disco (synchronous = NORMAL: fsync di gruppo), si fa l'fsync subito:
// così lo statement successivo riusa quelle pagine invece di allungare il file
#define PF_SYNC_PAGES 16
static void pf_process(void) {
    if (!g_npf || io_failed()) return;
    if (g_sync_mode >= 1 && g_db.wal->synced_lsn < wal_size(g_db.wal)) {
        uint64_t waiting = 0;
        for (int i = 0; i < g_npf; i++)
            if (!g_pf[i].tx && g_pf[i].ready >= 0 && g_pf[i].ready > g_db.wal->synced_lsn)
                waiting += g_pf[i].heap ? PF_SYNC_PAGES : g_pf[i].total / (SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE - 2) + 1;
        if (waiting >= PF_SYNC_PAGES && wal_flush(g_db.wal) == SV_OK) g_last_sync = get_time_ms();
    }
    int64_t synced = g_db.wal->synced_lsn;
    uint64_t lsn = (uint64_t)wal_size(g_db.wal);
    int k = 0;
    for (int i = 0; i < g_npf; i++) {
        PendFree* e = &g_pf[i];
        if (e->tx || e->ready < 0 || e->ready > synced) { g_pf[k++] = *e; continue; }
        if (e->heap) free_heap(e->first, lsn);
        else free_chain(e->first, e->total, lsn);
    }
    g_npf = k;
}

// fine della transazione in memoria (dopo COMMIT, ROLLBACK, o un errore che li ha interrotti)
static void tx_end(bool committed) {
    pf_tx_end(g_db.exec_ctx.tx_id);
    if (committed) txm_commit(g_db.txm, g_db.exec_ctx.tx_id);
    else txm_abort(g_db.txm, g_db.exec_ctx.tx_id);
    chg_drop(g_db.exec_ctx.tx_id, 0);
    g_db.exec_ctx.tx_id = 0;
    g_num_savepoints = 0;
    if (g_open_tx > 0) g_open_tx--;
    if (g_open_tx == 0) g_chg_lost = false;
    lk_release_all(lock_owner());          // fine della transazione: i lock delle tabelle si liberano
}

static char g_tx_err[400];               // perché l'ultimo COMMIT / ROLLBACK non è riuscito

static bool db_commit_impl(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    g_tx_err[0] = '\0';
    if (io_failed()) {                     // nessuna garanzia: il recupero all'avvio la annullerà
        snprintf(g_tx_err, sizeof(g_tx_err), "%s", io_failed_msg());
        tx_end(false);
        return false;
    }
    if (g_sync_mode == 0) {                // synchronous = OFF: il COMMIT resta nel log, senza fsync
        bool w = wal_write(g_db.wal, LOG_COMMIT, g_db.exec_ctx.tx_id, 0, (SVRID){0,0}, NULL, 0, NULL, 0) == SV_OK &&
                 wal_write_buffer(g_db.wal) == SV_OK;
        if (!w) { snprintf(g_tx_err, sizeof(g_tx_err), "COMMIT failed, the transaction was not saved: %.300s", io_failed_msg()); tx_end(false); return false; }
        tx_end(true);
        return true;
    }
    // il record COMMIT deve essere su disco prima di rispondere OK. Se la scrittura o l'fsync falliscono
    // il log torna alla lunghezza di prima, così il COMMIT non può arrivare su disco più tardi: la
    // transazione non c'è (il recupero all'avvio la annulla) e la risposta lo dice
    int64_t before = -1;
    bool ok = wal_write_buffer(g_db.wal) == SV_OK;
    if (ok) before = sv_lseek(g_db.wal->fd, 0, SEEK_END);
    ok = ok && wal_write(g_db.wal, LOG_COMMIT, g_db.exec_ctx.tx_id, 0, (SVRID){0,0}, NULL, 0, NULL, 0) == SV_OK &&
         wal_flush(g_db.wal) == SV_OK;
    if (!ok) {
        g_db.wal->failed = true;
        g_db.wal->blen = 0;
        if (before >= 0 && sv_ftruncate(g_db.wal->fd, before) == 0) g_db.wal->fsize = before;
        snprintf(g_tx_err, sizeof(g_tx_err), "COMMIT failed, the transaction was not saved: %.300s", io_failed_msg());
        tx_end(false);
        return false;
    }
    tx_end(true);
    return true;
}

// ROLLBACK: l'annullamento va su disco (CLR nel log, pagine, fsync) prima del record ABORT, che dice
// al recupero "annullamento completo": un arresto a metà lascia una transazione che il recupero finisce
static bool db_rollback_impl(void) {
    if (!g_db.initialized || !g_db.exec_ctx.tx_id) return false;
    g_tx_err[0] = '\0';
    // il log si legge dal BEGIN della transazione, non dall'inizio (O(transazione), non O(log))
    // (P1.1) sulle pagine della cache: CLR nel buffer del log, nessun flush, nessun fsync. L'ABORT non deve essere
    // durevole: se il processo cade prima, il recupero trova la transazione senza COMMIT, rifà i CLR e finisce
    uint32_t* pages = NULL;
    int npages = 0;
    bool undone = !io_failed() &&
                  wal_undo_tx_cache(g_db.wal, g_db.bp, g_db.exec_ctx.tx_id, g_tx_start, &pages, &npages) == SV_OK;
    bool ok = undone &&
              wal_write(g_db.wal, LOG_ABORT, g_db.exec_ctx.tx_id, 0, (SVRID){0,0}, NULL, 0, NULL, 0) == SV_OK;
    if (!ok && !io_failed()) g_db.wal->failed = true;     // (annullamento non riuscito: stato non più certo)
    if (!ok) snprintf(g_tx_err, sizeof(g_tx_err), "ROLLBACK failed: %.300s", io_failed_msg());
    invalidate_after_cache_undo(g_db.exec_ctx.tx_id, 0, undone);
    free(pages);
    tx_end(false);
    return ok;
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
    // nel buffer, senza fsync (niente deve essere su disco prima del COMMIT); la posizione è la fine
    // del record: file + buffer
    if (io_failed() || wal_write(g_db.wal, LOG_SAVEPOINT, g_db.exec_ctx.tx_id, 0,
                                 (SVRID){0,0}, (const uint8_t*)name, (uint32_t)strlen(name) + 1, NULL, 0) != SV_OK) return false;

    // Record offset
    snprintf(g_savepoints[g_num_savepoints].name, sizeof(g_savepoints[g_num_savepoints].name), "%s", name);
    g_savepoints[g_num_savepoints].offset = wal_size(g_db.wal);
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

    if (io_failed()) return false;
    int64_t sp_off = g_savepoints[sp_idx].offset;
    // annulla le voci della transazione scritte dopo il savepoint (stesso undo del ROLLBACK, con i CLR);
    // il log non si accorcia (dopo il savepoint ci sono anche le voci delle altre transazioni): una
    // voce ROLLBACK_TO dice a ROLLBACK e al recupero che quelle voci sono già state annullate
    int64_t sp64 = (int64_t)sp_off;
    uint32_t* pages = NULL;
    int npages = 0;
    bool undone = wal_undo_tx_cache(g_db.wal, g_db.bp, g_db.exec_ctx.tx_id, sp_off, &pages, &npages) == SV_OK;
    bool ok = undone &&
              wal_write(g_db.wal, LOG_ROLLBACK_TO, g_db.exec_ctx.tx_id, 0, (SVRID){0,0}, (const uint8_t*)&sp64, 8, NULL, 0) == SV_OK;
    invalidate_after_cache_undo(g_db.exec_ctx.tx_id, g_savepoints[sp_idx].chg_seq, undone);
    free(pages);
    if (!ok) {
        if (!io_failed()) g_db.wal->failed = true;
        return false;
    }
    chg_drop(g_db.exec_ctx.tx_id, g_savepoints[sp_idx].chg_seq);
    g_num_savepoints = sp_idx + 1;      // il savepoint resta (come in SQL): ci si può tornare ancora
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
        nl->names[i] = sv_strdup(c->databases[i].name);
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
        nl->names[i] = sv_strdup(db->tables[i]->name);
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
        nl->names[i] = sv_strdup(db->views[i].name);
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
            nl->names[nl->count++] = sv_strdup(db->routines[i].name);
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
            nl->names[nl->count++] = sv_strdup(db->routines[i].name);
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
            nl->names[nl->count++] = sv_strdup(db->triggers[i].name);
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
        snprintf(cols[i].name, sizeof(cols[i].name), "%s", t->columns[i].name);
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
    stats_clear();
    g_session_user[0] = '\0';
    // una transazione lasciata aperta viene annullata (le sue pagine sono già nel buffer)
    if (g_db.exec_ctx.tx_id) db_rollback();
    // Checkpoint finale (log eliminato) solo se tutto è confermato e su disco. Con transazioni ancora
    // aperte in altre sessioni (un thread fermo a metà di una scrittura) le loro pagine possono
    // arrivare sul file: il log resta, e il prossimo avvio le annulla con il recupero. Dopo un errore
    // di I/O non si scrive più niente (le pagine senza il loro log violerebbero la regola del WAL).
    bool clean = false;
    if (!io_failed()) {
        bool flushed = wal_flush(g_db.wal) == SV_OK && bp_flush_all(g_db.bp) == SV_OK;
        bool cat = catalog_save(g_db.catalog, g_db.cat_path) == SV_OK;
        if (flushed) free_list_save();
        clean = flushed && cat && g_open_tx == 0;
        if (!clean) fprintf(stderr, "[SegVault] %s: the transaction log is kept, the next start recovers from it\n",
                            g_open_tx > 0 ? "transactions still open at shutdown" : "the data could not be written to disk");
    }
    g_db.bp->before_write = NULL;          // il log sta per essere chiuso
    g_db.bp->on_link = NULL;
    if (io_failed()) g_db.bp->io_error = true;   // bp_destroy non scrive più pagine
    if (!io_failed()) wal_write_buffer(g_db.wal);
    else g_db.wal->blen = 0;
    // senza log (eliminato qui sotto) il prossimo avvio riparte dall'LSN scritto nell'intestazione del file
    if (clean && (page_header_write(g_db.db_fd, (uint64_t)wal_size(g_db.wal)) != SV_OK || sv_fsync(g_db.db_fd) != 0)) clean = false;
    wal_destroy(g_db.wal);
    // checkpoint: tutte le modifiche sono su disco, il log non serve più.
    // Al prossimo avvio il recovery lavora solo dopo un arresto anomalo.
    if (clean) remove(g_db.log_path);
    // le transazioni delle altre sessioni finiscono qui (il recupero al prossimo avvio le annulla):
    // niente contatori, modifiche in sospeso o lock che sopravvivono al riavvio
    g_open_tx = 0;
    for (int i = 0; i < g_nchg; i++) free(g_chg[i].old);
    g_nchg = 0;
    g_nctab = 0;
    g_ctab_last = -1;
    lk_reset();
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

// ── prepared statement analizzati una volta (P2.2) ───────────────
// Il testo con i '?' si analizza una volta: i segnaposto diventano nodi letterali dell'albero (il parser li
// elenca, copie comprese), e ogni esecuzione vi scrive i valori e riusa albero e token. Solo SELECT, INSERT,
// UPDATE e DELETE di un solo statement; tutto il resto (e ciò che l'executor rianalizza dal testo, come le
// condizioni degli UPDATE / DELETE su più tabelle) torna NULL e il chiamante usa il testo con i valori.
struct DbPrepared {
    char*        sql;
    TokenList*   tokens;
    ParseResult* parsed;
    ASTExpr**    nodes;
    int          nnodes, nparams;
    uint64_t     gen;               // g_schema_gen dell'analisi
    uint64_t     engine_gen;        // avvio del motore (dopo un riavvio si rianalizza)
    bool         broken;            // da non usare più: il chiamante passa al testo
};

static void prep_clear(DbPrepared* p) {
    free(p->nodes);
    p->nodes = NULL;
    p->nnodes = 0;
    if (p->parsed) parse_result_free(p->parsed);
    if (p->tokens) lexer_free(p->tokens);
    p->parsed = NULL;
    p->tokens = NULL;
}

static unsigned long long g_prep_runs, g_prep_parses;   // esecuzioni dall'albero, analisi (sotto il lock)

void db_prepared_counts(unsigned long long* runs, unsigned long long* parses) {
    db_lock();
    *runs = g_prep_runs;
    *parses = g_prep_parses;
    db_unlock();
}

static bool prep_parse(DbPrepared* p) {
    prep_clear(p);
    g_prep_parses++;
    p->tokens = lexer_tokenize(p->sql);
    if (!p->tokens) return false;
    int need = 0;
    for (int i = 0; i < p->tokens->count; i++) if (p->tokens->tokens[i].type == TOK_PARAM) need++;
    const Token* t0 = p->tokens->count ? &p->tokens->tokens[0] : NULL;
    bool dml = t0 && (t0->type == TOK_SELECT || t0->type == TOK_INSERT || t0->type == TOK_UPDATE || t0->type == TOK_DELETE ||
                      t0->type == TOK_LPAREN);
    if (!dml) return false;                         // PREPARE / EXECUTE / utenti / DDL / WITH ...: col testo
    parser_params_begin();
    p->parsed = parser_parse(p->tokens);
    p->nnodes = parser_params_end(&p->nodes);
    if (!p->parsed || !p->parsed->success || p->parsed->count != 1) return false;
    StmtType ty = p->parsed->statements[0]->type;
    if (ty != NODE_SELECT && ty != NODE_INSERT && ty != NODE_UPDATE && ty != NODE_DELETE) return false;
    // ogni '?' deve essere diventato almeno un nodo (uno finito in un pezzo di testo dello statement no)
    for (int k = 1; k <= need; k++) {
        bool seen = false;
        for (int i = 0; i < p->nnodes && !seen; i++) seen = p->nodes[i]->param == k;
        if (!seen) return false;
    }
    p->nparams = need;
    p->gen = g_schema_gen;
    p->engine_gen = g_engine_gen;
    return true;
}

DbPrepared* db_prepared_new(const char* sql) {
    if (!sql) return NULL;
    DbPrepared* p = sv_calloc(1, sizeof(DbPrepared));
    p->sql = sv_strdup(sql);
    db_lock();
    bool ok = g_db.initialized && prep_parse(p);
    db_unlock();
    if (!ok) { db_prepared_free(p); return NULL; }
    return p;
}

void db_prepared_free(DbPrepared* p) {
    if (!p) return;
    db_lock();
    prep_clear(p);
    db_unlock();
    free(p->sql);
    free(p);
}

int db_prepared_params(const DbPrepared* p) { return p ? p->nparams : -1; }

// un valore nel formato di un letterale SQL ("12", "-3.5", "'testo'", "NULL") nel nodo del suo '?'
static bool prep_bind(ASTExpr* e, const char* lit) {
    if (e->type == EXPR_STRING) free(e->str_val);
    e->str_val = NULL;
    e->type = EXPR_NULL;
    if (!lit || !strcmp(lit, "NULL")) return true;
    const char* q = lit;
    bool neg = *q == '-';
    if (neg) q++;
    if (isdigit((unsigned char)*q)) {                     // numero: come parse_primary (oltre 64 bit: decimale)
        char* end;
        errno = 0;
        long long v = strtoll(q, &end, 10);
        if (*end == '\0' && errno != ERANGE) { e->type = EXPR_INT; e->int_val = neg ? -v : v; return true; }
        double d = strtod(q, &end);
        if (*end == '\0') { e->type = EXPR_FLOAT; e->float_val = neg ? -d : d; return true; }
        return false;
    }
    if (*lit != '\'') return false;
    TokenList* tl = lexer_tokenize(lit);                  // stringa: gli escape come nel testo
    bool ok = tl && tl->count >= 1 && tl->tokens[0].type == TOK_STRING && (tl->count == 1 || tl->tokens[1].type == TOK_EOF);
    if (ok) { e->type = EXPR_STRING; e->str_val = sv_strdup(tl->tokens[0].text); }
    if (tl) lexer_free(tl);
    return ok;
}

QueryResult* db_prepared_exec(DbPrepared* p, const char* const* lits, int n) {
    if (!p || p->broken) return NULL;
    db_lock();
    if (!g_db.initialized || ((p->gen != g_schema_gen || p->engine_gen != g_engine_gen) && !prep_parse(p))) {
        p->broken = true;
        db_unlock();
        return NULL;
    }
    if (n != p->nparams) { db_unlock(); return NULL; }
    for (int i = 0; i < p->nnodes; i++) {
        int k = p->nodes[i]->param;
        if (k < 1 || k > n || !prep_bind(p->nodes[i], lits[k - 1])) { db_unlock(); return NULL; }
    }
    double t_start = get_time_ms();
    ASTNode* st = p->parsed->statements[0];
    QueryResult* result;
    LockSet* ls = sv_malloc(sizeof(LockSet));
    if (!ls) result = result_error("Out of memory");
    else {
        stmt_locks(st, p->tokens, ls, g_db.catalog->current_db);
        bool plain = stmt_lock_kind(st) == 1 && !locking_read(p->tokens);
        uint8_t prior[MAX_STMT_LOCKS];
        if (plain) lk_modes(lock_owner(), ls->r, ls->n, prior);
        int why = acquire_locks(ls, plain);
        result = why == 1 || why == 2 || why == 4 ? lock_error(why) : exec_one(st);
        if (why == 3) snapshot_clear();
        if (plain) release_plain_read(ls, prior);
        release_if_autocommit();
        free(ls);
    }
    // un pezzo dello statement rianalizzato dal testo dall'executor ha trovato un '?': si torna al testo
    if (result && !result->success && strstr(result->error, "placeholders work only in prepared statements")) {
        db_result_free(result);
        result = NULL;
        p->broken = true;
    }
    if (result) { result->exec_time_ms = get_time_ms() - t_start; g_prep_runs++; }
    wal_write_buffer(g_db.wal);
    pf_stmt_end();
    pf_process();
    maybe_checkpoint();
    db_unlock();
    return result;
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
    g_schema_gen++;                               // un ROLLBACK può annullare una DDL
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
        for (int k = 0; k < db->tables[t]->num_constraints; k++) n += db->tables[t]->constraints[k].kind == CONS_FOREIGN;
    ForeignKeyInfo* out = n ? sv_calloc((size_t)n, sizeof(ForeignKeyInfo)) : NULL;
    for (int t = 0; out && t < db->num_tables; t++) {
        const TableSchema* ts = db->tables[t];
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
    s->gen = g_engine_gen;
    s->tx_start = g_tx_start;
    s->tx_ddl = g_tx_ddl;
    s->fk_off = !executor_fk_checks();
    s->uvars = executor_uservars();
    s->nsp = g_num_savepoints;
    memcpy(s->sp, g_savepoints, sizeof(s->sp));
    snprintf(s->user, sizeof(s->user), "%s", g_session_user);
}

static void session_load(const DbSession* s) {
    if (g_db.catalog) g_db.catalog->current_db = s->cur_db < g_db.catalog->num_databases ? s->cur_db : -1;
    bool same = s->gen == g_engine_gen;      // una transazione di prima del riavvio è già stata annullata
    g_db.exec_ctx.tx_id = same ? s->tx_id : 0;
    g_tx_start = same ? s->tx_start : 0;
    g_tx_ddl = same && s->tx_ddl;
    executor_set_fk_checks(!s->fk_off);
    executor_set_uservars(s->uvars);
    g_num_savepoints = same ? s->nsp : 0;
    memcpy(g_savepoints, s->sp, sizeof(g_savepoints));
    snprintf(g_session_user, sizeof(g_session_user), "%s", s->user);
}

DbSession* db_session_new(void) {
    DbSession* s = sv_calloc(1, sizeof(DbSession));
    if (s) s->cur_db = -1;
    return s;
}

DbSession* db_session_new_temp(void) {
    DbSession* s = sv_calloc(1, sizeof(DbSession));
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
    bool left = false;
    if (g_active == s) {
        session_save(s);
        session_load(&g_default_session);
        g_active = NULL;
        left = true;
    }
    db_unlock();
    if (left) lk_notify();                       // chi aspetta di entrare (db_session_wait) riprova subito
}

unsigned long long db_session_seq(void) { return lk_sess_seq(); }
void db_session_wait(unsigned long long seq, int max_ms) { lk_wait_sess(seq, max_ms); }

void db_session_end(DbSession* s) {
    if (!s) return;
    if (s->tx_id && db_session_enter(s)) {
        db_rollback();
        db_session_leave(s);
    }
    lk_release_all(s);                          // anche i lock di letture senza transazione
    db_lock();
    if (g_active != s) executor_uservars_free(s->uvars);   // (se attiva, le libera session_save al prossimo cambio)
    else { executor_set_uservars(NULL); executor_uservars_free(s->uvars); }
    db_unlock();
    free(s);
}

bool db_session_in_transaction(const DbSession* s) { return s && s->tx_id != 0; }

bool db_session_blocked(void) { return false; }   // le transazioni non si escludono più a vicenda

// ── letture in parallelo ────────────────────────────────────────

// flag della definizione di colonna del protocollo MySQL (NOT_NULL 1, PRI_KEY 2, UNIQUE_KEY 4, MULTIPLE_KEY 8,
// UNSIGNED 32, AUTO_INCREMENT 512) per una colonna di tabella; 0 se la tabella o la colonna non esistono (P2.8)
unsigned db_column_flags(const char* db, const char* table, const char* column) {
    if (!g_db.initialized || !db || !db[0] || !table || !table[0] || !column || !column[0]) return 0;
    bool locked = false;
    if (g_lock_depth == 0) { sv_rw_rd(&g_db_rw); locked = true; }
    unsigned fl = 0;
    TableSchema* t = catalog_get_table(g_db.catalog, db, table);
    int ci = -1;
    for (int i = 0; t && i < t->num_columns && ci < 0; i++) if (!strcasecmp(t->columns[i].name, column)) ci = i;
    if (ci >= 0) {
        const Column* c = &t->columns[ci];
        if (!c->nullable || c->primary_key) fl |= 1;
        if (c->primary_key) fl |= 2;
        if (c->unique) fl |= 4;
        if (c->flags & COLF_UNSIGNED) fl |= 32;
        if (c->auto_increment) fl |= 512;
        for (int k = 0; k < t->num_constraints; k++) {
            const Constraint* cs = &t->constraints[k];
            if (cs->kind != CONS_PRIMARY && cs->kind != CONS_UNIQUE) continue;
            for (int j = 0; j < cs->ncols; j++)
                if (cs->cols[j] == ci) {
                    if (cs->kind == CONS_PRIMARY) fl |= 2 | 1;
                    else if (cs->ncols == 1) fl |= 4;
                    else if (j == 0) fl |= 8;
                }
        }
        for (int k = 0; k < t->num_indexes; k++) {
            const Index* ix = &t->indexes[k];
            if (strcasecmp(ix->col_name, column)) continue;   // prima colonna dell'indice
            bool single = ix->ncols <= 1;
            if (ix->unique && single) fl |= 4;
            else if (!(fl & 2)) fl |= 8;
        }
        if (fl & 2) fl &= ~(unsigned)(4 | 8);                  // come MySQL: la chiave primaria vince
        else if (fl & 4) fl &= ~(unsigned)8;
    }
    if (locked) sv_rw_rdunlock(&g_db_rw);
    return fl;
}

// solo letture (per il registro di controllo): ogni istruzione è una SELECT senza INTO, SHOW o DESCRIBE,
// senza @variabili (assegnazioni), sequenze o funzioni dell'utente (possono scrivere). Deciso dal parser,
// non dalla prima parola: "SELECT f()" con f che scrive e "SELECT … INTO @v" non sono letture
bool db_statement_is_read(const char* sql) {
    if (!g_db.initialized || !sql) return false;
    TokenList* tokens = lexer_tokenize(sql);
    ParseResult* parsed = tokens ? parser_parse(tokens) : NULL;
    bool ok = parsed && parsed->success && parsed->count >= 1;
    for (int i = 0; ok && i < parsed->count; i++) {
        const ASTNode* n = parsed->statements[i];
        if (n->type == NODE_SELECT) ok = n->select_stmt.num_into == 0;
        else ok = n->type == NODE_SHOW || n->type == NODE_DESCRIBE;
    }
    bool locked = false;
    if (ok && g_lock_depth == 0) { sv_rw_rd(&g_db_rw); locked = true; }
    for (int i = 0; ok && tokens && i < tokens->count; i++) {
        const Token* t = &tokens->tokens[i];
        if (t->type != TOK_IDENTIFIER || !t->text) continue;
        if (t->text[0] == '@') { ok = false; break; }
        if (!strcasecmp(t->text, "NEXTVAL") || !strcasecmp(t->text, "SETVAL") || !strcasecmp(t->text, "NEXT")) { ok = false; break; }
        if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOK_LPAREN)
            for (int di = 0; ok && di < g_db.catalog->num_databases; di++) {
                const Database* d = &g_db.catalog->databases[di];
                for (int ri = 0; ri < d->num_routines && ok; ri++)
                    if (d->routines[ri].is_function && !strcasecmp(d->routines[ri].name, t->text)) ok = false;
            }
    }
    if (locked) sv_rw_rdunlock(&g_db_rw);
    if (parsed) parse_result_free(parsed);
    if (tokens) lexer_free(tokens);
    return ok;
}

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
              parsed->statements[0]->select_stmt.num_into == 0 && parsed->statements[0]->nqual_dbs <= 1 &&
              (!dbname || !dbname[0] || idx >= 0) && (!user || !user[0] || auth_user_exists(user));
    // @variabili (stato della sessione) e funzioni dell'utente (possono scrivere): via normale
    for (int i = 0; ok && tokens && i < tokens->count; i++) {
        const Token* t = &tokens->tokens[i];
        if (t->type != TOK_IDENTIFIER || !t->text) continue;
        if (t->text[0] == '@') { ok = false; break; }
        if (!strcasecmp(t->text, "NEXTVAL") || !strcasecmp(t->text, "SETVAL") || !strcasecmp(t->text, "NEXT")) { ok = false; break; }   // sequenze
        if (idx >= 0 && i + 1 < tokens->count && tokens->tokens[i + 1].type == TOK_LPAREN) {
            const Database* d = &g_db.catalog->databases[idx];
            for (int ri = 0; ri < d->num_routines && ok; ri++)
                if (d->routines[ri].is_function && !strcasecmp(d->routines[ri].name, t->text)) ok = false;
        }
    }
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
        rls = sv_malloc(sizeof(LockSet));
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
            ctx.user = user && user[0] ? user : NULL;  // permessi di chi legge (non della sessione attiva)
            ctx.priv_off = 0;
            ctx.auth_hook = NULL;
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
        BufferStats stv;
        bp_stats(g_db.bp, &stv);
        const BufferStats* st = &stv;
        out->page_size = SV_PAGE_SIZE;
        out->db_pages = (long long)(sv_lseek(g_db.bp->db_fd, 0, SEEK_END) / SV_PAGE_SIZE);
        out->pool_frames = g_db.bp->capacity;
        out->pool_used = g_db.bp->num_used;
        out->pool_requests = st->requests;
        out->pool_hits = st->hits;
        out->pool_misses = st->misses;
        out->pool_evictions = st->evictions;
        out->pages_written = st->dirty_writes;
        out->wal_bytes = g_db.wal ? (long long)wal_bytes(g_db.wal) : 0;
        out->wal_records = g_db.wal ? (unsigned long long)g_db.wal->sequence : 0;
        out->checkpoints = g_db.wal ? (unsigned long long)g_db.wal->checkpoints : 0;
        out->databases = g_db.catalog ? g_db.catalog->num_databases : 0;
        for (int i = 0; g_db.catalog && i < g_db.catalog->num_databases; i++) out->tables += g_db.catalog->databases[i].num_tables;
        out->in_transaction = g_db.exec_ctx.tx_id != 0;
        for (int i = 0; g_db.txm && i < g_db.txm->num_transactions; i++) if (g_db.txm->transactions[i].active) out->open_transactions++;
    }
    lk_stats(&out->lock_waits, &out->deadlocks);
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

// l'hash della password (PBKDF2, 100 000 giri) si calcola fuori dal lock del motore: un'ondata di login
// sbagliati non ferma più le query di tutti. Dopo una verifica riuscita, sotto il lock: si ricontrolla che sale
// e hash siano ancora quelli (password cambiata o utente eliminato nel frattempo), si ricorda SHA256(SHA256(pw))
// per caching_sha2_password e un hash di formato vecchio si rifà
bool db_login(const char* name, const char* password) {
    if (!name || !name[0] || !password) return false;
    char salt[33], hash[AUTH_HASH_MAX];
    db_lock();
    bool found = auth_user_secret(name, salt, hash);
    db_unlock();
    bool ok = found && auth_password_matches(salt, hash, password);
    if (ok) {
        db_lock();
        char s2[33], h2[AUTH_HASH_MAX];
        ok = auth_user_secret(name, s2, h2) && !strcmp(s2, salt) && !strcmp(h2, hash);
        if (ok) auth_password_verified(name, salt, hash, password);
        db_unlock();
    }
    memset(hash, 0, sizeof(hash));
    return ok;
}

int db_login_sha2(const char* user, const uint8_t nonce[20], const uint8_t* resp, size_t rlen) {
    db_lock();
    int r = user && user[0] ? auth_login_sha2(user, nonce, resp, rlen) : 0;
    db_unlock();
    return r;
}

bool db_user_has_native(const char* user) {
    db_lock();
    bool r = user && auth_has_native(user);
    db_unlock();
    return r;
}

unsigned long db_users_version(void) {
    db_lock();
    unsigned long v = auth_users_version();
    db_unlock();
    return v;
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

bool db_endpoint_runas(const char* db, const char* definer, char* user_out, size_t un, bool* may_public,
                       char* err, size_t errn) {
    user_out[0] = '\0';
    *may_public = true;
    db_lock();
    bool ok = true;
    if (auth_user_count() == 0) {}                     // nessun utente: tutti hanno tutti i permessi
    else if (definer && definer[0]) {                  // salvato da un utente: i suoi permessi
        if (!auth_user_exists(definer)) {
            snprintf(err, errn, "the user who saved this endpoint ('%.63s') no longer exists", definer);
            ok = false;
        } else {
            snprintf(user_out, un, "%s", definer);
            *may_public = auth_is_admin(definer);
        }
    } else if (definer) {}                             // "" = salvato da root (token, GUI)
    // senza autore registrato (tabella di prima della 1.3): tutti i permessi solo se nessun utente
    // che non sia amministratore può averla scritta
    else if (!auth_only_admins(db, AUTH_ENDPOINTS_TABLE, AUTH_INSERT | AUTH_UPDATE | AUTH_ALTER | AUTH_DROP)) {
        snprintf(err, errn, AUTH_ENDPOINTS_TABLE " in '%.63s' can be changed by users who are not administrators, and this "
                 "endpoint has no author: an administrator must run ALTER TABLE " AUTH_ENDPOINTS_TABLE
                 " ADD COLUMN definer VARCHAR(64) and save the endpoint again", db);
        ok = false;
    }
    db_unlock();
    return ok;
}

size_t db_value_bytes(const char* v, char* out) { return v ? sv_bin_decode(v, strlen(v), out) : 0; }

void db_set_cache_size(long mb) { g_cache_mb = mb > 0 ? mb : 0; }
long db_cache_size(void) { return g_db.bp ? (long)((long long)g_db.bp->capacity * SV_PAGE_SIZE / (1024 * 1024)) : g_cache_mb; }

bool db_set_synchronous(const char* mode) {
    for (int i = 0; i < 3; i++)
        if (mode && !strcasecmp(mode, SYNC_NAMES[i])) { db_lock(); g_sync_mode = i; db_unlock(); return true; }
    return false;
}

const char* db_synchronous(void) { return SYNC_NAMES[g_sync_mode]; }

void db_set_thread_cancel(atomic_int* flag) { sv_scan_set_cancel(flag); }

// colonne di una SELECT senza eseguirla (PREPARE del protocollo MySQL): niente righe lette, nessun lock,
// nessun effetto (NEXTVAL, funzioni dell'utente, FOR UPDATE). NULL se non è una SELECT sola
QueryResult* db_describe(const char* sql) {
    if (!g_db.initialized || !sql) return NULL;
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    QueryResult* r = NULL;
    if (pr && !pr->success) r = result_error(pr->error);
    else if (pr && pr->count == 1 && pr->statements[0]->type == NODE_SELECT && pr->statements[0]->select_stmt.num_into == 0) {
        db_lock();
        char err[300];
        if (g_session_user[0] && !auth_check(g_session_user, pr->statements[0], cur_db_name(), err, sizeof(err))) r = result_error(err);
        else {
            g_db.exec_ctx.user = g_session_user[0] ? g_session_user : NULL;
            g_db.exec_ctx.priv_off = 0;
            executor_set_describe(true);
            r = executor_run(&g_db.exec_ctx, pr->statements[0]);
            executor_set_describe(false);
        }
        db_unlock();
    }
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    return r;
}

void db_sync_tick(void) {
    if (!g_db.initialized || g_lock_depth) return;
    if (!sv_rw_trywr(&g_db_rw)) return;              // il motore è occupato: lo statement successivo farà il sync
    g_lock_depth = 1;
    double now = get_time_ms();
    if (g_db.initialized && g_sync_mode >= 1 && g_db.wal && g_db.wal->unsynced && !g_db.exec_ctx.tx_id && !io_failed() &&
        now - g_last_sync >= SYNC_NORMAL_MS && wal_flush(g_db.wal) == SV_OK) g_last_sync = now;
    pf_process();                                    // catene liberate ora che il log è su disco
    db_unlock();
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
        char* q = sv_realloc(*p, nc);
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
    char* out = sv_malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    char head[400];
    // viste in ordine di dipendenza: una vista va dopo quelle che nomina (ciascuna viene controllata
    // quando si ricrea); con un ciclo apparente si prosegue nell'ordine del catalogo
    bool* done = what == 0 ? sv_calloc((size_t)db->num_views + 1, 1) : NULL;
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
        snprintf(head, sizeof(head), "CREATE %s %.64s", db->routines[i].is_function ? "FUNCTION" : "PROCEDURE", db->routines[i].name);
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
static const char* const BK_FILES[5] = { "segvault.db", "segvault.cat", "segvault.free", "users.dat", "sequences.txt" };

static bool bk_copy(const char* from, const char* to, bool must_exist, char* err, size_t errn) {
    int in = sv_open(from, SV_O_RDONLY, 0);
    if (in < 0) {
        if (!must_exist) { remove(to); return true; }
        snprintf(err, errn, "cannot read %.400s", from);
        return false;
    }
    int out = sv_open(to, SV_O_WRONLY | SV_O_CREAT | SV_O_TRUNC, 0600);
    if (out < 0) { sv_close(in); snprintf(err, errn, "cannot write %.400s", to); return false; }
    char* buf = sv_malloc(1 << 20);
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
    for (int i = 0; i < 5; i++) {
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
    executor_uservars_free(executor_uservars());          // @variabili della sessione predefinita
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
