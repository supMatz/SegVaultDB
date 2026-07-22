#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>

static int tests = 0, passed = 0;

static void test_sql(const char* label, const char* sql, int expect_rows) {
    tests++;
    printf("TEST %d: %-30s ", tests, label);
    fflush(stdout);
    QueryResult* r = db_execute(sql);
    if (!r) { printf("FAIL (NULL result)\n"); return; }
    if (!r->success) { printf("FAIL: %s\n", r->error); db_result_free(r); return; }
    if (expect_rows >= 0) {
        if (r->num_rows == expect_rows) { printf("OK (%d rows)\n", r->num_rows); passed++; }
        else { printf("FAIL: expected %d rows, got %d\n", expect_rows, r->num_rows); }
    } else {
        printf("OK (affected=%d, rows=%d)\n", r->rows_affected, r->num_rows);
        passed++;
    }
    if (r->num_rows > 0 && r->num_rows <= 5) {
        for (int c = 0; c < r->num_cols; c++) printf("    %s", r->col_names[c]);
        printf("\n");
        for (int i = 0; i < r->num_rows; i++) {
            for (int c = 0; c < r->num_cols; c++)
                printf("    %s", r->rows[i].cells[c].value);
            printf("\n");
        }
    }
    db_result_free(r);
}

int main() {
    system("rm -rf /tmp/svtest && mkdir -p /tmp/svtest");
    if (!db_init("/tmp/svtest")) { printf("FAIL: db_init\n"); return 1; }

    test_sql("CREATE DATABASE", "CREATE DATABASE testdb", -1);
    test_sql("USE", "USE testdb", -1);
    test_sql("CREATE TABLE", "CREATE TABLE users (id INT, name VARCHAR(50), age INT)", -1);
    test_sql("INSERT 1", "INSERT INTO users VALUES (1, 'Alice', 30)", -1);
    test_sql("INSERT 2", "INSERT INTO users VALUES (2, 'Bob', 25)", -1);
    test_sql("INSERT 3", "INSERT INTO users VALUES (3, 'Charlie', 35)", -1);
    test_sql("SELECT *", "SELECT * FROM users", 3);
    test_sql("WHERE", "SELECT * FROM users WHERE id = 1", 1);
    test_sql("ORDER BY DESC", "SELECT * FROM users ORDER BY age DESC", 3);
    test_sql("LIMIT", "SELECT * FROM users LIMIT 2", 2);
    test_sql("DISTINCT", "SELECT DISTINCT age FROM users", 3);
    test_sql("EXPLAIN", "EXPLAIN SELECT * FROM users", 2);
    test_sql("CREATE VIEW", "CREATE VIEW v1 AS SELECT * FROM users WHERE age > 25", -1);
    test_sql("SELECT VIEW", "SELECT * FROM v1", 2);
    test_sql("ALTER TABLE", "ALTER TABLE users RENAME TO people", -1);

    // Transaction rollback test
    test_sql("BEGIN", "BEGIN", -1);
    test_sql("INSERT in tx", "INSERT INTO people VALUES (4, 'Dave', 40)", -1);
    test_sql("SELECT in tx (4 rows)", "SELECT * FROM people", 4);
    test_sql("ROLLBACK", "ROLLBACK", -1);
    test_sql("SELECT after (3 rows)", "SELECT * FROM people", 3);

    // Auto-increment
    test_sql("CREATE TABLE AI", "CREATE TABLE items (id INT AUTO_INCREMENT, name VARCHAR(50))", -1);
    test_sql("INSERT AI", "INSERT INTO items (name) VALUES ('Widget')", -1);
    test_sql("SELECT AI", "SELECT * FROM items", 1);

    // Index
    test_sql("CREATE INDEX", "CREATE INDEX idx_age ON people (age)", -1);
    test_sql("INDEX SCAN", "SELECT * FROM people WHERE age = 30", 1);

    // SAVEPOINT
    test_sql("BEGIN SP", "BEGIN", -1);
    test_sql("INSERT pre-sp", "INSERT INTO people VALUES (5, 'Eve', 28)", -1);
    test_sql("SAVEPOINT", "SAVEPOINT sp1", -1);
    test_sql("INSERT post-sp", "INSERT INTO people VALUES (6, 'Frank', 32)", -1);
    test_sql("ROLLBACK TO", "ROLLBACK TO sp1", -1);
    test_sql("SELECT after SP (4 rows)", "SELECT * FROM people", 4);
    test_sql("COMMIT", "COMMIT", -1);

    // Cross join
    test_sql("CREATE T1", "CREATE TABLE t1 (a INT)", -1);
    test_sql("CREATE T2", "CREATE TABLE t2 (b INT)", -1);
    test_sql("INSERT t1(1)", "INSERT INTO t1 VALUES (1)", -1);
    test_sql("INSERT t1(2)", "INSERT INTO t1 VALUES (2)", -1);
    test_sql("INSERT t2(10)", "INSERT INTO t2 VALUES (10)", -1);
    test_sql("INSERT t2(20)", "INSERT INTO t2 VALUES (20)", -1);
    test_sql("CROSS JOIN", "SELECT * FROM t1, t2", 4);

    // Qualified columns
    test_sql("QUALIFIED SELECT", "SELECT t1.a FROM t1", 2);
    test_sql("QUALIFIED WHERE", "SELECT * FROM t1 WHERE t1.a = 1", 1);

    // JOIN with ON
    test_sql("CREATE T3", "CREATE TABLE t3 (c INT, d INT)", -1);
    test_sql("INSERT t3(10,1)", "INSERT INTO t3 VALUES (10, 1)", -1);
    test_sql("INSERT t3(20,2)", "INSERT INTO t3 VALUES (20, 2)", -1);
    test_sql("INSERT t3(30,3)", "INSERT INTO t3 VALUES (30, 3)", -1);
    test_sql("JOIN ON", "SELECT * FROM t1 JOIN t3 ON t1.a = t3.d", 2);
    test_sql("JOIN WHERE", "SELECT t1.a, t3.c FROM t1 JOIN t3 ON t1.a = t3.d WHERE t1.a = 1", 1);
    test_sql("QUALIFIED JOIN COLS", "SELECT t1.a, t3.c FROM t1, t3 WHERE t1.a = t3.d", 2);

    // LEFT JOIN
    test_sql("INSERT t4(1)", "INSERT INTO t1 VALUES (4)", -1);
    test_sql("LEFT JOIN", "SELECT * FROM t1 LEFT JOIN t3 ON t1.a = t3.d", 3);

    // Qualified UPDATE/DELETE
    // ORDER BY and LIMIT with JOIN
    test_sql("JOIN ORDER BY", "SELECT t1.a, t3.c FROM t1 JOIN t3 ON t1.a = t3.d ORDER BY t1.a DESC", 2);
    test_sql("JOIN LIMIT", "SELECT t1.a, t3.c FROM t1 JOIN t3 ON t1.a = t3.d LIMIT 1", 1);

    // GROUP BY
    test_sql("GROUP BY", "SELECT age FROM people GROUP BY age", 4);

    test_sql("CREATE T4", "CREATE TABLE t4 (x INT, y INT)", -1);
    test_sql("INSERT t4(1,10)", "INSERT INTO t4 VALUES (1, 10)", -1);
    test_sql("INSERT t4(2,20)", "INSERT INTO t4 VALUES (2, 20)", -1);
    test_sql("UPDATE QUALIFIED", "UPDATE t4 SET y = 99 WHERE t4.x = 1", -1);
    test_sql("DELETE QUALIFIED", "DELETE FROM t4 WHERE t4.x = 2", -1);
    test_sql("SELECT AFTER UPD/DEL", "SELECT * FROM t4", 1);

    // Cleanup
    test_sql("DROP VIEW", "DROP VIEW v1", -1);
    test_sql("DROP TABLE", "DROP TABLE people", -1);
    test_sql("DROP TABLE t1", "DROP TABLE t1", -1);
    test_sql("DROP TABLE t2", "DROP TABLE t2", -1);
    test_sql("DROP TABLE t3", "DROP TABLE t3", -1);
    test_sql("DROP TABLE t4", "DROP TABLE t4", -1);
    test_sql("DROP DB", "DROP DATABASE testdb", -1);

    db_shutdown();
    printf("\n=== %d/%d TESTS PASSED ===\n", passed, tests);
    return passed == tests ? 0 : 1;
}
