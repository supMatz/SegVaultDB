# 🗄️ SegVault — DBMS relazionale in C puro

> Un DBMS relazionale scritto a mano in C, senza dipendenze: parser SQL, executor con ottimizzatore,
> storage a pagine, Write-Ahead Log e recovery, con un'app desktop nativa per **Windows** (Win32) e
> **Linux** (X11) e un **server HTTP/JSON** per siti web e app in qualsiasi linguaggio.

---

## 📋 Indice

- [Panoramica](#panoramica)
- [Avvio rapido](#avvio-rapido)
- [Per i corsi di Basi di Dati 1 e 2](#per-i-corsi-di-basi-di-dati-1-e-2)
- [L'app desktop](#lapp-desktop)
  - [Scelta iniziale: editor o server](#scelta-iniziale-editor-o-server)
  - [Editor SQL](#editor-sql)
  - [Explorer, risultati, import ed export](#explorer-risultati-import-ed-export)
  - [Menu e impostazioni](#menu-e-impostazioni)
  - [Pannello del server API](#pannello-del-server-api)
  - [Temi](#temi)
- [SQL supportato](#sql-supportato)
- [API: usare SegVault da siti web e app desktop](#api-usare-segvault-da-siti-web-e-app-desktop)
- [Architettura](#architettura)
- [Affidabilità e test](#affidabilità-e-test)
- [Prestazioni](#prestazioni)
- [Struttura dei sorgenti](#struttura-dei-sorgenti)
- [Compilazione](#compilazione)
- [Documentazione e riferimenti](#documentazione-e-riferimenti)
- [Roadmap](#roadmap)
- [Licenza](#licenza)

---

## Panoramica

**SegVault** è un DBMS relazionale didattico ma completo, scritto interamente in C: nessuna libreria
esterna, nessun framework. È pensato per i corsi di **Basi di Dati 1 e 2** di ingegneria informatica
(lo stesso programma fa da ambiente per gli esercizi di SQL e da laboratorio sugli interni di un DBMS:
piani di esecuzione, indici, buffer pool, log, transazioni, trigger) e, con il server API, come
database per piccoli siti web e app desktop.

| Caratteristica | Dettagli |
|---|---|
| **Linguaggio** | C11, zero dipendenze per il motore e per il server |
| **Versione** | 1.2.0 |
| **App desktop** | Win32 / GDI (Windows) · Xlib/X11 + Xft (Linux); all'avvio si sceglie **editor SQL** o **server API** |
| **Server API** | `segvault-server` (o il pannello dell'app): HTTP/JSON, parametri `?` contro la SQL injection, sessioni con transazioni su più richieste, utenti e permessi (`GRANT`), token, CORS, limite di tempo, registro e monitor delle richieste |
| **Client** | TypeScript/JavaScript e Python pronti in [`clients/`](clients/); esempi per C# e Java; `libsegvault.a` per incorporare il motore in C/C++ |
| **SQL** | Lexer e parser a discesa ricorsiva scritti a mano; subquery anche correlate, `UNION` / `INTERSECT` / `EXCEPT`, `WITH [RECURSIVE]`, funzioni finestra con frame, `FULL` / `NATURAL` / `USING` join, `CASE`, viste, vincoli, trigger, `ALTER TABLE ... COLUMN` |
| **Vincoli** | `PRIMARY KEY` (anche composta), `UNIQUE`, `NOT NULL`, `CHECK`, `FOREIGN KEY` con `ON DELETE` / `ON UPDATE` `CASCADE`, `SET NULL`, `SET DEFAULT`, `RESTRICT` |
| **Storage** | Heap file a pagine da 4 KB con slot directory e free-space map; indici in memoria (anche su più colonne): tabella hash per le uguaglianze, chiavi ordinate per gli intervalli |
| **Buffer pool** | 1024 frame, tabella hash per la ricerca, sostituzione **CLOCK**; statistiche con `SHOW STATUS` |
| **Ottimizzatore** | Pushdown delle selezioni, hash join (anche per `FROM a, b WHERE a.x = b.y`), accesso con indice per uguaglianza o per intervallo (scelto solo se conviene), subquery decorrelate per chiave; `EXPLAIN` con stime alla System R e `EXPLAIN ANALYZE` con le righe reali |
| **Transazioni** | Write-Ahead Log con undo/redo, `BEGIN` / `COMMIT` / `ROLLBACK`, `SAVEPOINT`, recovery dopo un crash, `CHECKPOINT` (anche automatico) |
| **Basi di dati attive** | Trigger `BEFORE` / `AFTER`, `FOR EACH ROW` / `FOR EACH STATEMENT`, `NEW` / `OLD`, `WHEN`, `SET NEW.col`, `SIGNAL`, `IF` |
| **Testi** | Nessun limite fisso sulla lunghezza dei testi; una riga salvata deve stare in una pagina (al massimo 4080 byte) |
| **Strumenti visuali** | Designer dello schema ER (notazione di Chen) / UML a trascinamento, export PNG; modulo della struttura con SQL live; modifica dei dati nella griglia; ricerca in tutto il database; panoramica; utenti e permessi; monitor del server |
| **Piattaforme** | Linux nativo; Windows con `make windows` da Linux (MinGW-w64): `segvault.exe` e `segvault-server.exe` statici |

---

## Avvio rapido

```bash
make                        # ./segvault (app desktop) e ./segvault-server
./segvault                  # chiede: editor SQL o server API (dati in ./data)
./segvault --editor dati/   # direttamente l'editor, con i dati nella cartella dati/
./segvault --server         # direttamente il server API (http://127.0.0.1:8484/api/v1)
./segvault-server           # il server senza finestra (servizi, container)
```

Su Windows: `segvault.exe` e `segvault-server.exe`, da tenere accanto alla cartella `assets/`
(i font). La cartella dati viene creata se manca e contiene:

| File | Contenuto |
|---|---|
| `segvault.db` | Pagine delle tabelle |
| `segvault.cat` | Catalogo: database, tabelle, colonne, vincoli, indici, viste, trigger |
| `segvault.wal` | Write-Ahead Log (svuotato a ogni checkpoint) |
| `settings.ini` | Impostazioni dell'app |
| `session.sql`, `session.ini` | Script aperto nell'editor, cursore, file, zoom, tema |
| `history.sql` | Cronologia degli statement eseguiti |
| `server.token` | Token del server API (creato quando serve) |
| `server.ini` | Cartella del sito servito dal pannello del server |

Una cartella dati è usata da un solo processo alla volta: una seconda istanza sulla stessa cartella
viene rifiutata, perché la corromperebbe.

---

## Per i corsi di Basi di Dati 1 e 2

La cartella [`examples/`](examples/) contiene script pronti da aprire nell'editor (`Ctrl+O`): ogni istruzione è commentata con la domanda a cui risponde. `make test` li esegue tutti, quindi funzionano sempre con la versione corrente.

| Script | Argomenti |
|---|---|
| [`01_universita.sql`](examples/01_universita.sql) | Schema relazionale con chiavi primarie (anche composte), chiavi esterne con azioni referenziali, `UNIQUE`, `CHECK`, `DEFAULT` — il database usato dagli esercizi |
| [`02_query_bd1.sql`](examples/02_query_bd1.sql) | 36 esercizi: selezione e proiezione, join impliciti ed espliciti, outer join (anche `FULL`), self join, `USING`, aggregati con `GROUP BY` / `HAVING`, `IN` / `EXISTS` / `ANY` / `ALL`, subquery correlate, **divisione relazionale** (doppio `NOT EXISTS` e conteggio), `UNION` / `INTERSECT` / `EXCEPT`, viste, `WITH`, tabelle derivate, `CASE`, **chiusura transitiva** con `WITH RECURSIVE` (propedeuticità), classifiche, somme cumulative e medie mobili con le **funzioni finestra** |
| [`03_vincoli.sql`](examples/03_vincoli.sql) | Ogni vincolo violato apposta (le istruzioni marcate *errore atteso*), `CASCADE`, `SET NULL`, `ON UPDATE CASCADE`, `ALTER TABLE ADD/DROP CONSTRAINT`, `ADD / RENAME / DROP COLUMN`, `SHOW CREATE TABLE` |
| [`04_bd2_ottimizzazione.sql`](examples/04_bd2_ottimizzazione.sql) | Generazione di 10.000 righe con un prodotto cartesiano, scansione sequenziale contro accesso con indice (per chiave e per intervallo), `CREATE INDEX`, selettività e stime, pushdown e hash join, subquery correlate, `ORDER BY` + `LIMIT`, buffer pool e checkpoint |
| [`05_bd2_transazioni_trigger.sql`](examples/05_bd2_transazioni_trigger.sql) | Atomicità con `COMMIT` / `ROLLBACK`, `SAVEPOINT`, log WAL; trigger evento-condizione-azione, trigger a cascata, rifiuto con `SIGNAL`, trigger per riga e per istruzione |
| [`web/index.html`](examples/web/index.html) | Una web app (registro spese) che salva e legge i dati tramite il server API: `./segvault-server --static examples/web` |

**Basi di Dati 1** — modello relazionale e SQL: tutto il linguaggio di interrogazione dei corsi (compresi gli operatori insiemistici e le interrogazioni annidate con la logica a tre valori di SQL: `NOT IN` con un `NULL` non è mai vero, `ALL` su un insieme vuoto è vero), viste, vincoli di integrità di dominio, di tupla, di chiave e referenziali, con controllo prima di ogni scrittura: uno statement che viola un vincolo non modifica nulla.

**Basi di Dati 2** — come funziona un DBMS:
- `EXPLAIN` mostra il piano: scansione sequenziale o accesso con indice, filtri anticipati, hash join o nested loop, aggregazione, ordinamento, con le **stime delle righe secondo le selettività di default di System R** (1/10 per `=`, 1/3 per un intervallo). `EXPLAIN ANALYZE` esegue la query e affianca le righe reali di ogni passo, il numero di esecuzioni delle subquery correlate e i tempi.
- `SHOW STATUS` riporta richieste al buffer pool, hit, miss, hit ratio, sostituzioni (CLOCK), pagine scritte, dimensione del log e checkpoint; `SHOW INDEXES FROM t` gli indici e se sono già stati costruiti.
- Transazioni con WAL: ogni modifica viene scritta nel log prima delle pagine; `ROLLBACK` usa le immagini precedenti (undo), dopo un crash le transazioni completate vengono rifatte (redo) e quelle aperte annullate. `CHECKPOINT` scrive le pagine e svuota il log.
- Trigger con la semantica dei DBMS reali: i `BEFORE` vedono e possono cambiare la riga prima dei controlli dei vincoli, gli `AFTER` possono attivare altri trigger; se un trigger fallisce lo statement viene annullato per intero.

---

## L'app desktop

### Scelta iniziale: editor o server

All'avvio SegVault chiede come usarlo:

```
┌───────────────────────────────┐  ┌───────────────────────────────┐
│ ▤  SQL editor             [1] │  │ ▥  API server             [2] │
│ Write and run queries,        │  │ Let websites and desktop apps │
│ explore the schema, ...       │  │ read and write this database  │
└───────────────────────────────┘  └───────────────────────────────┘
 [ ] Remember my choice (change it in Settings › Server)
```

`1` / `2` (o frecce e `Enter`) scelgono; *Remember my choice* salva la scelta in Settings › Server ›
*At startup open*. Da riga di comando `--editor` e `--server` saltano la domanda. Editor e server non
sono attivi insieme, perché usano lo stesso motore con un solo stato di sessione: *File › Start API
server…* passa al server (non con una query in corso o una transazione aperta), *Open SQL editor*
lo ferma e torna all'editor.

### Editor SQL

```
┌──────────────────────────────────────────────────────────────────────┐
│ File  Edit  View  Query  Import  Export  Settings  Help   untitled.sql│
├──────────────────────────────────────────────────────────────────────┤
│ ◆ SegVault v1.2.0 [▶ Run F5] [Run statement] │ [Commit] [Rollback] │ [Format]   ● Sky  ▤ db │
├────────────┬─────────────────────────────────────────────────────────┤
│ EXPLORER ⟳ │  editor SQL (numeri di riga, highlighting, completamento)│
│  ▸ uni     │                                                         │
│    ▸ esame ├────────────────────────── ••• ─────────────────────────┤
│            │  RESULTS ● Query OK            filtro    12 rows  History│
│            │  griglia dei risultati (schede se più SELECT)           │
├────────────┴─────────────────────────────────────────────────────────┤
│ ● SegVault engine 1.2.0  ▤ uni  script.sql         Ln 3, Col 7  F1 Shortcuts│
└──────────────────────────────────────────────────────────────────────┘
```

- **Run** (`F5`) esegue l'intero script, oppure solo il testo selezionato.
- **Run statement** (`Ctrl+Enter`) esegue solo lo statement sotto il cursore (evidenziato da una barra colorata nel margine).
- Le query girano in un thread separato: l'interfaccia resta reattiva e `Esc` (o *Stop*) le interrompe, anche quelle con subquery correlate.

Gli statement separati da `;` vengono eseguiti in sequenza (i `;` dentro stringhe e commenti sono ignorati). In caso di errore l'esecuzione si ferma (o prosegue, in Settings › Execution), la riga dello statement fallito viene evidenziata in rosso e il messaggio compare nel pannello dei risultati. Un'esecuzione con più `SELECT` mostra un risultato per scheda (`Ctrl+Tab`).

#### Scorciatoie

| Scorciatoia | Azione |
|---|---|
| `F5` / `Ctrl+Enter` | Esegui script o selezione / statement sotto il cursore |
| `F2`, `Canc`, `Ctrl+Enter` (sulla griglia) | Modifica la cella / elimina la riga / salva la riga nuova |
| `Esc` | Interrompi la query in esecuzione |
| `Ctrl+Shift+E` | `EXPLAIN` dello statement sotto il cursore |
| `Ctrl+Shift+F` | Formatta l'SQL (selezione o intero script) |
| `Ctrl+Shift+H` | Cronologia delle query |
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
| `Ctrl+F` / `Ctrl+H` / `Ctrl+G` | Trova / trova e sostituisci / vai alla riga |
| `F3` / `Shift+F3` | Risultato successivo / precedente |
| `Alt+C` / `Alt+W` | Distingui maiuscole / solo parole intere |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | Nuovo / apri / salva / salva con nome |
| `Ctrl+,` | Impostazioni |
| `Ctrl+B` | Mostra / nascondi l'explorer |
| `Ctrl+Shift+T` | Tema successivo |
| `Alt` + lettera | Apre un menu (File = `F`, Export = `X`, ...) |
| `F1` | Pannello con tutte le scorciatoie |

L'editor chiude automaticamente parentesi e apici, mantiene l'indentazione andando a capo, evidenzia la parentesi corrispondente e le altre occorrenze dell'identificatore sotto il cursore, e colora keyword, tipi, funzioni, stringhe, numeri e commenti.

**Autocompletamento:** propone colonne, tabelle, viste e database del database corrente insieme a keyword, tipi e funzioni SQL. Dopo `FROM` / `JOIN` / `INTO` / `UPDATE` le tabelle vengono prima; scrivendo `tabella.` compaiono solo le colonne di quella tabella. `↑`/`↓` per scegliere, `Enter`/`Tab` o click per inserire, `Esc` per chiudere. Le keyword rispettano il maiuscolo/minuscolo che stai usando e le funzioni vengono inserite con le parentesi.

**Formattazione (`Ctrl+Shift+F` o pulsante *Format*):** keyword in maiuscolo (o minuscolo, da Settings), una clausola per riga, `AND`/`OR` indentati, colonne di `CREATE TABLE` una per riga, spaziatura normalizzata; stringhe e commenti non vengono toccati. Si annulla con `Ctrl+Z`.

**Trova e sostituisci:** `Ctrl+F` si apre precompilato con la selezione o la parola sotto il cursore; in `Ctrl+H`, `Enter` nel campo di sostituzione sostituisce e `Ctrl+Enter` sostituisce tutto; `Esc` chiude la barra.

**File, sessione e cronologia:** i percorsi si scrivono nella barra in alto dell'editor (`~` viene espanso); aprire un file o creare uno script nuovo resta annullabile con `Ctrl+Z`, e il footer mostra il nome del file con un pallino se ci sono modifiche non salvate. Alla chiusura (anche con `Ctrl+C` / `SIGTERM` dal terminale) script, cursore, file aperto, zoom e tema vengono salvati e ripristinati all'avvio successivo. Ogni statement eseguito finisce nella cronologia (data, esito, righe, durata): si apre nella griglia dei risultati, e doppio click o `Enter` aggiungono lo statement in fondo allo script.

### Explorer, risultati, import ed export

**Modificare i dati nella griglia** (come in phpMyAdmin): quando un risultato viene da una tabella
con la chiave primaria tra le colonne (l'anteprima dell'explorer, `SELECT * FROM t ...`, anche con
`WHERE`, `ORDER BY` o un join) l'intestazione mostra *Editable*.

| Azione | Come |
|---|---|
| Modificare una cella | doppio click, `F2` o si comincia a scrivere; `Invio` salva, `Tab` / `Shift+Tab` passa alla colonna accanto, `Esc` annulla |
| Impostare `NULL` | tasto destro › *Set to NULL* |
| Aggiungere una riga | *+ Row* (o tasto destro › *Add row*): si compilano le celle (le altre prendono il `DEFAULT`), poi *Save row*, `Ctrl+Invio` o `Invio` sull'ultima colonna; *Discard* / `Esc` la scarta |
| Eliminare una riga | `Canc` due volte (la seconda conferma) o tasto destro › *Delete row* |

Ogni modifica diventa un `UPDATE` / `INSERT` / `DELETE` che identifica la riga con la chiave
primaria, con i valori scritti come letterali sicuri; dopo la scrittura la riga viene riletta dal
database (valori di default, `AUTO_INCREMENT`, trigger). Se il database rifiuta il valore (tipo,
vincolo, trigger) il messaggio compare nella barra di stato e la cella resta in modifica. Lo
statement eseguito è nella barra dei risultati e nella cronologia; con una transazione aperta le
modifiche ne fanno parte (`COMMIT` / `ROLLBACK`). Restano in sola lettura (con il motivo nella barra
di stato) le colonne calcolate e i risultati di `GROUP BY`, `DISTINCT`, `UNION`, viste, e le tabelle
senza chiave primaria.

**Utenti e permessi** (*Settings › Users & permissions*): gli account del server API con una
matrice database × permesso (`SELECT` … `ALTER`, *All*, riga *All databases*), amministratore,
cambio password, permessi sulle singole tabelle; ogni clic è un `GRANT` / `REVOKE` mostrato sotto.
Gli stessi comandi si scrivono in SQL: `CREATE USER`, `GRANT`, `REVOKE`, `SHOW GRANTS`
([dettagli](docs/API.md#utenti-e-permessi)).

**Strumenti sul database** (tasto destro su un database nell'explorer, o menu *Query*):
- *Search in database…* (`Ctrl+Shift+G`): cerca un testo in tutte le colonne di tutte le tabelle, senza
  distinguere maiuscole e minuscole (anche numeri e date; `%` e `_` sono cercati come caratteri); il risultato ha una riga per valore trovato
  con tabella, colonna, valore e chiave della riga (`id=3`), fino a 1000 valori;
- *Database overview*: una riga per tabella con righe, colonne, chiave primaria, indici, chiavi esterne,
  tabelle che la riferiscono e spazio su disco, più i totali (oltre ~1,5 s di conteggi le righe
  delle tabelle restanti vengono dal catalogo, segnate con `~`);
- *Drop database…*; su una tabella *Empty table…* (`TRUNCATE`) e *Drop table…*, con una conferma che
  dice quante righe si perdono (le chiavi esterne che puntano alla tabella vengono tolte prima).

**Explorer:** click per espandere database e tabelle; doppio click su un database = `USE`, su una tabella = anteprima dei dati (`SELECT * … LIMIT 200`), su una colonna = inserisce il nome nell'editor.

**Tasto destro sull'explorer:** su un database *Use this database* e *New table…*; su una tabella
*Browse data*, *Structure of …*, *Insert SELECT into editor*, *Insert CREATE TABLE into editor* e
*Copy name*. Le stesse voci per tabelle sono nel menu *Query* (sulla tabella selezionata
nell'explorer o su quella del risultato modificabile).

**Pagina Designer** (selettore *SQL editor | Designer* nella toolbar, `Ctrl+Shift+D`, o *View ›
Schema designer*): il diagramma dello schema del database scelto in alto, per progettare un database
senza scrivere SQL.

| Azione | Come |
|---|---|
| Notazione | *ER* (entità, attributi a lecca-lecca — pieno = identificatore —, relazioni a rombo con le cardinalità `(1,1)` / `(0,N)`; una tabella di collegamento, la cui chiave è fatta dalle due chiavi esterne, è disegnata come relazione molti-a-molti con i suoi attributi, come `esame(voto, data)` tra `studente` e `corso`) o *UML* (classi con attributi e tipi, molteplicità `1` / `0..1` / `*`, freccia verso la tabella riferita). Notazione, zoom e posizione della vista sono ricordati per ogni database |
| Spostare una tabella | trascinarla (posizioni salvate per database in `diagram_<db>.txt` nella cartella dei dati); frecce = sposta la selezionata |
| Navigare | trascinare lo sfondo o rotella (`Shift` = orizzontale), `Ctrl` + rotella o *− / +* = zoom, *Fit* adatta, *Arrange* ridispone (genitori a sinistra, senza linee sopra le tabelle) |
| Nuovo database | *+ Database* (o il menu del database) |
| Nuova tabella | *+ Table*, doppio click su un punto vuoto o tasto destro › *New table here*: si apre il modulo della struttura e la tabella va in quel punto |
| Modificare una tabella | doppio click, `Invio` o tasto destro › *Edit structure* |
| Relazione (chiave esterna) | trascinare dal **+** sul bordo di una tabella alla tabella a cui si riferisce (crea la colonna `<tabella>_id` del tipo giusto, o una colonna per ogni parte se la chiave è composta); in UML anche dal pallino di una colonna: diventa lei la chiave esterna. Oppure tasto destro › *Add relationship…* e click sulla destinazione |
| Relazione molti-a-molti | come sopra tenendo premuto `Shift` al rilascio, o tasto destro › *Add many-to-many relationship…*: crea la tabella di collegamento `a_b` con le due chiavi (chiave primaria composta) e la mette tra le due tabelle |
| Relazione obbligatoria / facoltativa | tasto destro sulla linea (`SET` / `DROP NOT NULL` sulle colonne della chiave) |
| Eliminare | selezione + `Canc`, o tasto destro: tabella (con conferma; le relazioni verso di lei vengono tolte) o relazione (con la casella *Also drop the column* anche le sue colonne, se non servono ad altro) |
| Dati | tasto destro › *Browse data* apre l'anteprima nella pagina SQL |
| Immagine | *Export PNG*: tutto il diagramma (non solo la parte visibile) in un file `.png`, a risoluzione normale o doppia, con lo sfondo dell'app o chiaro per documenti e stampa |

Dall'explorer, tasto destro su un database o una tabella › *Show in the designer*.

**Designer di tabelle** (come la scheda *Struttura* di phpMyAdmin): un modulo con una riga per
colonna — nome, tipo (menu), lunghezza per `VARCHAR` / `CHAR`, `DEFAULT`, *Primary*, *Not null*,
*Unique*, *Index* (`CREATE INDEX` / `DROP INDEX`), *Auto inc* — e l'aggiunta o l'eliminazione di colonne. Sotto, l'SQL che verrà eseguito si
aggiorna mentre si scrive (`CREATE TABLE` per una tabella nuova, gli `ALTER TABLE` per le modifiche:
rinomina di tabella e colonne, tipo, default, `NOT NULL`, `UNIQUE`, colonne aggiunte ed eliminate),
con l'errore se la struttura non è valida. *Create table* / *Apply changes* (`Ctrl+Invio`) lo esegue
e aggiorna l'explorer; *Open in editor* lo aggiunge allo script invece di eseguirlo. Le modifiche che
dipendono dai dati (tipo, `NOT NULL`, `UNIQUE`) vengono eseguite per prime, così se i dati non le
rispettano la tabella resta com'era. La chiave primaria di una tabella esistente si cambia con
`ALTER TABLE` nell'editor.

**Risultati:**
- click su una cella e `Ctrl+C` per copiarla, frecce per navigare, doppio click o `Enter` per vederne il valore intero;
- trascina il bordo dell'intestazione per ridimensionare una colonna (doppio click = adatta al contenuto), `Shift` + rotella per lo scroll orizzontale;
- click sull'intestazione = ordina (crescente → decrescente → ordine originale);
- `Ctrl+F` sulla griglia, o il campo nell'intestazione, filtra le righe;
- tasto destro: copia di cella, riga, colonna o valore come letterale SQL, filtri, ordinamento, export.

**Export** (menu *Export*): CSV, TSV, JSON, Markdown, HTML o `INSERT` SQL, su file o negli appunti (`Ctrl+Shift+C` copia tutto come TSV, da incollare in un foglio di calcolo; `Ctrl+E` esporta in CSV). *Database dump* scrive in un file `.sql` le istruzioni che ricreano il database corrente (tabelle, dati, chiavi esterne).
**Import** (menu *Import*): un file CSV diventa una tabella (tipi delle colonne riconosciuti dai dati); *Run SQL file* esegue uno script senza aprirlo.

### Menu e impostazioni

| Menu | Contenuto |
|---|---|
| **File** | Nuovo, apri, salva, salva con nome, esegui un file SQL, *Start API server…*, esci |
| **Edit** | Annulla / ripeti, taglia / copia / incolla, trova e sostituisci, vai alla riga, duplica / elimina / commenta righe, maiuscolo / minuscolo, formatta |
| **View** | Explorer, barra di stato, numeri di riga e di colonna, zoom, tema, cronologia, aggiorna l'explorer |
| **Query** | Esegui, esegui statement, `EXPLAIN`, stop, `BEGIN` / `COMMIT` / `ROLLBACK`, modelli di `SELECT`, `JOIN`, `GROUP BY`, `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE` |
| **Import** / **Export** | Vedi sopra |
| **Settings** | Impostazioni (`Ctrl+,`), tema e interruttori rapidi, ripristino dei valori predefiniti |
| **Help** | Scorciatoie (`F1`), SQL supportato, **Online documentation** (apre questo README nel browser), informazioni |

Le **impostazioni** (`Ctrl+,`) sono salvate in `<data>/settings.ini`:

| Sezione | Opzioni |
|---|---|
| **Appearance** | Tema, colore d'accento, dimensione del testo dell'interfaccia, pulsanti della toolbar (testo o icone), explorer, barra di stato |
| **Editor** | Dimensione del font, indentazione, numeri di riga, riga corrente evidenziata, chiusura automatica di parentesi e apici, autocompletamento, maiuscole delle keyword, cursore lampeggiante, ripristino della sessione |
| **Results** | Altezza delle righe, come mostrare `NULL`, numeri di riga, righe alterne |
| **Execution** | Fermarsi o proseguire dopo un errore, *safe updates* (blocca `UPDATE` / `DELETE` senza `WHERE`), protezione da `DROP` / `TRUNCATE`, dimensione della cronologia |
| **Export** | Separatore e intestazione dei CSV, `NULL` nei CSV, JSON leggibile o compatto |
| **Server** | All'avvio aprire (chiedi / editor / server), porta, raggiungibile dalla rete locale, token, CORS, limite di tempo delle richieste |

### Pannello del server API

Ha la stessa struttura dell'editor: toolbar, una sidebar *SERVER* al posto dell'explorer, il registro
delle richieste al posto dei risultati.

```
┌──────────────────────────────────────────────────────────────────────┐
│ ◆ SegVault v1.2.0 [■ Stop server F5] [Copy address]  ● Running · port 8484 [Open SQL editor]│
├────────────────┬─────────────────────────────────────────────────────┤
│ SERVER         │ REQUESTS ● 4 requests · 1 error · 3 rows sent · up 1m │
│ ADDRESS        │ time      client     method path          status ms   sql │
│ http://127.0…  │ 21:51:51  127.0.0.1  POST   /api/v1/query  200   0.2  SELECT * FROM p │
│ PORT [−][8484][+]│ 21:51:51  127.0.0.1  POST   /api/v1/query  400   0.1  Table … not found│
│ REACHABLE FROM │                                                     │
│ TOKEN          │                                                     │
│ WEB APPS (CORS)│                                                     │
│ TIME LIMIT     │                                                     │
│ WEB FOLDER     │                                                     │
│ TRY IT  curl … │                                                     │
├────────────────┴─────────────────────────────────────────────────────┤
│ ● SegVault engine 1.2.0  API server running  this computer only    F5 Start / Stop│
└──────────────────────────────────────────────────────────────────────┘
```

- **Indirizzi** da copiare: locale e, con *Local network*, quello raggiungibile dagli altri computer.
- **Opzioni:** porta, *This computer* / *Local network*, *User login* (utente e password per ogni richiesta, con i permessi definiti in *Settings › Users & permissions*), token (obbligatorio in rete o con CORS; *Copy* e *New*, salvato in `<data>/server.token`), *Same origin* / *Any origin* per le web app, limite di tempo. Una modifica riavvia il server.
- **Registro** in tempo reale delle ultime 200 richieste, con l'SQL così come è stato inviato (con i `?`: i valori dei parametri, come password o dati personali, non compaiono a schermo).
- **Web folder:** una cartella con il sito (come la `htdocs` di XAMPP) viene servita su `/` insieme all'API su `/api/v1`, sulla stessa porta e quindi senza CORS; *Open site* lo apre nel browser. Il percorso è salvato in `<data>/server.ini`.
- **Monitor** (scheda accanto a *Requests*): richieste al secondo, tempo medio e p95, percentuale di
  errori, righe inviate, connessioni aperte e percentuale di pagine trovate in cache; due grafici degli
  ultimi 2 minuti (richieste al secondo con gli errori in rosso, tempo di risposta medio e massimo, con
  i valori al passaggio del mouse), le richieste recenti più lente con l'utente che le ha fatte e lo
  stato del motore (file dei dati, cache delle pagine, letture dal disco, WAL, checkpoint). Il registro
  delle richieste mostra `utente@indirizzo` quando c'è il login.
- **Try it:** il comando `curl` pronto da copiare (a schermo il token è mostrato solo in parte).

### Temi

5 temi, dalla pillola nella toolbar (anche con `↑`/`↓` e `Enter`), da `Ctrl+Shift+T` o da Settings. Il colore d'accento si può cambiare per tutti i temi.

| Tema | Tipo | Accento |
|---|---|---|
| **Sky** *(predefinito)* | scuro | azzurro `#38BDF8` |
| **Violet** | scuro | viola `#7C6AFF` |
| **Graphite** | scuro, grigio neutro | ambra `#F5A623` |
| **Forest** | scuro, verde | smeraldo `#34D399` |
| **Daylight** | chiaro | blu `#0284C7` |

Tutti i colori vivono in una struct `Theme` ([`src/widgets/theme.c`](src/widgets/theme.c)): le macro `TH_*` leggono dal tema attivo, quindi il codice di disegno non cambia; per aggiungere un tema basta una voce nell'array `THEMES[]`.

| Costante | Uso |
|---|---|
| `TH_BG`, `TH_SURFACE`, `TH_SURFACE_2`, `TH_ELEVATED*` | Sfondi: app, sidebar/pannelli, toolbar/intestazioni, pulsanti |
| `TH_ACCENT*`, `TH_ON_ACCENT` | Colore principale (azione primaria, selezione, focus) e testo sopra l'accento |
| `TH_TEXT*`, `TH_CELL_*` | Testo dell'interfaccia e delle celle |
| `TH_SUCCESS` / `TH_ERROR` / `TH_WARNING` / `TH_INFO` | Stati (OK, errore, transazione aperta, info) |
| `TH_SYN_*`, `TH_EDITOR_*`, `TH_CARET`, `TH_GUTTER_*` | Editor SQL e syntax highlighting |

---

## SQL supportato

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
ALTER TABLE studente ALTER [COLUMN] citta TYPE VARCHAR(40);              -- ogni valore viene convertito e controllato
ALTER TABLE studente MODIFY citta VARCHAR(40);                           -- la stessa cosa, sintassi MySQL
ALTER TABLE studente ALTER COLUMN citta SET DEFAULT 'Milano';            -- DROP DEFAULT
ALTER TABLE studente ALTER COLUMN cognome SET NOT NULL;                  -- rifiutato se ci sono già dei NULL; DROP NOT NULL
ALTER TABLE studente ADD CONSTRAINT nome_unico UNIQUE (nome, cognome);   -- i dati esistenti vengono controllati
ALTER TABLE studente DROP CONSTRAINT nome_unico;
SHOW CONSTRAINTS FROM esame;

CREATE INDEX idx_esame_corso ON esame (corso);   DROP INDEX idx_esame_corso ON esame;
SHOW INDEXES FROM esame;

CREATE VIEW carriera AS SELECT studente, AVG(voto) AS media FROM esame GROUP BY studente;
DROP VIEW [IF EXISTS] carriera;
```

**Vincoli:** `NOT NULL`, `DEFAULT`, `AUTO_INCREMENT`, `PRIMARY KEY` (di colonna o `PRIMARY KEY (a, b)`), `UNIQUE` (più `NULL` ammessi, come in SQL), `CHECK (condizione)` di colonna o di tabella (soddisfatto se vero o sconosciuto), `FOREIGN KEY (a, b) REFERENCES t [(x, y)]` o `REFERENCES t` sulla colonna (senza colonne: la chiave primaria di `t`), con `ON DELETE` / `ON UPDATE` `NO ACTION` (predefinito), `RESTRICT`, `CASCADE`, `SET NULL`, `SET DEFAULT`; anche autoreferenziali. Le colonne riferite devono essere la chiave primaria o una chiave `UNIQUE`. Ogni statement calcola prima tutte le modifiche, comprese quelle a cascata, controlla tutti i vincoli (e la dimensione delle righe) e solo dopo scrive.

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
| Operatori | `+ - * / %`, `= <> != < > <= >=`, `AND OR NOT` (logica a tre valori), `IS [NOT] NULL`, `[NOT] LIKE 'a%_'` (senza distinzione maiuscole/minuscole; `ESCAPE '!'` rende letterali `%` e `_`: `LIKE '100!%' ESCAPE '!'`), `[NOT] BETWEEN`, `[NOT] IN (lista)`; `/` fra interi dà un decimale (`7 / 2` = 3.5, come MySQL), la divisione per zero dà `NULL` |
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
Con `SELECT DISTINCT`, `ORDER BY` può usare solo ciò che la query restituisce (una colonna, un alias, una posizione o la stessa espressione), come in MySQL e PostgreSQL: altrimenti l'ordine non sarebbe definito e la query viene rifiutata.

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

Un `SAVEPOINT` con un nome già usato sostituisce il precedente (come in MySQL); in una transazione ce ne possono essere fino a 64.

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

### Tipi di dato

| Tipo | Descrizione |
|---|---|
| `INT` / `INTEGER` | 4 byte, con segno |
| `BIGINT` | 8 byte, con segno |
| `FLOAT` / `DOUBLE` | 8 byte, virgola mobile |
| `VARCHAR(n)` | Testo fino a `n` caratteri |
| `CHAR(n)` | Testo di `n` caratteri |
| `TEXT` | Testo lungo (fino a 65535 byte, nei limiti della riga) |
| `BOOL` / `BOOLEAN` | `TRUE` / `FALSE` (0 / 1) |
| `DATE` | `AAAA-MM-GG`, controllata |
| `DATETIME` | `AAAA-MM-GG HH:MM:SS`, controllata |
| `BLOB` | Dati binari |
| `DECIMAL(p, s)` | Numero decimale (conservato come testo, confrontato come numero) |

### Limiti

| Cosa | Limite |
|---|---|
| Database / tabelle per database | 64 / 256 |
| Colonne / indici / vincoli per tabella | 64 / 16 / 16 |
| Viste / trigger per database | 64 / 64 |
| Dimensione di una riga salvata | 4080 byte (una pagina da 4 KB: non ci sono pagine di overflow); oltre, l'`INSERT` / `UPDATE` è rifiutato senza scrivere nulla |
| Un testo | Nessun limite fisso (letterali fino a 1 MB), ma deve stare nella riga |
| Savepoint per transazione / livelli di trigger | 64 / 16 |
| Passi di `WITH RECURSIVE` | 1000 |
| Testo nell'editor | 64 KB per script (i file più grandi si eseguono con *Run SQL file*) |

### Non supportato

Frame `RANGE` con un intervallo di valori (`RANGE 5 PRECEDING`), stored procedure, nomi qualificati con il database (`shop.prodotto`: si usa `USE shop`), scritture in parallelo: le richieste di sola lettura girano insieme, quelle che scrivono una alla volta, e una sola sessione per volta può avere una transazione aperta (vedi [Roadmap](#roadmap)).

---

## API: usare SegVault da siti web e app desktop

SegVault può fare da database per siti web e app desktop in qualsiasi linguaggio con un client HTTP
(browser, Node.js, Python, C#, Java, Go, ...): il **server API** espone il motore come servizio
**HTTP/JSON**. Come il resto del progetto è scritto in C senza dipendenze, per Linux e Windows.
Riferimento completo del protocollo: [`docs/API.md`](docs/API.md). Client pronti: [`clients/`](clients/).

Il server si avvia in due modi:

- **dall'app desktop**, scegliendo *API server* all'avvio (o `segvault --server`): vedi [Pannello del server API](#pannello-del-server-api);
- **senza finestra**, per un server, un container o un servizio di sistema:

```bash
make server                                   # ./segvault-server (make windows: anche segvault-server.exe)
./segvault-server                             # http://127.0.0.1:8484/api/v1, dati in ./data
./segvault-server -p 9000 -d shop dati/       # porta, database predefinito, cartella dati
./segvault-server --static examples/web       # serve anche una web app: http://127.0.0.1:8484/
./segvault-server --cors http://localhost:5173 # una web app su un'altra origine (es. Vite in sviluppo)
./segvault-server -b 0.0.0.0 --token auto     # in rete locale, con un token casuale (stampato all'avvio)
```

| Opzione | Significato |
|---|---|
| `-p, --port N` | porta TCP (default 8484) |
| `-b, --bind ADDR` | indirizzo (default `127.0.0.1`: solo questa macchina); in rete serve un token |
| `-t, --token T` | richiede `Authorization: Bearer T` (anche `SEGVAULT_TOKEN`; `auto` ne genera uno) |
| `--users` | richiede utente e password (HTTP Basic); ogni query ha i permessi dell'utente (`CREATE USER`, `GRANT`: vedi [docs/API.md](docs/API.md#utenti-e-permessi)) |
| `-d, --database DB` | database usato se la richiesta non ne indica uno |
| `--cors ORIGIN` | permette le chiamate dal browser da `ORIGIN` (`*` = tutte) |
| `--static DIR` | serve i file di `DIR` fuori da `/api` |
| `--timeout S` | ferma una richiesta che dura più di `S` secondi (default 30) |
| `--max-body MB` | corpo massimo di una richiesta (default 16) |

Endpoint: `GET /api/v1/health`, `GET /api/v1/status`, `POST /api/v1/query`,
`GET /api/v1/databases`, `GET /api/v1/databases/{db}/tables`, `GET /api/v1/databases/{db}/tables/{t}`,
`GET|POST /api/v1/endpoints/{nome}` e `GET /api/v1/endpoints` (endpoint salvati, sotto).
Tutto il lavoro passa da `POST /api/v1/query`:

```bash
curl -s http://127.0.0.1:8484/api/v1/query -H 'Content-Type: application/json' -d '{
  "database": "shop",
  "sql": "SELECT nome, prezzo FROM prodotto WHERE prezzo < ?",
  "params": [10],
  "rows": "objects"
}'
# {"ok":true,"results":[{"columns":[...],"rows":[{"nome":"pane","prezzo":1.5}],"row_count":1,"time_ms":0.02}],"time_ms":0.05}
```

**Endpoint salvati — un'API senza scrivere backend.** Una query con un nome, salvata nella tabella
`api_endpoints` del database, diventa un URL (come in PocketBase o Supabase):

```sql
CREATE TABLE api_endpoints (name VARCHAR(60) PRIMARY KEY, sql TEXT NOT NULL,
                            access VARCHAR(10) NOT NULL DEFAULT 'token', description VARCHAR(200));
INSERT INTO api_endpoints VALUES
  ('prodotti_economici', 'SELECT id, nome, prezzo FROM prodotto WHERE prezzo < :max ORDER BY prezzo', 'public', NULL);
```

```js
// da una pagina pubblica, senza token e senza backend:
const r = await fetch("http://127.0.0.1:8484/api/v1/endpoints/prodotti_economici?database=shop&max=10");
const { results } = await r.json();          // results[0].rows = [{ id, nome, prezzo }, ...]
```

I parametri `:nome` vengono dalla query string o da un JSON nel corpo (`POST`) e sono legati in modo
sicuro; lo script è atomico; `public` = chiamabile senza token da qualsiasi sito, `token` = come il
resto dell'API. Dettagli in [`docs/API.md`](docs/API.md#endpoint-salvati-unapi-senza-scrivere-backend).
Gli endpoint si creano e modificano con l'SQL o direttamente nella griglia dell'app.

Regole da ricordare, valide per tutti i linguaggi:

- **I dati dell'utente vanno sempre in `params`**, con un `?` nell'SQL: il server li trasforma in
  letterali sicuri (niente SQL injection). Mai concatenare stringhe nell'SQL.
- Più statement separati da `;` in una richiesta danno un risultato ciascuno; con
  `"transaction": true` vengono applicati **tutti o nessuno**.
- Senza sessione ogni richiesta è un'unità chiusa: una transazione lasciata aperta viene annullata a
  fine richiesta, e il database corrente torna quello di prima.
- **Sessioni** (`POST /api/v1/sessions`, poi l'header `X-SegVault-Session`): database, utente e
  transazione restano tra le richieste (`BEGIN` in una, `COMMIT` in un'altra). Mentre una sessione ha
  una transazione aperta le altre richieste aspettano (fino a 10 s, poi HTTP 409); una transazione
  ferma da 30 s viene annullata. Dettagli in [docs/API.md](docs/API.md#sessioni).
- Le richieste di sola lettura (`SELECT`) girano in parallelo tra loro; quelle che scrivono usano il
  motore una alla volta: è pensato per app, siti piccoli e medi, laboratori, non per migliaia di
  scritture concorrenti al secondo.
  Una richiesta che supera il limite di tempo (default 30 s) viene fermata.
- Una pagina web di un'altra origine può chiamare il server solo se è ammessa (`--cors`, o
  *Web apps: Any origin* nel pannello); le altre ricevono 403 senza che nulla venga eseguito
  (protezione dai siti malevoli aperti nel browser). Curl, Python, C#, Java e Node non ne sono toccati.
- Tipi in JSON: `INT`/`FLOAT` → numero, `BOOL` → booleano, `NULL` → `null`, il resto (anche
  `DATE`, `AAAA-MM-GG`) → stringa.
- Con colonne qualificate (`p.nome`) la chiave JSON è `"p.nome"`: usa un alias (`p.nome AS nome`).
- I testi (letterali, parametri, risultati di `CONCAT` / `REPLACE` / ...) possono essere lunghi
  quanto serve; una riga salvata deve però stare in 4080 byte: oltre, l'`INSERT` / `UPDATE` viene
  rifiutato con un messaggio chiaro e non scrive nulla.

Gli esempi usano lo stesso schema:

```sql
CREATE DATABASE shop;
USE shop;
CREATE TABLE prodotto (id INT PRIMARY KEY AUTO_INCREMENT, nome VARCHAR(60) NOT NULL UNIQUE,
                       prezzo FLOAT NOT NULL CHECK (prezzo >= 0), categoria VARCHAR(20));
CREATE TABLE ordine (id INT PRIMARY KEY AUTO_INCREMENT, prodotto INT NOT NULL REFERENCES prodotto(id),
                     quantita INT NOT NULL CHECK (quantita > 0), giorno DATE NOT NULL);
```

### TypeScript / JavaScript (browser, Node.js 18+, Deno, Bun)

Client: [`clients/segvault.ts`](clients/segvault.ts) (TypeScript) o [`clients/segvault.js`](clients/segvault.js)
(JavaScript): usano solo `fetch`, nessun pacchetto da installare.

```ts
import { SegVault, SegVaultError } from "./segvault";

interface Prodotto { id: number; nome: string; prezzo: number; categoria: string | null }

const db = new SegVault("http://127.0.0.1:8484", { database: "shop" });

// salvare: un INSERT con parametri
await db.run("INSERT INTO prodotto (nome, prezzo, categoria) VALUES (?, ?, ?)", ["pane", 1.5, "cibo"]);

// salvare molti record arrivati dall'app (form, file, API esterna) in un colpo solo, atomico
const nuovi = [{ nome: "latte", prezzo: 1.2, categoria: "cibo" }, { nome: "vino", prezzo: 9.9, categoria: "bevande" }];
await db.transaction(
  "INSERT INTO prodotto (nome, prezzo, categoria) VALUES " + nuovi.map(() => "(?, ?, ?)").join(", "),
  nuovi.flatMap((p) => [p.nome, p.prezzo, p.categoria]),
);

// caricare: righe come oggetti tipizzati
const economici = await db.all<Prodotto>("SELECT * FROM prodotto WHERE prezzo < ? ORDER BY prezzo", [5]);
const uno = await db.get<Prodotto>("SELECT * FROM prodotto WHERE nome = ?", ["pane"]);

// elaborare: le aggregazioni conviene farle fare al database...
const perCategoria = await db.all<{ categoria: string; n: number; medio: number }>(
  "SELECT categoria, COUNT(*) AS n, AVG(prezzo) AS medio FROM prodotto GROUP BY categoria");
// ...e il resto in JavaScript sui dati caricati
const listino = economici.map((p) => `${p.nome}: ${p.prezzo.toFixed(2)} €`);

// aggiornare / cancellare
const cambiati = await db.run("UPDATE prodotto SET prezzo = prezzo * 1.1 WHERE categoria = ?", ["bevande"]);

// errori: vincoli violati, SQL errato, ...
try {
  await db.run("INSERT INTO prodotto (nome, prezzo) VALUES (?, ?)", ["pane", 2]);   // UNIQUE
} catch (e) {
  if (e instanceof SegVaultError) console.error(e.message, "statement", e.statement);
}
```

**Da una pagina web** ci sono due strade:
1. la pagina è servita dal server stesso (`--static cartella`) e chiama `/api/v1/...` sulla stessa
   origine: è il caso di [`examples/web/index.html`](examples/web/index.html), un registro spese completo;
2. la web app gira altrove (Vite, React, ...): avvia il server con `--cors http://localhost:5173`.

Per un **sito pubblico** il browser non deve parlare direttamente con il database (chiunque
vedrebbe e userebbe il token): il browser chiama il tuo backend (Node, Python, C#, Java, ...),
che usa SegVault con il client del suo linguaggio. Lo stesso vale per un'app **Electron/Tauri**:
le chiamate partono dal processo principale.

### Python

Client: [`clients/segvault.py`](clients/segvault.py), solo libreria standard.

```python
from segvault import SegVault, SegVaultError

db = SegVault("http://127.0.0.1:8484", database="shop")

# salvare
db.run("INSERT INTO prodotto (nome, prezzo, categoria) VALUES (?, ?, ?)", ["pane", 1.5, "cibo"])

# importare record (es. da un CSV) in un'unica transazione
import csv
with open("prodotti.csv", newline="", encoding="utf-8") as f:
    righe = [(r["nome"], float(r["prezzo"]), r["categoria"]) for r in csv.DictReader(f)]
for i in range(0, len(righe), 500):                     # blocchi da 500 righe
    blocco = righe[i:i + 500]
    db.transaction("INSERT INTO prodotto (nome, prezzo, categoria) VALUES "
                   + ", ".join(["(?, ?, ?)"] * len(blocco)),
                   [v for riga in blocco for v in riga])

# caricare: lista di dict
for p in db.all("SELECT nome, prezzo FROM prodotto WHERE prezzo < ? ORDER BY prezzo", [5]):
    print(p["nome"], p["prezzo"])

# elaborare in SQL (join + aggregazione)...
fatturato = db.all("""SELECT p.categoria AS categoria, SUM(p.prezzo * o.quantita) AS totale
                      FROM ordine o JOIN prodotto p ON p.id = o.prodotto
                      WHERE o.giorno >= ? GROUP BY p.categoria ORDER BY totale DESC""", ["2026-01-01"])

# ...o in Python, ad esempio con pandas
import pandas as pd
df = pd.DataFrame(db.all("SELECT * FROM prodotto"))
print(df.groupby("categoria")["prezzo"].describe())

try:
    db.run("INSERT INTO ordine (prodotto, quantita, giorno) VALUES (?, ?, ?)", [999, 1, "2026-11-10"])
except SegVaultError as e:                              # chiave esterna violata
    print("rifiutato:", e)
```

### C# (.NET 6+)

Nessun pacchetto: `HttpClient` e `System.Text.Json` sono nella libreria standard.

```csharp
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;

public record Prodotto(int Id, string Nome, double Prezzo, string? Categoria);

public class SegVaultException(string message, int? statement) : Exception(message)
{
    public int? Statement { get; } = statement;
}

public class SegVault(string url, string database, string? token = null)
{
    static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };
    readonly HttpClient http = new() { BaseAddress = new Uri(url) };

    record Request(string Sql, object?[] Params, string Database, bool Transaction, string Rows);
    record Error(string Message, int? Statement);
    record Result(JsonElement[]? Rows, int? RowsAffected);
    record Response(bool Ok, Result[] Results, Error? Error);

    async Task<Result[]> Send(string sql, object?[] args, bool transaction)
    {
        using var req = new HttpRequestMessage(HttpMethod.Post, "/api/v1/query")
        {
            Content = JsonContent.Create(new Request(sql, args, database, transaction, "objects"),
                options: new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase })
        };
        if (token != null) req.Headers.Authorization = new("Bearer", token);
        using var res = await http.SendAsync(req);
        var body = await res.Content.ReadFromJsonAsync<Response>(
            new JsonSerializerOptions(Json) { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower });
        if (body is null || !body.Ok)
            throw new SegVaultException(body?.Error?.Message ?? $"HTTP {(int)res.StatusCode}", body?.Error?.Statement);
        return body.Results;
    }

    // righe dell'ultimo statement convertite nel tipo T (le colonne diventano proprietà)
    public async Task<List<T>> All<T>(string sql, params object?[] args)
    {
        var results = await Send(sql, args, false);
        return results.Length == 0 || results[^1].Rows is null ? new()
             : results[^1].Rows!.Select(r => r.Deserialize<T>(Json)!).ToList();
    }

    public async Task<int> Run(string sql, params object?[] args) =>
        (await Send(sql, args, false)) is { Length: > 0 } r ? r[^1].RowsAffected ?? 0 : 0;

    public Task<Result[]> Transaction(string sql, params object?[] args) => Send(sql, args, true);
}
```

```csharp
var db = new SegVault("http://127.0.0.1:8484", "shop");

// salvare
await db.Run("INSERT INTO prodotto (nome, prezzo, categoria) VALUES (?, ?, ?)", "pane", 1.5, "cibo");

// salvare una lista di oggetti dell'app, atomicamente
var nuovi = new[] { new Prodotto(0, "latte", 1.2, "cibo"), new Prodotto(0, "vino", 9.9, "bevande") };
await db.Transaction(
    "INSERT INTO prodotto (nome, prezzo, categoria) VALUES " + string.Join(", ", nuovi.Select(_ => "(?, ?, ?)")),
    nuovi.SelectMany(p => new object?[] { p.Nome, p.Prezzo, p.Categoria }).ToArray());

// caricare in oggetti tipizzati
List<Prodotto> economici = await db.All<Prodotto>("SELECT * FROM prodotto WHERE prezzo < ?", 5);

// elaborare: in SQL o con LINQ
var medie = await db.All<JsonElement>("SELECT categoria, AVG(prezzo) AS medio FROM prodotto GROUP BY categoria");
var piuCaro = economici.MaxBy(p => p.Prezzo);

try { await db.Run("DELETE FROM prodotto WHERE id = ?", 1); }
catch (SegVaultException e) { Console.WriteLine($"errore: {e.Message}"); }   // es. RESTRICT su una FK
```

In un'app desktop (WPF, WinForms, Avalonia, MAUI) si può anche avviare il server insieme all'app con
`Process.Start("segvault-server.exe", "-p 8484 dati")` e chiuderlo all'uscita.

### Java (11+)

`java.net.http.HttpClient` è nella libreria standard; per il JSON serve una libreria, qui
**Jackson** (`com.fasterxml.jackson.core:jackson-databind`).

```java
import com.fasterxml.jackson.databind.*;
import java.net.URI;
import java.net.http.*;
import java.util.*;

public class SegVault {
    private static final ObjectMapper JSON = new ObjectMapper()
            .configure(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES, false);
    private final HttpClient http = HttpClient.newHttpClient();
    private final String url, database, token;

    public SegVault(String url, String database, String token) {
        this.url = url; this.database = database; this.token = token;
    }

    public static class SegVaultException extends RuntimeException {
        public final Integer statement;
        SegVaultException(String msg, Integer statement) { super(msg); this.statement = statement; }
    }

    private JsonNode send(String sql, List<?> params, boolean transaction) throws Exception {
        Map<String, Object> body = new HashMap<>();
        body.put("sql", sql);
        body.put("params", params);
        body.put("database", database);
        body.put("transaction", transaction);
        body.put("rows", "objects");
        HttpRequest.Builder rb = HttpRequest.newBuilder(URI.create(url + "/api/v1/query"))
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(JSON.writeValueAsString(body)));
        if (token != null) rb.header("Authorization", "Bearer " + token);
        HttpResponse<String> res = http.send(rb.build(), HttpResponse.BodyHandlers.ofString());
        JsonNode root = JSON.readTree(res.body());
        if (!root.path("ok").asBoolean()) {
            JsonNode e = root.path("error");
            throw new SegVaultException(e.path("message").asText("HTTP " + res.statusCode()),
                                        e.has("statement") ? e.get("statement").asInt() : null);
        }
        JsonNode results = root.get("results");
        return results.size() > 0 ? results.get(results.size() - 1) : JSON.createObjectNode();
    }

    /** righe dell'ultimo statement come oggetti della classe indicata */
    public <T> List<T> all(Class<T> type, String sql, Object... params) throws Exception {
        List<T> out = new ArrayList<>();
        for (JsonNode row : send(sql, Arrays.asList(params), false).path("rows")) out.add(JSON.treeToValue(row, type));
        return out;
    }

    public int run(String sql, Object... params) throws Exception {
        return send(sql, Arrays.asList(params), false).path("rows_affected").asInt();
    }

    public void transaction(String sql, List<?> params) throws Exception { send(sql, params, true); }
}
```

```java
public record Prodotto(int id, String nome, double prezzo, String categoria) {}

SegVault db = new SegVault("http://127.0.0.1:8484", "shop", null);

// salvare
db.run("INSERT INTO prodotto (nome, prezzo, categoria) VALUES (?, ?, ?)", "pane", 1.5, "cibo");

// salvare molti record in una transazione
List<Prodotto> nuovi = List.of(new Prodotto(0, "latte", 1.2, "cibo"), new Prodotto(0, "vino", 9.9, "bevande"));
String values = String.join(", ", Collections.nCopies(nuovi.size(), "(?, ?, ?)"));
List<Object> args = new ArrayList<>();
for (Prodotto p : nuovi) { args.add(p.nome()); args.add(p.prezzo()); args.add(p.categoria()); }
db.transaction("INSERT INTO prodotto (nome, prezzo, categoria) VALUES " + values, args);

// caricare in record Java
List<Prodotto> economici = db.all(Prodotto.class, "SELECT * FROM prodotto WHERE prezzo < ?", 5);

// elaborare: SQL per le aggregazioni, stream per il resto
double totale = economici.stream().mapToDouble(Prodotto::prezzo).sum();
```

### C / C++ (motore incorporato, senza server)

Un'app desktop in C o C++ può linkare direttamente il motore, come si fa con SQLite:
`make lib` produce `libsegvault.a`, l'header è [`src/bridge/db_api.h`](src/bridge/db_api.h).
Esempio in [`docs/API.md`](docs/API.md#incorporare-il-motore-in-unapp-cc-senza-server).

### Quale modalità scegliere

| Scenario | Come |
|

### Quale modalità scegliere

| Scenario | Come |
|---|---|
| Provare o sviluppare sul proprio computer | `segvault` → *API server* (pannello con il registro delle richieste) |
| Sito pubblico che legge dati, senza backend | endpoint salvati `public` |
| Sito statico + database, come con XAMPP | `segvault` → *API server* → *Web folder* (o `segvault-server --static cartella/`) |
| Sito / web app che gira sulla stessa macchina | `segvault-server --static app/` (stessa origine) |
| Frontend su un'altra origine (sviluppo) | `segvault-server --cors http://localhost:5173` |
| Sito pubblico | browser → tuo backend → `segvault-server` su `127.0.0.1` |
| App desktop in C#, Java, Python, Electron | il server avviato insieme all'app (porta locale) |
| App desktop in C/C++ | `libsegvault.a`, nessun processo in più |
| Server in rete locale | `segvault-server -b 0.0.0.0 --token ...`, meglio dietro un proxy HTTPS |

---

## Architettura

```
┌──────────────────────────────────┐   ┌──────────────────────────────────────┐
│ App desktop (src/widgets)        │   │ Siti web e app (TS/JS, Python, C#,   │
│ editor SQL · explorer · risultati│   │ Java, ...) — clients/                │
│ scelta iniziale · pannello server│   └──────────────────┬───────────────────┘
│ platform.h: win32.c │ xlib.c     │                      │ HTTP/JSON
└───────────────┬──────────────────┘   ┌──────────────────▼───────────────────┐
                │                      │ Server API (src/server)              │
                │                      │ HTTP/1.1, thread per connessione,    │
                │                      │ parametri ?, token, CORS, timeout    │
                │                      └──────────────────┬───────────────────┘
                │  db_api.h — unico punto di contatto con il motore (anche per libsegvault.a)
┌───────────────▼─────────────────────────────────────────▼───────────────────┐
│ SQL: lexer e parser a discesa ricorsiva scritti a mano (src/query)          │
├──────────────────────────────────────────────────────────────────────────────┤
│ Executor: sorgenti → filtri anticipati → hash join → WHERE → GROUP BY/HAVING │
│ → finestre → DISTINCT → ORDER BY/LIMIT → colonne; vincoli e trigger:         │
│ modifiche calcolate, controllate, poi scritte; indici in memoria             │
├──────────────────────────────────────────────────────────────────────────────┤
│ Storage: heap file a pagine da 4 KB + free-space map, buffer pool CLOCK      │
├──────────────────────────────────────────────────────────────────────────────┤
│ Write-Ahead Log: BEGIN · COMMIT · ROLLBACK · SAVEPOINT · CHECKPOINT · redo/undo│
├──────────────────────────────────────────────────────────────────────────────┤
│ File I/O (pread / pwrite, fsync; equivalenti Win32 in platform_compat.h)     │
└──────────────────────────────────────────────────────────────────────────────┘
```

Il sistema grafico è astratto da [`platform.h`](src/platform/platform.h): i widget usano solo quell'interfaccia e non toccano mai `<windows.h>` o `<X11/Xlib.h>`. App desktop, server e libreria parlano con il motore solo tramite [`db_api.h`](src/bridge/db_api.h), che serializza le chiamate con un mutex.

### Widget

Ogni widget estende `Widget` mettendolo come **primo campo** della propria struct — il modo C di emulare l'ereditarietà:

| Widget | File | Descrizione |
|---|---|---|
| `Widget` | `widget.h` / `widget.c` | Struct base con vtable (`draw`, `handle_event`, `destroy`) |
| `Button` | `button.h` / `button.c` | Pulsante con icona, testo e scorciatoia; primario, secondario, ghost |
| `Label` | `label.h` / `label.c` | Testo non interattivo, allineamento L/C/R |
| `TextBox` | `textbox.h` / `textbox.c` | Editor SQL multilinea: highlighting, ricerca/sostituzione, autocompletamento, annulla/ripeti |
| `LineEdit` | `line_edit.h` / `line_edit.c` | Campo a riga singola (trova, vai alla riga, percorsi, porta del server) |
| `TableView` | `table_view.h` / `table_view.c` | Griglia dei risultati: ordinamento, filtro, copia, export |
| `TreeView` | `tree_view.h` / `tree_view.c` | Explorer degli oggetti del database |
| `Scrollbar` | `scrollbar.h` / `scrollbar.c` | Scrollbar verticale/orizzontale |
| `Panel` / `Splitter` | `panel.*`, `splitter.*` | Contenitore; divisore ridimensionabile |
| *(finestra)* | `window.h` / `window.c` | Layout dell'editor, menu, impostazioni, esecuzione in background |
| *(schermate)* | `server_view.h` / `server_view.c` | Scelta iniziale e pannello del server |
| *(utenti)* | `users_view.*`; `src/bridge/auth.*` | Pannello Users & permissions; utenti, password, `GRANT` e controllo dei permessi nel motore |
| *(immagini)* | `png_write.h` / `png_write.c` | Encoder PNG senza librerie (deflate con LZ77 e Huffman fisso) |
| *(strumenti)* | `db_tools.h` / `db_tools.c` | Ricerca in tutto il database e panoramica delle tabelle |
| *(designer)* | `table_design.*`, `table_designer.*` | Struttura di una tabella e SQL che la realizza (`CREATE` / `ALTER`); il modulo del designer |
| *(griglia)* | `grid_edit.h` / `grid_edit.c` | SQL per modificare i dati nella griglia dei risultati |
| *(tema)* | `theme.h` / `theme.c` | Temi e palette, testo con ellissi, icone vettoriali antialiasing |
| *(SQL)* | `sql_text.h` / `sql_text.c` | Divisione degli statement, statement sotto il cursore, parole per highlighting e completamento, formattatore |

---

## Affidabilità e test

```bash
make test          # tutti i test
make test-asan     # gli stessi con AddressSanitizer + UndefinedBehaviorSanitizer (anche i memory leak)
```

| Suite | Cosa verifica |
|---|---|
| `test_btree` | B+Tree sotto stress contro un modello di riferimento: inserimenti, aggiornamenti, cancellazioni e scansioni ordinate, con un buffer pool piccolo che costringe a riscrivere e rileggere le pagine |
| `test_engine` (128) | Motore: pagine, heap, catalogo, WAL e recovery, import CSV |
| `test_sql` (994) | SQL dall'inizio alla fine: risultati attesi, errori, vincoli, trigger, transazioni, crash e recovery, testi lunghi, 100 transazioni di fila |
| `test_examples` (166) | Ogni istruzione di `examples/` si comporta come scritto nei commenti (anche gli errori attesi) |
| `test_grid_edit` (22) | Modifica dei dati nella griglia: analisi dei risultati, `UPDATE` / `INSERT` / `DELETE` generati ed eseguiti, chiavi composte, join, apici e backslash, letterali di export e dump |
| `test_server` (78) | Il server su socket reali, endpoint salvati compresi: JSON, parametri, transazioni, token, CORS, origini non ammesse, keep-alive e pipelining, timeout, concorrenza, file statici |

La CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) compila su Ubuntu con `-D_FORTIFY_SOURCE=2` e fallisce a qualunque warning, esegue `make test` e `make test-asan`, e compila per Windows con MinGW-w64. Durante lo sviluppo il motore è stato anche verificato con un fuzzer di SQL mutato e uno di richieste HTTP malformate (sotto ASan), con un confronto dei risultati contro SQLite su centinaia di migliaia di query e modifiche casuali, e con arresti forzati (`kill -9`) durante scritture concorrenti senza perdere nessuna scrittura confermata.

---

## Prestazioni

`make bench` crea uno schema universitario con 10.000 studenti, 50 corsi e 100.000 esami e misura le query tipiche (Linux, un core):

| Operazione | Tempo |
|---|---|
| 110.050 `INSERT` (blocchi da 500 righe) | 0,11 s |
| `COUNT(*)` su 100.000 righe | 11 ms |
| `SELECT *` di 100.000 righe (risultato completo) | 37 ms |
| `SELECT * ... ORDER BY` di 100.000 righe | 50 ms |
| `SELECT ... WHERE voto = 30` su 100.000 righe | 5 ms |
| `GROUP BY` + `AVG` su 100.000 righe | 6 ms |
| `ORDER BY ... LIMIT 10` su 100.000 righe | 6 ms |
| join di 2 tabelle (hash join) + filtro | 5 ms |
| join di 3 tabelle + `GROUP BY` | 16 ms |
| `NOT EXISTS` correlata, 10.000 × 100.000 righe | 9 ms |
| divisione relazionale (doppio `NOT EXISTS`) | 18 ms |
| ricerca per chiave primaria | 0,01 ms |
| intervallo sulla chiave primaria (`BETWEEN`, 500 righe) | 0,4 ms |
| funzione finestra con frame `ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING`, 10.000 righe | 8 ms |
| `UPDATE` / `DELETE` di 2.000 righe con filtro | 10 ms |

Da dove viene la velocità: free-space map dell'heap (un `INSERT` non legge tutta la tabella), buffer pool con tabella hash, log scritto una volta per statement, token e nodi del parser leggeri, scansioni senza allocare le righe scartate dai filtri e righe tenute in un'arena, numeri formattati come testo solo per il risultato, ogni riga del risultato in un solo blocco di memoria della dimensione dei suoi testi, `GROUP BY` e hash join con hash tipizzati, subquery correlate risolte con una ricerca per chiave sulle righe lette una volta sola, `ORDER BY` + `LIMIT` con un heap top-k che calcola le colonne solo delle righe tenute, intervalli trovati con una ricerca binaria sulle chiavi ordinate dell'indice e righe lette in ordine di pagina, frame delle funzioni finestra che scorrono in tempo lineare.

---

## Struttura dei sorgenti

```
SegVaultDB/
├── src/
│   ├── main.c                 # App desktop: avvio, --editor / --server, ciclo degli eventi
│   ├── platform_compat.h      # File I/O, tempo e sleep per Linux e Windows
│   ├── platform/              # Astrazione del sistema grafico
│   │   ├── platform.h         # Interfaccia unica (finestra, eventi, disegno, testo, appunti, thread, browser)
│   │   ├── win32.c            # Windows (Win32 / GDI)
│   │   └── xlib.c             # Linux (Xlib / Xft)
│   ├── widgets/               # Interfaccia (usa solo platform.h e db_api.h)
│   │   ├── window.h / .c      # Editor: layout, menu, impostazioni, esecuzione in background
│   │   ├── server_view.h / .c # Scelta iniziale e pannello del server API
│   │   ├── textbox, line_edit, button, label, table_view, tree_view, scrollbar, panel, splitter, widget
│   │   ├── theme.h / .c       # Temi, palette, icone
│   │   ├── settings.h / .c    # Impostazioni (settings.ini)
│   │   ├── sql_text.h / .c    # Divisione degli statement, highlighting, formattatore
│   │   ├── result_export.*    # Export CSV / TSV / JSON / Markdown / HTML / SQL
│   │   └── csv_import.*       # Import CSV
│   ├── bridge/
│   │   └── db_api.h / .c      # Unico punto di contatto con il motore (app, server, libsegvault.a)
│   ├── server/                # Server HTTP/JSON
│   │   ├── server.h / .c      # Socket, HTTP/1.1, endpoint, parametri ?, sicurezza, registro
│   │   ├── json.h / .c        # Lettura e scrittura JSON
│   │   └── main.c             # segvault-server: opzioni da riga di comando
│   ├── query/                 # SQL: lexer → parser → executor
│   │   ├── lexer.h / .c
│   │   ├── parser.h / .c
│   │   └── executor.h / .c    # Piani, valutazione, indici in memoria, vincoli, trigger
│   ├── catalog/schema.h / .c  # Catalogo: database, tabelle, colonne, vincoli, viste, trigger
│   ├── storage/               # Pagine e buffer pool
│   ├── table/                 # Heap file e tuple
│   ├── index/btree.h / .c     # B+Tree su pagine
│   └── tx/                    # Transazioni e Write-Ahead Log
├── include/common.h           # Versione, costanti, codici di ritorno
├── clients/                   # Client dell'API: segvault.ts, segvault.js, segvault.py
├── docs/API.md                # Riferimento dell'API HTTP/JSON
├── examples/                  # Script per i corsi e web/ (web app d'esempio)
├── tests/                     # test_sql, test_examples, test_server, test_btree
├── test_engine.c              # Test del motore
├── bench/bench.c              # Benchmark
├── assets/fonts/              # JetBrains Mono e Fira Sans (licenza OFL)
├── .github/workflows/ci.yml   # CI
└── Makefile
```

### Costanti globali (`common.h`)

| Costante | Valore | Significato |
|---|---|---|
| `SV_VERSION_STR` | `"1.2.0"` | Versione |
| `SV_PAGE_SIZE` | `4096` | Dimensione di una pagina su disco |
| `SV_BUFFER_POOL_CAP` | `1024` | Pagine nel buffer pool |
| `SV_MAX_DATABASES` | `64` | Database |
| `SV_MAX_TABLES` | `256` | Tabelle per database |
| `SV_MAX_COLUMNS` | `64` | Colonne per tabella |
| `SV_MAX_INDEXES` | `16` | Indici per tabella |
| `SV_MAX_NAME_LEN` | `64` | Lunghezza dei nomi |
| `SV_MAX_SQL_LEN` | `65536` | Testo nell'editor e definizione di una vista |
| `SV_DOCS_URL` | README su GitHub | Aperto da *Help › Online documentation* |

---

## Compilazione

```bash
make               # ./segvault (app desktop) e ./segvault-server
make server        # solo ./segvault-server (senza GUI e senza X11)
make lib           # libsegvault.a: il motore da incorporare in un'app C/C++ (header: src/bridge/db_api.h)
make windows       # segvault.exe e segvault-server.exe (cross-compilazione con MinGW-w64)
make test          # test (vedi Affidabilità e test)
make test-asan     # test con AddressSanitizer + UndefinedBehaviorSanitizer
make bench         # benchmark su 100.000 righe
make clean
```

**Linux:** servono `gcc`, `make`, `pkg-config` e le librerie di sviluppo di X11 e Xft (`sudo apt install build-essential pkg-config libx11-dev libxft-dev libxrender-dev libfontconfig1-dev` su Debian/Ubuntu, `sudo pacman -S base-devel libx11 libxft` su Arch). Il server e la libreria non hanno bisogno di X11. I font JetBrains Mono e Fira Sans sono in `assets/fonts/`; se mancano, fontconfig sceglie il miglior font monospace / sans-serif disponibile.

**Windows (da Linux):** `sudo apt install mingw-w64` (o `pacman -S mingw-w64-gcc`), poi `make windows`. Gli eseguibili sono statici: basta copiarli insieme alla cartella `assets/`. `segvault.exe` è l'app desktop (scelta iniziale, editor e server), `segvault-server.exe` il server da console. La CI pubblica entrambi come artifact a ogni push.

---

## Documentazione e riferimenti

### 1 · GUI — Win32 (Windows)

Documentazione ufficiale: [learn.microsoft.com/en-us/windows/win32/](https://learn.microsoft.com/en-us/windows/win32/)

| Argomento | Link |
|---|---|
| Creare una finestra | [Window Management](https://learn.microsoft.com/en-us/windows/win32/winmsg/windows) |
| Gestione degli eventi (WndProc) | [Window Procedures](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-procedures) |
| Disegno con GDI | [Windows GDI](https://learn.microsoft.com/en-us/windows/win32/gdi/windows-gdi) |
| Testo e font | [Fonts and Text](https://learn.microsoft.com/en-us/windows/win32/gdi/fonts-and-text) |
| Tastiera / mouse | [Keyboard Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/keyboard-input) · [Mouse Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/mouse-input) |
| Socket (server) | [Winsock](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-start-page-2) |
| Aprire un indirizzo nel browser | [ShellExecute](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutea) |

### 2 · GUI — Xlib/X11 (Linux)

Documentazione: [tronche.com/gui/x/xlib/](https://tronche.com/gui/x/xlib/)

| Argomento | Link |
|---|---|
| Connessione al server X | [Opening a Display](https://tronche.com/gui/x/xlib/display/opening.html) |
| Creare una finestra | [XCreateWindow](https://tronche.com/gui/x/xlib/window/XCreateWindow.html) |
| Eventi | [XSelectInput](https://tronche.com/gui/x/xlib/event-handling/XSelectInput.html) · [Event Queue](https://tronche.com/gui/x/xlib/event-handling/manipulating-event-queue/) |
| Disegno | [XFillRectangle](https://tronche.com/gui/x/xlib/graphics/drawing/XFillRectangle.html) · [GC](https://tronche.com/gui/x/xlib/GC/) · [Pixmap](https://tronche.com/gui/x/xlib/pixmap-and-cursor/pixmap.html) |
| Testo antialiasing | [Xft](https://www.x.org/releases/current/doc/man/man3/Xft.3.xhtml) |
| Tastiera / KeySym | [Keyboard Utilities](https://tronche.com/gui/x/xlib/utilities/keyboard/) |
| Chiusura della finestra (WM_DELETE) | [ICCCM §4](https://tronche.com/gui/x/icccm/sec-4.html) |

### 3 · C e POSIX

| Argomento | Link |
|---|---|
| Libreria standard C | [en.cppreference.com/w/c](https://en.cppreference.com/w/c) |
| `pread` / `pwrite` / `fsync` (pagine a un offset preciso, durabilità) | [pread()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pread.html) · [pwrite()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pwrite.html) · [fsync()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fsync.html) |
| Socket e thread (server) | [socket()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/socket.html) · [pthreads](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/pthread.h.html) |

### 4 · SQL, parser e ottimizzazione

SegVault implementa lexer e parser da zero, senza Flex né Bison.

| Risorsa | Link |
|---|---|
| Lexer scritto a mano | [craftinginterpreters.com — Scanning](https://craftinginterpreters.com/ch04-scanning.html) |
| Parser a discesa ricorsiva | [craftinginterpreters.com — Parsing](https://craftinginterpreters.com/ch06-parsing.html) |
| Grammatica SQL-92 | [SQL 1992 spec (CMU)](http://www.contrib.andrew.cmu.edu/~shadow/sql/sql1992.txt) |
| Dialetto SQLite | [sqlite.org/lang.html](https://www.sqlite.org/lang.html) |
| Selettività di System R | [Selinger et al., Access Path Selection (1979)](https://courses.cs.duke.edu/compsci516/cps216/spring03/papers/selinger-etal-1979.pdf) |
| Hash join | [Hash join](https://en.wikipedia.org/wiki/Hash_join) |

### 5 · Strutture dati e storage

| Struttura | Riferimento |
|---|---|
| **B+Tree** (`src/index/btree.c`) | [Wikipedia — B+Tree](https://en.wikipedia.org/wiki/B%2B_tree) |
| **Buffer pool / CLOCK** | [Page replacement — Clock](https://en.wikipedia.org/wiki/Page_replacement_algorithm#Clock) |
| **Tabelle hash** (indici, hash join, `GROUP BY`, `DISTINCT`) | [Hash table](https://en.wikipedia.org/wiki/Hash_table) |
| **Slotted pages** | [CMU 15-445 — Database Storage](https://15445.courses.cs.cmu.edu/fall2023/slides/03-storage1.pdf) |

### 6 · Write-Ahead Log e recovery

| Risorsa | Link |
|---|---|
| ARIES (1992) | [cs.stanford.edu — ARIES PDF](https://cs.stanford.edu/people/chrismre/cs345/rl/aries.pdf) |
| WAL di SQLite | [sqlite.org/wal.html](https://www.sqlite.org/wal.html) |
| WAL di PostgreSQL | [postgresql.org/docs/current/wal-internals.html](https://www.postgresql.org/docs/current/wal-internals.html) |

### 7 · HTTP e sicurezza (server API)

| Risorsa | Link |
|---|---|
| HTTP/1.1 | [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112) |
| JSON | [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259) |
| CORS | [MDN — CORS](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS) |
| SQL injection e query parametriche | [OWASP — Query Parameterization](https://cheatsheetseries.owasp.org/cheatsheets/Query_Parameterization_Cheat_Sheet.html) |
| DNS rebinding | [Wikipedia — DNS rebinding](https://en.wikipedia.org/wiki/DNS_rebinding) |

### 8 · Codice di riferimento

| Progetto | Perché è utile | Link |
|---|---|---|
| **SQLite amalgamation** | Un intero DBMS in un file `.c` commentato | [sqlite.org/amalgamation.html](https://www.sqlite.org/amalgamation.html) |
| **CMU 15-445 (BusTub)** | Progetto universitario guidato su storage, indici, esecuzione | [15445.courses.cs.cmu.edu](https://15445.courses.cs.cmu.edu/) |
| **MinGW-w64** | Compilare per Windows da Linux | [mingw-w64.org](https://www.mingw-w64.org/) |

---

## Roadmap

Cosa manca per superare phpMyAdmin + XAMPP, in ordine di importanza:

1. **Concorrenza** — fatto: sessioni del server con database, utente e transazione propri, anche su
   più richieste, con un solo proprietario delle transazioni; **letture in parallelo** (lock
   lettori/scrittori con precedenza alle scritture, latch del buffer pool, database corrente e
   annullamento per thread; verificato con ThreadSanitizer: `make test-tsan`). Da fare: scritture
   concorrenti (lock sulle righe o MVCC).
2. **Righe grandi** — pagine di overflow per testi e `BLOB` oltre i 4080 byte di una riga.
3. **Compatibilità MySQL** — import dei dump di `mysqldump`, poi il protocollo di rete MySQL (le app
   PHP come WordPress si collegherebbero senza modifiche).
4. **Limiti del motore** — indici composti e `UNIQUE INDEX`, stored procedure, nomi `db.tabella`, più
   tabelle / colonne / indici, ricerca full-text, tipo JSON.
5. **Sicurezza e gestione** — HTTPS integrato, backup pianificati e ripristino dalla GUI, avvio come
   servizio di sistema, registro di audit, `SHOW DATABASES` filtrato per utente.
6. **GUI** — costruttore visuale di query, navigazione delle chiavi esterne dalla griglia, grafici dai
   risultati, export XLSX/PDF, editor di trigger e viste, manutenzione delle tabelle, query salvate.
7. **Portata** — interfaccia web, versione macOS, interfaccia in italiano e altre lingue, accessibilità.

## Licenza

Progetto a scopo didattico: puoi studiarlo, farne un fork ed estenderlo.
I font in `assets/fonts/` sono distribuiti con la licenza SIL Open Font License (file `OFL-*.txt`).

---

<p align="center">
  Senza dipendenze, senza scorciatoie.
</p>
