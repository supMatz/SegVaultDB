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
    NODE_CREATE_PROCEDURE, NODE_DROP_PROCEDURE, NODE_CALL,   // usano create_trigger / drop_trigger / call
    NODE_CHECKPOINT,
    NODE_BACKUP,
    NODE_TABLE_MAINT,  // CHECK / ANALYZE / OPTIMIZE / REPAIR TABLE t [, ...]       // BACKUP [DATABASE] TO 'cartella'
    NODE_VACUUM,       // recupera le pagine di overflow non più usate
    NODE_MERGE,        // MERGE INTO t USING s ON ... WHEN [NOT] MATCHED THEN ...
    NODE_SEQUENCE,     // CREATE / ALTER / DROP SEQUENCE
    NODE_SET,          // SET @x = ..., SET NAMES ..., SET FOREIGN_KEY_CHECKS = 0, ... (variabili di MySQL)
} StmtType;

typedef enum { JOIN_INNER, JOIN_LEFT, JOIN_RIGHT, JOIN_CROSS, JOIN_FULL } JoinType;

// clausola WHEN di MERGE (testi SQL)
typedef struct {
    int    kind;                // 0 UPDATE, 1 DELETE, 2 INSERT
    bool   matched;
    char*  cond;                // AND ... (NULL = nessuna)
    char (*cols)[64];           // UPDATE SET / INSERT (colonne): liste dinamiche (P3.12: 32, e 2,3 KB per clausola)
    char** vals;
    int    n, ncols, cap;
    bool   has_cols;
} MergeWhen;

typedef enum { EXPR_COLUMN, EXPR_INT, EXPR_FLOAT, EXPR_STRING, EXPR_STAR,
               EXPR_BINARY, EXPR_UNARY, EXPR_NULL,
               EXPR_AGG,    // aggregato: func = COUNT/SUM/AVG/MIN/MAX; args[0] = argomento, col_name "*" per COUNT(*)
               EXPR_FUNC,   // funzione scalare: func = nome (UPPER, LENGTH, COALESCE, ...), args = argomenti
               EXPR_SUBQUERY, // subquery: subq; subkind = SUBQ_*; left = operando di IN / ANY / ALL
               EXPR_CASE,   // CASE [left] WHEN args[0] THEN args[1] ... [ELSE right] END
               EXPR_WINDOW  // funzione finestra: func(args) OVER (win)
} ExprType;

// estremi del frame: ROWS / RANGE BETWEEN inizio AND fine
enum { FB_UNB_PREC, FB_PREC, FB_CURRENT, FB_FOLL, FB_UNB_FOLL };
enum { FRAME_NONE, FRAME_ROWS, FRAME_RANGE };

// OVER ( [PARTITION BY ...] [ORDER BY ...] [ROWS | RANGE frame] )
typedef struct WindowSpec {
    struct ASTExpr* part[8];
    int             npart;
    struct ASTExpr* ord[8];
    bool            desc[8];
    int             nord;
    int             frame;         // FRAME_*
    int             fs, fe;        // FB_* di inizio e fine
    int64_t         fs_off, fe_off;// n di "n PRECEDING / n FOLLOWING"
    int             slot;          // posizione fra le funzioni finestra della query (executor)
    char            ref[64];       // OVER nome: finestra definita nella clausola WINDOW (risolta dal parser)
    bool            range_time;    // RANGE INTERVAL n unità PRECEDING: fs_off / fe_off in secondi
} WindowSpec;

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
               OP_NEG,      // meno unario (EXPR_UNARY)
               OP_MOD,      // a % b
               OP_ESCAPE    // a LIKE (p ESCAPE e): pattern e carattere di escape, solo come destra di LIKE
} OpType;

