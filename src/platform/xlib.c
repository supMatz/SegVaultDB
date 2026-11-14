/*
SCOPO: Stessa interfaccia di win32.c ma usa Xlib (Linux/X11).
       Il codice dei widget non cambia NULLA tra i due file.
       Compilato SOLO su Linux (vedi Makefile).

       Testo: Xft + fontconfig (font antialiasing, UTF-8).
       Alpha / angoli arrotondati antialiasing: XRender.
       Appunti: selezione CLIPBOARD (ICCCM).
*/

#include "fonts_embed.h"
#include <X11/X.h>
#include <X11/Xlib.h>     // API Xlib
#include <X11/Xutil.h>    // XLookupString, XSizeHints
#include <X11/Xatom.h>    // XA_STRING, XA_ATOM
#include <X11/keysym.h>   // costanti tasti (XK_Return, ecc.)
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>  // font antialiasing
#include <X11/extensions/Xrender.h>
#include <sys/select.h>
#include <unistd.h>      // readlink
#include <sys/stat.h>
#include <sys/wait.h>    // waitpid (platform_open_url)
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "platform.h"

// famiglie di font in ordine di preferenza: fontconfig sceglie la prima installata
// Fira Sans e JetBrains Mono sono inclusi in assets/fonts (stessi font anche su Windows)
#define SV_FONT_UI   "Fira Sans,Inter,Noto Sans,Cantarell,Ubuntu,DejaVu Sans,sans-serif"
#define SV_FONT_MONO "JetBrains Mono,JetBrainsMono Nerd Font Mono,Fira Code,FiraCode Nerd Font Mono," \
                     "Cascadia Mono,Source Code Pro,Hack,DejaVu Sans Mono,monospace"

#define FONT_CACHE_CAP 32

typedef struct {
    FontStyle style;
    int       size;
    XftFont*  font;
} FontEntry;

struct PlatformWindow {
    Display* display;       // connessione al server X (può essere remoto)
    Window   window;        // id finestra
    GC       gc;            // GC della finestra (per copiare il backbuffer)
    Pixmap   backbuf;       // backbuffer per double buffering, disegno nella Pixmap poi copio sulla finestra
    GC       gc_buf;        // GC del backbuffer dove disegnamo inizialmente
    int      screen;        // numero schermo (solitamente 0)
    int      w, h;          // dimensioni finestra
    Visual*  visual;
    Colormap colormap;
    int      depth;

    XftDraw* xft;           // contesto Xft legato al backbuffer
    FontEntry fonts[FONT_CACHE_CAP];
    int       num_fonts;

    Atom wm_delete;         // serve per non crashare o ignorare chiusura finestra

    // appunti
    Atom  a_clipboard, a_utf8, a_targets, a_text, a_prop;
    char* clip_text;        // testo che possediamo (NULL se non siamo owner)

    // cursori
    Cursor     cursors[CURSOR_COUNT];
    CursorKind cur_cursor;

    // clipping corrente
    bool has_clip;
    Rect clip;

    // icona nel system tray (XEmbed)
    Window tray;            // 0 = nessuna
    int    tray_w, tray_h;
    bool   tray_active;
    bool   visible;
};

// -- utilità --

static unsigned long to_pixel(Color c) {
    return ((unsigned long)c.r << 16) | ((unsigned long)c.g << 8) | c.b;
}

static XRenderColor to_render(Color c) {
    // XRender lavora con colori premoltiplicati a 16 bit
    XRenderColor rc;
    rc.red   = (unsigned short)((c.r * c.a / 255) * 257);
    rc.green = (unsigned short)((c.g * c.a / 255) * 257);
    rc.blue  = (unsigned short)((c.b * c.a / 255) * 257);
    rc.alpha = (unsigned short)(c.a * 257);
    return rc;
}

static XftFont* get_font(PlatformWindow* win, int size, FontStyle style) {
    if (size < 6) size = 6;
    for (int i = 0; i < win->num_fonts; i++)
        if (win->fonts[i].size == size && win->fonts[i].style == style)
            return win->fonts[i].font;

    char name[512];
    const char* family = (style == FONT_MONO || style == FONT_MONO_BOLD) ? SV_FONT_MONO : SV_FONT_UI;
    const char* weight = (style == FONT_UI_BOLD) ? ":weight=semibold"
                       : (style == FONT_MONO_BOLD) ? ":weight=bold" : "";
    snprintf(name, sizeof(name), "%s:pixelsize=%d%s:antialias=true", family, size, weight);

    XftFont* f = XftFontOpenName(win->display, win->screen, name);
    if (!f) f = XftFontOpenName(win->display, win->screen, "sans-serif");
    if (!f) return NULL;

    if (win->num_fonts < FONT_CACHE_CAP) {
        win->fonts[win->num_fonts++] = (FontEntry){style, size, f};
    } else {
        // cache piena: sostituisce l'ultima voce
        XftFontClose(win->display, win->fonts[FONT_CACHE_CAP - 1].font);
        win->fonts[FONT_CACHE_CAP - 1] = (FontEntry){style, size, f};
    }
    return f;
}

static void apply_clip(PlatformWindow* win) {
    if (win->has_clip) {
        XRectangle xr = {(short)win->clip.x, (short)win->clip.y,
                         (unsigned short)(win->clip.w > 0 ? win->clip.w : 0),
                         (unsigned short)(win->clip.h > 0 ? win->clip.h : 0)};
        XSetClipRectangles(win->display, win->gc_buf, 0, 0, &xr, 1, Unsorted);
        XftDrawSetClipRectangles(win->xft, 0, 0, &xr, 1);
    } else {
        XSetClipMask(win->display, win->gc_buf, None);
        XftDrawSetClip(win->xft, NULL);
    }
}

// -- init / finestra --

