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
    else if (r->num_cols == 0) snprintf(got, sizeof(got), "OK:%lld", r->rows_affected);
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
    t("SELECT * FROM altro.p JOIN js ON 1 = 1", "ERR:'js' not found");          // js: database corrente (dove non c'è più)
    t("SELECT * FROM nessuno.p", "ERR:Unknown database");
    t("SELECT * FROM altro.p, shop.js", "ERR:not found");
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
    t("CREATE PROCEDURE cattiva(OUT x INT) SELECT 1", "OK:0");                // parametri OUT e INOUT ammessi
    t("DROP PROCEDURE cattiva", "OK:0");
    t("CREATE PROCEDURE cattiva(x INT) BEGIN DROP TABLE conto; END", "OK:0");     // DROP solo delle tabelle TEMPORARY:
    t("CALL cattiva(1)", "ERR:Only TEMPORARY tables");                            // controllato all'esecuzione
    t("SELECT COUNT(*) FROM conto", "#1");
    t("DROP PROCEDURE cattiva", "OK:0");
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
    // funzioni chieste dai driver MySQL
    t("SELECT DATABASE(), SCHEMA(), VERSION()", "shop|shop|8.0.35-SegVault");
    t("CREATE TABLE li (id INT PRIMARY KEY AUTO_INCREMENT, x INT)", "OK:0");
    t("INSERT INTO li (x) VALUES (1), (2)", "OK:2");
    t("SELECT LAST_INSERT_ID()", "2");
    t("DROP TABLE li", "OK:0");
    // manutenzione delle tabelle (come MySQL / phpMyAdmin)
    t("CREATE TABLE mp (id INT PRIMARY KEY, nome VARCHAR(10) NOT NULL)", "OK:0");
    t("CREATE TABLE mc (id INT PRIMARY KEY, p INT REFERENCES mp(id))", "OK:0");
    t("INSERT INTO mp VALUES (1, 'a'), (2, 'b')", "OK:2");
    t("INSERT INTO mc VALUES (10, 1), (11, 2), (12, NULL)", "OK:3");
    t("CHECK TABLE mp, mc", "mp|check|status|OK (2 rows);mc|check|status|OK (3 rows)");
    t("ANALYZE TABLE mc", "mc|analyze|status|OK (3 rows)");
    t("REPAIR TABLE mp", "mp|repair|status|OK (2 rows)");
    t("OPTIMIZE TABLE mp", "#1");
    t("CHECK TABLE nessuna", "nessuna|check|Error|Table doesn't exist");
    t("CHECK TABLE mc EXTENDED", "mc|check|status|OK (3 rows)");
    t("DROP TABLE mc", "OK:0");
    t("DROP TABLE mp", "OK:0");
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
    cols("SELECT id, a + b AS total, a * 1.5, s FROM n", "id|total|a * 1.5|s", "INT|INT|DECIMAL|VARCHAR");
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
      "north|35.50;south|9.0;east|NULL");
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
    // sintassi di MySQL: START TRANSACTION, WORK, ROLLBACK TO SAVEPOINT
    t("START TRANSACTION READ WRITE", "OK:0");
    t("INSERT INTO u VALUES (101, 1, 1, 's')", "OK:1");
    t("SAVEPOINT a", "OK:0");
    t("INSERT INTO u VALUES (102, 1, 1, 's')", "OK:1");
    t("ROLLBACK WORK TO SAVEPOINT a", "OK:0");
    t("COMMIT WORK", "OK:0");
    t("SELECT COUNT(*) FROM u WHERE id IN (101, 102)", "1");
    t("DELETE FROM u WHERE id = 101", "OK:1");

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

    // ── P1.9: decimali esatti e BIGINT UNSIGNED (come MySQL 8) ──
    t("SELECT 0.10 + 0.20 = 0.30, 0.1 + 0.2, 1.5 * 2, 2.50 - 1, 3.0 / 2, 1.0 * 1.0, ROUND(150.000, 2), ROUND(150, 2)",
      "1|0.3|3.0|1.50|1.50000|1.00|150.00|150");
    t("CREATE TABLE dz (id INT PRIMARY KEY, a DECIMAL(20,2), u BIGINT UNSIGNED)", "OK:0");
    t("INSERT INTO dz VALUES (1, 12345678901234567.89, 1), (2, 0.01, 18446744073709551615)", "OK:2");
    t("SELECT SUM(a), AVG(a), MIN(a), MAX(a), SUM(a) * 2 FROM dz",
      "12345678901234567.90|6172839450617283.950000|0.01|12345678901234567.89|24691357802469135.80");
    cols("SELECT SUM(a) FROM dz", "SUM(a)", "DECIMAL");
    t("SELECT a FROM dz ORDER BY a DESC", "12345678901234567.89;0.01");
    t("SELECT u, u + 0 FROM dz WHERE u > 9223372036854775807", "18446744073709551615|18446744073709551615");
    t("INSERT INTO dz VALUES (3, 1, -1)", "ERR:out of range");
    t("INSERT INTO dz VALUES (4, 1, 18446744073709551616)", "ERR:out of range");
    t("SELECT a, COUNT(*) FROM (SELECT 1.50 AS a UNION ALL SELECT 1.5 UNION ALL SELECT 1.500) x GROUP BY a", "1.50|3");
    t("SELECT TRUNCATE(-1.999, 2), ROUND(-2.5), ROUND(2.675, 2), ROUND(14.5, -1), 12345678901234567.89 % 10", "-1.99|-3|2.68|10|7.89");
    t("DROP TABLE dz", "OK:0");

    // ── P1.13: statistiche, indice o scansione, ordine dei join, stime, EXPLAIN FORMAT=JSON ──
    {
        t("CREATE TABLE sk (id INT PRIMARY KEY, k INT, v INT)", "OK:0");
        t("CREATE INDEX ik ON sk (k)", "OK:0");
        t("INSERT INTO sk SELECT x, CASE WHEN x <= 9000 THEN 1 ELSE x END, x % 7 FROM GENERATE_SERIES(1, 10000) AS g(x)", "OK:10000");
        t("ANALYZE TABLE sk", "sk|analyze|status|OK (10000 rows)");
        // tabella storta: k = 1 in 9000 righe su 10000 → scansione; k = 9500 (una riga) → indice
        t("EXPLAIN SELECT id FROM sk WHERE k = 1",
          "1|Table scan|sk (about 10000 rows; the index on k finds 9000, too many: reading every page in order is cheaper)|10000;"
          "2|Filter|WHERE k = 1 (on sk, before the joins)|9000;3|Projection|id|9000");
        t("EXPLAIN SELECT id FROM sk WHERE k = 9500", "1|Index lookup|sk using sk.k (in-memory index)|1;2|Filter|WHERE k = 9500 (on sk, before the joins)|1;3|Projection|id|1");
        t("SELECT COUNT(*), SUM(id) FROM sk WHERE k = 1", "9000|40504500");
        // stima dall'istogramma: v = 3 AND id > 5000 → 714 (le righe vere sono 715)
        t("EXPLAIN SELECT id FROM sk WHERE v = 3 AND id > 5000",
          "1|Table scan|sk (about 10000 rows)|10000;2|Filter|WHERE v = 3 AND id > 5000 (on sk, before the joins)|714;3|Projection|id|714");
        // tre tabelle: lo stesso ordine (la più selettiva prima) comunque scritto il join; stime entro 2x
        t("CREATE TABLE jb (id INT PRIMARY KEY, m INT, a INT)", "OK:0");
        t("CREATE TABLE jm (id INT PRIMARY KEY, s INT, b INT)", "OK:0");
        t("CREATE TABLE js (id INT PRIMARY KEY, c INT)", "OK:0");
        t("INSERT INTO jb SELECT x, x % 1000, x % 10 FROM GENERATE_SERIES(1, 20000) AS g(x)", "OK:20000");
        t("INSERT INTO jm SELECT x, x % 20, x FROM GENERATE_SERIES(1, 1000) AS g(x)", "OK:1000");
        t("INSERT INTO js SELECT x, x FROM GENERATE_SERIES(1, 20) AS g(x)", "OK:20");
        t("ANALYZE TABLE jb, jm, js", "jb|analyze|status|OK (20000 rows);jm|analyze|status|OK (1000 rows);js|analyze|status|OK (20 rows)");
        const char* plan3 = "1|Table scan|js (about 20 rows)|20;2|Filter|WHERE js.c < 3 (on js, before the joins)|2;"
                            "3|Table scan|jm (about 1000 rows)|1000;4|Hash join|INNER JOIN jm ON jm.s = js.id|125;"
                            "5|Table scan|jb (about 20000 rows)|20000;6|Hash join|INNER JOIN jb ON jb.m = jm.id|2500;"
                            "7|Aggregate|all rows as one group|1;8|Projection|COUNT(*)|1";
        t("EXPLAIN SELECT COUNT(*) FROM jb JOIN jm ON jb.m = jm.id JOIN js ON jm.s = js.id WHERE js.c < 3", plan3);
        t("EXPLAIN SELECT COUNT(*) FROM js JOIN jm ON jm.s = js.id JOIN jb ON jb.m = jm.id WHERE js.c < 3", plan3);
        t("EXPLAIN SELECT COUNT(*) FROM jm, jb, js WHERE jb.m = jm.id AND jm.s = js.id AND js.c < 3", plan3);
        t("SELECT COUNT(*) FROM jb JOIN jm ON jb.m = jm.id JOIN js ON jm.s = js.id WHERE js.c < 3", "2000");
        {   // EXPLAIN ANALYZE: ogni stima entro 2x delle righe vere (l'ultima riga ha il tempo: non si confronta)
            tests++;
            QueryResult* r = db_execute("EXPLAIN ANALYZE SELECT COUNT(*) FROM jb JOIN jm ON jb.m = jm.id JOIN js ON jm.s = js.id WHERE js.c < 3");
            char got[4096] = "";
            if (r && r->success) render(r, got, sizeof(got));
            const char* want = "1|Table scan|js (about 20 rows)|20|20;2|Filter|WHERE js.c < 3 (on js, before the joins)|2|2;"
                               "3|Table scan|jm (about 1000 rows)|1000|1000;4|Hash join|INNER JOIN jm ON jm.s = js.id|125|100;"
                               "5|Table scan|jb (about 20000 rows)|20000|20000;6|Hash join|INNER JOIN jb ON jb.m = jm.id|2500|2000;"
                               "7|Aggregate|all rows as one group|1|1;8|Projection|COUNT(*)|1|1;9|Result|1 rows in ";
            if (strncmp(got, want, strlen(want))) { failed++; printf("FAIL %d: EXPLAIN ANALYZE estimates\n   got: %s\n", tests, got); }
            db_result_free(r);
        }
        // SELECT * dopo il riordino: colonne nell'ordine scritto, stessi nomi
        cols("SELECT * FROM jb JOIN jm ON jb.m = jm.id JOIN js ON jm.s = js.id WHERE js.c < 2 LIMIT 1",
             "jb.id|jb.m|jb.a|jm.id|jm.s|jm.b|js.id|js.c", "INT|INT|INT|INT|INT|INT|INT|INT");
        t("SELECT * FROM jb JOIN jm ON jb.m = jm.id JOIN js ON jm.s = js.id WHERE js.c < 2 ORDER BY jb.id LIMIT 2", "1|1|1|1|1|1|1|1;21|21|1|21|1|21|1|1");
        // num_rows esatto dopo un ROLLBACK (prima: le righe annullate restavano contate)
        t("BEGIN", "OK:0");
        t("INSERT INTO js SELECT x, x FROM GENERATE_SERIES(100, 599) AS g(x)", "OK:500");
        t("DELETE FROM js WHERE id <= 10", "OK:10");
        t("ROLLBACK", "OK:0");
        t("EXPLAIN SELECT * FROM js", "1|Table scan|js (about 20 rows)|20;2|Projection|id, c|20");
        t("EXPLAIN FORMAT=JSON SELECT id FROM sk WHERE k = 9500",
          "{\"query_plan\": [{\"step\": 1, \"depth\": 0, \"operation\": \"Index lookup\", \"details\": \"sk using sk.k (in-memory index)\", \"estimated_rows\": 1}, "
          "{\"step\": 2, \"depth\": 0, \"operation\": \"Filter\", \"details\": \"WHERE k = 9500 (on sk, before the joins)\", \"estimated_rows\": 1}, "
          "{\"step\": 3, \"depth\": 0, \"operation\": \"Projection\", \"details\": \"id\", \"estimated_rows\": 1}]}");
        t("EXPLAIN FORMAT=XML SELECT 1", "ERR:Expected FORMAT = JSON");
        t("DROP TABLE sk", "OK:0"); t("DROP TABLE jb", "OK:0"); t("DROP TABLE jm", "OK:0"); t("DROP TABLE js", "OK:0");
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
    t("SELECT 1 + 2 AS three, UPPER('x'), ROUND(2.5, 400), ROUND(123, -2)", "3|X|2.500000000000000000000000000000|100");
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
    t("SELECT voto, SUM(voto) OVER (ORDER BY voto RANGE 1 PRECEDING) FROM esame ORDER BY voto", "18|18;22|22;24|24;25|49;27|27;28|55;30|90;30|90;30|90");
    t("SELECT voto, COUNT(*) OVER (ORDER BY voto DESC RANGE BETWEEN 2 PRECEDING AND 2 FOLLOWING) FROM esame ORDER BY voto LIMIT 3", "18|1;22|2;24|3");
    t("SELECT SUM(voto) OVER (ORDER BY voto, corso RANGE 1 PRECEDING) FROM esame", "ERR:exactly one ORDER BY");
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
    t("SELECT id FROM rg WHERE id < 1 ORDER BY id", "-7;0");
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
    t("CREATE TRIGGER bad BEFORE INSERT ON r FOR EACH ROW DROP TABLE r", "ERR:cannot create or drop tables");
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
    // ── linguaggio delle stored routine: variabili, cicli, cursori, handler, funzioni ──
    t("CREATE DATABASE rt", "OK:0");
    t("USE rt", "OK:0");
    t("CREATE TABLE prod (id INT PRIMARY KEY, nome VARCHAR(20), prezzo DECIMAL(6,2), qta INT)", "OK:0");
    t("INSERT INTO prod VALUES (1, 'penna', 1.50, 10), (2, 'quaderno', 3.20, 0), (3, 'zaino', 25.00, 2)", "OK:3");
    // funzioni: DECLARE, SET, WHILE, RETURN, tipi del risultato
    t("CREATE FUNCTION fatt(n INT) RETURNS BIGINT DETERMINISTIC BEGIN DECLARE r BIGINT DEFAULT 1; DECLARE i INT DEFAULT 2; "
      "WHILE i <= n DO SET r = r * i; SET i = i + 1; END WHILE; RETURN r; END", "OK:0");
    t("SELECT fatt(0), fatt(5), fatt(20)", "1|120|2432902008176640000");
    t("CREATE FUNCTION iva(p DECIMAL(6,2)) RETURNS DECIMAL(6,2) RETURN p * 1.22", "OK:0");
    t("SELECT nome, iva(prezzo) FROM prod WHERE iva(prezzo) > 1 ORDER BY id", "penna|1.83;quaderno|3.90;zaino|30.50");
    t("SELECT nome FROM prod WHERE iva(prezzo) > 2 ORDER BY id", "quaderno;zaino");
    t("SELECT fatt(3) + iva(10)", "18.20");
    t("CREATE FUNCTION giudizio(v INT) RETURNS VARCHAR(20) BEGIN "
      "CASE WHEN v >= 28 THEN RETURN 'ottimo'; WHEN v >= 24 THEN RETURN 'buono'; ELSE RETURN 'sufficiente'; END CASE; END", "OK:0");
    t("SELECT giudizio(30), giudizio(25), giudizio(18)", "ottimo|buono|sufficiente");
    t("CREATE FUNCTION segno(x INT) RETURNS INT BEGIN IF x > 0 THEN RETURN 1; ELSEIF x < 0 THEN RETURN -1; END IF; RETURN 0; END", "OK:0");
    t("SELECT segno(-4), segno(0), segno(9)", "-1|0|1");
    t("CREATE FUNCTION nullo() RETURNS INT RETURN NULL", "OK:0");
    t("SELECT nullo() IS NULL", "1");
    // REPEAT, LOOP con etichette, LEAVE e ITERATE
    t("CREATE FUNCTION dispari(n INT) RETURNS INT BEGIN DECLARE i, s INT DEFAULT 0; "
      "ciclo: LOOP SET i = i + 1; IF i > n THEN LEAVE ciclo; END IF; IF i % 2 = 0 THEN ITERATE ciclo; END IF; SET s = s + i; END LOOP ciclo; "
      "RETURN s; END", "OK:0");
    t("SELECT dispari(10)", "25");
    t("CREATE FUNCTION cifre(n BIGINT) RETURNS INT BEGIN DECLARE c INT DEFAULT 0; REPEAT SET c = c + 1; SET n = n DIV 10; UNTIL n = 0 END REPEAT; RETURN c; END", "OK:0");
    t("SELECT cifre(7), cifre(12345)", "1|5");
    // errori delle funzioni
    t("CREATE FUNCTION senza() RETURNS INT BEGIN DECLARE x INT; END", "ERR:No RETURN found");
    t("CREATE FUNCTION tabella() RETURNS INT BEGIN SELECT * FROM prod; RETURN 1; END", "OK:0");
    t("SELECT tabella()", "ERR:Not allowed to return a result set");
    t("SELECT fatt(1, 2)", "ERR:expects 1 argument");
    t("CREATE FUNCTION giro(n INT) RETURNS INT RETURN giro(n + 1)", "OK:0");
    t("SELECT giro(1)", "ERR:nested too deeply");
    t("CREATE FUNCTION indefinita() RETURNS INT BEGIN SET y = 1; RETURN 1; END", "OK:0");
    t("SELECT indefinita()", "ERR:Undeclared variable: y");
    t("DROP FUNCTION tabella", "OK:0");
    t("DROP FUNCTION giro", "OK:0");
    t("DROP FUNCTION indefinita", "OK:0");
    t("DROP FUNCTION nessuna", "ERR:Function 'nessuna' not found");
    t("DROP FUNCTION IF EXISTS nessuna", "OK:0");
    // procedure: SELECT ... INTO, OUT / INOUT, @variabili, CALL annidate con variabili locali
    t("CREATE PROCEDURE scorte(IN pid INT, OUT n INT, INOUT tot DECIMAL(8,2)) BEGIN "
      "SELECT qta INTO n FROM prod WHERE id = pid; SET tot = tot + (SELECT prezzo * qta FROM prod WHERE id = pid); END", "OK:0");
    t("SET @tot = 100", "OK:0");
    t("CALL scorte(1, @n, @tot)", "OK:0");
    t("SELECT @n, @tot", "10|115.00");
    t("CALL scorte(3, @n, 5)", "ERR:OUT or INOUT argument 3");
    t("CREATE PROCEDURE totale(OUT somma DECIMAL(8,2)) BEGIN DECLARE n INT; DECLARE t DECIMAL(8,2) DEFAULT 0; "
      "CALL scorte(1, n, t); CALL scorte(3, n, t); SET somma = t; END", "OK:0");
    t("CALL totale(@s)", "OK:0");
    t("SELECT @s", "65.00");
    // cursore con handler NOT FOUND (lo schema classico)
    t("CREATE PROCEDURE elenco(OUT testo VARCHAR(200)) BEGIN DECLARE fine INT DEFAULT 0; DECLARE v VARCHAR(20); "
      "DECLARE c CURSOR FOR SELECT nome FROM prod WHERE qta > 0 ORDER BY nome; DECLARE CONTINUE HANDLER FOR NOT FOUND SET fine = 1; "
      "SET testo = ''; OPEN c; leggi: LOOP FETCH c INTO v; IF fine THEN LEAVE leggi; END IF; SET testo = CONCAT(testo, v, ';'); END LOOP; CLOSE c; END", "OK:0");
    t("CALL elenco(@e)", "OK:0");
    t("SELECT @e", "penna;zaino;");
    t("CREATE PROCEDURE fetch_oltre() BEGIN DECLARE v INT; DECLARE c CURSOR FOR SELECT id FROM prod WHERE id = 1; OPEN c; FETCH c INTO v; FETCH c INTO v; END", "OK:0");
    t("CALL fetch_oltre()", "ERR:No data");
    // handler: CONTINUE per un errore, EXIT, SELECT ... INTO senza righe (NOT FOUND)
    t("CREATE PROCEDURE prova_dup(OUT esito VARCHAR(30)) BEGIN DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET esito = 'errore gestito'; "
      "SET esito = 'ok'; INSERT INTO prod VALUES (1, 'doppio', 1, 1); END", "OK:0");
    t("CALL prova_dup(@x)", "OK:0");
    t("SELECT @x", "errore gestito");
    t("CREATE PROCEDURE prova_exit(OUT esito INT) BEGIN SET esito = 1; BEGIN DECLARE EXIT HANDLER FOR SQLSTATE '23000' SET esito = esito + 10; "
      "INSERT INTO prod VALUES (1, 'doppio', 1, 1); SET esito = 1000; END; SET esito = esito + 100; END", "OK:0");
    t("CALL prova_exit(@x)", "OK:0");
    t("SELECT @x", "111");
    t("CREATE PROCEDURE cerca(IN pid INT, OUT q INT) BEGIN DECLARE EXIT HANDLER FOR NOT FOUND SET q = -1; SELECT qta INTO q FROM prod WHERE id = pid; END", "OK:0");
    t("CALL cerca(3, @q)", "OK:0");
    t("SELECT @q", "2");
    t("CALL cerca(99, @q)", "OK:0");
    t("SELECT @q", "-1");
    t("CREATE PROCEDURE rilancia() BEGIN DECLARE EXIT HANDLER FOR SQLEXCEPTION RESIGNAL; SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'fermato'; END", "OK:0");
    t("CALL rilancia()", "ERR:fermato");
    // tabelle TEMPORARY: create nella procedura, sparite alla fine della CALL
    t("CREATE PROCEDURE riassunto() BEGIN CREATE TEMPORARY TABLE tmp_p (id INT PRIMARY KEY); "
      "INSERT INTO tmp_p SELECT id FROM prod WHERE qta > 0; SELECT COUNT(*) AS n FROM tmp_p; END", "OK:0");
    t("CALL riassunto()", "2");
    t("CALL riassunto()", "2");                                              // ricreata senza errori
    t("SELECT * FROM tmp_p", "ERR:");
    // variabili di sessione e operatori
    t("SET @a = 2, @b := @a * 10", "OK:0");
    t("SELECT @a, @b, @mai_assegnata", "2|20|NULL");
    t("SELECT @c := 7, @c + 1", "7|8");
    t("SELECT id INTO @primo FROM prod ORDER BY id LIMIT 1", "OK:1");
    t("SELECT @primo", "1");
    t("SELECT nome FROM prod WHERE id > @primo ORDER BY id", "quaderno;zaino");
    t("SELECT @@version LIKE '8.0%', @@autocommit", "1|1");
    t("SELECT 12 & 10, 12 | 3, 6 ^ 3, ~0, 1 << 10, 1024 >> 3", "8|15|5|18446744073709551615|1024|128");
    t("SELECT NULL <=> NULL, 1 <=> NULL, 2 <=> 2, 1 XOR 1, 1 XOR 0", "1|0|1|0|1");
    t("SELECT 'ab' || 'cd', NULL || 'x'", "abcd|NULL");
    t("SELECT 1 && 0, !0, 5 IS TRUE, 0 IS NOT TRUE, NULL IS UNKNOWN", "0|1|1|1|1");
    t("SELECT 1 IS DISTINCT FROM NULL, NULL IS NOT DISTINCT FROM NULL", "1|1");
    t("SELECT id FROM prod WHERE qta & 2 ORDER BY id", "1;3");
    // trigger con variabili locali
    t("CREATE TABLE storico (id INT, nota VARCHAR(40))", "OK:0");
    t("CREATE TRIGGER prod_ins AFTER INSERT ON prod FOR EACH ROW BEGIN DECLARE n INT; "
      "SELECT COUNT(*) INTO n FROM prod; INSERT INTO storico VALUES (NEW.id, CONCAT('prodotti: ', n)); END", "OK:0");
    t("INSERT INTO prod VALUES (4, 'gomma', 0.50, 7)", "OK:1");
    t("SELECT nota FROM storico", "prodotti: 4");
    t("DROP TRIGGER prod_ins", "OK:0");
    // ── aggregati, finestre, sintassi standard e di MySQL / PostgreSQL ──
    t("CREATE TABLE dip (id INT PRIMARY KEY, nome VARCHAR(30), rep VARCHAR(20), stip DECIMAL(8,2))", "OK:0");
    t("INSERT INTO dip VALUES (1,'Ada','IT',3000),(2,'Bruno','IT',2500),(3,'Carla','HR',2800),(4,'Dario','HR',NULL),(5,'Elena','IT',2600)", "OK:5");
    t("SELECT rep, STRING_AGG(nome, ', ' ORDER BY nome DESC) FROM dip GROUP BY rep ORDER BY rep", "HR|Dario, Carla;IT|Elena, Bruno, Ada");
    t("SELECT LISTAGG(nome, '/') WITHIN GROUP (ORDER BY nome) FROM dip WHERE rep = 'HR'", "Carla/Dario");
    t("SELECT ROUND(STDDEV(stip), 2), ROUND(STDDEV_SAMP(stip), 2), VARIANCE(stip), VAR_SAMP(stip) FROM dip WHERE rep = 'IT'", "216.02|264.58|46666.6666666667|70000");
    t("SELECT BIT_AND(id), BIT_OR(id), BIT_XOR(id), BOOL_AND(id > 0), BOOL_OR(id > 4), EVERY(id > 1) FROM dip", "0|7|1|1|1|0");
    t("SELECT JSON_ARRAYAGG(nome), JSON_OBJECTAGG(id, rep) FROM dip WHERE id < 3", "[\"Ada\", \"Bruno\"]|{\"1\": \"IT\", \"2\": \"IT\"}");
    t("SELECT COUNT(*) FILTER (WHERE stip > 2500), SUM(stip) FILTER (WHERE rep = 'IT') FROM dip", "3|8100.00");
    t("SELECT nome, PERCENT_RANK() OVER (ORDER BY id), CUME_DIST() OVER (ORDER BY id), NTH_VALUE(nome, 2) OVER (ORDER BY id) FROM dip WHERE id <= 3 ORDER BY id",
      "Ada|0|0.333333333333333|NULL;Bruno|0.5|0.666666666666667|Bruno;Carla|1|1|Bruno");
    t("SELECT nome, RANK() OVER w FROM dip WHERE stip IS NOT NULL WINDOW w AS (PARTITION BY rep ORDER BY stip DESC) ORDER BY rep, stip DESC", "Carla|1;Ada|1;Elena|2;Bruno|3");
    t("SELECT nome, RANK() OVER x FROM dip", "ERR:Unknown window name");
    t("SELECT id FROM dip ORDER BY stip NULLS LAST, id", "2;5;3;1;4");
    t("SELECT id FROM dip ORDER BY stip DESC NULLS FIRST", "4;1;3;5;2");
    t("SELECT id FROM dip ORDER BY id OFFSET 1 ROWS FETCH NEXT 2 ROWS ONLY", "2;3");
    t("SELECT id FROM dip ORDER BY id FETCH FIRST 1 ROW ONLY", "1");
    t("SELECT nome FROM dip WHERE (rep, stip) IN (('IT', 3000), ('HR', 2800)) ORDER BY id", "Ada;Carla");
    t("SELECT nome FROM dip WHERE (id, rep) = (2, 'IT')", "Bruno");
    t("SELECT nome FROM dip WHERE (id, rep) <> (2, 'IT') AND id < 3", "Ada");
    t("SELECT nome FROM dip WHERE (id, rep) < (2, 'IT')", "ERR:Row values can be compared");
    t("SELECT y FROM (VALUES (1, 'a'), (2, 'b')) AS v(x, y) ORDER BY x DESC", "b;a");
    t("VALUES (1, 2), (3, 4)", "1|2;3|4");
    t("SELECT t.k FROM (SELECT id, nome FROM dip) AS t(k, n) WHERE t.k = 1", "1");
    t("TABLE dip ORDER BY id DESC LIMIT 1", "5|Elena|IT|2600.00");
    t("SELECT nome FROM dip USE INDEX (PRIMARY) WHERE id = 1", "Ada");
    t("SELECT a.nome FROM dip AS a FORCE INDEX (PRIMARY) JOIN dip b IGNORE INDEX (PRIMARY) ON b.id = a.id WHERE a.id = 2", "Bruno");
    t("CREATE TABLE dip_it AS SELECT id, nome, stip FROM dip WHERE rep = 'IT'", "OK:3");
    t("SELECT nome, stip FROM dip_it ORDER BY id", "Ada|3000.00;Bruno|2500.00;Elena|2600.00");
    t("CREATE TABLE dip2 LIKE dip", "OK:0");
    t("INSERT INTO dip2 VALUES (1, 'x', 'y', 1.5)", "OK:1");
    t("SELECT stip FROM dip2", "1.50");
    t("INSERT INTO dip2 VALUES (1, 'z', 'y', 1)", "ERR:Duplicate");                 // la chiave primaria è stata copiata
    t("ALTER TABLE dip2 ADD COLUMN nota TEXT AFTER nome", "OK:0");
    t("RENAME TABLE dip2 TO dip3", "OK:0");
    t("DROP TABLE IF EXISTS dip3, dip_it, nessuna", "OK:0");
    t("SELECT COUNT(*) FROM dip3", "ERR:");
    t("LOCK TABLES dip READ", "OK:0");
    t("UNLOCK TABLES", "OK:0");
    t("SHOW VARIABLES LIKE 'version_c%'", "version_comment|SegVault;version_compile_os|SegVault");
    // funzioni JSON, di testo e di date
    t("SELECT JSON_SET('{\"a\":1}', '$.b', 2, '$.a', 'x')", "{\"a\":\"x\", \"b\": 2}");
    t("SELECT JSON_INSERT('{\"a\":1}', '$.a', 5, '$.c', JSON_ARRAY(1, 2)), JSON_REPLACE('{\"a\":1}', '$.a', 9, '$.z', 1)", "{\"a\":1, \"c\": [1, 2]}|{\"a\":9}");
    t("SELECT JSON_REMOVE('{\"a\":1,\"b\":[1,2,3]}', '$.b[1]', '$.a'), JSON_ARRAY_APPEND('[1,2]', '$', 3)", "{\"b\":[1,3]}|[1,2, 3]");
    t("SELECT JSON_KEYS('{\"a\":1,\"b\":2}'), JSON_CONTAINS('[1,2,3]', '2'), JSON_CONTAINS('{\"a\":1,\"b\":2}', '{\"a\":1}'), JSON_CONTAINS('[1]', '5')", "[\"a\", \"b\"]|1|1|0");
    t("SELECT JSON_CONTAINS_PATH('{\"a\":1}', 'one', '$.a', '$.z'), JSON_CONTAINS_PATH('{\"a\":1}', 'all', '$.a', '$.z')", "1|0");
    t("SELECT JSON_SEARCH('{\"x\":[\"ab\",\"cd\"]}', 'one', 'c%'), JSON_SEARCH('{\"x\":\"q\"}', 'one', 'z')", "\"$.x[1]\"|NULL");
    t("SELECT JSON_DEPTH('[1,[2]]'), JSON_MERGE_PATCH('{\"a\":1,\"b\":2}', '{\"b\":null,\"c\":3}'), JSON_QUOTE('a\"b')", "3|{\"a\": 1, \"c\": 3}|\"a\\\"b\"");
    t("SELECT INITCAP('ciao mondo'), SPLIT_PART('a,b,c', ',', 2), SPLIT_PART('a,b,c', ',', -1), TRANSLATE('abcd', 'ab', 'x'), CHR(65)", "Ciao Mondo|b|c|xcd|A");
    t("SELECT CONV('ff', 16, 10), CONV(255, 10, 2), SOUNDEX('Robert'), SOUNDEX('Rupert')", "255|11111111|R163|R163");
    t("SELECT OVERLAY('abcdef' PLACING 'XY' FROM 2 FOR 3), STARTS_WITH('ciao', 'ci'), REGEXP_COUNT('a1b22c333', '[0-9]+')", "aXYef|1|3");
    {   // P3.9: 100 000 parentesi annidate in una REGEXP facevano traboccare lo stack (il server cadeva)
        static char deep[200100];
        int k = snprintf(deep, sizeof(deep), "SELECT 'a' REGEXP '");
        for (int i = 0; i < 100000; i++) deep[k++] = '(';
        deep[k++] = 'a';
        for (int i = 0; i < 100000; i++) deep[k++] = ')';
        snprintf(deep + k, sizeof(deep) - (size_t)k, "'");
        t(deep, "ERR:nested parentheses");
        t("SELECT 'abc' REGEXP '((((a))))b', REGEXP_REPLACE('aXbXc', '(X)', '-')", "1|a-b-c");
        // JSON_SEARCH: il LIKE era ricorsivo per carattere (stack) ed esponenziale con più %
        int k2 = snprintf(deep, sizeof(deep), "SELECT JSON_SEARCH('[\"");
        for (int i = 0; i < 90000; i++) deep[k2++] = 'a';
        k2 += snprintf(deep + k2, sizeof(deep) - (size_t)k2, "\"]', 'one', '");
        for (int i = 0; i < 90000; i++) deep[k2++] = '_';
        snprintf(deep + k2, sizeof(deep) - (size_t)k2, "')");
        t(deep, "\"$[0]\"");
        t("SELECT JSON_SEARCH('[\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]', 'one', '%a%a%a%a%a%a%a%a%a%a%a%a%a%b'), "
          "JSON_SEARCH('[\"x_y\", \"xzy\"]', 'all', 'x\\\\_y'), JSON_SEARCH('[\"abc\"]', 'one', '%b%')", "NULL|\"$[0]\"|\"$[0]\"");
        // LIKE dell'executor: stessi due problemi (ricorsione per ogni %, tempo esponenziale)
        t("SELECT 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' LIKE '%a%a%a%a%a%a%a%a%a%a%a%a%a%b', 'abcabc' LIKE '%b_a%c', "
          "'100%' LIKE '100!%' ESCAPE '!', '1000' LIKE '100!%' ESCAPE '!', 'AbC' LIKE 'a%c', 'abc' LIKE 'ab', '' LIKE '%', 'x' LIKE ''",
          "0|1|1|0|1|0|1|0");
        k2 = snprintf(deep, sizeof(deep), "SELECT 'zzz' LIKE '");
        for (int i = 0; i < 60000; i++) { deep[k2++] = '%'; deep[k2++] = 'z'; }
        snprintf(deep + k2, sizeof(deep) - (size_t)k2, "'");
        t(deep, "0");
    }
    {   // P1.14: la scansione copia solo i testi usati dopo i filtri di lettura; ogni altro uso deve vedere il valore
        t("CREATE TABLE pr1 (id INT PRIMARY KEY, a VARCHAR(10), b VARCHAR(10), c TEXT)", "OK:0");
        t("CREATE TABLE pr2 (id INT PRIMARY KEY, a VARCHAR(10), z VARCHAR(10))", "OK:0");
        t("INSERT INTO pr1 VALUES (1, 'x', 'p', 'c1'), (2, 'y', 'q', 'c2'), (3, 'x', 'r', 'c3')", "OK:3");
        t("INSERT INTO pr2 VALUES (10, 'x', 'z1'), (20, 'y', 'z2')", "OK:2");
        t("SELECT COUNT(*), SUM(id) FROM pr1 WHERE b <> 'zz' AND c LIKE 'c%'", "3|6");
        t("SELECT id FROM pr1 WHERE a = 'x' ORDER BY c DESC", "3;1");
        t("SELECT a, COUNT(*) FROM pr1 WHERE b <> 'q' GROUP BY a", "x|2");
        t("SELECT a FROM pr1 GROUP BY a HAVING MAX(c) = 'c3'", "x");
        t("SELECT id, (SELECT z FROM pr2 WHERE pr2.a = pr1.a) FROM pr1 WHERE c <> 'c2' ORDER BY id", "1|z1;3|z1");
        t("SELECT pr1.id, z FROM pr1 NATURAL JOIN pr2 ORDER BY pr1.id", "");
        t("SELECT pr1.id, z FROM pr1 JOIN pr2 USING (a) ORDER BY pr1.id", "1|z1;2|z2;3|z1");
        t("SELECT * FROM pr1 WHERE b = 'q'", "2|y|q|c2");
        t("SELECT id, ROW_NUMBER() OVER (PARTITION BY a ORDER BY b DESC) FROM pr1 ORDER BY id", "1|2;2|1;3|1");
        t("SELECT id FROM pr1 WHERE EXISTS (SELECT 1 FROM pr2 WHERE pr2.a = pr1.a AND pr1.b = 'q')", "2");
        t("SELECT id FROM pr1 LIMIT 2", "1;2");
        t("SELECT id, c FROM pr1 LIMIT 1 OFFSET 2", "3|c3");
        t("SELECT id FROM pr1 WHERE b > 'p' LIMIT 1", "2");
        t("SELECT DISTINCT a FROM pr1 LIMIT 1", "x");
        t("SELECT CONCAT(a, b) FROM pr1 WHERE c = 'c1'", "xp");
        t("DROP TABLE pr1", "OK:0");
        t("DROP TABLE pr2", "OK:0");
    }
    t("SELECT DATE_TRUNC('month', '2024-03-15 10:20:30'), DATE_TRUNC('week', '2024-03-15'), DATE_PART('quarter', '2024-08-01')", "2024-03-01 00:00:00|2024-03-11 00:00:00|3");
    t("SELECT MAKE_SET(5, 'a', 'b', 'c'), EXPORT_SET(5, 'Y', 'N', ',', 4)", "a,c|Y,N,Y,N");
    // ── RETURNING, ON CONFLICT, MERGE, ROLLUP / CUBE / GROUPING SETS, PREPARE / EXECUTE ──
    t("CREATE TABLE ut (id INT PRIMARY KEY AUTO_INCREMENT, nome VARCHAR(20) UNIQUE, n INT DEFAULT 0)", "OK:0");
    t("INSERT INTO ut (nome) VALUES ('a'), ('b') RETURNING id, nome, n * 10 AS dieci", "1|a|0;2|b|0");
    t("INSERT INTO ut (nome, n) VALUES ('a', 5) ON CONFLICT (nome) DO NOTHING", "OK:0");
    t("INSERT INTO ut (nome, n) VALUES ('a', 5) ON CONFLICT (nome) DO UPDATE SET n = ut.n + EXCLUDED.n", "OK:2");
    t("SELECT n FROM ut WHERE nome = 'a'", "5");
    t("INSERT INTO ut (nome, n) VALUES ('b', 7) ON CONFLICT (nome) DO UPDATE SET n = EXCLUDED.n WHERE ut.n > 100", "OK:2");
    t("SELECT n FROM ut WHERE nome = 'b'", "0");
    t("UPDATE ut SET n = n + 1 WHERE nome = 'b' RETURNING *", "2|b|1");
    t("DELETE FROM ut WHERE nome = 'b' RETURNING id, UPPER(nome)", "2|B");
    t("UPDATE ut SET n = 0 WHERE id = 99 RETURNING id", "#0");
    t("CREATE TABLE mag (cod VARCHAR(10) PRIMARY KEY, qta INT, nota VARCHAR(20))", "OK:0");
    t("INSERT INTO mag VALUES ('A', 10, NULL), ('B', 0, NULL), ('C', 5, NULL)", "OK:3");
    t("CREATE TABLE arrivi (cod VARCHAR(10), qta INT)", "OK:0");
    t("INSERT INTO arrivi VALUES ('A', 5), ('B', 0), ('D', 7)", "OK:3");
    t("MERGE INTO mag AS m USING arrivi AS a ON m.cod = a.cod WHEN MATCHED AND a.qta = 0 THEN DELETE "
      "WHEN MATCHED THEN UPDATE SET qta = m.qta + a.qta, nota = 'agg' WHEN NOT MATCHED THEN INSERT (cod, qta, nota) VALUES (a.cod, a.qta, 'nuovo')", "OK:3");
    t("SELECT cod, qta, nota FROM mag ORDER BY cod", "A|15|agg;C|5|NULL;D|7|nuovo");
    t("MERGE INTO mag USING (VALUES ('Z', 9)) AS v(cod, q) ON mag.cod = v.cod WHEN NOT MATCHED THEN INSERT VALUES (v.cod, v.q, 'l''ape')", "OK:1");
    t("SELECT nota FROM mag WHERE cod = 'Z'", "l'ape");
    t("CREATE TABLE vend (anno INT, reg VARCHAR(10), imp INT)", "OK:0");
    t("INSERT INTO vend VALUES (2023,'N',10),(2023,'S',20),(2024,'N',30),(2024,'S',40),(2024,'S',5)", "OK:5");
    t("SELECT anno, reg, SUM(imp) FROM vend GROUP BY anno, reg WITH ROLLUP", "2023|N|10;2023|S|20;2024|N|30;2024|S|45;2023|NULL|30;2024|NULL|75;NULL|NULL|105");
    t("SELECT anno, SUM(imp), GROUPING(anno) FROM vend GROUP BY ROLLUP(anno) ORDER BY anno", "NULL|105|1;2023|30|0;2024|75|0");
    t("SELECT anno, reg, COUNT(*) FROM vend GROUP BY CUBE(anno, reg) HAVING COUNT(*) > 2", "2024|NULL|3;NULL|S|3;NULL|NULL|5");
    t("SELECT anno, reg, SUM(imp) FROM vend GROUP BY GROUPING SETS ((anno), (reg), ())", "2023|NULL|30;2024|NULL|75;NULL|N|40;NULL|S|65;NULL|NULL|105");
    t("PREPARE ins FROM 'INSERT INTO vend VALUES (?, ?, ?)'", "OK:0");
    t("SET @a = 2025, @b = 'l''ape?', @c = 1", "OK:0");
    t("EXECUTE ins USING @a, @b, @c", "OK:1");
    t("SELECT reg FROM vend WHERE anno = 2025", "l'ape?");
    t("SET @q = 'SELECT COUNT(*) FROM vend WHERE anno > ?'", "OK:0");
    t("PREPARE conta FROM @q", "OK:0");
    t("SET @y = 2023", "OK:0");
    t("EXECUTE conta USING @y", "4");
    t("EXECUTE conta", "ERR:Incorrect arguments to EXECUTE");
    t("DEALLOCATE PREPARE conta", "OK:0");
    t("EXECUTE conta USING @y", "ERR:Unknown prepared statement");
    t("CREATE TABLE idt (id INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, x INT)", "OK:0");
    t("CREATE TABLE idt2 (id INT IDENTITY(1, 1) PRIMARY KEY, x INT)", "OK:0");
    t("INSERT INTO idt (x) VALUES (5), (6) RETURNING id", "1;2");
    // colonne calcolate
    t("CREATE TABLE calc (id INT PRIMARY KEY, prezzo DECIMAL(8,2), qta INT, totale DECIMAL(10,2) GENERATED ALWAYS AS (prezzo * qta) STORED, "
      "nome VARCHAR(20), sigla VARCHAR(5) AS (UPPER(LEFT(nome, 3))))", "OK:0");
    t("INSERT INTO calc (id, prezzo, qta, nome) VALUES (1, 2.50, 4, 'penna'), (2, 10, NULL, 'zaino')", "OK:2");
    t("SELECT totale, sigla FROM calc ORDER BY id", "10.00|PEN;NULL|ZAI");
    t("INSERT INTO calc (id, prezzo, qta, totale) VALUES (3, 1, 1, 5)", "ERR:generated column 'totale'");
    t("UPDATE calc SET qta = 3 WHERE id = 2", "OK:1");
    t("SELECT totale FROM calc WHERE id = 2", "30.00");
    t("UPDATE calc SET totale = 1", "ERR:generated column 'totale'");
    t("SELECT GENERATION_EXPRESSION FROM information_schema.COLUMNS WHERE TABLE_NAME = 'calc' AND COLUMN_NAME = 'sigla'", "UPPER(LEFT(nome, 3))");
    t("ALTER TABLE calc ADD COLUMN iva DECIMAL(10,2) AS (totale * 0.22)", "OK:0");
    t("SELECT iva FROM calc ORDER BY id", "2.20;6.60");
    t("ALTER TABLE calc RENAME COLUMN qta TO quantita", "OK:0");
    t("UPDATE calc SET quantita = 10 WHERE id = 1", "OK:1");
    t("SELECT totale, iva FROM calc WHERE id = 1", "25.00|5.50");
    t("ALTER TABLE calc DROP COLUMN prezzo", "ERR:used by generated column 'totale'");
    t("CREATE TABLE calc2 (a INT, b INT AS (nonesiste + 1))", "ERR:Unknown column 'nonesiste'");
    // LATERAL, APPLY, GENERATE_SERIES, JSON_TABLE
    t("CREATE TABLE lrep (id INT PRIMARY KEY, nome VARCHAR(10))", "OK:0");
    t("INSERT INTO lrep VALUES (1,'IT'),(2,'HR'),(3,'Vuoto')", "OK:3");
    t("CREATE TABLE ldip (id INT PRIMARY KEY, rep INT, nome VARCHAR(10), stip INT)", "OK:0");
    t("INSERT INTO ldip VALUES (1,1,'Ada',3000),(2,1,'Bruno',2500),(3,1,'Elena',2600),(4,2,'Carla',2800),(5,2,'Dario',2200)", "OK:5");
    t("SELECT r.nome, t.nome FROM lrep r JOIN LATERAL (SELECT nome, stip FROM ldip d WHERE d.rep = r.id ORDER BY stip DESC LIMIT 2) t ON TRUE ORDER BY r.id, t.stip DESC",
      "IT|Ada;IT|Elena;HR|Carla;HR|Dario");
    t("SELECT r.nome, t.n FROM lrep r LEFT JOIN LATERAL (SELECT COUNT(*) AS n FROM ldip d WHERE d.rep = r.id) t ON TRUE ORDER BY r.id", "IT|3;HR|2;Vuoto|0");
    t("SELECT r.nome, t.nome FROM lrep r OUTER APPLY (SELECT nome FROM ldip d WHERE d.rep = r.id AND stip > 2700) t ORDER BY r.id", "IT|Ada;HR|Carla;Vuoto|NULL");
    t("SELECT r.nome FROM lrep r CROSS APPLY (SELECT 1 AS x FROM ldip d WHERE d.rep = r.id LIMIT 1) t ORDER BY r.id", "IT;HR");
    t("SELECT r.nome, x.tot FROM lrep r, LATERAL (SELECT SUM(stip) AS tot FROM ldip WHERE ldip.rep = r.id) x WHERE x.tot > 5000", "IT|8100");
    t("SELECT * FROM generate_series(1, 4)", "1;2;3;4");
    t("SELECT g.n * 2 FROM GENERATE_SERIES(10, 1, -3) AS g(n)", "20;14;8;2");
    t("SELECT r.nome, s.generate_series FROM lrep r CROSS JOIN LATERAL GENERATE_SERIES(1, r.id) s WHERE r.id < 3 ORDER BY r.id, 2", "IT|1;HR|1;HR|2");
    t("SELECT * FROM generate_series(1, 2, 0)", "ERR:step cannot be zero");
    t("SELECT * FROM JSON_TABLE('[{\"a\":1,\"b\":\"x\"},{\"a\":2,\"c\":true}]', '$[*]' COLUMNS (n FOR ORDINALITY, a INT PATH '$.a', b VARCHAR(10) PATH '$.b', hc INT EXISTS PATH '$.c')) AS jt",
      "1|1|x|0;2|2|NULL|1");
    t("CREATE TABLE lord (id INT, righe TEXT)", "OK:0");
    t("INSERT INTO lord VALUES (1, '{\"items\":[{\"sku\":\"A\",\"q\":2},{\"sku\":\"B\",\"q\":1}]}'), (2, '{\"items\":[{\"sku\":\"C\",\"q\":5}]}')", "OK:2");
    t("SELECT o.id, SUM(j.q) FROM lord o JOIN JSON_TABLE(o.righe, '$.items[*]' COLUMNS (q INT PATH '$.q')) j GROUP BY o.id ORDER BY o.id", "1|3;2|5");
    t("SELECT o.id, j.sku FROM lord o, JSON_TABLE(o.righe, '$.items[*]' COLUMNS (sku VARCHAR(5) PATH '$.sku')) AS j WHERE j.sku <> 'B' ORDER BY j.sku", "1|A;2|C");
    // JSON_TABLE: NESTED PATH (anche più livelli e fratelli), ON EMPTY / ON ERROR
    t("SELECT * FROM JSON_TABLE('[{\"a\":\"x\",\"b\":[{\"c\":\"c1\",\"l\":[1,2]},{\"c\":\"c2\",\"l\":[]}]},{\"a\":\"y\",\"b\":[]}]', '$[*]' COLUMNS (top FOR ORDINALITY, a VARCHAR(10) PATH '$.a', NESTED PATH '$.b[*]' COLUMNS (c VARCHAR(10) PATH '$.c', o FOR ORDINALITY, NESTED PATH '$.l[*]' COLUMNS (l INT PATH '$')))) jt",
      "1|x|c1|1|1;1|x|c1|1|2;1|x|c2|2|NULL;2|y|NULL|NULL|NULL");
    t("SELECT * FROM JSON_TABLE('{\"n\":\"bob\",\"tel\":[\"1\",\"2\"],\"mail\":[\"m\"]}', '$' COLUMNS (n TEXT PATH '$.n', NESTED PATH '$.tel[*]' COLUMNS (tel TEXT PATH '$'), NESTED '$.mail[*]' COLUMNS (mail TEXT PATH '$'))) j",
      "bob|1|NULL;bob|2|NULL;bob|NULL|m");
    t("SELECT o.id, j.sku FROM lord o, JSON_TABLE(o.righe, '$' COLUMNS (NESTED PATH '$.items[*]' COLUMNS (sku VARCHAR(5) PATH '$.sku'))) j ORDER BY 1, 2", "1|A;1|B;2|C");
    t("SELECT * FROM JSON_TABLE('[{\"x\":\"abc\"},{\"y\":1},{\"x\":5}]', '$[*]' COLUMNS (x INT PATH '$.x' DEFAULT '-1' ON EMPTY DEFAULT '0' ON ERROR, y INT PATH '$.y' DEFAULT -9 ON EMPTY)) j",
      "0|-9;-1|1;5|-9");
    t("SELECT * FROM JSON_TABLE('[{\"y\":1}]', '$[*]' COLUMNS (x INT PATH '$.x' ERROR ON EMPTY)) j", "ERR:Missing value for JSON_TABLE column 'x'");
    t("SELECT * FROM JSON_TABLE('[{\"x\":[1]}]', '$[*]' COLUMNS (x INT PATH '$.x' NULL ON EMPTY ERROR ON ERROR)) j", "ERR:Can't store the value");
    t("SELECT * FROM JSON_TABLE('[{\"x\":{\"k\":1}}]', '$[*]' COLUMNS (x INT PATH '$.x', xs TEXT PATH '$.x')) j", "NULL|{\"k\":1}");
    t("SELECT * FROM JSON_TABLE('[1]', '$[*]' COLUMNS (NESTED PATH '$' COLUMNS (v INT PATH '$')) j", "ERR:");
    // ALTER TABLE con più operazioni (migrazioni di Laravel / Doctrine), CHANGE, ADD / DROP INDEX
    t("create table `lu` (`id` bigint unsigned not null auto_increment primary key, `name` varchar(255) not null, `email` varchar(255) not null)", "OK:0");
    t("insert into lu (name, email) values ('Anna', 'a@x.it')", "OK:1");
    t("alter table `lu` add unique `lu_email_unique`(`email`)", "OK:0");
    t("alter table `lu` add `votes` int not null default '0' after `name`, add `bio` text null, add index `lu_votes_index`(`votes`)", "OK:0");
    t("SELECT votes, bio FROM lu", "0|NULL");
    t("SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_NAME = 'lu' ORDER BY INDEX_NAME", "PRIMARY;lu_email_unique;lu_votes_index");
    t("alter table lu change bio about varchar(500) null, modify name varchar(100) not null", "OK:0");
    t("SELECT name, about FROM lu", "Anna|NULL");
    t("alter table lu drop index lu_votes_index, drop column votes", "OK:0");
    t("SELECT * FROM lu", "1|Anna|a@x.it|NULL");
    t("alter table lu add column x int, add column x int", "ERR:");
    t("CREATE TABLE lp (id INT PRIMARY KEY, uid BIGINT, CONSTRAINT lp_uid_fk FOREIGN KEY (uid) REFERENCES lu (id))", "OK:0");
    t("ALTER TABLE lp DROP FOREIGN KEY lp_uid_fk", "OK:0");
    t("INSERT INTO lp VALUES (1, 99)", "OK:1");
    // sequenze
    t("CREATE SEQUENCE fatt START WITH 100 INCREMENT BY 10", "OK:0");
    t("SELECT CURRVAL('fatt')", "ERR:not yet defined in this session");
    t("SELECT NEXTVAL('fatt'), NEXTVAL('fatt')", "100|110");
    t("SELECT CURRVAL('fatt'), LASTVAL()", "110|110");
    t("CREATE TABLE fseq (n BIGINT PRIMARY KEY, x VARCHAR(5))", "OK:0");
    t("INSERT INTO fseq VALUES (NEXT VALUE FOR fatt, 'a'), (fatt.NEXTVAL, 'b')", "OK:2");
    t("SELECT n FROM fseq ORDER BY n", "120;130");
    t("SELECT SETVAL('fatt', 500)", "500");
    t("SELECT nextval('fatt')", "510");
    t("ALTER SEQUENCE fatt RESTART WITH 1", "OK:0");
    t("SELECT nextval('fatt')", "1");
    t("CREATE SEQUENCE giro MINVALUE 1 MAXVALUE 3 CYCLE", "OK:0");
    t("SELECT nextval('giro'), nextval('giro'), nextval('giro'), nextval('giro')", "1|2|3|1");
    t("CREATE SEQUENCE fine MAXVALUE 1", "OK:0");
    t("SELECT nextval('fine')", "1");
    t("SELECT nextval('fine')", "ERR:reached the limit");
    t("CREATE SEQUENCE giu INCREMENT BY -1", "OK:0");
    t("SELECT nextval('giu'), nextval('giu')", "-1|-2");
    t("SELECT SEQUENCE_NAME FROM information_schema.SEQUENCES WHERE SEQUENCE_SCHEMA = 'rt' ORDER BY 1", "fatt;fine;giro;giu");
    t("CREATE SEQUENCE fatt", "ERR:already exists");
    t("DROP SEQUENCE giro", "OK:0");
    t("DROP SEQUENCE giro", "ERR:does not exist");
    t("DROP SEQUENCE IF EXISTS giro", "OK:0");
    // INTERVAL enorme (trovato dal fuzzer, P3.3): NULL, non più un overflow di intero (comportamento indefinito)
    t("SELECT DATE_ADD('2020-01-01', INTERVAL '99999999999999999999' DAY), DATE_ADD('2020-01-01', INTERVAL '1844674407370955161' SECOND)", "NULL|NULL");
    // RANGE con INTERVAL sulle date; query su più database
    t("CREATE TABLE incassi (g DATE, x INT)", "OK:0");
    t("INSERT INTO incassi VALUES ('2024-01-01',10),('2024-01-02',20),('2024-01-05',30),('2024-01-06',40),('2024-01-10',50)", "OK:5");
    t("SELECT g, SUM(x) OVER (ORDER BY g RANGE BETWEEN INTERVAL 2 DAY PRECEDING AND CURRENT ROW) FROM incassi ORDER BY g",
      "2024-01-01|10;2024-01-02|30;2024-01-05|30;2024-01-06|70;2024-01-10|50");
    t("CREATE DATABASE negozio", "OK:0");
    t("CREATE TABLE negozio.articolo (id INT PRIMARY KEY, nome VARCHAR(10))", "OK:0");
    t("INSERT INTO negozio.articolo VALUES (1, 'penna'), (2, 'zaino')", "OK:2");
    t("CREATE TABLE righe (art INT, qta INT)", "OK:0");
    t("INSERT INTO righe VALUES (1, 3), (2, 1), (1, 2)", "OK:3");
    t("SELECT a.nome, SUM(r.qta) FROM negozio.articolo a JOIN righe r ON r.art = a.id GROUP BY a.nome ORDER BY a.nome", "penna|5;zaino|1");
    t("SELECT nome FROM negozio.articolo WHERE id IN (SELECT art FROM rt.righe WHERE qta > 2)", "penna");
    t("CREATE TABLE copia_art (n VARCHAR(10))", "OK:0");
    t("INSERT INTO copia_art SELECT nome FROM negozio.articolo", "OK:2");
    t("DELETE FROM copia_art WHERE n = (SELECT nome FROM negozio.articolo WHERE id = 2)", "OK:1");
    t("SELECT n FROM copia_art", "penna");
    t("INSERT INTO negozio.articolo SELECT 3, 'gomma' FROM rt.righe LIMIT 1", "OK:1");
    t("SELECT COUNT(*) FROM negozio.articolo", "3");
    t("SHOW FUNCTION STATUS", "#7");
    t("SHOW PROCEDURE STATUS LIKE 'p%'", "#2");
    t("SELECT `Type` FROM (SHOW FUNCTION STATUS) AS s LIMIT 1", "ERR:");
    t("SHOW CREATE FUNCTION segno", "#1");
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
    t("USE rt", "OK:0");                                                   // le routine restano dopo un riavvio
    t("SELECT fatt(6), giudizio(26)", "720|buono");
    t("SELECT @a", "NULL");                                                 // le @variabili no (sessione nuova)
    t("SELECT nextval('fatt')", "11");                                     // le sequenze sì (incremento 10)

    // trovati dal fuzzer (P3.3): SET incompleto in una routine (lettura oltre l'array), scala DECIMAL enorme
    t("CREATE PROCEDURE fz() BEGIN DECLARE a INT; SET a = 1, b END", "ERR:Expected '=' in SET");
    t("CREATE TABLE fzd (x DECIMAL(10,200))", "ERR:Too big scale");
    t("CREATE TABLE fzd (x DECIMAL(12,4))", "OK:0");
    t("INSERT INTO fzd VALUES (1.23456)", "OK:1");
    t("SELECT x FROM fzd", "1.2346");

    db_shutdown();
    crash_recovery_test();
    if (system("rm -rf /tmp/sv_sql_test") != 0) { /* nulla */ }
    printf("%s: %d/%d SQL tests passed\n", failed ? "FAIL" : "OK", tests - failed, tests);
    return failed ? 1 : 0;
}
