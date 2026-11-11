/*
SCOPO: import dei dump di MySQL / MariaDB (mysqldump, export di phpMyAdmin): traduce lo script nel
       dialetto di SegVault prima di eseguirlo.
       - salta SET, LOCK / UNLOCK TABLES, ALTER TABLE ... DISABLE / ENABLE KEYS
       - CREATE TABLE: tipi MySQL (tinyint, mediumtext, json, timestamp, enum, ...) e attributi
         (unsigned, COLLATE, CHARACTER SET, COMMENT, ON UPDATE) tradotti o tolti; opzioni della
         tabella (ENGINE=, CHARSET, AUTO_INCREMENT=) tolte; KEY -> CREATE INDEX (una colonna),
         UNIQUE KEY -> CONSTRAINT ... UNIQUE
       Nessuna dipendenza dal motore.
*/
#ifndef MYSQL_IMPORT_H
#define MYSQL_IMPORT_H

#include <stdbool.h>

typedef struct {
    int statements;     // statement nel risultato
    int skipped;        // statement saltati (SET, LOCK, ...)
    int indexes_dropped;// indici FULLTEXT / SPATIAL non riportati (MATCH ... AGAINST funziona anche senza)
} MyImportStats;

bool  mysql_looks_like_dump(const char* sql);                 // intestazione di mysqldump / phpMyAdmin, ENGINE=, ...
char* mysql_translate(const char* sql, MyImportStats* st);    // malloc'd; NULL se manca memoria

#endif
