/* 
SCOPO: Implementa platform.h usando le API native Windows.
       Compilato SOLO su Windows (vedi Makefile).
       Usa GDI (Graphics Device Interface) per il disegno.
*/
#include <windows.h>   // API Win32 completa
#include <stdio.h>
#include "platform.h"

// Struttura interna della finestra Win32
// Nascosta all'esterno: fuori da questo file si vede solo PlatformWindow*
struct PlatformWindow {
    HWND    hwnd;        // Handle alla finestra Win32
    HDC     hdc;         // Device context per disegnare
    HDC     hdc_mem;     // DC in memoria per il double buffering
    HBITMAP hbm_mem;     // Bitmap del backbuffer
    int     width;
    int     height;
    sEvent   pending_evt; // Evento in attesa dal WndProc
    bool    has_event;   // true se c'è un evento da processare
};

// puntatore globale alla finestra corrente (necessario per passare eventi dal WndProc alla poll_event)
static PlatformWindow* g_win = NULL;

// window procedure: chiamata da Windows per OGNI messaggio
// Traduce i messaggi Win32 negli Event di platform.h
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_win) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_DESTROY:
            // Utente ha premuto la X: genera EVT_QUIT
            g_win->pending_evt.type = EVT_QUIT;
            g_win->has_event = true;
            PostQuitMessage(0);
            break;

        case WM_MOUSEMOVE:
            // Mouse si è mosso: estrae coordinate X,Y dal lParam
            g_win->pending_evt.type    = EVT_MOUSE_MOVE;
            g_win->pending_evt.mouse_x = LOWORD(lp); // X nei 16 bit bassi
            g_win->pending_evt.mouse_y = HIWORD(lp); // Y nei 16 bit alti
            g_win->has_event = true;
            break;

        case WM_LBUTTONDOWN:
            g_win->pending_evt.type         = EVT_MOUSE_DOWN;
            g_win->pending_evt.mouse_x      = LOWORD(lp);
            g_win->pending_evt.mouse_y      = HIWORD(lp);
            g_win->pending_evt.mouse_button = 1; // 1 = tasto sinistro
            g_win->has_event = true;
            break;

        case WM_LBUTTONUP:
            g_win->pending_evt.type         = EVT_MOUSE_UP;
            g_win->pending_evt.mouse_x      = LOWORD(lp);
            g_win->pending_evt.mouse_y      = HIWORD(lp);
            g_win->pending_evt.mouse_button = 1;
            g_win->has_event = true;
            break;

        case WM_CHAR:
            // carattere unicode digitato (per le textbox SQL)
            g_win->pending_evt.type      = EVT_CHAR;
            g_win->pending_evt.character = (uint32_t)wp;
            g_win->has_event = true;
            break;

        case WM_SIZE:
            // finestra ridimensionata: aggiorna il backbuffer
            g_win->width  = LOWORD(lp);
            g_win->height = HIWORD(lp);

            // ricrea il backbuffer con le nuove dimensioni
            if (g_win->hbm_mem) DeleteObject(g_win->hbm_mem);

            g_win->hbm_mem = CreateCompatibleBitmap(g_win->hdc, g_win->width, g_win->height);
            
            SelectObject(g_win->hdc_mem, g_win->hbm_mem);
            
            g_win->pending_evt.type       = EVT_RESIZE;
            g_win->pending_evt.new_width  = g_win->width;
            g_win->pending_evt.new_height = g_win->height;
            g_win->has_event = true;
            
            break;

        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

bool platform_init(void) {
    // niente da inizializzare a livello globale su Win32
    return true;
}

PlatformWindow* platform_window_create(const char* title, int w, int h) {
    PlatformWindow* win = calloc(1, sizeof(PlatformWindow));
    
    win->width  = w;
    win->height = h;
    g_win = win;

    // registra la classe finestra
    WNDCLASSA wc    = {0};
    wc.lpfnWndProc  = wnd_proc;          // la WndProc
    wc.hInstance    = GetModuleHandleA(NULL);
    wc.lpszClassName= "MyDBApp";
    wc.hCursor      = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    // crea la finestra (inizialmente nascosta: WS_OVERLAPPEDWINDOW)
    win->hwnd = CreateWindowA(
        "MyDBApp", title,
        WS_OVERLAPPEDWINDOW,             // stile: ridimensionabile
        CW_USEDEFAULT, CW_USEDEFAULT,    // posizione: decide Windows
        w, h,
        NULL, NULL,
        GetModuleHandleA(NULL), NULL
    );

    // crea il double buffer (evita flickering durante il ridisegno)
    win->hdc     = GetDC(win->hwnd);     // DC della finestra reale
    win->hdc_mem = CreateCompatibleDC(win->hdc); // DC in memoria
    win->hbm_mem = CreateCompatibleBitmap(win->hdc, w, h);
    SelectObject(win->hdc_mem, win->hbm_mem); // collega bitmap al DC

    return win;
}

void platform_window_show(PlatformWindow* win) {
    ShowWindow(win->hwnd, SW_SHOW); // rende visibile la finestra
    UpdateWindow(win->hwnd);        // forza il primo WM_PAINT
}

