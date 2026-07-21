#include "src/bridge/db_api.h"
#include <stdio.h>

int main() {
    system("rm -rf /tmp/joindemo && mkdir -p /tmp/joindemo");
    db_init("/tmp/joindemo");
    db_execute("CREATE DATABASE test");
    db_execute("USE test");

    // Due tabelle: utenti(id) e users(id, nome)
    db_execute("CREATE TABLE utenti (id INT)");
    db_execute("CREATE TABLE users (id INT, nome VARCHAR(50))");

    db_execute("INSERT INTO utenti VALUES (1)");
    db_execute("INSERT INTO utenti VALUES (2)");
    db_execute("INSERT INTO users VALUES (1, 'Alice')");
    db_execute("INSERT INTO users VALUES (2, 'Bob')");

    // JOIN con ON
    QueryResult* r = db_execute("SELECT * FROM utenti JOIN users ON utenti.id = users.id");
    if (r && r->success) {
        printf("JOIN result: %d rows\n", r->num_rows);
        for (int i = 0; i < r->num_rows; i++) {
            for (int j = 0; j < r->num_cols; j++)
                printf("  %s", r->rows[i].cells[j].value);
            printf("\n");
        }
        db_result_free(r);
    }

    // Cross join con virgola
    r = db_execute("SELECT * FROM utenti, users");
    if (r && r->success) {
        printf("\nCross JOIN (virgola): %d rows\n", r->num_rows);
        for (int i = 0; i < r->num_rows; i++) {
            for (int j = 0; j < r->num_cols; j++)
                printf("  %s", r->rows[i].cells[j].value);
            printf("\n");
        }
        db_result_free(r);
    }

    db_shutdown();
    return 0;
}
