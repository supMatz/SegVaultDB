// Utenti e permessi: istruzioni di gestione, controllo degli statement, password, persistenza.
#include "src/bridge/db_api.h"
#include "src/bridge/auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_err[1100];

// true se lo statement riesce (l'errore resta in g_err)
static bool ok(const char* sql) {
    QueryResult* r = db_execute(sql);
    bool good = r && r->success;
    snprintf(g_err, sizeof(g_err), "%s", r ? r->error : "null");
    db_result_free(r);
    return good;
}

static bool denied(const char* sql) {
    return !ok(sql) && strstr(g_err, "Access denied");
}

static int rows(const char* sql) {
    QueryResult* r = db_execute(sql);
    int n = r && r->success ? r->num_rows : -1;
    db_result_free(r);
    return n;
}

static bool has_row(const char* sql, const char* text) {
    QueryResult* r = db_execute(sql);
    bool found = false;
    for (int i = 0; r && r->success && i < r->num_rows && !found; i++)
        for (int c = 0; c < r->num_cols && !found; c++) found = strstr(r->rows[i].cells[c].value, text) != NULL;
    db_result_free(r);
    return found;
}

int main(void) {
    // SHA-256: vettori di prova standard
    uint8_t h[32];
    char hx[65];
    auth_sha256("abc", 3, h);
    for (int i = 0; i < 32; i++) snprintf(hx + i * 2, 3, "%02x", h[i]);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) = %s", hx);
    auth_sha256("", 0, h);
    for (int i = 0; i < 32; i++) snprintf(hx + i * 2, 3, "%02x", h[i]);
    CHECK(!strcmp(hx, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256('')");
    const char* l = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auth_sha256(l, strlen(l), h);
    for (int i = 0; i < 32; i++) snprintf(hx + i * 2, 3, "%02x", h[i]);
    CHECK(!strcmp(hx, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448 bit)");

    if (system("rm -rf /tmp/sv_auth_test && mkdir -p /tmp/sv_auth_test") != 0) return 1;
    if (!db_init("/tmp/sv_auth_test")) return 1;
    ok("CREATE DATABASE shop");
    ok("CREATE DATABASE hr");
    ok("USE shop");
    ok("CREATE TABLE prodotto (id INT PRIMARY KEY, nome VARCHAR(30), prezzo FLOAT)");
    ok("CREATE TABLE ordine (id INT PRIMARY KEY, prodotto INT, qta INT)");
    ok("CREATE TABLE segreto (id INT PRIMARY KEY, x VARCHAR(10))");
    ok("INSERT INTO prodotto VALUES (1, 'pane', 1.5), (2, 'latte', 1.2)");
    ok("INSERT INTO ordine VALUES (1, 1, 3)");
    ok("INSERT INTO segreto VALUES (1, 'shh')");
    ok("CREATE PROCEDURE leggi() SELECT nome FROM prodotto WHERE id = 1");
    ok("CREATE PROCEDURE azzera() UPDATE prodotto SET prezzo = 0");
    ok("CREATE PROCEDURE spia() BEGIN SELECT 1; SELECT x FROM segreto; END");
    ok("USE hr");
    ok("CREATE TABLE stipendio (id INT PRIMARY KEY, euro INT)");
    ok("USE shop");

    // gestione utenti (sessione root)
    CHECK(!db_has_users(), "no users at start");
    CHECK(ok("CREATE USER anna IDENTIFIED BY 'segreta1'"), "create user: %s", g_err);
    CHECK(!ok("CREATE USER anna IDENTIFIED BY 'altra123'") && strstr(g_err, "already exists"), "duplicate user");
    CHECK(ok("CREATE USER IF NOT EXISTS anna IDENTIFIED BY 'altra123'"), "if not exists");
    CHECK(!ok("CREATE USER bob IDENTIFIED BY 'ab'") && strstr(g_err, "4 characters"), "short password");
    CHECK(ok("CREATE USER bob IDENTIFIED BY 'bob-pass'"), "create bob");
    CHECK(ok("CREATE USER capo IDENTIFIED BY 'capo-pass'"), "create capo");
    CHECK(rows("SHOW USERS") == 3, "show users");
    CHECK(db_login("anna", "segreta1") && !db_login("anna", "sbagliata") && !db_login("nessuno", "x"), "login");
    CHECK(ok("GRANT SELECT ON shop.* TO anna"), "grant db: %s", g_err);
    CHECK(ok("GRANT INSERT, UPDATE ON ordine TO anna"), "grant table in current db: %s", g_err);
    CHECK(ok("REVOKE SELECT ON shop.* FROM anna") && ok("GRANT SELECT ON shop.* TO anna"), "revoke and grant again");
    CHECK(ok("GRANT SELECT ON shop.prodotto TO bob"), "bob: one table");
    CHECK(ok("GRANT ALL ON *.* TO capo WITH GRANT OPTION"), "admin");
    CHECK(!ok("GRANT FLY ON *.* TO anna") && strstr(g_err, "Expected privileges"), "bad privilege");
    CHECK(!ok("GRANT SELECT ON shop.* TO nessuno") && strstr(g_err, "does not exist"), "grant to unknown");
    CHECK(has_row("SHOW GRANTS FOR anna", "GRANT SELECT ON shop.* TO anna") &&
          has_row("SHOW GRANTS FOR anna", "INSERT, UPDATE ON shop.ordine"), "show grants");
    CHECK(has_row("SHOW GRANTS FOR capo", "WITH GRANT OPTION"), "admin grant shown");
    CHECK(db_user_grant("anna", "shop", "*") == DB_PRIV_SELECT && db_user_grant("anna", "shop", "ordine") == (DB_PRIV_INSERT | DB_PRIV_UPDATE), "grant api");

    // ── anna: SELECT su shop, INSERT/UPDATE su ordine ──
    CHECK(db_set_user("anna") && !strcmp(db_session_user(), "anna"), "session anna");
    CHECK(rows("SELECT * FROM prodotto") == 2, "anna reads: %s", g_err);
    CHECK(rows("SELECT p.nome, o.qta FROM ordine o JOIN prodotto p ON p.id = o.prodotto") == 1, "join");
    CHECK(ok("INSERT INTO ordine VALUES (2, 2, 1)"), "anna inserts orders: %s", g_err);
    CHECK(ok("UPDATE ordine SET qta = 5 WHERE id = 2"), "anna updates orders");
    CHECK(denied("DELETE FROM ordine WHERE id = 2"), "no delete: %s", g_err);
    CHECK(denied("INSERT INTO prodotto VALUES (3, 'x', 1)"), "no insert on prodotto");
    CHECK(denied("DROP TABLE segreto"), "no drop");
    CHECK(denied("CREATE TABLE nuova (id INT)"), "no create");
    CHECK(denied("ALTER TABLE prodotto ADD COLUMN c INT"), "no alter");
    CHECK(denied("CREATE USER x IDENTIFIED BY 'xxxx'") && denied("GRANT SELECT ON *.* TO anna"), "no user management");
    CHECK(has_row("SHOW GRANTS", "TO anna"), "own grants visible");
    CHECK(denied("SHOW GRANTS FOR bob"), "others' grants hidden");
    CHECK(denied("USE hr") && denied("CREATE DATABASE altro"), "other database");
    // query su più database: i permessi valgono su ogni database nominato
    CHECK(denied("SELECT * FROM hr.stipendio"), "qualified table of another database");
    CHECK(denied("SELECT * FROM prodotto p JOIN hr.stipendio s ON s.id = p.id"), "cross-database join");
    CHECK(denied("SELECT nome FROM prodotto WHERE id IN (SELECT id FROM hr.stipendio)"), "cross-database subquery");
    CHECK(denied("INSERT INTO ordine SELECT id, id, euro FROM hr.stipendio"), "cross-database INSERT ... SELECT");
    CHECK(rows("SELECT p.nome FROM shop.prodotto p JOIN ordine o ON o.prodotto = p.id") == 2, "qualified own database: %s", g_err);
    CHECK(rows("SHOW DATABASES") == 1 && has_row("SHOW DATABASES", "shop"), "SHOW DATABASES lists only allowed databases");
    CHECK(ok("USE shop") && ok("BEGIN") && ok("ROLLBACK"), "use, transactions");

    // ── bob: solo SELECT su shop.prodotto: le tabelle nascoste anche in subquery, join, CTE, UNION ──
    db_set_user("bob");
    CHECK(rows("SELECT * FROM prodotto") == 2, "bob reads prodotto");
    CHECK(denied("SELECT * FROM segreto"), "plain");
    CHECK(denied("SELECT nome FROM prodotto WHERE id IN (SELECT id FROM segreto)"), "subquery in WHERE");
    CHECK(denied("SELECT (SELECT x FROM segreto WHERE id = 1) FROM prodotto"), "scalar subquery");
    CHECK(denied("SELECT * FROM prodotto JOIN segreto ON segreto.id = prodotto.id"), "join");
    CHECK(denied("SELECT * FROM (SELECT * FROM segreto) s"), "derived table");
    CHECK(denied("WITH s AS (SELECT * FROM segreto) SELECT * FROM s"), "cte body");
    CHECK(rows("WITH p AS (SELECT * FROM prodotto) SELECT * FROM p") == 2, "cte over allowed table");
    CHECK(denied("SELECT id FROM prodotto UNION SELECT id FROM segreto"), "union branch");
    CHECK(denied("SELECT * FROM prodotto WHERE EXISTS (SELECT 1 FROM ordine)"), "exists");
    CHECK(denied("EXPLAIN SELECT * FROM segreto"), "explain");
    CHECK(denied("UPDATE prodotto SET prezzo = 0"), "bob cannot update");
    // lo schema delle tabelle senza permessi resta nascosto (DESCRIBE, SHOW, information_schema)
    CHECK(denied("DESCRIBE segreto") && denied("SHOW CREATE TABLE segreto") && rows("SHOW COLUMNS FROM segreto") <= 0,
          "DESCRIBE / SHOW CREATE / SHOW COLUMNS of a hidden table: %s", g_err);
    CHECK(rows("SELECT * FROM information_schema.columns WHERE table_name = 'segreto'") == 0 &&
          rows("SELECT * FROM information_schema.tables WHERE table_name = 'prodotto'") == 1, "information_schema lists only allowed tables");
    CHECK(has_row("SHOW TABLES", "prodotto") && !has_row("SHOW TABLES", "segreto"), "SHOW TABLES lists only allowed tables");
    // procedure: ogni istruzione con i permessi di chi chiama; nomi db.tabella controllati su quel database
    CHECK(rows("CALL leggi()") == 1, "bob calls a reading procedure: %s", g_err);
    CHECK(denied("CALL azzera()") && rows("SELECT * FROM prodotto WHERE prezzo = 0") == 0, "procedure cannot update for bob");
    CHECK(denied("CALL spia()"), "procedure cannot read a hidden table");
    CHECK(denied("CREATE PROCEDURE p2() SELECT 1") && denied("DROP PROCEDURE leggi"), "no procedure DDL");
    CHECK(rows("SELECT * FROM shop.prodotto") == 2 && denied("SELECT * FROM hr.stipendio") && denied("SELECT * FROM shop.segreto"), "qualified names");

    // ── P0.1: ogni forma dello statement passa dal controllo (eve: tutto su mine e mine2, niente su segreto) ──
    db_set_user(NULL);
    ok("USE shop");
    ok("CREATE TABLE mine (id INT PRIMARY KEY, x VARCHAR(20))");
    ok("CREATE TABLE mine2 (id INT PRIMARY KEY, x VARCHAR(20))");
    ok("INSERT INTO mine VALUES (1, 'a'), (2, 'b')");
    ok("INSERT INTO mine2 VALUES (1, 'c')");
    ok("CREATE VIEW v_seg AS SELECT * FROM segreto");
    ok("CREATE VIEW v_seg2 AS SELECT * FROM v_seg");
    CHECK(ok("CREATE USER eve IDENTIFIED BY 'eve-pass'") && ok("GRANT SELECT, INSERT, UPDATE, DELETE, ALTER ON shop.mine TO eve") &&
          ok("GRANT SELECT, INSERT, UPDATE, DELETE ON shop.mine2 TO eve") && ok("GRANT CREATE ON shop.* TO eve"), "eve: %s", g_err);
    db_set_user("eve");
    ok("USE shop");
    CHECK(rows("SELECT * FROM mine") == 2, "eve reads mine: %s", g_err);
    CHECK(denied("WITH segreto AS (SELECT * FROM segreto) SELECT * FROM segreto"), "CTE named like the hidden table: %s", g_err);
    CHECK(denied("SELECT * FROM mine WHERE id IN (WITH s AS (SELECT id FROM segreto) SELECT id FROM s)"), "CTE in a subquery");
    CHECK(denied("SET @x = (SELECT x FROM segreto LIMIT 1)"), "SET @x = (SELECT ...): %s", g_err);
    CHECK(denied("INSERT INTO mine VALUES (10, 'z') RETURNING (SELECT x FROM segreto LIMIT 1)") && rows("SELECT * FROM mine WHERE id = 10") == 0,
          "INSERT ... RETURNING (SELECT ...): %s", g_err);
    CHECK(denied("UPDATE mine m JOIN segreto s ON s.id = m.id SET m.x = s.x") && rows("SELECT * FROM mine WHERE x = 'shh'") == 0,
          "UPDATE ... JOIN hidden table: %s", g_err);
    CHECK(denied("DELETE mine, segreto FROM mine JOIN segreto ON segreto.id = mine.id"), "multi-table DELETE of a hidden table: %s", g_err);
    CHECK(denied("INSERT INTO mine VALUES (1, 'a') ON DUPLICATE KEY UPDATE x = (SELECT x FROM segreto LIMIT 1)"), "ON DUPLICATE KEY UPDATE (SELECT ...): %s", g_err);
    CHECK(denied("SELECT id, ROW_NUMBER() OVER (ORDER BY (SELECT x FROM segreto LIMIT 1)) FROM mine"), "subquery in a window: %s", g_err);
    CHECK(denied("CREATE TABLE copia AS SELECT * FROM segreto") && denied("SELECT * FROM copia") == false, "CTAS from a hidden table: %s", g_err);
    CHECK(denied("SELECT * FROM mine m, LATERAL (SELECT x FROM segreto s WHERE s.id = m.id) l"), "LATERAL: %s", g_err);
    CHECK(denied("MERGE INTO mine USING segreto s ON mine.id = s.id WHEN MATCHED THEN UPDATE SET x = s.x"), "MERGE USING hidden table: %s", g_err);
    CHECK(denied("SELECT * FROM v_seg2") && denied("CREATE VIEW ev AS SELECT * FROM v_seg"), "nested views: %s", g_err);
    // un trigger su una tabella propria non scrive con i permessi di qualcun altro
    CHECK(ok("CREATE TRIGGER tg_pwn AFTER UPDATE ON mine FOR EACH ROW UPDATE segreto SET x = 'pwned'"), "trigger created: %s", g_err);
    CHECK(denied("UPDATE mine SET x = 'q' WHERE id = 1"), "trigger body checked: %s", g_err);
    // niente rifiuti sbagliati: funzioni di tabella e alias
    CHECK(rows("SELECT jt.a FROM mine, JSON_TABLE('[{\"a\":1}]', '$[*]' COLUMNS (a INT PATH '$.a')) jt") == 2, "JSON_TABLE alias: %s", g_err);
    CHECK(rows("SELECT * FROM GENERATE_SERIES(1, 3) g") == 3, "GENERATE_SERIES: %s", g_err);
    CHECK(rows("SELECT * FROM (VALUES (1), (2)) v") == 2, "VALUES: %s", g_err);
    CHECK(ok("DELETE m FROM mine2 m JOIN mine s ON s.id = m.id WHERE m.id = 99"), "aliased multi-table DELETE: %s", g_err);
    db_set_user(NULL);
    ok("USE shop");
    CHECK(rows("SELECT * FROM segreto WHERE x = 'shh'") == 1, "hidden table untouched");
    CHECK(rows("SELECT * FROM copia") == -1, "CTAS table not created");
    ok("DROP TRIGGER tg_pwn");
    ok("GRANT SELECT ON shop.v_seg2 TO eve");
    db_set_user("eve");
    ok("USE shop");
    CHECK(rows("SELECT * FROM v_seg2") == 1, "a granted view reads with its creator's rights: %s", g_err);
    db_set_user(NULL);
    ok("USE shop");

    // ── capo: amministratore ──
    db_set_user("capo");
    CHECK(ok("USE hr") && rows("SELECT * FROM stipendio") == 0 && ok("CREATE TABLE t2 (id INT PRIMARY KEY)") && ok("DROP TABLE t2"), "admin can do anything");
    CHECK(ok("CREATE USER dora IDENTIFIED BY 'dora-pass'") && ok("GRANT CREATE, SELECT ON hr.* TO dora"), "admin manages users");
    CHECK(!ok("DROP USER capo") && strstr(g_err, "own user"), "cannot drop self");
    CHECK(!ok("REVOKE GRANT OPTION ON *.* FROM capo"), "cannot demote self");

    // dora: CREATE su hr, la tabella creata la può leggere (SELECT su hr.*)
    db_set_user("dora");
    CHECK(ok("USE hr") && ok("CREATE TABLE nota (id INT PRIMARY KEY)") && rows("SELECT * FROM nota") == 0, "create in own db: %s", g_err);
    CHECK(denied("INSERT INTO nota VALUES (1)"), "but no insert");
    CHECK(denied("DROP DATABASE hr"), "no drop database");

    // root di nuovo; utente eliminato: la sessione rimasta non vale più
    db_set_user(NULL);
    CHECK(!db_session_user()[0], "root session");
    CHECK(ok("USE shop") && ok("DROP USER bob") && !db_set_user("bob"), "drop user");
    CHECK(ok("DROP USER IF EXISTS bob") && !ok("DROP USER bob"), "drop if exists");
    CHECK(ok("ALTER USER anna IDENTIFIED BY 'nuova-pw'") && db_login("anna", "nuova-pw") && !db_login("anna", "segreta1"), "change password");

    // persistenza: utenti e permessi dopo il riavvio; il file non contiene le password in chiaro
    db_shutdown();
    FILE* f = fopen("/tmp/sv_auth_test/users.dat", "r");
    char buf[4096] = "";
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[n] = '\0';
    if (f) fclose(f);
    CHECK(n > 0 && !strstr(buf, "nuova-pw") && !strstr(buf, "dora-pass"), "no clear-text passwords");
    CHECK(db_init("/tmp/sv_auth_test"), "reopen");
    CHECK(db_login("anna", "nuova-pw") && db_login("dora", "dora-pass"), "logins after restart");
    db_set_user("anna");
    CHECK(ok("USE shop") && rows("SELECT * FROM ordine") == 2 && denied("DELETE FROM ordine"), "grants after restart");
    db_set_user(NULL);

    db_shutdown();
    if (system("rm -rf /tmp/sv_auth_test")) {}
    if (g_fail) { printf("auth tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d auth checks passed\n", g_tests, g_tests);
    return 0;
}
