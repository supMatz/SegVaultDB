/*
SCOPO: programmi CGI classici (Perl, Python, Ruby, shell, eseguibili .cgi), come mod_cgi di Apache in XAMPP.
       L'interprete viene dalla prima riga "#!" (anche su Windows: "#!/usr/bin/perl" -> perl.exe del PATH,
       "#!/usr/bin/env python3" -> python3 / python / py) o dall'estensione (.pl perl, .py python, .rb ruby).
       Il corpo della richiesta arriva su stdin; stdout è la risposta CGI (intestazioni, riga vuota, corpo);
       stderr finisce nel registro. Il programma parte nella cartella dello script.
*/
#ifndef SV_CGI_H
#define SV_CGI_H

#include <stdbool.h>
#include <stddef.h>
#include "json.h"

// estensione di un programma CGI (".cgi", ".pl", ".py", ".rb")?
bool cgi_is_script(const char* path, size_t len);

// esegue script (percorso sul disco) con l'ambiente env ("NOME=valore", terminato da NULL).
// 0 = finito (out / errout pieni); altrimenti un codice HTTP: 502 (non parte), 504 (tempo scaduto)
int  cgi_exec(const char* script, const char* const* env, const char* body, size_t body_len, int timeout_ms,
              size_t max_out, JsonBuf* out, JsonBuf* errout, char* err, size_t errsz);

#endif
