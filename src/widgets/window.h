#ifndef WINDOW_H
#define WINDOW_H

/* 
SCOPO: Layout principale dell'applicazione SegVault.
       gestisce la disposizione di tutti i widget:

  ┌──────────────────────────────────────────────────────┐
  │  TOOLBAR: [Run] [Commit] [Rollback] [DB selector]    │
  ├────────────┬─────────────────────────────────────────┤
  │            │  EDITOR SQL (textbox multilinea)        │
  │  SIDEBAR   ├─────────────────────────────────────────┤
  │  (tree)    │  STATUS BAR (righe affected, tempo)     │
  │            ├─────────────────────────────────────────┤
  │            │  RESULTS (table_view)                   │
  └────────────┴─────────────────────────────────────────┘
 ───────────────────────────────────────────────────────────
*/

#include "widget.h"
#include "button.h"
#include "label.h"
#include "textbox.h"
#include "table_view.h"
#include "tree_view.h"
#include "scrollbar.h"
#include "../bridge/db_api.h"

#define TOOLBAR_H    40   // Altezza toolbar in pixel
#define SIDEBAR_W   240   // Larghezza sidebar in pixel
#define STATUSBAR_H  28   // Altezza status bar
#define SPLITTER_H   6    // Altezza splitter tra editor e risultati

typedef struct {
    PlatformWindow* win;        // Finestra OS sottostante

    // -- toolbar --
    Button* btn_run;            // esegui query (Ctrl+Enter)
    Button* btn_commit;         // COMMIT transazione
    Button* btn_rollback;       // ROLLBACK transazione
    Label*  lbl_db_name;        // nome database corrente

    // -- sidebar --
    TreeView*  tree;            // albero DB/Tabelle/Colonne
    Scrollbar* tree_scroll;     // scrollbar della sidebar

    // ── area centrale ────────────────────────────────────────────
    TextBox*   editor;          // editor SQL multilinea
    Scrollbar* editor_scroll;   // scrollbar dell'editor

    // -- status bar --
    Label* lbl_status;          // "3 rows | 12ms | OK"

    // -- risultati --
    TableView* results;          // griglia risultati
    Scrollbar* results_scroll_v; // scrollbar verticale risultati
    Scrollbar* results_scroll_h; // scrollbar orizzontale risultati

    // -- stato --
    QueryResult* last_result;     // ultimo risultato (da liberare)
    int          win_w, win_h;    // dimensioni correnti finestra
    int          splitter_y;      // y dello splitter (editor/risultati)
    bool         splitter_drag;   // true = utente sta spostando il splitter
} AppWindow;

// crea il layout principale
AppWindow* app_window_create(PlatformWindow* win, int width, int height);

// gestisce un evento OS e lo smista ai widget
void app_window_handle_event(AppWindow* app, sEvent* evt);

// ridisegna tutta la UI
void app_window_draw(AppWindow* app);

// aggiorna il layout dopo un resize
void app_window_resize(AppWindow* app, int new_w, int new_h);

// esegue la query nell'editor e aggiorna i risultati
void app_window_run_query(AppWindow* app);

// dealloca tutto
void app_window_destroy(AppWindow* app);

#endif