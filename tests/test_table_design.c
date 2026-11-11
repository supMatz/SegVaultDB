// Designer di tabelle: SQL generato (CREATE / ALTER) ed eseguito sul motore, poi ricaricato dal catalogo.
#include "src/bridge/db_api.h"
#include "src/widgets/table_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// esegue gli statement separati da ";\n"; false al primo errore
static bool exec_all(const char* sql, char* err, size_t n) {
    char* copy = strdup(sql);
    bool ok = true;
    for (char* st = strtok(copy, ";"); st && ok; st = strtok(NULL, ";")) {
        while (*st == '\n' || *st == ' ') st++;
        if (!*st) continue;
        QueryResult* r = db_execute(st);
        if (!r || !r->success) { snprintf(err, n, "%.150s -> %.200s", st, r ? r->error : "null"); ok = false; }
        db_result_free(r);
    }
    free(copy);
    return ok;
}

static const char* scalar(const char* sql) {
    static char buf[256];
    QueryResult* r = db_execute(sql);
    snprintf(buf, sizeof(buf), "%.255s", r && r->success && r->num_rows ? (r->rows[0].cells[0].is_null ? "NULL" : r->rows[0].cells[0].value) : r && !r->success ? r->error : "(none)");
    db_result_free(r);
    return buf;
}

static int col(const DesignTable* t, const char* name) {
    for (int i = 0; i < t->ncols; i++) if (!strcmp(t->cols[i].name, name)) return i;
    return -1;
}

