#ifndef PARSER_H
#define PARSER_H

#include "../../include/common.h"
#include "lexer.h"
#include "../catalog/schema.h"

typedef enum {
    NODE_SELECT, NODE_INSERT, NODE_UPDATE, NODE_DELETE,
    NODE_CREATE_DB, NODE_CREATE_TABLE, NODE_CREATE_INDEX, NODE_CREATE_VIEW,
    NODE_DROP_DB, NODE_DROP_TABLE, NODE_DROP_VIEW, NODE_DROP_INDEX,
    NODE_ALTER_TABLE, NODE_USE, NODE_SHOW, NODE_DESCRIBE,
    NODE_EXPLAIN, NODE_BEGIN, NODE_COMMIT, NODE_ROLLBACK,
    NODE_TRUNCATE, NODE_SAVEPOINT, NODE_ROLLBACK_TO,
    NODE_CREATE_TRIGGER, NODE_DROP_TRIGGER,
    NODE_CHECKPOINT,
} StmtType;

typedef enum { JOIN_INNER, JOIN_LEFT, JOIN_RIGHT, JOIN_CROSS, JOIN_FULL } JoinType;

typedef enum { EXPR_COLUMN, EXPR_INT, EXPR_FLOAT, EXPR_STRING, EXPR_STAR,
               EXPR_BINARY, EXPR_UNARY, EXPR_NULL,
               EXPR_AGG,    // aggregato: func = COUNT/SUM/AVG/MIN/MAX; args[0] = argomento, col_name "*" per COUNT(*)
               EXPR_FUNC,   // funzione scalare: func = nome (UPPER, LENGTH, COALESCE, ...), args = argomenti
               EXPR_SUBQUERY, // subquery: subq; subkind = SUBQ_*; left = operando di IN / ANY / ALL
               EXPR_CASE    // CASE [left] WHEN args[0] THEN args[1] ... [ELSE right] END
} ExprType;

// tipi di subquery
//   SUBQ_SCALAR  (SELECT ...)            un valore (NULL se nessuna riga, errore se più righe)
//   SUBQ_EXISTS  EXISTS (SELECT ...)
//   SUBQ_ANY     left op ANY (SELECT ...)   (x IN (SELECT ...) = x = ANY (...))
//   SUBQ_ALL     left op ALL (SELECT ...)   (x NOT IN (SELECT ...) = x <> ALL (...))
enum { SUBQ_SCALAR, SUBQ_EXISTS, SUBQ_ANY, SUBQ_ALL };

struct ASTNode;
// subquery condivisa: le copie di un'espressione (BETWEEN, IN) puntano alla stessa
typedef struct SubQuery {
    struct ASTNode* node;            // la SELECT
    int    refs;
    void*  plan;                     // piano preparato dall'executor (libero con plan_free)
    void (*plan_free)(void*);
} SubQuery;

typedef enum { OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
               OP_AND, OP_OR, OP_NOT, OP_PLUS, OP_MINUS, OP_MUL, OP_DIV,
               OP_IS_NULL, OP_IS_NOT_NULL, OP_LIKE, OP_NOT_LIKE,
               OP_NEG       // meno unario (EXPR_UNARY)
} OpType;

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
    struct ASTExpr** args;    // EXPR_FUNC / EXPR_AGG: argomenti
    int      nargs;
    bool     agg_distinct;    // COUNT(DISTINCT x)
    char     func[16];        // EXPR_AGG / EXPR_FUNC: nome della funzione (maiuscolo)
    char     alias[64];       // SELECT ... AS alias
    // risoluzione fatta dall'executor prima di eseguire (EXPR_COLUMN):
    //   > 0  sorgente * 1024 + colonna + 1      < 0  alias della SELECT numero -(bound + 1)
    int      bound;
    int      outer;           // EXPR_COLUMN: 0 = colonna di questa query, k = della query esterna di k livelli
    int      depth;           // profondità dell'albero (limitata: valutazione e liberazione sono ricorsive)
    SubQuery* subq;           // EXPR_SUBQUERY
    int      subkind;         // EXPR_SUBQUERY: SUBQ_*
} ASTExpr;