typedef struct ASTExpr {
    ExprType type;
    OpType   op;
    char     col_name[64];
    char     table_name[64];
    union {
        int64_t  int_val;
        double   float_val;
        char*    str_val;     // EXPR_STRING: testo allocato (liberato con il nodo)
    };
    struct ASTExpr* left;
    struct ASTExpr* right;
    struct ASTExpr** args;    // EXPR_FUNC / EXPR_AGG: argomenti
    int      nargs;
    bool     agg_distinct;    // COUNT(DISTINCT x)
    int8_t   fmine;           // EXPR_FUNC: cache dell'executor (0 = da calcolare, 1 / -1 = nome in / fuori dall'elenco di
                              // eval_mysql): prima 115 strcmp per riga per ogni funzione fuori elenco (CR-M2)
    char     func[32];        // EXPR_AGG / EXPR_FUNC: nome della funzione (maiuscolo)
    char     alias[64];       // SELECT ... AS alias
    // risoluzione fatta dall'executor prima di eseguire (EXPR_COLUMN):
    //   > 0  BOUND_MAKE(sorgente, colonna)      < 0  alias della SELECT numero -(bound + 1)
    int      bound;
    int      outer;           // EXPR_COLUMN: 0 = colonna di questa query, k = della query esterna di k livelli
    bool     rt_const;        // EXPR_COLUMN: variabile di una routine / NEW / OLD: costante per tutta la query
    int      depth;           // profondità dell'albero (limitata: valutazione e liberazione sono ricorsive)
    SubQuery* subq;           // EXPR_SUBQUERY
    int      subkind;         // EXPR_SUBQUERY: SUBQ_*
    WindowSpec* win;          // EXPR_WINDOW
    int      param;           // > 0: segnaposto '?' numero param (1 = il primo nel testo) di un prepared
                              // statement; il nodo è un letterale (EXPR_NULL finché non riceve un valore)
} ASTExpr;

// libera un'espressione e tutti i suoi figli
void ast_expr_free(ASTExpr* e);
// copia profonda (NULL se e è NULL o manca memoria)
ASTExpr* ast_expr_clone(const ASTExpr* e);
// testo SQL dell'espressione (intestazioni delle colonne calcolate, EXPLAIN)
void ast_expr_format(const ASTExpr* e, char* out, size_t n);
// testo del frame di una funzione finestra ("ROWS BETWEEN 2 PRECEDING AND CURRENT ROW")
void frame_text(const WindowSpec* w, char* buf, size_t n);

enum { ALT_NONE, ALT_TYPE, ALT_SET_DEFAULT, ALT_DROP_DEFAULT, ALT_SET_NOT_NULL, ALT_DROP_NOT_NULL };

typedef struct {
    char     name[64];
    char     type[32];
    int      size;
    bool     nullable;
    bool     primary_key;
    bool     auto_increment;
    char     default_val[128];
    bool     has_default;            // DEFAULT presente (anche DEFAULT '')
    // dialetto MySQL
    int      scale;                  // DECIMAL(p, s): s (se has_scale)
    bool     has_scale;
    char     sqltype[48];            // tipo come scritto, in minuscolo ("bigint(20) unsigned")
    uint8_t  flags;                  // COLF_* (schema.h)
    char     comment[256];
} ColumnDef;

// KEY / INDEX / FULLTEXT KEY di CREATE TABLE (UNIQUE KEY diventa un vincolo)
typedef struct {
    char     name[64];
    int      ncols;
    char     cols[SV_MAX_KEY_COLS][64];
    bool     fulltext;
} TableKeyDef;
#define SV_MAX_KEY_DEFS SV_MAX_INDEXES       // KEY / INDEX in CREATE TABLE (lista dinamica, P3.12)

// vincolo di CREATE TABLE / ALTER TABLE ADD CONSTRAINT (di colonna o di tabella)
typedef struct {
    char     name[64];               // CONSTRAINT nome ("" = generato)
    int      kind;                   // CONS_PRIMARY, CONS_UNIQUE, CONS_FOREIGN, CONS_CHECK (schema.h)
    int      ncols;
    char     cols[SV_MAX_KEY_COLS][64];
    char     ref_table[64];
    int      nref;
    char     ref_cols[SV_MAX_KEY_COLS][64];        // vuoto: la chiave primaria della tabella riferita
    int      on_delete, on_update;   // FkAction
    char     check[512];             // testo della condizione
} ConstraintDef;

#define SV_MAX_CONS_DEFS SV_MAX_CONSTRAINTS  // vincoli in CREATE TABLE (lista dinamica, P3.12)

