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

#include "chart_view.h"
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
#include "server_view.h"
#include "grid_edit.h"

#define MENUBAR_H         30   // barra dei menu (File, Edit, ...) sopra la toolbar
#define TOOLBAR_H         52   // Altezza toolbar in pixel
#define TOP_H             (MENUBAR_H + TOOLBAR_H)   // dove inizia il contenuto
#define SIDEBAR_W        264   // Larghezza predefinita della sidebar in pixel
#define SIDEBAR_MIN_W    170   // la sidebar si restringe fino a qui nelle finestre strette
#define CENTER_MIN_W     420   // larghezza minima garantita all'area editor/risultati
#define SIDEBAR_HEADER_H  38   // Intestazione "EXPLORER"
#define STATUSBAR_H       38   // Intestazione dei risultati (stato ultima query)
#define FOOTER_H          26   // Barra di stato in basso
#define SPLITTER_H         7   // Altezza splitter tra editor e risultati
#define SCROLLBAR_W       10   // Spessore delle scrollbar
#define RESULT_TABS_H     30   // barra delle schede quando una esecuzione produce più risultati
#define MAX_RESULT_TABS   20

typedef enum { RUN_SCRIPT, RUN_CURRENT } RunMode;

// cosa mostra la finestra: l'editor SQL, la scelta iniziale o il pannello del server API
typedef enum { APP_MODE_EDITOR = 0, APP_MODE_START, APP_MODE_SERVER } AppMode;
enum { PAGE_SQL = 0, PAGE_DESIGNER = 1 };   // pagine dell'editor

typedef enum { STATUS_IDLE, STATUS_OK, STATUS_ERROR, STATUS_INFO } StatusKind;

// barra comandi sovrapposta all'editor
typedef enum { BAR_NONE, BAR_FIND, BAR_GOTO, BAR_OPEN, BAR_SAVE_AS, BAR_EXPORT,
               BAR_IMPORT_CSV, BAR_RUN_FILE, BAR_DUMP, BAR_SEARCH_DB, BAR_BACKUP, BAR_RESTORE, BAR_SAVE_QUERY } BarMode;

#define HISTORY_MAX 500

// una query eseguita (cronologia)
typedef struct {
    char*  sql;
    char   when[20];        // "YYYY-MM-DD HH:MM:SS"
    bool   ok;
    int    rows;            // righe restituite o modificate
    double ms;
} HistoryEntry;

// query salvata con un nome (<dati>/saved_queries.sql)
#define SAVED_MAX 200
typedef struct { char name[80]; char* sql; } SavedQuery;

typedef enum {
    BAR_BTN_TOGGLE_REPLACE, BAR_BTN_CASE, BAR_BTN_WORD, BAR_BTN_PREV, BAR_BTN_NEXT, BAR_BTN_CLOSE,
    BAR_BTN_REPLACE, BAR_BTN_REPLACE_ALL, BAR_BTN_COUNT
} BarButton;

struct RunJob;   // esecuzione in corso nel thread di lavoro (window.c)

