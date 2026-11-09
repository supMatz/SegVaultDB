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
#define CAT_VERSION 4u   // v3: il flag "nullable" delle colonne è significativo (NOT NULL applicato)
                         // v4: vincoli (PRIMARY KEY composta, UNIQUE, FOREIGN KEY, CHECK) in coda a TableSchema

// TableSchema fino alla v3: gli stessi campi senza i vincoli in coda
#define TABLE_V3_SIZE offsetof(TableSchema, num_constraints)

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

// lettura/scrittura complete (read/write possono trasferire meno byte del richiesto)
static bool read_all(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        unsigned int chunk = n > (1u << 24) ? (1u << 24) : (unsigned int)n;
        int r = (int)sv_read(fd, p, chunk);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n > 0) {
        unsigned int chunk = n > (1u << 24) ? (1u << 24) : (unsigned int)n;
        int w = (int)sv_write(fd, p, chunk);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_i32(int fd, int32_t* v) { return read_all(fd, v, sizeof(*v)); }

Catalog* catalog_load(const char* path) {
    int fd = sv_open(path, SV_O_RDONLY, 0);
    if (fd < 0) return catalog_create();

    Catalog* c = SV_ALLOC(Catalog);
    if (!c) { sv_close(fd); return NULL; }

    char magic[8];
    bool migrate_nullable = false;
    bool ok = read_all(fd, magic, sizeof(magic));
    if (ok && memcmp(magic, CAT_MAGIC, 8) == 0) {
        uint32_t version = 0, next_page = 1;
        int32_t ndb = 0, cur = -1;
        ok = read_all(fd, &version, 4) && version >= 2u && version <= CAT_VERSION &&
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
            if (version >= 4u) ok = read_all(fd, db->tables, sizeof(TableSchema) * (size_t)nt);
            else for (int t = 0; ok && t < nt; t++) ok = read_all(fd, &db->tables[t], TABLE_V3_SIZE);   // senza vincoli
            ok = ok &&
                 read_all(fd, db->views,    sizeof(ViewSchema)    * (size_t)nv) &&
                 read_all(fd, db->routines, sizeof(RoutineSchema) * (size_t)nr) &&
                 read_all(fd, db->triggers, sizeof(TriggerSchema) * (size_t)ntr);
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
            for (int t = 0; t < od->num_tables; t++) memcpy(&nd->tables[t], od->tables[t].raw, TABLE_V3_SIZE);
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

    if (!ok) {
        fprintf(stderr, "[SegVault] catalog '%s' is unreadable or truncated: starting with an empty catalog\n", path);
        free(c);
        return catalog_create();
    }
    return c;
}

int catalog_save(Catalog* c, const char* path) {
    if (!c || !path) return SV_ERR;
    // scrittura su file temporaneo e sostituzione: un errore a metà non rovina il catalogo esistente
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = sv_open(tmp, SV_O_WRONLY | SV_O_CREAT | SV_O_TRUNC, 0644);
    if (fd < 0) return SV_IO_ERROR;

    uint32_t version = CAT_VERSION, next_page = c->next_page_id;
    int32_t ndb = c->num_databases, cur = c->current_db;
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
    if (ok) ok = sv_fsync(fd) == 0;
    sv_close(fd);
    if (!ok) { remove(tmp); return SV_IO_ERROR; }
#ifdef _WIN32
    remove(path);   // su Windows rename non sostituisce un file esistente
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return SV_IO_ERROR; }
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

Database* catalog_current_db(Catalog* c) {
    if (!c || c->current_db < 0 || c->current_db >= c->num_databases)
        return NULL;
    return &c->databases[c->current_db];
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
