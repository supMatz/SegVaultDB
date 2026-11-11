// Import di un dump di MySQL: traduzione nel dialetto di SegVault ed esecuzione sul motore.
#include "src/bridge/db_api.h"
#include "src/widgets/mysql_import.h"
#include "src/widgets/sql_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* DUMP =
"-- MySQL dump 10.13  Distrib 8.0.36, for Linux (x86_64)\n"
"--\n-- Host: localhost    Database: shop\n"
"/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;\n"
"/*!50503 SET NAMES utf8mb4 */;\n"
"/*!40103 SET TIME_ZONE='+00:00' */;\n"
"SET @saved_cs_client = @@character_set_client;\n"
"DROP TABLE IF EXISTS `cliente`;\n"
"CREATE TABLE `cliente` (\n"
"  `id` int unsigned NOT NULL AUTO_INCREMENT,\n"
"  `nome` varchar(80) COLLATE utf8mb4_unicode_ci NOT NULL COMMENT 'nome, cognome',\n"
"  `email` varchar(120) CHARACTER SET utf8mb4 DEFAULT NULL,\n"
"  `tipo` enum('privato','azienda') NOT NULL DEFAULT 'privato',\n"
"  `attivo` tinyint(1) NOT NULL DEFAULT '1',\n"
"  `saldo` decimal(10,2) DEFAULT '0.00',\n"
"  `creato` timestamp NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,\n"
"  `note` mediumtext,\n"
"  `extra` json DEFAULT NULL,\n"
"  PRIMARY KEY (`id`) USING BTREE,\n"
"  UNIQUE KEY `uq_email` (`email`),\n"
"  KEY `ix_nome` (`nome`),\n"
"  KEY `ix_tipo_attivo` (`tipo`,`attivo`),\n"
"  FULLTEXT KEY `ft_note` (`note`)\n"
") ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='clienti';\n"
"/*!40101 SET character_set_client = @saved_cs_client */;\n"
"LOCK TABLES `cliente` WRITE;\n"
"/*!40000 ALTER TABLE `cliente` DISABLE KEYS */;\n"
"INSERT INTO `cliente` VALUES (1,'Anna Rossi','anna@x.it','privato',1,10.50,'2024-01-02 10:00:00','dice \\\"ciao\\\"; e poi va',NULL),"
"(2,'L\\'Ottica Srl','info@ottica.it','azienda',1,0.00,'2024-02-03 11:00:00',NULL,'{\\\"a\\\": 1}'),"
"(3,'Bruno',NULL,'privato',0,-3.25,NULL,'riga1\\nriga2',NULL);\n"
"/*!40000 ALTER TABLE `cliente` ENABLE KEYS */;\n"
"UNLOCK TABLES;\n"
"DROP TABLE IF EXISTS `ordine`;\n"
"CREATE TABLE `ordine` (\n"
"  `id` bigint NOT NULL,\n"
"  `cliente_id` int unsigned NOT NULL,\n"
"  `quando` datetime(3) DEFAULT NULL,\n"
"  `stato` set('nuovo','pagato','spedito') DEFAULT NULL,\n"
"  PRIMARY KEY (`id`),\n"
"  KEY `fk_cliente` (`cliente_id`),\n"
"  KEY (`quando`),\n"
"  CONSTRAINT `fk_ordine_cliente` FOREIGN KEY (`cliente_id`) REFERENCES `cliente` (`id`) ON DELETE CASCADE\n"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n"
"LOCK TABLES `ordine` WRITE;\n"
"INSERT INTO `ordine` VALUES (10,1,'2024-03-01 09:00:00.000','nuovo,pagato'),(11,2,NULL,NULL);\n"
"UNLOCK TABLES;\n"
"--\n-- Temporary view structure for view `vista`\n--\n"
"DROP TABLE IF EXISTS `vista`;\n"
"/*!50001 DROP VIEW IF EXISTS `vista`*/;\n"
"SET @saved_cs_client     = @@character_set_client;\n"
"/*!50503 SET character_set_client = utf8mb4 */;\n"
"/*!50001 CREATE VIEW `vista` AS SELECT \n 1 AS `id`,\n 1 AS `nome`*/;\n"
"SET character_set_client = @saved_cs_client;\n"
"/*!50003 SET @saved_cs_client      = @@character_set_client */ ;\n"
"DELIMITER ;;\n"
"/*!50003 CREATE*/ /*!50017 DEFINER=`root`@`localhost`*/ /*!50003 TRIGGER `ordine_check` BEFORE INSERT ON `ordine` FOR EACH ROW BEGIN\n"
"  IF NEW.id < 0 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'id negativo'; END IF;\n"
"END */;;\n"
"DELIMITER ;\n"
"DELIMITER ;;\n"
"CREATE DEFINER=`root`@`localhost` PROCEDURE `clienti_di`(IN t VARCHAR(20))\n"
"    READS SQL DATA\n"
"    COMMENT 'clienti per tipo'\n"
"BEGIN\n"
"  SELECT COUNT(*) FROM cliente WHERE tipo = t;\n"
"END ;;\n"
"DELIMITER ;\n"
"--\n-- Final view structure for view `vista`\n--\n"
"/*!50001 DROP VIEW IF EXISTS `vista`*/;\n"
"/*!50001 SET @saved_cs_client          = @@character_set_client */;\n"
"/*!50001 CREATE ALGORITHM=UNDEFINED */\n"
"/*!50013 DEFINER=`root`@`localhost` SQL SECURITY DEFINER */\n"
"/*!50001 VIEW `vista` AS select `cliente`.`id` AS `id`,`cliente`.`nome` AS `nome` from `cliente` where (`cliente`.`attivo` = 1) */;\n"
"/*!50001 SET character_set_client      = @saved_cs_client */;\n"
"/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;\n"
"-- Dump completed on 2024-03-05\n";

