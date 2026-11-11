#include "schema_model.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int sm_find(const SchemaModel* m, const char* table) {
    for (int i = 0; i < m->nt; i++) if (!strcasecmp(m->t[i].name, table)) return i;
    return -1;
}

int sm_col(const SmTable* t, const char* col) {
    for (int i = 0; i < t->ncols; i++) if (!strcasecmp(t->cols[i].name, col)) return i;
    return -1;
}

void sm_free(SchemaModel* m) {
    free(m->t);
    free(m->r);
    memset(m, 0, sizeof(*m));
}

// chiave primaria = unione delle colonne di esattamente due chiavi esterne, nessuno la riferisce
static bool is_link_table(const SchemaModel* m, int i) {
    const SmTable* t = &m->t[i];
    int fk[2], n = 0;
    for (int k = 0; k < m->nr; k++) {
        if (m->r[k].parent == i) return false;
        if (m->r[k].child == i) { if (n == 2) return false; fk[n++] = k; }
    }
    if (n != 2) return false;
    int npk = 0;
    for (int c = 0; c < t->ncols; c++) {
        if (!t->cols[c].pk) continue;
        npk++;
        bool in = false;
        for (int a = 0; a < 2 && !in; a++)
            for (int j = 0; j < m->r[fk[a]].ncols && !in; j++) in = !strcasecmp(m->r[fk[a]].cols[j], t->cols[c].name);
        if (!in) return false;
    }
    for (int a = 0; a < 2; a++)
        for (int j = 0; j < m->r[fk[a]].ncols; j++) {
            int c = sm_col(t, m->r[fk[a]].cols[j]);
            if (c < 0 || !t->cols[c].pk) return false;
        }
    return npk > 0;
}

bool sm_load(SchemaModel* m, const char* db) {
    memset(m, 0, sizeof(*m));
    if (!db || !db[0]) return false;
    NameList* tables = db_list_tables(db);
    if (!tables) return false;
    snprintf(m->db, sizeof(m->db), "%.63s", db);
    m->t = calloc((size_t)(tables->count ? tables->count : 1), sizeof(SmTable));
    if (!m->t) { db_namelist_free(tables); return false; }
    for (int i = 0; i < tables->count; i++) {
        int n = 0;
        ColumnInfo* cols = db_list_columns(db, tables->names[i], &n);
        if (!cols) continue;
        SmTable* t = &m->t[m->nt++];
        snprintf(t->name, sizeof(t->name), "%.63s", tables->names[i]);
        for (int c = 0; c < n && c < SM_MAX_COLS; c++) {
            SmCol* sc = &t->cols[t->ncols++];
            snprintf(sc->name, sizeof(sc->name), "%.63s", cols[c].name);
            snprintf(sc->type, sizeof(sc->type), "%.63s", cols[c].type);
            sc->pk = cols[c].primary_key;
            sc->notnull = !cols[c].nullable || cols[c].primary_key;
            sc->unique = cols[c].unique;
            sc->autoinc = cols[c].auto_increment;
        }
        db_columns_free(cols, n);
    }
    db_namelist_free(tables);

    int nf = 0;
    ForeignKeyInfo* fks = db_list_foreign_keys(db, &nf);
    m->r = calloc((size_t)(nf ? nf : 1), sizeof(SmRel));
    for (int i = 0; m->r && i < nf; i++) {
        int child = sm_find(m, fks[i].table), parent = sm_find(m, fks[i].ref_table);
        if (child < 0 || parent < 0) continue;
        SmRel* r = &m->r[m->nr++];
        snprintf(r->name, sizeof(r->name), "%.63s", fks[i].name);
        snprintf(r->on_delete, sizeof(r->on_delete), "%.15s", fks[i].on_delete);
        r->child = child;
        r->parent = parent;
        r->ncols = fks[i].ncols;
        SmTable* t = &m->t[child];
        int npk = 0, fk_pk = 0;
        for (int c = 0; c < t->ncols; c++) npk += t->cols[c].pk;
        for (int k = 0; k < r->ncols; k++) {
            snprintf(r->cols[k], 64, "%.63s", fks[i].cols[k]);
            int c = sm_col(t, fks[i].cols[k]);
            if (c < 0) continue;
            t->cols[c].fk = true;
            if (!t->cols[c].notnull) r->optional = true;
            fk_pk += t->cols[c].pk;
        }
        int c0 = r->ncols == 1 ? sm_col(t, r->cols[0]) : -1;
        r->one_to_one = (fk_pk == r->ncols && npk == r->ncols) || (c0 >= 0 && t->cols[c0].unique);
    }
    db_foreign_keys_free(fks);
    for (int i = 0; i < m->nt; i++) m->t[i].link = is_link_table(m, i);
    return true;
}

