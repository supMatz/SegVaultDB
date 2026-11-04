/*
SCOPO: primitive grafiche condivise: testo troncato, box arrotondati,
       poligoni antialiasing e icone vettoriali disegnate a mano.
*/

#include "theme.h"
#include <math.h>
#include <string.h>

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
                                   (Color){255, 255, 255, (uint8_t)(i == 0 ? 245 : 190)}, bh / 2);
    }
}
