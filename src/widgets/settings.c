#include "settings.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

Settings g_settings;

#define MAX_THEMES 16
static const char* g_theme_names[MAX_THEMES];

static const char* const ACCENTS[]   = { "Theme default", "Sky", "Blue", "Indigo", "Violet", "Pink", "Rose",
                                         "Orange", "Amber", "Lime", "Green", "Teal" };
static const unsigned char ACCENT_RGB[][3] = {
    {0, 0, 0}, {56, 189, 248}, {59, 130, 246}, {99, 102, 241}, {139, 92, 246}, {236, 72, 153}, {244, 63, 94},
    {249, 115, 22}, {245, 158, 11}, {132, 204, 22}, {34, 197, 94}, {20, 184, 166},
};
static const char* const ON_OFF[]    = { "Off", "On" };
static const char* const TOOLBAR[]   = { "Labels when they fit", "Icons only" };
static const char* const COMPLETE[]  = { "While typing", "Only with Ctrl+Space" };
static const char* const KWCASE[]    = { "UPPERCASE", "lowercase" };
static const char* const TABS[]      = { "2 spaces", "4 spaces", "8 spaces" };
static const char* const ROWS[]      = { "Compact", "Normal", "Comfortable" };
static const char* const NULLS[]     = { "NULL", "(null)", "\xe2\x88\x85", "(empty)" };
static const char* const ONERR[]     = { "Stop at the first error", "Continue with the next statement" };
static const char* const HISTORY[]   = { "50 queries", "100 queries", "250 queries", "500 queries" };
static const char* const DELIMS[]    = { "Comma  ,", "Semicolon  ;", "Tab" };
static const char* const CSVNULLS[]  = { "Empty field", "NULL" };
static const char* const STARTUP[]   = { "Ask every time", "SQL editor", "API server" };

#define N(a) ((int)(sizeof(a) / sizeof(a[0])))
#define S(f) (&g_settings.f)

