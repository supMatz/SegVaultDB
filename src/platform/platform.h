/*
SCOPO: astrarre Win32 e Xlib in un'unica interfaccia.
       tutto il codice dei widget usa SOLO questo header.
       non include mai <windows.h> o <X11/Xlib.h> direttamente.

       Tutte le stringhe passate alle funzioni di testo sono UTF-8.
*/

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

// tipi geometrici (point, rectangle [x offset from left, y offset from top, h altezza, w larghezza], color)
typedef struct { int x, y; } Point;
typedef struct { int x, y, w, h; } Rect;
typedef struct { uint8_t r, g, b, a; } Color;

// palette di base (vedi anche widgets/theme.h per la palette completa)
#define COLOR_BG        (Color){13,  15,  20,  255}  // blu notte profondo
#define COLOR_PANEL     (Color){19,  22,  30,  255}  // pannelli blu scuro
#define COLOR_ACCENT    (Color){124, 106, 255, 255}  // viola elettrico
#define COLOR_TEXT      (Color){226, 230, 242, 255}  // bianco freddo
#define COLOR_TEXT_DIM  (Color){104, 112, 140, 255}  // grigio blu
#define COLOR_BORDER    (Color){36,  41,  56,  255}  // bordi appena visibili
#define COLOR_HOVER     (Color){30,  34,  47,  255}  // hover
#define COLOR_SELECT    (Color){52,  44,  120, 255}  // selezione viola

#define SV_MOD_CTRL  0x01
#define SV_MOD_SHIFT 0x02
#define SV_MOD_ALT   0x04

// pulsanti del mouse (evt->mouse_button)
#define SV_MOUSE_LEFT   1
#define SV_MOUSE_MIDDLE 2
#define SV_MOUSE_RIGHT  3

// platform.h — visibile a tutti
typedef struct PlatformWindow PlatformWindow;  // forward declaration, esiste ma non sai cosa contiene

// definizione tipi di evento nelle finestre
typedef enum {
    EVT_NONE,
    EVT_QUIT,          // uscita dall'app da parte dell'utente
    EVT_MOUSE_MOVE,    // movimento mouse
    EVT_MOUSE_UP,      // rilascio pulsante mouse
    EVT_MOUSE_DOWN,    // pressione pulsante mouse
    EVT_KEY_DOWN,      // pressione tasto (tasti speciali o scorciatoie Ctrl+lettera)
    EVT_KEY_UP,        // rilascio tasto
    EVT_CHAR,          // inserimento testo (evt->character = codepoint unicode)
    EVT_RESIZE,        // ridimensionamento finestra
    EVT_PAINT,         // stampa a schermo della finestra
    EVT_MOUSE_WHEEL,   // rotella: evt->wheel_delta > 0 su, < 0 giù
} sEventType;

// codici tasti speciali generalizzati
typedef enum {
    KEY_NONE,
    KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_DELETE,
    KEY_LEFT,  KEY_RIGHT,  KEY_UP,        KEY_DOWN,
    KEY_TAB,   KEY_HOME,   KEY_END,
    KEY_CTRL,  KEY_SHIFT,
    KEY_F1,    KEY_F2,     KEY_F3,        KEY_F4,     KEY_F5,
    KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_LETTER,   // Ctrl/Alt + lettera o simbolo: il carattere (minuscolo) è in evt->character
} sKeyCode;

typedef struct{
    sEventType type;
    int mouse_x, mouse_y;
    int mouse_button;            // SV_MOUSE_LEFT / MIDDLE / RIGHT
    sKeyCode key;                // tasto premuto se EVT_KEY_DOWN
    uint32_t character;          // unicode se EVT_CHAR (o lettera se key == KEY_LETTER)
    int new_width, new_height;   // nuove dimensioni se EVT_RESIZE
    uint32_t modifiers;          // SV_MOD_CTRL | SV_MOD_SHIFT | SV_MOD_ALT
    int wheel_delta;             // EVT_MOUSE_WHEEL: +1 su, -1 giù
} sEvent;

// stili di font disponibili
typedef enum {
    FONT_UI = 0,        // sans-serif per l'interfaccia
    FONT_UI_BOLD,       // sans-serif grassetto (titoli, header)
    FONT_MONO,          // monospace per editor SQL e dati
    FONT_MONO_BOLD,     // monospace grassetto
    FONT_STYLE_COUNT
} FontStyle;

