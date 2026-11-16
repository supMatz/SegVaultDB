// Benchmark riproducibile (P3.6): gli stessi scenari, con lo stesso SQL, su SegVault (in-process e, con
// --server, PDO sul protocollo MySQL) e su SQLite (in-process). Risultato in JSON su stdout, sempre come
// "più alto = meglio" (operazioni o righe al secondo), così bench/compare.py confronta e disegna la tabella.
//
//   make bench                         tabella + confronto con bench/baseline.json
//   ./bench_suite [--scale K] [--quick] [--server PATH]   > risultati.json
//
// Scale: K (default 1) moltiplica le righe; --quick divide per 4 (per la CI).
#define _GNU_SOURCE
#include "src/bridge/db_api.h"
#include "include/common.h"
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double g_scale = 1;
static int scaled(int n) { int v = (int)(n * g_scale); return v < 10 ? 10 : v; }

// ── i due motori dietro la stessa interfaccia ─────────────────────

typedef struct Eng Eng;
struct Eng {
    const char* name;
    char        dir[256];
    sqlite3*    lite;
    bool (*exec)(Eng* e, const char* sql);                      // statement senza righe da leggere
    long (*query)(Eng* e, const char* sql);                     // righe restituite (-1 = errore)
    void* (*prepare)(Eng* e, const char* sql);
    long (*run_prepared)(Eng* e, void* p, long long v);         // un parametro intero
    void (*free_prepared)(Eng* e, void* p);
    bool (*sync)(Eng* e, const char* mode);                     // FULL / NORMAL / OFF
    bool (*reopen)(Eng* e);                                     // avvio a freddo sui dati esistenti
    void (*close)(Eng* e);
};

static void die(const char* what, const char* err) { fprintf(stderr, "bench: %s: %s\n", what, err ? err : "?"); exit(1); }

// SegVault
static bool sv_exec(Eng* e, const char* sql) {
    (void)e;
    QueryResult* r = db_execute(sql);
    bool ok = r && r->success;
    if (!ok) die(sql, r ? r->error : "null");
    db_result_free(r);
    return ok;
}
static long sv_query(Eng* e, const char* sql) {
    (void)e;
    QueryResult* r = db_execute(sql);
    long n = r && r->success ? r->num_rows : -1;
    if (n < 0) die(sql, r ? r->error : "null");
    db_result_free(r);
    return n;
}
static void* sv_prepare(Eng* e, const char* sql) { (void)e; void* p = db_prepared_new(sql); if (!p) die("prepare", sql); return p; }
static long sv_run_prepared(Eng* e, void* p, long long v) {
    (void)e;
    char lit[32];
    snprintf(lit, sizeof(lit), "%lld", v);
    const char* l[1] = { lit };
    QueryResult* r = db_prepared_exec(p, l, 1);
    long n = r && r->success ? (r->num_cols ? r->num_rows : (long)r->rows_affected) : -1;
    if (n < 0) die("prepared", r ? r->error : "null");
    db_result_free(r);
    return n;
}
static void sv_free_prepared(Eng* e, void* p) { (void)e; db_prepared_free(p); }
static bool sv_sync(Eng* e, const char* mode) { (void)e; return db_set_synchronous(mode); }
static bool sv_reopen(Eng* e) {
    db_shutdown();
    if (!db_init(e->dir)) die("reopen", db_init_error());
    return sv_exec(e, "USE b");
}
static void sv_close(Eng* e) { (void)e; db_shutdown(); }

