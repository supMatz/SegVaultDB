#include "parser.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    TokenList* tokens;
    int        pos;
    ParseResult* result;
} ParserState;

static Token peek(ParserState* ps) { return ps->tokens->tokens[ps->pos]; }
static Token consume(ParserState* ps) { return ps->tokens->tokens[ps->pos++]; }
static bool match(ParserState* ps, TokenType tt) {
    if (peek(ps).type == tt) { consume(ps); return true; }
    return false;
}
static bool expect(ParserState* ps, TokenType tt) {
    if (match(ps, tt)) return true;
    snprintf(ps->result->error, sizeof(ps->result->error),
             "Expected token type %d at line %d", tt, peek(ps).line);
    ps->result->success = false;
    return false;
}

static void add_stmt(ParserState* ps, ASTNode* stmt) {
    if (ps->result->count >= ps->result->capacity) {
        ps->result->capacity = ps->result->capacity ? ps->result->capacity * 2 : 16;
        ps->result->statements = realloc(ps->result->statements,
                                          ps->result->capacity * sizeof(ASTNode*));
    }
    ps->result->statements[ps->result->count++] = stmt;
}

static ASTNode* parse_select(ParserState* ps);
static void parse_column_ref(ParserState* ps, ASTExpr* e) {
    Token t = consume(ps);
    if (t.type == TOK_STAR) {
        e->type = EXPR_STAR;
        return;
    }
    e->type = EXPR_COLUMN;
    if (peek(ps).type == TOK_DOT) {
        snprintf(e->table_name, sizeof(e->table_name), "%s", t.text);
        consume(ps);
        Token t2 = consume(ps);
        if (t2.type == TOK_STAR) {
            e->type = EXPR_STAR;
        } else {
            snprintf(e->col_name, sizeof(e->col_name), "%s", t2.text);
        }
    } else {
        snprintf(e->col_name, sizeof(e->col_name), "%s", t.text);
    }
}

static ASTNode* parse_create_trigger(ParserState* ps, ASTNode* n) {
    n->type = NODE_CREATE_TRIGGER;
    Token tg = consume(ps);
    snprintf(n->create_trigger.name, sizeof(n->create_trigger.name), "%s", tg.text);
    if (match(ps, TOK_BEFORE)) n->create_trigger.before = true;
    else if (match(ps, TOK_AFTER)) n->create_trigger.before = false;
    else { n->create_trigger.before = true; }
    Token ev = consume(ps);
    snprintf(n->create_trigger.event, sizeof(n->create_trigger.event), "%s", ev.text);
    expect(ps, TOK_ON);
    Token tbl = consume(ps);
    snprintf(n->create_trigger.table_name, sizeof(n->create_trigger.table_name), "%s", tbl.text);
    match(ps, TOK_FOR);
    match(ps, TOK_EACH);
    match(ps, TOK_ROW);
    char body[SV_MAX_SQL_LEN] = {0};
    int di = 0;
    while (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF && di < (int)sizeof(body) - 2) {
        Token t = consume(ps);
        int tl = strlen(t.text);
        if (di + tl + 1 < (int)sizeof(body)) {
            if (di > 0) body[di++] = ' ';
            memcpy(body + di, t.text, tl);
            di += tl;
        }
    }
    snprintf(n->create_trigger.body, sizeof(n->create_trigger.body), "%s", body);
    return n;
}

