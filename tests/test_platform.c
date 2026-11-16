/*
SCOPO: test della piattaforma X11 (src/platform/xlib.c) senza mostrare la finestra:
       - testo dal metodo di input: è, ñ, ő col tasto morto (dead_doubleacute + o) arrivano come EVT_CHAR
         con il codepoint giusto (non byte Latin-1);
       - scala dell'interfaccia: a 200 % le misure restano logiche (stessa larghezza di 100 % ±1 px) e il
         testo viene disegnato a dimensione doppia (pixel fisici), mouse e resize tornano logici.
       Senza display X (CI) il test si salta.
*/
#include "../src/platform/xlib.c"
#include <X11/XKBlib.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// stato dei modificatori per ottenere il keysym `ks` dal suo keycode (livello 1-4 del gruppo 1)
static bool key_for(Display* d, KeySym ks, KeyCode* kc, unsigned* state) {
    *kc = XKeysymToKeycode(d, ks);
    if (!*kc) return false;
    unsigned l3 = 0;
    KeyCode l3kc = XKeysymToKeycode(d, XK_ISO_Level3_Shift);
    XModifierKeymap* mm = XGetModifierMapping(d);
    for (int m = 0; m < 8 && l3kc; m++)
        for (int k = 0; k < mm->max_keypermod; k++)
            if (mm->modifiermap[m * mm->max_keypermod + k] == l3kc) l3 = 1u << m;
    XFreeModifiermap(mm);
    unsigned try_states[4] = {0, ShiftMask, l3, l3 | ShiftMask};
    for (int i = 0; i < 4; i++) {
        if (i >= 2 && !l3) break;
        if (XkbKeycodeToKeysym(d, *kc, 0, i) == ks) { *state = try_states[i]; return true; }
    }
    return false;
}

static void send_key(PlatformWindow* w, KeyCode kc, unsigned state) {
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xkey.type = KeyPress;
    e.xkey.display = w->display;
    e.xkey.window = w->window;
    e.xkey.root = RootWindow(w->display, w->screen);
    e.xkey.subwindow = None;
    e.xkey.time = CurrentTime;
    e.xkey.same_screen = True;
    e.xkey.keycode = kc;
    e.xkey.state = state;
    XSendEvent(w->display, w->window, True, KeyPressMask, &e);
    e.xkey.type = KeyRelease;
    XSendEvent(w->display, w->window, True, KeyReleaseMask, &e);
    XSync(w->display, False);
}

// raccoglie i caratteri arrivati (al massimo n)
static int collect(PlatformWindow* w, uint32_t* out, int n) {
    int k = 0;
    sEvent ev;
    for (int tries = 0; tries < 20; tries++) {
        XSync(w->display, False);
        while (platform_poll_event(w, &ev))
            if (ev.type == EVT_CHAR && k < n) out[k++] = ev.character;
        if (k >= n) break;
        usleep(5000);
    }
    return k;
}

