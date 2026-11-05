#include "executor.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>

static QueryResult* result_error_msg(const char* msg) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = false;
    snprintf(r->error, sizeof(r->error), "%s", msg);
    return r;
}

static QueryResult* result_ok_msg(int affected) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->rows_affected = affected;
    return r;
}

static void mi_invalidate_table(const char* db, const char* table);

static bool known_type(const char* t) {
    static const char* const T[] = { "INT", "INTEGER", "SMALLINT", "TINYINT", "BIGINT", "FLOAT", "DOUBLE", "REAL",
        "NUMERIC", "DECIMAL", "TIMESTAMP", "VARCHAR", "CHAR", "TEXT", "BOOL", "BOOLEAN", "DATE", "DATETIME", "BLOB" };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (!strcasecmp(t, T[i])) return true;
    return false;
}

static DataType parse_type(const char* type_name) {
    if (strcasecmp(type_name, "INT") == 0 || strcasecmp(type_name, "INTEGER") == 0 ||
        strcasecmp(type_name, "SMALLINT") == 0 || strcasecmp(type_name, "TINYINT") == 0) return SV_TYPE_INT;
    if (strcasecmp(type_name, "BIGINT") == 0) return SV_TYPE_BIGINT;
    if (strcasecmp(type_name, "FLOAT") == 0 || strcasecmp(type_name, "DOUBLE") == 0 ||
        strcasecmp(type_name, "REAL") == 0) return SV_TYPE_FLOAT;
    if (strcasecmp(type_name, "NUMERIC") == 0) return SV_TYPE_DECIMAL;
    if (strcasecmp(type_name, "TIMESTAMP") == 0) return SV_TYPE_DATETIME;
    if (strcasecmp(type_name, "VARCHAR") == 0) return SV_TYPE_VARCHAR;
    if (strcasecmp(type_name, "CHAR") == 0) return SV_TYPE_CHAR;
    if (strcasecmp(type_name, "TEXT") == 0) return SV_TYPE_TEXT;
    if (strcasecmp(type_name, "BOOL") == 0 || strcasecmp(type_name, "BOOLEAN") == 0) return SV_TYPE_BOOL;
    if (strcasecmp(type_name, "DATE") == 0) return SV_TYPE_DATE;
    if (strcasecmp(type_name, "DATETIME") == 0) return SV_TYPE_DATETIME;
    if (strcasecmp(type_name, "BLOB") == 0) return SV_TYPE_BLOB;
    if (strcasecmp(type_name, "DECIMAL") == 0) return SV_TYPE_DECIMAL;
    return SV_TYPE_VARCHAR;
}

// LIKE con % (qualsiasi sequenza) e _ (un carattere), senza distinzione maiuscole/minuscole
static bool like_match(const char* s, const char* p) {
    while (*p) {
        if (*p == '%') {
            while (*p == '%') p++;
            if (!*p) return true;
            for (; *s; s++) if (like_match(s, p)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p != '_' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
        s++; p++;
    }
    return *s == '\0';
}

static QueryResult* unknown_column_error(const char* col, const char* clause) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Unknown column '%.64s' in %s", col, clause);
    return result_error_msg(msg);
}

// ---- Executors ----
static QueryResult* exec_create_db(ExecCtx* ctx, ASTNode* n) {
    if (catalog_add_db(ctx->catalog, n->create_db.db_name) != SV_OK)
        return result_error_msg("Failed to create database (may already exist)");
    return result_ok_msg(0);
}

static QueryResult* exec_drop_db(ExecCtx* ctx, ASTNode* n) {
    executor_invalidate_indexes();
    if (catalog_drop_db(ctx->catalog, n->drop_db.db_name) != SV_OK)
        return result_error_msg("Database not found");
    return result_ok_msg(0);
}

static QueryResult* exec_create_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    char msg[200];
    if (n->create_table.num_columns == 0) return result_error_msg("A table needs at least one column");
    if (n->create_table.num_columns > SV_MAX_COLUMNS) return result_error_msg("Too many columns (max 64)");
    int pks = 0;
    for (int i = 0; i < n->create_table.num_columns; i++) {
        ColumnDef* cd = &n->create_table.columns[i];
        if (!known_type(cd->type)) {
            snprintf(msg, sizeof(msg), "Unknown type '%.32s' for column '%.64s'", cd->type, cd->name);
            return result_error_msg(msg);
        }
        if (cd->primary_key) pks++;
        for (int k = 0; k < i; k++)
            if (!strcasecmp(n->create_table.columns[k].name, cd->name)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is defined twice", cd->name);
                return result_error_msg(msg);
            }
    }
    if (pks > 1) return result_error_msg("A table can have only one PRIMARY KEY column");
    if (catalog_get_table(ctx->catalog, db->name, n->create_table.table_name) ||
        catalog_get_view(ctx->catalog, db->name, n->create_table.table_name)) {
        snprintf(msg, sizeof(msg), "Table or view '%.64s' already exists", n->create_table.table_name);
        return result_error_msg(msg);
    }

    TableSchema ts;
    memset(&ts, 0, sizeof(ts));
    snprintf(ts.name, sizeof(ts.name), "%s", n->create_table.table_name);
    ts.num_rows = 0;
    ts.num_pages = 0;

    for (int i = 0; i < n->create_table.num_columns && i < SV_MAX_COLUMNS; i++) {
        ColumnDef* cd = &n->create_table.columns[i];
        Column* col = &ts.columns[ts.num_columns++];
        memset(col, 0, sizeof(Column));
        snprintf(col->name, sizeof(col->name), "%s", cd->name);
        col->type = parse_type(cd->type);
        col->size = cd->size;
        col->nullable = cd->nullable;
        col->primary_key = cd->primary_key;
        col->auto_increment = cd->auto_increment;
        if (cd->default_val[0])
            snprintf(col->default_val, sizeof(col->default_val), "%s", cd->default_val);
    }

    if (catalog_add_table(ctx->catalog, db->name, &ts) != SV_OK)
        return result_error_msg("Failed to create table");

    TableSchema* saved = catalog_get_table(ctx->catalog, db->name, ts.name);
    if (saved) {
        uint32_t pid;
        if (bp_new_page(ctx->bp, &pid) == SV_OK) {
            saved->root_page_id = pid;
            saved->num_pages = 1;
            bp_unpin(ctx->bp, pid, false);
        }
    }
    return result_ok_msg(0);
}

static QueryResult* exec_drop_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    mi_invalidate_table(db->name, n->drop_table.table_name);
    if (catalog_drop_table(ctx->catalog, db->name, n->drop_table.table_name) != SV_OK)
        return result_error_msg("Table not found");
    return result_ok_msg(0);
}

static QueryResult* exec_alter_table(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->alter_table.table_name);
    if (!ts) return result_error_msg("Table not found");
    // RENAME TO
    if (n->alter_table.new_name[0]) {
        const char* nn = n->alter_table.new_name;
        if (strcasecmp(nn, ts->name) &&
            (catalog_get_table(ctx->catalog, db->name, nn) || catalog_get_view(ctx->catalog, db->name, nn)))
            return result_error_msg("A table or view with the new name already exists");
        mi_invalidate_table(db->name, ts->name);
        snprintf(ts->name, sizeof(ts->name), "%s", nn);
    }
    return result_ok_msg(0);
}

static int utf8_len(const char* s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static bool valid_date(const char* s, const char** rest) {
    int y, m, d, k = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &y, &m, &d, &k) != 3 || k != 10) return false;
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return false;
    int md = DAYS[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    if (d > md) return false;
    if (rest) *rest = s + 10;
    return true;
}

// NULL se il testo è un valore valido per la colonna, altrimenti scrive il motivo in msg
static const char* check_value(const Column* c, const char* v, char* msg, size_t n) {
    const char* p = v;
    while (isspace((unsigned char)*p)) p++;
    switch (c->type) {
        case SV_TYPE_INT: case SV_TYPE_BIGINT: {
            char* end = NULL;
            errno = 0;
            long long x = strtoll(p, &end, 10);
            bool range = errno == ERANGE;
            if (end != p && (*end == '.' || *end == 'e' || *end == 'E')) {   // 3.0 va bene, 3.5 no
                double d = strtod(p, &end);
                if (d != (double)(long long)d) { snprintf(msg, n, "Column '%s' expects an integer, got '%.40s'", c->name, v); return msg; }
                x = (long long)d;
            }
            while (end && isspace((unsigned char)*end)) end++;
            if (end == p || !end || *end) { snprintf(msg, n, "Column '%s' expects an integer, got '%.40s'", c->name, v); return msg; }
            if (range || (c->type == SV_TYPE_INT && (x < INT_MIN || x > INT_MAX))) {
                snprintf(msg, n, "Value %.40s is out of range for column '%s' (%s)", v, c->name, datatype_name(c->type));
                return msg;
            }
            return NULL;
        }
        case SV_TYPE_FLOAT: case SV_TYPE_DECIMAL: {
            char* end = NULL;
            double d = strtod(p, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (end == p || !end || *end || d != d) { snprintf(msg, n, "Column '%s' expects a number, got '%.40s'", c->name, v); return msg; }
            return NULL;
        }
        case SV_TYPE_BOOL:
            if (!strcmp(p, "0") || !strcmp(p, "1") || !strcasecmp(p, "true") || !strcasecmp(p, "false")) return NULL;
            snprintf(msg, n, "Column '%s' expects TRUE, FALSE, 1 or 0, got '%.40s'", c->name, v);
            return msg;
        case SV_TYPE_VARCHAR: case SV_TYPE_CHAR:
            if (c->size > 0 && utf8_len(v) > c->size) {
                snprintf(msg, n, "Value too long for column '%s' (%d characters, maximum %d)", c->name, utf8_len(v), c->size);
                return msg;
            }
            return NULL;
        case SV_TYPE_DATE: {
            const char* rest = NULL;
            if (valid_date(p, &rest) && !*rest) return NULL;
            snprintf(msg, n, "Column '%s' expects a valid date YYYY-MM-DD, got '%.40s'", c->name, v);
            return msg;
        }
        case SV_TYPE_DATETIME: {
            const char* rest = NULL;
            int hh = 0, mm = 0, ss = 0, k = 0;
            if (valid_date(p, &rest) &&
                (!*rest || ((sscanf(rest, " %2d:%2d:%2d%n", &hh, &mm, &ss, &k) == 3 ||
                             (ss = 0, sscanf(rest, " %2d:%2d%n", &hh, &mm, &k) == 2)) && !rest[k] &&
                            hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60)))
                return NULL;
            snprintf(msg, n, "Column '%s' expects a date-time YYYY-MM-DD HH:MM:SS, got '%.40s'", c->name, v);
            return msg;
        }
        default:
            return NULL;
    }
}

static CellType cell_type_of(DataType t) {
    switch (t) {
        case SV_TYPE_INT: case SV_TYPE_BIGINT:     return CELL_TYPE_INT;
        case SV_TYPE_FLOAT: case SV_TYPE_DECIMAL:  return CELL_TYPE_FLOAT;
        case SV_TYPE_BOOL:                         return CELL_TYPE_BOOL;
        case SV_TYPE_DATE: case SV_TYPE_DATETIME:  return CELL_TYPE_DATE;
        case SV_TYPE_BLOB:                         return CELL_TYPE_BLOB;
        default:                                   return CELL_TYPE_STRING;
    }
}

// ═════════════════════════════════════════════════════════════════
//  Valori calcolati
// ═════════════════════════════════════════════════════════════════

#define EV_STR_MAX 511   // come Cell.value: un testo più lungo non entrerebbe nel risultato

typedef struct {
    bool        null;
    bool        is_num;
    bool        is_int;
    int64_t     inum;      // valore intero esatto (se is_int)
    double      num;
    const char* str;       // testo (stringhe e numeri formattati); punta a buf o a memoria della tupla
    char        buf[EV_STR_MAX + 1];
} EvalVal;

static void ev_null(EvalVal* v) { v->null = true; v->is_num = v->is_int = false; v->str = ""; }

static void ev_int(EvalVal* v, int64_t x) {
    v->null = false; v->is_num = v->is_int = true; v->inum = x; v->num = (double)x;
    snprintf(v->buf, sizeof(v->buf), "%lld", (long long)x); v->str = v->buf;
}

static void ev_num(EvalVal* v, double x) {
    if (x != x || x - x != 0) { ev_null(v); return; }        // NaN / infinito: NULL
    v->null = false; v->is_num = true; v->is_int = false; v->num = x;
    snprintf(v->buf, sizeof(v->buf), "%.15g", x); v->str = v->buf;
}

// testo: copia nel buffer (false se più lungo di EV_STR_MAX)
static bool ev_text(EvalVal* v, const char* s, size_t n) {
    v->null = false; v->is_num = v->is_int = false;
    if (n > EV_STR_MAX) return false;
    memcpy(v->buf, s, n);
    v->buf[n] = '\0';
    v->str = v->buf;
    return true;
}

// copia sicura (il puntatore str può riferirsi al buffer interno)
static void ev_copy(EvalVal* dst, const EvalVal* src) {
    bool own = src->str == src->buf;
    *dst = *src;
    if (own) dst->str = dst->buf;
}

// testo numerico per intero? ("12", " -3 ") -> valore
static bool text_is_number(const char* s, double* out) {
    char* end = NULL;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return false;
    double d = strtod(s, &end);
    if (end == s) return false;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    *out = d;
    return true;
}

// valore numerico di qualunque cosa (testo: prefisso numerico come MySQL, altrimenti 0)
static double ev_as_num(const EvalVal* v) {
    if (v->is_num) return v->num;
    return strtod(v->str, NULL);
}

// confronto: numeri fra loro, numero contro testo numerico, altrimenti testo
static int ev_cmp(const EvalVal* a, const EvalVal* b) {
    if (a->is_num && b->is_num) {
        if (a->is_int && b->is_int) return a->inum < b->inum ? -1 : a->inum > b->inum;
        return a->num < b->num ? -1 : a->num > b->num;
    }
    if (a->is_num || b->is_num) {
        double d;
        const EvalVal* txt = a->is_num ? b : a;
        if (text_is_number(txt->str, &d)) {
            double x = a->is_num ? a->num : d, y = a->is_num ? d : b->num;
            return x < y ? -1 : x > y;
        }
    }
    int c = strcmp(a->str, b->str);
    return c < 0 ? -1 : c > 0;
}

// -1 = NULL (sconosciuto), 0 = falso, 1 = vero
static int ev_truth(const EvalVal* v) {
    if (v->null) return -1;
    return ev_as_num(v) != 0 ? 1 : 0;
}

// valore di una colonna della tupla
static void ev_from_value(EvalVal* out, const Value* v, DataType type) {
    if (v->is_null) { ev_null(out); return; }
    switch (type) {
        case SV_TYPE_INT:    ev_int(out, v->int_val); return;
        case SV_TYPE_BIGINT: ev_int(out, v->bigint_val); return;
        case SV_TYPE_BOOL:   ev_int(out, v->bool_val ? 1 : 0); return;
        case SV_TYPE_FLOAT:  ev_num(out, v->float_val); return;
        case SV_TYPE_DECIMAL: {   // salvato come testo, si comporta da numero
            out->null = false;
            out->str = v->str_val ? v->str_val : "";
            double d;
            out->is_int = false;
            out->is_num = text_is_number(out->str, &d);
            out->num = out->is_num ? d : 0;
            return;
        }
        default:
            out->null = false; out->is_num = out->is_int = false;
            out->str = v->str_val ? v->str_val : "";
            return;
    }
}

// ═════════════════════════════════════════════════════════════════
//  Mappa testo -> intero (indici, gruppi, DISTINCT)
// ═════════════════════════════════════════════════════════════════

typedef struct { char* key; int val; } SMSlot;
typedef struct { SMSlot* s; size_t cap, n; } StrMap;

static uint64_t hash_bytes(const char* k) {
    uint64_t h = 1469598103934665603ULL;          // FNV-1a
    for (; *k; k++) { h ^= (unsigned char)*k; h *= 1099511628211ULL; }
    return h;
}

static void sm_free(StrMap* m) {
    for (size_t i = 0; i < m->cap; i++) free(m->s[i].key);
    free(m->s);
    m->s = NULL; m->cap = m->n = 0;
}

static bool sm_grow(StrMap* m) {
    size_t nc = m->cap ? m->cap * 2 : 64;
    SMSlot* ns = calloc(nc, sizeof(SMSlot));
    if (!ns) return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->s[i].key) continue;
        size_t j = hash_bytes(m->s[i].key) & (nc - 1);
        while (ns[j].key) j = (j + 1) & (nc - 1);
        ns[j] = m->s[i];
    }
    free(m->s);
    m->s = ns;
    m->cap = nc;
    return true;
}

