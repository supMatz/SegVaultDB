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

    // ── 3. arresto alla k-esima scrittura di pagina durante ROLLBACK: il recupero finisce l'annullamento ──
    int ks = 0, done_all = 0;
    for (int k = 1; k < 400 && !done_all; k++) {
        sh("rm -rf " WORK " && cp -r " BASE " " WORK);
        pid_t pid = fork();
        if (pid == 0) {
            if (!db_init(WORK)) _exit(2);
            ok("USE d");
            ok("BEGIN");
            ok("DELETE FROM t WHERE id <= 200");
            ok("UPDATE t SET v = v + 1000 WHERE id > 400");
            ok("INSERT INTO t VALUES (20001, 5, 'new'), (20002, 6, 'new')");
            g_exit_pwrite = k;
            db_rollback();
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
        ok("ROLLBACK TO s");                            // scrive tutte le pagine modificate sul file
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
            DbSession* h = db_session_new();                // transazione aperta: niente checkpoint, il log cresce
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
        CHECK(rss[0] > 0 && rss[1] > 0 && rss[0] - rss[1] < 16 * 1024, "recovery of a %lld-byte log uses %ld KB more than a plain start (limit 16 MB)",
              (long long)ls.st_size, rss[0] - rss[1]);
        CHECK(db_init(WORK), "reopen after the big recovery");
        ok("USE d");
        CHECK(num("SELECT COUNT(*) FROM big") == 60000 && num("SELECT COUNT(*) FROM hold") == 0, "big recovery: committed rows kept, open one undone");
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
        unsigned v = 99;
        fseek(f, 8, SEEK_SET);
        fwrite(&v, 4, 1, f);
        fclose(f);
        CHECK(!db_init(WORK) && strstr(db_init_error(), "newer"), "catalog of a newer version refused: %s", db_init_error());
        f = fopen(WORK "/segvault.cat", "r+b");                       // un byte cambiato: il CRC lo vede
        fseek(f, 300, SEEK_SET);
        int ch = fgetc(f);
        fseek(f, 300, SEEK_SET);
        fputc(ch ^ 0x55, f);
        v = 5;
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

    sh("rm -rf " BASE " " WORK " " CRASH);
    if (g_fail) { printf("durability tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d durability checks passed\n", g_tests, g_tests);
    return 0;
}
