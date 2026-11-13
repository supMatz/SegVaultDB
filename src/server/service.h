/*
SCOPO: segvault-server come servizio dell'utente, avviato con il computer (senza diritti di amministratore):
       - Linux: unità systemd dell'utente (~/.config/systemd/user/segvault.service), abilitata e avviata;
       - macOS: LaunchAgent (~/Library/LaunchAgents/dev.segvault.server.plist);
       - Windows: valore "SegVault Server" nella chiave Run dell'utente, con --background (niente console,
         il registro va in DATA/segvault-server.log).
       La riga di comando è quella con cui si chiama --install-service, con i percorsi resi assoluti.
*/
#ifndef SV_SERVICE_H
#define SV_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

// argv: gli argomenti del server senza --install-service. out: cosa è stato fatto (o perché no). 0 = ok
int  sv_service_install(int argc, char** argv, char* out, size_t outsz);
int  sv_service_uninstall(char* out, size_t outsz);
// Windows: stacca la console e manda stdout / stderr in log_path (con --background). Altrove: nulla
void sv_service_background(const char* log_path);

#endif