// slot della chiave; con create la inserisce (val = -1) se manca. NULL se manca memoria.
static SMSlot* sm_slot(StrMap* m, const char* key, bool create) {
    if (create && (m->n + 1) * 10 >= m->cap * 7 && !sm_grow(m)) return NULL;
    if (!m->cap) return NULL;
    size_t j = hash_bytes(key) & (m->cap - 1);
    while (m->s[j].key) {
        if (strcmp(m->s[j].key, key) == 0) return &m->s[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!create) return NULL;
    m->s[j].key = strdup(key);
    if (!m->s[j].key) return NULL;
    m->s[j].val = -1;
    m->n++;
    return &m->s[j];
}

// ═════════════════════════════════════════════════════════════════
//  Indici in memoria
//  Chiave primaria, AUTO_INCREMENT e colonne con CREATE INDEX hanno un indice
//  valore -> righe, costruito dalle righe della tabella al primo uso e mantenuto da
//  INSERT/UPDATE/DELETE. Non vive su disco: non può mai essere disallineato dai dati
//  (dopo un arresto anomalo o un ROLLBACK viene semplicemente ricostruito).
// ═════════════════════════════════════════════════════════════════

typedef struct { RID* r; int n, cap; } RidList;

typedef struct MemIndex {
    char      db[SV_MAX_NAME_LEN], table[SV_MAX_NAME_LEN], col[SV_MAX_NAME_LEN];
    StrMap    map;          // chiave -> posizione in lists
    RidList*  lists;
    int       nlists, caplists;
    bool      has_max;      // massimo intero (AUTO_INCREMENT)
    int64_t   max_int;
    struct MemIndex* next;
} MemIndex;

static MemIndex* g_mem_indexes;

static void mi_free(MemIndex* ix) {
    sm_free(&ix->map);
    for (int i = 0; i < ix->nlists; i++) free(ix->lists[i].r);
    free(ix->lists);
    free(ix);
}

void executor_invalidate_indexes(void) {
    while (g_mem_indexes) {
        MemIndex* nx = g_mem_indexes->next;
        mi_free(g_mem_indexes);
        g_mem_indexes = nx;
    }
}

static void mi_invalidate_table(const char* db, const char* table) {
    MemIndex** pp = &g_mem_indexes;
    while (*pp) {
        MemIndex* ix = *pp;
        if (!strcasecmp(ix->db, db) && !strcasecmp(ix->table, table)) { *pp = ix->next; mi_free(ix); }
        else pp = &ix->next;
    }
}

// chiave canonica di un valore (NULL non viene indicizzato)
static void mi_key(const Value* v, DataType t, char* buf, size_t n) {
    value_to_string((Value*)v, t, buf, n);
}

static bool mi_add(MemIndex* ix, const Value* v, DataType t, RID rid) {
    if (v->is_null) return true;
    char key[EV_STR_MAX + 1];
    mi_key(v, t, key, sizeof(key));
    SMSlot* s = sm_slot(&ix->map, key, true);
    if (!s) return false;
    if (s->val < 0) {
        if (ix->nlists >= ix->caplists) {
            int nc = ix->caplists ? ix->caplists * 2 : 64;
            RidList* nl = realloc(ix->lists, sizeof(RidList) * (size_t)nc);
            if (!nl) return false;
            ix->lists = nl;
            ix->caplists = nc;
        }
        ix->lists[ix->nlists] = (RidList){ NULL, 0, 0 };
        s->val = ix->nlists++;
    }
    RidList* l = &ix->lists[s->val];
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 1;
        RID* nr = realloc(l->r, sizeof(RID) * (size_t)nc);
        if (!nr) return false;
        l->r = nr;
        l->cap = nc;
    }
    l->r[l->n++] = rid;
    if (t == SV_TYPE_INT || t == SV_TYPE_BIGINT) {
        int64_t x = t == SV_TYPE_INT ? v->int_val : v->bigint_val;
        if (!ix->has_max || x > ix->max_int) ix->max_int = x;
        ix->has_max = true;
    }
    return true;
}

static void mi_remove(MemIndex* ix, const Value* v, DataType t, RID rid) {
    if (v->is_null) return;
    char key[EV_STR_MAX + 1];
    mi_key(v, t, key, sizeof(key));
    SMSlot* s = sm_slot(&ix->map, key, false);
    if (!s || s->val < 0) return;
    RidList* l = &ix->lists[s->val];
    for (int i = 0; i < l->n; i++)
        if (l->r[i].page_id == rid.page_id && l->r[i].slot_id == rid.slot_id) { l->r[i] = l->r[--l->n]; return; }
}

static const RidList* mi_lookup(MemIndex* ix, const char* key) {
    SMSlot* s = sm_slot(&ix->map, key, false);
    return (s && s->val >= 0) ? &ix->lists[s->val] : NULL;
}

// la colonna ha un indice (chiave primaria, AUTO_INCREMENT o CREATE INDEX)?
static bool col_is_indexed(const TableSchema* ts, int ci) {
    if (ts->columns[ci].primary_key || ts->columns[ci].auto_increment) return true;
    for (int i = 0; i < ts->num_indexes; i++)
        if (!strcasecmp(ts->indexes[i].col_name, ts->columns[ci].name)) return true;
    return false;
}

// indice della colonna, costruito se manca. NULL se la colonna non è indicizzata o la
// costruzione è fallita (memoria, scansione annullata).
static MemIndex* mi_get(ExecCtx* ctx, const char* db, TableSchema* ts, int ci) {
    if (ci < 0 || ci >= ts->num_columns || !col_is_indexed(ts, ci)) return NULL;
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next)
        if (!strcasecmp(ix->db, db) && !strcasecmp(ix->table, ts->name) && !strcasecmp(ix->col, ts->columns[ci].name))
            return ix;
    MemIndex* ix = calloc(1, sizeof(MemIndex));
    if (!ix) return NULL;
    snprintf(ix->db, sizeof(ix->db), "%s", db);
    snprintf(ix->table, sizeof(ix->table), "%s", ts->name);
    snprintf(ix->col, sizeof(ix->col), "%s", ts->columns[ci].name);
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    HeapScan* scan = hf ? heap_scan_open(hf, ts) : NULL;
    bool ok = scan != NULL;
    Tuple* t;
    RID rid;
    while (ok && (t = heap_scan_next(scan, &rid)) != NULL) {
        if (ci < t->num_values) ok = mi_add(ix, &t->values[ci], ts->columns[ci].type, rid);
        tuple_free(t);
    }
    if (scan) heap_scan_close(scan);
    free(hf);
    if (!ok || atomic_load(&g_sv_scan_cancel)) { mi_free(ix); return NULL; }   // incompleto: scartato
    ix->next = g_mem_indexes;
    g_mem_indexes = ix;
    return ix;
}

// aggiornamento degli indici già costruiti della tabella
static void mi_on_insert(const char* db, TableSchema* ts, const Tuple* t, RID rid) {
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name)) continue;
        int ci = catalog_get_col_index(ts, ix->col);
        if (ci >= 0 && ci < t->num_values && !mi_add(ix, &t->values[ci], ts->columns[ci].type, rid)) {
            mi_invalidate_table(db, ts->name);   // memoria esaurita: meglio ricostruire che usarlo incompleto
            return;
        }
    }
}

static void mi_on_delete(const char* db, TableSchema* ts, const Tuple* t, RID rid) {
    for (MemIndex* ix = g_mem_indexes; ix; ix = ix->next) {
        if (strcasecmp(ix->db, db) || strcasecmp(ix->table, ts->name)) continue;
        int ci = catalog_get_col_index(ts, ix->col);
        if (ci >= 0 && ci < t->num_values) mi_remove(ix, &t->values[ci], ts->columns[ci].type, rid);
    }
}

// ═════════════════════════════════════════════════════════════════
//  Sorgenti di righe (FROM / JOIN): tabelle o viste
// ═════════════════════════════════════════════════════════════════

#define MAX_SOURCES 9            // FROM + 8 JOIN

typedef struct {
    char         name[SV_MAX_NAME_LEN];    // nome con cui la si qualifica (alias o nome)
    char         table[SV_MAX_NAME_LEN];   // nome reale
    TableSchema* ts;                       // schema (vista: schema costruito dal risultato)
    TableSchema* own_ts;                   // da liberare (viste)
    Tuple**      rows;
    RID*         rids;
    int          n;
    bool         is_view;
    bool         loaded;
} Source;

static void source_free(Source* s) {
    for (int i = 0; i < s->n; i++) tuple_free(s->rows[i]);
    free(s->rows);
    free(s->rids);
    free(s->own_ts);
    memset(s, 0, sizeof(*s));
}

static bool source_push(Source* s, Tuple* t, RID rid, int* cap) {
    if (s->n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        Tuple** nr = realloc(s->rows, sizeof(Tuple*) * (size_t)nc);
        if (!nr) return false;
        s->rows = nr;
        RID* nd = realloc(s->rids, sizeof(RID) * (size_t)nc);
        if (!nd) return false;
        s->rids = nd;
        *cap = nc;
    }
    s->rows[s->n] = t;
    s->rids[s->n] = rid;
    s->n++;
    return true;
}

// ═════════════════════════════════════════════════════════════════
//  Contesto di valutazione
// ═════════════════════════════════════════════════════════════════

typedef struct {
    Source*   src;
    int       nsrc;
    Tuple**   cur;              // riga corrente di ogni sorgente (NULL = riga mancante di un outer join)
    Tuple***  grp;              // righe del gruppo corrente (aggregati); NULL fuori dai gruppi
    int       ngrp;
    ASTExpr** sel;              // espressioni della SELECT (alias in HAVING / ORDER BY)
    int       nsel;
    char      err[256];         // primo errore di valutazione
} EvalCtx;

static void ctx_error(EvalCtx* c, const char* fmt, const char* a) {
    if (c->err[0]) return;
    snprintf(c->err, sizeof(c->err), fmt, a);
}

// ── binding: risolve le colonne e controlla funzioni e aggregati prima di eseguire ──

typedef enum { BIND_ROW, BIND_AGG_OK } BindMode;   // BIND_AGG_OK: SELECT, HAVING, ORDER BY

typedef struct {
    Source*   src;
    int       nsrc;             // sorgenti visibili (le condizioni ON vedono solo quelle già unite)
    ASTExpr** sel;              // alias utilizzabili (HAVING / ORDER BY / GROUP BY)
    int       nsel;
    BindMode  mode;
    bool      in_agg;
    bool      saw_agg;
    const char* clause;
    char      err[256];
} BindCtx;

static void bind_error(BindCtx* b, const char* msg) {
    if (!b->err[0]) snprintf(b->err, sizeof(b->err), "%s", msg);
}

typedef struct { const char* name; int min_args, max_args; } FuncDef;
static const FuncDef FUNCS[] = {
    { "UPPER", 1, 1 }, { "UCASE", 1, 1 }, { "LOWER", 1, 1 }, { "LCASE", 1, 1 },
    { "LENGTH", 1, 1 }, { "CHAR_LENGTH", 1, 1 }, { "CHARACTER_LENGTH", 1, 1 },
    { "TRIM", 1, 1 }, { "LTRIM", 1, 1 }, { "RTRIM", 1, 1 },
    { "CONCAT", 1, 32 }, { "CONCAT_WS", 2, 32 }, { "SUBSTRING", 2, 3 }, { "SUBSTR", 2, 3 },
    { "LEFT", 2, 2 }, { "RIGHT", 2, 2 }, { "REPLACE", 3, 3 }, { "REVERSE", 1, 1 },
    { "ABS", 1, 1 }, { "ROUND", 1, 2 }, { "FLOOR", 1, 1 }, { "CEIL", 1, 1 }, { "CEILING", 1, 1 },
    { "MOD", 2, 2 }, { "POWER", 2, 2 }, { "POW", 2, 2 }, { "SQRT", 1, 1 }, { "SIGN", 1, 1 },
    { "COALESCE", 1, 32 }, { "IFNULL", 2, 2 }, { "NULLIF", 2, 2 }, { "IF", 3, 3 },
    { "NOW", 0, 0 }, { "CURRENT_TIMESTAMP", 0, 0 }, { "CURDATE", 0, 0 }, { "CURRENT_DATE", 0, 0 },
    { "YEAR", 1, 1 }, { "MONTH", 1, 1 }, { "DAY", 1, 1 },
};

