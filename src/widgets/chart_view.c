#include "chart_view.h"
#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int row_at(const int* rows, int i) { return rows ? rows[i] : i; }

static bool num_text(const char* s, double* v) {
    char* e;
    double d = strtod(s, &e);
    if (e == s) return false;
    while (*e == ' ') e++;
    if (*e) return false;
    if (v) *v = d;
    return isfinite(d);
}

bool chart_numeric_col(const QueryResult* r, const int* rows, int nrows, int col) {
    int seen = 0;
    for (int i = 0; i < nrows; i++) {
        const Row* row = &r->rows[row_at(rows, i)];
        if (col >= row->num_cells || row->cells[col].is_null) continue;
        if (!num_text(row->cells[col].value, NULL)) return false;
        seen++;
    }
    return seen > 0;
}

bool chart_model_build(const QueryResult* r, const int* rows, int nrows, ChartModel* m, char* why, size_t wn) {
    bool keep_line = m->line, keep_auto = m->line_auto;
    memset(m, 0, sizeof(*m));
    m->label_col = -1;
    if (!r || r->num_cols <= 0 || nrows <= 0) { snprintf(why, wn, "No rows to chart"); return false; }
    int nums[64], nn = 0;
    for (int c = 0; c < r->num_cols && nn < 64; c++) {
        if (chart_numeric_col(r, rows, nrows, c)) nums[nn++] = c;
        else if (m->label_col < 0) m->label_col = c;
    }
    if (!nn) { snprintf(why, wn, "No numeric column to chart (charts need at least one column of numbers)"); return false; }
    int first = 0;
    if (m->label_col < 0 && nn >= 2) { m->label_col = nums[0]; first = 1; }   // tutte numeriche: la prima fa da etichetta
    for (int k = first; k < nn && m->nseries < CHART_MAX_SERIES; k++) m->series[m->nseries++] = nums[k];
    m->line_auto = keep_auto || (!keep_auto && !keep_line);
    m->line = m->line_auto ? nrows > 40 : keep_line;
    return true;
}

void chart_ticks(double min, double max, int target, double* lo, double* hi, double* step) {
    if (target < 2) target = 2;
    if (max < min) { double t = min; min = max; max = t; }
    if (max - min < 1e-12) { max = min + 1; if (min > 0) min = 0; }
    double raw = (max - min) / target;
    double mag = pow(10, floor(log10(raw)));
    double f = raw / mag;
    double nice = f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10;
    *step = nice * mag;
    *lo = floor(min / *step) * *step;
    *hi = ceil(max / *step) * *step;
}

void chart_fmt(double v, double step, char* out, size_t n) {
    double a = fabs(v);
    if (a >= 1e9) snprintf(out, n, "%.3gG", v / 1e9);
    else if (a >= 1e6) snprintf(out, n, "%.3gM", v / 1e6);
    else if (a >= 1e4) snprintf(out, n, "%.3gk", v / 1e3);
    else {
        int dec = step >= 1 ? 0 : (int)ceil(-log10(step));
        if (dec > 6) dec = 6;
        snprintf(out, n, "%.*f", dec, v);
    }
}

