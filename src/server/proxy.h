/*
SCOPO: reverse proxy HTTP/1.1 verso un dev server locale (Vite, Next.js, Node/Express, Django,
       Flask, Rails, Go, ...): un sito "nome.localhost" può essere servito da un'altra porta.
       Le risposte passano in streaming (Server-Sent Events, download), le connessioni WebSocket
       (ricarica a caldo di Vite / Next / webpack) vengono collegate nei due sensi.
       Nessuna dipendenza esterna.
*/
#ifndef SV_PROXY_H
#define SV_PROXY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #include <winsock2.h>
  typedef SOCKET px_sock;
#else
  typedef int px_sock;
#endif

typedef struct {
    int       status;          // stato della risposta del backend (101 = WebSocket collegato)
    long long bytes;           // byte del corpo inoltrati al client
    bool      keep_alive;      // la connessione del client può servire un'altra richiesta
} PxResult;

// "http://host:porta" | "host:porta" | "porta" -> host e porta (false se non valido)
bool px_parse_target(const char* target, char* host, size_t hn, int* port);

// Inoltra una richiesta già pronta (head: riga di richiesta + intestazioni + riga vuota, con
// "Connection: close" o "Connection: Upgrade"; body: il corpo; early: byte che il client ha già
// mandato dopo la richiesta, inoltrati dopo il corpo, utili con un Upgrade) e copia la risposta
// sul socket del client mentre arriva.
// Ritorna 0 se la risposta (anche parziale) è stata inoltrata; -1 se il backend non risponde e al
// client non è stato mandato niente (err spiega: chi chiama manda un 502).
// client_wants_keep: il client ha chiesto keep-alive. head_request: richiesta HEAD (corpo assente).
int px_forward(px_sock client, const char* host, int port, const char* head, size_t head_len,
               const char* body, size_t body_len, const char* early, size_t early_len,
               bool head_request, bool client_wants_keep, atomic_int* stop, PxResult* res,
               char* err, size_t errsz);

#endif