// rende disponibili a fontconfig i font inclusi nel progetto (assets/fonts accanto
// all'eseguibile o nella cartella corrente)
static bool is_dir(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

// i font inclusi nell'eseguibile, copiati una volta in ~/.cache/segvault/fonts (fontconfig vuole dei file)
static bool load_embedded_fonts(void) {
    const char* base = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    char dir[4200];
    if (base && base[0]) snprintf(dir, sizeof(dir), "%s/segvault", base);
    else if (home && home[0]) snprintf(dir, sizeof(dir), "%s/.cache/segvault", home);
    else return false;
    char parent[4200];
    snprintf(parent, sizeof(parent), "%s", dir);
    char* sl = strrchr(parent, '/');
    if (sl) { *sl = '\0'; mkdir(parent, 0755); }
    mkdir(dir, 0755);
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "/fonts");
    mkdir(dir, 0755);
    for (int i = 0; i < sv_embedded_font_count(); i++) {
        EmbeddedFont f = sv_embedded_font(i);
        char path[4400];
        snprintf(path, sizeof(path), "%s/%s", dir, f.name);
        struct stat st;
        if (stat(path, &st) == 0 && (size_t)st.st_size == f.size) continue;   // già copiato
        char tmp[4500];
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
        FILE* o = fopen(tmp, "wb");
        if (!o) return false;
        bool ok = fwrite(f.data, 1, f.size, o) == f.size;
        if (fclose(o) != 0) ok = false;
        if (!ok || rename(tmp, path) != 0) { remove(tmp); return false; }
    }
    return FcConfigAppFontAddDir(NULL, (const FcChar8*)dir);
}

static void load_bundled_fonts(void) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        char dir[4200];
        snprintf(dir, sizeof(dir), "%s/assets/fonts", exe);
        if (is_dir(dir) && FcConfigAppFontAddDir(NULL, (const FcChar8*)dir)) return;
        snprintf(dir, sizeof(dir), "%s/fonts", exe);
        if (is_dir(dir) && FcConfigAppFontAddDir(NULL, (const FcChar8*)dir)) return;
    }
    if (is_dir("assets/fonts") && FcConfigAppFontAddDir(NULL, (const FcChar8*)"assets/fonts")) return;
    load_embedded_fonts();                      // eseguibile scaricato da solo
}

bool platform_init(void) {
    // xlib richiede XInitThreads() perché le query girano in un thread separato
    if (!XInitThreads()) return false;
    load_bundled_fonts();
    return true;
}

