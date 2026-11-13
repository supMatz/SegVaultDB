// Sessioni: stato separato (database, utente, transazione, savepoint) e un solo proprietario delle transazioni.
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

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
        CHECK(r && !r->success && strstr(r->error, "Lock wait timeout"), "a table written by s2 waits (no dirty read): %s", r ? r->error : "");
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
