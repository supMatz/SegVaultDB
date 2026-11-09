-- ═══════════════════════════════════════════════════════════════════
--  Basi di Dati 2 — transazioni e basi di dati attive (trigger)
--  Esegui le istruzioni una alla volta con Ctrl+Enter e guarda i
--  risultati; quelle marcate "errore atteso" vengono rifiutate.
-- ═══════════════════════════════════════════════════════════════════

DROP DATABASE IF EXISTS banca;
CREATE DATABASE banca;
USE banca;

CREATE TABLE conto (
    numero  INT PRIMARY KEY,
    titolare VARCHAR(30) NOT NULL,
    saldo   INT NOT NULL DEFAULT 0
);
CREATE TABLE movimento (
    id      INT PRIMARY KEY AUTO_INCREMENT,
    conto   INT NOT NULL REFERENCES conto ON DELETE CASCADE,
    importo INT NOT NULL,
    causale VARCHAR(40)
);
CREATE TABLE storico_saldo (
    conto   INT,
    prima   INT,
    dopo    INT
);

INSERT INTO conto VALUES (1, 'Anna', 1000), (2, 'Bruno', 200), (3, 'Carla', 0);

-- ── Transazioni: COMMIT, ROLLBACK, SAVEPOINT ──────────────────────
-- Un bonifico: le due UPDATE avvengono tutte e due o nessuna (atomicità)
BEGIN;
UPDATE conto SET saldo = saldo - 300 WHERE numero = 1;
UPDATE conto SET saldo = saldo + 300 WHERE numero = 2;
COMMIT;
SELECT * FROM conto ORDER BY numero;

-- ROLLBACK annulla tutto quello che la transazione ha fatto (undo dal log WAL)
BEGIN;
UPDATE conto SET saldo = 0;
SELECT SUM(saldo) AS totale_durante FROM conto;
ROLLBACK;
SELECT SUM(saldo) AS totale_dopo FROM conto;

-- SAVEPOINT: si annulla solo l'ultima parte
BEGIN;
UPDATE conto SET saldo = saldo + 10 WHERE numero = 3;
SAVEPOINT dopo_primo;
UPDATE conto SET saldo = saldo + 1000 WHERE numero = 3;
ROLLBACK TO dopo_primo;
COMMIT;
SELECT saldo FROM conto WHERE numero = 3;

-- Il log (write-ahead log) contiene le modifiche fino al checkpoint
SHOW STATUS;

-- ── Trigger: regole evento - condizione - azione ──────────────────
-- 1. BEFORE ... FOR EACH ROW con WHEN e SIGNAL: rifiuta un saldo negativo
CREATE TRIGGER no_scoperto
BEFORE UPDATE OF saldo ON conto
FOR EACH ROW
WHEN (NEW.saldo < 0)
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Operazione rifiutata: saldo negativo';

-- 2. AFTER ... FOR EACH ROW: tiene lo storico dei saldi (OLD e NEW)
CREATE TRIGGER storico
AFTER UPDATE OF saldo ON conto
FOR EACH ROW
BEGIN
    INSERT INTO storico_saldo VALUES (OLD.numero, OLD.saldo, NEW.saldo);
END;

-- 3. AFTER INSERT: ogni movimento aggiorna il saldo (che a sua volta attiva no_scoperto e storico)
CREATE TRIGGER applica_movimento
AFTER INSERT ON movimento
FOR EACH ROW
UPDATE conto SET saldo = saldo + NEW.importo WHERE numero = NEW.conto;

-- 4. BEFORE INSERT con IF: completa la riga prima che venga scritta
CREATE TRIGGER causale_predefinita
BEFORE INSERT ON movimento
FOR EACH ROW
BEGIN
    IF NEW.causale IS NULL AND NEW.importo >= 0 THEN
        SET NEW.causale = 'versamento';
    ELSEIF NEW.causale IS NULL THEN
        SET NEW.causale = 'prelievo';
    END IF;
END;

SHOW TRIGGERS;

INSERT INTO movimento (conto, importo) VALUES (1, 150);
INSERT INTO movimento (conto, importo) VALUES (2, -100);
SELECT * FROM movimento;
SELECT * FROM conto ORDER BY numero;
SELECT * FROM storico_saldo;

-- errore atteso: il prelievo porterebbe il saldo sotto zero; l'intero INSERT viene annullato
INSERT INTO movimento (conto, importo, causale) VALUES (3, -500, 'bancomat');
SELECT COUNT(*) AS movimenti FROM movimento;

-- errore atteso: anche una UPDATE diretta viene controllata
UPDATE conto SET saldo = saldo - 5000 WHERE numero = 1;

-- 5. Trigger a livello di istruzione: una volta per statement, senza NEW e OLD
CREATE TABLE contatore (operazioni INT);
INSERT INTO contatore VALUES (0);
CREATE TRIGGER conta_cancellazioni AFTER DELETE ON movimento FOR EACH STATEMENT
UPDATE contatore SET operazioni = operazioni + 1;
DELETE FROM movimento WHERE importo < 0;
DELETE FROM movimento;
SELECT operazioni FROM contatore;

DROP TRIGGER conta_cancellazioni;
