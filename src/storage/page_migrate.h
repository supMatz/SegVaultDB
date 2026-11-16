/*
SCOPO: conversione del file dei dati dal formato 1 (SegVault fino alla 1.3: intestazione di pagina di 12 byte,
       nessun LSN né CRC, pagina 0 vuota) al formato 2 (intestazione di 24 byte con LSN e CRC-32C, pagina 0 =
       intestazione del file). Le righe restano negli stessi slot delle stesse pagine (gli indici e il log le
       ritrovano); le catene di overflow vengono riscritte con il passo nuovo (4070 byte per pagina) e lo stub
       aggiornato al suo posto. Prima di toccare il file se ne fa una copia (segvault.db.v1): una conversione
       interrotta si riprende dalla copia al prossimo avvio; finita, la copia si cancella.
*/
#ifndef SV_PAGE_MIGRATE_H
#define SV_PAGE_MIGRATE_H

#include <stdbool.h>
#include <stddef.h>

// fd = file dei dati aperto (lettura e scrittura), path = il suo percorso. log_present: c'è un log da
// recuperare (allora nessuna riga può cambiare posizione: se una pagina è piena il formato non si converte
// e serve la 1.3 per il recupero). SV_OK (anche: niente da fare), altrimenti errore con il motivo in err
int page_migrate(int fd, const char* path, bool log_present, char* err, size_t errn);

#endif
