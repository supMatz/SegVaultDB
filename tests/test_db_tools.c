// Ricerca in tutto il database e panoramica delle tabelle.
#include "src/bridge/db_api.h"
#include "src/widgets/db_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(const char* sql) {
    QueryResult* r = db_execute(sql);
    if (!r || !r->success) printf("setup failed: %s -> %s\n", sql, r ? r->error : "null");
    db_result_free(r);
}

static int find_row(const QueryResult* r, int col, const char* v) {
    for (int i = 0; i < r->num_rows; i++) if (!strcmp(r->rows[i].cells[col].value, v)) return i;
    return -1;
}

int main(void) {
    if (system("rm -rf /tmp/sv_tools_test && mkdir -p /tmp/sv_tools_test") != 0) return 1;
    if (!db_init("/tmp/sv_tools_test")) return 1;
    run("CREATE DATABASE s");
    run("USE s");
    run("CREATE TABLE cliente (id INT PRIMARY KEY, nome VARCHAR(40), citta VARCHAR(40), nato DATE)");
    run("CREATE TABLE ordine (n INT PRIMARY KEY, cliente INT, nota TEXT, totale FLOAT, FOREIGN KEY (cliente) REFERENCES cliente(id))");
    run("CREATE TABLE vuota (x INT)");
    run("CREATE INDEX ix_citta ON cliente (citta)");
    run("INSERT INTO cliente VALUES (1, 'Anna Rossi', 'Roma', '1990-05-01'), (2, 'Marco', 'Milano', NULL), (3, 'Romano', 'Torino', NULL)");
    run("INSERT INTO ordine VALUES (10, 1, 'consegna a Roma', 12.5), (11, 2, 'it''s ok', 99.0)");

    // ricerca: maiuscole indifferenti, più colonne e tabelle, chiave della riga
    int hit = 0;
    bool trunc = true;
    QueryResult* r = dbt_search("s", "ROM", 0, &hit, &trunc);
    CHECK(r && r->success && r->num_cols == 4, "search result");
    CHECK(r && r->num_rows == 3 && hit == 2 && !trunc, "rows %d tables %d", r ? r->num_rows : -1, hit);
    CHECK(r && find_row(r, 2, "Anna Rossi") < 0, "Rossi does not match ROM");
    int i = r ? find_row(r, 2, "Romano") : -1;
    CHECK(i >= 0 && !strcmp(r->rows[i].cells[0].value, "cliente") && !strcmp(r->rows[i].cells[1].value, "nome")
          && !strcmp(r->rows[i].cells[3].value, "id=3"), "Romano hit with key");
    CHECK(r && find_row(r, 2, "consegna a Roma") >= 0, "text column of another table");
    db_result_free(r);
    // numeri, date, apici
    r = dbt_search("s", "12.5", 0, &hit, NULL);
    CHECK(r && r->num_rows == 1 && !strcmp(r->rows[0].cells[1].value, "totale"), "number column: %d", r ? r->num_rows : -1);
    db_result_free(r);
    r = dbt_search("s", "1990-05", 0, &hit, NULL);
    CHECK(r && r->num_rows == 1 && !strcmp(r->rows[0].cells[1].value, "nato"), "date column");
    db_result_free(r);
    r = dbt_search("s", "it's", 0, &hit, NULL);
    CHECK(r && r->num_rows == 1, "quote in the text");
    db_result_free(r);
    r = dbt_search("s", "nessuno", 0, &hit, NULL);
    CHECK(r && r->num_rows == 0 && hit == 0, "no hits");
    db_result_free(r);
    // limite
    r = dbt_search("s", "o", 2, &hit, &trunc);
    CHECK(r && r->num_rows == 2 && trunc, "limit");
    db_result_free(r);
    char* sql = dbt_search_select("s", "cliente", "a'b");
    CHECK(sql && strstr(sql, "`nome` LIKE '%a''b%' ESCAPE '!'"), "select: %s", sql ? sql : "null");
    free(sql);
    // % e _ cercati come caratteri
    run("INSERT INTO ordine VALUES (12, 3, 'sconto 100% sul_totale!', 1.0)");
    r = dbt_search("s", "100%", 0, &hit, NULL);
    CHECK(r && r->num_rows == 1 && !strcmp(r->rows[0].cells[1].value, "nota"), "percent literal: %d", r ? r->num_rows : -1);
    db_result_free(r);
    r = dbt_search("s", "l_t", 0, &hit, NULL);
    CHECK(r && r->num_rows == 1, "underscore literal: %d", r ? r->num_rows : -1);
    db_result_free(r);
    r = dbt_search("s", "e!", 0, &hit, NULL);
    CHECK(r && r->num_rows == 1, "escape char literal: %d", r ? r->num_rows : -1);
    db_result_free(r);
    sql = dbt_search_select("s", "cliente", "x");
    free(sql);

    // panoramica
    r = dbt_overview("s");
    CHECK(r && r->num_rows == 4, "overview rows %d", r ? r->num_rows : -1);
    i = r ? find_row(r, 0, "cliente") : -1;
    CHECK(i >= 0 && !strcmp(r->rows[i].cells[1].value, "3") && !strcmp(r->rows[i].cells[2].value, "4")
          && !strcmp(r->rows[i].cells[3].value, "id") && !strcmp(r->rows[i].cells[4].value, "1")
          && !strcmp(r->rows[i].cells[6].value, "ordine"), "cliente stats");
    i = r ? find_row(r, 0, "ordine") : -1;
    CHECK(i >= 0 && !strcmp(r->rows[i].cells[5].value, "1") && !r->rows[i].cells[6].value[0], "ordine stats");
    i = r ? find_row(r, 0, "vuota") : -1;
    CHECK(i >= 0 && r->rows[i].cells[3].is_null && !strcmp(r->rows[i].cells[1].value, "0"), "table without key");
    CHECK(r && !strcmp(r->rows[r->num_rows - 1].cells[0].value, "3 tables") && !strcmp(r->rows[r->num_rows - 1].cells[1].value, "6"), "totals");
    db_result_free(r);

    db_shutdown();
    if (system("rm -rf /tmp/sv_tools_test")) {}
    if (g_fail) { printf("db tools tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d db tools checks passed\n", g_tests, g_tests);
    return 0;
}
