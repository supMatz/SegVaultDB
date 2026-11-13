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
- [ ] Prova con un php-cgi vero (il test la fa da solo se `php-cgi` è installato) e con WordPress
- [ ] Prefisso dell'API configurabile: `/api/v1` di SegVault copre quello di un'app Laravel
- [ ] Corpi `Transfer-Encoding: chunked` in ingresso
- [ ] Pannello del server: interruttore PHP, percorso di `php-cgi` (ricerca automatica in `php/` accanto all'eseguibile e nel PATH), log degli errori PHP
- [ ] Risposte grandi in streaming (ora la risposta di PHP è tenuta in memoria)

## 2. Server web completo
- [ ] Tipi MIME mancanti (`.pdf`, `.mp4`, `.webm`, `.mp3`, `.wasm`, `.xml`, `.zip`, `.map`, `.avif`, ...)
- [ ] File statici in streaming (ora vengono letti tutti in memoria) e richieste `Range` (video, download ripresi)
- [ ] Cache: `ETag` / `Last-Modified`, `304 Not Modified`
- [ ] Compressione gzip per testi (HTML, CSS, JS, JSON)
- [ ] Pagine d'errore personalizzate (`404.html`) ed elenco dei file di una cartella (opzionale)
- [ ] `/.well-known/` ammesso (ora ogni percorso con `/.` è rifiutato)
- [ ] Virtual host: più siti per nome (`progetto1.test`, `progetto2.test`)
- [ ] Regole di rewrite minime (alternativa a `.htaccess`)
- [ ] HTTPS integrato (certificato self-signed generato al primo avvio, come XAMPP), anche per il protocollo MySQL

## 3. Compatibilità MySQL per le app vere
- [ ] `information_schema` (TABLES, COLUMNS, STATISTICS, KEY_COLUMN_USAGE, ...): lo usano WordPress, Laravel, Doctrine, i tool grafici
- [ ] `SHOW TABLE STATUS`, `SHOW FULL COLUMNS`, `SHOW CREATE DATABASE`
- [ ] `SQL_CALC_FOUND_ROWS` / `FOUND_ROWS()`
- [ ] `INSERT IGNORE`; verificare `REPLACE INTO` e `ON DUPLICATE KEY UPDATE`
- [ ] `GROUP_CONCAT`, altre funzioni di stringa / data di MySQL
- [ ] `LOCK TABLES` / `UNLOCK TABLES` accettati
- [ ] Prova completa: installazione di WordPress, phpBB, un progetto Laravel

## 4. Concorrenza
- [ ] Scritture concorrenti (lock sulle righe o MVCC); più transazioni aperte insieme

## 5. Pannello di controllo stile XAMPP
- [ ] Servizi separati (Web, PHP, MySQL) avviabili e fermabili uno per uno, con il loro log
- [ ] Editor e server attivi insieme
- [ ] Icona nella barra di sistema; installazione come servizio con un clic (Windows e systemd)
- [ ] Controllo delle porte occupate per ogni servizio
- [ ] Modifica di `php.ini` e della configurazione dal pannello
- [ ] Pacchetto portabile: zip con `segvault(.exe)` + `php/` + `www/`, pronto all'uso

## 6. Facoltativi (XAMPP li ha, servono a pochi)
- [ ] Posta di test (cattura delle email inviate con `mail()` di PHP, come Mailpit)
- [ ] Perl / altri interpreti CGI
