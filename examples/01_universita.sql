-- ═══════════════════════════════════════════════════════════════════
--  Basi di Dati 1 — schema "università" con vincoli di integrità
--  Esegui tutto con F5: crea il database e lo popola.
--  Gli altri script degli esempi (02_..., 03_...) usano questo database.
-- ═══════════════════════════════════════════════════════════════════

DROP DATABASE IF EXISTS universita;   -- lo script si può rieseguire da capo
CREATE DATABASE universita;
USE universita;

-- DIPARTIMENTO(codice, nome)
CREATE TABLE dipartimento (
    codice  CHAR(4)     PRIMARY KEY,
    nome    VARCHAR(60) NOT NULL UNIQUE
);

-- DOCENTE(matricola, nome, cognome, dipartimento → DIPARTIMENTO)
CREATE TABLE docente (
    matricola    INT PRIMARY KEY,
    nome         VARCHAR(30) NOT NULL,
    cognome      VARCHAR(30) NOT NULL,
    dipartimento CHAR(4) REFERENCES dipartimento ON DELETE SET NULL ON UPDATE CASCADE
);

-- CORSO(codice, titolo, cfu, docente → DOCENTE)
CREATE TABLE corso (
    codice   VARCHAR(6)  PRIMARY KEY,
    titolo   VARCHAR(60) NOT NULL,
    cfu      INT NOT NULL CHECK (cfu BETWEEN 1 AND 15),
    docente  INT REFERENCES docente
);

-- STUDENTE(matricola, nome, cognome, citta, anno_iscrizione)
CREATE TABLE studente (
    matricola       INT PRIMARY KEY,
    nome            VARCHAR(30) NOT NULL,
    cognome         VARCHAR(30) NOT NULL,
    citta           VARCHAR(30),
    anno_iscrizione INT CHECK (anno_iscrizione >= 2000)
);

-- ESAME(studente → STUDENTE, corso → CORSO, data, voto, lode)
-- chiave primaria composta: uno studente supera un corso una volta sola
CREATE TABLE esame (
    studente INT        NOT NULL REFERENCES studente ON DELETE CASCADE,
    corso    VARCHAR(6) NOT NULL REFERENCES corso,
    data     DATE       NOT NULL,
    voto     INT        NOT NULL,
    lode     BOOLEAN    DEFAULT 0,
    PRIMARY KEY (studente, corso),
    CONSTRAINT voto_valido CHECK (voto BETWEEN 18 AND 30),
    CONSTRAINT lode_solo_30 CHECK (lode = 0 OR voto = 30)
);

-- PROPEDEUTICO(corso → CORSO, richiede → CORSO): per seguire "corso" bisogna aver superato "richiede"
CREATE TABLE propedeutico (
    corso    VARCHAR(6) REFERENCES corso ON DELETE CASCADE,
    richiede VARCHAR(6) REFERENCES corso ON DELETE CASCADE,
    PRIMARY KEY (corso, richiede),
    CHECK (corso <> richiede)
);

INSERT INTO dipartimento VALUES
    ('DEIB', 'Elettronica, Informazione e Bioingegneria'),
    ('DMAT', 'Matematica'),
    ('DFIS', 'Fisica');

INSERT INTO docente VALUES
    (101, 'Paola',    'Neri',     'DEIB'),
    (102, 'Marco',    'Galli',    'DEIB'),
    (103, 'Lucia',    'Ferrari',  'DMAT'),
    (104, 'Giorgio',  'Conti',    'DFIS'),
    (105, 'Sara',     'Romano',   NULL);

INSERT INTO corso VALUES
    ('BD1',  'Basi di dati 1',           9, 101),
    ('BD2',  'Basi di dati 2',           6, 101),
    ('INF1', 'Fondamenti di informatica', 10, 102),
    ('AN1',  'Analisi matematica 1',     12, 103),
    ('FIS1', 'Fisica 1',                 10, 104),
    ('SO',   'Sistemi operativi',         8, NULL);

INSERT INTO studente VALUES
    (1001, 'Anna',    'Bianchi',  'Milano',  2022),
    (1002, 'Bruno',   'Rossi',    'Roma',    2022),
    (1003, 'Carla',   'Verdi',    'Milano',  2023),
    (1004, 'Dario',   'Esposito', 'Napoli',  2023),
    (1005, 'Elena',   'Ricci',    'Torino',  2024),
    (1006, 'Fabio',   'Marino',   'Milano',  2024),
    (1007, 'Giulia',  'Greco',    NULL,      2024),
    (1008, 'Luca',    'Bruno',    'Roma',    2021);

INSERT INTO esame VALUES
    (1001, 'BD1',  '2024-02-10', 30, 1),
    (1001, 'BD2',  '2024-07-03', 28, 0),
    (1001, 'INF1', '2023-01-20', 27, 0),
    (1001, 'AN1',  '2023-02-15', 24, 0),
    (1001, 'FIS1', '2023-06-30', 26, 0),
    (1001, 'SO',   '2024-09-12', 30, 0),
    (1002, 'BD1',  '2024-02-10', 18, 0),
    (1002, 'INF1', '2023-01-20', 30, 1),
    (1002, 'AN1',  '2023-07-10', 21, 0),
    (1003, 'BD1',  '2024-06-25', 27, 0),
    (1003, 'BD2',  '2024-07-03', 30, 1),
    (1003, 'INF1', '2024-01-22', 29, 0),
    (1003, 'AN1',  '2024-02-14', 30, 0),
    (1003, 'FIS1', '2024-06-28', 25, 0),
    (1003, 'SO',   '2024-09-12', 28, 0),
    (1004, 'INF1', '2024-01-22', 22, 0),
    (1004, 'AN1',  '2024-07-09', 19, 0),
    (1005, 'INF1', '2025-01-21', 26, 0),
    (1008, 'BD1',  '2023-02-11', 25, 0),
    (1008, 'AN1',  '2022-02-16', 23, 0);

INSERT INTO propedeutico VALUES
    ('BD1', 'INF1'),
    ('BD2', 'BD1'),
    ('SO',  'INF1'),
    ('FIS1', 'AN1');

-- Controllo: righe per tabella
SELECT 'studenti' AS tabella, COUNT(*) AS righe FROM studente
UNION ALL SELECT 'corsi', COUNT(*) FROM corso
UNION ALL SELECT 'esami', COUNT(*) FROM esame;

-- I vincoli dichiarati
SHOW CONSTRAINTS FROM esame;
