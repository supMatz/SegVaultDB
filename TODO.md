# TODO — SegVault al livello di XAMPP / UniServerZ

XAMPP e UniServerZ sono uno *stack*: server web + PHP + MySQL + phpMyAdmin + pannello di controllo.
SegVault copre già database e phpMyAdmin; qui c'è quello che manca, in ordine di importanza.
`[x]` = fatto, `[~]` = in corso, `[ ]` = da fare.

## 1. PHP (FastCGI)
- [x] Client FastCGI nel server (`src/server/fastcgi.c`): richieste `.php` della Web folder a `php-cgi` / `php-fpm`
- [x] `--php PATH|auto` avvia un gruppo di processi `php-cgi -b` (riavviati se escono); `--php-fcgi HOST:PORT` usa un php-fpm esistente
- [x] Variabili CGI complete (`SCRIPT_FILENAME`, `REQUEST_URI`, `PATH_INFO`, `QUERY_STRING`, `HTTP_*`, `REDIRECT_STATUS`, ...)
- [x] Risposte CGI: `Status:`, `Location:` (redirect), più `Set-Cookie`, intestazioni arbitrarie
- [x] `index.php` come pagina predefinita; front controller (`/qualcosa` → `/index.php` se il file non esiste: URL "belli" di WordPress / Laravel)
- [x] Un `.php` non viene mai servito come testo se PHP è spento (403, niente codice sorgente con le password)
- [x] Tutti i metodi HTTP verso PHP (`POST`, `PUT`, `DELETE`, `PATCH`), corpi non JSON (form, multipart)
- [x] Prova con un php-cgi vero (PHP 8.5: PDO con prepared statement, `lastInsertId`, mysqli, redirect, cookie, URL belli)
- [x] Prefisso dell'API configurabile (`--api-prefix`, anche `none`); con PHP i percorsi sotto `/api/v1` che non sono dell'API vanno al sito (Laravel)
- [x] Corpi `Transfer-Encoding: chunked` in ingresso (API e PHP; `Content-Length` + chunked rifiutati contro il request smuggling)
- [x] I `php-cgi` riavviati da una connessione non muoiono con lei (nati da un thread apposito: `PR_SET_PDEATHSIG` è per thread)
- [x] Pannello del server: interruttore PHP, `php-cgi` cercato da solo (o `php_cgi=` in `server.ini`), errori PHP nel registro; colonna delle opzioni che scorre
- [x] Risposte di PHP in streaming: oltre 256 KB, o con `text/event-stream` / `X-Accel-Buffering: no`, partono subito a pezzi (download ed export grandi, `flush()`, Server-Sent Events); quelle piccole restano compresse con gzip