// SQLite (WAL, come si usa in un'app)
static bool lt_exec(Eng* e, const char* sql) {
    char* err = NULL;
    if (sqlite3_exec(e->lite, sql, NULL, NULL, &err) != SQLITE_OK) die(sql, err);
    return true;
}
static int lt_count_cb(void* n, int nc, char** v, char** c) { (void)nc; (void)v; (void)c; (*(long*)n)++; return 0; }
static long lt_query(Eng* e, const char* sql) {
    long n = 0;
    char* err = NULL;
    if (sqlite3_exec(e->lite, sql, lt_count_cb, &n, &err) != SQLITE_OK) die(sql, err);
    return n;
}
static void* lt_prepare(Eng* e, const char* sql) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(e->lite, sql, -1, &st, NULL) != SQLITE_OK) die("prepare", sqlite3_errmsg(e->lite));
    return st;
}
static long lt_run_prepared(Eng* e, void* p, long long v) {
    sqlite3_stmt* st = p;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, v);
    long n = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) n++;
    if (rc != SQLITE_DONE) die("step", sqlite3_errmsg(e->lite));
    return n ? n : sqlite3_changes(e->lite);
}
static void lt_free_prepared(Eng* e, void* p) { (void)e; sqlite3_finalize(p); }
static bool lt_sync(Eng* e, const char* mode) {
    char q[64];
    snprintf(q, sizeof(q), "PRAGMA synchronous = %s", mode);
    return lt_exec(e, q);
}
static bool lt_open(Eng* e) {
    char path[300];
    snprintf(path, sizeof(path), "%s/b.sqlite", e->dir);
    if (sqlite3_open(path, &e->lite) != SQLITE_OK) die("sqlite open", sqlite3_errmsg(e->lite));
    sqlite3_busy_timeout(e->lite, 10000);
    lt_exec(e, "PRAGMA journal_mode = WAL");
    return true;
}
static bool lt_reopen(Eng* e) { sqlite3_close(e->lite); e->lite = NULL; lt_open(e); lt_query(e, "SELECT COUNT(*) FROM s"); return true; }
static void lt_close(Eng* e) { sqlite3_close(e->lite); e->lite = NULL; }

static const char* bench_dir(void) {
    static char d[200];
    if (!d[0]) {
        const char* e = getenv("BENCH_DIR");
        snprintf(d, sizeof(d), "%s", e && e[0] ? e : ".");
        mkdir(d, 0700);
    }
    return d;
}

static Eng make_eng(bool lite) {
    Eng e;
    memset(&e, 0, sizeof(e));
    // su un disco vero: in /tmp (spesso tmpfs) fsync non costa niente e i commit sembrerebbero gratis
    snprintf(e.dir, sizeof(e.dir), "%s/svbench_%s_XXXXXX", bench_dir(), lite ? "sqlite" : "segvault");
    if (!mkdtemp(e.dir)) die("mkdtemp", e.dir);
    if (lite) {
        e.name = "sqlite"; e.exec = lt_exec; e.query = lt_query; e.prepare = lt_prepare; e.run_prepared = lt_run_prepared;
        e.free_prepared = lt_free_prepared; e.sync = lt_sync; e.reopen = lt_reopen; e.close = lt_close;
        lt_open(&e);
        lt_sync(&e, "NORMAL");
    } else {
        e.name = "segvault"; e.exec = sv_exec; e.query = sv_query; e.prepare = sv_prepare; e.run_prepared = sv_run_prepared;
        e.free_prepared = sv_free_prepared; e.sync = sv_sync; e.reopen = sv_reopen; e.close = sv_close;
        if (!db_init(e.dir)) die("init", db_init_error());
        sv_exec(&e, "CREATE DATABASE b");
        sv_exec(&e, "USE b");
        db_set_synchronous("NORMAL");
    }
    return e;
}

// ── risultati ────────────────────────────────────────────────────

#define MAXS 64
typedef struct { char name[48]; char unit[16]; char what[120]; double v[3]; bool has[3]; } Res;   // 0 segvault, 1 sqlite, 2 mariadb
static Res g_res[MAXS];
static int g_nres;

static Res* res(const char* name, const char* unit, const char* what) {
    for (int i = 0; i < g_nres; i++) if (!strcmp(g_res[i].name, name)) return &g_res[i];
    Res* r = &g_res[g_nres++];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->unit, sizeof(r->unit), "%s", unit);
    snprintf(r->what, sizeof(r->what), "%s", what);
    return r;
}
static void put(const char* name, const char* unit, const char* what, int eng, double v) {
    Res* r = res(name, unit, what);
    r->v[eng] = v;
    r->has[eng] = true;
    fprintf(stderr, "  %-28s %-9s %14.1f %s\n", name, eng == 0 ? "segvault" : eng == 1 ? "sqlite" : "mariadb", v, unit);
}

// ── scenari ──────────────────────────────────────────────────────

static char g_buf[1 << 22];

