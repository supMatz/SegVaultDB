/*
SCOPO: primitive grafiche condivise: testo troncato, box arrotondati,
       poligoni antialiasing e icone vettoriali disegnate a mano.
*/

#include "theme.h"
#include <math.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// altezza di un "disco" nelle icone a pila (database, logo)
#define SV_ICON_DISC_H(s) (((int)((s) * 0.26f)) < 3 ? 3 : (int)((s) * 0.26f))

Color ui_alpha(Color c, uint8_t a) { c.a = a; return c; }

Color ui_mix(Color a, Color b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    Color r;
    r.r = (uint8_t)(a.r + (b.r - a.r) * t);
    r.g = (uint8_t)(a.g + (b.g - a.g) * t);
    r.b = (uint8_t)(a.b + (b.b - a.b) * t);
    r.a = (uint8_t)(a.a + (b.a - a.a) * t);
    return r;
}

int ui_text_y(PlatformWindow* win, Rect r, int size, FontStyle style) {
    int th = platform_text_height(win, size, style);
    return r.y + (r.h - th) / 2;
}

int ui_text_clipped(PlatformWindow* win, const char* text, int x, int y, int max_w,
                    Color c, int size, FontStyle style) {
    if (!text || !text[0] || max_w <= 0) return 0;
    int len  = (int)strlen(text);
    int full = platform_measure_text_ex(win, text, len, size, style);
    if (full <= max_w)
        return platform_draw_text_ex(win, text, len, (Point){x, y}, c, size, style);

    static const char* ELL = "\xe2\x80\xa6"; // "…"
    int ell_w = platform_measure_text_ex(win, ELL, 3, size, style);
    int avail = max_w - ell_w;
    if (avail <= 0) return 0;

    // ricerca binaria del prefisso più lungo che entra (allineato ai caratteri UTF-8)
    int lo = 0, hi = len;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        while (mid > 0 && mid < len && ((unsigned char)text[mid] & 0xC0) == 0x80) mid--;
        if (mid <= lo) { hi = lo; break; }
        if (platform_measure_text_ex(win, text, mid, size, style) <= avail) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && lo < len && ((unsigned char)text[lo] & 0xC0) == 0x80) lo--;
    // niente spazi prima dei puntini
    while (lo > 0 && text[lo - 1] == ' ') lo--;

    int w = platform_draw_text_ex(win, text, lo, (Point){x, y}, c, size, style);
    w += platform_draw_text_ex(win, ELL, 3, (Point){x + w, y}, c, size, style);
    return w;
}

void ui_round_box(PlatformWindow* win, Rect r, Color fill, Color border, int radius) {
    platform_fill_rect_rounded(win, r, border, radius);
    platform_fill_rect_rounded(win, (Rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, fill,
                               radius > 1 ? radius - 1 : 0);
}

// -- poligoni antialiasing (4x4 sotto-campioni per pixel) --

static bool point_in_poly(const float* xy, int n, float px, float py) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = xy[i * 2], yi = xy[i * 2 + 1];
        float xj = xy[j * 2], yj = xy[j * 2 + 1];
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

void ui_fill_polygon(PlatformWindow* win, const float* xy, int n, Color c) {
    if (n < 3) return;
    float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 1; i < n; i++) {
        if (xy[i * 2]     < minx) minx = xy[i * 2];
        if (xy[i * 2]     > maxx) maxx = xy[i * 2];
        if (xy[i * 2 + 1] < miny) miny = xy[i * 2 + 1];
        if (xy[i * 2 + 1] > maxy) maxy = xy[i * 2 + 1];
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);

    for (int py = y0; py < y1; py++) {
        int run_start = -1;
        for (int px = x0; px <= x1; px++) {
            int inside = 0;
            if (px < x1) {
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        if (point_in_poly(xy, n, px + (sx + 0.5f) / 4.0f, py + (sy + 0.5f) / 4.0f))
                            inside++;
            }
            if (inside == 16) {
                if (run_start < 0) run_start = px;
                continue;
            }
            if (run_start >= 0) {
                platform_fill_rect(win, (Rect){run_start, py, px - run_start, 1}, c);
                run_start = -1;
            }
            if (inside > 0) {
                Color pc = c;
                pc.a = (uint8_t)(c.a * inside / 16);
                platform_fill_rect(win, (Rect){px, py, 1, 1}, pc);
            }
        }
    }
}

