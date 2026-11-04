/*
SCOPO: Palette colori, metriche e primitive grafiche condivise da tutti i widget.
       Tenere qui i colori evita valori "magici" sparsi nel codice e rende
       l'interfaccia coerente. Le funzioni ui_* sono costruite SOLO sopra platform.h.
*/

#ifndef THEME_H
#define THEME_H

#include "../platform/platform.h"

// ── temi ─────────────────────────────────────────────────────────
// Tutti i colori vivono nel tema attivo (g_theme): le macro TH_* leggono da lì,
// così cambiare tema a runtime aggiorna tutta l'interfaccia al frame successivo.

typedef struct {
    const char* name;
    bool        light;          // tema chiaro
    Color bg;
    Color surface;
    Color surface_2;
    Color editor_bg;
    Color elevated;
    Color elevated_hover;
    Color elevated_press;
    Color border;
    Color border_strong;
    Color hover;
    Color row_odd;
    Color text;
    Color text_muted;
    Color text_dim;
    Color text_faint;
    Color cell_text;
    Color cell_number;
    Color accent;
    Color accent_hover;
    Color accent_press;
    Color accent_soft;
    Color accent_text;
    Color on_accent;
    Color success;
    Color success_soft;
    Color error;
    Color error_soft;
    Color error_text;
    Color warning;
    Color warning_soft;
    Color info;
    Color syn_keyword;
    Color syn_type;
    Color syn_func;
    Color syn_string;
    Color syn_number;
    Color syn_comment;
    Color syn_operator;
    Color syn_punct;
    Color syn_ident;
    Color editor_line;
    Color editor_select;
    Color editor_match;
    Color editor_occur;
    Color editor_find;
    Color editor_find_cur;
    Color caret;
    Color gutter_text;
    Color gutter_active;
    Color scroll_thumb;
    Color scroll_thumb_hover;
    Color icon_db;
    Color icon_table;
    Color icon_view;
    Color icon_proc;
    Color icon_column;
} Theme;

extern Theme g_theme;

int          theme_count(void);
const Theme* theme_get(int index);
int          theme_current(void);
void         theme_apply(int index);            // imposta il tema attivo
int          theme_find(const char* name);      // indice per nome (senza maiuscole), -1 se non esiste

