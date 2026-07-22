#include "src/bridge/db_api.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main() {
    system("rm -rf /tmp/sv_varchar && mkdir -p /tmp/sv_varchar");
    if (!db_init("/tmp/sv_varchar")) { printf("FAIL: db_init\n"); return 1; }

    QueryResult* r;

    r = db_execute("CREATE DATABASE testdb"); printf("CREATE DB: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    r = db_execute("USE testdb"); printf("USE: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    r = db_execute("CREATE TABLE users (id INT, nome VARCHAR(50))"); printf("CREATE: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    r = db_execute("INSERT INTO users VALUES (1, 'Alice')"); printf("INSERT: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    r = db_execute("SELECT * FROM users");

    if (r && r->success) {
        printf("SELECT: %d rows, %d cols\n", r->num_rows, r->num_cols);
        for (int c = 0; c < r->num_cols; c++) {
            printf("  col[%d] name='%s'\n", c, r->col_names[c] ? r->col_names[c] : "(null)");
        }
        for (int i = 0; i < r->num_rows; i++) {
            for (int c = 0; c < r->num_cols; c++) {
                printf("  [%d][%d] value='%s'\n", i, c, r->rows[i].cells[c].value);
            }
        }
    } else {
        printf("SELECT FAIL: %s\n", r ? r->error : "NULL result");
    }
    if (r) db_result_free(r);

    db_shutdown();
    return 0;
}
