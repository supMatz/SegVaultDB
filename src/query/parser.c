#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <errno.h>

typedef struct {
    TokenList* tokens;
    int        pos;
    ParseResult* result;
    int        depth;          // annidamento corrente delle espressioni
} ParserState;

#define MAX_EXPR_DEPTH 128     // parentesi, operatori e funzioni annidati

// l'ultimo token è sempre TOK_EOF: non si va mai oltre (prima consume() avanzava fuori
// dall'array e i messaggi d'errore leggevano memoria non inizializzata)
static Token peek(ParserState* ps) {
    int last = ps->tokens->count - 1;
    return ps->tokens->tokens[ps->pos < last ? ps->pos : last];
}
static Token consume(ParserState* ps) {
    Token t = peek(ps);
    if (ps->pos < ps->tokens->count - 1) ps->pos++;
    return t;
}
static bool match(ParserState* ps, TokenType tt) {
    if (peek(ps).type == tt) { consume(ps); return true; }
    return false;
}
// nome leggibile di un tipo di token (per i messaggi d'errore)
static const char* token_name(TokenType tt) {
    static const char* const NAMES[] = {
        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "CREATE",
        "TABLE", "DATABASE", "DROP", "ALTER", "INDEX", "VIEW", "AND", "OR", "NOT", "IN", "IS",
        "NULL", "LIKE", "BETWEEN", "ORDER", "BY", "ASC", "DESC", "LIMIT", "JOIN", "LEFT",
        "RIGHT", "ON", "AS", "DISTINCT", "GROUP", "HAVING", "UNION", "ALL", "EXISTS", "CASE",
        "WHEN", "THEN", "ELSE", "END", "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION", "USE",
        "SHOW", "DESCRIBE", "EXPLAIN", "TRUNCATE", "KEY", "SAVEPOINT", "TO", "TRIGGER", "BEFORE", "AFTER",
        "FOR", "EACH", "ROW", "RENAME", "BOOLEAN",
        "an integer", "a number", "a string", "a name", "'*'", "','", "';'", "'.'",
        "'('", "')'", "'='", "'!='", "'<'", "'>'", "'<='", "'>='", "'+'", "'-'",
        "'/'", "'%'", "end of statement", "invalid token",
    };
    _Static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == TOK_ERROR + 1, "token_name non allineato a TokenType");
    return (tt >= 0 && tt <= TOK_ERROR) ? NAMES[tt] : "?";
}

static bool expect(ParserState* ps, TokenType tt) {
    if (match(ps, tt)) return true;
    if (!ps->result->success) return false;   // vale il primo errore (il più preciso)
    Token t = peek(ps);
    if (t.type == TOK_ERROR && strchr(t.text, ' '))      // messaggio del lexer (stringa troppo lunga, ...)
        snprintf(ps->result->error, sizeof(ps->result->error), "%.200s (line %d)", t.text, t.line);
    else if (t.type == TOK_ERROR)
        snprintf(ps->result->error, sizeof(ps->result->error), "Unexpected character '%.8s' (line %d)", t.text, t.line);
    else if (t.type == TOK_EOF)
        snprintf(ps->result->error, sizeof(ps->result->error), "Expected %s but the statement ended (line %d)",
                 token_name(tt), t.line);
    else
        snprintf(ps->result->error, sizeof(ps->result->error), "Expected %s near '%.40s' (line %d)",
                 token_name(tt), t.text, t.line);
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
static ASTNode* parse_query(ParserState* ps);
static void free_node(ASTNode* n);
void ast_node_free(ASTNode* n) { free_node(n); }
static void parse_column_ref(ParserState* ps, ASTExpr* e);

static void parse_fail(ParserState* ps, const char* msg, Token at);

static void parse_fail(ParserState* ps, const char* msg, Token at) {
    if (!ps->result->success) return;
    if (at.type == TOK_EOF)
        snprintf(ps->result->error, sizeof(ps->result->error), "%s, but the statement ended (line %d)", msg, at.line);
    else
        snprintf(ps->result->error, sizeof(ps->result->error), "%s near '%.60s' at line %d", msg, at.text, at.line);
    ps->result->success = false;
}

// ═════════════════════════════════════════════════════════════════
//  Espressioni
//    espr       := or_espr
//    or_espr    := and_espr { OR and_espr }
//    and_espr   := not_espr { AND not_espr }
//    not_espr   := NOT not_espr | predicato
//    predicato  := somma [ op_confronto somma
//                        | IS [NOT] NULL
//                        | [NOT] LIKE somma
//                        | [NOT] BETWEEN somma AND somma
//                        | [NOT] IN '(' espr { , espr } ')' ]
//    somma      := prodotto { (+ | -) prodotto }
//    prodotto   := unario { (* | /) unario }
//    unario     := - unario | + unario | primario
//    primario   := numero | 'testo' | NULL | colonna | tabella.colonna
//                | funzione '(' [ espr { , espr } ] ')'
//                | aggregato '(' [DISTINCT] ( * | espr ) ')'
//                | '(' espr ')'
// ═════════════════════════════════════════════════════════════════

static void subq_release(SubQuery* q) {
    if (!q || --q->refs > 0) return;
    if (q->plan && q->plan_free) q->plan_free(q->plan);
    free_node(q->node);
    free(q);
}

static void win_free(WindowSpec* w) {
    if (!w) return;
    for (int i = 0; i < w->npart; i++) ast_expr_free(w->part[i]);
    for (int i = 0; i < w->nord; i++) ast_expr_free(w->ord[i]);
    free(w);
}

void ast_expr_free(ASTExpr* e) {
    if (!e) return;
    if (e->type == EXPR_STRING) free(e->str_val);
    subq_release(e->subq);
    win_free(e->win);
    ast_expr_free(e->left);
    ast_expr_free(e->right);
    for (int i = 0; i < e->nargs; i++) ast_expr_free(e->args[i]);
    free(e->args);
    free(e);
}

ASTExpr* ast_expr_clone(const ASTExpr* e) {
    if (!e) return NULL;
    ASTExpr* c = malloc(sizeof(ASTExpr));
    if (!c) return NULL;
    *c = *e;
    if (e->type == EXPR_STRING) {
        c->str_val = e->str_val ? strdup(e->str_val) : NULL;
        if (e->str_val && !c->str_val) { free(c); return NULL; }
    }
    c->left = c->right = NULL;
    c->args = NULL;
    c->nargs = 0;
    if (c->subq) c->subq->refs++;
    c->win = NULL;
    if (e->win) {
        c->win = calloc(1, sizeof(WindowSpec));
        if (!c->win) { ast_expr_free(c); return NULL; }
        c->win->slot = e->win->slot;
        c->win->frame = e->win->frame; c->win->fs = e->win->fs; c->win->fe = e->win->fe;
        c->win->fs_off = e->win->fs_off; c->win->fe_off = e->win->fe_off;
        for (int i = 0; i < e->win->npart; i++) { c->win->part[i] = ast_expr_clone(e->win->part[i]); c->win->npart = i + 1; if (!c->win->part[i]) { ast_expr_free(c); return NULL; } }
        for (int i = 0; i < e->win->nord; i++) {
            c->win->ord[i] = ast_expr_clone(e->win->ord[i]); c->win->desc[i] = e->win->desc[i]; c->win->nord = i + 1;
            if (!c->win->ord[i]) { ast_expr_free(c); return NULL; }
        }
    }
    if (e->left && !(c->left = ast_expr_clone(e->left))) { ast_expr_free(c); return NULL; }
    if (e->right && !(c->right = ast_expr_clone(e->right))) { ast_expr_free(c); return NULL; }
    if (e->nargs > 0) {
        c->args = calloc((size_t)e->nargs, sizeof(ASTExpr*));
        if (!c->args) { ast_expr_free(c); return NULL; }
        for (int i = 0; i < e->nargs; i++) {
            c->args[i] = ast_expr_clone(e->args[i]);
            c->nargs = i + 1;
            if (!c->args[i]) { ast_expr_free(c); return NULL; }
        }
    }
    return c;
}

static void fmt_put(char* out, size_t n, size_t* len, const char* s) {
    size_t l = strlen(s);
    if (*len + 1 >= n) return;
    if (*len + l >= n) l = n - 1 - *len;
    memcpy(out + *len, s, l);
    *len += l;
    out[*len] = '\0';
}

static const char* op_text(OpType op) {
    switch (op) {
        case OP_EQ: return " = ";   case OP_NEQ: return " != "; case OP_LT: return " < ";
        case OP_GT: return " > ";   case OP_LE: return " <= ";  case OP_GE: return " >= ";
        case OP_AND: return " AND "; case OP_OR: return " OR ";
        case OP_PLUS: return " + "; case OP_MINUS: return " - "; case OP_MUL: return " * "; case OP_DIV: return " / ";
        case OP_MOD: return " % ";
        case OP_LIKE: return " LIKE "; case OP_NOT_LIKE: return " NOT LIKE ";
        default: return " ? ";
    }
}

static void fmt_rec(const ASTExpr* e, char* out, size_t n, size_t* len, bool top) {
    char buf[600];
    if (!e) { fmt_put(out, n, len, "NULL"); return; }
    switch (e->type) {
        case EXPR_INT:    snprintf(buf, sizeof(buf), "%lld", (long long)e->int_val); fmt_put(out, n, len, buf); break;
        case EXPR_FLOAT:  snprintf(buf, sizeof(buf), "%.15g", e->float_val); fmt_put(out, n, len, buf); break;
        case EXPR_NULL:   fmt_put(out, n, len, "NULL"); break;
        case EXPR_STRING: {
            fmt_put(out, n, len, "'");
            for (const char* p = e->str_val ? e->str_val : ""; *p; p++) {
                char one[3] = { *p, 0, 0 };
                if (*p == '\'') { one[1] = '\''; }
                fmt_put(out, n, len, one);
            }
            fmt_put(out, n, len, "'");
            break;
        }
        case EXPR_STAR:
            if (e->table_name[0]) { fmt_put(out, n, len, e->table_name); fmt_put(out, n, len, "."); }
            fmt_put(out, n, len, "*");
            break;
        case EXPR_COLUMN:
            if (e->table_name[0]) { fmt_put(out, n, len, e->table_name); fmt_put(out, n, len, "."); }
            fmt_put(out, n, len, e->col_name);
            break;
        case EXPR_AGG: case EXPR_FUNC: case EXPR_WINDOW:
            fmt_put(out, n, len, e->func);
            fmt_put(out, n, len, "(");
            if (e->agg_distinct) fmt_put(out, n, len, "DISTINCT ");
            if (e->type == EXPR_AGG && e->nargs == 0) fmt_put(out, n, len, "*");
            for (int i = 0; i < e->nargs; i++) {
                if (i) fmt_put(out, n, len, ", ");
                fmt_rec(e->args[i], out, n, len, true);
            }
            fmt_put(out, n, len, ")");
            if (e->type == EXPR_WINDOW && e->win) {
                fmt_put(out, n, len, " OVER (");
                for (int i = 0; i < e->win->npart; i++) {
                    fmt_put(out, n, len, i ? ", " : "PARTITION BY ");
                    fmt_rec(e->win->part[i], out, n, len, true);
                }
                for (int i = 0; i < e->win->nord; i++) {
                    fmt_put(out, n, len, i ? ", " : (e->win->npart ? " ORDER BY " : "ORDER BY "));
                    fmt_rec(e->win->ord[i], out, n, len, true);
                    if (e->win->desc[i]) fmt_put(out, n, len, " DESC");
                }
                if (e->win->frame) {
                    char fb[96];
                    frame_text(e->win, fb, sizeof(fb));
                    fmt_put(out, n, len, " ");
                    fmt_put(out, n, len, fb);
                }
                fmt_put(out, n, len, ")");
            }
            break;
        case EXPR_SUBQUERY:
            if (e->subkind == SUBQ_EXISTS) fmt_put(out, n, len, "EXISTS ");
            else if (e->subkind != SUBQ_SCALAR) {
                fmt_rec(e->left, out, n, len, false);
                if (e->subkind == SUBQ_ANY && e->op == OP_EQ) fmt_put(out, n, len, " IN ");
                else if (e->subkind == SUBQ_ALL && e->op == OP_NEQ) fmt_put(out, n, len, " NOT IN ");
                else { fmt_put(out, n, len, op_text(e->op)); fmt_put(out, n, len, e->subkind == SUBQ_ANY ? "ANY " : "ALL "); }
            }
            fmt_put(out, n, len, "(SELECT ...)");
            break;
        case EXPR_CASE:
            fmt_put(out, n, len, "CASE ");
            if (e->left) { fmt_rec(e->left, out, n, len, false); fmt_put(out, n, len, " "); }
            for (int i = 0; i + 1 < e->nargs; i += 2) {
                fmt_put(out, n, len, "WHEN ");
                fmt_rec(e->args[i], out, n, len, true);
                fmt_put(out, n, len, " THEN ");
                fmt_rec(e->args[i + 1], out, n, len, true);
                fmt_put(out, n, len, " ");
            }
            if (e->right) { fmt_put(out, n, len, "ELSE "); fmt_rec(e->right, out, n, len, true); fmt_put(out, n, len, " "); }
            fmt_put(out, n, len, "END");
            break;
        case EXPR_UNARY:
            if (e->op == OP_NOT)      { fmt_put(out, n, len, "NOT "); fmt_rec(e->left, out, n, len, false); }
            else if (e->op == OP_NEG) { fmt_put(out, n, len, "-"); fmt_rec(e->left, out, n, len, false); }
            else { fmt_rec(e->left, out, n, len, false); fmt_put(out, n, len, e->op == OP_IS_NULL ? " IS NULL" : " IS NOT NULL"); }
            break;
        case EXPR_BINARY:
            if (!top) fmt_put(out, n, len, "(");
            fmt_rec(e->left, out, n, len, false);
            fmt_put(out, n, len, op_text(e->op));
            fmt_rec(e->right, out, n, len, false);
            if (!top) fmt_put(out, n, len, ")");
            break;
    }
}

void ast_expr_format(const ASTExpr* e, char* out, size_t n) {
    if (!out || n == 0) return;
    size_t len = 0;
    out[0] = '\0';
    fmt_rec(e, out, n, &len, true);
}

static ASTExpr* new_expr(ExprType type) {
    ASTExpr* e = calloc(1, sizeof(ASTExpr));
    if (e) e->type = type;
    return e;
}

static ASTExpr* new_binary(OpType op, ASTExpr* l, ASTExpr* r) {
    if (!l || !r) { ast_expr_free(l); ast_expr_free(r); return NULL; }
    ASTExpr* e = new_expr(EXPR_BINARY);
    if (!e) { ast_expr_free(l); ast_expr_free(r); return NULL; }
    e->op = op; e->left = l; e->right = r;
    e->depth = 1 + (l->depth > r->depth ? l->depth : r->depth);
    return e;
}

static ASTExpr* new_unary(OpType op, ASTExpr* child) {
    if (!child) return NULL;
    ASTExpr* e = new_expr(EXPR_UNARY);
    if (!e) { ast_expr_free(child); return NULL; }
    e->op = op; e->left = child;
    e->depth = 1 + child->depth;
    return e;
}

// combina i termini con op in un albero bilanciato (AND e OR sono associativi): profondità
// logaritmica, così anche migliaia di condizioni o valori in IN restano sotto il limite
static ASTExpr* balanced(OpType op, ASTExpr** terms, int lo, int hi) {
    if (lo == hi) return terms[lo];
    int mid = (lo + hi) / 2;
    ASTExpr* l = balanced(op, terms, lo, mid);
    ASTExpr* r = balanced(op, terms, mid + 1, hi);
    return new_binary(op, l, r);
}

typedef struct { ASTExpr** v; int n, cap; } ExprList;

static bool elist_push(ExprList* l, ASTExpr* e) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        ASTExpr** nv = realloc(l->v, sizeof(ASTExpr*) * (size_t)nc);
        if (!nv) return false;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = e;
    return true;
}