// libera un'espressione e tutti i suoi figli
void ast_expr_free(ASTExpr* e);
// copia profonda (NULL se e è NULL o manca memoria)
ASTExpr* ast_expr_clone(const ASTExpr* e);
// testo SQL dell'espressione (intestazioni delle colonne calcolate, EXPLAIN)
void ast_expr_format(const ASTExpr* e, char* out, size_t n);

typedef struct {
    char     name[64];
    char     type[32];
    int      size;
    bool     nullable;
    bool     primary_key;
    bool     auto_increment;
    char     default_val[128];
} ColumnDef;

// vincolo di CREATE TABLE / ALTER TABLE ADD CONSTRAINT (di colonna o di tabella)
typedef struct {
    char     name[64];               // CONSTRAINT nome ("" = generato)
    int      kind;                   // CONS_PRIMARY, CONS_UNIQUE, CONS_FOREIGN, CONS_CHECK (schema.h)
    int      ncols;
    char     cols[8][64];
    char     ref_table[64];
    int      nref;
    char     ref_cols[8][64];        // vuoto: la chiave primaria della tabella riferita
    int      on_delete, on_update;   // FkAction
    char     check[512];             // testo della condizione
} ConstraintDef;

#define SV_MAX_CONS_DEFS 16

typedef struct {
    ASTExpr*   condition;      // ON (NULL = prodotto cartesiano)
    JoinType   type;
    char       table_name[64];
    char       alias[64];      // "" = nessun alias
    struct ASTNode* sub;       // JOIN (SELECT ...) alias: tabella derivata
    bool       natural;        // NATURAL JOIN: uguaglianza sulle colonne con lo stesso nome
    char       using_cols[8][64];   // JOIN ... USING (colonne)
    int        nusing;
    bool       using_built;    // condizione ON già costruita da NATURAL / USING
} ASTJoin;

// WITH nome AS (SELECT ...)
typedef struct {
    char            name[64];
    struct ASTNode* node;
    char            cols[16][64];  // WITH nome (colonne) AS ...
    int             ncols;
    bool            recursive;     // WITH RECURSIVE
    void*           work;          // durante il calcolo ricorsivo: righe dell'iterazione precedente
} ASTCte;

// operatori insiemistici fra SELECT
enum { SETOP_NONE, SETOP_UNION, SETOP_INTERSECT, SETOP_EXCEPT };

#define SV_MAX_ORDER   8
#define SV_MAX_GROUP   16
#define SV_MAX_SETS    64

