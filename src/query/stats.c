/*
SCOPO: statistiche delle colonne (vedi stats.h).
*/
#include "stats.h"
#include "platform_compat.h"
#include "sv_alloc.h"
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static TableStats* g_t;
static int         g_n, g_cap;

static void ts_free(TableStats* t) { free(t->cols); memset(t, 0, sizeof(*t)); }

static TableStats* find(const char* db, const char* table) {
    for (int i = 0; i < g_n; i++)
        if (!strcasecmp(g_t[i].db, db ? db : "") && !strcasecmp(g_t[i].table, table)) return &g_t[i];
    return NULL;
}

void stats_put(const TableStats* t) {
    TableStats* d = find(t->db, t->table);
    if (d) ts_free(d);
    else {
        if (g_n == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 16;
            g_t = sv_realloc(g_t, sizeof(TableStats) * (size_t)g_cap);
        }
        d = &g_t[g_n++];
    }
    *d = *t;
    d->cols = sv_calloc((size_t)(t->ncols > 0 ? t->ncols : 1), sizeof(ColStats));
    if (t->ncols > 0) memcpy(d->cols, t->cols, sizeof(ColStats) * (size_t)t->ncols);
}

const TableStats* stats_get(const char* db, const char* table) { return table ? find(db, table) : NULL; }

const ColStats* stats_col(const char* db, const char* table, const char* col) {
    const TableStats* t = stats_get(db, table);
    for (int i = 0; t && i < t->ncols; i++) if (!strcasecmp(t->cols[i].name, col)) return &t->cols[i];
    return NULL;
}

void stats_drop(const char* db, const char* table) {
    for (int i = 0; i < g_n; ) {
        if (!strcasecmp(g_t[i].db, db ? db : "") && (!table || !strcasecmp(g_t[i].table, table))) {
            ts_free(&g_t[i]);
            g_t[i] = g_t[--g_n];
        } else i++;
    }
}

void stats_clear(void) {
    for (int i = 0; i < g_n; i++) ts_free(&g_t[i]);
    free(g_t);
    g_t = NULL;
    g_n = g_cap = 0;
}

