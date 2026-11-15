// Fuzzer mutazionale (P3.3), senza libFuzzer: da compilare con -fsanitize=address,undefined (make fuzz).
//   ./fuzz TARGET SECONDI [SEED]       TARGET: sql | json | login | page | catalog | all
// Parte da input validi (seed qui sotto), li muta (bit, byte, inserimenti, cancellazioni, incroci, parole
// chiave) e li passa al parser. Un errore di memoria o un comportamento indefinito ferma tutto con il
// rapporto del sanitizer; l'input che l'ha causato è in fuzz-crash.bin.
#include "src/query/lexer.h"
#include "src/query/parser.h"
#include "src/server/json.h"
#include "src/server/mysql_wire.h"
#include "src/storage/page.h"
#include "src/table/tuple.h"
#include "src/catalog/schema.h"
#include "src/bridge/db_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t g_rng = 88172645463325252ULL;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static size_t rn(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static const char* const SQL_SEEDS[] = {
    "SELECT a, COUNT(*) FROM t WHERE b > 3 AND c LIKE 'x%' GROUP BY a HAVING COUNT(*) > 1 ORDER BY 2 DESC LIMIT 10",
    "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5) SELECT * FROM r",
    "INSERT INTO t (a, b) VALUES (1, 'x'), (2, X'00C0FF') ON DUPLICATE KEY UPDATE b = VALUES(b)",
    "UPDATE t a JOIN u b ON a.id = b.id SET a.x = b.y WHERE b.z IN (SELECT z FROM v)",
    "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, n VARCHAR(20) NOT NULL DEFAULT 'a', CHECK (id > 0), FOREIGN KEY (n) REFERENCES u(n) ON DELETE CASCADE)",
    "SELECT id, ROW_NUMBER() OVER (PARTITION BY a ORDER BY b ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) FROM t",
    "CREATE PROCEDURE p(IN x INT, OUT y INT) BEGIN DECLARE c CURSOR FOR SELECT 1; DECLARE CONTINUE HANDLER FOR NOT FOUND SET y = 0; WHILE x > 0 DO SET x = x - 1; END WHILE; END",
    "SELECT * FROM t, LATERAL (SELECT * FROM u WHERE u.a = t.a LIMIT 1) l, JSON_TABLE('[1,2]', '$[*]' COLUMNS (v INT PATH '$')) j",
    "MERGE INTO t USING s ON t.id = s.id WHEN MATCHED THEN UPDATE SET x = s.x WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.x)",
    "SET @a := 1, @b = (SELECT MAX(x) FROM t); SELECT CASE WHEN @a > 0 THEN 'p' ELSE 'n' END, CAST('1.5' AS DECIMAL(10,2))",
    "ALTER TABLE t ADD COLUMN c INT, DROP COLUMN d, RENAME COLUMN e TO f, ADD INDEX i (a, b)",
    "SELECT 'a\\0b\\Z', 0x41, b'101', _utf8mb4'x' COLLATE utf8mb4_bin, /* c */ 1 -- c\n",
};
static const char* const EXEC_SEEDS[] = {
    "SELECT t.id, a, b, COUNT(u.id) FROM t LEFT JOIN u ON u.t_id = t.id WHERE a >= 10 GROUP BY t.id, a, b ORDER BY 4 DESC, 1",
    "SELECT a, SUM(c) OVER (PARTITION BY a ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW), LAG(b) OVER (ORDER BY id) FROM t",
    "UPDATE t SET a = a + 1, b = CONCAT(b, '!') WHERE id IN (SELECT t_id FROM u WHERE x LIKE '%a%')",
    "DELETE FROM u WHERE t_id = 2 AND x <> 'z'",
    "INSERT INTO t VALUES (4, 40, 'w', 0.25, '2023-05-06', UNHEX('00FF')) ON DUPLICATE KEY UPDATE a = VALUES(a) + 1",
    "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 20) SELECT n, (SELECT b FROM t WHERE id = n) FROM r",
    "SELECT JSON_OBJECT('a', a, 'b', b), HEX(e), LENGTH(e), DATE_ADD(d, INTERVAL 1 MONTH), CAST(c AS DECIMAL(8,3)) FROM t",
    "BEGIN; UPDATE t SET c = c * 2; SAVEPOINT s; DELETE FROM u; ROLLBACK TO s; COMMIT",
    "SELECT * FROM v WHERE n > 1 UNION SELECT id, a FROM t EXCEPT SELECT 1, 1",
    "MERGE INTO t USING (SELECT 5 id, 50 a) s ON t.id = s.id WHEN MATCHED THEN UPDATE SET a = s.a WHEN NOT MATCHED THEN INSERT (id, a) VALUES (s.id, s.a)",
    "SELECT * FROM t, LATERAL (SELECT x FROM u WHERE u.t_id = t.id LIMIT 1) l, JSON_TABLE('[{\"k\":1}]', '$[*]' COLUMNS (k INT PATH '$.k')) j",
    "ALTER TABLE t ADD COLUMN g INT AS (a * 2) STORED; SELECT g FROM t; ALTER TABLE t DROP COLUMN g",
    "CREATE TRIGGER IF NOT EXISTS tr BEFORE INSERT ON u FOR EACH ROW SET NEW.x = UPPER(NEW.x); INSERT INTO u (t_id, x) VALUES (3, 'q')",
    "SELECT a, GROUP_CONCAT(b ORDER BY b SEPARATOR ','), ROUND(AVG(c), 2) FROM t GROUP BY a WITH ROLLUP",
};
static const char* const SQL_WORDS[] = { "SELECT", "FROM", "WHERE", "(", ")", ",", "'", "\"", "`", "--", "/*", "*/", "\\",
    "JOIN", "ON", "UNION", "WITH", "OVER", "CASE", "END", "BEGIN", "(SELECT", "NULL", "IN", "LIMIT", ";", "@", "@@", "?", ":x",
    "X'", "0x", "1e999", "-", "*", "JSON_TABLE(", "COLUMNS(", "PATH", "NESTED", "LATERAL", "RECURSIVE", "PARTITION BY" };
