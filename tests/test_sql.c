// Test SQL end-to-end: ogni caso confronta il risultato esatto.
//   atteso "a|b;c|d"   righe (celle separate da |, righe da ;), NULL scritto NULL
//   atteso "ERR:testo" lo statement deve fallire con un messaggio che contiene testo
//   atteso "OK:n"      statement senza risultato, n righe modificate
//   atteso "#n"        solo il numero di righe
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int tests = 0, failed = 0;

static void render(const QueryResult* r, char* out, size_t n) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < r->num_rows; i++) {
        for (int c = 0; c < r->num_cols; c++) {
            const Cell* cell = &r->rows[i].cells[c];
            len += (size_t)snprintf(out + len, n - len, "%s%s", c ? "|" : (i ? ";" : ""), cell->is_null ? "NULL" : cell->value);
            if (len >= n) return;
        }
    }
}

static void t(const char* sql, const char* expect) {
    tests++;
    QueryResult* r = db_execute(sql);
    char got[8192];
    if (!r) snprintf(got, sizeof(got), "ERR:(null result)");
    else if (!r->success) snprintf(got, sizeof(got), "ERR:%s", r->error);
    else if (r->num_cols == 0) snprintf(got, sizeof(got), "OK:%d", r->rows_affected);
    else if (expect[0] == '#') snprintf(got, sizeof(got), "#%d", r->num_rows);
    else render(r, got, sizeof(got));
    bool ok;
    if (!strncmp(expect, "ERR:", 4)) ok = !strncmp(got, "ERR:", 4) && strstr(got + 4, expect + 4);
    else ok = strcmp(got, expect) == 0;
    if (!ok) {
        failed++;
        printf("FAIL %d: %s\n   expected: %s\n   got:      %s\n", tests, sql, expect, got);
    }
    db_result_free(r);
}

