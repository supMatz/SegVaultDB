# Prompt per Fable 5.2 — analisi dell'architettura e roadmap "meglio di XAMPP e UniServerZ"

Copia tutto quello che segue (dalla riga `---` in giù) come primo messaggio di una sessione nuova, aperta nella
cartella del repository SegVaultDB.

---

Sei un principal engineer incaricato di una revisione dell'architettura di **SegVaultDB**, il repository in cui
ti trovi. Il tuo compito è **solo analizzare e scrivere la roadmap**: non modificare codice, test, README o
CHANGELOG; l'unico file che cambi è `TODO.md`.

## Il prodotto

SegVault vuole essere uno stack di sviluppo locale **vendibile e di fascia alta**, all'altezza e chiaramente
migliore di **XAMPP**, **UniServerZ** (e, come riferimento moderno, Laragon). È scritto in C11 senza dipendenze
esterne a parte OpenSSL caricata a runtime:

- motore SQL proprio, compatibile col protocollo e col dialetto MySQL (`src/storage`, `src/tx`, `src/table`,
  `src/index`, `src/catalog`, `src/query`, `src/bridge`): WAL v2 con CRC/LSN, pagine con checksum, recupero
  ARIES-like, lock di tabella con code FIFO, letture READ COMMITTED, decimali esatti, statistiche e ordine dei
  join a costo;
- server (`src/server`): HTTP/1.1 + HTTP/2 + TLS con CA locale, PHP via FastCGI (pool di `php-cgi` o php-fpm
  esterno), siti per cartella `NAME.localhost`, `.htaccess` (mod_rewrite), reverse proxy con WebSocket, CGI,
  mail catcher SMTP con pagina web, protocollo MySQL (TLS, `caching_sha2_password`, prepared statement,
  cursori), API HTTP JSON, metriche Prometheus, servizio utente (`--install-service`);
- app desktop con GUI disegnata a mano (`src/widgets`, `src/platform/{xlib,win32}.c`): editor SQL, designer,
  griglie, pannello dei servizi, tray.

Stato attuale: versione 1.4.0. `TODO.md` contiene la roadmap ingegneristica nata da un audit del codice (voci
P0–P3 con `file:line`, "Where / Why / Do / Done when", e una nota **Fixed / Done** per quelle chiuse). Le voci
ancora aperte sono P1.6 (B+tree su disco), P1.14 (esecuzione in streaming), P3.9 (audit dei moduli non ancora
letti: `parser.c`, gran parte di `executor.c`, `src/widgets/*`, `win32.c`), P3.10 (CA locale senza name
constraints). In fondo al file c'è il riassunto delle milestone XAMPP già completate.

## Cosa devi fare

