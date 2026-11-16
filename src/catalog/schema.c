#include "schema.h"
#include <string.h>
#include <stddef.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform_compat.h"
#include "sv_alloc.h"

int datatype_size(DataType t) {
    switch (t) {
        case SV_TYPE_INT:      return 4;
        case SV_TYPE_BIGINT:   return 8;
        case SV_TYPE_FLOAT:    return 8;
        case SV_TYPE_BOOL:     return 1;
        // DATE, DATETIME, DECIMAL, BLOB, VARCHAR, CHAR, TEXT: memorizzati come testo
        // con prefisso di lunghezza (dimensione variabile)
        default:               return 0;
    }
}

const char* datatype_name(DataType t) {
    switch (t) {
        case SV_TYPE_INT:      return "INT";
        case SV_TYPE_BIGINT:   return "BIGINT";
        case SV_TYPE_FLOAT:    return "FLOAT";
        case SV_TYPE_VARCHAR:  return "VARCHAR";
        case SV_TYPE_CHAR:     return "CHAR";
        case SV_TYPE_TEXT:     return "TEXT";
        case SV_TYPE_BOOL:     return "BOOL";
        case SV_TYPE_DATE:     return "DATE";
        case SV_TYPE_DATETIME: return "DATETIME";
        case SV_TYPE_BLOB:     return "BLOB";
        case SV_TYPE_DECIMAL:  return "DECIMAL";
        default:               return "UNKNOWN";
    }
}

// ── tabelle: memoria delle colonne ───────────────────────────────

TableSchema* schema_new(int cap) {
    if (cap < 0) cap = 0;
    TableSchema* t = sv_try_calloc(1, sizeof(TableSchema) + sizeof(Column) * (size_t)cap);
    if (!t) return NULL;
    t->columns = cap ? (Column*)(t + 1) : NULL;
    t->cap_columns = cap;
    t->own_columns = 0;
    return t;
}

bool schema_assign(TableSchema* dst, const TableSchema* src) {
    if (!dst || !src) return false;
    if (dst == src) return true;
    int n = src->num_columns > 0 ? src->num_columns : 0;
    if (n > dst->cap_columns) {
        if (!dst->own_columns && dst->columns) return false;          // colonne nel blocco: non crescono
        int nc = dst->cap_columns ? dst->cap_columns : 8;
        while (nc < n) nc *= 2;
        Column* nv = sv_try_realloc(dst->own_columns ? dst->columns : NULL, sizeof(Column) * (size_t)nc);
        if (!nv) return false;
        dst->columns = nv;
        dst->cap_columns = nc;
        dst->own_columns = 1;
    }
    Column* cols = dst->columns;
    int cap = dst->cap_columns;
    uint8_t own = dst->own_columns;
    memmove(cols ? cols : (Column*)NULL, src->columns, sizeof(Column) * (size_t)n);   // prima delle altre: src può essere dst
    *dst = *src;
    dst->columns = cols;
    dst->cap_columns = cap;
    dst->own_columns = own;
    dst->num_columns = n;
    return true;
}

TableSchema* schema_dup(const TableSchema* src, int extra) {
    if (!src) return NULL;
    TableSchema* t = schema_new(src->num_columns + (extra > 0 ? extra : 0));
    if (t && !schema_assign(t, src)) { free(t); return NULL; }
    return t;
}

Column* schema_add_column(TableSchema* ts) {
    if (!ts || ts->num_columns >= SV_MAX_COLUMNS) return NULL;
    if (ts->num_columns >= ts->cap_columns) {
        if (!ts->own_columns && ts->columns) return NULL;
        int nc = ts->cap_columns ? ts->cap_columns * 2 : 16;
        Column* nv = sv_try_realloc(ts->own_columns ? ts->columns : NULL, sizeof(Column) * (size_t)nc);
        if (!nv) return NULL;
        ts->columns = nv;
        ts->cap_columns = nc;
        ts->own_columns = 1;
    }
    Column* c = &ts->columns[ts->num_columns++];
    memset(c, 0, sizeof(*c));
    return c;
}

void schema_release(TableSchema* ts) {
    if (!ts) return;
    if (ts->own_columns) free(ts->columns);
    ts->columns = NULL;
    ts->cap_columns = 0;
    ts->own_columns = 0;
}

// tabella del catalogo: struttura e colonne sue
static TableSchema* table_alloc(void) {
    TableSchema* t = sv_try_calloc(1, sizeof(TableSchema));
    if (t) t->own_columns = 1;
    return t;
}

static void table_free(TableSchema* t) {
    if (!t) return;
    schema_release(t);
    free(t);
}

// ── liste dinamiche ──────────────────────────────────────────────

static bool grow(void** arr, int* cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int nc = *cap ? *cap : 8;
    while (nc < need) nc *= 2;
    void* nv = sv_try_realloc(*arr, elem * (size_t)nc);
    if (!nv) return false;
    memset((uint8_t*)nv + elem * (size_t)*cap, 0, elem * (size_t)(nc - *cap));
    *arr = nv;
    *cap = nc;
    return true;
}

static void db_clear(Database* d) {
    for (int i = 0; i < d->num_tables; i++) table_free(d->tables[i]);
    free(d->tables);
    free(d->views);
    free(d->routines);
    free(d->triggers);
    memset(d, 0, sizeof(*d));
}

RoutineSchema* catalog_new_routine(Database* d) {
    if (!d || !grow((void**)&d->routines, &d->cap_routines, d->num_routines + 1, sizeof(RoutineSchema))) return NULL;
    RoutineSchema* r = &d->routines[d->num_routines++];
    memset(r, 0, sizeof(*r));
    return r;
}

