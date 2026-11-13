# SegVault — API HTTP/JSON

`segvault-server` espone il motore di SegVault via HTTP: siti web, app desktop, script e servizi
in qualsiasi linguaggio leggono e scrivono il database con richieste JSON. È scritto in C come il
resto del progetto, senza dipendenze, per Linux e Windows (`segvault-server.exe`).

Gli esempi per TypeScript/JavaScript, Python, C# e Java sono nel [README](../README.md#api-usare-segvault-da-siti-web-e-app-desktop).
I client pronti sono in [`clients/`](../clients/).

## Avvio

**Dall'app desktop.** All'avvio `segvault` chiede se aprire l'editor SQL o il server API
(Settings › Server › *At startup open* per non chiederlo più; `segvault --server` / `--editor` da
riga di comando). Il pannello del server mostra gli indirizzi da copiare, le statistiche, le
opzioni (porta, rete locale, token, CORS, limite di tempo: una modifica riavvia il server) e il
registro delle richieste in tempo reale. *Open SQL editor* ferma il server e torna all'editor;
*File › Start API server…* fa il contrario (non con una query in corso o una transazione aperta,
perché server ed editor usano lo stesso motore). Il token è salvato in `server.token` nella
cartella dati.

**Senza finestra** (un server, un container, un servizio di sistema):

```bash
make server                          # produce ./segvault-server   (make windows: anche segvault-server.exe)
./segvault-server                    # dati in ./data, http://127.0.0.1:8484
./segvault-server -p 9000 -d shop /srv/segvault-data
./segvault-server --static examples/web          # serve anche una web app: http://127.0.0.1:8484/
./segvault-server --cors http://localhost:5173   # una web app su un'altra origine (es. Vite in sviluppo)
SEGVAULT_TOKEN=segreto ./segvault-server -b 0.0.0.0   # raggiungibile in rete: il token è obbligatorio
./segvault-server -b 0.0.0.0 --token auto              # genera un token casuale e lo stampa
```

| Opzione | Significato |
|---|---|
| `-p, --port N` | porta TCP (default 8484; `0` = una libera) |
| `-b, --bind ADDR` | indirizzo IPv4 (default `127.0.0.1`: solo questa macchina) |
| `-t, --token T` | richiede `Authorization: Bearer T` su `/api` (anche variabile `SEGVAULT_TOKEN`) |
| `-d, --database DB` | database usato se la richiesta non ne indica uno |
| `--cors ORIGIN` | permette le chiamate dal browser da `ORIGIN` (`*` = da tutte) |
| `--static DIR` | serve i file di `DIR` fuori da `/api` (pagina e API sulla stessa origine: niente CORS) |
| `--php PATH\|auto` | esegue i `.php` di `--static DIR` con `php-cgi` (FastCGI; `auto` lo cerca in `php/`, nel `PATH`, ...) |
| `--php-fcgi ADDR` | li esegue con php-fpm già avviato: `host:porta` o `unix:/percorso.sock` |
| `--php-workers N` | processi `php-cgi` (default 4) |
| `--site NOME=DEST` | un sito su `http://NOME.localhost:PORTA` (e `NOME.test`): `DEST` è una cartella oppure un dev server da raggiungere col reverse proxy (`http://127.0.0.1:5173`, anche solo `5173`; WebSocket compresi). Ripetibile |
| `--no-auto-sites` | le sottocartelle di `--static` non diventano siti |
| `--no-dir-listing` | 404 invece dell'elenco dei file per le cartelle senza index |
| `--no-htaccess` | ignora i file `.htaccess` (di default valgono come in Apache, vedi sotto) |
| `--https-port P` | anche HTTPS (es. 8443), con HTTP/2: certificati di una CA locale (`DATA/certs/ca.crt`) fatti per ogni nome di sito |
| `--trust-ca` | rende fidata la CA locale (browser; anche il sistema se si è root) ed esce |
| `--cert-dir DIR` | dove tenere la CA (default `DATA/certs`) |
| `--no-http2` | HTTPS solo con HTTP/1.1 |
| `--mail-port P` | cattura delle email: un server SMTP sulla porta `P` (es. 1025) tiene ogni messaggio invece di consegnarlo; si leggono su `http://localhost:PORTA/.segvault/mail/` |
| `--mail-dir DIR` | dove tenere i messaggi catturati (default `DATA/mail`) |
| `--api-prefix P` | prefisso dell'API (default `/api/v1`; `none` = spenta). Con PHP, i percorsi sotto il prefisso che non sono dell'API vanno al sito |
| `--max-body MB` | corpo massimo di una richiesta (default 16) |
| `--timeout S` | ferma una richiesta che dura più di S secondi (default 30, `0` = mai) |
| `--no-auth` | consente un indirizzo non locale senza token (sconsigliato) |
| `-q, --quiet` | niente log delle richieste |

