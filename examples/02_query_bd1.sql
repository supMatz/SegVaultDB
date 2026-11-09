-- ═══════════════════════════════════════════════════════════════════
--  Basi di Dati 1 — esercizi di interrogazione sullo schema "università"
--  Prima esegui 01_universita.sql. Poi Ctrl+Enter su una query per
--  eseguire solo quella (ogni SELECT apre la sua scheda di risultati).
-- ═══════════════════════════════════════════════════════════════════

USE universita;

-- ── Selezione, proiezione, ordinamento ────────────────────────────
-- 1. Nome e cognome degli studenti di Milano, in ordine alfabetico
SELECT nome, cognome FROM studente WHERE citta = 'Milano' ORDER BY cognome, nome;

-- 2. Città degli studenti, senza ripetizioni (NULL compare una volta sola)
SELECT DISTINCT citta FROM studente ORDER BY citta;

-- 3. Esami con voto fra 27 e 30 sostenuti nel 2024
SELECT studente, corso, voto FROM esame
WHERE voto BETWEEN 27 AND 30 AND data >= '2024-01-01' AND data < '2025-01-01'
ORDER BY voto DESC, studente;

-- 4. Studenti senza città: attenzione, "citta = NULL" non è mai vero
SELECT matricola, nome FROM studente WHERE citta IS NULL;

-- 5. Cognomi che iniziano per R o terminano per "no"
SELECT cognome FROM studente WHERE cognome LIKE 'R%' OR cognome LIKE '%no';

