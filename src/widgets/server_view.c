#include "server_view.h"
#include "theme.h"
#include "settings.h"
#include "line_edit.h"
#include "../server/server.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool in_rect(Rect r, int x, int y) { return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void text_center(PlatformWindow* win, Rect r, const char* t, Color c, int size, FontStyle st) {
    int tw = platform_measure_text_ex(win, t, -1, size, st);
    platform_draw_text_ex(win, t, -1, (Point){r.x + (r.w - tw) / 2, ui_text_y(win, r, size, st)}, c, size, st);
}

// testo su più righe nella larghezza w (a capo sulle parole); ritorna l'altezza usata
static int text_wrap(PlatformWindow* win, const char* t, int x, int y, int w, Color c, int size) {
    int lh = platform_text_height(win, size, FONT_UI) + 3, used = 0;
    while (*t) {
        int n = 0, last_space = -1;
        while (t[n] && t[n] != '\n') {
            if (t[n] == ' ') last_space = n;
            if (platform_measure_text_ex(win, t, n + 1, size, FONT_UI) > w && last_space > 0) { n = last_space; break; }
            n++;
        }
        platform_draw_text_ex(win, t, n, (Point){x, y + used}, c, size, FONT_UI);
        used += lh;
        t += n;
        while (*t == ' ' || *t == '\n') t++;
    }
    return used;
}

// ═════════════════════════════════════════════════════════════════
//  Schermata iniziale
// ═════════════════════════════════════════════════════════════════

static const char* const START_TITLE[2] = { "SQL editor", "API server" };
static const char* const START_TEXT[2] = {
    "Write and run queries, explore the schema, read execution plans, import and export data.",
    "Let websites and desktop apps read and write this database over HTTP/JSON "
    "(TypeScript, JavaScript, Python, C#, Java...). Live request log.",
};

static void draw_start_icon(PlatformWindow* win, int k, int cx, int cy, Color c, Color bg) {
    if (k == 0) {                                   // editor: righe di codice
        for (int i = 0; i < 4; i++) {
            int w = (i == 1 ? 26 : i == 3 ? 14 : 34);
            platform_fill_rect_rounded(win, (Rect){cx - 17 + (i == 1 || i == 2 ? 6 : 0), cy - 13 + i * 8, w, 4}, c, 2);
        }
    } else {                                        // server: due "rack" con i led
        for (int i = 0; i < 2; i++) {
            Rect r = { cx - 18, cy - 15 + i * 16, 36, 13 };
            ui_round_box(win, r, bg, c, 3);
            ui_circle(win, r.x + 7, r.y + 6, 2, c);
            platform_fill_rect(win, (Rect){r.x + 14, r.y + 5, 16, 2}, c);
        }
    }
}