PlatformWindow* platform_window_create(const char* title, int w, int h) {
    PlatformWindow* win = calloc(1, sizeof(PlatformWindow));
    if (!win) return NULL;

    // apertura della connessione al server X
    win->display = XOpenDisplay(NULL); // NULL per usare $DISPLAY default dell'OS
    if (!win->display) { free(win); return NULL; }

    win->screen   = DefaultScreen(win->display);
    win->visual   = DefaultVisual(win->display, win->screen);
    win->colormap = DefaultColormap(win->display, win->screen);
    win->depth    = DefaultDepth(win->display, win->screen);
    win->w = w;
    win->h = h;

    // creazione finestra (sfondo = colore dell'app, evita il flash nero all'avvio)
    win->window = XCreateSimpleWindow(
        win->display,
        RootWindow(win->display, win->screen),
        0, 0, w, h,
        0,
        to_pixel(COLOR_BG),
        to_pixel(COLOR_BG)
    );

    // registrazione eventi da ricevere nella finestra
    XSelectInput(
        win->display,
        win->window,
        ExposureMask        | // finestra da disegnare
        KeyPressMask        | // EVT_KEY_DOWN / EVT_CHAR
        ButtonPressMask     | // EVT_MOUSE_DOWN / EVT_MOUSE_WHEEL
        ButtonReleaseMask   | // EVT_MOUSE_UP
        PointerMotionMask   | // EVT_MOUSE_MOVE
        StructureNotifyMask | // EVT_RESIZE
        FocusChangeMask
    );

    // intercettazione click sulla X per generazione eventi chiusure
    win->wm_delete = XInternAtom(win->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(win->display, win->window, &win->wm_delete, 1);

    // titolo (anche in UTF-8 per i window manager moderni)
    XStoreName(win->display, win->window, title);
    Atom net_name = XInternAtom(win->display, "_NET_WM_NAME", False);
    Atom utf8     = XInternAtom(win->display, "UTF8_STRING", False);
    XChangeProperty(win->display, win->window, net_name, utf8, 8, PropModeReplace,
                    (const unsigned char*)title, (int)strlen(title));

    XClassHint* ch = XAllocClassHint();
    if (ch) {
        ch->res_name  = "segvault";
        ch->res_class = "SegVault";
        XSetClassHint(win->display, win->window, ch);
        XFree(ch);
    }

    // dimensione minima
    XSizeHints* sh = XAllocSizeHints();
    if (sh) {
        sh->flags = PMinSize;
        sh->min_width  = 820;
        sh->min_height = 520;
        XSetWMNormalHints(win->display, win->window, sh);
        XFree(sh);
    }

    // creazione backbuffer
    win->backbuf = XCreatePixmap(win->display, win->window, w, h, win->depth);

    // creazione dei graphic contexts
    win->gc     = XCreateGC(win->display, win->window, 0, NULL);
    win->gc_buf = XCreateGC(win->display, win->backbuf, 0, NULL);

    // contesto Xft per il testo antialiasing
    win->xft = XftDrawCreate(win->display, win->backbuf, win->visual, win->colormap);

    // appunti
    win->a_clipboard = XInternAtom(win->display, "CLIPBOARD", False);
    win->a_utf8      = utf8;
    win->a_targets   = XInternAtom(win->display, "TARGETS", False);
    win->a_text      = XInternAtom(win->display, "TEXT", False);
    win->a_prop      = XInternAtom(win->display, "SV_CLIPBOARD", False);

    // cursori
    win->cursors[CURSOR_ARROW]    = XCreateFontCursor(win->display, XC_left_ptr);
    win->cursors[CURSOR_IBEAM]    = XCreateFontCursor(win->display, XC_xterm);
    win->cursors[CURSOR_HAND]     = XCreateFontCursor(win->display, XC_hand2);
    win->cursors[CURSOR_RESIZE_V] = XCreateFontCursor(win->display, XC_sb_v_double_arrow);
    win->cursors[CURSOR_RESIZE_H] = XCreateFontCursor(win->display, XC_sb_h_double_arrow);
    win->cur_cursor = CURSOR_ARROW;
    XDefineCursor(win->display, win->window, win->cursors[CURSOR_ARROW]);

    // pre-carica i font principali
    get_font(win, 13, FONT_UI);
    get_font(win, 14, FONT_MONO);

    return win;
}

// funzione per mostrare la finestra passata
void platform_window_show(PlatformWindow* win) {
    XMapWindow(win->display, win->window); // rende visibile la finestra
    XFlush(win->display); // invia i comandi al server X
    win->visible = true;
}

void platform_window_set_visible(PlatformWindow* win, bool visible) {
    if (visible) XMapRaised(win->display, win->window);
    else XUnmapWindow(win->display, win->window);
    XFlush(win->display);
    win->visible = visible;
}

bool platform_window_visible(PlatformWindow* win) { return win->visible; }

// ── system tray (protocollo XEmbed di freedesktop.org) ──

static void tray_draw(PlatformWindow* win) {
    if (!win->tray) return;
    Display* d = win->display;
    int w = win->tray_w > 0 ? win->tray_w : 22, h = win->tray_h > 0 ? win->tray_h : 22;
    int sz = (w < h ? w : h) - 4;
    if (sz < 8) sz = 8;
    int x = (w - sz) / 2, y = (h - sz) / 2;
    GC gc = XCreateGC(d, win->tray, 0, NULL);
    XClearWindow(d, win->tray);
    // pila di dischi (un database): verde se il server gira, grigia altrimenti
    unsigned long col = win->tray_active ? 0x22a35a : 0x8a919c;
    XSetForeground(d, gc, col);
    int dh = sz / 3;
    for (int i = 0; i < 3; i++) {
        XFillArc(d, win->tray, gc, x, y + i * (sz - dh) / 2, sz, dh, 0, 360 * 64);
    }
    XFillRectangle(d, win->tray, gc, x, y + dh / 2, sz, sz - dh);
    XSetForeground(d, gc, 0xffffff);
    for (int i = 1; i < 3; i++) XDrawArc(d, win->tray, gc, x, y + i * (sz - dh) / 2 - 1, sz - 1, dh, 180 * 64, 180 * 64);
    XFreeGC(d, gc);
    XFlush(d);
}

bool platform_tray_enable(PlatformWindow* win, const char* tooltip) {
    if (win->tray) return true;
    Display* d = win->display;
    char name[48];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", win->screen);
    Window owner = XGetSelectionOwner(d, XInternAtom(d, name, False));
    if (owner == None) return false;                          // nessuna area di notifica (es. GNOME senza estensione)
    XSetWindowAttributes a;
    memset(&a, 0, sizeof(a));
    a.background_pixmap = ParentRelative;
    a.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
    win->tray = XCreateWindow(d, RootWindow(d, win->screen), 0, 0, 22, 22, 0, CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixmap | CWEventMask, &a);
    win->tray_w = win->tray_h = 22;
    XSizeHints* sh = XAllocSizeHints();
    if (sh) { sh->flags = PMinSize; sh->min_width = sh->min_height = 16; XSetWMNormalHints(d, win->tray, sh); XFree(sh); }
    XStoreName(d, win->tray, tooltip ? tooltip : "SegVault");
    Atom xi = XInternAtom(d, "_XEMBED_INFO", False);
    long info[2] = { 0, 1 };                                  // versione 0, XEMBED_MAPPED
    XChangeProperty(d, win->tray, xi, xi, 32, PropModeReplace, (unsigned char*)info, 2);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = owner;
    ev.xclient.message_type = XInternAtom(d, "_NET_SYSTEM_TRAY_OPCODE", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 0;                                 // SYSTEM_TRAY_REQUEST_DOCK
    ev.xclient.data.l[2] = (long)win->tray;
    XSendEvent(d, owner, False, NoEventMask, &ev);
    XFlush(d);
    return true;
}

void platform_tray_update(PlatformWindow* win, const char* tooltip, bool active) {
    if (!win->tray) return;
    if (tooltip) XStoreName(win->display, win->tray, tooltip);
    if (active != win->tray_active) { win->tray_active = active; tray_draw(win); }
}

void platform_tray_disable(PlatformWindow* win) {
    if (!win->tray) return;
    XDestroyWindow(win->display, win->tray);
    win->tray = 0;
    XFlush(win->display);
}

// ── avvio con il computer: ~/.config/autostart (specifica XDG) ──

static void autostart_path(char* out, size_t n) {
    const char* cfg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    if (cfg && cfg[0]) snprintf(out, n, "%s/autostart/segvault.desktop", cfg);
    else snprintf(out, n, "%s/.config/autostart/segvault.desktop", home ? home : ".");
}

bool platform_autostart_set(bool on, const char* command) {
    char p[1100];
    autostart_path(p, sizeof(p));
    if (!on) return remove(p) == 0 || access(p, F_OK) != 0;
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s", p);
    char* sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; char* sl2 = strrchr(dir, '/'); if (sl2) { *sl2 = '\0'; mkdir(dir, 0755); *sl2 = '/'; } mkdir(dir, 0755); }
    FILE* f = fopen(p, "w");
    if (!f) return false;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=SegVault\nComment=Local development server and database\n"
               "Exec=%s\nTerminal=false\nX-GNOME-Autostart-enabled=true\n", command);
    return fclose(f) == 0;
}