Il server rifiuta di mettersi in ascolto su un indirizzo di rete senza token: chiunque potrebbe
eseguire SQL arbitrario. `Ctrl+C` lo chiude in modo ordinato (log e catalogo scritti su disco).

**Una cartella dati, un processo.** Come per la GUI, la cartella dati è bloccata da chi la apre:
il server e la GUI non possono usare la stessa cartella nello stesso momento. In produzione il
server è l'unico proprietario dei dati e le applicazioni passano da lui.

## Endpoint

Tutti sotto `/api/v1`. Le risposte sono JSON con `"ok": true|false`.

| Metodo | Percorso | Risposta |
|---|---|---|
| `GET` | `/health` | `{"ok":true,"server":"SegVault","version":"1.0.4"}` (senza token) |
| `GET` | `/status` | uptime, richieste, errori, righe inviate, connessioni aperte, `busy`, `timeout_ms` |
| `POST` | `/query` | esegue SQL (sotto) |
| `GET` | `/databases` | `{"ok":true,"databases":["shop", ...]}` |
| `GET` | `/databases/{db}/tables` | `{"ok":true,"database":"shop","tables":[...],"views":[...]}` |
| `GET` | `/databases/{db}/tables/{t}` | colonne (`name`, `type`, `nullable`, `primary_key`, `auto_increment`, `default`) e `create` (lo statement `CREATE TABLE`) |

### `POST /api/v1/query`

```json
{
  "sql": "INSERT INTO ordine (cliente, totale) VALUES (?, ?); SELECT * FROM ordine WHERE cliente = ?",
  "params": ["Rossi", 42.5, "Rossi"],
  "database": "shop",
  "transaction": true,
  "rows": "objects"
}
```

| Campo | Tipo | Significato |
|---|---|---|
| `sql` | stringa, obbligatorio | uno o più statement separati da `;` |
| `params` | array | valori per i segnaposto `?`, nell'ordine (anche attraverso più statement) |
| `database` | stringa o `null` | `USE` eseguito prima dello script (default: `--database` del server) |
| `transaction` | booleano | tutto lo script in una transazione: se uno statement fallisce non resta nulla |
| `rows` | `"arrays"` (default) o `"objects"` | righe come `[1,"pane"]` oppure `{"id":1,"nome":"pane"}` |

In alternativa il corpo può essere SQL puro con `Content-Type: text/plain`, e database e formato
nella query string: `POST /api/v1/query?database=shop&rows=objects`.

**Risposta** — un risultato per statement eseguito:

```json
{
  "ok": true,
  "results": [
    { "rows_affected": 1, "time_ms": 0.031 },
    { "columns": [{"name":"id","type":"INT"}, {"name":"cliente","type":"VARCHAR"}, {"name":"totale","type":"FLOAT"}],
      "rows": [{"id":1,"cliente":"Rossi","totale":42.5}],
      "row_count": 1, "time_ms": 0.020 }
  ],
  "time_ms": 0.084
}
```

Un `INSERT` in una tabella con `AUTO_INCREMENT` riporta anche l'ultimo id generato:
`{ "rows_affected": 1, "last_insert_id": 42, "time_ms": 0.03 }`.

Tipi dei valori: `INT`/`BIGINT`/`FLOAT` → numero JSON, `BOOL` → `true`/`false`, `NULL` → `null`,
tutto il resto (`VARCHAR`, `TEXT`, `DATE`, ...) → stringa. Le date sono stringhe `AAAA-MM-GG`.

