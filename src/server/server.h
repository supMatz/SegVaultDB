/*
SCOPO: server HTTP/JSON di SegVault: espone il motore (db_api.h) a siti web,
       app desktop e qualsiasi linguaggio con un client HTTP.

       Il motore ha un solo stato di sessione (database corrente, transazione aperta):
       le richieste vengono eseguite una alla volta e ognuna è un'unità chiusa
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
    bool        quiet;        // niente log delle richieste su stderr
} SvServerConfig;

typedef struct SvServer SvServer;

// apre il socket in ascolto (il motore deve essere già inizializzato con db_init)
SvServer* sv_server_start(const SvServerConfig* cfg, char* err, size_t errsz);
int       sv_server_port(const SvServer* s);
void      sv_server_run(SvServer* s);     // accetta connessioni finché non viene chiamato sv_server_stop
void      sv_server_stop(SvServer* s);    // chiamabile da un altro thread o da un gestore di segnale
void      sv_server_free(SvServer* s);    // chiude le connessioni rimaste e libera tutto

#endif
