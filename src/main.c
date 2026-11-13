/*
SCOPO: Entry point di SegVault.
       1. Inizializza il DB engine (recovery, catalog, buffer pool)
       2. Inizializza la GUI (platform + finestra)
       3. Crea il layout dell'applicazione
       4. Gira il loop eventi finché l'utente chiude
       5. Shutdown ordinato (flush WAL, salva catalogo)
*/

#include "../include/common.h"
#include "platform/platform.h"
#include "widgets/window.h"
#include "bridge/db_api.h"
#include "widgets/settings.h"
#include "server/mail.h"
#include <string.h>
#include <stdio.h>
#include <signal.h>

// Ctrl+C / SIGTERM: chiusura ordinata (salva sessione, flush WAL) invece di terminare di colpo
static volatile sig_atomic_t g_quit_requested = 0;
static void on_quit_signal(int sig) { (void)sig; g_quit_requested = 1; }

#ifdef _WIN32
    #include <windows.h> // per Sleep
    #include <direct.h>   // _mkdir su Windows
    #define sleep_ms(ms) Sleep(ms)
#else
    #include <sys/stat.h> // mkdir su Linux
    #include <sys/types.h>
    #include <unistd.h>   // usleep
    #define sleep_ms(ms) usleep(ms * 1000) // generalizzazione sleep per windows e linux
#endif

// larghezza e altezza iniziale della finestra
#define INITIAL_W 1280
#define INITIAL_H  800

int main(int argc, char** argv) {
    // sendmail_path di PHP quando il server della GUI cattura le email (vedi src/server/mail.h)
    if (argc >= 3 && !strcmp(argv[1], "--sendmail")) return mail_sendmail_main(atoi(argv[2]), argc - 3, argv + 3);

    // -- argomenti: [--server | --editor] [cartella-dati] --
    const char* data_dir = "./data";
    int start_mode = -1;                       // -1 = come in Settings > Server > At startup open
    bool start_tray = false;                   // --tray: nascosto nell'area di notifica (avvio con il computer)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server"))      start_mode = START_SERVER;
        else if (!strcmp(argv[i], "--tray"))   start_tray = true;
        else if (!strcmp(argv[i], "--editor")) start_mode = START_EDITOR;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: %s [--editor | --server] [--tray] [data-directory]\n"
                   "  --editor   open the SQL editor\n"
                   "  --server   start the API server for web and desktop apps (see docs/API.md)\n"
                   "  --tray     start hidden in the notification area, with the server on\n"
                   "Without options SegVault asks (Settings > Server > At startup open).\n"
                   "For a server without a window use segvault-server.\n", argv[0]);
            return 0;
        }
        else data_dir = argv[i];
    }

    // crea la directory se non esiste
    // (mkdir -p equivalente per portabilità)
    #ifdef _WIN32
        _mkdir(data_dir);
    #else
        mkdir(data_dir, 0755);
    #endif

    // -- inizializza il DB engine --
    printf("[SegVault %s] DB engine initialization...\n", SV_VERSION_STR);

    if (!db_init(data_dir)) {
        char msg[700];
        snprintf(msg, sizeof(msg), "Cannot open the data directory '%.300s': %.300s", data_dir, db_init_error());
        platform_error_box(SV_NAME " cannot start", msg);
        return 1;
    }

    printf("[SegVault] DB engine ready.\n");

    // -- inizializza il sistema grafico dell'OS --
    if (!platform_init()) {
        fprintf(stderr, "[ERROR] Impossible to initialize the GUI\n");
        db_shutdown();
        return 1;
    }

    // -- crea la finestra --
    PlatformWindow* win = platform_window_create(
        SV_NAME " \xe2\x80\x94 SQL Client", INITIAL_W, INITIAL_H);

    if (!win) {
        fprintf(stderr, "[ERROR] Impossible to create the window\n");
        platform_shutdown();
        db_shutdown();
        return 1;
    }

    platform_window_show(win);

    // -- crea il layout dell'applicazione --
    AppWindow* app = app_window_create(win, INITIAL_W, INITIAL_H);
    if (!app) {
        fprintf(stderr, "[ERRORE] Impossible to create the layout\n");
        platform_window_destroy(win);
        platform_shutdown();
        db_shutdown();
        return 1;
    }
    app_window_resize(app, INITIAL_W, INITIAL_H);
    app_window_set_data_dir(app, data_dir);   // ripristina l'ultima sessione e la cronologia
    app_window_startup(app, start_tray ? START_SERVER : start_mode);   // editor, server API o la scelta iniziale

    // icona nell'area di notifica (se il desktop ne ha una): con il server acceso la finestra si può chiudere
    bool tray = platform_tray_enable(win, "SegVault");
    if (tray && start_tray) platform_window_set_visible(win, false);
    bool tray_state = false;

    printf("[SegVault] Started correctly. Window opened.\n");

    // loop principale //
    // ridisegna solo quando serve (eventi o blink del cursore) e attende
    // gli eventi senza consumare CPU a vuoto
    signal(SIGINT, on_quit_signal);
    signal(SIGTERM, on_quit_signal);

    sEvent evt;
    int running = 1;
    bool dirty = true;

    while (running) {
        if (g_quit_requested) break;
        while (platform_poll_event(win, &evt)) {
            if (evt.type == EVT_QUIT) {
                // chiusura della finestra con il server acceso: resta nell'area di notifica
                if (tray && g_settings.tray && app_window_server_running(app) && platform_window_visible(win)) {
                    platform_window_set_visible(win, false);
                    continue;
                }
                running = 0;
                break;
            }
            if (evt.type == EVT_TRAY_OPEN) { platform_window_set_visible(win, true); dirty = true; continue; }
            if (evt.type == EVT_TRAY_QUIT) { running = 0; break; }
            if (evt.type == EVT_TRAY_DASHBOARD) {
                const char* u = app_window_dashboard_url(app);
                if (u[0]) platform_open_url(u); else platform_window_set_visible(win, true);
                continue;
            }
            if (evt.type == EVT_TRAY_TOGGLE) { app_window_toggle_server(app); dirty = true; continue; }
            app_window_handle_event(app, &evt);
            dirty = true;
        }
        if (!running || app_window_quit_requested(app)) break;

        if (app_window_tick(app, platform_time_ms())) dirty = true;
        if (tray && app_window_server_running(app) != tray_state) {
            tray_state = app_window_server_running(app);
            platform_tray_update(win, tray_state ? "SegVault - server running" : "SegVault - server stopped", tray_state);
        }

        if (dirty) {
            app_window_draw(app);
            dirty = false;
        }
        platform_wait_event(win, 40);
    }

    printf("[SegVault] Closing...\n");

    // shutdown in ordine inverso all'inizializzazione //
    app_window_save_session(app);
    app_window_destroy(app);
    platform_window_destroy(win);
    platform_shutdown();
    db_shutdown();  // flush WAL + salva catalogo su disco

    printf("[SegVault] Closed correctly.\n");
    return 0;
}
