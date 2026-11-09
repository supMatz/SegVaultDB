# 🗄️ SegVault — Relational DBMS in pure C

> A fully hand-crafted relational database management system written in C,  
> complete with a native GUI for both **Windows** (Win32) and **Linux** (Xlib/X11),  
> a hand-written SQL parser, a B+Tree storage engine, Write-Ahead Logging, and zero dependencies.
---

## 📋 Table of Contents

- [Overview](#overview)
- [Per i corsi di Basi di Dati 1 e 2](#per-i-corsi-di-basi-di-dati-1-e-2)
- [Supported SQL](#supported-sql)
- [Architecture](#architecture)
- [Prestazioni](#prestazioni)
- [GUI Layout](#gui-layout)
- [Source Structure](#source-structure)
- [Documentation & References](#documentation--references)
  - [GUI — Win32 (Windows)](#1--gui--win32-windows)
  - [GUI — Xlib (Linux/X11)](#2--gui--xlibx11-linux)
  - [C Standard Library](#3--c-standard-library)
  - [File I/O — POSIX](#4--file-io--posix-storage-engine)
  - [SQL Parser — Flex + Bison](#5--sql-parser--flex--bison)
  - [Data Structures](#6--data-structures-in-c)
  - [WAL & Recovery (ARIES)](#7--write-ahead-log--recovery-aries)
  - [Reference Source Code](#8--reference-source-code)
  - [Cross-compilation Linux → Windows](#9--cross-compilation-linux--windows)
- [Building](#building)
- [License](#license)

---

## Overview

**SegVault** is a didactic yet fully functional relational DBMS built entirely in C — no external libraries, no frameworks, no shortcuts.
Pensato per i corsi di **Basi di Dati 1 e 2** di ingegneria informatica: lo stesso programma fa da ambiente per gli esercizi di SQL e da laboratorio sugli interni di un DBMS (piani di esecuzione, indici, buffer pool, log, transazioni, trigger).

| Feature | Details |
|---|---|
| **Language** | C (C11), zero dipendenze per il motore |
| **Version** | 1.2.0 |
| **GUI** | Win32 / GDI (Windows) · Xlib/X11 + Xft (Linux) |
| **Font** | JetBrains Mono (editor e dati) · Inter / Fira Sans (interfaccia), con fallback automatico |
| **SQL** | Lexer e parser a discesa ricorsiva scritti a mano; subquery (anche correlate), `UNION` / `INTERSECT` / `EXCEPT`, `WITH [RECURSIVE]`, funzioni finestra, `FULL` / `NATURAL` / `USING` join, `CASE`, viste, vincoli, trigger, `ALTER TABLE ... COLUMN` |
| **Vincoli** | `PRIMARY KEY` (anche composta), `UNIQUE`, `NOT NULL`, `CHECK`, `FOREIGN KEY` con `ON DELETE` / `ON UPDATE` `CASCADE`, `SET NULL`, `SET DEFAULT`, `RESTRICT` |
| **Storage** | Heap file a pagine da 4 KB con slot directory e free-space map; indici in memoria (anche su più colonne): tabella hash per le uguaglianze, chiavi ordinate per gli intervalli |
| **Buffer pool** | 1024 frame, tabella hash per la ricerca, sostituzione **CLOCK**; statistiche con `SHOW STATUS` |
| **Ottimizzatore** | Pushdown delle selezioni, hash join (anche per `FROM a, b WHERE a.x = b.y`), accesso con indice per uguaglianza o per intervallo (`<`, `BETWEEN`, scelto solo se conviene), subquery decorrelate per chiave; `EXPLAIN` con stime alla System R e `EXPLAIN ANALYZE` con le righe reali |
| **Transazioni** | Write-Ahead Log con undo/redo, `BEGIN` / `COMMIT` / `ROLLBACK`, `SAVEPOINT`, recovery dopo un crash, `CHECKPOINT` (anche automatico) |
| **Basi di dati attive** | Trigger `BEFORE` / `AFTER`, `FOR EACH ROW` / `FOR EACH STATEMENT`, `NEW` / `OLD`, `WHEN`, `SET NEW.col`, `SIGNAL`, `IF` |
| **Cross-compilation** | MinGW-w64 — `make windows` produce `segvault.exe` da Linux |

---

## Per i corsi di Basi di Dati 1 e 2

La cartella [`examples/`](examples/) contiene script pronti da aprire nell'editor (`Ctrl+O`): ogni istruzione è commentata con la domanda a cui risponde. Il test `make test` li esegue tutti, quindi funzionano sempre con la versione corrente.

| Script | Argomenti |
|---|---|
| [`01_universita.sql`](examples/01_universita.sql) | Schema relazionale con chiavi primarie (anche composte), chiavi esterne con azioni referenziali, `UNIQUE`, `CHECK`, `DEFAULT` — il database usato dagli esercizi |
| [`02_query_bd1.sql`](examples/02_query_bd1.sql) | 36 esercizi: selezione e proiezione, join impliciti ed espliciti, outer join (anche `FULL`), self join, `USING`, aggregati con `GROUP BY` / `HAVING`, `IN` / `EXISTS` / `ANY` / `ALL`, subquery correlate, **divisione relazionale** (doppio `NOT EXISTS` e conteggio), `UNION` / `INTERSECT` / `EXCEPT`, viste, `WITH`, tabelle derivate, `CASE`, **chiusura transitiva** con `WITH RECURSIVE` (propedeuticità), classifiche, somme cumulative e medie mobili con le **funzioni finestra** |
| [`03_vincoli.sql`](examples/03_vincoli.sql) | Ogni vincolo violato apposta (le istruzioni marcate *errore atteso*), `CASCADE`, `SET NULL`, `ON UPDATE CASCADE`, `ALTER TABLE ADD/DROP CONSTRAINT`, `ADD / RENAME / DROP COLUMN`, `SHOW CREATE TABLE` |
| [`04_bd2_ottimizzazione.sql`](examples/04_bd2_ottimizzazione.sql) | Generazione di 10.000 righe con un prodotto cartesiano, scansione sequenziale contro accesso con indice (per chiave e per intervallo), `CREATE INDEX`, selettività e stime, pushdown e hash join, subquery correlate, `ORDER BY` + `LIMIT`, buffer pool e checkpoint |
| [`05_bd2_transazioni_trigger.sql`](examples/05_bd2_transazioni_trigger.sql) | Atomicità con `COMMIT` / `ROLLBACK`, `SAVEPOINT`, log WAL; trigger evento-condizione-azione, trigger a cascata, rifiuto con `SIGNAL`, trigger per riga e per istruzione |

**Basi di Dati 1** — modello relazionale e SQL: tutto il linguaggio di interrogazione dei corsi (compresi gli operatori insiemistici e le interrogazioni annidate con la logica a tre valori di SQL: `NOT IN` con un `NULL` non è mai vero, `ALL` su un insieme vuoto è vero), viste, vincoli di integrità di dominio, di tupla, di chiave e referenziali, con controllo prima di ogni scrittura: uno statement che viola un vincolo non modifica nulla.

**Basi di Dati 2** — come funziona un DBMS:
- `EXPLAIN` mostra il piano: scansione sequenziale o accesso con indice, filtri anticipati, hash join o nested loop, aggregazione, ordinamento, con le **stime delle righe secondo le selettività di default di System R** (1/10 per `=`, 1/3 per un intervallo). `EXPLAIN ANALYZE` esegue la query e affianca le righe reali di ogni passo, il numero di esecuzioni delle subquery correlate e i tempi.
- `SHOW STATUS` riporta richieste al buffer pool, hit, miss, hit ratio, sostituzioni (CLOCK), pagine scritte, dimensione del log e checkpoint; `SHOW INDEXES FROM t` gli indici e se sono già stati costruiti.
- Transazioni con WAL: ogni modifica viene scritta nel log prima delle pagine; `ROLLBACK` usa le immagini precedenti (undo), dopo un crash le transazioni completate vengono rifatte (redo) e quelle aperte annullate. `CHECKPOINT` scrive le pagine e svuota il log.
- Trigger con la semantica dei DBMS reali: i `BEFORE` vedono e possono cambiare la riga prima dei controlli dei vincoli, gli `AFTER` possono attivare altri trigger; se un trigger fallisce lo statement viene annullato per intero.

---

## Supported SQL

Tutte le query si scrivono nell'editor SQL:

- **Run** (`F5`) esegue l'intero script, oppure solo il testo selezionato
- **Run statement** (`Ctrl+Enter`) esegue solo lo statement sotto il cursore (evidenziato da una barra colorata nel margine)

Gli statement separati da `;` vengono eseguiti in sequenza (i `;` dentro stringhe e commenti sono ignorati). In caso di errore l'esecuzione si ferma, la riga dello statement fallito viene evidenziata in rosso nell'editor e il messaggio compare nel pannello dei risultati.

### Editor SQL — scorciatoie

| Scorciatoia | Azione |
|---|---|
| `F5` | Esegui script / selezione |
| `Ctrl+Enter` | Esegui lo statement sotto il cursore |
| `Ctrl+A` / `C` / `X` / `V` | Seleziona tutto / copia / taglia / incolla (senza selezione: riga intera) |
| `Ctrl+Z` / `Ctrl+Y` (`Ctrl+Shift+Z`) | Annulla / ripeti |
| `Ctrl+D` | Duplica riga o selezione |
| `Ctrl+/` (`Ctrl+Shift+7` su tastiera italiana) | Commenta / decommenta le righe |
| `Tab` / `Shift+Tab` | Indenta / de-indenta (anche su più righe) |
| `Ctrl+←/→`, `Ctrl+Backspace/Canc` | Movimento e cancellazione per parola |
| `Home` | "Smart home": primo carattere, poi inizio riga |
| `Ctrl` + rotella, `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | Zoom del font dell'editor |
| doppio / triplo click, click sul numero di riga | Seleziona parola / riga |
| `Ctrl+Space` | Autocompletamento (si apre anche da solo dopo 2 caratteri) |
| `Alt+↑` / `Alt+↓` | Sposta la riga (o il blocco selezionato) su / giù |
| `Ctrl+Shift+K` | Elimina la riga / le righe selezionate |
| `Ctrl+L` | Seleziona la riga (ripetuto: estende alla successiva) |
| `Ctrl+Shift+U` / `Ctrl+U` | Maiuscolo / minuscolo della selezione o della parola |
| `Shift+Enter` / `Ctrl+Shift+Enter` | Nuova riga indentata sotto / sopra |
| `Ctrl+Shift+F` | Formatta l'SQL (selezione o intero script) |
| `F1` | Pannello con tutte le scorciatoie |
| `Ctrl+Shift+T` | Tema successivo |

L'editor chiude automaticamente parentesi e apici, mantiene l'indentazione andando a capo, evidenzia la parentesi corrispondente e le altre occorrenze dell'identificatore sotto il cursore, e colora keyword, tipi, funzioni, stringhe, numeri e commenti.

**Autocompletamento:** propone colonne, tabelle, viste e database del database corrente insieme a keyword, tipi e funzioni SQL. Dopo `FROM` / `JOIN` / `INTO` / `UPDATE` le tabelle vengono prima; scrivendo `tabella.` compaiono solo le colonne di quella tabella. `↑`/`↓` per scegliere, `Enter`/`Tab` o click per inserire, `Esc` per chiudere. Le keyword rispettano il maiuscolo/minuscolo che stai usando e le funzioni vengono inserite con le parentesi.

**Formattazione (`Ctrl+Shift+F` o pulsante *Format*):** keyword in maiuscolo, una clausola per riga, `AND`/`OR` indentati, colonne di `CREATE TABLE` una per riga, spaziatura normalizzata; stringhe e commenti non vengono toccati. Si annulla con `Ctrl+Z`.

### Trova e sostituisci

| Scorciatoia | Azione |
|---|---|
| `Ctrl+F` | Trova (precompilato con la selezione o la parola sotto il cursore) |
| `Ctrl+H` | Trova e sostituisci (`Enter` nel campo sostituisci = sostituisci, `Ctrl+Enter` = sostituisci tutto) |
| `Enter` / `Shift+Enter`, `F3` / `Shift+F3` | Risultato successivo / precedente |
| `Alt+C` / `Alt+W` | Distingui maiuscole / solo parole intere |
| `Ctrl+G` | Vai alla riga |
| `Esc` | Chiude la barra |

### File, sessione e cronologia

| Scorciatoia | Azione |
|---|---|
| `Ctrl+N` | Nuovo script |
| `Ctrl+O` | Apri un file `.sql` |
| `Ctrl+S` / `Ctrl+Shift+S` | Salva / salva con nome |
| `Ctrl+Shift+H` (o pulsante *History*) | Cronologia delle query eseguite |

I percorsi si scrivono nella barra in alto a destra dell'editor (`~` viene espanso). Aprire un file o creare uno script nuovo resta annullabile con `Ctrl+Z`. Il footer mostra il nome del file e un pallino se ci sono modifiche non salvate.

Alla chiusura (anche con `Ctrl+C` / `SIGTERM` dal terminale) il contenuto dell'editor, la posizione del cursore, il file aperto e lo zoom vengono salvati in `<data>/session.sql` e `<data>/session.ini` e ripristinati all'avvio successivo.

Ogni statement eseguito viene registrato in `<data>/history.sql` (data, esito, righe, durata). La cronologia si apre nella griglia dei risultati: doppio click o `Enter` su una riga aggiunge lo statement in fondo allo script.

**Explorer:** click per espandere database e tabelle; doppio click su un database = `USE`, su una tabella = anteprima dei dati (`SELECT * … LIMIT 200`), su una colonna = inserisce il nome nell'editor.
**Risultati:** click su una cella e `Ctrl+C` per copiarla, frecce per navigare, trascina il bordo dell'header per ridimensionare una colonna (doppio click = adatta al contenuto), `Shift` + rotella per lo scroll orizzontale. Click sull'intestazione di una colonna = ordina (crescente → decrescente → ordine originale). `Ctrl+Shift+C` copia l'intero risultato come TSV (si incolla direttamente in un foglio di calcolo), `Ctrl+E` esporta in CSV.

### Data Definition Language (DDL)

```sql
CREATE DATABASE [IF NOT EXISTS] uni;    DROP DATABASE [IF EXISTS] uni;    USE uni;
SHOW DATABASES;   SHOW TABLES;   DESCRIBE esame;   SHOW CREATE TABLE esame;

CREATE TABLE [IF NOT EXISTS] esame (
    studente INT        NOT NULL REFERENCES studente ON DELETE CASCADE,
    corso    VARCHAR(6) NOT NULL REFERENCES corso (codice),
    data     DATE       NOT NULL,
    voto     INT        NOT NULL CHECK (voto BETWEEN 18 AND 30),
    lode     BOOLEAN    DEFAULT 0,
    PRIMARY KEY (studente, corso),                          -- chiave composta
    CONSTRAINT lode_solo_30 CHECK (lode = 0 OR voto = 30)   -- vincolo di tupla con nome
);
DROP TABLE [IF EXISTS] esame;
TRUNCATE TABLE esame;

ALTER TABLE esame RENAME TO esami;
ALTER TABLE studente ADD [COLUMN] email VARCHAR(60) DEFAULT 'n/d';      -- le righe esistenti ricevono il DEFAULT
ALTER TABLE studente RENAME COLUMN email TO posta;                       -- aggiorna anche CHECK, FK e trigger
ALTER TABLE studente DROP [COLUMN] posta;
ALTER TABLE studente ADD CONSTRAINT nome_unico UNIQUE (nome, cognome);   -- i dati esistenti vengono controllati
ALTER TABLE studente DROP CONSTRAINT nome_unico;
SHOW CONSTRAINTS FROM esame;

CREATE INDEX idx_esame_corso ON esame (corso);   DROP INDEX idx_esame_corso ON esame;
SHOW INDEXES FROM esame;

CREATE VIEW carriera AS SELECT studente, AVG(voto) AS media FROM esame GROUP BY studente;
DROP VIEW [IF EXISTS] carriera;
```

**Vincoli:** `NOT NULL`, `DEFAULT`, `AUTO_INCREMENT`, `PRIMARY KEY` (di colonna o `PRIMARY KEY (a, b)`), `UNIQUE` (più `NULL` ammessi, come in SQL), `CHECK (condizione)` di colonna o di tabella (soddisfatto se vero o sconosciuto), `FOREIGN KEY (a, b) REFERENCES t [(x, y)]` o `REFERENCES t` sulla colonna (senza colonne: la chiave primaria di `t`), con `ON DELETE` / `ON UPDATE` `NO ACTION` (predefinito), `RESTRICT`, `CASCADE`, `SET NULL`, `SET DEFAULT`; anche autoreferenziali. Le colonne riferite devono essere la chiave primaria o una chiave `UNIQUE`. Ogni statement calcola prima tutte le modifiche, comprese quelle a cascata, controlla tutti i vincoli e solo dopo scrive.

### Data Manipulation Language (DML)

```sql
INSERT INTO studente VALUES (1, 'Anna', 'Milano'), (2, 'Bruno', 'Roma');
INSERT INTO studente (matricola, nome) VALUES (3, 'Carla');
INSERT INTO archivio SELECT * FROM esame WHERE data < '2020-01-01';

UPDATE esame SET voto = voto + 1, lode = 0 WHERE corso = 'BD1' AND voto < 30;
DELETE FROM esame WHERE studente NOT IN (SELECT matricola FROM studente);
-- subquery correlata: gli esami sotto la media del proprio corso
DELETE FROM esame WHERE voto < (SELECT AVG(e2.voto) FROM esame e2 WHERE e2.corso = esame.corso);
```

### Interrogazioni

```sql
SELECT [DISTINCT] espressioni [AS alias]
FROM t1 [alias] { , t2 | [NATURAL] [INNER | LEFT [OUTER] | RIGHT [OUTER] | FULL [OUTER] | CROSS] JOIN t2 [alias]
                  [ON condizione | USING (colonne)] }
WHERE condizione
GROUP BY espressioni HAVING condizione
ORDER BY espressioni [ASC | DESC]
LIMIT n [OFFSET m];
```

| Costrutto | Esempio |
|---|---|
| Operatori | `+ - * / %`, `= <> != < > <= >=`, `AND OR NOT` (logica a tre valori), `IS [NOT] NULL`, `[NOT] LIKE 'a%_'`, `[NOT] BETWEEN`, `[NOT] IN (lista)` |
| Aggregati | `COUNT(*)`, `COUNT([DISTINCT] x)`, `SUM`, `AVG`, `MIN`, `MAX` (anche su espressioni) |
| Subquery | `x IN (SELECT ...)`, `NOT IN`, `EXISTS`, `NOT EXISTS`, `x > ALL (SELECT ...)`, `x = ANY / SOME (...)`, subquery scalari `(SELECT MAX(voto) FROM esame)` in `SELECT`, `WHERE`, `HAVING`, `SET`; correlate a qualunque profondità |
| Insiemi | `UNION [ALL]`, `INTERSECT`, `EXCEPT` (`INTERSECT` lega più degli altri), `ORDER BY` / `LIMIT` sul risultato |
| Tabelle derivate | `FROM (SELECT ...) AS t`, `JOIN (SELECT ...) AS t ON ...` |
| `WITH` | `WITH medie (studente, media) AS (SELECT ...), migliori AS (SELECT ... FROM medie) SELECT ...` |
| `WITH RECURSIVE` | `WITH RECURSIVE sotto AS (SELECT id FROM persona WHERE id = 1 UNION SELECT p.id FROM persona p JOIN sotto s ON p.capo = s.id) SELECT ...` — con `UNION` i cicli terminano; una ricorsione senza fine viene fermata dopo 1000 passi |
| Funzioni finestra | `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `NTILE(n)`, `LAG(x [, k [, def]])`, `LEAD`, `FIRST_VALUE`, `LAST_VALUE`, `COUNT/SUM/AVG/MIN/MAX(...)` con `OVER ([PARTITION BY ...] [ORDER BY ...] [ROWS \| RANGE BETWEEN ... AND ...])`: frame con `UNBOUNDED PRECEDING`, `n PRECEDING`, `CURRENT ROW`, `n FOLLOWING`, `UNBOUNDED FOLLOWING` |
| `CASE` | `CASE WHEN voto >= 28 THEN 'alto' ELSE 'basso' END`, `CASE cfu WHEN 6 THEN ... END` |
| Funzioni | `UPPER`, `LOWER`, `LENGTH`, `CHAR_LENGTH`, `TRIM`, `CONCAT`, `CONCAT_WS`, `SUBSTRING`, `LEFT`, `RIGHT`, `REPLACE`, `REVERSE`, `ABS`, `ROUND`, `FLOOR`, `CEIL`, `MOD`, `POWER`, `SQRT`, `SIGN`, `COALESCE`, `IFNULL`, `NULLIF`, `IF`, `NOW`, `CURDATE`, `YEAR`, `MONTH`, `DAY` |

Un alias nasconde il nome della tabella, come in SQL standard: in `... FROM esame e2 WHERE e2.corso = esame.corso` il secondo `esame` è la query esterna.

### Piani di esecuzione e statistiche

```sql
EXPLAIN SELECT ...;            -- piano con le righe stimate di ogni passo
EXPLAIN ANALYZE SELECT ...;    -- esegue la query: righe stimate e reali, esecuzioni delle subquery, tempo
SHOW STATUS;                   -- buffer pool (hit ratio, sostituzioni CLOCK), pagine scritte, log, checkpoint
SHOW INDEXES FROM t;           -- chiavi e indici, costruiti o no, numero di chiavi distinte
CHECKPOINT;                    -- pagine modificate su disco e log svuotato
```

### Transazioni

```sql
BEGIN;
UPDATE conto SET saldo = saldo - 100 WHERE numero = 1;
SAVEPOINT prima;
UPDATE conto SET saldo = saldo + 100 WHERE numero = 2;
ROLLBACK TO prima;      -- annulla solo la seconda UPDATE (il savepoint resta)
COMMIT;                 -- oppure ROLLBACK
```

### Trigger

```sql
CREATE TRIGGER nome {BEFORE | AFTER} {INSERT | UPDATE [OF colonne] | DELETE} ON tabella
[FOR EACH ROW | FOR EACH STATEMENT]
[WHEN (condizione su NEW / OLD)]
BEGIN
    istruzione; ...      -- INSERT, UPDATE, DELETE, SELECT,
                         -- SET NEW.colonna = valore (solo BEFORE ... FOR EACH ROW),
                         -- SIGNAL [SQLSTATE '45000'] [SET MESSAGE_TEXT =] 'messaggio',
                         -- IF condizione THEN ... [ELSEIF ... THEN ...] [ELSE ...] END IF
END;

DROP TRIGGER [IF EXISTS] nome;   SHOW TRIGGERS [FROM tabella];
```

`NEW` e `OLD` si usano nel corpo come le colonne di una query esterna (`WHERE id = NEW.conto`). Un trigger può attivarne altri fino a 16 livelli; se fallisce, l'intero statement che lo ha attivato viene annullato. Scattano anche per le righe modificate dalle azioni `CASCADE` / `SET NULL` / `SET DEFAULT` delle chiavi esterne.

### Tipi di dato supportati

| Tipo | Descrizione |
|---|---|
| `INT` / `INTEGER` | 4 byte, signed |
| `BIGINT` | 8 byte, signed |
| `FLOAT` / `DOUBLE` | 8 byte floating point |
| `VARCHAR(n)` | Stringa a lunghezza variabile |
| `CHAR(n)` | Stringa a lunghezza fissa |
| `TEXT` | Stringa lunga (max 65535 byte) |
| `BOOL` / `BOOLEAN` | 1 byte (0/1) |
| `DATE` | `YYYY-MM-DD`, controllata |
| `DATETIME` | `YYYY-MM-DD HH:MM:SS`, controllata |
| `BLOB` | Dati binari |
| `DECIMAL(p, s)` | Numero decimale (conservato come testo, confrontato come numero) |

### Non supportato

Frame `RANGE` con un intervallo di valori (`RANGE 5 PRECEDING`), `ALTER COLUMN` per cambiare il tipo di una colonna, stored procedure, controllo della concorrenza fra più utenti (il motore è embedded: una sessione alla volta).

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        GUI Layer                            │
│  window.c / window.h  —  layout principale dell'app         │
│  Widget system: button · label · textbox · table_view · …   │
│                                                             │
│    platform/win32.c (Windows)  │  platform/xlib.c (Linux)   │
│    GDI — double buffer (HDC)   │  Xlib — Pixmap backbuffer  │
└──────────────────────┬──────────────────────────────────────┘
                       │  platform.h  (interfaccia unificata)
                       │  db_api.h    (unico punto di contatto GUI ↔ DB)
┌──────────────────────▼──────────────────────────────────────┐
│                    SQL Parser Layer                          │
│        Hand-written lexer (src/query/lexer.c)                │
│        Recursive descent parser (src/query/parser.c)         │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│              Query Executor                                 │
│   piano: sorgenti → filtri anticipati → hash join → WHERE   │
│   → GROUP BY/HAVING → DISTINCT → ORDER BY/LIMIT → colonne   │
│   vincoli e trigger: modifiche calcolate, controllate, scritte │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│                  Storage Engine                             │
│    Indici hash   │  Buffer pool CLOCK (1024 pagine)         │
│    Heap File + FSM │  Page size: 4 KB                       │
│    Limiti: 64 DB · 256 tabelle · 64 colonne · 16 indici     │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│           Write-Ahead Log (WAL)                             │
│   BEGIN · COMMIT · ROLLBACK · SAVEPOINT · CHECKPOINT · redo/undo │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│           POSIX File I/O  (pread / pwrite)                  │
└─────────────────────────────────────────────────────────────┘
```

---

## GUI Layout

Il layout principale è definito in `window.h` / `window.c`:

```
┌──────────────────────────────────────────────────────┐
│  TOOLBAR: [Run] [Commit] [Rollback] [DB selector]    │
├────────────┬─────────────────────────────────────────┤
│            │  EDITOR SQL (textbox multilinea)        │
│  SIDEBAR   ├─────────────────────────────────────────┤
│  (tree)    │  STATUS BAR (righe affected, tempo ms)  │
│            ├─────────────────────────────────────────┤
│            │  RESULTS (table_view)                   │
└────────────┴─────────────────────────────────────────┘
```

Il sistema grafico è astratto da `platform.h` — tutti i widget usano **solo** quell'interfaccia e non toccano mai `<windows.h>` o `<X11/Xlib.h>` direttamente. La GUI comunica con il DB engine **esclusivamente** tramite `db_api.h`.

### Widget system

Ogni widget estende `Widget` (da `widget.h`) mettendolo come **primo campo** della propria struct — pattern che emula l'ereditarietà in C:

| Widget | File | Descrizione |
|---|---|---|
| `Widget` | `widget.h` / `widget.c` | Struct base con vtable (`draw`, `handle_event`, `destroy`) |
| `Button` | `button.h` / `button.c` | Bottone con label, hover, pressed, disabled |
| `Label` | `label.h` / `label.c` | Testo non interattivo, allineamento L/C/R |
| `Textbox` | `textbox.h` / `textbox.c` | Editor SQL multilinea: highlighting, ricerca/sostituzione, autocompletamento |
| `LineEdit` | `line_edit.h` / `line_edit.c` | Campo di input a riga singola (barra trova / vai alla riga / percorsi file) |
| `TableView` | `table_view.h` / `table_view.c` | Griglia risultati query: ordinamento, copia TSV, export CSV |
| `TreeView` | `tree_view.h` / `tree_view.c` | Sidebar oggetti DB |
| `Scrollbar` | `scrollbar.h` / `scrollbar.c` | Scrollbar verticale/orizzontale |
| `Panel` | `panel.h` / `panel.c` | Contenitore con 128 figli, bordo, padding |
| `Splitter` | `splitter.h` / `splitter.c` | Divisore ridimensionabile orizzontale/verticale |
| *(tema)* | `theme.h` / `theme.c` | Temi e palette, testo con ellissi, icone vettoriali antialiasing |
| *(SQL)* | `sql_text.h` / `sql_text.c` | Divisione degli statement, statement sotto il cursore, keyword per highlighting e autocompletamento, formattatore SQL |

### Temi

L'interfaccia ha 5 temi, selezionabili dalla pillola con il nome del tema nella toolbar (menu a tendina, anche con `↑`/`↓` e `Enter`) oppure con `Ctrl+Shift+T` per passare al successivo. La scelta viene salvata in `<data>/session.ini` e ripristinata all'avvio.

| Tema | Tipo | Accento |
|---|---|---|
| **Sky** *(predefinito)* | scuro | azzurro `#38BDF8` |
| **Violet** | scuro | viola `#7C6AFF` (la palette originale) |
| **Graphite** | scuro, grigio neutro | ambra `#F5A623` |
| **Forest** | scuro, verde | smeraldo `#34D399` |
| **Daylight** | chiaro | blu `#0284C7` |

Tutti i colori vivono in una struct `Theme` (`src/widgets/theme.h` / `theme.c`): le macro `TH_*` leggono dal tema attivo (`g_theme`), quindi il codice di disegno non cambia. Per aggiungere un tema basta una nuova voce nell'array `THEMES[]` di `theme.c`. I widget che memorizzano i propri colori (bottoni, griglia, albero, scrollbar) li ricaricano con le funzioni `*_apply_theme()` al cambio di tema.

| Costante | Uso |
|---|---|
| `TH_BG`, `TH_SURFACE`, `TH_SURFACE_2`, `TH_ELEVATED*` | Sfondi: app, sidebar/pannelli, toolbar/header, bottoni |
| `TH_ACCENT*`, `TH_ON_ACCENT` | Colore principale (azione primaria, selezione, focus) e testo sopra l'accento |
| `TH_TEXT*`, `TH_CELL_*` | Testo dell'interfaccia e delle celle |
| `TH_SUCCESS` / `TH_ERROR` / `TH_WARNING` / `TH_INFO` | Stati (OK, errore, transazione aperta, info) |
| `TH_SYN_*`, `TH_EDITOR_*`, `TH_CARET`, `TH_GUTTER_*` | Editor SQL e syntax highlighting |

---

## Prestazioni

`make bench` crea uno schema universitario con 10.000 studenti, 50 corsi e 100.000 esami e misura le query tipiche (Linux, un core):

| Operazione | Tempo |
|---|---|
| 110.050 `INSERT` (blocchi da 500 righe) | 0,11 s |
| `COUNT(*)` su 100.000 righe | 9 ms |
| `SELECT ... WHERE voto = 30` su 100.000 righe | 10 ms |
| `GROUP BY` + `AVG` su 100.000 righe | 10 ms |
| `ORDER BY ... LIMIT 10` su 100.000 righe | 11 ms |
| join di 2 tabelle (hash join) + filtro | 8 ms |
| join di 3 tabelle + `GROUP BY` | 19 ms |
| `NOT EXISTS` correlata, 10.000 × 100.000 righe | 9 ms |
| divisione relazionale (doppio `NOT EXISTS`) | 22 ms |
| ricerca per chiave primaria | 0,01 ms |
| intervallo sulla chiave primaria (`BETWEEN`, 500 righe) | 0,4 ms |
| funzione finestra con frame `ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING`, 10.000 righe | 13 ms |

Da dove viene la velocità: free-space map dell'heap (un `INSERT` non legge più tutta la tabella), buffer pool con tabella hash, log scritto una volta per statement, token e nodi del parser leggeri, scansioni senza allocare le righe scartate dai filtri e righe tenute in un'arena, numeri formattati come testo solo per il risultato, `GROUP BY` e hash join con hash tipizzati, subquery correlate risolte con una ricerca per chiave sulle righe lette una volta sola, `ORDER BY` + `LIMIT` con un heap top-k che calcola le colonne solo delle righe tenute, intervalli trovati con una ricerca binaria sulle chiavi ordinate dell'indice e righe lette in ordine di pagina, frame delle funzioni finestra che scorrono in tempo lineare.

---

## Source Structure

```
SegVaultDB/
│
├── src/
│   │
│   ├── platform/              # Astrazione OS (Win32 vs Xlib)
│   │   ├── platform.h         # Interfaccia comune: UNICA per entrambi gli OS
│   │   ├── win32.c            # Implementazione Windows
│   │   └── xlib.c             # Implementazione Linux
│   │
│   ├── widgets/               # Componenti grafici (usano solo platform.h)
│   │   ├── widget.h           # Struttura base con vtable
│   │   ├── widget.c
│   │   ├── window.h
│   │   ├── window.c           # Finestra principale + layout app
│   │   ├── button.h
│   │   ├── button.c           # Bottoni cliccabili
│   │   ├── textbox.h
│   │   ├── textbox.c          # Editor SQL multilinea
│   │   ├── label.h
│   │   ├── label.c            # Testo non interattivo
│   │   ├── table_view.h
│   │   ├── table_view.c       # Griglia risultati query
│   │   ├── tree_view.h
│   │   ├── tree_view.c        # Albero DB/tabelle nel pannello sx
│   │   ├── scrollbar.h
│   │   ├── scrollbar.c        # Scrollbar per tabelle e alberi
│   │   ├── panel.h
│   │   ├── panel.c            # Contenitore con bordo/padding
│   │   ├── splitter.h
│   │   └── splitter.c         # Divisore ridimensionabile
│   │
│   ├── storage/               # Layer fisico: pagine su disco
│   │   ├── page.h / page.c
│   │   └── buffer_pool.h / buffer_pool.c
│   │
│   ├── catalog/               # Metadati: tabelle, colonne, tipi, viste
│   │   └── schema.h / schema.c
│   │
│   ├── table/                 # Heap file + tuple
│   │   ├── heap.h / heap.c
│   │   └── tuple.h / tuple.c
│   │
│   ├── index/                 # B+Tree
│   │   └── btree.h / btree.c
│   │
│   ├── query/                 # SQL: lexer → parser → executor
│   │   ├── lexer.h / lexer.c
│   │   ├── parser.h / parser.c
│   │   └── executor.h / executor.c
│   │
│   ├── tx/                    # Transazioni + WAL
│   │   ├── transaction.h / transaction.c
│   │   └── wal.h / wal.c
│   │
│   ├── bridge/                # IL PONTE TRA GUI E DB
│   │   └── db_api.h / db_api.c
│   │
│   └── main.c                 # Entry point
│
├── include/
│   └── common.h               # Tipi globali, macro
│
└── Makefile.
```

### Costanti globali (`common.h`)

| Costante | Valore | Significato |
|---|---|---|
| `SV_PAGE_SIZE` | `4096` | Dimensione pagina su disco (4 KB) |
| `SV_BUFFER_POOL_CAP` | `1024` | Max pagine in cache RAM |
| `SV_MAX_DATABASES` | `64` | Max database aperti |
| `SV_MAX_TABLES` | `256` | Max tabelle per DB |
| `SV_MAX_COLUMNS` | `64` | Max colonne per tabella |
| `SV_MAX_INDEXES` | `16` | Max indici per tabella |
| `SV_MAX_SQL_LEN` | `65536` | Max lunghezza query SQL |

---

## Documentation & References

### 1 · GUI — Win32 (Windows)

Official docs: [learn.microsoft.com/en-us/windows/win32/](https://learn.microsoft.com/en-us/windows/win32/)

| Topic | Link |
|---|---|
| Creating a window | [Window Management](https://learn.microsoft.com/en-us/windows/win32/winmsg/windows) |
| Event handling (WndProc) | [Window Procedures](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-procedures) |
| Drawing with GDI | [Windows GDI](https://learn.microsoft.com/en-us/windows/win32/gdi/windows-gdi) |
| Text & fonts | [Fonts and Text](https://learn.microsoft.com/en-us/windows/win32/gdi/fonts-and-text) |
| Keyboard input | [Keyboard Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/keyboard-input) |
| Mouse input | [Mouse Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/mouse-input) |
| Dialog boxes | [Dialog Boxes](https://learn.microsoft.com/en-us/windows/win32/dlgbox/dialog-boxes) |

---

### 2 · GUI — Xlib/X11 (Linux)

Official docs: [tronche.com/gui/x/xlib/](https://tronche.com/gui/x/xlib/)

| Topic | Link |
|---|---|
| Open X server connection | [Opening a Display](https://tronche.com/gui/x/xlib/display/opening.html) |
| Create a window | [XCreateWindow](https://tronche.com/gui/x/xlib/window/XCreateWindow.html) |
| Select events | [XSelectInput](https://tronche.com/gui/x/xlib/event-handling/XSelectInput.html) |
| Read events | [Event Queue](https://tronche.com/gui/x/xlib/event-handling/manipulating-event-queue/) |
| Draw rectangles | [XFillRectangle](https://tronche.com/gui/x/xlib/graphics/drawing/XFillRectangle.html) |
| Draw text | [XDrawString](https://tronche.com/gui/x/xlib/graphics/drawing/XDrawString.html) |
| Graphic Context (GC) | [GC Reference](https://tronche.com/gui/x/xlib/GC/) |
| Pixmap (backbuffer) | [Pixmap](https://tronche.com/gui/x/xlib/pixmap-and-cursor/pixmap.html) |
| Keyboard / KeySym | [Keyboard Utilities](https://tronche.com/gui/x/xlib/utilities/keyboard/) |
| Window close (WM_DELETE) | [ICCCM §4](https://tronche.com/gui/x/icccm/sec-4.html) |

---

### 3 · C Standard Library

Reference: [en.cppreference.com/w/c](https://en.cppreference.com/w/c)

| Header | Purpose | Link |
|---|---|---|
| `<stdint.h>` | `uint32_t`, `uint64_t`, … | [Integer types](https://en.cppreference.com/w/c/types/integer) |
| `<stdbool.h>` | `bool`, `true`, `false` | [Boolean type](https://en.cppreference.com/w/c/types/boolean) |
| `<stdlib.h>` | `malloc`, `free`, `calloc` | [Memory](https://en.cppreference.com/w/c/memory) |
| `<string.h>` | `memcpy`, `memset`, `strlen` | [Byte strings](https://en.cppreference.com/w/c/string/byte) |
| `<stdio.h>` | `printf`, `fopen`, `fread` | [I/O](https://en.cppreference.com/w/c/io) |
| `<unistd.h>` | `pread`, `pwrite`, `open` | [POSIX](https://pubs.opengroup.org/onlinepubs/9699919799/) |
| `<fcntl.h>` | `O_RDWR`, `O_CREAT` | [fcntl.h](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/fcntl.h.html) |

---

### 4 · File I/O — POSIX (Storage Engine)

POSIX reference: [pubs.opengroup.org/onlinepubs/9699919799/](https://pubs.opengroup.org/onlinepubs/9699919799/)

> `pread` e `pwrite` leggono/scrivono a un offset preciso **senza spostare il cursore del file** — ideale per le pagine da 4 KB del database.

| Function | Purpose | Link |
|---|---|---|
| `open()` | Open/create the DB file | [open()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html) |
| `pread()` | Read a page at offset | [pread()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pread.html) |
| `pwrite()` | Write a page at offset | [pwrite()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pwrite.html) |
| `fsync()` | Flush to disk (durability) | [fsync()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fsync.html) |
| `lseek()` | Seek within file | [lseek()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/lseek.html) |
| `close()` | Close the file descriptor | [close()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/close.html) |

---

### 5 · SQL Parser — Hand-written Recursive Descent

SegVault implements its own SQL parser from scratch — no Flex or Bison needed.

| Resource | Link |
|---|---|
| Hand-written lexer guide | [craftinginterpreters.com — Scanning](https://craftinginterpreters.com/ch04-scanning.html) |
| Recursive descent parsing | [craftinginterpreters.com — Parsing](https://craftinginterpreters.com/ch06-parsing.html) |
| SQL-92 grammar reference | [SQL 1992 spec (CMU)](http://www.contrib.andrew.cmu.edu/~shadow/sql/sql1992.txt) |
| SQLite SQL dialect (practical) | [sqlite.org/lang.html](https://www.sqlite.org/lang.html) |

---

### 6 · Data Structures in C

All core engine structures are implemented from scratch.

| Structure | Reference |
|---|---|
| **B+Tree** (`src/index/btree.c`, chiavi intere su pagine) | [Wikipedia — B+Tree](https://en.wikipedia.org/wiki/B%2B_tree) · [USC Indexing paper (PDF)](https://infolab.usc.edu/csci585/Spring2010/den_ar/indexing.pdf) |
| **Buffer Pool / CLOCK** | [Cache Replacement Policies — CLOCK](https://en.wikipedia.org/wiki/Page_replacement_algorithm#Clock) |
| **Hash Map** (indici, hash join, `GROUP BY`, `DISTINCT`) | [Hash Table](https://en.wikipedia.org/wiki/Hash_table) |
| **Hash join** | [Hash join](https://en.wikipedia.org/wiki/Hash_join) |
| **Selettività di System R** | [Selinger et al., Access Path Selection (1979)](https://courses.cs.duke.edu/compsci516/cps216/spring03/papers/selinger-etal-1979.pdf) |
| **Linked List** | [Linked List](https://en.wikipedia.org/wiki/Linked_list) |

---

### 7 · Write-Ahead Log (WAL)

| Resource | Link |
|---|---|
| ARIES original paper (1992) | [cs.stanford.edu — ARIES PDF](https://cs.stanford.edu/people/chrismre/cs345/rl/aries.pdf) |
| SQLite WAL explained | [sqlite.org/wal.html](https://www.sqlite.org/wal.html) |
| PostgreSQL WAL internals | [postgresql.org/docs/current/wal-internals.html](https://www.postgresql.org/docs/current/wal-internals.html) |

---

### 8 · Reference Source Code

Reading real implementations is the fastest way to understand how everything fits together.

| Project | Why it's useful | Link |
|---|---|---|
| **SQLite amalgamation** | Entire DB in one commented `.c` file | [sqlite.org/amalgamation.html](https://www.sqlite.org/amalgamation.html) |
| **LittleD** | Minimal relational DB in pure C | [github.com/graemedouglas/LittleD](https://github.com/graemedouglas/LittleD) |
| **MiniDB** | Small didactic DB | [github.com/willcrichton/minidb](https://github.com/willcrichton/minidb) |
| **CMU Tiny DB** | Guided university project | [15445.courses.cs.cmu.edu](https://15445.courses.cs.cmu.edu/fall2023/project0/) |

---

### 9 · Cross-compilation Linux → Windows

Build `.exe` files from Arch Linux without touching Windows.

| Tool | Documentation |
|---|---|
| **MinGW-w64** | [mingw-w64.org](https://www.mingw-w64.org/) |
| **CMake cross-compilation** | [cmake.org — Toolchains](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html) |

---

## Building

```bash
# Full build (real DB engine) — produce ./segvault
make

# GUI-only build (uses db_api_test.c stub, no engine) — same binary name
make gui

# Test: motore (128), SQL end-to-end (469, con crash e recovery), script di examples/ (159 istruzioni)
make test

# Gli stessi test con AddressSanitizer + UndefinedBehaviorSanitizer
make test-asan

# Benchmark su 100.000 righe
make bench
```

**Dipendenze su Linux:** `libX11`, `libXft`, `libXrender`, `fontconfig` (presenti su qualsiasi desktop X11; su Debian/Ubuntu: `sudo apt install libx11-dev libxft-dev`, su Arch: `sudo pacman -S libx11 libxft`). Per il miglior risultato installare **JetBrains Mono** e **Inter** o **Fira Sans**: se mancano, fontconfig sceglie automaticamente il miglior font monospace / sans-serif disponibile.

Il Makefile rileva automaticamente la piattaforma tramite `$(OS)` e seleziona il sorgente corretto (`src/platform/xlib.c` o `src/platform/win32.c`).
`make` (default) produce il binario completo con motore DB reale; `make gui` compila solo la GUI con uno stub di test (`db_api_test.c`).

### Compilazione per Windows (cross-compilation da Linux)

```bash
# Installare MinGW-w64
sudo apt install mingw-w64

# Compilare con x86_64-w64-mingw32-gcc
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -Iinclude -Isrc -Isrc/platform -Isrc/widgets -Isrc/catalog \
    -Isrc/storage -Isrc/table -Isrc/index -Isrc/query -Isrc/tx -Isrc/bridge \
    src/main.c src/platform/win32.c $(WIDGET_SRCS) $(ENGINE_SRCS) \
    -lgdi32 -luser32 -lkernel32 -o segvault.exe
```

**⚠ Nota:** Il layer storage (`page.c`) usa `pread()`/`pwrite()` (POSIX). Su Windows con MinGW-w64 queste funzioni sono disponibili tramite `_pread()`/`_pwrite()` o definendo `_WIN32_WINNT=0x0600`. Se la compilazione fallisce con `undefined reference to 'pread'`, editare `page.c` per usare `#ifdef _WIN32` con `SetFilePointer`/`ReadFile`/`WriteFile`.

---

## License

This project is for educational purposes.  
Feel free to study, fork, and extend it.

---

<p align="center">
  Built with no dependencies, no shortcuts.
</p>