static const FuncDef* find_func(const char* name) {
    for (size_t i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); i++)
        if (!strcmp(FUNCS[i].name, name)) return &FUNCS[i];
    return NULL;
}

// cerca la colonna nelle sorgenti visibili; 0 = non trovata, -1 = ambigua
static int find_column(BindCtx* b, const char* qual, const char* col, int* out_src, int* out_col) {
    int found = 0;
    for (int s = 0; s < b->nsrc; s++) {
        Source* src = &b->src[s];
        if (qual[0] && strcasecmp(qual, src->name) && strcasecmp(qual, src->table)) continue;
        for (int c = 0; c < src->ts->num_columns; c++) {
            if (strcasecmp(src->ts->columns[c].name, col)) continue;
            if (found) return -1;
            found = 1;
            *out_src = s;
            *out_col = c;
        }
    }
    return found;
}

static bool qualifier_exists(BindCtx* b, const char* qual) {
    for (int s = 0; s < b->nsrc; s++)
        if (!strcasecmp(qual, b->src[s].name) || !strcasecmp(qual, b->src[s].table)) return true;
    return false;
}

static void bind_expr(ASTExpr* e, BindCtx* b) {
    if (!e || b->err[0]) return;
    char msg[256];
    switch (e->type) {
        case EXPR_COLUMN: {
            e->bound = 0;
            int s = 0, c = 0;
            int r = find_column(b, e->table_name, e->col_name, &s, &c);
            if (r == 1) { e->bound = s * 1024 + c + 1; return; }
            if (r < 0) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is ambiguous: qualify it with the table name", e->col_name);
                bind_error(b, msg);
                return;
            }
            // alias della SELECT (HAVING, ORDER BY, GROUP BY)
            if (!e->table_name[0])
                for (int k = 0; k < b->nsel; k++)
                    if (b->sel[k]->alias[0] && !strcasecmp(b->sel[k]->alias, e->col_name)) { e->bound = -(k + 1); return; }
            if (e->table_name[0] && !qualifier_exists(b, e->table_name))
                snprintf(msg, sizeof(msg), "Unknown table '%.64s' in %s", e->table_name, b->clause);
            else
                snprintf(msg, sizeof(msg), "Unknown column '%.64s' in %s", e->col_name, b->clause);
            bind_error(b, msg);
            return;
        }
        case EXPR_AGG:
            if (b->mode != BIND_AGG_OK) {
                snprintf(msg, sizeof(msg), "Aggregate functions are not allowed in %s", b->clause);
                bind_error(b, msg);
                return;
            }
            if (b->in_agg) { bind_error(b, "Aggregate functions cannot be nested"); return; }
            b->saw_agg = true;
            b->in_agg = true;
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            b->in_agg = false;
            return;
        case EXPR_FUNC: {
            const FuncDef* f = find_func(e->func);
            if (!f) {
                snprintf(msg, sizeof(msg), "Unknown function '%.32s'", e->func);
                bind_error(b, msg);
                return;
            }
            if (e->nargs < f->min_args || e->nargs > f->max_args) {
                snprintf(msg, sizeof(msg), "Wrong number of arguments for %.32s", e->func);
                bind_error(b, msg);
                return;
            }
            for (int i = 0; i < e->nargs; i++) bind_expr(e->args[i], b);
            return;
        }
        case EXPR_STAR:
            bind_error(b, "* is only allowed in the SELECT list and in COUNT(*)");
            return;
        default:
            bind_expr(e->left, b);
            bind_expr(e->right, b);
            return;
    }
}

static bool expr_has_agg(const ASTExpr* e) {
    if (!e) return false;
    if (e->type == EXPR_AGG) return true;
    for (int i = 0; i < e->nargs; i++) if (expr_has_agg(e->args[i])) return true;
    return expr_has_agg(e->left) || expr_has_agg(e->right);
}

// ── valutazione ──

static void eval_expr(ASTExpr* e, EvalCtx* c, EvalVal* out);

static bool like_match(const char* s, const char* p);

// testo di un valore (numeri formattati); per i NULL: ""
static const char* ev_str(const EvalVal* v) { return v->null ? "" : v->str; }

static int utf8_chars(const char* s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

// byte di inizio del carattere numero ci (0-based) nel testo s
static const char* utf8_at(const char* s, int ci) {
    while (*s && ci > 0) { s++; while (((unsigned char)*s & 0xC0) == 0x80) s++; ci--; }
    return s;
}

static void ev_set_text(EvalCtx* c, EvalVal* out, const char* s, size_t n) {
    if (!ev_text(out, s, n)) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); }
}

static void now_text(char* buf, size_t n, bool date_only) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, n, date_only ? "%Y-%m-%d" : "%Y-%m-%d %H:%M:%S", &tmv);
}

static void eval_func(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    const char* f = e->func;
    EvalVal a[3];
    int na = e->nargs < 3 ? e->nargs : 3;

    // funzioni che non valutano tutti gli argomenti subito
    if (!strcmp(f, "COALESCE")) {
        for (int i = 0; i < e->nargs; i++) {
            eval_expr(e->args[i], c, out);
            if (!out->null) return;
        }
        ev_null(out);
        return;
    }
    if (!strcmp(f, "IFNULL")) {
        eval_expr(e->args[0], c, out);
        if (out->null) eval_expr(e->args[1], c, out);
        return;
    }
    if (!strcmp(f, "IF")) {
        EvalVal cond;
        eval_expr(e->args[0], c, &cond);
        eval_expr(e->args[ev_truth(&cond) == 1 ? 1 : 2], c, out);
        return;
    }
    if (!strcmp(f, "CONCAT") || !strcmp(f, "CONCAT_WS")) {
        bool ws = f[6] == '_';
        char buf[EV_STR_MAX + 2];
        size_t len = 0;
        EvalVal sep, v;
        if (ws) { eval_expr(e->args[0], c, &sep); if (sep.null) { ev_null(out); return; } }
        bool first = true;
        for (int i = ws ? 1 : 0; i < e->nargs; i++) {
            eval_expr(e->args[i], c, &v);
            if (v.null) { if (ws) continue; ev_null(out); return; }   // CONCAT con un NULL = NULL
            const char* parts[2] = { ws && !first ? sep.str : "", v.str };
            for (int p = 0; p < 2; p++) {
                size_t l = strlen(parts[p]);
                if (len + l > EV_STR_MAX) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); return; }
                memcpy(buf + len, parts[p], l);
                len += l;
            }
            first = false;
        }
        ev_set_text(c, out, buf, len);
        return;
    }
    if (!strcmp(f, "NOW") || !strcmp(f, "CURRENT_TIMESTAMP") || !strcmp(f, "CURDATE") || !strcmp(f, "CURRENT_DATE")) {
        char buf[32];
        now_text(buf, sizeof(buf), !strcmp(f, "CURDATE") || !strcmp(f, "CURRENT_DATE"));
        ev_set_text(c, out, buf, strlen(buf));
        return;
    }

    for (int i = 0; i < na; i++) {
        eval_expr(e->args[i], c, &a[i]);
        if (a[i].null && strcmp(f, "NULLIF")) { ev_null(out); return; }   // NULL in ingresso -> NULL
    }
    if (!strcmp(f, "NULLIF")) {
        if (a[0].null) { ev_null(out); return; }
        if (!a[1].null && ev_cmp(&a[0], &a[1]) == 0) { ev_null(out); return; }
        ev_copy(out, &a[0]);
        return;
    }
    if (!strcmp(f, "UPPER") || !strcmp(f, "UCASE") || !strcmp(f, "LOWER") || !strcmp(f, "LCASE")) {
        bool up = f[0] == 'U';
        char buf[EV_STR_MAX + 1];
        size_t l = strlen(a[0].str);
        for (size_t i = 0; i < l; i++) buf[i] = (char)(up ? toupper((unsigned char)a[0].str[i]) : tolower((unsigned char)a[0].str[i]));
        ev_set_text(c, out, buf, l);
        return;
    }
    if (!strcmp(f, "LENGTH"))        { ev_int(out, (int64_t)strlen(a[0].str)); return; }
    if (!strncmp(f, "CHAR", 4))      { ev_int(out, utf8_chars(a[0].str)); return; }
    if (!strcmp(f, "TRIM") || !strcmp(f, "LTRIM") || !strcmp(f, "RTRIM")) {
        const char* s = a[0].str;
        const char* e2 = s + strlen(s);
        if (f[0] != 'R') while (*s == ' ') s++;
        if (f[0] != 'L') while (e2 > s && e2[-1] == ' ') e2--;
        ev_set_text(c, out, s, (size_t)(e2 - s));
        return;
    }
    if (!strcmp(f, "SUBSTRING") || !strcmp(f, "SUBSTR") || !strcmp(f, "LEFT") || !strcmp(f, "RIGHT")) {
        const char* s = a[0].str;
        int chars = utf8_chars(s);
        int64_t start, count;
        if (!strcmp(f, "LEFT"))       { start = 1; count = (int64_t)ev_as_num(&a[1]); }
        else if (!strcmp(f, "RIGHT")) { count = (int64_t)ev_as_num(&a[1]); start = chars - count + 1; if (start < 1) start = 1; }
        else {
            start = (int64_t)ev_as_num(&a[1]);
            if (start < 0) start = chars + start + 1;        // posizioni negative: dalla fine
            count = e->nargs > 2 ? (int64_t)ev_as_num(&a[2]) : chars;
            if (start < 1) { ev_set_text(c, out, "", 0); return; }
        }
        if (count <= 0 || start > chars) { ev_set_text(c, out, "", 0); return; }
        const char* b0 = utf8_at(s, (int)(start - 1));
        const char* b1 = utf8_at(b0, (int)(count > chars ? chars : count));
        ev_set_text(c, out, b0, (size_t)(b1 - b0));
        return;
    }
    if (!strcmp(f, "REPLACE")) {
        const char* s = a[0].str, *from = a[1].str, *to = a[2].str;
        size_t fl = strlen(from), tl = strlen(to), len = 0;
        char buf[EV_STR_MAX + 2];
        while (*s) {
            if (fl && !strncmp(s, from, fl)) {
                if (len + tl > EV_STR_MAX) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); return; }
                memcpy(buf + len, to, tl); len += tl; s += fl;
            } else {
                if (len + 1 > EV_STR_MAX) { ctx_error(c, "%s", "String result longer than 511 bytes"); ev_null(out); return; }
                buf[len++] = *s++;
            }
        }
        ev_set_text(c, out, buf, len);
        return;
    }
    if (!strcmp(f, "REVERSE")) {
        const char* s = a[0].str;
        size_t l = strlen(s);
        char buf[EV_STR_MAX + 1];
        size_t k = 0;
        for (size_t i = l; i > 0; ) {                       // carattere UTF-8 intero
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
            memcpy(buf + k, s + j, i - j);
            k += i - j;
            i = j;
        }
        ev_set_text(c, out, buf, k);
        return;
    }
    if (!strcmp(f, "YEAR") || !strcmp(f, "MONTH") || !strcmp(f, "DAY")) {
        int y, m, d;
        if (sscanf(a[0].str, "%d-%d-%d", &y, &m, &d) != 3) { ev_null(out); return; }
        ev_int(out, f[0] == 'Y' ? y : f[0] == 'M' ? m : d);
        return;
    }
    // numeriche
    double x = ev_as_num(&a[0]);
    if (!strcmp(f, "ABS")) {
        if (a[0].is_int && a[0].inum != INT64_MIN) ev_int(out, a[0].inum < 0 ? -a[0].inum : a[0].inum);
        else ev_num(out, fabs(x));
        return;
    }
    if (!strcmp(f, "SIGN"))  { ev_int(out, x > 0 ? 1 : x < 0 ? -1 : 0); return; }
    if (!strcmp(f, "FLOOR") || !strncmp(f, "CEIL", 4)) {
        double r = f[0] == 'F' ? floor(x) : ceil(x);
        if (r >= -9.2e18 && r <= 9.2e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return;
    }
    if (!strcmp(f, "ROUND")) {
        double dd = e->nargs > 1 ? ev_as_num(&a[1]) : 0;
        if (dd > 15) { ev_copy(out, &a[0]); if (!out->is_num) ev_num(out, x); return; }   // più cifre di quante ne ha un double
        if (dd < -308) { ev_int(out, 0); return; }
        int d = (int)dd;
        if (a[0].is_int && d >= 0) { ev_int(out, a[0].inum); return; }
        double p = pow(10.0, d);
        double r = (x < 0 ? -floor(-x * p + 0.5) : floor(x * p + 0.5)) / p;   // metà lontano da zero
        if (d <= 0 && r >= -9.2e18 && r <= 9.2e18) ev_int(out, (int64_t)r); else ev_num(out, r);
        return;
    }
    if (!strcmp(f, "MOD")) {
        if (a[0].is_int && a[1].is_int) {
            if (a[1].inum == 0) { ev_null(out); return; }
            if (a[1].inum == -1) { ev_int(out, 0); return; }
            ev_int(out, a[0].inum % a[1].inum);
            return;
        }
        double y = ev_as_num(&a[1]);
        if (y == 0) { ev_null(out); return; }
        ev_num(out, fmod(x, y));
        return;
    }
    if (!strncmp(f, "POW", 3)) { ev_num(out, pow(x, ev_as_num(&a[1]))); return; }
    if (!strcmp(f, "SQRT"))    { if (x < 0) ev_null(out); else ev_num(out, sqrt(x)); return; }
    ev_null(out);
}

