// Decimali esatti (P1.9): espressioni casuali eseguite dal motore, poi tests/check_decimal.py le ricalcola con
// il modulo decimal di Python e le regole di MySQL (scala di + - * /, arrotondamento a metà lontano dallo zero).
//   ./test_decimal FILE [N]      scrive "op|a|b|risultato" per riga
#include "src/bridge/db_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SV_HARNESS_TMP_ONLY
#include "harness.h"

static uint64_t g = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return g; }

static void num(char* out, int max_int, int max_frac) {
    int ni = 1 + (int)(rnd() % (uint64_t)max_int), nf = (int)(rnd() % (uint64_t)(max_frac + 1));
    size_t k = 0;
    if (rnd() % 3 == 0) out[k++] = '-';
    for (int i = 0; i < ni; i++) out[k++] = (char)('0' + (i == 0 && ni > 1 ? 1 + rnd() % 9 : rnd() % 10));
    if (nf) { out[k++] = '.'; for (int i = 0; i < nf; i++) out[k++] = (char)('0' + rnd() % 10); }
    out[k] = '\0';
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s OUT [N]\n", argv[0]); return 2; }
    int n = argc > 2 ? atoi(argv[2]) : 10000;
    FILE* f = fopen(argv[1], "w");                        // il file dei risultati resta nella cartella di partenza
    test_enter_tmpdir();                                  // cartella privata (CR-M9)
    if (system("rm -rf sv_dec_test && mkdir -p sv_dec_test") != 0 || !db_init("sv_dec_test")) { printf("init failed\n"); if (f) fclose(f); return 1; }
    if (!f) return 1;
    static const char* const OPS[] = { "+", "-", "*", "/", "ROUND", "<", "SUM" };
    int bad = 0;
    // colonna DECIMAL(30,8): SUM esatta su righe casuali
    db_result_free(db_execute("CREATE DATABASE d"));
    db_result_free(db_execute("USE d"));
    db_result_free(db_execute("CREATE TABLE s (k INT, v DECIMAL(30,8))"));
    for (int i = 0; i < n; i++) {
        char a[64], b[160], sql[400], res[200];
        int op = (int)(rnd() % 7);
        num(a, 15, 8);
        num(b, op == 3 ? 6 : 15, op == 3 ? 4 : 8);
        if (op == 3 && !strpbrk(b, "123456789")) snprintf(b, sizeof(b), "7.5");
        int k = (int)(rnd() % 6);
        if (op == 4) snprintf(sql, sizeof(sql), "SELECT ROUND(%s, %d)", a, k);
        else if (op == 5) snprintf(sql, sizeof(sql), "SELECT %s < %s", a, b);
        else if (op == 6) {                              // tre righe nel gruppo i, poi la loro somma
            char c[64];
            num(c, 15, 8);
            snprintf(sql, sizeof(sql), "INSERT INTO s VALUES (%d, %s), (%d, %s), (%d, %s)", i, a, i, b, i, c);
            db_result_free(db_execute(sql));
            snprintf(sql, sizeof(sql), "SELECT SUM(v) FROM s WHERE k = %d", i);
            snprintf(b + strlen(b), sizeof(b) - strlen(b), ";%s", c);
        } else snprintf(sql, sizeof(sql), "SELECT (%s) %s (%s)", a, OPS[op], b);
        QueryResult* r = db_execute(sql);
        if (!r || !r->success || r->num_rows != 1) {
            snprintf(res, sizeof(res), "ERR %.180s", r ? r->error : "null");
            if (!r || !strstr(r->error, "out of range (64 bit)")) bad++;   // due interi oltre 64 bit: errore atteso
        }
        else snprintf(res, sizeof(res), "%s", r->rows[0].cells[0].is_null ? "NULL" : r->rows[0].cells[0].value);
        db_result_free(r);
        fprintf(f, "%s|%s|%s|%d|%s\n", OPS[op], a, b, k, res);
    }
    fclose(f);
    db_shutdown();
    if (system("rm -rf sv_dec_test")) {}
    if (bad) { printf("decimal: %d statements failed\n", bad); return 1; }
    return 0;
}