static void load(Eng* e, int id, int rows) {
    e->exec(e, "CREATE TABLE s (id INT PRIMARY KEY, nome VARCHAR(40), citta VARCHAR(20), anno INT)");
    e->exec(e, "CREATE TABLE x (id INT PRIMARY KEY, s INT, corso INT, voto INT)");
    static const char* C[] = { "Milano", "Roma", "Torino", "Napoli", "Bologna", "Pisa", "Padova", "Bari" };
    double t0 = now_ms();
    e->exec(e, "BEGIN");
    for (int i = 0; i < rows; ) {
        size_t len = (size_t)sprintf(g_buf, "INSERT INTO s VALUES ");
        for (int k = 0; k < 500 && i < rows; k++, i++)
            len += (size_t)sprintf(g_buf + len, "%s(%d, 'Studente %d', '%s', %d)", k ? "," : "", i, i, C[i % 8], 1 + i % 5);
        e->exec(e, g_buf);
    }
    int ex = rows * 4;
    for (int i = 0; i < ex; ) {
        size_t len = (size_t)sprintf(g_buf, "INSERT INTO x VALUES ");
        for (int k = 0; k < 500 && i < ex; k++, i++)
            len += (size_t)sprintf(g_buf + len, "%s(%d, %d, %d, %d)", k ? "," : "", i, (i * 7) % rows, i % 50, 18 + i % 13);
        e->exec(e, g_buf);
    }
    e->exec(e, "COMMIT");
    double ms = now_ms() - t0;
    put("bulk_load", "rows/s", "INSERT in blocks of 500 rows, one transaction", id, (rows + ex) / (ms / 1000));
}

static void point(Eng* e, int id, int rows) {
    int n = scaled(20000);
    char q[128];
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        snprintf(q, sizeof(q), "SELECT nome, citta FROM s WHERE id = %d", (int)((i * 7919LL) % rows));
        if (e->query(e, q) != 1) die("point", q);
    }
    put("point_lookup", "queries/s", "SELECT by primary key, SQL text each time", id, n / ((now_ms() - t0) / 1000));
    void* p = e->prepare(e, "SELECT nome, citta FROM s WHERE id = ?");
    t0 = now_ms();
    for (int i = 0; i < n; i++) if (e->run_prepared(e, p, (i * 7919LL) % rows) != 1) die("point prepared", "row count");
    put("point_lookup_prepared", "queries/s", "the same, prepared once (bound integer)", id, n / ((now_ms() - t0) / 1000));
    e->free_prepared(e, p);
}

static void range(Eng* e, int id, int rows) {
    int n = scaled(200), span = 1000 < rows ? 1000 : rows / 2;
    char q[160];
    long got = 0;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        int a = (int)((i * 104729LL) % (rows - span));
        snprintf(q, sizeof(q), "SELECT id, nome FROM s WHERE id BETWEEN %d AND %d", a, a + span - 1);
        got += e->query(e, q);
    }
    put("range_scan", "rows/s", "1000-row ranges on the primary key", id, got / ((now_ms() - t0) / 1000));
}

static void join(Eng* e, int id) {
    int n = 3;
    double t0 = now_ms();
    for (int i = 0; i < n; i++)
        e->query(e, "SELECT s.citta, COUNT(*), AVG(x.voto) FROM x JOIN s ON s.id = x.s WHERE x.voto >= 25 GROUP BY s.citta");
    put("join_group", "queries/s", "JOIN of both tables + filter + GROUP BY", id, n / ((now_ms() - t0) / 1000));
    t0 = now_ms();
    for (int i = 0; i < n; i++) e->query(e, "SELECT corso, COUNT(*), AVG(voto) FROM x GROUP BY corso");
    put("scan_aggregate", "queries/s", "full scan + GROUP BY", id, n / ((now_ms() - t0) / 1000));
}

static void updates(Eng* e, int id, int rows) {
    int n = scaled(20000);
    char q[128];
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        if (i % 100 == 0) e->exec(e, "BEGIN");
        snprintf(q, sizeof(q), "UPDATE s SET anno = anno + 1 WHERE id = %d", (int)((i * 7919LL) % rows));
        e->exec(e, q);
        if (i % 100 == 99 || i == n - 1) e->exec(e, "COMMIT");
    }
    put("update_heavy", "updates/s", "UPDATE by primary key, 100 per transaction", id, n / ((now_ms() - t0) / 1000));
}

static void commits(Eng* e, int id) {
    static const char* M[] = { "FULL", "NORMAL", "OFF" };
    static const char* N[] = { "commit_full", "commit_normal", "commit_off" };
    e->exec(e, "CREATE TABLE c (id INT PRIMARY KEY, v INT)");
    int base = 0;
    for (int m = 0; m < 3; m++) {
        e->sync(e, M[m]);
        int n = m == 0 ? scaled(200) : scaled(2000);
        char q[96];
        double t0 = now_ms();
        for (int i = 0; i < n; i++) { snprintf(q, sizeof(q), "INSERT INTO c VALUES (%d, %d)", base + i, i); e->exec(e, q); }
        char what[120];
        snprintf(what, sizeof(what), "single-row INSERT in autocommit, synchronous = %s", M[m]);
        put(N[m], "commits/s", what, id, n / ((now_ms() - t0) / 1000));
        base += n;
    }
    e->sync(e, "NORMAL");
}