static SettingDef DEFS[] = {
    { "Appearance", "theme", "Color theme", "Colors of the whole interface (Ctrl+Shift+T cycles them)",
      SET_CHOICE, S(theme), 0, 0, 0, g_theme_names, 0 },
    { "Appearance", "accent", "Accent color", "Buttons, selection, cursor and highlights; works with every theme",
      SET_CHOICE, S(accent), ACCENT_THEME, 0, 0, ACCENTS, N(ACCENTS) },
    { "Appearance", "ui_font_size", "Interface text size", "Size of menus, labels and buttons",
      SET_INT, S(ui_font_size), 13, 11, 16, NULL, 0 },
    { "Appearance", "toolbar_style", "Toolbar buttons", "Show text labels or only icons",
      SET_CHOICE, S(toolbar_style), TOOLBAR_AUTO, 0, 0, TOOLBAR, N(TOOLBAR) },
    { "Appearance", "show_sidebar", "Explorer sidebar", "Database tree on the left (Ctrl+B)",
      SET_BOOL, S(show_sidebar), 1, 0, 1, ON_OFF, 2 },
    { "Appearance", "show_footer", "Status bar", "Bottom bar with database, cursor position and file",
      SET_BOOL, S(show_footer), 1, 0, 1, ON_OFF, 2 },

    { "Editor", "font_size", "Font size", "Editor text size (also Ctrl + wheel, Ctrl+0 resets)",
      SET_INT, S(font_size), 15, 10, 28, NULL, 0 },
    { "Editor", "tab_size", "Indentation", "Spaces inserted by Tab and used by Format",
      SET_CHOICE, S(tab_size), 1, 0, 0, TABS, N(TABS) },
    { "Editor", "line_numbers", "Line numbers", "Gutter with the line numbers",
      SET_BOOL, S(line_numbers), 1, 0, 1, ON_OFF, 2 },
    { "Editor", "highlight_line", "Highlight current line", "Background on the line with the cursor",
      SET_BOOL, S(highlight_line), 1, 0, 1, ON_OFF, 2 },
    { "Editor", "auto_close", "Auto-close brackets and quotes", "Typing ( ' \" inserts the closing one too",
      SET_BOOL, S(auto_close), 1, 0, 1, ON_OFF, 2 },
    { "Editor", "autocomplete", "Autocomplete", "When the suggestion list opens",
      SET_CHOICE, S(autocomplete), COMPLETE_AUTO, 0, 0, COMPLETE, N(COMPLETE) },
    { "Editor", "keyword_case", "Keyword case", "Case of SQL keywords from autocomplete and Format",
      SET_CHOICE, S(keyword_case), CASE_UPPER, 0, 0, KWCASE, N(KWCASE) },
    { "Editor", "cursor_blink", "Blinking cursor", "Turn off for a steady cursor",
      SET_BOOL, S(cursor_blink), 1, 0, 1, ON_OFF, 2 },
    { "Editor", "restore_session", "Restore last session", "Reopen the script and cursor position at startup",
      SET_BOOL, S(restore_session), 1, 0, 1, ON_OFF, 2 },

    { "Results", "row_height", "Row height", "Density of the result grid",
      SET_CHOICE, S(row_height), ROWS_NORMAL, 0, 0, ROWS, N(ROWS) },
    { "Results", "null_display", "NULL values shown as", "How missing values appear in the grid",
      SET_CHOICE, S(null_display), NULL_TEXT, 0, 0, NULLS, N(NULLS) },
    { "Results", "row_numbers", "Row numbers", "Column with the row number on the left",
      SET_BOOL, S(row_numbers), 1, 0, 1, ON_OFF, 2 },
    { "Results", "zebra", "Alternating row colors", "Stripes that help follow long rows",
      SET_BOOL, S(zebra), 1, 0, 1, ON_OFF, 2 },

    { "Execution", "on_error", "When a statement fails", "Behaviour of scripts with several statements",
      SET_CHOICE, S(on_error), ONERR_STOP, 0, 0, ONERR, N(ONERR) },
    { "Execution", "safe_updates", "Safe updates", "Block UPDATE and DELETE without a WHERE clause",
      SET_BOOL, S(safe_updates), 1, 0, 1, ON_OFF, 2 },
    { "Execution", "confirm_drop", "Protect against DROP / TRUNCATE", "Block DROP and TRUNCATE until this is turned off",
      SET_BOOL, S(confirm_drop), 0, 0, 1, ON_OFF, 2 },
    { "Execution", "history_limit", "Query history size", "How many executed queries are kept",
      SET_CHOICE, S(history_limit), 3, 0, 0, HISTORY, N(HISTORY) },

    { "Export", "csv_delimiter", "CSV separator", "Use semicolon for Excel with Italian/European settings",
      SET_CHOICE, S(csv_delimiter), DELIM_COMMA, 0, 0, DELIMS, N(DELIMS) },
    { "Export", "csv_header", "CSV header row", "First line with the column names",
      SET_BOOL, S(csv_header), 1, 0, 1, ON_OFF, 2 },
    { "Export", "csv_null", "NULL in CSV", "How NULL values are written in CSV files",
      SET_CHOICE, S(csv_null), CSVNULL_EMPTY, 0, 0, CSVNULLS, N(CSVNULLS) },
    { "Export", "json_pretty", "Readable JSON", "Indent JSON exports (off = one compact line)",
      SET_BOOL, S(json_pretty), 1, 0, 1, ON_OFF, 2 },

    { "Server", "startup_mode", "At startup open", "The SQL editor, the API server for web and desktop apps, or ask",
      SET_CHOICE, S(startup_mode), START_ASK, 0, 0, STARTUP, N(STARTUP) },
    { "Server", "server_port", "Server port", "TCP port of the API server (http://127.0.0.1:PORT/api/v1)",
      SET_INT, S(server_port), 8484, 1024, 65535, NULL, 0 },
    { "Server", "server_lan", "Reachable from the local network", "Other computers can connect; a token is always required",
      SET_BOOL, S(server_lan), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_token", "Require a token", "Apps send \"Authorization: Bearer <token>\" (always on with network or CORS)",
      SET_BOOL, S(server_token), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_users", "Require a user login", "Apps send a user and password (HTTP Basic); each query runs with that user's privileges",
      SET_BOOL, S(server_users), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_mysql", "MySQL protocol (port 3306)", "PHP (mysqli, PDO), Python, Java and the mysql client connect as to a MySQL server",
      SET_BOOL, S(server_mysql), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_php", "Run PHP in the web folder", "The .php files of the web folder run with php-cgi (php/ next to SegVault, the PATH, C:\\xampp\\php, ...)",
      SET_BOOL, S(server_php), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_mail", "Catch outgoing email (SMTP port 1025)", "Apps send mail to 127.0.0.1:1025; nothing leaves the computer, messages appear in the inbox page",
      SET_BOOL, S(server_mail), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_cors", "Allow web apps from any origin", "CORS for pages served elsewhere (e.g. a dev server); requires the token",
      SET_BOOL, S(server_cors), 0, 0, 1, ON_OFF, 2 },
    { "Server", "server_timeout", "Request time limit (s)", "A request running longer is stopped (0 = no limit)",
      SET_INT, S(server_timeout), 30, 0, 600, NULL, 0 },
};

