// Durabilità e recupero (P0.4, P0.5, P0.6, P0.9): errori di scrittura / fsync iniettati, arresti a metà di
// ROLLBACK e a metà del recupero. Compilato con -DSV_FAULT_INJECTION: ogni write / pwrite / fsync del motore
// passa da qui.
#include "src/bridge/db_api.h"
#include "src/storage/page.h"
#include <fcntl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── iniezione dei guasti ──
// fail_*: la N-esima chiamata (da adesso) fallisce con EIO; exit_*: alla N-esima il processo finisce
// subito (_exit, come un kill: niente flush, niente distruttori). 0 = spento.
static long g_fail_write, g_fail_fsync, g_exit_pwrite, g_exit_write, g_fail_pread;
static long g_fsyncs;                                       // fsync eseguiti (contatore per i test di synchronous)

static bool hit(long* c) { return *c > 0 && --*c == 0; }

// ── caduta di corrente simulata (P3.1) ──
// Con g_power attivo ogni scrittura salva il contenuto di prima (e la lunghezza del file) finché un fsync dello
// stesso file non la rende definitiva. Alla g_power_at-esima scrittura il processo scrive il diario delle
// scritture non definitive in POWER_JOURNAL e muore; power_restore() (nel genitore) le annulla dal fondo: sul
// disco resta solo quello che un fsync aveva confermato, come dopo un'interruzione di corrente.
#define POWER_JOURNAL "/tmp/sv_dur_power.journal"
typedef struct { int fd; off_t off; size_t n; off_t oldsize; uint8_t* old; } PwEntry;
static bool g_power;
static long g_power_at;
static PwEntry* g_pw;
static int g_npw, g_cappw;

static void power_note(int fd, off_t off, size_t n) {
    if (!g_power) return;
    if (g_npw == g_cappw) { g_cappw = g_cappw ? g_cappw * 2 : 1024; g_pw = realloc(g_pw, sizeof(PwEntry) * (size_t)g_cappw); }
    PwEntry* e = &g_pw[g_npw++];
    e->fd = fd; e->off = off; e->n = n;
    e->oldsize = lseek(fd, 0, SEEK_END);
    e->old = malloc(n ? n : 1);
    ssize_t r = pread(fd, e->old, n, off);
    if (r < 0) r = 0;
    if ((size_t)r < n) memset(e->old + r, 0, n - (size_t)r);
}

static void power_cut(void) {                           // il diario su file, poi giù la corrente
    FILE* f = fopen(POWER_JOURNAL, "wb");
    for (int i = 0; f && i < g_npw; i++) {
        char link[64], path[512];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", g_pw[i].fd);
        ssize_t l = readlink(link, path, sizeof(path) - 1);
        if (l <= 0) continue;
        path[l] = 0;
        uint32_t pl = (uint32_t)l;
        int64_t off = g_pw[i].off, os = g_pw[i].oldsize;
        uint64_t n = g_pw[i].n;
        fwrite(&pl, 4, 1, f); fwrite(path, 1, pl, f); fwrite(&off, 8, 1, f); fwrite(&os, 8, 1, f); fwrite(&n, 8, 1, f);
        fwrite(g_pw[i].old, 1, (size_t)n, f);
    }
    if (f) fclose(f);
    _exit(0);
}

static int power_restore(void) {                        // nel genitore: annulla le scritture non definitive
    FILE* f = fopen(POWER_JOURNAL, "rb");
    if (!f) return 0;
    typedef struct { char path[512]; int64_t off, os; uint64_t n; uint8_t* old; } E;
    E* v = NULL;
    int nv = 0, cap = 0;
    for (;;) {
        uint32_t pl;
        if (fread(&pl, 4, 1, f) != 1 || pl >= 512) break;
        if (nv == cap) { cap = cap ? cap * 2 : 256; v = realloc(v, sizeof(E) * (size_t)cap); }
        E* e = &v[nv];
        if (fread(e->path, 1, pl, f) != pl) break;
        e->path[pl] = 0;
        if (fread(&e->off, 8, 1, f) != 1 || fread(&e->os, 8, 1, f) != 1 || fread(&e->n, 8, 1, f) != 1) break;
        e->old = malloc(e->n ? e->n : 1);
        if (fread(e->old, 1, (size_t)e->n, f) != e->n) { free(e->old); break; }
        nv++;
    }
    fclose(f);
    for (int i = nv - 1; i >= 0; i--) {                 // dal fondo: ogni scrittura torna come prima
        int fd = open(v[i].path, O_RDWR);
        if (fd >= 0) {
            if (pwrite(fd, v[i].old, (size_t)v[i].n, v[i].off) < 0) {}
            if (ftruncate(fd, v[i].os) != 0) {}
            close(fd);
        }
        free(v[i].old);
    }
    free(v);
    remove(POWER_JOURNAL);
    return nv;
}

static void power_write_hook(int fd, off_t off, size_t n) {
    if (!g_power) return;
    if (g_power_at > 0 && --g_power_at == 0) power_cut();
    power_note(fd, off, n);
}

ssize_t sv_fi_write(int fd, const void* buf, size_t count) {
    if (hit(&g_exit_write)) _exit(0);
    if (hit(&g_fail_write)) { errno = EIO; return -1; }
    if (g_power) {                                       // write(): alla posizione corrente o in fondo (O_APPEND)
        int fl = fcntl(fd, F_GETFL);
        off_t at = (fl >= 0 && (fl & O_APPEND)) ? lseek(fd, 0, SEEK_END) : lseek(fd, 0, SEEK_CUR);
        power_write_hook(fd, at, count);
    }
    return write(fd, buf, count);
}
ssize_t sv_fi_pwrite(int fd, const void* buf, size_t count, off_t off) {
    if (hit(&g_exit_pwrite)) _exit(0);
    power_write_hook(fd, off, count);
    return pwrite(fd, buf, count, off);
}
ssize_t sv_fi_pread(int fd, void* buf, size_t count, off_t off) {
    if (hit(&g_fail_pread)) { errno = EIO; return -1; }
    return pread(fd, buf, count, off);
}
int sv_fi_fsync(int fd) {
    if (hit(&g_fail_fsync)) { errno = EIO; return -1; }
    g_fsyncs++;
    int r = fsync(fd);
    if (r == 0 && g_power) {                             // definitive le scritture di questo file
        int k = 0;
        for (int i = 0; i < g_npw; i++) {
            if (g_pw[i].fd == fd) { free(g_pw[i].old); continue; }
            g_pw[k++] = g_pw[i];
        }
        g_npw = k;
    }
    return r;
}
// allocazioni del motore (include/sv_alloc.h): la N-esima fallisce; g_allocs le conta
static long g_fail_alloc, g_allocs;
int sv_fi_alloc_fail(void) { g_allocs++; return hit(&g_fail_alloc); }

static void faults_off(void) { g_fail_write = g_fail_fsync = g_exit_pwrite = g_exit_write = g_fail_pread = 0; }

// ── aiuti ──
static QueryResult* q(const char* sql) { return db_execute(sql); }
static bool ok(const char* sql) { QueryResult* r = q(sql); bool g = r && r->success; if (!g) printf("  (%s -> %s)\n", sql, r ? r->error : "null"); db_result_free(r); return g; }
static bool err_has(const char* sql, const char* text) { QueryResult* r = q(sql); bool g = r && !r->success && strstr(r->error, text); if (!g) printf("  (%s -> %s)\n", sql, r ? (r->success ? "ok" : r->error) : "null"); db_result_free(r); return g; }
static long long num(const char* sql) {
    QueryResult* r = q(sql);
    long long v = r && r->success && r->num_rows && !r->rows[0].cells[0].is_null ? atoll(r->rows[0].cells[0].value) : -1;
    db_result_free(r);
    return v;
}
static void sh(const char* cmd) { if (system(cmd) != 0) { printf("command failed: %s\n", cmd); exit(1); } }

#define BASE  "/tmp/sv_dur_base"
#define WORK  "/tmp/sv_dur_work"
#define CRASH "/tmp/sv_dur_crash"
#define N 600

