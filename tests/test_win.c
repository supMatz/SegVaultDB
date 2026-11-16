// Prove del codice solo Windows (cross-compilato con MinGW, eseguito su Windows o con wine):
// offset a 64 bit (P0.12), sostituzione atomica dei file (P0.7), motore completo con riavvio e recupero.
#include "src/bridge/db_api.h"
#include "src/storage/page.h"
#include "src/platform_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long long num(const char* sql) {
    QueryResult* r = db_execute(sql);
    long long v = r && r->success && r->num_rows && !r->rows[0].cells[0].is_null ? atoll(r->rows[0].cells[0].value) : -1;
    if (r && !r->success) printf("  (%s -> %s)\n", sql, r->error);
    db_result_free(r);
    return v;
}
static bool ok(const char* sql) {
    QueryResult* r = db_execute(sql);
    bool g = r && r->success;
    if (!g) printf("  (%s -> %s)\n", sql, r ? r->error : "null");
    db_result_free(r);
    return g;
}

int main(void) {
    const char* dir = "sv_win_test";
    system("rmdir /s /q sv_win_test >nul 2>&1");
    sv_mkdir(dir);

    // P0.12: pagina 600 000 (offset 2,4 GB), file sparso
    {
        int fd = sv_open("sv_win_test\\big.bin", SV_O_RDWR | SV_O_CREAT | SV_O_TRUNC, 0600);
        Page* pg = page_create(600000);
        CHECK(fd >= 0 && pg && page_insert(pg, "big-file-row", 12) == 0, "page prepared");
        CHECK(pg && page_write(pg, fd) == SV_OK, "write page 600000");
        Page* back = page_read(600000, fd);
        uint16_t bl = 0;
        const void* row = back ? page_get_slot(back, 0, &bl) : NULL;
        CHECK(back && back->page_id == 600000 && row && bl == 12 && !memcmp(row, "big-file-row", 12), "read page 600000 back");
        CHECK(sv_lseek(fd, 0, SEEK_END) == (int64_t)600001 * 4096, "file size past 2 GiB: %lld", (long long)sv_lseek(fd, 0, SEEK_END));
        CHECK(sv_lseek(fd, 0, SEEK_CUR) == (int64_t)600001 * 4096, "pread / pwrite do not move the descriptor");
        page_free(pg);
        page_free(back);
        sv_close(fd);
        remove("sv_win_test\\big.bin");
    }

    // P0.7: sostituzione di un file esistente
    {
        FILE* a = fopen("sv_win_test\\a.txt", "w"); fputs("old", a); fclose(a);
        FILE* b = fopen("sv_win_test\\a.txt.tmp", "w"); fputs("new", b); fclose(b);
        CHECK(sv_replace_file("sv_win_test\\a.txt.tmp", "sv_win_test\\a.txt") == 0, "replace an existing file");
        char buf[8] = "";
        FILE* c = fopen("sv_win_test\\a.txt", "r");
        if (c) { if (!fgets(buf, sizeof(buf), c)) buf[0] = 0; fclose(c); }
        CHECK(!strcmp(buf, "new") && !fopen("sv_win_test\\a.txt.tmp", "r"), "file replaced: %s", buf);
    }

    // motore: dati, ROLLBACK, riavvio, catalogo e utenti
    CHECK(db_init(dir), "init: %s", db_init_error());
    ok("CREATE DATABASE d");
    ok("USE d");
    ok("CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(200))");
    ok("BEGIN");
    for (int i = 1; i <= 500; i++) { char q[300]; snprintf(q, sizeof(q), "INSERT INTO t VALUES (%d, '%0120d')", i, i); ok(q); }
    ok("COMMIT");
    ok("BEGIN");
    ok("DELETE FROM t WHERE id <= 300");
    ok("ROLLBACK");
    CHECK(num("SELECT COUNT(*) FROM t") == 500, "rollback restores the rows: %lld", num("SELECT COUNT(*) FROM t"));
    ok("CREATE USER w IDENTIFIED BY 'w-pass'");
    ok("INSERT INTO t VALUES (9999, X'00C0FF')");
    db_shutdown();
    CHECK(db_init(dir), "reopen: %s", db_init_error());
    ok("USE d");
    CHECK(num("SELECT COUNT(*) FROM t") == 501 && num("SELECT LENGTH(v) FROM t WHERE id = 9999") == 3, "data after restart");
    CHECK(db_login("w", "w-pass"), "user after restart");
    {   // P2.6: ACL protetta, solo l'utente corrente e SYSTEM (prima: quella ereditata dalla cartella sopra)
        char up[600];
        snprintf(up, sizeof(up), "%s\\users.dat", dir);
        char au[600], ad[600];
        sv_win_acl_sddl(up, au, sizeof(au));
        sv_win_acl_sddl(dir, ad, sizeof(ad));
        if (sv_win_under_wine()) printf("(wine keeps no ACLs: owner-only ACL not checked; users.dat %s)\n", au);
        else {
            int na = 0, nd = 0;
            for (const char* q = au; (q = strstr(q, "(A;")); q++) na++;
            for (const char* q = ad; (q = strstr(q, "(A;")); q++) nd++;
            CHECK(!strncmp(au, "D:P", 3) && na == 2 && strstr(au, ";;;SY)") && (strstr(au, "S-1-5-") || strstr(au, ";;;LA)")) && !strstr(au, ";;;WD)") && !strstr(au, ";;;BU)"),
                  "owner-only ACL on users.dat: %s", au);
            CHECK(!strncmp(ad, "D:P", 3) && nd == 2 && strstr(ad, "OICI"), "owner-only, inherited ACL on the data folder: %s", ad);
        }
    }
    db_shutdown();
    system("rmdir /s /q sv_win_test >nul 2>&1");
    if (g_fail) { printf("windows tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d windows checks passed\n", g_tests, g_tests);
    return 0;
}
