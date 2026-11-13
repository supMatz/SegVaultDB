/*
SCOPO: impostazioni dell'applicazione (Settings).
       Ogni opzione è descritta una volta sola in una tabella (SettingDef): da lì
       nascono il pannello Settings, il salvataggio e il caricamento di settings.ini.
       I valori vivono in g_settings; la finestra li applica ai widget con
       app_window_apply_settings() dopo ogni modifica.
*/
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

typedef enum { SET_BOOL, SET_CHOICE, SET_INT } SettingKind;

// scelte (indici) delle opzioni a elenco
enum { ACCENT_THEME = 0 };                                         // 0 = accento del tema, poi i colori
enum { TOOLBAR_AUTO = 0, TOOLBAR_ICONS };
enum { COMPLETE_AUTO = 0, COMPLETE_MANUAL };
enum { CASE_UPPER = 0, CASE_LOWER };
enum { ROWS_COMPACT = 0, ROWS_NORMAL, ROWS_COMFORTABLE };
enum { NULL_TEXT = 0, NULL_PAREN, NULL_SYMBOL, NULL_EMPTY };
enum { ONERR_STOP = 0, ONERR_CONTINUE };
enum { DELIM_COMMA = 0, DELIM_SEMICOLON, DELIM_TAB };
enum { CSVNULL_EMPTY = 0, CSVNULL_TEXT };
enum { START_ASK = 0, START_EDITOR, START_SERVER };

typedef struct {
    // aspetto
    int theme;              // indice del tema
    int accent;             // ACCENT_THEME o colore (vedi settings_accent_color)
    int show_sidebar;
    int show_footer;
    int toolbar_style;
    int ui_font_size;       // dimensione del testo dell'interfaccia (11..16)
    // editor
    int font_size;
    int tab_size;           // indice: 2, 4, 8
    int line_numbers;
    int highlight_line;
    int auto_close;         // chiude automaticamente parentesi e apici
    int autocomplete;
    int keyword_case;
    int cursor_blink;
    int restore_session;
    // risultati
    int row_height;
    int null_display;
    int row_numbers;
    int zebra;              // righe alterne
    // esecuzione
    int on_error;
    int safe_updates;       // blocca UPDATE/DELETE senza WHERE
    int confirm_drop;       // blocca DROP / TRUNCATE (finché non si disattiva)
    int history_limit;      // indice: 50, 100, 250, 500
    // export
    int csv_delimiter;
    int csv_header;
    int csv_null;
    int json_pretty;
    // server API
    int startup_mode;       // START_ASK / START_EDITOR / START_SERVER
    int server_port;
    int server_lan;         // raggiungibile dalla rete locale (richiede il token)
    int server_token;       // richiede il token anche in locale
    int server_users;       // richiede utente e password (permessi per utente, vedi auth.h)
    int server_mysql;       // anche il protocollo di rete MySQL (porta 3306)
    int server_php;         // esegue i .php della cartella web con php-cgi (FastCGI)
    int server_https;       // anche HTTPS (porta 8443, HTTP/2) con la CA locale in DATA/certs
    int server_mail;        // cattura delle email: server SMTP sulla porta 1025 (vedi src/server/mail.h)
    int server_cors;        // le web app di qualsiasi origine possono chiamarlo (richiede il token)
    int server_timeout;     // secondi (0 = nessun limite)
    int server_mysql_port;  // porte dei servizi (default 3306, 1025, 8443)
    int server_mail_port;
    int server_https_port;
} Settings;

typedef struct {
    const char* section;    // "Appearance", "Editor", ...
    const char* key;        // chiave in settings.ini
    const char* label;
    const char* help;       // una riga di spiegazione
    SettingKind kind;
    int*        value;
    int         def;
    int         min, max;   // SET_INT
    const char* const* choices;   // SET_CHOICE
    int         nchoices;
} SettingDef;

extern Settings g_settings;

void              settings_init(void);                 // valori predefiniti (+ elenco dei temi)
int               settings_count(void);
const SettingDef* settings_get(int i);
void              settings_reset(void);                 // tutti i valori predefiniti
bool              settings_load(const char* path);      // false se il file non esiste
bool              settings_save(const char* path);
void              settings_step(int i, int dir);       // dir = +1/-1: valore successivo/precedente (bool: inverte)
const char*       settings_value_text(int i, char* buf, int n);   // testo del valore corrente

// valori derivati
int  settings_tab_size(void);
int  settings_history_limit(void);
int  settings_row_height(void);
const char* settings_null_text(void);
char settings_csv_delimiter(void);
bool settings_accent_color(unsigned char rgb[3]);      // false = accento del tema

#endif