1. **Leggi prima di giudicare.** Ricostruisci l'architettura dal codice, non dal README: punti di ingresso
   (`src/server/main.c` per il server, `src/main.c` per l'app desktop), thread e lock, formato su disco, percorso di
   una query (lexer → parser → planner/executor → heap/buffer pool → WAL), percorso di una richiesta HTTP/PHP,
   build (`Makefile`), CI (`.github/workflows/*.yml`), test (`tests/`, `make test`, `test-asan`, `test-tsan`,
   `test-win`, `fuzz`, `bench`). Usa ricerche ampie e letture mirate; verifica ogni affermazione con il file e
   la riga.
2. **Confronta funzione per funzione** con XAMPP, UniServerZ e Laragon (versioni attuali; se hai la ricerca
   web, controlla le loro pagine ufficiali e cita la fonte). Almeno questi assi:
   - *componenti*: Apache/Nginx, MariaDB/MySQL (versioni, InnoDB: lock di riga, MVCC REPEATABLE READ, FK con
     cascate, full-text, spaziale, eventi, replica), PHP (più versioni in parallelo e per sito, estensioni,
     `php.ini` per sito, Xdebug, Composer), phpMyAdmin/Adminer, Perl, Tomcat, FTP, mail, Redis/Memcached,
     Node.js/Python/Go, terminale integrato, condivisione pubblica (tunnel);
   - *esperienza*: installazione e aggiornamento, portabilità (USB), primo avvio, creazione rapida di un
     progetto (WordPress, Laravel, Symfony…), virtual host automatici, HTTPS automatico, gestione porte
     occupate, log, backup/ripristino, import/export di dump grandi, migrazione da XAMPP (dati MySQL esistenti,
     `htdocs`, vhost), messaggi d'errore, documentazione, lingua italiana, accessibilità;
   - *qualità*: sicurezza dei default (bind solo locale, password, token, permessi dei file), affidabilità
     (crash, recupero, corruzione), prestazioni misurate (`bench/`, confronto con SQLite già presente:
     aggiungi cosa servirebbe per un confronto con MariaDB), compatibilità con le app vere (WordPress,
     Laravel, Drupal, Magento, phpMyAdmin collegato al protocollo MySQL, ORM/driver: PDO, mysqli, JDBC,
     mysql2 di Node, SQLAlchemy), piattaforme (Windows, Linux, macOS), firma del codice, telemetria assente.
3. **Trova cosa manca o è debole** rispetto a quel confronto e rispetto a un prodotto che si vende: funzioni
   assenti, limiti di architettura (es. indici solo in memoria, esecuzione che materializza tutte le righe,
   lock di tabella, un solo processo), rischi di sicurezza, lacune di test, debito tecnico che blocca le
   funzioni future. Per ogni lacuna importante prova a **verificarla**: un test esistente che manca, una query
   che fallisce, una misura. Puoi compilare ed eseguire (`make`, i binari di test, piccoli programmi di prova
   linkati a `libsegvault.a`), ma **sempre** dentro un recinto di risorse:
   `systemd-run --user --scope -q -p MemoryMax=8G -p TasksMax=400 timeout N <comando>` (un test fuori
   controllo ha già fatto crashare il PC dell'utente per memoria esaurita). Non lanciare due suite pesanti
   insieme (ASan + suite completa).
4. **Scrivi in `TODO.md` man mano**, non tutto alla fine (la sessione può interrompersi): appena una lacuna è
   confermata, aggiungi la voce. Regole:
   - nuova sezione `## P4 — Product parity and beyond (XAMPP / UniServerZ / Laragon)` subito prima di
     `## Completed milestones`; se una lacuna è di motore o sicurezza e ha la gravità di P0–P3, mettila nella
     sezione giusta con il numero successivo libero (P0.13, P1.15, P2.16, P3.11…);
   - stesso formato delle voci esistenti, in **inglese**: titolo con tag (`[ENGINE]`, `[PERF]`, `[SECURITY]`,
     `[NET]`, `[PHP]`, `[GUI]`, `[UX]`, `[OPS]`, `[DOCS]`, `[COMPAT]`, `[PLATFORM]`), stato
     (`[reproduced]` / `[code]` / `[competitor]` con la fonte), poi **Where** (file:line o "missing"), **Why**
     (impatto concreto per l'utente o per la vendita), **Do** (soluzione proposta, con le alternative scartate
     e perché), **Done when** (criteri misurabili e il test che li verifica);
   - stima di massima (S / M / L / XL) e dipendenze da altre voci (es. "needs P1.6");
   - niente doppioni: se una lacuna è già in una voce aperta, aggiungi lì un sotto-punto "Review 2026-11-xx";
   - aggiorna la riga di stato in cima al file con il numero di voci aggiunte per livello.
5. **Alla fine**, sempre in `TODO.md`, aggiungi in testa alla sezione P4 una tabella di sintesi
   "SegVault vs XAMPP vs UniServerZ vs Laragon" (righe = capacità, colonne = prodotti, valori ✓ / parziale / ✗
   con una nota breve) e una proposta di ordine di lavoro in 3 rilasci (1.5, 1.6, 2.0) con il motivo
   dell'ordine.

## Vincoli

- Non toccare altri file oltre a `TODO.md`; non fare commit né push (li fa l'utente). Se vuoi lasciare un
  prototipo o una sonda, mettila in una cartella temporanea fuori dal repository.
- Niente affermazioni non verificate: se una cosa non l'hai controllata, scrivi "not verified" nella voce.
- Rispondi all'utente in italiano, breve: quante voci hai aggiunto per livello, le 5 più importanti in una
  riga ciascuna, e cosa non sei riuscito a verificare.
