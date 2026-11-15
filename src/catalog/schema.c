#include "schema.h"
#include <string.h>
#include <stddef.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform_compat.h"

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

Catalog* catalog_create(void) {
    Catalog* c = SV_ALLOC(Catalog);
    if (!c) return NULL;
    c->num_databases = 0;
    c->current_db    = -1;
    c->next_page_id  = 1;
    return c;
}

// ── persistenza del catalogo ─────────────────────────────────────
// Formato v2 (compatto): solo database e oggetti presenti.
//   "SVCAT2\0\0"  uint32 versione  int32 num_databases  int32 current_db  uint32 next_page_id
//   per ogni database: name[SV_MAX_NAME_LEN]  int32 num_tables num_views num_routines num_triggers
//                      seguiti dalle strutture TableSchema / ViewSchema / RoutineSchema / TriggerSchema
// Il formato precedente era la struct Catalog intera (~1,3 GB, quasi tutta vuota): viene ancora
// letto e alla prima chiusura riscritto nel formato v2.

#define CAT_MAGIC   "SVCAT2\0\0"
#define CAT_VERSION 5u   // v3: il flag "nullable" delle colonne è significativo (NOT NULL applicato)
                         // v4: vincoli (PRIMARY KEY composta, UNIQUE, FOREIGN KEY, CHECK) in coda a TableSchema
                         // v5: tipo SQL originale e flag delle colonne, colonne degli indici, collation

// ── formati precedenti (v1..v4): stesse strutture senza i campi della v5 ──
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
    ColumnV4 columns[SV_MAX_COLUMNS]; int num_columns;
    IndexV4 indexes[SV_MAX_INDEXES]; int num_indexes;
    uint32_t root_page_id, num_pages; uint64_t num_rows; char comment[256];
    int num_constraints; Constraint constraints[SV_MAX_CONSTRAINTS];
} TableSchemaV4;

// TableSchema fino alla v3: gli stessi campi della v4 senza i vincoli in coda
#define TABLE_V3_SIZE offsetof(TableSchemaV4, num_constraints)

static void table_from_v4(TableSchema* t, const TableSchemaV4* o) {
    memset(t, 0, sizeof(*t));
    memcpy(t->name, o->name, sizeof(t->name));
    t->num_columns = o->num_columns >= 0 && o->num_columns <= SV_MAX_COLUMNS ? o->num_columns : 0;
    for (int i = 0; i < SV_MAX_COLUMNS; i++) {
        Column* c = &t->columns[i];
        const ColumnV4* oc = &o->columns[i];
        memcpy(c->name, oc->name, sizeof(c->name));
        c->type = oc->type; c->size = oc->size; c->decimal_prec = oc->decimal_prec;
        c->nullable = oc->nullable; c->primary_key = oc->primary_key; c->auto_increment = oc->auto_increment;
        c->unique = oc->unique; c->has_default = oc->has_default;
        memcpy(c->default_val, oc->default_val, sizeof(c->default_val));
        memcpy(c->comment, oc->comment, sizeof(c->comment));
    }
    t->num_indexes = o->num_indexes >= 0 && o->num_indexes <= SV_MAX_INDEXES ? o->num_indexes : 0;
    for (int i = 0; i < SV_MAX_INDEXES; i++) {
        Index* x = &t->indexes[i];
        const IndexV4* ox = &o->indexes[i];
        memcpy(x->name, ox->name, sizeof(x->name));
        memcpy(x->col_name, ox->col_name, sizeof(x->col_name));
        x->unique = ox->unique;
        memcpy(x->type, ox->type, sizeof(x->type));
        x->root_page_id = ox->root_page_id;
    }
    t->root_page_id = o->root_page_id; t->num_pages = o->num_pages; t->num_rows = o->num_rows;
    memcpy(t->comment, o->comment, sizeof(t->comment));
    t->num_constraints = o->num_constraints >= 0 && o->num_constraints <= SV_MAX_CONSTRAINTS ? o->num_constraints : 0;
    memcpy(t->constraints, o->constraints, sizeof(t->constraints));
}