typedef struct ASTNode {
    StmtType type;
    bool     if_exists;        // DROP ... IF EXISTS / CREATE ... IF NOT EXISTS
    union {
        struct {
            ASTExpr*   columns[64];
            int        num_cols;
            char       table_name[64];
            char       table_alias[64];  // "" = nessun alias
            ASTExpr*   where;
            ASTExpr*   order_exprs[SV_MAX_ORDER];
            bool       order_desc[SV_MAX_ORDER];
            int        num_order;
            int        limit;
            int        offset;
            bool       has_limit;      // LIMIT presente (LIMIT 0 = nessuna riga)
            bool       distinct;
            ASTExpr*   group_exprs[SV_MAX_GROUP];
            int        num_group;
            ASTExpr*   having;
            ASTJoin    joins[8];
            int        num_joins;
            struct ASTNode* from_sub;  // FROM (SELECT ...) alias: tabella derivata
            // SELECT ... UNION [ALL] / INTERSECT / EXCEPT SELECT ...: catena dei rami.
            // Con set_next != NULL, ORDER BY e LIMIT di questo nodo valgono per l'intero risultato.
            struct ASTNode* set_next;
            int        set_op;         // operatore fra questo ramo e set_next (SETOP_*)
            bool       set_all;        // UNION ALL
            ASTCte*    ctes;           // WITH (solo sul primo ramo)
            int        num_ctes;
        } select_stmt;
        struct {
            char       table_name[64];
            char*      columns[64];
            int        num_cols;
            ASTExpr**  values;         // num_rows * num_values espressioni (riga per riga)
            int        num_values;     // valori per riga
            int        num_rows;
            struct ASTNode* select;    // INSERT INTO t [(colonne)] SELECT ...
        } insert_stmt;
        struct {
            char       table_name[64];
            char       set_cols[SV_MAX_SETS][64];
            ASTExpr*   set_exprs[SV_MAX_SETS];
            int        num_sets;
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
            ConstraintDef cons[SV_MAX_CONS_DEFS];
            int        num_cons;
        } create_table;
        struct {
            char       table_name[64];
            char       new_name[64];          // RENAME TO
            ConstraintDef add;                // ADD [CONSTRAINT nome] vincolo
            bool       has_add;
            char       drop_constraint[64];   // DROP CONSTRAINT nome
            struct ASTNode* add_column;       // ADD [COLUMN] definizione (nodo CREATE TABLE con una colonna)
            char       drop_column[64];       // DROP [COLUMN] nome
            char       rename_from[64];       // RENAME COLUMN vecchio TO nuovo
            char       rename_to[64];
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
        struct {
            char       name[64];
        } savepoint;
        struct {
            char       name[64];
        } rollback_to;
        struct {
            char       name[64];
            char       table_name[64];
            bool       before;
            char       event[16];
            char       body[SV_MAX_SQL_LEN];
        } create_trigger;
        struct {
            char       name[64];
        } drop_trigger;
        struct {
            void*      stmt;           // ASTNode* della SELECT da spiegare
            bool       analyze;        // EXPLAIN ANALYZE: esegue la query e mostra le righe reali
        } explain;
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
// una sola espressione (vincoli CHECK salvati nel catalogo); NULL con il messaggio in err
ASTExpr*    parser_parse_expr(TokenList* tokens, char* err, size_t errn);
void        ast_node_free(ASTNode* n);
void        parse_result_free(ParseResult* r);

// ── trigger ──
// Testo salvato nel catalogo (dopo "ON tabella"):
//   [OF colonna {, colonna}] [FOR EACH ROW | FOR EACH STATEMENT] [WHEN (condizione)] corpo
//   corpo := BEGIN istruzione; {istruzione;} END | istruzione
//   istruzione := INSERT | UPDATE | DELETE | SELECT
//              | SET NEW.colonna = espressione          (solo BEFORE ... FOR EACH ROW)
//              | SIGNAL [SQLSTATE '45000'] [SET MESSAGE_TEXT =] 'messaggio'   (annulla lo statement)
//              | IF condizione THEN istruzioni {ELSEIF condizione THEN istruzioni} [ELSE istruzioni] END IF
typedef enum { TS_SQL, TS_SET, TS_SIGNAL, TS_IF } TrigStmtKind;

typedef struct TrigStmt {
    TrigStmtKind      kind;
    ASTNode*          sql;          // TS_SQL
    char              col[64];      // TS_SET: colonna di NEW
    ASTExpr*          expr;         // TS_SET: valore; TS_SIGNAL: messaggio
    ASTExpr*          cond;         // TS_IF
    struct TrigStmt** then_v;
    int               nthen;
    struct TrigStmt** else_v;       // ELSEIF diventa un IF dentro l'ELSE
    int               nelse;
} TrigStmt;

typedef struct {
    bool       success;
    char       error[256];
    char       of_cols[8][64];      // UPDATE OF colonne
    int        n_of;
    bool       for_each_row;        // false = FOR EACH STATEMENT
    ASTExpr*   when;
    TrigStmt** v;
    int        n;
} TrigProgram;

TrigProgram* parser_parse_trigger(TokenList* tokens);
void         trig_program_free(TrigProgram* p);

#endif
