#include "src/query/lexer.h"
#include <stdio.h>

int main() {
    const char* tests[] = {
        "SELECT * FROM `users`",
        "SELECT * FROM `users`;",
        "SELECT * FROM `users` LIMIT 100;",
        NULL
    };
    for (int t = 0; tests[t]; t++) {
        printf("Input: '%s'\n", tests[t]);
        TokenList* toks = lexer_tokenize(tests[t]);
        if (!toks) { printf("  lexer returned NULL\n"); continue; }
        for (int i = 0; i < toks->count; i++) {
            printf("  [%d] type=%d text='%s'\n", i, toks->tokens[i].type, toks->tokens[i].text);
        }
        lexer_free(toks);
        printf("\n");
    }
    return 0;
}