static ASTNode* parse_statement(ParserState* ps) {
    ASTNode* n = calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    Token t = peek(ps);

    switch (t.type) {
        case TOK_SELECT:
            free(n);
            return parse_select(ps);
        case TOK_INSERT: {
            consume(ps);
            n->type = NODE_INSERT;
            expect(ps, TOK_INTO);
            Token tn = consume(ps);
            snprintf(n->insert_stmt.table_name, sizeof(n->insert_stmt.table_name), "%s", tn.text);
            if (match(ps, TOK_LPAREN)) {
                n->insert_stmt.num_cols = 0;
                while (peek(ps).type != TOK_RPAREN) {
                    Token c = consume(ps);
                    if (n->insert_stmt.num_cols >= 64) break;
                    n->insert_stmt.columns[n->insert_stmt.num_cols] = strdup(c.text);
                    n->insert_stmt.num_cols++;
                    if (!match(ps, TOK_COMMA)) break;
                }
                expect(ps, TOK_RPAREN);
            }
            expect(ps, TOK_VALUES);
            expect(ps, TOK_LPAREN);
            n->insert_stmt.num_values = 0;
            while (peek(ps).type != TOK_RPAREN && peek(ps).type != TOK_EOF) {
                ASTExpr* e = calloc(1, sizeof(ASTExpr));
                Token v = consume(ps);
                if (v.type == TOK_INTEGER) { e->type = EXPR_INT; e->int_val = atoll(v.text); }
                else if (v.type == TOK_FLOAT) { e->type = EXPR_FLOAT; e->float_val = atof(v.text); }
                else if (v.type == TOK_STRING) { e->type = EXPR_STRING; snprintf(e->str_val, sizeof(e->str_val), "%s", v.text); }
                else if (v.type == TOK_NULL) { e->type = EXPR_INT; e->int_val = 0; }
                n->insert_stmt.values[n->insert_stmt.num_values++] = e;
                if (!match(ps, TOK_COMMA)) break;
            }
            expect(ps, TOK_RPAREN);
            break;
        }
        case TOK_UPDATE: {
            consume(ps);
            n->type = NODE_UPDATE;
            Token tn = consume(ps);
            snprintf(n->update_stmt.table_name, sizeof(n->update_stmt.table_name), "%s", tn.text);
            expect(ps, TOK_SET);
            Token cn = consume(ps);
            snprintf(n->update_stmt.set_col, sizeof(n->update_stmt.set_col), "%s", cn.text);
            expect(ps, TOK_EQ);
            ASTExpr* e = calloc(1, sizeof(ASTExpr));
            Token v = consume(ps);
            if (v.type == TOK_INTEGER) { e->type = EXPR_INT; e->int_val = atoll(v.text); }
            else if (v.type == TOK_STRING) { e->type = EXPR_STRING; snprintf(e->str_val, sizeof(e->str_val), "%s", v.text); }
            n->update_stmt.set_expr = e;
            if (match(ps, TOK_WHERE)) {
                // Simple WHERE: identifier = value
                ASTExpr* w = calloc(1, sizeof(ASTExpr));
                parse_column_ref(ps, w);
                ASTExpr* we = calloc(1, sizeof(ASTExpr));
                we->type = EXPR_BINARY; we->op = OP_EQ; we->left = w;
                consume(ps); /* consume the = operator */
                ASTExpr* wr = calloc(1, sizeof(ASTExpr));
                Token wv = consume(ps);
                if (wv.type == TOK_INTEGER) { wr->type = EXPR_INT; wr->int_val = atoll(wv.text); }
                else if (wv.type == TOK_STRING) { wr->type = EXPR_STRING; snprintf(wr->str_val, sizeof(wr->str_val), "%s", wv.text); }
                we->right = wr;
                n->update_stmt.where = we;
            }
            break;
        }
        case TOK_DELETE: {
            consume(ps);
            n->type = NODE_DELETE;
            expect(ps, TOK_FROM);
            Token tn = consume(ps);
            snprintf(n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), "%s", tn.text);
            if (match(ps, TOK_WHERE)) {
                ASTExpr* w = calloc(1, sizeof(ASTExpr));
                parse_column_ref(ps, w);
                ASTExpr* we = calloc(1, sizeof(ASTExpr));
                we->type = EXPR_BINARY; we->op = OP_EQ; we->left = w;
                match(ps, TOK_EQ);
                ASTExpr* wr = calloc(1, sizeof(ASTExpr));
                Token wv = consume(ps);
                if (wv.type == TOK_INTEGER) { wr->type = EXPR_INT; wr->int_val = atoll(wv.text); }
                else if (wv.type == TOK_STRING) { wr->type = EXPR_STRING; snprintf(wr->str_val, sizeof(wr->str_val), "%s", wv.text); }
                we->right = wr;
                n->delete_stmt.where = we;
            }
            break;
        }
        case TOK_CREATE: {
            consume(ps);
            Token sub = consume(ps);
            if (sub.type == TOK_DATABASE) {
                n->type = NODE_CREATE_DB;
                Token dn = consume(ps);
                snprintf(n->create_db.db_name, sizeof(n->create_db.db_name), "%s", dn.text);
            } else if (sub.type == TOK_TRIGGER) {
                return parse_create_trigger(ps, n);
            } else if (sub.type == TOK_TABLE) {
                n->type = NODE_CREATE_TABLE;
                Token tn = consume(ps);
                snprintf(n->create_table.table_name, sizeof(n->create_table.table_name), "%s", tn.text);
                expect(ps, TOK_LPAREN);
                n->create_table.num_columns = 0;
                while (peek(ps).type != TOK_RPAREN && peek(ps).type != TOK_EOF) {
                    ColumnDef* cd = &n->create_table.columns[n->create_table.num_columns];
                    memset(cd, 0, sizeof(ColumnDef));
                    Token cn = consume(ps);
                    snprintf(cd->name, sizeof(cd->name), "%s", cn.text);
                    Token ct = consume(ps);
                    snprintf(cd->type, sizeof(cd->type), "%s", ct.text);
                    if (ct.type == TOK_INTEGER) { /* INT type */ }
                    if (peek(ps).type == TOK_LPAREN) {
                        consume(ps);
                        Token sz = consume(ps);
                        cd->size = atoi(sz.text);
                        expect(ps, TOK_RPAREN);
                    }
                    while (peek(ps).type == TOK_NOT || peek(ps).type == TOK_NULL ||
                           peek(ps).type == TOK_IDENTIFIER) {
                        Token mod = consume(ps);
                        if (strcmp(mod.text, "NOT") == 0 && match(ps, TOK_NULL)) cd->nullable = false;
                        else if (strcmp(mod.text, "NULL") == 0) cd->nullable = true;
                        else if (strcmp(mod.text, "PRIMARY") == 0 && match(ps, TOK_KEY)) cd->primary_key = true;
                        else if (strcmp(mod.text, "AUTO_INCREMENT") == 0) cd->auto_increment = true;
                        else if (strcmp(mod.text, "DEFAULT") == 0) {
                            Token dv = consume(ps);
                            snprintf(cd->default_val, sizeof(cd->default_val), "%s", dv.text);
                        }
                        else break;
                    }
                    n->create_table.num_columns++;
                    if (!match(ps, TOK_COMMA)) break;
                }
                expect(ps, TOK_RPAREN);
            } else if (sub.type == TOK_INDEX) {
                n->type = NODE_CREATE_INDEX;
                Token in = consume(ps);
                snprintf(n->create_index.index_name, sizeof(n->create_index.index_name), "%s", in.text);
                expect(ps, TOK_ON);
                Token tn = consume(ps);
                snprintf(n->create_index.table_name, sizeof(n->create_index.table_name), "%s", tn.text);
                expect(ps, TOK_LPAREN);
                Token cn = consume(ps);
                snprintf(n->create_index.col_name, sizeof(n->create_index.col_name), "%s", cn.text);
                expect(ps, TOK_RPAREN);
            } else if (sub.type == TOK_VIEW) {
                n->type = NODE_CREATE_VIEW;
                Token vn = consume(ps);
                snprintf(n->create_view.view_name, sizeof(n->create_view.view_name), "%s", vn.text);
                expect(ps, TOK_AS);
                // Store rest as definition
                char def[2048] = {0};
                int di = 0;
                while (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF && di < 2040) {
                    Token t = consume(ps);
                    int tl = strlen(t.text);
                    if (di + tl + 1 < 2048) {
                        if (di > 0) def[di++] = ' ';
                        memcpy(def + di, t.text, tl);
                        di += tl;
                    }
                }
                snprintf(n->create_view.definition, sizeof(n->create_view.definition), "%s", def);
            }
            break;
        }
        case TOK_ALTER: {
            consume(ps);
            expect(ps, TOK_TABLE);
            n->type = NODE_ALTER_TABLE;
            Token tn = consume(ps);
            snprintf(n->alter_table.table_name, sizeof(n->alter_table.table_name), "%s", tn.text);
            if (match(ps, TOK_RENAME)) {
                expect(ps, TOK_TO);
                Token nn = consume(ps);
                snprintf(n->alter_table.new_name, sizeof(n->alter_table.new_name), "%s", nn.text);
            }
            break;
        }
        case TOK_DROP: {
            consume(ps);
            Token sub = consume(ps);
            if (sub.type == TOK_DATABASE) {
                n->type = NODE_DROP_DB;
                Token dn = consume(ps);
                snprintf(n->drop_db.db_name, sizeof(n->drop_db.db_name), "%s", dn.text);
            } else if (sub.type == TOK_TABLE) {
                n->type = NODE_DROP_TABLE;
                Token tn = consume(ps);
                snprintf(n->drop_table.table_name, sizeof(n->drop_table.table_name), "%s", tn.text);
            } else if (sub.type == TOK_VIEW) {
                n->type = NODE_DROP_VIEW;
                Token vn = consume(ps);
                snprintf(n->drop_view.view_name, sizeof(n->drop_view.view_name), "%s", vn.text);
            } else if (sub.type == TOK_INDEX) {
                n->type = NODE_DROP_INDEX;
                Token in = consume(ps);
                snprintf(n->drop_index.index_name, sizeof(n->drop_index.index_name), "%s", in.text);
                expect(ps, TOK_ON);
                Token tn = consume(ps);
                snprintf(n->drop_index.table_name, sizeof(n->drop_index.table_name), "%s", tn.text);
            } else if (sub.type == TOK_TRIGGER) {
                n->type = NODE_DROP_TRIGGER;
                Token tg = consume(ps);
                snprintf(n->drop_trigger.name, sizeof(n->drop_trigger.name), "%s", tg.text);
            }
            break;
        }
        case TOK_USE: {
            consume(ps);
            n->type = NODE_USE;
            Token dn = consume(ps);
            snprintf(n->use_stmt.db_name, sizeof(n->use_stmt.db_name), "%s", dn.text);
            break;
        }
        case TOK_SHOW: {
            consume(ps);
            n->type = NODE_SHOW;
            break;
        }
        case TOK_DESCRIBE: {
            consume(ps);
            n->type = NODE_DESCRIBE;
            Token tn = consume(ps);
            if (peek(ps).type == TOK_IDENTIFIER) {
                // table name stored in select_stmt.table_name for convenience
                snprintf(n->select_stmt.table_name, sizeof(n->select_stmt.table_name), "%s", tn.text);
            }
            break;
        }
        case TOK_EXPLAIN: {
            consume(ps);
            n->type = NODE_EXPLAIN;
            break;
        }
        case TOK_BEGIN: {
            consume(ps);
            n->type = NODE_BEGIN;
            match(ps, TOK_TRANSACTION);
            break;
        }
        case TOK_COMMIT: {
            consume(ps);
            n->type = NODE_COMMIT;
            break;
        }
        case TOK_ROLLBACK: {
            consume(ps);
            if (match(ps, TOK_TO)) {
                n->type = NODE_ROLLBACK_TO;
                Token sn = consume(ps);
                snprintf(n->rollback_to.name, sizeof(n->rollback_to.name), "%s", sn.text);
            } else {
                n->type = NODE_ROLLBACK;
            }
            break;
        }
        case TOK_SAVEPOINT: {
            consume(ps);
            n->type = NODE_SAVEPOINT;
            Token sn = consume(ps);
            snprintf(n->savepoint.name, sizeof(n->savepoint.name), "%s", sn.text);
            break;
        }
        case TOK_TRUNCATE: {
            consume(ps);
            n->type = NODE_TRUNCATE;
            expect(ps, TOK_TABLE);
            Token tn = consume(ps);
            snprintf(n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), "%s", tn.text);
            break;
        }
        default:
            snprintf(ps->result->error, sizeof(ps->result->error),
                     "Unexpected token: %s at line %d", t.text, t.line);
            ps->result->success = false;
            free(n);
            return NULL;
    }
    return n;
}

