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

    // -- directory dei dati --
    const char* data_dir = (argc > 1) ? argv[1] : "./data";

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
        fprintf(stderr, "[ERROR] Impossible to open '%s'\n", data_dir);
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
            if (evt.type == EVT_QUIT) { running = 0; break; }
            app_window_handle_event(app, &evt);
            dirty = true;
        }
        if (!running) break;

        if (app_window_tick(app, platform_time_ms())) dirty = true;

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
