#include "schema.h"
#include <string.h>
#include "platform_compat.h"

int datatype_size(DataType t) {
    switch (t) {
        case SV_TYPE_INT:      return 4;
        case SV_TYPE_BIGINT:   return 8;
        case SV_TYPE_FLOAT:    return 8;
        case SV_TYPE_BOOL:     return 1;
        case SV_TYPE_DATE:     return 4;
        case SV_TYPE_DATETIME: return 8;
        case SV_TYPE_DECIMAL:  return 16;
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

Catalog* catalog_load(const char* path) {
    int fd = sv_open(path, SV_O_RDONLY, 0);
    if (fd < 0) return catalog_create();

    Catalog* c = SV_ALLOC(Catalog);
    if (!c) { sv_close(fd); return NULL; }

    ssize_t n = sv_read(fd, c, sizeof(Catalog));
    sv_close(fd);

    if (n != (ssize_t)sizeof(Catalog)) {
        free(c);
        return catalog_create();
    }
    return c;
}

int catalog_save(Catalog* c, const char* path) {
    if (!c || !path) return SV_ERR;
    int fd = sv_open(path, SV_O_WRONLY | SV_O_CREAT | SV_O_TRUNC, 0644);
    if (fd < 0) return SV_IO_ERROR;

    ssize_t n = sv_write(fd, c, sizeof(Catalog));
    sv_close(fd);
    return (n == (ssize_t)sizeof(Catalog)) ? SV_OK : SV_IO_ERROR;
}

void catalog_free(Catalog* c) { free(c); }

Database* catalog_get_db(Catalog* c, const char* name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->num_databases; i++)
        if (strcmp(c->databases[i].name, name) == 0)
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
        if (strcmp(c->databases[i].name, name) == 0) {
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
        if (strcmp(c->databases[i].name, name) == 0) {
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
        if (strcmp(d->tables[i].name, table) == 0)
            return &d->tables[i];
    return NULL;
}

int catalog_add_table(Catalog* c, const char* db, TableSchema* schema) {
    if (!c || !db || !schema) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    if (d->num_tables >= SV_MAX_TABLES) return SV_FULL;
    if (catalog_get_table(c, db, schema->name)) return SV_DUPLICATE;

    schema->root_page_id = c->next_page_id++;
    d->tables[d->num_tables++] = *schema;
    return SV_OK;
}

int catalog_drop_table(Catalog* c, const char* db, const char* table) {
    if (!c || !db || !table) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    for (int i = 0; i < d->num_tables; i++) {
        if (strcmp(d->tables[i].name, table) == 0) {
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
        if (strcmp(d->views[i].name, name) == 0)
            return &d->views[i];
    return NULL;
}

int catalog_add_view(Catalog* c, const char* db, ViewSchema* schema) {
    if (!c || !db || !schema) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    if (d->num_views >= 64) return SV_FULL;
    if (catalog_get_view(c, db, schema->name)) return SV_DUPLICATE;
    d->views[d->num_views++] = *schema;
    return SV_OK;
}

int catalog_drop_view(Catalog* c, const char* db, const char* name) {
    if (!c || !db || !name) return SV_ERR;
    Database* d = catalog_get_db(c, db);
    if (!d) return SV_NOT_FOUND;
    for (int i = 0; i < d->num_views; i++) {
        if (strcmp(d->views[i].name, name) == 0) {
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
        if (strcmp(t->columns[i].name, col_name) == 0)
            return i;
    return -1;
}
