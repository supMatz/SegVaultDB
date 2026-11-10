// Benchmark del motore: genera uno schema universitario e misura le query tipiche.
//   make bench && ./bench_sql [righe]
#include "src/bridge/db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void run(const char* label, const char* sql) {
    double t0 = now_ms();
    QueryResult* r = db_execute(sql);
    double t = now_ms() - t0;
    if (!r || !r->success) { printf("%-44s ERRORE: %s\n", label, r ? r->error : "null"); db_result_free(r); exit(1); }
    printf("%-44s %9.2f ms  (%d righe)\n", label, t, r->num_cols ? r->num_rows : r->rows_affected);
    db_result_free(r);
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 100000;
    char dir[] = "/tmp/svbenchXXXXXX";
    if (!mkdtemp(dir)) return 1;
    if (!db_init(dir)) { printf("init: %s\n", db_init_error()); return 1; }
    db_result_free(db_execute("CREATE DATABASE uni"));
    db_result_free(db_execute("USE uni"));
    db_result_free(db_execute("CREATE TABLE corso (id INT PRIMARY KEY, nome VARCHAR(40), cfu INT)"));
    db_result_free(db_execute("CREATE TABLE studente (matricola INT PRIMARY KEY, nome VARCHAR(40), citta VARCHAR(20), anno INT)"));
    db_result_free(db_execute("CREATE TABLE esame (matricola INT, corso INT, voto INT, data DATE)"));
    static const char* CITTA[] = { "Milano", "Roma", "Torino", "Napoli", "Bologna", "Pisa", "Padova", "Bari" };
    char* buf = malloc(1 << 22);
    double t0 = now_ms();
    for (int c = 0; c < 50; c++) {
        snprintf(buf, 256, "INSERT INTO corso VALUES (%d, 'Corso %d', %d)", c, c, 5 + c % 3 * 2);
        db_result_free(db_execute(buf));
    }
    // inserimenti a blocchi di 500 righe
    int ns = n / 10;
    for (int i = 0; i < ns; ) {
        size_t len = (size_t)sprintf(buf, "INSERT INTO studente VALUES ");
        for (int k = 0; k < 500 && i < ns; k++, i++)
            len += (size_t)sprintf(buf + len, "%s(%d, 'Studente %d', '%s', %d)", k ? "," : "", i, i, CITTA[i % 8], 1 + i % 5);
        QueryResult* r = db_execute(buf);
        if (!r || !r->success) { printf("insert: %s\n", r ? r->error : "?"); return 1; }
        db_result_free(r);
    }
    for (int i = 0; i < n; ) {
        size_t len = (size_t)sprintf(buf, "INSERT INTO esame VALUES ");
        for (int k = 0; k < 500 && i < n; k++, i++)
            len += (size_t)sprintf(buf + len, "%s(%d, %d, %d, '2025-%02d-%02d')", k ? "," : "", (i * 7) % ns, i % 50, 18 + i % 13, 1 + i % 12, 1 + i % 28);
        QueryResult* r = db_execute(buf);
        if (!r || !r->success) { printf("insert: %s\n", r ? r->error : "?"); return 1; }
        db_result_free(r);
    }
    printf("%-44s %9.2f ms  (%d righe)\n", "INSERT (a blocchi di 500)", now_ms() - t0, n + ns + 50);

    run("COUNT(*)", "SELECT COUNT(*) FROM esame");
    run("SELECT * intera tabella", "SELECT * FROM esame");
    run("SELECT * + ORDER BY", "SELECT * FROM esame ORDER BY data, matricola");
    run("scan + filtro", "SELECT * FROM esame WHERE voto = 30");
    run("scan + filtro composto", "SELECT matricola, voto FROM esame WHERE voto >= 28 AND corso < 10");
    run("lookup chiave primaria", "SELECT * FROM studente WHERE matricola = 4242");
    run("intervallo su chiave primaria (BETWEEN)", "SELECT COUNT(*), AVG(anno) FROM studente WHERE matricola BETWEEN 2000 AND 2499");
    run("funzione finestra con frame (ROWS)", "SELECT matricola, AVG(voto) OVER (PARTITION BY corso ORDER BY data ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) FROM esame WHERE corso < 5");
    run("GROUP BY + AVG", "SELECT corso, COUNT(*), AVG(voto) FROM esame GROUP BY corso");
    run("ORDER BY + LIMIT", "SELECT * FROM esame ORDER BY voto DESC, matricola LIMIT 10");
    run("JOIN 2 tabelle + filtro", "SELECT s.nome, e.voto FROM esame e JOIN studente s ON e.matricola = s.matricola WHERE e.voto = 30");
    run("JOIN 3 tabelle + GROUP BY", "SELECT c.nome, s.citta, AVG(e.voto) FROM esame e JOIN studente s ON e.matricola = s.matricola JOIN corso c ON c.id = e.corso GROUP BY c.nome, s.citta");
    run("DISTINCT", "SELECT DISTINCT citta, anno FROM studente");
    run("LIKE", "SELECT COUNT(*) FROM studente WHERE nome LIKE '%99%'");
    run("IN (subquery)", "SELECT COUNT(*) FROM studente WHERE matricola IN (SELECT matricola FROM esame WHERE voto = 30)");
    run("NOT EXISTS correlata", "SELECT COUNT(*) FROM studente s WHERE NOT EXISTS (SELECT * FROM esame e WHERE e.matricola = s.matricola AND e.voto > 28)");
    run("subquery scalare correlata", "SELECT matricola, (SELECT MAX(voto) FROM esame e WHERE e.matricola = s.matricola) FROM studente s WHERE anno = 1");
    run("divisione (doppio NOT EXISTS)", "SELECT COUNT(*) FROM studente s WHERE NOT EXISTS (SELECT * FROM corso c WHERE c.id < 3 AND NOT EXISTS "
        "(SELECT * FROM esame e WHERE e.matricola = s.matricola AND e.corso = c.id))");
    run("join implicito FROM a, b WHERE", "SELECT COUNT(*) FROM esame e, studente s WHERE e.matricola = s.matricola AND s.citta = 'Roma'");
    run("UNION", "SELECT citta FROM studente UNION SELECT nome FROM corso");
    run("UPDATE con filtro", "UPDATE esame SET voto = voto + 0 WHERE corso = 3");
    run("DELETE con filtro", "DELETE FROM esame WHERE corso = 49");
    db_shutdown();
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) {}
    free(buf);
    return 0;
}