bool platform_autostart_get(void) {
    char p[1100];
    autostart_path(p, sizeof(p));
    return access(p, F_OK) == 0;
}

// -- appunti: risposta alle richieste di altri programmi --

static void handle_selection_request(PlatformWindow* win, XSelectionRequestEvent* req) {
    XSelectionEvent ev = {0};
    ev.type      = SelectionNotify;
    ev.display   = req->display;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target    = req->target;
    ev.time      = req->time;
    ev.property  = None;

    Atom prop = req->property != None ? req->property : req->target;

    if (win->clip_text) {
        if (req->target == win->a_targets) {
            Atom targets[] = { win->a_targets, win->a_utf8, XA_STRING, win->a_text };
            XChangeProperty(win->display, req->requestor, prop, XA_ATOM, 32,
                            PropModeReplace, (unsigned char*)targets, 4);
            ev.property = prop;
        } else if (req->target == win->a_utf8 || req->target == XA_STRING || req->target == win->a_text) {
            XChangeProperty(win->display, req->requestor, prop, req->target, 8,
                            PropModeReplace, (unsigned char*)win->clip_text,
                            (int)strlen(win->clip_text));
            ev.property = prop;
        }
    }
    XSendEvent(win->display, req->requestor, False, 0, (XEvent*)&ev);
    XFlush(win->display);
}

// conversione keysym -> codepoint unicode (latin-1 e keysym unicode)
static uint32_t keysym_to_unicode(KeySym sym) {
    if (sym >= 0x20 && sym <= 0x7e) return (uint32_t)sym;
    if (sym >= 0xa0 && sym <= 0xff) return (uint32_t)sym;
    if ((sym & 0xff000000) == 0x01000000) return (uint32_t)(sym & 0x00ffffff);
    if (sym == XK_EuroSign) return 0x20ac;
    if (sym >= XK_KP_0 && sym <= XK_KP_9) return '0' + (uint32_t)(sym - XK_KP_0);
    switch (sym) {
        case XK_KP_Add:      return '+';
        case XK_KP_Subtract: return '-';
        case XK_KP_Multiply: return '*';
        case XK_KP_Divide:   return '/';
        case XK_KP_Decimal:  return '.';
        case XK_KP_Separator:return ',';
        default: return 0;
    }
}

static uint32_t x_modifiers(unsigned int state) {
    uint32_t m = 0;
    if (state & ControlMask) m |= SV_MOD_CTRL;
    if (state & ShiftMask)   m |= SV_MOD_SHIFT;
    if (state & Mod1Mask)    m |= SV_MOD_ALT;
    return m;
}