#define TH_BG                 (g_theme.bg)
#define TH_SURFACE            (g_theme.surface)
#define TH_SURFACE_2          (g_theme.surface_2)
#define TH_EDITOR_BG          (g_theme.editor_bg)
#define TH_ELEVATED           (g_theme.elevated)
#define TH_ELEVATED_HOVER     (g_theme.elevated_hover)
#define TH_ELEVATED_PRESS     (g_theme.elevated_press)
#define TH_BORDER             (g_theme.border)
#define TH_BORDER_STRONG      (g_theme.border_strong)
#define TH_HOVER              (g_theme.hover)
#define TH_ROW_ODD            (g_theme.row_odd)
#define TH_TEXT               (g_theme.text)
#define TH_TEXT_MUTED         (g_theme.text_muted)
#define TH_TEXT_DIM           (g_theme.text_dim)
#define TH_TEXT_FAINT         (g_theme.text_faint)
#define TH_CELL_TEXT          (g_theme.cell_text)
#define TH_CELL_NUMBER        (g_theme.cell_number)
#define TH_ACCENT             (g_theme.accent)
#define TH_ACCENT_HOVER       (g_theme.accent_hover)
#define TH_ACCENT_PRESS       (g_theme.accent_press)
#define TH_ACCENT_SOFT        (g_theme.accent_soft)
#define TH_ACCENT_TEXT        (g_theme.accent_text)
#define TH_ON_ACCENT          (g_theme.on_accent)
#define TH_SUCCESS            (g_theme.success)
#define TH_SUCCESS_SOFT       (g_theme.success_soft)
#define TH_ERROR              (g_theme.error)
#define TH_ERROR_SOFT         (g_theme.error_soft)
#define TH_ERROR_TEXT         (g_theme.error_text)
#define TH_WARNING            (g_theme.warning)
#define TH_WARNING_SOFT       (g_theme.warning_soft)
#define TH_INFO               (g_theme.info)
#define TH_SYN_KEYWORD        (g_theme.syn_keyword)
#define TH_SYN_TYPE           (g_theme.syn_type)
#define TH_SYN_FUNC           (g_theme.syn_func)
#define TH_SYN_STRING         (g_theme.syn_string)
#define TH_SYN_NUMBER         (g_theme.syn_number)
#define TH_SYN_COMMENT        (g_theme.syn_comment)
#define TH_SYN_OPERATOR       (g_theme.syn_operator)
#define TH_SYN_PUNCT          (g_theme.syn_punct)
#define TH_SYN_IDENT          (g_theme.syn_ident)
#define TH_EDITOR_LINE        (g_theme.editor_line)
#define TH_EDITOR_SELECT      (g_theme.editor_select)
#define TH_EDITOR_MATCH       (g_theme.editor_match)
#define TH_EDITOR_OCCUR       (g_theme.editor_occur)
#define TH_EDITOR_FIND        (g_theme.editor_find)
#define TH_EDITOR_FIND_CUR    (g_theme.editor_find_cur)
#define TH_CARET              (g_theme.caret)
#define TH_GUTTER_TEXT        (g_theme.gutter_text)
#define TH_GUTTER_ACTIVE      (g_theme.gutter_active)
#define TH_SCROLL_THUMB       (g_theme.scroll_thumb)
#define TH_SCROLL_THUMB_HOVER (g_theme.scroll_thumb_hover)
#define TH_ICON_DB            (g_theme.icon_db)
#define TH_ICON_TABLE         (g_theme.icon_table)
#define TH_ICON_VIEW          (g_theme.icon_view)
#define TH_ICON_PROC          (g_theme.icon_proc)
#define TH_ICON_COLUMN        (g_theme.icon_column)

// ── metriche ─────────────────────────────────────────────────────
#define TH_FONT_UI        13
#define TH_FONT_SMALL     11
#define TH_FONT_MONO      14
#define TH_RADIUS         6

// colore con alpha diverso
Color ui_alpha(Color c, uint8_t a);
// interpolazione lineare tra due colori (t in 0..1)
Color ui_mix(Color a, Color b, float t);

// y del testo per centrarlo verticalmente nel rettangolo r
int  ui_text_y(PlatformWindow* win, Rect r, int size, FontStyle style);

// testo troncato con "…" se più largo di max_w; ritorna la larghezza disegnata
int  ui_text_clipped(PlatformWindow* win, const char* text, int x, int y, int max_w,
                     Color c, int size, FontStyle style);

// rettangolo arrotondato con bordo di 1px
void ui_round_box(PlatformWindow* win, Rect r, Color fill, Color border, int radius);

// primitive antialiasing (per icone vettoriali)
void ui_fill_polygon(PlatformWindow* win, const float* xy, int n, Color c);
void ui_line(PlatformWindow* win, float x0, float y0, float x1, float y1, float thickness, Color c);
void ui_circle(PlatformWindow* win, int cx, int cy, int radius, Color c);

// icone (cx, cy = centro; s = lato in pixel)
typedef enum { CHEVRON_RIGHT, CHEVRON_DOWN, CHEVRON_LEFT, CHEVRON_UP } ChevronDir;
void ui_icon_chevron (PlatformWindow* win, int cx, int cy, int s, ChevronDir dir, Color c);
void ui_icon_play    (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_check   (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_undo    (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_cross   (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_database(PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_table   (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_column  (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_key     (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_refresh (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_trash   (PlatformWindow* win, int cx, int cy, int s, Color c);
void ui_icon_logo    (PlatformWindow* win, int cx, int cy, int s);

#endif