int settings_count(void) { return N(DEFS); }

const SettingDef* settings_get(int i) { return (i >= 0 && i < N(DEFS)) ? &DEFS[i] : NULL; }

void settings_reset(void) {
    for (int i = 0; i < N(DEFS); i++) *DEFS[i].value = DEFS[i].def;
}

void settings_init(void) {
    int n = theme_count();
    if (n > MAX_THEMES) n = MAX_THEMES;
    for (int i = 0; i < n; i++) g_theme_names[i] = theme_get(i)->name;
    for (int i = 0; i < N(DEFS); i++)
        if (DEFS[i].value == S(theme)) DEFS[i].nchoices = n;
    settings_reset();
}

static void clamp(const SettingDef* d) {
    int* v = d->value;
    int lo = d->kind == SET_INT ? d->min : 0;
    int hi = d->kind == SET_INT ? d->max : d->kind == SET_BOOL ? 1 : d->nchoices - 1;
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
}

void settings_step(int i, int dir) {
    const SettingDef* d = settings_get(i);
    if (!d) return;
    if (d->kind == SET_BOOL) { *d->value = !*d->value; return; }
    if (d->kind == SET_INT)  { *d->value += dir; clamp(d); return; }
    *d->value = (*d->value + dir + d->nchoices) % d->nchoices;   // le scelte girano
}

const char* settings_value_text(int i, char* buf, int n) {
    const SettingDef* d = settings_get(i);
    if (!d) return "";
    if (d->kind == SET_INT) { snprintf(buf, n, "%d", *d->value); return buf; }
    int v = *d->value;
    if (v < 0 || v >= d->nchoices) return "?";
    return d->choices[v];
}

// settings.ini: chiave=valore; le scelte sono salvate per nome (resistono a riordini dell'elenco)
bool settings_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        const char* val = eq + 1;
        for (int i = 0; i < N(DEFS); i++) {
            const SettingDef* d = &DEFS[i];
            if (strcmp(d->key, line) != 0) continue;
            if (d->kind == SET_CHOICE) {
                for (int c = 0; c < d->nchoices; c++)
                    if (strcasecmp(d->choices[c], val) == 0) *d->value = c;
            } else {
                *d->value = atoi(val);
            }
            clamp(d);
        }
    }
    fclose(f);
    return true;
}

bool settings_save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# SegVault settings (edited from Settings, Ctrl+,)\n");
    for (int i = 0; i < N(DEFS); i++) {
        const SettingDef* d = &DEFS[i];
        if (d->kind == SET_CHOICE) fprintf(f, "%s=%s\n", d->key, d->choices[*d->value]);
        else                       fprintf(f, "%s=%d\n", d->key, *d->value);
    }
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

int settings_tab_size(void)      { static const int T[] = {2, 4, 8}; return T[g_settings.tab_size % 3]; }
int settings_history_limit(void) { static const int H[] = {50, 100, 250, 500}; return H[g_settings.history_limit % 4]; }
int settings_row_height(void)    { static const int R[] = {22, 28, 34}; return R[g_settings.row_height % 3]; }

const char* settings_null_text(void) {
    static const char* const T[] = { "NULL", "(null)", "\xe2\x88\x85", "" };
    return T[g_settings.null_display % 4];
}

char settings_csv_delimiter(void) {
    return g_settings.csv_delimiter == DELIM_SEMICOLON ? ';' : g_settings.csv_delimiter == DELIM_TAB ? '\t' : ',';
}

bool settings_accent_color(unsigned char rgb[3]) {
    int a = g_settings.accent;
    if (a <= ACCENT_THEME || a >= N(ACCENTS)) return false;
    memcpy(rgb, ACCENT_RGB[a], 3);
    return true;
}