**Errori** — HTTP 400 con il messaggio, l'indice dello statement fallito (da 0) e la riga nel testo;
`results` contiene gli statement eseguiti prima:

```json
{ "ok": false, "results": [ ... ],
  "error": { "message": "Duplicate entry '3' for primary key 'id'", "statement": 1, "line": 2 } }
```

| HTTP | Quando |
|---|---|
| 400 | JSON non valido, SQL errato, vincolo violato, numero di `params` diverso dai `?` |
| 401 | token mancante o sbagliato |
| 403 | richiesta da una pagina web di un'altra origine non ammessa, o `Host` non locale senza token (vedi Sicurezza) |
| 404 | endpoint, database o tabella inesistente |
| 405 | metodo sbagliato (es. `GET /query`) |
| 411 | un `POST` senza `Content-Length` né corpo `Transfer-Encoding: chunked` |
| 501 | `Transfer-Encoding` diverso da `chunked` (es. `gzip`) |
| 413 | corpo oltre `--max-body` |
| 415 | `Content-Type` diverso da `application/json` / `text/plain` |
| 503 | troppe connessioni contemporanee (64) |

## Endpoint salvati (un'API senza scrivere backend)

Una query con un nome, salvata nel database, diventa un URL: un sito o un'app la chiamano senza
conoscere l'SQL e senza un backend in mezzo (come in PocketBase o Supabase). Gli endpoint sono le
righe della tabella `api_endpoints` del database (si gestiscono con l'SQL o dalla griglia dell'app):

```sql
CREATE TABLE api_endpoints (
    name        VARCHAR(60) PRIMARY KEY,
    sql         TEXT NOT NULL,                        -- uno o più statement con parametri :nome
    access      VARCHAR(10) NOT NULL DEFAULT 'token', -- 'public' o 'token'
    description VARCHAR(200)
);
INSERT INTO api_endpoints VALUES
  ('prodotti_economici', 'SELECT id, nome, prezzo FROM prodotto WHERE prezzo < :max ORDER BY prezzo', 'public', 'Listino'),
  ('nuovo_ordine', 'INSERT INTO ordine (prodotto, quantita, giorno) VALUES (:prodotto, :quantita, CURDATE())', 'token', NULL);
```

```bash
curl 'http://127.0.0.1:8484/api/v1/endpoints/prodotti_economici?database=shop&max=10'
curl -X POST http://127.0.0.1:8484/api/v1/endpoints/nuovo_ordine?database=shop \
     -H 'Authorization: Bearer TOKEN' -H 'Content-Type: application/json' -d '{"prodotto": 3, "quantita": 2}'
curl 'http://127.0.0.1:8484/api/v1/endpoints?database=shop'        # elenco (richiede il token, se c'è)
```

- I parametri `:nome` arrivano dalla query string (`GET`, i numeri restano numeri) o da un oggetto
  JSON nel corpo (`POST`) e vengono legati come i `?`: niente SQL injection. Un parametro mancante
  dà 400 `missing parameter :nome`.
- Lo script di un endpoint è sempre **atomico** (tutto o niente); le righe tornano come oggetti
  (`?rows=arrays` per gli array). Il database si indica con `?database=` (o `--database`).
- `access = 'public'`: chiamabile **senza token e da qualsiasi sito** (la risposta ha
  `Access-Control-Allow-Origin: *`): è il modo di dare dati a una pagina pubblica esponendo solo
  quelle query, non tutto il database. `'token'`: come il resto dell'API.

## Parametri `?` (niente SQL injection)

Non costruire mai l'SQL concatenando i dati dell'utente: usa `?` e `params`. Il server sostituisce
ogni `?` (fuori da stringhe, identificatori tra backtick e commenti) con un letterale SQL sicuro:
stringhe tra apici con `'` e `\` raddoppiati, numeri così come sono (i negativi tra parentesi),
`true`/`false`, `null`. Array e oggetti non sono ammessi. Un valore come `x'; DROP TABLE t; --`
resta una stringa.