// formato più vecchio (v1): la struct Catalog intera, con le TableSchema senza vincoli
typedef struct { uint8_t raw[TABLE_V3_SIZE]; } TableSchemaV3;
typedef struct {
    char           name[SV_MAX_NAME_LEN];
    TableSchemaV3  tables[SV_MAX_TABLES];
    int            num_tables;
    ViewSchema     views[64];
    int            num_views;
    RoutineSchema  routines[64];
    int            num_routines;
    TriggerSchema  triggers[64];
    int            num_triggers;
} DatabaseV1;
typedef struct {
    DatabaseV1  databases[SV_MAX_DATABASES];
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

static bool write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    g_crc = crc32_upd(g_crc, buf, n);
    while (n > 0) {
        unsigned int chunk = n > (1u << 24) ? (1u << 24) : (unsigned int)n;
        int w = (int)sv_write(fd, p, chunk);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
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

    Catalog* c = SV_ALLOC(Catalog);
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
            free(c);
            return NULL;
        }
        ok = ok && version >= 2u &&
             read_i32(fd, &ndb) && read_i32(fd, &cur) && read_all(fd, &next_page, 4) &&
             ndb >= 0 && ndb <= SV_MAX_DATABASES;
        if (ok) {
            migrate_nullable = version < 3u;
            c->num_databases = ndb;
            c->current_db    = (cur >= -1 && cur < ndb) ? cur : -1;
            c->next_page_id  = next_page;
        }
        for (int d = 0; ok && d < ndb; d++) {
            Database* db = &c->databases[d];
            int32_t nt, nv, nr, ntr;
            ok = read_all(fd, db->name, sizeof(db->name)) &&
                 read_i32(fd, &nt) && read_i32(fd, &nv) && read_i32(fd, &nr) && read_i32(fd, &ntr) &&
                 nt >= 0 && nt <= SV_MAX_TABLES && nv >= 0 && nv <= 64 &&
                 nr >= 0 && nr <= 64 && ntr >= 0 && ntr <= 64;
            if (!ok) break;
            db->num_tables = nt; db->num_views = nv; db->num_routines = nr; db->num_triggers = ntr;
            if (version >= 5u) {
                ok = read_all(fd, db->tables, sizeof(TableSchema) * (size_t)nt);
                for (int t = 0; ok && t < nt; t++) ok = table_sane(&db->tables[t]);
            }
            else {
                TableSchemaV4* old = calloc(1, sizeof(TableSchemaV4));
                ok = old != NULL;
                for (int t = 0; ok && t < nt; t++) {
                    memset(old, 0, sizeof(*old));
                    ok = read_all(fd, old, version >= 4u ? sizeof(TableSchemaV4) : TABLE_V3_SIZE);   // v3: senza vincoli
                    if (ok) table_from_v4(&db->tables[t], old);
                }
                free(old);
            }
            ok = ok &&
                 read_all(fd, db->views,    sizeof(ViewSchema)    * (size_t)nv) &&
                 read_all(fd, db->routines, sizeof(RoutineSchema) * (size_t)nr) &&
                 read_all(fd, db->triggers, sizeof(TriggerSchema) * (size_t)ntr);
            if (ok) db_sane(db);
        }
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
    } else if (ok) {
        // formato precedente: la struct Catalog intera (TableSchema senza vincoli)
        CatalogV1* old = calloc(1, sizeof(CatalogV1));
        ok = old != NULL;
        if (ok) {
            memcpy(old, magic, sizeof(magic));
            ok = read_all(fd, (uint8_t*)old + sizeof(magic), sizeof(CatalogV1) - sizeof(magic)) &&
                 old->num_databases >= 0 && old->num_databases <= SV_MAX_DATABASES;
        }
        for (int d = 0; ok && d < old->num_databases; d++) {
            DatabaseV1* od = &old->databases[d];
            Database* nd = &c->databases[d];
            if (od->num_tables < 0 || od->num_tables > SV_MAX_TABLES) { ok = false; break; }
            memcpy(nd->name, od->name, sizeof(nd->name));
            nd->num_tables = od->num_tables;
            for (int t = 0; t < od->num_tables; t++) {
                TableSchemaV4 tmp;
                memset(&tmp, 0, sizeof(tmp));
                memcpy(&tmp, od->tables[t].raw, TABLE_V3_SIZE);
                table_from_v4(&nd->tables[t], &tmp);
            }
            memcpy(nd->views, od->views, sizeof(nd->views));       nd->num_views = od->num_views;
            memcpy(nd->routines, od->routines, sizeof(nd->routines)); nd->num_routines = od->num_routines;
            memcpy(nd->triggers, od->triggers, sizeof(nd->triggers)); nd->num_triggers = od->num_triggers;
        }
        if (ok) {
            c->num_databases = old->num_databases;
            c->current_db = old->current_db;
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
                TableSchema* ts = &c->databases[d].tables[t];
                for (int k = 0; k < ts->num_columns; k++) ts->columns[k].nullable = !ts->columns[k].primary_key;
            }
    }

    // Un catalogo illeggibile non diventa mai uno vuoto: il prossimo salvataggio cancellerebbe ogni
    // database (le pagine sono ancora in segvault.db). Il motore non parte e il file resta com'è.
    if (!ok || !crc_ok) {
        snprintf(g_load_err, sizeof(g_load_err), "the catalog %.150s is %s: it was left unchanged (restore it from a backup, "
                 "or move it away to start with an empty catalog)", path, !ok ? "unreadable or truncated" : "damaged (checksum mismatch)");
        free(c);
        return NULL;
    }
    return c;
}

