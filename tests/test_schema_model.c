// Diagramma dello schema: relazioni lette dal catalogo, SQL delle azioni eseguito sul motore, posizioni e layout.
#include "src/bridge/db_api.h"
#include "src/widgets/schema_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool exec_all(const char* sql, char* err, size_t n) {
    char* copy = strdup(sql);
    bool ok = true;
    for (char* p = copy; p && ok; ) {
        char* e = strstr(p, ";\n");
        if (e) *e = '\0';
        QueryResult* r = db_execute(p);
        if (!r || !r->success) { snprintf(err, n, "%.150s -> %.200s", p, r ? r->error : "null"); ok = false; }
        db_result_free(r);
        p = e ? e + 2 : NULL;
    }
    free(copy);
    return ok;
}

static void run(const char* sql) {
    QueryResult* r = db_execute(sql);
    if (!r || !r->success) printf("setup failed: %s -> %s\n", sql, r ? r->error : "null");
    db_result_free(r);
}

static int rel_between(const SchemaModel* m, const char* child, const char* parent) {
    for (int k = 0; k < m->nr; k++)
        if (!strcmp(m->t[m->r[k].child].name, child) && !strcmp(m->t[m->r[k].parent].name, parent)) return k;
    return -1;
}

int main(void) {
    const char* dir = "/tmp/sv_schema_test";
    if (system("rm -rf /tmp/sv_schema_test && mkdir -p /tmp/sv_schema_test") != 0) return 1;
    if (!db_init(dir)) return 1;
    run("CREATE DATABASE u");
    run("USE u");
    run("CREATE TABLE studente (matricola INT PRIMARY KEY, nome VARCHAR(50) NOT NULL)");
    run("CREATE TABLE corso (id INT PRIMARY KEY AUTO_INCREMENT, titolo VARCHAR(80))");
    run("CREATE TABLE esame (studente INT NOT NULL, corso INT, voto INT, PRIMARY KEY (studente, corso), "
        "FOREIGN KEY (studente) REFERENCES studente(matricola), FOREIGN KEY (corso) REFERENCES corso(id) ON DELETE CASCADE)");
    run("CREATE TABLE tessera (matricola INT PRIMARY KEY, FOREIGN KEY (matricola) REFERENCES studente(matricola))");
    char err[400];

    SchemaModel m;
    CHECK(sm_load(&m, "u") && m.nt == 4 && m.nr == 3, "load: %d tables %d rels", m.nt, m.nr);
    int k = rel_between(&m, "esame", "studente");
    CHECK(k >= 0 && !m.r[k].optional && !m.r[k].one_to_one, "esame -> studente (1,1) / N");
    CHECK(m.t[sm_find(&m, "esame")].link, "esame is a relationship (key = both foreign keys)");
    k = rel_between(&m, "esame", "corso");
    CHECK(k >= 0 && !strcmp(m.r[k].on_delete, "CASCADE"), "on delete cascade");
    k = rel_between(&m, "tessera", "studente");
    CHECK(k >= 0 && m.r[k].one_to_one, "tessera is one-to-one");
    int e = sm_find(&m, "esame");
    CHECK(e >= 0 && m.t[e].cols[sm_col(&m.t[e], "corso")].fk && !m.t[e].cols[sm_col(&m.t[e], "voto")].fk, "fk flags");
    sm_free(&m);
    CHECK(sm_load(&m, "nope") == false, "unknown database");

    // layout: i genitori a sinistra delle figlie
    sm_load(&m, "u");
    for (int i = 0; i < m.nt; i++) { m.t[i].w = 160; m.t[i].h = 100; }
    sm_auto_layout(&m, true, 110, 50);
    CHECK(m.t[sm_find(&m, "studente")].x < m.t[sm_find(&m, "esame")].x
          && m.t[sm_find(&m, "corso")].x < m.t[sm_find(&m, "esame")].x, "parents left of children");
    for (int i = 0; i < m.nt; i++)
        for (int j = 0; j < i; j++) {
            const SmTable *a = &m.t[i], *b = &m.t[j];
            bool overlap = a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
            CHECK(!overlap, "%s overlaps %s", a->name, b->name);
        }

    // posizioni salvate e rilette
    m.t[0].x = 777; m.t[0].y = 333;
    CHECK(sm_save_positions(&m, "/tmp/sv_schema_test/pos.txt"), "save positions");
    SchemaModel m2;
    sm_load(&m2, "u");
    CHECK(sm_load_positions(&m2, "/tmp/sv_schema_test/pos.txt"), "load positions");
    int i0 = sm_find(&m2, m.t[0].name);
    CHECK(m2.t[i0].placed && m2.t[i0].x == 777 && m2.t[i0].y == 333, "position restored");
    char moved[64];
    snprintf(moved, sizeof(moved), "%s", m.t[0].name);
    sm_free(&m2);

    // tabella nuova: posizionata accanto alle altre senza spostarle
    run("CREATE TABLE aula (id INT PRIMARY KEY, nome VARCHAR(20))");
    sm_free(&m);
    sm_load(&m, "u");
    sm_load_positions(&m, "/tmp/sv_schema_test/pos.txt");
    for (int i = 0; i < m.nt; i++) { m.t[i].w = 160; m.t[i].h = 100; }
    int au = sm_find(&m, "aula");
    CHECK(!m.t[au].placed, "new table not placed");
    sm_auto_layout(&m, false, 110, 50);
    CHECK(m.t[au].placed && m.t[sm_find(&m, moved)].x == 777, "new table placed, others kept");

    // relazione con una colonna nuova: corso -> aula
    char* sql = sm_relation_sql(&m, sm_find(&m, "corso"), "", au, err, sizeof(err));
    CHECK(sql && strstr(sql, "ADD COLUMN `aula_id` INT") && strstr(sql, "REFERENCES `aula` (`id`)"), "new column fk: %s", sql ? sql : err);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "runs: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    k = rel_between(&m, "corso", "aula");
    CHECK(k >= 0 && m.r[k].optional, "corso -> aula optional");

    // relazione da una colonna esistente; doppione rifiutato
    run("CREATE TABLE prenotazione (id INT PRIMARY KEY, chi INT)");
    sm_free(&m);
    sm_load(&m, "u");
    sql = sm_relation_sql(&m, sm_find(&m, "prenotazione"), "chi", sm_find(&m, "studente"), err, sizeof(err));
    CHECK(sql && strstr(sql, "FOREIGN KEY (`chi`) REFERENCES `studente` (`matricola`)") && exec_all(sql, err, sizeof(err)), "existing column: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    CHECK(!sm_relation_sql(&m, sm_find(&m, "prenotazione"), "chi", sm_find(&m, "studente"), err, sizeof(err)) && strstr(err, "already"),
          "duplicate fk refused: %s", err);
    CHECK(!sm_relation_sql(&m, sm_find(&m, "studente"), "nome", sm_find(&m, "esame"), err, sizeof(err)) && strstr(err, "2 columns"),
          "single column to composite key refused: %s", err);
    // la colonna nuova non ripete un nome esistente
    sql = sm_relation_sql(&m, sm_find(&m, "corso"), "", sm_find(&m, "aula"), err, sizeof(err));
    CHECK(sql && strstr(sql, "`aula_id_2`"), "unique column name: %s", sql ? sql : err);
    free(sql);

    // obbligatoria / facoltativa
    k = rel_between(&m, "prenotazione", "studente");
    sql = sm_relation_required_sql(&m, k, true);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "set required: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    CHECK(!m.r[rel_between(&m, "prenotazione", "studente")].optional, "now (1,1)");

    // relazione eliminata
    sql = sm_drop_relation_sql(&m, rel_between(&m, "prenotazione", "studente"), false);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "drop relation: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    CHECK(rel_between(&m, "prenotazione", "studente") < 0, "relation gone");

    // relazione verso una chiave composta: una colonna nuova per ogni colonna della chiave
    run("CREATE TABLE ricorso (id INT PRIMARY KEY, motivo VARCHAR(40))");
    sm_free(&m);
    sm_load(&m, "u");
    sql = sm_relation_sql(&m, sm_find(&m, "ricorso"), "", sm_find(&m, "esame"), err, sizeof(err));
    CHECK(sql && strstr(sql, "`esame_studente`") && strstr(sql, "`esame_corso`")
          && strstr(sql, "REFERENCES `esame` (`studente`, `corso`)"), "composite fk: %s", sql ? sql : err);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "composite fk runs: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    k = rel_between(&m, "ricorso", "esame");
    CHECK(k >= 0 && m.r[k].ncols == 2, "composite relation loaded");
    // eliminata con le sue colonne
    CHECK(sm_droppable_column(&m, k, 0) && sm_droppable_column(&m, k, 1), "columns droppable");
    sql = sm_drop_relation_sql(&m, k, true);
    CHECK(sql && strstr(sql, "DROP COLUMN `esame_studente`") && exec_all(sql, err, sizeof(err)), "drop with columns: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    int ri = sm_find(&m, "ricorso");
    CHECK(rel_between(&m, "ricorso", "esame") < 0 && sm_col(&m.t[ri], "esame_corso") < 0 && m.t[ri].ncols == 2, "columns gone");
    // la chiave primaria della figlia non si elimina con la relazione
    k = rel_between(&m, "tessera", "studente");
    CHECK(!sm_droppable_column(&m, k, 0), "pk column kept");

    // molti-a-molti: tabella di collegamento
    char link[64];
    sql = sm_many_to_many_sql(&m, sm_find(&m, "studente"), sm_find(&m, "aula"), link, sizeof(link), err, sizeof(err));
    CHECK(sql && !strcmp(link, "studente_aula") && strstr(sql, "`studente_matricola` INT NOT NULL") && strstr(sql, "`aula_id` INT NOT NULL")
          && strstr(sql, "PRIMARY KEY (`studente_matricola`, `aula_id`)"), "n:m sql: %s", sql ? sql : err);
    CHECK(sql && exec_all(sql, err, sizeof(err)), "n:m runs: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    CHECK(rel_between(&m, "studente_aula", "studente") >= 0 && rel_between(&m, "studente_aula", "aula") >= 0, "n:m relations");
    CHECK(m.t[sm_find(&m, "studente_aula")].link && !m.t[sm_find(&m, "studente")].link && !m.t[sm_find(&m, "tessera")].link, "link table recognised");
    run("INSERT INTO aula VALUES (1, 'A1')");
    run("INSERT INTO studente VALUES (1, 'Anna')");
    run("INSERT INTO studente_aula VALUES (1, 1)");
    QueryResult* r = db_execute("INSERT INTO studente_aula VALUES (1, 1)");
    CHECK(r && !r->success && strstr(r->error, "Duplicate"), "n:m pair unique: %s", r ? r->error : "");
    db_result_free(r);
    r = db_execute("INSERT INTO studente_aula VALUES (1, 99)");
    CHECK(r && !r->success && strstr(r->error, "oreign"), "n:m foreign key enforced: %s", r ? r->error : "");
    db_result_free(r);
    // auto-relazione molti-a-molti (es. propedeuticità tra corsi)
    sql = sm_many_to_many_sql(&m, sm_find(&m, "corso"), sm_find(&m, "corso"), link, sizeof(link), err, sizeof(err));
    CHECK(sql && strstr(sql, "`corso_id`") && strstr(sql, "`corso_id_2`") && exec_all(sql, err, sizeof(err)), "self n:m: %s", sql ? sql : err);
    free(sql);
    // nome libero se la tabella esiste già
    sm_free(&m);
    sm_load(&m, "u");
    sql = sm_many_to_many_sql(&m, sm_find(&m, "studente"), sm_find(&m, "aula"), link, sizeof(link), err, sizeof(err));
    CHECK(sql && !strcmp(link, "studente_aula_2"), "link name taken: %s", link);
    free(sql);

    // stato della vista conservato nel file delle posizioni
    snprintf(m.view, sizeof(m.view), "v 1 0.800 -10.0 20.0");
    sm_save_positions(&m, "/tmp/sv_schema_test/pos2.txt");
    sm_free(&m);
    sm_load(&m, "u");
    sm_load_positions(&m, "/tmp/sv_schema_test/pos2.txt");
    CHECK(!strcmp(m.view, "v 1 0.800 -10.0 20.0"), "view line: '%s'", m.view);

    // tabella riferita da altre: prima le loro chiavi esterne, poi il DROP
    sql = sm_drop_table_sql(&m, sm_find(&m, "studente"));
    CHECK(sql && exec_all(sql, err, sizeof(err)), "drop referenced table: %s", err);
    free(sql);
    sm_free(&m);
    sm_load(&m, "u");
    CHECK(sm_find(&m, "studente") < 0 && rel_between(&m, "esame", "studente") < 0 && sm_find(&m, "esame") >= 0, "table dropped");
    sm_free(&m);

    db_shutdown();
    if (system("rm -rf /tmp/sv_schema_test")) {}
    if (g_fail) { printf("schema model tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d schema model checks passed\n", g_tests, g_tests);
    return 0;
}
