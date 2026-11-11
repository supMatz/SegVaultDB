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
void      sv_server_run(SvServer* s);     // accetta connessioni finché non viene chiamato sv_server_stop
void      sv_server_stop(SvServer* s);    // chiamabile da un altro thread o da un gestore di segnale
void      sv_server_free(SvServer* s);    // chiude le connessioni rimaste e libera tutto

// lettura thread-safe (copie): statistiche e ultime richieste, dalla più recente
void      sv_server_stats(SvServer* s, SvStats* out);
int       sv_server_recent(SvServer* s, SvLogEntry* out, int max);
// gli ultimi n secondi fino ad adesso, dal più vecchio (i secondi senza richieste valgono zero)
int       sv_server_series(SvServer* s, SvBucket* out, int n);

// token casuale (hex, 2*bytes caratteri + NUL) dalla sorgente casuale del sistema
bool      sv_random_token(char* out, int bytes);

// indirizzo IPv4 di questa macchina sulla rete locale (per mostrare l'URL raggiungibile da altri)
bool      sv_local_ip(char* out, size_t n);

#endif
