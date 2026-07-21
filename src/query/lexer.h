#ifndef LEXER_H
#define LEXER_H

#include "../../include/common.h"

typedef enum {
    TOK_SELECT, TOK_FROM, TOK_WHERE, TOK_INSERT, TOK_INTO,
    TOK_VALUES, TOK_UPDATE, TOK_SET, TOK_DELETE, TOK_CREATE,
    TOK_TABLE, TOK_DATABASE, TOK_DROP, TOK_ALTER, TOK_INDEX,
    TOK_VIEW, TOK_AND, TOK_OR, TOK_NOT, TOK_IN, TOK_IS,
    TOK_NULL, TOK_LIKE, TOK_BETWEEN, TOK_ORDER, TOK_BY,
    TOK_ASC, TOK_DESC, TOK_LIMIT, TOK_JOIN, TOK_LEFT,
    TOK_RIGHT, TOK_ON, TOK_AS, TOK_DISTINCT, TOK_GROUP,
    TOK_HAVING, TOK_UNION, TOK_ALL, TOK_EXISTS, TOK_CASE,
    TOK_WHEN, TOK_THEN, TOK_ELSE, TOK_END, TOK_BEGIN,
    TOK_COMMIT, TOK_ROLLBACK, TOK_TRANSACTION, TOK_USE,
    TOK_SHOW, TOK_DESCRIBE, TOK_EXPLAIN, TOK_TRUNCATE, TOK_KEY,

    TOK_INTEGER, TOK_FLOAT, TOK_STRING, TOK_IDENTIFIER,
    TOK_STAR, TOK_COMMA, TOK_SEMICOLON, TOK_DOT,
    TOK_LPAREN, TOK_RPAREN, TOK_EQ, TOK_NEQ, TOK_LT,
    TOK_GT, TOK_LE, TOK_GE, TOK_PLUS, TOK_MINUS,
    TOK_SLASH, TOK_EOF, TOK_ERROR,
} TokenType;

typedef struct {
    TokenType type;
    char      text[512];
    int       line;
    int       col;
} Token;

typedef struct {
    Token* tokens;
    int    count;
    int    capacity;
} TokenList;

TokenList* lexer_tokenize(const char* sql);
void       lexer_free(TokenList* list);

#endif