void ui_line(PlatformWindow* win, float x0, float y0, float x1, float y1, float thickness, Color c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float h  = thickness / 2.0f;
    float nx = -dy / len * h, ny = dx / len * h;
    // estende leggermente gli estremi per giunzioni pulite
    float ex = dx / len * h * 0.5f, ey = dy / len * h * 0.5f;
    float q[8] = {
        x0 + nx - ex, y0 + ny - ey,
        x1 + nx + ex, y1 + ny + ey,
        x1 - nx + ex, y1 - ny + ey,
        x0 - nx - ex, y0 - ny - ey,
    };
    ui_fill_polygon(win, q, 4, c);
}

void ui_circle(PlatformWindow* win, int cx, int cy, int radius, Color c) {
    platform_fill_rect_rounded(win, (Rect){cx - radius, cy - radius, radius * 2, radius * 2}, c, radius);
}

static void polyline(PlatformWindow* win, const float* pts, int n, float t, Color c) {
    for (int i = 0; i + 1 < n; i++)
        ui_line(win, pts[i * 2], pts[i * 2 + 1], pts[i * 2 + 2], pts[i * 2 + 3], t, c);
}

static void arc(PlatformWindow* win, float cx, float cy, float r, float a0, float a1, float t, Color c) {
    const int SEG = 14;
    float pts[(SEG + 1) * 2];
    for (int i = 0; i <= SEG; i++) {
        float a = a0 + (a1 - a0) * i / SEG;
        pts[i * 2]     = cx + r * cosf(a);
        pts[i * 2 + 1] = cy + r * sinf(a);
    }
    polyline(win, pts, SEG + 1, t, c);
}

// -- icone --

void ui_icon_chevron(PlatformWindow* win, int cx, int cy, int s, ChevronDir dir, Color c) {
    float a = s * 0.22f, b = s * 0.40f;
    float p[6];
    switch (dir) {
        case CHEVRON_RIGHT: { float q[6] = {cx - a, cy - b, cx + a, cy, cx - a, cy + b}; memcpy(p, q, sizeof p); break; }
        case CHEVRON_LEFT:  { float q[6] = {cx + a, cy - b, cx - a, cy, cx + a, cy + b}; memcpy(p, q, sizeof p); break; }
        case CHEVRON_DOWN:  { float q[6] = {cx - b, cy - a, cx, cy + a, cx + b, cy - a}; memcpy(p, q, sizeof p); break; }
        default:            { float q[6] = {cx - b, cy + a, cx, cy - a, cx + b, cy + a}; memcpy(p, q, sizeof p); break; }
    }
    polyline(win, p, 3, 1.6f, c);
}

void ui_icon_play(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float tri[6] = {
        cx - s * 0.30f, cy - s * 0.40f,
        cx + s * 0.40f, cy,
        cx - s * 0.30f, cy + s * 0.40f,
    };
    ui_fill_polygon(win, tri, 3, c);
}

void ui_icon_check(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float p[6] = {
        cx - s * 0.36f, cy + s * 0.02f,
        cx - s * 0.10f, cy + s * 0.28f,
        cx + s * 0.38f, cy - s * 0.28f,
    };
    polyline(win, p, 3, 2.0f, c);
}

void ui_icon_cross(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float d = s * 0.30f;
    ui_line(win, cx - d, cy - d, cx + d, cy + d, 1.8f, c);
    ui_line(win, cx + d, cy - d, cx - d, cy + d, 1.8f, c);
}

void ui_icon_undo(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float r = s * 0.32f;
    float ox = cx + s * 0.04f, oy = cy + s * 0.04f;
    arc(win, ox, oy, r, (float)M_PI, (float)(2.5 * M_PI), 1.8f, c);
    // punta di freccia sull'estremo sinistro, verso il basso
    float hx = ox - r, hy = oy;
    float tri[6] = {
        hx - s * 0.20f, hy - s * 0.04f,
        hx + s * 0.20f, hy - s * 0.04f,
        hx,             hy + s * 0.22f,
    };
    ui_fill_polygon(win, tri, 3, c);
}