typedef struct {
    PlatformWindow* win;        // Finestra OS sottostante

    // -- modalità: editor, scelta iniziale, server API --
    AppMode         mode;
    StartScreen     start;
    ServerPanel*    server;

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

    bool         help_open;         // pannello scorciatoie (F1)

    LineEdit*    filter_input;      // filtro delle righe nell'intestazione dei risultati

    // -- menu contestuale (tasto destro su griglia o editor) --
    int          ctx_kind;          // 0 = chiuso, 1 = griglia, 2 = editor
    int          ctx_items[40];     // azioni (CtxAction in window.c)
    int          ctx_count;
    int          ctx_hover;
    int          ctx_x, ctx_y;      // punto del click
    Rect         ctx_rect;

    // -- barra dei menu --
    int          menu_open;         // menu aperto (-1 = nessuno)
    Rect         menu_rects[10];    // titoli dei menu (calcolati nel disegno)
    int          menu_hover;        // titolo sotto il mouse (-1 = nessuno)

    struct TableDesigner* td;       // designer di tabelle (creazione / struttura)
    struct SchemaView*    schema;   // pagina Designer: diagramma ER / UML
    struct UsersView*     uv;       // pannello Users & permissions
    int          page;              // PAGE_SQL / PAGE_DESIGNER
    Rect         page_tabs[2];      // selettore di pagina nella toolbar

    // -- pannello Settings (Ctrl+,) --
    bool         settings_open;
    int          settings_section;  // sezione selezionata
    int          settings_sel;      // riga selezionata nella sezione
    Rect         settings_rect;
    char         settings_path[1100];

    // -- pannello informativo (About, riferimento SQL) --
    int          info_page;         // 0 = chiuso
    int          info_scroll;

    int          footer_h;          // 0 se la barra di stato è nascosta
    bool         quit_requested;    // File > Exit
    char         export_table[64];  // tabella per l'export SQL INSERT

    // -- visualizzatore di cella (doppio click / Invio su una cella) --
    bool         cell_open;

    // -- conferma di un'azione distruttiva (svuota / elimina tabella o database) --
    bool         conf_open;
    char         conf_title[200], conf_text[300], conf_btn[32], conf_db[64], conf_done[160];
    char*        conf_sql;          // statement separati da ";\n"
    bool         conf_drop_db;      // DROP DATABASE: non serve USE
    Rect         conf_rect, conf_ok, conf_cancel;
    char         search_db[64];     // database della ricerca (barra "Search in")
    int          cell_scroll;       // righe di testo scrollate
    int          cell_lines;        // righe totali (calcolate nel disegno)
    Rect         cell_rect, cell_btn_copy, cell_btn_close;

    // -- esecuzione in background --
    struct RunJob*  job;            // != NULL mentre una query è in esecuzione
    PlatformThread* job_thread;
    int             job_base_line;  // riga dell'editor da cui inizia il testo eseguito
    uint64_t        job_started;
    uint64_t        job_last_status;

    // -- layout adattivo --
    int          sidebar_w;         // larghezza scelta dall'utente (trascinando il bordo)
    int          side_w;            // larghezza effettiva (ridotta se la finestra è stretta)
    bool         sidebar_drag, sidebar_hover;
    int          compact;           // 0 = toolbar completa, 1 = senza scorciatoie, 2 = solo icone, 3 = minima

    // -- tooltip --
    int          tip_target;        // elemento sotto il mouse (-1 = nessuno)
    uint64_t     tip_since;         // da quando il mouse è fermo sull'elemento
    bool         tip_visible;

    // -- selettore temi --
    Rect         theme_btn;         // pillola nella toolbar (calcolata nel disegno)
    bool         theme_menu_open;
    Rect         theme_menu;        // menu a tendina
    int          theme_hover;       // voce sotto il mouse / evidenziata da tastiera

    // -- modifica dei dati nella griglia (come phpMyAdmin) --
    GridEdit     gedit;              // cosa si può modificare nel risultato mostrato
    bool         gedit_cols[64];     // colonne modificabili (letto dalla griglia)
    LineEdit*    cell_edit;          // editor della cella
    int          edit_src, edit_col; // riga del risultato e colonna in modifica (-1 = nessuna)
    int          draft_src;          // riga nuova non ancora salvata (indice nel risultato, -1 = nessuna)
    bool         draft_set[64], draft_null[64];
    char*        draft_vals[64];
    int          del_src;            // riga in attesa di conferma della cancellazione
    uint64_t     del_until;
    Rect         eb_add, eb_save, eb_discard;   // pulsanti nell'intestazione dei risultati
    Rect         eb_chart, eb_chart_kind;       // Chart / Table e Bars / Lines
    bool         chart_on;                      // risultati come grafico
    SavedQuery   saved[SAVED_MAX];
    int          saved_count;
    bool         list_saved;                    // la lista mostrata (showing_history) è quella delle query salvate
    ChartModel   chart;

    // -- stato --
    QueryResult* last_result;     // risultato mostrato (= tabs[cur_tab])
    QueryResult* tabs[MAX_RESULT_TABS];   // un risultato per SELECT dell'ultima esecuzione (posseduti)
    int          num_tabs, cur_tab;
    Rect         tab_rects[MAX_RESULT_TABS];
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
void app_window_cancel(AppWindow* app);                // ferma la query in esecuzione
bool app_window_busy(AppWindow* app);                  // true se una query è in esecuzione

// imposta la directory dei dati: ripristina l'ultima sessione dell'editor e la cronologia
void app_window_set_data_dir(AppWindow* app, const char* dir);

// true dopo File > Exit: il main loop chiude l'applicazione
bool app_window_quit_requested(AppWindow* app);

// all'avvio: apre l'editor, il server o la scelta iniziale secondo Settings > Server
// (forced = START_EDITOR / START_SERVER dalla riga di comando, -1 = usa l'impostazione)
void app_window_startup(AppWindow* app, int forced);

// passa all'editor o al server (il server parte solo se il motore è libero e senza transazioni aperte)
void app_window_set_mode(AppWindow* app, AppMode mode);

// salva il contenuto dell'editor per la prossima sessione (da chiamare prima di chiudere)
void app_window_save_session(AppWindow* app);

// dealloca tutto
void app_window_destroy(AppWindow* app);

#endif
