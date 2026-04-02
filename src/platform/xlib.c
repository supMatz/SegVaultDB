/* 
SCOPO: Stessa interfaccia di win32.c ma usa Xlib (Linux/X11).
       Il codice dei widget non cambia NULLA tra i due file.
       Compilato SOLO su Linux (vedi Makefile).
*/

#include <X11/X.h>
#include <X11/Xlib.h>     // API Xlib
#include <X11/Xutil.h>    // XTextExtents, XSizeHints
#include <X11/keysym.h>   // cstanti tasti (XK_Return, ecc.)
#include <stdlib.h>
#include <string.h>
#include "platform.h"

struct PlatformWindow {
    Display* display;       // connessione al server X (può essere remoto)
    Window window;          // id finestra
    GC gc;                  // equivalente all'HDC di Win32, per disegnare sulla finestra dal backbuffer
    Pixmap backbuf;         // backbuffer per double buffering, disegno nella Pixmap poi copio sulla finestra
    GC gc_buf;              // GC del backbuffer dove disegnamo inizialmente
    int screen;             // numero schermo (solitamente 0)
    int w, h;               // dimensioni finestra
    XFontStruct* font;      // font caricato
    Atom wm_delete;         // serve per non crashare o ignorare chiusura finestra
};

bool platform_init(void) {
    //xlib richiede che i thread usino XInitThreads(), per query async
    if(XInitThreads())
        return true;
    else 
        return false;  // se non va in errore ritorno true, altrimenti false
}

PlatformWindow* platform_window_create(const char* title, int w, int h) {
    PlatformWindow* win = calloc(1, sizeof(PlatformWindow));

    // apertura della connessione al server X
    win->display = XOpenDisplay(NULL); // NULL per usare $DISPLAY default dell'OS
    win->screen = DefaultScreen(win->display);
    win->w = w;
    win->h = h;

    // creazione finestra
    win->window = XCreateSimpleWindow(
        win->display,
        RootWindow(win->display, win->screen),
        0,0, w, h, // posizione iniziale e dimensioni  
        1, // spessore bordo
        BlackPixel(win->display, win->screen), // colore bordo
        BlackPixel(win->display, win->screen) // colore sfondo
    );

    // registrazione eventi da ricevere nella finestra
    XSelectInput(
        win->display, 
        win->window,
        ExposureMask        | // finestra da disegnare
        KeyPressMask        | // EVT_KEY_DOWN
        ButtonPressMask     | // EVT_MOUSE_DOWN
        ButtonReleaseMask   | // EVT_MOUSE_UP
        PointerMotionMask   | // EVT_MOUSE_MOVE
        StructureNotifyMask   // EVT_RESIZE
    );

    // intercettazione click sulla X per generazione eventi chiusure
    win->wm_delete = XInternAtom(win->display, "WM_DELETE_WINDOW", false);
    XSetWMProtocols(win->display, win->window, &win->wm_delete, 1);

    // impostazione titolo della finestra
    XStoreName(win->display, win->window, title);

    // crezione backbuffer
    win->backbuf = XCreatePixmap(
        win->display, 
        win->window, 
        w, h, 
        DefaultDepth(win->display, win->screen)
    );

    // creazione dei grafic contexts
    win->gc     = XCreateGC(win->display, win->window, 0, NULL);
    win->gc_buf = XCreateGC(win->display, win->window, 0, NULL);

    // caricamento font di sistema
    win->font = XLoadQueryFont(win->display, "-*-dejavu sans-medium-r-*-*-14-*-*-*-*-*-*-*");

    if(!win->font)
        win->font = XLoadQueryFont(win->display, "fixed");
    
    XSetFont(win->display, win->gc_buf, win->font->fid);

    return win;
}

// funzione per mostrare la finestra passata
void platform_window_show(PlatformWindow* win) {
    XMapWindow(win->display, win->window); // rende visibile la finestra
    XFlush(win->display); // invia i comandi al server X
}