void ui_icon_refresh(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float r = s * 0.33f;
    arc(win, cx, cy, r, (float)(-0.35 * M_PI), (float)(1.35 * M_PI), 1.7f, c);
    float a  = (float)(-0.35 * M_PI);
    float hx = cx + r * cosf(a), hy = cy + r * sinf(a);
    float tri[6] = {
        hx - s * 0.04f, hy - s * 0.24f,
        hx + s * 0.22f, hy + s * 0.04f,
        hx - s * 0.16f, hy + s * 0.12f,
    };
    ui_fill_polygon(win, tri, 3, c);
}

void ui_icon_trash(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float w = s * 0.30f, top = cy - s * 0.22f, bot = cy + s * 0.38f;
    ui_line(win, cx - s * 0.40f, top, cx + s * 0.40f, top, 1.6f, c);
    ui_line(win, cx - s * 0.12f, top - s * 0.12f, cx + s * 0.12f, top - s * 0.12f, 1.6f, c);
    float body[8] = { cx - w, top + 1, cx + w, top + 1, cx + w * 0.8f, bot, cx - w * 0.8f, bot };
    polyline(win, body, 4, 1.5f, c);
    ui_line(win, cx - w * 0.8f, bot, cx + w * 0.8f, bot, 1.5f, c);
}

void ui_icon_database(PlatformWindow* win, int cx, int cy, int s, Color c) {
    int w  = (int)(s * 0.80f);
    int dh = SV_ICON_DISC_H(s);
    int x  = cx - w / 2;
    int y  = cy - (dh * 3 + 2) / 2;
    for (int i = 0; i < 3; i++) {
        Color dc = (i == 0) ? c : ui_alpha(c, (uint8_t)(c.a * (i == 1 ? 0.75f : 0.55f)));
        platform_fill_rect_rounded(win, (Rect){x, y + i * (dh + 1), w, dh}, dc, dh / 2);
    }
}

void ui_icon_table(PlatformWindow* win, int cx, int cy, int s, Color c) {
    int w = (int)(s * 0.86f), h = (int)(s * 0.72f);
    int x = cx - w / 2, y = cy - h / 2;
    platform_fill_rect_rounded(win, (Rect){x, y, w, 4}, c, 1);          // header
    platform_fill_rect(win, (Rect){x, y + 2, 1, h - 2}, c);             // bordo sx
    platform_fill_rect(win, (Rect){x + w - 1, y + 2, 1, h - 2}, c);     // bordo dx
    platform_fill_rect(win, (Rect){x, y + h - 1, w, 1}, c);             // bordo basso
    platform_fill_rect(win, (Rect){x, y + 4 + (h - 5) / 2, w, 1}, ui_alpha(c, 160));
    platform_fill_rect(win, (Rect){x + w / 2, y + 4, 1, h - 5}, ui_alpha(c, 160));
}

void ui_icon_column(PlatformWindow* win, int cx, int cy, int s, Color c) {
    int w = (int)(s * 0.36f), h = (int)(s * 0.70f);
    platform_fill_rect_rounded(win, (Rect){cx - w / 2, cy - h / 2, w, h}, ui_alpha(c, 70), 2);
    platform_fill_rect_rounded(win, (Rect){cx - w / 2, cy - h / 2, w, 3}, c, 1);
}

void ui_icon_key(PlatformWindow* win, int cx, int cy, int s, Color c) {
    float r = s * 0.22f;
    float kx = cx - s * 0.18f;
    arc(win, kx, (float)cy, r, 0, (float)(2 * M_PI), 1.6f, c);
    ui_line(win, kx + r, (float)cy, cx + s * 0.42f, (float)cy, 1.6f, c);
    ui_line(win, cx + s * 0.30f, (float)cy, cx + s * 0.30f, cy + s * 0.20f, 1.6f, c);
}

void ui_icon_logo(PlatformWindow* win, int cx, int cy, int s) {
    Rect r = {cx - s / 2, cy - s / 2, s, s};
    platform_fill_rect_rounded(win, r, TH_ACCENT, s / 4);
    // leggero riflesso nella metà alta
    platform_fill_rect_rounded(win, (Rect){r.x + 1, r.y + 1, s - 2, s / 2}, ui_alpha((Color){255, 255, 255, 255}, 22), s / 4);
    // tre "segmenti" del vault
    int bw[3] = { (int)(s * 0.56f), (int)(s * 0.40f), (int)(s * 0.50f) };
    int bh = SV_ICON_DISC_H(s) - 1;
    if (bh < 2) bh = 2;
    int total = bh * 3 + 4;
    int y = cy - total / 2;
    for (int i = 0; i < 3; i++) {
        platform_fill_rect_rounded(win, (Rect){r.x + (int)(s * 0.22f), y + i * (bh + 2), bw[i], bh},
                                   ui_alpha(TH_ON_ACCENT, (uint8_t)(i == 0 ? 245 : 190)), bh / 2);
    }
}

