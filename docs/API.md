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
| `GET` | `/health` | `{"ok":true,"server":"SegVault","version":"1.2.0"}` (senza token) |
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
| 411 | manca `Content-Length` (i corpi *chunked* non sono supportati) |
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

Il motore ha un solo stato di sessione, quindi le richieste vengono eseguite **una alla volta**
(le connessioni sono comunque servite da thread separati: un client lento a inviare o ricevere non
blocca gli altri). Ogni richiesta è un'unità chiusa:

- con `"transaction": true` lo script è atomico (commit alla fine, rollback al primo errore);
- `BEGIN` ... `COMMIT` dentro lo stesso `sql` funzionano; una transazione lasciata aperta a fine
  richiesta viene annullata e la risposta contiene `"warning"`;
- non esistono transazioni che attraversano più richieste: ciò che deve essere atomico va in una
  richiesta sola.

Uno statement che fallisce non lascia mai modifiche a metà (come nella GUI); senza
`"transaction": true` gli statement **precedenti** della stessa richiesta restano invece applicati.

Il database corrente viene ripristinato alla fine di ogni richiesta: un `USE` dentro una richiesta
non cambia quello delle successive (passa sempre `database`, o avvia il server con `--database`).

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

## Sicurezza

- Default sicuri: ascolto solo su `127.0.0.1`; in rete serve `--token`.
- Il token viaggia in chiaro su HTTP: fuori dalla macchina locale metti il server dietro un
  reverse proxy HTTPS (nginx, Caddy) e lascialo in ascolto su `127.0.0.1`.
- Il token dà accesso completo (anche `DROP`): non metterlo nel codice di una pagina pubblica.
  Per un sito pubblico il browser parla con il tuo backend, che parla con SegVault.
- `--static` non serve file nascosti (`/.qualcosa`) né percorsi con `..`.
- **Siti aperti nel browser.** Una pagina qualsiasi può inviare un `POST` a `localhost` (con
  `text/plain` il browser non chiede il permesso CORS prima). Per questo una richiesta con un header
  `Origin` viene eseguita solo se l'origine è la pagina servita dal server stesso o quella ammessa
  con `--cors`; le altre ricevono 403 senza eseguire nulla. Senza token, inoltre, il server in
  ascolto su `127.0.0.1` risponde solo se `Host` è `localhost` / `127.x.x.x` (difesa dal *DNS rebinding*).
  Client che non sono browser (curl, Python, C#, Java, Node) non inviano `Origin` e non ne sono toccati.
