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
#include <unistd.h>
#include <sys/wait.h>

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

// arresto anomalo: un processo figlio inserisce righe e termina senza chiudere il database;
// poi il log viene troncato a metà dell'ultima voce (crash durante la scrittura).
// Alla riapertura ci devono essere tutte le righe complete e nessun dato spazzatura.
static void crash_recovery_test(void) {
    const char* dir = "/tmp/sv_crash_test";
    if (system("rm -rf /tmp/sv_crash_test && mkdir -p /tmp/sv_crash_test") != 0) return;
    pid_t pid = fork();                              // (il chiamante ha già chiuso il motore)
    if (pid == 0) {
        if (!db_init(dir)) _exit(2);
        const char* q[] = { "CREATE DATABASE c", "USE c", "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(20))" };
        for (int i = 0; i < 3; i++) db_result_free(db_execute(q[i]));
        char sql[100];
        for (int i = 1; i <= 50; i++) {
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'row %d')", i, i);
            db_result_free(db_execute(sql));
            if (i == 20) db_result_free(db_execute("UPDATE t SET v = 'changed' WHERE id <= 5"));   // righe spostate
        }
        _exit(0);                                   // niente db_shutdown: pagine non scritte, log aperto
    }
    int st = 0;
    waitpid(pid, &st, 0);
    tests++;
    // tronca l'ultima voce del log a metà
    FILE* f = fopen("/tmp/sv_crash_test/segvault.wal", "rb+");
    long size = 0;
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
    if (size > 10 && truncate("/tmp/sv_crash_test/segvault.wal", size - 10) != 0) size = 0;
    if (!size) { failed++; printf("FAIL: crash test setup (no log)\n"); return; }

    if (!db_init(dir)) { failed++; printf("FAIL: reopen after crash\n"); return; }
    t("USE c", "OK:0");
    t("SELECT COUNT(*), MIN(id), MAX(id) FROM t", "49|1|49");   // l'ultima riga era nella voce troncata
    t("SELECT v FROM t WHERE id = 49", "row 49");
    t("SELECT COUNT(*) FROM t WHERE v = 'changed'", "5");                // le UPDATE rifatte dal log
    t("INSERT INTO t VALUES (50, 'again')", "OK:1");
    db_shutdown();
    if (system("rm -rf /tmp/sv_crash_test") != 0) { /* nulla */ }
}

// testo lungo: n caratteri 'a'..'z' ripetuti, poi il suffisso (stringhe che differiscono solo alla fine)
static char* longtext(int n, const char* suffix) {
    size_t sl = strlen(suffix);
    char* b = malloc((size_t)n + sl + 1);
    for (int i = 0; i < n; i++) b[i] = (char)('a' + i % 26);
    memcpy(b + n, suffix, sl + 1);
    return b;
}