// cursori del mouse
typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_IBEAM,       // testo
    CURSOR_HAND,        // elemento cliccabile
    CURSOR_RESIZE_V,    // splitter orizzontale (trascina su/giù)
    CURSOR_RESIZE_H,    // splitter verticale (trascina sx/dx)
    CURSOR_COUNT
} CursorKind;

// inizializzazione della platform (api).
// connessione al sistema grafico del sistema grafico dell'OS.
bool platform_init(void);

// messaggio d'errore bloccante, utilizzabile anche prima di platform_init (es. avvio fallito)
void platform_error_box(const char* title, const char* message);

// creazione finestra con titolo
// ritorna NULL in caso di errore
PlatformWindow* platform_window_create(const char* title, int w, int h);

// mostra la finestra
void platform_window_show(PlatformWindow* win);

// legge il prossimo evento dalla coda, (non bloccante)
// ritorna false se non ci sono eventi
bool platform_poll_event(PlatformWindow* win, sEvent* evt);

// attende fino a timeout_ms che arrivi un evento (evita il busy-loop al 100% di CPU)
void platform_wait_event(PlatformWindow* win, int timeout_ms);

// millisecondi monotoni (per animazioni, doppio click, blink del cursore)
uint64_t platform_time_ms(void);

//--------------------------------------------------------------------------------------------
// Primitive di disegno -> tutte disegnano nel backbuffer; platform_present(); mostra il frame
//--------------------------------------------------------------------------------------------

// riempimento finestra con un solo colore (pulizia frame)
void platform_clear(PlatformWindow* win, Color c);

// rettangolo pieno (se c.a < 255 il colore viene fuso con lo sfondo dove supportato)
void platform_fill_rect(PlatformWindow* win, Rect r, Color c);

// rettangolo solo bordo
void platform_draw_rect(PlatformWindow* win, Rect r, Color c, int thickness);

// rettangolo con angoli arrotondati
void platform_fill_rect_rounded(PlatformWindow* win, Rect r, Color c, int radius);

// linea tra due punti
void platform_draw_line(PlatformWindow* win, Point a, Point b, Color c, int thickness);

// limita tutte le operazioni di disegno successive al rettangolo r
void platform_set_clip(PlatformWindow* win, Rect r);
void platform_reset_clip(PlatformWindow* win);

// testo con font di sistema (FONT_UI)
// pos = angolo in alto a sinistra del testo; size = altezza in pixel; ritorna la larghezza del testo disegnato
int  platform_draw_text(PlatformWindow* win, const char* text, Point pos, Color c, int size);

// misura la larghezza di un testo senza disegnarlo (FONT_UI)
int  platform_measure_text(PlatformWindow* win, const char* text, int size);

// versioni estese: scelta dello stile e lunghezza in byte (len < 0 = fino al '\0')
int  platform_draw_text_ex(PlatformWindow* win, const char* text, int len, Point pos, Color c, int size, FontStyle style);
int  platform_measure_text_ex(PlatformWindow* win, const char* text, int len, int size, FontStyle style);

// altezza della riga di testo (ascent + descent) per un font: serve per centrare verticalmente
int  platform_text_height(PlatformWindow* win, int size, FontStyle style);

// disegna un'immagine bitmap (per icone)
void platform_draw_bitmap(PlatformWindow* win, Rect dest, uint32_t* pixels, int pw, int ph);

// presenta il frame disegnato (swap backbuffer → schermo)
void platform_present(PlatformWindow* win);

// cursore del mouse sulla finestra
void platform_set_cursor(PlatformWindow* win, CursorKind kind);

// appunti di sistema (UTF-8)
void  platform_set_clipboard(PlatformWindow* win, const char* text);
char* platform_get_clipboard(PlatformWindow* win);   // stringa malloc'd (free() del chiamante) o NULL

// apre un indirizzo (https://...) nel browser predefinito; false se non è possibile
bool  platform_open_url(const char* url);

// thread di lavoro (usato per eseguire le query senza bloccare l'interfaccia)
typedef struct PlatformThread PlatformThread;
PlatformThread* platform_thread_start(void (*fn)(void* arg), void* arg);  // NULL se non riesce
void            platform_thread_join(PlatformThread* t);                 // attende la fine e libera

// cleanup finale
void platform_window_destroy(PlatformWindow* win);
void platform_shutdown(void);

#endif