// ── posizioni ──────────────────────────────────────────────────

bool sm_load_positions(SchemaModel* m, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256], name[64];
    int x, y;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "v ", 2)) {
            snprintf(m->view, sizeof(m->view), "%.120s", line);
            m->view[strcspn(m->view, "\r\n")] = '\0';
            continue;
        }
        if (sscanf(line, "t %63s %d %d", name, &x, &y) != 3) continue;
        int i = sm_find(m, name);
        if (i < 0) continue;
        m->t[i].x = x;
        m->t[i].y = y;
        m->t[i].placed = true;
    }
    fclose(f);
    return true;
}

bool sm_save_positions(const SchemaModel* m, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# SegVault schema diagram: %s\n", m->db);
    if (m->view[0]) fprintf(f, "%s\n", m->view);
    for (int i = 0; i < m->nt; i++)
        if (m->t[i].placed) fprintf(f, "t %s %d %d\n", m->t[i].name, m->t[i].x, m->t[i].y);
    return fclose(f) == 0;
}

// ── disposizione automatica ────────────────────────────────────

#define MARGIN 40

// il segmento (x0,y0)-(x1,y1) attraversa il rettangolo (Liang-Barsky)
static bool seg_hits_rect(float x0, float y0, float x1, float y1, float rx, float ry, float rw, float rh) {
    float dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
    float p[4] = { -dx, dx, -dy, dy }, q[4] = { x0 - rx, rx + rw - x0, y0 - ry, ry + rh - y0 };
    for (int k = 0; k < 4; k++) {
        if (p[k] == 0) { if (q[k] < 0) return false; continue; }
        float t = q[k] / p[k];
        if (p[k] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
        else { if (t < t0) return false; if (t < t1) t1 = t; }
    }
    return t0 <= t1;
}

// la tabella i (nella posizione attuale) si sovrappone a una già disposta, o una sua linea verso
// una tabella già disposta passa sopra un'altra
static bool crosses(const SchemaModel* m, int i, const bool* done) {
    const SmTable* t = &m->t[i];
    for (int j = 0; j < m->nt; j++) {
        if (!done[j] || j == i) continue;
        const SmTable* o = &m->t[j];
        if (t->x < o->x + o->w + 20 && o->x < t->x + t->w + 20 && t->y < o->y + o->h + 20 && o->y < t->y + t->h + 20) return true;
    }
    float cx = (float)t->x + (float)t->w / 2, cy = (float)t->y + (float)t->h / 2;
    for (int k = 0; k < m->nr; k++) {
        const SmRel* r = &m->r[k];
        int o = r->child == i ? r->parent : r->parent == i ? r->child : -1;
        if (o < 0 || o == i || !done[o]) continue;
        float ox = (float)m->t[o].x + (float)m->t[o].w / 2, oy = (float)m->t[o].y + (float)m->t[o].h / 2;
        for (int j = 0; j < m->nt; j++) {
            if (!done[j] || j == i || j == o) continue;
            const SmTable* b = &m->t[j];
            if (seg_hits_rect(cx, cy, ox, oy, (float)b->x - 8, (float)b->y - 8, (float)b->w + 16, (float)b->h + 16)) return true;
        }
    }
    return false;
}

void sm_auto_layout(SchemaModel* m, bool all, int GAP_X, int GAP_Y) {
    if (m->nt == 0) return;
    int placed = 0;
    for (int i = 0; i < m->nt; i++) placed += m->t[i].placed && !all;
    if (placed == 0) all = true;

    if (!all) {     // le nuove in colonna a destra di quelle già disposte
        int right = 0, top = 1 << 30;
        for (int i = 0; i < m->nt; i++)
            if (m->t[i].placed) {
                if (m->t[i].x + m->t[i].w > right) right = m->t[i].x + m->t[i].w;
                if (m->t[i].y < top) top = m->t[i].y;
            }
        int y = top;
        for (int i = 0; i < m->nt; i++)
            if (!m->t[i].placed) {
                m->t[i].x = right + GAP_X;
                m->t[i].y = y;
                m->t[i].placed = true;
                y += m->t[i].h + GAP_Y;
            }
        return;
    }

    // livello = lunghezza del cammino più lungo verso una tabella senza genitori (cicli limitati)
    int* level = calloc((unsigned)m->nt, sizeof(int));
    bool* done = calloc((unsigned)m->nt, sizeof(bool));
    if (!level || !done) { free(level); free(done); return; }
    for (int pass = 0; pass < m->nt; pass++) {
        bool changed = false;
        for (int k = 0; k < m->nr; k++) {
            const SmRel* r = &m->r[k];
            if (r->child == r->parent) continue;
            if (level[r->child] < level[r->parent] + 1 && level[r->parent] + 1 < m->nt) {
                level[r->child] = level[r->parent] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    int maxl = 0;
    for (int i = 0; i < m->nt; i++) if (level[i] > maxl) maxl = level[i];
    int x = MARGIN;
    for (int l = 0; l <= maxl; l++) {
        int colw = 0;
        for (int i = 0; i < m->nt; i++) if (level[i] == l && m->t[i].w > colw) colw = m->t[i].w;
        // ordine nella colonna: per la y media delle tabelle collegate già disposte (baricentro);
        // al primo livello prima le tabelle con più relazioni, in fondo quelle isolate
        int order[512], n = 0;
        float key[512];
        for (int i = 0; i < m->nt && n < 512; i++) {
            if (level[i] != l) continue;
            float sum = 0;
            int cnt = 0, deg = 0;
            for (int k = 0; k < m->nr; k++) {
                const SmRel* r = &m->r[k];
                int o = r->child == i ? r->parent : r->parent == i ? r->child : -1;
                if (o < 0 || o == i) continue;
                deg++;
                if (done[o]) { sum += (float)m->t[o].y + (float)m->t[o].h / 2; cnt++; }
            }
            key[n] = cnt ? sum / (float)cnt : deg ? 1e6f - (float)deg : 2e6f;
            order[n++] = i;
        }
        for (int a = 1; a < n; a++)
            for (int b = a; b > 0 && key[b] < key[b - 1]; b--) {
                float tk = key[b]; key[b] = key[b - 1]; key[b - 1] = tk;
                int to = order[b]; order[b] = order[b - 1]; order[b - 1] = to;
            }
        int next_y = MARGIN;
        for (int a = 0; a < n; a++) {
            int i = order[a];
            SmTable* t = &m->t[i];
            t->x = x + (colw - t->w) / 2;
            int y = next_y;
            if (key[a] < 1e5f) y = SV_MAX(next_y, ((int)key[a] - t->h / 2) / 10 * 10);
            // si scende finché le linee verso le tabelle già disposte non attraversano altri riquadri
            for (int tries = 0; tries < 80; tries++) {
                t->y = y;
                if (!crosses(m, i, done)) break;
                y += 20;
            }
            if (crosses(m, i, done)) t->y = SV_MAX(next_y, (int)key[a] < 1e5f ? ((int)key[a] - t->h / 2) / 10 * 10 : next_y);
            t->placed = true;
            done[i] = true;
            next_y = t->y + t->h + GAP_Y;
        }
        x += colw + GAP_X;
    }
    free(level);
    free(done);
}

// ── SQL ────────────────────────────────────────────────────────

static char* dupf(const char* fmt, const char* a, const char* b, const char* c, const char* d, const char* e) {
    size_t n = strlen(fmt) + strlen(a) + strlen(b) + strlen(c) + strlen(d) + strlen(e) + 1;
    char* s = malloc(n);
    if (s) snprintf(s, n, fmt, a, b, c, d, e);
    return s;
}

// nome di vincolo libero nel database
static void fk_name(const SchemaModel* m, const char* child, const char* parent, char* out, size_t n) {
    for (int k = 1; ; k++) {
        if (k == 1) snprintf(out, n, "fk_%.28s_%.28s", child, parent);
        else snprintf(out, n, "fk_%.26s_%.26s_%d", child, parent, k);
        bool used = false;
        for (int i = 0; i < m->nr && !used; i++) used = !strcasecmp(m->r[i].name, out);
        if (!used || k > 99) return;
    }
}

// testo che cresce (SQL di più statement)
typedef struct { char* p; size_t n, cap; bool oom; } SBuf;

static void sb_add(SBuf* b, const char* s) {
    size_t n = strlen(s);
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char* p = realloc(b->p, cap);
        if (!p) { b->oom = true; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
}

static void sb_id(SBuf* b, const char* name) { sb_add(b, "`"); sb_add(b, name); sb_add(b, "`"); }

static char* sb_done(SBuf* b, char* err, size_t errn) {
    if (b->oom || !b->p) { free(b->p); if (err) snprintf(err, errn, "Out of memory"); return NULL; }
    return b->p;
}

// colonne della chiave primaria di una tabella (posizioni); 0 se non ne ha
static int pk_cols(const SmTable* t, int* out) {
    int n = 0;
    for (int i = 0; i < t->ncols && n < 8; i++) if (t->cols[i].pk) out[n++] = i;
    return n;
}

// nome di colonna per riferirsi alla chiave `key` di `parent`: <parent>_id per "id", altrimenti il nome
// della chiave (con prefix se richiesto); mai uguale a una colonna di `t` né a quelli già in `taken`
static void ref_col_name(const char* parent, const char* key, bool prefix, const SmTable* t,
                         char taken[][64], int ntaken, char* out) {
    char base[64];
    if (!strcasecmp(key, "id")) snprintf(base, sizeof(base), "%.55s_id", parent);
    else if (prefix) snprintf(base, sizeof(base), "%.30s_%.30s", parent, key);
    else snprintf(base, sizeof(base), "%.60s", key);
    snprintf(out, 64, "%s", base);
    for (int k = 2; k < 100; k++) {
        bool used = t && sm_col(t, out) >= 0;
        for (int i = 0; i < ntaken && !used; i++) used = !strcasecmp(taken[i], out);
        if (!used) return;
        snprintf(out, 64, "%.58s_%d", base, k);
    }
}

char* sm_relation_sql(const SchemaModel* m, int child, const char* col, int parent, char* err, size_t errn) {
    if (child < 0 || child >= m->nt || parent < 0 || parent >= m->nt) { snprintf(err, errn, "Unknown table"); return NULL; }
    const SmTable* C = &m->t[child];
    const SmTable* P = &m->t[parent];
    int pk[8];
    int npk = pk_cols(P, pk);
    if (npk == 0) { snprintf(err, errn, "Table '%.60s' has no primary key to refer to", P->name); return NULL; }
    char cname[80];
    fk_name(m, C->name, P->name, cname, sizeof(cname));
    SBuf b = {0};
    if (col && col[0]) {
        if (npk > 1) {
            snprintf(err, errn, "The key of '%.60s' has %d columns: drag from the table's + handle instead", P->name, npk);
            return NULL;
        }
        int c = sm_col(C, col);
        if (c < 0) { snprintf(err, errn, "Column '%.60s' not found in '%.60s'", col, C->name); return NULL; }
        if (child == parent && C->cols[c].pk) { snprintf(err, errn, "A column cannot refer to itself"); return NULL; }
        for (int k = 0; k < m->nr; k++)
            if (m->r[k].child == child && m->r[k].ncols == 1 && !strcasecmp(m->r[k].cols[0], col)) {
                snprintf(err, errn, "Column '%.60s' is already a foreign key (%.60s)", col, m->r[k].name);
                return NULL;
            }
        sb_add(&b, "ALTER TABLE "); sb_id(&b, C->name); sb_add(&b, " ADD CONSTRAINT "); sb_id(&b, cname);
        sb_add(&b, " FOREIGN KEY ("); sb_id(&b, C->cols[c].name); sb_add(&b, ") REFERENCES "); sb_id(&b, P->name);
        sb_add(&b, " ("); sb_id(&b, P->cols[pk[0]].name); sb_add(&b, ")");
        return sb_done(&b, err, errn);
    }
    // colonne nuove, una per colonna della chiave del genitore, dello stesso tipo
    char names[8][64];
    for (int k = 0; k < npk; k++) {
        ref_col_name(P->name, P->cols[pk[k]].name, npk > 1, C, names, k, names[k]);
        sb_add(&b, "ALTER TABLE "); sb_id(&b, C->name); sb_add(&b, " ADD COLUMN "); sb_id(&b, names[k]);
        sb_add(&b, " "); sb_add(&b, P->cols[pk[k]].type); sb_add(&b, ";\n");
    }
    sb_add(&b, "ALTER TABLE "); sb_id(&b, C->name); sb_add(&b, " ADD CONSTRAINT "); sb_id(&b, cname); sb_add(&b, " FOREIGN KEY (");
    for (int k = 0; k < npk; k++) { if (k) sb_add(&b, ", "); sb_id(&b, names[k]); }
    sb_add(&b, ") REFERENCES "); sb_id(&b, P->name); sb_add(&b, " (");
    for (int k = 0; k < npk; k++) { if (k) sb_add(&b, ", "); sb_id(&b, P->cols[pk[k]].name); }
    sb_add(&b, ")");
    return sb_done(&b, err, errn);
}

char* sm_many_to_many_sql(const SchemaModel* m, int a, int b, char* table, size_t tn, char* err, size_t errn) {
    if (a < 0 || a >= m->nt || b < 0 || b >= m->nt) { snprintf(err, errn, "Unknown table"); return NULL; }
    const SmTable* A = &m->t[a];
    const SmTable* B = &m->t[b];
    int pa[8], pb[8];
    int na = pk_cols(A, pa), nb = pk_cols(B, pb);
    if (!na || !nb) { snprintf(err, errn, "Table '%.60s' has no primary key", !na ? A->name : B->name); return NULL; }
    if (na + nb > 8) { snprintf(err, errn, "The keys are too large for a link table"); return NULL; }
    // nome della tabella di collegamento: a_b, senza ripetere una tabella esistente
    char base[64];
    snprintf(base, sizeof(base), "%.30s_%.30s", A->name, B->name);
    snprintf(table, tn, "%s", base);
    for (int k = 2; sm_find(m, table) >= 0 && k < 100; k++) snprintf(table, tn, "%.58s_%d", base, k);
    char names[16][64];
    int n = 0;
    for (int k = 0; k < na; k++, n++) ref_col_name(A->name, A->cols[pa[k]].name, true, NULL, names, n, names[n]);
    for (int k = 0; k < nb; k++, n++) {
        // auto-relazione molti-a-molti: la seconda copia della chiave prende il suffisso _2
        ref_col_name(B->name, B->cols[pb[k]].name, true, NULL, names, n, names[n]);
    }
    SBuf s = {0};
    sb_add(&s, "CREATE TABLE "); sb_id(&s, table); sb_add(&s, " (\n");
    for (int k = 0; k < n; k++) {
        const SmCol* src = k < na ? &A->cols[pa[k]] : &B->cols[pb[k - na]];
        sb_add(&s, "    "); sb_id(&s, names[k]); sb_add(&s, " "); sb_add(&s, src->type); sb_add(&s, " NOT NULL,\n");
    }
    sb_add(&s, "    PRIMARY KEY (");
    for (int k = 0; k < n; k++) { if (k) sb_add(&s, ", "); sb_id(&s, names[k]); }
    sb_add(&s, "),\n");
    char cn[80];
    for (int side = 0; side < 2; side++) {
        const SmTable* T = side ? B : A;
        const int* pk = side ? pb : pa;
        int cnt = side ? nb : na, off = side ? na : 0;
        if (side && a == b) snprintf(cn, sizeof(cn), "fk_%.28s_%.26s_2", table, T->name);
        else snprintf(cn, sizeof(cn), "fk_%.28s_%.28s", table, T->name);
        sb_add(&s, "    CONSTRAINT "); sb_id(&s, cn); sb_add(&s, " FOREIGN KEY (");
        for (int k = 0; k < cnt; k++) { if (k) sb_add(&s, ", "); sb_id(&s, names[off + k]); }
        sb_add(&s, ") REFERENCES "); sb_id(&s, T->name); sb_add(&s, " (");
        for (int k = 0; k < cnt; k++) { if (k) sb_add(&s, ", "); sb_id(&s, T->cols[pk[k]].name); }
        sb_add(&s, side ? ")\n" : "),\n");
    }
    sb_add(&s, ")");
    return sb_done(&s, err, errn);
}

char* sm_drop_relation_sql(const SchemaModel* m, int rel, bool drop_columns) {
    if (rel < 0 || rel >= m->nr) return NULL;
    const SmRel* r = &m->r[rel];
    const SmTable* C = &m->t[r->child];
    SBuf b = {0};
    sb_add(&b, "ALTER TABLE "); sb_id(&b, C->name); sb_add(&b, " DROP CONSTRAINT "); sb_id(&b, r->name);
    for (int k = 0; drop_columns && k < r->ncols; k++) {
        if (!sm_droppable_column(m, rel, k)) continue;
        sb_add(&b, ";\nALTER TABLE "); sb_id(&b, C->name); sb_add(&b, " DROP COLUMN "); sb_id(&b, r->cols[k]);
    }
    return sb_done(&b, NULL, 0);
}

// la colonna k della relazione si può eliminare insieme a lei: non è chiave primaria, non serve a
// un'altra chiave esterna e non è l'ultima colonna della tabella
bool sm_droppable_column(const SchemaModel* m, int rel, int k) {
    if (rel < 0 || rel >= m->nr || k < 0 || k >= m->r[rel].ncols) return false;
    const SmRel* r = &m->r[rel];
    const SmTable* C = &m->t[r->child];
    int c = sm_col(C, r->cols[k]);
    if (c < 0 || C->cols[c].pk || C->ncols <= 1) return false;
    for (int q = 0; q < m->nr; q++) {
        if (q == rel || m->r[q].child != r->child) continue;
        for (int j = 0; j < m->r[q].ncols; j++) if (!strcasecmp(m->r[q].cols[j], r->cols[k])) return false;
    }
    return true;
}

char* sm_relation_required_sql(const SchemaModel* m, int rel, bool required) {
    if (rel < 0 || rel >= m->nr) return NULL;
    const SmRel* r = &m->r[rel];
    const SmTable* C = &m->t[r->child];
    size_t cap = 256 + (size_t)r->ncols * 200, n = 0;
    char* s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (int k = 0; k < r->ncols; k++) {
        int c = sm_col(C, r->cols[k]);
        if (c < 0 || C->cols[c].pk) continue;
        n += (size_t)snprintf(s + n, cap - n, "%sALTER TABLE `%s` ALTER COLUMN `%s` %s", n ? ";\n" : "",
                              C->name, r->cols[k], required ? "SET NOT NULL" : "DROP NOT NULL");
    }
    if (!n) { free(s); return NULL; }
    return s;
}

// le relazioni delle altre tabelle verso questa vengono tolte prima (altrimenti il DROP è rifiutato)
char* sm_drop_table_sql(const SchemaModel* m, int table) {
    if (table < 0 || table >= m->nt) return NULL;
    size_t cap = 256, n = 0;
    for (int k = 0; k < m->nr; k++) cap += 200;
    char* s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (int k = 0; k < m->nr; k++) {
        const SmRel* r = &m->r[k];
        if (r->parent != table || r->child == table) continue;
        n += (size_t)snprintf(s + n, cap - n, "ALTER TABLE `%s` DROP CONSTRAINT `%s`;\n", m->t[r->child].name, r->name);
    }
    snprintf(s + n, cap - n, "DROP TABLE `%s`", m->t[table].name);
    return s;
}
