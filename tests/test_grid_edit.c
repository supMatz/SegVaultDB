// Modifica dei dati nella griglia: analisi dei risultati e SQL generato, eseguito sul motore vero.
//   make test_grid_edit && ./test_grid_edit
#include "src/bridge/db_api.h"
#include "src/widgets/grid_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static QueryResult* q(const char* sql) {
    QueryResult* r = db_execute(sql);
    if (!r || !r->success) printf("  (sql error: %s -> %s)\n", sql, r ? r->error : "null");
    return r;
}
static void run(const char* sql) { db_result_free(q(sql)); }

// primo valore della prima riga
static char* scalar(const char* sql) {
    static char buf[512];
    QueryResult* r = db_execute(sql);
    snprintf(buf, sizeof(buf), "%s", r && r->success && r->num_rows ? (r->rows[0].cells[0].is_null ? "NULL" : r->rows[0].cells[0].value) : "(none)");
    db_result_free(r);
    return buf;
}

int main(void) {
    if (system("rm -rf /tmp/sv_grid_test && mkdir -p /tmp/sv_grid_test") != 0) return 1;
    if (!db_init("/tmp/sv_grid_test")) return 1;
    run("CREATE DATABASE g");
    run("USE g");
    run("CREATE TABLE p (id INT PRIMARY KEY AUTO_INCREMENT, nome VARCHAR(40) NOT NULL, prezzo FLOAT, ok BOOL DEFAULT TRUE, `order` INT)");
    run("INSERT INTO p (nome, prezzo, `order`) VALUES ('pane', 1.5, 1), ('latte', 1.2, 2)");
    run("CREATE TABLE coppia (a INT, b VARCHAR(5), v INT, PRIMARY KEY (a, b))");
    run("INSERT INTO coppia VALUES (1, 'x', 10), (1, 'y', 20)");
    run("CREATE TABLE nokey (x INT)");
    run("INSERT INTO nokey VALUES (1)");
    char err[256];
    GridEdit ge;

    // ── analisi ──
    QueryResult* r = q("SELECT * FROM p ORDER BY id");
    grid_edit_analyze(&ge, r);
    CHECK(grid_edit_any(&ge) && grid_edit_rows_ok(&ge) && grid_edit_cell_ok(&ge, 1), "SELECT * editable: %s", ge.why);
    CHECK(ge.t[0].auto_col == 0 && !strcmp(ge.col_type[1], "VARCHAR(40)"), "types and auto column");

    // UPDATE di un testo con apici e backslash: il valore deve arrivare identico
    char* sql = grid_edit_update_sql(&ge, &r->rows[0], 1, "l'acqua \\ fresca", false, err, sizeof(err));
    CHECK(sql && !strcmp(sql, "UPDATE `p` SET `nome` = 'l''acqua \\\\ fresca' WHERE `id` = 1"), "update sql: %s", sql ? sql : err);
    run(sql); free(sql);
    CHECK(!strcmp(scalar("SELECT nome FROM p WHERE id = 1"), "l'acqua \\ fresca"), "update applied: %s", scalar("SELECT nome FROM p WHERE id = 1"));
    // numero, booleano, NULL, colonna con nome riservato
    sql = grid_edit_update_sql(&ge, &r->rows[1], 2, " 2.75", false, err, sizeof(err));
    CHECK(sql && strstr(sql, "= 2.75 WHERE"), "number literal: %s", sql ? sql : err);
    run(sql); free(sql);
    sql = grid_edit_update_sql(&ge, &r->rows[1], 3, "false", false, err, sizeof(err));
    run(sql); free(sql);
    sql = grid_edit_update_sql(&ge, &r->rows[1], 4, NULL, true, err, sizeof(err));
    CHECK(sql && strstr(sql, "SET `order` = NULL"), "null + reserved name: %s", sql ? sql : err);
    run(sql); free(sql);
    CHECK(!strcmp(scalar("SELECT prezzo FROM p WHERE id = 2"), "2.75") && !strcmp(scalar("SELECT ok FROM p WHERE id = 2"), "0")
          && !strcmp(scalar("SELECT `order` FROM p WHERE id = 2"), "NULL"), "number / bool / null applied");
    // valore non valido: l'errore lo dà il motore e niente cambia
    sql = grid_edit_update_sql(&ge, &r->rows[0], 2, "abc", false, err, sizeof(err));
    QueryResult* bad = db_execute(sql);
    CHECK(bad && !bad->success && strstr(bad->error, "number"), "invalid number rejected: %s", bad ? bad->error : "");
    db_result_free(bad); free(sql);

    // INSERT con le sole colonne scritte (id generato) e rilettura della riga
    const char* vals[64] = {0}; bool nulls[64] = {0}, set[64] = {0};
    vals[1] = "burro"; set[1] = true;
    vals[2] = "3"; set[2] = true;
    sql = grid_edit_insert_sql(&ge, vals, nulls, set, err, sizeof(err));
    CHECK(sql && !strcmp(sql, "INSERT INTO `p` (`nome`, `prezzo`) VALUES ('burro', 3)"), "insert sql: %s", sql ? sql : err);
    QueryResult* ins = db_execute(sql);
    CHECK(ins && ins->success && ins->last_insert_id == 3, "insert id: %d", ins ? ins->last_insert_id : -1);
    char idtxt[16];
    snprintf(idtxt, sizeof(idtxt), "%d", ins ? ins->last_insert_id : 0);
    const char* key[1] = { idtxt };
    char* fetch = grid_edit_fetch_sql(&ge, 0, key);
    QueryResult* fr = db_execute(fetch);
    CHECK(fr && fr->success && fr->num_rows == 1 && !strcmp(fr->rows[0].cells[1].value, "burro") && !strcmp(fr->rows[0].cells[3].value, "1"),
          "fetch after insert (default applied): %s", fetch);
    db_result_free(fr); free(fetch); db_result_free(ins); free(sql);
    memset(set, 0, sizeof(set));
    CHECK(!grid_edit_insert_sql(&ge, vals, nulls, set, err, sizeof(err)), "insert with no values refused");

    // DELETE
    sql = grid_edit_delete_sql(&ge, &r->rows[1], err, sizeof(err));
    CHECK(sql && !strcmp(sql, "DELETE FROM `p` WHERE `id` = 2"), "delete sql: %s", sql ? sql : err);
    run(sql); free(sql);
    CHECK(!strcmp(scalar("SELECT COUNT(*) FROM p"), "2"), "delete applied");
    db_result_free(r);

    // ── chiave composta ──
    r = q("SELECT v, b, a FROM coppia ORDER BY b");
    grid_edit_analyze(&ge, r);
    sql = grid_edit_update_sql(&ge, &r->rows[1], 0, "99", false, err, sizeof(err));
    CHECK(sql && strstr(sql, "WHERE `a` = 1 AND `b` = 'y'"), "composite key: %s", sql ? sql : err);
    run(sql); free(sql);
    CHECK(!strcmp(scalar("SELECT v FROM coppia WHERE b = 'y'"), "99") && !strcmp(scalar("SELECT v FROM coppia WHERE b = 'x'"), "10"), "only that row");
    db_result_free(r);
    r = q("SELECT v, b FROM coppia");                              // manca a: sola lettura
    grid_edit_analyze(&ge, r);
    CHECK(!grid_edit_any(&ge) && strstr(ge.why, "primary key"), "partial key read-only: %s", ge.why);
    db_result_free(r);

    // ── join: ogni colonna appartiene alla sua tabella; niente INSERT / DELETE ──
    r = q("SELECT p.id, p.nome, c.a, c.b, c.v FROM p JOIN coppia c ON c.a = p.id");
    grid_edit_analyze(&ge, r);
    CHECK(grid_edit_cell_ok(&ge, 1) && grid_edit_cell_ok(&ge, 4) && !grid_edit_rows_ok(&ge), "join: cells yes, rows no");
    db_result_free(r);

    // ── colonne calcolate, GROUP BY, senza chiave ──
    r = q("SELECT id, prezzo * 2 AS doppio FROM p");
    grid_edit_analyze(&ge, r);
    CHECK(grid_edit_cell_ok(&ge, 0) && !grid_edit_cell_ok(&ge, 1), "computed column read-only");
    db_result_free(r);
    r = q("SELECT ok, COUNT(*) FROM p GROUP BY ok");
    grid_edit_analyze(&ge, r);
    CHECK(!grid_edit_any(&ge) && strstr(ge.why, "GROUP BY"), "group by read-only: %s", ge.why);
    db_result_free(r);
    r = q("SELECT * FROM nokey");
    grid_edit_analyze(&ge, r);
    CHECK(!grid_edit_any(&ge) && strstr(ge.why, "no primary key"), "no key: %s", ge.why);
    db_result_free(r);

    // export SQL / dump: un testo con ' e \ esportato come letterale e reinserito resta identico
    run("CREATE TABLE esc (s VARCHAR(40))");
    run("INSERT INTO esc VALUES ('a\\\\b''c\\\\n')");
    r = q("SELECT s FROM esc");
    char* lit = grid_edit_cell_literal(&r->rows[0].cells[0]);   // usata anche da export SQL e dump
    char* lit2 = grid_edit_cell_literal(&r->rows[0].cells[0]);
    char insq[200];
    snprintf(insq, sizeof(insq), "INSERT INTO esc VALUES (%s), (%s)", lit, lit2);
    run(insq);
    CHECK(!strcmp(scalar("SELECT COUNT(DISTINCT s) FROM esc"), "1") && !strcmp(r->rows[0].cells[0].value, "a\\b'c\\n"),
          "literal roundtrip: %s -> %s", r->rows[0].cells[0].value, lit);
    free(lit); free(lit2);
    db_result_free(r);

    db_shutdown();
    if (system("rm -rf /tmp/sv_grid_test")) {}
    if (g_fail) { printf("grid edit tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d grid edit checks passed\n", g_tests, g_tests);
    return 0;
}
