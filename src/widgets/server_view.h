/*
SCOPO: le due schermate della GUI oltre all'editor SQL.
       - StartScreen: all'avvio si sceglie se usare l'editor SQL o avviare il server API
       - ServerPanel: il server HTTP/JSON (src/server) dentro l'app, con stato, indirizzi,
         token, opzioni e registro delle richieste in tempo reale

       Server ed editor non sono attivi insieme: il motore ha un solo stato di sessione
       (database corrente, transazione aperta) e lo condividerebbero.
*/
#ifndef SERVER_VIEW_H
#define SERVER_VIEW_H

#include "../platform/platform.h"

typedef enum { VIEW_NONE = 0, VIEW_REDRAW, VIEW_OPEN_EDITOR, VIEW_OPEN_SERVER } ViewAction;

// ── schermata iniziale ──
typedef struct {
    int  hover;          // scheda sotto il mouse (-1 = nessuna)
    int  focus;          // scheda scelta con la tastiera
    bool remember;       // "ricorda la scelta"
    int  mx, my;
    Rect cards[2], remember_box;
} StartScreen;

void       start_screen_draw(StartScreen* s, PlatformWindow* win, int w, int h, const char* data_dir);
ViewAction start_screen_event(StartScreen* s, const sEvent* evt);

// ── pannello del server ──
typedef struct ServerPanel ServerPanel;

ServerPanel* server_panel_create(PlatformWindow* win);
void         server_panel_destroy(ServerPanel* p);      // ferma il server se è attivo
void         server_panel_set_paths(ServerPanel* p, const char* data_dir, const char* settings_path);
bool         server_panel_start(ServerPanel* p);        // false: il motivo è mostrato nel pannello
void         server_panel_stop(ServerPanel* p);
bool         server_panel_running(const ServerPanel* p);
void         server_panel_draw(ServerPanel* p, int w, int h);
ViewAction   server_panel_event(ServerPanel* p, sEvent* evt);
bool         server_panel_tick(ServerPanel* p, uint64_t now_ms);   // true se serve ridisegnare

#endif