static void test_input(PlatformWindow* w) {
    CHECK(w->xic != NULL, "no input context (XIM) created");
    struct { const char* what; KeySym seq[2]; int nseq; uint32_t want; } cases[] = {
        { "egrave",            { XK_egrave }, 1, 0x00e8 },
        { "ntilde",            { XK_ntilde }, 1, 0x00f1 },
        { "dead_doubleacute o", { XK_dead_doubleacute, XK_o }, 2, 0x0151 },
        { "dead_acute e",      { XK_dead_acute, XK_e }, 2, 0x00e9 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KeyCode kc[2]; unsigned st[2]; bool ok = true;
        for (int j = 0; j < cases[i].nseq; j++) ok = ok && key_for(w->display, cases[i].seq[j], &kc[j], &st[j]);
        if (!ok) { printf("  skip %s (not in this keyboard layout)\n", cases[i].what); continue; }
        for (int j = 0; j < cases[i].nseq; j++) send_key(w, kc[j], st[j]);
        uint32_t got[4] = {0};
        int n = collect(w, got, 1);
        CHECK(n == 1 && got[0] == cases[i].want, "%s: got %d chars, U+%04X (want U+%04X)", cases[i].what, n,
              n ? got[0] : 0, cases[i].want);
        uint32_t extra[4];
        CHECK(collect(w, extra, 4) == 0, "%s: unexpected extra characters", cases[i].what);
    }
    // testo composto lungo (CJK da un IME): la coda consegna ogni codepoint come EVT_CHAR, in ordine
    const char* cjk = "\xe6\xbc\xa2\xe5\xad\x97 ok";     // 漢字 ok
    w->npending = utf8_to_cps(cjk, (int)strlen(cjk), w->pending, 64);
    w->pending_pos = 0;
    uint32_t got[8] = {0};
    int n = collect(w, got, 5);
    CHECK(n == 5 && got[0] == 0x6f22 && got[1] == 0x5b57 && got[2] == ' ' && got[3] == 'o' && got[4] == 'k',
          "queued IME text: %d chars", n);
    // UTF-8 non valido: saltato, mai byte singoli Latin-1
    uint32_t cps[8];
    CHECK(utf8_to_cps("a\xff\xc3(b", 5, cps, 8) == 3 && cps[0] == 'a' && cps[1] == '(' && cps[2] == 'b',
          "invalid UTF-8 skipped");
}

static void test_scale(PlatformWindow* w) {
    const char* txt = "SELECT name FROM users";
    platform_set_ui_scale(w, 1.0f);
    sEvent ev; while (platform_poll_event(w, &ev)) {}
    int w1 = platform_measure_text_ex(w, txt, -1, 13, FONT_UI);
    int h1 = platform_text_height(w, 13, FONT_UI);
    platform_set_ui_scale(w, 2.0f);
    CHECK(platform_ui_scale(w) == 2.0f, "scale not applied");
    bool resized = false;
    for (int i = 0; i < 20 && !resized; i++) {
        XSync(w->display, False);
        while (platform_poll_event(w, &ev)) if (ev.type == EVT_RESIZE) resized = true;
        if (!resized) usleep(5000);
    }
    CHECK(resized, "no EVT_RESIZE after the scale change");
    int w2 = platform_measure_text_ex(w, txt, -1, 13, FONT_UI);
    int h2 = platform_text_height(w, 13, FONT_UI);
    CHECK(abs(w2 - w1) <= 2 && abs(h2 - h1) <= 1, "logical metrics changed: %d/%d -> %d/%d", w1, h1, w2, h2);
    // il font fisico è a 26 px: la larghezza fisica è il doppio della logica (testo nitido, non ingrandito)
    XftFont* f = get_font(w, SC(w, 13), FONT_UI);
    XGlyphInfo ext;
    XftTextExtentsUtf8(w->display, f, (const FcChar8*)txt, (int)strlen(txt), &ext);
    CHECK(abs(ext.xOff - 2 * w2) <= 3, "physical width %d is not 2 x %d", ext.xOff, w2);
    // un rettangolo logico 10x10 a (5,5) copre 20x20 pixel fisici da (10,10)
    platform_clear(w, (Color){0, 0, 0, 255});
    platform_fill_rect(w, (Rect){5, 5, 10, 10}, (Color){255, 255, 255, 255});
    XImage* img = XGetImage(w->display, w->backbuf, 0, 0, 40, 40, AllPlanes, ZPixmap);
    CHECK(img != NULL, "XGetImage");
    if (img) {
        unsigned long white = XGetPixel(img, 10, 10), in2 = XGetPixel(img, 29, 29);
        unsigned long out1 = XGetPixel(img, 9, 9), out2 = XGetPixel(img, 30, 30);
        CHECK(white == in2 && white != out1 && out1 == out2, "scaled rect covers the wrong pixels");
        XDestroyImage(img);
    }
    // lettura dei pixel in coordinate logiche
    uint32_t px[4];
    CHECK(platform_read_pixels(w, (Rect){4, 5, 2, 2}, px) && px[0] == 0 && (px[1] & 0xffffff) == 0xffffff,
          "read_pixels in logical units");
    // il mouse torna in pixel logici
    XEvent e; memset(&e, 0, sizeof(e));
    e.xbutton.type = ButtonPress; e.xbutton.display = w->display; e.xbutton.window = w->window;
    e.xbutton.x = 200; e.xbutton.y = 100; e.xbutton.button = Button1; e.xbutton.same_screen = True;
    XSendEvent(w->display, w->window, True, ButtonPressMask, &e);
    bool got = false;
    for (int i = 0; i < 20 && !got; i++) {
        XSync(w->display, False);
        while (platform_poll_event(w, &ev))
            if (ev.type == EVT_MOUSE_DOWN) { got = true; CHECK(ev.mouse_x == 100 && ev.mouse_y == 50, "mouse %d,%d", ev.mouse_x, ev.mouse_y); }
        if (!got) usleep(5000);
    }
    CHECK(got, "no mouse event");
    platform_set_ui_scale(w, 1.0f);
}

int main(void) {
    Display* probe = XOpenDisplay(NULL);
    if (!probe) { printf("test_platform: no X display, skipped\n"); return 0; }
    XCloseDisplay(probe);
    if (!platform_init()) { printf("FAIL platform_init\n"); return 1; }
    PlatformWindow* w = platform_window_create("SegVault test", 400, 300);   // mai mostrata
    if (!w) { printf("FAIL window\n"); return 1; }
    test_input(w);
    test_scale(w);
    platform_window_destroy(w);
    printf("test_platform: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