// ═════════════════════════════════════════════════════════════════
//  Temi
// ═════════════════════════════════════════════════════════════════

#define RGB(r, g, b) {r, g, b, 255}

static const Theme THEMES[] = {
    {
        .name = "Sky", .light = false,
        .bg = RGB(11, 15, 22),
        .surface = RGB(15, 20, 29),
        .surface_2 = RGB(19, 25, 36),
        .editor_bg = RGB(13, 18, 26),
        .elevated = RGB(28, 36, 50),
        .elevated_hover = RGB(37, 47, 65),
        .elevated_press = RGB(23, 30, 42),
        .border = RGB(30, 39, 54),
        .border_strong = RGB(43, 55, 76),
        .hover = RGB(24, 31, 44),
        .row_odd = RGB(14, 19, 27),
        .text = RGB(226, 234, 244),
        .text_muted = RGB(148, 163, 184),
        .text_dim = RGB(94, 108, 132),
        .text_faint = RGB(58, 70, 90),
        .cell_text = RGB(212, 222, 236),
        .cell_number = RGB(251, 191, 120),
        .accent = RGB(56, 189, 248),
        .accent_hover = RGB(125, 211, 252),
        .accent_press = RGB(14, 165, 233),
        .accent_soft = RGB(15, 52, 78),
        .accent_text = RGB(147, 219, 255),
        .on_accent = RGB(4, 24, 40),
        .success = RGB(52, 211, 153),
        .success_soft = RGB(14, 46, 38),
        .error = RGB(248, 113, 133),
        .error_soft = RGB(60, 22, 32),
        .error_text = RGB(255, 175, 188),
        .warning = RGB(251, 191, 36),
        .warning_soft = RGB(54, 40, 12),
        .info = RGB(96, 165, 250),
        .syn_keyword = RGB(103, 200, 255),
        .syn_type = RGB(94, 234, 212),
        .syn_func = RGB(165, 180, 252),
        .syn_string = RGB(163, 230, 123),
        .syn_number = RGB(251, 191, 120),
        .syn_comment = RGB(84, 98, 122),
        .syn_operator = RGB(125, 211, 252),
        .syn_punct = RGB(148, 163, 184),
        .syn_ident = RGB(222, 230, 240),
        .editor_line = RGB(18, 25, 37),
        .editor_select = RGB(22, 64, 96),
        .editor_match = RGB(40, 56, 78),
        .editor_occur = RGB(30, 44, 62),
        .editor_find = RGB(78, 62, 22),
        .editor_find_cur = RGB(150, 108, 30),
        .caret = RGB(125, 211, 252),
        .gutter_text = RGB(56, 68, 88),
        .gutter_active = RGB(170, 184, 205),
        .scroll_thumb = RGB(48, 60, 80),
        .scroll_thumb_hover = RGB(84, 100, 128),
        .icon_db = RGB(56, 189, 248),
        .icon_table = RGB(52, 211, 153),
        .icon_view = RGB(251, 191, 36),
        .icon_proc = RGB(165, 180, 252),
        .icon_column = RGB(110, 124, 150),
    },
    {
        .name = "Violet", .light = false,
        .bg = RGB(13, 15, 20),
        .surface = RGB(17, 20, 27),
        .surface_2 = RGB(21, 25, 34),
        .editor_bg = RGB(15, 17, 24),
        .elevated = RGB(30, 35, 48),
        .elevated_hover = RGB(39, 45, 61),
        .elevated_press = RGB(25, 29, 40),
        .border = RGB(33, 38, 52),
        .border_strong = RGB(46, 52, 72),
        .hover = RGB(26, 30, 42),
        .row_odd = RGB(16, 18, 25),
        .text = RGB(226, 230, 242),
        .text_muted = RGB(150, 159, 186),
        .text_dim = RGB(92, 100, 128),
        .text_faint = RGB(60, 67, 89),
        .cell_text = RGB(214, 219, 234),
        .cell_number = RGB(245, 200, 150),
        .accent = RGB(124, 106, 255),
        .accent_hover = RGB(143, 128, 255),
        .accent_press = RGB(99, 82, 224),
        .accent_soft = RGB(38, 33, 74),
        .accent_text = RGB(185, 175, 255),
        .on_accent = RGB(255, 255, 255),
        .success = RGB(61, 214, 140),
        .success_soft = RGB(18, 48, 38),
        .error = RGB(255, 92, 122),
        .error_soft = RGB(58, 22, 34),
        .error_text = RGB(255, 170, 185),
        .warning = RGB(255, 181, 71),
        .warning_soft = RGB(56, 40, 18),
        .info = RGB(76, 194, 255),
        .syn_keyword = RGB(199, 146, 255),
        .syn_type = RGB(92, 207, 230),
        .syn_func = RGB(130, 170, 255),
        .syn_string = RGB(171, 224, 125),
        .syn_number = RGB(255, 184, 108),
        .syn_comment = RGB(88, 98, 128),
        .syn_operator = RGB(137, 221, 255),
        .syn_punct = RGB(150, 159, 186),
        .syn_ident = RGB(222, 226, 240),
        .editor_line = RGB(21, 24, 35),
        .editor_select = RGB(48, 43, 98),
        .editor_match = RGB(52, 60, 84),
        .editor_occur = RGB(40, 46, 66),
        .editor_find = RGB(78, 62, 22),
        .editor_find_cur = RGB(150, 108, 30),
        .caret = RGB(176, 164, 255),
        .gutter_text = RGB(58, 65, 88),
        .gutter_active = RGB(170, 178, 205),
        .scroll_thumb = RGB(58, 64, 86),
        .scroll_thumb_hover = RGB(96, 104, 136),
        .icon_db = RGB(76, 194, 255),
        .icon_table = RGB(61, 214, 140),
        .icon_view = RGB(255, 181, 71),
        .icon_proc = RGB(199, 146, 255),
        .icon_column = RGB(110, 119, 150),
    },
    {
        .name = "Graphite", .light = false,
        .bg = RGB(18, 18, 19),
        .surface = RGB(22, 22, 24),
        .surface_2 = RGB(27, 27, 30),
        .editor_bg = RGB(20, 20, 22),
        .elevated = RGB(38, 38, 42),
        .elevated_hover = RGB(48, 48, 53),
        .elevated_press = RGB(31, 31, 35),
        .border = RGB(40, 40, 44),
        .border_strong = RGB(56, 56, 62),
        .hover = RGB(33, 33, 37),
        .row_odd = RGB(21, 21, 23),
        .text = RGB(232, 232, 235),
        .text_muted = RGB(165, 165, 172),
        .text_dim = RGB(110, 110, 118),
        .text_faint = RGB(70, 70, 76),
        .cell_text = RGB(220, 220, 225),
        .cell_number = RGB(250, 204, 140),
        .accent = RGB(245, 166, 35),
        .accent_hover = RGB(250, 190, 90),
        .accent_press = RGB(220, 140, 20),
        .accent_soft = RGB(60, 44, 16),
        .accent_text = RGB(252, 205, 130),
        .on_accent = RGB(30, 20, 4),
        .success = RGB(74, 222, 128),
        .success_soft = RGB(20, 46, 30),
        .error = RGB(248, 113, 113),
        .error_soft = RGB(60, 24, 24),
        .error_text = RGB(255, 180, 180),
        .warning = RGB(250, 204, 21),
        .warning_soft = RGB(56, 46, 10),
        .info = RGB(96, 165, 250),
        .syn_keyword = RGB(245, 166, 35),
        .syn_type = RGB(103, 232, 249),
        .syn_func = RGB(147, 197, 253),
        .syn_string = RGB(190, 227, 128),
        .syn_number = RGB(216, 180, 254),
        .syn_comment = RGB(100, 100, 108),
        .syn_operator = RGB(230, 230, 235),
        .syn_punct = RGB(160, 160, 168),
        .syn_ident = RGB(228, 228, 232),
        .editor_line = RGB(26, 26, 29),
        .editor_select = RGB(70, 55, 30),
        .editor_match = RGB(60, 60, 66),
        .editor_occur = RGB(46, 46, 52),
        .editor_find = RGB(30, 70, 90),
        .editor_find_cur = RGB(40, 110, 150),
        .caret = RGB(250, 190, 90),
        .gutter_text = RGB(72, 72, 78),
        .gutter_active = RGB(190, 190, 196),
        .scroll_thumb = RGB(60, 60, 66),
        .scroll_thumb_hover = RGB(100, 100, 108),
        .icon_db = RGB(96, 165, 250),
        .icon_table = RGB(74, 222, 128),
        .icon_view = RGB(245, 166, 35),
        .icon_proc = RGB(216, 180, 254),
        .icon_column = RGB(130, 130, 138),
    },
    {
        .name = "Forest", .light = false,
        .bg = RGB(12, 18, 16),
        .surface = RGB(16, 23, 20),
        .surface_2 = RGB(20, 29, 25),
        .editor_bg = RGB(14, 20, 18),
        .elevated = RGB(29, 40, 35),
        .elevated_hover = RGB(38, 52, 45),
        .elevated_press = RGB(24, 33, 29),
        .border = RGB(30, 42, 37),
        .border_strong = RGB(44, 60, 52),
        .hover = RGB(24, 34, 30),
        .row_odd = RGB(15, 21, 19),
        .text = RGB(225, 238, 230),
        .text_muted = RGB(152, 176, 163),
        .text_dim = RGB(96, 118, 106),
        .text_faint = RGB(58, 76, 66),
        .cell_text = RGB(212, 228, 219),
        .cell_number = RGB(250, 204, 140),
        .accent = RGB(52, 211, 153),
        .accent_hover = RGB(110, 231, 183),
        .accent_press = RGB(16, 185, 129),
        .accent_soft = RGB(16, 56, 42),
        .accent_text = RGB(134, 239, 190),
        .on_accent = RGB(3, 30, 20),
        .success = RGB(74, 222, 128),
        .success_soft = RGB(18, 50, 32),
        .error = RGB(248, 113, 113),
        .error_soft = RGB(60, 24, 26),
        .error_text = RGB(255, 180, 180),
        .warning = RGB(251, 191, 36),
        .warning_soft = RGB(54, 42, 12),
        .info = RGB(103, 232, 249),
        .syn_keyword = RGB(52, 211, 153),
        .syn_type = RGB(103, 232, 249),
        .syn_func = RGB(190, 242, 100),
        .syn_string = RGB(253, 224, 71),
        .syn_number = RGB(251, 146, 60),
        .syn_comment = RGB(90, 112, 100),
        .syn_operator = RGB(167, 243, 208),
        .syn_punct = RGB(152, 176, 163),
        .syn_ident = RGB(226, 236, 230),
        .editor_line = RGB(20, 29, 25),
        .editor_select = RGB(24, 72, 54),
        .editor_match = RGB(40, 58, 50),
        .editor_occur = RGB(30, 46, 40),
        .editor_find = RGB(78, 62, 22),
        .editor_find_cur = RGB(150, 108, 30),
        .caret = RGB(110, 231, 183),
        .gutter_text = RGB(58, 78, 68),
        .gutter_active = RGB(176, 200, 186),
        .scroll_thumb = RGB(48, 66, 58),
        .scroll_thumb_hover = RGB(86, 110, 98),
        .icon_db = RGB(103, 232, 249),
        .icon_table = RGB(52, 211, 153),
        .icon_view = RGB(251, 191, 36),
        .icon_proc = RGB(190, 242, 100),
        .icon_column = RGB(120, 142, 130),
    },
    {
        .name = "Daylight", .light = true,
        .bg = RGB(255, 255, 255),
        .surface = RGB(246, 248, 251),
        .surface_2 = RGB(240, 243, 247),
        .editor_bg = RGB(255, 255, 255),
        .elevated = RGB(255, 255, 255),
        .elevated_hover = RGB(236, 241, 247),
        .elevated_press = RGB(226, 232, 240),
        .border = RGB(222, 227, 234),
        .border_strong = RGB(203, 211, 222),
        .hover = RGB(238, 243, 249),
        .row_odd = RGB(248, 250, 252),
        .text = RGB(23, 32, 46),
        .text_muted = RGB(71, 85, 105),
        .text_dim = RGB(110, 123, 143),
        .text_faint = RGB(160, 171, 186),
        .cell_text = RGB(30, 41, 59),
        .cell_number = RGB(180, 83, 9),
        .accent = RGB(2, 132, 199),
        .accent_hover = RGB(14, 165, 233),
        .accent_press = RGB(3, 105, 161),
        .accent_soft = RGB(219, 238, 252),
        .accent_text = RGB(3, 105, 161),
        .on_accent = RGB(255, 255, 255),
        .success = RGB(22, 163, 74),
        .success_soft = RGB(220, 246, 229),
        .error = RGB(220, 38, 38),
        .error_soft = RGB(254, 226, 226),
        .error_text = RGB(185, 28, 28),
        .warning = RGB(202, 138, 4),
        .warning_soft = RGB(254, 243, 199),
        .info = RGB(37, 99, 235),
        .syn_keyword = RGB(2, 110, 180),
        .syn_type = RGB(13, 148, 136),
        .syn_func = RGB(124, 58, 237),
        .syn_string = RGB(22, 128, 61),
        .syn_number = RGB(194, 65, 12),
        .syn_comment = RGB(140, 150, 165),
        .syn_operator = RGB(71, 85, 105),
        .syn_punct = RGB(100, 116, 139),
        .syn_ident = RGB(30, 41, 59),
        .editor_line = RGB(241, 246, 252),
        .editor_select = RGB(191, 224, 250),
        .editor_match = RGB(219, 227, 236),
        .editor_occur = RGB(226, 234, 243),
        .editor_find = RGB(254, 240, 170),
        .editor_find_cur = RGB(252, 211, 77),
        .caret = RGB(2, 132, 199),
        .gutter_text = RGB(170, 180, 194),
        .gutter_active = RGB(51, 65, 85),
        .scroll_thumb = RGB(203, 211, 222),
        .scroll_thumb_hover = RGB(148, 163, 184),
        .icon_db = RGB(2, 132, 199),
        .icon_table = RGB(22, 163, 74),
        .icon_view = RGB(202, 138, 4),
        .icon_proc = RGB(124, 58, 237),
        .icon_column = RGB(120, 134, 155),
    },
};

