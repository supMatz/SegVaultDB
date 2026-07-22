#include "src/bridge/db_api.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main() {
    system("rm -rf /tmp/sv_bt && mkdir -p /tmp/sv_bt");
    if (!db_init("/tmp/sv_bt")) { printf("FAIL: db_init\n"); return 1; }

    QueryResult* r;
    r = db_execute("CREATE DATABASE testdb"); printf("CREATE DB: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    r = db_execute("USE testdb"); printf("USE: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    r = db_execute("CREATE TABLE users (id INT, nome VARCHAR(50))"); printf("CREATE: %s\n", r->success ? "OK" : r->error); db_result_free(r);
    
    // Test with backtick-quoted table name
    r = db_execute("SELECT * FROM `users`");
    if (r && r->success) {
        printf("BACKTICK SELECT OK: %d cols\n", r->num_cols);
    } else {
        printf("BACKTICK SELECT FAIL: %s\n", r ? r->error : "NULL");
    }
    if (r) db_result_free(r);

    db_shutdown();
    return 0;
}
