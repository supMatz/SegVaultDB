/*
SCOPO: Palette colori, metriche e primitive grafiche condivise da tutti i widget.
       Tenere qui i colori evita valori "magici" sparsi nel codice e rende
       l'interfaccia coerente. Le funzioni ui_* sono costruite SOLO sopra platform.h.
*/

#ifndef THEME_H
#define THEME_H

#include "../platform/platform.h"

// ── superfici ────────────────────────────────────────────────────
#define TH_BG             (Color){13,  15,  20,  255}  // sfondo app
#define TH_SURFACE        (Color){17,  20,  27,  255}  // sidebar, pannelli
#define TH_SURFACE_2      (Color){21,  25,  34,  255}  // toolbar, header tabella
#define TH_EDITOR_BG      (Color){15,  17,  24,  255}  // area editor SQL
#define TH_ELEVATED       (Color){30,  35,  48,  255}  // bottoni secondari
#define TH_ELEVATED_HOVER (Color){39,  45,  61,  255}
#define TH_ELEVATED_PRESS (Color){25,  29,  40,  255}
#define TH_BORDER         (Color){33,  38,  52,  255}
#define TH_BORDER_STRONG  (Color){46,  52,  72,  255}
#define TH_HOVER          (Color){26,  30,  42,  255}  // hover righe

// ── testo ────────────────────────────────────────────────────────
#define TH_TEXT           (Color){226, 230, 242, 255}
#define TH_TEXT_MUTED     (Color){150, 159, 186, 255}
#define TH_TEXT_DIM       (Color){92,  100, 128, 255}
#define TH_TEXT_FAINT     (Color){60,  67,  89,  255}

// ── accento e stati ──────────────────────────────────────────────
#define TH_ACCENT         (Color){124, 106, 255, 255}  // viola elettrico
#define TH_ACCENT_HOVER   (Color){143, 128, 255, 255}
#define TH_ACCENT_PRESS   (Color){99,  82,  224, 255}
#define TH_ACCENT_SOFT    (Color){38,  33,  74,  255}  // sfondo selezione
#define TH_ACCENT_TEXT    (Color){185, 175, 255, 255}

#define TH_SUCCESS        (Color){61,  214, 140, 255}
#define TH_SUCCESS_SOFT   (Color){18,  48,  38,  255}
#define TH_ERROR          (Color){255, 92,  122, 255}
#define TH_ERROR_SOFT     (Color){58,  22,  34,  255}
#define TH_WARNING        (Color){255, 181, 71,  255}
#define TH_WARNING_SOFT   (Color){56,  40,  18,  255}
#define TH_INFO           (Color){76,  194, 255, 255}

// ── editor SQL ───────────────────────────────────────────────────
#define TH_SYN_KEYWORD    (Color){199, 146, 255, 255}  // SELECT, FROM...
#define TH_SYN_TYPE       (Color){92,  207, 230, 255}  // INT, VARCHAR...
#define TH_SYN_FUNC       (Color){130, 170, 255, 255}  // COUNT, SUM...
#define TH_SYN_STRING     (Color){171, 224, 125, 255}
#define TH_SYN_NUMBER     (Color){255, 184, 108, 255}
#define TH_SYN_COMMENT    (Color){88,  98,  128, 255}
#define TH_SYN_OPERATOR   (Color){137, 221, 255, 255}
#define TH_SYN_PUNCT      (Color){150, 159, 186, 255}
#define TH_SYN_IDENT      (Color){222, 226, 240, 255}

#define TH_EDITOR_LINE    (Color){21,  24,  35,  255}  // riga corrente
#define TH_EDITOR_SELECT  (Color){48,  43,  98,  255}  // selezione testo
#define TH_EDITOR_MATCH   (Color){52,  60,  84,  255}  // parentesi corrispondente
#define TH_EDITOR_OCCUR   (Color){40,  46,  66,  255}  // altre occorrenze della parola sotto il cursore
#define TH_EDITOR_FIND    (Color){78,  62,  22,  255}  // risultati della ricerca
#define TH_EDITOR_FIND_CUR (Color){150, 108, 30, 255}  // risultato di ricerca corrente
#define TH_GUTTER_TEXT    (Color){58,  65,  88,  255}
#define TH_GUTTER_ACTIVE  (Color){170, 178, 205, 255}

// ── sidebar ──────────────────────────────────────────────────────
#define TH_ICON_DB        (Color){76,  194, 255, 255}
#define TH_ICON_TABLE     (Color){61,  214, 140, 255}
#define TH_ICON_VIEW      (Color){255, 181, 71,  255}
#define TH_ICON_PROC      (Color){199, 146, 255, 255}
#define TH_ICON_COLUMN    (Color){110, 119, 150, 255}

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