## 2. Server web completo
- [x] Tipi MIME mancanti (`.pdf`, `.mp4`, `.webm`, `.mp3`, `.wasm`, `.xml`, `.zip`, `.map`, `.avif`, ...)
- [x] File statici in streaming e richieste `Range` (video, download ripresi)
- [x] Cache: `ETag` / `Last-Modified`, `304 Not Modified`
- [x] Compressione gzip per testi (HTML, CSS, JS, JSON; anche API e PHP), con il deflate già usato per i PNG
- [x] Pagina d'errore personalizzata (`404.html`)
- [x] Elenco dei file di una cartella senza index (acceso di default nel server di sviluppo, `--no-dir-listing` per spegnerlo)
- [x] `/.well-known/` ammesso
- [x] Virtual host automatici come Laragon, senza toccare il file hosts: ogni cartella `www/NOME` è `http://NOME.localhost:PORTA` (anche `NOME.test` e i sottodomini `api.NOME.localhost`); `public/` come radice se la cartella non ha un index (Laravel, Symfony); siti espliciti `--site NOME=CARTELLA`, anche con un nome completo (`shop.example.com`)
- [x] Reverse proxy per i dev server (`--site app=http://127.0.0.1:5173`: Vite, Next.js, Express, Django, Rails, ...): streaming, chunked, keep-alive, WebSocket (ricarica a caldo), `X-Forwarded-*`, pagina 502 che spiega come avviare il dev server
- [x] Cruscotto su `http://localhost:PORTA/` (se la cartella non ha un index) e `/.segvault/`: servizi attivi e siti con il tipo di progetto riconosciuto (Laravel, WordPress, Symfony, Node, ...)
- [x] `.htaccess` come Apache: `mod_rewrite` (RewriteCond / RewriteRule con i flag usati da WordPress, Laravel, Symfony, ...), `Require all denied` / `Deny from all` (anche in `<Files>` / `<FilesMatch>`), `Options -Indexes`; `--no-htaccess` per spegnerli. Il motore regex ora ha i gruppi di cattura
- [x] HTTPS integrato (`--https-port 8443`, interruttore nel pannello): OpenSSL 3/4 caricato all'avvio (nessuna dipendenza di compilazione; su Windows le DLL di PHP), CA locale in `DATA/certs` come mkcert, un certificato per ogni nome (SNI) firmato al volo, `--trust-ca` / "Trust certificate" la rende fidata (Chrome, Firefox, Edge, sistema); PHP vede `HTTPS=on`, `%{HTTPS}` nei `.htaccess`, `X-Forwarded-Proto: https` al proxy
- [x] HTTP/2 su HTTPS (ALPN `h2`, HPACK con Huffman, controllo di flusso, fino a 64 stream per connessione): 200 file in parallelo su una connessione in ~30 ms; streaming, SSE, Range, 304 anche in HTTP/2. HTTP/3 (QUIC) non porta vantaggi in locale
- [ ] TLS anche per il protocollo MySQL