static void eval_aggregate(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    if (!c->grp) { ctx_error(c, "%s", "Aggregate used outside of a group"); ev_null(out); return; }
    bool count_star = e->nargs == 0;
    if (count_star) { ev_int(out, c->ngrp); return; }

    Tuple** saved = c->cur;
    int64_t count = 0, isum = 0;
    double sum = 0;
    bool all_int = true, overflow = false, have_best = false;
    bool is_min = !strcmp(e->func, "MIN"), is_max = !strcmp(e->func, "MAX");
    EvalVal best, v;
    StrMap seen = {0};
    for (int k = 0; k < c->ngrp; k++) {
        c->cur = c->grp[k];
        eval_expr(e->args[0], c, &v);
        if (v.null) continue;
        if (e->agg_distinct) {                                   // COUNT/SUM/AVG(DISTINCT x)
            // i numeri uguali hanno la stessa chiave (5 e 5.0), i testi restano distinti dai numeri
            char key[EV_STR_MAX + 4];
            if (v.is_int) snprintf(key, sizeof(key), "n%lld", (long long)v.inum);
            else if (v.is_num && v.num == floor(v.num) && fabs(v.num) < 9e18) snprintf(key, sizeof(key), "n%lld", (long long)v.num);
            else if (v.is_num) snprintf(key, sizeof(key), "n%.17g", v.num);
            else snprintf(key, sizeof(key), "s%s", v.str);
            SMSlot* sl = sm_slot(&seen, key, true);
            if (!sl) { ctx_error(c, "%s", "Out of memory"); break; }
            if (sl->val >= 0) continue;
            sl->val = 1;
        }
        count++;
        if (v.is_num || !(is_min || is_max)) {
            double d = ev_as_num(&v);
            sum += d;
            if (v.is_int && !overflow) { if (__builtin_add_overflow(isum, v.inum, &isum)) overflow = true; }
            else all_int = false;
        }
        if ((is_min || is_max) && (!have_best || (is_min ? ev_cmp(&v, &best) < 0 : ev_cmp(&v, &best) > 0))) {
            ev_copy(&best, &v);
            if (best.str != best.buf) {   // il testo punta alla tupla: copia (la riga corrente cambia)
                size_t l = strlen(best.str);
                memcpy(best.buf, best.str, l > EV_STR_MAX ? EV_STR_MAX : l);
                best.buf[l > EV_STR_MAX ? EV_STR_MAX : l] = '\0';
                best.str = best.buf;
            }
            have_best = true;
        }
    }
    sm_free(&seen);
    c->cur = saved;
    if (!strcmp(e->func, "COUNT")) { ev_int(out, count); return; }
    if (count == 0) { ev_null(out); return; }                    // SUM/AVG/MIN/MAX di nessun valore = NULL
    if (!strcmp(e->func, "SUM")) { if (all_int && !overflow) ev_int(out, isum); else ev_num(out, sum); return; }
    if (!strcmp(e->func, "AVG")) { ev_num(out, sum / (double)count); return; }
    ev_copy(out, &best);
}

static void eval_expr(ASTExpr* e, EvalCtx* c, EvalVal* out) {
    if (!e) { ev_null(out); return; }
    switch (e->type) {
        case EXPR_NULL:   ev_null(out); return;
        case EXPR_INT:    ev_int(out, e->int_val); return;
        case EXPR_FLOAT:  ev_num(out, e->float_val); return;
        case EXPR_STRING: out->null = false; out->is_num = out->is_int = false; out->str = e->str_val; return;
        case EXPR_COLUMN: {
            if (e->bound < 0) {                                  // alias della SELECT
                int k = -e->bound - 1;
                if (k < c->nsel) { eval_expr(c->sel[k], c, out); return; }
                ev_null(out);
                return;
            }
            if (e->bound == 0) { ctx_error(c, "Unresolved column '%s'", e->col_name); ev_null(out); return; }
            int s = (e->bound - 1) / 1024, col = (e->bound - 1) % 1024;
            Tuple* t = (c->cur && s < c->nsrc) ? c->cur[s] : NULL;
            if (!t || col >= t->num_values) { ev_null(out); return; }
            ev_from_value(out, &t->values[col], c->src[s].ts->columns[col].type);
            return;
        }
        case EXPR_AGG:  eval_aggregate(e, c, out); return;
        case EXPR_FUNC: eval_func(e, c, out); return;
        case EXPR_STAR: ev_null(out); return;
        case EXPR_UNARY: {
            EvalVal a;
            eval_expr(e->left, c, &a);
            switch (e->op) {
                case OP_IS_NULL:     ev_int(out, a.null); return;
                case OP_IS_NOT_NULL: ev_int(out, !a.null); return;
                case OP_NOT: {
                    int t = ev_truth(&a);
                    if (t < 0) ev_null(out); else ev_int(out, !t);
                    return;
                }
                case OP_NEG:
                    if (a.null) { ev_null(out); return; }
                    if (a.is_int && a.inum != INT64_MIN) ev_int(out, -a.inum);
                    else ev_num(out, -ev_as_num(&a));
                    return;
                default: ev_null(out); return;
            }
        }
        case EXPR_BINARY: {
            if (e->op == OP_AND || e->op == OP_OR) {             // logica a tre valori, con cortocircuito
                EvalVal a, b;
                eval_expr(e->left, c, &a);
                int ta = ev_truth(&a);
                if (e->op == OP_AND && ta == 0) { ev_int(out, 0); return; }
                if (e->op == OP_OR && ta == 1)  { ev_int(out, 1); return; }
                eval_expr(e->right, c, &b);
                int tb = ev_truth(&b);
                if (e->op == OP_AND) { if (tb == 0) ev_int(out, 0); else if (ta < 0 || tb < 0) ev_null(out); else ev_int(out, 1); }
                else                 { if (tb == 1) ev_int(out, 1); else if (ta < 0 || tb < 0) ev_null(out); else ev_int(out, 0); }
                return;
            }
            EvalVal a, b;
            eval_expr(e->left, c, &a);
            eval_expr(e->right, c, &b);
            if (a.null || b.null) { ev_null(out); return; }
            switch (e->op) {
                case OP_EQ: ev_int(out, ev_cmp(&a, &b) == 0); return;
                case OP_NEQ: ev_int(out, ev_cmp(&a, &b) != 0); return;
                case OP_LT: ev_int(out, ev_cmp(&a, &b) < 0); return;
                case OP_GT: ev_int(out, ev_cmp(&a, &b) > 0); return;
                case OP_LE: ev_int(out, ev_cmp(&a, &b) <= 0); return;
                case OP_GE: ev_int(out, ev_cmp(&a, &b) >= 0); return;
                case OP_LIKE:     ev_int(out, like_match(a.str, b.str)); return;
                case OP_NOT_LIKE: ev_int(out, !like_match(a.str, b.str)); return;
                case OP_PLUS: case OP_MINUS: case OP_MUL: {
                    double x = ev_as_num(&a), y = ev_as_num(&b);
                    bool ints = (a.is_int || (!a.is_num && x == (double)(int64_t)x && strpbrk(a.str, ".eE") == NULL)) &&
                                (b.is_int || (!b.is_num && y == (double)(int64_t)y && strpbrk(b.str, ".eE") == NULL));
                    if (ints) {
                        int64_t xi = a.is_int ? a.inum : (int64_t)x, yi = b.is_int ? b.inum : (int64_t)y, r;
                        bool ovf = e->op == OP_PLUS  ? __builtin_add_overflow(xi, yi, &r)
                                 : e->op == OP_MINUS ? __builtin_sub_overflow(xi, yi, &r)
                                                     : __builtin_mul_overflow(xi, yi, &r);
                        if (!ovf) { ev_int(out, r); return; }
                        ctx_error(c, "%s", "Integer result out of range (64 bit)");
                        ev_null(out);
                        return;
                    }
                    ev_num(out, e->op == OP_PLUS ? x + y : e->op == OP_MINUS ? x - y : x * y);
                    return;
                }
                case OP_DIV: {
                    double y = ev_as_num(&b);
                    if (y == 0) { ev_null(out); return; }            // divisione per zero: NULL (come MySQL)
                    ev_num(out, ev_as_num(&a) / y);
                    return;
                }
                default: ev_null(out); return;
            }
        }
    }
    ev_null(out);
}

// la condizione è vera per la riga corrente? (NULL conta come falso)
static bool eval_cond(ASTExpr* e, EvalCtx* c) {
    if (!e) return true;
    EvalVal v;
    eval_expr(e, c, &v);
    return ev_truth(&v) == 1;
}

// ═════════════════════════════════════════════════════════════════
//  SELECT: sorgenti -> JOIN -> WHERE -> GROUP BY / HAVING -> proiezione
//          -> DISTINCT -> ORDER BY -> OFFSET / LIMIT
// ═════════════════════════════════════════════════════════════════

#define MAX_VIEW_DEPTH   16
#define MAX_JOIN_ROWS    10000000

static QueryResult* run_select(ExecCtx* ctx, ASTNode* n, int depth, bool explain);

static bool is_text_type(DataType t) {
    return t == SV_TYPE_VARCHAR || t == SV_TYPE_CHAR || t == SV_TYPE_TEXT || t == SV_TYPE_DATE || t == SV_TYPE_DATETIME;
}
static bool is_int_type(DataType t) { return t == SV_TYPE_INT || t == SV_TYPE_BIGINT; }

// schema costruito dal risultato di una vista
static TableSchema* schema_from_result(const QueryResult* r, const char* name) {
    TableSchema* ts = calloc(1, sizeof(TableSchema));
    if (!ts) return NULL;
    snprintf(ts->name, sizeof(ts->name), "%s", name);
    ts->num_columns = r->num_cols < SV_MAX_COLUMNS ? r->num_cols : SV_MAX_COLUMNS;
    for (int i = 0; i < ts->num_columns; i++) {
        const char* cn = r->col_names[i] ? r->col_names[i] : "?";
        const char* dot = strrchr(cn, '.');
        snprintf(ts->columns[i].name, sizeof(ts->columns[i].name), "%s", dot ? dot + 1 : cn);
        const char* tn = (r->col_types && r->col_types[i] && r->col_types[i][0]) ? r->col_types[i] : "VARCHAR";
        ts->columns[i].type = parse_type(tn);
        ts->columns[i].nullable = true;
    }
    return ts;
}

// apre una sorgente: tabella (righe caricate dopo) o vista (eseguita subito)
static bool source_open(ExecCtx* ctx, Database* db, Source* s, const char* table, const char* alias,
                        int depth, char* err, size_t errn) {
    memset(s, 0, sizeof(*s));
    snprintf(s->table, sizeof(s->table), "%s", table);
    snprintf(s->name, sizeof(s->name), "%s", alias && alias[0] ? alias : table);
    s->ts = catalog_get_table(ctx->catalog, db->name, table);
    if (s->ts) return true;

    ViewSchema* view = catalog_get_view(ctx->catalog, db->name, table);
    if (!view) { snprintf(err, errn, "Table or view '%.64s' not found", table); return false; }
    if (depth >= MAX_VIEW_DEPTH) { snprintf(err, errn, "View '%.64s' is nested too deeply (or refers to itself)", table); return false; }
    TokenList* tk = lexer_tokenize(view->definition);
    ParseResult* pr = tk ? parser_parse(tk) : NULL;
    if (!pr || !pr->success || pr->count != 1 || pr->statements[0]->type != NODE_SELECT) {
        snprintf(err, errn, "View '%.64s' has an invalid definition", table);
        parse_result_free(pr);
        lexer_free(tk);
        return false;
    }
    QueryResult* r = run_select(ctx, pr->statements[0], depth + 1, false);
    parse_result_free(pr);
    lexer_free(tk);
    if (!r || !r->success) {
        snprintf(err, errn, "View '%.64s': %.180s", table, r ? r->error : "out of memory");
        db_result_free(r);
        return false;
    }
    s->is_view = true;
    s->own_ts = s->ts = schema_from_result(r, table);
    bool ok = s->ts != NULL;
    int cap = 0;
    for (int i = 0; ok && i < r->num_rows; i++) {
        Tuple* t = tuple_create(s->ts->num_columns);
        if (!t) { ok = false; break; }
        for (int c = 0; c < s->ts->num_columns; c++) {
            const Cell* cell = c < r->rows[i].num_cells ? &r->rows[i].cells[c] : NULL;
            value_set_from_text(&t->values[c], s->ts->columns[c].type, (!cell || cell->is_null) ? NULL : cell->value);
        }
        if (!source_push(s, t, (RID){0, 0}, &cap)) { tuple_free(t); ok = false; }
    }
    db_result_free(r);
    if (!ok) { snprintf(err, errn, "Out of memory while reading view '%.64s'", table); return false; }
    s->loaded = true;
    return true;
}

// in una congiunzione di AND cerca "colonna indicizzata della sorgente 0 = costante"
static bool find_index_eq(ASTExpr* w, TableSchema* ts, int* out_ci, char* key, size_t keyn) {
    if (!w || w->type != EXPR_BINARY) return false;
    if (w->op == OP_AND) return find_index_eq(w->left, ts, out_ci, key, keyn) || find_index_eq(w->right, ts, out_ci, key, keyn);
    if (w->op != OP_EQ) return false;
    for (int side = 0; side < 2; side++) {
        ASTExpr* col = side ? w->right : w->left;
        ASTExpr* val = side ? w->left : w->right;
        if (col->type != EXPR_COLUMN || col->bound <= 0 || (col->bound - 1) / 1024 != 0) continue;
        int ci = (col->bound - 1) % 1024;
        if (!col_is_indexed(ts, ci)) continue;
        DataType t = ts->columns[ci].type;
        if (is_int_type(t) && val->type == EXPR_INT) {
            if (t == SV_TYPE_INT && (val->int_val < INT32_MIN || val->int_val > INT32_MAX)) continue;
            snprintf(key, keyn, "%lld", (long long)val->int_val);
        } else if (is_text_type(t) && val->type == EXPR_STRING) {
            snprintf(key, keyn, "%s", val->str_val);
        } else {
            continue;
        }
        *out_ci = ci;
        return true;
    }
    return false;
}

// carica le righe di una tabella (tutte, o solo quelle trovate con l'indice)
static bool source_load(ExecCtx* ctx, const char* db, Source* s, ASTExpr* where_for_index, char* plan, size_t plann) {
    if (s->loaded) return true;
    s->loaded = true;
    HeapFile* hf = heap_open(ctx->bp, s->ts->root_page_id, s->ts->num_pages);
    if (!hf) return false;
    int cap = 0, ci = -1;
    char key[EV_STR_MAX + 1];
    if (where_for_index && find_index_eq(where_for_index, s->ts, &ci, key, sizeof(key))) {
        MemIndex* ix = mi_get(ctx, db, s->ts, ci);
        if (ix) {
            if (plan) snprintf(plan, plann, "%s = %s", s->ts->columns[ci].name, key);
            const RidList* l = mi_lookup(ix, key);
            for (int i = 0; l && i < l->n; i++) {
                Tuple* t = heap_get(hf, l->r[i], s->ts);
                if (t && !source_push(s, t, l->r[i], &cap)) { tuple_free(t); free(hf); return false; }
            }
            free(hf);
            return true;
        }
    }
    HeapScan* scan = heap_scan_open(hf, s->ts);
    if (!scan) { free(hf); return false; }
    Tuple* t;
    RID rid;
    bool ok = true;
    while (ok && (t = heap_scan_next(scan, &rid)) != NULL)
        if (!source_push(s, t, rid, &cap)) { tuple_free(t); ok = false; }
    heap_scan_close(scan);
    free(hf);
    return ok;
}

