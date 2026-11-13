/*
SCOPO: cattura delle email di sviluppo (come Mailpit / MailHog): un server SMTP locale accetta ogni
       messaggio senza consegnarlo, lo salva come .eml e lo mostra in una pagina web
       (http://localhost:PORTA/.segvault/mail) con un'API JSON per i test automatici.
       Le app lo usano come un server SMTP qualsiasi (Laravel MAIL_PORT, Symfony MAILER_DSN,
       Nodemailer, Django EMAIL_PORT); mail() di PHP ci arriva da solo (vedi mail_sendmail_main).
       Nessuna dipendenza esterna.
*/
#ifndef SV_MAIL_H
#define SV_MAIL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "json.h"

#ifdef _WIN32
  #include <winsock2.h>
  typedef SOCKET ml_sock;
#else
  typedef int ml_sock;
#endif

typedef struct MailStore MailStore;

// apre (o crea) la cartella dei messaggi e ne legge l'elenco; NULL + err se non si può
MailStore* mail_store_open(const char* dir, char* err, size_t errsz);
void       mail_store_free(MailStore* m);
int        mail_count(MailStore* m);

// una connessione SMTP (dal suo thread): risponde finché il client non chiude o stop
void mail_smtp_serve(MailStore* m, ml_sock fd, const char* client, atomic_int* stop);

// pagine e API sotto /.segvault/mail (sub = quello che segue, "" o "/..."): 0 se il percorso non è
// della posta, altrimenti lo stato HTTP; *ctype e *extra_headers puntano a stringhe costanti
int mail_web(MailStore* m, const char* method, const char* sub, const char* query, JsonBuf* out,
             const char** ctype, const char** extra_headers);

// modalità "sendmail" (sendmail_path di PHP su Linux / macOS): legge il messaggio da stdin e lo
// consegna al server SMTP di SegVault su 127.0.0.1:port. argv: gli argomenti dopo la porta
// (-f mittente, -t, -i: come sendmail). Ritorna il codice d'uscita del processo
int mail_sendmail_main(int port, int argc, char** argv);

#endif