// funzione-tabella in FROM: JSON_TABLE(doc, 'percorso' COLUMNS (...)), GENERATE_SERIES(da, a [, passo])
enum { TFC_PATH, TFC_ORDINALITY, TFC_EXISTS, TFC_NESTED };
enum { TFE_NULL, TFE_DEFAULT, TFE_ERROR };     // ON EMPTY / ON ERROR
typedef struct {
    char      name[64];
    int       kind;            // TFC_*
    ColumnDef def;
    char      path[256];
    int       out;             // indice della colonna prodotta (non per TFC_NESTED)
    int       nend;            // TFC_NESTED: le sue colonne sono le voci fino a nend (esclusa)
    int       on_empty, on_error;            // TFE_*
    char      def_empty[256], def_error[256];
} TFCol;
typedef struct TableFunc {
    char      func[32];        // JSON_TABLE, GENERATE_SERIES
    ASTExpr*  args[4];
    int       nargs;
    TFCol*    cols;            // JSON_TABLE: COLUMNS (...), con le NESTED PATH appiattite
    int       ncols;
    int       nout;            // colonne prodotte
    char      colnames[8][64]; // alias(colonne)
    int       ncolnames;
} TableFunc;

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
    bool       lateral;        // LATERAL (subquery) / CROSS APPLY / OUTER APPLY: ricalcolata per ogni riga a sinistra
    TableFunc* tf;             // JOIN JSON_TABLE(...) / GENERATE_SERIES(...) (sempre laterale)
    char       table_db[64];   // JOIN db.tabella ("" = il database corrente)
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

#define SV_MAX_JOINS   60          // come MySQL: 61 tabelle in una SELECT
// ORDER BY / GROUP BY: liste dinamiche (P3.12: erano 8 e 16); il tetto ferma solo testi assurdi
#define SV_MAX_ORDER   4096
#define SV_MAX_GROUP   4096
#define SV_MAX_GROUP_SETS 32        // ROLLUP / CUBE / GROUPING SETS: insiemi a bit su 32 espressioni
// colonne in UPDATE ... SET / ON DUPLICATE KEY UPDATE / ON CONFLICT DO UPDATE: liste dinamiche (P3.12: erano 64
// con 1024 colonne per tabella); il tetto ferma solo un testo assurdo (una colonna può comparire più volte)
#define SV_MAX_SETS    (4 * SV_MAX_COLUMNS)
enum { INS_NORMAL, INS_IGNORE, INS_REPLACE };

