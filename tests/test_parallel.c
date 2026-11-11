// Letture in parallelo: più SELECT insieme (lock condiviso) mentre le scritture restano esclusive.
#include "src/bridge/db_api.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void run(const char* sql) { QueryResult* r = db_execute(sql); if (!r || !r->success) printf("setup: %s -> %s\n", sql, r ? r->error : "null"); db_result_free(r); }

static atomic_int g_inside, g_max_inside, g_bad, g_refused, g_done_reads;
static atomic_int g_stop;

// SELECT pesante (join con aggregazione) ripetuta; conta quante letture sono in corso insieme
static void* reader(void* arg) {
    (void)arg;
    for (int i = 0; i < 6; i++) {
        bool refused;
        int in = atomic_fetch_add(&g_inside, 1) + 1;
        int m = atomic_load(&g_max_inside);
        while (in > m && !atomic_compare_exchange_weak(&g_max_inside, &m, in)) {}
        QueryResult* r = db_execute_readonly("SELECT COUNT(*), SUM(a.v) FROM big a JOIN big b ON b.k = a.k WHERE a.v >= 0",
                                             "p", NULL, 0, &refused);
        atomic_fetch_sub(&g_inside, 1);
        if (refused) atomic_fetch_add(&g_refused, 1);
        else if (!r || !r->success || r->num_rows != 1) atomic_fetch_add(&g_bad, 1);
        db_result_free(r);
        atomic_fetch_add(&g_done_reads, 1);
    }
    return NULL;
}

// scritture esclusive in mezzo alle letture: ogni INSERT aggiunge 2 righe in uno statement
static void* writer(void* arg) {
    (void)arg;
    for (int i = 0; i < 40 && !atomic_load(&g_stop); i++) {
        char q[160];
        snprintf(q, sizeof(q), "INSERT INTO w VALUES (%d, 1), (%d, 1)", 2 * i, 2 * i + 1);
        QueryResult* r = db_execute(q);
        if (!r || !r->success) atomic_fetch_add(&g_bad, 1);
        db_result_free(r);
        bool refused;
        r = db_execute_readonly("SELECT COUNT(*) FROM w", "p", NULL, 0, &refused);
        if (!refused && (!r || !r->success || atoi(r->rows[0].cells[0].value) % 2)) atomic_fetch_add(&g_bad, 1);   // mai a metà
        db_result_free(r);
    }
    return NULL;
}

int main(void) {
    if (system("rm -rf /tmp/sv_par_test && mkdir -p /tmp/sv_par_test") != 0) return 1;
    if (!db_init("/tmp/sv_par_test")) return 1;
    run("CREATE DATABASE p"); run("USE p");
    run("CREATE TABLE big (id INT PRIMARY KEY, k INT, v INT)");
    run("CREATE TABLE w (id INT PRIMARY KEY, x INT)");
    run("BEGIN");
    for (int i = 0; i < 3000; i++) { char q[96]; snprintf(q, sizeof(q), "INSERT INTO big VALUES (%d, %d, %d)", i, i % 50, i); run(q); }
    run("COMMIT");

    // una lettura da sola, per confronto e correttezza
    bool refused;
    QueryResult* r = db_execute_readonly("SELECT COUNT(*) FROM big WHERE k = 7", "p", NULL, 0, &refused);
    CHECK(!refused && r && r->success && !strcmp(r->rows[0].cells[0].value, "60"), "single read: %s", r ? r->error : "refused");
    db_result_free(r);
    r = db_execute_readonly("DELETE FROM big", "p", NULL, 0, &refused);
    CHECK(refused && !r, "writes are refused (go through db_execute)");
    r = db_execute_readonly("SELECT 1", "nope", NULL, 0, &refused);
    CHECK(refused, "unknown database refused");

    // 4 lettori + 1 scrittore insieme
    pthread_t th[5];
    double t0 = now_ms();
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, reader, NULL);
    pthread_create(&th[4], NULL, writer, NULL);
    for (int i = 0; i < 5; i++) pthread_join(th[i], NULL);
    double el = now_ms() - t0;
    CHECK(atomic_load(&g_bad) == 0, "results consistent: %d bad", atomic_load(&g_bad));
    CHECK(atomic_load(&g_refused) == 0, "no read refused: %d", atomic_load(&g_refused));
    CHECK(atomic_load(&g_done_reads) == 24, "all reads done");
    CHECK(atomic_load(&g_max_inside) >= 2, "reads overlapped (max %d at once)", atomic_load(&g_max_inside));
    run("USE p");
    QueryResult* c = db_execute("SELECT COUNT(*) FROM w");
    CHECK(c && c->success && !strcmp(c->rows[0].cells[0].value, "80"), "all writes applied");
    db_result_free(c);
    printf("parallel phase: %.0f ms, up to %d reads at once\n", el, atomic_load(&g_max_inside));

    // limite di tempo per thread
    r = db_execute_readonly("SELECT COUNT(*) FROM big a, big b, big c WHERE a.v + b.v = c.v", "p", NULL, 150, &refused);
    CHECK(!refused && r && !r->success && strstr(r->error, "time limit"), "per-read time limit: %s", r ? r->error : "refused");
    db_result_free(r);

    // con una transazione aperta le letture parallele si fanno da parte
    run("BEGIN");
    r = db_execute_readonly("SELECT 1", "p", NULL, 0, &refused);
    CHECK(refused, "refused while a transaction is open");
    run("ROLLBACK");

    // permessi dell'utente
    run("CREATE USER lu IDENTIFIED BY 'pass1234'");
    run("GRANT SELECT ON p.w TO lu");
    r = db_execute_readonly("SELECT COUNT(*) FROM big", "p", "lu", 0, &refused);
    CHECK(!refused && r && !r->success && strstr(r->error, "Access denied"), "user privileges checked");
    db_result_free(r);
    r = db_execute_readonly("SELECT COUNT(*) FROM w", "p", "lu", 0, &refused);
    CHECK(!refused && r && r->success, "allowed read");
    db_result_free(r);

    db_shutdown();
    if (system("rm -rf /tmp/sv_par_test")) {}
    if (g_fail) { printf("parallel tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d parallel checks passed\n", g_tests, g_tests);
    return 0;
}