void start_screen_draw(StartScreen* s, PlatformWindow* win, int w, int h, const char* data_dir) {
    platform_clear(win, TH_BG);
    int cw = SV_MIN(760, w - 48);
    int card_w = (cw - 20) / 2, card_h = 200;
    bool stacked = card_w < 250;                    // finestra stretta: schede una sopra l'altra
    if (stacked) card_w = cw;
    int total_h = 96 + (stacked ? card_h * 2 + 16 : card_h) + 70;
    int x0 = (w - cw) / 2, y = SV_MAX(24, (h - total_h) / 2);

    ui_icon_logo(win, x0 + 18, y + 18, 34);
    platform_draw_text_ex(win, SV_NAME, -1, (Point){x0 + 46, y + 2}, TH_TEXT, 22, FONT_UI_BOLD);
    platform_draw_text_ex(win, "How do you want to use SegVault?", -1, (Point){x0 + 46, y + 34}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    y += 96;

    for (int k = 0; k < 2; k++) {
        Rect r = stacked ? (Rect){x0, y + k * (card_h + 16), card_w, card_h} : (Rect){x0 + k * (card_w + 20), y, card_w, card_h};
        s->cards[k] = r;
        bool hot = s->hover == k || (s->hover < 0 && s->focus == k);
        ui_round_box(win, r, hot ? TH_ELEVATED_HOVER : TH_SURFACE, hot ? TH_ACCENT : TH_BORDER, 10);
        Rect ic = { r.x + 20, r.y + 20, 52, 52 };
        platform_fill_rect_rounded(win, ic, hot ? TH_ACCENT_SOFT : TH_ELEVATED, 10);
        draw_start_icon(win, k, ic.x + 26, ic.y + 26, hot ? TH_ACCENT_TEXT : TH_TEXT_MUTED, hot ? TH_ACCENT_SOFT : TH_ELEVATED);
        platform_draw_text_ex(win, START_TITLE[k], -1, (Point){r.x + 20, r.y + 88}, TH_TEXT, 17, FONT_UI_BOLD);
        text_wrap(win, START_TEXT[k], r.x + 20, r.y + 116, r.w - 40, TH_TEXT_MUTED, TH_FONT_SMALL + 1);
        char key[8];
        snprintf(key, sizeof(key), "%d", k + 1);
        Rect kr = { r.x + r.w - 34, r.y + 18, 18, 18 };
        ui_round_box(win, kr, TH_ELEVATED, TH_BORDER, 4);
        text_center(win, kr, key, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI);
    }
    y += stacked ? card_h * 2 + 16 : card_h;
    y += 22;

    Rect cb = { x0, y, 16, 16 };
    s->remember_box = (Rect){x0, y - 4, 340, 24};
    ui_round_box(win, cb, s->remember ? TH_ACCENT : TH_SURFACE, s->remember ? TH_ACCENT : TH_BORDER_STRONG, 4);
    if (s->remember) ui_icon_check(win, cb.x + 8, cb.y + 8, 11, TH_ON_ACCENT);
    platform_draw_text_ex(win, "Remember my choice (change it in Settings \xe2\x80\xba Server)", -1,
                          (Point){x0 + 26, ui_text_y(win, cb, TH_FONT_UI, FONT_UI)}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    ui_text_clipped(win, "Keys: 1 / 2, or arrows and Enter", x0, y + 32, cw, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    char dd[600];
    snprintf(dd, sizeof(dd), "Data: %s", data_dir && data_dir[0] ? data_dir : "./data");
    ui_text_clipped(win, dd, x0, y + 50, cw, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
}

ViewAction start_screen_event(StartScreen* s, const sEvent* evt) {
    switch (evt->type) {
        case EVT_MOUSE_MOVE: {
            s->mx = evt->mouse_x; s->my = evt->mouse_y;
            int h = in_rect(s->cards[0], s->mx, s->my) ? 0 : in_rect(s->cards[1], s->mx, s->my) ? 1 : -1;
            if (h != s->hover) { s->hover = h; return VIEW_REDRAW; }
            return VIEW_NONE;
        }
        case EVT_MOUSE_DOWN:
            if (evt->mouse_button != SV_MOUSE_LEFT) return VIEW_NONE;
            if (in_rect(s->cards[0], evt->mouse_x, evt->mouse_y)) return VIEW_OPEN_EDITOR;
            if (in_rect(s->cards[1], evt->mouse_x, evt->mouse_y)) return VIEW_OPEN_SERVER;
            if (in_rect(s->remember_box, evt->mouse_x, evt->mouse_y)) { s->remember = !s->remember; return VIEW_REDRAW; }
            return VIEW_NONE;
        case EVT_CHAR:
            if (evt->character == '1') return VIEW_OPEN_EDITOR;
            if (evt->character == '2') return VIEW_OPEN_SERVER;
            if (evt->character == ' ') { s->remember = !s->remember; return VIEW_REDRAW; }
            return VIEW_NONE;
        case EVT_KEY_DOWN:
            if (evt->key == KEY_LEFT || evt->key == KEY_UP)    { s->focus = 0; s->hover = -1; return VIEW_REDRAW; }
            if (evt->key == KEY_RIGHT || evt->key == KEY_DOWN || evt->key == KEY_TAB) { s->focus = !s->focus; s->hover = -1; return VIEW_REDRAW; }
            if (evt->key == KEY_ENTER) return s->focus ? VIEW_OPEN_SERVER : VIEW_OPEN_EDITOR;
            if (evt->key == KEY_ESCAPE) return VIEW_OPEN_EDITOR;
            return VIEW_NONE;
        default:
            return VIEW_NONE;
    }
}

// ═════════════════════════════════════════════════════════════════
//  Pannello del server
// ═════════════════════════════════════════════════════════════════

enum {
    HIT_EDITOR, HIT_TOGGLE, HIT_COPY_URL, HIT_COPY_LAN, HIT_PORT_MINUS, HIT_PORT_PLUS, HIT_ACCESS_LOCAL, HIT_ACCESS_LAN,
    HIT_TOKEN_OFF, HIT_TOKEN_ON, HIT_COPY_TOKEN, HIT_NEW_TOKEN, HIT_CORS_OFF, HIT_CORS_ON, HIT_TIME_MINUS,
    HIT_TIME_PLUS, HIT_COPY_EXAMPLE, HIT_CLEAR_LOG, HIT_COUNT
};

struct ServerPanel {
    PlatformWindow* win;
    char            data_dir[512];
    char            settings_path[1100];
    SvServer*       srv;
    PlatformThread* thread;
    char            token[65];
    char            error[300];
    char            url[96], lan_url[96];
    SvStats         stats;
    SvLogEntry      log[SV_LOG_MAX];
    int             nlog;
    long long       log_seq;         // ultimo numero di richiesta mostrato
    long long       log_cleared;     // richieste nascoste da "Clear"
    int             log_scroll;
    Rect            log_rect;
    uint64_t        last_poll;
    int             last_uptime;
    LineEdit*       port_edit;
    Rect            hit[HIT_COUNT];
    int             mx, my;
    char            flash[64];        // "Copied" vicino al pulsante
    int             flash_hit;
    uint64_t        flash_until;
};

static bool token_required(void);
static bool token_load(ServerPanel* p, bool regenerate);

ServerPanel* server_panel_create(PlatformWindow* win) {
    ServerPanel* p = calloc(1, sizeof(ServerPanel));
    if (!p) return NULL;
    p->win = win;
    p->port_edit = line_edit_create("8484");
    if (p->port_edit) p->port_edit->win = win;
    p->flash_hit = -1;
    return p;
}

void server_panel_set_paths(ServerPanel* p, const char* data_dir, const char* settings_path) {
    if (!p) return;
    snprintf(p->data_dir, sizeof(p->data_dir), "%s", data_dir ? data_dir : "");
    snprintf(p->settings_path, sizeof(p->settings_path), "%s", settings_path ? settings_path : "");
    if (token_required()) token_load(p, false);
}

bool server_panel_running(const ServerPanel* p) { return p && p->srv; }

static bool token_required(void) { return g_settings.server_token || g_settings.server_lan || g_settings.server_cors; }

static void token_path(ServerPanel* p, char* out, size_t n) {
    snprintf(out, n, "%s%s%s", p->data_dir, p->data_dir[0] ? "/" : "", "server.token");
}

// token salvato nella cartella dati (resta uguale fra un avvio e l'altro); creato se manca
static bool token_load(ServerPanel* p, bool regenerate) {
    char path[600];
    token_path(p, path, sizeof(path));
    if (!regenerate) {
        FILE* f = fopen(path, "rb");
        if (f) {
            size_t n = fread(p->token, 1, sizeof(p->token) - 1, f);
            fclose(f);
            p->token[n] = '\0';
            p->token[strcspn(p->token, "\r\n ")] = '\0';
            if (strlen(p->token) >= 16) return true;
        }
    }
    if (!sv_random_token(p->token, 24)) { p->token[0] = '\0'; return false; }
    FILE* f = fopen(path, "wb");
    if (f) { fprintf(f, "%s\n", p->token); fclose(f); }
    return true;
}

static void save_settings(ServerPanel* p) {
    if (p->settings_path[0]) settings_save(p->settings_path);
}

static void run_server_thread(void* arg) { sv_server_run((SvServer*)arg); }

bool server_panel_start(ServerPanel* p) {
    if (!p) return false;
    if (p->srv) return true;
    p->error[0] = '\0';
    if (token_required() && !p->token[0] && !token_load(p, false)) {
        snprintf(p->error, sizeof(p->error), "Cannot generate a token");
        return false;
    }
    SvServerConfig cfg = {
        .bind_addr   = g_settings.server_lan ? "0.0.0.0" : "127.0.0.1",
        .port        = g_settings.server_port,
        .token       = token_required() ? p->token : NULL,
        .cors_origin = g_settings.server_cors ? "*" : NULL,
        .timeout_ms  = g_settings.server_timeout * 1000,
        .quiet       = true,
    };
    char err[256];
    p->srv = sv_server_start(&cfg, err, sizeof(err));
    if (!p->srv) { snprintf(p->error, sizeof(p->error), "%s", err); return false; }
    p->thread = platform_thread_start(run_server_thread, p->srv);
    if (!p->thread) {
        sv_server_free(p->srv);
        p->srv = NULL;
        snprintf(p->error, sizeof(p->error), "Cannot start the server thread");
        return false;
    }
    snprintf(p->url, sizeof(p->url), "http://127.0.0.1:%d/api/v1", sv_server_port(p->srv));
    char ip[64];
    p->lan_url[0] = '\0';
    if (g_settings.server_lan && sv_local_ip(ip, sizeof(ip)))
        snprintf(p->lan_url, sizeof(p->lan_url), "http://%s:%d/api/v1", ip, sv_server_port(p->srv));
    p->log_seq = -1;            // aggiorna subito statistiche e registro
    p->nlog = 0;
    p->log_cleared = 0;
    p->last_poll = 0;
    return true;
}

void server_panel_stop(ServerPanel* p) {
    if (!p || !p->srv) return;
    sv_server_stop(p->srv);
    if (p->thread) platform_thread_join(p->thread);   // il ciclo di accettazione esce entro 200 ms
    p->thread = NULL;
    sv_server_free(p->srv);                           // chiude le connessioni e attende i loro thread
    p->srv = NULL;
    p->stats.connections = 0;
    p->stats.busy = false;
}

void server_panel_destroy(ServerPanel* p) {
    if (!p) return;
    server_panel_stop(p);
    if (p->port_edit) widget_destroy((Widget*)p->port_edit);
    free(p);
}

// una modifica alle opzioni: salvata e applicata riavviando il server
static void config_changed(ServerPanel* p) {
    save_settings(p);
    if (token_required() && !p->token[0]) token_load(p, false);
    if (p->srv) { server_panel_stop(p); server_panel_start(p); }
}

bool server_panel_tick(ServerPanel* p, uint64_t now_ms) {
    if (!p) return false;
    bool redraw = false;
    if (p->port_edit && line_edit_tick(p->port_edit, now_ms)) redraw = true;
    if (p->flash_hit >= 0 && now_ms >= p->flash_until) { p->flash_hit = -1; redraw = true; }
    if (!p->srv || now_ms - p->last_poll < 250) return redraw;
    p->last_poll = now_ms;
    SvStats st;
    sv_server_stats(p->srv, &st);
    if (st.requests != p->log_seq) {
        p->nlog = sv_server_recent(p->srv, p->log, SV_LOG_MAX);
        p->log_seq = st.requests;
        redraw = true;
    }
    if ((int)st.uptime_s != p->last_uptime || st.connections != p->stats.connections || st.busy != p->stats.busy) redraw = true;
    p->last_uptime = (int)st.uptime_s;
    p->stats = st;
    return redraw;
}

// ── disegno ─────────────────────────────────────────────────────

enum { BTN_NORMAL, BTN_PRIMARY, BTN_DANGER, BTN_GHOST };

static void button(ServerPanel* p, int id, Rect r, const char* label, int kind, bool enabled) {
    PlatformWindow* win = p->win;
    p->hit[id] = enabled ? r : (Rect){0, 0, 0, 0};
    bool hov = enabled && in_rect(r, p->mx, p->my);
    Color fill, border, text;
    switch (kind) {
        case BTN_PRIMARY: fill = hov ? TH_ACCENT_HOVER : TH_ACCENT; border = fill; text = TH_ON_ACCENT; break;
        case BTN_DANGER:  fill = hov ? TH_ERROR_SOFT : TH_ELEVATED; border = hov ? TH_ERROR : TH_BORDER; text = TH_ERROR_TEXT; break;
        case BTN_GHOST:   fill = hov ? TH_ELEVATED_HOVER : TH_SURFACE; border = TH_SURFACE; text = TH_TEXT_MUTED; break;
        default:          fill = hov ? TH_ELEVATED_HOVER : TH_ELEVATED; border = TH_BORDER; text = TH_TEXT; break;
    }
    if (!enabled) { fill = TH_SURFACE; border = TH_BORDER; text = TH_TEXT_FAINT; }
    ui_round_box(win, r, fill, border, TH_RADIUS);
    const char* l = (p->flash_hit == id) ? p->flash : label;
    text_center(win, r, l, text, TH_FONT_UI, kind == BTN_PRIMARY ? FONT_UI_BOLD : FONT_UI);
}

static int button_w(ServerPanel* p, const char* label) {
    return platform_measure_text_ex(p->win, label, -1, TH_FONT_UI, FONT_UI_BOLD) + 28;
}

// controllo a due posizioni
static void segmented(ServerPanel* p, int id0, Rect r, const char* a, const char* b, int sel, bool enabled) {
    PlatformWindow* win = p->win;
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER, TH_RADIUS);
    for (int k = 0; k < 2; k++) {
        Rect h = { r.x + 2 + k * (r.w - 4) / 2, r.y + 2, (r.w - 4) / 2, r.h - 4 };
        p->hit[id0 + k] = enabled ? h : (Rect){0, 0, 0, 0};
        bool on = sel == k, hov = enabled && in_rect(h, p->mx, p->my);
        if (on) ui_round_box(win, h, enabled ? TH_ACCENT_SOFT : TH_ELEVATED, enabled ? TH_ACCENT : TH_BORDER_STRONG, TH_RADIUS - 1);
        else if (hov) platform_fill_rect_rounded(win, h, TH_HOVER, TH_RADIUS - 1);
        // bloccato (es. token obbligatorio in rete): la scelta attiva resta leggibile, l'altra sbiadita
        Color tc = on ? (enabled ? TH_ACCENT_TEXT : TH_TEXT) : (enabled ? TH_TEXT_MUTED : TH_TEXT_FAINT);
        text_center(win, h, k ? b : a, tc, TH_FONT_UI, on ? FONT_UI_BOLD : FONT_UI);
    }
}

static void stepper(ServerPanel* p, int id_minus, int id_plus, Rect r, const char* value, bool enabled) {
    PlatformWindow* win = p->win;
    Rect m = { r.x, r.y, r.h, r.h }, pl = { r.x + r.w - r.h, r.y, r.h, r.h };
    button(p, id_minus, m, "\xe2\x88\x92", BTN_NORMAL, enabled);
    button(p, id_plus, pl, "+", BTN_NORMAL, enabled);
    Rect v = { m.x + m.w + 4, r.y, r.w - 2 * r.h - 8, r.h };
    ui_round_box(win, v, TH_SURFACE_2, TH_BORDER, TH_RADIUS);
    text_center(win, v, value, enabled ? TH_TEXT : TH_TEXT_FAINT, TH_FONT_UI, FONT_MONO);
}

static void card(PlatformWindow* win, Rect r) { ui_round_box(win, r, TH_SURFACE, TH_BORDER, 10); }

static void format_uptime(double s, char* out, size_t n) {
    long t = (long)s;
    if (t < 60) snprintf(out, n, "%lds", t);
    else if (t < 3600) snprintf(out, n, "%ldm %02lds", t / 60, t % 60);
    else snprintf(out, n, "%ldh %02ldm", t / 3600, t / 60 % 60);
}

// masked: il token a schermo solo per metà (chi guarda lo schermo non lo legge); la copia è completa
static void example_text(ServerPanel* p, char* out, size_t n, bool masked) {
    const char* url = p->srv ? p->url : "http://127.0.0.1:8484/api/v1";
    char auth[120] = "";
    if (token_required() && p->token[0])
        snprintf(auth, sizeof(auth), " -H \"Authorization: Bearer %.*s%s\"", masked ? 8 : 64, p->token, masked ? "\xe2\x80\xa6" : "");
    snprintf(out, n, "curl %s/query -H \"Content-Type: application/json\"%s -d \"{\\\"sql\\\": \\\"SHOW DATABASES\\\"}\"", url, auth);
}

void server_panel_draw(ServerPanel* p, int W, int H) {
    if (!p) return;
    PlatformWindow* win = p->win;
    memset(p->hit, 0, sizeof(p->hit));
    platform_clear(win, TH_BG);
    bool running = p->srv != NULL;
    bool editable = true;                          // le opzioni si cambiano anche col server attivo (riavvio)

    // ── intestazione ──
    Rect head = { 0, 0, W, 56 };
    platform_fill_rect(win, head, TH_SURFACE);
    platform_fill_rect(win, (Rect){0, 55, W, 1}, TH_BORDER);
    ui_icon_logo(win, 30, 28, 26);
    platform_draw_text_ex(win, SV_NAME, -1, (Point){52, ui_text_y(win, head, 16, FONT_UI_BOLD)}, TH_TEXT, 16, FONT_UI_BOLD);
    int tx = 52 + platform_measure_text_ex(win, SV_NAME, -1, 16, FONT_UI_BOLD) + 10;
    platform_draw_text_ex(win, "API server", -1, (Point){tx, ui_text_y(win, head, TH_FONT_UI, FONT_UI)}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    const char* ed = "Open SQL editor";
    int ew = button_w(p, ed);
    button(p, HIT_EDITOR, (Rect){W - ew - 16, 12, ew, 32}, ed, BTN_NORMAL, true);

    int pad = 20, cw = SV_MIN(1180, W - 2 * pad), x0 = (W - cw) / 2, y = 56 + 16;

    // ── stato ──
    Rect sc = { x0, y, cw, p->lan_url[0] && running ? 108 : 92 };
    card(win, sc);
    Color dot = running ? TH_SUCCESS : p->error[0] ? TH_ERROR : TH_TEXT_FAINT;
    if (running && p->stats.busy) dot = TH_WARNING;
    ui_circle(win, sc.x + 24, sc.y + 26, 6, dot);
    if (running) ui_circle(win, sc.x + 24, sc.y + 26, 10, ui_alpha(dot, 50));
    const char* st = running ? (p->stats.busy ? "Running \xc2\xb7 executing a request" : "Running")
                   : p->error[0] ? "Not running" : "Stopped";
    platform_draw_text_ex(win, st, -1, (Point){sc.x + 42, sc.y + 16}, TH_TEXT, 16, FONT_UI_BOLD);
    int bx = sc.x + sc.w - 16;
    const char* tl = running ? "Stop server" : "Start server";
    int tw = button_w(p, tl);
    bx -= tw;
    button(p, HIT_TOGGLE, (Rect){bx, sc.y + 14, tw, 32}, tl, running ? BTN_DANGER : BTN_PRIMARY, true);
    int text_max = bx - (sc.x + 42) - 16;
    if (running) {
        const char* cu = "Copy";
        int cuw = button_w(p, cu);
        int ux = sc.x + 42, uy = sc.y + 54;
        int uw = platform_measure_text_ex(win, p->url, -1, 15, FONT_MONO);
        platform_draw_text_ex(win, p->url, -1, (Point){ux, uy}, TH_ACCENT_TEXT, 15, FONT_MONO);
        button(p, HIT_COPY_URL, (Rect){ux + uw + 12, uy - 6, cuw, 28}, cu, BTN_NORMAL, true);
        if (p->lan_url[0]) {
            uy += 30;
            int lw = platform_measure_text_ex(win, p->lan_url, -1, 15, FONT_MONO);
            platform_draw_text_ex(win, p->lan_url, -1, (Point){ux, uy}, TH_TEXT, 15, FONT_MONO);
            button(p, HIT_COPY_LAN, (Rect){ux + lw + 12, uy - 6, cuw, 28}, cu, BTN_NORMAL, true);
            platform_draw_text_ex(win, "(local network)", -1, (Point){ux + lw + cuw + 24, uy + 1}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI);
        }
    } else {
        const char* msg = p->error[0] ? p->error : "Start the server to let web and desktop apps use this database.";
        ui_text_clipped(win, msg, sc.x + 42, sc.y + 54, SV_MAX(80, text_max), p->error[0] ? TH_ERROR_TEXT : TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    }
    y += sc.h + 12;

    // ── statistiche ──
    char v[5][32];
    snprintf(v[0], 32, "%lld", running ? p->stats.requests : 0LL);
    snprintf(v[1], 32, "%lld", running ? p->stats.errors : 0LL);
    snprintf(v[2], 32, "%lld", running ? p->stats.rows_sent : 0LL);
    snprintf(v[3], 32, "%d", running ? p->stats.connections : 0);
    if (running) format_uptime(p->stats.uptime_s, v[4], 32); else snprintf(v[4], 32, "\xe2\x80\x94");
    static const char* const LBL[5] = { "Requests", "Errors", "Rows sent", "Connections", "Uptime" };
    int tiles = cw >= 620 ? 5 : 3, tg = 12, tile_w = (cw - tg * (tiles - 1)) / tiles;
    for (int i = 0; i < tiles; i++) {
        int k = tiles == 5 ? i : (int[]){0, 1, 4}[i];
        Rect t = { x0 + i * (tile_w + tg), y, tile_w, 64 };
        card(win, t);
        platform_draw_text_ex(win, LBL[k], -1, (Point){t.x + 14, t.y + 10}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI);
        Color c = (k == 1 && running && p->stats.errors) ? TH_ERROR_TEXT : TH_TEXT;
        platform_draw_text_ex(win, v[k], -1, (Point){t.x + 14, t.y + 30}, c, 20, FONT_UI_BOLD);
    }
    y += 64 + 12;

    // ── opzioni ──
    bool two = cw >= 900;
    int rows = two ? 3 : 5, row_h = 40;
    Rect oc = { x0, y, cw, 40 + rows * row_h + 8 };
    card(win, oc);
    platform_draw_text_ex(win, "Options", -1, (Point){oc.x + 16, oc.y + 12}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
    platform_draw_text_ex(win, running ? "changes restart the server" : "saved for the next start", -1,
                          (Point){oc.x + 16 + platform_measure_text_ex(win, "Options", -1, TH_FONT_UI, FONT_UI_BOLD) + 10, oc.y + 13},
                          TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    int col_w = two ? (cw - 32) / 2 : cw - 32, lab_w = 150;
    for (int item = 0; item < 5; item++) {
        int col = two ? (item < 3 ? 0 : 1) : 0, row = two ? (item < 3 ? item : item - 3) : item;
        int ix = oc.x + 16 + col * (col_w + (two ? 16 : 0)), iy = oc.y + 40 + row * row_h;
        Rect lab = { ix, iy, lab_w, 30 };
        Rect ctl = { ix + lab_w, iy, SV_MIN(260, col_w - lab_w), 30 };
        static const char* const NAMES[5] = { "Port", "Reachable from", "Time limit", "Token", "Web apps (CORS)" };
        platform_draw_text_ex(win, NAMES[item], -1, (Point){lab.x, ui_text_y(win, lab, TH_FONT_UI, FONT_UI)}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
        char buf[32];
        switch (item) {
            case 0: {
                Rect r = { ctl.x, ctl.y, SV_MIN(ctl.w, 170), 30 };
                Rect m = { r.x, r.y, 30, 30 }, pl = { r.x + r.w - 30, r.y, 30, 30 };
                button(p, HIT_PORT_MINUS, m, "\xe2\x88\x92", BTN_NORMAL, editable);
                button(p, HIT_PORT_PLUS, pl, "+", BTN_NORMAL, editable);
                if (p->port_edit) {
                    if (!line_edit_focused(p->port_edit)) {
                        snprintf(buf, sizeof(buf), "%d", g_settings.server_port);
                        if (strcmp(p->port_edit->text, buf)) line_edit_set_text(p->port_edit, buf);
                    }
                    p->port_edit->base.bounds = (Rect){m.x + 34, r.y, r.w - 68, 30};
                    p->port_edit->base.visible = true;
                    widget_draw((Widget*)p->port_edit, win);
                }
                break;
            }
            case 1:
                segmented(p, HIT_ACCESS_LOCAL, ctl, "This computer", "Local network", g_settings.server_lan ? 1 : 0, editable);
                break;
            case 2:
                if (g_settings.server_timeout) snprintf(buf, sizeof(buf), "%d s", g_settings.server_timeout);
                else snprintf(buf, sizeof(buf), "no limit");
                stepper(p, HIT_TIME_MINUS, HIT_TIME_PLUS, (Rect){ctl.x, ctl.y, SV_MIN(ctl.w, 170), 30}, buf, editable);
                break;
            case 3: {
                bool forced = g_settings.server_lan || g_settings.server_cors;
                Rect sg = { ctl.x, ctl.y, SV_MIN(ctl.w, 170), 30 };
                segmented(p, HIT_TOKEN_OFF, sg, "Off", "Required", token_required() ? 1 : 0, editable && !forced);
                if (token_required()) {
                    int bx2 = sg.x + sg.w + 8;
                    int cw2 = button_w(p, "Copy"), nw = button_w(p, "New");
                    int avail = ix + col_w - bx2;
                    if (avail >= cw2 + nw + 8) {
                        button(p, HIT_COPY_TOKEN, (Rect){bx2, ctl.y, cw2, 30}, "Copy", BTN_NORMAL, p->token[0] != 0);
                        button(p, HIT_NEW_TOKEN, (Rect){bx2 + cw2 + 6, ctl.y, nw, 30}, "New", BTN_NORMAL, true);
                        if (avail > cw2 + nw + 60 && p->token[0]) {
                            char shown[24];
                            snprintf(shown, sizeof(shown), "%.8s\xe2\x80\xa6", p->token);
                            ui_text_clipped(win, shown, bx2 + cw2 + nw + 18, ui_text_y(win, ctl, TH_FONT_SMALL, FONT_MONO),
                                            avail - cw2 - nw - 18, TH_TEXT_DIM, TH_FONT_SMALL, FONT_MONO);
                        }
                    }
                }
                break;
            }
            case 4:
                segmented(p, HIT_CORS_OFF, (Rect){ctl.x, ctl.y, SV_MIN(ctl.w, 260), 30}, "Same origin", "Any origin",
                          g_settings.server_cors ? 1 : 0, editable);
                break;
        }
    }
    y += oc.h + 12;

    // ── prova rapida ──
    Rect xc = { x0, y, cw, 48 };
    card(win, xc);
    char ex[400];
    example_text(p, ex, sizeof(ex), true);
    const char* cl = "Copy";
    int clw = button_w(p, cl);
    platform_draw_text_ex(win, "Try it", -1, (Point){xc.x + 16, ui_text_y(win, xc, TH_FONT_UI, FONT_UI_BOLD)}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
    ui_text_clipped(win, ex, xc.x + 76, ui_text_y(win, xc, TH_FONT_SMALL + 1, FONT_MONO), xc.w - 76 - clw - 28, TH_TEXT_MUTED,
                    TH_FONT_SMALL + 1, FONT_MONO);
    button(p, HIT_COPY_EXAMPLE, (Rect){xc.x + xc.w - clw - 10, xc.y + 9, clw, 30}, cl, BTN_NORMAL, true);
    y += xc.h + 12;

    // ── registro delle richieste ──
    Rect lc = { x0, y, cw, SV_MAX(110, H - y - 16) };
    card(win, lc);
    platform_draw_text_ex(win, "Requests", -1, (Point){lc.x + 16, lc.y + 12}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
    int shown = 0;
    for (int i = 0; i < p->nlog; i++) if (p->log[i].seq > p->log_cleared) shown++;
    char cnt[64];
    snprintf(cnt, sizeof(cnt), shown ? "last %d" : "none yet", shown);
    platform_draw_text_ex(win, cnt, -1, (Point){lc.x + 16 + platform_measure_text_ex(win, "Requests", -1, TH_FONT_UI, FONT_UI_BOLD) + 10, lc.y + 13},
                          TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    int clrw = button_w(p, "Clear");
    button(p, HIT_CLEAR_LOG, (Rect){lc.x + lc.w - clrw - 10, lc.y + 7, clrw, 26}, "Clear", BTN_GHOST, shown > 0);

    // colonne: tempo, client, metodo, percorso, stato, ms, dettaglio
    static const char* const COLS[7] = { "Time", "Client", "Method", "Path", "Status", "ms", "SQL / message" };
    int cx[8];
    int widths[6] = { 72, 112, 62, 170, 56, 64 };
    if (cw < 760) { widths[1] = 0; widths[3] = 130; }
    cx[0] = lc.x + 16;
    for (int i = 0; i < 6; i++) cx[i + 1] = cx[i] + widths[i];
    cx[7] = lc.x + lc.w - 16;
    int hy = lc.y + 40;
    platform_fill_rect(win, (Rect){lc.x + 1, hy, lc.w - 2, 24}, TH_SURFACE_2);
    for (int i = 0; i < 7; i++) {
        if (cx[i + 1] - cx[i] <= 0) continue;
        platform_draw_text_ex(win, COLS[i], -1, (Point){cx[i], ui_text_y(win, (Rect){0, hy, 0, 24}, TH_FONT_SMALL, FONT_UI_BOLD)},
                              TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    }
    int ry = hy + 26, rh = 24;
    p->log_rect = (Rect){lc.x, ry, lc.w, lc.y + lc.h - ry - 6};
    int visible = SV_MAX(1, p->log_rect.h / rh);
    p->log_scroll = SV_CLAMP(p->log_scroll, 0, SV_MAX(0, shown - visible));
    platform_set_clip(win, p->log_rect);
    if (!shown) {
        const char* empty = running ? "Waiting for the first request\xe2\x80\xa6 (try the curl command above)"
                                    : "Requests appear here while the server is running.";
        platform_draw_text_ex(win, empty, -1, (Point){lc.x + 16, ry + 8}, TH_TEXT_FAINT, TH_FONT_UI, FONT_UI);
    }
    int row = 0;
    for (int i = 0; i < p->nlog; i++) {
        const SvLogEntry* e = &p->log[i];
        if (e->seq <= p->log_cleared) continue;
        if (row++ < p->log_scroll) continue;
        int yy = ry + (row - 1 - p->log_scroll) * rh;
        if (yy > p->log_rect.y + p->log_rect.h) break;
        Rect rr = { lc.x + 1, yy, lc.w - 2, rh };
        if ((row & 1) == 0) platform_fill_rect(win, rr, TH_ROW_ODD);
        int ty = ui_text_y(win, rr, TH_FONT_SMALL + 1, FONT_MONO);
        char ms[24], status[8];
        snprintf(ms, sizeof(ms), "%.1f", e->ms);
        snprintf(status, sizeof(status), "%d", e->status);
        Color sc2 = e->status >= 500 ? TH_ERROR_TEXT : e->status >= 400 ? TH_WARNING : TH_SUCCESS;
        const char* vals[7] = { e->when, e->client, e->method, e->path, status, ms, e->detail };
        for (int k = 0; k < 7; k++) {
            int w = cx[k + 1] - cx[k] - 8;
            if (w <= 0) continue;
            Color c = k == 4 ? sc2 : k == 6 ? TH_TEXT_MUTED : TH_TEXT;
            ui_text_clipped(win, vals[k], cx[k], ty, w, c, TH_FONT_SMALL + 1, FONT_MONO);
        }
    }
    platform_reset_clip(win);
}

// ── eventi ──────────────────────────────────────────────────────

static void copy_text(ServerPanel* p, int id, const char* text) {
    platform_set_clipboard(p->win, text);
    snprintf(p->flash, sizeof(p->flash), "Copied");
    p->flash_hit = id;
    p->flash_until = platform_time_ms() + 1200;
}

static void commit_port(ServerPanel* p) {
    if (!p->port_edit) return;
    line_edit_focus(p->port_edit, false);
    char* end;
    long v = strtol(p->port_edit->text, &end, 10);
    if (end != p->port_edit->text && !*end && v >= 1024 && v <= 65535 && v != g_settings.server_port) {
        g_settings.server_port = (int)v;
        config_changed(p);
    }
}

ViewAction server_panel_event(ServerPanel* p, sEvent* evt) {
    if (!p) return VIEW_NONE;
    LineEdit* pe = p->port_edit;
    if (evt->type == EVT_MOUSE_MOVE) {
        p->mx = evt->mouse_x; p->my = evt->mouse_y;
        if (pe) widget_handle_event((Widget*)pe, evt);
        return VIEW_REDRAW;
    }
    if (evt->type == EVT_MOUSE_WHEEL) {
        if (in_rect(p->log_rect, evt->mouse_x, evt->mouse_y)) { p->log_scroll -= evt->wheel_delta * 3; if (p->log_scroll < 0) p->log_scroll = 0; }
        return VIEW_REDRAW;
    }
    // campo della porta: tastiera mentre ha il focus, Invio conferma, Esc annulla
    if (pe && line_edit_focused(pe) && (evt->type == EVT_KEY_DOWN || evt->type == EVT_CHAR)) {
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ENTER) { commit_port(p); return VIEW_REDRAW; }
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ESCAPE) { line_edit_focus(pe, false); return VIEW_REDRAW; }
        if (evt->type == EVT_CHAR && (evt->character < '0' || evt->character > '9')) return VIEW_NONE;
        widget_handle_event((Widget*)pe, evt);
        return VIEW_REDRAW;
    }
    if (evt->type == EVT_MOUSE_UP) {
        if (pe) widget_handle_event((Widget*)pe, evt);
        return VIEW_REDRAW;
    }
    if (evt->type == EVT_KEY_DOWN) {
        if (evt->key == KEY_DOWN) { p->log_scroll++; return VIEW_REDRAW; }
        if (evt->key == KEY_UP && p->log_scroll > 0) { p->log_scroll--; return VIEW_REDRAW; }
        if (evt->key == KEY_PAGEDOWN) { p->log_scroll += 10; return VIEW_REDRAW; }
        if (evt->key == KEY_PAGEUP) { p->log_scroll = SV_MAX(0, p->log_scroll - 10); return VIEW_REDRAW; }
        if (evt->key == KEY_HOME) { p->log_scroll = 0; return VIEW_REDRAW; }
        if (evt->key == KEY_F5) {                      // F5 = avvia / ferma
            if (p->srv) server_panel_stop(p); else server_panel_start(p);
            return VIEW_REDRAW;
        }
        return VIEW_NONE;
    }
    if (evt->type != EVT_MOUSE_DOWN || evt->mouse_button != SV_MOUSE_LEFT) return VIEW_NONE;

    int x = evt->mouse_x, y = evt->mouse_y;
    if (pe) {
        if (in_rect(pe->base.bounds, x, y)) {
            if (!line_edit_focused(pe)) { line_edit_focus(pe, true); line_edit_select_all(pe); }
            else widget_handle_event((Widget*)pe, evt);
            return VIEW_REDRAW;
        }
        if (line_edit_focused(pe)) commit_port(p);
    }
    int hit = -1;
    for (int i = 0; i < HIT_COUNT && hit < 0; i++) if (in_rect(p->hit[i], x, y)) hit = i;
    char ex[400];
    switch (hit) {
        case HIT_EDITOR: return VIEW_OPEN_EDITOR;
        case HIT_TOGGLE: if (p->srv) server_panel_stop(p); else server_panel_start(p); break;
        case HIT_COPY_URL: copy_text(p, hit, p->url); break;
        case HIT_COPY_LAN: copy_text(p, hit, p->lan_url); break;
        case HIT_PORT_MINUS: case HIT_PORT_PLUS:
            g_settings.server_port = SV_CLAMP(g_settings.server_port + (hit == HIT_PORT_PLUS ? 1 : -1), 1024, 65535);
            config_changed(p);
            break;
        case HIT_ACCESS_LOCAL: case HIT_ACCESS_LAN:
            if (g_settings.server_lan != (hit == HIT_ACCESS_LAN)) { g_settings.server_lan = hit == HIT_ACCESS_LAN; config_changed(p); }
            break;
        case HIT_TOKEN_OFF: case HIT_TOKEN_ON:
            if (g_settings.server_token != (hit == HIT_TOKEN_ON)) { g_settings.server_token = hit == HIT_TOKEN_ON; config_changed(p); }
            break;
        case HIT_COPY_TOKEN: copy_text(p, hit, p->token); break;
        case HIT_NEW_TOKEN:
            token_load(p, true);                        // le app con il token vecchio ricevono 401
            config_changed(p);
            snprintf(p->flash, sizeof(p->flash), "Done");
            p->flash_hit = hit;
            p->flash_until = platform_time_ms() + 1200;
            break;
        case HIT_CORS_OFF: case HIT_CORS_ON:
            if (g_settings.server_cors != (hit == HIT_CORS_ON)) { g_settings.server_cors = hit == HIT_CORS_ON; config_changed(p); }
            break;
        case HIT_TIME_MINUS: case HIT_TIME_PLUS: {
            static const int STEPS[] = { 0, 5, 10, 30, 60, 120, 300, 600 };
            int n = (int)(sizeof(STEPS) / sizeof(STEPS[0])), k = 0;
            while (k < n - 1 && STEPS[k] < g_settings.server_timeout) k++;
            k = SV_CLAMP(k + (hit == HIT_TIME_PLUS ? 1 : -1), 0, n - 1);
            g_settings.server_timeout = STEPS[k];
            config_changed(p);
            break;
        }
        case HIT_COPY_EXAMPLE: example_text(p, ex, sizeof(ex), false); copy_text(p, hit, ex); break;
        case HIT_CLEAR_LOG: p->log_cleared = p->nlog ? p->log[0].seq : 0; p->log_scroll = 0; break;
        default: return VIEW_NONE;
    }
    return VIEW_REDRAW;
}
