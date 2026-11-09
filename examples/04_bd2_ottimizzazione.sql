-- ═══════════════════════════════════════════════════════════════════
--  Basi di Dati 2 — strutture di accesso e ottimizzazione delle query
--  Esegui la prima parte con F5 (crea ~10.000 ordini), poi le query una
--  alla volta con Ctrl+Enter e confronta i piani.
-- ═══════════════════════════════════════════════════════════════════

DROP DATABASE IF EXISTS negozio;
CREATE DATABASE negozio;
USE negozio;

-- Un trucco per generare dati senza script esterni: il prodotto cartesiano
-- di quattro copie delle cifre 0..9 dà 10.000 combinazioni.
CREATE TABLE cifra (d INT);
INSERT INTO cifra VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);
CREATE TABLE numero (n INT PRIMARY KEY);
INSERT INTO numero SELECT a.d + 10 * b.d + 100 * c.d + 1000 * e.d + 1 FROM cifra a, cifra b, cifra c, cifra e;

CREATE TABLE cliente (
    id      INT PRIMARY KEY,
    nome    VARCHAR(20) NOT NULL,
    regione VARCHAR(10) NOT NULL
);
INSERT INTO cliente
SELECT n, CONCAT('Cliente ', n),
       CASE MOD(n, 4) WHEN 0 THEN 'Nord' WHEN 1 THEN 'Centro' WHEN 2 THEN 'Sud' ELSE 'Isole' END
FROM numero WHERE n <= 500;

CREATE TABLE ordine (
    id      INT PRIMARY KEY,
    cliente INT NOT NULL REFERENCES cliente,
    importo INT NOT NULL CHECK (importo >= 0),
    stato   VARCHAR(10) NOT NULL
);
INSERT INTO ordine
SELECT n, MOD(n, 500) + 1, MOD(n * 37, 1000),
       CASE WHEN MOD(n, 10) = 0 THEN 'annullato' ELSE 'evaso' END
FROM numero;

SELECT COUNT(*) AS ordini, COUNT(DISTINCT cliente) AS clienti, AVG(importo) AS importo_medio FROM ordine;

-- ── 1. Accesso con indice o scansione sequenziale ─────────────────
-- La chiave primaria ha un indice (hash in memoria): una sola riga letta
EXPLAIN ANALYZE SELECT * FROM ordine WHERE id = 4242;

-- Sulla colonna cliente non c'è un indice: si leggono tutte le 10.000 righe
EXPLAIN ANALYZE SELECT * FROM ordine WHERE cliente = 42;

-- Con un indice secondario la stessa query legge solo le 20 righe del cliente
CREATE INDEX idx_ordine_cliente ON ordine (cliente);
EXPLAIN ANALYZE SELECT * FROM ordine WHERE cliente = 42;
SHOW INDEXES FROM ordine;

-- Un intervallo sulla chiave: l'indice tiene anche le chiavi in ordine (come un B+-albero),
-- trova le 50 righe con una ricerca binaria e le legge pagina per pagina
EXPLAIN ANALYZE SELECT * FROM ordine WHERE id BETWEEN 5001 AND 5050;

-- Se l'intervallo contiene quasi tutta la tabella l'indice non conviene: ogni riga
-- costerebbe un accesso alla sua pagina, la scansione legge le pagine in sequenza
EXPLAIN SELECT * FROM ordine WHERE id > 100;

-- ── 2. Selettività e stime ────────────────────────────────────────
-- Le stime usano le selettività di default di System R:
--   colonna = costante -> 1/10, confronto di intervallo -> 1/3, altro -> 1/2.
-- Confronta "Est. rows" con "Actual rows": dove sbaglia la stima e perché?
EXPLAIN ANALYZE SELECT * FROM ordine WHERE importo > 900;
EXPLAIN ANALYZE SELECT * FROM ordine WHERE stato = 'annullato' AND importo < 100;

-- ── 3. Join: pushdown della selezione e hash join ─────────────────
-- La condizione sulla regione viene applicata leggendo cliente, prima del join;
-- l'uguaglianza fra le tabelle diventa un hash join (non un prodotto cartesiano).
EXPLAIN ANALYZE
SELECT c.regione, COUNT(*) AS ordini, SUM(o.importo) AS totale
FROM ordine o, cliente c
WHERE o.cliente = c.id AND c.regione = 'Nord' AND o.stato = 'evaso'
GROUP BY c.regione;

-- ── 4. Subquery: una volta sola o una volta per riga ──────────────
-- Non correlata: calcolata una volta, i valori in una tabella hash
EXPLAIN ANALYZE SELECT COUNT(*) FROM cliente WHERE id IN (SELECT cliente FROM ordine WHERE importo > 990);

-- Correlata: eseguita per ogni cliente, ma le righe di ordine vengono lette
-- una volta e cercate per chiave (guarda "executed ... times")
EXPLAIN ANALYZE
SELECT c.id, c.nome FROM cliente c
WHERE NOT EXISTS (SELECT * FROM ordine o WHERE o.cliente = c.id AND o.stato = 'annullato');

-- ── 5. ORDER BY + LIMIT ───────────────────────────────────────────
-- Si ordina e si taglia prima di calcolare le colonne: guarda le righe della Projection
EXPLAIN ANALYZE SELECT id, cliente, importo FROM ordine ORDER BY importo DESC, id LIMIT 5;

-- ── 6. Buffer pool, log e checkpoint ──────────────────────────────
-- Il buffer pool tiene 1024 pagine da 4 KB e sostituisce con l'algoritmo CLOCK.
-- buffer_hit_ratio: richieste servite senza leggere dal disco.
SHOW STATUS;

-- Il checkpoint scrive su disco tutte le pagine modificate e svuota il log (WAL)
CHECKPOINT;
SHOW STATUS;
