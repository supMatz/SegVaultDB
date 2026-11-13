// Compatibilità con MySQL: il dialetto che usano le applicazioni PHP (WordPress, Laravel, ...).
//   make test_mysql && ./test_mysql
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests = 0, failed = 0;

static void render(const QueryResult* r, char* out, size_t n) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < r->num_rows; i++)
        for (int c = 0; c < r->num_cols; c++) {
            const Cell* cell = &r->rows[i].cells[c];
            len += (size_t)snprintf(out + len, n - len, "%s%s", c ? "|" : (i ? ";" : ""), cell->is_null ? "NULL" : cell->value);
            if (len >= n) return;
        }
}

// expect: righe "a|b;c|d", "OK:n" (righe modificate), "#n" (numero di righe), "ERR:testo"
static void t(const char* sql, const char* expect) {
    tests++;
    QueryResult* r = db_execute(sql);
    char got[8192];
    if (!r) snprintf(got, sizeof(got), "ERR:(null result)");
    else if (!r->success) snprintf(got, sizeof(got), "ERR:%s", r->error);
    else if (r->num_cols == 0) snprintf(got, sizeof(got), "OK:%d", r->rows_affected);
    else if (expect[0] == '#') snprintf(got, sizeof(got), "#%d", r->num_rows);
    else render(r, got, sizeof(got));
    bool ok = !strncmp(expect, "ERR:", 4) ? (!strncmp(got, "ERR:", 4) && strstr(got + 4, expect + 4)) : !strcmp(got, expect);
    if (!ok) { failed++; printf("FAIL %d: %s\n   expected: %s\n   got:      %s\n", tests, sql, expect, got); }
    db_result_free(r);
}

static void names(const char* sql, const char* expect) {
    tests++;
    QueryResult* r = db_execute(sql);
    char got[2048] = "";
    for (int c = 0; r && r->success && c < r->num_cols; c++)
        snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%s", c ? "|" : "", r->col_names[c]);
    if (!r || !r->success) snprintf(got, sizeof(got), "ERR:%s", r ? r->error : "null");
    if (strcmp(got, expect)) { failed++; printf("FAIL %d: %s\n   expected columns: %s\n   got:              %s\n", tests, sql, expect, got); }
    db_result_free(r);
}

