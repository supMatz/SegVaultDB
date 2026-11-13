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
    char       qual_db[64];    // database dei nomi db.tabella dello statement ("" = nessuno)
    bool       plain_table;    // lo statement usa anche tabelle senza database
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

// nome di tabella, anche qualificato col database (shop.prodotto): il database va nello statement.
// Per ora un solo database per statement (quello dei nomi qualificati)
static bool next_is(ParserState* ps, int ahead, TokenType tt);
static void parse_fail(ParserState* ps, const char* msg, Token at);
static Token table_tok(ParserState* ps) {
    Token t = consume(ps);
    if (t.type != TOK_IDENTIFIER) return t;
    if (peek(ps).type == TOK_DOT && next_is(ps, 1, TOK_IDENTIFIER)) {
        consume(ps);
        Token n = consume(ps);
        if (strlen(t.text) >= sizeof(ps->qual_db)) parse_fail(ps, "Database name is too long", t);
        else if (ps->qual_db[0] && strcasecmp(ps->qual_db, t.text))
            parse_fail(ps, "Tables of different databases in one statement are not supported yet", t);
        else snprintf(ps->qual_db, sizeof(ps->qual_db), "%s", t.text);
        return n;
    }
    ps->plain_table = true;
    return t;
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
        case OP_ESCAPE: return " ESCAPE ";
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
static ASTExpr* mkfunc2(const char* name, ASTExpr* a, ASTExpr* b);
static bool is_interval(const ASTExpr* e);
static bool peek2_ident(ParserState* ps, const char* w);
static ASTExpr* parse_match(ParserState* ps);


static bool ident_is(ParserState* ps, const char* word);
static bool word_is(ParserState* ps, const char* word);
static bool tokens_text(ParserState* ps, int from, int to, char* out, size_t cap);

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


// funzione con due argomenti già analizzati (NULL se manca memoria: gli argomenti vengono liberati)
static ASTExpr* mkfunc2(const char* name, ASTExpr* a, ASTExpr* b) {
    ASTExpr* f = new_expr(EXPR_FUNC);
    if (!f || !a || !b) { ast_expr_free(f); ast_expr_free(a); ast_expr_free(b); return NULL; }
    snprintf(f->func, sizeof(f->func), "%s", name);
    f->args = calloc(2, sizeof(ASTExpr*));
    if (!f->args) { ast_expr_free(f); ast_expr_free(a); ast_expr_free(b); return NULL; }
    f->args[0] = a;
    f->args[1] = b;
    f->nargs = 2;
    f->depth = (a->depth > b->depth ? a->depth : b->depth) + 1;
    return f;
}

// aggiunge un terzo argomento a una funzione di mkfunc2
static ASTExpr* add_arg(ASTExpr* f, ASTExpr* x) {
    if (!f || !x) { ast_expr_free(f); ast_expr_free(x); return NULL; }
    ASTExpr** na = realloc(f->args, (size_t)(f->nargs + 1) * sizeof(ASTExpr*));
    if (!na) { ast_expr_free(f); ast_expr_free(x); return NULL; }
    f->args = na;
    f->args[f->nargs++] = x;
    if (x->depth + 1 > f->depth) f->depth = x->depth + 1;
    return f;
}

static ASTExpr* mkstr(const char* text) {
    ASTExpr* e = new_expr(EXPR_STRING);
    if (e && !(e->str_val = strdup(text))) { free(e); return NULL; }
    return e;
}

static bool is_interval(const ASTExpr* e) { return e->type == EXPR_FUNC && !strcmp(e->func, "__INTERVAL"); }

// unità di INTERVAL / EXTRACT / TIMESTAMPDIFF (DAY, HOUR, DAY_HOUR, ...): testo maiuscolo
static bool parse_unit(ParserState* ps, char* out, size_t n) {
    Token u = consume(ps);
    if (u.type != TOK_IDENTIFIER && u.type != TOK_STRING) { parse_fail(ps, "Expected a time unit (DAY, HOUR, ...)", u); return false; }
    copy_text(ps, out, n, u.text);
    for (char* q = out; *q; q++) *q = (char)toupper((unsigned char)*q);
    return true;
}

static ASTExpr* parse_unary(ParserState* ps);

// forme di MySQL con parole chiave fra gli argomenti; NULL (posizione invariata) se non è una di queste
static ASTExpr* parse_special_form(ParserState* ps) {
    Token t = peek(ps);
    if (t.type != TOK_IDENTIFIER || !t.text) return NULL;
    const char* w = t.text;
    bool call = next_is(ps, 1, TOK_LPAREN);
    static const char* const NOW_WORDS[] = { "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME", "LOCALTIME", "LOCALTIMESTAMP",
                                             "UTC_TIMESTAMP", "UTC_DATE", "UTC_TIME", "CURRENT_USER" };
    if (!call) {
        for (size_t i = 0; i < sizeof(NOW_WORDS) / sizeof(NOW_WORDS[0]); i++)
            if (!strcasecmp(w, NOW_WORDS[i])) {
                consume(ps);
                ASTExpr* f = new_expr(EXPR_FUNC);
                if (f) snprintf(f->func, sizeof(f->func), "%s", NOW_WORDS[i]);
                return f;
            }
        if (w[0] == '_' && next_is(ps, 1, TOK_STRING)) { consume(ps); Token st = consume(ps); return mkstr(st.text); }   // _utf8mb4'x'
        if ((!strcasecmp(w, "DATE") || !strcasecmp(w, "TIME") || !strcasecmp(w, "TIMESTAMP")) && next_is(ps, 1, TOK_STRING)) {
            consume(ps); Token st = consume(ps); return mkstr(st.text);
        }
        if (!strcasecmp(w, "BINARY") && !next_is(ps, 1, TOK_COMMA) && !next_is(ps, 1, TOK_RPAREN) && !next_is(ps, 1, TOK_EOF)) {
            consume(ps);
            return parse_unary(ps);
        }
        if (!strcasecmp(w, "INTERVAL") && !next_is(ps, 1, TOK_COMMA) && !next_is(ps, 1, TOK_RPAREN) && !next_is(ps, 1, TOK_EOF)) {
            consume(ps);
            ASTExpr* v = parse_unary(ps);
            char unit[32];
            if (!v || !parse_unit(ps, unit, sizeof(unit))) { ast_expr_free(v); return NULL; }
            return mkfunc2("__INTERVAL", v, mkstr(unit));
        }
        return NULL;
    }
    if (!strcasecmp(w, "CAST") || !strcasecmp(w, "CONVERT")) {   // CAST(x AS tipo), CONVERT(x, tipo), CONVERT(x USING cs)
        bool cast = !strcasecmp(w, "CAST");
        consume(ps); consume(ps);
        ASTExpr* v = parse_expr(ps);
        if (!v) return NULL;
        if (!cast && ident_is(ps, "USING")) {
            consume(ps); consume(ps);
            if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')'", peek(ps)); ast_expr_free(v); return NULL; }
            return v;
        }
        if (cast ? !match(ps, TOK_AS) : !match(ps, TOK_COMMA)) {
            parse_fail(ps, cast ? "Expected AS in CAST" : "Expected ',' in CONVERT", peek(ps));
            ast_expr_free(v);
            return NULL;
        }
        char ty[64] = "";
        size_t tl = 0;
        int depth = 0;
        while (peek(ps).type != TOK_EOF && (depth > 0 || peek(ps).type != TOK_RPAREN)) {   // tipo, anche DECIMAL(10,2)
            Token x = consume(ps);
            if (x.type == TOK_LPAREN) depth++;
            if (x.type == TOK_RPAREN) depth--;
            const char* txt = x.type == TOK_LPAREN ? "(" : x.type == TOK_RPAREN ? ")" : x.type == TOK_COMMA ? "," : x.text ? x.text : "";
            bool space = tl > 0 && x.type != TOK_LPAREN && x.type != TOK_RPAREN && x.type != TOK_COMMA && ty[tl - 1] != '(' && ty[tl - 1] != ',';
            int k = snprintf(ty + tl, sizeof(ty) - tl, "%s%s", space ? " " : "", txt);
            if (k > 0) tl += (size_t)k;
            if (tl >= sizeof(ty)) tl = sizeof(ty) - 1;
        }
        if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')'", peek(ps)); ast_expr_free(v); return NULL; }
        for (char* q = ty; *q; q++) *q = (char)toupper((unsigned char)*q);
        return mkfunc2("CAST", v, mkstr(ty));
    }
    if (!strcasecmp(w, "EXTRACT")) {                            // EXTRACT(unità FROM data)
        consume(ps); consume(ps);
        char unit[32];
        if (!parse_unit(ps, unit, sizeof(unit))) return NULL;
        if (!match(ps, TOK_FROM)) { parse_fail(ps, "Expected FROM in EXTRACT", peek(ps)); return NULL; }
        ASTExpr* v = parse_expr(ps);
        if (!v) return NULL;
        if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')'", peek(ps)); ast_expr_free(v); return NULL; }
        return mkfunc2("EXTRACT", mkstr(unit), v);
    }
    if (!strcasecmp(w, "TIMESTAMPDIFF") || !strcasecmp(w, "TIMESTAMPADD")) {   // TIMESTAMPDIFF(unità, a, b)
        bool diff = !strcasecmp(w, "TIMESTAMPDIFF");
        consume(ps); consume(ps);
        char unit[32];
        if (!parse_unit(ps, unit, sizeof(unit))) return NULL;
        if (!match(ps, TOK_COMMA)) { parse_fail(ps, "Expected ','", peek(ps)); return NULL; }
        ASTExpr* a = parse_expr(ps);
        if (!a) return NULL;
        if (!match(ps, TOK_COMMA)) { ast_expr_free(a); parse_fail(ps, "Expected ','", peek(ps)); return NULL; }
        ASTExpr* b = parse_expr(ps);
        if (!b) { ast_expr_free(a); return NULL; }
        if (!match(ps, TOK_RPAREN)) { ast_expr_free(a); ast_expr_free(b); parse_fail(ps, "Expected ')'", peek(ps)); return NULL; }
        if (!diff) return mkfunc2("DATE_ADD", b, mkfunc2("__INTERVAL", a, mkstr(unit)));
        return add_arg(mkfunc2("TIMESTAMPDIFF", a, b), mkstr(unit));
    }
    if (!strcasecmp(w, "POSITION")) {                           // POSITION(sub IN s)
        int save = ps->pos;
        consume(ps); consume(ps);
        ASTExpr* a = parse_additive(ps);
        if (!a || !match(ps, TOK_IN)) { ast_expr_free(a); ps->pos = save; return NULL; }
        ASTExpr* b = parse_expr(ps);
        if (!b) { ast_expr_free(a); return NULL; }
        if (!match(ps, TOK_RPAREN)) { ast_expr_free(a); ast_expr_free(b); parse_fail(ps, "Expected ')'", peek(ps)); return NULL; }
        return mkfunc2("LOCATE", a, b);
    }
    if (!strcasecmp(w, "TRIM")) {                               // TRIM([LEADING|TRAILING|BOTH] [x] FROM s) / TRIM(s)
        consume(ps); consume(ps);
        char mode[12] = "BOTH";
        bool kw = false;
        if (ident_is(ps, "LEADING") || ident_is(ps, "TRAILING") || ident_is(ps, "BOTH")) {
            Token m = consume(ps);
            copy_text(ps, mode, sizeof(mode), m.text);
            for (char* q = mode; *q; q++) *q = (char)toupper((unsigned char)*q);
            kw = true;
        }
        ASTExpr* rem = NULL;
        ASTExpr* str = NULL;
        if (kw && match(ps, TOK_FROM)) str = parse_expr(ps);
        else {
            ASTExpr* first = parse_expr(ps);
            if (!first) return NULL;
            if (match(ps, TOK_FROM)) { rem = first; str = parse_expr(ps); }
            else str = first;
        }
        if (!str || !match(ps, TOK_RPAREN)) {
            ast_expr_free(rem); ast_expr_free(str);
            if (ps->result->success) parse_fail(ps, "Expected ')' to close TRIM", peek(ps));
            return NULL;
        }
        if (!rem && !kw) {
            ASTExpr* f = new_expr(EXPR_FUNC);
            if (!f) { ast_expr_free(str); return NULL; }
            snprintf(f->func, sizeof(f->func), "TRIM");
            f->args = calloc(1, sizeof(ASTExpr*));
            if (!f->args) { ast_expr_free(f); ast_expr_free(str); return NULL; }
            f->args[0] = str; f->nargs = 1; f->depth = str->depth + 1;
            return f;
        }
        return add_arg(mkfunc2("TRIM_EX", str, rem ? rem : mkstr(" ")), mkstr(mode));
    }
    if (!strcasecmp(w, "SUBSTRING") || !strcasecmp(w, "SUBSTR") || !strcasecmp(w, "MID")) {   // SUBSTRING(s FROM a [FOR b])
        int save = ps->pos;
        consume(ps); consume(ps);
        ASTExpr* s0 = parse_expr(ps);
        if (!s0) return NULL;
        if (!match(ps, TOK_FROM)) {                            // forma con le virgole: parse_call (MID = SUBSTRING)
            ast_expr_free(s0);
            ps->pos = save;
            if (!strcasecmp(w, "MID")) {
                consume(ps); consume(ps);
                ASTExpr* f = new_expr(EXPR_FUNC);
                if (!f) return NULL;
                snprintf(f->func, sizeof(f->func), "SUBSTRING");
                f->args = calloc(3, sizeof(ASTExpr*));
                if (!f->args) { ast_expr_free(f); return NULL; }
                do {
                    if (f->nargs >= 3) { parse_fail(ps, "MID takes 2 or 3 arguments", peek(ps)); break; }
                    ASTExpr* a = parse_expr(ps);
                    if (!a) break;
                    f->args[f->nargs++] = a;
                    if (a->depth + 1 > f->depth) f->depth = a->depth + 1;
                } while (match(ps, TOK_COMMA));
                if (ps->result->success && !match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')'", peek(ps));
                if (!ps->result->success) { ast_expr_free(f); return NULL; }
                return f;
            }
            return NULL;
        }
        ASTExpr* a = parse_expr(ps);
        ASTExpr* b = NULL;
        if (a && word_is(ps, "FOR")) { consume(ps); b = parse_expr(ps); }
        if (!a || !match(ps, TOK_RPAREN)) {
            ast_expr_free(s0); ast_expr_free(a); ast_expr_free(b);
            if (ps->result->success) parse_fail(ps, "Expected ')'", peek(ps));
            return NULL;
        }
        ASTExpr* f = mkfunc2("SUBSTRING", s0, a);
        return b ? add_arg(f, b) : f;
    }
    if (!strcasecmp(w, "GROUP_CONCAT")) {                       // GROUP_CONCAT([DISTINCT] e, ... [ORDER BY ...] [SEPARATOR 's'])
        consume(ps); consume(ps);
        ASTExpr* e = new_expr(EXPR_AGG);
        if (!e) return NULL;
        snprintf(e->func, sizeof(e->func), "GROUP_CONCAT");
        if (match(ps, TOK_DISTINCT)) e->agg_distinct = true;
        e->args = calloc(MAX_FUNC_ARGS, sizeof(ASTExpr*));
        if (!e->args) { ast_expr_free(e); return NULL; }
        int nv = 0, no = 0;
        char flags[MAX_FUNC_ARGS + 1] = "";
        do {
            if (e->nargs >= MAX_FUNC_ARGS - 2) { parse_fail(ps, "Too many GROUP_CONCAT arguments", peek(ps)); break; }
            ASTExpr* a = parse_expr(ps);
            if (!a) break;
            e->args[e->nargs++] = a;
            nv++;
        } while (ps->result->success && match(ps, TOK_COMMA));
        if (ps->result->success && match(ps, TOK_ORDER)) {
            expect(ps, TOK_BY);
            do {
                if (e->nargs >= MAX_FUNC_ARGS - 1) { parse_fail(ps, "Too many ORDER BY keys", peek(ps)); break; }
                ASTExpr* a = parse_expr(ps);
                if (!a) break;
                e->args[e->nargs++] = a;
                char d = 'A';
                if (match(ps, TOK_DESC)) d = 'D'; else match(ps, TOK_ASC);
                flags[no++] = d;
                flags[no] = '\0';
            } while (ps->result->success && match(ps, TOK_COMMA));
        }
        char sep[128] = ",";
        if (ps->result->success && ident_is(ps, "SEPARATOR")) {
            consume(ps);
            Token st = consume(ps);
            if (st.type != TOK_STRING) parse_fail(ps, "Expected a quoted SEPARATOR", st);
            else copy_text(ps, sep, sizeof(sep), st.text);
        }
        if (ps->result->success && !match(ps, TOK_RPAREN)) parse_fail(ps, "Expected ')' to close GROUP_CONCAT", peek(ps));
        if (!ps->result->success) { ast_expr_free(e); return NULL; }
        ASTExpr* se = mkstr(sep);
        if (!se) { ast_expr_free(e); return NULL; }
        e->args[e->nargs++] = se;
        snprintf(e->col_name, sizeof(e->col_name), "#gc %d %d %s", nv, no, flags);   // valori, chiavi, ASC/DESC
        for (int i = 0; i < e->nargs; i++) if (e->args[i]->depth + 1 > e->depth) e->depth = e->args[i]->depth + 1;
        return e;
    }
    return NULL;
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
        case TOK_DATABASE:                                       // DATABASE() (come MySQL)
            if (next_is(ps, 1, TOK_LPAREN)) return parse_call(ps);
            break;
        case TOK_VALUES:                                         // VALUES(col) in ON DUPLICATE KEY UPDATE
        case TOK_TRUNCATE:                                       // TRUNCATE(x, d)
            if (next_is(ps, 1, TOK_LPAREN)) return parse_call(ps);
            break;
        case TOK_IDENTIFIER: case TOK_LEFT: case TOK_RIGHT:   // LEFT(...)/RIGHT(...) sono funzioni
            if (t.type == TOK_IDENTIFIER && next_is(ps, 1, TOK_LPAREN) && !strcasecmp(t.text, "MATCH")) return parse_match(ps);
            if (t.type == TOK_IDENTIFIER) {
                ASTExpr* sp = parse_special_form(ps);
                if (sp || !ps->result->success) return sp;
            }
            if (next_is(ps, 1, TOK_LPAREN)) return parse_call(ps);
            if (t.type != TOK_IDENTIFIER) break;
            e = new_expr(EXPR_COLUMN);
            if (!e) return NULL;
            consume(ps);
            if (match(ps, TOK_DOT)) {
                Token c = consume(ps);
                if (c.type == TOK_STAR) { parse_fail(ps, "table.* is only allowed in the SELECT list", c); ast_expr_free(e); return NULL; }
                if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name after '.'", c); ast_expr_free(e); return NULL; }
                if (peek(ps).type == TOK_DOT && next_is(ps, 1, TOK_IDENTIFIER)) {   // db.tabella.colonna
                    consume(ps);
                    Token col = consume(ps);
                    if (ps->qual_db[0] && strcasecmp(ps->qual_db, t.text))
                        parse_fail(ps, "Tables of different databases in one statement are not supported yet", t);
                    t = c;
                    c = col;
                }
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

// MATCH(c1, c2, ...) AGAINST('parole' [IN NATURAL LANGUAGE MODE | IN BOOLEAN MODE | WITH QUERY EXPANSION])
// -> funzione MATCH_AGAINST(c1, ..., testo, modo)  (modo 1 = BOOLEAN)
static ASTExpr* parse_match(ParserState* ps) {
    consume(ps);
    consume(ps);                                          // MATCH (
    ASTExpr* f = new_expr(EXPR_FUNC);
    if (!f) return NULL;
    snprintf(f->func, sizeof(f->func), "MATCH_AGAINST");
    f->args = calloc(MAX_FUNC_ARGS, sizeof(ASTExpr*));
    if (!f->args) { ast_expr_free(f); parse_fail(ps, "Out of memory", peek(ps)); return NULL; }
    do {
        if (f->nargs >= 16) { parse_fail(ps, "Too many columns in MATCH (max 16)", peek(ps)); ast_expr_free(f); return NULL; }
        ASTExpr* c = parse_primary(ps);
        if (!c) { ast_expr_free(f); return NULL; }
        f->args[f->nargs++] = c;
    } while (match(ps, TOK_COMMA));
    if (!expect(ps, TOK_RPAREN) || !ident_is(ps, "AGAINST")) {
        if (ps->result->success) parse_fail(ps, "Expected AGAINST after MATCH (...)", peek(ps));
        ast_expr_free(f);
        return NULL;
    }
    consume(ps);
    if (!expect(ps, TOK_LPAREN)) { ast_expr_free(f); return NULL; }
    ASTExpr* q = parse_additive(ps);
    if (!q) { ast_expr_free(f); return NULL; }
    f->args[f->nargs++] = q;
    int mode = 0;
    if (match(ps, TOK_IN)) {
        if (peek(ps).text && !strcasecmp(peek(ps).text, "BOOLEAN")) { consume(ps); mode = 1; }
        else if (ident_is(ps, "NATURAL")) { consume(ps); if (ident_is(ps, "LANGUAGE")) consume(ps); }
        else { parse_fail(ps, "Expected BOOLEAN MODE or NATURAL LANGUAGE MODE", peek(ps)); ast_expr_free(f); return NULL; }
        if (!ident_is(ps, "MODE")) { parse_fail(ps, "Expected MODE", peek(ps)); ast_expr_free(f); return NULL; }
        consume(ps);
    }
    if (ident_is(ps, "WITH")) {                           // WITH QUERY EXPANSION: accettato, come la ricerca normale
        consume(ps);
        if (!ident_is(ps, "QUERY")) { parse_fail(ps, "Expected QUERY EXPANSION", peek(ps)); ast_expr_free(f); return NULL; }
        consume(ps);
        if (ident_is(ps, "EXPANSION")) consume(ps);
    }
    if (!expect(ps, TOK_RPAREN)) { ast_expr_free(f); return NULL; }
    ASTExpr* m = new_expr(EXPR_INT);
    if (!m) { ast_expr_free(f); return NULL; }
    m->int_val = mode;
    f->args[f->nargs++] = m;
    f->depth = 2;
    return f;
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
        else if (ident_is(ps, "DIV") || ident_is(ps, "MOD")) {   // a DIV b (divisione intera), a MOD b
            bool div = ident_is(ps, "DIV");
            consume(ps);
            ASTExpr* f = new_expr(EXPR_FUNC);
            ASTExpr* r = parse_unary(ps);
            if (!f || !r) { ast_expr_free(f); ast_expr_free(r); ast_expr_free(e); return NULL; }
            snprintf(f->func, sizeof(f->func), "%s", div ? "INTDIV" : "MOD");
            f->args = calloc(2, sizeof(ASTExpr*));
            if (!f->args) { ast_expr_free(f); ast_expr_free(r); ast_expr_free(e); return NULL; }
            f->args[0] = e; f->args[1] = r; f->nargs = 2;
            f->depth = (e->depth > r->depth ? e->depth : r->depth) + 1;
            e = depth_check(ps, f);
            continue;
        }
        else break;
        e = depth_check(ps, new_binary(op, e, parse_unary(ps)));
    }
    return e;
}

// a -> '$.x' = JSON_EXTRACT(a, '$.x'); a ->> '$.x' = JSON_UNQUOTE(JSON_EXTRACT(...)) (come MySQL)
static ASTExpr* json_call(const char* name, ASTExpr* a, ASTExpr* b) {
    ASTExpr* f = new_expr(EXPR_FUNC);
    if (!f) { ast_expr_free(a); ast_expr_free(b); return NULL; }
    snprintf(f->func, sizeof(f->func), "%s", name);
    f->args = calloc(MAX_FUNC_ARGS, sizeof(ASTExpr*));
    if (!f->args) { ast_expr_free(f); ast_expr_free(a); ast_expr_free(b); return NULL; }
    f->args[f->nargs++] = a;
    if (b) f->args[f->nargs++] = b;
    f->depth = 1 + (a ? a->depth : 0) + (b ? b->depth : 0);
    return f;
}

static bool adjacent(ParserState* ps, int ahead) {           // token ahead subito dopo il precedente
    int i = ps->pos + ahead;
    if (i <= 0 || i >= ps->tokens->count) return false;
    const Token* a = &ps->tokens->tokens[i - 1], *b = &ps->tokens->tokens[i];
    return a->line == b->line && b->col == a->col;      // col del lexer conta solo gli spazi
}

static ASTExpr* parse_additive(ParserState* ps) {
    ASTExpr* e = parse_multiplicative(ps);
    while (e && ps->result->success) {
        if (peek(ps).type == TOK_MINUS && next_is(ps, 1, TOK_GT) && adjacent(ps, 1)) {
            consume(ps); consume(ps);
            bool unquote = peek(ps).type == TOK_GT && adjacent(ps, 0);
            if (unquote) consume(ps);
            ASTExpr* path = parse_multiplicative(ps);
            if (!path) { ast_expr_free(e); return NULL; }
            e = json_call("JSON_EXTRACT", e, path);
            if (e && unquote) e = json_call("JSON_UNQUOTE", e, NULL);
            continue;
        }
        OpType op;
        if (match(ps, TOK_PLUS))       op = OP_PLUS;
        else if (match(ps, TOK_MINUS)) op = OP_MINUS;
        else break;
        ASTExpr* r = parse_multiplicative(ps);
        // data + INTERVAL n unità  ->  DATE_ADD(data, ...)   (anche INTERVAL n unità + data)
        if (r && is_interval(r)) { e = depth_check(ps, mkfunc2(op == OP_PLUS ? "DATE_ADD" : "DATE_SUB", e, r)); continue; }
        if (e && is_interval(e) && op == OP_PLUS) { e = depth_check(ps, mkfunc2("DATE_ADD", r, e)); continue; }
        e = depth_check(ps, new_binary(op, e, r));
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
    if (match(ps, TOK_LIKE)) {
        ASTExpr* pat = parse_additive(ps);
        if (pat && ident_is(ps, "ESCAPE")) {      // a LIKE p ESCAPE 'c'
            consume(ps);
            ASTExpr* esc = parse_additive(ps);
            if (esc && esc->type == EXPR_STRING && esc->str_val && strlen(esc->str_val) != 1) {
                parse_fail(ps, "ESCAPE needs a single character", peek(ps));
                ast_expr_free(left); ast_expr_free(pat); ast_expr_free(esc);
                return NULL;
            }
            pat = new_binary(OP_ESCAPE, pat, esc);
        }
        return new_binary(neg ? OP_NOT_LIKE : OP_LIKE, left, pat);
    }
    if (ident_is(ps, "REGEXP") || ident_is(ps, "RLIKE")) {        // x [NOT] REGEXP p
        consume(ps);
        ASTExpr* pat = parse_additive(ps);
        ASTExpr* f = new_expr(EXPR_FUNC);
        if (!pat || !f) { ast_expr_free(pat); ast_expr_free(f); ast_expr_free(left); return NULL; }
        snprintf(f->func, sizeof(f->func), "REGEXP_LIKE");
        f->args = calloc(2, sizeof(ASTExpr*));
        if (!f->args) { ast_expr_free(pat); ast_expr_free(f); ast_expr_free(left); return NULL; }
        f->args[0] = left; f->args[1] = pat; f->nargs = 2;
        f->depth = (left->depth > pat->depth ? left->depth : pat->depth) + 1;
        return neg ? new_unary(OP_NOT, f) : f;
    }
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

// la parola corrente è word, anche se il lexer la considera una parola chiave (FOR, ...)
static bool word_is(ParserState* ps, const char* word) {
    Token t = peek(ps);
    return t.text && t.type != TOK_STRING && strcasecmp(t.text, word) == 0;
}

// parole che dopo un nome di tabella non possono essere un alias
static bool alias_forbidden(ParserState* ps) {
    static const char* const W[] = { "INNER", "CROSS", "OUTER", "OFFSET", "INTERSECT", "EXCEPT", "MINUS", "NATURAL", "FULL", "USING", "WINDOW", "WITH" };   // WITH: ... WITH CHECK OPTION delle viste
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
        Token first = table_tok(ps);
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
            Token tt = table_tok(ps);
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

    for (;;) {                                         // DISTINCT / ALL e i modificatori di MySQL
        if (match(ps, TOK_DISTINCT)) { n->select_stmt.distinct = true; continue; }
        if (match(ps, TOK_ALL)) continue;
        if (ident_is(ps, "DISTINCTROW")) { consume(ps); n->select_stmt.distinct = true; continue; }
        if (ident_is(ps, "SQL_CALC_FOUND_ROWS")) { consume(ps); n->select_stmt.calc_found = true; continue; }
        if (ident_is(ps, "SQL_NO_CACHE") || ident_is(ps, "SQL_CACHE") || ident_is(ps, "HIGH_PRIORITY") || ident_is(ps, "STRAIGHT_JOIN") ||
            ident_is(ps, "SQL_SMALL_RESULT") || ident_is(ps, "SQL_BIG_RESULT") || ident_is(ps, "SQL_BUFFER_RESULT")) { consume(ps); continue; }
        break;
    }

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
    // FOR UPDATE / FOR SHARE [NOWAIT | SKIP LOCKED] / LOCK IN SHARE MODE: i lock li prende già la transazione
    if (ps->result->success && word_is(ps, "FOR") && (peek2_ident(ps, "UPDATE") || peek2_ident(ps, "SHARE") || next_is(ps, 1, TOK_UPDATE))) {
        consume(ps); consume(ps);
        if (ident_is(ps, "NOWAIT")) consume(ps);
        else if (ident_is(ps, "SKIP")) { consume(ps); if (ident_is(ps, "LOCKED")) consume(ps); }
    } else if (ps->result->success && ident_is(ps, "LOCK") && next_is(ps, 1, TOK_IN)) {
        consume(ps); consume(ps);
        if (ident_is(ps, "SHARE")) consume(ps);
        if (ident_is(ps, "MODE")) consume(ps);
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
static bool peek2_ident(ParserState* ps, const char* w);

// [ON DUPLICATE KEY UPDATE col = espr {, col = espr}]
static void parse_on_duplicate(ParserState* ps, ASTNode* n) {
    if (!ps->result->success || peek(ps).type != TOK_ON || !peek2_ident(ps, "DUPLICATE")) return;
    consume(ps);
    consume(ps);
    if (!match(ps, TOK_KEY) || !match(ps, TOK_UPDATE)) { parse_fail(ps, "Expected ON DUPLICATE KEY UPDATE", peek(ps)); return; }
    do {
        if (n->insert_stmt.num_dup >= SV_MAX_SETS) { parse_fail(ps, "Too many columns in ON DUPLICATE KEY UPDATE", peek(ps)); return; }
        Token c = consume(ps);
        if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", c); return; }
        if (match(ps, TOK_DOT)) { c = consume(ps); if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", c); return; } }
        if (!match(ps, TOK_EQ)) { parse_fail(ps, "Expected '='", peek(ps)); return; }
        ASTExpr* v = parse_expr(ps);
        if (!v) return;
        int k = n->insert_stmt.num_dup++;
        copy_text(ps, n->insert_stmt.dup_cols[k], sizeof(n->insert_stmt.dup_cols[k]), c.text);
        n->insert_stmt.dup_exprs[k] = v;
    } while (match(ps, TOK_COMMA));
}

static void parse_insert(ParserState* ps, ASTNode* n) {
    n->type = NODE_INSERT;
    // MySQL: INSERT [LOW_PRIORITY | DELAYED | HIGH_PRIORITY] [IGNORE] [INTO] t
    while (ident_is(ps, "LOW_PRIORITY") || ident_is(ps, "DELAYED") || ident_is(ps, "HIGH_PRIORITY")) consume(ps);
    if (ident_is(ps, "IGNORE")) { consume(ps); n->insert_stmt.mode = INS_IGNORE; }
    match(ps, TOK_INTO);
    Token tn = table_tok(ps);
    if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after INSERT INTO", tn); return; }
    copy_text(ps, n->insert_stmt.table_name, sizeof(n->insert_stmt.table_name), tn.text);
    if (match(ps, TOK_LPAREN) && !match(ps, TOK_RPAREN)) {      // MySQL: "()" = nessuna colonna (tutti i default)
        do {
            Token c = consume(ps);
            if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name", c); return; }
            if (n->insert_stmt.num_cols >= 64) { parse_fail(ps, "Too many columns in INSERT (max 64)", c); return; }
            n->insert_stmt.columns[n->insert_stmt.num_cols++] = strdup(c.text);
        } while (match(ps, TOK_COMMA));
        if (!match(ps, TOK_RPAREN)) { parse_fail(ps, "Expected ')' after the column list", peek(ps)); return; }
    }
    if (ident_is(ps, "DEFAULT") && n->insert_stmt.num_cols == 0) {   // PostgreSQL / SQLite: DEFAULT VALUES
        consume(ps);
        if (!match(ps, TOK_VALUES)) { parse_fail(ps, "Expected VALUES after DEFAULT", peek(ps)); return; }
        n->insert_stmt.num_rows = 1;
        n->insert_stmt.num_values = 0;
        return;
    }
    if (query_starts(ps, 0)) {                                    // INSERT INTO t SELECT ...
        n->insert_stmt.select = parse_query(ps);
        parse_on_duplicate(ps, n);
        return;
    }
    if (match(ps, TOK_SET) && n->insert_stmt.num_cols == 0) {    // MySQL: INSERT INTO t SET a = 1, b = 2
        int cap = 0;
        do {
            Token c = consume(ps);
            if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in SET", c); return; }
            if (n->insert_stmt.num_cols >= 64) { parse_fail(ps, "Too many columns in INSERT (max 64)", c); return; }
            if (!match(ps, TOK_EQ)) { parse_fail(ps, "Expected '=' in SET", peek(ps)); return; }
            ASTExpr* v = parse_expr(ps);
            if (!v) return;
            if (n->insert_stmt.num_cols >= cap) {
                cap = cap ? cap * 2 : 16;
                ASTExpr** nv = realloc(n->insert_stmt.values, sizeof(ASTExpr*) * (size_t)cap);
                if (!nv) { ast_expr_free(v); parse_fail(ps, "Out of memory", peek(ps)); return; }
                n->insert_stmt.values = nv;
            }
            n->insert_stmt.columns[n->insert_stmt.num_cols] = strdup(c.text);
            n->insert_stmt.values[n->insert_stmt.num_cols++] = v;
            n->insert_stmt.num_values = n->insert_stmt.num_cols;
            n->insert_stmt.num_rows = 1;
        } while (match(ps, TOK_COMMA));
        parse_on_duplicate(ps, n);
        return;
    }
    if (!match(ps, TOK_VALUES) && !(ident_is(ps, "VALUE") && (consume(ps), true))) { parse_fail(ps, "Expected VALUES or SELECT", peek(ps)); return; }
    // i valori sono in un unico array, riga dopo riga; nell'AST restano solo righe complete
    int cap = 0, total = 0;
    do {
        if (!match(ps, TOK_LPAREN)) { parse_fail(ps, "Expected '(' before the values", peek(ps)); return; }
        int count = 0;
        bool ok = true;
        if (peek(ps).type != TOK_RPAREN) do {                     // "()" = una riga di soli default
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
    if (match(ps, TOK_AS)) {                                      // MySQL 8: VALUES (...) AS nuovo [(colonne)]
        Token a = consume(ps);
        if (a.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected an alias after AS", a); return; }
        copy_text(ps, n->insert_stmt.row_alias, sizeof(n->insert_stmt.row_alias), a.text);
        if (peek(ps).type == TOK_LPAREN) { while (peek(ps).type != TOK_RPAREN && peek(ps).type != TOK_EOF) consume(ps); expect(ps, TOK_RPAREN); }
    }
    parse_on_duplicate(ps, n);
}

// UPDATE t SET col = espr {, col = espr} [WHERE condizione]

// ── UPDATE / DELETE di MySQL con più tabelle o ORDER BY / LIMIT: parti come testo ──

// fine della parte corrente: la prima parola fra stops (a profondità 0 di parentesi), ';' o la fine
static int scan_until(ParserState* ps, int from, const char* const* stops, int nstops) {
    int depth = 0, i = from;
    for (; i < ps->tokens->count; i++) {
        const Token* t = &ps->tokens->tokens[i];
        if (t->type == TOK_EOF || (t->type == TOK_SEMICOLON && depth == 0)) break;
        if (t->type == TOK_LPAREN) depth++;
        else if (t->type == TOK_RPAREN) { if (depth == 0) break; depth--; }
        else if (depth == 0 && t->text && t->type != TOK_STRING)
            for (int k = 0; k < nstops; k++) if (!strcasecmp(t->text, stops[k])) return i;
    }
    return i;
}

static char* text_of(ParserState* ps, int from, int to) {
    if (to <= from) return NULL;
    char* buf = malloc(SV_MAX_SQL_LEN);
    if (!buf) { parse_fail(ps, "Out of memory", peek(ps)); return NULL; }
    if (!tokens_text(ps, from, to, buf, SV_MAX_SQL_LEN)) { free(buf); parse_fail(ps, "Statement is too long", peek(ps)); return NULL; }
    char* r = strdup(buf);
    free(buf);
    return r;
}

// [WHERE ...] [ORDER BY ... ] [LIMIT ...] in coda: testo
static void tail_texts(ParserState* ps, char** where, char** order_limit) {
    static const char* const ST[] = { "ORDER", "LIMIT" };
    if (match(ps, TOK_WHERE)) {
        int start = ps->pos, end = scan_until(ps, start, ST, 2);
        *where = text_of(ps, start, end);
        ps->pos = end;
    }
    if (peek(ps).type == TOK_ORDER || peek(ps).type == TOK_LIMIT) {
        int start = ps->pos, end = scan_until(ps, start, NULL, 0);
        *order_limit = text_of(ps, start, end);
        ps->pos = end;
    }
}

// DELETE dopo la parola DELETE: la forma semplice va nei campi di sempre, le altre come testo
static void parse_delete(ParserState* ps, ASTNode* n) {
    n->type = NODE_DELETE;
    while (ident_is(ps, "LOW_PRIORITY") || ident_is(ps, "QUICK") || ident_is(ps, "IGNORE")) consume(ps);
    static const char* const FROM_END[] = { "WHERE", "ORDER", "LIMIT" };
    if (!match(ps, TOK_FROM)) {                                  // DELETE a, b FROM ... WHERE ...
        do {
            Token t = consume(ps);
            if (t.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after DELETE", t); return; }
            if (n->delete_stmt.num_targets >= 8) { parse_fail(ps, "Too many tables in DELETE (max 8)", t); return; }
            copy_text(ps, n->delete_stmt.targets[n->delete_stmt.num_targets++], 64, t.text);
            if (match(ps, TOK_DOT)) consume(ps);                 // a.*
        } while (match(ps, TOK_COMMA));
        if (!match(ps, TOK_FROM)) { parse_fail(ps, "Expected FROM", peek(ps)); return; }
        int start = ps->pos, end = scan_until(ps, start, FROM_END, 3);
        n->delete_stmt.multi_from = text_of(ps, start, end);
        ps->pos = end;
        tail_texts(ps, &n->delete_stmt.multi_where, &n->delete_stmt.order_limit);
        snprintf(n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), "%s", n->delete_stmt.targets[0]);
        return;
    }
    int tstart = ps->pos;
    Token tn = table_tok(ps);
    if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after DELETE FROM", tn); return; }
    copy_text(ps, n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), tn.text);
    if (peek(ps).type == TOK_COMMA || ident_is(ps, "USING")) {   // DELETE FROM a, b USING t1 a, t2 b WHERE ...
        ps->pos = tstart;
        do {
            Token t = consume(ps);
            if (t.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", t); return; }
            if (n->delete_stmt.num_targets >= 8) { parse_fail(ps, "Too many tables in DELETE (max 8)", t); return; }
            copy_text(ps, n->delete_stmt.targets[n->delete_stmt.num_targets++], 64, t.text);
            if (match(ps, TOK_DOT)) consume(ps);
        } while (match(ps, TOK_COMMA));
        if (!ident_is(ps, "USING")) { parse_fail(ps, "Expected USING", peek(ps)); return; }
        consume(ps);
        int start = ps->pos, end = scan_until(ps, start, FROM_END, 3);
        n->delete_stmt.multi_from = text_of(ps, start, end);
        ps->pos = end;
        tail_texts(ps, &n->delete_stmt.multi_where, &n->delete_stmt.order_limit);
        return;
    }
    bool alias = match(ps, TOK_AS) || (peek(ps).type == TOK_IDENTIFIER && !ident_is(ps, "USING"));
    if (alias || (ps->result->success && peek(ps).type != TOK_WHERE && peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF)) {
        // DELETE FROM t x WHERE ... / ORDER BY / LIMIT: come testo
        ps->pos = tstart;
        int end = scan_until(ps, tstart, FROM_END, 3);
        n->delete_stmt.multi_from = text_of(ps, tstart, end);
        ps->pos = end;
        snprintf(n->delete_stmt.targets[0], 64, "%s", n->delete_stmt.table_name);
        // l'alias (se c'è) è la tabella da cui cancellare
        if (end - tstart >= 2) {
            const Token* last = &ps->tokens->tokens[end - 1];
            if (last->type == TOK_IDENTIFIER && end - 1 > tstart) copy_text(ps, n->delete_stmt.targets[0], 64, last->text);
        }
        n->delete_stmt.num_targets = 1;
        tail_texts(ps, &n->delete_stmt.multi_where, &n->delete_stmt.order_limit);
        return;
    }
    if (ps->result->success && match(ps, TOK_WHERE)) {
        int wstart = ps->pos;
        n->delete_stmt.where = parse_expr(ps);
        if (ps->result->success && (peek(ps).type == TOK_ORDER || peek(ps).type == TOK_LIMIT)) {   // ... WHERE x ORDER BY / LIMIT
            n->delete_stmt.multi_where = text_of(ps, wstart, ps->pos);
            n->delete_stmt.multi_from = strdup(n->delete_stmt.table_name);
            snprintf(n->delete_stmt.targets[0], 64, "%s", n->delete_stmt.table_name);
            n->delete_stmt.num_targets = 1;
            int start = ps->pos, end = scan_until(ps, start, NULL, 0);
            n->delete_stmt.order_limit = text_of(ps, start, end);
            ps->pos = end;
        }
    }
}

static void parse_update(ParserState* ps, ASTNode* n) {
    n->type = NODE_UPDATE;
    while (ident_is(ps, "LOW_PRIORITY") || ident_is(ps, "IGNORE")) consume(ps);
    int tstart = ps->pos;
    Token tn = table_tok(ps);
    if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name after UPDATE", tn); return; }
    copy_text(ps, n->update_stmt.table_name, sizeof(n->update_stmt.table_name), tn.text);
    if (peek(ps).type != TOK_SET) {                              // MySQL: UPDATE t1 a JOIN t2 b ON ... SET ...
        static const char* const ST[] = { "SET" };
        int end = scan_until(ps, tstart, ST, 1);
        n->update_stmt.multi_from = text_of(ps, tstart, end);
        ps->pos = end;
    }
    if (!match(ps, TOK_SET)) { parse_fail(ps, "Expected SET", peek(ps)); return; }
    do {
        if (n->update_stmt.num_sets >= SV_MAX_SETS) { parse_fail(ps, "Too many columns in SET", peek(ps)); return; }
        Token cn = consume(ps);
        if (cn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in SET", cn); return; }
        Token col = cn;
        char qual[64] = "";
        if (match(ps, TOK_DOT)) {                     // tabella.colonna
            copy_text(ps, qual, sizeof(qual), cn.text);
            col = consume(ps);
            if (col.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in SET", col); return; }
        }
        if (!match(ps, TOK_EQ)) { parse_fail(ps, "Expected '=' in SET", peek(ps)); return; }
        int vstart = ps->pos;
        ASTExpr* v = parse_expr(ps);
        if (!v) return;
        int k = n->update_stmt.num_sets++;
        copy_text(ps, n->update_stmt.set_cols[k], sizeof(n->update_stmt.set_cols[k]), col.text);
        memcpy(n->update_stmt.set_tables[k], qual, sizeof(qual));
        n->update_stmt.set_exprs[k] = v;
        if (n->update_stmt.multi_from) n->update_stmt.set_texts[k] = text_of(ps, vstart, ps->pos);
    } while (match(ps, TOK_COMMA));
    if (n->update_stmt.multi_from) { tail_texts(ps, &n->update_stmt.multi_where, &n->update_stmt.order_limit); return; }
    int wstart = -1;
    if (match(ps, TOK_WHERE)) { wstart = ps->pos; n->update_stmt.where = parse_expr(ps); }
    if (ps->result->success && (peek(ps).type == TOK_ORDER || peek(ps).type == TOK_LIMIT)) {   // UPDATE t SET ... ORDER BY / LIMIT
        n->update_stmt.multi_from = strdup(n->update_stmt.table_name);
        if (wstart >= 0) n->update_stmt.multi_where = text_of(ps, wstart, ps->pos);
        int start = ps->pos, end = scan_until(ps, start, NULL, 0);
        n->update_stmt.order_limit = text_of(ps, start, end);
        ps->pos = end;
        // i testi delle espressioni: dal formato dell'AST
        for (int k = 0; k < n->update_stmt.num_sets; k++) {
            char buf[4096];
            ast_expr_format(n->update_stmt.set_exprs[k], buf, sizeof(buf));
            n->update_stmt.set_texts[k] = strdup(buf);
        }
    }
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
    if (k.type != TOK_INSERT && k.type != TOK_UPDATE && k.type != TOK_DELETE && !query_starts(ps, 0) && !ident_is(ps, "CALL")) {
        parse_fail(ps, "A trigger or procedure can contain INSERT, UPDATE, DELETE, SELECT, CALL, SET NEW.column, SIGNAL and IF", k);
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
    ParserState ps = { tokens, 0, &r, 0, "", false };
    parse_trigger_rest(&ps, p);
    match(&ps, TOK_SEMICOLON);
    if (r.success && peek(&ps).type != TOK_EOF) parse_fail(&ps, "Unexpected text after the trigger body", peek(&ps));
    p->success = r.success;
    snprintf(p->error, sizeof(p->error), "%s", r.error);
    return p;
}

// caratteristiche MySQL di una procedura fra i parametri e il corpo (ignorate):
// COMMENT '...', [NOT] DETERMINISTIC, LANGUAGE SQL, CONTAINS SQL, NO SQL, READS / MODIFIES SQL DATA, SQL SECURITY x
static void skip_routine_traits(ParserState* ps) {
    for (int guard = 0; guard < 16; guard++) {
        if (ident_is(ps, "COMMENT") && next_is(ps, 1, TOK_STRING)) { consume(ps); consume(ps); }
        else if (ident_is(ps, "DETERMINISTIC")) consume(ps);
        else if (peek(ps).type == TOK_NOT && next_is(ps, 1, TOK_IDENTIFIER)) { consume(ps); consume(ps); }
        else if (ident_is(ps, "LANGUAGE") || ident_is(ps, "CONTAINS") || ident_is(ps, "NO")) { consume(ps); consume(ps); }
        else if (ident_is(ps, "READS") || ident_is(ps, "MODIFIES")) { consume(ps); consume(ps); consume(ps); }
        else if (ident_is(ps, "SQL") && next_is(ps, 1, TOK_IDENTIFIER)) { consume(ps); consume(ps); consume(ps); }
        else return;
    }
}

TrigProgram* parser_parse_procedure(TokenList* tokens, int start) {
    TrigProgram* p = calloc(1, sizeof(TrigProgram));
    if (!p) return NULL;
    ParseResult r;
    memset(&r, 0, sizeof(r));
    r.success = true;
    ParserState ps = { tokens, start, &r, 0, "", false };
    p->for_each_row = false;
    skip_routine_traits(&ps);
    if (match(&ps, TOK_BEGIN)) {
        parse_trig_list(&ps, &p->v, &p->n);
        if (r.success && !match(&ps, TOK_END)) parse_fail(&ps, "Expected END to close the procedure body", peek(&ps));
    } else {
        TrigStmt* st = parse_trig_stmt(&ps);
        if (st) trig_push(&ps, &p->v, &p->n, st);
    }
    match(&ps, TOK_SEMICOLON);
    if (r.success && peek(&ps).type != TOK_EOF) parse_fail(&ps, "Unexpected text after the procedure body", peek(&ps));
    p->success = r.success;
    snprintf(p->error, sizeof(p->error), "%s", r.error);
    return p;
}

// CREATE PROCEDURE nome ([IN] p tipo, ...) corpo: salvato come testo "(parametri) corpo"
static ASTNode* parse_create_procedure(ParserState* ps, ASTNode* n) {
    n->type = NODE_CREATE_PROCEDURE;
    Token nm = consume(ps);
    if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the procedure name", nm); return n; }
    copy_text(ps, n->create_trigger.name, sizeof(n->create_trigger.name), nm.text);
    int start = ps->pos;
    if (!expect(ps, TOK_LPAREN)) return n;
    for (int d = 1; d > 0; ) {                              // parametri: controllati all'esecuzione di CREATE
        Token t = consume(ps);
        if (t.type == TOK_EOF) { parse_fail(ps, "Expected ')' after the parameters", t); return n; }
        if (t.type == TOK_LPAREN) d++;
        else if (t.type == TOK_RPAREN) d--;
    }
    TrigProgram* prog = calloc(1, sizeof(TrigProgram));
    if (!prog) { parse_fail(ps, "Out of memory", peek(ps)); return n; }
    skip_routine_traits(ps);
    if (match(ps, TOK_BEGIN)) {
        parse_trig_list(ps, &prog->v, &prog->n);
        if (ps->result->success && !match(ps, TOK_END)) parse_fail(ps, "Expected END to close the procedure body", peek(ps));
    } else {
        TrigStmt* st = parse_trig_stmt(ps);
        if (st) trig_push(ps, &prog->v, &prog->n, st);
    }
    trig_program_free(prog);
    if (!ps->result->success) return n;
    if (!tokens_text(ps, start, ps->pos, n->create_trigger.body, sizeof(n->create_trigger.body)))
        parse_fail(ps, "Procedure is too long", peek(ps));
    return n;
}

// CALL nome(argomenti)
static void parse_call_stmt(ParserState* ps, ASTNode* n) {
    consume(ps);
    n->type = NODE_CALL;
    Token nm = consume(ps);
    if (nm.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the procedure name after CALL", nm); return; }
    copy_text(ps, n->call.name, sizeof(n->call.name), nm.text);
    if (!match(ps, TOK_LPAREN)) return;                   // CALL p senza parentesi
    if (match(ps, TOK_RPAREN)) return;
    do {
        if (n->call.nargs >= 32) { parse_fail(ps, "Too many arguments (max 32)", peek(ps)); return; }
        ASTExpr* e = parse_expr(ps);
        if (!e) return;
        n->call.args[n->call.nargs++] = e;
    } while (match(ps, TOK_COMMA));
    expect(ps, TOK_RPAREN);
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
    Token tbl = table_tok(ps);
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
        // MySQL: prefisso dell'indice col(191) e direzione
        if (peek(ps).type == TOK_LPAREN && next_is(ps, 1, TOK_INTEGER) && next_is(ps, 2, TOK_RPAREN)) { consume(ps); consume(ps); consume(ps); }
        if (peek(ps).type == TOK_ASC || peek(ps).type == TOK_DESC) consume(ps);
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

// USING BTREE | HASH e altre opzioni d'indice di MySQL (KEY_BLOCK_SIZE = n, COMMENT 'x', VISIBLE)
static void skip_index_using(ParserState* ps) {
    for (;;) {
        if (ident_is(ps, "USING")) { consume(ps); consume(ps); continue; }
        if (ident_is(ps, "KEY_BLOCK_SIZE")) { consume(ps); match(ps, TOK_EQ); consume(ps); continue; }
        if (ident_is(ps, "COMMENT") && next_is(ps, 1, TOK_STRING)) { consume(ps); consume(ps); continue; }
        if (ident_is(ps, "VISIBLE") || ident_is(ps, "INVISIBLE")) { consume(ps); continue; }
        return;
    }
}

// vincolo di tabella (dopo l'eventuale CONSTRAINT nome); false se non è un vincolo
static bool parse_table_constraint(ParserState* ps, ConstraintDef* c) {
    if (ident_is(ps, "PRIMARY")) {
        consume(ps);
        expect(ps, TOK_KEY);
        c->kind = CONS_PRIMARY;
        skip_index_using(ps);
        c->ncols = parse_name_list(ps, c->cols, 8);
        skip_index_using(ps);
    } else if (ident_is(ps, "UNIQUE")) {
        consume(ps);
        if (peek(ps).type == TOK_KEY || peek(ps).type == TOK_INDEX) consume(ps);
        if (peek(ps).type == TOK_IDENTIFIER && !ident_is(ps, "USING")) {           // UNIQUE KEY nome (colonne)
            Token nm = consume(ps);
            if (!c->name[0]) copy_text(ps, c->name, sizeof(c->name), nm.text);
        }
        c->kind = CONS_UNIQUE;
        skip_index_using(ps);
        c->ncols = parse_name_list(ps, c->cols, 8);
        skip_index_using(ps);
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
// tipo di una colonna: nome [(dimensione [, decimali])], anche nei nomi di MySQL (bigint(20) unsigned,
// longtext, enum('a','b'), double precision, ...): cd->type diventa il tipo di SegVault, cd->sqltype
// conserva il tipo come scritto (SHOW COLUMNS, information_schema)
static bool parse_type_spec(ParserState* ps, ColumnDef* cd) {
    Token ct = consume(ps);
    if (ct.type != TOK_IDENTIFIER && ct.type != TOK_BOOLEAN) { parse_fail(ps, "Expected a column type", ct); return false; }
    char base[40];
    copy_text(ps, base, sizeof(base), ct.text);
    for (char* q = base; *q; q++) *q = (char)tolower((unsigned char)*q);
    // nomi in due parole
    if (!strcmp(base, "double") && ident_is(ps, "PRECISION")) consume(ps);
    else if ((!strcmp(base, "character") || !strcmp(base, "char")) && ident_is(ps, "VARYING")) { consume(ps); snprintf(base, sizeof(base), "varchar"); }
    else if (!strcmp(base, "national") && (ident_is(ps, "CHAR") || ident_is(ps, "VARCHAR") || ident_is(ps, "CHARACTER"))) {
        Token t2 = consume(ps);
        copy_text(ps, base, sizeof(base), t2.text);
        for (char* q = base; *q; q++) *q = (char)tolower((unsigned char)*q);
        if (ident_is(ps, "VARYING")) { consume(ps); snprintf(base, sizeof(base), "varchar"); }
    } else if (!strcmp(base, "long") && (ident_is(ps, "VARCHAR") || ident_is(ps, "VARBINARY"))) {
        Token t2 = consume(ps);
        snprintf(base, sizeof(base), "%s", !strcasecmp(t2.text, "VARBINARY") ? "mediumblob" : "mediumtext");
    }
    char sql[48];
    snprintf(sql, sizeof(sql), "%s", base);
    bool is_enum = !strcmp(base, "enum"), is_set = !strcmp(base, "set");
    int maxlen = 0;
    if (match(ps, TOK_LPAREN)) {
        if (is_enum || is_set) {                       // enum('a','b'): diventa un CHECK (enum)
            char list[480];
            size_t ll = 0;
            list[0] = '\0';
            do {
                Token v = consume(ps);
                if (v.type != TOK_STRING) { parse_fail(ps, "Expected a quoted value in ENUM / SET", v); return false; }
                size_t vl = v.text ? strlen(v.text) : 0;
                if ((int)vl > maxlen) maxlen = (int)vl;
                if (ll + vl * 2 + 6 < sizeof(list)) {
                    { char sep = ll ? ',' : '('; list[ll++] = sep; }
                    list[ll++] = '\'';
                    for (const char* q = v.text; q && *q; q++) { if (*q == '\'') list[ll++] = '\''; list[ll++] = *q; }
                    list[ll++] = '\'';
                    list[ll] = '\0';
                }
            } while (match(ps, TOK_COMMA));
            if (!expect(ps, TOK_RPAREN)) return false;
            list[ll++] = ')';
            list[ll] = '\0';
            snprintf(sql, sizeof(sql), "%s%.*s", base, (int)(sizeof(sql) - strlen(base) - 1), list);
            if (is_enum) snprintf(cd->comment, sizeof(cd->comment), "\x01%s", list);   // segnaposto: diventa un CHECK
        } else {
            Token sz = consume(ps);
            if (sz.type != TOK_INTEGER) { parse_fail(ps, "Expected a size", sz); return false; }
            cd->size = atoi(sz.text);
            if (match(ps, TOK_COMMA)) {                    // DECIMAL(p, s)
                Token sc = consume(ps);
                if (sc.type != TOK_INTEGER) { parse_fail(ps, "Expected the number of decimal digits", sc); return false; }
                cd->scale = atoi(sc.text);
                cd->has_scale = true;
                snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "(%d,%d)", cd->size, cd->scale);
            } else snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "(%d)", cd->size);
            if (!expect(ps, TOK_RPAREN)) return false;
        }
    }
    // attributi del tipo
    for (;;) {
        if (ident_is(ps, "UNSIGNED")) { consume(ps); cd->flags |= COLF_UNSIGNED; continue; }
        if (ident_is(ps, "SIGNED") || ident_is(ps, "ZEROFILL")) { consume(ps); continue; }
        if (ident_is(ps, "BINARY") || ident_is(ps, "ASCII") || ident_is(ps, "UNICODE")) { consume(ps); continue; }
        break;
    }
    if (cd->flags & COLF_UNSIGNED) snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " unsigned");
    snprintf(cd->sqltype, sizeof(cd->sqltype), "%s", sql);

    // tipo di SegVault
    static const struct { const char* my; const char* sv; } MAP[] = {
        { "integer", "INT" }, { "int", "INT" }, { "mediumint", "INT" }, { "smallint", "INT" }, { "tinyint", "INT" }, { "int1", "INT" },
        { "int2", "INT" }, { "int3", "INT" }, { "int4", "INT" }, { "middleint", "INT" }, { "year", "INT" },
        { "bigint", "BIGINT" }, { "int8", "BIGINT" }, { "serial", "BIGINT" }, { "bit", "BIGINT" },
        { "float", "FLOAT" }, { "double", "FLOAT" }, { "real", "FLOAT" }, { "float4", "FLOAT" }, { "float8", "FLOAT" },
        { "decimal", "DECIMAL" }, { "numeric", "DECIMAL" }, { "dec", "DECIMAL" }, { "fixed", "DECIMAL" },
        { "char", "CHAR" }, { "character", "CHAR" }, { "nchar", "CHAR" },
        { "varchar", "VARCHAR" }, { "nvarchar", "VARCHAR" }, { "varchar2", "VARCHAR" }, { "time", "VARCHAR" },
        { "text", "TEXT" }, { "tinytext", "TEXT" }, { "mediumtext", "TEXT" }, { "longtext", "TEXT" }, { "json", "TEXT" },
        { "enum", "VARCHAR" }, { "set", "TEXT" }, { "clob", "TEXT" }, { "string", "TEXT" },
        { "blob", "BLOB" }, { "tinyblob", "BLOB" }, { "mediumblob", "BLOB" }, { "longblob", "BLOB" }, { "binary", "BLOB" },
        { "varbinary", "BLOB" }, { "bytea", "BLOB" }, { "geometry", "BLOB" }, { "point", "BLOB" }, { "linestring", "BLOB" },
        { "polygon", "BLOB" },
        { "bool", "BOOL" }, { "boolean", "BOOL" },
        { "date", "DATE" }, { "datetime", "DATETIME" }, { "timestamp", "DATETIME" },
    };
    const char* sv = NULL;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]) && !sv; i++) if (!strcmp(base, MAP[i].my)) sv = MAP[i].sv;
    if (!sv) { copy_text(ps, cd->type, sizeof(cd->type), ct.text); cd->sqltype[0] = '\0'; return true; }   // l'executor dirà "Unknown type"
    snprintf(cd->type, sizeof(cd->type), "%s", sv);
    // un INT UNSIGNED arriva a 4294967295: serve un BIGINT
    if (!strcmp(sv, "INT") && (cd->flags & COLF_UNSIGNED) && (!strcmp(base, "int") || !strcmp(base, "integer") || !strcmp(base, "mediumint")))
        snprintf(cd->type, sizeof(cd->type), "BIGINT");
    if (!strcmp(base, "time")) cd->size = 20;
    if (is_enum) cd->size = maxlen > 0 ? maxlen : 1;
    if (!strcmp(sv, "INT") || !strcmp(sv, "BIGINT") || !strcmp(sv, "FLOAT") || !strcmp(sv, "TEXT") || !strcmp(sv, "BLOB") ||
        !strcmp(sv, "DATE") || !strcmp(sv, "DATETIME") || !strcmp(sv, "BOOL")) cd->size = 0;   // (n) di MySQL = larghezza di visualizzazione
    if (!strcmp(base, "serial")) { cd->auto_increment = true; cd->nullable = false; cd->flags |= COLF_UNSIGNED; }
    // il tipo scritto serve solo se dice più del tipo di SegVault
    if (!strcasecmp(cd->sqltype, cd->type)) cd->sqltype[0] = '\0';
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
    if (dv.type == TOK_IDENTIFIER && peek(ps).type == TOK_LPAREN) {        // CURRENT_TIMESTAMP(6), NOW()
        copy_text(ps, out, n, dv.text);
        consume(ps);
        while (peek(ps).type != TOK_RPAREN && peek(ps).type != TOK_EOF) consume(ps);
        expect(ps, TOK_RPAREN);
        return true;
    }
    if (dv.type == TOK_LPAREN) {                                           // DEFAULT (espressione): il testo
        int start = ps->pos, depth = 1;
        while (depth > 0 && peek(ps).type != TOK_EOF) {
            Token t = consume(ps);
            if (t.type == TOK_LPAREN) depth++;
            else if (t.type == TOK_RPAREN) depth--;
        }
        if (!tokens_text(ps, start, ps->pos - 1, out, n)) { parse_fail(ps, "DEFAULT expression is too long", dv); return false; }
        size_t l = strlen(out);                                            // valore fra apici: senza apici
        if (l >= 2 && out[0] == '\'' && out[l - 1] == '\'') { memmove(out, out + 1, l - 2); out[l - 2] = '\0'; }
        return true;
    }
    if (dv.type == TOK_INTEGER || dv.type == TOK_FLOAT || dv.type == TOK_STRING || dv.type == TOK_NULL || dv.type == TOK_IDENTIFIER ||
        dv.type == TOK_BOOLEAN) {
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
            if (peek(ps).type == TOK_KEY) consume(ps);               // UNIQUE KEY
            ConstraintDef* c = new_cons(ps, n);
            if (!c) return;
            memcpy(c->name, ccn, sizeof(c->name));
            c->kind = CONS_UNIQUE; c->ncols = 1;
            memcpy(c->cols[0], cd->name, 64);
        } else if (ident_is(ps, "AUTO_INCREMENT") || ident_is(ps, "AUTOINCREMENT")) {
            consume(ps);
            cd->auto_increment = true;
        } else if (ident_is(ps, "COMMENT")) {                         // MySQL: COMMENT 'testo'
            consume(ps);
            Token cm = consume(ps);
            if (cm.type != TOK_STRING) { parse_fail(ps, "Expected a quoted comment", cm); return; }
            if (cd->comment[0] != '\x01') copy_text(ps, cd->comment, sizeof(cd->comment), cm.text);
        } else if (ident_is(ps, "COLLATE") || ident_is(ps, "CHARSET") || ident_is(ps, "CHARACTER")) {
            bool cs = ident_is(ps, "CHARACTER");
            consume(ps);
            if (cs && ident_is(ps, "SET")) consume(ps);
            else if (cs && peek(ps).type == TOK_SET) consume(ps);
            match(ps, TOK_EQ);
            consume(ps);
        } else if (ident_is(ps, "UNSIGNED") || ident_is(ps, "SIGNED") || ident_is(ps, "ZEROFILL") ||
                   ident_is(ps, "VISIBLE") || ident_is(ps, "INVISIBLE")) {
            if (ident_is(ps, "UNSIGNED")) cd->flags |= COLF_UNSIGNED;
            consume(ps);
        } else if (ident_is(ps, "COLUMN_FORMAT") || ident_is(ps, "STORAGE")) {
            consume(ps);
            consume(ps);
        } else if (match(ps, TOK_ON)) {                               // ON UPDATE CURRENT_TIMESTAMP[(n)]
            if (!match(ps, TOK_UPDATE)) { parse_fail(ps, "Expected UPDATE after ON", peek(ps)); return; }
            Token f = consume(ps);
            if (f.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected CURRENT_TIMESTAMP after ON UPDATE", f); return; }
            if (match(ps, TOK_LPAREN)) { while (peek(ps).type != TOK_RPAREN && peek(ps).type != TOK_EOF) consume(ps); expect(ps, TOK_RPAREN); }
            cd->flags |= COLF_ON_UPDATE_NOW;
        } else if (peek(ps).type == TOK_KEY) {                       // MySQL: "col INT KEY" = PRIMARY KEY
            consume(ps);
            cd->primary_key = true;
            cd->nullable = false;
        } else if (ident_is(ps, "DEFAULT")) {
            consume(ps);
            if (!parse_default_value(ps, cd->default_val, sizeof(cd->default_val))) return;
            cd->has_default = true;
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
    Token tn = table_tok(ps);
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
        // MySQL: KEY | INDEX | FULLTEXT [KEY|INDEX] | SPATIAL [KEY|INDEX] [nome] (colonne)
        bool fulltext = ident_is(ps, "FULLTEXT"), spatial = ident_is(ps, "SPATIAL");
        if (peek(ps).type == TOK_KEY || peek(ps).type == TOK_INDEX || fulltext || spatial) {
            consume(ps);
            if ((fulltext || spatial) && (peek(ps).type == TOK_KEY || peek(ps).type == TOK_INDEX)) consume(ps);
            if (n->create_table.num_keys >= SV_MAX_KEY_DEFS) { parse_fail(ps, "Too many indexes (max 16)", peek(ps)); return; }
            TableKeyDef* k = &n->create_table.keys[n->create_table.num_keys];
            memset(k, 0, sizeof(*k));
            if (peek(ps).type == TOK_IDENTIFIER && !ident_is(ps, "USING")) { Token nm = consume(ps); copy_text(ps, k->name, sizeof(k->name), nm.text); }
            skip_index_using(ps);
            k->ncols = parse_name_list(ps, k->cols, 8);
            skip_index_using(ps);
            k->fulltext = fulltext;
            if (!ps->result->success) return;
            if (!spatial) n->create_table.num_keys++;
            continue;
        }
        parse_column_def(ps, n);
        if (!ps->result->success) return;
    } while (match(ps, TOK_COMMA));
    if (!expect(ps, TOK_RPAREN)) return;
    // opzioni di tabella di MySQL: ENGINE = x, [DEFAULT] CHARSET = x, COLLATE x, AUTO_INCREMENT = n, COMMENT = '...'
    while (ps->result->success && peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF) {
        if (match(ps, TOK_COMMA)) continue;
        if (ident_is(ps, "DEFAULT")) { consume(ps); continue; }
        Token o = consume(ps);
        if (o.type != TOK_IDENTIFIER && o.type != TOK_SET) { parse_fail(ps, "Unexpected text after the column list", o); return; }
        char opt[40];
        copy_text(ps, opt, sizeof(opt), o.text);
        if (!strcasecmp(opt, "CHARACTER") && (ident_is(ps, "SET") || peek(ps).type == TOK_SET)) { consume(ps); snprintf(opt, sizeof(opt), "CHARSET"); }
        if (!strcasecmp(opt, "PARTITION")) { parse_fail(ps, "PARTITION BY is not supported", o); return; }
        match(ps, TOK_EQ);
        Token v = consume(ps);
        if (v.type == TOK_EOF || v.type == TOK_SEMICOLON) { parse_fail(ps, "Expected a value for the table option", v); return; }
        if (!strcasecmp(opt, "COMMENT")) copy_text(ps, n->create_table.comment, sizeof(n->create_table.comment), v.text);
        else if (!strcasecmp(opt, "COLLATE")) copy_text(ps, n->create_table.collation, sizeof(n->create_table.collation), v.text);
        else if (!strcasecmp(opt, "CHARSET") && !n->create_table.collation[0] && v.text) {
            if (!strncasecmp(v.text, "utf8mb4", 7)) snprintf(n->create_table.collation, sizeof(n->create_table.collation), "utf8mb4_general_ci");
            else if (!strncasecmp(v.text, "utf8", 4)) snprintf(n->create_table.collation, sizeof(n->create_table.collation), "utf8mb3_general_ci");
            else if (!strncasecmp(v.text, "latin1", 6)) snprintf(n->create_table.collation, sizeof(n->create_table.collation), "latin1_swedish_ci");
        } else if (!strcasecmp(opt, "AUTO_INCREMENT") && v.text) n->create_table.auto_increment = atoll(v.text);
        // ENGINE, ROW_FORMAT, STATS_*, KEY_BLOCK_SIZE, ...: accettate e ignorate
    }
}

static bool view_option(Token t) {
    return t.type == TOK_IDENTIFIER && t.text &&
           (!strcasecmp(t.text, "ALGORITHM") || !strcasecmp(t.text, "DEFINER") || !strcasecmp(t.text, "SQL"));
}

// nome [(colonne)] AS SELECT ... [WITH [CASCADED | LOCAL] CHECK OPTION]   (dopo CREATE VIEW / ALTER VIEW)
// La definizione è ricostruita dai token (le stringhe tornano fra apici); con le colonne diventa
// WITH nome (colonne) AS (SELECT ...) SELECT * FROM nome
static void parse_view_def(ParserState* ps, ASTNode* n) {
    n->type = NODE_CREATE_VIEW;
    Token vn = consume(ps);
    if (vn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the view name", vn); return; }
    copy_text(ps, n->create_view.view_name, sizeof(n->create_view.view_name), vn.text);
    int cols_from = -1, cols_to = -1;
    if (peek(ps).type == TOK_LPAREN) {
        cols_from = ps->pos;
        consume(ps);
        do {
            Token c = consume(ps);
            if (c.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in the view column list", c); return; }
        } while (match(ps, TOK_COMMA));
        if (!expect(ps, TOK_RPAREN)) return;
        cols_to = ps->pos;
    }
    if (!expect(ps, TOK_AS)) return;
    if (!query_starts(ps, 0)) { parse_fail(ps, "A view must be defined by a SELECT", peek(ps)); return; }
    int start = ps->pos;
    ASTNode* probe = parse_query(ps);                    // la definizione deve essere una SELECT valida
    free_node(probe);
    if (!ps->result->success) return;
    int end = ps->pos;
    if (ident_is(ps, "WITH")) {                           // WITH [CASCADED | LOCAL] CHECK OPTION: accettato
        consume(ps);
        if (ident_is(ps, "CASCADED") || ident_is(ps, "LOCAL")) consume(ps);
        if (!ident_is(ps, "CHECK")) { parse_fail(ps, "Expected CHECK OPTION", peek(ps)); return; }
        consume(ps);
        if (!ident_is(ps, "OPTION")) { parse_fail(ps, "Expected CHECK OPTION", peek(ps)); return; }
        consume(ps);
    }
    if (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF) { parse_fail(ps, "Unexpected text in the view definition", peek(ps)); return; }
    char* def = n->create_view.definition;
    size_t cap = sizeof(n->create_view.definition);
    bool fits;
    if (cols_from >= 0) {
        size_t l = (size_t)snprintf(def, cap, "WITH %s ", n->create_view.view_name);
        fits = l < cap && tokens_text(ps, cols_from, cols_to, def + l, cap - l);
        l = strlen(def);
        if (fits) fits = l + 6 < cap && (strcat(def, " AS ("), tokens_text(ps, start, end, def + l + 5, cap - l - 5));
        l = strlen(def);
        if (fits) fits = (size_t)snprintf(def + l, cap - l, ") SELECT * FROM %s", n->create_view.view_name) < cap - l;
    } else fits = tokens_text(ps, start, end, def, cap);
    if (!fits) parse_fail(ps, "View definition is too long (max 64 KB)", peek(ps));
}

// il token dopo quello corrente è la parola w?
static bool peek2_ident(ParserState* ps, const char* w) {
    if (ps->pos + 1 >= ps->tokens->count) return false;
    const Token* t = &ps->tokens->tokens[ps->pos + 1];
    return t->text && !strcasecmp(t->text, w);
}

// ── SHOW di MySQL ────────────────────────────────────────────────
// SHOW COLUMNS, SHOW INDEX / KEYS, SHOW TABLE STATUS, SHOW DATABASES LIKE, SHOW CREATE DATABASE,
// SHOW ENGINES, ... diventano SELECT su information_schema (stessi nomi di colonna di MySQL).

// letterale SQL fra apici
static void sql_quote(char* out, size_t n, const char* v) {
    size_t o = 0;
    if (o + 1 < n) out[o++] = '\'';
    for (; v && *v && o + 3 < n; v++) {
        if (*v == '\'' || *v == '\\') out[o++] = *v;
        out[o++] = *v;
    }
    if (o + 1 < n) out[o++] = '\'';
    out[o] = '\0';
}

// [FROM | IN] nome [. nome]: table = ultima parte, db = prima (se c'è)
static bool show_target(ParserState* ps, char* db, char* table) {
    db[0] = table[0] = '\0';
    if (!match(ps, TOK_FROM) && !match(ps, TOK_IN)) { parse_fail(ps, "Expected FROM table", peek(ps)); return false; }
    Token a = consume(ps);
    if (a.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", a); return false; }
    copy_text(ps, table, 64, a.text);
    if (match(ps, TOK_DOT)) {
        Token b = consume(ps);
        if (b.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", b); return false; }
        memcpy(db, table, 64);
        copy_text(ps, table, 64, b.text);
    }
    if (match(ps, TOK_FROM) || match(ps, TOK_IN)) {            // SHOW COLUMNS FROM t FROM db
        Token d = consume(ps);
        if (d.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a database name", d); return false; }
        copy_text(ps, db, 64, d.text);
    }
    return true;
}

// [LIKE 'p' | WHERE condizione] in coda: like = condizione SQL sulla colonna, where = testo
static bool show_filter(ParserState* ps, const char* like_col, char* like, size_t ln, char* where, size_t wn) {
    like[0] = where[0] = '\0';
    if (ident_is(ps, "LIKE") || peek(ps).type == TOK_LIKE) {
        consume(ps);
        Token p = consume(ps);
        if (p.type != TOK_STRING) { parse_fail(ps, "Expected a quoted pattern after LIKE", p); return false; }
        char q[300];
        sql_quote(q, sizeof(q), p.text);
        snprintf(like, ln, " AND %s LIKE %s", like_col, q);
    } else if (match(ps, TOK_WHERE)) {
        int start = ps->pos;
        while (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF) consume(ps);
        if (!tokens_text(ps, start, ps->pos, where, wn)) { parse_fail(ps, "WHERE condition is too long", peek(ps)); return false; }
    }
    return true;
}

static ASTNode* parse_generated(ParserState* ps, const char* sql) {
    TokenList* tk = lexer_tokenize(sql);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    ASTNode* st = NULL;
    if (pr && pr->success && pr->count == 1) { st = pr->statements[0]; pr->count = 0; }
    else parse_fail(ps, pr && pr->error[0] ? pr->error : "Out of memory", peek(ps));
    if (pr) parse_result_free(pr);
    if (tk) lexer_free(tk);
    if (st) snprintf(ps->qual_db, sizeof(ps->qual_db), "information_schema");
    return st;
}

// NULL (posizione invariata) se non è uno dei SHOW di MySQL gestiti così
static ASTNode* parse_show_mysql(ParserState* ps) {
    int save = ps->pos;
    bool full = false;
    if (ident_is(ps, "FULL")) { consume(ps); full = true; }
    char db[64] = "", table[64] = "", like[400] = "", where[2048] = "", schema[200] = "", sql[6000] = "", tq[200] = "";
    if (ident_is(ps, "COLUMNS") || ident_is(ps, "FIELDS")) {
        consume(ps);
        if (!show_target(ps, db, table) || !show_filter(ps, "COLUMN_NAME", like, sizeof(like), where, sizeof(where))) return NULL;
        if (db[0]) sql_quote(schema, sizeof(schema), db); else snprintf(schema, sizeof(schema), "DATABASE()");
        sql_quote(tq, sizeof(tq), table);
        snprintf(sql, sizeof(sql),
                 "SELECT COLUMN_NAME AS `Field`, COLUMN_TYPE AS `Type`, %s IS_NULLABLE AS `Null`, COLUMN_KEY AS `Key`, "
                 "COLUMN_DEFAULT AS `Default`, EXTRA AS `Extra`%s FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA = %s AND TABLE_NAME = %s%s ORDER BY ORDINAL_POSITION",
                 full ? "COLLATION_NAME AS `Collation`," : "", full ? ", PRIVILEGES AS `Privileges`, COLUMN_COMMENT AS `Comment`" : "",
                 schema, tq, like);
    } else if ((peek(ps).type == TOK_INDEX && !full) || ident_is(ps, "KEYS")) {
        consume(ps);
        if (!show_target(ps, db, table) || !show_filter(ps, "INDEX_NAME", like, sizeof(like), where, sizeof(where))) return NULL;
        if (db[0]) sql_quote(schema, sizeof(schema), db); else snprintf(schema, sizeof(schema), "DATABASE()");
        sql_quote(tq, sizeof(tq), table);
        snprintf(sql, sizeof(sql),
                 "SELECT TABLE_NAME AS `Table`, NON_UNIQUE AS `Non_unique`, INDEX_NAME AS `Key_name`, SEQ_IN_INDEX AS `Seq_in_index`, "
                 "COLUMN_NAME AS `Column_name`, COLLATION AS `Collation`, CARDINALITY AS `Cardinality`, SUB_PART AS `Sub_part`, "
                 "PACKED AS `Packed`, NULLABLE AS `Null`, INDEX_TYPE AS `Index_type`, COMMENT AS `Comment`, "
                 "INDEX_COMMENT AS `Index_comment`, IS_VISIBLE AS `Visible`, EXPRESSION AS `Expression` "
                 "FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = %s AND TABLE_NAME = %s%s", schema, tq, like);
    } else if (peek(ps).type == TOK_TABLE && !full) {
        consume(ps);
        if (!ident_is(ps, "STATUS")) { ps->pos = save; return NULL; }
        consume(ps);
        db[0] = '\0';
        if (match(ps, TOK_FROM) || match(ps, TOK_IN)) { Token d = consume(ps); copy_text(ps, db, sizeof(db), d.text); }
        if (!show_filter(ps, "TABLE_NAME", like, sizeof(like), where, sizeof(where))) return NULL;
        if (db[0]) sql_quote(schema, sizeof(schema), db); else snprintf(schema, sizeof(schema), "DATABASE()");
        snprintf(sql, sizeof(sql),
                 "SELECT TABLE_NAME AS `Name`, ENGINE AS `Engine`, VERSION AS `Version`, ROW_FORMAT AS `Row_format`, "
                 "TABLE_ROWS AS `Rows`, AVG_ROW_LENGTH AS `Avg_row_length`, DATA_LENGTH AS `Data_length`, "
                 "MAX_DATA_LENGTH AS `Max_data_length`, INDEX_LENGTH AS `Index_length`, DATA_FREE AS `Data_free`, "
                 "AUTO_INCREMENT AS `Auto_increment`, CREATE_TIME AS `Create_time`, UPDATE_TIME AS `Update_time`, "
                 "CHECK_TIME AS `Check_time`, TABLE_COLLATION AS `Collation`, CHECKSUM AS `Checksum`, "
                 "CREATE_OPTIONS AS `Create_options`, TABLE_COMMENT AS `Comment` FROM information_schema.TABLES "
                 "WHERE TABLE_SCHEMA = %s%s", schema, like);
    } else if (!full && (ident_is(ps, "DATABASES") || ident_is(ps, "SCHEMAS")) &&
               (next_is(ps, 1, TOK_LIKE) || next_is(ps, 1, TOK_WHERE))) {
        consume(ps);
        if (!show_filter(ps, "SCHEMA_NAME", like, sizeof(like), where, sizeof(where))) return NULL;
        snprintf(sql, sizeof(sql), "SELECT SCHEMA_NAME AS `Database` FROM information_schema.SCHEMATA WHERE 1 = 1%s ORDER BY SCHEMA_NAME", like);
    } else if (!full && peek(ps).type == TOK_CREATE && (next_is(ps, 1, TOK_DATABASE) || (peek2_ident(ps, "SCHEMA")))) {
        consume(ps);
        consume(ps);
        if (ident_is(ps, "IF")) { consume(ps); match(ps, TOK_NOT); match(ps, TOK_EXISTS); }
        Token d = consume(ps);
        if (d.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a database name", d); return NULL; }
        copy_text(ps, db, sizeof(db), d.text);
        sql_quote(schema, sizeof(schema), db);
        char create[300], cq[400];
        snprintf(create, sizeof(create), "CREATE DATABASE `%s` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci */", db);
        sql_quote(cq, sizeof(cq), create);
        snprintf(sql, sizeof(sql), "SELECT SCHEMA_NAME AS `Database`, %s AS `Create Database` FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = %s", cq, schema);
    } else if (!full && ident_is(ps, "ENGINES")) {
        consume(ps);
        snprintf(sql, sizeof(sql), "SELECT ENGINE AS `Engine`, SUPPORT AS `Support`, COMMENT AS `Comment`, TRANSACTIONS AS `Transactions`, "
                 "XA AS `XA`, SAVEPOINTS AS `Savepoints` FROM information_schema.ENGINES");
    } else if (!full && (ident_is(ps, "CHARSET") || (ident_is(ps, "CHARACTER") && (next_is(ps, 1, TOK_SET) || peek2_ident(ps, "SET"))))) {
        if (ident_is(ps, "CHARACTER")) consume(ps);
        consume(ps);
        if (!show_filter(ps, "CHARACTER_SET_NAME", like, sizeof(like), where, sizeof(where))) return NULL;
        snprintf(sql, sizeof(sql), "SELECT CHARACTER_SET_NAME AS `Charset`, DESCRIPTION AS `Description`, DEFAULT_COLLATE_NAME AS "
                 "`Default collation`, MAXLEN AS `Maxlen` FROM information_schema.CHARACTER_SETS WHERE 1 = 1%s", like);
    } else if (!full && ident_is(ps, "COLLATION")) {
        consume(ps);
        if (!show_filter(ps, "COLLATION_NAME", like, sizeof(like), where, sizeof(where))) return NULL;
        snprintf(sql, sizeof(sql), "SELECT COLLATION_NAME AS `Collation`, CHARACTER_SET_NAME AS `Charset`, ID AS `Id`, IS_DEFAULT AS "
                 "`Default`, IS_COMPILED AS `Compiled`, SORTLEN AS `Sortlen`, PAD_ATTRIBUTE AS `Pad_attribute` "
                 "FROM information_schema.COLLATIONS WHERE 1 = 1%s", like);
    } else if (ident_is(ps, "PROCESSLIST")) {
        consume(ps);
        snprintf(sql, sizeof(sql), "SELECT ID AS `Id`, USER AS `User`, HOST AS `Host`, DB AS `db`, COMMAND AS `Command`, TIME AS `Time`, "
                 "STATE AS `State`, INFO AS `Info` FROM information_schema.PROCESSLIST");
    } else if (!full && (ident_is(ps, "WARNINGS") || ident_is(ps, "ERRORS") || ident_is(ps, "PLUGINS"))) {
        bool plugins = ident_is(ps, "PLUGINS");
        consume(ps);
        while (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF) consume(ps);    // LIMIT ...
        if (plugins) snprintf(sql, sizeof(sql), "SELECT PLUGIN_NAME AS `Name`, PLUGIN_STATUS AS `Status`, PLUGIN_TYPE AS `Type` FROM information_schema.PLUGINS");
        else snprintf(sql, sizeof(sql), "SELECT STATE AS `Level`, ID AS `Code`, INFO AS `Message` FROM information_schema.PROCESSLIST");
    } else {
        ps->pos = save;
        return NULL;
    }
    if (!ps->result->success) return NULL;
    if (where[0]) {                                            // SHOW ... WHERE: filtro sul risultato
        char wrapped[sizeof(sql) + sizeof(where) + 64];
        snprintf(wrapped, sizeof(wrapped), "SELECT * FROM (%s) AS show_result WHERE %s", sql, where);
        return parse_generated(ps, wrapped);
    }
    return parse_generated(ps, sql);
}

static ASTNode* parse_statement(ParserState* ps) {
    ASTNode* n = calloc(1, sizeof(ASTNode));
    if (!n) return NULL;
    Token t = peek(ps);
    if (ident_is(ps, "CALL") && next_is(ps, 1, TOK_IDENTIFIER)) { parse_call_stmt(ps, n); return n; }
    // START TRANSACTION [READ WRITE | READ ONLY | WITH CONSISTENT SNAPSHOT] = BEGIN (MySQL)
    if (ident_is(ps, "START") && next_is(ps, 1, TOK_TRANSACTION)) {
        consume(ps);
        consume(ps);
        n->type = NODE_BEGIN;
        while (peek(ps).type != TOK_SEMICOLON && peek(ps).type != TOK_EOF) consume(ps);
        return n;
    }
    // CHECK | ANALYZE | OPTIMIZE | REPAIR  TABLE t [, t2 ...]   (manutenzione, come MySQL)
    if ((ident_is(ps, "ANALYZE") || ident_is(ps, "OPTIMIZE") || ident_is(ps, "REPAIR") || ident_is(ps, "CHECK")) &&
        next_is(ps, 1, TOK_TABLE)) {
        Token op = consume(ps);
        consume(ps);
        n->type = NODE_TABLE_MAINT;
        snprintf(n->maint.op, sizeof(n->maint.op), "%s", op.text);
        for (char* c = n->maint.op; *c; c++) *c = (char)toupper((unsigned char)*c);
        do {
            if (n->maint.ntables >= 16) { parse_fail(ps, "Too many tables (max 16)", peek(ps)); return n; }
            Token tn = table_tok(ps);
            if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", tn); return n; }
            copy_text(ps, n->maint.tables[n->maint.ntables++], 64, tn.text);
        } while (match(ps, TOK_COMMA));
        while (ident_is(ps, "QUICK") || ident_is(ps, "FAST") || ident_is(ps, "MEDIUM") || ident_is(ps, "EXTENDED") ||
               ident_is(ps, "CHANGED") || word_is(ps, "FOR") || ident_is(ps, "UPGRADE")) consume(ps);   // opzioni di MySQL
        return n;
    }
    if (ident_is(ps, "BACKUP")) {                         // BACKUP [DATABASE] TO 'cartella'
        consume(ps);
        n->type = NODE_BACKUP;
        if (peek(ps).type == TOK_DATABASE) consume(ps);
        if (!expect(ps, TOK_TO)) return n;
        Token p = consume(ps);
        if (p.type != TOK_STRING || !p.text[0]) { parse_fail(ps, "Expected the backup folder as a string: BACKUP TO 'folder'", p); return n; }
        copy_text(ps, n->backup.path, sizeof(n->backup.path), p.text);
        return n;
    }

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
            parse_delete(ps, n);
            break;
        }
        case TOK_CREATE: {
            consume(ps);
            Token sub = consume(ps);
            bool or_replace = false;                            // CREATE OR REPLACE VIEW
            if (sub.type == TOK_OR && ident_is(ps, "REPLACE")) { consume(ps); or_replace = true; sub = consume(ps); }
            // opzioni MySQL delle viste (nei dump): ALGORITHM = x, DEFINER = `u`@`h`, SQL SECURITY x
            for (int g = 0; g < 4 && view_option(sub); g++) {
                while (peek(ps).type != TOK_VIEW && peek(ps).type != TOK_TRIGGER && peek(ps).type != TOK_EOF &&
                       peek(ps).type != TOK_SEMICOLON && !ident_is(ps, "PROCEDURE") && !view_option(peek(ps))) consume(ps);
                sub = consume(ps);
            }
            if (or_replace && sub.type != TOK_VIEW) { parse_fail(ps, "OR REPLACE is only supported for views", sub); break; }
            bool unique_index = false;                          // CREATE UNIQUE INDEX
            if (sub.type == TOK_IDENTIFIER && sub.text && !strcasecmp(sub.text, "UNIQUE") && peek(ps).type == TOK_INDEX) {
                unique_index = true;
                sub = consume(ps);
            }
            if (ident_is(ps, "IF")) {                            // CREATE ... IF NOT EXISTS
                consume(ps);
                if (!match(ps, TOK_NOT) || !match(ps, TOK_EXISTS)) { parse_fail(ps, "Expected IF NOT EXISTS", peek(ps)); break; }
                n->if_exists = true;
            }
            if (sub.type == TOK_IDENTIFIER && sub.text && !strcasecmp(sub.text, "PROCEDURE")) {
                return parse_create_procedure(ps, n);
            } else if (sub.type == TOK_DATABASE) {
                n->type = NODE_CREATE_DB;
                Token dn = consume(ps);
                copy_text(ps, n->create_db.db_name, sizeof(n->create_db.db_name), dn.text);
            } else if (sub.type == TOK_TRIGGER) {
                return parse_create_trigger(ps, n);
            } else if (sub.type == TOK_TABLE) {
                parse_create_table(ps, n);
            } else if (sub.type == TOK_INDEX) {
                // CREATE [UNIQUE] INDEX nome ON t (c1 [ASC|DESC] [, c2 ...]) [USING BTREE|HASH]
                n->type = NODE_CREATE_INDEX;
                Token in = consume(ps);
                copy_text(ps, n->create_index.index_name, sizeof(n->create_index.index_name), in.text);
                expect(ps, TOK_ON);
                Token tn = table_tok(ps);
                copy_text(ps, n->create_index.table_name, sizeof(n->create_index.table_name), tn.text);
                expect(ps, TOK_LPAREN);
                char cols[8][64];
                int ncols = 0;
                while (ps->result->success) {
                    Token cn = consume(ps);
                    if (cn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a column name in the index", cn); break; }
                    if (ncols < 8) copy_text(ps, cols[ncols++], sizeof(cols[0]), cn.text);
                    if (peek(ps).type == TOK_LPAREN) { consume(ps); consume(ps); expect(ps, TOK_RPAREN); }   // prefisso: col(10)
                    if (peek(ps).type == TOK_ASC || peek(ps).type == TOK_DESC) consume(ps);
                    if (match(ps, TOK_COMMA)) continue;
                    expect(ps, TOK_RPAREN);
                    break;
                }
                if (ident_is(ps, "USING")) { consume(ps); consume(ps); }
                if (ncols > 0) copy_text(ps, n->create_index.col_name, sizeof(n->create_index.col_name), cols[0]);
                if (unique_index && ps->result->success) {
                    // = ALTER TABLE t ADD CONSTRAINT nome UNIQUE (colonne): il vincolo fa da indice
                    char table[64], name[64];
                    snprintf(table, sizeof(table), "%s", n->create_index.table_name);
                    snprintf(name, sizeof(name), "%s", n->create_index.index_name);
                    memset(&n->alter_table, 0, sizeof(n->alter_table));
                    n->type = NODE_ALTER_TABLE;
                    snprintf(n->alter_table.table_name, sizeof(n->alter_table.table_name), "%s", table);
                    snprintf(n->alter_table.add.name, sizeof(n->alter_table.add.name), "%s", name);
                    n->alter_table.add.kind = CONS_UNIQUE;
                    n->alter_table.add.ncols = ncols;
                    for (int k = 0; k < ncols; k++) snprintf(n->alter_table.add.cols[k], sizeof(n->alter_table.add.cols[k]), "%.63s", cols[k]);
                    n->alter_table.has_add = true;
                }
                // più colonne: l'indice accelera tramite la prima (come il prefisso di un indice composto)
            } else if (sub.type == TOK_VIEW) {
                n->create_view.replace = or_replace;
                parse_view_def(ps, n);
            }
            break;
        }
        case TOK_ALTER: {
            consume(ps);
            if (match(ps, TOK_VIEW)) {                            // ALTER VIEW v [(colonne)] AS SELECT ...
                n->create_view.alter = true;
                parse_view_def(ps, n);
                break;
            }
            expect(ps, TOK_TABLE);
            n->type = NODE_ALTER_TABLE;
            Token tn = table_tok(ps);
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
                    if (set) { parse_default_value(ps, d->default_val, sizeof(d->default_val)); d->has_default = true; }
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
                Token tn = table_tok(ps);
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
                Token tn = table_tok(ps);
                copy_text(ps, n->drop_index.table_name, sizeof(n->drop_index.table_name), tn.text);
            } else if (sub.type == TOK_IDENTIFIER && sub.text && !strcasecmp(sub.text, "PROCEDURE")) {
                n->type = NODE_DROP_PROCEDURE;
                Token pn = consume(ps);
                if (pn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected the procedure name", pn); break; }
                copy_text(ps, n->drop_trigger.name, sizeof(n->drop_trigger.name), pn.text);
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
            ASTNode* mysql_show = parse_show_mysql(ps);
            if (mysql_show || !ps->result->success) { free_node(n); return mysql_show; }
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
            else if (what.type == TOK_CREATE && match(ps, TOK_VIEW)) { kind = "CREATE VIEW"; needs_table = true; }
            else if (what.type == TOK_CREATE) { expect(ps, TOK_TABLE); kind = "CREATE"; needs_table = true; }
            else if (!strcasecmp(what.text, "FULL") && ident_is(ps, "TABLES")) { consume(ps); kind = "FULL TABLES"; }
            else if (!strcasecmp(what.text, "CONSTRAINTS")) { kind = "CONSTRAINTS"; needs_table = true; }
            else if (what.type == TOK_INDEX || !strcasecmp(what.text, "INDEXES") || !strcasecmp(what.text, "KEYS")) { kind = "INDEXES"; needs_table = true; }
            if (!kind) { parse_fail(ps, "Expected DATABASES, TABLES, FULL TABLES, STATUS, TRIGGERS, CREATE TABLE / VIEW, CONSTRAINTS or INDEXES after SHOW", what); break; }
            if ((!strcmp(kind, "TABLES") || !strcmp(kind, "FULL TABLES") || !strcmp(kind, "DATABASES")) && ps->result->success) {
                if (strcmp(kind, "DATABASES") && (match(ps, TOK_FROM) || match(ps, TOK_IN))) {   // SHOW TABLES FROM db
                    Token d = consume(ps);
                    if (d.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a database name", d); break; }
                    copy_text(ps, ps->qual_db, sizeof(ps->qual_db), d.text);
                }
                if (ident_is(ps, "LIKE") || peek(ps).type == TOK_LIKE) {                     // ... LIKE 'wp\_%'
                    consume(ps);
                    Token p = consume(ps);
                    if (p.type != TOK_STRING) { parse_fail(ps, "Expected a quoted pattern after LIKE", p); break; }
                    copy_text(ps, n->select_stmt.show_like, sizeof(n->select_stmt.show_like), p.text);
                    n->select_stmt.has_show_like = true;
                }
            }
            if (!strcmp(kind, "TRIGGERS") && (match(ps, TOK_FROM) || match(ps, TOK_IN))) {      // SHOW TRIGGERS [FROM t]
                Token tn = table_tok(ps);
                if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", tn); break; }
                copy_text(ps, n->select_stmt.table_alias, sizeof(n->select_stmt.table_alias), tn.text);
            }
            snprintf(n->select_stmt.table_name, sizeof(n->select_stmt.table_name), "%s", kind);
            if (needs_table) {
                if (strncmp(kind, "CREATE", 6) && !match(ps, TOK_FROM) && !match(ps, TOK_IN)) { parse_fail(ps, "Expected FROM table", peek(ps)); break; }
                Token tn = table_tok(ps);
                if (tn.type != TOK_IDENTIFIER) { parse_fail(ps, "Expected a table name", tn); break; }
                copy_text(ps, n->select_stmt.table_alias, sizeof(n->select_stmt.table_alias), tn.text);
            }
            break;
        }
        case TOK_DESCRIBE: {
            consume(ps);
            n->type = NODE_DESCRIBE;
            if (peek(ps).type == TOK_TABLE) consume(ps);          // DESCRIBE TABLE t
            Token tn = table_tok(ps);
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
            if (!match(ps, TOK_TRANSACTION) && ident_is(ps, "WORK")) consume(ps);
            break;
        }
        case TOK_COMMIT: {
            consume(ps);
            n->type = NODE_COMMIT;
            if (ident_is(ps, "WORK")) consume(ps);
            break;
        }
        case TOK_ROLLBACK: {
            consume(ps);
            if (ident_is(ps, "WORK")) consume(ps);
            if (match(ps, TOK_TO)) {
                match(ps, TOK_SAVEPOINT);
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
            Token tn = table_tok(ps);
            copy_text(ps, n->delete_stmt.table_name, sizeof(n->delete_stmt.table_name), tn.text);
            break;
        }
        default:
            if (query_starts(ps, 0)) { free(n); return parse_query(ps); }   // WITH ...
            if (ident_is(ps, "CHECKPOINT")) { consume(ps); n->type = NODE_CHECKPOINT; break; }
            if (ident_is(ps, "VACUUM")) { consume(ps); n->type = NODE_VACUUM; break; }
            if (ident_is(ps, "REPLACE") && !next_is(ps, 1, TOK_LPAREN)) {   // MySQL: REPLACE [INTO] t ...
                consume(ps);
                parse_insert(ps, n);
                n->insert_stmt.mode = INS_REPLACE;
                break;
            }
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
    ps.qual_db[0] = '\0';
    ps.plain_table = false;

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
        ps.qual_db[0] = '\0';
        ps.plain_table = false;
        ASTNode* stmt = parse_statement(&ps);
        if (stmt) {
            memcpy(stmt->qual_db, ps.qual_db, sizeof(stmt->qual_db));
            stmt->qual_mixed = ps.qual_db[0] && ps.plain_table;
            add_stmt(&ps, stmt);
        }
        match(&ps, TOK_SEMICOLON);
        if (!ps.result->success) break;
    }

    return r;
}

ASTExpr* parser_parse_expr(TokenList* tokens, char* err, size_t errn) {
    ParseResult r;
    memset(&r, 0, sizeof(r));
    r.success = true;
    ParserState ps = { tokens, 0, &r, 0, "", false };
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
            for (int j = 0; j < n->insert_stmt.num_dup; j++) ast_expr_free(n->insert_stmt.dup_exprs[j]);
            break;
        case NODE_UPDATE:
            for (int j = 0; j < n->update_stmt.num_sets; j++) { ast_expr_free(n->update_stmt.set_exprs[j]); free(n->update_stmt.set_texts[j]); }
            ast_expr_free(n->update_stmt.where);
            free(n->update_stmt.multi_from); free(n->update_stmt.multi_where); free(n->update_stmt.order_limit);
            break;
        case NODE_DELETE:
            ast_expr_free(n->delete_stmt.where);
            free(n->delete_stmt.multi_from); free(n->delete_stmt.multi_where); free(n->delete_stmt.order_limit);
            break;
        case NODE_EXPLAIN:
            free_node((ASTNode*)n->explain.stmt);
            break;
        case NODE_ALTER_TABLE:
            free_node(n->alter_table.add_column);
            break;
        case NODE_CALL:
            for (int j = 0; j < n->call.nargs; j++) ast_expr_free(n->call.args[j]);
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