// ── combinazioni di righe (una tupla per sorgente) ──

typedef struct { Tuple*** v; int n, cap; } Combos;

static void combos_free(Combos* c) {
    for (int i = 0; i < c->n; i++) free(c->v[i]);
    free(c->v);
    memset(c, 0, sizeof(*c));
}

static bool combos_push(Combos* c, Tuple** base, int nsrc, int k, Tuple* row) {
    if (c->n >= MAX_JOIN_ROWS) return false;
    if (c->n >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 256;
        Tuple*** nv = realloc(c->v, sizeof(Tuple**) * (size_t)nc);
        if (!nv) return false;
        c->v = nv;
        c->cap = nc;
    }
    Tuple** t = calloc((size_t)nsrc, sizeof(Tuple*));
    if (!t) return false;
    if (base) memcpy(t, base, sizeof(Tuple*) * (size_t)nsrc);
    if (k >= 0) t[k] = row;
    c->v[c->n++] = t;
    return true;
}

// chiave per l'hash join: interi -> numero, testo -> testo; false per NULL
static bool join_key(const Tuple* t, int col, DataType type, char* buf, size_t n) {
    if (!t || col >= t->num_values || t->values[col].is_null) return false;
    const Value* v = &t->values[col];
    if (type == SV_TYPE_INT)         snprintf(buf, n, "%d", v->int_val);
    else if (type == SV_TYPE_BIGINT) snprintf(buf, n, "%lld", (long long)v->bigint_val);
    else                             snprintf(buf, n, "%s", v->str_val ? v->str_val : "");
    return true;
}

// "colonna della sorgente k = colonna di una sorgente precedente", con tipi confrontabili per testo
static bool find_hash_cond(ASTExpr* on, int k, Source* src, int* nk_col, int* other_src, int* other_col) {
    if (!on || on->type != EXPR_BINARY) return false;
    if (on->op == OP_AND) return find_hash_cond(on->left, k, src, nk_col, other_src, other_col) ||
                                 find_hash_cond(on->right, k, src, nk_col, other_src, other_col);
    if (on->op != OP_EQ || on->left->type != EXPR_COLUMN || on->right->type != EXPR_COLUMN) return false;
    if (on->left->bound <= 0 || on->right->bound <= 0) return false;
    int ls = (on->left->bound - 1) / 1024, lc = (on->left->bound - 1) % 1024;
    int rs = (on->right->bound - 1) / 1024, rc = (on->right->bound - 1) % 1024;
    if (rs == k && ls < k) { int ts = ls, tc = lc; ls = rs; lc = rc; rs = ts; rc = tc; }
    if (ls != k || rs >= k) return false;
    DataType a = src[ls].ts->columns[lc].type, b = src[rs].ts->columns[rc].type;
    bool same_class = (is_int_type(a) && is_int_type(b)) || (is_text_type(a) && is_text_type(b));
    if (!same_class) return false;
    *nk_col = lc; *other_src = rs; *other_col = rc;
    return true;
}

// unisce la sorgente k alle combinazioni correnti
static const char* join_step(EvalCtx* ec, Combos* in, Combos* out, int k, ASTJoin* j) {
    Source* s = &ec->src[k];
    int nsrc = ec->nsrc;
    bool* right_hit = (j->type == JOIN_RIGHT && s->n) ? calloc((size_t)s->n, sizeof(bool)) : NULL;
    if (j->type == JOIN_RIGHT && s->n && !right_hit) return "Out of memory";

    // hash join quando la condizione contiene un'uguaglianza fra colonne confrontabili
    int kc, os, oc;
    StrMap map = {0};
    int* next = NULL;                     // catena delle righe con la stessa chiave
    bool hashed = find_hash_cond(j->condition, k, ec->src, &kc, &os, &oc);
    if (hashed) {
        next = malloc(sizeof(int) * (size_t)(s->n ? s->n : 1));
        if (!next) { free(right_hit); return "Out of memory"; }
        DataType kt = s->ts->columns[kc].type;
        for (int r = s->n - 1; r >= 0; r--) {          // in ordine: la catena parte dalla prima riga
            char key[EV_STR_MAX + 1];
            next[r] = -1;
            if (!join_key(s->rows[r], kc, kt, key, sizeof(key))) continue;
            SMSlot* sl = sm_slot(&map, key, true);
            if (!sl) { sm_free(&map); free(next); free(right_hit); return "Out of memory"; }
            next[r] = sl->val;
            sl->val = r;
        }
    }
    const char* fail = NULL;
    for (int i = 0; i < in->n && !fail; i++) {
        if ((i & 1023) == 0 && atomic_load(&g_sv_scan_cancel)) { fail = "Query cancelled"; break; }
        ec->cur = in->v[i];
        bool matched = false;
        int r = 0;
        if (hashed) {
            char key[EV_STR_MAX + 1];
            DataType ot = ec->src[os].ts->columns[oc].type;
            SMSlot* sl = join_key(in->v[i][os], oc, ot, key, sizeof(key)) ? sm_slot(&map, key, false) : NULL;
            r = sl ? sl->val : -1;
        }
        for (; hashed ? r >= 0 : r < s->n; r = hashed ? next[r] : r + 1) {
            ec->cur[k] = s->rows[r];
            bool ok = eval_cond(j->condition, ec);
            ec->cur[k] = NULL;
            if (!ok) continue;
            matched = true;
            if (right_hit) right_hit[r] = true;
            if (!combos_push(out, in->v[i], nsrc, k, s->rows[r])) { fail = out->n >= MAX_JOIN_ROWS ? "The JOIN produces too many rows (over 10 million)" : "Out of memory"; break; }
        }
        if (!fail && !matched && j->type == JOIN_LEFT && !combos_push(out, in->v[i], nsrc, k, NULL)) fail = "Out of memory";
    }
    if (!fail && right_hit)
        for (int r = 0; r < s->n && !fail; r++)
            if (!right_hit[r] && !combos_push(out, NULL, nsrc, k, s->rows[r])) fail = "Out of memory";
    sm_free(&map);
    free(next);
    free(right_hit);
    ec->cur = NULL;
    return fail;
}

// ── chiavi di ordinamento ──

typedef struct { bool null, is_num, is_int; int64_t inum; double num; char* str; } SortKey;

static void key_set(SortKey* k, const EvalVal* v) {
    k->null = v->null; k->is_num = v->is_num; k->is_int = v->is_int; k->inum = v->inum; k->num = v->num;
    k->str = v->null ? NULL : strdup(v->str);
}

static int key_cmp(const SortKey* a, const SortKey* b) {
    if (a->null || b->null) return a->null == b->null ? 0 : a->null ? -1 : 1;   // NULL per primi
    EvalVal x, y;
    x.null = y.null = false;
    x.is_num = a->is_num; x.is_int = a->is_int; x.inum = a->inum; x.num = a->num; x.str = a->str ? a->str : "";
    y.is_num = b->is_num; y.is_int = b->is_int; y.inum = b->inum; y.num = b->num; y.str = b->str ? b->str : "";
    return ev_cmp(&x, &y);
}

typedef struct { Cell* cells; SortKey* keys; } OutRow;

static struct { OutRow* rows; int nkeys; const bool* desc; } g_sort;

static int out_row_cmp(const void* pa, const void* pb) {
    int ia = *(const int*)pa, ib = *(const int*)pb;
    for (int k = 0; k < g_sort.nkeys; k++) {
        int c = key_cmp(&g_sort.rows[ia].keys[k], &g_sort.rows[ib].keys[k]);
        if (c) return g_sort.desc[k] ? -c : c;
    }
    return ia < ib ? -1 : ia > ib;   // stabile
}

// ── colonne di output ──

typedef struct {
    ASTExpr* e;
    bool     owned;          // creata per espandere *
    char     name[160];
    int      src_type;       // tipo della colonna se è un riferimento diretto, altrimenti -1
} OutCol;

static void fill_cell(Cell* cell, const EvalVal* v, int src_type) {
    if (v->null) { cell->is_null = true; cell->type = CELL_TYPE_NULL; snprintf(cell->value, sizeof(cell->value), "NULL"); return; }
    cell->is_null = false;
    snprintf(cell->value, sizeof(cell->value), "%s", v->str);
    if (src_type >= 0)  cell->type = cell_type_of((DataType)src_type);
    else if (v->is_int) cell->type = CELL_TYPE_INT;
    else if (v->is_num) cell->type = CELL_TYPE_FLOAT;
    else                cell->type = CELL_TYPE_STRING;
}

// tipo di una colonna calcolata, dedotto dai valori (INT, FLOAT o VARCHAR; "" se tutti NULL)
static const char* infer_type(const QueryResult* r, int j) {
    bool any = false, all_int = true, all_num = true;
    for (int i = 0; i < r->num_rows; i++) {
        const Cell* c = &r->rows[i].cells[j];
        if (c->is_null) continue;
        any = true;
        if (c->type != CELL_TYPE_INT) all_int = false;
        if (c->type != CELL_TYPE_INT && c->type != CELL_TYPE_FLOAT) all_num = false;
    }
    return !any ? "" : all_int ? "INT" : all_num ? "FLOAT" : "VARCHAR";
}