bool platform_poll_event(PlatformWindow *win, sEvent *evt) {
    if(!XPending(win->display)) return false; // nessun evento

    XEvent xe;
    XNextEvent(win->display, &xe);
    evt->type = EVT_NONE;

    switch (xe.type) {
        case ClientMessage: 
            if((Atom)xe.xclient.data.l[0] == win->wm_delete) 
                evt->type = EVT_QUIT; 
            break;

        case MotionNotify:
            evt->type = EVT_MOUSE_MOVE;
            evt->mouse_x = xe.xmotion.x;
            evt->mouse_y = xe.xmotion.y;
            break;

        case ButtonPress: 
            evt->type = EVT_MOUSE_DOWN;
            evt->mouse_x = xe.xmotion.x;
            evt->mouse_y = xe.xmotion.y;
            evt->mouse_button = xe.xbutton.button;
            break;

        case ButtonRelease:
            evt->type         = EVT_MOUSE_UP;
            evt->mouse_x      = xe.xbutton.x;
            evt->mouse_y      = xe.xbutton.y;
            evt->mouse_button = xe.xbutton.button;
            break;

        case KeyPress: {
            // conversione KeySym X11 -> sKeyCode di platform.h
            KeySym sym = XLookupKeysym(&xe.xkey, 0);
            evt->type = EVT_KEY_DOWN;

            switch (sym) {
                case XK_Return:    evt->key = KEY_ENTER;     break;
                case XK_Escape:    evt->key = KEY_ESCAPE;    break;
                case XK_BackSpace: evt->key = KEY_BACKSPACE; break;
                case XK_Delete:    evt->key = KEY_DELETE;    break;  // canc
                case XK_Left:      evt->key = KEY_LEFT;      break;
                case XK_Right:     evt->key = KEY_RIGHT;     break;
                case XK_Up:        evt->key = KEY_UP;        break;
                case XK_Down:      evt->key = KEY_DOWN;      break;
                case XK_Home:      evt->key = KEY_HOME;      break;
                case XK_End:       evt->key = KEY_END;       break;
                case XK_Tab:       evt->key = KEY_TAB;       break;
                default:           evt->key = KEY_NONE;      break;
            }

            // generazione EVT_CHAR per differenziare caratteri di controllo da lettere o num
            // EVT_CHAR solo se NON è un tasto speciale già riconosciuto
            if (evt->key == KEY_NONE) {
                char buff[4] = {0};
                if (XLookupString(&xe.xkey, buff, 4, NULL, NULL) > 0
                    && buff[0] >= 32
                    && buff[0] != 127) {
                    evt->type      = EVT_CHAR;
                    evt->character = (uint32_t)buff[0];
                }
            }
            break;
        }

        case ConfigureNotify: {
            // finestra ridimensionata: ricrea il backbuffer
            if (xe.xconfigure.width  != win->w || xe.xconfigure.height != win->h) {
                win->w  = xe.xconfigure.width;
                win->h = xe.xconfigure.height;
                XFreePixmap(win->display, win->backbuf);
                win->backbuf = XCreatePixmap(
                    win->display, win->window,
                    win->w, win->h,
                    DefaultDepth(win->display, win->screen)
                );
                XFreeGC(win->display, win->gc_buf);
                win->gc_buf = XCreateGC(win->display,
                                         win->backbuf, 0, NULL);
                evt->type       = EVT_RESIZE;
                evt->new_width  = win->w;
                evt->new_height = win->h;
            }
            break;
        }
        
    }
    return evt->type != EVT_NONE; // return del fatto se l'evento è diverso da "nessuno"
}

void platform_clear(PlatformWindow* win, Color c) {
    unsigned long pixel = ((c.r << 16) | (c.g << 8) | c.b); // conversione color in pixel X11 - rosso nei bit significativi, verde al centro e blu nei bit finali : r-g-b

    XSetForeground(win->display, win->gc_buf, pixel); // scelta colore per la prossima operazione di disegno
    XFillRectangle(win->display, win->backbuf, win->gc_buf, 0, 0, win->w, win->h); // disegno rettangolo in pos (0;0) con w e h tante quante la finestra (copre tutto)   
}

