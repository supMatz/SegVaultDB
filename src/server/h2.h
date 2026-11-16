/*
SCOPO: HTTP/2 (RFC 9113) sulle connessioni HTTPS: il browser apre una sola connessione e ci fa
       passare tutte le richieste insieme (niente limite di 6 connessioni per sito: utile con Vite,
       che in sviluppo carica centinaia di moduli). Ogni stream diventa una richiesta HTTP/1.1 al
       server interno (come nginx con http2 davanti a un backend), su connessioni locali riusate.
       Intestazioni con HPACK (RFC 7541, Huffman compreso), controllo di flusso, priorità ignorate.
*/
#ifndef SV_H2_H
#define SV_H2_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "tls.h"

// lato client cifrato (OpenSSL), con il socket non bloccante
typedef struct {
    void*    ssl;
    tls_sock fd;
    int  (*recv)(void* ssl, char* buf, int n);                         // >0 byte, 0 = niente ora, -1 = chiusa
    bool (*send)(void* ssl, tls_sock fd, const char* p, size_t n);     // tutto o false
    int  (*pending)(void* ssl);                                         // byte già decifrati
} H2Io;

bool h2_enabled(void);
void h2_set_enabled(bool on);

// serve la connessione HTTP/2 finché il client non chiude (o stop); marker: la prima riga per ogni
// connessione al server interno (vedi tls.h)
void h2_serve(H2Io* io, int inner_port, const char* marker, atomic_int* stop);

// ── connessioni al server interno ──
// Il server HTTP dello stesso processo registra per la sua porta una funzione che crea la connessione
// in memoria (socketpair: niente accept, niente porta TCP, niente slot di connessione); one_request =
// uno stream HTTP/2 (una richiesta, servita dal pool fisso di thread), altrimenti il ponte TLS HTTP/1.1.
// Senza registrazione (o se fallisce) si apre una connessione TCP a 127.0.0.1:port come prima.
typedef tls_sock (*SvInnerConnect)(void* ud, bool one_request);
void     sv_inner_register(int port, SvInnerConnect fn, void* ud);
void     sv_inner_unregister(int port);
tls_sock sv_inner_connect(int port, bool one_request);   // socket bloccante, BAD_SOCK se non riesce
// attesa su più socket (poll; WSAPoll su Windows): niente limite di FD_SETSIZE. ev: 1 = lettura, 2 = scrittura
typedef struct { tls_sock fd; short want; short got; } SvPoll;
int      sv_poll(SvPoll* p, int n, int timeout_ms);

#endif