static QueryResult* run_select(ExecCtx* ctx, ASTNode* n, int depth, bool explain) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db && n->select_stmt.table_name[0]) return result_error_msg("No database selected");
    char err[300] = "";
    Source src[MAX_SOURCES];
    int nsrc = 0;
    OutCol out[SV_MAX_COLUMNS];
    int nout = 0;
    Combos combos = {0};
    OutRow* rows = NULL;
    int nrows = 0, caprows = 0;
    Tuple*** groups_buf = NULL;
    Tuple** null_combo = NULL;
    QueryResult* r = NULL;
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    memset(src, 0, sizeof(src));
    char plan_lookup[600] = "";

    // 1. sorgenti (SELECT senza FROM: nessuna, il risultato ha una riga)
    if (n->select_stmt.table_name[0] &&
        !source_open(ctx, db, &src[nsrc++], n->select_stmt.table_name, n->select_stmt.table_alias, depth, err, sizeof(err)))
        goto done;
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        ASTJoin* jn = &n->select_stmt.joins[j];
        if (!source_open(ctx, db, &src[nsrc++], jn->table_name, jn->alias, depth, err, sizeof(err))) goto done;
    }
    for (int a = 0; a < nsrc; a++)
        for (int b = 0; b < a; b++)
            if (!strcasecmp(src[a].name, src[b].name)) {
                snprintf(err, sizeof(err), "Table '%.64s' is used twice: give each one an alias", src[a].name);
                goto done;
            }

    // 2. colonne di output (* espanso)
    for (int i = 0; i < n->select_stmt.num_cols; i++) {
        ASTExpr* e = n->select_stmt.columns[i];
        if (e->type == EXPR_STAR) {
            bool any = false;
            for (int s = 0; s < nsrc; s++) {
                if (e->table_name[0] && strcasecmp(e->table_name, src[s].name) && strcasecmp(e->table_name, src[s].table)) continue;
                any = true;
                for (int c = 0; c < src[s].ts->num_columns; c++) {
                    if (nout >= SV_MAX_COLUMNS) { snprintf(err, sizeof(err), "Too many columns in the result (max %d)", SV_MAX_COLUMNS); goto done; }
                    ASTExpr* col = calloc(1, sizeof(ASTExpr));
                    if (!col) { snprintf(err, sizeof(err), "Out of memory"); goto done; }
                    col->type = EXPR_COLUMN;
                    snprintf(col->col_name, sizeof(col->col_name), "%s", src[s].ts->columns[c].name);
                    col->bound = s * 1024 + c + 1;
                    OutCol* oc = &out[nout++];
                    oc->e = col;
                    oc->owned = true;
                    oc->src_type = (int)src[s].ts->columns[c].type;
                    if (nsrc > 1) snprintf(oc->name, sizeof(oc->name), "%.64s.%.64s", src[s].name, col->col_name);
                    else          snprintf(oc->name, sizeof(oc->name), "%s", col->col_name);
                }
            }
            if (!any && !nsrc) { snprintf(err, sizeof(err), "SELECT * needs a FROM clause"); goto done; }
            if (!any) { snprintf(err, sizeof(err), "Unknown table '%.64s' in SELECT", e->table_name); goto done; }
            continue;
        }
        if (nout >= SV_MAX_COLUMNS) { snprintf(err, sizeof(err), "Too many columns in the result (max %d)", SV_MAX_COLUMNS); goto done; }
        out[nout].e = e;
        out[nout].owned = false;
        out[nout].src_type = -1;
        nout++;
    }

    // 3. binding
    ASTExpr* sel[SV_MAX_COLUMNS];
    for (int i = 0; i < nout; i++) sel[i] = out[i].e;
    bool has_agg = false;
    {
        BindCtx b;
        memset(&b, 0, sizeof(b));
        b.src = src;
        for (int j = 0; j < n->select_stmt.num_joins; j++) {          // ON vede solo le tabelle già unite
            b.nsrc = j + 2; b.mode = BIND_ROW; b.clause = "ON";
            bind_expr(n->select_stmt.joins[j].condition, &b);
        }
        b.nsrc = nsrc;
        b.mode = BIND_AGG_OK; b.clause = "SELECT";
        for (int i = 0; i < nout; i++) if (!out[i].owned) bind_expr(out[i].e, &b);
        has_agg = b.saw_agg;
        b.mode = BIND_ROW; b.clause = "WHERE";
        bind_expr(n->select_stmt.where, &b);
        b.sel = sel; b.nsel = nout;
        b.clause = "GROUP BY";
        for (int i = 0; i < n->select_stmt.num_group; i++) {
            ASTExpr* g = n->select_stmt.group_exprs[i];
            bind_expr(g, &b);
            if (!b.err[0] && g->type == EXPR_COLUMN && g->bound < 0 && expr_has_agg(sel[-g->bound - 1]))
                bind_error(&b, "Cannot GROUP BY an aggregate");
        }
        b.mode = BIND_AGG_OK; b.saw_agg = false; b.clause = "HAVING";
        bind_expr(n->select_stmt.having, &b);
        b.clause = "ORDER BY";
        for (int i = 0; i < n->select_stmt.num_order; i++) {
            ASTExpr* o = n->select_stmt.order_exprs[i];
            if (o->type == EXPR_INT) {                                  // ORDER BY 2 = seconda colonna
                if (o->int_val < 1 || o->int_val > nout) { bind_error(&b, "ORDER BY position is out of range"); break; }
                continue;
            }
            // un nome che è alias di una colonna di output vale l'alias (come MySQL)
            if (o->type == EXPR_COLUMN && !o->table_name[0]) {
                int k;
                for (k = 0; k < nout; k++) if (sel[k]->alias[0] && !strcasecmp(sel[k]->alias, o->col_name)) break;
                if (k < nout) { o->bound = -(k + 1); continue; }
            }
            bind_expr(o, &b);
        }
        if (b.saw_agg) has_agg = true;
        if (b.err[0]) { snprintf(err, sizeof(err), "%s", b.err); goto done; }
    }
    for (int i = 0; i < nout; i++) {
        OutCol* oc = &out[i];
        if (oc->owned) continue;
        ASTExpr* e = oc->e;
        if (e->type == EXPR_COLUMN && e->bound > 0) {
            int s = (e->bound - 1) / 1024, c = (e->bound - 1) % 1024;
            oc->src_type = (int)src[s].ts->columns[c].type;
        }
        if (e->alias[0])                                snprintf(oc->name, sizeof(oc->name), "%s", e->alias);
        else if (e->type == EXPR_COLUMN && nsrc > 1)    snprintf(oc->name, sizeof(oc->name), "%.64s.%.64s",
                                                                 src[(e->bound - 1) / 1024].name, e->col_name);
        else if (e->type == EXPR_COLUMN)                snprintf(oc->name, sizeof(oc->name), "%s", e->col_name);
        else                                            ast_expr_format(e, oc->name, sizeof(oc->name));
    }
    bool grouped = has_agg || n->select_stmt.num_group > 0 || n->select_stmt.having;

    // 4. EXPLAIN: descrive il piano senza eseguire
    if (explain) {
        char fake[400] = "";
        int fci;
        bool lookup = nsrc == 1 && !src[0].is_view && find_index_eq(n->select_stmt.where, src[0].ts, &fci, fake, sizeof(fake));
        r = calloc(1, sizeof(QueryResult));
        if (!r) goto done;
        r->success = true;
        r->num_cols = 3;
        r->col_names = calloc(3, sizeof(char*));
        r->col_types = calloc(3, sizeof(char*));
        r->col_names[0] = strdup("Step"); r->col_names[1] = strdup("Operation"); r->col_names[2] = strdup("Details");
        for (int i = 0; i < 3; i++) r->col_types[i] = strdup(i ? "VARCHAR" : "INT");
        r->rows = calloc(64, sizeof(Row));
        char detail[440], buf[400];
        #define PLAN(op, ...) do { if (r->num_rows < 64) { Row* pr = &r->rows[r->num_rows++]; pr->num_cells = 3; \
            pr->cells = calloc(3, sizeof(Cell)); snprintf(pr->cells[0].value, sizeof(pr->cells[0].value), "%d", r->num_rows); \
            pr->cells[0].type = CELL_TYPE_INT; snprintf(pr->cells[1].value, sizeof(pr->cells[1].value), "%s", op); \
            snprintf(pr->cells[2].value, sizeof(pr->cells[2].value), __VA_ARGS__); \
            pr->cells[1].type = pr->cells[2].type = CELL_TYPE_STRING; } } while (0)
        for (int s = 0; s < nsrc; s++) {
            if (src[s].is_view)  PLAN("View", "%.64s (%d rows, computed)", src[s].name, src[s].n);
            else if (s == 0 && lookup) {
                ast_expr_format(n->select_stmt.where, buf, sizeof(buf));
                PLAN("Index lookup", "%.64s using %.64s.%.64s (in-memory index)", src[s].name, src[s].table, src[s].ts->columns[fci].name);
            } else PLAN("Table scan", "%.64s (about %llu rows)", src[s].name, (unsigned long long)src[s].ts->num_rows);
            if (s > 0) {
                ASTJoin* j = &n->select_stmt.joins[s - 1];
                int a1, a2, a3;
                const char* kind = j->type == JOIN_LEFT ? "LEFT" : j->type == JOIN_RIGHT ? "RIGHT" : j->type == JOIN_CROSS ? "CROSS" : "INNER";
                if (j->condition) ast_expr_format(j->condition, buf, sizeof(buf)); else snprintf(buf, sizeof(buf), "every combination");
                snprintf(detail, sizeof(detail), "%s JOIN ON %s", kind, buf);
                PLAN(find_hash_cond(j->condition, s, src, &a1, &a2, &a3) ? "Hash join" : "Nested loop join", "%s", detail);
            }
        }
        if (n->select_stmt.where) { ast_expr_format(n->select_stmt.where, buf, sizeof(buf)); PLAN("Filter", "WHERE %s", buf); }
        if (grouped) {
            buf[0] = '\0';
            for (int i = 0; i < n->select_stmt.num_group; i++) {
                char g[200];
                ast_expr_format(n->select_stmt.group_exprs[i], g, sizeof(g));
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", i ? ", " : "", g);
            }
            PLAN("Aggregate", "%s", n->select_stmt.num_group ? buf : "all rows as one group");
        }
        if (n->select_stmt.having) { ast_expr_format(n->select_stmt.having, buf, sizeof(buf)); PLAN("Filter groups", "HAVING %s", buf); }
        buf[0] = '\0';
        for (int i = 0; i < nout; i++) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", i ? ", " : "", out[i].name);
        PLAN("Projection", "%s", buf);
        if (n->select_stmt.distinct) PLAN("Distinct", "remove duplicate rows");
        if (n->select_stmt.num_order) {
            buf[0] = '\0';
            for (int i = 0; i < n->select_stmt.num_order; i++) {
                char o[200];
                ast_expr_format(n->select_stmt.order_exprs[i], o, sizeof(o));
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s%s", i ? ", " : "", o, n->select_stmt.order_desc[i] ? " DESC" : "");
            }
            PLAN("Sort", "%s", buf);
        }
        if (n->select_stmt.has_limit) PLAN("Limit", "%d rows, skipping %d", n->select_stmt.limit, n->select_stmt.offset);
        #undef PLAN
        goto done;
    }

    // 5. righe delle tabelle (con l'indice se la WHERE lo permette e c'è una sola tabella)
    for (int s = 0; s < nsrc; s++) {
        if (!source_load(ctx, db->name, &src[s], nsrc == 1 ? n->select_stmt.where : NULL, plan_lookup, sizeof(plan_lookup))) {
            snprintf(err, sizeof(err), "Failed to read table '%.64s'", src[s].table);
            goto done;
        }
    }
    if (atomic_load(&g_sv_scan_cancel)) { snprintf(err, sizeof(err), "Query cancelled"); goto done; }

    // 6. combinazioni (JOIN) e WHERE
    ec.src = src;
    ec.nsrc = nsrc;
    ec.sel = sel;
    ec.nsel = nout;
    if (nsrc == 0) {                                     // SELECT senza FROM: una riga senza colonne di tabella
        if (!combos_push(&combos, NULL, 1, -1, NULL)) { snprintf(err, sizeof(err), "Out of memory"); goto done; }
    }
    for (int i = 0; nsrc > 0 && i < src[0].n; i++)
        if (!combos_push(&combos, NULL, nsrc, 0, src[0].rows[i])) { snprintf(err, sizeof(err), "Out of memory"); goto done; }
    for (int j = 0; j < n->select_stmt.num_joins; j++) {
        Combos next = {0};
        const char* f = join_step(&ec, &combos, &next, j + 1, &n->select_stmt.joins[j]);
        combos_free(&combos);
        combos = next;
        if (f) { snprintf(err, sizeof(err), "%s", f); goto done; }
        if (ec.err[0]) { snprintf(err, sizeof(err), "%s", ec.err); goto done; }
    }
    if (n->select_stmt.where) {
        int w = 0;
        for (int i = 0; i < combos.n; i++) {
            ec.cur = combos.v[i];
            if (eval_cond(n->select_stmt.where, &ec)) combos.v[w++] = combos.v[i];
            else free(combos.v[i]);
        }
        combos.n = w;
        ec.cur = NULL;
        if (ec.err[0]) { snprintf(err, sizeof(err), "%s", ec.err); goto done; }
    }

    // 7. righe di output (una per combinazione, o una per gruppo)
    int nkeys = n->select_stmt.num_order;
    null_combo = calloc((size_t)(nsrc ? nsrc : 1), sizeof(Tuple*));
    if (!null_combo) { snprintf(err, sizeof(err), "Out of memory"); goto done; }

    int ngroups = 0;
    int* group_of = NULL;      // gruppo di ogni combinazione
    int* group_start = NULL;   // prima combinazione (in groups_buf) di ogni gruppo
    int* group_size = NULL;
    if (grouped) {
        group_of = malloc(sizeof(int) * (size_t)(combos.n ? combos.n : 1));
        StrMap gm = {0};
        bool ok = group_of != NULL;
        char* sig = malloc(SV_MAX_GROUP * (EV_STR_MAX + 8) + 1);
        if (!sig) ok = false;
        for (int i = 0; ok && i < combos.n; i++) {
            ec.cur = combos.v[i];
            size_t len = 0;
            sig[0] = '\0';
            for (int g = 0; g < n->select_stmt.num_group; g++) {
                EvalVal v;
                eval_expr(n->select_stmt.group_exprs[g], &ec, &v);
                if (v.null) len += (size_t)sprintf(sig + len, "\x01\x1f");
                else if (v.is_int) len += (size_t)sprintf(sig + len, "n%lld\x1f", (long long)v.inum);
                else if (v.is_num) len += (size_t)sprintf(sig + len, "n%.17g\x1f", v.num);
                else len += (size_t)snprintf(sig + len, EV_STR_MAX + 4, "s%s\x1f", v.str);
            }
            SMSlot* sl = sm_slot(&gm, sig, true);
            if (!sl) { ok = false; break; }
            if (sl->val < 0) sl->val = ngroups++;
            group_of[i] = sl->val;
        }
        sm_free(&gm);
        free(sig);
        ec.cur = NULL;
        if (ok && n->select_stmt.num_group == 0 && ngroups == 0) ngroups = 1;   // solo aggregati: un gruppo anche vuoto
        group_start = calloc((size_t)ngroups + 1, sizeof(int));
        group_size  = calloc((size_t)ngroups + 1, sizeof(int));
        groups_buf  = malloc(sizeof(Tuple**) * (size_t)(combos.n ? combos.n : 1));
        if (!ok || !group_start || !group_size || !groups_buf) {
            free(group_of); free(group_start); free(group_size);
            snprintf(err, sizeof(err), "Out of memory");
            goto done;
        }
        for (int i = 0; i < combos.n; i++) group_size[group_of[i]]++;
        for (int g = 1; g < ngroups; g++) group_start[g] = group_start[g - 1] + group_size[g - 1];
        int* fill = calloc((size_t)ngroups + 1, sizeof(int));
        if (!fill) { free(group_of); free(group_start); free(group_size); snprintf(err, sizeof(err), "Out of memory"); goto done; }
        for (int i = 0; i < combos.n; i++) {
            int g = group_of[i];
            groups_buf[group_start[g] + fill[g]++] = combos.v[i];
        }
        free(fill);
        free(group_of);
    }

    int units = grouped ? ngroups : combos.n;
    for (int u = 0; u < units; u++) {
        if ((u & 1023) == 0 && atomic_load(&g_sv_scan_cancel)) { snprintf(err, sizeof(err), "Query cancelled"); break; }
        if (grouped) {
            ec.grp  = group_size[u] ? &groups_buf[group_start[u]] : NULL;
            ec.ngrp = group_size[u];
            ec.cur  = group_size[u] ? groups_buf[group_start[u]] : null_combo;
            if (!ec.grp) { ec.grp = &null_combo; ec.ngrp = 0; }
            if (n->select_stmt.having && !eval_cond(n->select_stmt.having, &ec)) continue;
        } else {
            ec.cur = combos.v[u];
        }
        if (nrows >= caprows) {
            int nc = caprows ? caprows * 2 : 64;
            OutRow* nr = realloc(rows, sizeof(OutRow) * (size_t)nc);
            if (!nr) { snprintf(err, sizeof(err), "Out of memory"); break; }
            rows = nr;
            caprows = nc;
        }
        OutRow* orow = &rows[nrows];
        orow->cells = calloc((size_t)(nout ? nout : 1), sizeof(Cell));
        orow->keys = nkeys ? calloc((size_t)nkeys, sizeof(SortKey)) : NULL;
        if (!orow->cells || (nkeys && !orow->keys)) { free(orow->cells); free(orow->keys); snprintf(err, sizeof(err), "Out of memory"); break; }
        nrows++;
        for (int j = 0; j < nout; j++) {
            EvalVal v;
            eval_expr(out[j].e, &ec, &v);
            fill_cell(&orow->cells[j], &v, v.null ? -1 : out[j].src_type);
        }
        for (int k = 0; k < nkeys; k++) {
            EvalVal v;
            ASTExpr* o = n->select_stmt.order_exprs[k];
            eval_expr(o->type == EXPR_INT ? out[o->int_val - 1].e : o, &ec, &v);
            key_set(&orow->keys[k], &v);
        }
    }
    free(group_start);
    free(group_size);
    ec.cur = NULL;
    ec.grp = NULL;
    if (!err[0] && ec.err[0]) snprintf(err, sizeof(err), "%s", ec.err);
    if (err[0]) goto done;

    // 8. DISTINCT, ORDER BY, OFFSET / LIMIT
    int* order = malloc(sizeof(int) * (size_t)(nrows ? nrows : 1));
    if (!order) { snprintf(err, sizeof(err), "Out of memory"); goto done; }
    int nkeep = 0;
    if (n->select_stmt.distinct) {
        StrMap seen = {0};
        char* sig = malloc((size_t)nout * (sizeof(((Cell*)0)->value) + 2) + 1);
        bool ok = sig != NULL;
        for (int i = 0; ok && i < nrows; i++) {
            size_t len = 0;
            for (int j = 0; j < nout; j++) {
                const Cell* c = &rows[i].cells[j];
                len += (size_t)sprintf(sig + len, "%s\x1f", c->is_null ? "\x01" : c->value);
            }
            SMSlot* sl = sm_slot(&seen, sig, true);
            if (!sl) { ok = false; break; }
            if (sl->val >= 0) continue;
            sl->val = i;
            order[nkeep++] = i;
        }
        free(sig);
        sm_free(&seen);
        if (!ok) { free(order); snprintf(err, sizeof(err), "Out of memory"); goto done; }
    } else {
        for (int i = 0; i < nrows; i++) order[nkeep++] = i;
    }
    if (nkeys) {
        g_sort.rows = rows;
        g_sort.nkeys = nkeys;
        g_sort.desc = n->select_stmt.order_desc;
        qsort(order, (size_t)nkeep, sizeof(int), out_row_cmp);
    }
    int first = n->select_stmt.offset < nkeep ? n->select_stmt.offset : nkeep;
    int count = nkeep - first;
    if (n->select_stmt.has_limit && n->select_stmt.limit < count) count = n->select_stmt.limit;

    // 9. risultato
    r = calloc(1, sizeof(QueryResult));
    if (!r) { free(order); snprintf(err, sizeof(err), "Out of memory"); goto done; }
    r->success = true;
    r->num_cols = nout;
    r->col_names = calloc((size_t)nout + 1, sizeof(char*));
    r->col_types = calloc((size_t)nout + 1, sizeof(char*));
    r->rows = calloc((size_t)(count ? count : 1), sizeof(Row));
    if (!r->col_names || !r->col_types || !r->rows) { free(order); snprintf(err, sizeof(err), "Out of memory"); goto done; }
    for (int i = 0; i < count; i++) {
        OutRow* orow = &rows[order[first + i]];
        r->rows[i].cells = orow->cells;
        r->rows[i].num_cells = nout;
        orow->cells = NULL;                 // passata al risultato
    }
    r->num_rows = count;
    for (int j = 0; j < nout; j++) {
        r->col_names[j] = strdup(out[j].name);
        const ASTExpr* e = out[j].e;
        const char* tn;
        if (out[j].src_type >= 0) tn = datatype_name((DataType)out[j].src_type);
        else if (e->type == EXPR_AGG && !strcmp(e->func, "COUNT")) tn = "INT";
        else if (e->type == EXPR_AGG && !strcmp(e->func, "AVG")) tn = "FLOAT";
        else if (e->type == EXPR_AGG && strcmp(e->func, "SUM") && e->nargs == 1 &&
                 e->args[0]->type == EXPR_COLUMN && e->args[0]->bound > 0)        // MIN/MAX di una colonna
            tn = datatype_name(src[(e->args[0]->bound - 1) / 1024].ts->columns[(e->args[0]->bound - 1) % 1024].type);
        else tn = infer_type(r, j);
        r->col_types[j] = strdup(tn);
    }
    free(order);

