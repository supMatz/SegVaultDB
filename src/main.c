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
    printf("[SegVault %s] Inizializzazione DB engine...\n", SV_VERSION_STR);

    if (!db_init(data_dir)) {
        fprintf(stderr, "[ERRORE] Impossibile aprire '%s'\n", data_dir);
        return 1;
    }

    printf("[SegVault] DB engine pronto.\n");

    // -- inizializza il sistema grafico dell'OS --
    if (!platform_init()) {
        fprintf(stderr, "[ERRORE] Impossibile inizializzare la GUI\n");
        db_shutdown();
        return 1;
    }

    // -- crea la finestra --
    PlatformWindow* win = platform_window_create(
        SV_NAME " — SQL Client", INITIAL_W, INITIAL_H);

    if (!win) {
        fprintf(stderr, "[ERRORE] Impossibile creare la finestra\n");
        platform_shutdown();
        db_shutdown();
        return 1;
    }

    platform_window_show(win);

    // -- crea il layout dell'applicazione --
    AppWindow* app = app_window_create(win, INITIAL_W, INITIAL_H);
    if (!app) {
        fprintf(stderr, "[ERRORE] Impossibile creare il layout\n");
        platform_window_destroy(win);
        platform_shutdown();
        db_shutdown();
        return 1;
    }
    app_window_resize(app, INITIAL_W, INITIAL_H);

    printf("[SegVault] Avvio completato. Finestra aperta.\n");

    // -- loop principale --
    sEvent evt;
    int     running = 1;

    bool needs_redraw = true;

    while (running) {
    bool got_event = false;

    while (platform_poll_event(win, &evt)) {
        if (evt.type == EVT_QUIT) { running = 0; break; }
        app_window_handle_event(app, &evt);
        got_event = true;
    }

       if (!running) break;
       if (got_event) app_window_draw(app);	
    }

    printf("[SegVault] Chiusura in corso...\n");

    // -- shutdown in ordine inverso all'inizializzazione --
    app_window_destroy(app);
    platform_window_destroy(win);
    platform_shutdown();
    db_shutdown();  // flush WAL + salva catalogo su disco

    printf("[SegVault] Chiusura completata.\n");
    return 0;
}
