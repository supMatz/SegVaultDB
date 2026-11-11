// Righe grandi: pagine di overflow (fino a 64 MB per riga, 16 MB per testo), transazioni, riavvio, crash.
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_err[1100];
static bool ok(const char* sql) {
    QueryResult* r = db_execute(sql);
    bool g = r && r->success;
    snprintf(g_err, sizeof(g_err), "%.1000s", r ? r->error : "null");
    db_result_free(r);
    return g;
}
static long long num(const char* sql) {
    QueryResult* r = db_execute(sql);
    long long v = r && r->success && r->num_rows && !r->rows[0].cells[0].is_null ? atoll(r->rows[0].cells[0].value) : -1;
    if (r && !r->success) snprintf(g_err, sizeof(g_err), "%.1000s", r->error);
    db_result_free(r);
    return v;
}

// INSERT INTO big VALUES (id, 'testo di n caratteri ripetuti da c', tag)
static bool insert_text(int id, size_t n, char c, const char* tag) {
    char* sql = malloc(n + 200);
    if (!sql) return false;
    int k = snprintf(sql, 200, "INSERT INTO big VALUES (%d, '", id);
    memset(sql + k, c, n);
    snprintf(sql + k + n, 200, "', '%s')", tag);
    bool r = ok(sql);
    free(sql);
    return r;
}

static bool text_ok(int id, size_t n, char c) {
    char q[120];
    snprintf(q, sizeof(q), "SELECT body FROM big WHERE id = %d", id);
    QueryResult* r = db_execute(q);
    bool good = r && r->success && r->num_rows == 1 && strlen(r->rows[0].cells[0].value) == n;
    for (size_t i = 0; good && i < n; i += 997) good = r->rows[0].cells[0].value[i] == c;
    db_result_free(r);
    return good;
}

