// Fuzzer mutazionale (P3.3), senza libFuzzer: da compilare con -fsanitize=address,undefined (make fuzz).
//   ./fuzz TARGET SECONDI [SEED]       TARGET: sql | json | login | page | catalog | exec | http | h2 | smtp |
//                                       htaccess | regex | sqljson | csv | diff | all
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
#include "src/server/server.h"
#include "src/server/h2.h"
#include "src/server/mail.h"
#include "src/server/rewrite.h"
#include "src/query/regex.h"
#include "src/query/sqljson.h"
#include "src/widgets/csv_import.h"
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec / 1e9; }

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
static TableSchema* g_tsp;                 // schema per decodificare le righe (schema_new: colonne nel blocco)
#define g_ts (*g_tsp)
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
static void engine_ready(void) {
    if (g_exec_ready) return;
    if (system("rm -rf fuzz-db && mkdir -p fuzz-db") != 0 || !db_init("fuzz-db")) { printf("cannot init the fuzz database\n"); exit(1); }
    db_set_synchronous("OFF");
    exec_setup();
    g_exec_ready = 1;
}
static void run_exec(void) {
    engine_ready();
    if (++g_exec_count % 500 == 0) {                         // le tabelle eliminate tornano; niente transazioni appese
        db_result_free(db_execute("ROLLBACK"));
        exec_setup();
    }
    g_buf[g_n] = 0;
    db_result_free(db_execute((const char*)g_buf));
}

// ── HTTP: il server vero nel processo (API, file statici, .htaccess); ogni input è una connessione ──
static const char* const HTTP_SEEDS[] = {
    "GET /index.html HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-3,5-\r\nIf-None-Match: \"x\"\r\nAccept-Encoding: gzip\r\n\r\n",
    "POST /api/v1/query HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
    "1a\r\n{\"sql\":\"SELECT 1 AS x\",\r\n9\r\n\"rows\":1}\r\n0\r\nX-T: 1\r\n\r\n",
    "POST /api/v1/query?database=f&rows=objects&max_rows=1 HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: 22\r\n\r\nSELECT * FROM t; SELECT",
    "GET /old/page?a=1&b=%2F%00 HTTP/1.0\r\nHost: blog.localhost:80\r\nCookie: a=b; c\r\nAuthorization: Basic Zm9vOmJhcg==\r\n\r\n",
    "OPTIONS /api/v1/sessions HTTP/1.1\r\nHost: localhost\r\nOrigin: http://x\r\nAccess-Control-Request-Method: POST\r\n\r\n"
    "GET /api/v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\nGET /dir/ HTTP/1.1\r\n\r\n",
    "GET /../../etc/passwd%00.html HTTP/1.1\r\nHost: [::1]\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n",
};
static const char* const HTTP_WORDS[] = { "\r\n", "\r\n\r\n", ": ", "%", "%00", "%2e%2e/", "/../", "Content-Length: ",
    "Transfer-Encoding: chunked", "Host: ", "HTTP/1.1", "HTTP/1.0", "GET ", "POST ", "0\r\n\r\n", "ffffffff\r\n", ";ext=1",
    "Range: bytes=", "-", ",", "?", "&", "=", "#", " ", "\t", "\x80", "\xff", "Expect: 100-continue", "Connection: keep-alive" };

static SvServer* g_http;
static int g_http_port;
static void* http_thread(void* a) { sv_server_run(a); return NULL; }
static void fz_put(const char* path, const char* text) { FILE* f = fopen(path, "w"); if (f) { fputs(text, f); fclose(f); } }

static void engine_ready(void);
static void http_setup(void) {
    engine_ready();
    if (system("rm -rf fuzz-www && mkdir -p fuzz-www/dir fuzz-www/old") != 0) exit(1);
    fz_put("fuzz-www/index.html", "<p>hello</p>\n");
    fz_put("fuzz-www/dir/a.txt", "abcdefghij");
    fz_put("fuzz-www/.htaccess", "RewriteEngine On\nRewriteCond %{REQUEST_FILENAME} !-f\nRewriteRule ^old/(.*)$ /index.html?p=$1 [QSA,L]\n"
                                 "<FilesMatch \"\\.(log|bak)$\">\nRequire all denied\n</FilesMatch>\nOptions -Indexes\n");
    static SvServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.quiet = true; cfg.static_dir = "fuzz-www"; cfg.default_db = "f"; cfg.timeout_ms = 2000; cfg.max_body = 1 << 20;
    char err[256];
    g_http = sv_server_start(&cfg, err, sizeof(err));
    if (!g_http) { printf("http server: %s\n", err); exit(1); }
    g_http_port = sv_server_port(g_http);
    pthread_t th;
    pthread_create(&th, NULL, http_thread, g_http);
    pthread_detach(th);
}