bool platform_poll_event(PlatformWindow *win, sEvent *evt) {
    while (XPending(win->display)) {
        XEvent xe;
        XNextEvent(win->display, &xe);
        memset(evt, 0, sizeof(*evt));
        evt->type = EVT_NONE;

        if (win->tray && xe.xany.window == win->tray) {          // eventi dell'icona nel tray
            if (xe.type == Expose && xe.xexpose.count == 0) tray_draw(win);
            else if (xe.type == ConfigureNotify) { win->tray_w = xe.xconfigure.width; win->tray_h = xe.xconfigure.height; tray_draw(win); }
            else if (xe.type == DestroyNotify) win->tray = 0;
            else if (xe.type == ButtonPress) { evt->type = EVT_TRAY_OPEN; return true; }
            continue;
        }
        switch (xe.type) {
            case ClientMessage:
                if ((Atom)xe.xclient.data.l[0] == win->wm_delete)
                    evt->type = EVT_QUIT;
                break;

            case Expose:
                if (xe.xexpose.count == 0) evt->type = EVT_PAINT;
                break;

            case FocusIn:
            case FocusOut:
                evt->type = EVT_PAINT;
                break;

            case MotionNotify:
                evt->type      = EVT_MOUSE_MOVE;
                evt->mouse_x   = xe.xmotion.x;
                evt->mouse_y   = xe.xmotion.y;
                evt->modifiers = x_modifiers(xe.xmotion.state);
                break;

            case ButtonPress:
                evt->mouse_x   = xe.xbutton.x;
                evt->mouse_y   = xe.xbutton.y;
                evt->modifiers = x_modifiers(xe.xbutton.state);
                if (xe.xbutton.button == Button4 || xe.xbutton.button == Button5) {
                    evt->type        = EVT_MOUSE_WHEEL;
                    evt->wheel_delta = (xe.xbutton.button == Button4) ? 1 : -1;
                } else if (xe.xbutton.button == 6 || xe.xbutton.button == 7) {
                    // rotella orizzontale: trattata come Shift + rotella
                    evt->type        = EVT_MOUSE_WHEEL;
                    evt->wheel_delta = (xe.xbutton.button == 6) ? 1 : -1;
                    evt->modifiers  |= SV_MOD_SHIFT;
                } else {
                    evt->type         = EVT_MOUSE_DOWN;
                    evt->mouse_button = (int)xe.xbutton.button;
                }
                break;

            case ButtonRelease:
                if (xe.xbutton.button >= Button4 && xe.xbutton.button <= 7) break; // rilascio rotella: ignorato
                evt->type         = EVT_MOUSE_UP;
                evt->mouse_x      = xe.xbutton.x;
                evt->mouse_y      = xe.xbutton.y;
                evt->mouse_button = (int)xe.xbutton.button;
                evt->modifiers    = x_modifiers(xe.xbutton.state);
                break;

            case KeyPress: {
                char   buff[16] = {0};
                KeySym sym = 0;
                XLookupString(&xe.xkey, buff, sizeof(buff) - 1, &sym, NULL);
                KeySym base = XLookupKeysym(&xe.xkey, 0);

                evt->type      = EVT_KEY_DOWN;
                evt->modifiers = x_modifiers(xe.xkey.state);

                switch (sym) {
                    case XK_Return:
                    case XK_KP_Enter:     evt->key = KEY_ENTER;     break;
                    case XK_Escape:       evt->key = KEY_ESCAPE;    break;
                    case XK_BackSpace:    evt->key = KEY_BACKSPACE; break;
                    case XK_Delete:
                    case XK_KP_Delete:    evt->key = KEY_DELETE;    break;
                    case XK_Left:
                    case XK_KP_Left:      evt->key = KEY_LEFT;      break;
                    case XK_Right:
                    case XK_KP_Right:     evt->key = KEY_RIGHT;     break;
                    case XK_Up:
                    case XK_KP_Up:        evt->key = KEY_UP;        break;
                    case XK_Down:
                    case XK_KP_Down:      evt->key = KEY_DOWN;      break;
                    case XK_Home:
                    case XK_KP_Home:      evt->key = KEY_HOME;      break;
                    case XK_End:
                    case XK_KP_End:       evt->key = KEY_END;       break;
                    case XK_Page_Up:
                    case XK_KP_Page_Up:   evt->key = KEY_PAGEUP;    break;
                    case XK_Page_Down:
                    case XK_KP_Page_Down: evt->key = KEY_PAGEDOWN;  break;
                    case XK_Tab:
                    case XK_ISO_Left_Tab: evt->key = KEY_TAB;       break;
                    case XK_F1:           evt->key = KEY_F1;        break;
                    case XK_F2:           evt->key = KEY_F2;        break;
                    case XK_F3:           evt->key = KEY_F3;        break;
                    case XK_F4:           evt->key = KEY_F4;        break;
                    case XK_F5:           evt->key = KEY_F5;        break;
                    default:              evt->key = KEY_NONE;      break;
                }

                if (evt->key == KEY_NONE) {
                    if (evt->modifiers & (SV_MOD_CTRL | SV_MOD_ALT)) {
                        // scorciatoia (Ctrl+C, Ctrl+Z, Ctrl+/ ...): lettera "base" del tasto
                        if (base >= 0x20 && base <= 0x7e) {
                            evt->key       = KEY_LETTER;
                            evt->character = (uint32_t)base;
                        } else {
                            evt->type = EVT_NONE;
                        }
                    } else {
                        uint32_t cp = keysym_to_unicode(sym);
                        if (cp >= 0x20 && cp != 0x7f) {
                            evt->type      = EVT_CHAR;
                            evt->character = cp;
                        } else {
                            evt->type = EVT_NONE;
                        }
                    }
                }
                break;
            }

            case ConfigureNotify: {
                // finestra ridimensionata: ricrea il backbuffer
                if (xe.xconfigure.width != win->w || xe.xconfigure.height != win->h) {
                    win->w = xe.xconfigure.width;
                    win->h = xe.xconfigure.height;
                    XFreePixmap(win->display, win->backbuf);
                    win->backbuf = XCreatePixmap(win->display, win->window,
                                                 win->w, win->h, win->depth);
                    XFreeGC(win->display, win->gc_buf);
                    win->gc_buf = XCreateGC(win->display, win->backbuf, 0, NULL);
                    XftDrawChange(win->xft, win->backbuf);
                    win->has_clip = false;
                    apply_clip(win);
                    evt->type       = EVT_RESIZE;
                    evt->new_width  = win->w;
                    evt->new_height = win->h;
                }
                break;
            }

            case SelectionRequest:
                handle_selection_request(win, &xe.xselectionrequest);
                break;

            case SelectionClear:
                if (xe.xselectionclear.selection == win->a_clipboard) {
                    free(win->clip_text);
                    win->clip_text = NULL;
                }
                break;

            default:
                break;
        }

        if (evt->type != EVT_NONE) return true;
    }
    return false;
}

void platform_wait_event(PlatformWindow* win, int timeout_ms) {
    if (XPending(win->display)) return;
    int fd = ConnectionNumber(win->display);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    select(fd + 1, &fds, NULL, NULL, &tv);
}

uint64_t platform_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

// -- disegno --

void platform_clear(PlatformWindow* win, Color c) {
    XSetForeground(win->display, win->gc_buf, to_pixel(c));
    XFillRectangle(win->display, win->backbuf, win->gc_buf, 0, 0, win->w, win->h);
}

void platform_fill_rect(PlatformWindow* win, Rect r, Color c) {
    if (r.w <= 0 || r.h <= 0 || c.a == 0) return;
    if (c.a == 255) {
        XSetForeground(win->display, win->gc_buf, to_pixel(c));
        XFillRectangle(win->display, win->backbuf, win->gc_buf, r.x, r.y, r.w, r.h);
    } else {
        XRenderColor rc = to_render(c);
        XRenderFillRectangle(win->display, PictOpOver, XftDrawPicture(win->xft),
                             &rc, r.x, r.y, r.w, r.h);
    }
}

void platform_draw_rect(PlatformWindow* win, Rect r, Color c, int thickness) {
    if (thickness <= 1) {
        XSetForeground(win->display, win->gc_buf, to_pixel(c));
        XSetLineAttributes(win->display, win->gc_buf, 1, LineSolid, CapButt, JoinMiter);
        XDrawRectangle(win->display, win->backbuf, win->gc_buf, r.x, r.y,
                       r.w > 0 ? r.w - 1 : 0, r.h > 0 ? r.h - 1 : 0);
        return;
    }
    platform_fill_rect(win, (Rect){r.x, r.y, r.w, thickness}, c);
    platform_fill_rect(win, (Rect){r.x, r.y + r.h - thickness, r.w, thickness}, c);
    platform_fill_rect(win, (Rect){r.x, r.y, thickness, r.h}, c);
    platform_fill_rect(win, (Rect){r.x + r.w - thickness, r.y, thickness, r.h}, c);
}