static void elist_free(ExprList* l) {
    for (int i = 0; i < l->n; i++) ast_expr_free(l->v[i]);
    free(l->v);
}

// lista -> albero bilanciato (la lista viene consumata)
static ASTExpr* elist_combine(ExprList* l, OpType op) {
    ASTExpr* e = l->n ? balanced(op, l->v, 0, l->n - 1) : NULL;
    free(l->v);
    return e;
}

// rifiuta gli alberi troppo profondi (es. 1+1+1+... con migliaia di termini)
static ASTExpr* depth_check(ParserState* ps, ASTExpr* e) {
    if (e && e->depth > MAX_EXPR_DEPTH) {
        parse_fail(ps, "Expression is too complex (more than 128 nested levels)", peek(ps));
        ast_expr_free(e);
        return NULL;
    }
    return e;
}

static bool is_aggregate_name(const char* s) {
    static const char* AGG[] = { "COUNT", "SUM", "AVG", "MIN", "MAX" };
    for (size_t i = 0; i < sizeof(AGG) / sizeof(AGG[0]); i++)
        if (strcasecmp(s, AGG[i]) == 0) return true;
    return false;
}

static bool next_is(ParserState* ps, int ahead, TokenType tt) {
    int i = ps->pos + ahead;
    return i < ps->tokens->count && ps->tokens->tokens[i].type == tt;
}

static ASTExpr* parse_expr(ParserState* ps);
static ASTExpr* parse_additive(ParserState* ps);


static bool ident_is(ParserState* ps, const char* word);

// testo di un frame: "ROWS BETWEEN 2 PRECEDING AND CURRENT ROW"
void frame_text(const WindowSpec* w, char* buf, size_t n) {
    char b[2][40];
    int k[2] = { w->fs, w->fe };
    int64_t off[2] = { w->fs_off, w->fe_off };
    for (int i = 0; i < 2; i++) {
        switch (k[i]) {
            case FB_UNB_PREC: snprintf(b[i], sizeof(b[i]), "UNBOUNDED PRECEDING"); break;
            case FB_PREC:     snprintf(b[i], sizeof(b[i]), "%lld PRECEDING", (long long)off[i]); break;
            case FB_CURRENT:  snprintf(b[i], sizeof(b[i]), "CURRENT ROW"); break;
            case FB_FOLL:     snprintf(b[i], sizeof(b[i]), "%lld FOLLOWING", (long long)off[i]); break;
            default:          snprintf(b[i], sizeof(b[i]), "UNBOUNDED FOLLOWING"); break;
        }
    }
    snprintf(buf, n, "%s BETWEEN %s AND %s", w->frame == FRAME_ROWS ? "ROWS" : "RANGE", b[0], b[1]);
}

// un estremo del frame; end = true per quello dopo AND
static bool parse_frame_bound(ParserState* ps, int* kind, int64_t* off) {
    Token t = peek(ps);
    if (ident_is(ps, "UNBOUNDED")) {
        consume(ps);
        if (ident_is(ps, "PRECEDING")) { consume(ps); *kind = FB_UNB_PREC; return true; }
        if (ident_is(ps, "FOLLOWING")) { consume(ps); *kind = FB_UNB_FOLL; return true; }
        parse_fail(ps, "Expected PRECEDING or FOLLOWING after UNBOUNDED", peek(ps));
        return false;
    }
    if (ident_is(ps, "CURRENT")) {
        consume(ps);
        if (peek(ps).type != TOK_ROW && !ident_is(ps, "ROW")) { parse_fail(ps, "Expected ROW after CURRENT", peek(ps)); return false; }
        consume(ps);
        *kind = FB_CURRENT;
        return true;
    }
    if (t.type == TOK_INTEGER) {
        consume(ps);
        *off = strtoll(t.text, NULL, 10);
        if (ident_is(ps, "PRECEDING")) { consume(ps); *kind = FB_PREC; return true; }
        if (ident_is(ps, "FOLLOWING")) { consume(ps); *kind = FB_FOLL; return true; }
        parse_fail(ps, "Expected PRECEDING or FOLLOWING after the number of rows", peek(ps));
        return false;
    }
    parse_fail(ps, "Expected UNBOUNDED, CURRENT ROW or a number in the window frame", t);
    return false;
}

// ROWS | RANGE { inizio | BETWEEN inizio AND fine }   (solo inizio: la fine è CURRENT ROW)
static void parse_frame(ParserState* ps, WindowSpec* w) {
    Token t = peek(ps);
    w->frame = ident_is(ps, "ROWS") ? FRAME_ROWS : FRAME_RANGE;
    consume(ps);
    w->fe = FB_CURRENT;
    if (match(ps, TOK_BETWEEN)) {
        if (!parse_frame_bound(ps, &w->fs, &w->fs_off)) return;
        if (!match(ps, TOK_AND)) { parse_fail(ps, "Expected AND in the window frame", peek(ps)); return; }
        if (!parse_frame_bound(ps, &w->fe, &w->fe_off)) return;
    } else if (!parse_frame_bound(ps, &w->fs, &w->fs_off)) return;
    if (w->fs == FB_UNB_FOLL || w->fe == FB_UNB_PREC || w->fs > w->fe)
        parse_fail(ps, "The window frame must start before it ends", t);
    else if (w->frame == FRAME_RANGE && (w->fs == FB_PREC || w->fs == FB_FOLL || w->fe == FB_PREC || w->fe == FB_FOLL))
        parse_fail(ps, "RANGE frames support only UNBOUNDED and CURRENT ROW (use ROWS for n PRECEDING / FOLLOWING)", t);
}

// all'offset ahead inizia una query (SELECT ... o WITH ...)?
static bool query_starts(ParserState* ps, int ahead) {
    int i = ps->pos + ahead;
    if (i >= ps->tokens->count) return false;
    const Token* t = &ps->tokens->tokens[i];
    return t->type == TOK_SELECT || (t->type == TOK_IDENTIFIER && strcasecmp(t->text, "WITH") == 0);
}

// '(' query ')' -> espressione subquery del tipo indicato (il '(' non è ancora consumato)
static ASTExpr* parse_subquery(ParserState* ps, int kind, OpType op, ASTExpr* left) {
    if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' before the subquery", peek(ps)); ast_expr_free(left); return NULL; }
    if (++ps->depth > MAX_EXPR_DEPTH) {
        parse_fail(ps, "Query is too complex (too many nested subqueries)", peek(ps));
        ps->depth--;
        ast_expr_free(left);
        return NULL;
    }
    ASTNode* q = parse_query(ps);
    ps->depth--;
    if (!q || !ps->result->success) { free_node(q); ast_expr_free(left); return NULL; }
    if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' to close the subquery", peek(ps)); free_node(q); ast_expr_free(left); return NULL; }
    ASTExpr* e = new_expr(EXPR_SUBQUERY);
    SubQuery* sq = calloc(1, sizeof(SubQuery));
    if (!e || !sq) { free(e); free(sq); free_node(q); ast_expr_free(left); parse_fail(ps, "Out of memory", peek(ps)); return NULL; }
    sq->node = q;
    sq->refs = 1;
    e->subq = sq;
    e->subkind = kind;
    e->op = op;
    e->left = left;
    e->depth = 1 + (left ? left->depth : 0);
    return e;
}

// CASE [operando] WHEN a THEN b {WHEN ...} [ELSE c] END   (CASE già consumato)
static ASTExpr* parse_case(ParserState* ps) {
    ASTExpr* e = new_expr(EXPR_CASE);
    if (!e) return NULL;
    if (peek(ps).type != TOK_WHEN) {
        e->left = parse_expr(ps);
        if (!e->left) { ast_expr_free(e); return NULL; }
    }
    int cap = 0;
    while (ps->result->success && match(ps, TOK_WHEN)) {
        if (e->nargs + 2 > cap) {
            int nc = cap ? cap * 2 : 8;
            ASTExpr** na = realloc(e->args, sizeof(ASTExpr*) * (size_t)nc);
            if (!na) { parse_fail(ps, "Out of memory", peek(ps)); break; }
            e->args = na;
            cap = nc;
        }
        ASTExpr* w = parse_expr(ps);
        if (!w) break;
        e->args[e->nargs++] = w;
        if (!match(ps, TOK_THEN)) { parse_fail(ps, "Expected THEN in CASE", peek(ps)); break; }
        ASTExpr* t = parse_expr(ps);
        if (!t) { e->args[e->nargs++] = new_expr(EXPR_NULL); break; }
        e->args[e->nargs++] = t;
    }
    if (ps->result->success && e->nargs == 0) parse_fail(ps, "CASE needs at least one WHEN ... THEN", peek(ps));
    if (ps->result->success && match(ps, TOK_ELSE)) e->right = parse_expr(ps);
    if (ps->result->success && !match(ps, TOK_END)) parse_fail(ps, "Expected END to close CASE", peek(ps));
    if (!ps->result->success) { ast_expr_free(e); return NULL; }
    int d = e->left ? e->left->depth : 0;
    if (e->right && e->right->depth > d) d = e->right->depth;
    for (int i = 0; i < e->nargs; i++) if (e->args[i] && e->args[i]->depth > d) d = e->args[i]->depth;
    e->depth = d + 1;
    return e;
}