done:
    for (int i = 0; i < nrows; i++) {
        free(rows[i].cells);
        for (int k = 0; rows[i].keys && k < n->select_stmt.num_order; k++) free(rows[i].keys[k].str);
        free(rows[i].keys);
    }
    free(rows);
    free(groups_buf);
    free(null_combo);
    combos_free(&combos);
    for (int i = 0; i < nout; i++) if (out[i].owned) ast_expr_free(out[i].e);
    for (int s = 0; s < nsrc; s++) source_free(&src[s]);
    if (err[0]) {
        if (r) db_result_free(r);
        return result_error_msg(err);
    }
    return r ? r : result_error_msg("Out of memory");
}

// ═════════════════════════════════════════════════════════════════
//  INSERT / UPDATE / DELETE
//  Prima si valida tutto (tipi, NOT NULL, chiave primaria anche fra le righe dello stesso
//  statement), poi si scrive: uno statement che fallisce non lascia modifiche a metà.
// ═════════════════════════════════════════════════════════════════

static Tuple* tuple_clone(const Tuple* t, const TableSchema* ts) {
    Tuple* c = tuple_create(t->num_values);
    if (!c) return NULL;
    for (int i = 0; i < t->num_values; i++) {
        const Value* v = &t->values[i];
        if (v->is_null) { c->values[i].is_null = true; continue; }
        DataType ty = i < ts->num_columns ? ts->columns[i].type : SV_TYPE_VARCHAR;
        switch (ty) {
            case SV_TYPE_INT: case SV_TYPE_BIGINT: case SV_TYPE_FLOAT: case SV_TYPE_BOOL:
                c->values[i] = *v;
                c->values[i].owns_str = false;
                break;
            default:
                c->values[i].str_val = strdup(v->str_val ? v->str_val : "");
                if (!c->values[i].str_val) { tuple_free(c); return NULL; }
                c->values[i].owns_str = true;
                break;
        }
    }
    return c;
}

// testo di un valore calcolato da salvare (NULL per i NULL)
static const char* ev_store_text(const EvalVal* v) { return v->null ? NULL : v->str; }

static int pk_column(const TableSchema* ts) {
    for (int i = 0; i < ts->num_columns; i++) if (ts->columns[i].primary_key) return i;
    return -1;
}

static void rid_key(RID r, char* buf, size_t n) { snprintf(buf, n, "%u:%u", r.page_id, (unsigned)r.slot_id); }

static void wal_log(ExecCtx* ctx, uint8_t type, RID rid, const Tuple* newt, const Tuple* oldt, TableSchema* ts) {
    if (!ctx->wal) return;
    size_t nl = 0, ol = 0;
    uint8_t* ns = newt ? tuple_serialize((Tuple*)newt, ts, &nl) : NULL;
    uint8_t* os = oldt ? tuple_serialize((Tuple*)oldt, ts, &ol) : NULL;
    wal_write(ctx->wal, type, ctx->tx_id, rid.page_id, (SVRID){rid.page_id, rid.slot_id},
              ns, (uint32_t)nl, os, (uint32_t)ol);
    free(ns);
    free(os);
}

static QueryResult* exec_insert(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->insert_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->insert_stmt.table_name))
            return result_error_msg("Cannot INSERT into a view");
        return result_error_msg("Table not found");
    }
    char msg[300];
    int nc = ts->num_columns;

    // colonne indicate
    int map[64];
    int ncols = n->insert_stmt.num_cols > 0 ? n->insert_stmt.num_cols : nc;
    for (int i = 0; i < ncols; i++) {
        if (n->insert_stmt.num_cols == 0) { map[i] = i; continue; }
        int ci = catalog_get_col_index(ts, n->insert_stmt.columns[i]);
        if (ci < 0) return unknown_column_error(n->insert_stmt.columns[i], "INSERT");
        for (int k = 0; k < i; k++)
            if (map[k] == ci) {
                snprintf(msg, sizeof(msg), "Column '%.64s' is listed twice", ts->columns[ci].name);
                return result_error_msg(msg);
            }
        map[i] = ci;
    }
    if (n->insert_stmt.num_values != ncols) {
        snprintf(msg, sizeof(msg), "Column count doesn't match value count: %d column%s, %d value%s",
                 ncols, ncols == 1 ? "" : "s", n->insert_stmt.num_values, n->insert_stmt.num_values == 1 ? "" : "s");
        return result_error_msg(msg);
    }
    int nrows = n->insert_stmt.num_rows;

    // i valori sono espressioni senza colonne (numeri, testi, funzioni come NOW())
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.mode = BIND_ROW;
    b.clause = "VALUES";
    for (int i = 0; i < nrows * ncols; i++) bind_expr(n->insert_stmt.values[i], &b);
    if (b.err[0]) return result_error_msg(b.err);

    int pk = pk_column(ts);
    MemIndex* pk_ix = pk >= 0 ? mi_get(ctx, db->name, ts, pk) : NULL;
    if (pk >= 0 && !pk_ix) return result_error_msg("Out of memory (primary key index)");
    int ai = -1;
    for (int i = 0; i < nc; i++) if (ts->columns[i].auto_increment) { ai = i; break; }
    int64_t auto_next = 0;
    if (ai >= 0) {
        MemIndex* ix = mi_get(ctx, db->name, ts, ai);
        if (!ix) return result_error_msg("Out of memory (AUTO_INCREMENT index)");
        auto_next = ix->has_max ? ix->max_int : 0;
    }

    Tuple** tuples = calloc((size_t)(nrows ? nrows : 1), sizeof(Tuple*));
    StrMap batch = {0};
    if (!tuples) return result_error_msg("Out of memory");
    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    const char* fail = NULL;
    static char vals[64][EV_STR_MAX + 1];   // testi della riga corrente
    char ai_buf[32];

    for (int r = 0; r < nrows && !fail; r++) {
        const char* text[64];
        bool given[64] = {0};
        for (int i = 0; i < nc; i++) text[i] = NULL;
        for (int k = 0; k < ncols; k++) {
            EvalVal v;
            eval_expr(n->insert_stmt.values[r * ncols + k], &ec, &v);
            if (ec.err[0]) { fail = ec.err; break; }
            int ci = map[k];
            given[ci] = true;
            if (v.null) { text[ci] = NULL; continue; }
            snprintf(vals[ci], sizeof(vals[ci]), "%s", v.str);
            text[ci] = vals[ci];
        }
        if (fail) break;
        for (int i = 0; i < nc; i++) {
            Column* c = &ts->columns[i];
            if (!given[i] && c->default_val[0] && strcasecmp(c->default_val, "NULL") != 0) text[i] = c->default_val;
            if (i == ai) {
                if (!text[i]) {                                   // valore generato (anche per un NULL esplicito)
                    if (auto_next == INT64_MAX) { fail = "AUTO_INCREMENT value out of range"; break; }
                    snprintf(ai_buf, sizeof(ai_buf), "%lld", (long long)++auto_next);
                    text[i] = ai_buf;
                } else {
                    char* end = NULL;
                    long long x = strtoll(text[i], &end, 10);
                    if (end != text[i] && x > auto_next) auto_next = x;
                }
            }
            if (!text[i]) {
                if (!c->nullable || c->primary_key) {
                    snprintf(msg, sizeof(msg), c->primary_key ? "Primary key '%.64s' cannot be NULL" : "Column '%.64s' cannot be NULL", c->name);
                    fail = msg;
                    break;
                }
                continue;
            }
            if (check_value(c, text[i], msg, sizeof(msg))) { fail = msg; break; }
        }
        if (fail) break;
        tuples[r] = tuple_from_strings(text, nc, ts);
        if (!tuples[r]) { fail = "Out of memory"; break; }
        if (pk >= 0) {                                            // chiave primaria univoca
            char key[EV_STR_MAX + 1];
            mi_key(&tuples[r]->values[pk], ts->columns[pk].type, key, sizeof(key));
            const RidList* l = mi_lookup(pk_ix, key);
            SMSlot* sl = sm_slot(&batch, key, true);
            if (!sl) { fail = "Out of memory"; break; }
            if ((l && l->n > 0) || sl->val >= 0) {
                snprintf(msg, sizeof(msg), "Duplicate entry '%.60s' for primary key '%.64s'", key, ts->columns[pk].name);
                fail = msg;
                break;
            }
            sl->val = r;
        }
    }
    sm_free(&batch);
    if (fail) {
        for (int r = 0; r < nrows; r++) if (tuples[r]) tuple_free(tuples[r]);
        free(tuples);
        return result_error_msg(fail);
    }

    // scrittura
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    int done = 0;
    if (hf) {
        for (int r = 0; r < nrows; r++) {
            RID rid;
            if (heap_insert(hf, tuples[r], ts, &rid) != SV_OK) break;
            wal_log(ctx, LOG_INSERT, rid, tuples[r], NULL, ts);
            mi_on_insert(db->name, ts, tuples[r], rid);
            ts->num_rows++;
            done++;
        }
        ts->num_pages = hf->num_pages;
        free(hf);
    }
    for (int r = 0; r < nrows; r++) tuple_free(tuples[r]);
    free(tuples);
    if (done < nrows) {
        mi_invalidate_table(db->name, ts->name);
        snprintf(msg, sizeof(msg), "Failed to insert row %d of %d (the previous rows were inserted)", done + 1, nrows);
        return result_error_msg(msg);
    }
    return result_ok_msg(done);
}

// righe della tabella che soddisfano la WHERE (con l'indice quando possibile)
static const char* select_matches(ExecCtx* ctx, Database* db, Source* s, ASTExpr* where, EvalCtx* ec,
                                  int** out_idx, int* out_n) {
    char plan[8];
    if (!source_load(ctx, db->name, s, where, plan, sizeof(plan))) return "Failed to read the table";
    int* idx = malloc(sizeof(int) * (size_t)(s->n ? s->n : 1));
    if (!idx) return "Out of memory";
    int n = 0;
    Tuple* cur[1];
    ec->src = s;
    ec->nsrc = 1;
    ec->cur = cur;
    for (int i = 0; i < s->n; i++) {
        cur[0] = s->rows[i];
        if (eval_cond(where, ec)) idx[n++] = i;
    }
    ec->cur = NULL;
    if (ec->err[0]) { free(idx); return ec->err; }
    *out_idx = idx;
    *out_n = n;
    return NULL;
}