int main(void) {
    const char* dir = "/tmp/sv_ovf_test";
    if (system("rm -rf /tmp/sv_ovf_test && mkdir -p /tmp/sv_ovf_test") != 0) return 1;
    if (!db_init(dir)) return 1;
    ok("CREATE DATABASE o"); ok("USE o");
    ok("CREATE TABLE big (id INT PRIMARY KEY, body TEXT, tag VARCHAR(20))");

    // dimensioni diverse: sotto e sopra la soglia, sopra 65535 (lunghezza a 32 bit), alcuni MB
    size_t sizes[] = { 100, 3000, 10000, 70000, 300000, 2u << 20 };
    char chars[] = { 'a', 'b', 'c', 'd', 'e', 'f' };
    for (int i = 0; i < 6; i++) CHECK(insert_text(i + 1, sizes[i], chars[i], "orig"), "insert %llu bytes: %s", (unsigned long long)sizes[i], g_err);
    for (int i = 0; i < 6; i++) CHECK(text_ok(i + 1, sizes[i], chars[i]), "read back %llu bytes", (unsigned long long)sizes[i]);
    CHECK(num("SELECT LENGTH(body) FROM big WHERE id = 6") == (long long)(2u << 20), "LENGTH of 2 MB: %s", g_err);
    CHECK(num("SELECT COUNT(*) FROM big WHERE tag = 'orig' AND LENGTH(body) > 5000") == 4, "scan with filters over large rows: %s", g_err);
    CHECK(num("SELECT SUM(LENGTH(body)) FROM big") == (long long)(100 + 3000 + 10000 + 70000 + 300000 + (2u << 20)), "sum of lengths");
    // altre righe piccole nella stessa tabella: non disturbate dalle grandi
    for (int i = 100; i < 300; i++) { char q[100]; snprintf(q, sizeof(q), "INSERT INTO big VALUES (%d, 'small %d', 'small')", i, i); ok(q); }
    CHECK(num("SELECT COUNT(*) FROM big") == 206, "mixed rows");

    // UPDATE: piccola -> grande, grande -> piccola, grande -> grande
    char* up = malloc(90000);
    strcpy(up, "UPDATE big SET body = '");
    memset(up + strlen(up), 'x', 80000);
    strcpy(up + 23 + 80000, "' WHERE id = 1");
    CHECK(ok(up), "small -> large: %s", g_err);
    CHECK(text_ok(1, 80000, 'x'), "updated to large");
    CHECK(ok("UPDATE big SET body = 'tiny' WHERE id = 5") && text_ok(5, 4, 't'), "large -> small");
    CHECK(ok("UPDATE big SET tag = 'changed' WHERE id = 6") && text_ok(6, 2u << 20, 'f'), "other column of a large row");
    CHECK(ok("DELETE FROM big WHERE id = 4") && num("SELECT COUNT(*) FROM big WHERE id = 4") == 0, "delete large");

    // transazioni: ROLLBACK riporta le righe grandi
    CHECK(ok("BEGIN") && ok("UPDATE big SET body = 'gone' WHERE id = 3") && ok("DELETE FROM big WHERE id = 2"), "tx changes");
    CHECK(insert_text(50, 120000, 'z', "tx"), "tx insert large");
    CHECK(ok("ROLLBACK"), "rollback");
    CHECK(text_ok(3, 10000, 'c') && text_ok(2, 3000, 'b') && num("SELECT COUNT(*) FROM big WHERE id = 50") == 0, "rollback restored large rows");
    CHECK(ok("BEGIN") && ok("UPDATE big SET body = CONCAT(body, 'END') WHERE id = 6") && ok("COMMIT"), "commit large update: %s", g_err);
    CHECK(num("SELECT LENGTH(body) FROM big WHERE id = 6") == (long long)(2u << 20) + 3, "committed");

    // limite: testo oltre 16 MB rifiutato con un messaggio chiaro, nulla scritto
    CHECK(!insert_text(70, (16u << 20) + 10, 'w', "huge") && strstr(g_err, "longer than"), "text over 16 MB refused: %.80s", g_err);
    CHECK(num("SELECT COUNT(*) FROM big") == 205, "count before");

    // riavvio
    db_shutdown();
    CHECK(db_init(dir), "reopen");
    ok("USE o");
    CHECK(text_ok(1, 80000, 'x') && text_ok(3, 10000, 'c') && num("SELECT LENGTH(body) FROM big WHERE id = 6") == (long long)(2u << 20) + 3,
          "large rows after restart");
    db_shutdown();

    // crash: un processo inserisce e aggiorna righe grandi e termina senza chiudere il database
    pid_t pid = fork();
    if (pid == 0) {
        if (!db_init(dir)) _exit(2);
        db_result_free(db_execute("USE o"));
        insert_text(60, 200000, 'k', "crash");
        insert_text(61, 5000, 'm', "crash");
        db_result_free(db_execute("UPDATE big SET body = 'short now' WHERE id = 3"));
        db_result_free(db_execute("BEGIN"));
        insert_text(62, 150000, 'q', "uncommitted");
        _exit(0);                                      // niente COMMIT né db_shutdown
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child ran");
    CHECK(db_init(dir), "recovery after crash");
    ok("USE o");
    CHECK(text_ok(60, 200000, 'k') && text_ok(61, 5000, 'm'), "committed large rows survive the crash");
    CHECK(text_ok(3, 9, 's'), "committed update survives");
    CHECK(num("SELECT COUNT(*) FROM big WHERE id = 62") == 0, "uncommitted large insert rolled back");
    CHECK(text_ok(6, (2u << 20) + 3, 'f') && text_ok(1, 80000, 'x'), "older large rows intact");

    // VACUUM: le catene non più usate tornano disponibili e il file smette di crescere
    for (int i = 0; i < 3; i++) ok("UPDATE big SET body = CONCAT(SUBSTR(body, 2), 'f') WHERE id = 6");
    QueryResult* v = db_execute("VACUUM");
    int freed = v && v->success ? v->rows_affected : -1;
    db_result_free(v);
    CHECK(freed > 1000, "vacuum freed pages: %d", freed);
    FILE* fd = fopen("/tmp/sv_ovf_test/segvault.db", "rb");
    long before = 0, after = 0;
    if (fd) { fseek(fd, 0, SEEK_END); before = ftell(fd); fclose(fd); }
    for (int i = 0; i < 3; i++) ok("UPDATE big SET body = CONCAT(SUBSTR(body, 2), 'f') WHERE id = 6");
    fd = fopen("/tmp/sv_ovf_test/segvault.db", "rb");
    if (fd) { fseek(fd, 0, SEEK_END); after = ftell(fd); fclose(fd); }
    CHECK(after == before, "freed pages reused: file %ld -> %ld", before, after);
    CHECK(text_ok(6, (2u << 20) + 3, 'f') && text_ok(60, 200000, 'k') && text_ok(1, 80000, 'x'), "data intact after vacuum and reuse");
    CHECK(ok("BEGIN") && !ok("VACUUM") && strstr(g_err, "transaction") && ok("ROLLBACK"), "vacuum refused inside a transaction");
    db_shutdown();
    CHECK(db_init(dir), "reopen after vacuum");
    ok("USE o");
    CHECK(text_ok(6, (2u << 20) + 3, 'f') && text_ok(3, 9, 's') && num("SELECT COUNT(*) FROM big") == 207, "all rows after restart: %lld", num("SELECT COUNT(*) FROM big"));
    v = db_execute("VACUUM");
    CHECK(v && v->success, "second vacuum");
    db_result_free(v);
    CHECK(insert_text(80, 500000, 'n', "after") && text_ok(80, 500000, 'n'), "insert after vacuum");
    db_shutdown();
    if (system("rm -rf /tmp/sv_ovf_test")) {}
    if (g_fail) { printf("overflow tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d overflow checks passed\n", g_tests, g_tests);
    free(up);
    return 0;
}