static void run_http(void) {
    if (!g_http) http_setup();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)g_http_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return; }
    (void)!send(fd, g_buf, g_n, MSG_NOSIGNAL);
    shutdown(fd, SHUT_WR);                        // fine dell'input: il server non aspetta altri byte
    char tmp[16384];
    while (recv(fd, tmp, sizeof(tmp), 0) > 0) {}
    close(fd);
}

// ── HTTP/2: frame e HPACK dal client, senza TLS (H2Io su una socketpair); gli stream vanno al server HTTP ──
static int h2_recv(void* ssl, char* buf, int n) {
    int fd = (int)(intptr_t)ssl;
    ssize_t g = recv(fd, buf, (size_t)n, MSG_DONTWAIT);
    if (g > 0) return (int)g;
    if (g < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}
static bool h2_send(void* ssl, tls_sock fd, const char* p, size_t n) { (void)ssl; (void)fd; (void)p; (void)n; return true; }
static int  h2_pending(void* ssl) { (void)ssl; return 0; }
static uint8_t g_h2_seed[512];
static size_t g_h2_seed_n;
static void h2_seed(void) {
    uint8_t* p = g_h2_seed;
    size_t k = 0;
    memcpy(p, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24); k = 24;
    uint8_t settings[] = { 0, 0, 6, 0x4, 0, 0, 0, 0, 0, 0, 0x3, 0, 0, 0, 100 };          // SETTINGS max streams
    memcpy(p + k, settings, sizeof(settings)); k += sizeof(settings);
    // HEADERS stream 1, END_STREAM | END_HEADERS: :method GET, :scheme https, :path /, :authority localhost
    uint8_t hb[] = { 0x82, 0x87, 0x84, 0x41, 0x09, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0x40, 0x03, 'x', '-', 'a', 0x01, 'b' };
    uint8_t fh[9] = { 0, 0, (uint8_t)sizeof(hb), 0x1, 0x5, 0, 0, 0, 1 };
    memcpy(p + k, fh, 9); k += 9;
    memcpy(p + k, hb, sizeof(hb)); k += sizeof(hb);
    uint8_t ping[] = { 0, 0, 8, 0x6, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    memcpy(p + k, ping, sizeof(ping)); k += sizeof(ping);
    uint8_t wu[] = { 0, 0, 4, 0x8, 0, 0, 0, 0, 1, 0, 0, 0x10, 0 };                      // WINDOW_UPDATE stream 1
    memcpy(p + k, wu, sizeof(wu)); k += sizeof(wu);
    g_h2_seed_n = k;
}
static void run_h2(void) {
    if (!g_http) http_setup();
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return;
    (void)!write(sp[0], g_buf, g_n);
    shutdown(sp[0], SHUT_WR);
    H2Io io = { (void*)(intptr_t)sp[1], sp[1], h2_recv, h2_send, h2_pending };
    atomic_int stop = 0;
    h2_serve(&io, g_http_port, "", &stop);
    close(sp[0]);
    close(sp[1]);
}

// ── SMTP e MIME: una sessione intera, poi le pagine del messaggio come le chiede il pannello ──
static MailStore* g_mail;
static const char* const SMTP_SEEDS[] = {
    "EHLO x\r\nMAIL FROM:<a@b.c>\r\nRCPT TO:<d@e.f>\r\nDATA\r\nSubject: =?UTF-8?B?w6g=?=\r\nMIME-Version: 1.0\r\n"
    "Content-Type: multipart/alternative; boundary=\"zz\"\r\n\r\n--zz\r\nContent-Type: text/plain; charset=utf-8\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n\r\nci=C3=A0o\r\n--zz\r\nContent-Type: text/html\r\n"
    "Content-Transfer-Encoding: base64\r\n\r\nPGI+aGk8L2I+\r\n--zz\r\nContent-Type: image/png; name=a.png\r\n"
    "Content-Disposition: attachment; filename=\"a.png\"\r\nContent-Transfer-Encoding: base64\r\n\r\niVBORw0KGgo=\r\n--zz--\r\n.\r\nQUIT\r\n",
    "HELO y\r\nMAIL FROM:<>\r\nRCPT TO:<x@y>\r\nBDAT 5 LAST\r\nhello\r\nRSET\r\nNOOP\r\nAUTH PLAIN AGZvbwBiYXI=\r\nQUIT\r\n",
};
static void run_smtp(void) {
    if (!g_mail) {
        if (system("rm -rf fuzz-mail && mkdir -p fuzz-mail") != 0) exit(1);
        char err[200];
        g_mail = mail_store_open("fuzz-mail", err, sizeof(err));
        if (!g_mail) { printf("mail store: %s\n", err); exit(1); }
    }
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return;
    int fl = fcntl(sp[0], F_GETFL, 0);
    fcntl(sp[0], F_SETFL, fl | O_NONBLOCK);
    (void)!write(sp[0], g_buf, g_n);
    shutdown(sp[0], SHUT_WR);
    struct timeval tv = { 1, 0 };
    setsockopt(sp[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    atomic_int stop = 0;
    mail_smtp_serve(g_mail, sp[1], "127.0.0.1", &stop);
    char tmp[4096];
    while (read(sp[0], tmp, sizeof(tmp)) > 0) {}
    close(sp[0]);
    close(sp[1]);
    static const char* const SUB[] = { "/api/messages", "/api/messages/latest", "/api/messages/latest/html", "/api/messages/latest/raw",
                                       "/api/messages/latest/text", "/api/messages/latest/parts/0", "/api/messages/latest/parts/2" };
    for (size_t i = 0; i < sizeof(SUB) / sizeof(SUB[0]); i++) {
        JsonBuf out = {0};
        const char* ct = NULL; const char* xh = NULL;
        mail_web(g_mail, "GET", SUB[i], "q=a&limit=3", &out, &ct, &xh);
        free(out.data);
    }
    if (mail_count(g_mail) > 50) { JsonBuf out = {0}; const char* ct = NULL; const char* xh = NULL; mail_web(g_mail, "DELETE", "/api/messages", "", &out, &ct, &xh); free(out.data); }
}

// ── .htaccess: regole mutate su richieste fisse ──
static const char* const HTA_SEEDS[] = {
    "RewriteEngine On\nRewriteBase /app/\nRewriteCond %{HTTP_HOST} ^www\\.(.*)$ [NC]\nRewriteRule ^(.*)$ https://%1/$1 [R=301,L]\n"
    "RewriteCond %{REQUEST_FILENAME} !-f\nRewriteCond %{REQUEST_FILENAME} !-d\nRewriteRule ^(.+)$ index.php?q=$1 [QSA,L,B]\n",
    "<Files \"secret.txt\">\nRequire all denied\n</Files>\nOptions -Indexes +FollowSymLinks\nRewriteEngine on\n"
    "RewriteCond %{QUERY_STRING} (^|&)id=([0-9]+) [OR]\nRewriteCond %{HTTP:X-Forwarded-Proto} =https\nRewriteRule ^p/([a-z]+)/?$ /x/%2/$1 [S=1,C,NE]\n"
    "RewriteRule .* - [F]\nRewriteRule ^gone$ - [G]\n",
};
static void run_htaccess(void) {
    static bool ready;
    if (!ready) { if (system("rm -rf fuzz-hta && mkdir -p fuzz-hta/app/sub") != 0) exit(1); fz_put("fuzz-hta/app/real.txt", "x"); ready = true; }
    FILE* f = fopen("fuzz-hta/app/.htaccess", "wb");
    if (!f) return;
    fwrite(g_buf, 1, g_n, f);
    fclose(f);
    static const char* const PATHS[] = { "/app/real.txt", "/app/p/abc/", "/app/sub/deep/file", "/app/", "/app/gone", "/app/secret.txt" };
    static RwResult out;
    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        RwRequest rq = { "fuzz-hta", PATHS[i], "id=42&x=%2F", "GET", "www.example.localhost", "X-Forwarded-Proto: https\r\nCookie: a=b\r\n",
                         "127.0.0.1", "GET /app/ HTTP/1.1", 80, i % 2 == 0 };
        rw_apply(&rq, &out);
    }
}

// ── regex e SQL/JSON: pattern / documento mutati contro testi fissi ──
static const char* const RX_SEEDS[] = { "^(a|b)*?c{2,5}[^x-z]\\d+(?:foo|bar)$", "([[:alpha:]]+)@(\\w+\\.)+[a-z]{2,}", "(a*)*b|.{0,3}\\1" };
static void run_regex(void) {
    g_buf[g_n] = 0;
    char err[128];
    SvRegex* re = sv_regex_compile((const char*)g_buf, rnd() & 1, err, sizeof(err));
    if (!re) return;
    static const char* const SUBJ[] = { "aabcc123foo", "x@y.example.com", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!", "", "\xc3\xa8\xe2\x82\xac b" };
    for (size_t i = 0; i < sizeof(SUBJ) / sizeof(SUBJ[0]); i++) {
        size_t st, en, caps[10][2];
        sv_regex_match(re, SUBJ[i]);
        sv_regex_search(re, SUBJ[i], 0, &st, &en);
        sv_regex_search_caps(re, SUBJ[i], 0, caps, 10);
    }
    sv_regex_free(re);
}
// CSV importato dall'app (P3.9): file che arrivano da fuori
static const char* const CSV_SEEDS[] = {
    "id,nome,prezzo\n1,\"Rossi, Mario\",12.5\n2,\"a \"\"b\"\"\",-3\n3,,1e5\n",
    "\xef\xbb\xbf" "col;Città;x\r\n\"multi\nline\";è;+7\r\n",
};
static void run_csv(void) {
    g_buf[g_n] = 0;
    CsvImportInfo info;
    char* sql = csv_to_sql((const char*)g_buf, "imp", true, 0, &info);
    free(sql);
    sql = csv_to_sql((const char*)g_buf, "imp", false, ';', &info);
    free(sql);
    char name[64];
    csv_table_name_from_path((const char*)g_buf, name, sizeof(name));
}
static const char* const SJ_SEEDS[] = {
    "{\"a\": [1, 2.5e3, {\"b\": null, \"c\": \"\\u00e8\\n\"}], \"d\": true, \"e\": {\"f\": [[]]}}",
    "[{\"k\": \"v\"}, -0, 1e-400, \"\\ud83d\\ude00\", false]",
};
static void run_sqljson(void) {
    g_buf[g_n] = 0;
    const char* doc = (const char*)g_buf;
    static const char* const PATHS[] = { "$", "$.a[1]", "$.a[*].b", "$**.c", "$.e.f[0][last]", "$[1 to 3]", "lax $.a" , "$.\"d\"" };
    sj_valid(doc);
    size_t dl = strlen(doc);
    sj_type(doc, dl); sj_length(doc, dl); sj_depth(doc);
    char* k = sj_keys(doc, dl); free(k);
    char* u = sj_unquote(doc, dl); free(u);
    char* q = sj_quote(doc); free(q);
    char* pr = sj_pretty(doc); free(pr);
    char* se = sj_search(doc, true, "%e%"); free(se);
    char* mp = sj_merge_patch(doc, "{\"a\": null, \"z\": [1]}"); free(mp);
    sj_contains(doc, dl, "{\"d\": true}", 11);
    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        const char* st; size_t len;
        sj_extract(doc, PATHS[i], &st, &len);
        for (int op = 0; op < 4; op++) { char* m = sj_modify(doc, PATHS[i], "[1, \"x\"]", op); free(m); }
    }
}

// ── differenziale: SQL casuale (il sottoinsieme comune) su SegVault e su SQLite, stessi risultati ──
static sqlite3* g_lite;
static long g_diff_checked, g_diff_found;
static void gen_expr(char* o, size_t n, int depth) {
    static const char* const COL[] = { "a", "b", "c" };
    int k = depth > 2 ? (int)rn(2) : (int)rn(9);
    char x[600], y[600];
    switch (k) {
        case 0: snprintf(o, n, "%s", COL[rn(3)]); break;
        case 1: snprintf(o, n, "%d", (int)rn(21) - 10); break;
        case 2: gen_expr(x, sizeof(x), depth + 1); gen_expr(y, sizeof(y), depth + 1); snprintf(o, n, "(%s %s %s)", x, (const char*[]){ "+", "-", "*" }[rn(3)], y); break;
        case 3: gen_expr(x, sizeof(x), depth + 1); snprintf(o, n, "ABS(%s)", x); break;
        case 4: gen_expr(x, sizeof(x), depth + 1); gen_expr(y, sizeof(y), depth + 1); snprintf(o, n, "COALESCE(%s, %s)", x, y); break;
        case 5: gen_expr(x, sizeof(x), depth + 1); snprintf(o, n, "CASE WHEN %s > 0 THEN %s ELSE %d END", x, COL[rn(3)], (int)rn(5)); break;
        case 6: gen_expr(x, sizeof(x), depth + 1); snprintf(o, n, "(-%s)", x); break;
        case 7: snprintf(o, n, "NULL"); break;
        default: gen_expr(x, sizeof(x), depth + 1); gen_expr(y, sizeof(y), depth + 1); snprintf(o, n, "NULLIF(%s, %s)", x, y); break;
    }
}
static void gen_cond(char* o, size_t n, int depth) {
    char x[700], y[700];
    int k = depth > 1 ? (int)rn(4) : (int)rn(8);
    switch (k) {
        case 0: case 1: gen_expr(x, sizeof(x), 1); gen_expr(y, sizeof(y), 1);
                snprintf(o, n, "%s %s %s", x, (const char*[]){ "=", "<", ">", "<=", ">=", "<>" }[rn(6)], y); break;
        case 2: gen_expr(x, sizeof(x), 1); snprintf(o, n, "%s IS %sNULL", x, rn(2) ? "NOT " : ""); break;
        case 3: gen_expr(x, sizeof(x), 1); snprintf(o, n, "%s %sBETWEEN %d AND %d", x, rn(2) ? "NOT " : "", (int)rn(10) - 5, (int)rn(10)); break;
        case 4: gen_cond(x, sizeof(x), depth + 1); gen_cond(y, sizeof(y), depth + 1); snprintf(o, n, "(%s) %s (%s)", x, rn(2) ? "AND" : "OR", y); break;
        case 5: gen_cond(x, sizeof(x), depth + 1); snprintf(o, n, "NOT (%s)", x); break;
        case 6: gen_expr(x, sizeof(x), 1); snprintf(o, n, "%s %sIN (%d, %d, NULL, %d)", x, rn(2) ? "NOT " : "", (int)rn(7) - 3, (int)rn(7), (int)rn(3)); break;
        default: snprintf(o, n, "EXISTS (SELECT 1 FROM dt2 WHERE dt2.k = dt.a %s)", rn(2) ? "AND dt2.v > 2" : ""); break;
    }
}
static int lite_cb(void* acc, int nc, char** v, char** c) {
    (void)c;
    char* s = acc;
    size_t l = strlen(s);
    for (int i = 0; i < nc && l < 60000; i++) l += (size_t)snprintf(s + l, 65536 - l, "%s%s", i ? "|" : "", v[i] ? v[i] : "NULL");
    if (l < 65000) { s[l++] = '\n'; s[l] = 0; }
    return 0;
}
static int cmp_lines(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static void sort_lines(char* s) {                   // multiinsieme di righe: l'ordine non conta
    char* v[4096]; int n = 0;
    for (char* p = strtok(s, "\n"); p && n < 4096; p = strtok(NULL, "\n")) v[n++] = p;
    qsort(v, (size_t)n, sizeof(char*), cmp_lines);
    static char t[65536];
    size_t l = 0;
    for (int i = 0; i < n && l < 65000; i++) l += (size_t)snprintf(t + l, sizeof(t) - l, "%s\n", v[i]);
    t[l] = 0;
    memcpy(s, t, l + 1);
}
static void run_diff(void) {
    if (!g_lite) {
        engine_ready();
        if (sqlite3_open(":memory:", &g_lite) != SQLITE_OK) exit(1);
        const char* setup[] = { "DROP TABLE IF EXISTS dt", "DROP TABLE IF EXISTS dt2",
                                "CREATE TABLE dt (id INT PRIMARY KEY, a INT, b INT, c INT)", "CREATE TABLE dt2 (k INT, v INT)" };
        for (int i = 0; i < 4; i++) { db_result_free(db_execute(setup[i])); sqlite3_exec(g_lite, setup[i], NULL, NULL, NULL); }
        char q[200];
        for (int i = 1; i <= 40; i++) {                 // a: NULL ogni 7 righe, b: NULL ogni 5, c senza NULL
            char av[16], bv[16];
            if (i % 7) snprintf(av, sizeof(av), "%d", i % 9 - 4); else snprintf(av, sizeof(av), "NULL");
            if (i % 5) snprintf(bv, sizeof(bv), "%d", i % 3); else snprintf(bv, sizeof(bv), "NULL");
            snprintf(q, sizeof(q), "INSERT INTO dt VALUES (%d, %s, %s, %d)", i, av, bv, (i * 37) % 11 - 5);
            db_result_free(db_execute(q)); sqlite3_exec(g_lite, q, NULL, NULL, NULL);
            snprintf(q, sizeof(q), "INSERT INTO dt2 VALUES (%d, %d)", i % 6 - 2, i % 4);
            db_result_free(db_execute(q)); sqlite3_exec(g_lite, q, NULL, NULL, NULL);
        }
    }
    char cond[2000], e1[700], e2[700], sql[4200];
    gen_cond(cond, sizeof(cond), 0);
    gen_expr(e1, sizeof(e1), 1);
    gen_expr(e2, sizeof(e2), 1);
    int form = (int)rn(3);
    if (form == 0) snprintf(sql, sizeof(sql), "SELECT id, %s, %s FROM dt WHERE %s", e1, e2, cond);
    else if (form == 1) snprintf(sql, sizeof(sql), "SELECT %s, COUNT(*), SUM(%s), MIN(%s), MAX(c) FROM dt WHERE %s GROUP BY %s", "b", e1, e2, cond, "b");
    else snprintf(sql, sizeof(sql), "SELECT COUNT(*), SUM(CASE WHEN %s THEN 1 ELSE 0 END) FROM dt", cond);
    static char ra[65536], rb[65536];
    ra[0] = rb[0] = 0;
    QueryResult* r = db_execute(sql);
    char* err = NULL;
    int lr = sqlite3_exec(g_lite, sql, lite_cb, rb, &err);
    if (!r || !r->success || lr != SQLITE_OK) {                       // uno dei due rifiuta: non si confronta
        db_result_free(r);
        sqlite3_free(err);
        return;
    }
    size_t l = 0;
    for (int i = 0; i < r->num_rows && l < 60000; i++) {
        for (int j = 0; j < r->num_cols; j++) {
            const Cell* c = &r->rows[i].cells[j];
            l += (size_t)snprintf(ra + l, sizeof(ra) - l, "%s%s", j ? "|" : "", c->is_null ? "NULL" : c->value);
        }
        ra[l++] = '\n'; ra[l] = 0;
    }
    db_result_free(r);
    sort_lines(ra);
    sort_lines(rb);
    g_diff_checked++;
    if (strcmp(ra, rb)) {
        g_diff_found++;
        if (g_diff_found <= 5) printf("DIFF %s\n  segvault: %.300s\n  sqlite:   %.300s\n", sql, ra, rb);
        FILE* f = fopen("fuzz-diff.txt", "a");
        if (f) { fprintf(f, "%s;\n-- segvault:\n%s-- sqlite:\n%s\n", sql, ra, rb); fclose(f); }
    }
}

typedef struct {
    const char* name;
    void (*run)(void);
    size_t maxlen;
    const char* const* seeds;  int nseeds;           // seed di testo, oppure
    const uint8_t* bseed;      size_t bseed_n;        // un seed binario
    const char* const* words;  int nwords;            // parole da inserire (NULL = solo mutazioni di byte)
    bool generated;                                   // l'input lo genera il bersaglio (diff)
} Target;
#define NS(a) (const char* const*)(a), (int)(sizeof(a) / sizeof((a)[0]))

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "all";
    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(save_crash);
    double secs = argc > 2 ? atof(argv[2]) : 10;
    if (argc > 3) g_rng = strtoull(argv[3], NULL, 10) | 1;
    // schema per decodificare le righe: un po' di tutti i tipi
    g_tsp = schema_new(6);
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
    h2_seed();
    Target T[] = {
        { "sql",      run_sql,      8000, NS(SQL_SEEDS),  NULL, 0, NS(SQL_WORDS), false },
        { "json",     run_json,     8000, NS(JSON_SEEDS), NULL, 0, NULL, 0, false },
        { "login",    run_login,    512,  NULL, 0, login_seed, 0, NULL, 0, false },
        { "page",     run_page,     4096, NULL, 0, page_seed, 0, NULL, 0, false },
        { "catalog",  run_catalog,  sizeof(cat_seed), NULL, 0, cat_seed, 0, NULL, 0, false },
        { "exec",     run_exec,     4000, NS(EXEC_SEEDS), NULL, 0, NS(SQL_WORDS), false },
        { "http",     run_http,     16000, NS(HTTP_SEEDS), NULL, 0, NS(HTTP_WORDS), false },
        { "h2",       run_h2,       2048, NULL, 0, g_h2_seed, 0, NULL, 0, false },
        { "smtp",     run_smtp,     8000, NS(SMTP_SEEDS), NULL, 0, NS(HTTP_WORDS), false },
        { "htaccess", run_htaccess, 4000, NS(HTA_SEEDS),  NULL, 0, NS(HTTP_WORDS), false },
        { "regex",    run_regex,    200,  NS(RX_SEEDS),   NULL, 0, NULL, 0, false },
        { "sqljson",  run_sqljson,  4000, NS(SJ_SEEDS),   NULL, 0, NULL, 0, false },
        { "csv",      run_csv,      8000, NS(CSV_SEEDS),  NULL, 0, NULL, 0, false },
        { "diff",     run_diff,     0,    NULL, 0, NULL, 0, NULL, 0, true },
    };
    T[2].bseed_n = login_n; T[3].bseed_n = page_n; T[4].bseed_n = cat_n; T[7].bseed_n = g_h2_seed_n;
    int nt = (int)(sizeof(T) / sizeof(T[0]));
    int fails = 0;
    for (int ti = 0; ti < nt; ti++) {
        if (strcmp(which, "all") && strcmp(which, T[ti].name)) continue;
        if (!strcmp(which, "all") && !strcmp(T[ti].name, "diff")) continue;   // il differenziale si chiede da solo
        double t0 = now_s(), per = !strcmp(which, "all") ? secs / (nt - 1) : secs;
        long iters = 0;
        while (now_s() - t0 < per) {
            if (!T[ti].generated) {
                const uint8_t* seed; size_t sn; const uint8_t* other = NULL; size_t on = 0;
                if (T[ti].seeds) {
                    int k = (int)rn((size_t)T[ti].nseeds), o = (int)rn((size_t)T[ti].nseeds);
                    seed = (const uint8_t*)T[ti].seeds[k]; sn = strlen(T[ti].seeds[k]);
                    other = (const uint8_t*)T[ti].seeds[o]; on = strlen(T[ti].seeds[o]);
                } else { seed = T[ti].bseed; sn = T[ti].bseed_n; }
                if (sn > T[ti].maxlen) sn = T[ti].maxlen;
                memcpy(g_buf, seed, sn);
                g_n = sn;
                int rounds = 1 + (int)rn(4);
                for (int r = 0; r < rounds; r++)
                    mutate(g_buf, &g_n, T[ti].maxlen < sizeof(g_buf) - 1 ? T[ti].maxlen : sizeof(g_buf) - 1,
                           T[ti].words, T[ti].nwords, other, on);
            }
            T[ti].run();
            iters++;
        }
        if (T[ti].generated) {
            printf("fuzz %-8s %ld queries in %.0f s: %ld compared with SQLite, %ld different (fuzz-diff.txt)\n",
                   T[ti].name, iters, per, g_diff_checked, g_diff_found);
            if (g_diff_found) fails++;
        } else printf("fuzz %-8s %ld inputs in %.0f s: no sanitizer error\n", T[ti].name, iters, per);
    }
    remove("fuzz-crash.bin");
    remove("fuzz-catalog.cat");
    remove("fuzz-catalog.cat.tmp");
    if (g_http) { sv_server_stop(g_http); usleep(200000); }
    if (g_mail) mail_store_free(g_mail);
    if (g_lite) sqlite3_close(g_lite);
    if (g_exec_ready && !g_http) { db_shutdown(); if (system("rm -rf fuzz-db")) {} }
    if (system("rm -rf fuzz-www fuzz-mail fuzz-hta")) {}
    return fails ? 1 : 0;
}
