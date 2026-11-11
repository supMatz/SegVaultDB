#include "table_design.h"
#include "../bridge/db_api.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char* const TYPES[] = { "INT", "BIGINT", "FLOAT", "DECIMAL", "VARCHAR", "CHAR", "TEXT", "BOOL", "DATE", "DATETIME", "BLOB" };
#define NTYPES ((int)(sizeof(TYPES) / sizeof(TYPES[0])))
enum { T_INT, T_BIGINT, T_FLOAT, T_DECIMAL, T_VARCHAR, T_CHAR, T_TEXT, T_BOOL, T_DATE, T_DATETIME, T_BLOB };

const char* const* design_types(int* n) { *n = NTYPES; return TYPES; }
bool design_type_sized(int type) { return type == T_VARCHAR || type == T_CHAR; }

int design_type_index(const char* name) {
    char t[32];
    size_t k = 0;
    while (name[k] && name[k] != '(' && k < sizeof(t) - 1) { t[k] = (char)toupper((unsigned char)name[k]); k++; }
    t[k] = '\0';
    if (!strcmp(t, "INTEGER")) return T_INT;
    if (!strcmp(t, "DOUBLE")) return T_FLOAT;
    if (!strcmp(t, "BOOLEAN")) return T_BOOL;
    for (int i = 0; i < NTYPES; i++) if (!strcmp(t, TYPES[i])) return i;
    return -1;
}

bool design_load(DesignTable* t, const char* db, const char* table) {
    memset(t, 0, sizeof(*t));
    int n = 0;
    ColumnInfo* cols = db_list_columns(db, table, &n);
    if (!cols) return false;
    snprintf(t->name, sizeof(t->name), "%s", table);
    for (int i = 0; i < n && i < DESIGN_MAX_COLS; i++) {
        DesignCol* c = &t->cols[t->ncols++];
        c->orig = i;
        snprintf(c->name, sizeof(c->name), "%s", cols[i].name);
        c->type = design_type_index(cols[i].type);
        if (c->type < 0) c->type = T_TEXT;
        const char* par = strchr(cols[i].type, '(');
        if (par) { snprintf(c->size, sizeof(c->size), "%s", par + 1); c->size[strcspn(c->size, ")")] = '\0'; }
        snprintf(c->def, sizeof(c->def), "%s", cols[i].default_val);
        c->pk = cols[i].primary_key;
        c->notnull = !cols[i].nullable && !cols[i].primary_key;
        c->unique = cols[i].unique;
        snprintf(c->unique_name, sizeof(c->unique_name), "%s", cols[i].unique_name);
        c->autoinc = cols[i].auto_increment;
    }
    db_columns_free(cols, n);
    return true;
}

void design_new(DesignTable* t) {
    memset(t, 0, sizeof(*t));
    DesignCol* c = &t->cols[t->ncols++];
    c->orig = -1;
    snprintf(c->name, sizeof(c->name), "id");
    c->type = T_INT;
    c->pk = c->autoinc = true;
}

void design_add_column(DesignTable* t) {
    if (t->ncols >= DESIGN_MAX_COLS) return;
    DesignCol* c = &t->cols[t->ncols++];
    memset(c, 0, sizeof(*c));
    c->orig = -1;
    c->type = T_VARCHAR;
    snprintf(c->size, sizeof(c->size), "100");
}

// ── SQL ─────────────────────────────────────────────────────────

typedef struct { char* p; size_t n, cap; bool oom; } DBuf;

