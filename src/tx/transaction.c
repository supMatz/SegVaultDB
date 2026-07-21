#include "transaction.h"
#include <string.h>

TxManager* txm_create(void) {
    TxManager* txm = calloc(1, sizeof(TxManager));
    txm->autocommit = true;
    txm->next_tx_id = 1;
    return txm;
}

Transaction* txm_begin(TxManager* txm) {
    if (!txm) return NULL;
    if (txm->num_transactions >= SV_MAX_TRANSACTIONS) return NULL;

    Transaction* t = &txm->transactions[txm->num_transactions++];
    memset(t, 0, sizeof(Transaction));
    t->tx_id = txm->next_tx_id++;
    t->active = true;
    return t;
}

int txm_commit(TxManager* txm, uint64_t tx_id) {
    if (!txm) return SV_ERR;
    for (int i = 0; i < txm->num_transactions; i++) {
        if (txm->transactions[i].tx_id == tx_id) {
            txm->transactions[i].active = false;
            return SV_OK;
        }
    }
    return SV_NOT_FOUND;
}

int txm_abort(TxManager* txm, uint64_t tx_id) {
    if (!txm) return SV_ERR;
    for (int i = 0; i < txm->num_transactions; i++) {
        if (txm->transactions[i].tx_id == tx_id) {
            txm->transactions[i].active = false;
            return SV_OK;
        }
    }
    return SV_NOT_FOUND;
}

void txm_destroy(TxManager* txm) {
    free(txm);
}
