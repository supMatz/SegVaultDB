// Letture in parallelo: più SELECT insieme (lock condiviso) mentre le scritture restano esclusive.
#include "src/bridge/db_api.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "harness.h"



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

// letture a freddo (CR-A1): ogni thread legge le tre tabelle in un ordine diverso
static void* cold_reader(void* arg) {
    static const char* const Q[] = { "SELECT COUNT(*), SUM(v) FROM big WHERE v >= 0", "SELECT SUM(x) FROM w WHERE x >= 0",
                                     "SELECT COUNT(*) FROM sc WHERE v >= 0" };
    int k = (int)(intptr_t)arg;
    for (int i = 0; i < 3; i++) {
        bool refused;
        QueryResult* r = db_execute_readonly(Q[(k + i) % 3], "p", NULL, 0, &refused);
        if (!refused && (!r || !r->success)) atomic_fetch_add(&g_bad, 1);
        db_result_free(r);
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

// ── letture miste in parallelo (P0.3): funzioni finestra, UNION … ORDER BY DESC, primo uso di un
// indice per intervallo. Ogni risultato è confrontato con quello calcolato da un thread solo.
static const char* MIX_Q[] = {
    "SELECT id, ROW_NUMBER() OVER (ORDER BY v DESC, id), SUM(v) OVER (PARTITION BY k ORDER BY id) FROM big WHERE id < 400 ORDER BY id",
    "SELECT id, RANK() OVER (PARTITION BY k % 7 ORDER BY v) FROM big WHERE id < 300 ORDER BY id",
    "SELECT id FROM big WHERE id < 150 UNION SELECT id + 5000 FROM big WHERE id < 150 ORDER BY 1 DESC LIMIT 120",
    "SELECT k, v FROM big WHERE id < 80 UNION ALL SELECT v, k FROM big WHERE id < 80 ORDER BY 1 DESC, 2",
    "SELECT COUNT(*), SUM(v) FROM rg WHERE k BETWEEN 100 AND 700",
    "SELECT COUNT(*), MIN(k) FROM rg WHERE v > 2000 AND v < 9000",
    "SELECT COUNT(*) FROM rg WHERE k < 300",
    "SELECT COUNT(*) FROM rg WHERE v >= 5000 AND v < 10000",
};
#define MIX_NQ ((int)(sizeof(MIX_Q) / sizeof(MIX_Q[0])))
#define MIX_THREADS 8
#define MIX_ROUNDS 12
static char* g_mix_expect[MIX_NQ];
static pthread_barrier_t g_mix_bar;
static atomic_int g_mix_bad, g_mix_refused;

static char* result_text(const QueryResult* r) {
    if (!r) return strdup("(null)");
    if (!r->success) return strdup(r->error);
    size_t cap = 64, len = 0;
    char* s = malloc(cap);
    s[0] = '\0';
    for (int i = 0; i < r->num_rows; i++)
        for (int c = 0; c < r->rows[i].num_cells; c++) {
            const char* v = r->rows[i].cells[c].value ? r->rows[i].cells[c].value : "NULL";
            size_t need = len + strlen(v) + 2;
            if (need > cap) { while (need > cap) cap *= 2; s = realloc(s, cap); }
            len += (size_t)sprintf(s + len, "%s%c", v, c + 1 == r->rows[i].num_cells ? '\n' : '|');
        }
    return s;
}

static void* mix_reader(void* arg) {
    int me = (int)(intptr_t)arg;
    for (int round = 0; round < MIX_ROUNDS; round++) {
        pthread_barrier_wait(&g_mix_bar);         // tutti insieme: il primo uso dell'indice è in gara
        for (int k = 0; k < MIX_NQ; k++) {
            int q = (me + k) % MIX_NQ;
            bool refused;
            QueryResult* r = db_execute_readonly(MIX_Q[q], "p", NULL, 0, &refused);
            if (refused) atomic_fetch_add(&g_mix_refused, 1);
            else {
                char* got = result_text(r);
                if (strcmp(got, g_mix_expect[q])) atomic_fetch_add(&g_mix_bad, 1);
                free(got);
            }
            db_result_free(r);
        }
        pthread_barrier_wait(&g_mix_bar);         // il thread principale rende di nuovo da ordinare gli indici
    }
    return NULL;
}

static atomic_int g_scan_bad;
static void scan_rounds(int n) {
    for (int i = 0; i < n; i++) {
        bool refused;
        QueryResult* r = db_execute_readonly("SELECT COUNT(*), SUM(v) FROM sc WHERE pad <> 'x'", "p", NULL, 0, &refused);
        if (refused || !r || !r->success || strcmp(r->rows[0].cells[0].value, "130000")) atomic_fetch_add(&g_scan_bad, 1);
        db_result_free(r);
    }
}
static void* scan_reader(void* arg) { (void)arg; scan_rounds(3); return NULL; }

// CR-M1: SELECT a FROM deep WHERE a IN (SELECT a FROM deep WHERE a IN (...)) con 1..30 livelli
typedef struct { int answered, refused; } DeepRun;
static void* deep_reader(void* arg) {
    DeepRun* d = arg;
    for (int n = 1; n <= 30; n++) {
        char q[4096];
        size_t k = 0;
        for (int i = 0; i < n; i++) k += (size_t)snprintf(q + k, sizeof(q) - k, "SELECT a FROM deep WHERE a IN (");
        k += (size_t)snprintf(q + k, sizeof(q) - k, "SELECT a FROM deep");
        for (int i = 0; i < n; i++) q[k++] = ')';
        q[k] = '\0';
        QueryResult* r = db_execute(q);
        if (r && r->success && r->num_rows == 2) d->answered++;
        else if (r && !r->success) d->refused++;
        db_result_free(r);
    }
    return NULL;
}

int main(void) {
    test_enter_tmpdir();                                  // cartella privata (CR-M9)
    if (system("rm -rf sv_par_test && mkdir -p sv_par_test") != 0) return 1;
    if (!db_init("sv_par_test")) return 1;
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

    // letture miste in parallelo (P0.3)
    run("USE p");
    run("CREATE TABLE rg (id INT PRIMARY KEY, k INT, v INT)");
    run("CREATE INDEX rg_k ON rg (k)");
    run("CREATE INDEX rg_v ON rg (v)");
    run("BEGIN");
    for (int i = 0; i < 2000; i++) { char q[96]; snprintf(q, sizeof(q), "INSERT INTO rg VALUES (%d, %d, %d)", i, (i * 37) % 1000, (i * 7919) % 10000); run(q); }
    run("COMMIT");
    for (int q = 0; q < MIX_NQ; q++) {
        QueryResult* e = db_execute_readonly(MIX_Q[q], "p", NULL, 0, &refused);
        CHECK(!refused && e && e->success && e->num_rows > 0, "mixed query %d alone: %s", q, e ? e->error : "refused");
        g_mix_expect[q] = result_text(e);
        db_result_free(e);
    }
    pthread_barrier_init(&g_mix_bar, NULL, MIX_THREADS + 1);
    pthread_t mth[MIX_THREADS];
    for (int i = 0; i < MIX_THREADS; i++) pthread_create(&mth[i], NULL, mix_reader, (void*)(intptr_t)i);
    for (int round = 0; round < MIX_ROUNDS; round++) {
        char q[128];                              // chiavi nuove fuori da ogni intervallo: l'ordine va rifatto
        snprintf(q, sizeof(q), "INSERT INTO rg VALUES (%d, %d, %d)", 100000 + round, 5000 + round, 50000 + round);
        run(q);
        pthread_barrier_wait(&g_mix_bar);
        pthread_barrier_wait(&g_mix_bar);
    }
    for (int i = 0; i < MIX_THREADS; i++) pthread_join(mth[i], NULL);
    pthread_barrier_destroy(&g_mix_bar);
    CHECK(atomic_load(&g_mix_bad) == 0, "mixed parallel reads match the single-thread results: %d differ", atomic_load(&g_mix_bad));
    CHECK(atomic_load(&g_mix_refused) == 0, "mixed parallel reads not refused: %d", atomic_load(&g_mix_refused));
    for (int q = 0; q < MIX_NQ; q++) free(g_mix_expect[q]);

    // limite di tempo per thread
    r = db_execute_readonly("SELECT COUNT(*) FROM big a, big b, big c WHERE a.v + b.v = c.v", "p", NULL, 150, &refused);
    CHECK(!refused && r && !r->success && strstr(r->error, "time limit"), "per-read time limit: %s", r ? r->error : "refused");
    db_result_free(r);

    // con una transazione aperta: le letture parallele delle tabelle che scrive si fanno da parte
    // (niente dati non confermati), quelle delle altre tabelle vanno avanti
    run("BEGIN");
    run("INSERT INTO w VALUES (100000, 1)");
    r = db_execute_readonly("SELECT COUNT(*) FROM w", "p", NULL, 0, &refused);
    CHECK(refused, "refused on a table written by an open transaction");
    db_result_free(r);
    r = db_execute_readonly("SELECT COUNT(*) FROM big", "p", NULL, 0, &refused);
    CHECK(!refused && r && r->success, "allowed on another table");
    db_result_free(r);
    run("ROLLBACK");
    r = db_execute_readonly("SELECT COUNT(*) FROM w", "p", NULL, 0, &refused);
    CHECK(!refused && r && r->success && !strcmp(r->rows[0].cells[0].value, "80"), "after ROLLBACK: %s", r && r->success ? r->rows[0].cells[0].value : "?");
    db_result_free(r);

    // permessi dell'utente
    run("CREATE USER lu IDENTIFIED BY 'pass1234'");
    run("GRANT SELECT ON p.w TO lu");
    r = db_execute_readonly("SELECT COUNT(*) FROM big", "p", "lu", 0, &refused);
    CHECK(!refused && r && !r->success && strstr(r->error, "Access denied"), "user privileges checked");
    db_result_free(r);
    r = db_execute_readonly("SELECT COUNT(*) FROM w", "p", "lu", 0, &refused);
    CHECK(!refused && r && r->success, "allowed read");
    db_result_free(r);

    // P1.10: scansioni complete in parallelo su una tabella più grande della cache (16 MB, tabella ~57 MB): ogni
    // pagina passa dal disco. min(8, CPU) lettori devono fare più lavoro insieme che uno solo. Guardia di
    // regressione (2,5x con 8 lettori), solo con almeno 8 CPU e fuori dalla CI (macchine condivise): oggi ~3-3,9x,
    // il limite è la copia in memoria di ogni riga letta (P1.14), non la cache delle pagine (in cache: 7x)
    {
        run("CREATE TABLE sc (id INT PRIMARY KEY, v INT, pad VARCHAR(500))");
        run("BEGIN");
        static char b[8000];
        for (int i = 0; i < 130000; i += 10) {
            int k = snprintf(b, sizeof(b), "INSERT INTO sc VALUES ");
            for (int j = 0; j < 10; j++) k += snprintf(b + k, sizeof(b) - (size_t)k, "%s(%d, %d, '%0400d')", j ? "," : "", i + j, (i + j) % 97, i + j);
            run(b);
        }
        run("COMMIT");
        db_shutdown();                                          // a freddo: motore riaperto, cache vuota
        CHECK(db_init("sv_par_test"), "reopen for the scaling test");
        run("USE p");
        run("SET GLOBAL cache_size = 16");
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        int nth = ncpu >= 8 ? 8 : ncpu >= 2 ? (int)ncpu : 2;
        scan_rounds(1);                                         // a regime
        double ts0 = now_ms();
        scan_rounds(3);
        double one = now_ms() - ts0;
        pthread_t sth[8];
        ts0 = now_ms();
        for (int i = 0; i < nth; i++) pthread_create(&sth[i], NULL, scan_reader, NULL);
        for (int i = 0; i < nth; i++) pthread_join(sth[i], NULL);
        double many = now_ms() - ts0;
        double speed = many > 0 ? nth * one / many : 0;
        printf("%d parallel full scans of a table larger than the cache: %.2fx the work of one reader (one: %.0f ms, %d: %.0f ms, %ld CPUs)\n",
               nth, speed, one / 3, nth, many / 3, ncpu);
        CHECK(atomic_load(&g_scan_bad) == 0, "parallel scans all correct (%d bad)", atomic_load(&g_scan_bad));
        if (ncpu >= 8 && !getenv("CI")) CHECK(speed >= 2.5, "8 readers do at least 2.5x the work of one (%.2fx)", speed);
    }

    // CR-A1: subito dopo un riavvio, lettori paralleli prima di qualunque lettura da un thread solo: le mappe delle
    // tabelle si costruiscono insieme (sotto make test-tsan una race su g_heaps comparirebbe qui)
    {
        db_shutdown();
        CHECK(db_init("sv_par_test"), "reopen for the cold parallel test");
        atomic_store(&g_bad, 0);
        atomic_store(&g_refused, 0);
        pthread_t cth[8];
        for (int i = 0; i < 8; i++) pthread_create(&cth[i], NULL, cold_reader, (void*)(intptr_t)i);
        for (int i = 0; i < 8; i++) pthread_join(cth[i], NULL);
        CHECK(atomic_load(&g_bad) == 0, "cold parallel reads all correct (%d bad)", atomic_load(&g_bad));
    }

    // CR-M1: con lo stack minimo documentato (512 KB, db_api.h) le sottoquery annidate fino al limite del motore e
    // oltre rispondono o vengono rifiutate, senza esaurire lo stack (prima: crash a 14 livelli con 1 MB)
    {
        QueryResult* dq = db_execute("CREATE TABLE deep (a INT)");
        db_result_free(dq);
        dq = db_execute("INSERT INTO deep VALUES (1), (2)");
        db_result_free(dq);
        pthread_attr_t at;
        pthread_attr_init(&at);
#if defined(__SANITIZE_ADDRESS__)
        size_t stack = 4u << 20;                         // ASan: frame molto più grandi, il limite vale per la build normale
#else
        size_t stack = 512u << 10;
#endif
        pthread_attr_setstacksize(&at, stack);
        DeepRun dr = { 0, 0 };
        pthread_t dt;
        CHECK(pthread_create(&dt, &at, deep_reader, &dr) == 0, "thread with a small stack");
        pthread_join(dt, NULL);
        pthread_attr_destroy(&at);
        CHECK(dr.answered + dr.refused == 30 && dr.answered >= 5, "nested subqueries on a %u KB stack: %d answered, %d refused",
              (unsigned)(stack >> 10), dr.answered, dr.refused);
    }

    db_shutdown();
    if (system("rm -rf sv_par_test")) {}
    if (g_fail) { printf("parallel tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d parallel checks passed\n", g_tests, g_tests);
    return 0;
}
