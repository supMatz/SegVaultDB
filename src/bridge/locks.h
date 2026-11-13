/*
SCOPO: lock delle tabelle per le transazioni concorrenti (locking a due fasi stretto).
       Ogni proprietario (una sessione) prende lock S (lettura) o X (scrittura) sulle tabelle che
       uno statement tocca; in una transazione li tiene fino a COMMIT / ROLLBACK. Più transazioni
       restano aperte insieme finché toccano tabelle diverse o solo leggono le stesse; chi trova una
       tabella occupata aspetta (vedi db_api.c), e un ciclo di attese è un deadlock.
       Il gestore è indipendente dal lock del motore: le attese avvengono senza tenerlo.
*/
#ifndef SV_LOCKS_H
#define SV_LOCKS_H

#include <stdbool.h>
#include <stdint.h>

#define LK_S 1
#define LK_X 2

typedef struct {
    int     db;            // indice del database nel catalogo
    char    table[64];     // nome in minuscolo
    uint8_t mode;          // LK_S / LK_X
} LkReq;

enum { LK_GRANTED = 0, LK_WAIT = 1, LK_DEADLOCK = 2 };

void lk_init(void);
// tutti i lock richiesti o nessuno. LK_WAIT: occupati da altri (registra l'attesa per il rilevamento
// dei deadlock); LK_DEADLOCK: aspettare chiuderebbe un ciclo
int  lk_acquire(const void* owner, const LkReq* req, int n);
// come lk_acquire ma senza registrare attese (letture in parallelo): true se presi
bool lk_try(const void* owner, const LkReq* req, int n);
void lk_cancel_wait(const void* owner);   // non aspetta più
void lk_release_all(const void* owner);
int  lk_count(const void* owner);         // lock tenuti (diagnostica, test)
// proprietario che tiene un lock in conflitto con req (per i messaggi), NULL se nessuno
const void* lk_blocker(const void* owner, const LkReq* req, int n);
// chi (diverso da except) tiene la tabella in modo esclusivo, NULL se nessuno
const void* lk_x_holder(int db, const char* table, const void* except);

#endif