void platform_draw_line(PlatformWindow* win, Point a, Point b, Color c, int thickness) {
    XSetForeground(win->display, win->gc_buf, to_pixel(c));
    XSetLineAttributes(win->display, win->gc_buf,
                       thickness, LineSolid, CapButt, JoinMiter);
    XDrawLine(win->display, win->backbuf, win->gc_buf, a.x, a.y, b.x, b.y);
}

// rettangolo arrotondato con angoli antialiasing (copertura calcolata con 4x4 sotto-campioni)
void platform_fill_rect_rounded(PlatformWindow* win, Rect r, Color c, int radius) {
    if (r.w <= 0 || r.h <= 0) return;
    int rad = radius;
    if (rad > r.w / 2) rad = r.w / 2;
    if (rad > r.h / 2) rad = r.h / 2;
    if (rad <= 0) { platform_fill_rect(win, r, c); return; }

    // corpo centrale
    platform_fill_rect(win, (Rect){r.x, r.y + rad, r.w, r.h - 2 * rad}, c);
    platform_fill_rect(win, (Rect){r.x + rad, r.y, r.w - 2 * rad, rad}, c);
    platform_fill_rect(win, (Rect){r.x + rad, r.y + r.h - rad, r.w - 2 * rad, rad}, c);

    // angoli: per ogni pixel del quadrato rad x rad calcola la copertura del cerchio
    float rr = (float)rad * (float)rad;
    for (int j = 0; j < rad; j++) {
        int full_from = rad; // prima colonna completamente coperta
        for (int i = 0; i < rad; i++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float px = (float)i + (sx + 0.5f) / 4.0f - (float)rad;
                    float py = (float)j + (sy + 0.5f) / 4.0f - (float)rad;
                    if (px * px + py * py <= rr) inside++;
                }
            }
            if (inside == 16) { full_from = i; break; }
            if (inside > 0) {
                Color pc = c;
                pc.a = (uint8_t)(c.a * inside / 16);
                int lx = r.x + i,            rx = r.x + r.w - 1 - i;
                int ty = r.y + j,            by = r.y + r.h - 1 - j;
                platform_fill_rect(win, (Rect){lx, ty, 1, 1}, pc);
                platform_fill_rect(win, (Rect){rx, ty, 1, 1}, pc);
                platform_fill_rect(win, (Rect){lx, by, 1, 1}, pc);
                platform_fill_rect(win, (Rect){rx, by, 1, 1}, pc);
            }
        }
        int run = rad - full_from;
        if (run > 0) {
            platform_fill_rect(win, (Rect){r.x + full_from, r.y + j, run, 1}, c);
            platform_fill_rect(win, (Rect){r.x + r.w - rad, r.y + j, run, 1}, c);
            platform_fill_rect(win, (Rect){r.x + full_from, r.y + r.h - 1 - j, run, 1}, c);
            platform_fill_rect(win, (Rect){r.x + r.w - rad, r.y + r.h - 1 - j, run, 1}, c);
        }
    }
}

void platform_set_clip(PlatformWindow* win, Rect r) {
    win->has_clip = true;
    win->clip = r;
    apply_clip(win);
}

void platform_reset_clip(PlatformWindow* win) {
    win->has_clip = false;
    apply_clip(win);
}

// -- testo --

int platform_draw_text_ex(PlatformWindow* win, const char* text, int len, Point pos, Color c, int size, FontStyle style) {
    if (!text) return 0;
    if (len < 0) len = (int)strlen(text);
    if (len == 0) return 0;
    XftFont* f = get_font(win, size, style);
    if (!f) return 0;

    XftColor xc;
    XRenderColor rc = { (unsigned short)(c.r * 257), (unsigned short)(c.g * 257),
                        (unsigned short)(c.b * 257), (unsigned short)(c.a * 257) };
    XftColorAllocValue(win->display, win->visual, win->colormap, &rc, &xc);
    XftDrawStringUtf8(win->xft, &xc, f, pos.x, pos.y + f->ascent, (const FcChar8*)text, len);
    XftColorFree(win->display, win->visual, win->colormap, &xc);

    XGlyphInfo ext;
    XftTextExtentsUtf8(win->display, f, (const FcChar8*)text, len, &ext);
    return ext.xOff;
}

int platform_measure_text_ex(PlatformWindow* win, const char* text, int len, int size, FontStyle style) {
    if (!text) return 0;
    if (len < 0) len = (int)strlen(text);
    if (len == 0) return 0;
    XftFont* f = get_font(win, size, style);
    if (!f) return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(win->display, f, (const FcChar8*)text, len, &ext);
    return ext.xOff;
}

int platform_text_height(PlatformWindow* win, int size, FontStyle style) {
    XftFont* f = get_font(win, size, style);
    return f ? f->ascent + f->descent : size;
}

int platform_draw_text(PlatformWindow *win, const char *text, Point pos, Color c, int size) {
    return platform_draw_text_ex(win, text, -1, pos, c, size, FONT_UI);
}

int platform_measure_text(PlatformWindow* win, const char* text, int size) {
    return platform_measure_text_ex(win, text, -1, size, FONT_UI);
}

// componente di un pixel secondo la maschera del visual (0..255)
static uint32_t mask_channel(unsigned long px, unsigned long mask) {
    if (!mask) return 0;
    int shift = 0;
    while (!((mask >> shift) & 1)) shift++;
    unsigned long v = (px & mask) >> shift, max = mask >> shift;
    return (uint32_t)(v * 255 / max);
}