TriggerSchema* catalog_new_trigger(Database* d) {
    if (!d || !grow((void**)&d->triggers, &d->cap_triggers, d->num_triggers + 1, sizeof(TriggerSchema))) return NULL;
    TriggerSchema* g = &d->triggers[d->num_triggers++];
    memset(g, 0, sizeof(*g));
    return g;
}

// tabella nuova in fondo al database (copia profonda di src); NULL senza memoria
static TableSchema* db_push_table(Database* d, const TableSchema* src) {
    if (!grow((void**)&d->tables, &d->cap_tables, d->num_tables + 1, sizeof(TableSchema*))) return NULL;
    TableSchema* t = table_alloc();
    if (!t) return NULL;
    if (src && !schema_assign(t, src)) { table_free(t); return NULL; }
    d->tables[d->num_tables++] = t;
    return t;
}

static Database* cat_push_db(Catalog* c) {
    if (!grow((void**)&c->databases, &c->cap_databases, c->num_databases + 1, sizeof(Database))) return NULL;
    Database* d = &c->databases[c->num_databases++];
    memset(d, 0, sizeof(*d));
    return d;
}

Catalog* catalog_create(void) {
    Catalog* c = (Catalog*)sv_try_calloc(1, sizeof(Catalog));
    if (!c) return NULL;
    c->num_databases = 0;
    c->current_db    = -1;
    c->next_page_id  = 1;
    return c;
}

void catalog_free(Catalog* c) {
    if (!c) return;
    for (int i = 0; i < c->num_databases; i++) db_clear(&c->databases[i]);
    free(c->databases);
    free(c);
}

// ── persistenza del catalogo ─────────────────────────────────────
// Formato v6 (SegVault 1.4, P1.11): ogni campo scritto esplicitamente (interi little-endian a larghezza fissa,
// testi come lunghezza + byte), indipendente dalla disposizione delle struct in memoria e dal compilatore.
//   "SVCAT2\0\0"  uint32 versione  int32 num_databases  int32 current_db  uint32 next_page_id
//   per ogni database: testo nome, poi tabelle, viste, routine, trigger (ognuno con il suo numero davanti)
//   in coda "CRC1" + CRC-32 di tutto quello che precede.
// Le versioni 1..5 (le struct C scritte così come erano in memoria) si leggono ancora; il prossimo
// salvataggio le riscrive nel formato v6.

#define CAT_MAGIC   "SVCAT2\0\0"
#define CAT_VERSION 6u   // v3: il flag "nullable" delle colonne è significativo (NOT NULL applicato)
                         // v4: vincoli (PRIMARY KEY composta, UNIQUE, FOREIGN KEY, CHECK) in coda a TableSchema
                         // v5: tipo SQL originale e flag delle colonne, colonne degli indici, collation
                         // v6: codifica esplicita, liste dinamiche (più di 64 colonne / 256 tabelle / 64 database)

// ── formati precedenti ──
// v5: la TableSchema di SegVault 1.3 (64 colonne dentro la struttura)
typedef struct {
    char      name[SV_MAX_NAME_LEN];
    Column    columns[64];
    int       num_columns;
    Index     indexes[SV_MAX_INDEXES];
    int       num_indexes;
    uint32_t  root_page_id;
    uint32_t  num_pages;
    uint64_t  num_rows;
    char      comment[256];
    int       num_constraints;
    Constraint constraints[SV_MAX_CONSTRAINTS];
    char      collation[40];
    uint64_t  auto_increment;
} TableSchemaV5;
_Static_assert(sizeof(TableSchemaV5) == 62736, "TableSchemaV5: disposizione del formato v5 del catalogo");

// v1..v4: stesse strutture senza i campi della v5
typedef struct {
    char name[SV_MAX_NAME_LEN]; DataType type; int size; int decimal_prec;
    bool nullable, primary_key, auto_increment, unique, has_default;
    char default_val[128]; char comment[256];
} ColumnV4;
typedef struct {
    char name[SV_MAX_NAME_LEN]; char col_name[SV_MAX_NAME_LEN]; bool unique; char type[16]; uint32_t root_page_id;
} IndexV4;
typedef struct {
    char name[SV_MAX_NAME_LEN];
    ColumnV4 columns[64]; int num_columns;
    IndexV4 indexes[SV_MAX_INDEXES]; int num_indexes;
    uint32_t root_page_id, num_pages; uint64_t num_rows; char comment[256];
    int num_constraints; Constraint constraints[SV_MAX_CONSTRAINTS];
} TableSchemaV4;

// TableSchema fino alla v3: gli stessi campi della v4 senza i vincoli in coda
#define TABLE_V3_SIZE offsetof(TableSchemaV4, num_constraints)

static bool table_from_v5(TableSchema* t, const TableSchemaV5* o) {
    int n = o->num_columns >= 0 && o->num_columns <= 64 ? o->num_columns : 0;
    TableSchema* tmp = schema_new(n);
    if (!tmp) return false;
    memcpy(tmp->name, o->name, sizeof(tmp->name));
    memcpy(tmp->columns, o->columns, sizeof(Column) * (size_t)n);
    tmp->num_columns = n;
    memcpy(tmp->indexes, o->indexes, sizeof(tmp->indexes));
    tmp->num_indexes = o->num_indexes;
    tmp->root_page_id = o->root_page_id; tmp->num_pages = o->num_pages; tmp->num_rows = o->num_rows;
    memcpy(tmp->comment, o->comment, sizeof(tmp->comment));
    tmp->num_constraints = o->num_constraints;
    memcpy(tmp->constraints, o->constraints, sizeof(tmp->constraints));
    memcpy(tmp->collation, o->collation, sizeof(tmp->collation));
    tmp->auto_increment = o->auto_increment;
    bool ok = schema_assign(t, tmp);
    free(tmp);
    return ok;
}