static void rollbacks(Eng* e, int id) {
    int n = scaled(500);
    double t0 = now_ms();
    for (int k = 0; k < n; k++) {
        e->exec(e, "BEGIN");
        size_t len = (size_t)sprintf(g_buf, "INSERT INTO c VALUES ");
        for (int j = 0; j < 10; j++) len += (size_t)sprintf(g_buf + len, "%s(%d, %d)", j ? "," : "", 10000000 + k * 10 + j, j);
        e->exec(e, g_buf);
        e->exec(e, "ROLLBACK");
    }
    put("rollback", "rollbacks/s", "BEGIN; INSERT 10 rows; ROLLBACK on the loaded database", id, n / ((now_ms() - t0) / 1000));
}

static void cold(Eng* e, int id) {
    int n = 5;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) e->reopen(e);
    put("cold_start", "starts/s", "close and reopen the loaded database, first query", id, n / ((now_ms() - t0) / 1000));
}

// lettori e scrittori in parallelo: ognuno con la sua connessione (SQLite) o sessione (SegVault)
typedef struct { Eng* e; int rows; bool writer; atomic_long* done; atomic_int* stop; int tid; } Worker;

static void* worker(void* a) {
    Worker* w = a;
    char q[128];
    sqlite3* lite = NULL;
    DbSession* ds = NULL;
    if (w->e->lite) {
        char path[300];
        snprintf(path, sizeof(path), "%s/b.sqlite", w->e->dir);
        sqlite3_open(path, &lite);
        sqlite3_busy_timeout(lite, 10000);
        sqlite3_exec(lite, "PRAGMA synchronous = NORMAL", NULL, NULL, NULL);   // vale per connessione
    } else if (w->writer) ds = db_session_new();
    long k = 0;
    while (!atomic_load(w->stop)) {
        long i = (k * 7919L + w->tid * 104729L) % w->rows;
        if (w->writer) snprintf(q, sizeof(q), "UPDATE s SET anno = anno + 1 WHERE id = %ld", i);
        else snprintf(q, sizeof(q), "SELECT nome FROM s WHERE id = %ld", i);
        bool ok;
        if (lite) ok = sqlite3_exec(lite, q, NULL, NULL, NULL) == SQLITE_OK;
        else if (w->writer) {
            while (!db_session_enter(ds)) { if (atomic_load(w->stop)) break; usleep(50); }
            if (atomic_load(w->stop)) break;
            if (k == 0) db_result_free(db_execute("USE b"));   // una sessione nuova non ha un database corrente
            QueryResult* r = db_execute(q);
            ok = r && r->success;
            db_result_free(r);
            db_session_leave(ds);
        } else {
            bool refused = false;
            QueryResult* r = db_execute_readonly(q, "b", NULL, 0, &refused);
            ok = r && r->success && r->num_rows == 1;
            db_result_free(r);
        }
        if (ok) atomic_fetch_add(w->done, 1);
        k++;
    }
    if (lite) sqlite3_close(lite);
    if (ds) db_session_end(ds);
    return NULL;
}

static void concurrent(Eng* e, int id, int rows) {
    static const int R[] = { 1, 4, 16 };
    double secs = g_scale < 1 ? 0.5 : 1.0;
    for (int mode = 0; mode < 2; mode++)
        for (int ri = 0; ri < 3; ri++) {
            int nt = R[ri];
            if (mode == 1 && nt == 16) continue;                // scrittori: 1 e 4
            atomic_long done = 0;
            atomic_int stop = 0;
            pthread_t th[16];
            Worker w[16];
            for (int t = 0; t < nt; t++) { w[t] = (Worker){ e, rows, mode == 1, &done, &stop, t }; pthread_create(&th[t], NULL, worker, &w[t]); }
            usleep((useconds_t)(secs * 1e6));
            atomic_store(&stop, 1);
            for (int t = 0; t < nt; t++) pthread_join(th[t], NULL);
            char name[48], what[120];
            snprintf(name, sizeof(name), "%s_%d", mode ? "writers" : "readers", nt);
            snprintf(what, sizeof(what), "%d concurrent %s (point %s), own connection each", nt, mode ? "writers" : "readers",
                     mode ? "UPDATE, autocommit" : "SELECT");
            put(name, "ops/s", what, id, (double)done / secs);
        }
}