-- ── Join ──────────────────────────────────────────────────────────
-- 6. Per ogni esame: studente, titolo del corso e voto (join implicito, come nell'algebra relazionale)
SELECT s.cognome, c.titolo, e.voto
FROM studente s, esame e, corso c
WHERE s.matricola = e.studente AND e.corso = c.codice
ORDER BY s.cognome, c.titolo;

-- 7. La stessa query con JOIN esplicito
SELECT s.cognome, c.titolo, e.voto
FROM esame e
JOIN studente s ON s.matricola = e.studente
JOIN corso c    ON c.codice = e.corso
ORDER BY s.cognome, c.titolo;

-- 8. Corsi e docente; anche i corsi senza docente (outer join)
SELECT c.titolo, d.cognome AS docente
FROM corso c LEFT JOIN docente d ON d.matricola = c.docente
ORDER BY c.titolo;

-- 9. Self join: coppie di studenti della stessa città (ogni coppia una volta)
SELECT a.nome, b.nome, a.citta
FROM studente a JOIN studente b ON a.citta = b.citta AND a.matricola < b.matricola
ORDER BY a.citta, a.nome;

-- ── Aggregati, GROUP BY, HAVING ───────────────────────────────────
-- 10. Numero di esami, media e voto massimo per corso
SELECT corso, COUNT(*) AS esami, AVG(voto) AS media, MAX(voto) AS massimo
FROM esame GROUP BY corso ORDER BY media DESC;

-- 11. Studenti con almeno 3 esami e media almeno 27
SELECT studente, COUNT(*) AS esami, ROUND(AVG(voto), 2) AS media
FROM esame GROUP BY studente
HAVING COUNT(*) >= 3 AND AVG(voto) >= 27;

-- 12. Media ponderata sui CFU di ogni studente
SELECT s.matricola, s.cognome, ROUND(SUM(e.voto * c.cfu) * 1.0 / SUM(c.cfu), 2) AS media_ponderata
FROM studente s JOIN esame e ON e.studente = s.matricola JOIN corso c ON c.codice = e.corso
GROUP BY s.matricola, s.cognome
ORDER BY media_ponderata DESC;

-- 13. CFU acquisiti da tutti gli studenti, anche da chi non ha esami (COUNT e SUM con LEFT JOIN)
SELECT s.matricola, s.cognome, COUNT(e.corso) AS esami, COALESCE(SUM(c.cfu), 0) AS cfu
FROM studente s
LEFT JOIN esame e ON e.studente = s.matricola
LEFT JOIN corso c ON c.codice = e.corso
GROUP BY s.matricola, s.cognome
ORDER BY cfu DESC, s.matricola;

-- ── Interrogazioni annidate ───────────────────────────────────────
-- 14. Studenti che hanno preso almeno un 30 (IN)
SELECT nome, cognome FROM studente
WHERE matricola IN (SELECT studente FROM esame WHERE voto = 30);

-- 15. Studenti che non hanno mai sostenuto esami (NOT IN e NOT EXISTS: stesso risultato)
SELECT nome FROM studente WHERE matricola NOT IN (SELECT studente FROM esame);
SELECT nome FROM studente s WHERE NOT EXISTS (SELECT * FROM esame e WHERE e.studente = s.matricola);

-- 16. Il voto più alto di Basi di dati 1 e chi l'ha preso (subquery scalare)
SELECT studente, voto FROM esame
WHERE corso = 'BD1' AND voto = (SELECT MAX(voto) FROM esame WHERE corso = 'BD1');

-- 17. Esami con un voto superiore alla media del proprio corso (subquery correlata)
SELECT e.studente, e.corso, e.voto FROM esame e
WHERE e.voto > (SELECT AVG(e2.voto) FROM esame e2 WHERE e2.corso = e.corso)
ORDER BY e.corso, e.voto DESC;

-- 18. Studenti con la media più alta (>= ALL)
SELECT studente, AVG(voto) AS media FROM esame GROUP BY studente
HAVING AVG(voto) >= ALL (SELECT AVG(voto) FROM esame GROUP BY studente);

-- 19. Divisione relazionale: studenti che hanno superato TUTTI i corsi del docente 101
--     "non esiste un corso del docente 101 per cui non esista un esame dello studente"
SELECT s.nome, s.cognome FROM studente s
WHERE NOT EXISTS (
    SELECT * FROM corso c
    WHERE c.docente = 101
      AND NOT EXISTS (SELECT * FROM esame e WHERE e.studente = s.matricola AND e.corso = c.codice));

-- 20. La divisione con il conteggio: tanti esami di quel docente quanti sono i suoi corsi
SELECT e.studente FROM esame e JOIN corso c ON c.codice = e.corso
WHERE c.docente = 101
GROUP BY e.studente
HAVING COUNT(*) = (SELECT COUNT(*) FROM corso WHERE docente = 101);

-- ── Operatori insiemistici ────────────────────────────────────────
-- 21. Città di studenti o di... nessun docente: UNION elimina i duplicati, UNION ALL no
SELECT citta FROM studente WHERE citta IS NOT NULL
UNION
SELECT 'Pavia'
ORDER BY citta;

-- 22. Studenti che hanno superato sia BD1 sia BD2 (INTERSECT)
SELECT studente FROM esame WHERE corso = 'BD1'
INTERSECT
SELECT studente FROM esame WHERE corso = 'BD2';

-- 23. Studenti che hanno superato BD1 ma non BD2 (EXCEPT)
SELECT studente FROM esame WHERE corso = 'BD1'
EXCEPT
SELECT studente FROM esame WHERE corso = 'BD2';

-- ── Viste, WITH, tabelle derivate, CASE ───────────────────────────
-- 24. Una vista con la carriera di ogni studente
DROP VIEW IF EXISTS carriera;
CREATE VIEW carriera AS
    SELECT studente, COUNT(*) AS esami, AVG(voto) AS media
    FROM esame GROUP BY studente;
SELECT s.cognome, c.esami, ROUND(c.media, 2) AS media
FROM carriera c JOIN studente s ON s.matricola = c.studente
ORDER BY c.media DESC;

-- 25. WITH: la stessa domanda senza creare una vista
WITH medie AS (SELECT studente, AVG(voto) AS media FROM esame GROUP BY studente)
SELECT COUNT(*) AS studenti_sopra_27 FROM medie WHERE media > 27;

-- 26. Tabella derivata: numero medio di esami per studente (fra chi ne ha almeno uno)
SELECT AVG(n) AS esami_medi FROM (SELECT studente, COUNT(*) AS n FROM esame GROUP BY studente) AS t;

-- 27. CASE: fasce di voto
SELECT corso,
       SUM(CASE WHEN voto >= 28 THEN 1 ELSE 0 END) AS alti,
       SUM(CASE WHEN voto BETWEEN 24 AND 27 THEN 1 ELSE 0 END) AS medi,
       SUM(CASE WHEN voto < 24 THEN 1 ELSE 0 END) AS bassi
FROM esame GROUP BY corso ORDER BY corso;

-- 28. Classifica: i 3 voti migliori (ORDER BY + LIMIT)
SELECT studente, corso, voto FROM esame ORDER BY voto DESC, data LIMIT 3;