## Transazioni e concorrenza

Le connessioni sono servite da thread separati. Una richiesta **di sola lettura** (solo `SELECT` /
`WITH ... SELECT`, senza sessione e senza `"transaction": true`) viene eseguita **in parallelo** con
le altre letture; tutte le altre richieste usano il motore una alla volta, e mentre una scrive le
letture aspettano (le scritture hanno la precedenza, così un flusso continuo di letture non le blocca).
Senza sessione ogni richiesta è un'unità chiusa:

- con `"transaction": true` lo script è atomico (commit alla fine, rollback al primo errore);
- `BEGIN` ... `COMMIT` dentro lo stesso `sql` funzionano; una transazione lasciata aperta a fine
  richiesta viene annullata e la risposta contiene `"warning"`;
- per una transazione che attraversa più richieste si usa una **sessione** (sotto).

Uno statement che fallisce non lascia mai modifiche a metà (come nella GUI); senza
`"transaction": true` gli statement **precedenti** della stessa richiesta restano invece applicati.

Il database corrente viene ripristinato alla fine di ogni richiesta: un `USE` dentro una richiesta
non cambia quello delle successive (passa sempre `database`, o avvia il server con `--database`).

### Sessioni

Una sessione conserva tra le richieste il database corrente, l'utente e la transazione aperta:

```bash
curl -X POST http://127.0.0.1:8484/api/v1/sessions          # {"ok":true,"session":"3f9a...","idle_timeout_s":300,...}
curl http://127.0.0.1:8484/api/v1/query -H "X-SegVault-Session: 3f9a..." -H "Content-Type: application/json" \
     -d '{"database":"shop","sql":"BEGIN; UPDATE conto SET saldo = saldo - 10 WHERE id = 1"}'
# ... altre richieste nella stessa transazione (anche dopo aver chiesto qualcosa all'utente) ...
curl http://127.0.0.1:8484/api/v1/query -H "X-SegVault-Session: 3f9a..." -H "Content-Type: application/json" \
     -d '{"sql":"UPDATE conto SET saldo = saldo + 10 WHERE id = 2; COMMIT"}'
curl -X DELETE http://127.0.0.1:8484/api/v1/sessions/3f9a...   # chiusa: una transazione aperta viene annullata
```

- La risposta di una query in sessione contiene `"session":{"id":...,"in_transaction":true|false}`.
- **Transazioni concorrenti.** Più sessioni (e connessioni MySQL) tengono aperte transazioni nello
  stesso momento. Ogni statement prende un lock sulle tabelle che usa (condiviso per leggere, esclusivo
  per scrivere: anche le tabelle toccate da trigger, chiavi esterne con `CASCADE`, viste e procedure)
  e lo tiene fino a `COMMIT` / `ROLLBACK` (locking a due fasi stretto: isolamento serializzabile,
  nessuna lettura di dati non confermati). Transazioni su tabelle diverse, o che leggono soltanto,
  vanno avanti insieme. **Le letture non aspettano mai le scritture (MVCC):** una `SELECT` su una
  tabella che un'altra transazione sta modificando vede subito la versione confermata (le righe nuove
  non ci sono, quelle modificate o cancellate sono come prima), come il *read committed* di MySQL e
  PostgreSQL; `SELECT ... FOR UPDATE` / `LOCK IN SHARE MODE` invece aspetta. Chi vuole scrivere una
  tabella in scrittura altrui aspetta il `COMMIT`, fino a 30 s
  (`Lock wait timeout exceeded`, errore MySQL 1205). Un ciclo di attese viene riconosciuto subito:
  una delle transazioni riceve `Deadlock found when trying to get lock; try restarting transaction`
  (1213, come MySQL: Laravel, Doctrine e gli altri ORM la ripetono da soli) e viene annullata.
  `CREATE` / `ALTER` / `DROP` aspettano che non ci siano transazioni aperte. Le richieste senza
  sessione hanno una sessione propria per la durata della richiesta. Tieni le transazioni brevi.
- Una transazione ferma da **30 s** viene annullata e una sessione inattiva da **5 minuti** viene
  chiusa: un client che si blocca o sparisce non ferma gli altri.