// PDO sul protocollo MySQL: segvault-server avviato qui, uno script PHP con 5000 ricerche preparate
static void pdo(const char* server, int rows) {
    if (system("php -n -d extension=pdo_mysql -r 'exit(class_exists(\"PDO\") && in_array(\"mysql\", PDO::getAvailableDrivers()) ? 0 : 1);' >/dev/null 2>&1") != 0) {
        fprintf(stderr, "  (php with pdo_mysql not found: PDO scenario skipped)\n");
        return;
    }
    char dir[260];
    snprintf(dir, sizeof(dir), "%s/svbench_pdo_XXXXXX", bench_dir());
    if (!mkdtemp(dir)) return;
    int port = 23000 + (int)(getpid() % 2000);
    pid_t pid = fork();
    if (pid == 0) {
        char ps[16];
        snprintf(ps, sizeof(ps), "%d", port);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl(server, server, "-p", "0", "--mysql-port", ps, "-q", dir, (char*)NULL);
        _exit(127);
    }
    usleep(500000);
    int n = scaled(5000);
    char cmd[2600];
    snprintf(cmd, sizeof(cmd),
             "php -n -d extension=pdo_mysql -r '"
             "$d = new PDO(\"mysql:host=127.0.0.1;port=%d\", \"root\", \"\", [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_EMULATE_PREPARES => false]);"
             "$d->exec(\"CREATE DATABASE b\"); $d->exec(\"USE b\");"
             "$d->exec(\"CREATE TABLE s (id INT PRIMARY KEY, nome VARCHAR(40))\");"
             "for ($i = 0; $i < %d; $i += 500) { $v = []; for ($k = $i; $k < min($i + 500, %d); $k++) $v[] = \"($k, \\\"n$k\\\")\";"
             "  $d->exec(\"INSERT INTO s VALUES \" . implode(\",\", $v)); }"
             "$st = $d->prepare(\"SELECT nome FROM s WHERE id = ?\"); $t = microtime(true);"
             "for ($i = 0; $i < %d; $i++) { $st->execute([($i * 7919) %% %d]); $st->fetchAll(); }"
             "echo %d / (microtime(true) - $t);' 2>&1",
             port, rows, rows, n, rows, n);
    FILE* p = popen(cmd, "r");
    char out[256] = "";
    size_t got = p ? fread(out, 1, sizeof(out) - 1, p) : 0;
    out[got] = '\0';
    if (p) pclose(p);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    char rm[300];
    snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    if (system(rm)) {}
    double v = atof(out);
    if (v > 0) put("pdo_point_lookup", "queries/s", "PHP PDO over the MySQL protocol, prepared point lookups", 0, v);
    else fprintf(stderr, "  (PDO scenario failed: %.200s)\n", out);
}

static void run_engine(bool lite) {
    int rows = scaled(50000);
    Eng e = make_eng(lite);
    int id = lite ? 1 : 0;
    fprintf(stderr, "%s, %d rows:\n", e.name, rows);
    load(&e, id, rows);
    point(&e, id, rows);
    range(&e, id, rows);
    join(&e, id);
    updates(&e, id, rows);
    commits(&e, id);
    rollbacks(&e, id);
    concurrent(&e, id, rows);
    cold(&e, id);
    e.close(&e);
    char rm[300];
    snprintf(rm, sizeof(rm), "rm -rf %s", e.dir);
    if (system(rm)) {}
}

int main(int argc, char** argv) {
    const char* server = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quick")) g_scale = 0.25;
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
    }
    run_engine(false);
    run_engine(true);
    if (server) pdo(server, scaled(50000));
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    printf("{\n  \"segvault\": \"%s\",\n  \"sqlite\": \"%s\",\n  \"scale\": %g,\n  \"cpus\": %ld,\n  \"scenarios\": [\n",
           SV_VERSION_STR, sqlite3_libversion(), g_scale, cpus);
    for (int i = 0; i < g_nres; i++) {
        Res* r = &g_res[i];
        printf("    {\"name\": \"%s\", \"unit\": \"%s\", \"what\": \"%s\"", r->name, r->unit, r->what);
        static const char* K[3] = { "segvault", "sqlite", "mariadb" };
        for (int k = 0; k < 3; k++) if (r->has[k]) printf(", \"%s\": %.1f", K[k], r->v[k]);
        printf("}%s\n", i + 1 < g_nres ? "," : "");
    }
    printf("  ]\n}\n");
    return 0;
}