int main(void) {
    char dir[] = "/tmp/svmysqlXXXXXX";
    if (!mkdtemp(dir) || !db_init(dir)) { printf("init failed\n"); return 1; }
    t("CREATE DATABASE wp", "OK:0");
    t("USE wp", "OK:0");

    // ── CREATE TABLE come lo scrive WordPress ──
    t("CREATE TABLE wp_posts (\n"
      " ID bigint(20) unsigned NOT NULL auto_increment,\n"
      " post_author bigint(20) unsigned NOT NULL default '0',\n"
      " post_date datetime NOT NULL default '0000-00-00 00:00:00',\n"
      " post_content longtext NOT NULL,\n"
      " post_title text NOT NULL,\n"
      " post_status varchar(20) NOT NULL default 'publish',\n"
      " post_password varchar(255) NOT NULL default '',\n"
      " post_name varchar(200) NOT NULL default '',\n"
      " menu_order int(11) NOT NULL default '0',\n"
      " price decimal(10,2) DEFAULT NULL COMMENT 'prezzo',\n"
      " kind enum('a','b','it''s') NOT NULL DEFAULT 'a',\n"
      " created timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,\n"
      " PRIMARY KEY  (ID),\n"
      " KEY post_name (post_name(191)),\n"
      " KEY type_status_date (post_status,post_date,ID),\n"
      " UNIQUE KEY uq_title (post_name, menu_order),\n"
      " FULLTEXT KEY ft (post_title, post_content)\n"
      ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_520_ci AUTO_INCREMENT=100", "OK:0");
    t("CREATE TABLE IF NOT EXISTS `t2` (`id` INT NOT NULL, `v` TINYINT(1) DEFAULT 1, PRIMARY KEY (`id`)) ENGINE=MyISAM DEFAULT CHARSET=latin1", "OK:0");
    t("CREATE TABLE bad (id INT, PARTITION BY HASH(id))", "ERR:");
    t("INSERT INTO wp_posts (post_content, post_title, post_name) VALUES ('c', 't', 'n1')", "OK:1");
    t("SELECT ID, post_password, post_date, post_status, kind, menu_order FROM wp_posts", "100||0000-00-00 00:00:00|publish|a|0");
    t("SELECT created IS NOT NULL FROM wp_posts", "1");
    t("INSERT INTO wp_posts (post_content, post_title, post_name, kind) VALUES ('c', 't', 'n2', 'zz')", "ERR:");
    t("INSERT INTO wp_posts (post_content, post_title, post_name, kind) VALUES ('c', 't', 'n3', 'it''s')", "OK:1");
    t("INSERT INTO wp_posts (post_content, post_title, post_name, post_author) VALUES ('c', 't', 'n4', -1)", "ERR:out of range");
    t("INSERT INTO wp_posts (post_content, post_title, post_name, price) VALUES ('c', 't', 'n5', 1.255), ('c', 't', 'n6', '7'), ('c', 't', 'n7', -0.004)", "OK:3");
    t("SELECT price FROM wp_posts WHERE price IS NOT NULL ORDER BY ID", "1.26;7.00;0.00");

    // ── SHOW e information_schema ──
    names("SHOW FULL COLUMNS FROM wp_posts", "Field|Type|Collation|Null|Key|Default|Extra|Privileges|Comment");
    t("SELECT COLUMN_TYPE, IS_NULLABLE, COLUMN_KEY, EXTRA FROM (SELECT * FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'wp' AND TABLE_NAME = 'wp_posts' AND COLUMN_NAME = 'ID') AS x", "bigint(20) unsigned|NO|PRI|auto_increment");
    t("SHOW COLUMNS FROM wp_posts LIKE 'ID'", "ID|bigint(20) unsigned|NO|PRI|NULL|auto_increment");
    t("SHOW COLUMNS FROM wp_posts WHERE `Field` = 'price'", "price|decimal(10,2)|YES||NULL|");
    t("SHOW COLUMNS FROM wp_posts LIKE 'kind'", "kind|enum('a','b','it''s')|NO||a|");
    t("SHOW FIELDS FROM wp_posts FROM wp LIKE 'created'", "created|timestamp|NO||CURRENT_TIMESTAMP|DEFAULT_GENERATED on update CURRENT_TIMESTAMP");
    t("SELECT Key_name, Seq_in_index, Column_name, Non_unique FROM (SHOW INDEX FROM wp_posts) AS i", "ERR:");
    names("SHOW INDEX FROM wp_posts", "Table|Non_unique|Key_name|Seq_in_index|Column_name|Collation|Cardinality|Sub_part|Packed|Null|Index_type|Comment|Index_comment|Visible|Expression");
    t("SHOW KEYS FROM wp_posts WHERE Key_name = 'type_status_date'", "#3");
    t("SELECT INDEX_NAME, SEQ_IN_INDEX, COLUMN_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = 'wp' AND TABLE_NAME = 'wp_posts' AND INDEX_NAME = 'uq_title' ORDER BY SEQ_IN_INDEX", "uq_title|1|post_name;uq_title|2|menu_order");
    t("SELECT INDEX_TYPE FROM information_schema.STATISTICS WHERE INDEX_NAME = 'ft' AND SEQ_IN_INDEX = 1", "FULLTEXT");
    t("SHOW TABLES LIKE 'wp\\_%'", "wp_posts");
    names("SHOW TABLES", "Tables_in_wp");
    t("SHOW TABLES FROM wp LIKE 't%'", "t2");
    t("SELECT `Name`, `Engine`, `Rows`, `Auto_increment`, `Collation`, `Comment` FROM (SELECT * FROM information_schema.TABLES) AS x", "ERR:");
    t("SHOW TABLE STATUS LIKE 'wp_posts'", "#1");
    t("SELECT TABLE_NAME, TABLE_TYPE, ENGINE, AUTO_INCREMENT, TABLE_COLLATION FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'wp' ORDER BY TABLE_NAME",
      "t2|BASE TABLE|InnoDB|NULL|latin1_swedish_ci;wp_posts|BASE TABLE|InnoDB|105|utf8mb4_unicode_520_ci");
    t("SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = 'wp'", "1");
    t("SELECT DATA_TYPE, CHARACTER_MAXIMUM_LENGTH, NUMERIC_PRECISION, NUMERIC_SCALE FROM information_schema.COLUMNS WHERE TABLE_NAME = 'wp_posts' AND COLUMN_NAME = 'price'", "decimal|NULL|10|2");
    t("SELECT CONSTRAINT_TYPE FROM information_schema.TABLE_CONSTRAINTS WHERE TABLE_NAME = 'wp_posts' ORDER BY CONSTRAINT_TYPE", "CHECK;PRIMARY KEY;UNIQUE");
    t("DELETE FROM information_schema.TABLES", "ERR:read-only");
    t("SELECT * FROM information_schema.NOPE", "ERR:Unknown table");
    t("SHOW CREATE DATABASE wp", "#1");
    t("SHOW DATABASES LIKE 'w%'", "wp");
    t("SHOW ENGINES", "#1");
    t("SHOW COLLATION LIKE 'utf8mb4_general%'", "#1");
    t("SHOW WARNINGS", "#0");

    // ── INSERT di MySQL ──
    t("CREATE TABLE o (option_id bigint unsigned NOT NULL auto_increment, option_name varchar(191) NOT NULL default '', "
      "option_value longtext NOT NULL, autoload varchar(20) NOT NULL DEFAULT 'yes', PRIMARY KEY (option_id), UNIQUE KEY option_name (option_name))", "OK:0");
    t("INSERT INTO o (option_name, option_value) VALUES ('a', '1') ON DUPLICATE KEY UPDATE option_value = VALUES(option_value)", "OK:1");
    t("INSERT INTO `o` (`option_name`, `option_value`, `autoload`) VALUES ('a', '2', 'no') ON DUPLICATE KEY UPDATE `option_name` = VALUES(`option_name`), `option_value` = VALUES(`option_value`), `autoload` = VALUES(`autoload`)", "OK:2");
    t("SELECT option_id, option_value, autoload FROM o", "1|2|no");
    t("INSERT INTO o (option_name, option_value) VALUES ('a', 'x'), ('b', 'y') AS n ON DUPLICATE KEY UPDATE option_value = CONCAT(o.option_value, n.option_value)", "OK:3");
    t("SELECT option_name, option_value FROM o ORDER BY option_name", "a|2x;b|y");
    t("INSERT IGNORE INTO o (option_name, option_value) VALUES ('a', 'dup'), ('c', 'z')", "OK:1");
    // righe di soli default: () VALUES (), VALUES (), più righe, DEFAULT VALUES
    t("CREATE TABLE visits (id INT AUTO_INCREMENT PRIMARY KEY, n INT DEFAULT 7, at DATETIME DEFAULT CURRENT_TIMESTAMP)", "OK:0");
    t("INSERT INTO visits () VALUES ()", "OK:1");
    t("INSERT INTO visits VALUES (), ()", "OK:2");
    t("INSERT INTO visits DEFAULT VALUES", "OK:1");
    t("SELECT COUNT(*), MAX(id), MIN(n), COUNT(at) FROM visits", "4|4|7|4");
    t("INSERT INTO visits () VALUES (1)", "ERR:Column count");
    t("REPLACE INTO o (option_name, option_value) VALUES ('c', 'new')", "OK:2");
    t("INSERT INTO o SET option_name = 'd', option_value = 'set'", "OK:1");
    t("SELECT option_name, option_value FROM o ORDER BY option_name", "a|2x;b|y;c|new;d|set");
    t("INSERT INTO o (option_name, option_value) VALUES ('a', 'q')", "ERR:Duplicate");
    t("INSERT INTO o (option_name, option_value) VALUES ('e', 'ok'), ('a', 'boom') ON DUPLICATE KEY UPDATE option_value = 1/0 + NOOO", "ERR:");
    t("SELECT COUNT(*) FROM o WHERE option_name = 'e'", "0");      // tutto o niente

    // ── UPDATE / DELETE con più tabelle, ORDER BY, LIMIT ──
    t("CREATE TABLE p (id INT PRIMARY KEY, n INT)", "OK:0");
    t("CREATE TABLE c (pid INT, v INT)", "OK:0");
    t("INSERT INTO p VALUES (1,0),(2,0),(3,0)", "OK:3");
    t("INSERT INTO c VALUES (1,5),(1,7),(2,1)", "OK:3");
    t("UPDATE p JOIN c ON c.pid = p.id SET p.n = c.v + 100 WHERE c.v > 2", "OK:1");
    t("UPDATE p SET n = n + 1 ORDER BY id DESC LIMIT 2", "OK:2");
    t("SELECT * FROM p ORDER BY id", "1|105;2|1;3|1");
    t("DELETE p, c FROM p JOIN c ON c.pid = p.id WHERE c.v = 1", "OK:2");
    t("SELECT (SELECT COUNT(*) FROM p), (SELECT COUNT(*) FROM c)", "2|2");
    t("DELETE FROM c AS x WHERE x.v = 5", "OK:1");
    t("DELETE FROM p ORDER BY id DESC LIMIT 1", "OK:1");
    t("SELECT id FROM p", "1");
    t("INSERT INTO o (option_name, option_value) VALUES ('_transient_k', 'x'), ('_transient_timeout_k', '100'), ('_transient_j', 'y'), ('_transient_timeout_j', '99999999999')", "OK:4");
    t("DELETE a, b FROM o a, o b WHERE a.option_name LIKE '\\_transient\\_%' AND a.option_name NOT LIKE '\\_transient\\_timeout\\_%' "
      "AND b.option_name = CONCAT('_transient_timeout_', SUBSTRING(a.option_name, 12)) AND b.option_value < 1000", "OK:2");
    t("SELECT option_name FROM o WHERE option_name LIKE '\\_transient%' ORDER BY option_name", "_transient_j;_transient_timeout_j");
    t("DELETE FROM o WHERE option_name REGEXP '^_transient' LIMIT 5", "OK:2");

    // ── SELECT: modificatori, FOUND_ROWS, lock, DISTINCT ──
    t("SELECT SQL_CALC_FOUND_ROWS option_name FROM o ORDER BY option_name LIMIT 1", "a");
    t("SELECT FOUND_ROWS()", "4");
    t("SELECT SQL_NO_CACHE DISTINCTROW autoload FROM o ORDER BY autoload", "no;yes");
    t("SELECT option_name FROM o WHERE option_name = 'a' FOR UPDATE", "a");
    t("SELECT option_name FROM o WHERE option_name = 'a' LOCK IN SHARE MODE", "a");
    t("SELECT DISTINCT autoload FROM o ORDER BY option_id", "ERR:must also appear");   // regola dello standard ...
    db_set_mysql_mode(true);
    t("SELECT DISTINCT autoload FROM o ORDER BY option_id", "no;yes");                 // ... e quella di MySQL
    db_set_mysql_mode(false);
    t("UPDATE o SET autoload = 'off' WHERE option_name = 'b'", "OK:1");
    t("SELECT ROW_COUNT()", "1");
    t("SELECT option_id FROM o WHERE option_id = '1'", "1");     // numero fra apici sulla chiave primaria
    t("SELECT option_id FROM o WHERE option_id IN ('1', '2')", "1;2");

    // ── funzioni ──
    t("SELECT LOCATE('b', 'abc'), INSTR('foobar', 'bar'), LPAD('42', 5, '0'), RPAD('ab', 4, '.'), FIELD('b', 'a', 'b'), FIND_IN_SET('c', 'a,b,c')", "2|4|00042|ab..|2|3");
    t("SELECT SUBSTRING_INDEX('www.mysql.com', '.', 2), SUBSTRING_INDEX('www.mysql.com', '.', -2), SUBSTRING('abcdef' FROM 2 FOR 3), MID('abcdef', 2, 2)", "www.mysql|mysql.com|bcd|bc");
    t("SELECT MD5('hello'), SHA1('abc'), CRC32('hello'), TO_BASE64('hi'), FROM_BASE64('aGk=')", "5d41402abc4b2a76b9719d911017c592|a9993e364706816aba3e25717850c26c9cd0d89d|907060870|aGk=|hi");
    t("SELECT SHA2('abc', 256)", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    t("SELECT FORMAT(1234567.891, 2), HEX('AB'), UNHEX('4142'), ASCII('A'), CHAR(72, 105), REPEAT('ab', 3), REVERSE('abc')", "1,234,567.89|4142|AB|65|Hi|ababab|cba");
    t("SELECT GREATEST(3, 9, 1), LEAST('b', 'a'), 7 DIV 2, 7 MOD 4, TRUNCATE(1.239, 2), ROUND(2.5), POW(2, 10)", "9|a|3|3|1.23|3|1024");
    t("SELECT CAST('42abc' AS SIGNED), CAST(12.345 AS DECIMAL(10,2)), CAST('2026-11-13 10:00:00' AS DATE), CONVERT('x', CHAR), CONVERT('y' USING utf8mb4)", "42|12.35|2026-11-13|x|y");
    t("SELECT IF(1 > 2, 'si', 'no'), IFNULL(NULL, 'd'), ISNULL(NULL), COALESCE(NULL, NULL, 3), NULLIF(1, 1)", "no|d|1|3|NULL");
    t("SELECT 'abc123' REGEXP '^[a-z]+[0-9]+$', 'abc' NOT REGEXP 'b', 'ABC' RLIKE 'b', REGEXP_LIKE('Abc', 'abc', 'c')", "1|0|1|0");
    t("SELECT REGEXP_REPLACE('x1y22z', '[0-9]+', '-'), REGEXP_SUBSTR('x1y22z', '[0-9]{2}'), REGEXP_INSTR('abc', 'c')", "x-y-z|22|3");
    t("SELECT 'a' REGEXP '('", "ERR:Invalid regular expression");
    t("SELECT TRIM(LEADING '0' FROM '00x0'), TRIM(TRAILING '0' FROM '00x0'), TRIM(BOTH 'x' FROM 'xxaxx'), TRIM('  s  '), POSITION('c' IN 'abcd')", "x0|00x|a|s|3");
    t("SELECT DATE_ADD('2026-02-15', INTERVAL 1 MONTH), DATE_ADD('2026-01-31', INTERVAL 1 MONTH), DATE_SUB('2026-03-01', INTERVAL 1 DAY)", "2026-03-15|2026-02-28|2026-02-28");
    t("SELECT '2026-11-13 08:00:00' + INTERVAL 2 HOUR, INTERVAL 1 DAY + '2026-12-31', DATE_ADD('2026-11-13 05:00:00', INTERVAL '1 1:30' DAY_MINUTE)", "2026-11-13 10:00:00|2027-01-01|2026-11-14 06:30:00");
    t("SELECT DATEDIFF('2026-11-27', '2026-11-13'), TIMESTAMPDIFF(MONTH, '2026-01-15', '2026-04-20'), TIMESTAMPDIFF(HOUR, '2026-01-01', '2026-01-02 06:00:00')", "14|3|30");
    t("SELECT DATE_FORMAT('2026-11-13 08:05:09', '%W %D %M %Y %H:%i:%s %p %j'), DATE_FORMAT('2026-11-13', '%a %b %e %y')", "Friday 13th November 2026 08:05:09 AM 317|Fri Nov 13 26");
    t("SELECT STR_TO_DATE('13/11/2026 08:05', '%d/%m/%Y %H:%i'), STR_TO_DATE('November 5 2026', '%M %e %Y'), STR_TO_DATE('x', '%Y')", "2026-11-13 08:05:00|2026-11-05|NULL");
    t("SELECT EXTRACT(YEAR FROM '2026-11-13'), EXTRACT(YEAR_MONTH FROM '2026-11-13'), YEAR('2026-11-13'), MONTH('2026-11-13'), DAYOFWEEK('2026-11-13'), WEEKDAY('2026-11-13')", "2026|202611|2026|11|6|4");
    t("SELECT WEEK('2026-11-13'), WEEK('2026-11-13', 3), YEARWEEK('2026-01-01'), QUARTER('2026-11-13'), LAST_DAY('2026-02-10'), DAYNAME('2026-11-13'), MONTHNAME('2026-11-13')", "45|46|202552|4|2026-02-28|Friday|November");
    t("SELECT UNIX_TIMESTAMP(FROM_UNIXTIME(1700000000)), FROM_UNIXTIME(0, '%Y') >= '1969', TO_DAYS('2026-11-13') - TO_DAYS('2026-11-12'), SEC_TO_TIME(3661), TIME_TO_SEC('01:01:01')", "1700000000|1|1|01:01:01|3661");
    t("SELECT DATE('2026-11-13 10:20:30'), TIME('2026-11-13 10:20:30'), HOUR('10:20:30'), MAKEDATE(2026, 32)", "2026-11-13|10:20:30|10|2026-02-01");
    t("SELECT CURRENT_TIMESTAMP IS NOT NULL, UTC_TIMESTAMP() IS NOT NULL, CURDATE() = CURRENT_DATE, LENGTH(UUID()), CONNECTION_ID() > 0", "1|1|1|36|1");
    t("SELECT USER(), DATABASE(), VERSION() LIKE '8.%', GET_LOCK('x', 1), RELEASE_LOCK('x')", "root@localhost|wp|1|1|1");
    t("SELECT _utf8mb4'testo', BINARY 'x' = 'x', DATE '2026-01-01'", "testo|1|2026-01-01");
    t("SELECT LAST_INSERT_ID(42), LAST_INSERT_ID()", "42|42");

    // ── GROUP_CONCAT ──
    t("CREATE TABLE g (k INT, v VARCHAR(10), n INT)", "OK:0");
    t("INSERT INTO g VALUES (1,'b',2),(1,'a',1),(2,'c',5),(1,'a',3),(2,NULL,1)", "OK:5");
    t("SELECT k, GROUP_CONCAT(v) FROM g GROUP BY k ORDER BY k", "1|b,a,a;2|c");
    t("SELECT k, GROUP_CONCAT(DISTINCT v ORDER BY v DESC SEPARATOR '|') FROM g GROUP BY k ORDER BY k", "1|b|a;2|c");
    t("SELECT GROUP_CONCAT(v, n ORDER BY n DESC SEPARATOR ';') FROM g WHERE k = 1", "a3;b2;a1");
    t("SELECT GROUP_CONCAT(v) FROM g WHERE k = 99", "NULL");

    // ── collation _ci: confronti senza maiuscole (solo per le tabelle create con una collation _ci) ──
    t("CREATE TABLE u (login VARCHAR(60) NOT NULL, nome VARCHAR(30), PRIMARY KEY (login), UNIQUE KEY em (nome)) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_520_ci", "OK:0");
    t("INSERT INTO u VALUES ('Admin', 'Anna'), ('bob', 'Bruno')", "OK:2");
    t("SELECT login FROM u WHERE login = 'admin'", "Admin");                 // chiave primaria (indice)
    t("SELECT login FROM u WHERE nome = 'ANNA'", "Admin");                   // indice UNIQUE
    t("SELECT login FROM u WHERE UPPER(login) = 'BOB' AND login = 'BOB'", "bob");
    t("INSERT INTO u VALUES ('ADMIN', 'x')", "ERR:Duplicate");               // come MySQL: stessa chiave
    t("INSERT INTO u VALUES ('carla', 'bruno')", "ERR:Duplicate");
    t("INSERT INTO u VALUES ('carla', 'Carla'), ('Dario', 'dario')", "OK:2");
    t("SELECT login FROM u ORDER BY login", "Admin;bob;carla;Dario");        // ordine senza maiuscole
    t("SELECT login FROM u WHERE login IN ('ADMIN', 'DARIO') ORDER BY login", "Admin;Dario");
    t("SELECT login FROM u WHERE login IN (SELECT UPPER(login) FROM u WHERE login < 'c') ORDER BY login", "Admin;bob");
    t("CREATE TABLE ux (k VARCHAR(10)) COLLATE utf8mb4_general_ci", "OK:0");
    t("INSERT INTO ux VALUES ('a'), ('A'), ('b'), ('B'), ('b')", "OK:5");
    t("SELECT COUNT(*) FROM (SELECT DISTINCT k FROM ux) AS d", "2");
    t("SELECT LOWER(k), COUNT(*) FROM ux GROUP BY k ORDER BY k", "a|2;b|3");
    t("SELECT COUNT(*) FROM ux JOIN u ON u.login = ux.k", "0");
    t("SELECT MIN(k) = 'a', MAX(k) = 'B' FROM ux", "1|1");
    t("CREATE TABLE cs (k VARCHAR(10) PRIMARY KEY)", "OK:0");                // SegVault: distingue le maiuscole
    t("INSERT INTO cs VALUES ('a'), ('A')", "OK:2");
    t("SELECT k FROM cs WHERE k = 'a'", "a");
    t("SELECT COUNT(DISTINCT k) FROM cs", "2");

    // ── LIKE: \ protegge % e _ come in MySQL e PostgreSQL ──
    t("SELECT 'a_b' LIKE 'a\\_b', 'axb' LIKE 'a\\_b', '50%' LIKE '50\\%', 'a\\\\b' LIKE 'a\\\\\\\\b'", "1|0|1|1");

    db_shutdown();
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) {}
    if (failed) { printf("MySQL compatibility: %d of %d tests FAILED\n", failed, tests); return 1; }
    printf("OK: %d/%d MySQL compatibility tests passed\n", tests, tests);
    return 0;
}