- Con il login (`--users`) la sessione appartiene all'utente che l'ha creata.
- Fino a 256 sessioni aperte. Il database indicato con `"database"` vale per la richiesta e resta
  quello corrente della sessione.

**Limite di tempo.** Una richiesta che lavora più di `--timeout` secondi (default 30) viene fermata
con HTTP 400 `"the request took longer than ... and was stopped"`: le `SELECT` (anche con subquery
correlate) si interrompono subito, le modifiche alla fine dello statement in corso, gli statement
successivi non vengono eseguiti. Così una query sbagliata non blocca tutte le altre app.

## Incorporare il motore in un'app C/C++ (senza server)

Per un'app desktop in C o C++ si può linkare direttamente il motore, come si fa con SQLite:

```bash
make lib                    # libsegvault.a
cc app.c -Isrc/bridge libsegvault.a -lm -pthread -o app
```

```c
#include "db_api.h"
db_init("./dati");
db_result_free(db_execute("USE shop"));
QueryResult* r = db_execute("SELECT nome, prezzo FROM prodotti");
for (int i = 0; r->success && i < r->num_rows; i++)
    printf("%s %s\n", r->rows[i].cells[0].value, r->rows[i].cells[1].value);
db_result_free(r);
db_shutdown();
```

## Utenti e permessi

Con `--users` (o *USER LOGIN: Required* nel pannello del server) ogni richiesta `/api` deve presentare
utente e password con HTTP Basic, e le sue query girano con i permessi di quell'utente:

```bash
curl -u anna:password http://127.0.0.1:8484/api/v1/query -H "Content-Type: application/json" \
     -d '{"database":"shop","sql":"SELECT * FROM prodotto"}'
```

Gli utenti si gestiscono in SQL (dall'editor, che lavora come amministratore, o da un utente
amministratore) o dal pannello *Settings › Users & permissions*:

```sql
CREATE USER anna IDENTIFIED BY 'una-password';
GRANT SELECT, INSERT ON shop.* TO anna;          -- tutto il database
GRANT UPDATE ON shop.ordine TO anna;             -- una tabella
GRANT ALL ON *.* TO capo WITH GRANT OPTION;      -- amministratore: tutto, anche gestire gli utenti
REVOKE INSERT ON shop.* FROM anna;
ALTER USER anna IDENTIFIED BY 'nuova-password';
SHOW USERS;  SHOW GRANTS FOR anna;
DROP USER anna;
```

- Permessi: `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `CREATE` (tabelle, viste, database),
  `DROP` (anche `TRUNCATE`), `ALTER` (anche indici e trigger), `ALL`.
- Il controllo copre tutte le tabelle toccate da uno statement: subquery, `JOIN`, tabelle derivate,
  CTE, rami di `UNION`, `INSERT ... SELECT`, `EXPLAIN`. Una vista si autorizza come una tabella.
- Le password sono salvate in `<data>/users.dat` come hash SHA-256 con sale, ripetuto 20.000 volte;
  dopo un tentativo sbagliato il server aspetta 300 ms. Le credenziali giuste restano in memoria al
  massimo un minuto (un utente eliminato è escluso subito).
- Il token, se impostato, continua a dare accesso completo. Gli endpoint salvati girano con i
  permessi di chi li ha creati (come una procedura), quindi un endpoint pubblico può leggere una
  tabella che i visitatori non vedono direttamente.

## Siti di sviluppo (virtual host)

Come Laragon, ma senza modificare il file `hosts`: i browser risolvono da soli ogni nome che finisce
in `.localhost`.

- Ogni sottocartella di `--static` è un sito: `www/blog` → `http://blog.localhost:8484/`
  (anche `blog.test`, se il nome punta a questa macchina, e i sottodomini `api.blog.localhost`).
  Se la cartella non ha un `index.php` / `index.html` ma ha una `public/`, la radice è `public/`
  (Laravel, Symfony): `.env` e il codice restano fuori dal sito.
- `--site NOME=CARTELLA` aggiunge un sito che sta altrove; un `NOME` con un punto
  (`shop.example.com`) risponde solo a quel nome esatto.