bool platform_read_pixels(PlatformWindow* win, Rect r, uint32_t* out) {
    if (!win || !out || r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > win->w || r.y + r.h > win->h) return false;
    XImage* img = XGetImage(win->display, win->backbuf, r.x, r.y, (unsigned)r.w, (unsigned)r.h, AllPlanes, ZPixmap);
    if (!img) return false;
    unsigned long rm = img->red_mask, gm = img->green_mask, bm = img->blue_mask;
    if (!rm) { rm = win->visual->red_mask; gm = win->visual->green_mask; bm = win->visual->blue_mask; }
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            unsigned long px = XGetPixel(img, x, y);
            out[y * r.w + x] = mask_channel(px, rm) << 16 | mask_channel(px, gm) << 8 | mask_channel(px, bm);
        }
    XDestroyImage(img);
    return true;
}

void platform_present(PlatformWindow *win) {
    // copio il backbuffer sulla finestra principale (equivalente a BitBlt)
    XCopyArea(win->display, win->backbuf, win->window, win->gc,
              0, 0, win->w, win->h, 0, 0);
    // XSync (non solo XFlush): aspetta che il server abbia eseguito il disegno. Su un server
    // lento (desktop remoto, X senza accelerazione) altrimenti i fotogrammi si accumulano e
    // l'interfaccia resta indietro di secondi rispetto all'input
    XSync(win->display, False);
}

void platform_draw_bitmap(PlatformWindow* win, Rect dest, uint32_t* pixels, int pw, int ph) {
    // Stub: da implementare con XPutImage se necessario
    (void)win; (void)dest; (void)pixels; (void)pw; (void)ph;
}

// -- cursore --

void platform_set_cursor(PlatformWindow* win, CursorKind kind) {
    if (kind < 0 || kind >= CURSOR_COUNT || kind == win->cur_cursor) return;
    win->cur_cursor = kind;
    XDefineCursor(win->display, win->window, win->cursors[kind]);
    XFlush(win->display);
}

// -- appunti --

bool platform_open_url(const char* url) {
    // solo pagine web (o il server locale di SegVault), mai comandi o file
    if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://127.0.0.1:", 17) && strncmp(url, "http://localhost:", 17)))
        return false;
    for (const char* c = url; *c; c++) if ((unsigned char)*c <= ' ' || *c == '"' || *c == '\'') return false;
    // doppio fork: il browser non resta figlio dell'app (niente processi zombie, nessuna attesa)
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", url, (char*)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    return true;
}

bool platform_open_file(const char* path) {
    struct stat st;
    if (!path || path[0] != '/' || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    for (const char* c = path; *c; c++) if ((unsigned char)*c < ' ') return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", path, (char*)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int st2;
    waitpid(pid, &st2, 0);
    return true;
}

// ── finestra Apri / Salva: zenity (GNOME, XFCE, ...) o kdialog (KDE) ──

static bool in_path(const char* prog, char* full, size_t fn) {
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char* e = strchr(path, ':');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        if (n) {
            snprintf(full, fn, "%.*s/%s", (int)n, path, prog);
            if (access(full, X_OK) == 0) return true;
        }
        path += n + (e ? 1 : 0);
    }
    return false;
}

