/*
SCOPO: server HTTP/JSON di SegVault: espone il motore (db_api.h) a siti web,
       app desktop e qualsiasi linguaggio con un client HTTP.

       Il motore ha un solo stato di sessione (database corrente, transazione aperta):
       le richieste vengono eseguite una alla volta; senza sessione ognuna è un'unità chiusa,
       con una sessione (POST /api/v1/sessions) database e transazione restano tra le richieste
       (vedi docs/API.md). Le connessioni sono servite da thread separati, quindi un
       client lento non blocca gli altri mentre invia o riceve.
*/
#ifndef SV_SERVER_H
#define SV_SERVER_H

#include <stdbool.h>
#include <stddef.h>

// un sito con un nome: "blog" risponde a http://blog.localhost e http://blog.test (anche ai
// sottodomini: api.blog.localhost), un nome con un punto ("shop.example.com") solo a sé stesso
typedef struct {
    const char* name;
    const char* target;       // cartella del sito, oppure "http://host:porta" / "porta" (reverse proxy verso
                              // un dev server: Vite, Next.js, Express, Django, ...; WebSocket compresi)
} SvSite;

typedef struct {
    const char* bind_addr;    // indirizzo IPv4 (default 127.0.0.1)
    int         port;         // 0 = porta libera scelta dal sistema (vedi sv_server_port)
    const char* token;        // se non NULL: le richieste /api richiedono "Authorization: Bearer <token>"
    const char* cors_origin;  // se non NULL: origine ammessa per le chiamate dal browser ("*" = tutte)
    const char* default_db;   // database usato quando la richiesta non ne indica uno
    const char* static_dir;   // se non NULL: file statici serviti fuori da /api (una web app)
    size_t      max_body;     // byte massimi del corpo di una richiesta (0 = 16 MB)
    int         max_clients;  // connessioni contemporanee (0 = 64)
    int         timeout_ms;   // una richiesta che lavora più a lungo viene interrotta (0 = mai)
    bool        quiet;        // niente log delle richieste su stderr
    bool        require_login;// le richieste /api devono avere utente e password (Authorization: Basic)
                              // o il token; ogni query gira con i permessi dell'utente (vedi auth.h)
    bool        mysql;        // ascolta anche col protocollo di rete MySQL (driver mysqli, PDO, JDBC, client mysql)
    int         mysql_port;   // porta MySQL (0 = libera scelta dal sistema; vedi sv_server_mysql_port)
    const char* audit_file;   // se non NULL: ogni istruzione che non è una lettura viene aggiunta a questo
                              // file (ora, client, utente, database, esito, testo)
    // PHP nella cartella del sito (static_dir), via FastCGI (vedi fastcgi.h); senza, un .php non viene servito
    const char* php_cgi;      // percorso di php-cgi: SegVault avvia e sorveglia php_workers processi
    const char* php_fcgi;     // oppure un backend già avviato: "host:porta" o "unix:/percorso.sock" (php-fpm)
    int         php_workers;  // processi php-cgi (0 = 4): richieste PHP eseguite insieme
    bool        php_start_off;// PHP configurato ma spento all'avvio (si accende con sv_server_service)
    const char* api_prefix;   // dove sta l'API HTTP (NULL = "/api/v1"; "none" = spenta, resta il protocollo MySQL).
                              // Con PHP le richieste sotto il prefisso che non sono dell'API vanno al sito
    // siti (virtual host). Ogni sottocartella di static_dir è anche un sito automatico:
    // www/blog -> http://blog.localhost:PORTA (i browser risolvono *.localhost da soli), con
    // www/blog/public come radice se la cartella non ha un index (Laravel, Symfony)
    const SvSite* sites;
    int         n_sites;
    bool        no_auto_sites;// niente siti automatici dalle sottocartelle
    bool        dir_listing;  // elenco dei file di una cartella senza index (come Apache "Indexes")
    bool        no_htaccess;  // ignora i file .htaccess (di default si applicano come in Apache, vedi rewrite.h)
    bool        cgi;          // esegue i programmi CGI del sito: .cgi, .pl, .py, .rb (vedi cgi.h)
    // cattura delle email (vedi mail.h): un server SMTP che tiene i messaggi invece di consegnarli,
    // visibili su http://localhost:PORTA/.segvault/mail. Con PHP, mail() arriva qui da solo
    bool        mail;
    int         mail_port;    // porta SMTP (0 = libera scelta dal sistema; vedi sv_server_mail_port)
    const char* mail_dir;     // cartella dei messaggi (.eml)
    // HTTPS (vedi tls.h): OpenSSL caricato all'avvio, CA locale in cert_dir, un certificato per sito
    bool        https;
    int         https_port;   // 0 = libera scelta dal sistema (vedi sv_server_https_port)
    const char* cert_dir;     // cartella della CA (default "certs")
    bool        http2;        // HTTP/2 sulle connessioni HTTPS (ALPN "h2")
    const char* self_exe;     // eseguibile di SegVault (sendmail_path di PHP: "EXE --sendmail PORTA");
                              // NULL = cercato da solo
} SvServerConfig;