static bool table_from_v4(TableSchema* t, const TableSchemaV4* o) {
    TableSchemaV5* v = sv_try_calloc(1, sizeof(TableSchemaV5));
    if (!v) return false;
    memcpy(v->name, o->name, sizeof(v->name));
    v->num_columns = o->num_columns >= 0 && o->num_columns <= 64 ? o->num_columns : 0;
    for (int i = 0; i < 64; i++) {
        Column* c = &v->columns[i];
        const ColumnV4* oc = &o->columns[i];
        memcpy(c->name, oc->name, sizeof(c->name));
        c->type = oc->type; c->size = oc->size; c->decimal_prec = oc->decimal_prec;
        c->nullable = oc->nullable; c->primary_key = oc->primary_key; c->auto_increment = oc->auto_increment;
        c->unique = oc->unique; c->has_default = oc->has_default;
        memcpy(c->default_val, oc->default_val, sizeof(c->default_val));
        memcpy(c->comment, oc->comment, sizeof(c->comment));
    }
    v->num_indexes = o->num_indexes >= 0 && o->num_indexes <= SV_MAX_INDEXES ? o->num_indexes : 0;
    for (int i = 0; i < SV_MAX_INDEXES; i++) {
        Index* x = &v->indexes[i];
        const IndexV4* ox = &o->indexes[i];
        memcpy(x->name, ox->name, sizeof(x->name));
        memcpy(x->col_name, ox->col_name, sizeof(x->col_name));
        x->unique = ox->unique;
        memcpy(x->type, ox->type, sizeof(x->type));
        x->root_page_id = ox->root_page_id;
    }
    v->root_page_id = o->root_page_id; v->num_pages = o->num_pages; v->num_rows = o->num_rows;
    memcpy(v->comment, o->comment, sizeof(v->comment));
    v->num_constraints = o->num_constraints >= 0 && o->num_constraints <= SV_MAX_CONSTRAINTS ? o->num_constraints : 0;
    memcpy(v->constraints, o->constraints, sizeof(v->constraints));
    bool ok = table_from_v5(t, v);
    free(v);
    return ok;
}

// formato più vecchio (v1): la struct Catalog intera, con le TableSchema senza vincoli
typedef struct { uint8_t raw[TABLE_V3_SIZE]; } TableSchemaV3;
typedef struct {
    char           name[SV_MAX_NAME_LEN];
    TableSchemaV3  tables[256];
    int            num_tables;
    ViewSchema     views[64];
    int            num_views;
    RoutineSchema  routines[64];
    int            num_routines;
    TriggerSchema  triggers[64];
    int            num_triggers;
} DatabaseV1;
typedef struct {
    DatabaseV1  databases[64];
    int         num_databases;
    int         current_db;
    uint32_t    next_page_id;
} CatalogV1;