- `--site app=http://127.0.0.1:5173` (o `--site app=5173`) inoltra tutto `app.localhost` a un dev
  server: Vite, Next.js, Nuxt, Express, Django, Flask, Rails, Go, `php artisan serve`. Le risposte
  passano in streaming (Server-Sent Events, download), le connessioni WebSocket della ricarica a
  caldo vengono collegate, il backend riceve `Host` originale e `X-Forwarded-For / Proto / Host / Port`.
  Se il dev server è spento, il browser mostra una pagina 502 che spiega cosa avviare.
- `http://localhost:PORTA/` senza un index (e `/.segvault/` da qualsiasi sito) mostra il cruscotto:
  servizi attivi e siti, con il tipo di progetto riconosciuto. Solo per i client di questa macchina.
- Una cartella senza index mostra l'elenco dei suoi file (senza quelli nascosti); `--no-dir-listing`
  lo spegne.

## HTTPS e HTTP/2

Con `--https-port 8443` ogni sito risponde anche su `https://NOME.localhost:8443`.

- Al primo avvio SegVault crea un'autorità di certificazione locale (`DATA/certs/ca.crt`, chiave
  leggibile solo dall'utente) e firma al volo un certificato per ogni nome richiesto (SNI):
  `localhost`, `127.0.0.1`, `blog.localhost`, `shop.test`, ...
- `segvault-server --trust-ca` (o "Trust certificate" nel pannello) la aggiunge a quelle fidate:
  Windows (archivio Root dell'utente: Chrome, Edge), macOS (portachiavi), Linux (Chrome / Chromium
  e i profili di Firefox con `certutil`; anche il sistema, per curl / PHP / Node, se lanciato da root).
- OpenSSL 3 o successivo viene caricato all'avvio: su Linux e macOS c'è già, su Windows bastano
  `libssl-3-x64.dll` e `libcrypto-3-x64.dll`, che PHP per Windows ha nella sua cartella.
- HTTP/2 viene negoziato con ALPN (`--no-http2` per spegnerlo): una sola connessione per tutte le
  richieste del browser, senza il limite di 6 connessioni per sito di HTTP/1.1 (utile con Vite e
  con le pagine che caricano molti file). I browser usano HTTP/2 solo su HTTPS.
- PHP riceve `HTTPS=on`, `REQUEST_SCHEME=https` e la porta HTTPS; nei `.htaccess` `%{HTTPS}` vale
  `on`; i dev server dietro al proxy ricevono `X-Forwarded-Proto: https`.

## .htaccess

I progetti presi da Apache (XAMPP, UniServerZ, Laragon, hosting condivisi) funzionano senza
modifiche: SegVault legge i `.htaccess` delle cartelle del percorso a ogni richiesta (una modifica
vale subito), come Apache.

- `mod_rewrite`: `RewriteEngine`, `RewriteBase`, `RewriteCond` (variabili `%{REQUEST_FILENAME}`,
  `%{REQUEST_URI}`, `%{QUERY_STRING}`, `%{HTTP_HOST}`, `%{HTTP:Nome}`, `%{HTTP_USER_AGENT}`,
  `%{REQUEST_METHOD}`, `%{HTTPS}`, `%{THE_REQUEST}`, ...; prove `-f -d -s -l`, regex, `!`, `=`, `<`, `>`;
  flag `NC` e `OR`), `RewriteRule` (riferimenti `$N` e `%N`; flag `L`, `END`, `QSA`, `QSD`, `NC`,
  `R[=301]`, `F`, `G`, `C`, `S=n`). Le regole di WordPress, Laravel, Symfony, CodeIgniter, Drupal e
  Joomla funzionano così come sono. `REQUEST_URI` resta quello originale (come lo vede PHP con Apache).
- `Require all denied` / `Deny from all` (anche dentro `<Files>` e `<FilesMatch>`): 403;
  `Require all granted` in una sottocartella riapre.