// SQL con %s sostituiti dai testi lunghi
static void tl(const char* fmt, const char* a, const char* b, const char* expect) {
    size_t n = strlen(fmt) + (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + 1;
    char* q = malloc(n);
    snprintf(q, n, fmt, a ? a : "", b ? b : "");
    t(q, expect);
    free(q);
}

// origine delle colonne: "tabella.colonna" o "-" per le calcolate; "" se il risultato non è modificabile
static void origin(const char* sql, const char* expect) {
    tests++;
    QueryResult* r = db_execute(sql);
    char got[512] = "";
    if (!r || !r->success) snprintf(got, sizeof(got), "ERR:%.400s", r ? r->error : "null");
    else if (r->col_tables)
        for (int c = 0; c < r->num_cols; c++)
            snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%s%s%s", c ? "|" : "",
                     r->col_tables[c] ? r->col_tables[c] : "-", r->col_tables[c] ? "." : "", r->col_columns[c] ? r->col_columns[c] : "");
    if (strcmp(got, expect)) { failed++; printf("FAIL %d: %s\n   expected: %s\n   got:      %s\n", tests, sql, expect, got); }
    db_result_free(r);
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
    // date-time con i decimali dei secondi (come datetime(3) di MySQL)
    t("CREATE TABLE dtf (id INT PRIMARY KEY, d DATETIME)", "OK:0");
    t("INSERT INTO dtf VALUES (1, '2024-03-01 09:00:00.123'), (2, '2024-03-01 09:00:00')", "OK:2");
    t("INSERT INTO dtf VALUES (3, '2024-03-01 09:00:00.1234567')", "ERR:date-time");
    t("INSERT INTO dtf VALUES (3, '2024-03-01 09:00:00.')", "ERR:date-time");
    t("SELECT id FROM dtf ORDER BY d", "2;1");
    t("DROP TABLE dtf", "OK:0");
    // indici: UNIQUE (= vincolo) e su più colonne (accelera con la prima), sintassi di MySQL
    t("CREATE TABLE ux (id INT PRIMARY KEY, a INT, b VARCHAR(10))", "OK:0");
    t("INSERT INTO ux VALUES (1, 1, 'x'), (2, 1, 'y')", "OK:2");
    t("CREATE UNIQUE INDEX uq_ab ON ux (a, b)", "OK:0");
    t("INSERT INTO ux VALUES (3, 1, 'x')", "ERR:Duplicate");
    t("INSERT INTO ux VALUES (3, 1, 'z')", "OK:1");
    t("CREATE UNIQUE INDEX uq_a ON ux (a)", "ERR:duplicate values");
    t("CREATE INDEX ix_ab ON ux (a DESC, b(5)) USING BTREE", "OK:0");
    t("SELECT id FROM ux WHERE a = 1 AND b = 'z'", "3");
    t("DROP INDEX uq_ab ON ux", "OK:0");
    t("INSERT INTO ux VALUES (4, 1, 'x')", "OK:1");
    t("DROP INDEX ix_ab ON ux", "OK:0");
    t("DROP INDEX nope ON ux", "ERR:Index not found");
    t("DROP TABLE ux", "OK:0");
    // LIKE ... ESCAPE: il carattere dopo l'escape vale per sé
    t("CREATE TABLE lk (id INT PRIMARY KEY, s VARCHAR(30))", "OK:0");
    t("INSERT INTO lk VALUES (1, '100%'), (2, '100 euro'), (3, 'a_b'), (4, 'axb'), (5, 'c!d')", "OK:5");
    t("SELECT id FROM lk WHERE s LIKE '100!%' ESCAPE '!'", "1");
    t("SELECT id FROM lk WHERE s LIKE '%!%%' ESCAPE '!' ORDER BY id", "1");
    t("SELECT id FROM lk WHERE s LIKE 'a\\_b' ESCAPE '\\\\'", "3");      // in SQL il backslash si scrive '\\'
    t("SELECT id FROM lk WHERE s LIKE 'a#_b' ESCAPE '#'", "3");
    t("SELECT id FROM lk WHERE s LIKE 'a_b' ORDER BY id", "3;4");
    t("SELECT id FROM lk WHERE s LIKE 'c!!d' ESCAPE '!'", "5");
    t("SELECT id FROM lk WHERE s NOT LIKE '%!%%' ESCAPE '!' ORDER BY id", "2;3;4;5");
    t("SELECT id FROM lk WHERE s LIKE 'A!_B' ESCAPE '!'", "3");
    t("SELECT id FROM lk WHERE s LIKE '1%' ESCAPE 'ab'", "ERR:single character");
    t("SELECT '50%' LIKE '50!%' ESCAPE '!', '50x' LIKE '50!%' ESCAPE '!' FROM lk WHERE id = 1", "1|0");
    t("CREATE VIEW lkv AS SELECT id FROM lk WHERE s LIKE '%!_%' ESCAPE '!'", "OK:0");
    t("SELECT * FROM lkv", "3");
    t("DROP VIEW lkv", "OK:0");
    t("DROP TABLE lk", "OK:0");
    // JSON (come MySQL): funzioni e operatori -> / ->>
    t("CREATE TABLE js (id INT PRIMARY KEY, doc TEXT)", "OK:0");
    t("INSERT INTO js VALUES (1, '{\"nome\": \"Ada\", \"età\": 36, \"tag\": [\"a\", \"b\", {\"x\": 1.5}], \"ok\": true, \"n\": null}'), (2, '[1, 2, 3]'), (3, 'non json')", "OK:3");
    t("SELECT JSON_VALID(doc) FROM js ORDER BY id", "1;1;0");
    t("SELECT JSON_EXTRACT(doc, '$.nome') FROM js WHERE id = 1", "\"Ada\"");
    t("SELECT JSON_UNQUOTE(JSON_EXTRACT(doc, '$.nome')) FROM js WHERE id = 1", "Ada");
    t("SELECT doc->'$.nome', doc->>'$.nome', doc->>'$.\"età\"' FROM js WHERE id = 1", "\"Ada\"|Ada|36");
    t("SELECT doc->'$.tag[2].x', doc->>'$.tag[1]', doc->'$.manca' FROM js WHERE id = 1", "1.5|b|NULL");
    t("SELECT JSON_TYPE(doc), JSON_LENGTH(doc), JSON_LENGTH(doc, '$.tag') FROM js WHERE id = 1", "OBJECT|5|3");
    t("SELECT JSON_TYPE(doc->'$.ok'), JSON_TYPE(doc->'$.n'), JSON_TYPE(doc->'$.tag[2].x') FROM js WHERE id = 1", "BOOLEAN|NULL|DOUBLE");
    t("SELECT doc->'$[1]', JSON_LENGTH(doc), JSON_TYPE(doc->'$[0]') FROM js WHERE id = 2", "2|3|INTEGER");
    t("SELECT id FROM js WHERE doc->>'$.nome' = 'Ada'", "1");
    t("SELECT id FROM js WHERE JSON_VALID(doc) AND doc->'$[0]' = 1", "2");
    t("SELECT JSON_TYPE(doc) FROM js WHERE id = 3", "ERR:Invalid JSON");
    t("SELECT JSON_OBJECT('a', 1, 'b', 'x\"y', 'c', NULL, 'd', JSON_ARRAY(1, 'z')) FROM js WHERE id = 1",
      "{\"a\": 1, \"b\": \"x\\\"y\", \"c\": null, \"d\": [1, \"z\"]}");
    t("SELECT JSON_ARRAY(), JSON_OBJECT() FROM js WHERE id = 1", "[]|{}");
    t("SELECT JSON_OBJECT('a') FROM js WHERE id = 1", "ERR:key, value");
    t("SELECT JSON_UNQUOTE('\"a\\\\nb\\\\u00e8\"') FROM js WHERE id = 1", "a\nb\xc3\xa8");
    t("SELECT 5 - -3, 7 -1 FROM js WHERE id = 1", "8|6");
    t("DROP TABLE js", "OK:0");
    // nomi qualificati col database: db.tabella (lo statement gira in quel database)
    t("CREATE DATABASE altro", "OK:0");
    t("CREATE TABLE altro.p (id INT PRIMARY KEY, nome VARCHAR(20))", "OK:0");
    t("CREATE TABLE altro.q (id INT PRIMARY KEY, p_id INT REFERENCES p(id))", "OK:0");
    t("INSERT INTO altro.p VALUES (1, 'uno'), (2, 'due')", "OK:2");
    t("INSERT INTO `altro`.`q` VALUES (10, 1), (11, 2)", "OK:2");
    t("SELECT altro.p.nome, x.id FROM altro.p JOIN altro.q x ON x.p_id = altro.p.id ORDER BY x.id", "uno|10;due|11");
    t("SELECT nome FROM altro.p WHERE id IN (SELECT p_id FROM altro.q WHERE id = 11)", "due");
    t("UPDATE altro.p SET nome = 'UNO' WHERE id = 1", "OK:1");
    t("DELETE FROM altro.q WHERE id = 11", "OK:1");
    t("SELECT COUNT(*) FROM altro.q", "1");
    t("SELECT nome FROM altro.p p WHERE p.id = 1", "UNO");
    t("SELECT * FROM p", "ERR:");                                       // il database corrente non cambia
    t("SELECT * FROM altro.p JOIN js ON 1 = 1", "ERR:different databases");
    t("SELECT * FROM nessuno.p", "ERR:Unknown database");
    t("SELECT * FROM altro.p, shop.js", "ERR:different databases");
    t("CREATE INDEX ix_nome ON altro.p (nome)", "OK:0");
    t("DROP INDEX ix_nome ON altro.p", "OK:0");
    t("ALTER TABLE altro.p ADD COLUMN eta INT", "OK:0");
    t("DROP TABLE altro.q", "OK:0");
    t("DROP TABLE altro.p", "OK:0");
    t("DROP DATABASE altro", "OK:0");
    // stored procedure: parametri IN, IF / SIGNAL, risultato dell'ultima SELECT, atomiche
    t("CREATE TABLE conto (id INT PRIMARY KEY, saldo INT CHECK (saldo >= 0))", "OK:0");
    t("INSERT INTO conto VALUES (1, 100), (2, 50)", "OK:2");
    t("CREATE PROCEDURE bonifico(IN da INT, a INT, importo INT)\n"
      "BEGIN\n"
      "  IF importo <= 0 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'importo non valido'; END IF;\n"
      "  UPDATE conto SET saldo = saldo - importo WHERE id = da;\n"
      "  UPDATE conto SET saldo = saldo + importo WHERE id = a;\n"
      "  SELECT id, saldo FROM conto ORDER BY id;\n"
      "END", "OK:0");
    t("CALL bonifico(1, 2, 30)", "1|70;2|80");
    t("CALL bonifico(1, 2, 0)", "ERR:importo non valido");
    t("CALL bonifico(1, 2, 500)", "ERR:CHECK");                            // il primo UPDATE viola il vincolo
    t("CALL bonifico(2, 1, 80)", "1|150;2|0");
    t("CALL bonifico(2, 1, 1)", "ERR:Procedure 'bonifico'");               // il secondo passo fallisce: niente cambia
    t("SELECT saldo FROM conto ORDER BY id", "150;0");
    t("CALL bonifico(1, 2)", "ERR:expects 3 arguments");
    t("CALL nessuna()", "ERR:not found");
    t("CREATE PROCEDURE bonifico() SELECT 1", "ERR:already exists");
    t("CREATE PROCEDURE IF NOT EXISTS bonifico() SELECT 1", "OK:0");
    t("CREATE PROCEDURE nome_di(cid INT) SELECT CONCAT('conto ', id, ': ', saldo) FROM conto WHERE id = cid", "OK:0");
    t("CALL nome_di(1 + 1)", "conto 2: 0");
    t("CREATE PROCEDURE testo(s VARCHAR(20)) SELECT UPPER(s), LENGTH(s) FROM conto WHERE id = 1", "OK:0");
    t("CALL testo('l''ape')", "L'APE|5");
    t("CALL testo(NULL)", "NULL|NULL");
    t("CREATE PROCEDURE doppio(x INT) BEGIN CALL nome_di(x); INSERT INTO conto VALUES (x + 10, 5); END", "OK:0");
    t("CALL doppio(1)", "conto 1: 150");                                // la SELECT della CALL annidata
    t("SELECT COUNT(*) FROM conto", "3");
    t("CREATE PROCEDURE ricorsiva() CALL ricorsiva()", "OK:0");
    t("CALL ricorsiva()", "ERR:nested too deeply");
    t("CREATE PROCEDURE cattiva(OUT x INT) SELECT 1", "ERR:OUT and INOUT");
    t("CREATE PROCEDURE cattiva(x INT) BEGIN DROP TABLE conto; END", "ERR:");
    t("CREATE PROCEDURE cattiva(x INT, x INT) SELECT 1", "ERR:Duplicate parameter");
    t("DROP PROCEDURE ricorsiva", "OK:0");
    t("DROP PROCEDURE ricorsiva", "ERR:not found");
    t("DROP PROCEDURE IF EXISTS ricorsiva", "OK:0");
    t("DROP PROCEDURE doppio", "OK:0");
    t("DROP PROCEDURE testo", "OK:0");
    t("DROP PROCEDURE nome_di", "OK:0");
    t("DROP PROCEDURE bonifico", "OK:0");
    t("DROP TABLE conto", "OK:0");
    // ricerca full-text: MATCH ... AGAINST (come MySQL, anche BOOLEAN MODE)
    t("CREATE TABLE art (id INT PRIMARY KEY, titolo VARCHAR(60), testo TEXT)", "OK:0");
    t("INSERT INTO art VALUES (1, 'Basi di dati', 'Le basi di dati relazionali usano SQL. SQL è standard.'),"
      " (2, 'Reti', 'Protocolli di rete: TCP e IP.'), (3, 'Database NoSQL', 'Documenti JSON, niente SQL classico.'),"
      " (4, 'Città', 'Perché la città è bella')", "OK:4");
    t("SELECT id FROM art WHERE MATCH(titolo, testo) AGAINST('sql') ORDER BY MATCH(titolo, testo) AGAINST('sql') DESC, id", "1;3");
    t("SELECT id, MATCH(titolo, testo) AGAINST('SQL') FROM art WHERE id IN (1, 2) ORDER BY id", "1|1.6931;2|0");
    t("SELECT id FROM art WHERE MATCH(testo) AGAINST('tcp json') ORDER BY id", "2;3");
    t("SELECT id FROM art WHERE MATCH(titolo, testo) AGAINST('+sql -nosql' IN BOOLEAN MODE)", "1");
    t("SELECT id FROM art WHERE MATCH(titolo, testo) AGAINST('relaz*' IN BOOLEAN MODE)", "1");
    t("SELECT id FROM art WHERE MATCH(testo) AGAINST('relaz*')", "");                        // senza BOOLEAN MODE niente prefissi
    t("SELECT id FROM art WHERE MATCH(testo) AGAINST('\"di rete\"' IN BOOLEAN MODE)", "2");
    t("SELECT id FROM art WHERE MATCH(titolo, testo) AGAINST('+basi +reti' IN BOOLEAN MODE)", "");
    t("SELECT id FROM art WHERE MATCH(testo) AGAINST('CITTÀ città' IN NATURAL LANGUAGE MODE)", "4");
    t("SELECT id FROM art WHERE MATCH(testo) AGAINST('sql' WITH QUERY EXPANSION) ORDER BY id", "1;3");
    t("SELECT id FROM art WHERE MATCH(testo) AGAINST(NULL)", "");
    t("SELECT id FROM art WHERE MATCH(testo) 'x'", "ERR:Expected AGAINST");
    t("DROP TABLE art", "OK:0");
    // viste: OR REPLACE, ALTER, colonne, opzioni MySQL, SHOW CREATE VIEW / FULL TABLES, DESCRIBE, controlli
    t("CREATE TABLE vt (id INT PRIMARY KEY, v INT)", "OK:0");
    t("INSERT INTO vt VALUES (1, 5), (2, 20)", "OK:2");
    t("CREATE VIEW vg AS SELECT id, v FROM vt WHERE v > 10", "OK:0");
    t("CREATE VIEW vg AS SELECT id FROM vt", "ERR:already exists");
    t("CREATE OR REPLACE VIEW vg AS SELECT id FROM vt", "OK:0");
    t("SELECT * FROM vg ORDER BY id", "1;2");
    t("ALTER VIEW vg AS SELECT v FROM vt WITH CASCADED CHECK OPTION", "OK:0");
    t("SELECT * FROM vg ORDER BY v", "5;20");
    t("ALTER VIEW nessuna AS SELECT 1", "ERR:not found");
    t("CREATE VIEW vc (a, b) AS SELECT id, v FROM vt", "OK:0");
    t("SELECT b FROM vc WHERE a = 2", "20");
    t("CREATE VIEW vbad (a) AS SELECT id, v FROM vt", "ERR:names 1 column");
    t("CREATE VIEW vself AS SELECT * FROM vself", "ERR:not found");
    t("CREATE VIEW vbad AS SELECT nessuna FROM vt", "ERR:Unknown column");
    t("CREATE VIEW vh AS SELECT * FROM vg", "OK:0");
    t("CREATE OR REPLACE VIEW vg AS SELECT * FROM vh", "ERR:not found");             // niente cicli
    t("CREATE OR REPLACE ALGORITHM=UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW vm AS select `vt`.`id` AS `id` from `vt`", "OK:0");
    t("SELECT COUNT(*) FROM vm", "2");
    t("CREATE OR REPLACE TABLE vx (id INT)", "ERR:only supported for views");
    t("CREATE VIEW vt AS SELECT 1", "ERR:already exists");
    t("SHOW CREATE VIEW vc", "vc|CREATE VIEW vc AS WITH vc (a, b) AS (SELECT id, v FROM vt) SELECT * FROM vc");
    t("DESCRIBE vc", "a|INT|YES|;b|INT|YES|");
    t("SELECT id FROM vt WITH", "ERR:");
    t("DROP VIEW vm", "OK:0");
    t("DROP VIEW vh", "OK:0");
    t("DROP VIEW vc", "OK:0");
    t("DROP VIEW vg", "OK:0");
    t("DROP TABLE vt", "OK:0");
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
    t("SELECT * FROM customers c LEFT JOIN orders o", "ERR:need ON, USING or NATURAL");
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
      "1|Index lookup|u using u.id (in-memory index)|1;2|Filter|WHERE id = 4 (on u, before the joins)|1;3|Projection|id, x, y, label|1");
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
    t("SELECT id FROM perf ORDER BY id DESC LIMIT 3 OFFSET 2", "19998;19997;19996");           // top-k con heap
    t("SELECT id % 7, id FROM perf ORDER BY id % 7 DESC, id LIMIT 4", "6|6;6|13;6|20;6|27");  // pareggi: ordine stabile
    t("SELECT MAX(id), COUNT(DISTINCT id % 10) FROM perf GROUP BY id % 2 ORDER BY 1", "19999|5;20000|5");
    t("INSERT INTO perf VALUES (777, 'dup')", "ERR:Duplicate entry '777'");

    // ── Basi di Dati 1: subquery, operatori insiemistici, CASE, WITH (schema universitario) ──
    t("CREATE DATABASE uni", "OK:0");
    t("USE uni", "OK:0");
    t("CREATE TABLE studente (matricola INT PRIMARY KEY, nome VARCHAR(20), citta VARCHAR(20))", "OK:0");
    t("CREATE TABLE corso (codice VARCHAR(5) PRIMARY KEY, titolo VARCHAR(30), cfu INT)", "OK:0");
    t("CREATE TABLE esame (matricola INT, corso VARCHAR(5), voto INT)", "OK:0");
    t("INSERT INTO studente VALUES (1, 'Anna', 'Milano'), (2, 'Bruno', 'Roma'), (3, 'Carla', 'Milano'), (4, 'Dario', 'Torino'), (5, 'Elena', NULL)", "OK:5");
    t("INSERT INTO corso VALUES ('BD1', 'Basi di dati 1', 9), ('BD2', 'Basi di dati 2', 6), ('AN1', 'Analisi 1', 12)", "OK:3");
    t("INSERT INTO esame VALUES (1, 'BD1', 30), (1, 'BD2', 28), (1, 'AN1', 25), (2, 'BD1', 18), (2, 'AN1', 30), "
      "(3, 'BD1', 27), (3, 'BD2', 30), (3, 'AN1', 24), (4, 'BD2', 22)", "OK:9");
    // IN / NOT IN
    t("SELECT nome FROM studente WHERE matricola IN (SELECT matricola FROM esame WHERE voto = 30) ORDER BY nome", "Anna;Bruno;Carla");
    t("SELECT nome FROM studente WHERE matricola NOT IN (SELECT matricola FROM esame)", "Elena");
    t("SELECT nome FROM studente WHERE citta NOT IN (SELECT citta FROM studente WHERE matricola > 3)", "");   // la subquery contiene NULL
    t("SELECT nome FROM studente WHERE citta IN (SELECT citta FROM studente WHERE matricola > 3)", "Dario");
    t("SELECT nome FROM studente WHERE matricola IN (SELECT matricola FROM esame WHERE corso IN "
      "(SELECT codice FROM corso WHERE cfu = 6)) ORDER BY nome", "Anna;Carla;Dario");
    // EXISTS / NOT EXISTS correlate
    t("SELECT nome FROM studente s WHERE EXISTS (SELECT * FROM esame e WHERE e.matricola = s.matricola AND e.voto >= 28) ORDER BY nome",
      "Anna;Bruno;Carla");
    t("SELECT nome FROM studente s WHERE NOT EXISTS (SELECT * FROM esame e WHERE e.matricola = s.matricola)", "Elena");
    // divisione relazionale: studenti che hanno superato TUTTI i corsi
    t("SELECT nome FROM studente s WHERE NOT EXISTS (SELECT * FROM corso c WHERE NOT EXISTS "
      "(SELECT * FROM esame e WHERE e.matricola = s.matricola AND e.corso = c.codice)) ORDER BY nome", "Anna;Carla");
    t("SELECT s.nome FROM studente s JOIN esame e ON e.matricola = s.matricola GROUP BY s.nome "
      "HAVING COUNT(*) = (SELECT COUNT(*) FROM corso) ORDER BY s.nome", "Anna;Carla");
    // subquery scalari
    t("SELECT DISTINCT s.nome FROM studente s JOIN esame e ON e.matricola = s.matricola WHERE e.voto = (SELECT MAX(voto) FROM esame) ORDER BY s.nome",
      "Anna;Bruno;Carla");
    t("SELECT nome, (SELECT COUNT(*) FROM esame e WHERE e.matricola = s.matricola) FROM studente s ORDER BY matricola",
      "Anna|3;Bruno|2;Carla|3;Dario|1;Elena|0");
    t("SELECT nome, (SELECT AVG(voto) FROM esame e WHERE e.matricola = s.matricola) AS media FROM studente s WHERE matricola = 2", "Bruno|24");
    t("SELECT matricola FROM esame GROUP BY matricola HAVING AVG(voto) > (SELECT AVG(voto) FROM esame) ORDER BY matricola", "1;3");
    t("SELECT (SELECT voto FROM esame)", "ERR:more than one row");
    t("SELECT (SELECT voto FROM esame WHERE voto > 100)", "NULL");
    t("SELECT nome FROM studente WHERE matricola IN (SELECT matricola, voto FROM esame)", "ERR:exactly one column");
    t("SELECT nome FROM studente WHERE matricola IN (SELECT pippo FROM esame)", "ERR:Unknown column 'pippo'");
    // ANY / ALL
    t("SELECT matricola, voto FROM esame WHERE voto >= ALL (SELECT voto FROM esame) ORDER BY matricola", "1|30;2|30;3|30");
    t("SELECT DISTINCT matricola FROM esame WHERE voto < ANY (SELECT voto FROM esame WHERE matricola = 4)", "2");
    t("SELECT COUNT(*) FROM esame WHERE voto > ALL (SELECT voto FROM esame WHERE matricola = 99)", "9");   // ALL su insieme vuoto: vero
    // tabelle derivate e WITH
    t("SELECT AVG(n) FROM (SELECT matricola, COUNT(*) AS n FROM esame GROUP BY matricola) AS t", "2.25");
    t("SELECT MAX(n) FROM (SELECT COUNT(*) AS n FROM esame GROUP BY corso)", "3");
    t("WITH medie AS (SELECT matricola, AVG(voto) AS media FROM esame GROUP BY matricola) "
      "SELECT s.nome FROM studente s JOIN medie m ON m.matricola = s.matricola WHERE m.media >= 27 ORDER BY s.nome", "Anna;Carla");
    t("WITH a AS (SELECT matricola FROM esame WHERE voto = 30), b AS (SELECT * FROM a WHERE matricola > 1) SELECT COUNT(*) FROM b", "2");
    // UNION / INTERSECT / EXCEPT
    t("SELECT citta FROM studente WHERE citta IS NOT NULL UNION SELECT 'Roma' ORDER BY citta", "Milano;Roma;Torino");
    t("SELECT COUNT(*) FROM (SELECT matricola FROM esame UNION ALL SELECT matricola FROM studente) AS u", "14");
    t("SELECT matricola FROM esame WHERE corso = 'BD1' INTERSECT SELECT matricola FROM esame WHERE corso = 'BD2' ORDER BY 1", "1;3");
    t("SELECT matricola FROM studente EXCEPT SELECT matricola FROM esame", "5");
    t("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3", "1");                 // INTERSECT lega più di UNION
    t("SELECT 2 UNION SELECT 1 UNION SELECT 2 ORDER BY 1 DESC LIMIT 1", "2");
    t("SELECT 1 UNION SELECT 1, 2", "ERR:same number of columns");
    t("SELECT nome FROM studente UNION SELECT titolo FROM corso ORDER BY voto", "ERR:must name a result column");
    // funzioni finestra
    t("SELECT corso, matricola, ROW_NUMBER() OVER (PARTITION BY corso ORDER BY voto DESC) AS pos FROM esame ORDER BY corso, pos",
      "AN1|2|1;AN1|1|2;AN1|3|3;BD1|1|1;BD1|3|2;BD1|2|3;BD2|3|1;BD2|1|2;BD2|4|3");
    t("SELECT voto, RANK() OVER (ORDER BY voto DESC), DENSE_RANK() OVER (ORDER BY voto DESC) FROM esame ORDER BY voto DESC, matricola",
      "30|1|1;30|1|1;30|1|1;28|4|2;27|5|3;25|6|4;24|7|5;22|8|6;18|9|7");
    t("SELECT matricola, corso, SUM(voto) OVER (PARTITION BY matricola ORDER BY corso) FROM esame WHERE matricola = 1 ORDER BY corso",
      "1|AN1|25;1|BD1|55;1|BD2|83");
    t("SELECT matricola, voto, AVG(voto) OVER (PARTITION BY matricola) FROM esame WHERE matricola = 2 ORDER BY voto", "2|18|24;2|30|24");
    t("SELECT COUNT(*) OVER () FROM esame LIMIT 1", "9");
    t("SELECT corso, voto, LAG(voto) OVER (PARTITION BY corso ORDER BY voto), LEAD(voto, 1, 0) OVER (PARTITION BY corso ORDER BY voto) "
      "FROM esame WHERE corso = 'BD1' ORDER BY voto", "BD1|18|NULL|27;BD1|27|18|30;BD1|30|27|0");
    t("SELECT corso, RANK() OVER (ORDER BY AVG(voto) DESC) FROM esame GROUP BY corso ORDER BY 2", "BD2|1;AN1|2;BD1|3");
    t("SELECT corso, matricola FROM (SELECT corso, matricola, ROW_NUMBER() OVER (PARTITION BY corso ORDER BY voto DESC) AS r FROM esame) AS t "
      "WHERE r = 1 ORDER BY corso", "AN1|2;BD1|1;BD2|3");
    t("SELECT voto, NTILE(2) OVER (ORDER BY voto) FROM esame WHERE corso = 'BD1' ORDER BY voto", "18|1;27|1;30|2");
    t("SELECT DISTINCT corso, FIRST_VALUE(matricola) OVER (PARTITION BY corso ORDER BY voto DESC) FROM esame ORDER BY corso", "AN1|2;BD1|1;BD2|3");
    t("SELECT matricola, MAX(voto) OVER (PARTITION BY matricola) - voto AS distacco FROM esame WHERE corso = 'AN1' ORDER BY matricola", "1|0;2|0;3|0");
    t("SELECT voto FROM esame WHERE ROW_NUMBER() OVER () = 1", "ERR:not allowed in WHERE");
    t("SELECT ROW_NUMBER() FROM esame", "ERR:Unknown function");
    t("SELECT UPPER(corso) OVER () FROM esame", "ERR:cannot be used with OVER");
    // frame delle funzioni finestra
    t("SELECT voto, SUM(voto) OVER (ORDER BY voto ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM esame WHERE corso = 'BD1' ORDER BY voto",
      "18|45;27|75;30|57");
    t("SELECT voto, MAX(voto) OVER (ORDER BY voto ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING) FROM esame WHERE corso = 'BD1' ORDER BY voto",
      "18|27;27|30;30|30");
    t("SELECT voto, MIN(voto) OVER (ORDER BY voto DESC ROWS 1 PRECEDING) FROM esame WHERE corso = 'BD1' ORDER BY voto DESC",
      "30|30;27|27;18|18");
    t("SELECT voto, COUNT(*) OVER (ORDER BY voto ROWS 1 PRECEDING) FROM esame WHERE corso = 'BD1' ORDER BY voto", "18|1;27|2;30|2");
    t("SELECT voto, SUM(voto) OVER (ORDER BY voto ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING) FROM esame WHERE corso = 'BD1' ORDER BY voto",
      "18|NULL;27|18;30|45");
    t("SELECT voto, LAST_VALUE(voto) OVER (ORDER BY voto ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM esame WHERE corso = 'BD1' ORDER BY voto",
      "18|30;27|30;30|30");
    t("SELECT DISTINCT voto, COUNT(*) OVER (ORDER BY voto DESC RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) FROM esame ORDER BY voto DESC",
      "30|9;28|6;27|5;25|4;24|3;22|2;18|1");
    t("SELECT DISTINCT voto, COUNT(*) OVER (ORDER BY voto DESC RANGE UNBOUNDED PRECEDING) FROM esame ORDER BY voto DESC LIMIT 2", "30|3;28|4");
    t("SELECT corso, voto, ROUND(AVG(voto) OVER (PARTITION BY corso ORDER BY voto ROWS BETWEEN 1 PRECEDING AND CURRENT ROW), 1) FROM esame "
      "WHERE corso <> 'BD2' ORDER BY corso, voto", "AN1|24|24;AN1|25|24.5;AN1|30|27.5;BD1|18|18;BD1|27|22.5;BD1|30|28.5");
    t("SELECT SUM(voto) OVER (ORDER BY voto RANGE 1 PRECEDING) FROM esame", "ERR:RANGE frames support only");
    t("SELECT SUM(voto) OVER (ORDER BY voto ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM esame", "ERR:must start before it ends");
    t("SELECT SUM(voto) OVER (ROWS BETWEEN 1 PRECEDING) FROM esame", "ERR:Expected AND");
    // CASE
    t("SELECT nome, CASE WHEN citta = 'Milano' THEN 'MI' WHEN citta IS NULL THEN '?' ELSE 'altro' END FROM studente ORDER BY matricola",
      "Anna|MI;Bruno|altro;Carla|MI;Dario|altro;Elena|?");
    t("SELECT CASE cfu WHEN 9 THEN 'nove' WHEN 6 THEN 'sei' END FROM corso ORDER BY codice", "NULL;nove;sei");
    t("SELECT SUM(CASE WHEN voto >= 28 THEN 1 ELSE 0 END) FROM esame", "4");
    t("SELECT CASE WHEN 1 THEN 'x' END", "x");
    t("SELECT CASE WHEN 1 'x' END", "ERR:Expected THEN");
    // join impliciti (FROM a, b WHERE ...) e outer join
    t("SELECT s.nome, c.titolo FROM studente s, esame e, corso c WHERE s.matricola = e.matricola AND e.corso = c.codice AND e.voto = 18",
      "Bruno|Basi di dati 1");
    {
        tests++;
        QueryResult* r = db_execute("EXPLAIN SELECT s.nome FROM studente s, esame e WHERE s.matricola = e.matricola AND e.voto = 18");
        char got[4096];
        if (r && r->success) render(r, got, sizeof(got)); else snprintf(got, sizeof(got), "ERR");
        if (!strstr(got, "Hash join") || !strstr(got, "WHERE e.voto = 18 (on e, before the joins)")) {
            failed++;
            printf("FAIL %d: implicit join plan\n   got: %s\n", tests, got);
        }
        db_result_free(r);
    }
    {
        tests++;
        QueryResult* r = db_execute("EXPLAIN ANALYZE SELECT nome FROM studente s WHERE NOT EXISTS "
                                    "(SELECT * FROM esame e WHERE e.matricola = s.matricola)");
        char got[4096];
        if (r && r->success) render(r, got, sizeof(got)); else snprintf(got, sizeof(got), "ERR");
        if (!r || !r->success || r->num_cols != 5 || !strstr(got, "Table scan|s (about 5 rows)|5|5") ||
            !strstr(got, "executed 5 times") || !strstr(got, "lookup by matricola in a hash table built once") ||
            !strstr(got, "Result|1 rows in")) {
            failed++;
            printf("FAIL %d: EXPLAIN ANALYZE\n   got: %s\n", tests, got);
        }
        db_result_free(r);
    }
    // scansioni per intervallo con l'indice ordinato
    t("CREATE TABLE cifre (d INT)", "OK:0");
    t("INSERT INTO cifre VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)", "OK:10");
    t("CREATE TABLE rg (id INT PRIMARY KEY, cod VARCHAR(10), v INT)", "OK:0");
    t("INSERT INTO rg SELECT a.d * 100 + b.d * 10 + c.d, CONCAT('k', a.d, b.d, c.d), MOD(a.d * 100 + b.d * 10 + c.d, 7) "
      "FROM cifre a, cifre b, cifre c", "OK:1000");
    t("CREATE INDEX rg_cod ON rg (cod)", "OK:0");
    t("SELECT COUNT(*), MIN(id), MAX(id) FROM rg WHERE id BETWEEN 100 AND 149", "50|100|149");
    t("SELECT id FROM rg WHERE id > 995", "996;997;998;999");
    t("SELECT id FROM rg WHERE 3 >= id", "0;1;2;3");
    t("SELECT id FROM rg WHERE id >= 10 AND id < 13 AND v <> 4", "10;12");
    t("SELECT id FROM rg WHERE id > 997.5", "998;999");
    t("SELECT cod FROM rg WHERE cod >= 'k997'", "k997;k998;k999");
    t("SELECT cod FROM rg WHERE cod < 'k002'", "k000;k001");
    t("SELECT COUNT(*) FROM rg WHERE id < NULL", "0");
    t("SELECT COUNT(*) FROM rg WHERE id > 'abc'", "0");
    t("SELECT COUNT(*) FROM rg WHERE id > 100", "899");
    t("DELETE FROM rg WHERE id BETWEEN 990 AND 994", "OK:5");
    t("INSERT INTO rg VALUES (5000, 'z', 0), (-7, 'a', 0)", "OK:2");
    t("SELECT id FROM rg WHERE id > 988 AND id < 996 OR id > 4000", "989;995;5000");
    t("SELECT id FROM rg WHERE id > 988 AND id < 996", "989;995");
    t("SELECT id FROM rg WHERE id < 1", "0;-7");
    {
        tests++;
        QueryResult* r = db_execute("EXPLAIN ANALYZE SELECT * FROM rg WHERE id BETWEEN 200 AND 219");
        QueryResult* r2 = db_execute("EXPLAIN SELECT * FROM rg WHERE id > 10");
        char got[4096], got2[4096];
        if (r && r->success) render(r, got, sizeof(got)); else snprintf(got, sizeof(got), "ERR");
        if (r2 && r2->success) render(r2, got2, sizeof(got2)); else snprintf(got2, sizeof(got2), "ERR");
        if (!strstr(got, "Index range scan|rg using rg.id (ordered in-memory index: 20 rows in the range") ||
            !strstr(got, "|20|20") || !strstr(got2, "Table scan") || !strstr(got2, "too many")) {
            failed++;
            printf("FAIL %d: index range scan plan\n   got: %s\n   got: %s\n", tests, got, got2);
        }
        db_result_free(r);
        db_result_free(r2);
    }
    t("SELECT s.nome FROM studente s LEFT JOIN esame e ON e.matricola = s.matricola WHERE e.matricola IS NULL", "Elena");
    t("SELECT s.nome, COUNT(e.voto) FROM studente s LEFT JOIN esame e ON e.matricola = s.matricola AND e.voto = 30 "
      "GROUP BY s.nome ORDER BY s.nome", "Anna|1;Bruno|1;Carla|1;Dario|0;Elena|0");
    t("SELECT c.codice, s.nome FROM esame e RIGHT JOIN corso c ON c.codice = e.corso AND e.voto = 18 LEFT JOIN studente s ON s.matricola = e.matricola ORDER BY c.codice",
      "AN1|NULL;BD1|Bruno;BD2|NULL");
    // FULL OUTER JOIN, NATURAL JOIN, USING
    t("CREATE TABLE iscritto (matricola INT, corso VARCHAR(5))", "OK:0");
    t("INSERT INTO iscritto VALUES (1, 'BD1'), (5, 'BD1'), (9, 'XX')", "OK:3");
    t("SELECT s.nome, i.corso FROM studente s FULL OUTER JOIN iscritto i ON i.matricola = s.matricola ORDER BY s.matricola, i.corso",
      "NULL|XX;Anna|BD1;Bruno|NULL;Carla|NULL;Dario|NULL;Elena|BD1");
    t("SELECT COUNT(*) FROM studente s FULL JOIN iscritto i ON i.matricola = s.matricola WHERE s.matricola IS NULL OR i.corso IS NULL", "4");
    t("SELECT * FROM studente NATURAL JOIN iscritto ORDER BY matricola", "1|Anna|Milano|BD1;5|Elena|NULL|BD1");
    t("SELECT nome, corso FROM studente JOIN iscritto USING (matricola) ORDER BY matricola", "Anna|BD1;Elena|BD1");
    t("SELECT matricola, nome FROM studente LEFT JOIN iscritto USING (matricola) WHERE corso IS NULL ORDER BY matricola", "2|Bruno;3|Carla;4|Dario");
    t("SELECT * FROM studente s FULL JOIN iscritto i USING (matricola) ORDER BY matricola", "1|Anna|Milano|BD1;2|Bruno|Roma|NULL;3|Carla|Milano|NULL;"
      "4|Dario|Torino|NULL;5|Elena|NULL|BD1;9|NULL|NULL|XX");
    t("SELECT * FROM studente JOIN iscritto USING (pippo)", "ERR:USING");
    cols("SELECT matricola, corso FROM studente FULL JOIN iscritto USING (matricola) WHERE matricola = 9", "matricola|iscritto.corso", "INT|VARCHAR");
    cols("SELECT * FROM studente NATURAL JOIN iscritto", "matricola|studente.nome|studente.citta|iscritto.corso", "INT|VARCHAR|VARCHAR|VARCHAR");
    // WITH RECURSIVE e nomi delle colonne di WITH
    t("WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n WHERE x < 5) SELECT SUM(x), COUNT(*) FROM n", "15|5");
    t("WITH RECURSIVE f(n, v) AS (SELECT 1, 1 UNION ALL SELECT n + 1, v * (n + 1) FROM f WHERE n < 20) SELECT v FROM f WHERE n = 20",
      "2432902008176640000");
    t("CREATE TABLE persona (id INT PRIMARY KEY, nome VARCHAR(10), capo INT REFERENCES persona)", "OK:0");
    t("INSERT INTO persona VALUES (1, 'Ada', NULL), (2, 'Bea', 1), (3, 'Ciro', 2), (4, 'Dino', 2), (5, 'Eva', 4)", "OK:5");
    t("WITH RECURSIVE sotto(id, livello) AS (SELECT id, 0 FROM persona WHERE id = 2 "
      "UNION ALL SELECT p.id, s.livello + 1 FROM persona p JOIN sotto s ON p.capo = s.id) SELECT id, livello FROM sotto ORDER BY id",
      "2|0;3|1;4|1;5|2");
    t("WITH RECURSIVE capi AS (SELECT capo FROM persona WHERE id = 5 UNION SELECT p.capo FROM persona p, capi c WHERE p.id = c.capo) "
      "SELECT COUNT(capo) FROM capi", "3");
    t("CREATE TABLE arco (da INT, a INT)", "OK:0");
    t("INSERT INTO arco VALUES (1, 2), (2, 3), (3, 1), (3, 4)", "OK:4");
    t("WITH RECURSIVE r(n) AS (SELECT 1 UNION SELECT a FROM arco JOIN r ON da = n) SELECT n FROM r ORDER BY n", "1;2;3;4");   // ciclo: UNION si ferma
    t("WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT a FROM arco JOIN r ON da = n) SELECT COUNT(*) FROM r", "ERR:does not stop");
    t("WITH RECURSIVE r(n) AS (SELECT n FROM r UNION SELECT 1) SELECT * FROM r", "ERR:anchor");
    t("WITH t(a, b) AS (SELECT 1, 2) SELECT b FROM t", "2");
    t("WITH t(a) AS (SELECT 1, 2) SELECT * FROM t", "ERR:names 1 column");
    // ALTER TABLE ... ADD / DROP / RENAME COLUMN
    t("ALTER TABLE studente ADD COLUMN email VARCHAR(40)", "OK:0");
    t("SELECT matricola, email FROM studente WHERE matricola = 1", "1|NULL");
    t("ALTER TABLE studente ADD anno INT NOT NULL", "ERR:give it a DEFAULT");
    t("ALTER TABLE studente ADD anno INT NOT NULL DEFAULT 2024 CHECK (anno >= 2000)", "OK:0");
    t("SELECT COUNT(*) FROM studente WHERE anno = 2024", "5");
    t("INSERT INTO studente VALUES (9, 'Zeno', 'Pisa', 'z@x.it', 1990)", "ERR:CHECK constraint");
    t("INSERT INTO studente (matricola, nome) VALUES (9, 'Zeno')", "OK:1");
    t("SELECT anno, email FROM studente WHERE matricola = 9", "2024|NULL");
    t("ALTER TABLE studente ADD nome INT", "ERR:already exists");
    t("ALTER TABLE studente DROP COLUMN anno", "ERR:used by constraint");
    t("ALTER TABLE studente DROP COLUMN matricola", "ERR:used by constraint");
    t("ALTER TABLE studente RENAME COLUMN anno TO anno_iscrizione", "OK:0");
    t("SHOW CONSTRAINTS FROM studente", "#2");
    t("INSERT INTO studente (matricola, nome, anno_iscrizione) VALUES (10, 'Ugo', 1500)", "ERR:anno_iscrizione >= 2000");
    t("ALTER TABLE studente DROP COLUMN email", "OK:0");
    t("SELECT * FROM studente WHERE matricola = 1", "1|Anna|Milano|2024");
    t("BEGIN", "OK:0");
    t("ALTER TABLE studente ADD x INT", "ERR:inside a transaction");
    t("ROLLBACK", "OK:0");
    t("ALTER TABLE corso RENAME COLUMN codice TO sigla", "OK:0");
    t("SELECT COUNT(*) FROM esame e JOIN corso c ON c.sigla = e.corso", "9");
    t("DELETE FROM studente WHERE matricola = 9", "OK:1");
    t("SHOW CONSTRAINTS FROM esame", "#0");
    // ALTER TABLE ... ALTER COLUMN: tipo, DEFAULT, NOT NULL
    t("CREATE TABLE misura (id INT PRIMARY KEY, valore VARCHAR(10), nota VARCHAR(20), quando VARCHAR(20))", "OK:0");
    t("INSERT INTO misura VALUES (1, '12', 'breve', '2025-03-01'), (2, '7.0', 'una nota lunga', NULL), (3, NULL, NULL, '2025-04-02')", "OK:3");
    t("ALTER TABLE misura ALTER COLUMN valore TYPE INT", "OK:0");
    t("SELECT id, valore + 1 FROM misura ORDER BY valore", "3|NULL;2|8;1|13");
    t("ALTER TABLE misura ALTER COLUMN nota SET DATA TYPE VARCHAR(10)", "ERR:Value too long for column 'nota' (14 characters, maximum 10)");
    t("SELECT nota FROM misura WHERE id = 2", "una nota lunga");
    t("ALTER TABLE misura MODIFY nota VARCHAR(5)", "ERR:Cannot change the type of 'nota'");
    t("ALTER TABLE misura MODIFY COLUMN quando DATE", "OK:0");
    t("INSERT INTO misura VALUES (4, 1, 'x', '2025-02-30')", "ERR:valid date");
    t("ALTER TABLE misura ALTER valore TYPE FLOAT", "OK:0");
    t("INSERT INTO misura (id, valore) VALUES (4, 2.5)", "OK:1");
    t("ALTER TABLE misura ALTER COLUMN valore TYPE INT", "ERR:expects an integer, got '2.5'");
    t("ALTER TABLE misura ALTER COLUMN valore TYPE COLORE", "ERR:Unknown type");
    t("ALTER TABLE misura ALTER COLUMN nota SET DEFAULT 'nessuna'", "OK:0");
    t("INSERT INTO misura (id) VALUES (5)", "OK:1");
    t("SELECT nota FROM misura WHERE id = 5", "nessuna");
    t("ALTER TABLE misura ALTER COLUMN nota DROP DEFAULT", "OK:0");
    t("ALTER TABLE misura ALTER COLUMN valore SET DEFAULT 'tanto'", "ERR:expects a number");
    t("ALTER TABLE misura ALTER COLUMN nota SET NOT NULL", "ERR:contains NULL in 2 rows");
    t("UPDATE misura SET nota = '-' WHERE nota IS NULL", "OK:2");
    t("ALTER TABLE misura ALTER COLUMN nota SET NOT NULL", "OK:0");
    t("INSERT INTO misura (id) VALUES (6)", "ERR:cannot be NULL");
    t("ALTER TABLE misura ALTER COLUMN nota DROP NOT NULL", "OK:0");
    t("INSERT INTO misura (id) VALUES (6)", "OK:1");
    t("ALTER TABLE misura ALTER COLUMN id DROP NOT NULL", "ERR:primary key");
    t("ALTER TABLE misura ALTER COLUMN id TYPE BIGINT", "OK:0");
    t("SELECT id FROM misura WHERE id = 4", "4");
    t("ALTER TABLE misura ALTER COLUMN nota", "ERR:Expected TYPE, SET or DROP");
    t("DROP TABLE misura", "OK:0");
    // INSERT ... SELECT, UPDATE e DELETE con subquery
    t("CREATE TABLE lodati (matricola INT, voto INT)", "OK:0");
    t("INSERT INTO lodati SELECT matricola, voto FROM esame WHERE voto = 30", "OK:3");
    t("INSERT INTO lodati (matricola) SELECT matricola FROM studente WHERE citta = 'Torino'", "OK:1");
    t("SELECT COUNT(*), COUNT(voto) FROM lodati", "4|3");
    t("INSERT INTO lodati SELECT matricola FROM esame", "ERR:Column count doesn't match");
    t("DELETE FROM lodati WHERE matricola IN (SELECT matricola FROM studente WHERE citta = 'Roma')", "OK:1");
    t("UPDATE studente SET citta = 'Napoli' WHERE matricola NOT IN (SELECT matricola FROM esame)", "OK:1");
    t("SELECT citta FROM studente WHERE matricola = 5", "Napoli");
    // correlata nella DELETE: esami sotto la media del proprio corso
    t("DELETE FROM esame WHERE voto < (SELECT AVG(voto) FROM esame e2 WHERE e2.corso = esame.corso)", "OK:4");
    t("SELECT COUNT(*) FROM esame", "5");

    // ── vincoli di integrità: PRIMARY KEY composta, UNIQUE, CHECK, FOREIGN KEY con azioni ──
    t("CREATE DATABASE vinc", "OK:0");
    t("USE vinc", "OK:0");
    t("CREATE TABLE dipartimento (codice CHAR(3) PRIMARY KEY, nome VARCHAR(30) NOT NULL UNIQUE)", "OK:0");
    t("CREATE TABLE impiegato (matricola INT PRIMARY KEY, nome VARCHAR(30) NOT NULL, stipendio INT CHECK (stipendio > 0), "
      "dip CHAR(3) REFERENCES dipartimento ON DELETE SET NULL ON UPDATE CASCADE, capo INT, "
      "CONSTRAINT fk_capo FOREIGN KEY (capo) REFERENCES impiegato (matricola) ON DELETE CASCADE, "
      "CONSTRAINT stip_max CHECK (stipendio <= 10000))", "OK:0");
    t("ALTER TABLE impiegato ALTER COLUMN dip TYPE VARCHAR(5)", "ERR:part of foreign key");
    t("ALTER TABLE dipartimento ALTER COLUMN codice TYPE VARCHAR(5)", "ERR:referenced by foreign key");
    t("INSERT INTO dipartimento VALUES ('AMM', 'Amministrazione'), ('PRO', 'Produzione')", "OK:2");
    t("INSERT INTO dipartimento VALUES ('XXX', 'Produzione')", "ERR:Duplicate entry 'Produzione' for unique key 'nome'");
    t("INSERT INTO impiegato VALUES (1, 'Rossi', 5000, 'AMM', NULL)", "OK:1");
    t("INSERT INTO impiegato VALUES (2, 'Bianchi', 3000, 'PRO', 1), (3, 'Verdi', 2000, 'PRO', 2)", "OK:2");   // 3 riferisce 2 dello stesso INSERT
    t("INSERT INTO impiegato VALUES (4, 'Neri', 1000, 'ZZZ', NULL)", "ERR:Foreign key");
    t("INSERT INTO impiegato VALUES (4, 'Neri', 1000, NULL, 99)", "ERR:Foreign key 'fk_capo' is violated");
    t("INSERT INTO impiegato VALUES (4, 'Neri', -5, NULL, NULL)", "ERR:CHECK constraint");
    t("INSERT INTO impiegato VALUES (4, 'Neri', 20000, NULL, NULL)", "ERR:'stip_max' is violated");
    t("INSERT INTO impiegato VALUES (4, 'Neri', NULL, NULL, NULL)", "OK:1");                 // CHECK sconosciuto: ammesso
    t("UPDATE impiegato SET stipendio = stipendio * 3 WHERE matricola = 1", "ERR:stip_max");
    t("SELECT stipendio FROM impiegato WHERE matricola = 1", "5000");
    t("UPDATE impiegato SET capo = 77 WHERE matricola = 4", "ERR:fk_capo");
    t("UPDATE dipartimento SET codice = 'ADM' WHERE codice = 'AMM'", "OK:1");               // ON UPDATE CASCADE
    t("SELECT dip FROM impiegato WHERE matricola = 1", "ADM");
    t("DELETE FROM dipartimento WHERE codice = 'PRO'", "OK:1");                            // ON DELETE SET NULL
    t("SELECT matricola FROM impiegato WHERE dip IS NULL ORDER BY 1", "2;3;4");
    t("DELETE FROM impiegato WHERE matricola = 1", "OK:1");                                // a cascata: 2 e poi 3
    t("SELECT matricola FROM impiegato", "4");
    t("UPDATE impiegato SET capo = 4 WHERE matricola = 4", "OK:1");                        // riferisce sé stesso
    t("DELETE FROM impiegato", "OK:1");
    // chiave primaria composta e chiavi esterne verso più tabelle
    t("CREATE TABLE corso (codice VARCHAR(5) PRIMARY KEY, cfu INT NOT NULL CHECK (cfu BETWEEN 1 AND 15))", "OK:0");
    t("CREATE TABLE studente (matricola INT PRIMARY KEY, nome VARCHAR(20))", "OK:0");
    t("CREATE TABLE esame (matricola INT REFERENCES studente ON DELETE CASCADE, corso VARCHAR(5) REFERENCES corso, "
      "voto INT NOT NULL CHECK (voto BETWEEN 18 AND 30), lode BOOL DEFAULT 0, PRIMARY KEY (matricola, corso), "
      "CHECK (lode = 0 OR voto = 30))", "OK:0");
    t("INSERT INTO corso VALUES ('BD1', 9), ('BD2', 6)", "OK:2");
    t("INSERT INTO corso VALUES ('XX', 20)", "ERR:CHECK constraint");
    t("INSERT INTO studente VALUES (1, 'Anna'), (2, 'Bruno')", "OK:2");
    t("INSERT INTO esame VALUES (1, 'BD1', 30, 1)", "OK:1");
    t("INSERT INTO esame VALUES (1, 'BD1', 28, 0)", "ERR:Duplicate entry '1-BD1' for primary key (matricola, corso)");
    t("INSERT INTO esame VALUES (1, 'BD2', 28, 1)", "ERR:CHECK constraint");
    t("INSERT INTO esame VALUES (2, 'BD2', 17, 0)", "ERR:CHECK constraint");
    t("INSERT INTO esame VALUES (3, 'BD2', 25, 0)", "ERR:Foreign key");
    t("INSERT INTO esame VALUES (NULL, 'BD2', 25, 0)", "ERR:cannot be NULL");
    t("INSERT INTO esame (matricola, corso, voto) VALUES (2, 'BD2', 25), (2, 'BD1', 27)", "OK:2");
    t("SELECT lode FROM esame WHERE matricola = 2 AND corso = 'BD2'", "0");
    t("UPDATE esame SET corso = 'BD1' WHERE matricola = 2 AND corso = 'BD2'", "ERR:Duplicate entry '2-BD1'");
    t("UPDATE esame SET voto = voto + 1 WHERE matricola = 2", "OK:2");
    t("DELETE FROM corso WHERE codice = 'BD1'", "ERR:referenced by table 'esame'");        // NO ACTION
    t("UPDATE studente SET matricola = 20 WHERE matricola = 2", "ERR:referenced by table 'esame'");
    t("DELETE FROM studente WHERE matricola = 1", "OK:1");                                 // CASCADE sugli esami
    t("SELECT COUNT(*) FROM esame", "2");
    t("DROP TABLE studente", "ERR:referenced by the foreign key");
    t("TRUNCATE TABLE corso", "ERR:Cannot truncate");
    t("ALTER TABLE esame DROP CONSTRAINT nope", "ERR:not found");
    t("ALTER TABLE studente ADD CONSTRAINT nome_unico UNIQUE (nome)", "OK:0");
    t("INSERT INTO studente VALUES (5, 'Bruno')", "ERR:Duplicate entry 'Bruno' for unique key 'nome'");
    t("ALTER TABLE studente DROP CONSTRAINT nome_unico", "OK:0");
    t("INSERT INTO studente VALUES (5, 'Bruno')", "OK:1");
    t("ALTER TABLE studente ADD CONSTRAINT nome_unico UNIQUE (nome)", "ERR:duplicate values");
    t("ALTER TABLE studente ADD CHECK (matricola > 0)", "OK:0");
    t("ALTER TABLE studente ADD CHECK (matricola > 3)", "ERR:CHECK constraint");
    t("ALTER TABLE studente DROP CONSTRAINT pk_studente", "ERR:uses it");
    t("CREATE TABLE bad1 (a INT REFERENCES nonexist)", "ERR:Referenced table 'nonexist' not found");
    t("CREATE TABLE bad2 (a INT REFERENCES corso (cfu))", "ERR:must be the PRIMARY KEY or a UNIQUE key");
    t("CREATE TABLE bad3 (a INT REFERENCES corso)", "ERR:different types");
    t("CREATE TABLE bad4 (a INT CHECK (b > 0))", "ERR:Unknown column 'b'");
    t("CREATE TABLE bad5 (a INT, PRIMARY KEY (a, a))", "ERR:listed twice");
    t("CREATE TABLE bad6 (a INT NOT NULL REFERENCES studente ON DELETE SET NULL)", "ERR:SET NULL is impossible");
    t("CREATE TABLE bad7 (a INT CHECK (COUNT(*) > 0))", "ERR:Aggregate functions are not allowed");
    t("CREATE TABLE bad8 (a INT CHECK (a IN (SELECT 1)))", "ERR:Subqueries are not allowed");
    t("CREATE TABLE bad9 (a INT PRIMARY KEY, b INT, PRIMARY KEY (b))", "ERR:only one PRIMARY KEY");
    t("SELECT COUNT(*) FROM bad1", "ERR:not found");                                      // nessuna tabella a metà
    // DDL rigenerato, SHOW, statistiche, CHECKPOINT
    t("SHOW CONSTRAINTS FROM esame",
      "pk_esame|PRIMARY KEY|matricola, corso|NULL|NULL|NULL|NULL;fk_esame_1|FOREIGN KEY|matricola|studente (matricola)|CASCADE|NO ACTION|NULL;"
      "fk_esame_2|FOREIGN KEY|corso|corso (codice)|NO ACTION|NO ACTION|NULL;chk_esame_1|CHECK|NULL|NULL|NULL|NULL|voto BETWEEN 18 AND 30;"
      "chk_esame_2|CHECK|NULL|NULL|NULL|NULL|lode = 0 OR voto = 30");
    t("DESCRIBE esame", "matricola|INT|NO|PRI;corso|VARCHAR(5)|NO|PRI;voto|INT|NO|;lode|BOOL|YES|");
    t("DESCRIBE dipartimento", "codice|CHAR(3)|NO|PRI;nome|VARCHAR(30)|NO|UNI");
    t("SHOW CREATE TABLE studente", "studente|CREATE TABLE studente (\n    matricola INT PRIMARY KEY,\n    nome VARCHAR(20),\n"
      "    CONSTRAINT chk_studente_1 CHECK (matricola > 0)\n);");
    t("SHOW INDEXES FROM esame", "#3");
    t("SHOW STATUS", "#16");
    t("CHECKPOINT", "OK:0");
    t("SHOW CREATE TABLE nope", "ERR:not found");
    {
        // lo script generato ricrea gli stessi vincoli in un altro database
        tests++;
        const char* tabs[] = { "corso", "studente", "esame" };
        char* ddl[3] = {0};
        char* fks[3] = {0};
        for (int i = 0; i < 3; i++) { ddl[i] = db_get_create_statement_nofk("vinc", tabs[i]); fks[i] = db_get_foreign_keys("vinc", tabs[i]); }
        db_result_free(db_execute("CREATE DATABASE copia"));
        db_result_free(db_execute("USE copia"));
        bool ok = true;
        for (int i = 2; i >= 0 && ok; i--) {          // in ordine inverso: senza FK non importa
            QueryResult* r = ddl[i] ? db_execute(ddl[i]) : NULL;
            ok = r && r->success;
            if (!ok) printf("FAIL %d: DDL %s: %s\n", tests, ddl[i] ? ddl[i] : "NULL", r ? r->error : "");
            db_result_free(r);
        }
        for (int i = 0; i < 3 && ok; i++) {
            if (!fks[i] || !fks[i][0]) continue;
            QueryResult** rs = NULL;
            int nr = 0;
            rs = db_execute_multi(fks[i], &nr);
            for (int k = 0; k < nr; k++) if (!rs[k] || !rs[k]->success) { ok = false; printf("FAIL %d: %s: %s\n", tests, fks[i], rs[k] ? rs[k]->error : ""); }
            db_result_free_multi(rs, nr);
        }
        for (int i = 0; i < 3; i++) { db_free_string(ddl[i]); db_free_string(fks[i]); }
        if (!ok) failed++;
    }
    t("SELECT COUNT(*) FROM (SELECT 1 FROM esame) AS x", "0");
    t("INSERT INTO esame VALUES (1, 'BD1', 30, 0)", "ERR:Foreign key");
    t("SHOW CONSTRAINTS FROM esame", "#5");
    t("USE vinc", "OK:0");

    // ── UPDATE e DELETE annullati: niente righe duplicate o perse ──
    t("CREATE DATABASE tx", "OK:0");
    t("USE tx", "OK:0");
    t("CREATE TABLE c (id INT PRIMARY KEY, v INT)", "OK:0");
    t("INSERT INTO c VALUES (1, 10), (2, 20), (3, 30)", "OK:3");
    t("BEGIN", "OK:0");
    t("UPDATE c SET v = v + 1", "OK:3");
    t("ROLLBACK", "OK:0");
    t("SELECT id, v FROM c ORDER BY id", "1|10;2|20;3|30");
    t("UPDATE c SET v = 0 WHERE id = 2", "OK:1");                      // nessun duplicato della chiave
    t("BEGIN", "OK:0");
    t("UPDATE c SET v = 100 WHERE id = 1", "OK:1");
    t("SAVEPOINT s1", "OK:0");
    t("UPDATE c SET v = 200 WHERE id = 1", "OK:1");
    t("DELETE FROM c WHERE id = 3", "OK:1");
    t("INSERT INTO c VALUES (4, 40)", "OK:1");
    t("ROLLBACK TO s1", "OK:0");
    t("SELECT id, v FROM c ORDER BY id", "1|100;2|0;3|30");               // anche la DELETE è annullata
    t("UPDATE c SET v = 300 WHERE id = 1", "OK:1");
    t("ROLLBACK TO s1", "OK:0");                                         // il savepoint resta
    t("COMMIT", "OK:0");
    t("SELECT id, v FROM c ORDER BY id", "1|100;2|0;3|30");
    // regressione: dopo 64 transazioni BEGIN falliva per sempre (i posti non venivano riusati)
    for (int i = 0; i < 100; i++) {
        t("BEGIN", "OK:0");
        t("UPDATE c SET v = v + 1 WHERE id = 2", "OK:1");
        t(i % 2 ? "COMMIT" : "ROLLBACK", "OK:0");
    }
    t("SELECT v FROM c WHERE id = 2", "50");
    // DISTINCT + ORDER BY: solo colonne restituite (posizione, alias, stessa colonna o espressione)
    t("SELECT DISTINCT v FROM c ORDER BY v", "30;50;100");
    t("SELECT DISTINCT v AS w FROM c ORDER BY w DESC", "100;50;30");
    t("SELECT DISTINCT v FROM c ORDER BY 1 DESC", "100;50;30");
    t("SELECT DISTINCT c.v, v + 1 FROM c ORDER BY v + 1", "30|31;50|51;100|101");
    t("SELECT DISTINCT v FROM c ORDER BY id", "ERR:must also appear in the SELECT list when DISTINCT is used");
    t("SELECT DISTINCT v FROM c ORDER BY v * 2", "ERR:must also appear in the SELECT list");
    t("SELECT v FROM c ORDER BY id", "100;50;30");
    // SAVEPOINT ripetuto con lo stesso nome: sostituisce, non riempie l'elenco (max 64)
    t("BEGIN", "OK:0");
    for (int i = 0; i < 100; i++) t("SAVEPOINT ciclo", "OK:0");
    t("UPDATE c SET v = 7 WHERE id = 2", "OK:1");
    t("ROLLBACK TO ciclo", "OK:0");
    t("SELECT v FROM c WHERE id = 2", "50");
    t("COMMIT", "OK:0");

    // ── origine delle colonne (griglia modificabile) ──
    t("USE tx", "OK:0");
    t("CREATE TABLE riga (k INT PRIMARY KEY, w VARCHAR(10))", "OK:0");
    t("INSERT INTO riga VALUES (1, 'a')", "OK:1");
    origin("SELECT * FROM c", "c.id|c.v");
    origin("SELECT v AS valore, v + 1, id FROM c x WHERE id > 0 ORDER BY id", "c.v|-|c.id");
    origin("SELECT c.id, r.w FROM c JOIN riga r ON r.k = c.id", "c.id|riga.w");
    origin("SELECT v, COUNT(*) FROM c GROUP BY v", "");
    origin("SELECT DISTINCT v FROM c", "");
    origin("SELECT id FROM c UNION SELECT k FROM riga", "");
    origin("SELECT * FROM (SELECT id FROM c) d", "-");
    origin("WITH q AS (SELECT id FROM c) SELECT id FROM q", "-");

    // ── testi lunghi (oltre i vecchi 511 byte): niente troncamenti, chiavi senza collisioni ──
    {
        char* A = longtext(1500, "-A");          // stessi 1500 caratteri iniziali, fine diversa
        char* B = longtext(1500, "-B");
        char* L = longtext(3000, "");
        t("CREATE DATABASE lunghi", "OK:0");
        t("USE lunghi", "OK:0");
        t("CREATE TABLE doc (id INT PRIMARY KEY, titolo VARCHAR(2000) UNIQUE, corpo TEXT)", "OK:0");
        t("CREATE INDEX i_corpo ON doc (corpo)", "OK:0");
        tl("INSERT INTO doc VALUES (1, '%s', 'x'), (2, '%s', 'y')", A, B, "OK:2");      // UNIQUE: non sono uguali
        tl("INSERT INTO doc VALUES (3, '%s', 'z')", A, NULL, "ERR:Duplicate");          // questo sì
        tl("INSERT INTO doc VALUES (4, 't', '%s')", L, NULL, "OK:1");
        t("SELECT id, LENGTH(titolo), LENGTH(corpo) FROM doc ORDER BY id", "1|1502|1;2|1502|1;4|1|3000");
        tl("SELECT id FROM doc WHERE titolo = '%s'", B, NULL, "2");                     // ricerca con l'indice UNIQUE
        tl("SELECT id FROM doc WHERE corpo = '%s'", L, NULL, "4");                      // e con CREATE INDEX
        t("SELECT COUNT(DISTINCT titolo), COUNT(*) FROM doc", "3|3");
        t("SELECT RIGHT(titolo, 2), COUNT(*) FROM doc GROUP BY titolo ORDER BY 1", "-A|1;-B|1;t|1");
        t("SELECT DISTINCT RIGHT(titolo, 1) FROM doc ORDER BY 1", "A;B;t");
        t("SELECT LENGTH(CONCAT(titolo, corpo, titolo)) FROM doc WHERE id = 4", "3002");
        t("SELECT LENGTH(UPPER(corpo)), RIGHT(UPPER(corpo), 3), LEFT(REVERSE(corpo), 3) FROM doc WHERE id = 4", "3000|HIJ|jih");
        t("SELECT LENGTH(REPLACE(corpo, 'a', 'AA')) FROM doc WHERE id = 4", "3116");
        t("SELECT RIGHT(MAX(titolo), 2), RIGHT(MIN(titolo), 2) FROM doc WHERE id < 3", "-B|-A");
        t("SELECT LENGTH((SELECT corpo FROM doc WHERE id = 4))", "3000");
        t("SELECT a.id, b.id FROM doc a JOIN doc b ON a.titolo = b.titolo ORDER BY a.id", "1|1;2|2;4|4");    // hash join
        t("SELECT COUNT(*) FROM doc WHERE titolo > 'abc'", "3");                          // intervallo: testi lunghi nell'indice
        t("SELECT COUNT(*) FROM doc WHERE titolo < 'abd'", "2");
        t("UPDATE doc SET titolo = CONCAT(titolo, '!') WHERE id = 1", "OK:1");
        t("SELECT RIGHT(titolo, 3) FROM doc WHERE id = 1", "-A!");
        tl("UPDATE doc SET titolo = '%s' WHERE id = 1", B, NULL, "ERR:Duplicate");
        // chiave composta e chiave esterna su testi lunghi
        t("CREATE TABLE coppia (a VARCHAR(2000), b VARCHAR(2000), PRIMARY KEY (a, b))", "OK:0");
        tl("INSERT INTO coppia VALUES ('%s', '%s')", A, B, "OK:1");
        tl("INSERT INTO coppia VALUES ('%s', '%s')", B, A, "OK:1");
        tl("INSERT INTO coppia VALUES ('%s', '%s')", A, B, "ERR:Duplicate");
        t("CREATE TABLE rif (t VARCHAR(2000) REFERENCES doc(titolo))", "OK:0");
        tl("INSERT INTO rif VALUES ('%s')", B, NULL, "OK:1");
        char* C = longtext(1500, "-C");
        tl("INSERT INTO rif VALUES ('%s')", C, NULL, "ERR:Foreign key");                // stesso inizio di A e B
        free(C);
        // trigger che costruisce un testo lungo
        t("CREATE TABLE nota (id INT, s TEXT)", "OK:0");
        t("CREATE TRIGGER lunga BEFORE INSERT ON nota FOR EACH ROW SET NEW.s = CONCAT(NEW.s, NEW.s, NEW.s)", "OK:0");
        char* M = longtext(1000, "-M");
        tl("INSERT INTO nota VALUES (1, '%s')", M, NULL, "OK:1");
        t("SELECT LENGTH(s), RIGHT(s, 2) FROM nota", "3006|-M");
        free(M);
        // riga più grande di una pagina: va nelle pagine di overflow e si rilegge intera
        tl("INSERT INTO nota VALUES (2, CONCAT('%s', '%s'))", L, L, "OK:1");
        t("SELECT COUNT(*) FROM nota", "2");
        t("SELECT LENGTH(s) > 4096, RIGHT(s, 3) = RIGHT(SUBSTR(s, 1, LENGTH(s) / 3), 3) FROM nota WHERE id = 2", "1|1");
        free(A); free(B); free(L);
    }

    // ── trigger (basi di dati attive) ──
    t("CREATE DATABASE att", "OK:0");
    t("USE att", "OK:0");
    t("CREATE TABLE conto (id INT PRIMARY KEY, saldo INT NOT NULL)", "OK:0");
    t("CREATE TABLE movimento (id INT AUTO_INCREMENT PRIMARY KEY, conto INT, importo INT, nota VARCHAR(40))", "OK:0");
    t("CREATE TABLE log_saldo (conto INT, vecchio INT, nuovo INT)", "OK:0");
    t("CREATE TRIGGER no_scoperto BEFORE UPDATE OF saldo ON conto FOR EACH ROW WHEN (NEW.saldo < 0) "
      "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Saldo negativo'", "OK:0");
    t("CREATE TRIGGER storico AFTER UPDATE ON conto FOR EACH ROW BEGIN INSERT INTO log_saldo VALUES (OLD.id, OLD.saldo, NEW.saldo); END", "OK:0");
    t("CREATE TRIGGER applica AFTER INSERT ON movimento FOR EACH ROW UPDATE conto SET saldo = saldo + NEW.importo WHERE id = NEW.conto", "OK:0");
    t("CREATE TRIGGER nota_default BEFORE INSERT ON movimento FOR EACH ROW BEGIN "
      "IF NEW.nota IS NULL THEN SET NEW.nota = 'n/d'; ELSEIF NEW.importo < 0 THEN SET NEW.nota = CONCAT('prelievo: ', NEW.nota); END IF; END", "OK:0");
    t("INSERT INTO conto VALUES (1, 100), (2, 0)", "OK:2");
    t("INSERT INTO movimento (conto, importo) VALUES (1, 50)", "OK:1");
    t("INSERT INTO movimento (conto, importo, nota) VALUES (1, -20, 'bancomat')", "OK:1");
    t("SELECT saldo FROM conto WHERE id = 1", "130");
    t("SELECT nota FROM movimento ORDER BY id", "n/d;prelievo: bancomat");
    t("SELECT * FROM log_saldo", "1|100|150;1|150|130");
    t("INSERT INTO movimento (conto, importo) VALUES (2, -10)", "ERR:Saldo negativo");     // rifiutato da no_scoperto
    t("SELECT COUNT(*) FROM movimento", "2");                                               // annullato per intero
    t("SELECT COUNT(*) FROM log_saldo", "2");
    t("UPDATE conto SET saldo = saldo - 1000 WHERE id = 1", "ERR:Saldo negativo");
    t("SELECT saldo FROM conto WHERE id = 1", "130");
    t("UPDATE conto SET id = 3 WHERE id = 2", "OK:1");                                     // OF saldo: no_scoperto non scatta
    t("SELECT COUNT(*) FROM log_saldo", "3");
    t("CREATE TABLE contatore (n INT)", "OK:0");
    t("INSERT INTO contatore VALUES (0)", "OK:1");
    t("CREATE TRIGGER conta AFTER DELETE ON movimento FOR EACH STATEMENT UPDATE contatore SET n = n + 1", "OK:0");
    t("DELETE FROM movimento", "OK:2");
    t("SELECT n FROM contatore", "1");                                                      // una volta per statement
    t("BEGIN", "OK:0");
    t("INSERT INTO movimento (conto, importo) VALUES (1, 5)", "OK:1");
    t("INSERT INTO movimento (conto, importo) VALUES (3, -100)", "ERR:Saldo negativo");
    t("COMMIT", "OK:0");
    t("SELECT saldo FROM conto ORDER BY id", "135;0");                                      // il primo INSERT resta
    t("SELECT COUNT(*) FROM movimento", "1");
    t("CREATE TABLE r (x INT)", "OK:0");
    t("CREATE TRIGGER ciclo AFTER INSERT ON r FOR EACH ROW INSERT INTO r VALUES (NEW.x + 1)", "OK:0");
    t("INSERT INTO r VALUES (1)", "ERR:nested too deeply");
    t("SELECT COUNT(*) FROM r", "0");
    t("DROP TRIGGER ciclo", "OK:0");
    t("DROP TRIGGER ciclo", "ERR:not found");
    t("CREATE TRIGGER bad AFTER INSERT ON r FOR EACH ROW SET NEW.x = 1", "ERR:only allowed in BEFORE");
    t("CREATE TRIGGER bad BEFORE INSERT ON nope FOR EACH ROW SIGNAL 'x'", "ERR:Table 'nope' not found");
    t("CREATE TRIGGER bad BEFORE INSERT ON r FOR EACH ROW WHEN (NEW.y > 0) SIGNAL 'x'", "ERR:Unknown column 'y'");
    t("CREATE TRIGGER bad BEFORE INSERT ON r FOR EACH ROW DROP TABLE r", "ERR:can contain INSERT");
    t("CREATE TRIGGER storico AFTER INSERT ON r FOR EACH ROW SIGNAL 'x'", "ERR:already exists");
    // i trigger scattano anche per le modifiche a cascata delle chiavi esterne
    t("CREATE TABLE pa (id INT PRIMARY KEY)", "OK:0");
    t("CREATE TABLE fi (id INT PRIMARY KEY, pid INT REFERENCES pa ON DELETE CASCADE ON UPDATE CASCADE)", "OK:0");
    t("CREATE TABLE registro (evento VARCHAR(30))", "OK:0");
    t("INSERT INTO pa VALUES (1), (2)", "OK:2");
    t("INSERT INTO fi VALUES (10, 1), (11, 1), (12, 2)", "OK:3");
    t("CREATE TRIGGER fi_del AFTER DELETE ON fi FOR EACH ROW INSERT INTO registro VALUES (CONCAT('cancellato ', OLD.id))", "OK:0");
    t("CREATE TRIGGER fi_upd BEFORE UPDATE OF pid ON fi FOR EACH ROW WHEN (NEW.pid = 99) SIGNAL 'pid 99 vietato'", "OK:0");
    t("DELETE FROM pa WHERE id = 1", "OK:1");
    t("SELECT evento FROM registro ORDER BY evento", "cancellato 10;cancellato 11");
    t("UPDATE pa SET id = 99 WHERE id = 2", "ERR:pid 99 vietato");
    t("SELECT id FROM pa", "2");
    t("UPDATE pa SET id = 3 WHERE id = 2", "OK:1");
    t("SELECT pid FROM fi", "3");
    t("SHOW TRIGGERS", "#7");
    t("SHOW TRIGGERS FROM conto", "#2");
    {
        // lo split dell'editor tiene insieme BEGIN ... END di un trigger
        tests++;
        const char* script = "CREATE TRIGGER t1 AFTER INSERT ON r FOR EACH ROW BEGIN INSERT INTO r VALUES (1); "
                             "UPDATE r SET x = CASE WHEN x > 0 THEN 1 ELSE 0 END; END; SELECT 1; SELECT 2";
        int nr = 0;
        QueryResult** rs = db_execute_multi(script, &nr);
        if (nr != 3 || !rs[0] || !rs[0]->success) { failed++; printf("FAIL %d: trigger script split (%d)\n", tests, nr); }
        db_result_free_multi(rs, nr);
    }
    t("DROP TRIGGER t1", "OK:0");
    t("USE vinc", "OK:0");
    // i vincoli restano dopo un riavvio
    db_shutdown();
    if (!db_init("/tmp/sv_sql_test")) { printf("FAIL: reopen\n"); return 1; }
    t("USE vinc", "OK:0");
    t("INSERT INTO esame VALUES (2, 'BD1', 30, 0)", "ERR:Duplicate entry '2-BD1'");
    t("INSERT INTO esame VALUES (9, 'BD1', 30, 0)", "ERR:Foreign key");
    t("INSERT INTO studente VALUES (-1, 'X')", "ERR:CHECK constraint");
    t("USE uni", "OK:0");
    t("SELECT nome, anno_iscrizione FROM studente WHERE matricola = 1", "Anna|2024");    // colonna aggiunta, dopo il riavvio
    t("USE vinc", "OK:0");
    t("DELETE FROM studente WHERE matricola = 2", "OK:1");
    t("SELECT COUNT(*) FROM esame", "0");

    db_shutdown();
    crash_recovery_test();
    if (system("rm -rf /tmp/sv_sql_test") != 0) { /* nulla */ }
    printf("%s: %d/%d SQL tests passed\n", failed ? "FAIL" : "OK", tests - failed, tests);
    return failed ? 1 : 0;
}