int platform_draw_text(PlatformWindow *win, const char *text, Point pos, Color c, int size) {
    unsigned long pixel = ((c.r << 16) | (c.g << 8) | c.b); // stessa conversione che per platform_clear()

    XSetForeground(win->display, win->gc_buf, pixel); // stessa scelta colore per la prossima operazione di disegno
    
    XDrawString(        // funzione per disegnare una stringa (testo)
        win->display, 
        win->backbuf, 
        win->gc_buf, 
        pos.x, 
        (pos.y + size), // pos.y è la base line in Xlib a cui applico un offset di +size (altezza font) 
        text, 
        strlen(text)
    );

    return XTextWidth(win->font, text, strlen(text)); // ritorno la misurazione della grandezza del testo nella finestra
}

void platform_present(PlatformWindow *win) {
    // copio il backbuffer sulla finestra principale (equivalente a BitBlt)
    XCopyArea(
        win->display, 
        win->backbuf,   // sorgente copiatura
        win->window,    // destinazione : finestra utente
        win->gc,      
        0, 0,           // posizione sorgente
        win->w, win->h, // dimensioni finestra
        0,0             // posizione destinazione
    );       
    XFlush(win->display); // invio al server X
}

void platform_fill_rect(PlatformWindow* win, Rect r, Color c) {
    unsigned long pixel = ((c.r << 16) | (c.g << 8) | c.b);
    XSetForeground(win->display, win->gc_buf, pixel);
    XFillRectangle(win->display, win->backbuf, win->gc_buf,
                   r.x, r.y, r.w, r.h);
}

void platform_draw_rect(PlatformWindow* win, Rect r, Color c, int thickness) {
    unsigned long pixel = ((c.r << 16) | (c.g << 8) | c.b);
    XSetForeground(win->display, win->gc_buf, pixel);
    XSetLineAttributes(win->display, win->gc_buf,
                       thickness, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(win->display, win->backbuf, win->gc_buf,
                   r.x, r.y, r.w, r.h);
}

void platform_draw_line(PlatformWindow* win, Point a, Point b, Color c, int thickness) {
    unsigned long pixel = ((c.r << 16) | (c.g << 8) | c.b);
    XSetForeground(win->display, win->gc_buf, pixel);
    XSetLineAttributes(win->display, win->gc_buf,
                       thickness, LineSolid, CapButt, JoinMiter);
    XDrawLine(win->display, win->backbuf, win->gc_buf,
              a.x, a.y, b.x, b.y);
}

int platform_measure_text(PlatformWindow* win, const char* text, int size) {
    (void)size; // Xlib usa il font caricato, size ignorata
    if (!win->font || !text) return 0;
    return XTextWidth(win->font, text, strlen(text));
}

void platform_fill_rect_rounded(PlatformWindow* win, Rect r, Color c, int radius) {
    // Xlib non ha rettangoli arrotondati nativi: disegna rettangolo normale
    (void)radius;
    platform_fill_rect(win, r, c);
}

void platform_draw_bitmap(PlatformWindow* win, Rect dest, uint32_t* pixels, int pw, int ph) {
    // Stub: da implementare con XPutImage se necessario
    (void)win; (void)dest; (void)pixels; (void)pw; (void)ph;
}

void platform_window_destroy(PlatformWindow* win) {
    if (!win) return;
    XFreePixmap(win->display, win->backbuf);
    XFreeGC(win->display, win->gc);
    XFreeGC(win->display, win->gc_buf);
    if (win->font) XFreeFont(win->display, win->font);
    XDestroyWindow(win->display, win->window);
    XCloseDisplay(win->display);
    free(win);
}

void platform_shutdown(void) {
    // Niente da fare a livello globale su Xlib
}