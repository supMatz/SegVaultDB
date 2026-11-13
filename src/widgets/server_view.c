#include "server_view.h"
#include "theme.h"
#include "settings.h"
#include "line_edit.h"
#include "button.h"
#include "../server/server.h"
#include "../server/fastcgi.h"
#include "../server/tls.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
//  Stessa struttura dell'editor: toolbar, sidebar a sinistra (qui le opzioni del server al posto
//  dell'explorer), intestazione "REQUESTS" come "RESULTS", griglia del registro, barra di stato.
// ═════════════════════════════════════════════════════════════════

#define SP_TOOLBAR_H  52
#define SP_HEADER_H   38          // intestazioni "SERVER" e "REQUESTS" (come EXPLORER / RESULTS)
#define SP_FOOTER_H   26
#define SP_ROW_H      26          // righe del registro
#define SP_SIDEBAR_W  296

enum {
    HIT_COPY_URL, HIT_COPY_LAN, HIT_PORT_MINUS, HIT_PORT_PLUS, HIT_ACCESS_LOCAL, HIT_ACCESS_LAN,
    HIT_TOKEN_OFF, HIT_TOKEN_ON, HIT_COPY_TOKEN, HIT_NEW_TOKEN, HIT_CORS_OFF, HIT_CORS_ON, HIT_TIME_MINUS,
    HIT_TIME_PLUS, HIT_COPY_EXAMPLE, HIT_CLEAR_LOG, HIT_OPEN_SITE, HIT_CLEAR_WEB, HIT_USERS_OFF, HIT_USERS_ON, HIT_TAB_REQ, HIT_TAB_MON, HIT_MYSQL_OFF, HIT_MYSQL_ON, HIT_COPY_MYSQL, HIT_PHP_OFF, HIT_PHP_ON, HIT_MAIL_OFF, HIT_MAIL_ON, HIT_OPEN_MAIL, HIT_OPEN_DASH, HIT_HTTPS_OFF, HIT_HTTPS_ON, HIT_TRUST_CA, HIT_OPEN_HTTPS, HIT_EDIT_PHPINI, HIT_LF0, HIT_LF1, HIT_LF2, HIT_LF3, HIT_LF4, HIT_LF5, HIT_COUNT
};

enum { ACT_NONE, ACT_TOGGLE, ACT_EDITOR, ACT_COPY };

#define MON_SECS 120             // secondi mostrati nei grafici

struct ServerPanel {
    PlatformWindow* win;
    char            data_dir[512];
    char            settings_path[1100];
    SvServer*       srv;
    PlatformThread* thread;
    char            token[65];
    char            error[700];
    char            url[96], lan_url[96], mysql_addr[96];
    SvStats         stats;
    SvLogEntry      log[SV_LOG_MAX];
    int             nlog;
    long long       log_seq;         // ultimo numero di richiesta mostrato
    long long       log_cleared;     // richieste nascoste da "Clear"
    int             log_scroll;
    Rect            log_rect;
    Rect            side_view;       // parte visibile della colonna delle opzioni (scorre con la rotella)
    int             side_scroll;     // pixel scorsi
    int             side_content;    // altezza del contenuto
    uint64_t        last_poll;
    int             last_uptime;
    LineEdit*       port_edit;
    Button*         btn_toggle;      // Start / Stop (F5)
    Button*         btn_copy;        // Copy address
    Button*         btn_editor;      // Open SQL editor
    int             pending;         // azione chiesta da un pulsante (ACT_*)
    Rect            hit[HIT_COUNT];
    int             mx, my;
    char            flash[64];       // "Copied" al posto del testo del controllo
    LineEdit*       web_edit;        // cartella del sito servito insieme all'API (come la htdocs di XAMPP)
    char            web_dir[1024];
    char            php_cgi[1024];   // percorso di php-cgi scelto a mano (server.ini "php_cgi="); "" = cercato
    char            php_desc[600];   // processi PHP del server avviato
    char            site_url[96];
    char            mail_url[128], dash_url[128], https_url[128];
    char            trust_msg[400];  // esito di "Trust certificate"
    int             log_filter;      // registro: 0 tutto, 1 web, 2 PHP, 3 API, 4 MySQL, 5 posta
    int             mail_count;
    // siti con nome da server.ini ("site.blog=/percorso" o "site.app=http://127.0.0.1:5173")
    char            site_name[32][64];
    char            site_target[32][1024];
    int             n_sites;
    int             flash_hit;
    uint64_t        flash_until;
    // pagina Monitor
    int             tab;             // 0 = registro delle richieste, 1 = Monitor
    SvBucket        series[MON_SECS];
    DbEngineStats   es;
    bool            have_es;
    uint64_t        last_mon;
    Rect            chart_req, chart_lat;   // aree dei grafici (per il tooltip)
};

static void on_toggle(void* ud) { ((ServerPanel*)ud)->pending = ACT_TOGGLE; }
static void on_editor(void* ud) { ((ServerPanel*)ud)->pending = ACT_EDITOR; }
static void on_copy(void* ud)   { ((ServerPanel*)ud)->pending = ACT_COPY; }

static bool token_required(void);
static bool token_load(ServerPanel* p, bool regenerate);
static void web_load(ServerPanel* p);
static void web_commit(ServerPanel* p);

ServerPanel* server_panel_create(PlatformWindow* win) {
    ServerPanel* p = calloc(1, sizeof(ServerPanel));
    if (!p) return NULL;
    p->win = win;
    p->port_edit = line_edit_create("8484");
    if (p->port_edit) p->port_edit->win = win;
    p->web_edit = line_edit_create("Folder with index.html (optional)");
    if (p->web_edit) p->web_edit->win = win;
    p->btn_toggle = button_create(0, 0, 0, 0, "Start server", on_toggle, p);
    p->btn_copy   = button_create(0, 0, 0, 0, "Copy address", on_copy, p);
    p->btn_editor = button_create(0, 0, 0, 0, "Open SQL editor", on_editor, p);
    if (p->btn_toggle) button_set_hint(p->btn_toggle, "F5");
    p->flash_hit = -1;
    return p;
}

void server_panel_set_paths(ServerPanel* p, const char* data_dir, const char* settings_path) {
    if (!p) return;
    snprintf(p->data_dir, sizeof(p->data_dir), "%s", data_dir ? data_dir : "");
    snprintf(p->settings_path, sizeof(p->settings_path), "%s", settings_path ? settings_path : "");
    if (token_required()) token_load(p, false);
    web_load(p);
    if (p->web_edit) line_edit_set_text(p->web_edit, p->web_dir);
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

// cartella web e PHP in <data>/server.ini ("web_folder=...", "php_cgi=...")
static void web_path(ServerPanel* p, char* out, size_t n) {
    snprintf(out, n, "%s%s%s", p->data_dir, p->data_dir[0] ? "/" : "", "server.ini");
}

static void web_load(ServerPanel* p) {
    char path[600], line[1200];
    p->web_dir[0] = '\0';
    p->php_cgi[0] = '\0';
    p->n_sites = 0;
    web_path(p, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "web_folder=", 11)) snprintf(p->web_dir, sizeof(p->web_dir), "%.1000s", line + 11);
        if (!strncmp(line, "php_cgi=", 8)) snprintf(p->php_cgi, sizeof(p->php_cgi), "%.1000s", line + 8);
        char* eq = strchr(line, '=');
        if (!strncmp(line, "site.", 5) && eq && eq - line > 5 && eq - line < 5 + 63 && p->n_sites < 32) {
            snprintf(p->site_name[p->n_sites], sizeof(p->site_name[0]), "%.*s", (int)(eq - line - 5), line + 5);
            snprintf(p->site_target[p->n_sites], sizeof(p->site_target[0]), "%.1000s", eq + 1);
            p->n_sites++;
        }
    }
    fclose(f);
}

static void web_save(ServerPanel* p) {
    char path[600];
    web_path(p, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "web_folder=%s\n", p->web_dir);
    if (p->php_cgi[0]) fprintf(f, "php_cgi=%s\n", p->php_cgi);
    for (int i = 0; i < p->n_sites; i++) fprintf(f, "site.%s=%s\n", p->site_name[i], p->site_target[i]);
    fclose(f);
}

static void save_settings(ServerPanel* p) {
    if (p->settings_path[0]) settings_save(p->settings_path);
}

static void run_server_thread(void* arg) { sv_server_run((SvServer*)arg); }

// indirizzi dei servizi accesi (dopo l'avvio e dopo ogni interruttore)
static void panel_addrs(ServerPanel* p) {
    if (!p->srv) return;
    snprintf(p->php_desc, sizeof(p->php_desc), "%s", sv_server_php(p->srv));
    snprintf(p->mail_url, sizeof(p->mail_url), "http://localhost:%d/.segvault/mail/", sv_server_port(p->srv));
    snprintf(p->dash_url, sizeof(p->dash_url), "http://localhost:%d/.segvault/", sv_server_port(p->srv));
    p->mail_count = sv_server_mail_count(p->srv);
    p->https_url[0] = '\0';
    if (sv_server_https_port(p->srv)) snprintf(p->https_url, sizeof(p->https_url), "https://localhost:%d/", sv_server_https_port(p->srv));
    p->mysql_addr[0] = '\0';
    if (sv_server_mysql_port(p->srv)) snprintf(p->mysql_addr, sizeof(p->mysql_addr), "mysql://127.0.0.1:%d", sv_server_mysql_port(p->srv));
}

// un servizio acceso o spento: con il server in funzione cambia solo lui (gli altri non si fermano)
static void service_switch(ServerPanel* p, int* setting, bool on, SvService svc) {
    if (*setting == (int)on) return;
    *setting = on;
    save_settings(p);
    if (!p->srv) return;
    char e[400];
    p->error[0] = '\0';
    if (!sv_server_service(p->srv, svc, on, e, sizeof(e))) {
        if (strstr(e, "no php-cgi configured")) {     // PHP non era pronto all'avvio (cartella web scelta dopo): riavvio
            server_panel_stop(p);
            server_panel_start(p);
            return;
        }
        snprintf(p->error, sizeof(p->error), "%s%s", e, strstr(e, "is in use") ? ". Stop that program or change the port in Settings > Server" : "");
        *setting = !on;
        save_settings(p);
    }
    panel_addrs(p);
}

