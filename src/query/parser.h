#ifndef PARSER_H
#define PARSER_H

#include "../../include/common.h"
#include "lexer.h"

typedef enum {
    NODE_SELECT, NODE_INSERT, NODE_UPDATE, NODE_DELETE,
    NODE_CREATE_DB, NODE_CREATE_TABLE, NODE_CREATE_INDEX, NODE_CREATE_VIEW,
    NODE_DROP_DB, NODE_DROP_TABLE, NODE_DROP_VIEW, NODE_DROP_INDEX,
    NODE_ALTER_TABLE, NODE_USE, NODE_SHOW, NODE_DESCRIBE,
    NODE_EXPLAIN, NODE_BEGIN, NODE_COMMIT, NODE_ROLLBACK,
    NODE_TRUNCATE,
} StmtType;

typedef enum { EXPR_COLUMN, EXPR_INT, EXPR_FLOAT, EXPR_STRING, EXPR_STAR,
               EXPR_BINARY, EXPR_UNARY } ExprType;

typedef enum { OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
               OP_AND, OP_OR, OP_NOT, OP_PLUS, OP_MINUS, OP_MUL, OP_DIV } OpType;

typedef struct ASTExpr {
    ExprType type;
    OpType   op;
    char     col_name[64];
    char     table_name[64];
    union {
        int64_t  int_val;
        double   float_val;
        char     str_val[512];
    };
    struct ASTExpr* left;
    struct ASTExpr* right;
} ASTExpr;

typedef struct {
    char     name[64];
    char     type[32];
    int      size;
    bool     nullable;
    bool     primary_key;
    bool     auto_increment;
    char     default_val[128];
} ColumnDef;

typedef struct {
    StmtType type;
    union {
        struct {
            ASTExpr**  columns;
            int        num_cols;
            char       table_name[64];
            ASTExpr*   where;
            char       order_col[64];
            bool       order_asc;
            int        limit;
            bool       distinct;
        } select_stmt;
        struct {
            char       table_name[64];
            char**     columns;
            int        num_cols;
            ASTExpr**  values;
            int        num_values;
        } insert_stmt;
        struct {
            char       table_name[64];
            char       set_col[64];
            ASTExpr*   set_expr;
            ASTExpr*   where;
        } update_stmt;
        struct {
            char       table_name[64];
            ASTExpr*   where;
        } delete_stmt;
        struct {
            char       db_name[64];
        } create_db;
        struct {
            char       db_name[64];
        } drop_db;
        struct {
            char       table_name[64];
            ColumnDef  columns[64];
            int        num_columns;
        } create_table;
        struct {
            char       table_name[64];
            char       new_name[64];
        } alter_table;
        struct {
            char       table_name[64];
        } drop_table;
        struct {
            char       index_name[64];
            char       table_name[64];
            char       col_name[64];
        } create_index;
        struct {
            char       index_name[64];
            char       table_name[64];
        } drop_index;
        struct {
            char       view_name[64];
            char       definition[2048];
        } create_view;
        struct {
            char       view_name[64];
        } drop_view;
        struct {
            char       db_name[64];
        } use_stmt;
    };
} ASTNode;

typedef struct {
    ASTNode** statements;
    int       count;
    int       capacity;
    bool      success;
    char      error[256];
} ParseResult;

ParseResult* parser_parse(TokenList* tokens);
void        parse_result_free(ParseResult* r);

#endif