static ASTNode* parse_select(ParserState* ps) {
    ASTNode* n = calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    n->type = NODE_SELECT;
    consume(ps);

    if (match(ps, TOK_DISTINCT)) n->select_stmt.distinct = true;

    // Columns
    n->select_stmt.num_cols = 0;
    while (peek(ps).type != TOK_FROM && peek(ps).type != TOK_EOF) {
        ASTExpr* e = calloc(1, sizeof(ASTExpr));
        parse_column_ref(ps, e);
        n->select_stmt.columns[n->select_stmt.num_cols++] = e;
        if (!match(ps, TOK_COMMA)) break;
    }

    expect(ps, TOK_FROM);

    // Handle JOIN: comma-separated tables or JOIN keyword
    n->select_stmt.num_joins = 0;
    bool first_table = true;

    while (1) {
        Token tn = consume(ps);
        if (first_table) {
            snprintf(n->select_stmt.table_name, sizeof(n->select_stmt.table_name), "%s", tn.text);
            first_table = false;
        } else {
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            snprintf(j->table_name, sizeof(j->table_name), "%s", tn.text);
            j->type = JOIN_INNER;
        }

        if (match(ps, TOK_JOIN)) {
            // JOIN table ON condition (next iteration handles the table name)
            Token jt = consume(ps);
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            snprintf(j->table_name, sizeof(j->table_name), "%s", jt.text);
            j->type = JOIN_INNER;
            if (match(ps, TOK_ON)) {
                ASTExpr* cond = calloc(1, sizeof(ASTExpr));
                ASTExpr* left = calloc(1, sizeof(ASTExpr));
                parse_column_ref(ps, left);
                match(ps, TOK_EQ);
                ASTExpr* right = calloc(1, sizeof(ASTExpr));
                parse_column_ref(ps, right);
                cond->type = EXPR_BINARY; cond->op = OP_EQ; cond->left = left; cond->right = right;
                j->condition = cond;
            }
        } else if (match(ps, TOK_LEFT) || match(ps, TOK_RIGHT)) {
            expect(ps, TOK_JOIN);
            Token jt = consume(ps);
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            snprintf(j->table_name, sizeof(j->table_name), "%s", jt.text);
            j->type = JOIN_LEFT;
            if (match(ps, TOK_ON)) {
                ASTExpr* cond = calloc(1, sizeof(ASTExpr));
                ASTExpr* left = calloc(1, sizeof(ASTExpr));
                parse_column_ref(ps, left);
                match(ps, TOK_EQ);
                ASTExpr* right = calloc(1, sizeof(ASTExpr));
                parse_column_ref(ps, right);
                cond->type = EXPR_BINARY; cond->op = OP_EQ; cond->left = left; cond->right = right;
                j->condition = cond;
            }
        } else if (match(ps, TOK_COMMA)) {
            continue;
        }
        break;
    }

    if (match(ps, TOK_WHERE)) {
        ASTExpr* w = calloc(1, sizeof(ASTExpr));
        w->type = EXPR_BINARY;
        ASTExpr* left = calloc(1, sizeof(ASTExpr));
        parse_column_ref(ps, left);
        w->left = left;
        if (match(ps, TOK_EQ)) w->op = OP_EQ;
        else if (match(ps, TOK_NEQ)) w->op = OP_NEQ;
        else if (match(ps, TOK_LT)) w->op = OP_LT;
        else if (match(ps, TOK_GT)) w->op = OP_GT;
        else if (match(ps, TOK_LE)) w->op = OP_LE;
        else if (match(ps, TOK_GE)) w->op = OP_GE;
        else w->op = OP_EQ;
        ASTExpr* right = calloc(1, sizeof(ASTExpr));
        if (peek(ps).type == TOK_INTEGER) { Token v = consume(ps); right->type = EXPR_INT; right->int_val = atoll(v.text); }
        else if (peek(ps).type == TOK_FLOAT) { Token v = consume(ps); right->type = EXPR_FLOAT; right->float_val = atof(v.text); }
        else if (peek(ps).type == TOK_STRING) { Token v = consume(ps); right->type = EXPR_STRING; snprintf(right->str_val, sizeof(right->str_val), "%s", v.text); }
        else { parse_column_ref(ps, right); }
        w->right = right;
        n->select_stmt.where = w;
    }

    if (match(ps, TOK_GROUP)) {
        expect(ps, TOK_BY);
        n->select_stmt.num_group_cols = 0;
        while (1) {
            Token gc = consume(ps);
            snprintf(n->select_stmt.group_cols[n->select_stmt.num_group_cols++],
                     sizeof(n->select_stmt.group_cols[0]), "%s", gc.text);
            if (!match(ps, TOK_COMMA)) break;
        }
    }

    if (match(ps, TOK_HAVING)) {
        ASTExpr* h = calloc(1, sizeof(ASTExpr));
        ASTExpr* left = calloc(1, sizeof(ASTExpr));
        parse_column_ref(ps, left);
        h->type = EXPR_BINARY; h->left = left;
        match(ps, TOK_EQ);
        ASTExpr* right = calloc(1, sizeof(ASTExpr));
        if (peek(ps).type == TOK_INTEGER) { Token v = consume(ps); right->type = EXPR_INT; right->int_val = atoll(v.text); }
        else { parse_column_ref(ps, right); }
        h->op = OP_EQ; h->right = right;
        n->select_stmt.having = h;
    }

    if (match(ps, TOK_ORDER)) {
        expect(ps, TOK_BY);
        Token oc = consume(ps);
        snprintf(n->select_stmt.order_col, sizeof(n->select_stmt.order_col), "%s", oc.text);
        n->select_stmt.order_asc = true;
        if (match(ps, TOK_DESC)) n->select_stmt.order_asc = false;
    }

    if (match(ps, TOK_LIMIT)) {
        Token lv = consume(ps);
        n->select_stmt.limit = atoi(lv.text);
    }

    return n;
}