int main(void) {
    if (system("rm -rf /tmp/sv_design_test && mkdir -p /tmp/sv_design_test") != 0) return 1;
    if (!db_init("/tmp/sv_design_test")) return 1;
    db_result_free(db_execute("CREATE DATABASE d"));
    db_result_free(db_execute("USE d"));
    char err[400];
    int nt;
    const char* const* types = design_types(&nt);

    // ── tabella nuova ──
    DesignTable t;
    design_new(&t);
    snprintf(t.name, sizeof(t.name), "cliente");
    design_add_column(&t);                                   // VARCHAR(100)
    snprintf(t.cols[1].name, 64, "nome"); t.cols[1].notnull = true;
    design_add_column(&t);
    snprintf(t.cols[2].name, 64, "email"); t.cols[2].unique = true;
    design_add_column(&t);
    snprintf(t.cols[3].name, 64, "citta"); snprintf(t.cols[3].def, 128, "l'Aquila");
    design_add_column(&t);
    snprintf(t.cols[4].name, 64, "punti"); t.cols[4].type = design_type_index("INT"); snprintf(t.cols[4].def, 128, "10");
    char* sql = design_sql(NULL, &t, err, sizeof(err));
    CHECK(sql && strstr(sql, "`id` INT PRIMARY KEY AUTO_INCREMENT") && strstr(sql, "`citta` VARCHAR(100) DEFAULT 'l''Aquila'")
          && strstr(sql, "`email` VARCHAR(100) UNIQUE") && strstr(sql, "`punti` INT DEFAULT 10"), "create sql: %s", sql ? sql : err);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "create runs: %s", err);
    free(sql);
    db_result_free(db_execute("INSERT INTO cliente (nome, email) VALUES ('Anna', 'a@x.it')"));
    CHECK(!strcmp(scalar("SELECT citta FROM cliente"), "l'Aquila") && !strcmp(scalar("SELECT punti FROM cliente"), "10"), "defaults applied");

    // ── validazione ──
    DesignTable bad = t;
    snprintf(bad.cols[1].name, 64, "email");
    CHECK(!design_sql(NULL, &bad, err, sizeof(err)) && strstr(err, "twice"), "duplicate column: %s", err);
    bad = t; bad.cols[1].size[0] = '\0';
    CHECK(!design_sql(NULL, &bad, err, sizeof(err)) && strstr(err, "length"), "varchar without length: %s", err);
    bad = t; snprintf(bad.name, 64, "1x");
    CHECK(!design_sql(NULL, &bad, err, sizeof(err)), "bad table name");

    // ── modifica: ricarica, rinomina, cambia tipo, default, NOT NULL, UNIQUE, aggiungi, elimina, rinomina tabella ──
    DesignTable orig, cur;
    CHECK(design_load(&orig, "d", "cliente"), "load");
    cur = orig;
    CHECK(cur.ncols == 5 && cur.cols[0].pk && cur.cols[0].autoinc && cur.cols[1].notnull && cur.cols[2].unique
          && !strcmp(cur.cols[3].def, "l'Aquila") && !strcmp(cur.cols[1].size, "100"), "loaded structure");
    CHECK(!design_sql(&orig, &cur, err, sizeof(err)) && strstr(err, "No changes"), "no changes: %s", err);
    int c;
    c = col(&cur, "nome");  snprintf(cur.cols[c].name, 64, "nominativo"); snprintf(cur.cols[c].size, 16, "150");
    c = col(&cur, "punti"); snprintf(cur.cols[c].def, 128, "0"); cur.cols[c].notnull = true; cur.cols[c].type = design_type_index("BIGINT");
    c = col(&cur, "email"); cur.cols[c].unique = false;
    c = col(&cur, "citta"); cur.cols[c] = cur.cols[cur.ncols - 1]; cur.ncols--;          // elimina citta
    design_add_column(&cur);
    snprintf(cur.cols[cur.ncols - 1].name, 64, "attivo"); cur.cols[cur.ncols - 1].type = design_type_index("BOOL");
    snprintf(cur.cols[cur.ncols - 1].def, 128, "true");
    snprintf(cur.name, 64, "clienti");
    sql = design_sql(&orig, &cur, err, sizeof(err));
    CHECK(sql != NULL, "alter sql: %s", err);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "alter runs: %s\n%s", err, sql ? sql : "");
    free(sql);
    DesignTable after;
    CHECK(design_load(&after, "d", "clienti"), "renamed table");
    CHECK(col(&after, "citta") < 0 && col(&after, "nominativo") >= 0 && !strcmp(after.cols[col(&after, "nominativo")].size, "150"),
          "rename + type of nominativo");
    c = col(&after, "punti");
    CHECK(c >= 0 && !strcmp(types[after.cols[c].type], "BIGINT") && after.cols[c].notnull && !strcmp(after.cols[c].def, "0"), "punti changed");
    CHECK(!after.cols[col(&after, "email")].unique, "unique removed");
    CHECK(!strcmp(scalar("SELECT nominativo FROM clienti"), "Anna") && !strcmp(scalar("SELECT attivo FROM clienti"), "1"),
          "data kept, new column default: %s", scalar("SELECT attivo FROM clienti"));
    // UNIQUE di nuovo, poi verifica che sia applicato
    orig = after; cur = after;
    cur.cols[col(&cur, "email")].unique = true;
    sql = design_sql(&orig, &cur, err, sizeof(err));
    CHECK(sql && exec_all(sql, err, sizeof(err)), "add unique: %s", err);
    free(sql);
    QueryResult* r = db_execute("INSERT INTO clienti (nominativo, email, punti) VALUES ('B', 'a@x.it', 1)");
    CHECK(r && !r->success && strstr(r->error, "Duplicate"), "unique enforced");
    db_result_free(r);
    // chiave primaria di una tabella esistente: non si cambia qui
    orig = after; cur = after;
    cur.cols[col(&cur, "email")].pk = true;
    CHECK(!design_sql(&orig, &cur, err, sizeof(err)) && strstr(err, "primary key"), "pk change refused: %s", err);

    // un vincolo che i dati non rispettano fallisce per primo: il resto non è ancora stato applicato
    db_result_free(db_execute("INSERT INTO clienti (nominativo, punti) VALUES ('C', 2)"));     // email NULL
    CHECK(design_load(&orig, "d", "clienti"), "reload");
    cur = orig;
    c = col(&cur, "email"); cur.cols[c].notnull = true;
    c = col(&cur, "punti"); snprintf(cur.cols[c].name, 64, "saldo"); snprintf(cur.cols[c].def, 128, "5");
    sql = design_sql(&orig, &cur, err, sizeof(err));
    CHECK(sql && strstr(sql, "SET NOT NULL") && strstr(sql, "SET NOT NULL") < strstr(sql, "RENAME COLUMN"), "fallible first: %s", sql ? sql : err);
    CHECK(sql && !exec_all(sql, err, sizeof(err)) && strstr(err, "NULL"), "not null refused: %s", err);
    free(sql);
    CHECK(design_load(&after, "d", "clienti") && col(&after, "punti") >= 0 && !strcmp(after.cols[col(&after, "punti")].def, "0"),
          "nothing applied after the failure");

    // indici: nella tabella nuova, aggiunto, mantenuto con la rinomina, tolto
    DesignTable ix;
    design_new(&ix);
    snprintf(ix.name, 64, "ordine");
    design_add_column(&ix); snprintf(ix.cols[1].name, 64, "cliente"); ix.cols[1].index = true;
    design_add_column(&ix); snprintf(ix.cols[2].name, 64, "nota");
    sql = design_sql(NULL, &ix, err, sizeof(err));
    CHECK(sql && strstr(sql, "CREATE INDEX `ix_ordine_cliente` ON `ordine` (`cliente`)") && exec_all(sql, err, sizeof(err)), "index on create: %s", sql ? sql : err);
    free(sql);
    CHECK(design_load(&orig, "d", "ordine") && orig.cols[col(&orig, "cliente")].index && !orig.cols[col(&orig, "nota")].index, "index loaded");
    cur = orig;
    cur.cols[col(&cur, "nota")].index = true;
    snprintf(cur.cols[col(&cur, "cliente")].name, 64, "cliente_id");
    sql = design_sql(&orig, &cur, err, sizeof(err));
    CHECK(sql && strstr(sql, "CREATE INDEX `ix_ordine_nota`") && exec_all(sql, err, sizeof(err)), "add index: %s", sql ? sql : err);
    free(sql);
    CHECK(design_load(&after, "d", "ordine") && after.cols[col(&after, "cliente_id")].index && after.cols[col(&after, "nota")].index, "indexes after rename");
    orig = after; cur = after;
    cur.cols[col(&cur, "cliente_id")].index = false;
    sql = design_sql(&orig, &cur, err, sizeof(err));
    CHECK(sql && strstr(sql, "DROP INDEX `ix_ordine_cliente` ON `ordine`") && exec_all(sql, err, sizeof(err)), "drop index: %s", sql ? sql : err);
    free(sql);
    CHECK(design_load(&after, "d", "ordine") && !after.cols[col(&after, "cliente_id")].index, "index gone");

    // chiave composta
    DesignTable k;
    memset(&k, 0, sizeof(k));
    snprintf(k.name, 64, "riga");
    design_add_column(&k); snprintf(k.cols[0].name, 64, "ordine"); k.cols[0].type = design_type_index("INT"); k.cols[0].pk = true;
    design_add_column(&k); snprintf(k.cols[1].name, 64, "n"); k.cols[1].type = design_type_index("INT"); k.cols[1].pk = true;
    sql = design_sql(NULL, &k, err, sizeof(err));
    CHECK(sql && strstr(sql, "PRIMARY KEY (`ordine`, `n`)") && exec_all(sql, err, sizeof(err)), "composite key: %s", sql ? sql : err);
    free(sql);

    db_shutdown();
    if (system("rm -rf /tmp/sv_design_test")) {}
    if (g_fail) { printf("table design tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d table design checks passed\n", g_tests, g_tests);
    return 0;
}