// formato: "table DB TABLE ROWS NCOLS", poi per colonna "col NAME NDV NULLFRAC NUMERIC MIN MAX H0 ... H16"
bool stats_save(const char* path) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = sv_fopen_private(tmp, "w");
    if (!f) return false;
    fprintf(f, "# SegVault optimizer statistics (ANALYZE TABLE): rebuilt by ANALYZE, safe to delete\n");
    for (int i = 0; i < g_n; i++) {
        const TableStats* t = &g_t[i];
        char edb[3 * 64], etb[3 * 64], ecol[3 * 64];       // nomi codificati: uno spazio non sposta i campi (CR-B5)
        sv_name_encode(t->db, edb, sizeof(edb));
        sv_name_encode(t->table, etb, sizeof(etb));
        fprintf(f, "table %s %s %.17g %d\n", t->db[0] ? edb : "-", etb, t->rows, t->ncols);
        for (int c = 0; c < t->ncols; c++) {
            const ColStats* s = &t->cols[c];
            sv_name_encode(s->name, ecol, sizeof(ecol));
            fprintf(f, "col %s %.17g %.17g %d %.17g %.17g", ecol, s->ndv, s->null_frac, s->numeric ? 1 : 0, s->min, s->max);
            for (int k = 0; k <= STATS_BUCKETS; k++) fprintf(f, " %.17g", s->hist[k]);
            fputc('\n', f);
        }
    }
    bool ok = fflush(f) == 0 && sv_fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = sv_replace_file(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

bool stats_load(const char* path) {
    stats_clear();
    FILE* f = fopen(path, "r" SV_FE);
    if (!f) return true;                         // nessuna statistica: va bene
    char line[2048];
    TableStats cur;
    memset(&cur, 0, sizeof(cur));
    int want = 0;
    bool bad = false;                            // tabella corrente illeggibile: scartata lei sola (CR-B5)
    bool all_ok = true;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strncmp(line, "table ", 6)) {
            if (!bad && cur.cols && cur.ncols == want) stats_put(&cur);
            free(cur.cols);
            memset(&cur, 0, sizeof(cur));
            bad = false;
            char edb[3 * 64], etb[3 * 64];
            int used = 0;
            if (sscanf(line, "table %191s %191s %lf %d%n", edb, etb, &cur.rows, &want, &used) != 4 || want < 0 || want > 4096 ||
                strspn(line + used, " \r\n") != strlen(line + used) ||
                (strcmp(edb, "-") && !sv_name_decode(edb, sizeof(cur.db))) || !sv_name_decode(etb, sizeof(cur.table))) {
                bad = true; all_ok = false; continue;
            }
            snprintf(cur.db, sizeof(cur.db), "%.63s", strcmp(edb, "-") ? edb : "");
            snprintf(cur.table, sizeof(cur.table), "%.63s", etb);
            cur.cols = sv_calloc((size_t)(want ? want : 1), sizeof(ColStats));
        } else if (!strncmp(line, "col ", 4) && !bad && cur.cols && cur.ncols < want) {
            ColStats* s = &cur.cols[cur.ncols];
            char ecol[3 * 64];
            int num = 0, used = 0;
            if (sscanf(line, "col %191s %lf %lf %d %lf %lf%n", ecol, &s->ndv, &s->null_frac, &num, &s->min, &s->max, &used) < 6 ||
                !sv_name_decode(ecol, sizeof(s->name))) { bad = true; all_ok = false; continue; }
            snprintf(s->name, sizeof(s->name), "%.63s", ecol);
            s->numeric = num != 0;
            const char* p = line + used;
            for (int k = 0; k <= STATS_BUCKETS && !bad; k++) {
                char* e;
                s->hist[k] = strtod(p, &e);
                if (e == p) bad = true;
                p = e;
            }
            if (bad) { all_ok = false; continue; }
            cur.ncols++;
        }
    }
    if (!bad && cur.cols && cur.ncols == want) stats_put(&cur);
    free(cur.cols);
    fclose(f);
    return all_ok;                               // false: almeno una tabella scartata (solo stime peggiori per lei)
}

// ── stime ────────────────────────────────────────────────────────

// frazione delle righe non NULL con valore < v (inclusive: <= v); interpolazione lineare nella fascia.
// Una fascia [a, b] con b < v (o b == v e inclusive) è tutta sotto; con a < v < b, la parte (v - a) / (b - a)
static double frac_below(const ColStats* c, double v, bool inclusive) {
    const double* h = c->hist;
    if (v < h[0]) return 0;
    if (v > h[STATS_BUCKETS]) return 1;
    double f = 0;
    for (int k = 0; k < STATS_BUCKETS; k++) {
        double a = h[k], b = h[k + 1];
        if (b < v || (b == v && inclusive)) { f += 1.0 / STATS_BUCKETS; continue; }
        if (a < v) f += (v - a) / (b - a) / STATS_BUCKETS;
        break;
    }
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

double stats_sel_eq(const ColStats* c, bool have_value, double value) {
    if (!c) return 0.1;
    double nn = 1 - c->null_frac;
    double uni = c->ndv > 0 ? 1.0 / c->ndv : 1;
    if (!have_value || !c->numeric) return nn * uni;
    if (value < c->min || value > c->max) return 0;
    // un valore frequente copre intere fasce: hist[k] == hist[k + 1] == value
    int full = 0;
    for (int k = 0; k < STATS_BUCKETS; k++) if (c->hist[k] == value && c->hist[k + 1] == value) full++;
    double s = full ? (double)full / STATS_BUCKETS : uni;
    if (s < uni) s = uni;
    return nn * (s > 1 ? 1 : s);
}

double stats_sel_range(const ColStats* c, int op, double value) {
    if (!c || !c->numeric) return 1.0 / 3;
    double r = op == '<' ? frac_below(c, value, false) : op == 'l' ? frac_below(c, value, true)
             : op == '>' ? 1 - frac_below(c, value, true) : 1 - frac_below(c, value, false);
    return (1 - c->null_frac) * r;
}
