// Sessioni: stato separato (database, utente, transazione, savepoint) e un solo proprietario delle transazioni.
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdatomic.h>
#include <time.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool ok(const char* sql) { QueryResult* r = db_execute(sql); bool g = r && r->success; db_result_free(r); return g; }
static long long num(const char* sql) {
    QueryResult* r = db_execute(sql);
    long long v = r && r->success && r->num_rows ? atoll(r->rows[0].cells[0].value) : -1;
    db_result_free(r);
    return v;
}
static const char* cur(void) { const char* c = db_current_database(); return c ? c : ""; }

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

// statement con il flag di annullamento del thread (Cancel della GUI, KILL QUERY): ferma anche l'attesa di un lock
typedef struct { const char* sql; atomic_int cancel; bool ok; char err[300]; double ms; } CancelArg;
static void* cancel_thread(void* p) {
    CancelArg* a = p;
    db_set_thread_cancel(&a->cancel);
    double t0 = now_ms();
    QueryResult* r = db_execute(a->sql);
    a->ms = now_ms() - t0;
    a->ok = r && r->success;
    snprintf(a->err, sizeof(a->err), "%.290s", r && !r->success ? r->error : "");
    db_result_free(r);
    db_set_thread_cancel(NULL);
    return NULL;
}

// lettori sovrapposti (P1.7): BEGIN; SELECT ... FOR SHARE; pausa; COMMIT in ciclo finché stop
typedef struct { DbSession* s; atomic_int* stop; int rounds; } RdArg;
static void enter_wait(DbSession* s) {
    for (;;) {
        unsigned long long seq = db_session_seq();
        if (db_session_enter(s)) return;
        db_session_wait(seq, 50);
    }
}
static void* reader_thread(void* p) {
    RdArg* a = p;
    while (!atomic_load(a->stop)) {
        enter_wait(a->s);
        QueryResult* r = db_execute("BEGIN");
        db_result_free(r);
        r = db_execute("SELECT COUNT(*) FROM a.t FOR SHARE");
        db_result_free(r);
        db_session_leave(a->s);
        usleep(15 * 1000);                           // tiene il lock S mentre gli altri entrano
        enter_wait(a->s);
        r = db_execute("COMMIT");
        db_result_free(r);
        db_session_leave(a->s);
        a->rounds++;
        usleep(2 * 1000);
    }
    return NULL;
}

// uno statement di una sessione in un thread (deadlock)
typedef struct { DbSession* s; const char* sql; bool ok; char err[300]; } DlArg;
static void* dl_thread(void* p) {
    DlArg* a = p;
    while (!db_session_enter(a->s)) usleep(1000);
    QueryResult* r = db_execute(a->sql);
    a->ok = r && r->success;
    snprintf(a->err, sizeof(a->err), "%.290s", r && !r->success ? r->error : "");
    db_result_free(r);
    db_session_leave(a->s);
    return NULL;
}