static QueryResult* exec_update(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->update_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->update_stmt.table_name)) return result_error_msg("Cannot UPDATE a view");
        return result_error_msg("Table not found");
    }
    char msg[300];
    int set_ci[SV_MAX_SETS];
    for (int k = 0; k < n->update_stmt.num_sets; k++) {
        set_ci[k] = catalog_get_col_index(ts, n->update_stmt.set_cols[k]);
        if (set_ci[k] < 0) return unknown_column_error(n->update_stmt.set_cols[k], "SET");
    }
    Source s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", ts->name);
    snprintf(s.table, sizeof(s.table), "%s", ts->name);
    s.ts = ts;
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.src = &s; b.nsrc = 1; b.mode = BIND_ROW;
    b.clause = "SET";
    for (int k = 0; k < n->update_stmt.num_sets; k++) bind_expr(n->update_stmt.set_exprs[k], &b);
    b.clause = "WHERE";
    bind_expr(n->update_stmt.where, &b);
    if (b.err[0]) return result_error_msg(b.err);

    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    int* idx = NULL;
    int nm = 0;
    const char* fail = select_matches(ctx, db, &s, n->update_stmt.where, &ec, &idx, &nm);
    if (fail) { snprintf(msg, sizeof(msg), "%s", fail); source_free(&s); return result_error_msg(msg); }

    // nuove righe: tutte le espressioni usano i valori precedenti della riga
    Tuple** nt = calloc((size_t)(nm ? nm : 1), sizeof(Tuple*));
    if (!nt) { free(idx); source_free(&s); return result_error_msg("Out of memory"); }
    Tuple* cur[1];
    ec.src = &s; ec.nsrc = 1; ec.cur = cur;
    for (int i = 0; i < nm && !fail; i++) {
        Tuple* old = s.rows[idx[i]];
        cur[0] = old;
        nt[i] = tuple_clone(old, ts);
        if (!nt[i]) { fail = "Out of memory"; break; }
        for (int k = 0; k < n->update_stmt.num_sets; k++) {
            int ci = set_ci[k];
            Column* c = &ts->columns[ci];
            EvalVal v;
            eval_expr(n->update_stmt.set_exprs[k], &ec, &v);
            if (ec.err[0]) { fail = ec.err; break; }
            const char* text = ev_store_text(&v);
            if (!text && (!c->nullable || c->primary_key)) {
                snprintf(msg, sizeof(msg), "Column '%.64s' cannot be NULL", c->name);
                fail = msg;
                break;
            }
            if (text && check_value(c, text, msg, sizeof(msg))) { fail = msg; break; }
            value_set_from_text(&nt[i]->values[ci], c->type, text);
        }
    }
    ec.cur = NULL;

    // chiave primaria: univoca fra le righe modificate e rispetto a tutte le altre
    int pk = pk_column(ts);
    bool pk_changed = false;
    for (int k = 0; k < n->update_stmt.num_sets; k++) if (set_ci[k] == pk) pk_changed = true;
    if (!fail && pk >= 0 && pk_changed && nm > 0) {
        MemIndex* ix = mi_get(ctx, db->name, ts, pk);
        StrMap mine = {0}, newkeys = {0};
        if (!ix) fail = "Out of memory (primary key index)";
        for (int i = 0; !fail && i < nm; i++) {
            char rk[32];
            rid_key(s.rids[idx[i]], rk, sizeof(rk));
            if (!sm_slot(&mine, rk, true)) fail = "Out of memory";
        }
        for (int i = 0; !fail && i < nm; i++) {
            char key[EV_STR_MAX + 1];
            mi_key(&nt[i]->values[pk], ts->columns[pk].type, key, sizeof(key));
            SMSlot* sl = sm_slot(&newkeys, key, true);
            if (!sl) { fail = "Out of memory"; break; }
            bool dup = sl->val >= 0;
            sl->val = i;
            const RidList* l = mi_lookup(ix, key);
            for (int q = 0; !dup && l && q < l->n; q++) {
                char rk[32];
                rid_key(l->r[q], rk, sizeof(rk));
                if (!sm_slot(&mine, rk, false)) dup = true;       // la chiave appartiene a una riga non modificata
            }
            if (dup) {
                snprintf(msg, sizeof(msg), "Duplicate entry '%.60s' for primary key '%.64s'", key, ts->columns[pk].name);
                fail = msg;
            }
        }
        sm_free(&mine);
        sm_free(&newkeys);
    }

    int affected = 0;
    if (!fail) {
        HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
        if (!hf) fail = "Failed to open the table";
        for (int i = 0; hf && i < nm; i++) {
            RID rid = s.rids[idx[i]], nrid = rid;
            Tuple* old = s.rows[idx[i]];
            if (heap_update(hf, rid, nt[i], ts, &nrid) != SV_OK) {
                mi_invalidate_table(db->name, ts->name);
                snprintf(msg, sizeof(msg), "Failed to update row %d of %d (the previous rows were updated)", i + 1, nm);
                fail = msg;
                break;
            }
            wal_log(ctx, LOG_UPDATE, rid, nt[i], old, ts);
            mi_on_delete(db->name, ts, old, rid);
            mi_on_insert(db->name, ts, nt[i], nrid);
            affected++;
        }
        if (hf) { ts->num_pages = hf->num_pages; free(hf); }
    }
    for (int i = 0; i < nm; i++) if (nt[i]) tuple_free(nt[i]);
    free(nt);
    free(idx);
    source_free(&s);
    if (fail) { if (fail != msg) snprintf(msg, sizeof(msg), "%s", fail); return result_error_msg(msg); }
    return result_ok_msg(affected);
}

static QueryResult* exec_delete(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) {
        if (catalog_get_view(ctx->catalog, db->name, n->delete_stmt.table_name)) return result_error_msg("Cannot DELETE from a view");
        return result_error_msg("Table not found");
    }
    Source s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", ts->name);
    snprintf(s.table, sizeof(s.table), "%s", ts->name);
    s.ts = ts;
    BindCtx b;
    memset(&b, 0, sizeof(b));
    b.src = &s; b.nsrc = 1; b.mode = BIND_ROW; b.clause = "WHERE";
    bind_expr(n->delete_stmt.where, &b);
    if (b.err[0]) return result_error_msg(b.err);

    EvalCtx ec;
    memset(&ec, 0, sizeof(ec));
    int* idx = NULL;
    int nm = 0;
    char msg[300];
    const char* fail = select_matches(ctx, db, &s, n->delete_stmt.where, &ec, &idx, &nm);
    if (fail) { snprintf(msg, sizeof(msg), "%s", fail); source_free(&s); return result_error_msg(msg); }

    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    int affected = 0;
    for (int i = 0; hf && i < nm; i++) {
        RID rid = s.rids[idx[i]];
        Tuple* old = s.rows[idx[i]];
        wal_log(ctx, LOG_DELETE, rid, NULL, old, ts);
        if (heap_delete(hf, rid) != SV_OK) { mi_invalidate_table(db->name, ts->name); continue; }
        mi_on_delete(db->name, ts, old, rid);
        if (ts->num_rows > 0) ts->num_rows--;
        affected++;
    }
    free(hf);
    free(idx);
    source_free(&s);
    return result_ok_msg(affected);
}

static QueryResult* exec_use(ExecCtx* ctx, ASTNode* n) {
    if (catalog_use_db(ctx->catalog, n->use_stmt.db_name) != SV_OK)
        return result_error_msg("Database not found");
    return result_ok_msg(0);
}

static QueryResult* exec_show(ExecCtx* ctx, ASTNode* n) {
    bool tables = strcmp(n->select_stmt.table_name, "TABLES") == 0;
    Database* db = catalog_current_db(ctx->catalog);
    if (tables && !db) return result_error_msg("No database selected");
    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;

    if (!tables) {
        r->num_cols = 1;
        r->col_names = calloc(1, sizeof(char*));
        r->col_names[0] = strdup("Database");
        r->num_rows = ctx->catalog->num_databases;
        r->rows = calloc(r->num_rows, sizeof(Row));
        for (int i = 0; i < r->num_rows; i++) {
            r->rows[i].num_cells = 1;
            r->rows[i].cells = calloc(1, sizeof(Cell));
            snprintf(r->rows[i].cells[0].value, sizeof(r->rows[i].cells[0].value),
                     "%s", ctx->catalog->databases[i].name);
        }
    } else {
        r->num_cols = 1;
        r->col_names = calloc(1, sizeof(char*));
        r->col_names[0] = strdup("Tables");
        r->num_rows = db->num_tables;
        r->rows = calloc(r->num_rows, sizeof(Row));
        for (int i = 0; i < r->num_rows; i++) {
            r->rows[i].num_cells = 1;
            r->rows[i].cells = calloc(1, sizeof(Cell));
            snprintf(r->rows[i].cells[0].value, sizeof(r->rows[i].cells[0].value),
                     "%s", db->tables[i].name);
        }
    }
    return r;
}

static QueryResult* exec_describe(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    const char* tname = n->select_stmt.table_name;
    if (!tname[0]) return result_error_msg("No table specified");

    int num_cols;
    ColumnInfo* cols = db_list_columns(db->name, tname, &num_cols);
    if (!cols) return result_error_msg("Table not found");

    QueryResult* r = calloc(1, sizeof(QueryResult));
    r->success = true;
    r->num_cols = 4;
    r->col_names = calloc(4, sizeof(char*));
    r->col_names[0] = strdup("Field");
    r->col_names[1] = strdup("Type");
    r->col_names[2] = strdup("Null");
    r->col_names[3] = strdup("Key");
    r->num_rows = num_cols;
    r->rows = calloc(num_cols, sizeof(Row));
    for (int i = 0; i < num_cols; i++) {
        r->rows[i].num_cells = 4;
        r->rows[i].cells = calloc(4, sizeof(Cell));
        snprintf(r->rows[i].cells[0].value, sizeof(r->rows[i].cells[0].value), "%s", cols[i].name);
        snprintf(r->rows[i].cells[1].value, sizeof(r->rows[i].cells[1].value), "%s", cols[i].type);
        snprintf(r->rows[i].cells[2].value, sizeof(r->rows[i].cells[2].value), "%s", cols[i].nullable ? "YES" : "NO");
        snprintf(r->rows[i].cells[3].value, sizeof(r->rows[i].cells[3].value), "%s", cols[i].primary_key ? "PRI" : "");
    }
    db_columns_free(cols, num_cols);
    return r;
}

static QueryResult* exec_create_index(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");

    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->create_index.table_name);
    if (!ts) return result_error_msg("Table not found");

    if (catalog_get_col_index(ts, n->create_index.col_name) < 0)
        return unknown_column_error(n->create_index.col_name, "CREATE INDEX");
    for (int i = 0; i < ts->num_indexes; i++)
        if (!strcasecmp(ts->indexes[i].name, n->create_index.index_name))
            return result_error_msg("An index with this name already exists on the table");
    if (ts->num_indexes >= SV_MAX_INDEXES) return result_error_msg("Too many indexes on this table (max 16)");

    Index idx;
    memset(&idx, 0, sizeof(idx));
    snprintf(idx.name, sizeof(idx.name), "%s", n->create_index.index_name);
    snprintf(idx.col_name, sizeof(idx.col_name), "%s", n->create_index.col_name);
    snprintf(idx.type, sizeof(idx.type), "BTREE");

    if (catalog_add_index(ctx->catalog, db->name, n->create_index.table_name, &idx) != SV_OK)
        return result_error_msg("Failed to create index");

    // l'indice in memoria viene costruito dalle righe al primo utilizzo
    mi_invalidate_table(db->name, ts->name);

    return result_ok_msg(0);
}

static QueryResult* exec_drop_index(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->drop_index.table_name);
    if (!ts) return result_error_msg("Table not found");
    for (int i = 0; i < ts->num_indexes; i++) {
        if (strcasecmp(ts->indexes[i].name, n->drop_index.index_name) == 0) {
            mi_invalidate_table(db->name, ts->name);
            memmove(&ts->indexes[i], &ts->indexes[i+1], (ts->num_indexes - i - 1) * sizeof(Index));
            ts->num_indexes--;
            return result_ok_msg(0);
        }
    }
    return result_error_msg("Index not found");
}

static QueryResult* exec_create_view(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    ViewSchema vs;
    memset(&vs, 0, sizeof(vs));
    snprintf(vs.name, sizeof(vs.name), "%s", n->create_view.view_name);
    snprintf(vs.definition, sizeof(vs.definition), "%s", n->create_view.definition);
    if (catalog_get_table(ctx->catalog, db->name, vs.name) || catalog_get_view(ctx->catalog, db->name, vs.name)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Table or view '%.64s' already exists", vs.name);
        return result_error_msg(msg);
    }
    if (catalog_add_view(ctx->catalog, db->name, &vs) != SV_OK)
        return result_error_msg("Failed to create view (too many views?)");
    return result_ok_msg(0);
}

static QueryResult* exec_drop_view(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    if (catalog_drop_view(ctx->catalog, db->name, n->drop_view.view_name) != SV_OK)
        return result_error_msg("View not found");
    return result_ok_msg(0);
}

static QueryResult* exec_truncate(ExecCtx* ctx, ASTNode* n) {
    Database* db = catalog_current_db(ctx->catalog);
    if (!db) return result_error_msg("No database selected");
    TableSchema* ts = catalog_get_table(ctx->catalog, db->name, n->delete_stmt.table_name);
    if (!ts) return result_error_msg("Table not found");
    // Delete all rows
    HeapFile* hf = heap_open(ctx->bp, ts->root_page_id, ts->num_pages);
    if (!hf) return result_error_msg("Failed to open heap");
    int affected = 0;
    HeapScan* scan = heap_scan_open(hf, ts);
    if (!scan) { free(hf); return result_error_msg("Failed to scan"); }
    RID rid;
    Tuple* t;
    while ((t = heap_scan_next(scan, &rid)) != NULL) {
        heap_delete(hf, rid);
        affected++;
        tuple_free(t);
    }
    heap_scan_close(scan);
    ts->num_rows = 0;
    free(hf);
    mi_invalidate_table(db->name, ts->name);
    return result_ok_msg(affected);
}

static QueryResult* exec_explain(ExecCtx* ctx, ASTNode* n) {
    ASTNode* stmt = (ASTNode*)n->explain.stmt;
    if (!stmt || stmt->type != NODE_SELECT) return result_error_msg("EXPLAIN works with SELECT statements");
    return run_select(ctx, stmt, 0, true);
}

QueryResult* executor_run(ExecCtx* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return result_error_msg("Invalid executor context");

    switch (stmt->type) {
        case NODE_CREATE_DB:    return exec_create_db(ctx, stmt);
        case NODE_DROP_DB:      return exec_drop_db(ctx, stmt);
        case NODE_CREATE_TABLE: return exec_create_table(ctx, stmt);
        case NODE_DROP_TABLE:   return exec_drop_table(ctx, stmt);
        case NODE_ALTER_TABLE:  return exec_alter_table(ctx, stmt);
        case NODE_INSERT:       return exec_insert(ctx, stmt);
        case NODE_SELECT:       return run_select(ctx, stmt, 0, false);
        case NODE_UPDATE:       return exec_update(ctx, stmt);
        case NODE_DELETE:       return exec_delete(ctx, stmt);
        case NODE_USE:          return exec_use(ctx, stmt);
        case NODE_SHOW:         return exec_show(ctx, stmt);
        case NODE_DESCRIBE:     return exec_describe(ctx, stmt);
        case NODE_CREATE_INDEX: return exec_create_index(ctx, stmt);
        case NODE_DROP_INDEX:   return exec_drop_index(ctx, stmt);
        case NODE_CREATE_VIEW:  return exec_create_view(ctx, stmt);
        case NODE_DROP_VIEW:    return exec_drop_view(ctx, stmt);
        case NODE_EXPLAIN:      return exec_explain(ctx, stmt);
        case NODE_TRUNCATE:     return exec_truncate(ctx, stmt);
        case NODE_BEGIN:
        case NODE_COMMIT:
        case NODE_ROLLBACK:
            return result_ok_msg(0);
        case NODE_SAVEPOINT:    return result_ok_msg(0);
        case NODE_ROLLBACK_TO:  return result_ok_msg(0);
        case NODE_CREATE_TRIGGER: return result_ok_msg(0);
        case NODE_DROP_TRIGGER: return result_ok_msg(0);
        default:
            return result_error_msg("Unsupported statement type");
    }
}
