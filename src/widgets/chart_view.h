/*
SCOPO: grafico dei risultati della griglia (pulsante "Chart" sopra i risultati).
       Il modello sceglie da solo cosa disegnare: le etichette dalla prima colonna di testo
       (o dalla prima colonna numerica se sono tutte numeriche), i valori dalle colonne numeriche
       (fino a 4 serie). Barre per pochi valori, linee per molti; si può cambiare.
       Un solo asse delle Y (le serie devono avere la stessa unità), legenda con 2 o più serie,
       colori categorici in ordine fisso (palette validata per daltonismo), valore sotto il mouse.
*/
#ifndef SV_CHART_VIEW_H
#define SV_CHART_VIEW_H

#include "../bridge/db_api.h"
#include "../platform/platform.h"
#include <stdbool.h>
#include <stddef.h>

#define CHART_MAX_SERIES 4

typedef struct {
    int  label_col;                    // -1 = numero di riga
    int  series[CHART_MAX_SERIES];     // colonne dei valori
    int  nseries;
    bool line;                         // linee invece di barre
    bool line_auto;                    // scelta automatica (barre fino a 40 valori)
} ChartModel;

// colonna con tutti valori numerici (o NULL) nelle righe mostrate?
bool chart_numeric_col(const QueryResult* r, const int* rows, int nrows, int col);
// false + motivo se il risultato non ha niente da disegnare
bool chart_model_build(const QueryResult* r, const int* rows, int nrows, ChartModel* m, char* why, size_t wn);
// tacche "tonde" dell'asse: da *lo a *hi a passi di *step (circa target tacche)
void chart_ticks(double min, double max, int target, double* lo, double* hi, double* step);
// numero compatto per le tacche: 1.5k, 2M, 0.25
void chart_fmt(double v, double step, char* out, size_t n);

// disegna il grafico in area; mx, my = mouse (valore evidenziato); dark = tema scuro
void chart_draw(PlatformWindow* win, Rect area, const QueryResult* r, const int* rows, int nrows,
                const ChartModel* m, int mx, int my, bool dark);

#endif