static const char* scalar(const char* sql) {
    static char buf[300];
    QueryResult* r = db_execute(sql);
    snprintf(buf, sizeof(buf), "%.290s", r && r->success && r->num_rows ? (r->rows[0].cells[0].is_null ? "NULL" : r->rows[0].cells[0].value) : r && !r->success ? r->error : "(none)");
    db_result_free(r);
    return buf;
}

int main(void) {
    CHECK(mysql_looks_like_dump(DUMP) && !mysql_looks_like_dump("SELECT 1;"), "detection");
    MyImportStats st;
    char* sql = mysql_translate(DUMP, &st);
    CHECK(sql && !strstr(sql, "ENGINE") && !strstr(sql, "unsigned") && !strstr(sql, "COLLATE") && !strstr(sql, "LOCK TABLES"), "cleaned:\n%s", sql ? sql : "");
    CHECK(sql && strstr(sql, "CREATE INDEX `ix_nome` ON `cliente` (`nome`)") && strstr(sql, "CONSTRAINT `uq_email` UNIQUE (`email`)"), "indexes: %s", sql ? sql : "");
    CHECK(sql && strstr(sql, "CREATE INDEX `ix_tipo_attivo` ON `cliente` (`tipo`,`attivo`)"), "multi-column index kept");
    CHECK(sql && strstr(sql, "CREATE INDEX `ix_ordine_quando` ON `ordine` (`quando`)"), "unnamed index: %s", sql ? sql : "");
    CHECK(st.indexes_dropped == 1, "fulltext index reported: %d", st.indexes_dropped);
    CHECK(st.skipped >= 5, "skipped SET/LOCK/...: %d", st.skipped);

    if (system("rm -rf /tmp/sv_my_test && mkdir -p /tmp/sv_my_test") != 0) return 1;
    if (!db_init("/tmp/sv_my_test")) return 1;
    QueryResult* q = db_execute("CREATE DATABASE shop"); db_result_free(q);
    q = db_execute("USE shop"); db_result_free(q);
    int len = (int)strlen(sql), errors = 0;
    SqlRange r[128];
    int n = sql_split(sql, len, r, 128);
    for (int i = 0; i < n; i++) {
        if (!r[i].has_code) continue;
        char save = sql[r[i].end];
        sql[r[i].end] = '\0';
        QueryResult* x = db_execute(sql + r[i].code_start);
        if (!x || !x->success) { errors++; printf("  stmt failed: %.120s -> %s\n", sql + r[i].code_start, x ? x->error : "null"); }
        db_result_free(x);
        sql[r[i].end] = save;
    }
    CHECK(errors == 0, "all translated statements run (%d errors)", errors);
    CHECK(!strcmp(scalar("SELECT COUNT(*) FROM cliente"), "3") && !strcmp(scalar("SELECT COUNT(*) FROM ordine"), "2"), "rows imported");
    CHECK(!strcmp(scalar("SELECT nome FROM cliente WHERE id = 2"), "L'Ottica Srl"), "escaped quote: %s", scalar("SELECT nome FROM cliente WHERE id = 2"));
    CHECK(!strcmp(scalar("SELECT note FROM cliente WHERE id = 1"), "dice \"ciao\"; e poi va"), "semicolon inside a string: %s", scalar("SELECT note FROM cliente WHERE id = 1"));
    CHECK(!strcmp(scalar("SELECT saldo FROM cliente WHERE id = 3"), "-3.25"), "decimal: %s", scalar("SELECT saldo FROM cliente WHERE id = 3"));
    CHECK(!strcmp(scalar("SELECT stato FROM ordine WHERE id = 10"), "nuovo,pagato"), "set value kept");
    CHECK(!strcmp(scalar("SELECT COUNT(*) FROM vista"), "2"), "view from the dump: %s", scalar("SELECT COUNT(*) FROM vista"));
    CHECK(!strcmp(scalar("CALL clienti_di('privato')"), "2"), "procedure from the dump: %s", scalar("CALL clienti_di('privato')"));
    CHECK(strstr(scalar("INSERT INTO ordine (id, cliente_id) VALUES (-1, 1)"), "id negativo"), "trigger from the dump: %s", scalar("INSERT INTO ordine (id, cliente_id) VALUES (-1, 1)"));
    q = db_execute("INSERT INTO cliente (nome, email) VALUES ('X', 'anna@x.it')");
    CHECK(q && !q->success, "unique email enforced");
    db_result_free(q);
    q = db_execute("INSERT INTO cliente (nome) VALUES ('Nuovo')");
    CHECK(q && q->success && !strcmp(scalar("SELECT tipo FROM cliente WHERE nome = 'Nuovo'"), "privato") && !strcmp(scalar("SELECT id FROM cliente WHERE nome = 'Nuovo'"), "4"),
          "defaults and auto increment: %s", q ? q->error : "");
    db_result_free(q);
    q = db_execute("DELETE FROM cliente WHERE id = 1");
    db_result_free(q);
    CHECK(!strcmp(scalar("SELECT COUNT(*) FROM ordine"), "1"), "foreign key ON DELETE CASCADE kept");
    free(sql);
    db_shutdown();
    if (system("rm -rf /tmp/sv_my_test")) {}
    if (g_fail) { printf("mysql import tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d mysql import checks passed\n", g_tests, g_tests);
    return 0;
}
