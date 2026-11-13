/*
SCOPO: client FastCGI per eseguire PHP (come Apache + mod_fcgid in XAMPP o nginx + php-fpm).
       Due modi:
       - gruppo di processi php-cgi avviati e sorvegliati da SegVault (`php-cgi -b 127.0.0.1:PORTA`,
         uno per richiesta contemporanea; un processo che esce viene riavviato);
       - un backend esterno già in ascolto (php-fpm): "host:porta" oppure "unix:/percorso.sock".
       Nessuna dipendenza esterna. Specifica: https://fastcgi-archives.github.io/FastCGI_Specification.html
*/
#ifndef SV_FASTCGI_H
#define SV_FASTCGI_H

#include <stdbool.h>
#include <stddef.h>
#include "json.h"

typedef struct FcgiPool FcgiPool;

// una variabile CGI (nome / valore) passata a PHP
typedef struct {
    const char* name;
    const char* value;
} FcgiParam;

// avvia `workers` processi php-cgi (0 = 4); NULL + messaggio in err se l'eseguibile non parte
// ini: impostazioni "chiave=valore" passate a ogni processo con -d (n_ini voci, anche 0)
FcgiPool* fcgi_pool_spawn(const char* php_cgi, int workers, const char* const* ini, int n_ini, char* err, size_t errsz);
// backend esterno: "host:porta" (TCP) o "unix:/run/php/php-fpm.sock" (solo Linux)
FcgiPool* fcgi_pool_external(const char* addr, char* err, size_t errsz);
void      fcgi_pool_free(FcgiPool* p);     // ferma i processi avviati
const char* fcgi_pool_describe(const FcgiPool* p);   // "php-cgi ×4 (/usr/bin/php-cgi)" o l'indirizzo

// esegue una richiesta: `out` riceve l'uscita di PHP così com'è (intestazioni CGI + corpo),
// `errout` quello che PHP scrive su stderr (avvisi, errori). timeout_ms = 0: nessun limite.
// Ritorna 0 se riuscita, altrimenti un codice HTTP: 502 (PHP non risponde), 503 (tutti occupati),
// 504 (tempo scaduto); err spiega il motivo.
// on_output (facoltativo): chiamata dopo ogni pezzo d'uscita con tutto quello che è in `out`; può
// consumarlo (inviarlo al client e svuotare out->len) per le risposte in streaming. false = il client
// non c'è più: la richiesta si interrompe (ritorna 499)
typedef bool (*FcgiOnOutput)(void* ctx, JsonBuf* out);
int fcgi_request(FcgiPool* p, const FcgiParam* params, int nparams, const char* body, size_t body_len,
                 int timeout_ms, size_t max_out, JsonBuf* out, JsonBuf* errout, char* err, size_t errsz,
                 FcgiOnOutput on_output, void* ctx);

// cerca php-cgi: cartella php/ accanto all'eseguibile o in quella corrente, poi il PATH.
// false se non c'è.
bool fcgi_find_php_cgi(char* out, size_t outsz);

// PHP senza configurazione: se accanto a php-cgi non c'è un php.ini (lo zip di PHP per Windows non lo
// ha) ma c'è la cartella ext/, impostazioni "-d" che attivano le estensioni presenti (mysqli,
// pdo_mysql, openssl, mbstring, curl, gd, intl, zip, ...) e valori da sviluppo (errori visibili,
// upload grandi). Scrive fino a max voci in out (ognuna < 1400 byte); ritorna quante
int  fcgi_zero_config(const char* php_cgi, char out[][1400], int max);

// php.ini di quel php-cgi: il percorso (accanto all'eseguibile); se manca e create, lo crea da
// php.ini-development (o da zero) con le estensioni presenti già attive. false se non si può
bool fcgi_php_ini(const char* php_cgi, bool create, char* path, size_t pathsz);

#endif
