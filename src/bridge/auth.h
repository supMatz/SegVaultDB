/*
SCOPO: utenti e permessi del motore (come gli account di MySQL).
       - utenti con password (SHA-256 con sale, iterato), salvati in users.dat nella cartella dei dati
       - permessi SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, ALTER su *.* | db.* | db.tabella;
         WITH GRANT OPTION = amministratore (gestisce utenti e permessi)
       - istruzioni CREATE / ALTER / DROP USER, GRANT, REVOKE, SHOW USERS, SHOW GRANTS
       - controllo di ogni statement prima dell'esecuzione (tabelle lette e scritte, anche nelle
         subquery, nei JOIN e nelle CTE)
       Senza utente di sessione (stringa vuota) si è "root": tutto è permesso. La GUI lavora così;
       il server imposta l'utente della richiesta quando è richiesto il login.
       Usato da db_api.c con il lock del motore già preso.
*/
#ifndef SV_AUTH_H
#define SV_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_SELECT 0x01u
#define AUTH_INSERT 0x02u
#define AUTH_UPDATE 0x04u
#define AUTH_DELETE 0x08u
#define AUTH_CREATE 0x10u
#define AUTH_DROP   0x20u
#define AUTH_ALTER  0x40u
#define AUTH_ALL    0x7Fu

struct ASTNode;

void auth_init(const char* data_dir);     // legge users.dat (se c'è)
void auth_shutdown(void);

// istruzione di gestione degli utenti? (tokens = TokenList del solo statement). Se sì la esegue,
// mette il risultato in *out (QueryResult* di db_api.h, malloc'd) e ritorna true
bool auth_statement(const void* tokens, const char* session_user, const char* cur_db, void** out);

// lo statement è permesso all'utente? (cur_db = database corrente, "" se nessuno)
bool auth_check(const char* user, const struct ASTNode* stmt, const char* cur_db, char* err, size_t errn);

bool     auth_login(const char* user, const char* password);
bool     auth_user_exists(const char* user);
bool     auth_is_admin(const char* user);
unsigned auth_privs(const char* user, const char* db, const char* table);   // permessi effettivi
int      auth_user_count(void);
const char* auth_user_name(int i);
// permessi dati esattamente su db.table (senza ereditare da *.* o db.*)
unsigned auth_grant_on(const char* user, const char* db, const char* table);
int      auth_grant_count(const char* user);
bool     auth_grant_at(const char* user, int i, char* db, char* table, unsigned* privs);   // db/table 64 byte

void auth_sha256(const void* data, size_t n, uint8_t out[32]);
const char* auth_priv_name(unsigned bit);

#endif