## 3. Compatibilità MySQL per le app vere
- [x] `CREATE TABLE` nel dialetto di MySQL: `bigint(20) unsigned`, `longtext`, `enum(...)` (diventa un `CHECK`), `KEY` / `INDEX` / `FULLTEXT` / `UNIQUE KEY nome`, prefissi `col(191)`, `ENGINE=`, `CHARSET`, `COLLATE`, `AUTO_INCREMENT=n`, `COMMENT`, `ON UPDATE CURRENT_TIMESTAMP`, `DEFAULT CURRENT_TIMESTAMP`, `DEFAULT ''`
- [x] Catalogo v5: tipo SQL originale, `UNSIGNED`, colonne degli indici, collation (cataloghi v1–v4 convertiti al caricamento)
- [x] `information_schema` (SCHEMATA, TABLES, COLUMNS, STATISTICS, TABLE_CONSTRAINTS, KEY_COLUMN_USAGE, REFERENTIAL_CONSTRAINTS, CHECK_CONSTRAINTS, VIEWS, TRIGGERS, ROUTINES, ENGINES, CHARACTER_SETS, COLLATIONS, ...), filtrato per i permessi dell'utente
- [x] `SHOW [FULL] COLUMNS / FIELDS`, `SHOW INDEX / KEYS`, `SHOW TABLE STATUS`, `SHOW TABLES [FROM db] LIKE`, `SHOW DATABASES LIKE`, `SHOW CREATE DATABASE`, `SHOW ENGINES / COLLATION / CHARSET / WARNINGS / PROCESSLIST`, anche con `WHERE`
- [x] `SQL_CALC_FOUND_ROWS` / `FOUND_ROWS()`, `ROW_COUNT()`, `FOR UPDATE`, `LOCK IN SHARE MODE`, `DISTINCTROW`, `SQL_NO_CACHE`
- [x] `INSERT IGNORE`, `REPLACE INTO`, `INSERT ... SET`, `ON DUPLICATE KEY UPDATE` con `VALUES(col)` e l'alias di MySQL 8
- [x] `UPDATE` / `DELETE` su più tabelle (`DELETE a, b FROM ...`, `UPDATE t1 JOIN t2 ... SET`), `ORDER BY` / `LIMIT`, alias
- [x] `REGEXP` / `RLIKE` / `REGEXP_LIKE / REPLACE / SUBSTR / INSTR` (motore di espressioni regolari proprio, anche su Windows)
- [x] `GROUP_CONCAT([DISTINCT] ... ORDER BY ... SEPARATOR ...)`, più di 100 funzioni di stringa, numeri, date e informazione (`DATE_FORMAT`, `STR_TO_DATE`, `DATE_ADD` con `INTERVAL`, `TIMESTAMPDIFF`, `UNIX_TIMESTAMP`, `MD5`, `SHA2`, `CAST`, `CONVERT`, `EXTRACT`, ...)
- [x] `DECIMAL(p,s)` con `s` decimali (`1.50`), date zero `0000-00-00`, `LIKE` con `\` come escape (come MySQL e PostgreSQL)
- [x] Modalità MySQL per le connessioni del protocollo MySQL (regole permissive di MySQL: `DISTINCT ... ORDER BY` su colonne non selezionate); nomi di colonna senza alias (`t.*, tt.*`)
- [x] Bug del motore trovati con WordPress: `WHERE id = '1'` sulla chiave primaria restituiva 0 righe (ora la chiave viene convertita), buffer non inizializzati in `SHOW`
- [x] Prova completa con **WordPress 7.1.4**: installazione, login, tutta l'amministrazione, articoli con `meta_query` / `tax_query`, paginazione, ricerca, commenti, utenti, transient, cestino, menu, feed, API REST: nessun errore di database (`test_mysql`: 112 controlli)
- [x] Confronti senza distinzione di maiuscole per le tabelle con collation `_ci` (come MySQL: `=`, `IN`, indici, `UNIQUE`, `GROUP BY`, `DISTINCT`, `ORDER BY`, join); le tabelle native di SegVault restano case-sensitive

## 4. Concorrenza
- [x] Più transazioni aperte insieme (sessioni HTTP, connessioni MySQL, GUI): lock di tabella S / X per statement (anche trigger, chiavi esterne, viste, procedure) tenuti fino a COMMIT / ROLLBACK, attesa che lascia lavorare gli altri, deadlock riconosciuti (errore 1213, la vittima viene annullata), `Lock wait timeout` (1205) dopo 30 s; letture parallele solo su tabelle non in scrittura (niente letture sporche); recupero dopo un crash con più transazioni interlacciate; ROLLBACK TO senza troncare il log
- [ ] Lock sulle righe o MVCC (letture che non aspettano mai le scritture)

## 5. Pannello di controllo stile XAMPP
- [x] Pannello del server: interruttore della cattura delle email (porta 1025, "Open inbox (N)" aggiornato da solo), link al cruscotto "All sites", siti `site.NOME=DEST` letti da `server.ini`, messaggi chiari se la porta 1025 è già presa (Mailpit / MailHog)
- [ ] Servizi separati (Web, PHP, MySQL) avviabili e fermabili uno per uno, con il loro log
- [ ] Editor e server attivi insieme
- [ ] Icona nella barra di sistema; installazione come servizio con un clic (Windows e systemd)
- [ ] Controllo delle porte occupate per ogni servizio
- [ ] Modifica di `php.ini` e della configurazione dal pannello
- [ ] Pacchetto portabile: zip con `segvault(.exe)` + `php/` + `www/`, pronto all'uso

## 6. Facoltativi (XAMPP li ha, servono a pochi)
- [x] Cattura delle email (come Mailpit, integrata): server SMTP `--mail-port 1025` (AUTH con qualsiasi credenziale, pipelining, Bcc dalla busta), messaggi salvati in `DATA/mail/*.eml` (restano dopo un riavvio), pagina `/.segvault/mail/` con HTML in un iframe sandbox, testo, sorgente, allegati; API JSON con filtri (`?to=`, `?subject=`, `/latest`) per i test E2E; `mail()` di PHP instradato da solo (`sendmail_path` su Linux, `SMTP`/`smtp_port` su Windows)
- [ ] Perl / altri interpreti CGI
