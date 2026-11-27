/*
SCOPO: infrastruttura comune dei test (CR-M9).
       - CHECK(cond, fmt, ...): conta il controllo; se fallisce stampa file:riga e il messaggio;
       - test_enter_tmpdir(): crea una cartella privata con mkdtemp (in $TMPDIR o /tmp), ci entra con chdir e la
         cancella all'uscita. I test usano percorsi relativi ("sv_backup_test/data"): due esecuzioni insieme non si
         toccano più, e nessun altro utente può preparare prima la cartella o un link simbolico con quel nome
         (prima: percorsi fissi come /tmp/sv_par_test con rm -rf all'inizio);
       - test_src(): cartella del sorgente (quella da cui è partito il test), per i file del repository;
       - test_summary(nome): riga finale "OK: n/n <nome> checks passed" o "FAIL: ..." e codice d'uscita.
*/
#ifndef SV_TEST_HARNESS_H
#define SV_TEST_HARNESS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SV_HARNESS_TMP_ONLY          // i test con contatori propri prendono solo la cartella privata
static int g_tests, g_fail;

#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                         printf(__VA_ARGS__); printf("\n"); } } while (0)
#endif

static char g_test_tmp[512];
static char g_test_src[1024];
static pid_t g_test_owner;

static void test_tmpdir_cleanup(void) {
    if (!g_test_tmp[0] || getpid() != g_test_owner) return;    // i processi figli (fork) non cancellano nulla
    if (g_test_src[0] && chdir(g_test_src) != 0) return;
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_test_tmp);     // il nome viene da mkdtemp: niente apici dentro
    if (system(cmd) != 0) fprintf(stderr, "warning: cannot remove %s\n", g_test_tmp);
}

// entra in una cartella temporanea privata (0700), cancellata all'uscita; SV_TEST_KEEP=1 la conserva
static inline const char* test_enter_tmpdir(void) {
    if (g_test_tmp[0]) return g_test_tmp;
    if (!getcwd(g_test_src, sizeof(g_test_src))) g_test_src[0] = '\0';
    const char* base = getenv("TMPDIR");
    if (!base || !base[0] || strchr(base, '\'')) base = "/tmp";
    snprintf(g_test_tmp, sizeof(g_test_tmp), "%s/svtest-XXXXXX", base);
    if (!mkdtemp(g_test_tmp) || chdir(g_test_tmp) != 0) { perror("test temporary folder"); exit(2); }
    g_test_owner = getpid();
    const char* keep = getenv("SV_TEST_KEEP");
    if (keep && keep[0] == '1') fprintf(stderr, "test folder kept: %s\n", g_test_tmp);
    else atexit(test_tmpdir_cleanup);
    return g_test_tmp;
}

// path relativo alla cartella del sorgente (es. "examples/01.sql"), in un buffer che resta valido fino a 4 chiamate dopo
static inline const char* test_src(const char* rel) {
    static char buf[4][1200];
    static unsigned k;
    char* b = buf[k++ % 4];
    snprintf(b, sizeof(buf[0]), "%s/%s", g_test_src[0] ? g_test_src : ".", rel);
    return b;
}

#ifndef SV_HARNESS_TMP_ONLY
static inline int test_summary(const char* what) {
    if (g_fail) { printf("FAIL: %d/%d %s checks passed\n", g_tests - g_fail, g_tests, what); return 1; }
    printf("OK: %d/%d %s checks passed\n", g_tests, g_tests, what);
    return 0;
}
#endif

#endif