int main(void) {
    if (system("rm -rf /tmp/sv_sess_test && mkdir -p /tmp/sv_sess_test") != 0) return 1;
    if (!db_init("/tmp/sv_sess_test")) return 1;
    ok("CREATE DATABASE a"); ok("CREATE DATABASE b");
    ok("USE a"); ok("CREATE TABLE t (id INT PRIMARY KEY, v INT)"); ok("INSERT INTO t VALUES (1, 10)");
    ok("USE b"); ok("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    ok("USE a");

    DbSession* s1 = db_session_new();
    DbSession* s2 = db_session_new();
    // database corrente separato
    CHECK(db_session_enter(s1) && !cur()[0], "s1 starts without database");
    CHECK(ok("USE b") && !strcmp(cur(), "b"), "s1 uses b");
    db_session_leave(s1);
    CHECK(!strcmp(cur(), "a"), "default session still on a: %s", cur());
    CHECK(db_session_enter(s2) && ok("USE a") && num("SELECT COUNT(*) FROM t") == 1, "s2 on a");
    db_session_leave(s2);
    CHECK(db_session_enter(s1) && !strcmp(cur(), "b") && num("SELECT COUNT(*) FROM t") == 0, "s1 back on b");
    db_session_leave(s1);

    // transazione che dura oltre una "richiesta"; le altre sessioni lavorano accanto
    db_set_lock_wait_timeout(300);
    CHECK(db_session_enter(s2) && ok("BEGIN") && ok("INSERT INTO t VALUES (2, 20)") && ok("SAVEPOINT p")
          && ok("INSERT INTO t VALUES (3, 30)"), "s2 begins");
    db_session_leave(s2);
    CHECK(db_session_in_transaction(s2) && !db_session_blocked(), "s2 owns its transaction, nobody is blocked");
    CHECK(db_session_enter(s1) && ok("USE b") && ok("INSERT INTO t VALUES (7, 70)") && num("SELECT COUNT(*) FROM t") == 1,
          "s1 writes another table meanwhile");
    {
        QueryResult* r = db_execute("SELECT COUNT(*) FROM a.t");
        CHECK(r && r->success && !strcmp(r->rows[0].cells[0].value, "1"), "a table written by s2: committed version, no wait (MVCC): %s",
              r && r->success ? r->rows[0].cells[0].value : r ? r->error : "");
        db_result_free(r);
        r = db_execute("SELECT COUNT(*) FROM a.t FOR UPDATE");
        CHECK(r && !r->success && strstr(r->error, "Lock wait timeout"), "SELECT ... FOR UPDATE waits: %s", r ? r->error : "");
        db_result_free(r);
    }
    db_session_leave(s1);
    CHECK(db_session_enter(s2) && ok("ROLLBACK TO p") && num("SELECT COUNT(*) FROM t") == 2, "savepoint kept across requests");
    CHECK(ok("COMMIT"), "commit");
    db_session_leave(s2);
    CHECK(db_session_enter(s1) && num("SELECT COUNT(*) FROM a.t") == 2, "visible to s1 after COMMIT");
    CHECK(ok("DELETE FROM b.t") && num("SELECT COUNT(*) FROM b.t") == 0, "s1 cleans b");
    db_session_leave(s1);
    CHECK(num("SELECT COUNT(*) FROM t") == 2, "committed row visible to the default session");

    // MVCC: chi legge vede la versione confermata mentre un'altra transazione scrive, senza aspettare
    {
        ok("USE a");
        ok("CREATE TABLE m (id INT PRIMARY KEY, name TEXT, n INT)");
        ok("INSERT INTO m VALUES (1, 'uno', 10), (2, 'due', 20), (3, 'tre', 30), (4, 'quattro', 40)");
        DbSession* w = db_session_new();
        DbSession* rd = db_session_new();
        CHECK(db_session_enter(w) && ok("USE a") && ok("BEGIN") && ok("UPDATE m SET n = n + 1 WHERE id = 1") &&
              ok("UPDATE m SET name = 'due due due due due due due due due due due due due' WHERE id = 2") &&
              ok("DELETE FROM m WHERE id = 3") && ok("INSERT INTO m VALUES (5, 'cinque', 50)") &&
              ok("UPDATE m SET n = 99 WHERE id = 5") && ok("UPDATE m SET n = n * 100 WHERE id = 1") &&
              ok("SAVEPOINT x") && ok("DELETE FROM m WHERE id = 4") && ok("ROLLBACK TO x"), "writer changes rows");
        CHECK(num("SELECT COUNT(*) FROM m") == 4 && num("SELECT SUM(n) FROM m") == 1100 + 20 + 40 + 99, "the writer sees its own changes: %lld", num("SELECT SUM(n) FROM m"));
        db_session_leave(w);
        CHECK(db_session_enter(rd) && ok("USE a"), "reader enters");
        CHECK(num("SELECT COUNT(*) FROM m") == 4, "reader: committed row count: %lld", num("SELECT COUNT(*) FROM m"));
        CHECK(num("SELECT SUM(n) FROM m") == 100, "reader: committed values: %lld", num("SELECT SUM(n) FROM m"));
        CHECK(num("SELECT n FROM m WHERE id = 1") == 10, "reader: primary key lookup sees the committed version: %lld", num("SELECT n FROM m WHERE id = 1"));
        CHECK(num("SELECT COUNT(*) FROM m WHERE id = 3") == 1 && num("SELECT COUNT(*) FROM m WHERE id = 5") == 0, "reader: deleted row back, new row hidden");
        {
            QueryResult* r = db_execute("SELECT name FROM m WHERE id = 2");
            CHECK(r && r->success && r->num_rows == 1 && !strcmp(r->rows[0].cells[0].value, "due"), "reader: moved row in its old version: %s",
                  r && r->success && r->num_rows ? r->rows[0].cells[0].value : "?");
            db_result_free(r);
        }
        db_session_leave(rd);
        CHECK(db_session_enter(w) && ok("COMMIT"), "writer commits");
        db_session_leave(w);
        CHECK(db_session_enter(rd) && num("SELECT SUM(n) FROM m") == 1100 + 20 + 40 + 99 && num("SELECT COUNT(*) FROM m WHERE id = 3") == 0,
              "after COMMIT the reader sees the new version: %lld", num("SELECT SUM(n) FROM m"));
        db_session_leave(rd);
        // una transazione annullata non lascia tracce nella versione confermata
        CHECK(db_session_enter(w) && ok("BEGIN") && ok("DELETE FROM m") && num("SELECT COUNT(*) FROM m") == 0, "writer empties the table");
        db_session_leave(w);
        CHECK(db_session_enter(rd) && num("SELECT COUNT(*) FROM m") == 4, "reader still sees 4 rows: %lld", num("SELECT COUNT(*) FROM m"));
        db_session_leave(rd);
        CHECK(db_session_enter(w) && ok("ROLLBACK"), "writer rolls back");
        db_session_leave(w);
        CHECK(num("SELECT COUNT(*) FROM m") == 4, "all rows back");
        ok("DROP TABLE m");
        db_session_end(w);
        db_session_end(rd);
    }

    // ROLLBACK di una sessione sospesa (fine sessione = client sparito)
    CHECK(db_session_enter(s1) && ok("USE a") && ok("BEGIN") && ok("DELETE FROM t"), "s1 deletes in a transaction");
    db_session_leave(s1);
    db_session_end(s1);
    CHECK(!db_session_blocked() && num("SELECT COUNT(*) FROM t") == 2, "ending s1 rolled back its delete");

    // utente per sessione
    ok("CREATE USER u IDENTIFIED BY 'pass1234'");
    ok("GRANT SELECT ON a.* TO u");
    CHECK(db_session_enter(s2) && db_set_user("u") && ok("USE a") && !ok("DELETE FROM t"), "s2 as u");
    db_session_leave(s2);
    CHECK(!db_session_user()[0] && ok("DELETE FROM t WHERE id = 2"), "default session is root again");
    CHECK(db_session_enter(s2) && !strcmp(db_session_user(), "u"), "s2 keeps its user");
    db_session_leave(s2);
    db_session_end(s2);

    // la transazione della sessione predefinita (GUI) non blocca le sessioni che usano altre tabelle
    DbSession* s3 = db_session_new();
    ok("BEGIN");
    ok("INSERT INTO t VALUES (9, 90)");
    CHECK(db_session_enter(s3) && ok("USE b") && ok("INSERT INTO t VALUES (1, 1)"), "a session works beside the GUI transaction");
    db_session_leave(s3);
    ok("ROLLBACK");
    CHECK(num("SELECT COUNT(*) FROM t") == 1 && num("SELECT COUNT(*) FROM b.t") == 1, "GUI rollback undoes only its rows: %lld %lld", num("SELECT COUNT(*) FROM t"), num("SELECT COUNT(*) FROM b.t"));
    db_session_end(s3);

    // deadlock: s4 tiene a.t e vuole b.t, s5 tiene b.t e vuole a.t
    db_set_lock_wait_timeout(10000);
    DbSession* s4 = db_session_new();
    DbSession* s5 = db_session_new();
    CHECK(db_session_enter(s4) && ok("USE a") && ok("BEGIN") && ok("UPDATE t SET v = v + 1"), "s4 locks a.t");
    db_session_leave(s4);
    CHECK(db_session_enter(s5) && ok("USE b") && ok("BEGIN") && ok("UPDATE t SET v = v + 1"), "s5 locks b.t");
    db_session_leave(s5);
    DlArg x = { s4, "UPDATE b.t SET v = v + 10", false, "" }, y = { s5, "UPDATE a.t SET v = v + 10", false, "" };
    pthread_t tx, ty;
    pthread_create(&tx, NULL, dl_thread, &x);
    usleep(50000);
    pthread_create(&ty, NULL, dl_thread, &y);
    pthread_join(tx, NULL);
    pthread_join(ty, NULL);
    CHECK(x.ok != y.ok, "exactly one wins: %d %d (%s / %s)", x.ok, y.ok, x.err, y.err);
    DlArg* loser = x.ok ? &y : &x;
    CHECK(strstr(loser->err, "Deadlock found"), "the other gets a deadlock: %s", loser->err);
    CHECK(!db_session_in_transaction(loser->s), "the loser's transaction was rolled back");
    DbSession* winner = x.ok ? s4 : s5;
    CHECK(db_session_enter(winner) && ok("COMMIT"), "winner commits");
    db_session_leave(winner);
    CHECK(num("SELECT v FROM a.t WHERE id = 1") == (x.ok ? 11 : 20) , "a.t after the deadlock: %lld", num("SELECT v FROM a.t WHERE id = 1"));
    db_session_end(s4);
    db_session_end(s5);

    // P1.7: due sessioni leggono e poi scrivono la stessa tabella: nessun deadlock, confermano entrambe
    {
        db_set_lock_wait_timeout(5000);
        DbSession* ra = db_session_new();
        DbSession* rb = db_session_new();
        CHECK(db_session_enter(ra) && ok("USE a") && ok("BEGIN") && num("SELECT v FROM t WHERE id = 1") >= 0, "A reads t");
        db_session_leave(ra);
        CHECK(db_session_enter(rb) && ok("USE a") && ok("BEGIN") && num("SELECT v FROM t WHERE id = 1") >= 0, "B reads t");
        db_session_leave(rb);
        CHECK(db_session_enter(ra) && ok("UPDATE t SET v = v + 1 WHERE id = 1"), "A writes t after B read it");
        db_session_leave(ra);
        DlArg bw = { rb, "UPDATE t SET v = v + 1 WHERE id = 1", false, "" };
        pthread_t th;
        pthread_create(&th, NULL, dl_thread, &bw);       // B aspetta A (non un deadlock)
        usleep(100 * 1000);
        CHECK(db_session_enter(ra) && ok("COMMIT"), "A commits");
        db_session_leave(ra);
        pthread_join(th, NULL);
        CHECK(bw.ok, "B's write went through after A's commit: %s", bw.err);
        CHECK(db_session_enter(rb) && ok("COMMIT"), "B commits");
        db_session_leave(rb);
        db_session_end(ra);
        db_session_end(rb);
    }
    // P1.7: uno scrittore dietro 8 lettori che si sovrappongono (FOR SHARE) entra in tempo limitato (coda FIFO)
    {
        db_set_lock_wait_timeout(10000);
        atomic_int stop;
        atomic_init(&stop, 0);
        RdArg ra[8];
        pthread_t rt[8];
        for (int i = 0; i < 8; i++) { ra[i] = (RdArg){ db_session_new(), &stop, 0 }; pthread_create(&rt[i], NULL, reader_thread, &ra[i]); }
        usleep(150 * 1000);
        DbSession* wr = db_session_new();
        enter_wait(wr);
        double t0 = now_ms();
        bool wok = ok("UPDATE a.t SET v = v + 1 WHERE id = 1");
        double wms = now_ms() - t0;
        db_session_leave(wr);
        usleep(100 * 1000);
        atomic_store(&stop, 1);
        int rounds = 0;
        for (int i = 0; i < 8; i++) { pthread_join(rt[i], NULL); rounds += ra[i].rounds; db_session_end(ra[i].s); }
        db_session_end(wr);
        CHECK(wok && wms < 2000, "writer behind 8 readers: %s after %.0f ms", wok ? "granted" : "refused", wms);
        if (getenv("SV_VERBOSE")) printf("writer behind 8 readers: %.0f ms\n", wms);
        CHECK(rounds > 8, "readers kept going: %d rounds", rounds);
    }
    // P1.7: chi aspetta un lock dorme (nessun polling): quasi niente CPU in 1 s di attesa
    {
        db_set_lock_wait_timeout(1000);
        DbSession* holder = db_session_new();
        CHECK(db_session_enter(holder) && ok("USE a") && ok("BEGIN") && ok("UPDATE t SET v = v WHERE id = 1"), "holder takes X");
        db_session_leave(holder);
        struct timespec c0, c1;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
        double t0 = now_ms();
        CHECK(!ok("UPDATE a.t SET v = 0 WHERE id = 1"), "the waiter times out");
        double wall = now_ms() - t0;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
        double cpu = (c1.tv_sec - c0.tv_sec) * 1e3 + (c1.tv_nsec - c0.tv_nsec) / 1e6;
        CHECK(wall >= 900 && cpu < 60, "idle waiter: %.0f ms CPU in %.0f ms", cpu, wall);
        CHECK(db_session_enter(holder) && ok("ROLLBACK"), "holder rolls back");
        db_session_leave(holder);
        db_session_end(holder);
    }

    // P2.14: la GUI aspetta un lock tenuto da un'altra sessione; Cancel lo interrompe subito (non 30 s)
    {
        db_set_lock_wait_timeout(30000);
        DbSession* holder = db_session_new();
        CHECK(db_session_enter(holder) && ok("USE a") && ok("BEGIN") && ok("UPDATE t SET v = v + 100 WHERE id = 1"),
              "another session holds a write lock on a.t");
        db_session_leave(holder);
        CancelArg ca = { .sql = "UPDATE a.t SET v = 0 WHERE id = 1" };
        atomic_init(&ca.cancel, 0);
        pthread_t th;
        pthread_create(&th, NULL, cancel_thread, &ca);
        usleep(200 * 1000);
        atomic_store(&ca.cancel, 1);
        pthread_join(th, NULL);
        CHECK(!ca.ok && strstr(ca.err, "cancelled") && ca.ms < 2000, "lock wait cancelled after %.0f ms: %s", ca.ms, ca.err);
        CHECK(db_session_enter(holder) && ok("ROLLBACK"), "holder rolls back");
        db_session_leave(holder);
        db_session_end(holder);
        CHECK(ok("UPDATE a.t SET v = v WHERE id = 1"), "the table is usable afterwards");
    }

    db_shutdown();
    if (system("rm -rf /tmp/sv_sess_test")) {}

    // crash con più transazioni aperte insieme: al riavvio restano solo quelle confermate
    if (system("rm -rf /tmp/sv_sess_crash && mkdir -p /tmp/sv_sess_crash") != 0) return 1;
    pid_t pid = fork();
    if (pid == 0) {
        if (!db_init("/tmp/sv_sess_crash")) _exit(2);
        ok("CREATE DATABASE c"); ok("USE c");
        ok("CREATE TABLE a (id INT PRIMARY KEY, v TEXT)"); ok("CREATE TABLE b (id INT PRIMARY KEY, v TEXT)");
        ok("CREATE TABLE d (id INT PRIMARY KEY, v TEXT)");
        ok("INSERT INTO a VALUES (1, 'base')");
        DbSession* x1 = db_session_new();
        DbSession* x2 = db_session_new();
        DbSession* x3 = db_session_new();
        db_session_enter(x1); ok("USE c"); ok("BEGIN"); ok("INSERT INTO a VALUES (2, 'x1')"); ok("UPDATE a SET v = 'x1' WHERE id = 1"); db_session_leave(x1);
        db_session_enter(x2); ok("USE c"); ok("BEGIN"); ok("INSERT INTO b VALUES (1, 'x2')"); db_session_leave(x2);
        db_session_enter(x3); ok("USE c"); ok("BEGIN"); ok("INSERT INTO d VALUES (1, 'x3')"); ok("SAVEPOINT s");
        ok("INSERT INTO d VALUES (2, 'gone')"); ok("ROLLBACK TO s"); ok("INSERT INTO d VALUES (3, 'x3')"); db_session_leave(x3);
        db_session_enter(x1); ok("INSERT INTO a VALUES (3, 'x1')"); db_session_leave(x1);
        db_session_enter(x2); ok("INSERT INTO b VALUES (2, 'x2')"); ok("COMMIT"); db_session_leave(x2);
        db_session_enter(x3); ok("COMMIT"); db_session_leave(x3);
        _exit(0);                                    // x1 aperta: niente COMMIT né db_shutdown
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "crashing process ran");
    CHECK(db_init("/tmp/sv_sess_crash") && ok("USE c"), "recovery with several transactions");
    CHECK(num("SELECT COUNT(*) FROM a") == 1, "open transaction undone: %lld rows", num("SELECT COUNT(*) FROM a"));
    {
        QueryResult* r = db_execute("SELECT v FROM a WHERE id = 1");
        CHECK(r && r->success && r->num_rows == 1 && !strcmp(r->rows[0].cells[0].value, "base"), "its update undone too");
        db_result_free(r);
    }
    CHECK(num("SELECT COUNT(*) FROM b") == 2, "committed transaction kept: %lld", num("SELECT COUNT(*) FROM b"));
    CHECK(num("SELECT COUNT(*) FROM d") == 2 && num("SELECT COUNT(*) FROM d WHERE id = 2") == 0, "ROLLBACK TO inside a committed transaction respected");
    db_shutdown();
    if (system("rm -rf /tmp/sv_sess_crash")) {}
    if (g_fail) { printf("session tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d session checks passed\n", g_tests, g_tests);
    return 0;
}