#define MAX_FUNC_ARGS 32

// nome '(' argomenti ')' -> EXPR_FUNC o EXPR_AGG
static ASTExpr* parse_call(ParserState* ps) {
    Token fn = consume(ps);
    consume(ps);   // '('
    bool agg = is_aggregate_name(fn.text);
    ASTExpr* e = new_expr(agg ? EXPR_AGG : EXPR_FUNC);
    if (!e) return NULL;
    if (strlen(fn.text) >= sizeof(e->func)) { parse_fail(ps, "Unknown function", fn); ast_expr_free(e); return NULL; }
    for (int i = 0; fn.text[i]; i++) e->func[i] = (char)toupper((unsigned char)fn.text[i]);
    if (agg && match(ps, TOK_DISTINCT)) e->agg_distinct = true;
    if (agg && peek(ps).type == TOK_STAR) {
        consume(ps);
        if (strcmp(e->func, "COUNT") != 0 || e->agg_distinct) parse_fail(ps, "Only COUNT accepts *", fn);
        strcpy(e->col_name, "*");
    } else if (peek(ps).type != TOK_RPAREN) {
        e->args = calloc(MAX_FUNC_ARGS, sizeof(ASTExpr*));
        if (!e->args) { ast_expr_free(e); return NULL; }
        do {
            if (e->nargs >= MAX_FUNC_ARGS) { parse_fail(ps, "Too many function arguments", peek(ps)); break; }
            ASTExpr* a = parse_expr(ps);
            if (!a) break;
            e->args[e->nargs++] = a;
            if (a->depth + 1 > e->depth) e->depth = a->depth + 1;
        } while (ps->result->success && match(ps, TOK_COMMA));
    }
    if (!match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' to close the function call", peek(ps));
    if (agg && e->nargs != (strcmp(e->col_name, "*") == 0 ? 0 : 1))
        parse_fail(ps, "Aggregate functions take exactly one argument", fn);
    if (!ps->result->success) { ast_expr_free(e); return NULL; }
    if (ident_is(ps, "OVER")) {                       // funzione finestra
        consume(ps);
        if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' after OVER", peek(ps)); ast_expr_free(e); return NULL; }
        WindowSpec* w = calloc(1, sizeof(WindowSpec));
        if (!w) { ast_expr_free(e); parse_fail(ps, "Out of memory", peek(ps)); return NULL; }
        e->win = w;
        e->type = EXPR_WINDOW;
        if (ident_is(ps, "PARTITION")) {
            consume(ps);
            expect(ps, TOK_BY);
            do {
                if (w->npart >= 8) { parse_fail(ps, "Too many PARTITION BY expressions (max 8)", peek(ps)); break; }
                ASTExpr* x = parse_expr(ps);
                if (!x) break;
                w->part[w->npart++] = x;
            } while (ps->result->success && match(ps, TOK_COMMA));
        }
        if (ps->result->success && match(ps, TOK_ORDER)) {
            expect(ps, TOK_BY);
            do {
                if (w->nord >= 8) { parse_fail(ps, "Too many ORDER BY expressions in OVER (max 8)", peek(ps)); break; }
                ASTExpr* x = parse_expr(ps);
                if (!x) break;
                w->ord[w->nord] = x;
                if (match(ps, TOK_DESC)) w->desc[w->nord] = true; else match(ps, TOK_ASC);
                w->nord++;
            } while (ps->result->success && match(ps, TOK_COMMA));
        }
        if (ps->result->success && (ident_is(ps, "ROWS") || ident_is(ps, "RANGE"))) parse_frame(ps, w);
        if (ps->result->success && !match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' to close OVER", peek(ps));
        if (!ps->result->success) { ast_expr_free(e); return NULL; }
    }
    return e;
}

static ASTExpr* parse_primary(ParserState* ps) {
    Token t = peek(ps);
    ASTExpr* e = NULL;
    switch (t.type) {
        case TOK_INTEGER: {
            consume(ps);
            errno = 0;
            long long v = strtoll(t.text, NULL, 10);
            if (errno == ERANGE) {   // oltre 64 bit: numero decimale
                e = new_expr(EXPR_FLOAT);
                if (e) e->float_val = strtod(t.text, NULL);
            } else {
                e = new_expr(EXPR_INT);
                if (e) e->int_val = v;
            }
            return e;
        }
        case TOK_FLOAT:   consume(ps); e = new_expr(EXPR_FLOAT); if (e) e->float_val = strtod(t.text, NULL); return e;
        case TOK_STRING:
            consume(ps);
            e = new_expr(EXPR_STRING);
            if (e && !(e->str_val = strdup(t.text))) { free(e); e = NULL; parse_fail(ps, "Out of memory", t); }
            return e;
        case TOK_NULL:    consume(ps); return new_expr(EXPR_NULL);
        case TOK_EXISTS:
            consume(ps);
            return parse_subquery(ps, SUBQ_EXISTS, OP_EQ, NULL);
        case TOK_CASE:
            consume(ps);
            return parse_case(ps);
        case TOK_LPAREN: {
            if (query_starts(ps, 1)) return parse_subquery(ps, SUBQ_SCALAR, OP_EQ, NULL);
            consume(ps);
            e = parse_expr(ps);
            if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')'", peek(ps)); ast_expr_free(e); return NULL; }
            return e;
        }
        case TOK_IDENTIFIER: case TOK_LEFT: case TOK_RIGHT:   // LEFT(...)/RIGHT(...) sono funzioni
            if (next_is(ps, 1, TOK_LPAREN)) return parse_call(ps);
            if (t.type != TOK_IDENTIFIER) break;
            e = new_expr(EXPR_COLUMN);
            if (!e) return NULL;
            consume(ps);
            if (match(ps, TOK_DOT)) {
                Token c = consume(ps);
                if (c.type == TOK_STAR) { parse_fail(ps, "table.* is only allowed in the SELECT list", c); ast_expr_free(e); return NULL; }
                if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name after '.'", c); ast_expr_free(e); return NULL; }
                copy_text(ps, e->table_name, sizeof(e->table_name), t.text);
                copy_text(ps, e->col_name, sizeof(e->col_name), c.text);
            } else {
                copy_text(ps, e->col_name, sizeof(e->col_name), t.text);
            }
            return e;
        default:
            break;
    }
    parse_fail(ps, "Expected a value or a column", t);
    if (t.type != TOK_EOF) consume(ps);
    return NULL;
}

static ASTExpr* parse_unary(ParserState* ps) {
    // la ricorsione (parentesi, meno unari, argomenti) è limitata: una query costruita apposta
    // con migliaia di livelli esaurirebbe lo stack
    if (++ps->depth > MAX_EXPR_DEPTH) {
        parse_fail(ps, "Expression is too complex (more than 128 nested levels)", peek(ps));
        ps->depth--;
        return NULL;
    }
    ASTExpr* e;
    if (match(ps, TOK_MINUS))     e = new_unary(OP_NEG, parse_unary(ps));
    else if (match(ps, TOK_PLUS)) e = parse_unary(ps);
    else                          e = parse_primary(ps);
    ps->depth--;
    return depth_check(ps, e);
}

static ASTExpr* parse_multiplicative(ParserState* ps) {
    ASTExpr* e = parse_unary(ps);
    while (e && ps->result->success) {
        OpType op;
        if (match(ps, TOK_STAR))       op = OP_MUL;
        else if (match(ps, TOK_SLASH)) op = OP_DIV;
        else if (match(ps, TOK_PERCENT)) op = OP_MOD;
        else break;
        e = depth_check(ps, new_binary(op, e, parse_unary(ps)));
    }
    return e;
}

static ASTExpr* parse_additive(ParserState* ps) {
    ASTExpr* e = parse_multiplicative(ps);
    while (e && ps->result->success) {
        OpType op;
        if (match(ps, TOK_PLUS))       op = OP_PLUS;
        else if (match(ps, TOK_MINUS)) op = OP_MINUS;
        else break;
        e = depth_check(ps, new_binary(op, e, parse_multiplicative(ps)));
    }
    return e;
}

static ASTExpr* parse_predicate(ParserState* ps) {
    ASTExpr* left = parse_additive(ps);
    if (!left) return NULL;

    if (match(ps, TOK_IS)) {
        bool neg = match(ps, TOK_NOT);
        if (!match(ps, TOK_NULL)) { parse_fail(ps, "Expected NULL after IS", peek(ps)); ast_expr_free(left); return NULL; }
        return new_unary(neg ? OP_IS_NOT_NULL : OP_IS_NULL, left);
    }
    bool neg = match(ps, TOK_NOT);
    if (match(ps, TOK_LIKE)) return new_binary(neg ? OP_NOT_LIKE : OP_LIKE, left, parse_additive(ps));
    if (match(ps, TOK_BETWEEN)) {
        // x BETWEEN a AND b  ->  x >= a AND x <= b   (NOT: x < a OR x > b)
        ASTExpr* lo = parse_additive(ps);
        if (!match(ps, TOK_AND)) { parse_fail(ps, "Expected AND in BETWEEN", peek(ps)); ast_expr_free(left); ast_expr_free(lo); return NULL; }
        ASTExpr* hi = parse_additive(ps);
        ASTExpr* left2 = ast_expr_clone(left);
        if (neg) return new_binary(OP_OR, new_binary(OP_LT, left, lo), new_binary(OP_GT, left2, hi));
        return new_binary(OP_AND, new_binary(OP_GE, left, lo), new_binary(OP_LE, left2, hi));
    }
    if (match(ps, TOK_IN)) {
        // x IN (a, b)  ->  x = a OR x = b      (NOT IN: x != a AND x != b)
        if (peek(ps).type == TOK_LPAREN && query_starts(ps, 1))     // IN (SELECT ...)
            return parse_subquery(ps, neg ? SUBQ_ALL : SUBQ_ANY, neg ? OP_NEQ : OP_EQ, left);
        if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' after IN", peek(ps)); ast_expr_free(left); return NULL; }
        ExprList terms = {0};
        bool ok = true;
        do {
            ASTExpr* v = parse_expr(ps);
            if (!v) { ok = false; break; }
            ASTExpr* cmp = new_binary(neg ? OP_NEQ : OP_EQ, ast_expr_clone(left), v);
            if (!cmp || !elist_push(&terms, cmp)) { ast_expr_free(cmp); ok = false; break; }
        } while (ps->result->success && match(ps, TOK_COMMA));
        ast_expr_free(left);
        if (ok && !match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' to close the IN list", peek(ps));
        if (!ok || !ps->result->success) { elist_free(&terms); if (ps->result->success) parse_fail(ps, "Out of memory", peek(ps)); return NULL; }
        return depth_check(ps, elist_combine(&terms, neg ? OP_AND : OP_OR));
    }
    if (neg) { parse_fail(ps, "Expected LIKE, BETWEEN or IN after NOT", peek(ps)); ast_expr_free(left); return NULL; }

    OpType op;
    if      (match(ps, TOK_EQ))  op = OP_EQ;
    else if (match(ps, TOK_NEQ)) op = OP_NEQ;
    else if (match(ps, TOK_LT))  op = OP_LT;
    else if (match(ps, TOK_GT))  op = OP_GT;
    else if (match(ps, TOK_LE))  op = OP_LE;
    else if (match(ps, TOK_GE))  op = OP_GE;
    else return left;                                   // espressione senza confronto
    // op ANY / SOME / ALL (SELECT ...)
    bool quant_all = peek(ps).type == TOK_ALL;
    if ((quant_all || ident_is(ps, "ANY") || ident_is(ps, "SOME")) && next_is(ps, 1, TOK_LPAREN)) {
        consume(ps);
        if (!query_starts(ps, 1)) { parse_fail(ps, "ANY / ALL need a subquery", peek(ps)); ast_expr_free(left); return NULL; }
        return parse_subquery(ps, quant_all ? SUBQ_ALL : SUBQ_ANY, op, left);
    }
    return new_binary(op, left, parse_additive(ps));
}

static ASTExpr* parse_not(ParserState* ps) {
    if (match(ps, TOK_NOT)) {
        if (++ps->depth > MAX_EXPR_DEPTH) {
            parse_fail(ps, "Expression is too complex (more than 128 nested levels)", peek(ps));
            ps->depth--;
            return NULL;
        }
        ASTExpr* e = depth_check(ps, new_unary(OP_NOT, parse_not(ps)));
        ps->depth--;
        return e;
    }
    return parse_predicate(ps);
}

static ASTExpr* parse_and(ParserState* ps) {
    ASTExpr* first = parse_not(ps);                   // caso comune: un solo termine, nessuna lista
    if (!first || !ps->result->success || peek(ps).type != TOK_AND) {
        if (!ps->result->success) { ast_expr_free(first); return NULL; }
        return first;
    }
    ExprList terms = {0};
    if (!elist_push(&terms, first)) { ast_expr_free(first); return NULL; }
    match(ps, TOK_AND);
    do {
        ASTExpr* e = parse_not(ps);
        if (!e || !elist_push(&terms, e)) { ast_expr_free(e); elist_free(&terms); return NULL; }
    } while (ps->result->success && match(ps, TOK_AND));
    if (!ps->result->success) { elist_free(&terms); return NULL; }
    return depth_check(ps, elist_combine(&terms, OP_AND));
}

static ASTExpr* parse_expr(ParserState* ps) {
    ASTExpr* first = parse_and(ps);                   // caso comune: un solo termine, nessuna lista
    if (!first || !ps->result->success || peek(ps).type != TOK_OR) {
        if (!ps->result->success) { ast_expr_free(first); return NULL; }
        return first;
    }
    ExprList terms = {0};
    if (!elist_push(&terms, first)) { ast_expr_free(first); return NULL; }
    match(ps, TOK_OR);
    do {
        ASTExpr* e = parse_and(ps);
        if (!e || !elist_push(&terms, e)) { ast_expr_free(e); elist_free(&terms); return NULL; }
    } while (ps->result->success && match(ps, TOK_OR));
    if (!ps->result->success) { elist_free(&terms); return NULL; }
    return depth_check(ps, elist_combine(&terms, OP_OR));
}

static ASTExpr* parse_condition(ParserState* ps) { return parse_expr(ps); }

// identificatore con quel nome (parole come INNER/OUTER/CROSS non sono keyword del lexer)
static bool ident_is(ParserState* ps, const char* word) {
    Token t = peek(ps);
    return t.type == TOK_IDENTIFIER && strcasecmp(t.text, word) == 0;
}

// parole che dopo un nome di tabella non possono essere un alias
static bool alias_forbidden(ParserState* ps) {
    static const char* const W[] = { "INNER", "CROSS", "OUTER", "OFFSET", "INTERSECT", "EXCEPT", "MINUS", "NATURAL", "FULL", "USING", "WINDOW" };
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++) if (ident_is(ps, W[i])) return true;
    return false;
}

// [AS] alias dopo un nome di tabella
static void parse_table_alias(ParserState* ps, char* alias, size_t cap) {
    bool as = match(ps, TOK_AS);
    Token t = peek(ps);
    if (t.type != TOK_IDENTIFIER || alias_forbidden(ps)) {
        if (as) parse_fail(ps, "Expected an alias after AS", t);
        return;
    }
    consume(ps);
    copy_text(ps, alias, cap, t.text);
}

// [AS] alias dopo una tabella derivata: obbligatorio in SQL standard, qui facoltativo
static ASTNode* parse_derived(ParserState* ps, char* name, size_t cap) {
    consume(ps);   // '('
    if (++ps->depth > MAX_EXPR_DEPTH) { parse_fail(ps, "Query is too complex", peek(ps)); ps->depth--; return NULL; }
    ASTNode* q = parse_query(ps);
    ps->depth--;
    if (!ps->result->success) return q;
    if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the subquery in FROM", peek(ps)); return q; }
    char alias[64] = "";
    parse_table_alias(ps, alias, sizeof(alias));
    copy_text(ps, name, cap, alias[0] ? alias : "subquery");
    return q;
}

// SELECT [DISTINCT] colonne [FROM ...] [WHERE] [GROUP BY] [HAVING]  (senza ORDER BY / LIMIT)
// FROM con le sue sorgenti e i JOIN (dopo la parola FROM)
static void parse_from_clause(ParserState* ps, ASTNode* n) {
    // FROM fonte [[AS] alias] { , fonte [[AS] alias]
    //   | [INNER | CROSS | LEFT [OUTER] | RIGHT [OUTER]] JOIN fonte [[AS] alias] [ON condizione] }
    // fonte := tabella | vista | '(' SELECT ... ')'
    if (peek(ps).type == TOK_LPAREN && query_starts(ps, 1)) {
        n->select_stmt.from_sub = parse_derived(ps, n->select_stmt.table_name, sizeof(n->select_stmt.table_name));
        memcpy(n->select_stmt.table_alias, n->select_stmt.table_name, sizeof(n->select_stmt.table_alias));
    } else {
        Token first = consume(ps);
        if (first.type != TOK_IDENTIFIER) parse_fail(ps, "Expected a table name after FROM", first);
        copy_text(ps, n->select_stmt.table_name, sizeof(n->select_stmt.table_name), first.text);
        parse_table_alias(ps, n->select_stmt.table_alias, sizeof(n->select_stmt.table_alias));
    }

    while (ps->result->success) {
        JoinType jt;
        bool on_allowed = true;
        bool natural = false;
        if (ident_is(ps, "NATURAL")) { consume(ps); natural = true; }
        if (!natural && match(ps, TOK_COMMA)) { jt = JOIN_CROSS; on_allowed = false; }
        else if (match(ps, TOK_JOIN))   jt = JOIN_INNER;
        else if (ident_is(ps, "INNER")) { consume(ps); expect(ps, TOK_JOIN); jt = JOIN_INNER; }
        else if (!natural && ident_is(ps, "CROSS")) { consume(ps); expect(ps, TOK_JOIN); jt = JOIN_CROSS; on_allowed = false; }
        else if (match(ps, TOK_LEFT))   { if (ident_is(ps, "OUTER")) consume(ps); expect(ps, TOK_JOIN); jt = JOIN_LEFT; }
        else if (match(ps, TOK_RIGHT))  { if (ident_is(ps, "OUTER")) consume(ps); expect(ps, TOK_JOIN); jt = JOIN_RIGHT; }
        else if (ident_is(ps, "FULL"))  { consume(ps); if (ident_is(ps, "OUTER")) consume(ps); expect(ps, TOK_JOIN); jt = JOIN_FULL; }
        else { if (natural) parse_fail(ps, "Expected JOIN after NATURAL", peek(ps)); break; }

        if (n->select_stmt.num_joins >= 8) { parse_fail(ps, "Too many JOINs (max 8)", peek(ps)); break; }
        ASTJoin* j = &n->select_stmt.joins[n->select_stmt.num_joins++];
        memset(j, 0, sizeof(ASTJoin));
        j->type = jt;
        if (peek(ps).type == TOK_LPAREN && query_starts(ps, 1)) {
            j->sub = parse_derived(ps, j->table_name, sizeof(j->table_name));
            memcpy(j->alias, j->table_name, sizeof(j->alias));
            if (!ps->result->success) break;
        } else {
            Token tt = consume(ps);
            if (tt.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after JOIN", tt); break; }
            copy_text(ps, j->table_name, sizeof(j->table_name), tt.text);
            parse_table_alias(ps, j->alias, sizeof(j->alias));
        }
        j->natural = natural;
        if (natural) { /* le colonne comuni si trovano dopo, conoscendo le tabelle */ }
        else if (on_allowed && match(ps, TOK_ON)) j->condition = parse_expr(ps);
        else if (on_allowed && ident_is(ps, "USING")) {
            consume(ps);
            if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' after USING", peek(ps)); break; }
            do {
                Token c = consume(ps);
                if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in USING", c); break; }
                if (j->nusing >= 8) { parse_fail(ps, "Too many columns in USING (max 8)", c); break; }
                copy_text(ps, j->using_cols[j->nusing++], 64, c.text);
            } while (match(ps, TOK_COMMA));
            if (ps->result->success && !match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' after the USING columns", peek(ps));
        }
        else if (jt == JOIN_LEFT || jt == JOIN_RIGHT || jt == JOIN_FULL) parse_fail(ps, "LEFT, RIGHT and FULL JOIN need ON, USING or NATURAL", peek(ps));
    }
}

static ASTNode* parse_select_core(ParserState* ps) {
    ASTNode* n = calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    n->type = NODE_SELECT;
    if (!match(ps, TOK_SELECT)) { parse_fail(ps, "Expected SELECT", peek(ps)); return n; }

    if (match(ps, TOK_DISTINCT)) n->select_stmt.distinct = true;
    else match(ps, TOK_ALL);

    // elenco: *  |  tabella.*  |  espressione [[AS] alias]
    n->select_stmt.num_cols = 0;
    do {
        if (n->select_stmt.num_cols >= 64) { parse_fail(ps, "Too many columns in SELECT (max 64)", peek(ps)); break; }
        ASTExpr* e = NULL;
        if (peek(ps).type == TOK_STAR) {
            consume(ps);
            e = new_expr(EXPR_STAR);
        } else if (peek(ps).type == TOK_IDENTIFIER && next_is(ps, 1, TOK_DOT) && next_is(ps, 2, TOK_STAR)) {
            Token t = consume(ps);
            consume(ps); consume(ps);
            e = new_expr(EXPR_STAR);
            if (e) copy_text(ps, e->table_name, sizeof(e->table_name), t.text);
        } else {
            e = parse_expr(ps);
            if (e && (match(ps, TOK_AS) || (peek(ps).type == TOK_IDENTIFIER && !alias_forbidden(ps)))) {
                Token al = consume(ps);
                if (al.type != TOK_IDENTIFIER && al.type != TOK_STRING) parse_fail(ps, "Expected an alias after AS", al);
                copy_text(ps, e->alias, sizeof(e->alias), al.text);
            }
        }
        if (!e) { if (ps->result->success) parse_fail(ps, "Out of memory", peek(ps)); break; }
        n->select_stmt.columns[n->select_stmt.num_cols++] = e;
    } while (ps->result->success && match(ps, TOK_COMMA));

    if (!ps->result->success) return n;
    // senza FROM (SELECT 1, SELECT NOW()): nessuna tabella
    if (!match(ps, TOK_FROM)) {
        Token nx = peek(ps);
        if (nx.type != TOK_EOF && nx.type != TOK_SEMICOLON && nx.type != TOK_WHERE && nx.type != TOK_ORDER &&
            nx.type != TOK_LIMIT && nx.type != TOK_RPAREN && nx.type != TOK_UNION && nx.type != TOK_GROUP &&
            nx.type != TOK_HAVING && !ident_is(ps, "INTERSECT") && !ident_is(ps, "EXCEPT") && !ident_is(ps, "MINUS"))
            parse_fail(ps, "Expected FROM", nx);
    } else {
        parse_from_clause(ps, n);
    }

    if (ps->result->success && match(ps, TOK_WHERE)) n->select_stmt.where = parse_expr(ps);

    if (ps->result->success && match(ps, TOK_GROUP)) {
        expect(ps, TOK_BY);
        do {
            if (n->select_stmt.num_group >= SV_MAX_GROUP) { parse_fail(ps, "Too many GROUP BY expressions", peek(ps)); break; }
            ASTExpr* g = parse_expr(ps);
            if (!g) break;
            n->select_stmt.group_exprs[n->select_stmt.num_group++] = g;
        } while (ps->result->success && match(ps, TOK_COMMA));
    }

    if (ps->result->success && match(ps, TOK_HAVING)) n->select_stmt.having = parse_expr(ps);
    return n;
}

// [ORDER BY ...] [LIMIT n [OFFSET m] | LIMIT m, n]
static void parse_order_limit(ParserState* ps, ASTNode* n) {
    if (ps->result->success && match(ps, TOK_ORDER)) {
        expect(ps, TOK_BY);
        do {
            if (n->select_stmt.num_order >= SV_MAX_ORDER) { parse_fail(ps, "Too many ORDER BY expressions (max 8)", peek(ps)); break; }
            ASTExpr* o = parse_expr(ps);
            if (!o) break;
            int k = n->select_stmt.num_order++;
            n->select_stmt.order_exprs[k] = o;
            if (match(ps, TOK_DESC)) n->select_stmt.order_desc[k] = true;
            else match(ps, TOK_ASC);
        } while (ps->result->success && match(ps, TOK_COMMA));
    }

    if (ps->result->success && match(ps, TOK_LIMIT)) {
        Token a = consume(ps);
        if (a.type != TOK_INTEGER || a.text[0] == '-') parse_fail(ps, "LIMIT expects a number >= 0", a);
        n->select_stmt.limit = atoi(a.text);
        n->select_stmt.has_limit = true;
        if (match(ps, TOK_COMMA)) {
            Token b = consume(ps);
            if (b.type != TOK_INTEGER || b.text[0] == '-') parse_fail(ps, "LIMIT expects a number >= 0", b);
            n->select_stmt.offset = n->select_stmt.limit;
            n->select_stmt.limit = atoi(b.text);
        } else if (ident_is(ps, "OFFSET")) {
            consume(ps);
            Token b = consume(ps);
            if (b.type != TOK_INTEGER || b.text[0] == '-') parse_fail(ps, "OFFSET expects a number >= 0", b);
            n->select_stmt.offset = atoi(b.text);
        }
    }
}

// query := [WITH nome AS (query) {, ...}] core { (UNION [ALL] | INTERSECT | EXCEPT) core }
//          [ORDER BY ...] [LIMIT ...]
static ASTNode* parse_query(ParserState* ps) {
    ASTCte* ctes = NULL;
    int nctes = 0;
    if (ident_is(ps, "WITH")) {
        consume(ps);
        bool recursive = false;
        if (ident_is(ps, "RECURSIVE")) { consume(ps); recursive = true; }
        do {
            Token nm = consume(ps);
            if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a name after WITH", nm); break; }
            ASTCte* nc = realloc(ctes, sizeof(ASTCte) * (size_t)(nctes + 1));
            if (!nc) { parse_fail(ps, "Out of memory", nm); break; }
            ctes = nc;
            ASTCte* c = &ctes[nctes++];
            memset(c, 0, sizeof(*c));
            copy_text(ps, c->name, sizeof(c->name), nm.text);
            c->recursive = recursive;
            if (match(ps, TOK_LPAREN)) {                               // WITH nome (colonne) AS ...
                do {
                    Token col = consume(ps);
                    if (col.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", col); break; }
                    if (c->ncols >= 16) { parse_fail(ps, "Too many columns (max 16)", col); break; }
                    copy_text(ps, c->cols[c->ncols++], 64, col.text);
                } while (match(ps, TOK_COMMA));
                if (ps->result->success && !match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' after the column names", peek(ps));
                if (!ps->result->success) break;
            }
            if (!match(ps, TOK_AS)) { parse_fail(ps, "Expected AS after the WITH name", peek(ps)); break; }
            if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' after AS", peek(ps)); break; }
            c->node = parse_query(ps);
            if (!ps->result->success) break;
            if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the WITH query", peek(ps)); break; }
        } while (match(ps, TOK_COMMA));
        if (!ps->result->success) {
            for (int i = 0; i < nctes; i++) free_node(ctes[i].node);
            free(ctes);
            return NULL;
        }
    }

    ASTNode* head = parse_select_core(ps);
    if (!head) { for (int i = 0; i < nctes; i++) free_node(ctes[i].node); free(ctes); return NULL; }
    head->select_stmt.ctes = ctes;
    head->select_stmt.num_ctes = nctes;
    ASTNode* tail = head;
    while (ps->result->success) {
        int op;
        if (match(ps, TOK_UNION)) op = SETOP_UNION;
        else if (ident_is(ps, "INTERSECT")) { consume(ps); op = SETOP_INTERSECT; }
        else if (ident_is(ps, "EXCEPT") || ident_is(ps, "MINUS")) { consume(ps); op = SETOP_EXCEPT; }
        else break;
        bool all = match(ps, TOK_ALL);
        if (!all) match(ps, TOK_DISTINCT);
        if (all && op != SETOP_UNION) { parse_fail(ps, "Only UNION supports ALL", peek(ps)); break; }
        tail->select_stmt.set_op = op;
        tail->select_stmt.set_all = all;
        if (peek(ps).type == TOK_LPAREN) { parse_fail(ps, "Write the SELECT of a UNION / INTERSECT / EXCEPT without parentheses", peek(ps)); break; }
        ASTNode* arm = parse_select_core(ps);
        tail->select_stmt.set_next = arm;
        if (!arm) break;
        tail = arm;
    }
    parse_order_limit(ps, head);
    return head;
}

static ASTNode* parse_select(ParserState* ps) { return parse_query(ps); }

// INSERT INTO t [(colonne)] VALUES (espr, ...) [, (espr, ...)] ...
static void parse_insert(ParserState* ps, ASTNode* n) {
    n->type = NODE_INSERT;
    expect(ps, TOK_INTO);
    Token tn = consume(ps);
    if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after INSERT INTO", tn); return; }
    copy_text(ps, n->insert_stmt.table_name, sizeof(n->insert_stmt.table_name), tn.text);
    if (match(ps, TOK_LPAREN)) {
        do {
            Token c = consume(ps);
            if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", c); return; }
            if (n->insert_stmt.num_cols >= 64) { parse_fail(ps, "Too many columns in INSERT (max 64)", c); return; }
            n->insert_stmt.columns[n->insert_stmt.num_cols++] = strdup(c.text);
        } while (match(ps, TOK_COMMA));
        if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the column list", peek(ps)); return; }
    }
    if (query_starts(ps, 0)) {                                    // INSERT INTO t SELECT ...
        n->insert_stmt.select = parse_query(ps);
        return;
    }
    if (!match(ps, TOK_VALUES)) { parse_fail(ps, "Expected VALUES or SELECT", peek(ps)); return; }
    // i valori sono in un unico array, riga dopo riga; nell'AST restano solo righe complete
    int cap = 0, total = 0;
    do {
        if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' before the values", peek(ps)); return; }
        int count = 0;
        bool ok = true;
        do {
            if (count >= 64) { parse_fail(ps, "Too many values in INSERT (max 64)", peek(ps)); ok = false; break; }
            if (total + count >= cap) {
                int nc = cap ? cap * 2 : 64;
                ASTExpr** nv = realloc(n->insert_stmt.values, sizeof(ASTExpr*) * (size_t)nc);
                if (!nv) { parse_fail(ps, "Out of memory", peek(ps)); ok = false; break; }
                n->insert_stmt.values = nv;
                cap = nc;
            }
            ASTExpr* v = parse_expr(ps);
            if (!v) { ok = false; break; }
            n->insert_stmt.values[total + count++] = v;
        } while (match(ps, TOK_COMMA));
        if (ok && !match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the values", peek(ps)); ok = false; }
        if (ok && n->insert_stmt.num_rows > 0 && count != n->insert_stmt.num_values) {
            parse_fail(ps, "Every row of VALUES must have the same number of values", peek(ps));
            ok = false;
        }
        if (!ok) {
            for (int i = 0; i < count; i++) ast_expr_free(n->insert_stmt.values[total + i]);
            return;
        }
        if (n->insert_stmt.num_rows == 0) n->insert_stmt.num_values = count;
        total += count;
        n->insert_stmt.num_rows++;
    } while (match(ps, TOK_COMMA));
}

// UPDATE t SET col = espr {, col = espr} [WHERE condizione]
static void parse_update(ParserState* ps, ASTNode* n) {
    n->type = NODE_UPDATE;
    Token tn = consume(ps);
    if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after UPDATE", tn); return; }
    copy_text(ps, n->update_stmt.table_name, sizeof(n->update_stmt.table_name), tn.text);
    if (!match(ps, TOK_SET)) { parse_fail(ps, "Expected SET", peek(ps)); return; }
    do {
        if (n->update_stmt.num_sets >= SV_MAX_SETS) { parse_fail(ps, "Too many columns in SET", peek(ps)); return; }
        Token cn = consume(ps);
        if (cn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in SET", cn); return; }
        Token col = cn;
        if (match(ps, TOK_DOT)) {                     // tabella.colonna
            col = consume(ps);
            if (col.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in SET", col); return; }
        }
        if (!match(ps, TOK_EQ)) { parse_fail(ps, "Expected '=' in SET", peek(ps)); return; }
        ASTExpr* v = parse_expr(ps);
        if (!v) return;
        int k = n->update_stmt.num_sets++;
        copy_text(ps, n->update_stmt.set_cols[k], sizeof(n->update_stmt.set_cols[k]), col.text);
        n->update_stmt.set_exprs[k] = v;
    } while (match(ps, TOK_COMMA));
    if (match(ps, TOK_WHERE)) n->update_stmt.where = parse_expr(ps);
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

// ── trigger ──

static bool tokens_text(ParserState* ps, int from, int to, char* out, size_t cap);
static void trig_stmts_free(TrigStmt** v, int n);

static void trig_stmt_free(TrigStmt* t) {
    if (!t) return;
    free_node(t->sql);
    ast_expr_free(t->expr);
    ast_expr_free(t->cond);
    trig_stmts_free(t->then_v, t->nthen);
    trig_stmts_free(t->else_v, t->nelse);
    free(t);
}

static void trig_stmts_free(TrigStmt** v, int n) {
    for (int i = 0; i < n; i++) trig_stmt_free(v[i]);
    free(v);
}

void trig_program_free(TrigProgram* p) {
    if (!p) return;
    ast_expr_free(p->when);
    trig_stmts_free(p->v, p->n);
    free(p);
}

static bool trig_push(ParserState* ps, TrigStmt*** v, int* n, TrigStmt* t) {
    TrigStmt** nv = realloc(*v, sizeof(TrigStmt*) * (size_t)(*n + 1));
    if (!nv) { trig_stmt_free(t); parse_fail(ps, "Out of memory", peek(ps)); return false; }
    *v = nv;
    (*v)[(*n)++] = t;
    return true;
}

static ASTNode* parse_statement(ParserState* ps);
static TrigStmt* parse_trig_stmt(ParserState* ps);
static void parse_column_def(ParserState* ps, ASTNode* n);

// istruzioni fino a una delle parole di chiusura (END, ELSE, ELSEIF), ciascuna seguita da ';'
static void parse_trig_list(ParserState* ps, TrigStmt*** v, int* n) {
    while (ps->result->success) {
        Token t = peek(ps);
        if (t.type == TOK_EOF) { parse_fail(ps, "Expected END to close the trigger body", t); return; }
        if (t.type == TOK_END || t.type == TOK_ELSE || ident_is(ps, "ELSEIF")) return;
        if (t.type == TOK_SEMICOLON) { consume(ps); continue; }
        TrigStmt* st = parse_trig_stmt(ps);
        if (!st) return;
        if (!trig_push(ps, v, n, st)) return;
        if (!match(ps, TOK_SEMICOLON) && peek(ps).type != TOK_END) { parse_fail(ps, "Expected ';' after the statement in the trigger", peek(ps)); return; }
    }
}

// IF ... THEN ... {ELSEIF ... THEN ...} [ELSE ...] END IF  (IF già consumato)
static TrigStmt* parse_trig_if(ParserState* ps) {
    TrigStmt* t = calloc(1, sizeof(TrigStmt));
    if (!t) return NULL;
    t->kind = TS_IF;
    t->cond = parse_expr(ps);
    if (!t->cond || !match(ps, TOK_THEN)) { if (ps->result->success) parse_fail(ps, "Expected THEN after the IF condition", peek(ps)); trig_stmt_free(t); return NULL; }
    parse_trig_list(ps, &t->then_v, &t->nthen);
    if (!ps->result->success) { trig_stmt_free(t); return NULL; }
    if (ident_is(ps, "ELSEIF")) {
        consume(ps);
        TrigStmt* inner = parse_trig_if(ps);           // consuma anche END IF
        if (!inner || !trig_push(ps, &t->else_v, &t->nelse, inner)) { trig_stmt_free(t); return NULL; }
        return t;
    }
    if (match(ps, TOK_ELSE)) {
        parse_trig_list(ps, &t->else_v, &t->nelse);
        if (!ps->result->success) { trig_stmt_free(t); return NULL; }
    }
    if (!match(ps, TOK_END) || !(ident_is(ps, "IF") || peek(ps).type == TOK_IDENTIFIER)) {
        parse_fail(ps, "Expected END IF", peek(ps));
        trig_stmt_free(t);
        return NULL;
    }
    if (!ident_is(ps, "IF")) { parse_fail(ps, "Expected END IF", peek(ps)); trig_stmt_free(t); return NULL; }
    consume(ps);
    return t;
}

static TrigStmt* parse_trig_stmt(ParserState* ps) {
    if (ident_is(ps, "IF")) { consume(ps); return parse_trig_if(ps); }
    TrigStmt* t = calloc(1, sizeof(TrigStmt));
    if (!t) { parse_fail(ps, "Out of memory", peek(ps)); return NULL; }
    if (peek(ps).type == TOK_SET) {                     // SET NEW.colonna = espressione
        consume(ps);
        Token q = consume(ps);
        if (q.type != TOK_IDENTIFIER || strcasecmp(q.text, "NEW") || !match(ps, TOK_DOT)) {
            parse_fail(ps, "In a trigger SET can only change NEW.column", q);
            free(t);
            return NULL;
        }
        Token c = consume(ps);
        if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name after NEW.", c); free(t); return NULL; }
        copy_text(ps, t->col, sizeof(t->col), c.text);
        if (!match(ps, TOK_EQ)) { parse_fail(ps, "Expected '=' in SET", peek(ps)); free(t); return NULL; }
        t->kind = TS_SET;
        t->expr = parse_expr(ps);
        if (!t->expr) { free(t); return NULL; }
        return t;
    }
    if (ident_is(ps, "SIGNAL") || ident_is(ps, "RAISE")) {
        // SIGNAL [SQLSTATE ['VALUE'] '45000'] [SET MESSAGE_TEXT =] 'messaggio'   |   RAISE 'messaggio'
        consume(ps);
        if (ident_is(ps, "SQLSTATE")) {
            consume(ps);
            if (ident_is(ps, "VALUE")) consume(ps);
            if (peek(ps).type == TOK_STRING) consume(ps);
        }
        if (peek(ps).type == TOK_SET) {
            consume(ps);
            if (!ident_is(ps, "MESSAGE_TEXT")) { parse_fail(ps, "Expected MESSAGE_TEXT", peek(ps)); free(t); return NULL; }
            consume(ps);
            if (!match(ps, TOK_EQ)) { parse_fail(ps, "Expected '=' after MESSAGE_TEXT", peek(ps)); free(t); return NULL; }
        }
        t->kind = TS_SIGNAL;
        if (peek(ps).type == TOK_SEMICOLON || peek(ps).type == TOK_END) {
            t->expr = new_expr(EXPR_STRING);
            if (t->expr) t->expr->str_val = strdup("Error raised by a trigger");
        } else {
            t->expr = parse_expr(ps);
        }
        if (!t->expr) { free(t); return NULL; }
        return t;
    }
    Token k = peek(ps);
    if (k.type != TOK_INSERT && k.type != TOK_UPDATE && k.type != TOK_DELETE && !query_starts(ps, 0)) {
        parse_fail(ps, "A trigger can contain INSERT, UPDATE, DELETE, SELECT, SET NEW.column, SIGNAL and IF", k);
        free(t);
        return NULL;
    }
    t->kind = TS_SQL;
    t->sql = parse_statement(ps);
    if (!t->sql || !ps->result->success) { trig_stmt_free(t); return NULL; }
    return t;
}

// [OF colonne] [FOR EACH ROW | STATEMENT] [WHEN (condizione)] corpo
static void parse_trigger_rest(ParserState* ps, TrigProgram* p) {
    p->for_each_row = true;
    if (ident_is(ps, "OF")) {
        consume(ps);
        do {
            Token c = consume(ps);
            if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name after OF", c); return; }
            if (p->n_of >= 8) { parse_fail(ps, "Too many columns after OF (max 8)", c); return; }
            copy_text(ps, p->of_cols[p->n_of++], 64, c.text);
        } while (match(ps, TOK_COMMA));
    }
    if (match(ps, TOK_FOR)) {
        match(ps, TOK_EACH);
        if (match(ps, TOK_ROW)) p->for_each_row = true;
        else if (ident_is(ps, "STATEMENT")) { consume(ps); p->for_each_row = false; }
        else { parse_fail(ps, "Expected ROW or STATEMENT after FOR EACH", peek(ps)); return; }
    }
    if (match(ps, TOK_WHEN)) {
        bool par = match(ps, TOK_LPAREN);
        p->when = parse_expr(ps);
        if (!p->when) return;
        if (par && !match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the WHEN condition", peek(ps)); return; }
    }
    if (match(ps, TOK_BEGIN)) {
        parse_trig_list(ps, &p->v, &p->n);
        if (ps->result->success && !match(ps, TOK_END)) parse_fail(ps, "Expected END to close the trigger body", peek(ps));
    } else {
        TrigStmt* st = parse_trig_stmt(ps);
        if (st) trig_push(ps, &p->v, &p->n, st);
    }
}

TrigProgram* parser_parse_trigger(TokenList* tokens) {
    TrigProgram* p = calloc(1, sizeof(TrigProgram));
    if (!p) return NULL;
    ParseResult r;
    memset(&r, 0, sizeof(r));
    r.success = true;
    ParserState ps = { tokens, 0, &r, 0 };
    parse_trigger_rest(&ps, p);
    match(&ps, TOK_SEMICOLON);
    if (r.success && peek(&ps).type != TOK_EOF) parse_fail(&ps, "Unexpected text after the trigger body", peek(&ps));
    p->success = r.success;
    snprintf(p->error, sizeof(p->error), "%s", r.error);
    return p;
}

// CREATE TRIGGER nome {BEFORE | AFTER} {INSERT | UPDATE [OF colonne] | DELETE} ON tabella resto
static ASTNode* parse_create_trigger(ParserState* ps, ASTNode* n) {
    n->type = NODE_CREATE_TRIGGER;
    Token tg = consume(ps);
    if (tg.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the trigger name", tg); return n; }
    copy_text(ps, n->create_trigger.name, sizeof(n->create_trigger.name), tg.text);
    if (match(ps, TOK_BEFORE)) n->create_trigger.before = true;
    else if (match(ps, TOK_AFTER)) n->create_trigger.before = false;
    else { parse_fail(ps, "Expected BEFORE or AFTER", peek(ps)); return n; }
    Token ev = consume(ps);
    if (ev.type != TOK_INSERT && ev.type != TOK_UPDATE && ev.type != TOK_DELETE) { parse_fail(ps, "Expected INSERT, UPDATE or DELETE", ev); return n; }
    copy_text(ps, n->create_trigger.event, sizeof(n->create_trigger.event), ev.type == TOK_INSERT ? "INSERT" : ev.type == TOK_UPDATE ? "UPDATE" : "DELETE");
    int of_start = ps->pos;
    if (ident_is(ps, "OF")) {                            // UPDATE OF colonne: le colonne vanno nel testo salvato
        if (ev.type != TOK_UPDATE) { parse_fail(ps, "OF columns is only allowed with UPDATE", peek(ps)); return n; }
        consume(ps);
        do { consume(ps); } while (match(ps, TOK_COMMA));
    }
    int of_end = ps->pos;
    if (!expect(ps, TOK_ON)) return n;
    Token tbl = consume(ps);
    if (tbl.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the table name after ON", tbl); return n; }
    copy_text(ps, n->create_trigger.table_name, sizeof(n->create_trigger.table_name), tbl.text);
    // il resto viene controllato ora e salvato come testo (OF ... + FOR EACH ... + corpo)
    int start = ps->pos;
    TrigProgram* prog = calloc(1, sizeof(TrigProgram));
    if (!prog) { parse_fail(ps, "Out of memory", peek(ps)); return n; }
    parse_trigger_rest(ps, prog);
    trig_program_free(prog);
    if (!ps->result->success) return n;
    int end = ps->pos;
    char* text = malloc(SV_MAX_SQL_LEN);
    if (!text) { parse_fail(ps, "Out of memory", peek(ps)); return n; }
    size_t len = 0;
    text[0] = '\0';
    if (of_end > of_start && !tokens_text(ps, of_start, of_end, text, SV_MAX_SQL_LEN - 1)) { free(text); parse_fail(ps, "Trigger is too long", peek(ps)); return n; }
    len = strlen(text);
    if (len) text[len++] = ' ';
    if (!tokens_text(ps, start, end, text + len, SV_MAX_SQL_LEN - len)) { free(text); parse_fail(ps, "Trigger is too long", peek(ps)); return n; }
    copy_text(ps, n->create_trigger.body, sizeof(n->create_trigger.body), text);
    free(text);
    return n;
}

// ═════════════════════════════════════════════════════════════════
//  CREATE TABLE con vincoli
//    CREATE TABLE t ( elemento {, elemento} )
//    elemento := colonna tipo [(n [, m])] { NOT NULL | NULL | PRIMARY KEY | UNIQUE | AUTO_INCREMENT
//                  | DEFAULT valore | [CONSTRAINT nome] CHECK (condizione)
//                  | [CONSTRAINT nome] REFERENCES t2 [(colonna)] [azioni] }
//              | [CONSTRAINT nome] PRIMARY KEY (colonne) | UNIQUE (colonne)
//              | FOREIGN KEY (colonne) REFERENCES t2 [(colonne)] [azioni] | CHECK (condizione)
//    azioni   := { ON DELETE | ON UPDATE  CASCADE | SET NULL | SET DEFAULT | RESTRICT | NO ACTION }
// ═════════════════════════════════════════════════════════════════

// testo SQL dei token [from, to): le stringhe tornano fra apici (con '' raddoppiati)
static bool tokens_text(ParserState* ps, int from, int to, char* out, size_t cap) {
    size_t di = 0;
    out[0] = '\0';
    for (int i = from; i < to; i++) {
        const Token* t = &ps->tokens->tokens[i];
        char piece[1100];
        if (t->type == TOK_STRING) {
            size_t k = 0;
            piece[k++] = '\'';
            for (const char* q = t->text; *q && k < sizeof(piece) - 3; q++) {
                if (*q == '\'' || *q == '\\') piece[k++] = *q;
                piece[k++] = *q;
            }
            piece[k++] = '\'';
            piece[k] = '\0';
        } else {
            snprintf(piece, sizeof(piece), "%s", t->text);
        }
        size_t pl = strlen(piece);
        bool tight = t->type == TOK_DOT || t->type == TOK_RPAREN || t->type == TOK_COMMA || t->type == TOK_SEMICOLON ||
                     (i > from && (ps->tokens->tokens[i - 1].type == TOK_DOT || ps->tokens->tokens[i - 1].type == TOK_LPAREN));
        if (di + pl + 2 >= cap) return false;
        if (di > 0 && !tight) out[di++] = ' ';
        memcpy(out + di, piece, pl);
        di += pl;
        out[di] = '\0';
    }
    return true;
}

// '(' nome {, nome} ')'
static int parse_name_list(ParserState* ps, char names[][64], int max) {
    int n = 0;
    if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' before the column list", peek(ps)); return 0; }
    do {
        Token c = consume(ps);
        if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", c); return n; }
        if (n >= max) { parse_fail(ps, "Too many columns in the key (max 8)", c); return n; }
        copy_text(ps, names[n++], 64, c.text);
    } while (match(ps, TOK_COMMA));
    if (!match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' after the column list", peek(ps));
    return n;
}

static int parse_fk_action(ParserState* ps) {
    if (ident_is(ps, "CASCADE"))  { consume(ps); return FK_CASCADE; }
    if (ident_is(ps, "RESTRICT")) { consume(ps); return FK_RESTRICT; }
    if (ident_is(ps, "NO"))       { consume(ps); if (ident_is(ps, "ACTION")) consume(ps); else parse_fail(ps, "Expected ACTION after NO", peek(ps)); return FK_NO_ACTION; }
    if (match(ps, TOK_SET)) {
        if (match(ps, TOK_NULL)) return FK_SET_NULL;
        if (ident_is(ps, "DEFAULT")) { consume(ps); return FK_SET_DEFAULT; }
        parse_fail(ps, "Expected NULL or DEFAULT after SET", peek(ps));
        return FK_NO_ACTION;
    }
    parse_fail(ps, "Expected CASCADE, SET NULL, SET DEFAULT, RESTRICT or NO ACTION", peek(ps));
    return FK_NO_ACTION;
}

// REFERENCES t [(colonne)] [ON DELETE azione] [ON UPDATE azione]   (REFERENCES già consumato)
static void parse_references(ParserState* ps, ConstraintDef* c) {
    Token t = consume(ps);
    if (t.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after REFERENCES", t); return; }
    copy_text(ps, c->ref_table, sizeof(c->ref_table), t.text);
    if (peek(ps).type == TOK_LPAREN) c->nref = parse_name_list(ps, c->ref_cols, 8);
    while (ps->result->success && peek(ps).type == TOK_ON) {
        consume(ps);
        if (match(ps, TOK_DELETE))      c->on_delete = parse_fk_action(ps);
        else if (match(ps, TOK_UPDATE)) c->on_update = parse_fk_action(ps);
        else { parse_fail(ps, "Expected DELETE or UPDATE after ON", peek(ps)); return; }
    }
}

// CHECK '(' condizione ')'   (CHECK già consumato): salva il testo della condizione
static void parse_check(ParserState* ps, ConstraintDef* c) {
    if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' after CHECK", peek(ps)); return; }
    int start = ps->pos;
    ASTExpr* e = parse_expr(ps);
    int end = ps->pos;
    ast_expr_free(e);
    if (!ps->result->success) return;
    if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the CHECK condition", peek(ps)); return; }
    if (!tokens_text(ps, start, end, c->check, sizeof(c->check)))
        parse_fail(ps, "CHECK condition is too long (max 511 characters)", peek(ps));
}

static ConstraintDef* new_cons(ParserState* ps, ASTNode* n) {
    if (n->create_table.num_cons >= SV_MAX_CONS_DEFS) { parse_fail(ps, "Too many constraints (max 16)", peek(ps)); return NULL; }
    ConstraintDef* c = &n->create_table.cons[n->create_table.num_cons++];
    memset(c, 0, sizeof(*c));
    return c;
}

// vincolo di tabella (dopo l'eventuale CONSTRAINT nome); false se non è un vincolo
static bool parse_table_constraint(ParserState* ps, ConstraintDef* c) {
    if (ident_is(ps, "PRIMARY")) {
        consume(ps);
        expect(ps, TOK_KEY);
        c->kind = CONS_PRIMARY;
        c->ncols = parse_name_list(ps, c->cols, 8);
    } else if (ident_is(ps, "UNIQUE")) {
        consume(ps);
        if (peek(ps).type == TOK_KEY) consume(ps);
        c->kind = CONS_UNIQUE;
        c->ncols = parse_name_list(ps, c->cols, 8);
    } else if (ident_is(ps, "FOREIGN")) {
        consume(ps);
        expect(ps, TOK_KEY);
        c->kind = CONS_FOREIGN;
        c->ncols = parse_name_list(ps, c->cols, 8);
        if (!ident_is(ps, "REFERENCES")) { parse_fail(ps, "Expected REFERENCES", peek(ps)); return true; }
        consume(ps);
        parse_references(ps, c);
    } else if (ident_is(ps, "CHECK")) {
        consume(ps);
        c->kind = CONS_CHECK;
        parse_check(ps, c);
    } else {
        return false;
    }
    return true;
}

// definizione di colonna: nome tipo [(n [, m])] modificatori (vincoli di colonna in n->create_table.cons)
// tipo di una colonna: nome [(dimensione [, decimali])]
static bool parse_type_spec(ParserState* ps, ColumnDef* cd) {
    Token ct = consume(ps);
    if (ct.type != TOK_IDENTIFIER && ct.type != TOK_BOOLEAN) { parse_fail(ps, "Expected a column type", ct); return false; }
    copy_text(ps, cd->type, sizeof(cd->type), ct.text);
    if (match(ps, TOK_LPAREN)) {
        Token sz = consume(ps);
        if (sz.type != TOK_INTEGER) { parse_fail(ps, "Expected a size", sz); return false; }
        cd->size = atoi(sz.text);
        if (match(ps, TOK_COMMA)) {                    // DECIMAL(p, s)
            Token sc = consume(ps);
            if (sc.type != TOK_INTEGER) { parse_fail(ps, "Expected the number of decimal digits", sc); return false; }
        }
        if (!expect(ps, TOK_RPAREN)) return false;
    }
    return true;
}

// valore dopo DEFAULT: numero (anche con segno), stringa, NULL o parola
static bool parse_default_value(ParserState* ps, char* out, size_t n) {
    Token dv = consume(ps);
    if (dv.type == TOK_MINUS || dv.type == TOK_PLUS) {          // DEFAULT -1
        Token num = consume(ps);
        if (num.type != TOK_INTEGER && num.type != TOK_FLOAT) { parse_fail(ps, "Expected a number after the sign", num); return false; }
        snprintf(out, n, "%s%.100s", dv.type == TOK_MINUS ? "-" : "", num.text);
        return true;
    }
    if (dv.type == TOK_INTEGER || dv.type == TOK_FLOAT || dv.type == TOK_STRING || dv.type == TOK_NULL || dv.type == TOK_IDENTIFIER) {
        copy_text(ps, out, n, dv.text);
        return true;
    }
    parse_fail(ps, "Expected a value after DEFAULT", dv);
    return false;
}

static void parse_column_def(ParserState* ps, ASTNode* n) {
    if (n->create_table.num_columns >= 64) { parse_fail(ps, "Too many columns (max 64)", peek(ps)); return; }
    ColumnDef* cd = &n->create_table.columns[n->create_table.num_columns];
    memset(cd, 0, sizeof(ColumnDef));
    cd->nullable = true;                       // come in SQL standard: NULL ammesso se non dichiarato
    Token cn = consume(ps);
    if (cn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", cn); return; }
    copy_text(ps, cd->name, sizeof(cd->name), cn.text);
    if (!parse_type_spec(ps, cd)) return;
    n->create_table.num_columns++;
    for (;;) {
        if (!ps->result->success) return;
        char ccn[64] = "";
        if (ident_is(ps, "CONSTRAINT")) {
            consume(ps);
            Token nm = consume(ps);
            if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a name after CONSTRAINT", nm); return; }
            copy_text(ps, ccn, sizeof(ccn), nm.text);
            if (!ident_is(ps, "CHECK") && !ident_is(ps, "REFERENCES") && !ident_is(ps, "UNIQUE") && !ident_is(ps, "PRIMARY")) {
                parse_fail(ps, "Expected CHECK, REFERENCES, UNIQUE or PRIMARY KEY after the constraint name", peek(ps));
                return;
            }
        }
        if (match(ps, TOK_NOT)) {
            if (!match(ps, TOK_NULL)) { parse_fail(ps, "Expected NULL after NOT", peek(ps)); return; }
            cd->nullable = false;
        } else if (match(ps, TOK_NULL)) {
            cd->nullable = true;
        } else if (ident_is(ps, "PRIMARY")) {
            consume(ps);
            expect(ps, TOK_KEY);
            cd->primary_key = true;
            cd->nullable = false;
            if (ccn[0]) {
                ConstraintDef* c = new_cons(ps, n);
                if (!c) return;
                memcpy(c->name, ccn, sizeof(c->name));
                c->kind = CONS_PRIMARY; c->ncols = 1;
                memcpy(c->cols[0], cd->name, 64);
            }
        } else if (ident_is(ps, "UNIQUE")) {
            consume(ps);
            ConstraintDef* c = new_cons(ps, n);
            if (!c) return;
            memcpy(c->name, ccn, sizeof(c->name));
            c->kind = CONS_UNIQUE; c->ncols = 1;
            memcpy(c->cols[0], cd->name, 64);
        } else if (ident_is(ps, "AUTO_INCREMENT") || ident_is(ps, "AUTOINCREMENT")) {
            consume(ps);
            cd->auto_increment = true;
        } else if (ident_is(ps, "DEFAULT")) {
            consume(ps);
            if (!parse_default_value(ps, cd->default_val, sizeof(cd->default_val))) return;
        } else if (ident_is(ps, "CHECK")) {
            consume(ps);
            ConstraintDef* c = new_cons(ps, n);
            if (!c) return;
            memcpy(c->name, ccn, sizeof(c->name));
            c->kind = CONS_CHECK;
            parse_check(ps, c);
        } else if (ident_is(ps, "REFERENCES")) {
            consume(ps);
            ConstraintDef* c = new_cons(ps, n);
            if (!c) return;
            memcpy(c->name, ccn, sizeof(c->name));
            c->kind = CONS_FOREIGN; c->ncols = 1;
            memcpy(c->cols[0], cd->name, 64);
            parse_references(ps, c);
        } else {
            break;
        }
    }
}

static void parse_create_table(ParserState* ps, ASTNode* n) {
    n->type = NODE_CREATE_TABLE;
    Token tn = consume(ps);
    if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after CREATE TABLE", tn); return; }
    copy_text(ps, n->create_table.table_name, sizeof(n->create_table.table_name), tn.text);
    if (!expect(ps, TOK_LPAREN)) return;
    do {
        if (!ps->result->success) return;
        // vincolo di tabella
        char cname[64] = "";
        bool named = false;
        if (ident_is(ps, "CONSTRAINT")) {
            consume(ps);
            Token nm = consume(ps);
            if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a name after CONSTRAINT", nm); return; }
            copy_text(ps, cname, sizeof(cname), nm.text);
            named = true;
        }
        if (named || ident_is(ps, "PRIMARY") || ident_is(ps, "UNIQUE") || ident_is(ps, "FOREIGN") || ident_is(ps, "CHECK")) {
            ConstraintDef* c = new_cons(ps, n);
            if (!c) return;
            memcpy(c->name, cname, sizeof(c->name));
            if (!parse_table_constraint(ps, c)) { parse_fail(ps, "Expected PRIMARY KEY, UNIQUE, FOREIGN KEY or CHECK", peek(ps)); return; }
            continue;
        }
        parse_column_def(ps, n);
        if (!ps->result->success) return;
    } while (match(ps, TOK_COMMA));
    expect(ps, TOK_RPAREN);
}

static ASTNode* parse_statement(ParserState* ps) {
    ASTNode* n = calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    Token t = peek(ps);

    switch (t.type) {
        case TOK_SELECT:
            free(n);
            return parse_query(ps);
        case TOK_INSERT:
            consume(ps);
            parse_insert(ps, n);
            break;
        case TOK_UPDATE:
            consume(ps);
            parse_update(ps, n);
            break;
        case TOK_DELETE: {
            consume(ps);
            n->type = NODE_DELETE;
            expect(ps, TOK_FROM);
            Token tn = consume(ps);
            if (tn.type != TOK_IDENTIFIER) parse_fail(ps, "Expected a table name after DELETE FROM", tn);
            copy_text(ps, n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), tn.text);
            if (ps->result->success && match(ps, TOK_WHERE)) n->delete_stmt.where = parse_expr(ps);
            break;
        }
        case TOK_CREATE: {
            consume(ps);
            Token sub = consume(ps);
            if (ident_is(ps, "IF")) {                            // CREATE ... IF NOT EXISTS
                consume(ps);
                if (!match(ps, TOK_NOT) || !match(ps, TOK_EXISTS)) { parse_fail(ps, "Expected IF NOT EXISTS", peek(ps)); break; }
                n->if_exists = true;
            }
            if (sub.type == TOK_DATABASE) {
                n->type = NODE_CREATE_DB;
                Token dn = consume(ps);
                copy_text(ps, n->create_db.db_name, sizeof(n->create_db.db_name), dn.text);
            } else if (sub.type == TOK_TRIGGER) {
                return parse_create_trigger(ps, n);
            } else if (sub.type == TOK_TABLE) {
                parse_create_table(ps, n);
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
                // la definizione viene ricostruita dai token: le stringhe tornano tra apici (con ''
                // raddoppiati), altrimenti alla rilettura 'x' diventerebbe la colonna x
                if (!query_starts(ps, 0)) { parse_fail(ps, "A view must be defined by a SELECT", peek(ps)); break; }
                int start = ps->pos;
                char def[sizeof(n->create_view.definition)];
                size_t di = 0;
                bool fits = true;
                while (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF) {
                    Token t = consume(ps);
                    char piece[1100];
                    if (t.type == TOK_STRING) {
                        size_t k = 0;
                        piece[k++] = '\'';
                        for (const char* q = t.text; *q && k < sizeof(piece) - 3; q++) {
                            if (*q == '\'' || *q == '\\') piece[k++] = *q;   // '' e \\ come nel testo originale
                            piece[k++] = *q;
                        }
                        piece[k++] = '\'';
                        piece[k] = '\0';
                    } else {
                        snprintf(piece, sizeof(piece), "%s", t.text);
                    }
                    size_t pl = strlen(piece);
                    if (di + pl + 2 >= sizeof(def)) { fits = false; break; }
                    if (di > 0) def[di++] = ' ';
                    memcpy(def + di, piece, pl);
                    di += pl;
                }
                def[di] = '\0';
                if (!fits) { parse_fail(ps, "View definition is too long (max 2047 characters)", peek(ps)); break; }
                // la definizione deve essere una SELECT valida
                {
                    int save = ps->pos;
                    ps->pos = start;
                    ASTNode* probe = parse_query(ps);
                    bool clean = ps->result->success && (peek(ps).type == TOK_SEMICOLON || peek(ps).type == TOK_EOF);
                    free_node(probe);
                    if (ps->result->success && !clean) parse_fail(ps, "Unexpected text in the view definition", peek(ps));
                    ps->pos = save;
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
            if (match(ps, TOK_RENAME) && ident_is(ps, "COLUMN")) {         // RENAME COLUMN a TO b
                consume(ps);
                Token a = consume(ps);
                if (a.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the column name after RENAME COLUMN", a); break; }
                if (!expect(ps, TOK_TO)) break;
                Token b = consume(ps);
                if (b.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the new column name after TO", b); break; }
                copy_text(ps, n->alter_table.rename_from, sizeof(n->alter_table.rename_from), a.text);
                copy_text(ps, n->alter_table.rename_to, sizeof(n->alter_table.rename_to), b.text);
            } else if (ps->tokens->tokens[ps->pos - 1].type == TOK_RENAME) {
                expect(ps, TOK_TO);
                Token nn = consume(ps);
                if (nn.type != TOK_IDENTIFIER) parse_fail(ps, "Expected the new table name after RENAME TO", nn);
                copy_text(ps, n->alter_table.new_name, sizeof(n->alter_table.new_name), nn.text);
            } else if (ident_is(ps, "ADD")) {
                // ALTER TABLE t ADD [CONSTRAINT nome] PRIMARY KEY | UNIQUE | FOREIGN KEY | CHECK ...
                consume(ps);
                if (ident_is(ps, "COLUMN") || (peek(ps).type == TOK_IDENTIFIER && !ident_is(ps, "CONSTRAINT") &&
                    !ident_is(ps, "PRIMARY") && !ident_is(ps, "UNIQUE") && !ident_is(ps, "FOREIGN") && !ident_is(ps, "CHECK"))) {
                    if (ident_is(ps, "COLUMN")) consume(ps);           // ADD [COLUMN] nome tipo modificatori
                    ASTNode* def = calloc(1, sizeof(ASTNode));
                    if (!def) { parse_fail(ps, "Out of memory", peek(ps)); break; }
                    def->type = NODE_CREATE_TABLE;
                    n->alter_table.add_column = def;
                    parse_column_def(ps, def);
                    break;
                }
                ConstraintDef* c = &n->alter_table.add;
                memset(c, 0, sizeof(*c));
                if (ident_is(ps, "CONSTRAINT")) {
                    consume(ps);
                    Token nm = consume(ps);
                    if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a name after CONSTRAINT", nm); break; }
                    copy_text(ps, c->name, sizeof(c->name), nm.text);
                }
                if (!parse_table_constraint(ps, c))
                    parse_fail(ps, "Expected PRIMARY KEY, UNIQUE, FOREIGN KEY or CHECK after ADD", peek(ps));
                n->alter_table.has_add = true;
            } else if (match(ps, TOK_DROP)) {
                if (!ident_is(ps, "CONSTRAINT")) {                          // DROP [COLUMN] nome
                    if (ident_is(ps, "COLUMN")) consume(ps);
                    Token c = consume(ps);
                    if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the column name after DROP", c); break; }
                    copy_text(ps, n->alter_table.drop_column, sizeof(n->alter_table.drop_column), c.text);
                    break;
                }
                consume(ps);
                Token nm = consume(ps);
                if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the constraint name", nm); break; }
                copy_text(ps, n->alter_table.drop_constraint, sizeof(n->alter_table.drop_constraint), nm.text);
            } else if (match(ps, TOK_ALTER) || ident_is(ps, "MODIFY")) {
                // ALTER [COLUMN] c TYPE t | SET DATA TYPE t | SET DEFAULT v | DROP DEFAULT | SET / DROP NOT NULL
                bool modify = ps->tokens->tokens[ps->pos - 1].type != TOK_ALTER;
                if (modify) consume(ps);
                if (ident_is(ps, "COLUMN")) consume(ps);
                Token c = consume(ps);
                if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the column name", c); break; }
                copy_text(ps, n->alter_table.alter_col, sizeof(n->alter_table.alter_col), c.text);
                ColumnDef* d = &n->alter_table.alter_def;
                memset(d, 0, sizeof(*d));
                if (modify) { n->alter_table.alter_kind = ALT_TYPE; parse_type_spec(ps, d); break; }
                if (ident_is(ps, "TYPE")) { consume(ps); n->alter_table.alter_kind = ALT_TYPE; parse_type_spec(ps, d); break; }
                bool set = match(ps, TOK_SET);
                if (!set && !match(ps, TOK_DROP)) { parse_fail(ps, "Expected TYPE, SET or DROP after the column name", peek(ps)); break; }
                if (set && ident_is(ps, "DATA")) {
                    consume(ps);
                    if (!ident_is(ps, "TYPE")) { parse_fail(ps, "Expected TYPE after SET DATA", peek(ps)); break; }
                    consume(ps);
                    n->alter_table.alter_kind = ALT_TYPE;
                    parse_type_spec(ps, d);
                } else if (ident_is(ps, "DEFAULT")) {
                    consume(ps);
                    n->alter_table.alter_kind = set ? ALT_SET_DEFAULT : ALT_DROP_DEFAULT;
                    if (set) parse_default_value(ps, d->default_val, sizeof(d->default_val));
                } else if (match(ps, TOK_NOT)) {
                    if (!match(ps, TOK_NULL)) { parse_fail(ps, "Expected NULL after NOT", peek(ps)); break; }
                    n->alter_table.alter_kind = set ? ALT_SET_NOT_NULL : ALT_DROP_NOT_NULL;
                } else {
                    parse_fail(ps, set ? "Expected DATA TYPE, DEFAULT or NOT NULL after SET" : "Expected DEFAULT or NOT NULL after DROP", peek(ps));
                }
            } else {
                parse_fail(ps, "Expected RENAME, ADD, DROP or ALTER after ALTER TABLE name", peek(ps));
            }
            break;
        }
        case TOK_DROP: {
            consume(ps);
            Token sub = consume(ps);
            if (ident_is(ps, "IF")) {                            // DROP ... IF EXISTS
                consume(ps);
                if (!match(ps, TOK_EXISTS)) { parse_fail(ps, "Expected IF EXISTS", peek(ps)); break; }
                n->if_exists = true;
            }
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
            // SHOW DATABASES | TABLES | STATUS | CREATE TABLE t | CONSTRAINTS FROM t | INDEXES FROM t
            // (il tipo va in table_name, la tabella in table_alias)
            Token what = consume(ps);
            const char* kind = NULL;
            bool needs_table = false;
            if (!strcasecmp(what.text, "DATABASES")) kind = "DATABASES";
            else if (!strcasecmp(what.text, "TABLES")) kind = "TABLES";
            else if (!strcasecmp(what.text, "STATUS")) kind = "STATUS";
            else if (!strcasecmp(what.text, "TRIGGERS")) kind = "TRIGGERS";
            else if (what.type == TOK_CREATE) { expect(ps, TOK_TABLE); kind = "CREATE"; needs_table = true; }
            else if (!strcasecmp(what.text, "CONSTRAINTS")) { kind = "CONSTRAINTS"; needs_table = true; }
            else if (what.type == TOK_INDEX || !strcasecmp(what.text, "INDEXES") || !strcasecmp(what.text, "KEYS")) { kind = "INDEXES"; needs_table = true; }
            if (!kind) { parse_fail(ps, "Expected DATABASES, TABLES, STATUS, TRIGGERS, CREATE TABLE, CONSTRAINTS or INDEXES after SHOW", what); break; }
            if (!strcmp(kind, "TRIGGERS") && (match(ps, TOK_FROM) || match(ps, TOK_IN))) {      // SHOW TRIGGERS [FROM t]
                Token tn = consume(ps);
                if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", tn); break; }
                copy_text(ps, n->select_stmt.table_alias, sizeof(n->select_stmt.table_alias), tn.text);
            }
            snprintf(n->select_stmt.table_name, sizeof(n->select_stmt.table_name), "%s", kind);
            if (needs_table) {
                if (strcmp(kind, "CREATE") && !match(ps, TOK_FROM) && !match(ps, TOK_IN)) { parse_fail(ps, "Expected FROM table", peek(ps)); break; }
                Token tn = consume(ps);
                if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", tn); break; }
                copy_text(ps, n->select_stmt.table_alias, sizeof(n->select_stmt.table_alias), tn.text);
            }
            break;
        }
        case TOK_DESCRIBE: {
            consume(ps);
            n->type = NODE_DESCRIBE;
            if (peek(ps).type == TOK_TABLE) consume(ps);          // DESCRIBE TABLE t
            Token tn = consume(ps);
            if (tn.type == TOK_IDENTIFIER) {
                // table name stored in select_stmt.table_name for convenience
                copy_text(ps, n->select_stmt.table_name, sizeof(n->select_stmt.table_name), tn.text);
            } else {
                parse_fail(ps, "Expected a table name after DESCRIBE", tn);
            }
            break;
        }
        case TOK_EXPLAIN: {
            consume(ps);
            n->type = NODE_EXPLAIN;
            if (ident_is(ps, "ANALYZE")) { consume(ps); n->explain.analyze = true; }
            if (!query_starts(ps, 0)) { parse_fail(ps, "EXPLAIN works with SELECT statements", peek(ps)); break; }
            n->explain.stmt = parse_query(ps);
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
            if (query_starts(ps, 0)) { free(n); return parse_query(ps); }   // WITH ...
            if (ident_is(ps, "CHECKPOINT")) { consume(ps); n->type = NODE_CHECKPOINT; break; }
            snprintf(ps->result->error, sizeof(ps->result->error),
                     "Unexpected token: %.200s at line %d", t.text, t.line);
            ps->result->success = false;
            free(n);
            return NULL;
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
    ps.depth = 0;

    // errori del lexer (stringa non chiusa o troppo lunga): sono più precisi di quelli del parser
    for (int i = 0; i < tokens->count; i++) {
        Token* t = &tokens->tokens[i];
        if (t->type == TOK_ERROR && strchr(t->text, ' ')) {
            snprintf(r->error, sizeof(r->error), "%.200s (line %d)", t->text, t->line);
            r->success = false;
            return r;
        }
    }

    while (peek(&ps).type != TOK_EOF) {
        ASTNode* stmt = parse_statement(&ps);
        if (stmt) add_stmt(&ps, stmt);
        match(&ps, TOK_SEMICOLON);
        if (!ps.result->success) break;
    }

    return r;
}

ASTExpr* parser_parse_expr(TokenList* tokens, char* err, size_t errn) {
    ParseResult r;
    memset(&r, 0, sizeof(r));
    r.success = true;
    ParserState ps = { tokens, 0, &r, 0 };
    ASTExpr* e = parse_expr(&ps);
    if (r.success && peek(&ps).type != TOK_EOF) parse_fail(&ps, "Unexpected text after the condition", peek(&ps));
    if (!r.success) { ast_expr_free(e); snprintf(err, errn, "%s", r.error); return NULL; }
    return e;
}

static void free_node(ASTNode* n) {
    if (!n) return;
    switch (n->type) {
        case NODE_SELECT:
            for (int j = 0; j < n->select_stmt.num_cols; j++) ast_expr_free(n->select_stmt.columns[j]);
            ast_expr_free(n->select_stmt.where);
            ast_expr_free(n->select_stmt.having);
            for (int j = 0; j < n->select_stmt.num_joins; j++) {
                ast_expr_free(n->select_stmt.joins[j].condition);
                free_node(n->select_stmt.joins[j].sub);
            }
            free_node(n->select_stmt.from_sub);
            free_node(n->select_stmt.set_next);
            for (int j = 0; j < n->select_stmt.num_ctes; j++) free_node(n->select_stmt.ctes[j].node);
            free(n->select_stmt.ctes);
            for (int j = 0; j < n->select_stmt.num_group; j++) ast_expr_free(n->select_stmt.group_exprs[j]);
            for (int j = 0; j < n->select_stmt.num_order; j++) ast_expr_free(n->select_stmt.order_exprs[j]);
            break;
        case NODE_INSERT:
            for (int j = 0; j < n->insert_stmt.num_cols; j++) free(n->insert_stmt.columns[j]);
            for (int j = 0; j < n->insert_stmt.num_rows * n->insert_stmt.num_values; j++)
                ast_expr_free(n->insert_stmt.values[j]);
            free(n->insert_stmt.values);
            free_node(n->insert_stmt.select);
            break;
        case NODE_UPDATE:
            for (int j = 0; j < n->update_stmt.num_sets; j++) ast_expr_free(n->update_stmt.set_exprs[j]);
            ast_expr_free(n->update_stmt.where);
            break;
        case NODE_DELETE:
            ast_expr_free(n->delete_stmt.where);
            break;
        case NODE_EXPLAIN:
            free_node((ASTNode*)n->explain.stmt);
            break;
        case NODE_ALTER_TABLE:
            free_node(n->alter_table.add_column);
            break;
        default:
            break;
    }
    free(n);
}

void parse_result_free(ParseResult* r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) free_node(r->statements[i]);
    free(r->statements);
    free(r);
}
