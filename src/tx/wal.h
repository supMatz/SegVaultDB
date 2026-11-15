#ifndef WAL_H
#define WAL_H

#include "../../include/common.h"
#include "../table/tuple.h"

#define LOG_BEGIN       1
#define LOG_COMMIT      2
#define LOG_ABORT       3
#define LOG_INSERT      4
#define LOG_DELETE      5
#define LOG_UPDATE      6
#define LOG_SAVEPOINT   7
#define LOG_ROLLBACK_TO 8
// modifica di una riga che può cambiare posizione: un solo record con le due posizioni
//   intestazione: page_id / slot della riga vecchia
//   data:     [page_id nuovo: 4 byte][slot nuovo: 2 byte][riga nuova]
//   old_data: riga vecchia
#define LOG_UPDATE_MOVE 9
// record di compensazione (CLR): un passo dell'annullamento di un'altra voce, già deciso, da rifare
// uguale nel recupero (così ROLLBACK e recupero si possono interrompere e riprendere senza duplicare
// righe). intestazione: page_id / slot toccati; data: [offset della voce compensata: 8 byte, -1 =
// passo intermedio][azione: 1 byte, CLR_DELETE / CLR_REVIVE][riga (CLR_REVIVE)]. CLR_REVIVE rimette la
// riga nel suo slot (eliminato): il RID non cambia e lo spazio c'è sempre
#define LOG_CLR        10
#define CLR_DELETE      1
#define CLR_REVIVE      2
// pagina nuova di una tabella collegata alla catena: page_id = pagina nuova, data = [pagina
// precedente: 4 byte] (0 = prima pagina). Le pagine nuove non sono su disco finché non c'è un
// checkpoint: dopo una caduta di corrente il recupero le ricrea e le ricollega
#define LOG_LINK       11

typedef struct {
    uint32_t page_id;
    uint16_t slot_id;
} SVRID;

typedef struct {
    int      fd;
    char     path[512];
    uint64_t sequence;
    uint64_t checkpoints;   // checkpoint eseguiti in questa sessione
    uint8_t* buf;           // voci non ancora scritte nel file (una write per gruppo, non per riga)
    size_t   blen, bcap;
    bool     failed;        // una scrittura o un fsync del log non è riuscito: niente più scritture
    bool     unsynced;      // voci scritte (nel file o nel buffer) dopo l'ultimo fsync
} WAL;

WAL* wal_create(const char* path);
int  wal_write(WAL* wal, uint8_t type, uint64_t tx_id, uint32_t page_id,
               SVRID rid, const uint8_t* data, uint32_t data_len,
               const uint8_t* old_data, uint32_t old_data_len);
int  wal_flush(WAL* wal);            // scrive il buffer e fa fsync (COMMIT)
int  wal_write_buffer(WAL* wal);     // scrive il buffer senza fsync (fine statement, prima di una pagina)
// recupero dopo un arresto anomalo: rifà tutte le voci, annulla le transazioni senza COMMIT né ABORT,
// porta il file dei dati su disco e solo allora elimina il log. SV_OK, oppure un errore (il log resta:
// nessun dato perso, il prossimo avvio riprova); il motivo in err
int  wal_recovery(const char* log_path, const char* db_path, const char* cat_path, char* err, size_t errn);
// annulla le voci della transazione (dall'offset 'from' del log: ROLLBACK TO; 0 = tutte). Ogni passo
// è registrato come CLR in wal (con fsync) prima di scrivere le pagine, che poi vanno su disco (fsync):
// al ritorno con SV_OK l'annullamento è definitivo. Il log va scritto (wal_flush) prima di chiamarla.
// pages / npages (se non NULL): le pagine scritte dall'annullamento (da togliere dalla cache), malloc'd
int  wal_undo_tx(WAL* wal, const char* db_path, uint64_t tx_id);
int  wal_undo_tx_from(WAL* wal, const char* db_path, uint64_t tx_id, int64_t from, uint32_t** pages, int* npages);
// dimensione del log in byte
int64_t wal_size(WAL* wal);
// svuota il log (checkpoint: da chiamare solo con tutte le pagine già su disco e nessuna
// transazione aperta)
int  wal_truncate(WAL* wal);
void wal_destroy(WAL* wal);

#endif