typedef struct ASTNode {
    StmtType type;
    bool     if_exists;        // DROP ... IF EXISTS / CREATE ... IF NOT EXISTS
    char     qual_db[64];      // database dei nomi qualificati (shop.prodotto); "" = quello corrente
    bool     qual_mixed;       // anche tabelle senza database: valide solo se il corrente è qual_db
    // tutti i database nominati (shop.prodotto JOIN uni.esame): i permessi si controllano su ognuno
    char     qual_dbs[8][64];
    int      nqual_dbs;
    int      set_fk_checks;    // NODE_SET: FOREIGN_KEY_CHECKS = 0 / 1 (-1 = non nominato)
    int      set_sync;         // NODE_SET: synchronous = OFF 0 / NORMAL 1 / FULL 2 (-1 = non nominato, -2 = valore sbagliato)
    long     set_cache_mb;     // NODE_SET: cache_size = MB (-1 = non nominato, -2 = valore sbagliato)
    // ALTER TABLE t op1, op2, ...: le operazioni dopo la prima (ALTER, CREATE / DROP INDEX), eseguite in ordine
    struct ASTNode* chain;
    // INSERT / UPDATE / DELETE ... RETURNING espr [AS nome], ... | * (PostgreSQL)
    struct ASTExpr** returning;
    int      nret;
    // NODE_SET: assegnazioni di variabili di sessione (SET @a = 1, @b := 2)
    char   (*set_names)[64];
    struct ASTExpr** set_exprs;
    int      set_n;
    union {
        struct {
            ASTExpr**  columns;          // num_cols espressioni (lista dinamica dal P1.11, prima 64 al massimo)
            int        num_cols, cap_cols;
            char       table_name[64];
            char       table_alias[64];  // "" = nessun alias
            char       show_like[128];   // SHOW TABLES / DATABASES LIKE 'p'
            bool       has_show_like;
            ASTExpr*   where;
            ASTExpr**  order_exprs;    // num_order (cap_order allocate, come order_desc / order_nulls)
            bool*      order_desc;
            char*      order_nulls;    // 0 = come MySQL (NULL primi in ASC), 'F' = NULLS FIRST, 'L' = NULLS LAST
            int        num_order, cap_order;
            int        limit;
            int        offset;
            bool       has_limit;      // LIMIT presente (LIMIT 0 = nessuna riga)
            bool       distinct;
            bool       calc_found;     // SQL_CALC_FOUND_ROWS: FOUND_ROWS() = righe senza LIMIT
            // SELECT ... INTO v1, @v2: i valori dell'unica riga vanno nelle variabili (routine o sessione)
            char       into[16][64];
            int        num_into;
            ASTExpr**  group_exprs;    // num_group (cap_group allocate)
            int        num_group, cap_group;
            ASTExpr*   having;
            ASTJoin*   joins;          // lista dinamica (P3.12: erano 8; il piano ne accetta MAX_SOURCES - 1 = 60)
            int        num_joins, cap_joins;
            struct ASTNode* from_sub;  // FROM (SELECT ...) alias: tabella derivata
            TableFunc* from_tf;        // FROM JSON_TABLE(...) / GENERATE_SERIES(...) alias
            char       table_db[64];   // FROM db.tabella: il database della prima sorgente ("" = corrente)
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
            char**     columns;        // lista dinamica (P1.11)
            int        num_cols, cap_cols;
            ASTExpr**  values;         // num_rows * num_values espressioni (riga per riga)
            int        num_values;     // valori per riga
            int        num_rows;
            struct ASTNode* select;    // INSERT INTO t [(colonne)] SELECT ...
            int        mode;           // INS_* (INSERT IGNORE, REPLACE)
            char     (*dup_cols)[64];  // ON DUPLICATE KEY UPDATE col = espr, ... (num_dup, cap_dup allocate)
            ASTExpr**  dup_exprs;
            int        num_dup, cap_dup;
            char       row_alias[64];  // VALUES (...) AS nuovo: nuovo.col = il valore proposto
        } insert_stmt;
        struct {
            char       table_name[64];
            char     (*set_cols)[64];  // num_sets colonne (cap_sets allocate, come set_exprs / set_tables / set_texts)
            ASTExpr**  set_exprs;
            int        num_sets, cap_sets;
            ASTExpr*   where;
            // MySQL: UPDATE con più tabelle (JOIN), alias, ORDER BY / LIMIT: tutto come testo SQL
            char*      multi_from;            // "t1 a JOIN t2 b ON ..." (NULL = forma semplice)
            char*      multi_where;           // condizione (NULL = nessuna)
            char*      order_limit;           // "ORDER BY ... LIMIT n" (NULL = nessuno)
            char     (*set_tables)[64];       // a.col: "a" ("" = non qualificata)
            char**     set_texts;                // testo dell'espressione (multi-tabella / ORDER BY)
        } update_stmt;
        struct {
            char       table_name[64];
            ASTExpr*   where;
            // MySQL: DELETE a, b FROM ..., DELETE FROM t AS x ..., ORDER BY / LIMIT
            char*      multi_from;
            char*      multi_where;
            char*      order_limit;
            char       targets[8][64];        // tabelle (o alias) da cui si cancella
            int        num_targets;
        } delete_stmt;
        struct {
            char       db_name[64];
        } create_db;
        struct {
            char       db_name[64];
        } drop_db;
        struct {
            char       table_name[64];
            ColumnDef* columns;               // lista dinamica (P1.11: fino a SV_MAX_COLUMNS colonne)
            int        num_columns, cap_columns;
            ConstraintDef* cons;              // num_cons (ast_cons_add): prima 16 dentro ogni ASTNode (27 KB)
            int        num_cons, cap_cons;
            TableKeyDef* keys;                // num_keys (ast_key_add)
            int        num_keys, cap_keys;
            char       comment[256];          // COMMENT = '...'
            char       collation[40];         // [DEFAULT] COLLATE x / CHARSET x
            long long  auto_increment;        // AUTO_INCREMENT = n
            bool       temporary;             // CREATE TEMPORARY TABLE
            struct ASTNode* as_select;        // CREATE TABLE t AS SELECT ...
            char       like_table[64];        // CREATE TABLE t LIKE s
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
            // ALTER [COLUMN] nome TYPE t | SET DEFAULT v | DROP DEFAULT | SET NOT NULL | DROP NOT NULL
            // (MODIFY [COLUMN] nome tipo: come TYPE)
            char       alter_col[64];
            int        alter_kind;            // ALT_*
            ColumnDef  alter_def;             // tipo e dimensione (TYPE) o valore (SET DEFAULT)
            bool       noop;                  // l'operazione è nel nodo next (ADD INDEX / DROP INDEX)
        } alter_table;
        struct {
            char       table_name[64];
            bool       temporary;             // DROP TEMPORARY TABLE
            char       more[15][64];          // DROP TABLE a, b, c: le tabelle dopo la prima
            int        nmore;
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
            char*      definition;     // malloc'd (fino a SV_MAX_SQL_LEN): prima 64 KB dentro ogni ASTNode
            bool       replace;        // CREATE OR REPLACE VIEW
            bool       alter;          // ALTER VIEW: la vista deve esistere
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
            char*      body;           // malloc'd (fino a SV_MAX_SQL_LEN; anche il testo delle routine)
        } create_trigger;
        struct {
            char       name[64];
            bool       is_function;     // DROP FUNCTION (NODE_DROP_PROCEDURE)
        } drop_trigger;
        // CREATE SEQUENCE s [START WITH n] [INCREMENT BY k] [MINVALUE m] [MAXVALUE x] [CYCLE], ALTER ... RESTART, DROP
        struct {
            char       name[64];
            int        op;                  // 0 CREATE, 1 DROP, 2 ALTER
            long long  start, inc, minv, maxv, restart_with;
            bool       has_start, has_inc, has_min, has_max, cycle, has_cycle, restart, has_restart_with;
        } seq;
        // MERGE: le parti come testo SQL (eseguite come INSERT / UPDATE / DELETE)
        struct {
            char       target[64], talias[64], salias[64];
            char*      source;              // tabella o (subquery), con " AS alias[(colonne)]"
            char*      on;
            MergeWhen* when;                // clausole WHEN (P3.12: erano 8 dentro il nodo, 18 KB)
            int        nwhen, cap_when;
        } merge;
        struct {
            char       name[64];
            ASTExpr*   args[32];
            int        nargs;
        } call;
        struct {
            char       path[512];
        } backup;
        struct {
            char       op[12];         // CHECK, ANALYZE, OPTIMIZE, REPAIR
            char       tables[16][64];
            int        ntables;
        } maint;
        struct {
            void*      stmt;           // ASTNode* della SELECT da spiegare
            bool       analyze;        // EXPLAIN ANALYZE: esegue la query e mostra le righe reali
            bool       json;           // EXPLAIN FORMAT=JSON: una riga con il piano in JSON (vista del piano della GUI)
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
// prepared statement analizzati una volta (P2.2): fra begin ed end il parser accetta i '?' e ricorda ogni
// nodo che li rappresenta (anche le copie fatte durante l'analisi); end restituisce l'elenco (da liberare con
// free, i nodi appartengono all'albero). Fuori da begin / end un '?' è un errore di sintassi.
void parser_params_begin(void);
int  parser_params_end(ASTExpr*** nodes);
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
// Procedure e funzioni (linguaggio delle stored routine di MySQL / SQL/PSM) aggiungono:
//   [etichetta:] BEGIN ... END [etichetta]          blocco annidato
//   DECLARE v1 [, v2] tipo [DEFAULT espr]          variabili locali
//   DECLARE c CURSOR FOR select                     cursori: OPEN c / FETCH c INTO v, ... / CLOSE c
//   DECLARE {CONTINUE | EXIT} HANDLER FOR {NOT FOUND | SQLEXCEPTION | SQLWARNING | SQLSTATE 'x' | n} istruzione
//   DECLARE nome CONDITION FOR ...                  (accettata)
//   SET v = espr [, @x = espr, NEW.c = espr]       assegnazioni
//   SELECT ... INTO v1, v2 ...                      (nel nodo SELECT)
//   RETURN espr                                     (funzioni)
//   [e:] WHILE c DO ... END WHILE [e]  |  [e:] REPEAT ... UNTIL c END REPEAT [e]  |  [e:] LOOP ... END LOOP [e]
//   LEAVE e  |  ITERATE e
//   CASE [x] WHEN v THEN ... [ELSE ...] END CASE    (diventa una catena di IF)
typedef enum { TS_SQL, TS_SET, TS_SIGNAL, TS_IF,
               TS_DECLARE, TS_ASSIGN, TS_RETURN, TS_BLOCK, TS_WHILE, TS_REPEAT, TS_LOOP, TS_LEAVE, TS_ITERATE,
               TS_HANDLER, TS_CURSOR, TS_OPEN, TS_FETCH, TS_CLOSE, TS_NOOP } TrigStmtKind;

typedef struct TrigStmt {
    TrigStmtKind      kind;
    ASTNode*          sql;          // TS_SQL; TS_CURSOR: la SELECT
    char              col[64];      // TS_SET: colonna di NEW
    ASTExpr*          expr;         // TS_SET / TS_RETURN / DEFAULT di TS_DECLARE: valore; TS_SIGNAL: messaggio (NULL = RESIGNAL)
    ASTExpr*          cond;         // TS_IF / TS_WHILE / TS_REPEAT (UNTIL)
    struct TrigStmt** then_v;       // TS_IF: THEN; blocchi e cicli: corpo; TS_HANDLER: l'istruzione
    int               nthen;
    struct TrigStmt** else_v;       // ELSEIF diventa un IF dentro l'ELSE
    int               nelse;
    char              label[64];    // etichetta di blocco / ciclo; LEAVE / ITERATE: destinazione; cursore
    char            (*names)[64];   // TS_DECLARE: variabili; TS_ASSIGN / TS_FETCH: destinazioni
    int               nnames;
    struct ASTExpr**  exprs;        // TS_ASSIGN: un valore per destinazione
    ColumnDef*        vdef;         // TS_DECLARE: tipo
    bool              exit_handler; // TS_HANDLER: EXIT (altrimenti CONTINUE)
    char            (*conds)[16];   // TS_HANDLER: "NOTFOUND", "SQLEXCEPTION", "SQLWARNING", SQLSTATE o numero
    int               nconds;
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

// parametro di una routine: IN / OUT / INOUT nome tipo
enum { PARAM_IN, PARAM_OUT, PARAM_INOUT };
typedef struct {
    char      name[64];
    int       mode;
    ColumnDef def;
} RoutineParam;

// definizione completa (testo salvato nel catalogo: "(parametri) [RETURNS tipo] caratteristiche corpo")
typedef struct {
    bool         success;
    char         error[256];
    RoutineParam params[32];
    int          nparams;
    bool         is_function;
    ColumnDef    ret;               // RETURNS
    TrigProgram* prog;
} RoutineDef;

RoutineDef* parser_parse_routine(TokenList* tokens, bool is_function);
void        routine_def_free(RoutineDef* d);

TrigProgram* parser_parse_trigger(TokenList* tokens);
// corpo di una procedura dal token start: BEGIN ... END oppure una sola istruzione
TrigProgram* parser_parse_procedure(TokenList* tokens, int start);
void         trig_program_free(TrigProgram* p);

// liste dinamiche dei nodi (P1.11): false senza memoria o oltre SV_MAX_COLUMNS
bool ast_select_push(ASTNode* n, ASTExpr* e);
bool ast_insert_push_col(ASTNode* n, char* name);
ColumnDef* ast_create_new_col(ASTNode* n);          // azzerata in fondo, NULL se non c'è posto

// un vincolo / un indice in più (azzerato) in un nodo CREATE TABLE; NULL oltre SV_MAX_CONS_DEFS / SV_MAX_KEY_DEFS o
// senza memoria
ConstraintDef* ast_cons_add(ASTNode* n);
TableKeyDef*   ast_key_add(ASTNode* n);

#endif
