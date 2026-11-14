
/* 
SCOPO: Albero gerarchico nella sidebar sinistra.
       Struttura: Database → Tabelle → Colonne
                           → Viste
                           → Procedure
                           → Funzioni
                           → Trigger 
*/

#ifndef TREE_VIEW_H
#define TREE_VIEW_H

#include "widget.h"

#define TREE_MAX_NODES   1024
#define TREE_ROW_H       26
#define TREE_INDENT      16   // pixel di indentazione per livello
#define TREE_PAD_TOP     4

// tipo del nodo — determina icona e azioni disponibili
typedef enum {
    TREE_NODE_ROOT,        // radice invisibile
    TREE_NODE_DATABASE,    // nome del database
    TREE_NODE_TABLE,       // tabella
    TREE_NODE_VIEW,        // vista
    TREE_NODE_PROCEDURE,   // stored procedure
    TREE_NODE_FUNCTION,    // function
    TREE_NODE_TRIGGER,     // trigger
    TREE_NODE_COLUMN,      // colonna (foglia)
    TREE_NODE_INDEX,       // indice
    TREE_NODE_CATEGORY,    // categoria (es: "Tables", "Views", ecc.)
} TreeNodeType;

// un singolo nodo dell'albero
typedef struct TreeNode {
    TreeNodeType type;
    char           label[128];      // testo mostrato
    char           extra[64];       // info aggiuntiva (tipo colonna, ecc.)
    bool           expanded;        // true = figli visibili
    bool           selected;        // true = nodo selezionato
    bool           has_children;    // true = ha figli (anche se non caricati)
    bool           loaded;          // true = figli già caricati dal DB

    struct TreeNode* parent;      // nodo genitore (NULL per la radice)
    struct TreeNode* children;    // array di figli
    int                num_children;
    int                children_cap;
} TreeNode;

typedef struct {
    Widget    base;

    TreeNode* root;           // radice dell'albero (invisibile)
    TreeNode* selected;       // nodo attualmente selezionato
    int         scroll_y;       // righe scrollate
    int         font_size;
    int         hover_row;      // riga sotto il mouse (-1 = nessuna)
    char        current_db[128];// database in uso nella sessione: segnato con un punto e il colore d'accento

    // doppio click
    uint64_t    last_click_ms;
    TreeNode*   last_click_node;

    Color color_bg;
    Color color_row_hover;
    Color color_row_selected;
    Color color_text;
    Color color_text_extra;   // colore info aggiuntiva (tipo colonna)
    Color color_icon_db;      // colore icona database
    Color color_icon_table;
    Color color_icon_view;
    Color color_icon_proc;

    // callback: utente ha selezionato un nodo
    void (*on_select)(TreeNode* node, void* user_data);

    // callback: utente ha fatto doppio click (es: apri tabella)
    void (*on_activate)(TreeNode* node, void* user_data);
} TreeView;

// crea il tree view
TreeView* tree_view_create(int x, int y, int w, int h);

void tree_view_apply_theme(TreeView* tv); // ricarica i colori dal tema attivo

// aggiunge un nodo figlio a un nodo esistente
TreeNode* tree_node_add_child(TreeNode* parent,
                                 TreeNodeType type,
                                 const char* label,
                                 const char* extra);

// espande/collassa un nodo
void tree_node_toggle(TreeView* tv, TreeNode* node);

// rimuove tutti i figli di un nodo (per ricaricare)
void tree_node_clear_children(TreeNode* node);

// popola l'albero con i database disponibili
void tree_view_refresh(TreeView* tv);

// seleziona un nodo programmaticamente
void tree_view_select(TreeView* tv, TreeNode* node);

// database in uso (NULL / "" = nessuno); ritorna il suo nodo, se c'è
TreeNode* tree_view_set_current_db(TreeView* tv, const char* db);

// numero di righe attualmente visibili (nodi espansi inclusi) e righe che entrano nel widget
int  tree_view_row_count(TreeView* tv);
int  tree_view_visible_rows(TreeView* tv);
void tree_view_clamp_scroll(TreeView* tv);

#endif 