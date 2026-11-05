#include "src/bridge/db_api.h"
#include "src/widgets/csv_import.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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


// lo statement deve fallire e il messaggio deve contenere "needle"
static void test_fail(const char* label, const char* sql, const char* needle) {
    tests++;
    printf("TEST %d: %-30s ", tests, label);
    QueryResult* r = db_execute(sql);
    if (!r) { printf("FAIL (NULL result)\n"); return; }
    if (r->success) printf("FAIL: expected an error\n");
    else if (!strstr(r->error, needle)) printf("FAIL: error \"%s\" lacks \"%s\"\n", r->error, needle);
    else { printf("OK (%s)\n", r->error); passed++; }
    db_result_free(r);
}

// la prima cella del risultato deve valere "want"
static void test_value(const char* label, const char* sql, const char* want) {
    tests++;
    printf("TEST %d: %-30s ", tests, label);
    QueryResult* r = db_execute(sql);
    if (!r || !r->success) { printf("FAIL: %s\n", r ? r->error : "NULL result"); db_result_free(r); return; }
    if (r->num_rows < 1 || r->num_cols < 1) printf("FAIL: no rows\n");
    else if (strcmp(r->rows[0].cells[0].value, want)) printf("FAIL: got \"%s\", want \"%s\"\n", r->rows[0].cells[0].value, want);
    else { printf("OK (%s)\n", want); passed++; }
    db_result_free(r);
}