// nomi e tipi delle colonne del risultato
static void cols(const char* sql, const char* expect_names, const char* expect_types) {
    tests++;
    QueryResult* r = db_execute(sql);
    char names[1024] = "", types[1024] = "";
    for (int c = 0; r && r->success && c < r->num_cols; c++) {
        snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", c ? "|" : "", r->col_names[c]);
        snprintf(types + strlen(types), sizeof(types) - strlen(types), "%s%s", c ? "|" : "",
                 r->col_types && r->col_types[c] ? r->col_types[c] : "?");
    }
    if (!r || !r->success || strcmp(names, expect_names) || strcmp(types, expect_types)) {
        failed++;
        printf("FAIL %d: %s\n   expected: %s / %s\n   got:      %s / %s %s\n", tests, sql, expect_names, expect_types,
               names, types, r && !r->success ? r->error : "");
    }
    db_result_free(r);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void) {
    if (system("rm -rf /tmp/sv_sql_test && mkdir -p /tmp/sv_sql_test") != 0) return 1;
    if (!db_init("/tmp/sv_sql_test")) { printf("FAIL: db_init\n"); return 1; }

    t("CREATE DATABASE shop", "OK:0");
    t("USE shop", "OK:0");

    // ── espressioni ──
    t("CREATE TABLE n (id INT PRIMARY KEY, a INT, b INT, f FLOAT, s VARCHAR(20))", "OK:0");
    t("INSERT INTO n VALUES (1, 7, 2, 1.5, 'Hello'), (2, -3, 0, NULL, NULL), (3, NULL, 4, 2.25, 'abc')", "OK:3");
    t("SELECT a + b * 2, (a + b) * 2, a - -b, -a FROM n WHERE id = 1", "11|18|9|-7");
    t("SELECT a / b, a / 0, 7 / 2 FROM n WHERE id = 1", "3.5|NULL|3.5");
    t("SELECT a + NULL, NULL = NULL, a IS NULL FROM n WHERE id = 3", "NULL|NULL|1");
    t("SELECT 9223372036854775807 + 1 FROM n WHERE id = 1", "ERR:out of range");
    t("SELECT f * 2 FROM n ORDER BY id", "3;NULL;4.5");
    t("SELECT id FROM n WHERE a * 2 > b + 5", "1");
    t("SELECT id FROM n WHERE NOT (a = 7)", "2");                       // NULL non diventa vero
    t("SELECT id FROM n WHERE NOT (a = 7) OR a IS NULL", "2;3");
    t("SELECT id FROM n WHERE a > 0 OR b > 3", "1;3");                 // NULL OR vero = vero
    t("SELECT id FROM n WHERE a > 0 AND b > 3", "");                   // NULL AND vero = NULL
    t("SELECT id FROM n WHERE a IN (7, NULL)", "1");
    t("SELECT id FROM n WHERE a NOT IN (7, NULL)", "");                // NOT IN con NULL: mai vero
    t("SELECT id FROM n WHERE b BETWEEN a AND a + 10", "2");
    t("SELECT id FROM n WHERE s LIKE 'h%' AND s NOT LIKE '%x%'", "1");
    t("SELECT id FROM n WHERE s LIKE NULL", "");
    t("SELECT TRUE + TRUE, FALSE FROM n WHERE id = 1", "2|0");

    // ── funzioni ──
    t("SELECT UPPER(s), LOWER(s), LENGTH(s), CHAR_LENGTH('città') FROM n WHERE id = 1", "HELLO|hello|5|5");
    t("SELECT CONCAT(s, '-', id), CONCAT(s, NULL), CONCAT_WS(',', 'a', NULL, 'b') FROM n WHERE id = 1", "Hello-1|NULL|a,b");
    t("SELECT SUBSTRING(s, 2, 3), SUBSTR(s, -2), LEFT(s, 2), RIGHT(s, 3), REPLACE(s, 'l', 'L') FROM n WHERE id = 1", "ell|lo|He|llo|HeLLo");
    t("SELECT TRIM('  x  '), LTRIM('  x'), RTRIM('x  '), REVERSE('abc') FROM n WHERE id = 1", "x|x|x|cba");
    t("SELECT ABS(-4), ROUND(2.5), ROUND(-2.5), ROUND(3.14159, 2), FLOOR(-1.5), CEIL(1.2), MOD(7, 3), MOD(7, 0) FROM n WHERE id = 1",
      "4|3|-3|3.14|-2|2|1|NULL");
    t("SELECT COALESCE(a, b, 0), IFNULL(s, 'none'), NULLIF(b, 0), IF(a > 0, 'pos', 'neg') FROM n ORDER BY id",
      "7|Hello|2|pos;-3|none|NULL|neg;4|abc|4|neg");
    t("SELECT POWER(2, 10), SQRT(16), SIGN(-3) FROM n WHERE id = 1", "1024|4|-1");
    t("SELECT YEAR('2024-03-09'), MONTH('2024-03-09'), DAY('2024-03-09') FROM n WHERE id = 1", "2024|3|9");
    t("SELECT LENGTH(NOW()), LENGTH(CURDATE()) FROM n WHERE id = 1", "19|10");
    t("SELECT NOSUCH(a) FROM n", "ERR:Unknown function");
    t("SELECT UPPER(a, b) FROM n", "ERR:Wrong number of arguments");

    // ── colonne, alias, ordinamento, LIMIT ──
    cols("SELECT id, a + b AS total, a * 1.5, s FROM n", "id|total|a * 1.5|s", "INT|INT|FLOAT|VARCHAR");
    t("SELECT id, a + b AS total FROM n ORDER BY total DESC", "1|9;2|-3;3|NULL");   // NULL in fondo con DESC
    t("SELECT id FROM n ORDER BY 1 DESC", "3;2;1");
    t("SELECT id, s FROM n ORDER BY s IS NULL, s", "1|Hello;3|abc;2|NULL");
    t("SELECT id FROM n ORDER BY id LIMIT 1 OFFSET 1", "2");
    t("SELECT id FROM n ORDER BY id LIMIT 1, 2", "2;3");
    t("SELECT id FROM n ORDER BY id LIMIT 0", "");
    t("SELECT ID, S FROM N WHERE Id = 1", "1|Hello");                   // nomi senza maiuscole/minuscole
    t("SELECT nope FROM n", "ERR:Unknown column 'nope'");
    t("SELECT id FROM n ORDER BY 9", "ERR:out of range");

    // ── aggregati e gruppi ──
    t("CREATE TABLE sales (id INT PRIMARY KEY AUTO_INCREMENT, region VARCHAR(10), product VARCHAR(10), qty INT, price DECIMAL)", "OK:0");
    t("INSERT INTO sales (region, product, qty, price) VALUES ('north','pen',10,1.5), ('north','ink',2,10.25), "
      "('south','pen',5,1.5), ('south','pen',1,1.5), ('east','ink',NULL,9.5)", "OK:5");
    t("SELECT COUNT(*), COUNT(qty), SUM(qty), MIN(price), MAX(price) FROM sales", "5|4|18|1.5|10.25");
    t("SELECT region, SUM(qty * price) AS revenue FROM sales GROUP BY region ORDER BY revenue DESC",
      "north|35.5;south|9;east|NULL");
    t("SELECT region, COUNT(*) AS n FROM sales GROUP BY region HAVING n > 1 ORDER BY region", "north|2;south|2");
    t("SELECT product, COUNT(DISTINCT region) FROM sales GROUP BY product ORDER BY product", "ink|2;pen|2");
    t("SELECT UPPER(region) AS r, AVG(qty) FROM sales GROUP BY r ORDER BY r", "EAST|NULL;NORTH|6;SOUTH|3");
    t("SELECT COUNT(*), SUM(qty), AVG(qty) FROM sales WHERE qty > 100", "0|NULL|NULL");
    t("SELECT region FROM sales GROUP BY region HAVING SUM(qty) >= 6 ORDER BY region", "north;south");
    t("SELECT SUM(qty) FROM sales WHERE SUM(qty) > 1", "ERR:not allowed in WHERE");
    t("SELECT SUM(MAX(qty)) FROM sales", "ERR:cannot be nested");
    t("SELECT DISTINCT region, product FROM sales ORDER BY region, product", "east|ink;north|ink;north|pen;south|pen");
    t("SELECT * FROM sales ORDER BY price DESC, id LIMIT 2", "2|north|ink|2|10.25;5|east|ink|NULL|9.5");
    cols("SELECT region, COUNT(*), AVG(qty), MAX(price), SUM(qty) FROM sales GROUP BY region",
         "region|COUNT(*)|AVG(qty)|MAX(price)|SUM(qty)", "VARCHAR|INT|FLOAT|DECIMAL|INT");

    // ── JOIN ──
    t("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(20), city VARCHAR(20))", "OK:0");
    t("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total FLOAT)", "OK:0");
    t("INSERT INTO customers VALUES (1,'Ada','Roma'), (2,'Bob','Milano'), (3,'Cy',NULL)", "OK:3");
    t("INSERT INTO orders VALUES (10,1,50), (11,1,25.5), (12,2,100), (13,9,1)", "OK:4");
    t("SELECT c.name, o.total FROM customers c JOIN orders o ON o.customer_id = c.id ORDER BY o.id",
      "Ada|50;Ada|25.5;Bob|100");
    t("SELECT c.name, o.id FROM customers c LEFT JOIN orders o ON o.customer_id = c.id ORDER BY c.id, o.id",
      "Ada|10;Ada|11;Bob|12;Cy|NULL");
    t("SELECT c.name, o.id FROM customers c RIGHT JOIN orders o ON o.customer_id = c.id ORDER BY o.id",
      "Ada|10;Ada|11;Bob|12;NULL|13");
    t("SELECT c.name, o.id FROM customers c JOIN orders o ON o.customer_id = c.id AND o.total > 30 ORDER BY o.id",
      "Ada|10;Bob|12");
    t("SELECT c.name FROM customers c JOIN orders o ON o.total > c.id * 40 ORDER BY c.id, o.id", "Ada;Ada;Bob");
    t("SELECT c.name, o.id FROM customers c JOIN orders o ON o.customer_id = c.id WHERE o.total < 60 AND c.city = 'Roma' ORDER BY o.id",
      "Ada|10;Ada|11");
    t("SELECT c.name, o.id FROM customers c JOIN orders o ON o.customer_id = c.id WHERE o.total > 90 OR c.name LIKE 'A%' ORDER BY o.id",
      "Ada|10;Ada|11;Bob|12");
    t("SELECT c.name, COUNT(o.id) AS n, SUM(o.total) FROM customers c LEFT JOIN orders o ON o.customer_id = c.id GROUP BY c.name ORDER BY c.name",
      "Ada|2|75.5;Bob|1|100;Cy|0|NULL");
    t("SELECT COUNT(*) FROM customers, orders", "12");
    t("SELECT COUNT(*) FROM customers CROSS JOIN orders", "12");
    t("SELECT a.name, b.name FROM customers a JOIN customers b ON b.id = a.id + 1 ORDER BY a.id", "Ada|Bob;Bob|Cy");
    t("SELECT id FROM customers c JOIN orders o ON o.customer_id = c.id", "ERR:ambiguous");
    t("SELECT x.id FROM customers c", "ERR:Unknown table 'x'");
    t("SELECT * FROM customers JOIN customers ON 1 = 1", "ERR:used twice");
    t("SELECT * FROM customers c LEFT JOIN orders o", "ERR:need an ON condition");
    cols("SELECT * FROM customers c JOIN orders o ON o.customer_id = c.id LIMIT 1",
         "c.id|c.name|c.city|o.id|o.customer_id|o.total", "INT|VARCHAR|VARCHAR|INT|INT|FLOAT");
    t("CREATE TABLE items (order_id INT, sku VARCHAR(10))", "OK:0");
    t("INSERT INTO items VALUES (10,'A'), (10,'B'), (12,'C')", "OK:3");
    t("SELECT c.name, i.sku FROM customers c JOIN orders o ON o.customer_id = c.id JOIN items i ON i.order_id = o.id ORDER BY i.sku",
      "Ada|A;Ada|B;Bob|C");

    // ── viste ──
    t("CREATE VIEW romans AS SELECT id, name FROM customers WHERE city = 'Roma'", "OK:0");
    t("SELECT * FROM romans", "1|Ada");
    t("CREATE VIEW big AS SELECT customer_id, SUM(total) AS spent FROM orders GROUP BY customer_id", "OK:0");
    t("SELECT customer_id FROM big WHERE spent > 60 ORDER BY spent DESC", "2;1");
    t("SELECT c.name, b.spent FROM customers c JOIN big b ON b.customer_id = c.id ORDER BY b.spent", "Ada|75.5;Bob|100");
    t("CREATE VIEW top AS SELECT * FROM big WHERE spent >= 100", "OK:0");
    t("SELECT * FROM top", "2|100");
    t("CREATE VIEW customers AS SELECT 1 FROM orders", "ERR:already exists");
    t("CREATE VIEW bad AS DELETE FROM orders", "ERR:must be defined by a SELECT");
    t("INSERT INTO romans VALUES (5, 'x')", "ERR:Cannot INSERT into a view");

    // ── INSERT ──
    t("CREATE TABLE p (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(10) NOT NULL, qty INT DEFAULT 5, created VARCHAR(30))", "OK:0");
    t("INSERT INTO p (name) VALUES ('a'), ('b')", "OK:2");
    t("INSERT INTO p (id, name) VALUES (10, 'c')", "OK:1");
    t("INSERT INTO p (name, qty) VALUES ('d', 1 + 1)", "OK:1");
    t("SELECT id, name, qty FROM p ORDER BY id", "1|a|5;2|b|5;10|c|5;11|d|2");
    t("INSERT INTO p (name) VALUES ('e'), (NULL)", "ERR:cannot be NULL");
    t("INSERT INTO p (id, name) VALUES (20, 'x'), (20, 'y')", "ERR:Duplicate entry '20'");
    t("INSERT INTO p (id, name) VALUES (21, 'x'), (1, 'y')", "ERR:Duplicate entry '1'");
    t("SELECT COUNT(*) FROM p", "4");                                   // nessuna riga dei batch falliti
    t("INSERT INTO p (name, created) VALUES ('now', NOW())", "OK:1");
    t("SELECT LENGTH(created) FROM p WHERE name = 'now'", "19");
    t("INSERT INTO p (name, name) VALUES ('a', 'b')", "ERR:listed twice");
    t("INSERT INTO p (name) VALUES ('a', 'b')", "ERR:Column count");
    t("INSERT INTO p (name) VALUES (qty)", "ERR:Unknown column 'qty' in VALUES");
    t("INSERT INTO p (name) VALUES ('a'), ('b', 'c')", "ERR:same number of values");

    // ── UPDATE ──
    t("CREATE TABLE u (id INT PRIMARY KEY, x INT, y INT, label VARCHAR(5))", "OK:0");
    t("INSERT INTO u VALUES (1, 10, 20, 'a'), (2, 30, 40, 'b'), (3, 50, 60, 'c')", "OK:3");
    t("UPDATE u SET x = y, y = x WHERE id = 1", "OK:1");                 // valori della riga prima della modifica
    t("SELECT x, y FROM u WHERE id = 1", "20|10");
    t("UPDATE u SET x = x + 1, label = CONCAT(label, '!')", "OK:3");
    t("SELECT x, label FROM u ORDER BY id", "21|a!;31|b!;51|c!");
    t("UPDATE u SET id = id + 1", "OK:3");                              // nessun falso duplicato
    t("SELECT id FROM u ORDER BY id", "2;3;4");
    t("UPDATE u SET id = 2 WHERE id = 4", "ERR:Duplicate entry '2'");
    t("UPDATE u SET id = 9", "ERR:Duplicate entry '9'");
    t("UPDATE u SET x = 'abc' WHERE id = 2", "ERR:expects an integer");
    t("UPDATE u SET label = 'toolong' WHERE id = 2", "ERR:too long");
    t("SELECT id, x, label FROM u ORDER BY id", "2|21|a!;3|31|b!;4|51|c!");   // invariato dopo gli errori
    t("UPDATE u SET nope = 1", "ERR:Unknown column 'nope' in SET");
    t("UPDATE u SET x = NULL WHERE id = 2", "OK:1");
    t("UPDATE u SET id = NULL WHERE id = 2", "ERR:cannot be NULL");

    // ── DELETE e indici ──
    t("DELETE FROM u WHERE id = 3", "OK:1");
    t("SELECT id FROM u ORDER BY id", "2;4");
    t("INSERT INTO u VALUES (3, 0, 0, 'z')", "OK:1");                   // la chiave liberata si riusa
    t("DELETE FROM u WHERE x IS NULL OR label = 'z'", "OK:2");
    t("SELECT id FROM u", "4");
    t("CREATE INDEX ix_label ON u (label)", "OK:0");
    t("INSERT INTO u VALUES (5, 1, 1, 'c!'), (6, 1, 1, 'q')", "OK:2");
    t("SELECT id FROM u WHERE label = 'c!' ORDER BY id", "4;5");
    t("UPDATE u SET label = 'w' WHERE id = 4", "OK:1");
    t("SELECT id FROM u WHERE label = 'c!'", "5");
    t("SELECT id FROM u WHERE label = 'w'", "4");
    t("CREATE INDEX ix_bad ON u (nope)", "ERR:Unknown column");
    t("CREATE INDEX ix_label ON u (x)", "ERR:already exists");

    // ── transazioni: gli indici seguono il ROLLBACK ──
    t("BEGIN", "OK:0");
    t("INSERT INTO u VALUES (100, 1, 1, 'r')", "OK:1");
    t("ROLLBACK", "OK:0");
    t("SELECT COUNT(*) FROM u WHERE id = 100", "0");
    t("INSERT INTO u VALUES (100, 1, 1, 'r')", "OK:1");
    t("SELECT label FROM u WHERE id = 100", "r");

    // ── EXPLAIN ──
    t("EXPLAIN SELECT * FROM u WHERE id = 4",
      "1|Index lookup|u using u.id (in-memory index);2|Filter|WHERE id = 4;3|Projection|id, x, y, label");
    t("EXPLAIN DELETE FROM u", "ERR:EXPLAIN works with SELECT");
    {
        tests++;
        QueryResult* r = db_execute("EXPLAIN SELECT c.name, SUM(o.total) FROM customers c JOIN orders o ON o.customer_id = c.id "
                                    "WHERE o.total > 1 GROUP BY c.name ORDER BY c.name LIMIT 5");
        char got[4096];
        if (r && r->success) render(r, got, sizeof(got)); else snprintf(got, sizeof(got), "ERR");
        if (!strstr(got, "Table scan") || !strstr(got, "Hash join") || !strstr(got, "Filter|WHERE o.total > 1") ||
            !strstr(got, "Aggregate|c.name") || !strstr(got, "Sort|c.name") || !strstr(got, "Limit|5 rows")) {
            failed++;
            printf("FAIL %d: EXPLAIN plan\n   got: %s\n", tests, got);
        }
        db_result_free(r);
    }

    // ── DDL ──
    t("CREATE TABLE d1 (a INT, A INT)", "ERR:defined twice");
    t("CREATE TABLE d1 (a INT PRIMARY KEY, b INT PRIMARY KEY)", "ERR:only one PRIMARY KEY");
    t("CREATE TABLE d1 (a STRNG)", "ERR:Unknown type");
    t("CREATE TABLE U (z INT)", "ERR:already exists");
    t("ALTER TABLE u RENAME TO customers", "ERR:already exists");
    t("ALTER TABLE u RENAME TO u2", "OK:0");
    t("SELECT COUNT(*) FROM u2", "4");
    t("SELECT * FROM u", "ERR:not found");

    // ── tipi numerici ──
    t("CREATE TABLE nums (id INT PRIMARY KEY, v BIGINT, d DECIMAL)", "OK:0");
    t("INSERT INTO nums VALUES (1, 5000000000, 10.5), (2, -3000000000, 9), (3, 7, 100)", "OK:3");
    t("SELECT id FROM nums ORDER BY v", "2;3;1");
    t("SELECT id FROM nums ORDER BY d", "2;1;3");
    t("SELECT MAX(d), MIN(d), SUM(v) FROM nums", "100|9|2000000007");
    t("SELECT id FROM nums WHERE d > 9.5 ORDER BY id", "1;3");

    // ── SELECT senza FROM ──
    t("SELECT 1", "1");
    t("SELECT 1 + 2 AS three, UPPER('x'), ROUND(2.5, 400), ROUND(123, -2)", "3|X|2.5|100");
    t("SELECT LENGTH(NOW()) WHERE 1 = 0", "");
    t("SELECT *", "ERR:needs a FROM");
    t("SELECT id", "ERR:Unknown column 'id'");

    // ── espressioni enormi: niente crash, limiti chiari ──
    {
        static char big[200000];
        size_t k = (size_t)sprintf(big, "SELECT id FROM n WHERE id = ");
        for (int i = 0; i < 3000; i++) big[k++] = '(';
        big[k++] = '1';
        for (int i = 0; i < 3000; i++) big[k++] = ')';
        big[k] = '\0';
        t(big, "ERR:too complex");
        k = (size_t)sprintf(big, "SELECT COUNT(*) FROM n WHERE id IN (");
        for (int i = 1; i <= 2000; i++) k += (size_t)sprintf(big + k, "%s%d", i > 1 ? ", " : "", i);
        sprintf(big + k, ")");
        t(big, "3");
        k = (size_t)sprintf(big, "SELECT COUNT(*) FROM n WHERE id = -1");
        for (int i = 1; i <= 1000; i++) k += (size_t)sprintf(big + k, " OR id = %d", i);
        t(big, "3");
        k = (size_t)sprintf(big, "SELECT COUNT(*) FROM n WHERE id > 0");
        for (int i = 1; i <= 1000; i++) k += (size_t)sprintf(big + k, " AND id < %d", 5000 + i);
        t(big, "3");
    }

    // ── prestazioni: chiave primaria indicizzata ──
    t("CREATE TABLE perf (id INT PRIMARY KEY, v VARCHAR(20))", "OK:0");
    double t0 = now_ms();
    char sql[128];
    for (int i = 1; i <= 20000; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO perf VALUES (%d, 'value %d')", i, i);
        QueryResult* r = db_execute(sql);
        if (!r || !r->success) { failed++; printf("FAIL perf insert %d: %s\n", i, r ? r->error : "null"); db_result_free(r); break; }
        db_result_free(r);
    }
    double t1 = now_ms();
    for (int i = 1; i <= 2000; i++) {
        snprintf(sql, sizeof(sql), "SELECT v FROM perf WHERE id = %d", (i * 7919) % 20000 + 1);
        QueryResult* r = db_execute(sql);
        if (!r || !r->success || r->num_rows != 1) { failed++; printf("FAIL perf lookup %d\n", i); db_result_free(r); break; }
        db_result_free(r);
    }
    double t2 = now_ms();
    tests++;
    printf("perf: 20000 INSERT in %.0f ms, 2000 lookups by key in %.0f ms\n", t1 - t0, t2 - t1);
    if (t1 - t0 > 20000 || t2 - t1 > 5000) { failed++; printf("FAIL: too slow\n"); }
    t("SELECT COUNT(*) FROM perf", "20000");
    t("INSERT INTO perf VALUES (777, 'dup')", "ERR:Duplicate entry '777'");

    db_shutdown();
    if (system("rm -rf /tmp/sv_sql_test") != 0) { /* nulla */ }
    printf("%s: %d/%d SQL tests passed\n", failed ? "FAIL" : "OK", tests - failed, tests);
    return failed ? 1 : 0;
}