static const char* const JSON_SEEDS[] = {
    "{\"sql\":\"SELECT ?\",\"params\":[1,\"a\\u0000b\",null,true,-2.5e3],\"database\":\"t\",\"rows\":\"objects\"}",
    "[{\"a\":[1,{\"b\":\"\\ud83d\\ude00\"}]},\"\\\\\\\"\",123456789012345678901234567890]",
};

static void mutate(uint8_t* b, size_t* n, size_t cap, const char* const* words, int nwords, const uint8_t* other, size_t on) {
    int k = 1 + (int)rn(6);
    for (int m = 0; m < k; m++) {
        int op = (int)rn(7);
        if (op == 0 && *n) b[rn(*n)] ^= (uint8_t)(1u << rn(8));                         // bit
        else if (op == 1 && *n) b[rn(*n)] = (uint8_t)rnd();                              // byte
        else if (op == 2 && *n + 1 < cap) {                                              // inserisci byte
            size_t p = rn(*n + 1);
            memmove(b + p + 1, b + p, *n - p); b[p] = (uint8_t)rnd(); (*n)++;
        } else if (op == 3 && *n) {                                                      // cancella un tratto
            size_t p = rn(*n), l = 1 + rn(*n - p < 16 ? *n - p : 16);
            memmove(b + p, b + p + l, *n - p - l); *n -= l;
        } else if (op == 4 && words) {                                                   // parola chiave
            const char* w = words[rn((size_t)nwords)];
            size_t wl = strlen(w), p = rn(*n + 1);
            if (*n + wl + 1 < cap) { memmove(b + p + wl, b + p, *n - p); memcpy(b + p, w, wl); *n += wl; }
        } else if (op == 5 && other && on) {                                             // incrocio con un altro seed
            size_t p = rn(*n + 1), q = rn(on), l = 1 + rn(on - q);
            if (*n + l + 1 < cap) { memmove(b + p + l, b + p, *n - p); memcpy(b + p, other + q, l); *n += l; }
        } else if (op == 6 && *n > 1) {                                                  // duplica un tratto
            size_t p = rn(*n), l = 1 + rn(*n - p < 64 ? *n - p : 64);
            if (*n + l + 1 < cap) { memmove(b + p + l, b + p, *n - p); *n += l; }
        }
    }
}

static uint8_t g_buf[1 << 16];
static size_t g_n;

// il sanitizer sta per fermare il processo: l'input in fuzz-crash.bin
void __sanitizer_set_death_callback(void (*cb)(void)) __attribute__((weak));
static void save_crash(void) {
    FILE* f = fopen("fuzz-crash.bin", "wb");
    if (f) { fwrite(g_buf, 1, g_n, f); fclose(f); }
}

