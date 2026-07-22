#include "src/bridge/db_api.h"
#include "src/query/lexer.h"
#include "src/query/parser.h"
#include <stdio.h>
#include <string.h>

int main() {
    system("rm -rf /tmp/sv_dbt && mkdir -p /tmp/sv_dbt");
    if (!db_init("/tmp/sv_dbt")) { printf("FAIL: db_init\n"); return 1; }

    db_execute("CREATE DATABASE testdb");
    db_execute("USE testdb");
    db_execute("CREATE TABLE users (id INT, nome VARCHAR(50))");

    // Manually lex and parse
    const char* sql = "SELECT * FROM `users`";
    TokenList* tokens = lexer_tokenize(sql);
    printf("Tokens for '%s':\n", sql);
    for (int i = 0; i < tokens->count; i++)
        printf("  [%d] type=%d text='%s'\n", i, tokens->tokens[i].type, tokens->tokens[i].text);

    ParseResult* parsed = parser_parse(tokens);
    if (parsed->success && parsed->count > 0) {
        ASTNode* stmt = parsed->statements[0];
        if (stmt->type == NODE_SELECT) {
            printf("SELECT: table_name='%s'\n", stmt->select_stmt.table_name);
            printf("  num_cols=%d\n", stmt->select_stmt.num_cols);
            printf("  where=%p\n", (void*)stmt->select_stmt.where);
        }
    } else {
        printf("Parse failed: %s\n", parsed->error);
    }

    parse_result_free(parsed);
    lexer_free(tokens);

    // Now try through db_execute
    printf("\nThrough db_execute:\n");
    QueryResult* r = db_execute("SELECT * FROM `users`");
    if (r) {
        printf("  success=%d error='%s'\n", r->success, r->error);
        printf("  num_rows=%d num_cols=%d\n", r->num_rows, r->num_cols);
        db_result_free(r);
    }

    db_shutdown();
    return 0;
}