int catalog_save(Catalog* c, const char* path) {
    if (!c || !path) return SV_ERR;
    // scrittura su file temporaneo e sostituzione: un errore a metà non rovina il catalogo esistente
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = sv_open(tmp, SV_O_WRONLY | SV_O_CREAT | SV_O_TRUNC, 0600);
    if (fd < 0) return SV_IO_ERROR;

    uint32_t version = CAT_VERSION, next_page = c->next_page_id;
    int32_t ndb = c->num_databases, cur = c->current_db;
    g_crc = 0;
    bool ok = write_all(fd, CAT_MAGIC, 8) && write_all(fd, &version, 4) &&
              write_all(fd, &ndb, 4) && write_all(fd, &cur, 4) && write_all(fd, &next_page, 4);
    for (int d = 0; ok && d < c->num_databases; d++) {
        Database* db = &c->databases[d];
        int32_t n[4] = { db->num_tables, db->num_views, db->num_routines, db->num_triggers };
        ok = write_all(fd, db->name, sizeof(db->name)) && write_all(fd, n, sizeof(n)) &&
             write_all(fd, db->tables,   sizeof(TableSchema)   * (size_t)db->num_tables) &&
             write_all(fd, db->views,    sizeof(ViewSchema)    * (size_t)db->num_views) &&
             write_all(fd, db->routines, sizeof(RoutineSchema) * (size_t)db->num_routines) &&
             write_all(fd, db->triggers, sizeof(TriggerSchema) * (size_t)db->num_triggers);
    }
    if (ok) {
        uint8_t tail[8];
        uint32_t crc = g_crc;
        memcpy(tail, "CRC1", 4);
        memcpy(tail + 4, &crc, 4);
        ok = write_all(fd, tail, 8);
    }
    if (ok) ok = sv_fsync(fd) == 0;
    sv_close(fd);
    if (!ok) { remove(tmp); return SV_IO_ERROR; }
    if (sv_replace_file(tmp, path) != 0) { remove(tmp); return SV_IO_ERROR; }
    return SV_OK;
}

void catalog_free(Catalog* c) { free(c); }

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

    Database* db = &c->databases[c->num_databases++];
    memset(db, 0, sizeof(Database));
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
            memmove(&c->databases[i], &c->databases[i + 1],
                    (c->num_databases - i - 1) * sizeof(Database));
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
        if (strcasecmp(d->tables[i].name, table) == 0)
            return &d->tables[i];
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
    d->tables[d->num_tables++] = *schema;
    return SV_OK;
}

int catalog_drop_table(Catalog* c, const char* db, const char* table) {
    if (!c || !db || !table) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    for (int i = 0; i < d->num_tables; i++) {
        if (strcasecmp(d->tables[i].name, table) == 0) {
            memmove(&d->tables[i], &d->tables[i + 1],
                    (d->num_tables - i - 1) * sizeof(TableSchema));
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
    if (d->num_views >= 64) return SV_FULL;
    if (catalog_get_view(c, db, schema->name)) return SV_DUPLICATE;
    if (catalog_get_table(c, db, schema->name)) return SV_DUPLICATE;  // stesso nome di una tabella
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
                    (d->num_views - i - 1) * sizeof(ViewSchema));
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
