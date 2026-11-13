/*
SCOPO: i file .htaccess dei siti, come li legge Apache (XAMPP, UniServerZ, Laragon): un progetto
       preso da lì funziona senza modifiche.
       - mod_rewrite: RewriteEngine, RewriteBase, RewriteCond (variabili %{...}, -f -d -s -l, regex,
         !, =, <, >, [NC,OR]), RewriteRule (pattern, sostituzione con $N e %N, flag L END QSA QSD NC
         R[=codice] F G NE PT C S=n), con il contesto per cartella di Apache;
       - accesso: "Require all denied" / "Deny from all" (anche dentro <Files> / <FilesMatch>);
       - Options -Indexes.
       Ogni richiesta rilegge i .htaccess delle cartelle del percorso (come Apache): una modifica
       vale subito.
*/
#ifndef SV_REWRITE_H
#define SV_REWRITE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char* root;           // cartella del sito
    const char* path;           // percorso decodificato ("/blog/2026/ciao")
    const char* query;          // QUERY_STRING
    const char* method;
    const char* host;           // intestazione Host
    const char* headers;        // righe "Nome: valore\r\n" della richiesta (per %{HTTP:...})
    const char* client;         // REMOTE_ADDR
    const char* the_request;    // "GET /x?y HTTP/1.1"
    int         port;
    bool        https;
} RwRequest;

typedef enum { RW_NONE, RW_REWRITE, RW_REDIRECT, RW_FORBIDDEN, RW_GONE } RwAction;

typedef struct {
    RwAction action;
    char     path[2048];        // RW_REWRITE: nuovo percorso interno (decodificato)
    char     query[2048];       // RW_REWRITE: nuova QUERY_STRING
    int      status;            // RW_REDIRECT: 301, 302, ...
    char     location[4096];    // RW_REDIRECT
    bool     no_listing;        // Options -Indexes
    char     error[200];        // un problema nel .htaccess (per il registro), "" se nessuno
} RwResult;

void rw_apply(const RwRequest* rq, RwResult* out);

#endif
