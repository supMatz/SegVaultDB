/*
SCOPO: protocollo di rete di MySQL (client/server protocol 4.1) per segvault-server --mysql-port:
       le app che usano un driver MySQL (PHP mysqli / PDO, Python, Java JDBC, il client mysql)
       si collegano a SegVault senza modifiche.
       Qui solo la codifica dei pacchetti, senza socket né motore: handshake, login
       mysql_native_password, OK / ERR / EOF, risultati in formato testo.
       Il collegamento con il motore è in server.c (mysql_conn).
*/
#ifndef SV_MYSQL_WIRE_H
#define SV_MYSQL_WIRE_H

#include "../bridge/db_api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// capacità del client / server
#define MW_CLIENT_LONG_PASSWORD     0x00000001u
#define MW_CLIENT_FOUND_ROWS        0x00000002u
#define MW_CLIENT_LONG_FLAG         0x00000004u
#define MW_CLIENT_CONNECT_WITH_DB   0x00000008u
#define MW_CLIENT_PROTOCOL_41       0x00000200u
#define MW_CLIENT_SSL               0x00000800u
#define MW_CLIENT_TRANSACTIONS      0x00002000u
#define MW_CLIENT_SECURE_CONNECTION 0x00008000u
#define MW_CLIENT_MULTI_STATEMENTS  0x00010000u
#define MW_CLIENT_MULTI_RESULTS     0x00020000u
#define MW_CLIENT_PLUGIN_AUTH       0x00080000u
#define MW_CLIENT_CONNECT_ATTRS     0x00100000u
#define MW_CLIENT_PLUGIN_AUTH_LENENC 0x00200000u
#define MW_CLIENT_DEPRECATE_EOF     0x01000000u

#define MW_SERVER_CAPS (MW_CLIENT_LONG_PASSWORD | MW_CLIENT_FOUND_ROWS | MW_CLIENT_LONG_FLAG | MW_CLIENT_CONNECT_WITH_DB | \
                        MW_CLIENT_PROTOCOL_41 | MW_CLIENT_TRANSACTIONS | MW_CLIENT_SECURE_CONNECTION | \
                        MW_CLIENT_MULTI_STATEMENTS | MW_CLIENT_MULTI_RESULTS | MW_CLIENT_PLUGIN_AUTH | \
                        MW_CLIENT_CONNECT_ATTRS | MW_CLIENT_PLUGIN_AUTH_LENENC)

// stato del server nei pacchetti OK / EOF
#define MW_STATUS_IN_TRANS          0x0001
#define MW_STATUS_AUTOCOMMIT        0x0002
#define MW_STATUS_MORE_RESULTS      0x0008

// comandi
#define MW_COM_QUIT        0x01
#define MW_COM_INIT_DB     0x02
#define MW_COM_QUERY       0x03
#define MW_COM_FIELD_LIST  0x04
#define MW_COM_PING        0x0e
#define MW_COM_STMT_PREPARE 0x16
#define MW_COM_RESET_CONNECTION 0x1f

// pacchetti in uscita: header (3 byte di lunghezza + numero di sequenza) e contenuto
typedef struct {
    uint8_t* p;
    size_t   n, cap;
    bool     oom;
    uint8_t  seq;        // sequenza del prossimo pacchetto
    size_t   start;      // inizio del pacchetto in costruzione
} MwOut;

void mw_begin(MwOut* o);                                  // nuovo pacchetto
void mw_end(MwOut* o);                                    // chiude il pacchetto (scrive la lunghezza)
void mw_u8(MwOut* o, unsigned v);
void mw_u16(MwOut* o, unsigned v);
void mw_u32(MwOut* o, uint32_t v);
void mw_bytes(MwOut* o, const void* d, size_t n);
void mw_lenenc(MwOut* o, uint64_t v);
void mw_lenstr(MwOut* o, const char* s, size_t n);

void mw_handshake(MwOut* o, uint32_t conn_id, const uint8_t scramble[20]);
void mw_ok(MwOut* o, uint64_t affected, uint64_t insert_id, unsigned status, const char* info);
void mw_err(MwOut* o, unsigned code, const char* sqlstate, const char* msg);
void mw_eof(MwOut* o, unsigned status);
void mw_auth_switch(MwOut* o, const uint8_t scramble[20]);
// risultato in formato testo: colonne, EOF, righe, EOF (status: con MORE_RESULTS se ne seguono altri)
void mw_resultset(MwOut* o, const QueryResult* r, const char* db, unsigned status);

// risposta al login (HandshakeResponse41)
typedef struct {
    uint32_t caps;
    char     user[64];
    uint8_t  auth[64];
    size_t   auth_len;
    char     db[64];
    char     plugin[64];
} MwLogin;
bool mw_parse_login(const uint8_t* p, size_t n, MwLogin* out);

// istruzioni che i driver mandano all'avvio e che SegVault non ha (SET NAMES, SELECT @@version,
// SHOW VARIABLES, SHOW WARNINGS, ...): se la riconosce scrive la risposta e restituisce true
bool mw_client_noise(MwOut* o, const char* sql, unsigned status);

#endif
