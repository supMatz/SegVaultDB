/*
SCOPO: HTTPS per il server di sviluppo, senza dipendenze di compilazione.
       - OpenSSL caricato all'avvio (libssl / libcrypto 3 o 4: su Linux e macOS sono già nel
         sistema, su Windows le DLL che PHP per Windows ha nella sua cartella);
       - un'autorità di certificazione locale (DATA/certs/ca.crt, come mkcert) creata al primo avvio:
         installata una volta tra quelle fidate (--trust-ca), i siti https://NOME.localhost
         si aprono senza avvisi;
       - un certificato per ogni nome richiesto (SNI), firmato al volo dalla CA;
       - ogni connessione TLS viene decifrata e passata al server HTTP interno, con una prima riga
         segreta che porta l'indirizzo del client e il fatto che è HTTPS (vedi tls_marker).
*/
#ifndef SV_TLS_H
#define SV_TLS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
  #include <winsock2.h>
  typedef SOCKET tls_sock;
#else
  typedef int tls_sock;
#endif

typedef struct TlsCtx TlsCtx;

// carica OpenSSL e la CA (creata in cert_dir se manca). NULL + err se non si può
TlsCtx* tls_create(const char* cert_dir, char* err, size_t errsz);
void    tls_free(TlsCtx* t);
const char* tls_ca_path(const TlsCtx* t);     // percorso di ca.crt
const char* tls_library(const TlsCtx* t);     // "OpenSSL 3.x ..." (per il log)

// una connessione dal client: handshake, poi i byte decifrati vanno e vengono da 127.0.0.1:inner_port.
// La prima riga mandata al server interno è "SVTLS <secret> <ip> <porta>\r\n".
// h2: se il client propone HTTP/2 (ALPN "h2"), lo serve con h2_serve (vedi h2.h) invece del ponte
void tls_serve(TlsCtx* t, tls_sock fd, const char* client_ip, int client_port, int inner_port,
               const char* secret, atomic_int* stop);

// TLS su una connessione già aperta (protocollo MySQL dopo la richiesta SSL del client), socket bloccante
typedef struct TlsConn TlsConn;
TlsConn* tls_accept(TlsCtx* t, tls_sock fd);          // NULL se l'handshake fallisce
int      tls_read(TlsConn* c, char* buf, int n);      // >0 byte, 0 chiusa, <0 errore
bool     tls_write(TlsConn* c, const char* p, size_t n);
void     tls_close(TlsConn* c);
const char* tls_cipher(TlsConn* c);                   // "TLS_AES_256_GCM_SHA384"
const char* tls_version(TlsConn* c);                  // "TLSv1.3"

// installa la CA tra quelle fidate da questo utente (Windows: archivio "Root" dell'utente; macOS:
// portachiavi login; Linux: archivio NSS di Chrome / Firefox con certutil e, se si è root, quello del
// sistema). Scrive su out cosa ha fatto. 0 = fatto
int tls_trust_ca(const char* cert_dir, char* out, size_t outsz);

#endif