static void run_sql(void) {
    g_buf[g_n] = 0;
    TokenList* t = lexer_tokenize((const char*)g_buf);
    if (!t) return;
    ParseResult* r = parser_parse(t);
    if (r) parse_result_free(r);
    lexer_free(t);
}
static void run_json(void) {
    char err[128];
    JsonVal* v = json_parse((const char*)g_buf, g_n, err, sizeof(err));
    if (v) json_free(v);
}
static void run_login(void) {
    MwLogin lg;
    (void)mw_parse_login(g_buf, g_n, &lg);
}
static TableSchema g_ts;
static void run_page(void) {
    Page p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, g_buf, g_n < sizeof(p) ? g_n : sizeof(p));
    if (page_validate(&p, p.page_id)) {
        for (uint16_t s = 0; s < p.num_slots; s++) {
            uint16_t len = 0;
            const void* d = page_get_slot(&p, s, &len);
            if (d) { Tuple* tu = tuple_deserialize(d, len, &g_ts); if (tu) tuple_free(tu); }
        }
    }
    page_compact(&p);                                   // anche su una pagina danneggiata: niente scritture fuori
    Tuple* tu = tuple_deserialize(g_buf, g_n, &g_ts);
    if (tu) tuple_free(tu);
}
static void run_catalog(void) {
    FILE* f = fopen("fuzz-catalog.cat", "wb");
    if (!f) return;
    fwrite(g_buf, 1, g_n, f);
    fclose(f);
    Catalog* c = catalog_load("fuzz-catalog.cat");
    if (c) catalog_free(c);
}

// il motore vero: istruzioni mutate su un database di prova (tabelle, indici, viste, trigger, routine)
static const char* const EXEC_SETUP[] = {
    "CREATE DATABASE IF NOT EXISTS f", "USE f",
    "CREATE TABLE IF NOT EXISTS t (id INT PRIMARY KEY, a INT, b VARCHAR(30), c DOUBLE, d DATE, e BLOB)",
    "CREATE TABLE IF NOT EXISTS u (id INT PRIMARY KEY AUTO_INCREMENT, t_id INT, x TEXT, FOREIGN KEY (t_id) REFERENCES t(id) ON DELETE CASCADE)",
    "CREATE INDEX IF NOT EXISTS ta ON t (a)",
    "INSERT IGNORE INTO t VALUES (1, 10, 'x', 1.5, '2025-01-02', X'00C0'), (2, 20, 'y', NULL, NULL, NULL), (3, 10, 'zz', -2, '2024-12-31', 'b')",
    "INSERT IGNORE INTO u (t_id, x) VALUES (1, 'a'), (2, 'b'), (1, 'c')",
    "CREATE OR REPLACE VIEW v AS SELECT a, COUNT(*) n FROM t GROUP BY a",
};
static int g_exec_ready, g_exec_count;
static void exec_setup(void) {
    for (size_t i = 0; i < sizeof(EXEC_SETUP) / sizeof(EXEC_SETUP[0]); i++) db_result_free(db_execute(EXEC_SETUP[i]));
}
static void run_exec(void) {
    if (!g_exec_ready) {
        if (system("rm -rf fuzz-db && mkdir -p fuzz-db") != 0 || !db_init("fuzz-db")) { printf("cannot init the fuzz database\n"); exit(1); }
        db_set_synchronous("OFF");
        exec_setup();
        g_exec_ready = 1;
    }
    if (++g_exec_count % 500 == 0) {                         // le tabelle eliminate tornano; niente transazioni appese
        db_result_free(db_execute("ROLLBACK"));
        exec_setup();
    }
    g_buf[g_n] = 0;
    db_result_free(db_execute((const char*)g_buf));
}

