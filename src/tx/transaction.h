#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "../../include/common.h"

#define SV_MAX_TRANSACTIONS 64

typedef struct {
    uint64_t tx_id;
    bool     active;
    uint8_t  isolation;
} Transaction;

typedef struct {
    Transaction transactions[SV_MAX_TRANSACTIONS];
    int         num_transactions;
    uint64_t    next_tx_id;
    bool        autocommit;
} TxManager;

TxManager*    txm_create(void);
Transaction*  txm_begin(TxManager* txm);
int           txm_commit(TxManager* txm, uint64_t tx_id);
int           txm_abort(TxManager* txm, uint64_t tx_id);
void          txm_destroy(TxManager* txm);

#endif
