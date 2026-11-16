/*
SCOPO: Implementa platform.h usando le API native Windows.
       Compilato SOLO su Windows (vedi Makefile).
       Usa GDI (Graphics Device Interface) per il disegno.
       Il testo è UTF-8 lato widget e viene convertito in UTF-16 per GDI.
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fonts_embed.h"
#include <windows.h>   // API Win32 completa
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>  // ShellExecuteA (platform_open_url)
#include <commdlg.h>   // GetOpenFileNameA / GetSaveFileNameA
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "platform.h"

#define EVT_QUEUE_CAP  512
#define FONT_CACHE_CAP 32

typedef struct {
    FontStyle style;
    int       size;
    HFONT     font;
    int       height;  // tmHeight
} FontEntry;

// Struttura interna della finestra Win32
// Nascosta all'esterno: fuori da questo file si vede solo PlatformWindow*
struct PlatformWindow {
    HWND    hwnd;        // Handle alla finestra Win32
    HDC     hdc;         // Device context per disegnare
    HDC     hdc_mem;     // DC in memoria per il double buffering
    HBITMAP hbm_mem;     // Bitmap del backbuffer
    int     width;
    int     height;

    // coda eventi (il WndProc può generare più eventi per ogni giro di PeekMessage)
    sEvent  queue[EVT_QUEUE_CAP];
    int     q_head, q_tail;

    FontEntry fonts[FONT_CACHE_CAP];
    int       num_fonts;

    // pixel 1x1 per riempimenti con trasparenza (GdiAlphaBlend)
    HDC      hdc_px;
    HBITMAP  hbm_px;
    uint32_t* px;

    HCURSOR    cursors[CURSOR_COUNT];
    CursorKind cur_cursor;

    WCHAR high_surrogate;

    // icona nell'area di notifica
    bool            tray_on, tray_active;
    NOTIFYICONDATAA nid;
    HICON           tray_icons[2];          // spento / acceso
    bool            visible;

    // scala dell'interfaccia: i widget lavorano in pixel logici (96 dpi), qui si moltiplica per scale
    float   scale;
    float   scale_req;   // 0 = dpi del monitor (aggiornato da WM_DPICHANGED), altrimenti da Settings
    UINT    dpi;
};

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static inline int SC(const PlatformWindow* w, int v) { return (int)lroundf((float)v * w->scale); }
static inline int UNSC(const PlatformWindow* w, int v) { return (int)lroundf((float)v / w->scale); }
static inline Rect SCR(const PlatformWindow* w, Rect r) {
    int x0 = SC(w, r.x), y0 = SC(w, r.y), x1 = SC(w, r.x + r.w), y1 = SC(w, r.y + r.h);
    return (Rect){x0, y0, x1 - x0, y1 - y0};
}

// per-monitor DPI v2 (Windows 10 1703+), poi v1 (8.1), poi system-aware (Vista): chiamate risolte a
// runtime, così l'eseguibile parte anche dove non esistono. Senza, Windows ingrandisce la bitmap (sfocata)
static void enable_dpi_awareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    SetCtx set_ctx = u ? (SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (set_ctx && set_ctx((HANDLE)(INT_PTR)-4)) return;          // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    typedef HRESULT (WINAPI *SetAw)(int);
    SetAw set_aw = sh ? (SetAw)(void*)GetProcAddress(sh, "SetProcessDpiAwareness") : NULL;
    if (set_aw && set_aw(2) == S_OK) return;                      // PROCESS_PER_MONITOR_DPI_AWARE
    typedef BOOL (WINAPI *SetOld)(void);
    SetOld set_old = u ? (SetOld)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL;
    if (set_old) set_old();
}

static UINT window_dpi(HWND hwnd) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *GetDpi)(HWND);
    GetDpi get = u ? (GetDpi)(void*)GetProcAddress(u, "GetDpiForWindow") : NULL;
    UINT d = (get && hwnd) ? get(hwnd) : 0;
    if (!d) {
        HDC dc = GetDC(NULL);
        d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(NULL, dc);
    }
    return d ? d : 96;
}

static float scale_for(const PlatformWindow* win) {
    float s = win->scale_req > 0 ? win->scale_req : (float)win->dpi / 96.0f;
    const char* e = getenv("SV_UI_SCALE");
    if (win->scale_req <= 0 && e && *e && strtof(e, NULL) > 0) s = strtof(e, NULL);
    if (s < 0.75f) s = 0.75f;
    if (s > 4.0f) s = 4.0f;
    return s;
}

#define WM_SV_TRAY (WM_APP + 7)

// puntatore globale alla finestra corrente (necessario per passare eventi dal WndProc alla poll_event)
static PlatformWindow* g_win = NULL;

static void push_event(PlatformWindow* win, const sEvent* e) {
    int next = (win->q_tail + 1) % EVT_QUEUE_CAP;
    if (next == win->q_head) return; // coda piena: scarta
    win->queue[win->q_tail] = *e;
    win->q_tail = next;
}

static uint32_t win_modifiers(void) {
    uint32_t m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= SV_MOD_CTRL;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= SV_MOD_SHIFT;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= SV_MOD_ALT;
    return m;
}

static void push_mouse(PlatformWindow* win, sEventType type, int button, LPARAM lp) {
    sEvent e = {0};
    e.type         = type;
    e.mouse_x      = UNSC(win, GET_X_LPARAM(lp));
    e.mouse_y      = UNSC(win, GET_Y_LPARAM(lp));
    e.mouse_button = button;
    e.modifiers    = win_modifiers();
    push_event(win, &e);
}

static void recreate_backbuffer(PlatformWindow* win) {
    if (win->width <= 0 || win->height <= 0) return;
    if (win->hbm_mem) DeleteObject(win->hbm_mem);
    win->hbm_mem = CreateCompatibleBitmap(win->hdc, win->width, win->height);
    SelectObject(win->hdc_mem, win->hbm_mem);
}

// window procedure: chiamata da Windows per OGNI messaggio
// Traduce i messaggi Win32 negli Event di platform.h
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlatformWindow* win = g_win;
    if (!win) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            sEvent e = {0};
            e.type      = EVT_KEY_DOWN;
            e.modifiers = win_modifiers();
            switch (wp) {
                case VK_RETURN:    e.key = KEY_ENTER;     break;
                case VK_ESCAPE:    e.key = KEY_ESCAPE;    break;
                case VK_BACK:      e.key = KEY_BACKSPACE; break;
                case VK_DELETE:    e.key = KEY_DELETE;    break;
                case VK_LEFT:      e.key = KEY_LEFT;      break;
                case VK_RIGHT:     e.key = KEY_RIGHT;     break;
                case VK_UP:        e.key = KEY_UP;        break;
                case VK_DOWN:      e.key = KEY_DOWN;      break;
                case VK_HOME:      e.key = KEY_HOME;      break;
                case VK_END:       e.key = KEY_END;       break;
                case VK_PRIOR:     e.key = KEY_PAGEUP;    break;
                case VK_NEXT:      e.key = KEY_PAGEDOWN;  break;
                case VK_TAB:       e.key = KEY_TAB;       break;
                case VK_F1:        e.key = KEY_F1;        break;
                case VK_F2:        e.key = KEY_F2;        break;
                case VK_F3:        e.key = KEY_F3;        break;
                case VK_F4:        e.key = KEY_F4;        break;
                case VK_F5:        e.key = KEY_F5;        break;
                default: {
                    // Ctrl + tasto (escluso AltGr = Ctrl+Alt): scorciatoia
                    bool ctrl = (e.modifiers & SV_MOD_CTRL) && !(e.modifiers & SV_MOD_ALT);
                    UINT ch = MapVirtualKeyW((UINT)wp, MAPVK_VK_TO_CHAR) & 0x7FFF;
                    if (ctrl && ch >= 0x20 && ch < 0x7F) {
                        e.key       = KEY_LETTER;
                        e.character = (uint32_t)tolower((int)ch);
                    } else {
                        e.type = EVT_NONE;
                    }
                    break;
                }
            }
            if (e.type != EVT_NONE) push_event(win, &e);
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) return DefWindowProcW(hwnd, msg, wp, lp);
            return 0;
        }

        case WM_CHAR: {
            // i caratteri di controllo (backspace, tab, invio, Ctrl+lettera...) sono già gestiti in WM_KEYDOWN
            WCHAR wc = (WCHAR)wp;
            if (wc >= 0xD800 && wc <= 0xDBFF) { win->high_surrogate = wc; return 0; }
            uint32_t cp = wc;
            if (wc >= 0xDC00 && wc <= 0xDFFF) {
                if (!win->high_surrogate) return 0;
                cp = 0x10000 + (((uint32_t)win->high_surrogate - 0xD800) << 10) + (wc - 0xDC00);
                win->high_surrogate = 0;
            }
            if (cp < 0x20 || cp == 0x7F) return 0;
            sEvent e = {0};
            e.type      = EVT_CHAR;
            e.character = cp;
            e.modifiers = win_modifiers();
            push_event(win, &e);
            return 0;
        }

        case WM_SV_TRAY: {                          // clic sull'icona nell'area di notifica
            PlatformWindow* tw = win;
            if (!tw) return 0;
            UINT m = (UINT)lp;
            sEvent e = {0};
            if (m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK) { e.type = EVT_TRAY_OPEN; push_event(tw, &e); }
            else if (m == WM_RBUTTONUP || m == WM_CONTEXTMENU) {
                HMENU menu = CreatePopupMenu();
                AppendMenuA(menu, MF_STRING, 1, "Open SegVault");
                AppendMenuA(menu, MF_STRING, 2, "Sites and services (browser)");
                AppendMenuA(menu, MF_STRING, 3, tw->tray_active ? "Stop the server" : "Start the server");
                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(menu, MF_STRING, 4, "Quit");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);              // il menu si chiude cliccando altrove
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
                e.type = cmd == 1 ? EVT_TRAY_OPEN : cmd == 2 ? EVT_TRAY_DASHBOARD : cmd == 3 ? EVT_TRAY_TOGGLE : cmd == 4 ? EVT_TRAY_QUIT : EVT_NONE;
                if (e.type != EVT_NONE) push_event(tw, &e);
            }
            return 0;
        }

        case WM_CLOSE: {
            sEvent e = {0};
            e.type = EVT_QUIT;
            push_event(win, &e);
            return 0; // la finestra viene distrutta da platform_window_destroy
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_MOUSEMOVE:   push_mouse(win, EVT_MOUSE_MOVE, 0, lp); return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: SetCapture(hwnd); push_mouse(win, EVT_MOUSE_DOWN, SV_MOUSE_LEFT, lp);   return 0;
        case WM_RBUTTONDOWN:   push_mouse(win, EVT_MOUSE_DOWN, SV_MOUSE_RIGHT, lp);  return 0;
        case WM_MBUTTONDOWN:   push_mouse(win, EVT_MOUSE_DOWN, SV_MOUSE_MIDDLE, lp); return 0;
        case WM_LBUTTONUP:     ReleaseCapture(); push_mouse(win, EVT_MOUSE_UP, SV_MOUSE_LEFT, lp); return 0;
        case WM_RBUTTONUP:     push_mouse(win, EVT_MOUSE_UP, SV_MOUSE_RIGHT, lp);    return 0;
        case WM_MBUTTONUP:     push_mouse(win, EVT_MOUSE_UP, SV_MOUSE_MIDDLE, lp);   return 0;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            sEvent e = {0};
            e.type        = EVT_MOUSE_WHEEL;
            e.mouse_x     = UNSC(win, p.x);
            e.mouse_y     = UNSC(win, p.y);
            e.modifiers   = win_modifiers();
            e.wheel_delta = delta > 0 ? 1 : -1;
            if (msg == WM_MOUSEHWHEEL) { e.modifiers |= SV_MOD_SHIFT; e.wheel_delta = -e.wheel_delta; }
            push_event(win, &e);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(win->cursors[win->cur_cursor]);
                return TRUE;
            }
            break;

        case WM_ERASEBKGND:
            return 1; // evita il flickering: ridisegniamo tutto noi

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            sEvent e = {0};
            e.type = EVT_PAINT;
            push_event(win, &e);
            return 0;
        }

        case WM_ACTIVATE: {
            sEvent e = {0};
            e.type = EVT_PAINT;
            push_event(win, &e);
            break;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = SC(win, 820);
            mmi->ptMinTrackSize.y = SC(win, 520);
            return 0;
        }

        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w <= 0 || h <= 0) return 0; // minimizzata
            win->width  = w;
            win->height = h;
            recreate_backbuffer(win);
            sEvent e = {0};
            e.type       = EVT_RESIZE;
            e.new_width  = UNSC(win, w);
            e.new_height = UNSC(win, h);
            push_event(win, &e);
            return 0;
        }

        case WM_DPICHANGED: {                       // finestra spostata su un monitor con un'altra scala
            win->dpi = HIWORD(wp) ? HIWORD(wp) : 96;
            win->scale = scale_for(win);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            sEvent e = {0};
            e.type       = EVT_RESIZE;
            e.new_width  = UNSC(win, win->width);
            e.new_height = UNSC(win, win->height);
            push_event(win, &e);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// carica come font privati dell'app i .ttf inclusi nel progetto (assets\fonts accanto
// all'eseguibile o nella cartella corrente), così l'aspetto è uguale a Linux
static int load_fonts_from(const WCHAR* dir) {
    WCHAR pattern[MAX_PATH];
    if (swprintf(pattern, MAX_PATH, L"%ls\\*.ttf", dir) < 0) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        WCHAR path[MAX_PATH];
        if (swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) >= 0 &&
            AddFontResourceExW(path, FR_PRIVATE, 0) > 0) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void load_bundled_fonts(void) {
    // i font sono dentro l'eseguibile: niente file accanto (basta scaricare l'exe)
    int loaded = 0;
    for (int i = 0; i < sv_embedded_font_count(); i++) {
        EmbeddedFont f = sv_embedded_font(i);
        DWORD n = 0;
        if (f.size && AddFontMemResourceEx((void*)f.data, (DWORD)f.size, NULL, &n)) loaded++;
    }
    if (loaded == sv_embedded_font_count()) return;
    WCHAR exe[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        WCHAR* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = 0;
        WCHAR dir[MAX_PATH];
        swprintf(dir, MAX_PATH, L"%ls\\assets\\fonts", exe);
        if (load_fonts_from(dir) > 0) return;
        swprintf(dir, MAX_PATH, L"%ls\\fonts", exe);
        if (load_fonts_from(dir) > 0) return;
    }
    load_fonts_from(L"assets\\fonts");
}

bool platform_init(void) {
    enable_dpi_awareness();
    load_bundled_fonts();
    return true;
}

PlatformWindow* platform_window_create(const char* title, int w, int h) {
    PlatformWindow* win = calloc(1, sizeof(PlatformWindow));
    if (!win) return NULL;

    win->dpi    = window_dpi(NULL);
    win->scale  = scale_for(win);
    w = SC(win, w);
    h = SC(win, h);
    win->width  = w;
    win->height = h;
    g_win = win;

    // registra la classe finestra
    WNDCLASSW wc     = {0};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SegVaultApp";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    WCHAR wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);

    // la dimensione richiesta è quella dell'area client
    RECT wr = {0, 0, w, h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    win->hwnd = CreateWindowW(
        L"SegVaultApp", wtitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL,
        GetModuleHandleW(NULL), NULL
    );
    if (!win->hwnd) { free(win); g_win = NULL; return NULL; }
    {                                               // il monitor della finestra può avere un'altra scala
        UINT d = window_dpi(win->hwnd);
        if (d != win->dpi) {
            int lw = UNSC(win, w), lh = UNSC(win, h);
            win->dpi = d;
            win->scale = scale_for(win);
            RECT r2 = {0, 0, SC(win, lw), SC(win, lh)};
            AdjustWindowRect(&r2, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(win->hwnd, NULL, 0, 0, r2.right - r2.left, r2.bottom - r2.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // crea il double buffer (evita flickering durante il ridisegno)
    win->hdc     = GetDC(win->hwnd);
    win->hdc_mem = CreateCompatibleDC(win->hdc);
    recreate_backbuffer(win);
    SetBkMode(win->hdc_mem, TRANSPARENT);

    // pixel per alpha blending
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = 1;
    bmi.bmiHeader.biHeight      = 1;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    win->hdc_px = CreateCompatibleDC(win->hdc);
    win->hbm_px = CreateDIBSection(win->hdc, &bmi, DIB_RGB_COLORS, (void**)&win->px, NULL, 0);
    SelectObject(win->hdc_px, win->hbm_px);

    win->cursors[CURSOR_ARROW]    = LoadCursor(NULL, IDC_ARROW);
    win->cursors[CURSOR_IBEAM]    = LoadCursor(NULL, IDC_IBEAM);
    win->cursors[CURSOR_HAND]     = LoadCursor(NULL, IDC_HAND);
    win->cursors[CURSOR_RESIZE_V] = LoadCursor(NULL, IDC_SIZENS);
    win->cursors[CURSOR_RESIZE_H] = LoadCursor(NULL, IDC_SIZEWE);
    win->cur_cursor = CURSOR_ARROW;

    return win;
}

void platform_window_show(PlatformWindow* win) {
    ShowWindow(win->hwnd, SW_SHOW);
    UpdateWindow(win->hwnd);
    win->visible = true;
}

bool platform_poll_event(PlatformWindow* win, sEvent* evt) {
    MSG msg;
    while (win->q_head == win->q_tail && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (win->q_head != win->q_tail) {
        *evt = win->queue[win->q_head];
        win->q_head = (win->q_head + 1) % EVT_QUEUE_CAP;
        return true;
    }
    return false;
}

void platform_wait_event(PlatformWindow* win, int timeout_ms) {
    if (win->q_head != win->q_tail) return;
    MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)timeout_ms, QS_ALLINPUT);
}

uint64_t platform_time_ms(void) {
    return (uint64_t)GetTickCount64();
}

// -- font --

static FontEntry* get_font(PlatformWindow* win, int size, FontStyle style) {
    if (size < 6) size = 6;
    for (int i = 0; i < win->num_fonts; i++)
        if (win->fonts[i].size == size && win->fonts[i].style == style)
            return &win->fonts[i];

    bool mono = (style == FONT_MONO || style == FONT_MONO_BOLD);
    int weight = (style == FONT_UI_BOLD) ? FW_SEMIBOLD : (style == FONT_MONO_BOLD) ? FW_BOLD : FW_NORMAL;

    // stessi font di Linux (inclusi in assets/fonts), poi i font di sistema di Windows.
    // Il SemiBold di Fira Sans è una famiglia GDI a sé: chiederlo per nome evita il grassetto sintetico.
    const WCHAR* faces_ui[]      = { L"Fira Sans", L"Segoe UI Variable Text", L"Segoe UI", NULL };
    const WCHAR* faces_ui_bold[] = { L"Fira Sans SemiBold", L"Fira Sans", L"Segoe UI Semibold", L"Segoe UI", NULL };
    const WCHAR* faces_mono[]    = { L"JetBrains Mono", L"Cascadia Mono", L"Cascadia Code", L"Consolas", NULL };
    const WCHAR** faces = mono ? faces_mono : (style == FONT_UI_BOLD ? faces_ui_bold : faces_ui);

    HFONT font = NULL;
    for (int i = 0; faces[i]; i++) {
        HFONT f = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, (mono ? FIXED_PITCH : VARIABLE_PITCH) | FF_DONTCARE,
                              faces[i]);
        HFONT old = SelectObject(win->hdc_mem, f);
        WCHAR got[LF_FACESIZE] = {0};
        GetTextFaceW(win->hdc_mem, LF_FACESIZE, got);
        SelectObject(win->hdc_mem, old);
        if (_wcsicmp(got, faces[i]) == 0 || !faces[i + 1]) { font = f; break; }
        DeleteObject(f);
    }

    HFONT old = SelectObject(win->hdc_mem, font);
    TEXTMETRICW tm;
    GetTextMetricsW(win->hdc_mem, &tm);
    SelectObject(win->hdc_mem, old);

    int slot = win->num_fonts < FONT_CACHE_CAP ? win->num_fonts++ : FONT_CACHE_CAP - 1;
    if (slot == FONT_CACHE_CAP - 1 && win->fonts[slot].font) DeleteObject(win->fonts[slot].font);
    win->fonts[slot] = (FontEntry){style, size, font, tm.tmHeight};
    return &win->fonts[slot];
}

// converte UTF-8 -> UTF-16 in un buffer temporaneo (statico per evitare malloc ad ogni frame)
static int to_wide(const char* text, int len, WCHAR** out) {
    static WCHAR* buf = NULL;
    static int cap = 0;
    if (len < 0) len = (int)strlen(text);
    int need = MultiByteToWideChar(CP_UTF8, 0, text, len, NULL, 0);
    if (need + 1 > cap) {
        cap = need + 256;
        buf = realloc(buf, cap * sizeof(WCHAR));
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, text, len, buf, cap);
    *out = buf;
    return n;
}

int platform_draw_text_ex(PlatformWindow* win, const char* text, int len, Point pos, Color c, int size, FontStyle style) {
    if (!text) return 0;
    if (len < 0) len = (int)strlen(text);
    if (len == 0) return 0;
    FontEntry* fe = get_font(win, SC(win, size), style);
    WCHAR* w;
    int n = to_wide(text, len, &w);

    HFONT old = SelectObject(win->hdc_mem, fe->font);
    SetTextColor(win->hdc_mem, RGB(c.r, c.g, c.b));
    SetBkMode(win->hdc_mem, TRANSPARENT);
    TextOutW(win->hdc_mem, SC(win, pos.x), SC(win, pos.y), w, n);
    SIZE sz;
    GetTextExtentPoint32W(win->hdc_mem, w, n, &sz);
    SelectObject(win->hdc_mem, old);
    return UNSC(win, sz.cx);
}

int platform_measure_text_ex(PlatformWindow* win, const char* text, int len, int size, FontStyle style) {
    if (!text) return 0;
    if (len < 0) len = (int)strlen(text);
    if (len == 0) return 0;
    FontEntry* fe = get_font(win, SC(win, size), style);
    WCHAR* w;
    int n = to_wide(text, len, &w);
    HFONT old = SelectObject(win->hdc_mem, fe->font);
    SIZE sz;
    GetTextExtentPoint32W(win->hdc_mem, w, n, &sz);
    SelectObject(win->hdc_mem, old);
    return UNSC(win, sz.cx);
}

int platform_text_height(PlatformWindow* win, int size, FontStyle style) {
    return UNSC(win, get_font(win, SC(win, size), style)->height);
}

int platform_draw_text(PlatformWindow* win, const char* text, Point pos, Color c, int size) {
    return platform_draw_text_ex(win, text, -1, pos, c, size, FONT_UI);
}

int platform_measure_text(PlatformWindow* win, const char* text, int size) {
    return platform_measure_text_ex(win, text, -1, size, FONT_UI);
}

// -- disegno --

void platform_clear(PlatformWindow* win, Color c) {
    HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
    RECT r = {0, 0, win->width, win->height};
    FillRect(win->hdc_mem, &r, brush);
    DeleteObject(brush);
}

static void phys_fill_rect(PlatformWindow* win, Rect r, Color c) {
    if (r.w <= 0 || r.h <= 0 || c.a == 0) return;
    if (c.a == 255) {
        HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
        RECT wr = {r.x, r.y, r.x + r.w, r.y + r.h};
        FillRect(win->hdc_mem, &wr, brush);
        DeleteObject(brush);
        return;
    }
    // colore premoltiplicato in formato BGRA
    uint32_t a = c.a;
    *win->px = (a << 24) | ((c.r * a / 255) << 16) | ((c.g * a / 255) << 8) | (c.b * a / 255);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    GdiAlphaBlend(win->hdc_mem, r.x, r.y, r.w, r.h, win->hdc_px, 0, 0, 1, 1, bf);
}

void platform_fill_rect(PlatformWindow* win, Rect r, Color c) {
    phys_fill_rect(win, SCR(win, r), c);
}

void platform_draw_rect(PlatformWindow* win, Rect r, Color c, int thickness) {
    if (thickness < 1) thickness = 1;
    r = SCR(win, r);
    thickness = thickness == 1 ? (win->scale >= 2.0f ? (int)win->scale : 1) : SC(win, thickness);
    phys_fill_rect(win, (Rect){r.x, r.y, r.w, thickness}, c);
    phys_fill_rect(win, (Rect){r.x, r.y + r.h - thickness, r.w, thickness}, c);
    phys_fill_rect(win, (Rect){r.x, r.y, thickness, r.h}, c);
    phys_fill_rect(win, (Rect){r.x + r.w - thickness, r.y, thickness, r.h}, c);
}

void platform_draw_line(PlatformWindow* win, Point a, Point b, Color c, int thickness) {
    HPEN pen = CreatePen(PS_SOLID, SC(win, thickness < 1 ? 1 : thickness), RGB(c.r, c.g, c.b));
    HPEN old = SelectObject(win->hdc_mem, pen);
    MoveToEx(win->hdc_mem, SC(win, a.x), SC(win, a.y), NULL);
    LineTo(win->hdc_mem, SC(win, b.x), SC(win, b.y));
    SelectObject(win->hdc_mem, old);
    DeleteObject(pen);
}

// rettangolo arrotondato con angoli antialiasing (stesso algoritmo di xlib.c)
void platform_fill_rect_rounded(PlatformWindow* win, Rect r, Color c, int radius) {
    if (r.w <= 0 || r.h <= 0) return;
    r = SCR(win, r);
    int rad = SC(win, radius);
    if (rad > r.w / 2) rad = r.w / 2;
    if (rad > r.h / 2) rad = r.h / 2;
    if (rad <= 0) { phys_fill_rect(win, r, c); return; }

    phys_fill_rect(win, (Rect){r.x, r.y + rad, r.w, r.h - 2 * rad}, c);
    phys_fill_rect(win, (Rect){r.x + rad, r.y, r.w - 2 * rad, rad}, c);
    phys_fill_rect(win, (Rect){r.x + rad, r.y + r.h - rad, r.w - 2 * rad, rad}, c);

    float rr = (float)rad * (float)rad;
    for (int j = 0; j < rad; j++) {
        int full_from = rad;
        for (int i = 0; i < rad; i++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = (float)i + (sx + 0.5f) / 4.0f - (float)rad;
                    float py = (float)j + (sy + 0.5f) / 4.0f - (float)rad;
                    if (px * px + py * py <= rr) inside++;
                }
            if (inside == 16) { full_from = i; break; }
            if (inside > 0) {
                Color pc = c;
                pc.a = (uint8_t)(c.a * inside / 16);
                int lx = r.x + i, rx = r.x + r.w - 1 - i;
                int ty = r.y + j, by = r.y + r.h - 1 - j;
                phys_fill_rect(win, (Rect){lx, ty, 1, 1}, pc);
                phys_fill_rect(win, (Rect){rx, ty, 1, 1}, pc);
                phys_fill_rect(win, (Rect){lx, by, 1, 1}, pc);
                phys_fill_rect(win, (Rect){rx, by, 1, 1}, pc);
            }
        }
        int run = rad - full_from;
        if (run > 0) {
            phys_fill_rect(win, (Rect){r.x + full_from, r.y + j, run, 1}, c);
            phys_fill_rect(win, (Rect){r.x + r.w - rad, r.y + j, run, 1}, c);
            phys_fill_rect(win, (Rect){r.x + full_from, r.y + r.h - 1 - j, run, 1}, c);
            phys_fill_rect(win, (Rect){r.x + r.w - rad, r.y + r.h - 1 - j, run, 1}, c);
        }
    }
}

void platform_set_clip(PlatformWindow* win, Rect r) {
    r = SCR(win, r);
    HRGN rgn = CreateRectRgn(r.x, r.y, r.x + r.w, r.y + r.h);
    SelectClipRgn(win->hdc_mem, rgn);
    DeleteObject(rgn);
}

void platform_reset_clip(PlatformWindow* win) {
    SelectClipRgn(win->hdc_mem, NULL);
}

bool platform_read_pixels(PlatformWindow* win, Rect lr, uint32_t* out) {
    if (!win || !out || lr.w <= 0 || lr.h <= 0 || lr.x < 0 || lr.y < 0) return false;
    Rect r = SCR(win, lr);
    if (r.w <= 0 || r.h <= 0 || r.x + r.w > win->width || r.y + r.h > win->height) return false;
    // copia del backbuffer in una DIB a 32 bit (GetDIBits non vuole bitmap selezionate in un DC)
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = r.w;
    bmi.bmiHeader.biHeight      = -r.h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HDC dc = CreateCompatibleDC(win->hdc_mem);
    HBITMAP dib = CreateDIBSection(win->hdc_mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dc || !dib || !bits) { if (dib) DeleteObject(dib); if (dc) DeleteDC(dc); return false; }
    HGDIOBJ old = SelectObject(dc, dib);
    BitBlt(dc, 0, 0, r.w, r.h, win->hdc_mem, r.x, r.y, SRCCOPY);
    GdiFlush();
    const uint32_t* p = (const uint32_t*)bits;      // BGRA in memoria = 0xAARRGGBB
    for (int y = 0; y < lr.h; y++)                  // un pixel fisico per pixel logico (il più vicino)
        for (int x = 0; x < lr.w; x++) {
            int sx = (int)((float)x * (float)r.w / (float)lr.w), sy = (int)((float)y * (float)r.h / (float)lr.h);
            if (sx >= r.w) sx = r.w - 1;
            if (sy >= r.h) sy = r.h - 1;
            out[y * lr.w + x] = p[sy * r.w + sx] & 0x00FFFFFFu;
        }
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    return true;
}

void platform_present(PlatformWindow* win) {
    BitBlt(win->hdc, 0, 0, win->width, win->height, win->hdc_mem, 0, 0, SRCCOPY);
}

void platform_draw_bitmap(PlatformWindow* win, Rect dest, uint32_t* pixels, int pw, int ph) {
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = pw;
    bmi.bmiHeader.biHeight      = -ph; // negativo = top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dest = SCR(win, dest);
    StretchDIBits(win->hdc_mem,
        dest.x, dest.y, dest.w, dest.h,
        0, 0, pw, ph,
        pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

// -- scala --

float platform_ui_scale(PlatformWindow* win) { return win ? win->scale : 1.0f; }

void platform_set_ui_scale(PlatformWindow* win, float scale) {
    if (!win) return;
    win->scale_req = scale > 0 ? scale : 0;
    float s = scale_for(win);
    if (fabsf(s - win->scale) < 0.001f) return;
    int lw = UNSC(win, win->width), lh = UNSC(win, win->height);
    win->scale = s;
    RECT r = {0, 0, SC(win, lw), SC(win, lh)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    if (!IsZoomed(win->hwnd))
        SetWindowPos(win->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    sEvent e = {0};
    e.type       = EVT_RESIZE;
    e.new_width  = UNSC(win, win->width);
    e.new_height = UNSC(win, win->height);
    push_event(win, &e);
}

// -- cursore --

void platform_set_cursor(PlatformWindow* win, CursorKind kind) {
    if (kind < 0 || kind >= CURSOR_COUNT || kind == win->cur_cursor) return;
    win->cur_cursor = kind;
    SetCursor(win->cursors[kind]);
}

// -- appunti (UTF-8 <-> UTF-16, \n <-> \r\n) --

bool platform_open_url(const char* url) {
    // solo pagine web (o il server locale di SegVault), mai comandi o file
    if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://127.0.0.1:", 17) && strncmp(url, "http://localhost:", 17)))
        return false;
    for (const char* c = url; *c; c++) if ((unsigned char)*c <= ' ' || *c == '"' || *c == '\'') return false;
    return (INT_PTR)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL) > 32;
}

bool platform_open_file(const char* path) {
    DWORD at = path ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
    if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) return false;
    char q[1100];
    snprintf(q, sizeof(q), "\"%s\"", path);
    return (INT_PTR)ShellExecuteA(NULL, "open", "notepad.exe", q, NULL, SW_SHOWNORMAL) > 32;
}

// ── finestra Apri / Salva di Windows ──
int platform_file_dialog(PlatformWindow* win, FileDialogKind kind, const char* title, const char* filter,
                         const char* initial, char* out, size_t outsz) {
    if (!out || outsz < 2) return -1;
    out[0] = '\0';
    // "SQL files|*.sql;*.txt" -> "SQL files\0*.sql;*.txt\0All files\0*.*\0\0"
    char flt[512];
    size_t fl = 0;
    char defext[16] = "";
    if (filter && *filter) {
        const char* bar = strchr(filter, '|');
        const char* pats = bar ? bar + 1 : "*.*";
        int nn = bar ? (int)(bar - filter) : (int)strlen(filter);
        fl += (size_t)snprintf(flt + fl, sizeof(flt) - fl, "%.*s (%s)", nn, filter, pats) + 1;
        if (fl < sizeof(flt)) fl += (size_t)snprintf(flt + fl, sizeof(flt) - fl, "%s", pats) + 1;
        const char* dot = strstr(pats, "*.");
        if (dot) {
            int k = 0;
            for (dot += 2; *dot && *dot != ';' && *dot != '*' && k < (int)sizeof(defext) - 1; dot++) defext[k++] = *dot;
            defext[k] = '\0';
        }
    }
    if (fl < sizeof(flt)) fl += (size_t)snprintf(flt + fl, sizeof(flt) - fl, "All files (*.*)") + 1;
    if (fl < sizeof(flt)) fl += (size_t)snprintf(flt + fl, sizeof(flt) - fl, "*.*") + 1;
    if (fl < sizeof(flt)) flt[fl] = '\0'; else { flt[sizeof(flt) - 2] = '\0'; flt[sizeof(flt) - 1] = '\0'; }

    char file[MAX_PATH * 4] = "", dir[MAX_PATH * 4] = "";
    if (initial && *initial) {
        char ini[MAX_PATH * 4];
        snprintf(ini, sizeof(ini), "%s", initial);
        for (char* c = ini; *c; c++) if (*c == '/') *c = '\\';
        DWORD at = GetFileAttributesA(ini);
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
            snprintf(dir, sizeof(dir), "%s", ini);
        } else {
            char* sl = strrchr(ini, '\\');
            if (sl) { *sl = '\0'; snprintf(dir, sizeof(dir), "%s", ini); snprintf(file, sizeof(file), "%s", sl + 1); }
            else snprintf(file, sizeof(file), "%s", ini);
        }
    }
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = win ? win->hwnd : NULL;
    ofn.lpstrFilter  = flt;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file);
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.lpstrTitle   = title && *title ? title : NULL;
    ofn.lpstrDefExt  = defext[0] ? defext : NULL;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY |
                       (kind == FILE_DIALOG_SAVE ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = kind == FILE_DIALOG_SAVE ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (!ok) return CommDlgExtendedError() == 0 ? 0 : -1;
    snprintf(out, outsz, "%s", file);
    return 1;
}

// ── area di notifica ──

// icona 32x32 disegnata in memoria: una pila di dischi (database), verde se attivo, grigia se no
static HICON make_tray_icon(bool active) {
    const int N = 32;
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = N;
    bi.bV5Height = -N;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    HDC dc = GetDC(NULL);
    void* bits = NULL;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return LoadIcon(NULL, IDI_APPLICATION);
    uint32_t* px = bits;
    uint32_t c = active ? 0xFF22A35A : 0xFF8A919C;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            // cilindro: corpo rettangolare con ellissi sopra e sotto, due linee bianche a 1/3 e 2/3
            double cx = (x - 15.5) / 13.0;
            bool in_body = x >= 3 && x <= 28 && y >= 7 && y <= 25;
            double ty = (y - 7) / 4.0, by = (y - 25) / 4.0;
            bool in_top = cx * cx + ty * ty <= 1.0, in_bot = cx * cx + by * by <= 1.0;
            uint32_t v = 0;
            if (in_body || in_top || in_bot) v = c;
            for (int k = 1; k <= 2; k++) {
                double ly = (y - (7 + k * 6)) / 4.0;
                double r = cx * cx + ly * ly;
                if (in_body && ly > 0 && r > 0.75 && r < 1.05) v = 0xFFFFFFFF;
            }
            px[y * N + x] = v;
        }
    HBITMAP mask = CreateBitmap(N, N, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h ? h : LoadIcon(NULL, IDI_APPLICATION);
}

bool platform_tray_enable(PlatformWindow* win, const char* tooltip) {
    if (win->tray_on) return true;
    if (!win->tray_icons[0]) { win->tray_icons[0] = make_tray_icon(false); win->tray_icons[1] = make_tray_icon(true); }
    memset(&win->nid, 0, sizeof(win->nid));
    win->nid.cbSize = sizeof(win->nid);
    win->nid.hWnd = win->hwnd;
    win->nid.uID = 1;
    win->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    win->nid.uCallbackMessage = WM_SV_TRAY;
    win->nid.hIcon = win->tray_icons[win->tray_active ? 1 : 0];
    snprintf(win->nid.szTip, sizeof(win->nid.szTip), "%s", tooltip ? tooltip : "SegVault");
    win->tray_on = Shell_NotifyIconA(NIM_ADD, &win->nid) != 0;
    return win->tray_on;
}

void platform_tray_update(PlatformWindow* win, const char* tooltip, bool active) {
    if (!win->tray_on) { win->tray_active = active; return; }
    bool changed = active != win->tray_active;
    win->tray_active = active;
    if (tooltip && strcmp(tooltip, win->nid.szTip)) { snprintf(win->nid.szTip, sizeof(win->nid.szTip), "%s", tooltip); changed = true; }
    if (!changed) return;
    win->nid.hIcon = win->tray_icons[active ? 1 : 0];
    win->nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &win->nid);
}

void platform_tray_disable(PlatformWindow* win) {
    if (!win->tray_on) return;
    Shell_NotifyIconA(NIM_DELETE, &win->nid);
    win->tray_on = false;
}

void platform_window_set_visible(PlatformWindow* win, bool visible) {
    if (visible) {
        ShowWindow(win->hwnd, IsIconic(win->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(win->hwnd);
    } else ShowWindow(win->hwnd, SW_HIDE);
    win->visible = visible;
}

bool platform_window_visible(PlatformWindow* win) { return win->visible; }

// ── avvio con il computer: HKCU\Software\Microsoft\Windows\CurrentVersion\Run ──

#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

bool platform_autostart_set(bool on, const char* command) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG r = on ? RegSetValueExA(k, "SegVault", 0, REG_SZ, (const BYTE*)command, (DWORD)strlen(command) + 1)
                : RegDeleteValueA(k, "SegVault");
    RegCloseKey(k);
    return r == ERROR_SUCCESS || (!on && r == ERROR_FILE_NOT_FOUND);
}

bool platform_autostart_get(void) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExA(k, "SegVault", NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

void platform_set_clipboard(PlatformWindow* win, const char* text) {
    if (!text || !OpenClipboard(win->hwnd)) return;
    EmptyClipboard();

    // converte \n in \r\n
    size_t len = strlen(text), extra = 0;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') extra++;
    char* crlf = malloc(len + extra + 1);
    if (crlf) {
        size_t j = 0;
        for (size_t i = 0; i < len; i++) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf[j++] = '\r';
            crlf[j++] = text[i];
        }
        crlf[j] = '\0';

        int n = MultiByteToWideChar(CP_UTF8, 0, crlf, -1, NULL, 0);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(WCHAR));
        if (mem) {
            WCHAR* dst = GlobalLock(mem);
            MultiByteToWideChar(CP_UTF8, 0, crlf, -1, dst, n);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        free(crlf);
    }
    CloseClipboard();
}

char* platform_get_clipboard(PlatformWindow* win) {
    if (!OpenClipboard(win->hwnd)) return NULL;
    char* out = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR* w = GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            out = malloc(n);
            if (out) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
                // rimuove i \r
                char* d = out;
                for (char* s = out; *s; s++) if (*s != '\r') *d++ = *s;
                *d = '\0';
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void platform_window_destroy(PlatformWindow* win) {
    if (!win) return;
    platform_tray_disable(win);
    for (int i = 0; i < win->num_fonts; i++) DeleteObject(win->fonts[i].font);
    if (win->hbm_px) DeleteObject(win->hbm_px);
    if (win->hdc_px) DeleteDC(win->hdc_px);
    DeleteObject(win->hbm_mem);
    DeleteDC(win->hdc_mem);
    ReleaseDC(win->hwnd, win->hdc);
    g_win = NULL;
    DestroyWindow(win->hwnd);
    free(win);
}

void platform_shutdown(void) {
    // Niente da fare a livello globale su Win32
}

// -- thread --

struct PlatformThread {
    HANDLE handle;
    void (*fn)(void*);
    void* arg;
};

static DWORD WINAPI thread_entry(LPVOID p) {
    PlatformThread* t = (PlatformThread*)p;
    t->fn(t->arg);
    return 0;
}

PlatformThread* platform_thread_start(void (*fn)(void* arg), void* arg) {
    PlatformThread* t = calloc(1, sizeof(PlatformThread));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 16u << 20, thread_entry, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);   // stack 16 MB
    if (!t->handle) { free(t); return NULL; }
    return t;
}

void platform_thread_join(PlatformThread* t) {
    if (!t) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

void platform_error_box(const char* title, const char* message) {
    MessageBoxA(NULL, message, title, MB_OK | MB_ICONERROR);
}