// CRC-32 (IEEE) di tutto il file, in coda: "CRC1" + crc. I file scritti prima della 1.3 non l'hanno
// (accettati così); le versioni precedenti leggono i campi e ignorano la coda
static uint32_t g_crc;
static uint32_t crc32_upd(uint32_t c, const void* buf, size_t n) {
    const uint8_t* p = buf;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// lettura/scrittura complete (read/write possono trasferire meno byte del richiesto)
static bool read_all(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t total = n;
    while (n > 0) {
        unsigned int chunk = n > (1u << 24) ? (1u << 24) : (unsigned int)n;
        int r = (int)sv_read(fd, p, chunk);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    g_crc = crc32_upd(g_crc, buf, total);
    return true;
}

static char g_load_err[400];
const char* catalog_load_error(void) { return g_load_err; }

// nomi e testi letti dal file: sempre terminati (un file danneggiato non fa leggere oltre il campo)
static void nul_end(char* s, size_t n) { s[n - 1] = '\0'; }

static bool table_sane(TableSchema* t) {
    if (t->num_columns < 0 || t->num_columns > SV_MAX_COLUMNS || t->num_indexes < 0 || t->num_indexes > SV_MAX_INDEXES ||
        t->num_constraints < 0 || t->num_constraints > SV_MAX_CONSTRAINTS) return false;
    nul_end(t->name, sizeof(t->name));
    nul_end(t->comment, sizeof(t->comment));
    nul_end(t->collation, sizeof(t->collation));
    for (int c = 0; c < t->num_columns; c++) {
        Column* col = &t->columns[c];
        nul_end(col->name, sizeof(col->name));
        nul_end(col->comment, sizeof(col->comment));
        nul_end(col->default_val, sizeof(col->default_val));
        nul_end(col->sqltype, sizeof(col->sqltype));
    }
    for (int i = 0; i < t->num_indexes; i++) {
        Index* x = &t->indexes[i];
        nul_end(x->name, sizeof(x->name));
        nul_end(x->col_name, sizeof(x->col_name));
        nul_end(x->type, sizeof(x->type));
        if (x->ncols > 8) return false;
        for (int k = 0; k < 8; k++) nul_end(x->cols[k], sizeof(x->cols[k]));
    }
    for (int i = 0; i < t->num_constraints; i++) {
        Constraint* k = &t->constraints[i];
        if (k->ncols > SV_MAX_KEY_COLS) return false;
        nul_end(k->name, sizeof(k->name));
        nul_end(k->ref_table, sizeof(k->ref_table));
        nul_end(k->check, sizeof(k->check));
        for (int j = 0; j < SV_MAX_KEY_COLS; j++) nul_end(k->ref_cols[j], sizeof(k->ref_cols[j]));
    }
    return true;
}

static void db_sane(Database* db) {
    nul_end(db->name, sizeof(db->name));
    for (int i = 0; i < db->num_views; i++) { nul_end(db->views[i].name, sizeof(db->views[i].name)); nul_end(db->views[i].definition, sizeof(db->views[i].definition)); }
    for (int i = 0; i < db->num_routines; i++) {
        RoutineSchema* r = &db->routines[i];
        nul_end(r->name, sizeof(r->name)); nul_end(r->body, sizeof(r->body)); nul_end(r->return_type, sizeof(r->return_type));
    }
    for (int i = 0; i < db->num_triggers; i++) {
        TriggerSchema* g = &db->triggers[i];
        nul_end(g->name, sizeof(g->name)); nul_end(g->table_name, sizeof(g->table_name));
        nul_end(g->event, sizeof(g->event)); nul_end(g->body, sizeof(g->body));
    }
}

static bool read_i32(int fd, int32_t* v) { return read_all(fd, v, sizeof(*v)); }

// ── formato v6: lettore e scrittore con buffer ──

typedef struct { uint8_t* b; size_t n, cap; bool oom; } WBuf;

static void wb_raw(WBuf* w, const void* p, size_t n) {
    if (w->oom) return;
    if (w->n + n > w->cap) {
        size_t nc = w->cap ? w->cap : 65536;
        while (nc < w->n + n) nc *= 2;
        uint8_t* nb = sv_try_realloc(w->b, nc);
        if (!nb) { w->oom = true; return; }
        w->b = nb;
        w->cap = nc;
    }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void wb_u8(WBuf* w, uint8_t v) { wb_raw(w, &v, 1); }
static void wb_u32(WBuf* w, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; wb_raw(w, b, 4); }
static void wb_i32(WBuf* w, int32_t v) { wb_u32(w, (uint32_t)v); }
static void wb_u64(WBuf* w, uint64_t v) { wb_u32(w, (uint32_t)v); wb_u32(w, (uint32_t)(v >> 32)); }
static void wb_str(WBuf* w, const char* s) { size_t l = s ? strlen(s) : 0; wb_u32(w, (uint32_t)l); wb_raw(w, s, l); }

typedef struct { const uint8_t* b; size_t n, at; bool bad; } RBuf;

static void rb_raw(RBuf* r, void* p, size_t n) {
    if (r->bad || r->n - r->at < n) { r->bad = true; if (p && n) memset(p, 0, n); return; }
    memcpy(p, r->b + r->at, n);
    r->at += n;
}
static uint8_t rb_u8(RBuf* r) { uint8_t v = 0; rb_raw(r, &v, 1); return v; }
static uint32_t rb_u32(RBuf* r) { uint8_t b[4]; rb_raw(r, b, 4); return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static int32_t rb_i32(RBuf* r) { return (int32_t)rb_u32(r); }
static uint64_t rb_u64(RBuf* r) { uint64_t lo = rb_u32(r); return lo | (uint64_t)rb_u32(r) << 32; }
// testo nel campo dst di dstn byte (troncato se più lungo: mai oltre il campo)
static void rb_str(RBuf* r, char* dst, size_t dstn) {
    uint32_t l = rb_u32(r);
    if (r->bad || r->n - r->at < l) { r->bad = true; if (dstn) dst[0] = '\0'; return; }
    size_t k = l < dstn ? l : dstn - 1;
    memcpy(dst, r->b + r->at, k);
    dst[k] = '\0';
    r->at += l;
}

static void w_table(WBuf* w, const TableSchema* t) {
    wb_str(w, t->name);
    wb_u32(w, (uint32_t)t->num_columns);
    for (int i = 0; i < t->num_columns; i++) {
        const Column* c = &t->columns[i];
        wb_str(w, c->name); wb_i32(w, (int32_t)c->type); wb_i32(w, c->size); wb_i32(w, c->decimal_prec);
        wb_u8(w, (uint8_t)((c->nullable ? 1 : 0) | (c->primary_key ? 2 : 0) | (c->auto_increment ? 4 : 0) |
                           (c->unique ? 8 : 0) | (c->has_default ? 16 : 0)));
        wb_str(w, c->default_val); wb_str(w, c->comment); wb_str(w, c->sqltype); wb_u8(w, c->flags);
    }
    wb_u32(w, (uint32_t)t->num_indexes);
    for (int i = 0; i < t->num_indexes; i++) {
        const Index* x = &t->indexes[i];
        wb_str(w, x->name); wb_str(w, x->col_name); wb_u8(w, x->unique ? 1 : 0); wb_str(w, x->type);
        wb_u32(w, x->root_page_id); wb_u8(w, x->ncols); wb_u8(w, x->fulltext);
        for (int k = 0; k < 8; k++) wb_str(w, x->cols[k]);
    }
    wb_u32(w, t->root_page_id); wb_u32(w, t->num_pages); wb_u64(w, t->num_rows); wb_str(w, t->comment);
    wb_u32(w, (uint32_t)t->num_constraints);
    for (int i = 0; i < t->num_constraints; i++) {
        const Constraint* k = &t->constraints[i];
        wb_str(w, k->name); wb_u8(w, k->kind); wb_u8(w, k->ncols); wb_u8(w, k->on_delete); wb_u8(w, k->on_update);
        wb_raw(w, k->cols, SV_MAX_KEY_COLS);
        wb_str(w, k->ref_table);
        for (int j = 0; j < SV_MAX_KEY_COLS; j++) wb_str(w, k->ref_cols[j]);
        wb_str(w, k->check);
    }
    wb_str(w, t->collation);
    wb_u64(w, t->auto_increment);
}

static bool r_table(RBuf* r, TableSchema* t) {
    rb_str(r, t->name, sizeof(t->name));
    uint32_t nc = rb_u32(r);
    if (r->bad || nc > SV_MAX_COLUMNS) return false;
    t->num_columns = 0;
    for (uint32_t i = 0; i < nc; i++) {
        Column* c = schema_add_column(t);
        if (!c) return false;
        rb_str(r, c->name, sizeof(c->name));
        c->type = (DataType)rb_i32(r); c->size = rb_i32(r); c->decimal_prec = rb_i32(r);
        uint8_t f = rb_u8(r);
        c->nullable = f & 1; c->primary_key = (f & 2) != 0; c->auto_increment = (f & 4) != 0; c->unique = (f & 8) != 0; c->has_default = (f & 16) != 0;
        rb_str(r, c->default_val, sizeof(c->default_val)); rb_str(r, c->comment, sizeof(c->comment));
        rb_str(r, c->sqltype, sizeof(c->sqltype)); c->flags = rb_u8(r);
    }
    uint32_t ni = rb_u32(r);
    if (r->bad || ni > SV_MAX_INDEXES) return false;
    t->num_indexes = (int)ni;
    for (uint32_t i = 0; i < ni; i++) {
        Index* x = &t->indexes[i];
        rb_str(r, x->name, sizeof(x->name)); rb_str(r, x->col_name, sizeof(x->col_name)); x->unique = rb_u8(r) != 0;
        rb_str(r, x->type, sizeof(x->type)); x->root_page_id = rb_u32(r); x->ncols = rb_u8(r); x->fulltext = rb_u8(r);
        for (int k = 0; k < 8; k++) rb_str(r, x->cols[k], sizeof(x->cols[k]));
        if (x->ncols > 8) return false;
    }
    t->root_page_id = rb_u32(r); t->num_pages = rb_u32(r); t->num_rows = rb_u64(r); rb_str(r, t->comment, sizeof(t->comment));
    uint32_t nk = rb_u32(r);
    if (r->bad || nk > SV_MAX_CONSTRAINTS) return false;
    t->num_constraints = (int)nk;
    for (uint32_t i = 0; i < nk; i++) {
        Constraint* k = &t->constraints[i];
        rb_str(r, k->name, sizeof(k->name)); k->kind = rb_u8(r); k->ncols = rb_u8(r); k->on_delete = rb_u8(r); k->on_update = rb_u8(r);
        rb_raw(r, k->cols, SV_MAX_KEY_COLS);
        rb_str(r, k->ref_table, sizeof(k->ref_table));
        for (int j = 0; j < SV_MAX_KEY_COLS; j++) rb_str(r, k->ref_cols[j], sizeof(k->ref_cols[j]));
        rb_str(r, k->check, sizeof(k->check));
        if (k->ncols > SV_MAX_KEY_COLS) return false;
    }
    rb_str(r, t->collation, sizeof(t->collation));
    t->auto_increment = rb_u64(r);
    return !r->bad;
}

static void w_db(WBuf* w, const Database* d) {
    wb_str(w, d->name);
    wb_u32(w, (uint32_t)d->num_tables);
    for (int t = 0; t < d->num_tables; t++) w_table(w, d->tables[t]);
    wb_u32(w, (uint32_t)d->num_views);
    for (int i = 0; i < d->num_views; i++) { wb_str(w, d->views[i].name); wb_str(w, d->views[i].definition); }
    wb_u32(w, (uint32_t)d->num_routines);
    for (int i = 0; i < d->num_routines; i++) {
        const RoutineSchema* r = &d->routines[i];
        wb_str(w, r->name); wb_u8(w, r->is_function ? 1 : 0); wb_str(w, r->body); wb_str(w, r->return_type);
    }
    wb_u32(w, (uint32_t)d->num_triggers);
    for (int i = 0; i < d->num_triggers; i++) {
        const TriggerSchema* g = &d->triggers[i];
        wb_str(w, g->name); wb_str(w, g->table_name); wb_u8(w, g->before ? 1 : 0); wb_str(w, g->event); wb_str(w, g->body);
    }
}

static bool r_db(RBuf* r, Database* d) {
    rb_str(r, d->name, sizeof(d->name));
    uint32_t nt = rb_u32(r);
    if (r->bad || nt > SV_MAX_TABLES) return false;
    for (uint32_t t = 0; t < nt; t++) {
        TableSchema* ts = db_push_table(d, NULL);
        if (!ts || !r_table(r, ts)) return false;
    }
    uint32_t nv = rb_u32(r);
    if (r->bad || nv > (1u << 20) || !grow((void**)&d->views, &d->cap_views, (int)nv, sizeof(ViewSchema))) return false;
    for (uint32_t i = 0; i < nv; i++) { rb_str(r, d->views[i].name, sizeof(d->views[i].name)); rb_str(r, d->views[i].definition, sizeof(d->views[i].definition)); }
    d->num_views = (int)nv;
    uint32_t nr = rb_u32(r);
    if (r->bad || nr > (1u << 20) || !grow((void**)&d->routines, &d->cap_routines, (int)nr, sizeof(RoutineSchema))) return false;
    for (uint32_t i = 0; i < nr; i++) {
        RoutineSchema* x = &d->routines[i];
        rb_str(r, x->name, sizeof(x->name)); x->is_function = rb_u8(r) != 0; rb_str(r, x->body, sizeof(x->body)); rb_str(r, x->return_type, sizeof(x->return_type));
    }
    d->num_routines = (int)nr;
    uint32_t ng = rb_u32(r);
    if (r->bad || ng > (1u << 20) || !grow((void**)&d->triggers, &d->cap_triggers, (int)ng, sizeof(TriggerSchema))) return false;
    for (uint32_t i = 0; i < ng; i++) {
        TriggerSchema* g = &d->triggers[i];
        rb_str(r, g->name, sizeof(g->name)); rb_str(r, g->table_name, sizeof(g->table_name)); g->before = rb_u8(r) != 0;
        rb_str(r, g->event, sizeof(g->event)); rb_str(r, g->body, sizeof(g->body));
    }
    d->num_triggers = (int)ng;
    return !r->bad;
}

// file v6 già letto in memoria (dopo l'intestazione di 12 byte): i database
static bool load_v6(Catalog* c, const uint8_t* b, size_t n, int32_t ndb, int32_t cur, uint32_t next_page) {
    RBuf r = { b, n, 0, false };
    c->next_page_id = next_page;
    for (int32_t i = 0; i < ndb; i++) {
        Database* d = cat_push_db(c);
        if (!d || !r_db(&r, d)) return false;
        db_sane(d);
        for (int t = 0; t < d->num_tables; t++) if (!table_sane(d->tables[t])) return false;
    }
    c->current_db = (cur >= -1 && cur < ndb) ? cur : -1;
    return !r.bad && r.at == n;
}

Catalog* catalog_load(const char* path) {
    g_load_err[0] = '\0';
    int fd = sv_open(path, SV_O_RDONLY, 0);
    if (fd < 0) {
        // nessun catalogo: database nuovo, oppure un salvataggio interrotto fra la scrittura del file
        // temporaneo e la sostituzione (il temporaneo è completo: ha il suo fsync e il CRC)
        char tmp[1100];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        int tf = sv_open(tmp, SV_O_RDONLY, 0);
        if (tf < 0) return catalog_create();
        sv_close(tf);
        Catalog* c = catalog_load(tmp);
        if (c && sv_replace_file(tmp, path) != 0) fprintf(stderr, "[SegVault] cannot rename %s\n", tmp);
        return c;
    }

    Catalog* c = catalog_create();
    if (!c) { sv_close(fd); snprintf(g_load_err, sizeof(g_load_err), "out of memory"); return NULL; }

    char magic[8];
    bool migrate_nullable = false, crc_ok = true;
    g_crc = 0;
    bool ok = read_all(fd, magic, sizeof(magic));
    if (ok && memcmp(magic, CAT_MAGIC, 8) == 0) {
        uint32_t version = 0, next_page = 1;
        int32_t ndb = 0, cur = -1;
        ok = read_all(fd, &version, 4);
        if (ok && version > CAT_VERSION) {            // mai riscritto da una versione che non lo capisce
            snprintf(g_load_err, sizeof(g_load_err), "the catalog was created by a newer SegVault (format %u, this version reads up to %u)",
                     version, CAT_VERSION);
            sv_close(fd);
            catalog_free(c);
            return NULL;
        }
        ok = ok && version >= 2u &&
             read_i32(fd, &ndb) && read_i32(fd, &cur) && read_all(fd, &next_page, 4) &&
             ndb >= 0 && ndb <= SV_MAX_DATABASES;
        if (ok && version >= 6u) {
            // il resto del file in memoria: database, poi la coda CRC1 (8 byte)
            int64_t end = sv_lseek(fd, 0, SEEK_END), here = 8 + 4 + 12;
            ok = end >= here + 8 && sv_lseek(fd, here, SEEK_SET) == here;
            size_t body = ok ? (size_t)(end - here - 8) : 0;
            uint8_t* b = ok ? sv_try_malloc(body ? body : 1) : NULL;
            ok = ok && b && (body == 0 || read_all(fd, b, body));
            uint32_t want = g_crc;
            uint8_t tail[8];
            if (ok && sv_read(fd, tail, 8) == 8 && !memcmp(tail, "CRC1", 4)) {
                uint32_t got;
                memcpy(&got, tail + 4, 4);
                crc_ok = got == want;
            } else if (ok) crc_ok = false;
            if (ok && crc_ok) ok = load_v6(c, b, body, ndb, cur, next_page);
            free(b);
        } else if (ok) {
            migrate_nullable = version < 3u;
            c->next_page_id  = next_page;
            for (int d = 0; ok && d < ndb; d++) {
                Database* db = cat_push_db(c);
                int32_t nt, nv, nr, ntr;
                ok = db && read_all(fd, db->name, sizeof(db->name)) &&
                     read_i32(fd, &nt) && read_i32(fd, &nv) && read_i32(fd, &nr) && read_i32(fd, &ntr) &&
                     nt >= 0 && nt <= 256 && nv >= 0 && nv <= 64 && nr >= 0 && nr <= 64 && ntr >= 0 && ntr <= 64;
                if (!ok) break;
                if (version >= 5u) {
                    TableSchemaV5* old = sv_try_malloc(sizeof(TableSchemaV5));
                    ok = old != NULL;
                    for (int t = 0; ok && t < nt; t++) {
                        TableSchema* ts = db_push_table(db, NULL);
                        ok = ts && read_all(fd, old, sizeof(TableSchemaV5)) && table_from_v5(ts, old) && table_sane(ts);
                    }
                    free(old);
                } else {
                    TableSchemaV4* old = sv_try_calloc(1, sizeof(TableSchemaV4));
                    ok = old != NULL;
                    for (int t = 0; ok && t < nt; t++) {
                        memset(old, 0, sizeof(*old));
                        TableSchema* ts = db_push_table(db, NULL);
                        ok = ts && read_all(fd, old, version >= 4u ? sizeof(TableSchemaV4) : TABLE_V3_SIZE) && table_from_v4(ts, old);   // v3: senza vincoli
                    }
                    free(old);
                }
                ok = ok && grow((void**)&db->views, &db->cap_views, nv, sizeof(ViewSchema)) &&
                     grow((void**)&db->routines, &db->cap_routines, nr, sizeof(RoutineSchema)) &&
                     grow((void**)&db->triggers, &db->cap_triggers, ntr, sizeof(TriggerSchema)) &&
                     read_all(fd, db->views,    sizeof(ViewSchema)    * (size_t)nv) &&
                     read_all(fd, db->routines, sizeof(RoutineSchema) * (size_t)nr) &&
                     read_all(fd, db->triggers, sizeof(TriggerSchema) * (size_t)ntr);
                if (ok) { db->num_views = nv; db->num_routines = nr; db->num_triggers = ntr; db_sane(db); }
            }
            if (ok) c->current_db = (cur >= -1 && cur < ndb) ? cur : -1;
            // coda: "CRC1" + CRC-32 di tutto quello che precede (assente nei file di prima della 1.3)
            if (ok) {
                uint32_t want = g_crc;
                uint8_t tail[8];
                long r = (long)sv_read(fd, tail, 8);
                if (r == 8 && !memcmp(tail, "CRC1", 4)) {
                    uint32_t got;
                    memcpy(&got, tail + 4, 4);
                    crc_ok = got == want;
                    uint8_t extra;
                    if (crc_ok && sv_read(fd, &extra, 1) > 0) crc_ok = false;     // byte dopo la coda
                } else if (r != 0) crc_ok = false;                                 // byte che non c'entrano
            }
        }
    } else if (ok) {
        // formato precedente: la struct Catalog intera (TableSchema senza vincoli)
        CatalogV1* old = sv_try_calloc(1, sizeof(CatalogV1));
        ok = old != NULL;
        if (ok) {
            memcpy(old, magic, sizeof(magic));
            ok = read_all(fd, (uint8_t*)old + sizeof(magic), sizeof(CatalogV1) - sizeof(magic)) &&
                 old->num_databases >= 0 && old->num_databases <= 64;
        }
        for (int d = 0; ok && d < old->num_databases; d++) {
            DatabaseV1* od = &old->databases[d];
            Database* nd = cat_push_db(c);
            if (!nd || od->num_tables < 0 || od->num_tables > 256 || od->num_views < 0 || od->num_views > 64 ||
                od->num_routines < 0 || od->num_routines > 64 || od->num_triggers < 0 || od->num_triggers > 64) { ok = false; break; }
            memcpy(nd->name, od->name, sizeof(nd->name));
            for (int t = 0; ok && t < od->num_tables; t++) {
                TableSchemaV4 tmp;
                memset(&tmp, 0, sizeof(tmp));
                memcpy(&tmp, od->tables[t].raw, TABLE_V3_SIZE);
                TableSchema* ts = db_push_table(nd, NULL);
                ok = ts && table_from_v4(ts, &tmp);
            }
            ok = ok && grow((void**)&nd->views, &nd->cap_views, od->num_views, sizeof(ViewSchema)) &&
                 grow((void**)&nd->routines, &nd->cap_routines, od->num_routines, sizeof(RoutineSchema)) &&
                 grow((void**)&nd->triggers, &nd->cap_triggers, od->num_triggers, sizeof(TriggerSchema));
            if (!ok) break;
            memcpy(nd->views, od->views, sizeof(ViewSchema) * (size_t)od->num_views);          nd->num_views = od->num_views;
            memcpy(nd->routines, od->routines, sizeof(RoutineSchema) * (size_t)od->num_routines); nd->num_routines = od->num_routines;
            memcpy(nd->triggers, od->triggers, sizeof(TriggerSchema) * (size_t)od->num_triggers); nd->num_triggers = od->num_triggers;
            db_sane(nd);
        }
        if (ok) {
            c->current_db = old->current_db >= -1 && old->current_db < c->num_databases ? old->current_db : -1;
            c->next_page_id = old->next_page_id;
        }
        free(old);
        migrate_nullable = true;
    }
    sv_close(fd);

    // fino alla v2 "nullable" restava sempre false (NOT NULL non era applicato): ogni colonna
    // ammette NULL tranne la chiave primaria, come si comportava il database in pratica
    if (ok && migrate_nullable) {
        for (int d = 0; d < c->num_databases; d++)
            for (int t = 0; t < c->databases[d].num_tables; t++) {
                TableSchema* ts = c->databases[d].tables[t];
                for (int k = 0; k < ts->num_columns; k++) ts->columns[k].nullable = !ts->columns[k].primary_key;
            }
    }

    // Un catalogo illeggibile non diventa mai uno vuoto: il prossimo salvataggio cancellerebbe ogni
    // database (le pagine sono ancora in segvault.db). Il motore non parte e il file resta com'è.
    if (!ok || !crc_ok) {
        snprintf(g_load_err, sizeof(g_load_err), "the catalog %.150s is %s: it was left unchanged (restore it from a backup, "
                 "or move it away to start with an empty catalog)", path, !ok ? "unreadable or truncated" : "damaged (checksum mismatch)");
        catalog_free(c);
        return NULL;
    }
    return c;
}

uint8_t* catalog_serialize(const Catalog* c, size_t* out_n) {
    WBuf w = { NULL, 0, 0, false };
    uint32_t version = CAT_VERSION, next_page = c->next_page_id;
    wb_raw(&w, CAT_MAGIC, 8);
    wb_u32(&w, version);
    wb_i32(&w, c->num_databases);
    wb_i32(&w, c->current_db);
    wb_u32(&w, next_page);
    for (int d = 0; d < c->num_databases; d++) w_db(&w, &c->databases[d]);
    uint32_t crc = crc32_upd(0, w.b, w.n);
    wb_raw(&w, "CRC1", 4);
    wb_u32(&w, crc);
    if (w.oom) { free(w.b); return NULL; }
    *out_n = w.n;
    return w.b;
}

int catalog_save(Catalog* c, const char* path) {
    if (!c || !path) return SV_ERR;
    size_t n = 0;
    uint8_t* b = catalog_serialize(c, &n);
    if (!b) return SV_ERR;
    // scrittura su file temporaneo e sostituzione: un errore a metà non rovina il catalogo esistente
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = sv_open(tmp, SV_O_WRONLY | SV_O_CREAT | SV_O_TRUNC, 0600);
    if (fd < 0) { free(b); return SV_IO_ERROR; }
    bool ok = sv_write_full(fd, b, n);
    free(b);
    if (ok) ok = sv_fsync(fd) == 0;
    sv_close(fd);
    if (!ok) { remove(tmp); return SV_IO_ERROR; }
    if (sv_replace_file(tmp, path) != 0) { remove(tmp); return SV_IO_ERROR; }
    return SV_OK;
}

Database* catalog_get_db(Catalog* c, const char* name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->num_databases; i++)
        if (strcasecmp(c->databases[i].name, name) == 0)
            return &c->databases[i];
    return NULL;
}

int catalog_add_db(Catalog* c, const char* name) {
    if (!c || !name) return SV_ERR;
    if (c->num_databases >= SV_MAX_DATABASES) return SV_FULL;
    if (catalog_get_db(c, name)) return SV_DUPLICATE;
    Database* db = cat_push_db(c);
    if (!db) return SV_FULL;
    strncpy(db->name, name, SV_MAX_NAME_LEN - 1);
    if (c->current_db < 0) c->current_db = c->num_databases - 1;
    return SV_OK;
}

int catalog_drop_db(Catalog* c, const char* name) {
    if (!c || !name) return SV_ERR;
    for (int i = 0; i < c->num_databases; i++) {
        if (strcasecmp(c->databases[i].name, name) == 0) {
            if (c->current_db == i) c->current_db = -1;
            else if (c->current_db > i) c->current_db--;
            db_clear(&c->databases[i]);
            memmove(&c->databases[i], &c->databases[i + 1],
                    (size_t)(c->num_databases - i - 1) * sizeof(Database));
            c->num_databases--;
            return SV_OK;
        }
    }
    return SV_NOT_FOUND;
}

int catalog_use_db(Catalog* c, const char* name) {
    if (!c || !name) return SV_ERR;
    for (int i = 0; i < c->num_databases; i++)
        if (strcasecmp(c->databases[i].name, name) == 0) {
            c->current_db = i;
            return SV_OK;
        }
    return SV_NOT_FOUND;
}

// database corrente di questo thread (letture in parallelo: ognuna il suo); -2 = quello del catalogo
static _Thread_local int g_thread_db = -2;

void catalog_set_thread_db(int index) { g_thread_db = index; }
int  catalog_get_thread_db(void) { return g_thread_db; }

Database* catalog_current_db(Catalog* c) {
    int i = g_thread_db != -2 ? g_thread_db : c ? c->current_db : -1;
    if (!c || i < 0 || i >= c->num_databases)
        return NULL;
    return &c->databases[i];
}

TableSchema* catalog_get_table(Catalog* c, const char* db, const char* table) {
    if (!c || !db || !table) return NULL;
    Database* d = catalog_get_db(c, db);
    if (!d) return NULL;
    for (int i = 0; i < d->num_tables; i++)
        if (strcasecmp(d->tables[i]->name, table) == 0)
            return d->tables[i];
    return NULL;
}

int catalog_add_table(Catalog* c, const char* db, TableSchema* schema) {
    if (!c || !db || !schema) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    if (d->num_tables >= SV_MAX_TABLES) return SV_FULL;
    if (catalog_get_table(c, db, schema->name)) return SV_DUPLICATE;
    if (catalog_get_view(c, db, schema->name)) return SV_DUPLICATE;   // stesso nome di una vista

    schema->root_page_id = c->next_page_id++;
    return db_push_table(d, schema) ? SV_OK : SV_FULL;
}

int catalog_drop_table(Catalog* c, const char* db, const char* table) {
    if (!c || !db || !table) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    for (int i = 0; i < d->num_tables; i++) {
        if (strcasecmp(d->tables[i]->name, table) == 0) {
            table_free(d->tables[i]);
            memmove(&d->tables[i], &d->tables[i + 1], (size_t)(d->num_tables - i - 1) * sizeof(TableSchema*));
            d->num_tables--;
            return SV_OK;
        }
    }
    return SV_NOT_FOUND;
}

ViewSchema* catalog_get_view(Catalog* c, const char* db, const char* name) {
    if (!c || !db || !name) return NULL;
    Database* d = catalog_get_db(c, db);
    if (!d) return NULL;
    for (int i = 0; i < d->num_views; i++)
        if (strcasecmp(d->views[i].name, name) == 0)
            return &d->views[i];
    return NULL;
}

int catalog_add_view(Catalog* c, const char* db, ViewSchema* schema) {
    if (!c || !db || !schema) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    if (catalog_get_view(c, db, schema->name)) return SV_DUPLICATE;
    if (catalog_get_table(c, db, schema->name)) return SV_DUPLICATE;  // stesso nome di una tabella
    if (!grow((void**)&d->views, &d->cap_views, d->num_views + 1, sizeof(ViewSchema))) return SV_FULL;
    d->views[d->num_views++] = *schema;
    return SV_OK;
}

int catalog_drop_view(Catalog* c, const char* db, const char* name) {
    if (!c || !db || !name) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    for (int i = 0; i < d->num_views; i++) {
        if (strcasecmp(d->views[i].name, name) == 0) {
            memmove(&d->views[i], &d->views[i + 1],
                    (size_t)(d->num_views - i - 1) * sizeof(ViewSchema));
            d->num_views--;
            return SV_OK;
        }
    }
    return SV_NOT_FOUND;
}

int catalog_add_index(Catalog* c, const char* db, const char* table, Index* idx) {
    if (!c || !db || !table || !idx) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    TableSchema* t = catalog_get_table(c, db, table);
    if (!t) return SV_NOT_FOUND;
    if (t->num_indexes >= SV_MAX_INDEXES) return SV_FULL;
    t->indexes[t->num_indexes++] = *idx;
    return SV_OK;
}

int catalog_get_col_index(TableSchema* t, const char* col_name) {
    if (!t || !col_name) return -1;
    for (int i = 0; i < t->num_columns; i++)
        if (strcasecmp(t->columns[i].name, col_name) == 0)
            return i;
    return -1;
}
