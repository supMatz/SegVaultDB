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

bool auth_init(const char* data_dir);     // legge users.dat (se c'è); false = file danneggiato (non toccato)
const char* auth_init_error(void);
void auth_shutdown(void);

// istruzione di gestione degli utenti? (tokens = TokenList del solo statement). Se sì la esegue,
// mette il risultato in *out (QueryResult* di db_api.h, malloc'd) e ritorna true
bool auth_statement(const void* tokens, const char* session_user, const char* cur_db, void** out);

// lo statement è permesso all'utente? (cur_db = database corrente, "" se nessuno)
bool auth_check(const char* user, const struct ASTNode* stmt, const char* cur_db, char* err, size_t errn);

// hash delle password: "$p2$<giri>$<hex>" (PBKDF2-HMAC-SHA256); vedi auth.c
#define AUTH_HASH_MAX    96
#define AUTH_PBKDF2_ITER 100000
bool     auth_login(const char* user, const char* password);
bool     auth_user_exists(const char* user);
bool     auth_is_admin(const char* user);
// login in due tempi: sale e hash copiati (sotto il lock del motore), poi l'hash lento calcolato fuori
bool     auth_user_secret(const char* user, char salt[33], char hash[AUTH_HASH_MAX]);
bool     auth_password_matches(const char* salt, const char* hash, const char* password);
bool     auth_hash_is_current(const char* hash);   // PBKDF2 con almeno AUTH_PBKDF2_ITER giri?
// dopo una verifica riuscita (sale e hash quelli copiati): ricorda SHA256(SHA256(pw)) per caching_sha2 e
// rifà un hash vecchio; true se l'hash è stato rifatto
bool     auth_password_verified(const char* user, const char* salt, const char* hash, const char* password);
void     auth_pbkdf2_sha256(const void* pw, size_t pl, const void* salt, size_t sl, unsigned iter, uint8_t out[32]);
// caching_sha2_password: SHA256(SHA256(pw)); verifica di una risposta veloce; login (1 / 0 / -2 = serve
// l'accesso completo con la password in chiaro)
void     auth_sha2_stage2(const char* password, uint8_t out[32]);
bool     auth_sha2_check(const uint8_t stage2[32], const uint8_t nonce[20], const uint8_t* resp, size_t rlen);
int      auth_login_sha2(const char* user, const uint8_t nonce[20], const uint8_t* resp, size_t rlen);
bool     auth_has_native(const char* user);          // ha il verificatore mysql_native_password?
unsigned long auth_users_version(void);   // cambia a ogni CREATE / ALTER / DROP USER, GRANT, REVOKE
// nessun utente non amministratore ha uno dei permessi privs su db.table?
bool     auth_only_admins(const char* db, const char* table, unsigned privs);
// tabella degli endpoint salvati dell'API HTTP (vedi server.c)
#define AUTH_ENDPOINTS_TABLE "api_endpoints"
unsigned auth_privs(const char* user, const char* db, const char* table);   // permessi effettivi
void auth_sha1(const void* data, size_t n, uint8_t out[20]);
// login col protocollo MySQL (mysql_native_password): 1 = giusto, 0 = sbagliato,
// -1 = utente creato prima del supporto MySQL (va reimpostata la password con ALTER USER)
int  auth_login_native(const char* user, const uint8_t scramble[20], const uint8_t* resp, size_t rlen);
bool auth_can_see_db(const char* user, const char* db);              // qualche permesso nel database?
int      auth_user_count(void);
const char* auth_user_name(int i);
// permessi dati esattamente su db.table (senza ereditare da *.* o db.*)
unsigned auth_grant_on(const char* user, const char* db, const char* table);
int      auth_grant_count(const char* user);
bool     auth_grant_at(const char* user, int i, char* db, char* table, unsigned* privs);   // db/table 64 byte

void auth_sha256(const void* data, size_t n, uint8_t out[32]);
const char* auth_priv_name(unsigned bit);

#endif