bool server_panel_start(ServerPanel* p) {
    if (!p) return false;
    if (p->srv) return true;
    p->error[0] = '\0';
    if (token_required() && !p->token[0] && !token_load(p, false)) {
        snprintf(p->error, sizeof(p->error), "Cannot generate a token");
        return false;
    }
    // cartella del sito: "~" = home; deve esistere
    char web[1100] = "";
    if (p->web_dir[0]) {
        const char* home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");
        if (p->web_dir[0] == '~' && home) snprintf(web, sizeof(web), "%s%s", home, p->web_dir + 1);
        else snprintf(web, sizeof(web), "%s", p->web_dir);
        struct stat st;
        if (stat(web, &st) != 0 || (st.st_mode & S_IFMT) != S_IFDIR) {
            snprintf(p->error, sizeof(p->error), "Web folder not found: %.200s", web);
            return false;
        }
    }
    // PHP: il php-cgi di server.ini, altrimenti cercato (php/ accanto a SegVault, PATH, C:\xampp\php, ...)
    // trovato anche con PHP spento: si potrà accendere con il server in funzione
    char php[1024] = "";
    if (p->php_cgi[0]) snprintf(php, sizeof(php), "%s", p->php_cgi);
    else if (!fcgi_find_php_cgi(php, sizeof(php))) php[0] = '\0';
    if (!web[0] && p->n_sites == 0) php[0] = '\0';
    if (g_settings.server_php) {
        if (!web[0] && p->n_sites == 0) {
            snprintf(p->error, sizeof(p->error), "PHP runs the files of the web folder: choose one below (or turn PHP off)");
            return false;
        }
        if (!php[0]) {
            snprintf(p->error, sizeof(p->error), "php-cgi not found: put PHP in a php folder next to SegVault, install it "
                     "(e.g. XAMPP, or php-cgi on Linux), or write php_cgi=PATH in server.ini");
            return false;
        }
    }
    SvSite sites[32];
    for (int i = 0; i < p->n_sites; i++) { sites[i].name = p->site_name[i]; sites[i].target = p->site_target[i]; }
    char mail_dir[600], cert_dir[600];
    snprintf(cert_dir, sizeof(cert_dir), "%s%scerts", p->data_dir, p->data_dir[0] ? "/" : "");
    snprintf(mail_dir, sizeof(mail_dir), "%s%smail", p->data_dir, p->data_dir[0] ? "/" : "");
    SvServerConfig cfg = {
        .sites       = sites,
        .n_sites     = p->n_sites,
        .dir_listing = true,
        .mail        = g_settings.server_mail != 0,
        .mail_port   = g_settings.server_mail_port,
        .mail_dir    = mail_dir,
        .https       = g_settings.server_https != 0,
        .https_port  = g_settings.server_https_port,
        .cert_dir    = cert_dir,
        .http2       = true,
        .php_cgi     = php[0] ? php : NULL,
        .php_start_off = !g_settings.server_php,
        .static_dir  = web[0] ? web : NULL,
        .bind_addr   = g_settings.server_lan ? "0.0.0.0" : "127.0.0.1",
        .port        = g_settings.server_port,
        .token       = token_required() ? p->token : NULL,
        .cors_origin = g_settings.server_cors ? "*" : NULL,
        .timeout_ms  = g_settings.server_timeout * 1000,
        .quiet       = true,
        .require_login = g_settings.server_users != 0,
        .mysql       = g_settings.server_mysql != 0,
        .mysql_port  = g_settings.server_mysql_port,
    };
    char err[600];
    p->srv = sv_server_start(&cfg, err, sizeof(err));
    // porte occupate: chi le usa, e come uscirne (fermare l'altro programma o cambiare porta)
    if (!p->srv && strstr(err, "is in use"))
        snprintf(err + strlen(err), sizeof(err) - strlen(err), ". Stop that program, turn the service off, or change its port in Settings > Server");
    if (!p->srv) { snprintf(p->error, sizeof(p->error), "%s", err); return false; }
    panel_addrs(p);
    p->thread = platform_thread_start(run_server_thread, p->srv);
    if (!p->thread) {
        sv_server_free(p->srv);
        p->srv = NULL;
        snprintf(p->error, sizeof(p->error), "Cannot start the server thread");
        return false;
    }
    snprintf(p->url, sizeof(p->url), "http://127.0.0.1:%d%s", sv_server_port(p->srv), sv_server_api_prefix(p->srv));
    snprintf(p->site_url, sizeof(p->site_url), "http://127.0.0.1:%d/", sv_server_port(p->srv));
    panel_addrs(p);
    char ip[64];
    p->lan_url[0] = '\0';
    if (g_settings.server_lan && sv_local_ip(ip, sizeof(ip)))
        snprintf(p->lan_url, sizeof(p->lan_url), "http://%s:%d%s", ip, sv_server_port(p->srv), sv_server_api_prefix(p->srv));
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
    if (p->btn_toggle) widget_destroy((Widget*)p->btn_toggle);
    if (p->btn_copy) widget_destroy((Widget*)p->btn_copy);
    if (p->btn_editor) widget_destroy((Widget*)p->btn_editor);
    if (p->web_edit) widget_destroy((Widget*)p->web_edit);
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
    if (p->web_edit && line_edit_tick(p->web_edit, now_ms)) redraw = true;
    if (p->flash_hit >= 0 && now_ms >= p->flash_until) { p->flash_hit = -1; redraw = true; }
    if (p->tab == 1 && now_ms - p->last_mon >= 1000) {      // Monitor: dati una volta al secondo
        p->last_mon = now_ms;
        if (p->srv) sv_server_series(p->srv, p->series, MON_SECS);
        p->have_es = db_engine_stats(&p->es);
        redraw = true;
    }
    if (!p->srv || now_ms - p->last_poll < 250) return redraw;
    p->last_poll = now_ms;
    SvStats st;
    sv_server_stats(p->srv, &st);
    if (st.requests != p->log_seq) {
        p->nlog = sv_server_recent(p->srv, p->log, SV_LOG_MAX);
        p->log_seq = st.requests;
        redraw = true;
    }
    int mc = sv_server_mail_count(p->srv);
    if (mc != p->mail_count) { p->mail_count = mc; redraw = true; }
    if ((int)st.uptime_s != p->last_uptime || st.connections != p->stats.connections || st.busy != p->stats.busy) redraw = true;
    p->last_uptime = (int)st.uptime_s;
    p->stats = st;
    return redraw;
}

// ── disegno ─────────────────────────────────────────────────────

// testo cliccabile piccolo (Copy, New, Clear): come i pulsanti "ghost" dell'editor
static void link_btn(ServerPanel* p, int id, Rect r, const char* label, bool enabled) {
    PlatformWindow* win = p->win;
    p->hit[id] = enabled ? r : (Rect){0, 0, 0, 0};
    bool hov = enabled && in_rect(r, p->mx, p->my);
    if (hov) platform_fill_rect_rounded(win, r, TH_HOVER, 5);
    const char* l = p->flash_hit == id ? p->flash : label;
    Color c = !enabled ? TH_TEXT_FAINT : p->flash_hit == id ? TH_SUCCESS : hov ? TH_TEXT : TH_TEXT_MUTED;
    text_center(win, r, l, c, TH_FONT_SMALL + 1, FONT_UI);
}

static int link_w(ServerPanel* p, const char* label) {
    return platform_measure_text_ex(p->win, label, -1, TH_FONT_SMALL + 1, FONT_UI) + 16;
}

// controllo a due posizioni (stesso stile delle schede dei risultati)
static void segmented(ServerPanel* p, int id0, Rect r, const char* a, const char* b, int sel, bool enabled) {
    PlatformWindow* win = p->win;
    ui_round_box(win, r, TH_SURFACE_2, TH_BORDER, 6);
    for (int k = 0; k < 2; k++) {
        Rect h = { r.x + 2 + k * (r.w - 4) / 2, r.y + 2, (r.w - 4) / 2, r.h - 4 };
        p->hit[id0 + k] = enabled ? h : (Rect){0, 0, 0, 0};
        bool on = sel == k, hov = enabled && !on && in_rect(h, p->mx, p->my);
        if (on) platform_fill_rect_rounded(win, h, enabled ? TH_ACCENT_SOFT : TH_ELEVATED, 5);
        else if (hov) platform_fill_rect_rounded(win, h, TH_HOVER, 5);
        // bloccato (es. token obbligatorio in rete): la scelta attiva resta leggibile
        Color tc = on ? (enabled ? TH_ACCENT_TEXT : TH_TEXT_MUTED) : (enabled ? TH_TEXT_MUTED : TH_TEXT_FAINT);
        text_center(win, h, k ? b : a, tc, TH_FONT_SMALL + 1, on ? FONT_UI_BOLD : FONT_UI);
    }
}