typedef struct { const char* name; void (*run)(void); size_t maxlen; } Target;

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "all";
    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(save_crash);
    double secs = argc > 2 ? atof(argv[2]) : 10;
    if (argc > 3) g_rng = strtoull(argv[3], NULL, 10) | 1;
    // schema per decodificare le righe: un po' di tutti i tipi
    snprintf(g_ts.name, sizeof(g_ts.name), "f");
    DataType ty[6] = { SV_TYPE_INT, SV_TYPE_BIGINT, SV_TYPE_VARCHAR, SV_TYPE_FLOAT, SV_TYPE_BOOL, SV_TYPE_TEXT };
    for (int i = 0; i < 6; i++) { g_ts.columns[i].type = ty[i]; snprintf(g_ts.columns[i].name, 64, "c%d", i); g_ts.columns[i].nullable = true; }
    g_ts.num_columns = 6;
    // seed binari: una pagina valida con due righe, un catalogo vero, un login
    static uint8_t page_seed[4096], cat_seed[1 << 16], login_seed[128];
    size_t page_n = 0, cat_n = 0, login_n = 0;
    {
        Page* p = page_create(7);
        Tuple* tu = tuple_create(6);
        tu->values[0].int_val = 5; tu->values[1].bigint_val = 9; tu->values[2].str_val = "abc"; tu->values[3].float_val = 1.5;
        tu->values[4].bool_val = true; tu->values[5].is_null = true;
        size_t rl = 0;
        uint8_t* row = tuple_serialize(tu, &g_ts, &rl);
        page_insert(p, row, (uint16_t)rl);
        page_insert(p, row, (uint16_t)rl);
        memcpy(page_seed, p, 4096);
        page_n = 4096;
        free(row);
        free(tu->values); free(tu);
        page_free(p);
        Catalog* c = catalog_create();
        catalog_add_db(c, "d");
        catalog_add_table(c, "d", &g_ts);
        catalog_save(c, "fuzz-catalog.cat");
        catalog_free(c);
        FILE* f = fopen("fuzz-catalog.cat", "rb");
        if (f) { cat_n = fread(cat_seed, 1, sizeof(cat_seed), f); fclose(f); }
        uint32_t caps = 0x200 | 0x8000 | 0x80000 | 8;
        memcpy(login_seed, &caps, 4);
        login_n = 32;
        memcpy(login_seed + login_n, "root", 5); login_n += 5;
        login_seed[login_n++] = 20; memset(login_seed + login_n, 7, 20); login_n += 20;
        memcpy(login_seed + login_n, "db", 3); login_n += 3;
        memcpy(login_seed + login_n, "mysql_native_password", 22); login_n += 22;
    }
    Target T[] = { { "sql", run_sql, 8000 }, { "json", run_json, 8000 }, { "login", run_login, 512 },
                   { "page", run_page, 4096 }, { "catalog", run_catalog, sizeof(cat_seed) }, { "exec", run_exec, 4000 } };
    int nt = (int)(sizeof(T) / sizeof(T[0]));
    for (int ti = 0; ti < nt; ti++) {
        if (strcmp(which, "all") && strcmp(which, T[ti].name)) continue;
        double t0 = (double)clock() / CLOCKS_PER_SEC, per = !strcmp(which, "all") ? secs / nt : secs;
        long iters = 0;
        const bool text = ti < 2 || ti == 5;
        while ((double)clock() / CLOCKS_PER_SEC - t0 < per) {
            const uint8_t* seed; size_t sn; const uint8_t* other = NULL; size_t on = 0;
            if (ti == 5) { int k = (int)rn(sizeof(EXEC_SEEDS) / sizeof(EXEC_SEEDS[0])); seed = (const uint8_t*)EXEC_SEEDS[k]; sn = strlen(EXEC_SEEDS[k]);
                           int o = (int)rn(sizeof(EXEC_SEEDS) / sizeof(EXEC_SEEDS[0])); other = (const uint8_t*)EXEC_SEEDS[o]; on = strlen(EXEC_SEEDS[o]); }
            else if (ti == 0) { int k = (int)rn(sizeof(SQL_SEEDS) / sizeof(SQL_SEEDS[0])); seed = (const uint8_t*)SQL_SEEDS[k]; sn = strlen(SQL_SEEDS[k]);
                           int o = (int)rn(sizeof(SQL_SEEDS) / sizeof(SQL_SEEDS[0])); other = (const uint8_t*)SQL_SEEDS[o]; on = strlen(SQL_SEEDS[o]); }
            else if (ti == 1) { int k = (int)rn(2); seed = (const uint8_t*)JSON_SEEDS[k]; sn = strlen(JSON_SEEDS[k]); other = (const uint8_t*)JSON_SEEDS[1 - k]; on = strlen(JSON_SEEDS[1 - k]); }
            else if (ti == 2) { seed = login_seed; sn = login_n; }
            else if (ti == 3) { seed = page_seed; sn = page_n; }
            else { seed = cat_seed; sn = cat_n; }
            if (sn > T[ti].maxlen) sn = T[ti].maxlen;
            memcpy(g_buf, seed, sn);
            g_n = sn;
            int rounds = 1 + (int)rn(4);
            for (int r = 0; r < rounds; r++)
                mutate(g_buf, &g_n, T[ti].maxlen < sizeof(g_buf) - 1 ? T[ti].maxlen : sizeof(g_buf) - 1,
                       text && ti != 1 ? SQL_WORDS : NULL, (int)(sizeof(SQL_WORDS) / sizeof(SQL_WORDS[0])), other, on);
            T[ti].run();
            iters++;
        }
        printf("fuzz %-8s %ld inputs in %.0f s: no sanitizer error\n", T[ti].name, iters, per);
    }
    remove("fuzz-crash.bin");
    remove("fuzz-catalog.cat");
    remove("fuzz-catalog.cat.tmp");
    if (g_exec_ready) { db_shutdown(); if (system("rm -rf fuzz-db")) {} }
    return 0;
}
