/*
SCOPO: import dei dump di MySQL / MariaDB (mysqldump, export di phpMyAdmin, script con DELIMITER come
       classicmodels, Northwind, Sakila, world): prepara lo script prima di eseguirlo.
       - DELIMITER ;; ... ;; tolti; i commenti versionati di viste, trigger, procedure e
         FOREIGN_KEY_CHECKS ("!50001 ...") diventano testo normale
       - salta SET (tranne FOREIGN_KEY_CHECKS), LOCK / UNLOCK TABLES, ALTER TABLE ... DISABLE / ENABLE KEYS
       - CREATE TABLE e il resto passano così come sono: il motore capisce il dialetto di MySQL
         (UNSIGNED, KEY, FULLTEXT, ON UPDATE, ENGINE=, CHARSET, COLLATE, SET(...), 0x.., N'..', ...)
       Nessuna dipendenza dal motore.
*/
#ifndef MYSQL_IMPORT_H
#define MYSQL_IMPORT_H

#include <stdbool.h>

typedef struct {
    int statements;     // statement nel risultato
    int skipped;        // statement saltati (SET, LOCK, ...)
    int indexes_dropped;// sempre 0: gli indici FULLTEXT / SPATIAL ora arrivano al motore
} MyImportStats;

bool  mysql_looks_like_dump(const char* sql);                 // intestazione di mysqldump / phpMyAdmin, ENGINE=, ...
char* mysql_translate(const char* sql, MyImportStats* st);    // malloc'd; NULL se manca memoria

#endif