bool platform_poll_event(PlatformWindow* win, sEvent* evt) {
    MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT); // aspetta un messaggio invece di girare a vuoto
    
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (win->has_event) {
        *evt = win->pending_evt;
        win->has_event = false;
        return true;
    }
    return false;
}

void platform_clear(PlatformWindow* win, Color c) {
    // Crea un pennello con il colore richiesto e riempi tutto
    HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
    RECT r = {0, 0, win->width, win->height};
    FillRect(win->hdc_mem, &r, brush); // Disegna sul backbuffer
    DeleteObject(brush);               // Libera il pennello GDI
}

void platform_fill_rect(PlatformWindow* win, Rect r, Color c) {
    HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
    RECT wr = {r.x, r.y, r.x + r.w, r.y + r.h};
    FillRect(win->hdc_mem, &wr, brush);
    DeleteObject(brush);
}

int platform_draw_text(PlatformWindow* win, const char* text, Point pos, Color c, int size) {
    // seleziona un font di sistema proporzionale
    HFONT font = CreateFontA(
        size, 0, 0, 0,
        FW_NORMAL,          // peso: normale (FW_BOLD per grassetto)
        FALSE, FALSE, FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,  // antialiasing testo
        DEFAULT_PITCH,
        "Segoe UI"          // font di sistema Windows
    );
    HFONT old_font = SelectObject(win->hdc_mem, font);

    SetTextColor(win->hdc_mem, RGB(c.r, c.g, c.b));
    SetBkMode(win->hdc_mem, TRANSPARENT); // sfondo testo trasparente

    // misura la larghezza prima di disegnare
    SIZE sz;
    GetTextExtentPoint32A(win->hdc_mem, text, strlen(text), &sz);
    TextOutA(win->hdc_mem, pos.x, pos.y, text, strlen(text));

    SelectObject(win->hdc_mem, old_font);
    DeleteObject(font);
    return sz.cx; // ritorna larghezza in pixel
}

void platform_present(PlatformWindow* win) {
    // copio il backbuffer (hdc_mem) sulla finestra reale (hdc)
    BitBlt(
        win->hdc,                // destinazione: finestra reale
        0, 0,                    // posizione destinazione
        win->width, win->height, // dimensioni da copiare
        win->hdc_mem,            // sorgente: backbuffer
        0, 0,                    // posizione sorgente
        SRCCOPY
    );                // operazione: copia diretta
}

void platform_draw_rect(PlatformWindow* win, Rect r, Color c, int thickness) {
    HPEN pen = CreatePen(PS_SOLID, thickness, RGB(c.r, c.g, c.b));
    HPEN old = SelectObject(win->hdc_mem, pen);
    HBRUSH old_brush = SelectObject(win->hdc_mem, GetStockObject(NULL_BRUSH));
    Rectangle(win->hdc_mem, r.x, r.y, r.x + r.w, r.y + r.h);
    SelectObject(win->hdc_mem, old);
    SelectObject(win->hdc_mem, old_brush);
    DeleteObject(pen);
}

void platform_draw_line(PlatformWindow* win, Point a, Point b, Color c, int thickness) {
    HPEN pen = CreatePen(PS_SOLID, thickness, RGB(c.r, c.g, c.b));
    HPEN old = SelectObject(win->hdc_mem, pen);
    MoveToEx(win->hdc_mem, a.x, a.y, NULL);
    LineTo(win->hdc_mem, b.x, b.y);
    SelectObject(win->hdc_mem, old);
    DeleteObject(pen);
}

int platform_measure_text(PlatformWindow* win, const char* text, int size) {
    HFONT font = CreateFontA(
        size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI"
    );
    HFONT old = SelectObject(win->hdc_mem, font);
    SIZE sz;
    GetTextExtentPoint32A(win->hdc_mem, text, strlen(text), &sz);
    SelectObject(win->hdc_mem, old);
    DeleteObject(font);
    return sz.cx;
}

void platform_fill_rect_rounded(PlatformWindow* win, Rect r, Color c, int radius) {
    HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
    HBRUSH old = SelectObject(win->hdc_mem, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(c.r, c.g, c.b));
    HPEN old_pen = SelectObject(win->hdc_mem, pen);
    RoundRect(win->hdc_mem, r.x, r.y, r.x + r.w, r.y + r.h,
              radius * 2, radius * 2);
    SelectObject(win->hdc_mem, old);
    SelectObject(win->hdc_mem, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void platform_draw_bitmap(PlatformWindow* win, Rect dest,
                           uint32_t* pixels, int pw, int ph) {
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = pw;
    bmi.bmiHeader.biHeight      = -ph; // negativo = top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(win->hdc_mem,
        dest.x, dest.y, dest.w, dest.h,
        0, 0, pw, ph,
        pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

void platform_window_destroy(PlatformWindow* win) {
    if (!win) return;
    DeleteObject(win->hbm_mem);
    DeleteDC(win->hdc_mem);
    ReleaseDC(win->hwnd, win->hdc);
    DestroyWindow(win->hwnd);
    free(win);
}

void platform_shutdown(void) {
    // Niente da fare a livello globale su Win32
}