typedef struct SvServer SvServer;

// statistiche (per il pannello della GUI e GET /api/v1/status)
typedef struct {
    long long requests;       // richieste servite
    long long errors;         // risposte 4xx / 5xx
    long long rows_sent;      // righe restituite dalle query
    int       connections;    // connessioni aperte adesso
    double    uptime_s;
    bool      busy;           // una richiesta sta usando il motore
} SvStats;

// una richiesta servita (registro delle ultime SV_LOG_MAX)
#define SV_LOG_MAX 200
typedef struct {
    long long seq;            // numero progressivo (1 = la prima)
    char      when[9];        // "HH:MM:SS"
    char      method[8];
    char      path[96];
    char      client[48];     // indirizzo del client
    char      detail[120];    // primo statement SQL o messaggio d'errore
    char      user[64];       // utente del login ("" = token o nessun login)
    char      svc[8];         // servizio: "web", "php", "api", "proxy", "mysql" (una query), "mail" (un messaggio)
    int       status;
    double    ms;
} SvLogEntry;

// attività di un secondo (pagina Monitor): gli ultimi SV_SERIES_MAX secondi
#define SV_SERIES_MAX 300
typedef struct {
    long long sec;            // secondi dall'avvio del server
    int       requests, errors;
    double    total_ms, max_ms;
    long long rows;
} SvBucket;

// apre il socket in ascolto (il motore deve essere già inizializzato con db_init)
SvServer* sv_server_start(const SvServerConfig* cfg, char* err, size_t errsz);
int       sv_server_port(const SvServer* s);
int       sv_server_mysql_port(const SvServer* s);   // 0 se il protocollo MySQL è spento
int       sv_server_mail_port(const SvServer* s);    // 0 se la cattura delle email è spenta
int       sv_server_mail_count(SvServer* s);         // messaggi catturati
int       sv_server_https_port(const SvServer* s);   // 0 se HTTPS è spento
// servizi accesi e spenti con il server in funzione, senza toccare gli altri (porte e cartelle quelle
// della configurazione). Spegnere chiude anche le connessioni di quel servizio; con PHP le richieste in
// corso finiscono prima che i processi php-cgi si fermino.
typedef enum { SV_SVC_PHP = 1, SV_SVC_MYSQL, SV_SVC_MAIL, SV_SVC_HTTPS } SvService;
bool      sv_server_service(SvServer* s, SvService svc, bool on, char* err, size_t errsz);
bool      sv_server_service_on(const SvServer* s, SvService svc);
const char* sv_server_tls_info(const SvServer* s);   // libreria TLS e CA ("" se spento)
const char* sv_server_php(const SvServer* s);
const char* sv_server_api_prefix(const SvServer* s);  // "/api/v1", "" se l'API HTTP è spenta         // descrizione del backend PHP ("" se spento)
void      sv_server_run(SvServer* s);     // accetta connessioni finché non viene chiamato sv_server_stop
void      sv_server_stop(SvServer* s);    // chiamabile da un altro thread o da un gestore di segnale
void      sv_server_free(SvServer* s);    // chiude le connessioni rimaste e libera tutto

// lettura thread-safe (copie): statistiche e ultime richieste, dalla più recente
void      sv_server_stats(SvServer* s, SvStats* out);
int       sv_server_recent(SvServer* s, SvLogEntry* out, int max);
// gli ultimi n secondi fino ad adesso, dal più vecchio (i secondi senza richieste valgono zero)
int       sv_server_series(SvServer* s, SvBucket* out, int n);

// porte della configurazione già occupate (web, MySQL, posta, HTTPS), tutte insieme, con il programma che
// le usa quando si riesce a saperlo: "port 3306 (MySQL) is used by mysqld (pid 812)". 0 = tutte libere
int       sv_ports_check(const SvServerConfig* cfg, char* msg, size_t msgsz);
// chi è in ascolto su una porta TCP ("mysqld (pid 812)"); false se non si sa
bool      sv_port_owner(int port, char* out, size_t outsz);

// token casuale (hex, 2*bytes caratteri + NUL) dalla sorgente casuale del sistema
bool      sv_random_token(char* out, int bytes);

// indirizzo IPv4 di questa macchina sulla rete locale (per mostrare l'URL raggiungibile da altri)
bool      sv_local_ip(char* out, size_t n);

#endif