// palette categorica (ordine fisso; passi diversi per tema chiaro e scuro)
static Color series_color(int i, bool dark) {
    static const uint32_t L[8] = { 0x2a78d6, 0xeb6834, 0x1baf7a, 0xeda100, 0xe87ba4, 0x008300, 0x6250d6, 0xe34948 };
    static const uint32_t D[8] = { 0x3987e5, 0xd95926, 0x199e70, 0xc98500, 0xd55181, 0x008300, 0x9085e9, 0xe66767 };
    uint32_t v = (dark ? D : L)[i & 7];
    return (Color){ (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, 255 };
}

static double cell_value(const QueryResult* r, int row, int col, bool* ok) {
    const Row* rw = &r->rows[row];
    double v = 0;
    *ok = col < rw->num_cells && !rw->cells[col].is_null && num_text(rw->cells[col].value, &v);
    return *ok ? v : 0;
}

static void label_of(const QueryResult* r, const int* rows, int i, int col, char* out, size_t n) {
    if (col < 0) { snprintf(out, n, "%d", i + 1); return; }
    const Row* rw = &r->rows[row_at(rows, i)];
    snprintf(out, n, "%.60s", col < rw->num_cells && !rw->cells[col].is_null ? rw->cells[col].value : "NULL");
}

// testo accorciato con "…" per stare in w pixel
static void fit_text(PlatformWindow* win, char* s, int w, int size) {
    if (platform_measure_text(win, s, size) <= w) return;
    size_t l = strlen(s);
    while (l > 0) {
        do l--; while (l > 0 && ((unsigned char)s[l] & 0xC0) == 0x80);
        char save[4];
        memcpy(save, s + l, 4);
        memcpy(s + l, "\xe2\x80\xa6", 4);
        if (platform_measure_text(win, s, size) <= w || l == 0) return;
        memcpy(s + l, save, 4);
        s[l] = '\0';
    }
}

void chart_draw(PlatformWindow* win, Rect area, const QueryResult* r, const int* rows, int nrows,
                const ChartModel* m, int mx, int my, bool dark) {
    Color text = TH_TEXT, muted = TH_TEXT_MUTED, grid = TH_BORDER, faint = TH_TEXT_DIM;
    platform_fill_rect(win, area, TH_SURFACE);
    if (!r || m->nseries == 0 || nrows <= 0) return;
    int fs = 12;
    int th = platform_text_height(win, fs, FONT_UI);
    // valori
    double vmin = 0, vmax = 0;
    for (int i = 0; i < nrows; i++)
        for (int s = 0; s < m->nseries; s++) {
            bool ok;
            double v = cell_value(r, row_at(rows, i), m->series[s], &ok);
            if (ok) { if (v < vmin) vmin = v; if (v > vmax) vmax = v; }
        }
    // legenda (2 o più serie) in alto
    int top = area.y + 12;
    if (m->nseries >= 2) {
        int lx = area.x + 16;
        for (int s = 0; s < m->nseries; s++) {
            const char* nm = r->col_names[m->series[s]] ? r->col_names[m->series[s]] : "";
            platform_fill_rect_rounded(win, (Rect){lx, top + th / 2 - 5, 10, 10}, series_color(s, dark), 2);
            lx += 16 + platform_draw_text(win, nm, (Point){lx + 16, top}, muted, fs) + 18;
        }
        top += th + 10;
    } else {
        char t[160];
        snprintf(t, sizeof(t), "%.60s%s%.60s", r->col_names[m->series[0]] ? r->col_names[m->series[0]] : "",
                 m->label_col >= 0 ? " by " : "", m->label_col >= 0 && r->col_names[m->label_col] ? r->col_names[m->label_col] : "");
        platform_draw_text_ex(win, t, -1, (Point){area.x + 16, top}, text, fs, FONT_UI_BOLD);
        top += th + 10;
    }
    double lo, hi, step;
    chart_ticks(vmin, vmax, 5, &lo, &hi, &step);
    // spazio per le etichette dell'asse Y
    int ylw = 0;
    char buf[64];
    for (double v = lo; v <= hi + step / 2; v += step) {
        chart_fmt(v, step, buf, sizeof(buf));
        int w = platform_measure_text(win, buf, fs - 1);
        if (w > ylw) ylw = w;
    }
    Rect plot = { area.x + 16 + ylw + 8, top + 4, 0, 0 };
    plot.w = area.x + area.w - 20 - plot.x;
    plot.h = area.y + area.h - th - 18 - plot.y;
    if (plot.w < 40 || plot.h < 40) return;
    #define YPOS(v) (plot.y + plot.h - (int)lround(((v) - lo) / (hi - lo) * plot.h))
    // griglia orizzontale e tacche
    for (double v = lo; v <= hi + step / 2; v += step) {
        int y = YPOS(v);
        platform_fill_rect(win, (Rect){plot.x, y, plot.w, 1}, fabs(v) < step / 2 ? TH_BORDER_STRONG : ui_alpha(grid, 140));
        chart_fmt(v, step, buf, sizeof(buf));
        int w = platform_measure_text(win, buf, fs - 1);
        platform_draw_text(win, buf, (Point){plot.x - 8 - w, y - th / 2}, faint, fs - 1);
    }
    int zero = YPOS(lo > 0 ? lo : hi < 0 ? hi : 0);
    double slot = (double)plot.w / nrows;
    // elemento sotto il mouse
    int hover = -1;
    if (mx >= plot.x && mx < plot.x + plot.w && my >= plot.y - 6 && my <= plot.y + plot.h + 6) {
        hover = (int)((mx - plot.x) / slot);
        if (hover >= nrows) hover = nrows - 1;
    }
    platform_set_clip(win, (Rect){plot.x - 4, plot.y - 4, plot.w + 8, plot.h + 8});
    if (hover >= 0) platform_fill_rect(win, (Rect){plot.x + (int)(hover * slot), plot.y, (int)ceil(slot), plot.h}, ui_alpha(TH_HOVER, 160));
    if (!m->line) {
        // barre raggruppate: sottili, 2 px fra le barre, estremità arrotondate
        double group = slot * 0.72;
        double bw = group / m->nseries;
        for (int i = 0; i < nrows; i++)
            for (int s = 0; s < m->nseries; s++) {
                bool ok;
                double v = cell_value(r, row_at(rows, i), m->series[s], &ok);
                if (!ok) continue;
                int x0 = plot.x + (int)(i * slot + (slot - group) / 2 + s * bw);
                int w = (int)bw - (m->nseries > 1 || slot > 6 ? 2 : 0);
                if (w < 1) w = 1;
                int y = YPOS(v);
                Rect b = v >= 0 ? (Rect){x0, y, w, zero - y} : (Rect){x0, zero, w, y - zero};
                if (b.h < 1) b.h = 1;
                Color c = series_color(s, dark);
                if (hover >= 0 && hover != i) c = ui_alpha(c, 150);
                if (b.h > 6 && w > 6) {
                    platform_fill_rect_rounded(win, b, c, 3);
                    // estremità dalla parte dello zero: squadrata
                    platform_fill_rect(win, v >= 0 ? (Rect){b.x, b.y + b.h - 3, b.w, 3} : (Rect){b.x, b.y, b.w, 3}, c);
                } else platform_fill_rect(win, b, c);
            }
    } else {
        for (int s = 0; s < m->nseries; s++) {
            Color c = series_color(s, dark);
            Point prev = {0, 0};
            bool have = false;
            for (int i = 0; i < nrows; i++) {
                bool ok;
                double v = cell_value(r, row_at(rows, i), m->series[s], &ok);
                if (!ok) { have = false; continue; }           // NULL: la linea si interrompe
                Point p = { plot.x + (int)(i * slot + slot / 2), YPOS(v) };
                if (have) platform_draw_line(win, prev, p, c, 2);
                prev = p;
                have = true;
            }
            if (hover >= 0) {
                bool ok;
                double v = cell_value(r, row_at(rows, hover), m->series[s], &ok);
                if (ok) {
                    Point p = { plot.x + (int)(hover * slot + slot / 2), YPOS(v) };
                    platform_fill_rect_rounded(win, (Rect){p.x - 6, p.y - 6, 12, 12}, TH_SURFACE, 6);   // anello del colore della superficie
                    platform_fill_rect_rounded(win, (Rect){p.x - 4, p.y - 4, 8, 8}, c, 4);
                }
            }
        }
    }
    platform_reset_clip(win);
    // etichette dell'asse X: solo quelle che ci stanno
    int maxlw = 0;
    char lab[80];
    for (int i = 0; i < nrows && i < 400; i++) {
        label_of(r, rows, i, m->label_col, lab, sizeof(lab));
        int w = platform_measure_text(win, lab, fs - 1);
        if (w > maxlw) maxlw = w;
    }
    if (maxlw > 120) maxlw = 120;
    int every = (int)ceil((maxlw + 12) / slot);
    if (every < 1) every = 1;
    for (int i = 0; i < nrows; i += every) {
        label_of(r, rows, i, m->label_col, lab, sizeof(lab));
        fit_text(win, lab, (int)(slot * every) - 8 > 20 ? (int)(slot * every) - 8 : 20, fs - 1);
        int w = platform_measure_text(win, lab, fs - 1);
        int cx = plot.x + (int)(i * slot + slot / 2) - w / 2;
        if (cx < plot.x - 10) cx = plot.x - 10;
        platform_draw_text(win, lab, (Point){cx, plot.y + plot.h + 6}, faint, fs - 1);
    }
    // valore sotto il mouse
    if (hover >= 0) {
        char lines[CHART_MAX_SERIES + 1][120];
        int nl = 0, tw = 0;
        label_of(r, rows, hover, m->label_col, lab, sizeof(lab));
        snprintf(lines[nl++], sizeof(lines[0]), "%s", lab);
        for (int s = 0; s < m->nseries; s++) {
            const Row* rw = &r->rows[row_at(rows, hover)];
            int c = m->series[s];
            const char* v = c < rw->num_cells && !rw->cells[c].is_null ? rw->cells[c].value : "NULL";
            snprintf(lines[nl++], sizeof(lines[0]), "%.50s: %.50s", r->col_names[c] ? r->col_names[c] : "", v);
        }
        for (int k = 0; k < nl; k++) { int w = platform_measure_text(win, lines[k], fs) + (k ? 16 : 0); if (w > tw) tw = w; }
        int bw = tw + 20, bh = nl * (th + 4) + 12;
        int bx = mx + 14, by = my - bh / 2;
        if (bx + bw > area.x + area.w - 4) bx = mx - 14 - bw;
        if (by < area.y + 4) by = area.y + 4;
        if (by + bh > area.y + area.h - 4) by = area.y + area.h - 4 - bh;
        platform_fill_rect_rounded(win, (Rect){bx, by, bw, bh}, TH_ELEVATED, 6);
        platform_draw_rect(win, (Rect){bx, by, bw, bh}, TH_BORDER_STRONG, 1);
        for (int k = 0; k < nl; k++) {
            int ty = by + 6 + k * (th + 4);
            if (k) platform_fill_rect_rounded(win, (Rect){bx + 10, ty + th / 2 - 4, 8, 8}, series_color(k - 1, dark), 2);
            platform_draw_text_ex(win, lines[k], -1, (Point){bx + 10 + (k ? 16 : 0), ty}, k ? text : muted, fs, k ? FONT_UI : FONT_UI_BOLD);
        }
    }
    (void)muted;
    #undef YPOS
}
