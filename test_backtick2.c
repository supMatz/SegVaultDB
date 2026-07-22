#include "src/query/lexer.h"
#include <stdio.h>
#include <string.h>

int main() {
    TokenList* toks = lexer_tokenize("SELECT * FROM `users`");
    if (!toks) { printf("lexer returned NULL\n"); return 1; }
    for (int i = 0; i < toks->count; i++) {
        printf("token %d: type=%d text='%s'\n", i, toks->tokens[i].type, toks->tokens[i].text);
    }
    return 0;
}
