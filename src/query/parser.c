#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>

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


// copia un testo del token in un campo dell'AST; se non entra lo statement viene rifiutato
// (troncarlo in silenzio poteva far coincidere due nomi lunghi diversi)
static void copy_text(ParserState* ps, char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) {
        if (ps->result->success) {
            snprintf(ps->result->error, sizeof(ps->result->error),
                     "Name or value too long (max %d characters): %.40s...", (int)cap - 1, src);
            ps->result->success = false;
        }
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// "tabella.colonna" in un campo di dimensione fissa, con lo stesso controllo
static void copy_qualified(ParserState* ps, char* dst, size_t cap, const char* table, const char* col) {
    if (strlen(table) + 1 + strlen(col) >= cap) {
        copy_text(ps, dst, cap, "");
        if (ps->result->success) {
            snprintf(ps->result->error, sizeof(ps->result->error),
                     "Qualified name too long (max %d characters): %.30s.%.30s", (int)cap - 1, table, col);
            ps->result->success = false;
        }
        return;
    }
    size_t tl = strlen(table), cl = strlen(col);   // lunghezza già verificata sopra
    memcpy(dst, table, tl);
    dst[tl] = '.';
    memcpy(dst + tl + 1, col, cl + 1);
}

static ASTNode* parse_select(ParserState* ps);
static void parse_column_ref(ParserState* ps, ASTExpr* e);

// ═════════════════════════════════════════════════════════════════
//  Condizioni (WHERE / HAVING)
//    condizione := and_expr { OR and_expr }
//    and_expr   := not_expr { AND not_expr }
//    not_expr   := NOT not_expr | confronto
//    confronto  := '(' condizione ')'
//                | operando ( = | != | <> | < | > | <= | >= ) operando
//                | operando IS [NOT] NULL
//                | operando [NOT] LIKE stringa
//                | operando [NOT] BETWEEN operando AND operando
//                | operando [NOT] IN '(' operando { , operando } ')'
//    operando   := numero | stringa | NULL | [tabella.]colonna
// ═════════════════════════════════════════════════════════════════

void ast_expr_free(ASTExpr* e) {
    if (!e) return;
    ast_expr_free(e->left);
    ast_expr_free(e->right);
    free(e);
}

static void parse_fail(ParserState* ps, const char* msg, Token at) {
    if (!ps->result->success) return;
    snprintf(ps->result->error, sizeof(ps->result->error), "%s near '%.60s' at line %d", msg, at.text, at.line);
    ps->result->success = false;
}

static ASTExpr* new_expr(ExprType type) {
    ASTExpr* e = calloc(1, sizeof(ASTExpr));
    if (e) e->type = type;
    return e;
}

static ASTExpr* new_binary(OpType op, ASTExpr* l, ASTExpr* r) {
    ASTExpr* e = new_expr(EXPR_BINARY);
    if (!e) { ast_expr_free(l); ast_expr_free(r); return NULL; }
    e->op = op; e->left = l; e->right = r;
    return e;
}

static bool is_aggregate_name(const char* s) {
    static const char* AGG[] = { "COUNT", "SUM", "AVG", "MIN", "MAX" };
    for (size_t i = 0; i < sizeof(AGG) / sizeof(AGG[0]); i++)
        if (strcasecmp(s, AGG[i]) == 0) return true;
    return false;
}

// COUNT(*) | COUNT(col) | SUM(col) | AVG(col) | MIN(col) | MAX(col)
static bool at_aggregate(ParserState* ps) {
    Token t = peek(ps);
    if (t.type != TOK_IDENTIFIER || !is_aggregate_name(t.text)) return false;
    return ps->pos + 1 < ps->tokens->count && ps->tokens->tokens[ps->pos + 1].type == TOK_LPAREN;
}

static ASTExpr* parse_aggregate(ParserState* ps) {
    Token fn = consume(ps);
    consume(ps);   // '('
    ASTExpr* e = new_expr(EXPR_AGG);
    if (!e) return NULL;
    for (int i = 0; fn.text[i] && i < (int)sizeof(e->func) - 1; i++) e->func[i] = (char)toupper((unsigned char)fn.text[i]);
    if (match(ps, TOK_STAR)) {
        if (strcmp(e->func, "COUNT") != 0) parse_fail(ps, "Only COUNT accepts *", fn);
        strcpy(e->col_name, "*");
    } else {
        ASTExpr arg;
        memset(&arg, 0, sizeof(arg));
        parse_column_ref(ps, &arg);
        copy_text(ps, e->col_name, sizeof(e->col_name), arg.col_name);
        copy_text(ps, e->table_name, sizeof(e->table_name), arg.table_name);
    }
    if (!match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' after aggregate argument", peek(ps));
    return e;
}

static ASTExpr* parse_operand(ParserState* ps) {
    if (at_aggregate(ps)) return parse_aggregate(ps);
    Token t = peek(ps);
    ASTExpr* e = NULL;
    switch (t.type) {
        case TOK_INTEGER: consume(ps); e = new_expr(EXPR_INT);   if (e) e->int_val = atoll(t.text); break;
        case TOK_FLOAT:   consume(ps); e = new_expr(EXPR_FLOAT); if (e) e->float_val = atof(t.text); break;
        case TOK_STRING:  consume(ps); e = new_expr(EXPR_STRING); if (e) copy_text(ps, e->str_val, sizeof(e->str_val), t.text); break;
        case TOK_NULL:    consume(ps); e = new_expr(EXPR_NULL); break;
        case TOK_IDENTIFIER:
            e = new_expr(EXPR_COLUMN);
            if (e) parse_column_ref(ps, e);
            break;
        default:
            parse_fail(ps, "Expected a value or a column", t);
            if (t.type != TOK_EOF) consume(ps);
            return NULL;
    }
    return e;
}

static ASTExpr* parse_condition(ParserState* ps);

static ASTExpr* parse_comparison(ParserState* ps) {
    if (match(ps, TOK_LPAREN)) {
        ASTExpr* inner = parse_condition(ps);
        if (!match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')'", peek(ps));
        return inner;
    }
    ASTExpr* left = parse_operand(ps);
    if (!left) return NULL;

    if (match(ps, TOK_IS)) {
        bool neg = match(ps, TOK_NOT);
        if (!match(ps, TOK_NULL)) { parse_fail(ps, "Expected NULL after IS", peek(ps)); ast_expr_free(left); return NULL; }
        ASTExpr* e = new_expr(EXPR_UNARY);
        if (!e) { ast_expr_free(left); return NULL; }
        e->op = neg ? OP_IS_NOT_NULL : OP_IS_NULL;
        e->left = left;
        return e;
    }
    bool neg = match(ps, TOK_NOT);
    if (match(ps, TOK_LIKE)) {
        ASTExpr* pat = parse_operand(ps);
        return new_binary(neg ? OP_NOT_LIKE : OP_LIKE, left, pat);
    }
    if (match(ps, TOK_BETWEEN)) {
        // x BETWEEN a AND b  ->  x >= a AND x <= b   (NOT: x < a OR x > b)
        ASTExpr* lo = parse_operand(ps);
        if (!match(ps, TOK_AND)) parse_fail(ps, "Expected AND in BETWEEN", peek(ps));
        ASTExpr* hi = parse_operand(ps);
        ASTExpr* left2 = new_expr(left->type);
        if (left2) *left2 = *left;
        if (neg) return new_binary(OP_OR, new_binary(OP_LT, left, lo), new_binary(OP_GT, left2, hi));
        return new_binary(OP_AND, new_binary(OP_GE, left, lo), new_binary(OP_LE, left2, hi));
    }
    if (match(ps, TOK_IN)) {
        // x IN (a, b, c)  ->  x = a OR x = b OR x = c   (NOT: x != a AND x != b ...)
        if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' after IN", peek(ps)); ast_expr_free(left); return NULL; }
        ASTExpr* acc = NULL;
        bool first = true;
        do {
            ASTExpr* v = parse_operand(ps);
            if (!v) break;
            ASTExpr* l = left;
            if (!first) { l = new_expr(left->type); if (l) *l = *left; }
            first = false;
            ASTExpr* cmp = new_binary(neg ? OP_NEQ : OP_EQ, l, v);
            acc = acc ? new_binary(neg ? OP_AND : OP_OR, acc, cmp) : cmp;
        } while (match(ps, TOK_COMMA));
        if (!match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' to close IN list", peek(ps));
        if (first) ast_expr_free(left);
        return acc;
    }
    if (neg) { parse_fail(ps, "Expected LIKE, BETWEEN or IN after NOT", peek(ps)); ast_expr_free(left); return NULL; }

    OpType op;
    if      (match(ps, TOK_EQ))  op = OP_EQ;
    else if (match(ps, TOK_NEQ)) op = OP_NEQ;
    else if (match(ps, TOK_LT))  op = OP_LT;
    else if (match(ps, TOK_GT))  op = OP_GT;
    else if (match(ps, TOK_LE))  op = OP_LE;
    else if (match(ps, TOK_GE))  op = OP_GE;
    else { parse_fail(ps, "Expected a comparison operator", peek(ps)); ast_expr_free(left); return NULL; }
    ASTExpr* right = parse_operand(ps);
    return new_binary(op, left, right);
}

static ASTExpr* parse_not(ParserState* ps) {
    if (match(ps, TOK_NOT)) {
        ASTExpr* e = new_expr(EXPR_UNARY);
        ASTExpr* child = parse_not(ps);
        if (!e) { ast_expr_free(child); return NULL; }
        e->op = OP_NOT;
        e->left = child;
        return e;
    }
    return parse_comparison(ps);
}

static ASTExpr* parse_and(ParserState* ps) {
    ASTExpr* e = parse_not(ps);
    while (ps->result->success && match(ps, TOK_AND)) e = new_binary(OP_AND, e, parse_not(ps));
    return e;
}

static ASTExpr* parse_condition(ParserState* ps) {
    ASTExpr* e = parse_and(ps);
    while (ps->result->success && match(ps, TOK_OR)) e = new_binary(OP_OR, e, parse_and(ps));
    return e;
}
static void parse_column_ref(ParserState* ps, ASTExpr* e) {
    Token t = consume(ps);
    if (t.type == TOK_STAR) {
        e->type = EXPR_STAR;
        return;
    }
    e->type = EXPR_COLUMN;
    if (peek(ps).type == TOK_DOT) {
        copy_text(ps, e->table_name, sizeof(e->table_name), t.text);
        consume(ps);
        Token t2 = consume(ps);
        if (t2.type == TOK_STAR) {
            e->type = EXPR_STAR;
        } else {
            copy_text(ps, e->col_name, sizeof(e->col_name), t2.text);
        }
    } else {
        copy_text(ps, e->col_name, sizeof(e->col_name), t.text);
    }
}

static ASTNode* parse_create_trigger(ParserState* ps, ASTNode* n) {
    n->type = NODE_CREATE_TRIGGER;
    Token tg = consume(ps);
    copy_text(ps, n->create_trigger.name, sizeof(n->create_trigger.name), tg.text);
    if (match(ps, TOK_BEFORE)) n->create_trigger.before = true;
    else if (match(ps, TOK_AFTER)) n->create_trigger.before = false;
    else { n->create_trigger.before = true; }
    Token ev = consume(ps);
    copy_text(ps, n->create_trigger.event, sizeof(n->create_trigger.event), ev.text);
    expect(ps, TOK_ON);
    Token tbl = consume(ps);
    copy_text(ps, n->create_trigger.table_name, sizeof(n->create_trigger.table_name), tbl.text);
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
    copy_text(ps, n->create_trigger.body, sizeof(n->create_trigger.body), body);
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
            copy_text(ps, n->insert_stmt.table_name, sizeof(n->insert_stmt.table_name), tn.text);
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
                else if (v.type == TOK_STRING) { e->type = EXPR_STRING; copy_text(ps, e->str_val, sizeof(e->str_val), v.text); }
                else if (v.type == TOK_NULL) { e->type = EXPR_NULL; }
                else parse_fail(ps, "Expected a value", v);
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
            copy_text(ps, n->update_stmt.table_name, sizeof(n->update_stmt.table_name), tn.text);
            expect(ps, TOK_SET);
            Token cn = consume(ps);
            copy_text(ps, n->update_stmt.set_col, sizeof(n->update_stmt.set_col), cn.text);
            expect(ps, TOK_EQ);
            ASTExpr* e = calloc(1, sizeof(ASTExpr));
            Token v = consume(ps);
            if (v.type == TOK_INTEGER) { e->type = EXPR_INT; e->int_val = atoll(v.text); }
            else if (v.type == TOK_FLOAT) { e->type = EXPR_FLOAT; e->float_val = atof(v.text); }
            else if (v.type == TOK_STRING) { e->type = EXPR_STRING; copy_text(ps, e->str_val, sizeof(e->str_val), v.text); }
            else if (v.type == TOK_NULL) { e->type = EXPR_NULL; }
            else parse_fail(ps, "Expected a value after '='", v);
            n->update_stmt.set_expr = e;
            if (match(ps, TOK_WHERE)) n->update_stmt.where = parse_condition(ps);
            break;
        }
        case TOK_DELETE: {
            consume(ps);
            n->type = NODE_DELETE;
            expect(ps, TOK_FROM);
            Token tn = consume(ps);
            copy_text(ps, n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), tn.text);
            if (match(ps, TOK_WHERE)) n->delete_stmt.where = parse_condition(ps);
            break;
        }
        case TOK_CREATE: {
            consume(ps);
            Token sub = consume(ps);
            if (sub.type == TOK_DATABASE) {
                n->type = NODE_CREATE_DB;
                Token dn = consume(ps);
                copy_text(ps, n->create_db.db_name, sizeof(n->create_db.db_name), dn.text);
            } else if (sub.type == TOK_TRIGGER) {
                return parse_create_trigger(ps, n);
            } else if (sub.type == TOK_TABLE) {
                n->type = NODE_CREATE_TABLE;
                Token tn = consume(ps);
                copy_text(ps, n->create_table.table_name, sizeof(n->create_table.table_name), tn.text);
                expect(ps, TOK_LPAREN);
                n->create_table.num_columns = 0;
                while (peek(ps).type != TOK_RPAREN && peek(ps).type != TOK_EOF) {
                    ColumnDef* cd = &n->create_table.columns[n->create_table.num_columns];
                    memset(cd, 0, sizeof(ColumnDef));
                    Token cn = consume(ps);
                    copy_text(ps, cd->name, sizeof(cd->name), cn.text);
                    Token ct = consume(ps);
                    copy_text(ps, cd->type, sizeof(cd->type), ct.text);
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
                            copy_text(ps, cd->default_val, sizeof(cd->default_val), dv.text);
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
                copy_text(ps, n->create_index.index_name, sizeof(n->create_index.index_name), in.text);
                expect(ps, TOK_ON);
                Token tn = consume(ps);
                copy_text(ps, n->create_index.table_name, sizeof(n->create_index.table_name), tn.text);
                expect(ps, TOK_LPAREN);
                Token cn = consume(ps);
                copy_text(ps, n->create_index.col_name, sizeof(n->create_index.col_name), cn.text);
                expect(ps, TOK_RPAREN);
            } else if (sub.type == TOK_VIEW) {
                n->type = NODE_CREATE_VIEW;
                Token vn = consume(ps);
                copy_text(ps, n->create_view.view_name, sizeof(n->create_view.view_name), vn.text);
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
                copy_text(ps, n->create_view.definition, sizeof(n->create_view.definition), def);
            }
            break;
        }
        case TOK_ALTER: {
            consume(ps);
            expect(ps, TOK_TABLE);
            n->type = NODE_ALTER_TABLE;
            Token tn = consume(ps);
            copy_text(ps, n->alter_table.table_name, sizeof(n->alter_table.table_name), tn.text);
            if (match(ps, TOK_RENAME)) {
                expect(ps, TOK_TO);
                Token nn = consume(ps);
                copy_text(ps, n->alter_table.new_name, sizeof(n->alter_table.new_name), nn.text);
            }
            break;
        }
        case TOK_DROP: {
            consume(ps);
            Token sub = consume(ps);
            if (sub.type == TOK_DATABASE) {
                n->type = NODE_DROP_DB;
                Token dn = consume(ps);
                copy_text(ps, n->drop_db.db_name, sizeof(n->drop_db.db_name), dn.text);
            } else if (sub.type == TOK_TABLE) {
                n->type = NODE_DROP_TABLE;
                Token tn = consume(ps);
                copy_text(ps, n->drop_table.table_name, sizeof(n->drop_table.table_name), tn.text);
            } else if (sub.type == TOK_VIEW) {
                n->type = NODE_DROP_VIEW;
                Token vn = consume(ps);
                copy_text(ps, n->drop_view.view_name, sizeof(n->drop_view.view_name), vn.text);
            } else if (sub.type == TOK_INDEX) {
                n->type = NODE_DROP_INDEX;
                Token in = consume(ps);
                copy_text(ps, n->drop_index.index_name, sizeof(n->drop_index.index_name), in.text);
                expect(ps, TOK_ON);
                Token tn = consume(ps);
                copy_text(ps, n->drop_index.table_name, sizeof(n->drop_index.table_name), tn.text);
            } else if (sub.type == TOK_TRIGGER) {
                n->type = NODE_DROP_TRIGGER;
                Token tg = consume(ps);
                copy_text(ps, n->drop_trigger.name, sizeof(n->drop_trigger.name), tg.text);
            }
            break;
        }
        case TOK_USE: {
            consume(ps);
            n->type = NODE_USE;
            Token dn = consume(ps);
            copy_text(ps, n->use_stmt.db_name, sizeof(n->use_stmt.db_name), dn.text);
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
                copy_text(ps, n->select_stmt.table_name, sizeof(n->select_stmt.table_name), tn.text);
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
                copy_text(ps, n->rollback_to.name, sizeof(n->rollback_to.name), sn.text);
            } else {
                n->type = NODE_ROLLBACK;
            }
            break;
        }
        case TOK_SAVEPOINT: {
            consume(ps);
            n->type = NODE_SAVEPOINT;
            Token sn = consume(ps);
            copy_text(ps, n->savepoint.name, sizeof(n->savepoint.name), sn.text);
            break;
        }
        case TOK_TRUNCATE: {
            consume(ps);
            n->type = NODE_TRUNCATE;
            expect(ps, TOK_TABLE);
            Token tn = consume(ps);
            copy_text(ps, n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), tn.text);
            break;
        }
        default:
            snprintf(ps->result->error, sizeof(ps->result->error),
                     "Unexpected token: %.200s at line %d", t.text, t.line);
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

    // Columns: colonne, * e aggregati, con alias opzionale ("AS nome" o "nome")
    n->select_stmt.num_cols = 0;
    while (peek(ps).type != TOK_FROM && peek(ps).type != TOK_EOF) {
        if (n->select_stmt.num_cols >= 64) { parse_fail(ps, "Too many columns in SELECT (max 64)", peek(ps)); break; }
        ASTExpr* e;
        if (at_aggregate(ps)) {
            e = parse_aggregate(ps);
        } else {
            e = calloc(1, sizeof(ASTExpr));
            if (e) parse_column_ref(ps, e);
        }
        if (!e) break;
        if (match(ps, TOK_AS) || peek(ps).type == TOK_IDENTIFIER) {
            Token al = consume(ps);
            copy_text(ps, e->alias, sizeof(e->alias), al.text);
        }
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
            copy_text(ps, n->select_stmt.table_name, sizeof(n->select_stmt.table_name), tn.text);
            first_table = false;
        } else {
            if (n->select_stmt.num_joins >= 8) { parse_fail(ps, "Too many JOINs (max 8)", peek(ps)); break; }
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            copy_text(ps, j->table_name, sizeof(j->table_name), tn.text);
            j->type = JOIN_INNER;
        }

        if (match(ps, TOK_JOIN)) {
            Token jt = consume(ps);
            if (n->select_stmt.num_joins >= 8) { parse_fail(ps, "Too many JOINs (max 8)", peek(ps)); break; }
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            copy_text(ps, j->table_name, sizeof(j->table_name), jt.text);
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
        } else if (match(ps, TOK_LEFT)) {
            expect(ps, TOK_JOIN);
            Token jt = consume(ps);
            if (n->select_stmt.num_joins >= 8) { parse_fail(ps, "Too many JOINs (max 8)", peek(ps)); break; }
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            copy_text(ps, j->table_name, sizeof(j->table_name), jt.text);
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
        } else if (match(ps, TOK_RIGHT)) {
            expect(ps, TOK_JOIN);
            Token jt = consume(ps);
            if (n->select_stmt.num_joins >= 8) { parse_fail(ps, "Too many JOINs (max 8)", peek(ps)); break; }
            ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
            memset(j, 0, sizeof(ASTJoin));
            copy_text(ps, j->table_name, sizeof(j->table_name), jt.text);
            j->type = JOIN_RIGHT;
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
        if (match(ps, TOK_COMMA)) continue;
        TokenType ptype = peek(ps).type;
        if (ptype == TOK_JOIN || ptype == TOK_LEFT || ptype == TOK_RIGHT) continue;
        break;
    }

    if (match(ps, TOK_WHERE)) n->select_stmt.where = parse_condition(ps);

    if (match(ps, TOK_GROUP)) {
        expect(ps, TOK_BY);
        n->select_stmt.num_group_cols = 0;
        while (1) {
            if (n->select_stmt.num_group_cols >= 64) { parse_fail(ps, "Too many GROUP BY columns (max 64)", peek(ps)); break; }
            ASTExpr gc_expr;
            memset(&gc_expr, 0, sizeof(gc_expr));
            parse_column_ref(ps, &gc_expr);
            if (gc_expr.table_name[0])
                copy_qualified(ps, n->select_stmt.group_cols[n->select_stmt.num_group_cols++], sizeof(n->select_stmt.group_cols[0]), gc_expr.table_name, gc_expr.col_name);
            else
                snprintf(n->select_stmt.group_cols[n->select_stmt.num_group_cols++],
                         sizeof(n->select_stmt.group_cols[0]), "%s", gc_expr.col_name);
            if (!match(ps, TOK_COMMA)) break;
        }
    }

    if (match(ps, TOK_HAVING)) n->select_stmt.having = parse_condition(ps);

    if (match(ps, TOK_ORDER)) {
        expect(ps, TOK_BY);
        ASTExpr oc;
        memset(&oc, 0, sizeof(oc));
        parse_column_ref(ps, &oc);
        if (oc.table_name[0])
            copy_qualified(ps, n->select_stmt.order_col, sizeof(n->select_stmt.order_col), oc.table_name, oc.col_name);
        else
            copy_text(ps, n->select_stmt.order_col, sizeof(n->select_stmt.order_col), oc.col_name);
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
            ast_expr_free(n->select_stmt.where);
            ast_expr_free(n->select_stmt.having);
        } else if (n->type == NODE_INSERT) {
            for (int j = 0; j < n->insert_stmt.num_cols; j++)
                free(n->insert_stmt.columns[j]);
            for (int j = 0; j < n->insert_stmt.num_values; j++)
                free(n->insert_stmt.values[j]);
        } else if (n->type == NODE_UPDATE) {
            free(n->update_stmt.set_expr);
            ast_expr_free(n->update_stmt.where);
        } else if (n->type == NODE_DELETE) {
            ast_expr_free(n->delete_stmt.where);
        }
        free(n);
    }
    free(r->statements);
    free(r);
}