// stato atteso dopo ogni prova: le N righe confermate, nessuna duplicata, valori originali
static void check_committed(const char* what, int k) {
    long long c = num("SELECT COUNT(*) FROM t"), d = num("SELECT COUNT(DISTINCT id) FROM t"), s = num("SELECT SUM(v) FROM t");
    CHECK(c == N && d == N && s == (long long)N * (N + 1) / 2, "%s (k=%d): count %lld, distinct %lld, sum %lld", what, k, c, d, s);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sh("rm -rf " BASE " " WORK " " CRASH " && mkdir -p " BASE);
    if (!db_init(BASE)) { printf("init: %s\n", db_init_error()); return 1; }
    ok("CREATE DATABASE d");
    ok("USE d");
    ok("CREATE TABLE t (id INT PRIMARY KEY, v INT, pad VARCHAR(200))");
    ok("BEGIN");
    for (int i = 1; i <= N; i++) {
        char s[400];
        snprintf(s, sizeof(s), "INSERT INTO t VALUES (%d, %d, '%0150d')", i, i, i);
        ok(s);
    }
    ok("COMMIT");
    ok("CREATE USER perm IDENTIFIED BY 'perm-pass'");
    db_shutdown();
    {   // P2.6: i file del motore sono del solo proprietario
        const char* files[] = { BASE "/segvault.db", BASE "/segvault.cat", BASE "/users.dat" };
        for (int i = 0; i < 3; i++) {
            struct stat st;
            CHECK(stat(files[i], &st) == 0 && (st.st_mode & 077) == 0, "%s has no group / other permissions (mode %o)", files[i],
                  (unsigned)(st.st_mode & 0777));
        }
    }

    // ── 1. fsync del COMMIT non riuscito: errore, motore fermo, dopo il riavvio la transazione non c'è ──
    sh("rm -rf " WORK " && cp -r " BASE " " WORK);
    CHECK(db_init(WORK), "init work: %s", db_init_error());
    ok("USE d");
    ok("BEGIN");
    ok("INSERT INTO t VALUES (10001, 1, 'x')");
    ok("DELETE FROM t WHERE id <= 100");
    g_fail_fsync = 1;                                   // l'fsync del COMMIT
    CHECK(err_has("COMMIT", "COMMIT failed"), "COMMIT reports the failed fsync");
    faults_off();
    CHECK(err_has("SELECT COUNT(*) FROM t", "I/O error") && err_has("INSERT INTO t VALUES (10002, 1, 'x')", "I/O error"),
          "engine stopped after the I/O error");
    db_shutdown();
    CHECK(db_init(WORK), "restart after a failed COMMIT: %s", db_init_error());
    ok("USE d");
    check_committed("failed COMMIT not applied", 0);
    CHECK(num("SELECT COUNT(*) FROM t WHERE id = 10001") == 0, "row of the failed COMMIT absent");
    CHECK(ok("INSERT INTO t VALUES (10003, 1, 'x')") && ok("DELETE FROM t WHERE id = 10003"), "writes work again after restart");
    db_shutdown();

    // ── 2. log pieno (write non riuscita) durante una transazione ──
    sh("rm -rf " WORK " && cp -r " BASE " " WORK);
    CHECK(db_init(WORK), "init work 2");
    ok("USE d");
    ok("BEGIN");
    g_fail_write = 1;
    QueryResult* r = q("UPDATE t SET v = v + 1");
    bool failed_somewhere = !r || !r->success;
    db_result_free(r);
    r = q("COMMIT");
    failed_somewhere = failed_somewhere || !r || !r->success;
    db_result_free(r);
    faults_off();
    CHECK(failed_somewhere, "a failed log write is reported (statement or COMMIT)");
    db_shutdown();
    CHECK(db_init(WORK), "restart after a failed log write: %s", db_init_error());
    ok("USE d");
    check_committed("failed log write", 0);
    db_shutdown();

    // ── 3. arresto alla k-esima scrittura (di pagina, poi del log) durante ROLLBACK: il recupero finisce
    //       l'annullamento. Dal P1.1 il ROLLBACK lavora sulle pagine della cache: scrive solo il log ──
    int ks = 0, done_all = 0;
    for (int k = 1, by_write = 0; k < 400 && (!done_all || !by_write); k++) {
        if (done_all && !by_write) { by_write = 1; done_all = 0; k = 1; }
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        pid_t pid = fork();
        if (pid == 0) {
            if (!db_init(WORK)) _exit(2);
            ok("USE d");
            ok("BEGIN");
            ok("DELETE FROM t WHERE id <= 200");
            ok("UPDATE t SET v = v + 1000 WHERE id > 400");
            ok("INSERT INTO t VALUES (20001, 5, 'new'), (20002, 6, 'new')");
            if (by_write) g_exit_write = k; else g_exit_pwrite = k;
            db_rollback();
            ok("SELECT 1");                             // anche il buffer del log del ROLLBACK arriva nel file
            g_exit_write = g_exit_pwrite = 0;
            _exit(3);                                   // ROLLBACK finito prima della k-esima scrittura
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 3) done_all = 1;
        CHECK(db_init(WORK), "restart after a crash in ROLLBACK (k=%d): %s", k, db_init_error());
        ok("USE d");
        check_committed("crash during ROLLBACK", k);
        db_shutdown();
        ks++;
    }
    CHECK(done_all, "ROLLBACK eventually completes without a crash");
    printf("crash during ROLLBACK: %d crash points checked\n", ks);

    // ── 4. transazione aperta su disco (pagine già scritte), poi arresto alla k-esima scrittura del recupero ──
    sh("rm -rf " CRASH " && cp -r " BASE " " CRASH);
    pid_t pid = fork();
    if (pid == 0) {
        if (!db_init(CRASH)) _exit(2);
        ok("USE d");
        ok("BEGIN");
        ok("DELETE FROM t WHERE id <= 250");
        ok("UPDATE t SET v = v * 2 WHERE id > 500");
        ok("SAVEPOINT s");
        ok("ROLLBACK TO s");
        ok("CHECKPOINT");                               // pagine sul file, nel log solo i record della transazione aperta
        _exit(0);                                       // arresto con la transazione aperta
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "crash state prepared");
    ks = 0;
    done_all = 0;
    for (int k = 1; k < 400 && !done_all; k++) {
        sh("rm -rf " WORK " && cp -r " CRASH " " WORK);
        pid = fork();
        if (pid == 0) {
            g_exit_pwrite = k;
            if (!db_init(WORK)) _exit(2);
            _exit(3);                                   // recupero finito prima della k-esima scrittura
        }
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 3) done_all = 1;
        CHECK(db_init(WORK), "restart after a crash in recovery (k=%d): %s", k, db_init_error());
        ok("USE d");
        check_committed("crash during recovery", k);
        db_shutdown();
        ks++;
    }
    CHECK(done_all, "recovery eventually completes without a crash");
    printf("crash during recovery: %d crash points checked\n", ks);

    // ── 4b. errore di lettura del log durante il recupero: niente avvio, log e dati intatti; poi riesce ──
    sh("rm -rf " WORK " && cp -r " CRASH " " WORK);
    {
        FILE* f = fopen(WORK "/segvault.wal", "rb");
        fseek(f, 0, SEEK_END);
        long wl = ftell(f);
        fclose(f);
        g_fail_pread = 3;                                   // la terza lettura del recupero
        CHECK(!db_init(WORK) && strstr(db_init_error(), "recovery"), "recovery read error refuses to start: %s", db_init_error());
        faults_off();
        f = fopen(WORK "/segvault.wal", "rb");
        long wl2 = -1;
        if (f) { fseek(f, 0, SEEK_END); wl2 = ftell(f); fclose(f); }
        CHECK(wl2 == wl, "log kept after a failed recovery (%ld -> %ld)", wl, wl2);
        CHECK(db_init(WORK), "recovery succeeds afterwards: %s", db_init_error());
        ok("USE d");
        check_committed("recovery after a failed attempt", 0);
        db_shutdown();
    }
    // ── 4c. arresto subito dopo il recupero (prima di qualsiasi checkpoint): non si perde niente ──
    sh("rm -rf " WORK " && cp -r " CRASH " " WORK);
    pid = fork();
    if (pid == 0) { if (!db_init(WORK)) _exit(2); _exit(0); }
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "recovery in a child");
    CHECK(db_init(WORK), "restart after a crash right after recovery: %s", db_init_error());
    ok("USE d");
    check_committed("crash right after recovery", 0);
    db_shutdown();

    // ── 4d. memoria del recupero: un log di decine di MB non viene caricato in RAM ──
    {
        sh("rm -rf " WORK " && mkdir -p " WORK);
        pid = fork();
        if (pid == 0) {
            if (!db_init(WORK)) _exit(2);
            ok("CREATE DATABASE d"); ok("USE d");
            ok("CREATE TABLE big (id INT PRIMARY KEY, pad VARCHAR(400))");
            ok("CREATE TABLE hold (id INT PRIMARY KEY)");
            // il log cresce oltre la soglia del checkpoint (alzata: dal P1.1 una transazione aperta non lo ferma più)
            db_set_checkpoint_bytes(1LL << 30);
            DbSession* h = db_session_new();
            db_session_enter(h); ok("USE d"); ok("BEGIN"); ok("INSERT INTO hold VALUES (1)"); db_session_leave(h);
            char q[600], pad[401];
            memset(pad, 'p', 400); pad[400] = 0;
            for (int i = 0; i < 60000; i++) { snprintf(q, sizeof(q), "INSERT INTO big VALUES (%d, '%s')", i, pad); ok(q); }
            _exit(0);                                       // arresto: il log resta
        }
        waitpid(pid, &st, 0);
        struct stat ls;
        CHECK(stat(WORK "/segvault.wal", &ls) == 0 && ls.st_size > (24 << 20), "large log prepared: %lld bytes", (long long)ls.st_size);
        long rss[2] = { 0, 0 };
        for (int t = 0; t < 2; t++) {                       // 0: recupero; 1: avvio senza log (riferimento)
            int pfd[2];
            if (pipe(pfd) != 0) break;
            pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                bool good = db_init(WORK);
                struct rusage ru;
                getrusage(RUSAGE_SELF, &ru);
                long v = good ? ru.ru_maxrss : -1;
                if (write(pfd[1], &v, sizeof(v)) < 0) {}
                _exit(0);
            }
            close(pfd[1]);
            if (read(pfd[0], &rss[t], sizeof(rss[t])) != sizeof(rss[t])) rss[t] = -1;
            close(pfd[0]);
            waitpid(pid, &st, 0);
        }