// esegue argv e legge stdout (prima riga): 1 = uscita 0 con un percorso, 0 = annullato, -1 = non parte
static int run_chooser(char* const* argv, char* out, size_t outsz) {
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        dup2(fd[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        close(fd[0]); close(fd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    size_t got = 0;
    char buf[4096];
    for (;;) {
        ssize_t r = read(fd[0], buf + got, sizeof(buf) - 1 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
        if (got >= sizeof(buf) - 1) break;
    }
    buf[got] = '\0';
    close(fd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st) == 127) return -1;
    char* nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (WEXITSTATUS(st) != 0 || !buf[0]) return 0;
    snprintf(out, outsz, "%s", buf);
    return 1;
}

int platform_file_dialog(PlatformWindow* win, FileDialogKind kind, const char* title, const char* filter,
                         const char* initial, char* out, size_t outsz) {
    (void)win;
    if (!out || !outsz) return -1;
    out[0] = '\0';
    // "SQL files|*.sql;*.txt" -> nome e modelli separati da spazi
    char fname[128] = "", fpat[256] = "";
    if (filter && *filter) {
        const char* bar = strchr(filter, '|');
        snprintf(fname, sizeof(fname), "%.*s", bar ? (int)(bar - filter) : (int)strlen(filter), filter);
        snprintf(fpat, sizeof(fpat), "%s", bar ? bar + 1 : "*");
        for (char* c = fpat; *c; c++) if (*c == ';') *c = ' ';
    }
    char start[1200];
    snprintf(start, sizeof(start), "%s", initial && *initial ? initial : (getenv("HOME") ? getenv("HOME") : "/"));
    struct stat st;
    size_t sl = strlen(start);
    if (stat(start, &st) == 0 && S_ISDIR(st.st_mode) && sl && start[sl - 1] != '/' && sl + 1 < sizeof(start)) {
        start[sl] = '/'; start[sl + 1] = '\0';                  // zenity: cartella di partenza solo con la '/'
    }
    const char* t = title && *title ? title : (kind == FILE_DIALOG_SAVE ? "Save as" : "Open");
    char prog[1100];
    const char* desk = getenv("XDG_CURRENT_DESKTOP");
    bool kde = desk && strstr(desk, "KDE");
    for (int pass = 0; pass < 2; pass++) {
        bool use_kdialog = (pass == 0) == kde;
        char* argv[16];
        int a = 0;
        char a_title[300], a_file[1300], a_f1[400], a_f2[64];
        if (!use_kdialog) {
            if (!in_path("zenity", prog, sizeof(prog)) && !in_path("qarma", prog, sizeof(prog))) continue;
            argv[a++] = prog;
            argv[a++] = "--file-selection";
            snprintf(a_title, sizeof(a_title), "--title=%s", t);
            argv[a++] = a_title;
            if (kind == FILE_DIALOG_SAVE) argv[a++] = "--save";
            snprintf(a_file, sizeof(a_file), "--filename=%s", start);
            argv[a++] = a_file;
            if (fpat[0]) {
                snprintf(a_f1, sizeof(a_f1), "--file-filter=%s | %s", fname[0] ? fname : "Files", fpat);
                argv[a++] = a_f1;
                snprintf(a_f2, sizeof(a_f2), "--file-filter=All files | *");
                argv[a++] = a_f2;
            }
        } else {
            if (!in_path("kdialog", prog, sizeof(prog))) continue;
            argv[a++] = prog;
            argv[a++] = "--title";
            snprintf(a_title, sizeof(a_title), "%s", t);
            argv[a++] = a_title;
            argv[a++] = kind == FILE_DIALOG_SAVE ? "--getsavefilename" : "--getopenfilename";
            snprintf(a_file, sizeof(a_file), "%s", start);
            argv[a++] = a_file;
            if (fpat[0]) {
                snprintf(a_f1, sizeof(a_f1), "%s|%s\n*|All files", fpat, fname[0] ? fname : "Files");
                argv[a++] = a_f1;
            }
        }
        argv[a] = NULL;
        int r = run_chooser(argv, out, outsz);
        if (r >= 0) return r;
    }
    return -1;
}

void platform_set_clipboard(PlatformWindow* win, const char* text) {
    if (!text) return;
    free(win->clip_text);
    win->clip_text = strdup(text);
    XSetSelectionOwner(win->display, win->a_clipboard, win->window, CurrentTime);
    XFlush(win->display);
}

char* platform_get_clipboard(PlatformWindow* win) {
    Window owner = XGetSelectionOwner(win->display, win->a_clipboard);
    if (owner == win->window && win->clip_text) return strdup(win->clip_text);
    if (owner == None) return NULL;

    XConvertSelection(win->display, win->a_clipboard, win->a_utf8, win->a_prop,
                      win->window, CurrentTime);
    XFlush(win->display);

    // attende la risposta (max ~1 s) senza bloccare per sempre
    uint64_t start = platform_time_ms();
    XEvent ev;
    bool got = false;
    while (platform_time_ms() - start < 1000) {
        if (XCheckTypedWindowEvent(win->display, win->window, SelectionNotify, &ev)) {
            got = true;
            break;
        }
        if (XCheckTypedEvent(win->display, SelectionRequest, &ev))
            handle_selection_request(win, &ev.xselectionrequest);
        platform_wait_event(win, 10);
    }
    if (!got || ev.xselection.property == None) return NULL;

    Atom type; int format; unsigned long nitems, after;
    unsigned char* data = NULL;
    char* out = NULL;
    if (XGetWindowProperty(win->display, win->window, win->a_prop, 0, 1 << 24, True,
                           AnyPropertyType, &type, &format, &nitems, &after, &data) == Success) {
        if (data && format == 8) {
            out = malloc(nitems + 1);
            if (out) {
                memcpy(out, data, nitems);
                out[nitems] = '\0';
            }
        }
        if (data) XFree(data);
    }
    return out;
}

// -- cleanup --

void platform_window_destroy(PlatformWindow* win) {
    if (!win) return;
    platform_tray_disable(win);
    for (int i = 0; i < win->num_fonts; i++)
        XftFontClose(win->display, win->fonts[i].font);
    for (int i = 0; i < CURSOR_COUNT; i++)
        if (win->cursors[i]) XFreeCursor(win->display, win->cursors[i]);
    if (win->xft) XftDrawDestroy(win->xft);
    XFreePixmap(win->display, win->backbuf);
    XFreeGC(win->display, win->gc);
    XFreeGC(win->display, win->gc_buf);
    XDestroyWindow(win->display, win->window);
    XCloseDisplay(win->display);
    free(win->clip_text);
    free(win);
}

void platform_shutdown(void) {
    // Niente da fare a livello globale su Xlib
}

// -- thread --

struct PlatformThread {
    pthread_t id;
    void (*fn)(void*);
    void* arg;
};

static void* thread_entry(void* p) {
    PlatformThread* t = (PlatformThread*)p;
    t->fn(t->arg);
    return NULL;
}

PlatformThread* platform_thread_start(void (*fn)(void* arg), void* arg) {
    PlatformThread* t = calloc(1, sizeof(PlatformThread));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;                                   // stack 16 MB per il motore (query molto annidate)
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)16 << 20);
    int rc = pthread_create(&t->id, &attr, thread_entry, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return NULL; }
    return t;
}

void platform_thread_join(PlatformThread* t) {
    if (!t) return;
    pthread_join(t->id, NULL);
    free(t);
}

// finestrella con il messaggio: si chiude con un click, un tasto o la X della finestra
void platform_error_box(const char* title, const char* message) {
    fprintf(stderr, "[%s] %s\n", title, message);
    Display* d = XOpenDisplay(NULL);
    if (!d) return;
    int scr = DefaultScreen(d);
    int w = 520, h = 130;
    Window win = XCreateSimpleWindow(d, RootWindow(d, scr), 100, 100, (unsigned)w, (unsigned)h, 1,
                                     BlackPixel(d, scr), WhitePixel(d, scr));
    XStoreName(d, win, title);
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, win, &del, 1);
    XSelectInput(d, win, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(d, win);
    GC gc = XCreateGC(d, win, 0, NULL);
    XSetForeground(d, gc, BlackPixel(d, scr));
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose) {
            XDrawString(d, win, gc, 20, 40, title, (int)strlen(title));
            XDrawString(d, win, gc, 20, 70, message, (int)strlen(message));
            const char* hint = "Click or press a key to close.";
            XDrawString(d, win, gc, 20, 105, hint, (int)strlen(hint));
        } else if (e.type == KeyPress || e.type == ButtonPress ||
                   (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == del)) {
            break;
        }
    }
    XFreeGC(d, gc);
    XDestroyWindow(d, win);
    XCloseDisplay(d);
}
