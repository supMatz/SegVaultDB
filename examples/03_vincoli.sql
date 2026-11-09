-- ═══════════════════════════════════════════════════════════════════
--  Basi di Dati 1 — vincoli di integrità in azione
--  Prima esegui 01_universita.sql. Esegui le istruzioni una alla volta
--  con Ctrl+Enter: quelle marcate "errore atteso" violano un vincolo e
--  vengono rifiutate senza modificare nulla.
-- ═══════════════════════════════════════════════════════════════════

USE universita;

-- ── Vincoli di chiave ─────────────────────────────────────────────
-- errore atteso: chiave primaria duplicata
INSERT INTO studente VALUES (1001, 'Mario', 'Rossi', 'Pavia', 2024);

-- errore atteso: chiave primaria composta (studente, corso) duplicata
INSERT INTO esame VALUES (1001, 'BD1', '2025-01-10', 29, 0);

-- errore atteso: UNIQUE sul nome del dipartimento
INSERT INTO dipartimento VALUES ('DXXX', 'Matematica');

-- ── NOT NULL e CHECK ──────────────────────────────────────────────
-- errore atteso: NOT NULL
INSERT INTO studente (matricola, nome) VALUES (2000, 'Senza cognome');

-- errore atteso: CHECK voto_valido (voto fra 18 e 30)
INSERT INTO esame VALUES (1005, 'BD1', '2025-02-01', 31, 0);

-- errore atteso: CHECK lode_solo_30
INSERT INTO esame VALUES (1005, 'BD1', '2025-02-01', 28, 1);

-- un CHECK con valore sconosciuto (NULL) è soddisfatto, come prevede SQL
INSERT INTO studente VALUES (2001, 'Nuovo', 'Iscritto', NULL, NULL);

-- ── Integrità referenziale ────────────────────────────────────────
-- errore atteso: lo studente 9999 non esiste
INSERT INTO esame VALUES (9999, 'BD1', '2025-02-01', 25, 0);

-- errore atteso: il corso XYZ non esiste
UPDATE esame SET corso = 'XYZ' WHERE studente = 1002 AND corso = 'BD1';

-- errore atteso: ON DELETE NO ACTION (predefinito) su esame.corso: BD1 ha degli esami
DELETE FROM corso WHERE codice = 'BD1';

-- ON DELETE CASCADE: cancellare uno studente cancella i suoi esami
SELECT COUNT(*) AS esami_di_1008 FROM esame WHERE studente = 1008;
DELETE FROM studente WHERE matricola = 1008;
SELECT COUNT(*) AS esami_di_1008 FROM esame WHERE studente = 1008;

-- ON UPDATE CASCADE: il nuovo codice del dipartimento arriva ai docenti
UPDATE dipartimento SET codice = 'DINF' WHERE codice = 'DEIB';
SELECT matricola, cognome, dipartimento FROM docente ORDER BY matricola;

-- ON DELETE SET NULL: i docenti restano, senza dipartimento
DELETE FROM dipartimento WHERE codice = 'DFIS';
SELECT matricola, cognome, dipartimento FROM docente WHERE dipartimento IS NULL;

-- errore atteso: una tabella riferita non si può eliminare
DROP TABLE corso;

-- ── Vincoli aggiunti e tolti dopo ─────────────────────────────────
-- i dati esistenti vengono controllati prima di aggiungere il vincolo
-- errore atteso: Giulia (1007) ha un anno di iscrizione ma nessuna città
ALTER TABLE studente ADD CONSTRAINT citta_nota CHECK (citta IS NOT NULL OR anno_iscrizione IS NULL);

-- nessuna coppia (nome, cognome) si ripete: il vincolo viene aggiunto
ALTER TABLE studente ADD CONSTRAINT nome_cognome_unici UNIQUE (nome, cognome);
SHOW CONSTRAINTS FROM studente;
ALTER TABLE studente DROP CONSTRAINT nome_cognome_unici;

-- lo schema completo, rigenerato dal catalogo
SHOW CREATE TABLE esame;
