#ifndef WINDOW_H
#define WINDOW_H

/*
SCOPO: Layout principale dell'applicazione SegVault.
       gestisce la disposizione di tutti i widget:

  ┌──────────────────────────────────────────────────────────────┐
  │ ◆ SegVault │ [▶ Run F5] [Run statement] [✓ Commit] [↺ Roll.] │ ● db │
  ├────────────┬─────────────────────────────────────────────────┤
  │ EXPLORER ⟳ │  EDITOR SQL (textbox multilinea)                │
  │            ├──────────────────── splitter ───────────────────┤
  │  (tree)    │  RESULTS ● stato                     meta       │
  │            │  griglia risultati (table_view)                 │
  ├────────────┴─────────────────────────────────────────────────┤
  │ footer: versione · database · transazione      Ln, Col · ... │
  └──────────────────────────────────────────────────────────────┘
*/

#include "widget.h"
#include "button.h"
#include "label.h"
#include "textbox.h"
#include "line_edit.h"
#include "table_view.h"
#include "tree_view.h"
#include "scrollbar.h"
#include "panel.h"
#include "splitter.h"
#include "../bridge/db_api.h"

#define TOOLBAR_H         52   // Altezza toolbar in pixel
#define SIDEBAR_W        264   // Larghezza sidebar in pixel
#define SIDEBAR_HEADER_H  38   // Intestazione "EXPLORER"
#define STATUSBAR_H       38   // Intestazione dei risultati (stato ultima query)
#define FOOTER_H          26   // Barra di stato in basso
#define SPLITTER_H         7   // Altezza splitter tra editor e risultati
#define SCROLLBAR_W       10   // Spessore delle scrollbar

typedef enum { RUN_SCRIPT, RUN_CURRENT } RunMode;

typedef enum { STATUS_IDLE, STATUS_OK, STATUS_ERROR, STATUS_INFO } StatusKind;

// barra comandi sovrapposta all'editor
typedef enum { BAR_NONE, BAR_FIND, BAR_GOTO, BAR_OPEN, BAR_SAVE_AS, BAR_EXPORT } BarMode;

#define HISTORY_MAX 500

// una query eseguita (cronologia)
typedef struct {
    char*  sql;
    char   when[20];        // "YYYY-MM-DD HH:MM:SS"
    bool   ok;
    int    rows;            // righe restituite o modificate
    double ms;
} HistoryEntry;

typedef enum {
    BAR_BTN_TOGGLE_REPLACE, BAR_BTN_CASE, BAR_BTN_WORD, BAR_BTN_PREV, BAR_BTN_NEXT, BAR_BTN_CLOSE,
    BAR_BTN_REPLACE, BAR_BTN_REPLACE_ALL, BAR_BTN_COUNT
} BarButton;

typedef struct {
    PlatformWindow* win;        // Finestra OS sottostante

    // -- toolbar --
    Button* btn_run;            // esegui script / selezione (F5)
    Button* btn_run_current;    // esegui statement sotto il cursore (Ctrl+Enter)
    Button* btn_commit;         // COMMIT transazione
    Button* btn_rollback;       // ROLLBACK transazione
    Button* btn_format;         // formatta lo script (Ctrl+Shift+F)
    Label*  lbl_db_name;        // nome database corrente

    // -- sidebar --
    TreeView*  tree;            // albero DB/Tabelle/Colonne
    Scrollbar* tree_scroll;     // scrollbar della sidebar
    Button*    btn_refresh;     // ricarica l'albero

    // ── area centrale ────────────────────────────────────────────
    TextBox*   editor;          // editor SQL multilinea
    Scrollbar* editor_scroll;   // scrollbar dell'editor

    // -- status bar --
    Label* lbl_status;          // "3 rows | 12ms | OK"
    StatusKind status_kind;
    char       status_meta[128]; // testo a destra (righe, colonne, tempo)

    // -- risultati --
    TableView* results;          // griglia risultati
    Scrollbar* results_scroll_v; // scrollbar verticale risultati
    Scrollbar* results_scroll_h; // scrollbar orizzontale risultati

    // -- barra comandi (trova / sostituisci / vai alla riga) --
    BarMode    bar_mode;
    bool       bar_replace;          // riga "sostituisci" visibile
    bool       find_case, find_word; // opzioni di ricerca
    LineEdit*  bar_input;
    LineEdit*  bar_replace_input;
    Rect       bar_rect;
    Rect       bar_btn[BAR_BTN_COUNT];
    int        bar_label_w;          // larghezza dell'etichetta a sinistra (modalità non-find)

    // -- file, sessione e cronologia --
    char         data_dir[512];     // directory dei dati (sessione e cronologia)
    char         file_path[1024];   // script aperto ("" = senza nome)
    bool         modified;          // modifiche non salvate
    HistoryEntry history[HISTORY_MAX];
    int          history_count;
    bool         showing_history;   // la griglia mostra la cronologia
    Button*      btn_history;

    // -- stato --
    QueryResult* last_result;     // ultimo risultato (da liberare)
    int          win_w, win_h;    // dimensioni correnti finestra
    int          splitter_y;      // y dello splitter (editor/risultati)
    bool         splitter_drag;   // true = utente sta spostando il splitter
    bool         splitter_hover;
    bool         in_transaction;  // transazione aperta (badge in toolbar e footer)
    int          mouse_x, mouse_y;
} AppWindow;

// crea il layout principale
AppWindow* app_window_create(PlatformWindow* win, int width, int height);

// gestisce un evento OS e lo smista ai widget
void app_window_handle_event(AppWindow* app, sEvent* evt);

// animazioni (blink del cursore): true se serve ridisegnare
bool app_window_tick(AppWindow* app, uint64_t now_ms);

// ridisegna tutta la UI
void app_window_draw(AppWindow* app);

// aggiorna il layout dopo un resize
void app_window_resize(AppWindow* app, int new_w, int new_h);

// esegue la query nell'editor e aggiorna i risultati
void app_window_run_query(AppWindow* app);              // = RUN_SCRIPT
void app_window_run(AppWindow* app, RunMode mode);

// imposta la directory dei dati: ripristina l'ultima sessione dell'editor e la cronologia
void app_window_set_data_dir(AppWindow* app, const char* dir);

// salva il contenuto dell'editor per la prossima sessione (da chiamare prima di chiudere)
void app_window_save_session(AppWindow* app);

// dealloca tutto
void app_window_destroy(AppWindow* app);

#endif