// [−] valore [+]
static void stepper(ServerPanel* p, int id_minus, int id_plus, Rect r, const char* value, Widget* field) {
    PlatformWindow* win = p->win;
    Rect m = { r.x, r.y, r.h, r.h }, pl = { r.x + r.w - r.h, r.y, r.h, r.h };
    for (int k = 0; k < 2; k++) {
        Rect b = k ? pl : m;
        int id = k ? id_plus : id_minus;
        p->hit[id] = b;
        bool hov = in_rect(b, p->mx, p->my);
        ui_round_box(win, b, hov ? TH_ELEVATED_HOVER : TH_ELEVATED, hov ? TH_BORDER_STRONG : TH_BORDER, 6);
        text_center(win, b, k ? "+" : "\xe2\x88\x92", TH_TEXT_MUTED, TH_FONT_UI, FONT_UI);
    }
    Rect v = { m.x + m.w + 4, r.y, r.w - 2 * r.h - 8, r.h };
    if (field) { field->bounds = v; field->visible = true; widget_draw(field, win); }
    else {
        ui_round_box(win, v, TH_EDITOR_BG, TH_BORDER, 6);
        text_center(win, v, value, TH_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
    }
}

// etichetta di sezione della sidebar (come "EXPLORER")
static int section_label(PlatformWindow* win, const char* t, int x, int y) {
    platform_draw_text_ex(win, t, -1, (Point){x, y}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    return y + platform_text_height(win, TH_FONT_SMALL, FONT_UI_BOLD) + 6;
}

static void format_uptime(double s, char* out, size_t n) {
    long t = (long)s;
    if (t < 60) snprintf(out, n, "%lds", t);
    else if (t < 3600) snprintf(out, n, "%ldm %02lds", t / 60, t % 60);
    else snprintf(out, n, "%ldh %02ldm", t / 3600, t / 60 % 60);
}

// masked: il token a schermo solo in parte (chi guarda lo schermo non lo legge); la copia è completa
static void example_text(ServerPanel* p, char* out, size_t n, bool masked) {
    const char* url = p->srv ? p->url : "http://127.0.0.1:8484/api/v1";
    char auth[120] = "";
    if (g_settings.server_users)
        snprintf(auth, sizeof(auth), " -u user:password");
    else if (token_required() && p->token[0])
        snprintf(auth, sizeof(auth), " -H \"Authorization: Bearer %.*s%s\"", masked ? 8 : 64, p->token, masked ? "\xe2\x80\xa6" : "");
    snprintf(out, n, "curl %s/query -H \"Content-Type: application/json\"%s -d \"{\\\"sql\\\": \\\"SHOW DATABASES\\\"}\"", url, auth);
}

static void draw_toolbar(ServerPanel* p, int W) {
    PlatformWindow* win = p->win;
    bool running = p->srv != NULL;
    platform_fill_rect(win, (Rect){0, 0, W, SP_TOOLBAR_H}, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){0, SP_TOOLBAR_H - 1, W, 1}, TH_BORDER);
    int cy = SP_TOOLBAR_H / 2;
    ui_icon_logo(win, 30, cy, 26);
    int ty = ui_text_y(win, (Rect){0, 0, 0, SP_TOOLBAR_H}, 16, FONT_UI_BOLD);
    int nw = platform_draw_text_ex(win, SV_NAME, -1, (Point){52, ty}, TH_TEXT, 16, FONT_UI_BOLD);
    char ver[32];
    snprintf(ver, sizeof(ver), "v%s", SV_VERSION_STR);
    Rect vr = { 52 + nw + 8, cy - 9, platform_measure_text_ex(win, ver, -1, TH_FONT_SMALL, FONT_MONO) + 12, 18 };
    platform_fill_rect_rounded(win, vr, TH_ELEVATED, 9);
    platform_draw_text_ex(win, ver, -1, (Point){vr.x + 6, ui_text_y(win, vr, TH_FONT_SMALL, FONT_MONO)}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_MONO);

    // pulsanti: avvia / ferma, copia indirizzo (come Run e Run statement)
    int x = vr.x + vr.w + 24;
    Button* t = p->btn_toggle;
    if (t) {
        button_set_label(t, running ? "Stop server" : "Start server");
        button_set_style(t, running ? BUTTON_STYLE_SECONDARY : BUTTON_STYLE_PRIMARY);
        button_set_icon(t, running ? BUTTON_ICON_STOP : BUTTON_ICON_PLAY, running ? TH_ERROR : (Color){0, 0, 0, 0});
        int w = button_preferred_width(t, win);
        t->base.bounds = (Rect){x, cy - 16, w, 32};
        widget_draw((Widget*)t, win);
        x += w + 8;
    }
    if (p->btn_copy) {
        button_set_label(p->btn_copy, p->flash_hit == HIT_COPY_URL && p->flash_until ? "Copied" : "Copy address");
        button_set_enabled(p->btn_copy, running);
        int w = button_preferred_width(p->btn_copy, win);
        p->btn_copy->base.bounds = (Rect){x, cy - 16, w, 32};
        widget_draw((Widget*)p->btn_copy, win);
        x += w;
    }

    // a destra: Open SQL editor e la pillola dello stato (come quella del database)
    int right = W - 16;
    if (p->btn_editor) {
        int w = button_preferred_width(p->btn_editor, win);
        right -= w;
        p->btn_editor->base.bounds = (Rect){right, cy - 16, w, 32};
        widget_draw((Widget*)p->btn_editor, win);
        right -= 10;
    }
    char st[96];
    if (running) snprintf(st, sizeof(st), "%s \xc2\xb7 port %d", p->stats.busy ? "Busy" : "Running", g_settings.server_port);
    else snprintf(st, sizeof(st), "%s", p->error[0] ? "Not running" : "Stopped");
    int sw = platform_measure_text_ex(win, st, -1, TH_FONT_UI, FONT_UI_BOLD);
    Rect pill = { right - (sw + 40), cy - 15, sw + 40, 30 };
    if (pill.x > x + 16) {
        ui_round_box(win, pill, TH_ELEVATED, TH_BORDER_STRONG, 15);
        Color dot = running ? (p->stats.busy ? TH_WARNING : TH_SUCCESS) : p->error[0] ? TH_ERROR : TH_TEXT_FAINT;
        ui_circle(win, pill.x + 17, cy, 4, dot);
        platform_draw_text_ex(win, st, -1, (Point){pill.x + 28, ui_text_y(win, pill, TH_FONT_UI, FONT_UI_BOLD)},
                              running ? TH_TEXT : TH_TEXT_DIM, TH_FONT_UI, FONT_UI_BOLD);
    }
}

static bool rect_inside(Rect a, Rect b) {
    return a.y >= b.y && a.y + a.h <= b.y + b.h;
}

// un punto della colonna delle opzioni nascosto dallo scorrimento (sotto l'intestazione o il piè di pagina)
static bool side_hidden(const ServerPanel* p, int x, int y) {
    return x < p->side_view.x + p->side_view.w && !in_rect(p->side_view, x, y);
}

static void draw_sidebar(ServerPanel* p, int SW, int H) {
    PlatformWindow* win = p->win;
    int top = SP_TOOLBAR_H, bottom = H - SP_FOOTER_H;
    platform_fill_rect(win, (Rect){0, top, SW, bottom - top}, TH_SURFACE);
    Rect hdr = { 0, top, SW - 1, SP_HEADER_H };
    platform_draw_text_ex(win, "SERVER", -1, (Point){18, ui_text_y(win, hdr, TH_FONT_SMALL, FONT_UI_BOLD)}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    platform_fill_rect(win, (Rect){SW - 1, top, 1, bottom - top}, TH_BORDER);
    // le opzioni scorrono sotto l'intestazione "SERVER" quando la finestra è bassa
    p->side_view = (Rect){ 0, top + SP_HEADER_H, SW - 1, SV_MAX(0, bottom - top - SP_HEADER_H) };
    p->side_scroll = SV_CLAMP(p->side_scroll, 0, SV_MAX(0, p->side_content - p->side_view.h));
    platform_set_clip(win, p->side_view);

    int x = 18, w = SW - 36, y0 = top + SP_HEADER_H + 4, y = y0 - p->side_scroll;
    char buf[64];
    bool running = p->srv != NULL;

    // indirizzi
    y = section_label(win, "ADDRESS", x, y);
    if (running) {
        int cw = link_w(p, "Copy");
        ui_text_clipped(win, p->url, x, y + 4, w - cw - 4, TH_ACCENT_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
        link_btn(p, HIT_COPY_URL, (Rect){x + w - cw, y, cw, 24}, "Copy", true);
        y += 26;
        if (p->lan_url[0]) {
            ui_text_clipped(win, p->lan_url, x, y + 4, w - cw - 4, TH_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
            link_btn(p, HIT_COPY_LAN, (Rect){x + w - cw, y, cw, 24}, "Copy", true);
            y += 26;
        }
        if (p->mysql_addr[0]) {
            ui_text_clipped(win, p->mysql_addr, x, y + 4, w - cw - 4, TH_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
            link_btn(p, HIT_COPY_MYSQL, (Rect){x + w - cw, y, cw, 24}, "Copy", true);
            y += 26;
        }
        if (p->error[0])                                // un servizio che non è partito
            y += text_wrap(win, p->error, x, y + 4, w, TH_ERROR_TEXT, TH_FONT_SMALL + 1) + 2;
    } else {
        y += text_wrap(win, p->error[0] ? p->error : "Not running. Press F5 or Start server.", x, y + 4, w,
                       p->error[0] ? TH_ERROR_TEXT : TH_TEXT_FAINT, TH_FONT_SMALL + 1) + 2;
    }
    y += 14;

    // opzioni
    y = section_label(win, "PORT", x, y);
    snprintf(buf, sizeof(buf), "%d", g_settings.server_port);
    if (p->port_edit && !line_edit_focused(p->port_edit) && strcmp(p->port_edit->text, buf)) line_edit_set_text(p->port_edit, buf);
    stepper(p, HIT_PORT_MINUS, HIT_PORT_PLUS, (Rect){x, y, SV_MIN(w, 170), 28}, buf, p->port_edit ? (Widget*)p->port_edit : NULL);
    y += 28 + 16;

    y = section_label(win, "REACHABLE FROM", x, y);
    segmented(p, HIT_ACCESS_LOCAL, (Rect){x, y, w, 28}, "This computer", "Local network", g_settings.server_lan ? 1 : 0, true);
    y += 28 + 16;

    y = section_label(win, "TOKEN", x, y);
    bool forced = g_settings.server_lan || g_settings.server_cors;
    segmented(p, HIT_TOKEN_OFF, (Rect){x, y, w, 28}, "Off", forced ? "Required (forced)" : "Required", token_required() ? 1 : 0, !forced);
    y += 28 + 4;
    if (token_required() && p->token[0]) {
        int cw = link_w(p, "Copy"), nw2 = link_w(p, "New");
        char shown[40];
        snprintf(shown, sizeof(shown), "%.10s\xe2\x80\xa6", p->token);
        ui_text_clipped(win, shown, x, y + 5, w - cw - nw2 - 8, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
        link_btn(p, HIT_COPY_TOKEN, (Rect){x + w - cw - nw2 - 2, y, cw, 24}, "Copy", true);
        link_btn(p, HIT_NEW_TOKEN, (Rect){x + w - nw2, y, nw2, 24}, "New", true);
        y += 26;
    }
    y += 12;

    y = section_label(win, "USER LOGIN", x, y);
    segmented(p, HIT_USERS_OFF, (Rect){x, y, w, 28}, "Off", "Required", g_settings.server_users ? 1 : 0, true);
    y += 28 + 4;
    if (g_settings.server_users) {
        int nu = 0;
        if (!running) { DbUserInfo tmp[1]; nu = db_list_users(tmp, 1); }
        y += text_wrap(win, running || nu ? "Apps send user:password (HTTP Basic); the token, if set, still gives full access."
                                          : "No users yet: create them in Settings \xe2\x80\xba Users & permissions.",
                       x, y + 2, w, running || nu ? TH_TEXT_FAINT : TH_WARNING, TH_FONT_SMALL) + 2;
    }
    y += 12;

    y = section_label(win, "MYSQL PROTOCOL", x, y);
    char lb[200];
    snprintf(lb, sizeof(lb), "Port %d", g_settings.server_mysql_port);
    segmented(p, HIT_MYSQL_OFF, (Rect){x, y, w, 28}, "Off", lb, g_settings.server_mysql ? 1 : 0, true);
    y += 28 + 4;
    if (g_settings.server_mysql) {
        snprintf(lb, sizeof(lb), "PHP, Python, Java and the mysql client connect as to MySQL: host 127.0.0.1, port %d, "
                 "a user from Users & permissions (or root with the token as password).", g_settings.server_mysql_port);
        y += text_wrap(win, lb, x, y + 2, w, TH_TEXT_FAINT, TH_FONT_SMALL) + 2;
    }
    y += 12;

    y = section_label(win, "WEB APPS (CORS)", x, y);
    segmented(p, HIT_CORS_OFF, (Rect){x, y, w, 28}, "Same origin", "Any origin", g_settings.server_cors ? 1 : 0, true);
    y += 28 + 16;

    y = section_label(win, "TIME LIMIT", x, y);
    if (g_settings.server_timeout) snprintf(buf, sizeof(buf), "%d s", g_settings.server_timeout);
    else snprintf(buf, sizeof(buf), "none");
    stepper(p, HIT_TIME_MINUS, HIT_TIME_PLUS, (Rect){x, y, SV_MIN(w, 170), 28}, buf, NULL);
    y += 28 + 16;

    // sito servito insieme all'API (stessa origine: niente CORS), come la htdocs di XAMPP
    y = section_label(win, "WEB FOLDER", x, y);
    if (p->web_edit) {
        p->web_edit->base.bounds = (Rect){x, y, w, 28};
        p->web_edit->base.visible = true;
        widget_draw((Widget*)p->web_edit, win);
    }
    y += 28 + 4;
    if (p->web_dir[0]) {
        int ow = link_w(p, "Open site"), cw2 = link_w(p, "Clear");
        if (running) {
            link_btn(p, HIT_OPEN_SITE, (Rect){x - 8, y, ow, 24}, "Open site", true);
            ui_text_clipped(win, p->site_url, x + ow, y + 5, w - ow - cw2 - 8, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
        } else platform_draw_text_ex(win, "Served at / when running", -1, (Point){x, y + 5}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
        link_btn(p, HIT_CLEAR_WEB, (Rect){x + w - cw2, y, cw2, 24}, "Clear", true);
        y += 26;
    } else {
        platform_draw_text_ex(win, "Enter a folder to serve a website", -1, (Point){x, y + 3}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
        y += 22;
    }
    y += 12;

    // PHP nella cartella web (come Apache + PHP di XAMPP)
    y = section_label(win, "PHP", x, y);
    segmented(p, HIT_PHP_OFF, (Rect){x, y, w, 28}, "Off", "Run .php files", g_settings.server_php ? 1 : 0, true);
    y += 28 + 4;
    if (g_settings.server_php) {
        char msg[800];
        if (running && p->php_desc[0])
            snprintf(msg, sizeof(msg), "%s. Pretty URLs go to index.php; PHP warnings appear in the request log.%s", p->php_desc,
                     g_settings.server_mysql ? "" : " Turn the MySQL protocol on so scripts reach the database (mysqli, PDO).");
        else
            snprintf(msg, sizeof(msg), "php-cgi is looked for in a php folder next to SegVault, the PATH, C:\\xampp\\php "
                     "(or set php_cgi=PATH in server.ini).%s", g_settings.server_mysql ? "" : " Scripts reach the database through the MySQL protocol.");
        y += text_wrap(win, msg, x, y + 2, w, !g_settings.server_mysql && running ? TH_WARNING : TH_TEXT_FAINT, TH_FONT_SMALL) + 2;
        int ew = link_w(p, "Edit php.ini");
        link_btn(p, HIT_EDIT_PHPINI, (Rect){x - 8, y, ew, 24}, "Edit php.ini", true);
        y += 26;
    }
    y += 12;

    // siti per cartella (www/blog -> blog.localhost) e cruscotto
    if (p->web_dir[0] || p->n_sites) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Every folder of the web folder is a site: http://NAME.localhost:%d (Laravel: its public/). "
                 "Dev servers: site.NAME=http://127.0.0.1:5173 in server.ini.", g_settings.server_port);
        y += text_wrap(win, msg, x, y + 2, w, TH_TEXT_FAINT, TH_FONT_SMALL) + 2;
        if (running) {
            int dw = link_w(p, "All sites");
            link_btn(p, HIT_OPEN_DASH, (Rect){x - 8, y, dw, 24}, "All sites", true);
            y += 26;
        }
        y += 12;
    }

    // HTTPS con la CA locale (come Laragon / mkcert)
    y = section_label(win, "HTTPS", x, y);
    snprintf(lb, sizeof(lb), "Port %d + HTTP/2", g_settings.server_https_port);
    segmented(p, HIT_HTTPS_OFF, (Rect){x, y, w, 28}, "Off", lb, g_settings.server_https ? 1 : 0, true);
    y += 28 + 4;
    if (g_settings.server_https) {
        int tw = link_w(p, "Trust certificate");
        if (running && p->https_url[0]) {
            int ow = link_w(p, "Open");
            link_btn(p, HIT_OPEN_HTTPS, (Rect){x - 8, y, ow, 24}, "Open", true);
            ui_text_clipped(win, p->https_url, x + ow - 4, y + 5, w - ow - tw - 4, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
        }
        link_btn(p, HIT_TRUST_CA, (Rect){x + w - tw, y, tw, 24}, "Trust certificate", true);
        y += 26;
        snprintf(lb, sizeof(lb), "Every site also answers on https://NAME.localhost:%d with a certificate from a local authority "
                 "(DATA/certs/ca.crt). Trust it once and browsers open the sites without warnings.", g_settings.server_https_port);
        y += text_wrap(win, p->trust_msg[0] ? p->trust_msg : lb, x, y + 2, w, TH_TEXT_FAINT, TH_FONT_SMALL) + 2;
    }
    y += 12;

    // cattura delle email (come Mailpit)
    y = section_label(win, "MAIL CATCHER", x, y);
    snprintf(lb, sizeof(lb), "SMTP port %d", g_settings.server_mail_port);
    segmented(p, HIT_MAIL_OFF, (Rect){x, y, w, 28}, "Off", lb, g_settings.server_mail ? 1 : 0, true);
    y += 28 + 4;
    if (g_settings.server_mail) {
        if (running && p->mail_url[0]) {
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "Open inbox (%d)", p->mail_count);
            int mw = link_w(p, lbl);
            link_btn(p, HIT_OPEN_MAIL, (Rect){x - 8, y, mw, 24}, lbl, true);
            y += 26;
        }
        snprintf(lb, sizeof(lb), "Apps send mail to 127.0.0.1:%d (any user and password, no TLS); PHP mail() goes there by itself. "
                 "Nothing leaves this computer.", g_settings.server_mail_port);
        y += text_wrap(win, lb, x, y + 2, w, TH_TEXT_FAINT, TH_FONT_SMALL) + 2;
    }
    y += 12;

    // prova rapida
    y = section_label(win, "TRY IT", x, y);
    char ex[400];
    example_text(p, ex, sizeof(ex), true);
    int cw = link_w(p, "Copy curl");
    y += text_wrap(win, ex, x, y + 2, w, TH_TEXT_MUTED, TH_FONT_SMALL) + 2;
    link_btn(p, HIT_COPY_EXAMPLE, (Rect){x - 8, y, cw, 24}, "Copy curl", true);
    y += 30;
    platform_draw_text_ex(win, running ? "Option changes restart the server" : "Saved in settings.ini", -1,
                          (Point){x, y}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    y += 28;
    p->side_content = y + p->side_scroll - y0 + 4;
    // barra di scorrimento sottile quando le opzioni non entrano
    Rect sv = p->side_view;
    if (p->side_content > sv.h && sv.h > 40) {
        int th = SV_MAX(30, sv.h * sv.h / p->side_content);
        int ty = sv.y + (sv.h - th) * p->side_scroll / SV_MAX(1, p->side_content - sv.h);
        platform_fill_rect_rounded(win, (Rect){SW - 7, ty + 2, 4, th - 4}, TH_BORDER_STRONG, 2);
    }
    // campi fuori dalla parte visibile: non disegnati altrove e non cliccabili
    if (p->port_edit && !rect_inside(p->port_edit->base.bounds, sv)) p->port_edit->base.visible = false;
    if (p->web_edit && !rect_inside(p->web_edit->base.bounds, sv)) p->web_edit->base.visible = false;
    platform_reset_clip(win);
}

// schede Requests / Monitor nell'intestazione; ritorna la x dopo le schede
static int draw_tabs(ServerPanel* p, int x0, Rect st) {
    PlatformWindow* win = p->win;
    static const char* const T[2] = { "REQUESTS", "MONITOR" };
    int x = x0 + 10;
    for (int i = 0; i < 2; i++) {
        int w = platform_measure_text_ex(win, T[i], -1, TH_FONT_SMALL, FONT_UI_BOLD) + 20;
        Rect r = { x, st.y + 7, w, st.h - 14 };
        p->hit[i == 0 ? HIT_TAB_REQ : HIT_TAB_MON] = r;
        bool on = p->tab == i, hov = in_rect(r, p->mx, p->my);
        if (on || hov) platform_fill_rect_rounded(win, r, on ? TH_ACCENT_SOFT : TH_HOVER, 6);
        platform_draw_text_ex(win, T[i], -1, (Point){r.x + 10, ui_text_y(win, r, TH_FONT_SMALL, FONT_UI_BOLD)},
                              on ? TH_ACCENT_TEXT : TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
        x += w + 4;
    }
    return x + 10;
}

// ── pagina Monitor ──────────────────────────────────────────────

static void fmt_count(double v, char* out, size_t n) {
    if (v >= 1e6) snprintf(out, n, "%.1fM", v / 1e6);
    else if (v >= 1e4) snprintf(out, n, "%.1fk", v / 1e3);
    else if (v == (double)(long long)v) snprintf(out, n, "%lld", (long long)v);
    else snprintf(out, n, "%.1f", v);
}

static void fmt_bytes(double b, char* out, size_t n) {
    if (b < 1024) snprintf(out, n, "%.0f B", b);
    else if (b < 1024.0 * 1024) snprintf(out, n, "%.1f KB", b / 1024);
    else if (b < 1024.0 * 1024 * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
    else snprintf(out, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

// riquadro con un numero: etichetta, valore grande, dettaglio
static void stat_tile(PlatformWindow* win, Rect r, const char* label, const char* value, const char* detail, Color vc) {
    ui_round_box(win, r, TH_SURFACE, TH_BORDER, 8);
    platform_draw_text_ex(win, label, -1, (Point){r.x + 14, r.y + 12}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
    platform_draw_text_ex(win, value, -1, (Point){r.x + 14, r.y + 32}, vc, 22, FONT_UI_BOLD);
    if (detail) ui_text_clipped(win, detail, r.x + 14, r.y + r.h - 22, r.w - 28, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
}

// scala "bella" per l'asse: 1, 2, 5 x 10^k sopra il massimo
static double nice_max(double v) {
    if (v <= 0) return 1;
    double p = 1;
    while (p * 10 <= v) p *= 10;
    while (p > v) p /= 10;
    double m = v / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}

// griglia orizzontale discreta con 3 valori sull'asse
static void chart_grid(PlatformWindow* win, Rect plot, double max, const char* unit) {
    for (int k = 0; k <= 2; k++) {
        int y = plot.y + plot.h - plot.h * k / 2;
        platform_fill_rect(win, (Rect){plot.x, y, plot.w, 1}, k ? ui_alpha(TH_BORDER, 140) : TH_BORDER_STRONG);
        char t[32];
        double v = max * k / 2;
        if (v == (double)(long long)v) snprintf(t, sizeof(t), "%lld%s", (long long)v, k == 2 ? unit : "");
        else snprintf(t, sizeof(t), "%.1f%s", v, k == 2 ? unit : "");
        int tw = platform_measure_text_ex(win, t, -1, TH_FONT_SMALL, FONT_MONO);
        platform_draw_text_ex(win, t, -1, (Point){plot.x - tw - 8, y - 7}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_MONO);
    }
    platform_draw_text_ex(win, "2 min ago", -1, (Point){plot.x, plot.y + plot.h + 6}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
    int nw = platform_measure_text_ex(win, "now", -1, TH_FONT_SMALL, FONT_UI);
    platform_draw_text_ex(win, "now", -1, (Point){plot.x + plot.w - nw, plot.y + plot.h + 6}, TH_TEXT_FAINT, TH_FONT_SMALL, FONT_UI);
}

static void legend_item(PlatformWindow* win, int* x, int y, Color c, const char* t, bool dot) {
    if (dot) ui_circle(win, *x + 5, y + 7, 4, c);
    else platform_fill_rect_rounded(win, (Rect){*x, y + 2, 10, 10}, c, 3);
    *x += 16;
    *x += platform_draw_text_ex(win, t, -1, (Point){*x, y}, TH_TEXT_MUTED, TH_FONT_SMALL, FONT_UI) + 14;
}

// indice del secondo sotto il mouse nel grafico (-1 = fuori)
static int chart_hover(ServerPanel* p, Rect plot) {
    if (!in_rect((Rect){plot.x, plot.y - 4, plot.w, plot.h + 8}, p->mx, p->my)) return -1;
    int i = (p->mx - plot.x) * MON_SECS / SV_MAX(1, plot.w);
    return SV_CLAMP(i, 0, MON_SECS - 1);
}

static void tooltip(PlatformWindow* win, int x, int y, const char* l1, const char* l2, Rect bounds) {
    int w = SV_MAX(platform_measure_text_ex(win, l1, -1, TH_FONT_SMALL + 1, FONT_UI_BOLD), platform_measure_text_ex(win, l2, -1, TH_FONT_SMALL + 1, FONT_UI)) + 20;
    Rect r = { x + 12, y - 48, w, 44 };
    if (r.x + r.w > bounds.x + bounds.w) r.x = x - 12 - w;
    if (r.y < bounds.y) r.y = y + 12;
    platform_fill_rect_rounded(win, (Rect){r.x - 1, r.y + 2, r.w + 2, r.h + 2}, ui_alpha(TH_BG, 150), 7);
    ui_round_box(win, r, TH_ELEVATED, TH_BORDER_STRONG, 6);
    platform_draw_text_ex(win, l1, -1, (Point){r.x + 10, r.y + 6}, TH_TEXT, TH_FONT_SMALL + 1, FONT_UI_BOLD);
    platform_draw_text_ex(win, l2, -1, (Point){r.x + 10, r.y + 24}, TH_TEXT_MUTED, TH_FONT_SMALL + 1, FONT_UI);
}

static void draw_monitor(ServerPanel* p, int x0, int cw, int top, int bottom) {
    PlatformWindow* win = p->win;
    bool running = p->srv != NULL;
    if (!running) {
        int ccx = x0 + cw / 2, ccy = top + (bottom - top) / 2 - 30;
        const char* t = "Server stopped";
        int tw = platform_measure_text_ex(win, t, -1, TH_FONT_UI + 2, FONT_UI_BOLD);
        platform_draw_text_ex(win, t, -1, (Point){ccx - tw / 2, ccy}, TH_TEXT_MUTED, TH_FONT_UI + 2, FONT_UI_BOLD);
        const char* d = "Start it (F5) to watch requests, response times and the engine live";
        int dw = platform_measure_text_ex(win, d, -1, TH_FONT_UI, FONT_UI);
        platform_draw_text_ex(win, d, -1, (Point){ccx - dw / 2, ccy + 28}, TH_TEXT_DIM, TH_FONT_UI, FONT_UI);
        return;
    }
    // aggregati: ultimi 10 s (ritmo) e 60 s (tempi, errori)
    double r10 = 0, r60 = 0, e60 = 0, ms60 = 0, rows60 = 0, peak = 0;
    for (int i = 0; i < MON_SECS; i++) {
        const SvBucket* b = &p->series[i];
        if (i >= MON_SECS - 10) r10 += b->requests;
        if (i >= MON_SECS - 60) { r60 += b->requests; e60 += b->errors; ms60 += b->total_ms; rows60 += (double)b->rows; }
        if (b->requests > peak) peak = b->requests;
    }
    // p95 dei tempi dalle ultime richieste del registro
    double lat[SV_LOG_MAX];
    int nl = 0;
    for (int i = 0; i < p->nlog && nl < SV_LOG_MAX; i++) lat[nl++] = p->log[i].ms;
    for (int a = 1; a < nl; a++) for (int b = a; b > 0 && lat[b] < lat[b - 1]; b--) { double t = lat[b]; lat[b] = lat[b - 1]; lat[b - 1] = t; }
    double p95 = nl ? lat[(int)((nl - 1) * 0.95)] : 0;

    int pad = 16, x = x0 + pad, w = cw - pad * 2, y = top + pad;
    // ── riquadri ──
    char v[6][32], d[6][64];
    snprintf(v[0], 32, "%.1f", r10 / 10.0);                       snprintf(d[0], 64, "peak %.0f/s in 2 min", peak);
    snprintf(v[1], 32, "%.1f ms", r60 ? ms60 / r60 : 0.0);         snprintf(d[1], 64, "p95 %.1f ms (last %d)", p95, nl);
    snprintf(v[2], 32, "%.1f%%", r60 ? 100.0 * e60 / r60 : 0.0);  snprintf(d[2], 64, "%.0f in the last minute", e60);
    fmt_count(rows60, v[3], 32);                                   snprintf(d[3], 64, "rows sent in the last minute");
    snprintf(v[4], 32, "%d", p->stats.connections);                snprintf(d[4], 64, "%s", p->stats.busy ? "engine busy now" : "engine idle");
    double hit = p->have_es && p->es.pool_requests ? 100.0 * (double)p->es.pool_hits / (double)p->es.pool_requests : 100.0;
    snprintf(v[5], 32, "%.1f%%", hit);                             snprintf(d[5], 64, "page cache hits");
    static const char* const L[6] = { "REQUESTS / S", "AVG RESPONSE", "ERRORS", "ROWS / MIN", "CONNECTIONS", "CACHE" };
    int cols = cw >= 980 ? 6 : 3, tw = (w - (cols - 1) * 10) / cols, th = 92;
    for (int i = 0; i < 6; i++) {
        Rect r = { x + (i % cols) * (tw + 10), y + (i / cols) * (th + 10), tw, th };
        Color vc = i == 2 && e60 > 0 ? TH_ERROR_TEXT : TH_TEXT;
        stat_tile(win, r, L[i], v[i], d[i], vc);
    }
    y += (6 / cols) * (th + 10) + 8;

    // ── grafici: un asse ciascuno ──
    int remain = bottom - y - pad;
    int ch = SV_MAX(110, SV_MIN(200, (remain - 230) / 2));
    // 1. richieste al secondo (errori in rosso nella stessa barra)
    platform_draw_text_ex(win, "Requests per second", -1, (Point){x, y}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
    int lx = x + w - 160;
    legend_item(win, &lx, y + 2, TH_ACCENT, "ok", false);
    legend_item(win, &lx, y + 2, TH_ERROR, "errors", false);
    y += 26;
    Rect plot = { x + 44, y, w - 44, ch };
    p->chart_req = plot;
    double maxr = nice_max(peak);
    chart_grid(win, plot, maxr, "");
    float bw = (float)plot.w / MON_SECS;
    for (int i = 0; i < MON_SECS; i++) {
        const SvBucket* b = &p->series[i];
        if (!b->requests) continue;
        int bx = plot.x + (int)(i * bw), bwi = SV_MAX(1, (int)bw - 1);
        int hok = (int)((double)(b->requests - b->errors) / maxr * plot.h), her = (int)((double)b->errors / maxr * plot.h);
        if (b->requests && hok + her < 2) hok = 2;
        int base = plot.y + plot.h;
        if (hok) platform_fill_rect(win, (Rect){bx, base - hok, bwi, hok}, TH_ACCENT);
        if (her) platform_fill_rect(win, (Rect){bx, base - hok - her - (hok ? 1 : 0), bwi, her}, TH_ERROR);
    }
    y += ch + 30;
    // 2. tempo di risposta: media (linea) e massimo (punti) per secondo
    platform_draw_text_ex(win, "Response time", -1, (Point){x, y}, TH_TEXT, TH_FONT_UI, FONT_UI_BOLD);
    lx = x + w - 170;
    legend_item(win, &lx, y + 2, TH_ACCENT, "average", false);
    legend_item(win, &lx, y + 2, TH_TEXT_DIM, "slowest", true);
    y += 26;
    Rect pl2 = { x + 44, y, w - 44, ch };
    p->chart_lat = pl2;
    double maxl = 0;
    for (int i = 0; i < MON_SECS; i++) if (p->series[i].max_ms > maxl) maxl = p->series[i].max_ms;
    maxl = nice_max(maxl);
    chart_grid(win, pl2, maxl, " ms");
    float px = -1, py = 0;
    for (int i = 0; i < MON_SECS; i++) {
        const SvBucket* b = &p->series[i];
        float cx = (float)pl2.x + ((float)i + 0.5f) * bw;
        if (!b->requests) { px = -1; continue; }
        float cy = (float)(pl2.y + pl2.h) - (float)(b->total_ms / b->requests / maxl * pl2.h);
        if (px >= 0) ui_line(win, px, py, cx, cy, 2, TH_ACCENT);
        else ui_circle(win, (int)cx, (int)cy, 2, TH_ACCENT);
        int my = pl2.y + pl2.h - (int)(b->max_ms / maxl * pl2.h);
        if (b->max_ms > b->total_ms / b->requests * 1.05) ui_circle(win, (int)cx, my, 3, TH_TEXT_DIM);
        px = cx; py = cy;
    }
    y += ch + 30;

    // ── in basso: richieste più lente e motore ──
    int colw = (w - 16) / 2, by = y, bh = bottom - by - pad;
    if (bh > 60) {
        // richieste più lente (dal registro)
        Rect a = { x, by, colw, bh };
        ui_round_box(win, a, TH_SURFACE, TH_BORDER, 8);
        platform_draw_text_ex(win, "SLOWEST RECENT REQUESTS", -1, (Point){a.x + 14, a.y + 12}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
        int idx[SV_LOG_MAX], n = 0;
        for (int i = 0; i < p->nlog; i++) idx[n++] = i;
        for (int i = 1; i < n; i++) for (int k = i; k > 0 && p->log[idx[k]].ms > p->log[idx[k - 1]].ms; k--) { int t = idx[k]; idx[k] = idx[k - 1]; idx[k - 1] = t; }
        int rowy = a.y + 34;
        for (int i = 0; i < n && rowy + 20 <= a.y + a.h - 6; i++, rowy += 22) {
            const SvLogEntry* e = &p->log[idx[i]];
            char ms[24], who[80];
            snprintf(ms, sizeof(ms), "%.1f ms", e->ms);
            snprintf(who, sizeof(who), "%s", e->user[0] ? e->user : e->client);
            platform_draw_text_ex(win, ms, -1, (Point){a.x + 14, rowy}, e->status >= 400 ? TH_ERROR_TEXT : TH_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
            ui_text_clipped(win, who, a.x + 92, rowy, 90, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
            ui_text_clipped(win, e->detail[0] ? e->detail : e->path, a.x + 190, rowy, a.w - 204, TH_CELL_TEXT, TH_FONT_SMALL + 1, FONT_MONO);
        }
        if (!n) platform_draw_text_ex(win, "No requests yet", -1, (Point){a.x + 14, a.y + 36}, TH_TEXT_FAINT, TH_FONT_UI, FONT_UI);
        // motore
        Rect e = { x + colw + 16, by, w - colw - 16, bh };
        ui_round_box(win, e, TH_SURFACE, TH_BORDER, 8);
        platform_draw_text_ex(win, "ENGINE", -1, (Point){e.x + 14, e.y + 12}, TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI_BOLD);
        if (p->have_es) {
            char rows[8][2][64];
            int nr = 0;
            char t1[32], t2[32];
            fmt_bytes((double)p->es.db_pages * p->es.page_size, t1, sizeof(t1));
            snprintf(rows[nr][0], 64, "Data file"); snprintf(rows[nr++][1], 64, "%s (%lld pages)", t1, p->es.db_pages);
            snprintf(rows[nr][0], 64, "Page cache"); snprintf(rows[nr++][1], 64, "%d of %d pages used", p->es.pool_used, p->es.pool_frames);
            snprintf(rows[nr][0], 64, "Disk reads"); snprintf(rows[nr++][1], 64, "%llu pages (%llu evicted)", p->es.pool_misses, p->es.pool_evictions);
            fmt_bytes((double)p->es.wal_bytes, t2, sizeof(t2));
            snprintf(rows[nr][0], 64, "Write-ahead log"); snprintf(rows[nr++][1], 64, "%s, %llu checkpoints", t2, p->es.checkpoints);
            snprintf(rows[nr][0], 64, "Databases"); snprintf(rows[nr++][1], 64, "%d (%d tables)", p->es.databases, p->es.tables);
            snprintf(rows[nr][0], 64, "Transaction"); snprintf(rows[nr++][1], 64, "%s", p->es.in_transaction ? "open" : "none");
            int ry = e.y + 34;
            for (int i = 0; i < nr && ry + 20 <= e.y + e.h - 6; i++, ry += 22) {
                platform_draw_text_ex(win, rows[i][0], -1, (Point){e.x + 14, ry}, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
                ui_text_clipped(win, rows[i][1], e.x + 140, ry, e.w - 154, TH_TEXT, TH_FONT_SMALL + 1, FONT_UI);
            }
            // barra della cache
            if (ry + 14 <= e.y + e.h - 6) {
                Rect bar = { e.x + 14, ry + 4, e.w - 28, 6 };
                platform_fill_rect_rounded(win, bar, TH_ELEVATED, 3);
                int fw = (int)(bar.w * hit / 100.0);
                if (fw > 0) platform_fill_rect_rounded(win, (Rect){bar.x, bar.y, fw, bar.h}, TH_SUCCESS, 3);
            }
        }
    }

    // tooltip sul secondo sotto il mouse
    int hi = chart_hover(p, p->chart_req), hl = chart_hover(p, p->chart_lat);
    if (hi >= 0 || hl >= 0) {
        int i = hi >= 0 ? hi : hl;
        Rect pr = hi >= 0 ? p->chart_req : p->chart_lat;
        const SvBucket* b = &p->series[i];
        int cx = pr.x + (int)((i + 0.5f) * bw);
        platform_fill_rect(win, (Rect){cx, pr.y, 1, pr.h}, ui_alpha(TH_TEXT_DIM, 120));
        char l1[64], l2[96];
        snprintf(l1, sizeof(l1), "%d s ago", MON_SECS - 1 - i);
        if (b->requests) snprintf(l2, sizeof(l2), "%d req Â· %d err Â· avg %.1f ms Â· max %.1f ms",
                                  b->requests, b->errors, b->total_ms / b->requests, b->max_ms);
        else snprintf(l2, sizeof(l2), "no requests");
        tooltip(win, p->mx, p->my, l1, l2, (Rect){x0, top, cw, bottom - top});
    }
}

static const char* const LOG_FILTERS[6] = { "All", "Web", "PHP", "API", "MySQL", "Mail" };

static bool log_match(const ServerPanel* p, const SvLogEntry* e) {
    switch (p->log_filter) {
    case 1: return !strcmp(e->svc, "web") || !strcmp(e->svc, "proxy") || !e->svc[0];
    case 2: return !strcmp(e->svc, "php");
    case 3: return !strcmp(e->svc, "api");
    case 4: return !strcmp(e->svc, "mysql");
    case 5: return !strcmp(e->svc, "mail");
    default: return true;
    }
}

static void draw_requests(ServerPanel* p, int SW, int W, int H) {
    PlatformWindow* win = p->win;
    bool running = p->srv != NULL;
    int x0 = SW, cw = W - SW, top = SP_TOOLBAR_H, bottom = H - SP_FOOTER_H;
    platform_fill_rect(win, (Rect){x0, top, cw, bottom - top}, TH_EDITOR_BG);

    // intestazione (come RESULTS)
    Rect st = { x0, top, cw, SP_HEADER_H };
    platform_fill_rect(win, st, TH_SURFACE);
    platform_fill_rect(win, (Rect){x0, st.y + st.h - 1, cw, 1}, TH_BORDER);
    int hx = draw_tabs(p, x0, st);
    Color dot = running ? (p->stats.errors ? TH_WARNING : TH_SUCCESS) : TH_TEXT_FAINT;
    ui_circle(win, hx + 4, st.y + st.h / 2, 4, dot);
    char msg[200], up[32];
    if (running) {
        format_uptime(p->stats.uptime_s, up, sizeof(up));
        snprintf(msg, sizeof(msg), "%lld request%s \xc2\xb7 %lld error%s \xc2\xb7 %lld rows sent \xc2\xb7 %d open \xc2\xb7 up %s",
                 p->stats.requests, p->stats.requests == 1 ? "" : "s", p->stats.errors, p->stats.errors == 1 ? "" : "s",
                 p->stats.rows_sent, p->stats.connections, up);
    } else snprintf(msg, sizeof(msg), "Server stopped");
    int clw = link_w(p, "Clear");
    int shown = 0;
    for (int i = 0; i < p->nlog; i++) if (p->log[i].seq > p->log_cleared && log_match(p, &p->log[i])) shown++;
    int right = x0 + cw - 12;
    for (int f = 0; f < 6; f++) p->hit[HIT_LF0 + f] = (Rect){0, 0, 0, 0};
    if (p->tab == 0) {
        link_btn(p, HIT_CLEAR_LOG, (Rect){right - clw, st.y + 7, clw, st.h - 14}, "Clear", shown > 0);
        right -= clw + 10;
        // filtro per servizio, da destra a sinistra (se c'è spazio)
        for (int f = 5; f >= 0; f--) {
            int fw = platform_measure_text_ex(win, LOG_FILTERS[f], -1, TH_FONT_SMALL + 1, FONT_UI) + 16;
            if (right - fw < hx + 260) break;
            Rect r = { right - fw, st.y + 8, fw, st.h - 16 };
            p->hit[HIT_LF0 + f] = r;
            bool on = p->log_filter == f, hov = in_rect(r, p->mx, p->my);
            if (on || hov) platform_fill_rect_rounded(win, r, on ? TH_ACCENT_SOFT : TH_HOVER, 5);
            platform_draw_text_ex(win, LOG_FILTERS[f], -1, (Point){r.x + 8, ui_text_y(win, r, TH_FONT_SMALL + 1, FONT_UI)},
                                  on ? TH_ACCENT_TEXT : TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_UI);
            right = r.x - 2;
        }
        right -= 8;
    }
    ui_text_clipped(win, msg, hx + 16, ui_text_y(win, st, TH_FONT_UI, FONT_UI), right - (hx + 16),
                    TH_TEXT, TH_FONT_UI, FONT_UI);

    if (p->tab == 1) { draw_monitor(p, x0, cw, top + SP_HEADER_H, bottom); return; }

    // griglia del registro
    static const char* const COLS[8] = { "time", "service", "client", "method", "path / query", "status", "ms", "sql / message" };
    int widths[7] = { 78, 64, 118, 66, 220, 60, 64 };
    if (cw < 900) { widths[2] = 0; widths[4] = 170; }
    if (cw < 640) { widths[3] = 0; widths[6] = 0; widths[1] = 0; }
    int cx[9];
    cx[0] = x0 + 16;
    for (int i = 0; i < 7; i++) cx[i + 1] = cx[i] + widths[i];
    cx[8] = x0 + cw - 12;
    Rect gh = { x0, top + SP_HEADER_H, cw, 30 };
    platform_fill_rect(win, gh, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){x0, gh.y + gh.h - 1, cw, 1}, TH_BORDER);
    for (int i = 0; i < 8; i++) {
        if (cx[i + 1] - cx[i] <= 0) continue;
        platform_draw_text_ex(win, COLS[i], -1, (Point){cx[i], ui_text_y(win, gh, TH_FONT_SMALL + 1, FONT_UI_BOLD)},
                              TH_TEXT_MUTED, TH_FONT_SMALL + 1, FONT_UI_BOLD);
    }
    int ry = gh.y + gh.h;
    p->log_rect = (Rect){x0, ry, cw, bottom - ry};
    int visible = SV_MAX(1, p->log_rect.h / SP_ROW_H);
    p->log_scroll = SV_CLAMP(p->log_scroll, 0, SV_MAX(0, shown - visible));

    if (!shown) {                                       // stato vuoto (come "No results yet")
        int ccx = x0 + cw / 2, ccy = ry + (bottom - ry) / 2 - 30;
        ui_icon_table(win, ccx, ccy, 34, TH_TEXT_FAINT);
        const char* title = running ? "No requests yet" : "Server stopped";
        const char* detail = running ? "Call the address from an app, or copy the curl command on the left"
                                     : "Press F5 or Start server to accept requests from web and desktop apps";
        int tw = platform_measure_text_ex(win, title, -1, TH_FONT_UI + 2, FONT_UI_BOLD);
        platform_draw_text_ex(win, title, -1, (Point){ccx - tw / 2, ccy + 36}, TH_TEXT_MUTED, TH_FONT_UI + 2, FONT_UI_BOLD);
        int dw = platform_measure_text_ex(win, detail, -1, TH_FONT_UI, FONT_UI);
        ui_text_clipped(win, detail, SV_MAX(x0 + 16, ccx - dw / 2), ccy + 62, cw - 32, TH_TEXT_DIM, TH_FONT_UI, FONT_UI);
        return;
    }
    platform_set_clip(win, p->log_rect);
    int row = 0;
    for (int i = 0; i < p->nlog; i++) {
        const SvLogEntry* e = &p->log[i];
        if (e->seq <= p->log_cleared || !log_match(p, e)) continue;
        if (row++ < p->log_scroll) continue;
        int yy = ry + (row - 1 - p->log_scroll) * SP_ROW_H;
        if (yy > bottom) break;
        Rect rr = { x0, yy, cw, SP_ROW_H };
        if ((row & 1) == 0) platform_fill_rect(win, rr, TH_ROW_ODD);
        platform_fill_rect(win, (Rect){x0, yy + SP_ROW_H - 1, cw, 1}, ui_alpha(TH_BORDER, 120));
        int ty = ui_text_y(win, rr, TH_FONT_SMALL + 1, FONT_MONO);
        char ms[24], status[8];
        snprintf(ms, sizeof(ms), "%.1f", e->ms);
        snprintf(status, sizeof(status), "%d", e->status);
        Color sc = e->status >= 500 ? TH_ERROR_TEXT : e->status >= 400 ? TH_WARNING : TH_SUCCESS;
        char who[120];
        if (e->user[0]) snprintf(who, sizeof(who), "%.60s@%.48s", e->user, e->client);
        else snprintf(who, sizeof(who), "%.48s", e->client);
        const char* vals[8] = { e->when, e->svc[0] ? e->svc : "web", who, e->method, e->path, status, ms, e->detail };
        for (int k = 0; k < 8; k++) {
            int cwk = cx[k + 1] - cx[k] - 10;
            if (cwk <= 0) continue;
            Color c = k == 5 ? sc : k == 7 ? (e->status >= 400 ? TH_ERROR_TEXT : TH_CELL_TEXT) : k == 0 || k == 1 ? TH_TEXT_DIM : TH_CELL_TEXT;
            ui_text_clipped(win, vals[k], cx[k], ty, cwk, c, TH_FONT_SMALL + 1, FONT_MONO);
        }
    }
    platform_reset_clip(win);
}

static void draw_footer(ServerPanel* p, int W, int H) {
    PlatformWindow* win = p->win;
    Rect f = { 0, H - SP_FOOTER_H, W, SP_FOOTER_H };
    platform_fill_rect(win, f, TH_SURFACE_2);
    platform_fill_rect(win, (Rect){0, f.y, W, 1}, TH_BORDER);
    int fs = TH_FONT_SMALL + 1, ty = ui_text_y(win, f, fs, FONT_UI), cy = f.y + SP_FOOTER_H / 2;
    bool running = p->srv != NULL;
    const char* help = "F5 Start / Stop";
    int hw = platform_measure_text_ex(win, help, -1, fs, FONT_UI_BOLD);
    platform_draw_text_ex(win, help, -1, (Point){W - 16 - hw, ty}, TH_ACCENT_TEXT, fs, FONT_UI_BOLD);
    char buf[200];
    snprintf(buf, sizeof(buf), "%s engine %s", SV_NAME, SV_VERSION_STR);
    int x = 16;
    ui_circle(win, x + 3, cy, 3, running ? TH_SUCCESS : TH_TEXT_FAINT);
    x += 12;
    x += platform_draw_text_ex(win, buf, -1, (Point){x, ty}, TH_TEXT_DIM, fs, FONT_UI) + 18;
    snprintf(buf, sizeof(buf), "API server %s", running ? "running" : "stopped");
    x += platform_draw_text_ex(win, buf, -1, (Point){x, ty}, running ? TH_TEXT_MUTED : TH_TEXT_DIM, fs, FONT_UI) + 18;
    snprintf(buf, sizeof(buf), "%s%s%s", g_settings.server_lan ? "local network" : "this computer only",
             token_required() ? " \xc2\xb7 token required" : "", g_settings.server_cors ? " \xc2\xb7 CORS any origin" : "");
    ui_text_clipped(win, buf, x, ty, W - 16 - hw - 18 - x, TH_TEXT_DIM, fs, FONT_UI);
}

void server_panel_draw(ServerPanel* p, int W, int H) {
    if (!p) return;
    memset(p->hit, 0, sizeof(p->hit));
    platform_clear(p->win, TH_BG);
    int SW = W >= 900 ? SP_SIDEBAR_W : SV_MAX(220, W * 32 / 100);
    draw_requests(p, SW, W, H);
    draw_sidebar(p, SW, H);
    draw_toolbar(p, W);
    draw_footer(p, W, H);
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

static void web_commit(ServerPanel* p) {
    LineEdit* we = p->web_edit;
    if (!we) return;
    line_edit_focus(we, false);
    char v[1024];
    snprintf(v, sizeof(v), "%s", we->text);
    size_t n = strlen(v);
    while (n > 1 && (v[n - 1] == '/' || v[n - 1] == '\\' || v[n - 1] == ' ')) v[--n] = '\0';
    if (!strcmp(v, p->web_dir)) return;
    snprintf(p->web_dir, sizeof(p->web_dir), "%s", v);
    line_edit_set_text(we, v);
    web_save(p);
    if (p->srv) { server_panel_stop(p); server_panel_start(p); }
    else p->error[0] = '\0';
}

static void toggle(ServerPanel* p) {
    if (p->srv) server_panel_stop(p);
    else server_panel_start(p);
}

ViewAction server_panel_event(ServerPanel* p, sEvent* evt) {
    if (!p) return VIEW_NONE;
    LineEdit* pe = p->port_edit;
    Widget* btns[3] = { (Widget*)p->btn_toggle, (Widget*)p->btn_copy, (Widget*)p->btn_editor };

    // pulsanti della toolbar (stesso widget dell'editor: hover, pressione, click al rilascio)
    if (evt->type == EVT_MOUSE_MOVE || evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_UP) {
        p->pending = ACT_NONE;
        for (int i = 0; i < 3; i++) if (btns[i]) widget_handle_event(btns[i], evt);
        int act = p->pending;
        p->pending = ACT_NONE;
        if (act == ACT_EDITOR) return VIEW_OPEN_EDITOR;
        if (act == ACT_TOGGLE) { toggle(p); return VIEW_REDRAW; }
        if (act == ACT_COPY && p->srv) { copy_text(p, HIT_COPY_URL, p->url); return VIEW_REDRAW; }
    }
    if (evt->type == EVT_MOUSE_MOVE) {
        p->mx = evt->mouse_x; p->my = evt->mouse_y;
        if (pe) widget_handle_event((Widget*)pe, evt);
        if (p->web_edit) widget_handle_event((Widget*)p->web_edit, evt);
        return VIEW_REDRAW;
    }
    if (evt->type == EVT_MOUSE_WHEEL) {
        if (in_rect(p->log_rect, evt->mouse_x, evt->mouse_y)) { p->log_scroll -= evt->wheel_delta * 3; if (p->log_scroll < 0) p->log_scroll = 0; }
        else if (in_rect(p->side_view, evt->mouse_x, evt->mouse_y)) {
            p->side_scroll = SV_CLAMP(p->side_scroll - evt->wheel_delta * 48, 0, SV_MAX(0, p->side_content - p->side_view.h));
        }
        return VIEW_REDRAW;
    }
    // cartella web: Invio conferma (e riavvia il server), Esc annulla
    LineEdit* we = p->web_edit;
    if (we && line_edit_focused(we) && (evt->type == EVT_KEY_DOWN || evt->type == EVT_CHAR)) {
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ENTER) { web_commit(p); return VIEW_REDRAW; }
        if (evt->type == EVT_KEY_DOWN && evt->key == KEY_ESCAPE) { line_edit_set_text(we, p->web_dir); line_edit_focus(we, false); return VIEW_REDRAW; }
        widget_handle_event((Widget*)we, evt);
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
        if (p->web_edit) widget_handle_event((Widget*)p->web_edit, evt);
        return VIEW_REDRAW;
    }
    if (evt->type == EVT_KEY_DOWN) {
        if (evt->key == KEY_F5) { toggle(p); return VIEW_REDRAW; }
        if (evt->key == KEY_DOWN) { p->log_scroll++; return VIEW_REDRAW; }
        if (evt->key == KEY_UP && p->log_scroll > 0) { p->log_scroll--; return VIEW_REDRAW; }
        if (evt->key == KEY_PAGEDOWN) { p->log_scroll += 10; return VIEW_REDRAW; }
        if (evt->key == KEY_PAGEUP) { p->log_scroll = SV_MAX(0, p->log_scroll - 10); return VIEW_REDRAW; }
        if (evt->key == KEY_HOME) { p->log_scroll = 0; return VIEW_REDRAW; }
        return VIEW_NONE;
    }
    if (evt->type != EVT_MOUSE_DOWN || evt->mouse_button != SV_MOUSE_LEFT) return VIEW_NONE;

    int x = evt->mouse_x, y = evt->mouse_y;
    if (we) {
        if (we->base.visible && in_rect(we->base.bounds, x, y) && !side_hidden(p, x, y)) {
            if (pe && line_edit_focused(pe)) commit_port(p);
            if (!line_edit_focused(we)) line_edit_focus(we, true);
            widget_handle_event((Widget*)we, evt);
            return VIEW_REDRAW;
        }
        if (line_edit_focused(we)) web_commit(p);
    }
    if (pe) {
        if (pe->base.visible && in_rect(pe->base.bounds, x, y) && !side_hidden(p, x, y)) {
            if (!line_edit_focused(pe)) { line_edit_focus(pe, true); line_edit_select_all(pe); }
            else widget_handle_event((Widget*)pe, evt);
            return VIEW_REDRAW;
        }
        if (line_edit_focused(pe)) commit_port(p);
    }
    int hit = -1;
    for (int i = 0; i < HIT_COUNT && hit < 0; i++) if (in_rect(p->hit[i], x, y)) hit = i;
    if (hit >= 0 && hit != HIT_TAB_REQ && hit != HIT_TAB_MON && hit != HIT_CLEAR_LOG && (hit < HIT_LF0 || hit > HIT_LF5) && side_hidden(p, x, y)) hit = -1;
    char ex[400];
    switch (hit) {
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
        case HIT_LF0: case HIT_LF1: case HIT_LF2: case HIT_LF3: case HIT_LF4: case HIT_LF5:
            p->log_filter = hit - HIT_LF0;
            p->log_scroll = 0;
            break;
        case HIT_TAB_REQ: case HIT_TAB_MON:
            p->tab = hit == HIT_TAB_MON;
            p->last_mon = 0;
            break;
        case HIT_USERS_OFF: case HIT_USERS_ON:
            if (g_settings.server_users != (hit == HIT_USERS_ON)) { g_settings.server_users = hit == HIT_USERS_ON; config_changed(p); }
            break;
        case HIT_MYSQL_OFF: case HIT_MYSQL_ON:
            service_switch(p, &g_settings.server_mysql, hit == HIT_MYSQL_ON, SV_SVC_MYSQL);
            break;
        case HIT_COPY_MYSQL: copy_text(p, hit, p->mysql_addr); break;
        case HIT_PHP_OFF: case HIT_PHP_ON:
            service_switch(p, &g_settings.server_php, hit == HIT_PHP_ON, SV_SVC_PHP);
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
        case HIT_OPEN_SITE:
            if (!platform_open_url(p->site_url)) copy_text(p, hit, p->site_url);
            break;
        case HIT_OPEN_MAIL:
            if (!platform_open_url(p->mail_url)) copy_text(p, hit, p->mail_url);
            break;
        case HIT_OPEN_DASH:
            if (!platform_open_url(p->dash_url)) copy_text(p, hit, p->dash_url);
            break;
        case HIT_EDIT_PHPINI: {
            // php.ini del php-cgi in uso (creato da php.ini-development se manca); vale dal prossimo avvio
            char php[1024] = "", ini[1200];
            if (p->php_cgi[0]) snprintf(php, sizeof(php), "%s", p->php_cgi);
            else if (!fcgi_find_php_cgi(php, sizeof(php))) { snprintf(p->error, sizeof(p->error), "php-cgi not found"); break; }
            if (!fcgi_php_ini(php, true, ini, sizeof(ini)) || !platform_open_file(ini)) {
                snprintf(p->error, sizeof(p->error), "Cannot open %.200s", ini);
                break;
            }
            snprintf(p->flash, sizeof(p->flash), "Restart the server after saving");
            break;
        }
        case HIT_HTTPS_OFF: case HIT_HTTPS_ON:
            service_switch(p, &g_settings.server_https, hit == HIT_HTTPS_ON, SV_SVC_HTTPS);
            break;
        case HIT_OPEN_HTTPS:
            if (!platform_open_url(p->https_url)) copy_text(p, hit, p->https_url);
            break;
        case HIT_TRUST_CA: {
            char cd[600], out[4000];
            snprintf(cd, sizeof(cd), "%s%scerts", p->data_dir, p->data_dir[0] ? "/" : "");
            int r = tls_trust_ca(cd, out, sizeof(out));
            // in una riga: le righe dopo la prima dicono dove è stata aggiunta
            char* nl = strchr(out, '\n');
            const char* rest = nl ? nl + 1 : out;
            snprintf(p->trust_msg, sizeof(p->trust_msg), "%s%.380s", r == 0 ? "Trusted. " : "", rest);
            for (char* q = p->trust_msg; *q; q++) if (*q == '\n') *q = ' ';
            break;
        }
        case HIT_MAIL_OFF: case HIT_MAIL_ON:
            service_switch(p, &g_settings.server_mail, hit == HIT_MAIL_ON, SV_SVC_MAIL);
            break;
        case HIT_CLEAR_WEB:
            if (we) line_edit_set_text(we, "");
            web_commit(p);
            break;
        default: return VIEW_NONE;
    }
    return VIEW_REDRAW;
}