#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

int   g_ui_font = 13;
Theme g_theme = THEMES[0];       // Sky: tema predefinito
static int g_theme_index = 0;

int theme_count(void) { return THEME_COUNT; }

const Theme* theme_get(int index) {
    return (index >= 0 && index < THEME_COUNT) ? &THEMES[index] : NULL;
}

int theme_current(void) { return g_theme_index; }

static bool  g_accent_custom;
static Color g_accent_base;

static Color mixc(Color a, Color b, float t) {
    return (Color){ (uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                    (uint8_t)(a.b + (b.b - a.b) * t), 255 };
}

// ricava dall'accento scelto tutte le sue varianti, adattate a tema chiaro o scuro
static void apply_accent(Theme* t, Color base) {
    Color white = {255, 255, 255, 255}, black = {0, 0, 0, 255};
    t->accent       = base;
    t->accent_hover = t->light ? mixc(base, black, 0.12f) : mixc(base, white, 0.30f);
    t->accent_press = mixc(base, black, 0.20f);
    t->accent_soft  = mixc(t->bg, base, t->light ? 0.14f : 0.24f);
    t->accent_text  = t->light ? mixc(base, black, 0.35f) : mixc(base, white, 0.45f);
    int lum = (base.r * 299 + base.g * 587 + base.b * 114) / 1000;
    t->on_accent    = lum > 150 ? mixc(base, black, 0.85f) : white;
    t->caret        = t->light ? t->accent_press : t->accent_hover;
    t->editor_select = mixc(t->editor_bg, base, t->light ? 0.22f : 0.32f);
    t->gutter_active = t->accent_text;
}

void theme_apply(int index) {
    if (index < 0 || index >= THEME_COUNT) return;
    g_theme_index = index;
    g_theme = THEMES[index];
    if (g_accent_custom) apply_accent(&g_theme, g_accent_base);
}

void theme_set_accent(bool custom, Color base) {
    g_accent_custom = custom;
    g_accent_base   = base;
    theme_apply(g_theme_index);
}

int theme_find(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < THEME_COUNT; i++)
        if (strcasecmp(THEMES[i].name, name) == 0) return i;
    return -1;
}
