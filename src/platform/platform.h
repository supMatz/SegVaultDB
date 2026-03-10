/* 
SCOPO: astrarre Win32 e Xlib in un'unica interfaccia.
       tutto il codice dei widget usa SOLO questo header.
       non include mai <windows.h> o <X11/Xlib.h> direttamente.
*/

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

// tipi geometrici (point, rectangle [x offset from left, y offset from top, h altezza, w larghezza], color)
typedef struct { int x, y; } Point;
typedef struct { int x, y, h, w; } Rect;
typedef struct { uint8_t r, g, b, a; } Color;

#define COLOR_BG        (Color){15,  17,  22,  255}  // blu notte profondo
#define COLOR_PANEL     (Color){22,  25,  34,  255}  // pannelli blu scuro
#define COLOR_ACCENT    (Color){110, 86,  255, 255}  // viola elettrico
#define COLOR_TEXT      (Color){225, 228, 240, 255}  // bianco freddo
#define COLOR_TEXT_DIM  (Color){88,  94,  120, 255}  // grigio blu
#define COLOR_BORDER    (Color){38,  42,  58,  255}  // bordi appena visibili
#define COLOR_HOVER     (Color){32,  36,  50,  255}  // hover
#define COLOR_SELECT    (Color){60,  46,  140, 255}  // selezione viola

// platform.h — visibile a tutti
typedef struct PlatformWindow PlatformWindow;  // forward declaration, esiste ma non sai cosa contiene

// definizione tipi di evento nelle finestre
typedef enum {
    EVT_NONE,
    EVT_QUIT,            // uscita dall'app da parte dell'utente
    EVT_MOUSE_MOVE,     // movimento mouse
    EVT_MOUSE_UP,      // rotella mouse verso sù
    EVT_MOUSE_DOWN,   // rotella mouse verso giù
    EVT_KEY_DOWN,    // pressione bottone
    EVT_KEY_UP,     // rilascio bottone
    EVT_CHAR,      // inserimento testo
    EVT_RESIZE,   // ridimensionamento finestra
    EVT_PAINT,   // stampa a schermo della finestra
} sEventType;

// codici tasti speciali generalizzati
typedef enum {
    KEY_NONE,
    KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_DELETE, 
    KEY_LEFT,  KEY_RIGHT,  KEY_UP,        KEY_DOWN,
    KEY_TAB,   KEY_HOME,   KEY_END,
    KEY_CTRL,  KEY_SHIFT, 
    KEY_F1,    KEY_F2,     KEY_F3,        KEY_F4,     KEY_F5,    
} sKeyCode;

typedef struct{
    sEventType type;
    int mouse_x, mouse_y;
    int mouse_button;              // left click, tasto rotella, right click
    sKeyCode key;                  // tasto premuto se EVT_KEY_DOWN
    uint32_t character;          // unicode se EVT_CHAR
    int new_width, new_height;  // nuove dimensioni se EVT_RESIZE
} sEvent;

// inizializzazione della platform (api).
// connessione al sistema grafico del sistema grafico dell'OS.
bool platform_init(void);

// creazione finestra con titolo
// ritorna NULL in caso di errore
PlatformWindow* platform_window_create(const char* title, int w, int h);

// mostra la finestra
void platform_window_show(PlatformWindow* win);

// legge il prossimo evento dalla coda, (non bloccante)
// ritorna fasle se ci sono eventi
bool platform_poll_event(PlatformWindow* win, sEvent* evt);

//--------------------------------------------------------------------------------------------
// Primitive di disegno -> tutte disegnano nel backbuffer; platform_present(); mostra il frame 
//--------------------------------------------------------------------------------------------

// riempimento finestra con un solo colore (pulizia frame)
void platform_clear(PlatformWindow* win, Color c);

// rettangolo pieno
void platform_fill_rect(PlatformWindow* win, Rect r, Color c);

// rettangolo solo bordo
void platform_draw_rect(PlatformWindow* win, Rect r, Color c, int thickness);

// rettangolo con angoli arrotondati
void platform_fill_rect_rounded(PlatformWindow* win, Rect r, Color c, int radius);

// linea tra due punti
void platform_draw_line(PlatformWindow* win, Point a, Point b, Color c, int thickness);

// testo con font di sistema
// size = altezza in pixel; ritorna la larghezza del testo disegnato
int  platform_draw_text(PlatformWindow* win, const char* text, Point pos, Color c, int size);

// misura la larghezza di un testo senza disegnarlo
// necessario per centrare testo nei bottoni
int  platform_measure_text(PlatformWindow* win, const char* text, int size);

// disegna un'immagine bitmap (per icone)
void platform_draw_bitmap(PlatformWindow* win, Rect dest, uint32_t* pixels, int pw, int ph);

// presenta il frame disegnato (swap backbuffer → schermo)
void platform_present(PlatformWindow* win);

// cleanup finale
void platform_window_destroy(PlatformWindow* win);
void platform_shutdown(void);

#endif