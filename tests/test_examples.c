// Esegue gli script di examples/ come farebbe l'editor (stesso split in statement):
// ogni statement deve riuscire, tranne quelli preceduti da un commento "errore atteso".
//   ./test_examples            controlla
//   ./test_examples -v         mostra anche i risultati
#include "src/bridge/db_api.h"
#include "src/widgets/sql_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = '\0';
    fclose(f);
    return b;
}

int main(int argc, char** argv) {
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    const char* files[] = { "examples/01_universita.sql", "examples/02_query_bd1.sql", "examples/03_vincoli.sql",
                            "examples/04_bd2_ottimizzazione.sql", "examples/05_bd2_transazioni_trigger.sql" };
    if (system("rm -rf /tmp/sv_examples_test && mkdir -p /tmp/sv_examples_test") != 0) return 1;
    if (!db_init("/tmp/sv_examples_test")) { printf("FAIL: db_init\n"); return 1; }
    int total = 0, failed = 0;
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        char* text = read_file(files[f]);
        if (!text) { printf("FAIL: cannot read %s\n", files[f]); failed++; continue; }
        int len = (int)strlen(text);
        SqlRange* r = malloc(sizeof(SqlRange) * (size_t)(len + 2));
        int n = sql_split(text, len, r, len + 2);
        for (int i = 0; i < n; i++) {
            if (!r[i].has_code) continue;
            char* stmt = strndup(text + r[i].code_start, (size_t)(r[i].end - r[i].code_start));
            char* lead = strndup(text + r[i].start, (size_t)(r[i].code_start - r[i].start));
            // "errore atteso" nell'ultima riga di commento prima dello statement
            char* last = lead + strlen(lead);
            while (last > lead && (last[-1] == '\n' || last[-1] == ' ' || last[-1] == '\r')) last--;
            *last = '\0';
            char* nl = strrchr(lead, '\n');
            bool expect_err = strstr(nl ? nl : lead, "errore atteso") != NULL;
            QueryResult* q = db_execute(stmt);
            bool ok = q && q->success;
            total++;
            if (ok == expect_err) {
                failed++;
                printf("FAIL %s: %s\n   %s\n", files[f], stmt, ok ? "succeeded but an error was expected" : (q ? q->error : "null"));
            } else if (verbose) {
                printf("\n> %s\n", stmt);
                if (!ok) printf("  (errore atteso) %s\n", q->error);
                else if (q->num_cols) {
                    for (int k = 0; k < q->num_rows && k < 12; k++) {
                        printf("  ");
                        for (int c = 0; c < q->num_cols; c++) printf("%s%s", c ? " | " : "", q->rows[k].cells[c].value);
                        printf("\n");
                    }
                    if (q->num_rows > 12) printf("  ... (%d rows)\n", q->num_rows);
                } else printf("  OK (%d)\n", q->rows_affected);
            }
            db_result_free(q);
            free(stmt);
            free(lead);
        }
        free(r);
        free(text);
    }
    db_shutdown();
    if (system("rm -rf /tmp/sv_examples_test") != 0) { /* nulla */ }
    printf("%s: %d/%d example statements behaved as documented\n", failed ? "FAIL" : "OK", total - failed, total);
    return failed ? 1 : 0;
}