// importa un CSV con csv_to_sql ed esegue lo script statement per statement
static void test_csv(const char* label, const char* csv, const char* table, int expect_rows) {
    tests++;
    printf("TEST %d: %-30s ", tests, label);
    CsvImportInfo info;
    char* sql = csv_to_sql(csv, table, true, 0, &info);
    if (!sql) { printf("FAIL: %s\n", info.error); return; }
    int ok = 1;
    char* p = sql;
    while (*p && ok) {
        char* end = strstr(p, ";\n");
        if (!end) break;
        *end = '\0';
        QueryResult* r = db_execute(p);
        if (!r || !r->success) { printf("FAIL: %s\n  in: %.120s\n", r ? r->error : "NULL", p); ok = 0; }
        db_result_free(r);
        p = end + 2;
    }
    if (ok && info.rows != expect_rows) { printf("FAIL: %d rows, want %d\n", info.rows, expect_rows); ok = 0; }
    if (ok) { printf("OK (%d rows, delimiter '%c')\n", info.rows, info.delimiter == '\t' ? 'T' : info.delimiter); passed++; }
    free(sql);
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
    test_sql("WHERE STR", "SELECT * FROM users WHERE name = 'Alice'", 1);
    test_sql("WHERE QUAL STR", "SELECT * FROM users WHERE users.name = 'Alice'", 1);
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
    test_sql("RIGHT JOIN", "SELECT * FROM t1 RIGHT JOIN t3 ON t1.a = t3.d", 3);

    // Qualified UPDATE/DELETE
    // ORDER BY and LIMIT with JOIN
    test_sql("JOIN ORDER BY", "SELECT t1.a, t3.c FROM t1 JOIN t3 ON t1.a = t3.d ORDER BY t1.a DESC", 2);
    test_sql("JOIN LIMIT", "SELECT t1.a, t3.c FROM t1 JOIN t3 ON t1.a = t3.d LIMIT 1", 1);
    test_sql("JOIN DISTINCT", "SELECT DISTINCT t1.a FROM t1, t3", 3);

    // GROUP BY
    test_sql("GROUP BY", "SELECT age FROM people GROUP BY age", 4);
    test_sql("HAVING", "SELECT age FROM people GROUP BY age HAVING age > 28", 2);

    test_sql("CREATE T4", "CREATE TABLE t4 (x INT, y INT)", -1);
    test_sql("INSERT t4(1,10)", "INSERT INTO t4 VALUES (1, 10)", -1);
    test_sql("INSERT t4(2,20)", "INSERT INTO t4 VALUES (2, 20)", -1);
    test_sql("UPDATE QUALIFIED", "UPDATE t4 SET y = 99 WHERE t4.x = 1", -1);
    test_sql("DELETE QUALIFIED", "DELETE FROM t4 WHERE t4.x = 2", -1);
    test_sql("SELECT AFTER UPD/DEL", "SELECT * FROM t4", 1);

    // Stringhe: apici raddoppiati, escape, errori leggibili
    test_sql("CREATE TABLE s", "CREATE TABLE s (id INT, v VARCHAR(100))", -1);
    test_sql("INSERT doubled quote", "INSERT INTO s VALUES (1, 'It''s ok')", -1);
    test_value("SELECT doubled quote", "SELECT v FROM s WHERE id = 1", "It's ok");
    test_sql("INSERT backslash esc", "INSERT INTO s VALUES (2, 'a\\'b\\\\c')", -1);
    test_value("SELECT backslash esc", "SELECT v FROM s WHERE id = 2", "a'b\\c");
    test_value("WHERE with quote", "SELECT id FROM s WHERE v = 'It''s ok'", "1");
    test_fail("Unterminated string", "INSERT INTO s VALUES (3, 'abc)", "Unterminated string");
    test_fail("Readable syntax error", "INSERT INTO s VALUES (3, 'x'", "Expected ')'");
    test_sql("DROP TABLE s", "DROP TABLE s", -1);
    test_fail("INSERT > 64 values", "INSERT INTO s VALUES (1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1)", "Too many values");

    // Import CSV: virgolette, a capo nei campi, separatore ';', tipi dedotti, zeri iniziali, NULL
    test_csv("CSV import", "id;Nome Cliente;Citt\xc3\xa0;amount;cap\r\n1;\"Rossi; Mario\";Roma;12.5;00184\r\n"
             "2;\"Line\nbreak \"\"quoted\"\"\";;7;20100\r\n3;O'Brien;Milano;-3;\r\n", "clienti", 3);
    test_value("CSV quoted delimiter", "SELECT nome_cliente FROM clienti WHERE id = 1", "Rossi; Mario");
    test_value("CSV leading zero kept", "SELECT cap FROM clienti WHERE id = 1", "00184");
    test_value("CSV apostrophe", "SELECT nome_cliente FROM clienti WHERE id = 3", "O'Brien");
    test_sql("CSV empty -> NULL", "SELECT * FROM clienti WHERE citta IS NULL", 1);
    test_sql("CSV float column", "SELECT * FROM clienti WHERE amount > 5", 2);
    test_sql("DROP TABLE clienti", "DROP TABLE clienti", -1);

    // NOT NULL, tipi e modificatori in minuscolo
    test_sql("create lowercase", "create table lc (id int primary key auto_increment, name varchar(20) not null, note text)", -1);
    test_value("lowercase int type", "DESCRIBE lc", "id");
    test_fail("NOT NULL on insert", "INSERT INTO lc (name, note) VALUES (NULL, 'x')", "cannot be NULL");
    test_sql("nullable by default", "INSERT INTO lc (name, note) VALUES ('a', NULL)", -1);
    test_value("auto_increment lower", "SELECT id FROM lc WHERE name = 'a'", "1");
    test_fail("NOT NULL on update", "UPDATE lc SET name = NULL WHERE id = 1", "cannot be NULL");
    {
        tests++;
        char* ddl = db_get_create_statement("testdb", "lc");
        printf("TEST %d: %-30s ", tests, "CREATE statement");
        if (ddl && strstr(ddl, "id INT PRIMARY KEY AUTO_INCREMENT") && strstr(ddl, "name VARCHAR(20) NOT NULL") &&
            strstr(ddl, "note TEXT")) { printf("OK\n"); passed++; }
        else printf("FAIL: %s\n", ddl ? ddl : "NULL");
        db_free_string(ddl);
    }
    test_sql("DROP TABLE lc", "DROP TABLE lc", -1);

    // DISTINCT su duplicati non adiacenti e con LIMIT
    test_sql("CREATE TABLE dd", "CREATE TABLE dd (id INT PRIMARY KEY, k VARCHAR(5))", -1);
    test_sql("dd 1", "INSERT INTO dd VALUES (1, 'a')", -1);
    test_sql("dd 2", "INSERT INTO dd VALUES (2, 'b')", -1);
    test_sql("dd 3", "INSERT INTO dd VALUES (3, 'a')", -1);
    test_sql("dd 4", "INSERT INTO dd VALUES (4, 'a')", -1);
    test_sql("DISTINCT non-adjacent", "SELECT DISTINCT k FROM dd", 2);
    test_sql("DISTINCT then LIMIT", "SELECT DISTINCT k FROM dd LIMIT 2", 2);
    test_sql("LIMIT", "SELECT * FROM dd LIMIT 3", 3);
    test_sql("DROP TABLE dd", "DROP TABLE dd", -1);

    // Validazione dei valori, DEFAULT, NULL, LIMIT, colonne sconosciute
    test_sql("CREATE TABLE vv", "CREATE TABLE vv (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(5), n INT, "
             "f FLOAT, ok BOOLEAN, d DATE, st VARCHAR(10) DEFAULT 'new')", -1);
    test_fail("text into INT", "INSERT INTO vv (n) VALUES ('abc')", "expects an integer");
    test_fail("INT out of range", "INSERT INTO vv (n) VALUES (99999999999)", "out of range");
    test_fail("text into FLOAT", "INSERT INTO vv (f) VALUES ('notanumber')", "expects a number");
    test_fail("VARCHAR too long", "INSERT INTO vv (name) VALUES ('toolong')", "too long");
    test_fail("impossible date", "INSERT INTO vv (d) VALUES ('2024-02-30')", "valid date");
    test_sql("leap day ok", "INSERT INTO vv (name, d, ok) VALUES ('a', '2024-02-29', TRUE)", -1);
    test_value("TRUE literal", "SELECT ok FROM vv WHERE name = 'a'", "1");
    test_value("DEFAULT applied", "SELECT st FROM vv WHERE name = 'a'", "new");
    test_sql("string 'NULL' is text", "INSERT INTO vv (id, name) VALUES (NULL, 'NULL')", -1);
    test_sql("'NULL' not null", "SELECT * FROM vv WHERE name IS NULL", 0);
    test_value("NULL id auto-incremented", "SELECT id FROM vv WHERE name = 'NULL'", "2");
    test_fail("UPDATE type checked", "UPDATE vv SET n = 'x' WHERE id = 1", "expects an integer");
    test_sql("LIMIT 0", "SELECT * FROM vv LIMIT 0", 0);
    test_fail("LIMIT negative", "SELECT * FROM vv LIMIT -1", "LIMIT expects");
    test_fail("unknown SELECT column", "SELECT nope FROM vv", "Unknown column");
    test_fail("unknown ORDER BY", "SELECT * FROM vv ORDER BY nope", "Unknown column");
    test_value("ORDER BY alias", "SELECT name AS who FROM vv ORDER BY who DESC", "a");
    test_fail("missing table name", "SELECT * FROM", "Expected");
    test_sql("DROP TABLE vv", "DROP TABLE vv", -1);

    test_sql("SHOW DATABASES", "SHOW DATABASES", 1);
    test_fail("SHOW nonsense", "SHOW FOO", "Expected DATABASES or TABLES");

    test_fail("ALTER unsupported form", "ALTER TABLE t1 ADD COLUMN x INT", "Only ALTER TABLE");

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
