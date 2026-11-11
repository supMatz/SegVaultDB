// BACKUP TO 'cartella' e ripristino: la copia si apre come una cartella di dati normale.
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_err[1100];
static bool ok(const char* sql) {
    QueryResult* r = db_execute(sql);
    bool g = r && r->success;
    snprintf(g_err, sizeof(g_err), "%.1000s", r ? r->error : "null");
    db_result_free(r);
    return g;
}
static long long num(const char* sql) {
    QueryResult* r = db_execute(sql);
    long long v = r && r->success && r->num_rows && !r->rows[0].cells[0].is_null ? atoll(r->rows[0].cells[0].value) : -1;
    if (r && !r->success) snprintf(g_err, sizeof(g_err), "%.1000s", r->error);
    db_result_free(r);
    return v;
}

#define DIR  "/tmp/sv_backup_test"
#define DATA DIR "/data"
#define BK1  DIR "/bk1"
#define BK2  DIR "/bk2"

int main(void) {
    if (system("rm -rf " DIR " && mkdir -p " DATA) != 0) return 1;
    if (!db_init(DATA)) { printf("init failed\n"); return 1; }
    CHECK(ok("CREATE DATABASE shop") && ok("USE shop"), "setup");
    CHECK(ok("CREATE TABLE p (id INT PRIMARY KEY, nome VARCHAR(20), note TEXT)"), "table");
    CHECK(ok("INSERT INTO p VALUES (1, 'pane', NULL), (2, 'latte', NULL)"), "rows");
    char* big = malloc(100000);
    memset(big, 'x', 99990);
    strcpy(big + 99990, "');");
    char* sql = malloc(100100);
    snprintf(sql, 100100, "INSERT INTO p VALUES (3, 'grande', '%s", big);
    CHECK(ok(sql), "large row: %s", g_err);
    CHECK(ok("CREATE VIEW v AS SELECT nome FROM p WHERE id < 3") && ok("CREATE PROCEDURE conta() SELECT COUNT(*) FROM p"), "objects");
    CHECK(ok("CREATE USER anna IDENTIFIED BY 'anna-pass'"), "user");

    CHECK(ok("BEGIN") && !ok("BACKUP TO '" BK1 "'") && strstr(g_err, "transaction") && ok("ROLLBACK"), "no backup inside a transaction");
    CHECK(ok("BACKUP TO '" BK1 "'"), "backup: %s", g_err);
    CHECK(!ok("BACKUP DATABASE TO '" BK1 "'") && strstr(g_err, "already contains"), "existing backup not overwritten");
    CHECK(!ok("BACKUP TO '" DATA "'") && strstr(g_err, "data folder"), "not into the data folder");
    CHECK(!ok("BACKUP TO ''"), "empty folder name");

    // modifiche dopo il backup
    CHECK(ok("DELETE FROM p WHERE id = 1") && ok("DROP VIEW v") && ok("CREATE TABLE dopo (id INT)"), "changes after backup");
    CHECK(num("SELECT COUNT(*) FROM p") == 2, "2 rows now");

    // la copia è una cartella di dati: si apre da sola
    db_shutdown();
    CHECK(db_init(BK1), "open the backup");
    CHECK(ok("USE shop") && num("SELECT COUNT(*) FROM p") == 3, "backup has 3 rows: %s", g_err);
    CHECK(num("SELECT LENGTH(note) FROM p WHERE id = 3") == 99990, "large row in backup");
    CHECK(num("SELECT COUNT(*) FROM v") == 2 && num("CALL conta()") == 3, "view and procedure in backup");
    CHECK(!ok("SELECT * FROM dopo"), "later table not in backup");
    CHECK(db_login("anna", "anna-pass"), "users in backup");
    db_shutdown();

    // ripristino sui dati in uso
    CHECK(db_init(DATA) && ok("USE shop"), "reopen data");
    CHECK(ok("BACKUP TO '" BK2 "'"), "second backup");
    char err[600];
    CHECK(!db_restore(DIR "/nessuno", err, sizeof(err)) && strstr(err, "not a SegVault backup"), "restore from a non-backup: %s", err);
    CHECK(num("SELECT COUNT(*) FROM p") == 2, "nothing changed after failed restore");
    CHECK(db_restore(BK1, err, sizeof(err)), "restore: %s", err);
    CHECK(ok("USE shop") && num("SELECT COUNT(*) FROM p") == 3 && !ok("SELECT * FROM dopo"), "data restored");
    CHECK(num("SELECT COUNT(*) FROM v") == 2, "view restored");
    CHECK(ok("INSERT INTO p VALUES (4, 'nuovo', NULL)") && num("SELECT COUNT(*) FROM p") == 4, "writable after restore");
    // i dati di prima restano in before-restore
    db_shutdown();
    CHECK(db_init(DATA "/before-restore") && ok("USE shop") && num("SELECT COUNT(*) FROM p") == 2, "previous data kept: %s", g_err);
    db_shutdown();
    CHECK(db_init(DATA) && ok("USE shop") && num("SELECT COUNT(*) FROM p") == 4, "restored data survives restart");
    db_shutdown();

    free(big);
    free(sql);
    if (system("rm -rf " DIR) != 0) { /* nulla */ }
    printf("%s: %d/%d backup checks passed\n", g_fail ? "FAILED" : "OK", g_tests - g_fail, g_tests);
    return g_fail ? 1 : 0;
}