ParseResult* parser_parse(TokenList* tokens) {
    ParseResult* r = calloc(1, sizeof(ParseResult));
    if (!r) return NULL;
    r->success = true;
    r->count = 0;
    r->capacity = 0;
    r->statements = NULL;

    ParserState ps;
    ps.tokens = tokens;
    ps.pos = 0;
    ps.result = r;

    while (peek(&ps).type != TOK_EOF) {
        ASTNode* stmt = parse_statement(&ps);
        if (stmt) add_stmt(&ps, stmt);
        match(&ps, TOK_SEMICOLON);
        if (!ps.result->success) break;
    }

    return r;
}

void parse_result_free(ParseResult* r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        ASTNode* n = r->statements[i];
        if (!n) continue;
        // Free expressions
        if (n->type == NODE_SELECT) {
            for (int j = 0; j < n->select_stmt.num_cols; j++)
                free(n->select_stmt.columns[j]);
            if (n->select_stmt.where) {
                free(n->select_stmt.where->left);
                free(n->select_stmt.where->right);
                free(n->select_stmt.where);
            }
        } else if (n->type == NODE_INSERT) {
            for (int j = 0; j < n->insert_stmt.num_cols; j++)
                free(n->insert_stmt.columns[j]);
            for (int j = 0; j < n->insert_stmt.num_values; j++)
                free(n->insert_stmt.values[j]);
        } else if (n->type == NODE_UPDATE) {
            free(n->update_stmt.set_expr);
            if (n->update_stmt.where) {
                free(n->update_stmt.where->left);
                free(n->update_stmt.where->right);
                free(n->update_stmt.where);
            }
        } else if (n->type == NODE_DELETE) {
            if (n->delete_stmt.where) {
                free(n->delete_stmt.where->left);
                free(n->delete_stmt.where->right);
                free(n->delete_stmt.where);
            }
        }
        free(n);
    }
    free(r->statements);
    free(r);
}