#ifdef __SANITIZE_ADDRESS__
        // sotto ASan la memoria liberata resta in quarantena (256 MB): il confronto non vale
        CHECK(rss[0] > 0 && rss[1] > 0, "recovery of a %lld-byte log ran (memory not compared under ASan: %ld KB more)", (long long)ls.st_size, rss[0] - rss[1]);
#else
        CHECK(rss[0] > 0 && rss[1] > 0 && rss[0] - rss[1] < 16 * 1024, "recovery of a %lld-byte log uses %ld KB more than a plain start (limit 16 MB)",
              (long long)ls.st_size, rss[0] - rss[1]);
#endif
        CHECK(db_init(WORK), "reopen after the big recovery");
        ok("USE d");
        CHECK(num("SELECT COUNT(*) FROM big") == 60000 && num("SELECT COUNT(*) FROM hold") == 0, "big recovery: committed rows kept, open one undone");
        db_shutdown();
    }

    // ── 4f. P1.1: una transazione aperta non impedisce più al log di accorciarsi (checkpoint con i soli record
    //        delle transazioni aperte); dopo una caduta la transazione aperta viene annullata dai record portati ──
    for (int crash = 0; crash < 2; crash++) {
        sh("rm -rf " WORK " && mkdir -p " WORK);
        pid = fork();
        if (pid == 0) {
            if (!db_init(WORK)) _exit(2);
            ok("CREATE DATABASE d"); ok("USE d");
            ok("CREATE TABLE big (id INT PRIMARY KEY, pad VARCHAR(400))");
            ok("CREATE TABLE hold (id INT PRIMARY KEY, v INT)");
            ok("INSERT INTO hold VALUES (100, 1)");
            DbSession* h = db_session_new();
            db_session_enter(h); ok("USE d"); ok("BEGIN"); ok("INSERT INTO hold VALUES (1, 1)");
            ok("UPDATE hold SET v = 2 WHERE id = 100"); db_session_leave(h);
            char q[600], pad[401];
            memset(pad, 'p', 400); pad[400] = 0;
            for (int i = 0; i < 60000; i++) { snprintf(q, sizeof(q), "INSERT INTO big VALUES (%d, '%s')", i, pad); ok(q); }
            struct stat ws;
            long long wb = stat(WORK "/segvault.wal", &ws) == 0 ? (long long)ws.st_size : -1;
            if (wb < 0 || wb > (20LL << 20)) { printf("FAIL: log of %lld bytes with a transaction open\n", wb); _exit(4); }
            db_session_enter(h); ok("INSERT INTO hold VALUES (2, 1)"); db_session_leave(h);
            if (crash) _exit(0);                              // caduta: la transazione aperta va annullata
            db_session_enter(h);
            bool rb = ok("ROLLBACK");
            db_session_leave(h);
            if (!rb || num("SELECT COUNT(*) FROM hold") != 1 || num("SELECT v FROM hold WHERE id = 100") != 1) _exit(5);
            db_shutdown();
            _exit(0);
        }
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "log stays under 20 MB with a transaction open for 60 000 statements (%s): exit %d",
              crash ? "then a crash" : "then ROLLBACK", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        CHECK(db_init(WORK), "restart: %s", db_init_error());
        ok("USE d");
        CHECK(num("SELECT COUNT(*) FROM big") == 60000 && num("SELECT COUNT(*) FROM hold") == 1 &&
              num("SELECT v FROM hold WHERE id = 100") == 1, "open transaction undone from the carried records (%s): big %lld, hold %lld",
              crash ? "crash" : "rollback", num("SELECT COUNT(*) FROM big"), num("SELECT COUNT(*) FROM hold"));
        db_shutdown();
    }

    // ── 4g. P1.1: un byte cambiato nel log non viene mai applicato: a metà il recupero si ferma e lo dice; in fondo
    //        (una scrittura interrotta) il record si scarta ──
    {
        sh("rm -rf " BASE "_log && mkdir -p " BASE "_log");
        pid = fork();
        if (pid == 0) {
            if (!db_init(BASE "_log")) _exit(2);
            ok("CREATE DATABASE d"); ok("USE d");
            ok("CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(40))");
            ok("CHECKPOINT");
            ok("SET GLOBAL synchronous = FULL");
            for (int i = 0; i < 40; i++) { char q[100]; snprintf(q, sizeof(q), "INSERT INTO t VALUES (%d, 'row %d')", i, i); ok(q); }
            _exit(0);                                         // il log resta con 40 inserimenti confermati
        }
        waitpid(pid, &st, 0);
        struct stat ls;
        CHECK(stat(BASE "_log/segvault.wal", &ls) == 0 && ls.st_size > 2000, "log with 40 committed inserts: %lld bytes", (long long)ls.st_size);
        long long wsz = (long long)ls.st_size;
        const long long spots[3] = { 20, wsz / 2, wsz - 30 };   // intestazione, a metà, nell'ultimo record
        for (int k = 0; k < 3; k++) {
            sh("rm -rf " WORK " && cp -r " BASE "_log " WORK);
            FILE* f = fopen(WORK "/segvault.wal", "r+b");
            fseek(f, spots[k], SEEK_SET);
            int ch = fgetc(f);
            fseek(f, spots[k], SEEK_SET);
            fputc(ch ^ 0x40, f);
            fclose(f);
            bool up = db_init(WORK);
            if (k < 2) {
                CHECK(!up && strstr(db_init_error(), "damaged"), "flipped byte at %lld of the log refused: %s", spots[k], db_init_error());
                struct stat l2;
                CHECK(stat(WORK "/segvault.wal", &l2) == 0 && l2.st_size == wsz, "damaged log left unchanged");
            } else {
                CHECK(up, "damaged last record dropped as an interrupted write: %s", db_init_error());
                if (up) {
                    ok("USE d");
                    long long n = num("SELECT COUNT(*) FROM t");
                    CHECK(n == 39 && num("SELECT COUNT(*) FROM t WHERE v <> CONCAT('row ', id)") == 0,
                          "only the damaged last insert lost, nothing wrong applied: %lld rows", n);
                    db_shutdown();
                }
            }
        }
        sh("rm -rf " BASE "_log");
    }

    // ── 4h. P1.1: file dei dati della 1.3.0 (formato 1) convertito al formato 2 all'apertura: stessi dati, righe
    //        grandi riscritte, pagine piene sistemate; una conversione interrotta riparte dalla copia .v1 ──
    {
        for (int interrupted = 0; interrupted < 2; interrupted++) {
            sh("rm -rf " WORK " && mkdir -p " WORK " && tar -C " WORK " -xzf tests/fixtures/format1-v1.3.0.tar.gz");
            if (interrupted) {                                  // copia fatta, file dei dati rovinato a metà conversione
                sh("cp " WORK "/segvault.db " WORK "/segvault.db.v1 && dd if=/dev/zero of=" WORK "/segvault.db bs=4096 seek=20 count=30 conv=notrunc 2>/dev/null");
            }
            CHECK(db_init(WORK), "format 1 file opened (%s): %s", interrupted ? "interrupted upgrade" : "first open", db_init_error());
            ok("USE m");
            CHECK(num("SELECT COUNT(*) FROM small") == 2572 && num("SELECT SUM(LENGTH(v)) FROM small") == 90942 &&
                  num("SELECT SUM(id) FROM small") == 3858858, "small rows unchanged after the upgrade: %lld", num("SELECT COUNT(*) FROM small"));
            CHECK(num("SELECT SUM(LENGTH(data)) FROM big") == 300000 && num("SELECT COUNT(*) FROM big WHERE data = REPEAT(SUBSTR(data, 1, 1), LENGTH(data))") == 5,
                  "large rows (overflow chains rewritten) intact");
            CHECK(num("SELECT SUM(LENGTH(pad)) FROM full1") == 39921, "rows of full pages intact");
            CHECK(ok("INSERT INTO small VALUES (99999, 'after upgrade')") && num("SELECT COUNT(*) FROM small") == 2573, "writes after the upgrade");
            db_shutdown();
            CHECK(access(WORK "/segvault.db.v1", F_OK) != 0, "the format 1 copy is removed after the upgrade");
            CHECK(db_init(WORK), "reopen in format 2");
            ok("USE m");
            CHECK(num("SELECT COUNT(*) FROM small") == 2573, "data kept across the restart");
            db_shutdown();
        }
        // un byte cambiato in una pagina del file dei dati: la pagina è rifiutata (CRC), mai letta come buona
        sh("rm -rf " WORK " && mkdir -p " WORK " && tar -C " WORK " -xzf tests/fixtures/format1-v1.3.0.tar.gz");
        CHECK(db_init(WORK), "open for the page corruption test");
        db_shutdown();
        FILE* f = fopen(WORK "/segvault.db", "r+b");
        fseek(f, 5 * 4096 + 1000, SEEK_SET);
        int ch = fgetc(f);
        fseek(f, 5 * 4096 + 1000, SEEK_SET);
        fputc(ch ^ 0x20, f);
        fclose(f);
        CHECK(db_init(WORK), "open with a damaged page");
        ok("USE m");
        QueryResult* qr = q("SELECT COUNT(*) FROM small");
        CHECK(qr && !qr->success && strstr(qr->error, "damaged"), "flipped byte in a data page reported: %s", qr ? (qr->success ? "no error" : qr->error) : "null");
        db_result_free(qr);
        db_shutdown();
    }

    // ── 4i. P1.1: pagina scritta a metà (scrittura interrotta, "torn page"): il recupero la rimette dall'immagine
    //        scritta nel log prima della pagina (doppia scrittura) ──
    for (int tpass = 0; tpass < 2; tpass++) {
        sh("rm -rf " WORK " && mkdir -p " WORK);
        pid = fork();
        if (pid == 0) {
            db_set_cache_size(4);                               // cache piccola: le pagine sporche escono durante il carico
            db_set_checkpoint_bytes(1LL << 30);                 // nessun checkpoint automatico: le scritture restano "in volo"
            if (!db_init(WORK)) _exit(2);
            ok("CREATE DATABASE d"); ok("USE d");
            ok("CREATE TABLE tp (id INT PRIMARY KEY, pad VARCHAR(200))");
            char q2[400], pad[201];
            memset(pad, 'q', 200); pad[200] = 0;
            ok("BEGIN");
            for (int i = 0; i < 30000; i++) { snprintf(q2, sizeof(q2), "INSERT INTO tp VALUES (%d, '%s')", i, pad); ok(q2); }
            ok("COMMIT");
            if (tpass == 1) {                                   // pagine vecchie (prima del checkpoint): scritte con l'immagine
                ok("CHECKPOINT");
                ok("UPDATE tp SET pad = REPEAT('r', 200)");
            }
            _exit(0);                                           // pagine nuove: rifatte da vuote dal log; vecchie: dall'immagine
        }
        waitpid(pid, &st, 0);
        struct stat ds;
        CHECK(stat(WORK "/segvault.db", &ds) == 0 && ds.st_size > 200 * 4096, "pages written during the load: %lld bytes", (long long)ds.st_size);
        int torn = 0;
        for (long pg = 20; pg < 200; pg += 37) {                // metà pagina azzerata: come una scrittura interrotta
            FILE* f = fopen(WORK "/segvault.db", "r+b");
            char z[2048];
            memset(z, 0, sizeof(z));
            fseek(f, pg * 4096 + 2048, SEEK_SET);
            if (fwrite(z, 1, sizeof(z), f) == sizeof(z)) torn++;
            fclose(f);
        }
        CHECK(db_init(WORK), "recovery with %d torn pages: %s", torn, db_init_error());
        ok("USE d");
        CHECK(num("SELECT COUNT(*) FROM tp") == 30000 && num("SELECT COUNT(*) FROM tp WHERE LENGTH(pad) = 200") == 30000 &&
              num("SELECT SUM(id) FROM tp") == 449985000LL && num("SELECT COUNT(*) FROM tp WHERE pad LIKE 'r%'") == (tpass ? 30000 : 0),
              "torn pages repaired (%s): %lld rows", tpass ? "old pages, from their images" : "new pages, from the log",
              num("SELECT COUNT(*) FROM tp"));
        db_shutdown();
    }

    // ── 4e. SET synchronous (P1.4): quanti fsync per 20 INSERT in autocommit e un COMMIT ──
    {
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        CHECK(db_init(WORK), "init work 4e");
        ok("USE d");
        ok("CREATE TABLE s (id INT PRIMARY KEY)");
        long counts[3];
        const char* modes[3] = { "OFF", "NORMAL", "FULL" };
        for (int m = 0; m < 3; m++) {
            char q[120];
            snprintf(q, sizeof(q), "SET GLOBAL synchronous = %s", modes[m]);
            CHECK(ok(q), "set synchronous %s", modes[m]);
            usleep(150000);                                     // NORMAL: fuori dalla finestra di 100 ms
            long before = g_fsyncs;
            for (int i = 0; i < 20; i++) { snprintf(q, sizeof(q), "INSERT INTO s VALUES (%d)", m * 100 + i); ok(q); }
            ok("BEGIN"); snprintf(q, sizeof(q), "INSERT INTO s VALUES (%d)", m * 100 + 50); ok(q); ok("COMMIT");
            counts[m] = g_fsyncs - before;
        }
        CHECK(counts[0] == 0, "synchronous = OFF: no fsync (%ld)", counts[0]);
        CHECK(counts[1] >= 1 && counts[1] <= 4, "synchronous = NORMAL: grouped (%ld fsyncs for 21 commits)", counts[1]);
        CHECK(counts[2] >= 21, "synchronous = FULL: one fsync per commit (%ld)", counts[2]);
        QueryResult* sr = q("SELECT @@synchronous");
        CHECK(sr && sr->success && !strcmp(sr->rows[0].cells[0].value, "FULL"), "@@synchronous");
        db_result_free(sr);
        CHECK(!ok("SET synchronous = SOMETIMES"), "bad synchronous value refused");
        // P1.10: cache delle pagine ridimensionata a caldo, i dati restano giusti
        CHECK(num("SELECT @@cache_size") >= 4, "automatic cache size: %lld MB", num("SELECT @@cache_size"));
        CHECK(ok("SET GLOBAL cache_size = 1") && num("SELECT @@cache_size") == 1, "cache_size = 1 MB");
        check_committed("after shrinking the cache", 0);
        CHECK(ok("SET GLOBAL cache_size = 64") && num("SELECT @@cache_size") == 64, "cache_size = 64 MB");
        check_committed("after growing the cache", 0);
        CHECK(!ok("SET GLOBAL cache_size = 0"), "cache_size 0 refused");
        ok("SET synchronous = NORMAL");
        ok("INSERT INTO s VALUES (999)");
        long before = g_fsyncs;
        usleep(150000);
        db_sync_tick();
        CHECK(g_fsyncs == before + 1, "NORMAL: the tick syncs the last autocommit write");
        db_shutdown();
    }

    // ── 4f. ROLLBACK O(transazione) (P1.2): gli indici delle altre tabelle restano, quelli toccati sono giusti ──
    {
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        CHECK(db_init(WORK), "init work 4f");
        ok("USE d");
        ok("CREATE TABLE small (id INT PRIMARY KEY, v INT)");
        ok("INSERT INTO small VALUES (1, 10), (2, 20), (3, 30)");
        CHECK(num("SELECT v FROM t WHERE id = 77") == 77 && num("SELECT v FROM small WHERE id = 2") == 20, "indexes built");
        QueryResult* st0 = q("SHOW STATUS");
        int mi0 = -1;
        for (int i = 0; st0 && st0->success && i < st0->num_rows; i++)
            if (!strcmp(st0->rows[i].cells[0].value, "in_memory_indexes")) mi0 = atoi(st0->rows[i].cells[1].value);
        db_result_free(st0);
        double t0 = 0;
        {
            struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a); t0 = a.tv_sec * 1e3 + a.tv_nsec / 1e6;
        }
        for (int k = 0; k < 200; k++) {
            ok("BEGIN");
            char q2[120];
            snprintf(q2, sizeof(q2), "INSERT INTO small VALUES (%d, 1)", 1000 + k);
            ok(q2);
            ok("DELETE FROM small WHERE id = 1");
            ok("ROLLBACK");
        }
        struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
        printf("200 x (BEGIN; INSERT; DELETE; ROLLBACK): %.1f ms\n", b.tv_sec * 1e3 + b.tv_nsec / 1e6 - t0);
        QueryResult* st1 = q("SHOW STATUS");
        int mi1 = -1;
        for (int i = 0; st1 && st1->success && i < st1->num_rows; i++)
            if (!strcmp(st1->rows[i].cells[0].value, "in_memory_indexes")) mi1 = atoi(st1->rows[i].cells[1].value);
        db_result_free(st1);
        CHECK(mi0 >= 2 && mi1 >= 1 && mi1 >= mi0 - 1, "the index of the untouched table survives the rollbacks (%d -> %d)", mi0, mi1);
        CHECK(num("SELECT v FROM small WHERE id = 1") == 10 && num("SELECT COUNT(*) FROM small") == 3 &&
              num("SELECT COUNT(*) FROM small WHERE id = 1050") == 0 && num("SELECT v FROM t WHERE id = 77") == 77,
              "rows and indexes right after the rollbacks");
        // ROLLBACK TO su una tabella, poi COMMIT
        ok("BEGIN"); ok("INSERT INTO small VALUES (4, 40)"); ok("SAVEPOINT a"); ok("DELETE FROM small WHERE id = 2");
        ok("ROLLBACK TO a"); ok("COMMIT");
        CHECK(num("SELECT v FROM small WHERE id = 2") == 20 && num("SELECT v FROM small WHERE id = 4") == 40, "ROLLBACK TO keeps indexes right");
        // DDL nella transazione: ROLLBACK invalida tutto e resta corretto
        ok("BEGIN"); ok("CREATE TABLE tmpx (id INT PRIMARY KEY)"); ok("INSERT INTO small VALUES (5, 50)"); ok("ROLLBACK");
        CHECK(num("SELECT COUNT(*) FROM small WHERE id = 5") == 0, "rollback with DDL");
        check_committed("rollback scope", 0);
        db_shutdown();
    }

    // ── 4g. versione confermata con 100 000 righe modificate in sospeso (P1.8): lineare, non quadratica ──
    {
        sh("rm -rf " WORK " && mkdir -p " WORK);
        CHECK(db_init(WORK), "init work 4g");
        ok("CREATE DATABASE d"); ok("USE d");
        ok("CREATE TABLE m (id INT PRIMARY KEY, v INT)");
        ok("INSERT INTO m VALUES (1, 1), (2, 2)");
        DbSession* w = db_session_new();
        db_session_enter(w);
        ok("USE d");
        ok("BEGIN");
        char* big = malloc(1 << 20);
        for (int i = 0; i < 100000; ) {
            size_t len = (size_t)sprintf(big, "INSERT INTO m VALUES ");
            for (int k = 0; k < 1000 && i < 100000; k++, i++) len += (size_t)sprintf(big + len, "%s(%d, %d)", k ? "," : "", 10 + i, i);
            ok(big);
        }
        ok("UPDATE m SET v = v + 1 WHERE id < 50000");
        free(big);
        db_session_leave(w);
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        long long c = num("SELECT COUNT(*) FROM m");            // altra sessione: la versione confermata
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        printf("committed-version read with 150 000 pending changes: %.1f ms\n", ms);
        CHECK(c == 2 && ms < 2000, "committed version with 150 000 pending changes: %lld rows in %.1f ms", c, ms);
        db_session_enter(w); CHECK(ok("ROLLBACK"), "rollback of the big transaction"); db_session_leave(w);
        CHECK(num("SELECT COUNT(*) FROM m") == 2, "big transaction rolled back: %lld", num("SELECT COUNT(*) FROM m"));
        db_session_end(w);
        db_shutdown();
    }

    // ── 5. arresto con una transazione aperta in un'altra sessione: il log resta, le sue righe spariscono ──
    sh("rm -rf " WORK " && cp -r " BASE " " WORK);
    CHECK(db_init(WORK), "init work 5");
    DbSession* a = db_session_new();
    CHECK(a && db_session_enter(a), "session A");
    ok("USE d");
    ok("BEGIN");
    ok("DELETE FROM t WHERE id <= 50");
    ok("INSERT INTO t VALUES (30001, 1, 'a')");
    db_session_leave(a);                                // A resta con la transazione aperta
    db_shutdown();
    CHECK(access(WORK "/segvault.wal", F_OK) == 0, "log kept when a session still has an open transaction");
    CHECK(db_init(WORK), "restart: %s", db_init_error());
    ok("USE d");
    check_committed("open transaction of another session at shutdown", 0);
    CHECK(num("SELECT COUNT(*) FROM t WHERE id = 30001") == 0, "uncommitted row of session A gone");
    db_shutdown();
    free(a);                                            // (la sessione del motore di prima del riavvio)

    // ── 6. pagina danneggiata: lo statement fallisce, mai un risultato con meno righe (P0.10) ──
    for (int variant = 0; variant < 3; variant++) {
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        FILE* f = fopen(WORK "/segvault.db", "r+b");
        CHECK(f != NULL, "open data file");
        if (f) {
            long page = 6 + variant;
            unsigned char bad[2] = { 0xF0, 0xFF };
            // 0: num_slots enorme; 1: offset di uno slot fuori dalla pagina; 2: page_id sbagliato
            long off = variant == 0 ? page * 4096 + 6 : variant == 1 ? page * 4096 + 12 : page * 4096;
            fseek(f, off, SEEK_SET);
            fwrite(bad, 1, 2, f);
            fclose(f);
        }
        CHECK(db_init(WORK), "init with a damaged page: %s", db_init_error());
        ok("USE d");
        QueryResult* dr = q("SELECT COUNT(*) FROM t");
        CHECK(dr && !dr->success && strstr(dr->error, "damaged"), "COUNT(*) over a damaged page fails (variant %d): %s", variant,
              dr ? (dr->success ? dr->rows[0].cells[0].value : dr->error) : "null");
        db_result_free(dr);
        CHECK(err_has("UPDATE t SET v = v + 1", "damaged") && err_has("DELETE FROM t WHERE v > 0", "damaged"),
              "UPDATE / DELETE over a damaged page fail (variant %d)", variant);
        db_shutdown();
    }

    // ── 7. catalogo e utenti danneggiati: il motore non parte e i file restano com'è (P0.7) ──
    {
        FILE* f = fopen(BASE "/segvault.cat", "rb");
        static unsigned char cat[1 << 22];
        size_t clen = f ? fread(cat, 1, sizeof(cat), f) : 0;
        if (f) fclose(f);
        CHECK(clen > 100, "catalog read");
        for (int t = 0; t < 10 && clen > 100; t++) {
            size_t cut = 1 + (size_t)((unsigned long long)(t + 1) * 2654435761ull % (clen - 20));   // mai la sola coda
            sh("rm -rf " WORK " && cp -r " BASE " " WORK);
            f = fopen(WORK "/segvault.cat", "wb");
            fwrite(cat, 1, cut, f);
            fclose(f);
            CHECK(!db_init(WORK) && strstr(db_init_error(), "catalog"), "truncated catalog (%zu of %zu bytes) refused: %s", cut, clen, db_init_error());
            f = fopen(WORK "/segvault.cat", "rb");
            static unsigned char back[1 << 22];
            size_t bl = f ? fread(back, 1, sizeof(back), f) : 0;
            if (f) fclose(f);
            CHECK(bl == cut && !memcmp(back, cat, cut), "truncated catalog left unchanged");
        }
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        f = fopen(WORK "/segvault.cat", "r+b");
        unsigned v = 99, v_orig = 0;
        fseek(f, 8, SEEK_SET);
        if (fread(&v_orig, 4, 1, f) != 1) v_orig = 0;
        fseek(f, 8, SEEK_SET);
        fwrite(&v, 4, 1, f);
        fclose(f);
        CHECK(!db_init(WORK) && strstr(db_init_error(), "newer"), "catalog of a newer version refused: %s", db_init_error());
        f = fopen(WORK "/segvault.cat", "r+b");                       // un byte cambiato: il CRC lo vede
        fseek(f, 300, SEEK_SET);
        int ch = fgetc(f);
        fseek(f, 300, SEEK_SET);
        fputc(ch ^ 0x55, f);
        v = v_orig;
        fseek(f, 8, SEEK_SET);
        fwrite(&v, 4, 1, f);
        fclose(f);
        CHECK(!db_init(WORK) && strstr(db_init_error(), "checksum"), "flipped byte in the catalog detected: %s", db_init_error());
        // salvataggio interrotto prima della sostituzione: resta solo il temporaneo, che si usa
        sh("rm -rf " WORK " && cp -r " BASE " " WORK " && mv " WORK "/segvault.cat " WORK "/segvault.cat.tmp");
        CHECK(db_init(WORK), "catalog recovered from segvault.cat.tmp: %s", db_init_error());
        ok("USE d");
        check_committed("catalog from the temporary file", 0);
        CHECK(ok("CREATE USER zoe IDENTIFIED BY 'zoe-pass'") && ok("CREATE USER yan IDENTIFIED BY 'yan-pass'"), "users created");
        db_shutdown();
        CHECK(access(WORK "/segvault.cat", F_OK) == 0 && access(WORK "/segvault.cat.tmp", F_OK) != 0, "temporary catalog renamed");
        // users.dat troncato (l'ultimo utente perso): rifiutato, mai "nessun utente" = tutti root
        sh("grep -v '^user yan\\|^grant yan' " WORK "/users.dat > " WORK "/u && mv " WORK "/u " WORK "/users.dat");
        CHECK(!db_init(WORK) && strstr(db_init_error(), "users"), "truncated users file refused: %s", db_init_error());
    }

    // ── 7b. caduta di corrente alla k-esima scrittura (P3.1): resta su disco solo ciò che un fsync aveva
    //        confermato; dopo il recupero ci sono tutte le transazioni confermate e nessuna delle altre ──
    {
        int points = 0, pw_done = 0;
        for (int k = 1; k < 2000 && !pw_done; k += (k < 60 ? 1 : 7)) {
            sh("rm -rf " WORK " && cp -r " BASE " " WORK);
            remove(POWER_JOURNAL);
            pid_t pp = fork();
            if (pp == 0) {
                if (!db_init(WORK)) _exit(2);
                ok("USE d");
                ok("SET GLOBAL synchronous = FULL");
                g_power = true;
                g_power_at = k;
                // ogni COMMIT riuscito scrive il suo numero in un file fuori dal motore (sopravvive alla corrente)
                for (int t2 = 0; t2 < 12; t2++) {
                    char q2[200];
                    ok("BEGIN");
                    snprintf(q2, sizeof(q2), "INSERT INTO t VALUES (%d, 0, 'p')", 50000 + t2);
                    ok(q2);
                    snprintf(q2, sizeof(q2), "DELETE FROM t WHERE id = %d", 1 + t2);
                    ok(q2);
                    snprintf(q2, sizeof(q2), "UPDATE t SET v = v + 1000000 WHERE id = %d", 100 + t2);
                    ok(q2);
                    if (ok("COMMIT")) {
                        FILE* cf = fopen("/tmp/sv_dur_commits", "a");
                        if (cf) { fprintf(cf, "%d\n", t2); fclose(cf); }
                    }
                }
                g_power = false;
                _exit(3);
            }
            remove("/tmp/sv_dur_commits");
            int ps;
            waitpid(pp, &ps, 0);
            if (WIFEXITED(ps) && WEXITSTATUS(ps) == 3) pw_done = 1;
            power_restore();
            int committed = 0;
            FILE* cf = fopen("/tmp/sv_dur_commits", "r");
            int x;
            while (cf && fscanf(cf, "%d", &x) == 1) committed = x + 1;
            if (cf) fclose(cf);
            remove("/tmp/sv_dur_commits");
            CHECK(db_init(WORK), "restart after a power cut at write %d: %s", k, db_init_error());
            ok("USE d");
            // transazioni confermate: le prime committed; quella in corso (se c'è) per intero o per niente
            long long c_new = num("SELECT COUNT(*) FROM t WHERE id >= 50000");
            long long c_all = num("SELECT COUNT(*) FROM t"), c_dist = num("SELECT COUNT(DISTINCT id) FROM t");
            long long upd = num("SELECT COUNT(*) FROM t WHERE v >= 1000000");
            bool whole = (c_new == committed || c_new == committed + 1) && upd == c_new && c_all == N && c_dist == N;
            CHECK(whole, "power cut at write %d: %d commits reported, %lld new rows, %lld updated, %lld rows (%lld distinct)",
                  k, committed, c_new, upd, c_all, c_dist);
            db_shutdown();
            points++;
        }
        CHECK(pw_done, "the workload eventually completes before the power cut");
        printf("power cut: %d points checked\n", points);
    }

    // ── 7c. P1.1: caduta di corrente a OGNI scrittura di un carico di 10 transazioni interlacciate su 3 sessioni
    //        (ognuna sulla sua tabella: i lock sono per tabella). Dopo il recupero: ogni transazione confermata
    //        c'è per intero, quella del COMMIT in corso per intero o per niente, le altre per niente ──
    {
        sh("rm -rf " BASE "_il && mkdir -p " BASE "_il");
        if (db_init(BASE "_il")) {
            ok("CREATE DATABASE d"); ok("USE d");
            for (int k = 0; k < 3; k++) {
                char q[200];
                snprintf(q, sizeof(q), "CREATE TABLE s%d (id INT PRIMARY KEY, v INT, pad VARCHAR(100))", k);
                ok(q);
                for (int i = 0; i < 40; i++) { snprintf(q, sizeof(q), "INSERT INTO s%d VALUES (%d, %d, 'base')", k, i, i); ok(q); }
            }
            db_shutdown();
        }
        int points = 0, il_done = 0, bad = 0;
        for (int k = 1; k < 4000 && !il_done; k++) {
            sh("rm -rf " WORK " && cp -r " BASE "_il " WORK);
            remove(POWER_JOURNAL);
            remove("/tmp/sv_dur_commits");
            pid_t pp = fork();
            if (pp == 0) {
                if (!db_init(WORK)) _exit(2);
                ok("USE d");
                ok("SET GLOBAL synchronous = FULL");
                DbSession* ses[3];
                for (int j = 0; j < 3; j++) { ses[j] = db_session_new(); db_session_enter(ses[j]); ok("USE d"); db_session_leave(ses[j]); }
                g_power = true;
                g_power_at = k;
                int cur[3] = { -1, -1, -1 }, step[3] = { 0, 0, 0 }, next_tx = 0, done = 0;
                for (int tick = 0; done < 10 && tick < 1000; tick++) {
                    int j = tick % 3;
                    if (cur[j] < 0) { if (next_tx >= 10) continue; cur[j] = next_tx++; step[j] = 0; }
                    int t = cur[j];
                    char q[200];
                    db_session_enter(ses[j]);
                    switch (step[j]) {
                        case 0: ok("BEGIN"); break;
                        case 1: snprintf(q, sizeof(q), "INSERT INTO s%d VALUES (%d, 0, 'tx')", j, 1000 + t); ok(q); break;
                        case 2: snprintf(q, sizeof(q), "UPDATE s%d SET v = v + 1000000 WHERE id = %d", j, t); ok(q); break;
                        case 3: snprintf(q, sizeof(q), "DELETE FROM s%d WHERE id = %d", j, 20 + t); ok(q); break;
                        case 4:
                            if (ok("COMMIT")) {
                                FILE* cf = fopen("/tmp/sv_dur_commits", "a");
                                if (cf) { fprintf(cf, "%d\n", t); fclose(cf); }
                            }
                            break;
                    }
                    db_session_leave(ses[j]);
                    if (++step[j] == 5) { cur[j] = -1; done++; }
                }
                g_power = false;
                _exit(3);
            }
            int ps;
            waitpid(pp, &ps, 0);
            if (WIFEXITED(ps) && WEXITSTATUS(ps) == 3) il_done = 1;
            power_restore();
            bool reported[10] = { false };
            int nrep = 0, x;
            FILE* cf = fopen("/tmp/sv_dur_commits", "r");
            while (cf && fscanf(cf, "%d", &x) == 1) if (x >= 0 && x < 10) { reported[x] = true; nrep++; }
            if (cf) fclose(cf);
            if (!db_init(WORK)) { bad++; CHECK(false, "interleaved: restart after a power cut at write %d: %s", k, db_init_error()); continue; }
            ok("USE d");
            int extra = 0;
            bool whole = true;
            for (int t = 0; t < 10; t++) {
                int j = t % 3;                                  // la sessione che l'ha eseguita (assegnazione a turno)
                (void)j;
                long long ins = 0, upd = 0, del = 0;
                for (int jj = 0; jj < 3; jj++) {
                    char q[200];
                    snprintf(q, sizeof(q), "SELECT COUNT(*) FROM s%d WHERE id = %d", jj, 1000 + t); ins += num(q);
                }
                // UPDATE e DELETE della transazione t sono sulla stessa tabella del suo INSERT
                int tab = -1;
                for (int jj = 0; jj < 3 && tab < 0; jj++) { char q[200]; snprintf(q, sizeof(q), "SELECT COUNT(*) FROM s%d WHERE id = %d", jj, 1000 + t); if (num(q)) tab = jj; }
                if (tab >= 0) {
                    char q[200];
                    snprintf(q, sizeof(q), "SELECT COUNT(*) FROM s%d WHERE id = %d AND v >= 1000000", tab, t); upd = num(q);
                    snprintf(q, sizeof(q), "SELECT COUNT(*) FROM s%d WHERE id = %d", tab, 20 + t); del = 1 - num(q);
                }
                bool present = ins == 1;
                if (present && (upd != 1 || del != 1)) whole = false;          // parziale: mai
                if (ins > 1) whole = false;
                if (reported[t] && !present) whole = false;                    // confermata ma persa
                if (present && !reported[t]) extra++;                          // il COMMIT in corso
            }
            long long total = 0;
            for (int jj = 0; jj < 3; jj++) { char q[100]; snprintf(q, sizeof(q), "SELECT COUNT(*) FROM s%d WHERE v >= 1000000", jj); total += num(q); }
            if (!whole || extra > 1) bad++;
            CHECK(whole && extra <= 1, "interleaved power cut at write %d: %d commits reported, %d unreported present, %lld updates",
                  k, nrep, extra, total);
            db_shutdown();
            points++;
        }
        CHECK(il_done, "the interleaved workload eventually completes before the power cut");
        printf("interleaved power cut (10 transactions, 3 sessions): every one of %d writes checked, %d bad\n", points, bad);
        sh("rm -rf " BASE "_il");
    }

    // ── 7b. memoria esaurita (P3.8): l'N-esima allocazione fallisce, per ogni N del carico (avvio compreso).
    //        Regola: arresto controllato (SIGABRT con "out of memory") o errore dell'istruzione, mai un crash
    //        diverso; al riavvio le righe confermate ci sono tutte, quelle della transazione aperta nessuna;
    //        un'istruzione fallita con un errore lascia la transazione aperta (come MySQL), quindi il COMMIT
    //        può confermare meno di 30 righe, ma ogni UPDATE è intero. ──
    {
        #define OOM_WORK() do { \
            if (!db_init(WORK)) _exit(2); \
            q("SET synchronous = FULL"); q("USE d"); \
            q("BEGIN"); \
            for (int i_ = 0; i_ < 30; i_++) { char s_[200]; snprintf(s_, sizeof(s_), "INSERT INTO t VALUES (%d, 1, 'oom')", 100000 + i_); q(s_); } \
            q("UPDATE t SET pad = 'x' WHERE id >= 100000"); \
            q("COMMIT"); \
            q("BEGIN"); q("DELETE FROM t WHERE id <= 300"); q("SELECT COUNT(*), SUM(v) FROM t"); \
        } while (0)
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        pid_t cp = fork();
        if (cp == 0) {                                   // passata di conteggio: quante allocazioni fa il carico
            g_allocs = 0;
            OOM_WORK();
            FILE* f = fopen(WORK "_allocs", "w");
            if (f) { fprintf(f, "%ld\n", g_allocs); fclose(f); }
            _exit(3);
        }
        int cst = 0;
        waitpid(cp, &cst, 0);
        long total = 0;
        FILE* f = fopen(WORK "_allocs", "r");
        if (f) { if (fscanf(f, "%ld", &total) != 1) total = 0; fclose(f); }
        remove(WORK "_allocs");
        CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 3 && total > 100, "allocation count of the workload: %ld", total);
        long stride = total > 400 ? total / 400 : 1;   // SV_OOM_ALL=1: ogni allocazione
        if (getenv("SV_OOM_ALL")) stride = 1;
        int points = 0, aborted = 0, bad = 0;
        for (long k = 1; k <= total + 1; k += stride) {
            sh("rm -rf " WORK " && cp -r " BASE " " WORK);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                struct rlimit nocore = { 0, 0 };
                setrlimit(RLIMIT_CORE, &nocore);
                if (!freopen("/dev/null", "w", stderr)) {}
                g_fail_alloc = k;
                OOM_WORK();
                _exit(3);
            }
            int st2 = 0;
            waitpid(pid2, &st2, 0);
            bool clean = (WIFEXITED(st2) && (WEXITSTATUS(st2) == 3 || WEXITSTATUS(st2) == 2)) ||
                         (WIFSIGNALED(st2) && WTERMSIG(st2) == SIGABRT);
            if (WIFSIGNALED(st2) && WTERMSIG(st2) == SIGABRT) aborted++;
            if (!clean) { bad++; CHECK(0, "allocation failure %ld: child ended with status %d", k, st2); continue; }
            if (!db_init(WORK)) { bad++; CHECK(0, "restart after allocation failure %ld: %s", k, db_init_error()); continue; }
            ok("USE d");
            long long c = num("SELECT COUNT(*) FROM t WHERE id <= 600"), sum = num("SELECT SUM(v) FROM t WHERE id <= 600");
            long long extra = num("SELECT COUNT(*) FROM t WHERE id >= 100000");
            long long padx = num("SELECT COUNT(*) FROM t WHERE id >= 100000 AND pad = 'x'");
            if (!(c == N && sum == (long long)N * (N + 1) / 2 && extra >= 0 && extra <= 30 && (padx == 0 || padx == extra))) {
                bad++;
                CHECK(0, "after allocation failure %ld: count %lld, sum %lld, new rows %lld (updated %lld)", k, c, sum, extra, padx);
            }
            db_shutdown();
            points++;
        }
        g_fail_alloc = 0;
        CHECK(bad == 0, "allocation-failure sweep: %d bad outcomes", bad);
        CHECK(aborted > 0, "some allocation failures stop the process with the out-of-memory handler");
        printf("allocation failures: %d points checked (%d stopped by the out-of-memory handler)\n", points, aborted);
        #undef OOM_WORK
    }

    // ── 8. file oltre 2 GiB: pagina 600 000 (offset 2,4 GB) scritta e riletta (P0.12) ──
    {
        int fd = open(WORK "_big", O_RDWR | O_CREAT | O_TRUNC, 0600);
        Page* pg = page_create(600000);
        CHECK(fd >= 0 && pg && page_insert(pg, "big-file-row", 12) == 0, "sparse page prepared");
        CHECK(pg && page_write(pg, fd) == SV_OK, "write page 600000");
        Page* back = page_read(600000, fd);
        uint16_t bl = 0;
        const void* row = back ? page_get_slot(back, 0, &bl) : NULL;
        CHECK(back && back->page_id == 600000 && row && bl == 12 && !memcmp(row, "big-file-row", 12), "read page 600000 back");
        CHECK(lseek(fd, 0, SEEK_END) == (off_t)600001 * 4096, "file size past 2 GiB");
        page_free(pg);
        page_free(back);
        if (fd >= 0) close(fd);
        unlink(WORK "_big");
    }

    // ── 9. P1.11: il DDL va nel log (record LOG_CATALOG), non più catalogo intero + fsync. Caduta di corrente a
    //       ogni scrittura di un carico di DDL e DML: dopo il recupero lo schema è quello dopo gli statement
    //       confermati (quello in corso per intero o per niente) ──
    {
        static const char* steps[] = {
            "CREATE TABLE a (id INT PRIMARY KEY, v INT)",
            "INSERT INTO a VALUES (1, 1), (2, 2)",
            "ALTER TABLE a ADD COLUMN w VARCHAR(20) DEFAULT 'x'",
            "CREATE INDEX ia ON a (v)",
            "CREATE VIEW va AS SELECT id FROM a",
            "CREATE TABLE b (id INT)",
            "INSERT INTO b VALUES (7)",
            "RENAME TABLE b TO c",
            "CREATE DATABASE e2",
            "CREATE TABLE e2.x (id INT)",
            "DROP TABLE c",
            "DROP DATABASE e2",
            "TRUNCATE TABLE a",
            "INSERT INTO a (id, v) VALUES (5, 5)",
        };
        enum { NSTEP = (int)(sizeof(steps) / sizeof(steps[0])) };
        // stato atteso dopo i primi p statement: righe di a (-1 = non c'è), colonna w, indice ia, vista va,
        // righe di b, righe di c, database e2, tabella e2.x
        static const int expect[NSTEP + 1][8] = {
            { -1, 0, 0, 0, -1, -1, 0, 0 },
            {  0, 0, 0, 0, -1, -1, 0, 0 },
            {  2, 0, 0, 0, -1, -1, 0, 0 },
            {  2, 1, 0, 0, -1, -1, 0, 0 },
            {  2, 1, 1, 0, -1, -1, 0, 0 },
            {  2, 1, 1, 1, -1, -1, 0, 0 },
            {  2, 1, 1, 1,  0, -1, 0, 0 },
            {  2, 1, 1, 1,  1, -1, 0, 0 },
            {  2, 1, 1, 1, -1,  1, 0, 0 },
            {  2, 1, 1, 1, -1,  1, 1, 0 },
            {  2, 1, 1, 1, -1,  1, 1, 1 },
            {  2, 1, 1, 1, -1, -1, 1, 1 },
            {  2, 1, 1, 1, -1, -1, 0, 0 },
            {  0, 1, 1, 1, -1, -1, 0, 0 },
            {  1, 1, 1, 1, -1, -1, 0, 0 },
        };
        sh("rm -rf " BASE "_ddl && mkdir -p " BASE "_ddl");
        if (db_init(BASE "_ddl")) { ok("CREATE DATABASE d"); db_shutdown(); }
        int points = 0, ddl_done = 0, bad = 0;
        for (int k = 1; k < 3000 && !ddl_done; k++) {
            sh("rm -rf " WORK " && cp -r " BASE "_ddl " WORK);
            remove(POWER_JOURNAL);
            remove("/tmp/sv_dur_commits");
            pid_t pp = fork();
            if (pp == 0) {
                if (!db_init(WORK)) _exit(2);
                ok("USE d");
                ok("SET GLOBAL synchronous = FULL");
                g_power = true;
                g_power_at = k;
                for (int i = 0; i < NSTEP; i++) {
                    if (!ok(steps[i])) { g_power = false; _exit(4); }
                    FILE* cf = fopen("/tmp/sv_dur_commits", "a");
                    if (cf) { fprintf(cf, "%d\n", i); fclose(cf); }
                }
                g_power = false;
                _exit(3);
            }
            int ps;
            waitpid(pp, &ps, 0);
            if (WIFEXITED(ps) && WEXITSTATUS(ps) == 4) { CHECK(false, "DDL workload: a statement failed without a power cut (k=%d)", k); break; }
            if (WIFEXITED(ps) && WEXITSTATUS(ps) == 3) ddl_done = 1;
            power_restore();
            int done = 0, x;
            FILE* cf = fopen("/tmp/sv_dur_commits", "r");
            while (cf && fscanf(cf, "%d", &x) == 1) done = x + 1;
            if (cf) fclose(cf);
            if (!db_init(WORK)) { bad++; CHECK(false, "DDL: restart after a power cut at write %d: %s", k, db_init_error()); continue; }
            ok("USE d");
            int got[8];
            got[0] = (int)num("SELECT COUNT(*) FROM a");
            got[1] = num("SELECT COUNT(w) FROM a") >= 0;
            got[2] = 0;
            QueryResult* ir = q("SHOW INDEX FROM a");
            for (int i = 0; ir && ir->success && i < ir->num_rows; i++)
                for (int j = 0; j < ir->num_cols; j++)
                    if (!ir->rows[i].cells[j].is_null && !strcmp(ir->rows[i].cells[j].value, "ia")) got[2] = 1;
            db_result_free(ir);
            got[3] = num("SELECT COUNT(*) FROM va") >= 0;
            got[4] = (int)num("SELECT COUNT(*) FROM b");
            got[5] = (int)num("SELECT COUNT(*) FROM c");
            got[6] = 0;
            QueryResult* dr = q("SHOW DATABASES");
            for (int i = 0; dr && dr->success && i < dr->num_rows; i++)
                if (!dr->rows[i].cells[0].is_null && !strcmp(dr->rows[i].cells[0].value, "e2")) got[6] = 1;
            db_result_free(dr);
            got[7] = num("SELECT COUNT(*) FROM e2.x") >= 0;
            bool match = !memcmp(got, expect[done], sizeof(got)) || (done < NSTEP && !memcmp(got, expect[done + 1], sizeof(got)));
            if (!match) bad++;
            CHECK(match, "DDL power cut at write %d: %d statements confirmed, schema {a=%d w=%d ia=%d va=%d b=%d c=%d e2=%d x=%d}",
                  k, done, got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
            // il catalogo recuperato è stato salvato: un secondo avvio senza log vede lo stesso schema
            db_shutdown();
            if (db_init(WORK)) {
                ok("USE d");
                CHECK((int)num("SELECT COUNT(*) FROM a") == got[0], "DDL power cut at write %d: same schema after a second restart", k);
                db_shutdown();
            }
            points++;
            if (bad > 5) break;
        }
        CHECK(ddl_done, "the DDL workload eventually completes before the power cut");
        printf("DDL power cut: %d points checked, %d bad\n", points, bad);
    }

    // ── 9b. synchronous = NORMAL: il DDL è nel file del log anche senza fsync, un processo che cade non lo perde ──
    {
        sh("rm -rf " WORK " && cp -r " BASE "_ddl " WORK);
        pid_t pp = fork();
        if (pp == 0) {
            if (!db_init(WORK)) _exit(2);
            ok("USE d");
            for (int i = 0; i < 20; i++) { char sq[120]; snprintf(sq, sizeof(sq), "CREATE TABLE n%d (id INT, s VARCHAR(10))", i); if (!ok(sq)) _exit(4); }
            if (!ok("DROP TABLE n3") || !ok("ALTER TABLE n4 ADD COLUMN z INT")) _exit(4);
            _exit(0);                                         // niente db_shutdown: il catalogo sul file è quello di prima
        }
        int ps;
        waitpid(pp, &ps, 0);
        CHECK(WIFEXITED(ps) && WEXITSTATUS(ps) == 0, "DDL workload in NORMAL mode ran");
        CHECK(db_init(WORK), "restart after a crash with DDL only in the log: %s", db_init_error());
        ok("USE d");
        CHECK(num("SELECT COUNT(*) FROM n0") == 0 && num("SELECT COUNT(*) FROM n19") == 0 && num("SELECT COUNT(*) FROM n3") == -1 &&
              num("SELECT COUNT(z) FROM n4") == 0, "tables created, dropped and altered before the crash are recovered from the log");
        db_shutdown();
        sh("rm -rf " BASE "_ddl");
    }

    // ── 9c. P1.11: limiti del catalogo e costo del DDL — 300 colonne, 2 000 tabelle, 1 000 CREATE TABLE in meno di un
    //        secondo (qui 2 s di margine), memoria del motore all'avvio ──
    {
        sh("rm -rf " WORK " && mkdir -p " WORK);
        CHECK(db_init(WORK), "init for the catalog limits");
        ok("CREATE DATABASE big");
        ok("USE big");
        static char wide[20000];
        int at = snprintf(wide, sizeof(wide), "CREATE TABLE wide (id INT PRIMARY KEY");
        for (int i = 1; i < 300; i++) at += snprintf(wide + at, sizeof(wide) - (size_t)at, ", c%d VARCHAR(10)", i);
        snprintf(wide + at, sizeof(wide) - (size_t)at, ")");
        CHECK(ok(wide), "CREATE TABLE with 300 columns");
        CHECK(ok("INSERT INTO wide (id, c1, c299) VALUES (1, 'first', 'last')"), "row into the 300-column table");
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int made = 0;
        for (int i = 0; i < 2000; i++) {
            char sq[200];
            snprintf(sq, sizeof(sq), "CREATE TABLE t%04d (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50), price DECIMAL(10,2))", i);
            if (ok(sq)) made++;
            if (i == 999) clock_gettime(CLOCK_MONOTONIC, &t1);
        }
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        CHECK(made == 2000, "2 000 tables in one database (%d created)", made);
        CHECK(ms < 2000, "1 000 CREATE TABLE in %.0f ms", ms);
        printf("1000 CREATE TABLE: %.0f ms\n", ms);
        db_shutdown();
        pid_t pp = fork();
        if (pp == 0) {                                        // processo nuovo: memoria del solo avvio del motore
            long before = 0, after = 0;
            char line[256];
            FILE* f = fopen("/proc/self/status", "r");
            while (f && fgets(line, sizeof(line), f)) if (!strncmp(line, "VmRSS:", 6)) before = atol(line + 6);
            if (f) fclose(f);
            if (!db_init(WORK)) _exit(2);
            f = fopen("/proc/self/status", "r");
            while (f && fgets(line, sizeof(line), f)) if (!strncmp(line, "VmRSS:", 6)) after = atol(line + 6);
            if (f) fclose(f);
            ok("USE big");
            bool good = num("SELECT COUNT(*) FROM t1999") == 0 && num("SELECT COUNT(*) FROM wide WHERE c299 = 'last'") == 1;
            printf("engine start with 2 001 tables: +%ld KB resident\n", after - before);
            _exit(!good ? 5 : after - before >= 20 * 1024 ? 6 : 0);
        }
        int ps;
        waitpid(pp, &ps, 0);
        CHECK(WIFEXITED(ps) && WEXITSTATUS(ps) == 0, "restart with 2 001 tables reads them back in under 20 MB of memory (exit %d)",
              WIFEXITED(ps) ? WEXITSTATUS(ps) : -1);
    }

    sh("rm -rf " BASE " " WORK " " CRASH);
    if (g_fail) { printf("durability tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d durability checks passed\n", g_tests, g_tests);
    return 0;
}
