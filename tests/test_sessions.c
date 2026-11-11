// Sessioni: stato separato (database, utente, transazione, savepoint) e un solo proprietario delle transazioni.
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    // transazione che dura oltre una "richiesta"
    CHECK(db_session_enter(s2) && ok("BEGIN") && ok("INSERT INTO t VALUES (2, 20)") && ok("SAVEPOINT p")
          && ok("INSERT INTO t VALUES (3, 30)"), "s2 begins");
    db_session_leave(s2);
    CHECK(db_session_in_transaction(s2) && db_session_blocked(), "s2 owns the transaction");
    CHECK(!db_session_enter(s1) && !db_session_enter(NULL), "others wait while s2 has a transaction");
    CHECK(db_session_enter(s2) && ok("ROLLBACK TO p") && num("SELECT COUNT(*) FROM t") == 2, "savepoint kept across requests");
    CHECK(ok("COMMIT"), "commit");
    db_session_leave(s2);
    CHECK(!db_session_blocked() && db_session_enter(s1), "free again after COMMIT");
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

    // la transazione della sessione predefinita (GUI) blocca le sessioni
    DbSession* s3 = db_session_new();
    ok("BEGIN");
    CHECK(!db_session_enter(s3), "GUI transaction blocks sessions");
    ok("ROLLBACK");
    CHECK(db_session_enter(s3), "then they enter");
    db_session_leave(s3);
    db_session_end(s3);

    db_shutdown();
    if (system("rm -rf /tmp/sv_sess_test")) {}
    if (g_fail) { printf("session tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d session checks passed\n", g_tests, g_tests);
    return 0;
}