- `Options -Indexes`: niente elenco dei file.
- I blocchi `<IfModule !...>`, `<Limit>`, `<If>` vengono saltati; le altre direttive (es. `Header`,
  `php_value`, `ExpiresByType`) ignorate. Un pattern non valido viene scritto nel log e la regola saltata.

## Email di sviluppo (cattura)

Con `--mail-port 1025` nessuna email esce dalla macchina: il server SMTP di SegVault accetta ogni
messaggio (qualsiasi utente e password, niente TLS) e lo salva in `DATA/mail/ID.eml`.

- Configurazione delle app: host `127.0.0.1`, porta `1025`, senza cifratura. Laravel
  `MAIL_MAILER=smtp MAIL_HOST=127.0.0.1 MAIL_PORT=1025`; Symfony `MAILER_DSN=smtp://127.0.0.1:1025`;
  Nodemailer `{ host: '127.0.0.1', port: 1025, secure: false }`; Django `EMAIL_HOST='127.0.0.1'`,
  `EMAIL_PORT=1025`; WordPress e `mail()` di PHP: niente da fare (SegVault imposta `sendmail_path`
  o `SMTP` / `smtp_port` dei processi `php-cgi` che avvia).
- `http://localhost:PORTA/.segvault/mail/`: elenco aggiornato da solo, ricerca, HTML (in un iframe
  `sandbox`, senza script), testo, sorgente `.eml`, allegati, cancellazione.
- API per i test automatici (Playwright, Cypress, PHPUnit, pytest):

| Richiesta | Risposta |
|---|---|
| `GET /.segvault/mail/api/messages` | `{"total":N,"messages":[{"id","time","size","from","to","subject"}]}`, dal più recente; filtri `?to=` `?from=` `?subject=` `?q=` (sottostringhe, senza distinzione di maiuscole) e `?limit=` |
| `GET /.segvault/mail/api/messages/ID` (o `/latest`) | il messaggio: `cc`, `date`, `message_id`, `headers`, `text`, `html` (o `null`), `attachments` |
| `GET /.segvault/mail/ID.eml` · `/ID.html` · `/ID/att/N` | sorgente, parte HTML, allegato N |
| `DELETE /.segvault/mail/api/messages[/ID]` | cancella tutti i messaggi (o uno) |

Le pagine della posta rispondono solo ai client di questa macchina.

## Sicurezza

- Default sicuri: ascolto solo su `127.0.0.1`; in rete serve `--token` o `--users`.
- Il token viaggia in chiaro su HTTP: fuori dalla macchina locale metti il server dietro un
  reverse proxy HTTPS (nginx, Caddy) e lascialo in ascolto su `127.0.0.1`.
- Il token dà accesso completo (anche `DROP`): non metterlo nel codice di una pagina pubblica.
  Per un sito pubblico il browser parla con il tuo backend, che parla con SegVault.
- `--static` non serve file nascosti (`/.qualcosa`, tranne `/.well-known/`) né percorsi con `..`.
- File statici: inviati a pezzi (anche più grandi della memoria), `Range` (video, download ripresi:
  `206` / `416`), `ETag` e `Last-Modified` con `Cache-Control: no-cache` (il browser chiede ogni volta
  e riceve `304` se il file non è cambiato), testi fra 1 KB e 8 MB compressi con gzip quando il client
  lo accetta (anche le risposte JSON dell'API e le pagine PHP). Una `404.html` nella cartella diventa
  la pagina degli errori 404 del sito.
- **Siti aperti nel browser.** Una pagina qualsiasi può inviare un `POST` a `localhost` (con
  `text/plain` il browser non chiede il permesso CORS prima). Per questo una richiesta con un header
  `Origin` viene eseguita solo se l'origine è la pagina servita dal server stesso o quella ammessa
  con `--cors`; le altre ricevono 403 senza eseguire nulla. Senza token, inoltre, il server in
  ascolto su `127.0.0.1` risponde solo se `Host` è `localhost` / `*.localhost` / `127.x.x.x` (difesa dal
  *DNS rebinding*: i nomi `.localhost` puntano sempre a questa macchina, RFC 6761).
  Client che non sono browser (curl, Python, C#, Java, Node) non inviano `Origin` e non ne sono toccati.