static void db_add(DBuf* b, const char* s) {
    size_t n = strlen(s);
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 512;
        while (cap < b->n + n + 1) cap *= 2;
        char* p = realloc(b->p, cap);
        if (!p) { b->oom = true; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
}

static void db_ident(DBuf* b, const char* name) { db_add(b, "`"); db_add(b, name); db_add(b, "`"); }

static bool valid_name(const char* s) {
    if (!s[0] || strlen(s) > 63 || isdigit((unsigned char)s[0])) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return false;
    return true;
}

static bool is_number(const char* s) {
    char* end;
    if (!*s) return false;
    strtod(s, &end);
    return *end == '\0' && !strpbrk(s, "xXnNiI");
}

// tipo SQL della colonna ("VARCHAR(40)")
static void type_sql(const DesignCol* c, char* out, size_t n) {
    if (design_type_sized(c->type)) snprintf(out, n, "%s(%s)", TYPES[c->type], c->size);
    else snprintf(out, n, "%s", TYPES[c->type]);
}

// DEFAULT come letterale SQL secondo il tipo (testo tra apici, numeri e booleani così come sono)
static void default_sql(DBuf* b, const DesignCol* c) {
    const char* v = c->def;
    if (!strcasecmp(v, "NULL")) { db_add(b, "NULL"); return; }
    bool numeric = c->type == T_INT || c->type == T_BIGINT || c->type == T_FLOAT || c->type == T_DECIMAL;
    if (numeric && is_number(v)) { db_add(b, v); return; }
    if (c->type == T_BOOL && (!strcasecmp(v, "true") || !strcasecmp(v, "false") || !strcmp(v, "1") || !strcmp(v, "0"))) {
        db_add(b, !strcasecmp(v, "true") || !strcmp(v, "1") ? "TRUE" : "FALSE");
        return;
    }
    db_add(b, "'");
    for (const char* s = v; *s; s++) {
        char one[3] = { *s, 0, 0 };
        if (*s == '\'') { one[1] = '\''; }
        else if (*s == '\\') { one[1] = '\\'; }
        db_add(b, one);
    }
    db_add(b, "'");
}

// definizione di colonna per CREATE TABLE / ADD COLUMN (la chiave primaria composta va a parte)
static void column_sql(DBuf* b, const DesignCol* c, bool inline_pk) {
    char t[48];
    type_sql(c, t, sizeof(t));
    db_ident(b, c->name);
    db_add(b, " ");
    db_add(b, t);
    if (inline_pk && c->pk) db_add(b, " PRIMARY KEY");
    if (c->autoinc) db_add(b, " AUTO_INCREMENT");
    if (c->notnull && !c->pk) db_add(b, " NOT NULL");
    if (c->unique && !c->pk) db_add(b, " UNIQUE");
    if (c->def[0] && !c->autoinc) { db_add(b, " DEFAULT "); default_sql(b, c); }
}

static bool validate(const DesignTable* t, char* err, size_t errn) {
    if (!valid_name(t->name)) { snprintf(err, errn, "Table name: letters, digits and _ (not starting with a digit)"); return false; }
    if (t->ncols == 0) { snprintf(err, errn, "A table needs at least one column"); return false; }
    int npk = 0;
    for (int i = 0; i < t->ncols; i++) {
        const DesignCol* c = &t->cols[i];
        if (!valid_name(c->name)) {
            if (c->name[0]) snprintf(err, errn, "Column '%.60s': use letters, digits and _ only", c->name);
            else snprintf(err, errn, "Column %d has no name", i + 1);
            return false;
        }
        for (int k = 0; k < i; k++)
            if (!strcasecmp(t->cols[k].name, c->name)) { snprintf(err, errn, "Column '%.60s' appears twice", c->name); return false; }
        if (design_type_sized(c->type) && (atoi(c->size) <= 0 || !is_number(c->size))) {
            snprintf(err, errn, "Column '%.60s': %s needs a length (e.g. 100)", c->name, TYPES[c->type]);
            return false;
        }
        if (c->autoinc && c->type != T_INT && c->type != T_BIGINT) {
            snprintf(err, errn, "Column '%.60s': AUTO_INCREMENT needs INT or BIGINT", c->name);
            return false;
        }
        if (c->autoinc && !c->pk) { snprintf(err, errn, "Column '%.60s': AUTO_INCREMENT must be the primary key", c->name); return false; }
        if (c->pk) npk++;
    }
    for (int i = 0; i < t->ncols; i++)
        if (t->cols[i].autoinc && npk > 1) { snprintf(err, errn, "AUTO_INCREMENT needs a single-column primary key"); return false; }
    return true;
}

static char* create_sql(const DesignTable* t) {
    DBuf b = {0};
    int npk = 0;
    for (int i = 0; i < t->ncols; i++) npk += t->cols[i].pk;
    db_add(&b, "CREATE TABLE ");
    db_ident(&b, t->name);
    db_add(&b, " (\n");
    for (int i = 0; i < t->ncols; i++) {
        db_add(&b, "    ");
        column_sql(&b, &t->cols[i], npk == 1);
        if (i + 1 < t->ncols || npk > 1) db_add(&b, ",");
        db_add(&b, "\n");
    }
    if (npk > 1) {
        db_add(&b, "    PRIMARY KEY (");
        int k = 0;
        for (int i = 0; i < t->ncols; i++) if (t->cols[i].pk) { if (k++) db_add(&b, ", "); db_ident(&b, t->cols[i].name); }
        db_add(&b, ")\n");
    }
    db_add(&b, ")");
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

static void stmt_begin(DBuf* b, const char* table) {
    if (b->n) db_add(b, ";\n");
    db_add(b, "ALTER TABLE ");
    db_ident(b, table);
    db_add(b, " ");
}

char* design_sql(const DesignTable* orig, const DesignTable* cur, char* err, size_t errn) {
    if (!validate(cur, err, errn)) return NULL;
    if (!orig) return create_sql(cur);

    // tabella esistente: chiave primaria e AUTO_INCREMENT non si cambiano qui
    for (int i = 0; i < cur->ncols; i++) {
        const DesignCol* c = &cur->cols[i];
        bool was_pk = c->orig >= 0 && orig->cols[c->orig].pk, was_ai = c->orig >= 0 && orig->cols[c->orig].autoinc;
        if (c->pk != was_pk || c->autoinc != was_ai) {
            snprintf(err, errn, "The primary key and AUTO_INCREMENT of an existing table cannot be changed here "
                                "(column '%.60s')", c->name);
            return NULL;
        }
    }
    const char* tn = orig->name;          // i comandi usano il nome attuale; RENAME TO per ultimo
    DBuf b = {0};
    char t[48];
    // 1. modifiche che dipendono dai dati e possono fallire (tipo, NOT NULL, UNIQUE): per prime, così un
    //    errore lascia la tabella com'era (il DDL del motore non si annulla con ROLLBACK)
    for (int i = 0; i < cur->ncols; i++) {
        const DesignCol* c = &cur->cols[i];
        if (c->orig < 0) continue;
        const DesignCol* o = &orig->cols[c->orig];
        if (c->type != o->type || (design_type_sized(c->type) && strcmp(c->size, o->size))) {
            type_sql(c, t, sizeof(t));
            stmt_begin(&b, tn); db_add(&b, "ALTER COLUMN "); db_ident(&b, o->name); db_add(&b, " TYPE "); db_add(&b, t);
        }
        if (c->notnull && !o->notnull && !c->pk) {
            stmt_begin(&b, tn); db_add(&b, "ALTER COLUMN "); db_ident(&b, o->name); db_add(&b, " SET NOT NULL");
        }
        if (c->unique && !o->unique && !c->pk) {
            char cn[80];
            snprintf(cn, sizeof(cn), "uq_%.30s_%.30s", tn, c->name);
            stmt_begin(&b, tn); db_add(&b, "ADD CONSTRAINT "); db_ident(&b, cn);
            db_add(&b, " UNIQUE ("); db_ident(&b, o->name); db_add(&b, ")");
        }
    }
    // 2. colonne eliminate
    for (int i = 0; i < orig->ncols; i++) {
        bool kept = false;
        for (int k = 0; k < cur->ncols && !kept; k++) kept = cur->cols[k].orig == i;
        if (kept) continue;
        if (orig->cols[i].pk) { free(b.p); snprintf(err, errn, "Column '%.60s' is part of the primary key and cannot be removed here", orig->cols[i].name); return NULL; }
        if (orig->cols[i].unique && orig->cols[i].unique_name[0]) {
            stmt_begin(&b, tn); db_add(&b, "DROP CONSTRAINT "); db_ident(&b, orig->cols[i].unique_name);
        }
        stmt_begin(&b, tn); db_add(&b, "DROP COLUMN "); db_ident(&b, orig->cols[i].name);
    }
    // 3. il resto delle colonne esistenti: UNIQUE e NOT NULL tolti, DEFAULT, nome
    for (int i = 0; i < cur->ncols; i++) {
        const DesignCol* c = &cur->cols[i];
        if (c->orig < 0) continue;
        const DesignCol* o = &orig->cols[c->orig];
        if (!c->unique && o->unique && !c->pk) {
            if (!o->unique_name[0]) {
                free(b.p);
                snprintf(err, errn, "The UNIQUE of column '%.60s' has no constraint name: remove it with ALTER TABLE", c->name);
                return NULL;
            }
            stmt_begin(&b, tn); db_add(&b, "DROP CONSTRAINT "); db_ident(&b, o->unique_name);
        }
        if (!c->notnull && o->notnull && !c->pk) {
            stmt_begin(&b, tn); db_add(&b, "ALTER COLUMN "); db_ident(&b, o->name); db_add(&b, " DROP NOT NULL");
        }
        if (strcmp(c->def, o->def) && !c->autoinc) {
            stmt_begin(&b, tn); db_add(&b, "ALTER COLUMN "); db_ident(&b, o->name);
            if (c->def[0]) { db_add(&b, " SET DEFAULT "); default_sql(&b, c); }
            else db_add(&b, " DROP DEFAULT");
        }
        if (strcmp(c->name, o->name)) {
            stmt_begin(&b, tn); db_add(&b, "RENAME COLUMN "); db_ident(&b, o->name); db_add(&b, " TO "); db_ident(&b, c->name);
        }
    }
    // 4. colonne nuove
    for (int i = 0; i < cur->ncols; i++) {
        const DesignCol* c = &cur->cols[i];
        if (c->orig >= 0) continue;
        if (c->pk || c->autoinc) { free(b.p); snprintf(err, errn, "A new column cannot be part of the primary key of an existing table ('%.60s')", c->name); return NULL; }
        stmt_begin(&b, tn); db_add(&b, "ADD COLUMN "); column_sql(&b, c, false);
    }
    // 5. nome della tabella
    if (strcmp(cur->name, orig->name)) { stmt_begin(&b, tn); db_add(&b, "RENAME TO "); db_ident(&b, cur->name); }
    if (b.oom) { free(b.p); snprintf(err, errn, "Out of memory"); return NULL; }
    if (!b.p) { snprintf(err, errn, "No changes"); return NULL; }
    return b.p;
}
