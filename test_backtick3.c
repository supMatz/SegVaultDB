#include "src/bridge/db_api.h"
#include "src/query/lexer.h"
#include "src/query/parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main() {
    system("rm -rf /tmp/sv_bt3 && mkdir -p /tmp/sv_bt3");
    if (!db_init("/tmp/sv_bt3")) { printf("FAIL: db_init\n"); return 1; }

    db_execute("CREATE DATABASE testdb"); db_result_free(db_execute("USE testdb"));
    db_execute("CREATE TABLE users (id INT, nome VARCHAR(50))"); db_result_free(db_execute("USE testdb"));

    // First, check what the catalog thinks the table name is
    printf("Tables in testdb:\n");
    NameList* nl = db_list_tables("testdb");
    if (nl) for (int i = 0; i < nl->count; i++) printf("  '%s'\n", nl->names[i]);
    db_namelist_free(nl);

    // Parse the SQL and check table name
    ParserState* ps = parser_create("SELECT * FROM `users`");
    if (ps) {
        ASTNode* n = parser_parse(ps);
        if (n && n->type == NODE_SELECT) {
            printf("Parsed table_name: '%s'\n", n->select_stmt.table_name);
        } else {
            printf("Parse error: %s\n", ps->result->error);
        }
        parser_free(ps);
    }

    // Compare
    printf("strcmp with 'users': %d\n", strcmp("users", "users"));
    printf("strcmp from lexer: %d\n", strcmp("users", "users"));

    db_shutdown();
    return 0;
}
