#include "export_bin.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint8_t* p; size_t n, cap; bool oom; } Bin;

static void bput(Bin* b, const void* d, size_t n) {
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + n + 1) nc *= 2;
        uint8_t* q = realloc(b->p, nc);
        if (!q) { b->oom = true; return; }
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(Bin* b, const char* s) { bput(b, s, strlen(s)); }
static void bu16(Bin* b, unsigned v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; bput(b, x, 2); }
static void bu32(Bin* b, uint32_t v) { uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; bput(b, x, 4); }
static void bfmt(Bin* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { bput(b, tmp, (size_t)n); return; }
    char* big = malloc((size_t)n + 1);                    // testo lungo: si riformatta per intero
    if (!big) { b->oom = true; return; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    bput(b, big, (size_t)n);
    free(big);
}

uint32_t export_crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int row_at(const int* rows, int i) { return rows ? rows[i] : i; }

// numero che Excel / PDF possono mostrare così com'è
static bool plain_number(const Cell* c) {
    if (c->is_null || (c->type != CELL_TYPE_INT && c->type != CELL_TYPE_FLOAT)) return false;
    const char* s = c->value;
    if (*s == '-') s++;
    if (!(*s >= '0' && *s <= '9')) return false;
    bool dot = false, exp = false;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') continue;
        if (*s == '.' && !dot && !exp) { dot = true; continue; }
        if ((*s == 'e' || *s == 'E') && !exp) { exp = true; if (s[1] == '-' || s[1] == '+') s++; continue; }
        return false;
    }
    return true;
}

static int utf8_len(const char* s) { int n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }

// ── XLSX ─────────────────────────────────────────────────────────

static void xml_text(Bin* b, const char* s, size_t max) {
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && n < max; p++, n++) {
        switch (*p) {
            case '&': bstr(b, "&amp;"); break;
            case '<': bstr(b, "&lt;"); break;
            case '>': bstr(b, "&gt;"); break;
            case '"': bstr(b, "&quot;"); break;
            default:
                if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r') bstr(b, " ");   // non ammessi in XML
                else bput(b, p, 1);
        }
    }
}

static void col_ref(char* out, int c, int r) {
    char tmp[8];
    int k = 0;
    for (c++; c > 0; c = (c - 1) / 26) tmp[k++] = (char)('A' + (c - 1) % 26);
    int j = 0;
    while (k) out[j++] = tmp[--k];
    sprintf(out + j, "%d", r);
}

typedef struct { const char* name; Bin data; } ZFile;

static void zip_write(Bin* out, ZFile* f, int nf) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    unsigned dtime = tm ? (unsigned)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2) : 0;
    unsigned ddate = tm ? (unsigned)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday) : 0x21;
    uint32_t offs[16], crc[16];
    for (int i = 0; i < nf; i++) {
        offs[i] = (uint32_t)out->n;
        crc[i] = export_crc32(f[i].data.p, f[i].data.n);
        bu32(out, 0x04034b50); bu16(out, 20); bu16(out, 0x0800); bu16(out, 0);       // UTF-8, nessuna compressione
        bu16(out, dtime); bu16(out, ddate);
        bu32(out, crc[i]); bu32(out, (uint32_t)f[i].data.n); bu32(out, (uint32_t)f[i].data.n);
        bu16(out, (unsigned)strlen(f[i].name)); bu16(out, 0);
        bstr(out, f[i].name);
        bput(out, f[i].data.p, f[i].data.n);
    }
    uint32_t cd = (uint32_t)out->n;
    for (int i = 0; i < nf; i++) {
        bu32(out, 0x02014b50); bu16(out, 20); bu16(out, 20); bu16(out, 0x0800); bu16(out, 0);
        bu16(out, dtime); bu16(out, ddate);
        bu32(out, crc[i]); bu32(out, (uint32_t)f[i].data.n); bu32(out, (uint32_t)f[i].data.n);
        bu16(out, (unsigned)strlen(f[i].name)); bu16(out, 0); bu16(out, 0); bu16(out, 0); bu16(out, 0);
        bu32(out, 0); bu32(out, offs[i]);
        bstr(out, f[i].name);
    }
    uint32_t cdlen = (uint32_t)out->n - cd;
    bu32(out, 0x06054b50); bu16(out, 0); bu16(out, 0); bu16(out, (unsigned)nf); bu16(out, (unsigned)nf);
    bu32(out, cdlen); bu32(out, cd); bu16(out, 0);
}

uint8_t* export_xlsx(const QueryResult* r, const int* rows, int nrows, size_t* len) {
    if (!r || r->num_cols <= 0) return NULL;
    int nc = r->num_cols;
    static const char* X = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    static const char* MAIN = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    static const char* RELNS = "http://schemas.openxmlformats.org/package/2006/relationships";
    static const char* ODREL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    ZFile f[6] = { { "[Content_Types].xml", {0} }, { "_rels/.rels", {0} }, { "xl/workbook.xml", {0} },
                   { "xl/_rels/workbook.xml.rels", {0} }, { "xl/styles.xml", {0} }, { "xl/worksheets/sheet1.xml", {0} } };
    bstr(&f[0].data, X);
    bstr(&f[0].data, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>");
    bstr(&f[1].data, X);
    bfmt(&f[1].data, "<Relationships xmlns=\"%s\"><Relationship Id=\"rId1\" Type=\"%s/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>", RELNS, ODREL);
    bstr(&f[2].data, X);
    bfmt(&f[2].data, "<workbook xmlns=\"%s\" xmlns:r=\"%s\"><sheets><sheet name=\"Results\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>", MAIN, ODREL);
    bstr(&f[3].data, X);
    bfmt(&f[3].data, "<Relationships xmlns=\"%s\"><Relationship Id=\"rId1\" Type=\"%s/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
                     "<Relationship Id=\"rId2\" Type=\"%s/styles\" Target=\"styles.xml\"/></Relationships>", RELNS, ODREL, ODREL);
    bstr(&f[4].data, X);
    bfmt(&f[4].data, "<styleSheet xmlns=\"%s\"><fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
        "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>", MAIN);
    Bin* s = &f[5].data;
    bstr(s, X);
    bfmt(s, "<worksheet xmlns=\"%s\"><sheetViews><sheetView workbookViewId=\"0\">"
            "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews><cols>", MAIN);
    for (int c = 0; c < nc; c++) {                         // larghezza: il contenuto più lungo (fino a 60)
        int w = r->col_names[c] ? utf8_len(r->col_names[c]) : 4;
        for (int i = 0; i < nrows && i < 1000; i++) {
            const Row* row = &r->rows[row_at(rows, i)];
            if (c < row->num_cells && !row->cells[c].is_null) { int l = utf8_len(row->cells[c].value); if (l > w) w = l; }
        }
        if (w > 60) w = 60;
        bfmt(s, "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>", c + 1, c + 1, w + 2);
    }
    bstr(s, "</cols><sheetData><row r=\"1\">");
    char ref[16];
    for (int c = 0; c < nc; c++) {
        col_ref(ref, c, 1);
        bfmt(s, "<c r=\"%s\" t=\"inlineStr\" s=\"1\"><is><t>", ref);
        xml_text(s, r->col_names[c] ? r->col_names[c] : "", 32767);
        bstr(s, "</t></is></c>");
    }
    bstr(s, "</row>");
    for (int i = 0; i < nrows && !s->oom; i++) {
        const Row* row = &r->rows[row_at(rows, i)];
        bfmt(s, "<row r=\"%d\">", i + 2);
        for (int c = 0; c < nc && c < row->num_cells; c++) {
            const Cell* cell = &row->cells[c];
            if (cell->is_null) continue;                   // NULL = cella vuota
            col_ref(ref, c, i + 2);
            if (plain_number(cell)) bfmt(s, "<c r=\"%s\"><v>%.60s</v></c>", ref, cell->value);
            else {
                const char* v = cell->value;
                size_t vl = strlen(v);
                bool sp = vl && (v[0] == ' ' || v[vl - 1] == ' ' || strchr(v, '\n'));
                bfmt(s, "<c r=\"%s\" t=\"inlineStr\"><is><t%s>", ref, sp ? " xml:space=\"preserve\"" : "");
                xml_text(s, v, 32767);                     // limite di una cella di Excel
                bstr(s, "</t></is></c>");
            }
        }
        bstr(s, "</row>");
    }
    bstr(s, "</sheetData></worksheet>");
    Bin out = {0};
    bool oom = false;
    for (int i = 0; i < 6; i++) oom |= f[i].data.oom;
    if (!oom) zip_write(&out, f, 6);
    for (int i = 0; i < 6; i++) free(f[i].data.p);
    if (oom || out.oom) { free(out.p); return NULL; }
    *len = out.n;
    return out.p;
}

// ── PDF ──────────────────────────────────────────────────────────

// UTF-8 -> WinAnsi (Latin-1 per i caratteri comuni), al massimo max caratteri; '~' se tagliato
static void pdf_cell(char* out, size_t on, const char* s, int max, int* used) {
    size_t k = 0;
    int n = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && k + 3 < on) {
        unsigned cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = (unsigned)(*p & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = (unsigned)(*p & 0x0F) << 12 | (unsigned)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else { cp = '?'; p++; while ((*p & 0xC0) == 0x80) p++; }
        if (n == max - 1 && *p) { out[k++] = '~'; n++; break; }
        if (n >= max) break;
        if (cp == 0x20AC) cp = 0x80;                      // euro in WinAnsi
        else if (cp < 0x20 || (cp > 0x7E && cp < 0xA0) || cp > 0xFF) cp = cp < 0x20 ? ' ' : '?';
        if (cp == '(' || cp == ')' || cp == '\\') out[k++] = '\\';
        out[k++] = (char)cp;
        n++;
    }
    out[k] = '\0';
    *used = n;
}

uint8_t* export_pdf(const QueryResult* r, const int* rows, int nrows, const char* title, size_t* len) {
    if (!r || r->num_cols <= 0) return NULL;
    int nc = r->num_cols;
    const double PW = 842, PH = 595, M = 36;              // A4 orizzontale, margini di mezzo pollice
    int* w = calloc((size_t)nc, sizeof(int));
    bool* num = calloc((size_t)nc, sizeof(bool));
    if (!w || !num) { free(w); free(num); return NULL; }
    for (int c = 0; c < nc; c++) {
        w[c] = r->col_names[c] ? utf8_len(r->col_names[c]) : 4;
        bool all_num = true;
        for (int i = 0; i < nrows && i < 2000; i++) {
            const Row* row = &r->rows[row_at(rows, i)];
            if (c >= row->num_cells || row->cells[c].is_null) continue;
            int l = utf8_len(row->cells[c].value);
            if (l > w[c]) w[c] = l;
            if (!plain_number(&row->cells[c])) all_num = false;
        }
        num[c] = all_num;
        if (w[c] > 40) w[c] = 40;
        if (w[c] < 4) w[c] = 4;
    }
    // corpo del carattere: tutto in una pagina di larghezza, da 9 a 5 punti; poi si tolgono colonne
    int shown = nc, total = 0;
    for (int c = 0; c < nc; c++) total += w[c] + (c ? 2 : 0);
    double fs = (PW - 2 * M) / (0.6 * total);
    if (fs > 9) fs = 9;
    if (fs < 5) {
        fs = 5;
        int maxch = (int)((PW - 2 * M) / (0.6 * fs)), acc = 0;
        for (shown = 0; shown < nc && acc + w[shown] + (shown ? 2 : 0) <= maxch; shown++) acc += w[shown] + (shown ? 2 : 0);
        if (shown == 0) { shown = 1; w[0] = maxch; }
    }
    double lh = fs * 1.35, cw = 0.6 * fs;
    int per_page = (int)((PH - 2 * M - 30 - 2 * lh) / lh);
    if (per_page < 1) per_page = 1;
    int pages = nrows ? (nrows + per_page - 1) / per_page : 1;

    Bin out = {0};
    int nobj = 4 + 2 * pages;
    long* off = calloc((size_t)nobj + 1, sizeof(long));
    if (!off) { free(w); free(num); return NULL; }
    bstr(&out, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");
    off[1] = (long)out.n; bstr(&out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = (long)out.n; bfmt(&out, "2 0 obj\n<< /Type /Pages /Count %d /Kids [", pages);
    for (int p = 0; p < pages; p++) bfmt(&out, "%d 0 R ", 5 + 2 * p);
    bstr(&out, "] >>\nendobj\n");
    off[3] = (long)out.n; bstr(&out, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>\nendobj\n");
    off[4] = (long)out.n; bstr(&out, "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier-Bold /Encoding /WinAnsiEncoding >>\nendobj\n");
    char cell[600], head[300];
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    char when[32] = "";
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    for (int p = 0; p < pages && !out.oom; p++) {
        Bin cs = {0};
        int u;
        double y = PH - M - 11;
        pdf_cell(head, sizeof(head), title && title[0] ? title : "SegVault export", 80, &u);
        bfmt(&cs, "BT /F2 11 Tf 1 0 0 1 %.1f %.1f Tm (%s) Tj ET\n", M, y, head);
        snprintf(cell, sizeof(cell), "%d row%s, %s%s", nrows, nrows == 1 ? "" : "s", when,
                 shown < nc ? "  (some columns do not fit the page: use XLSX or CSV)" : "");
        bfmt(&cs, "BT /F1 8 Tf 0.4 g 1 0 0 1 %.1f %.1f Tm (%s) Tj ET 0 g\n", M, y - 13, cell);
        y -= 30;
        double x = M;
        bfmt(&cs, "BT /F2 %.2f Tf\n", fs);                 // intestazione
        for (int c = 0; c < shown; c++) {
            pdf_cell(cell, sizeof(cell), r->col_names[c] ? r->col_names[c] : "", w[c], &u);
            double cx = num[c] ? x + (w[c] - u) * cw : x;
            bfmt(&cs, "1 0 0 1 %.2f %.2f Tm (%s) Tj\n", cx, y, cell);
            x += (w[c] + 2) * cw;
        }
        bstr(&cs, "ET\n");
        bfmt(&cs, "0.5 w 0.6 G %.1f %.2f m %.1f %.2f l S 0 G\n", M, y - lh * 0.35, PW - M, y - lh * 0.35);
        y -= lh * 1.2;
        bfmt(&cs, "BT /F1 %.2f Tf\n", fs);
        for (int i = p * per_page; i < nrows && i < (p + 1) * per_page; i++) {
            const Row* row = &r->rows[row_at(rows, i)];
            x = M;
            for (int c = 0; c < shown; c++) {
                const Cell* ce = c < row->num_cells ? &row->cells[c] : NULL;
                if (ce && !ce->is_null) {
                    pdf_cell(cell, sizeof(cell), ce->value, w[c], &u);
                    double cx = num[c] ? x + (w[c] - u) * cw : x;
                    bfmt(&cs, "1 0 0 1 %.2f %.2f Tm (%s) Tj\n", cx, y, cell);
                } else if (ce) {
                    bfmt(&cs, "0.6 g 1 0 0 1 %.2f %.2f Tm (NULL) Tj 0 g\n", num[c] && w[c] > 4 ? x + (w[c] - 4) * cw : x, y);
                }
                x += (w[c] + 2) * cw;
            }
            y -= lh;
        }
        bstr(&cs, "ET\n");
        bfmt(&cs, "BT /F1 8 Tf 0.4 g 1 0 0 1 %.1f %.1f Tm (%d / %d) Tj ET\n", PW - M - 40, M - 14, p + 1, pages);
        off[5 + 2 * p] = (long)out.n;
        bfmt(&out, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.0f %.0f] "
                   "/Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents %d 0 R >>\nendobj\n", 5 + 2 * p, PW, PH, 6 + 2 * p);
        off[6 + 2 * p] = (long)out.n;
        bfmt(&out, "%d 0 obj\n<< /Length %lu >>\nstream\n", 6 + 2 * p, (unsigned long)cs.n);
        bput(&out, cs.p, cs.n);
        bstr(&out, "\nendstream\nendobj\n");
        if (cs.oom) out.oom = true;
        free(cs.p);
    }
    long xref = (long)out.n;
    bfmt(&out, "xref\n0 %d\n0000000000 65535 f \n", nobj + 1);
    for (int i = 1; i <= nobj; i++) bfmt(&out, "%010ld 00000 n \n", off[i]);
    bfmt(&out, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", nobj + 1, xref);
    free(off); free(w); free(num);
    if (out.oom) { free(out.p); return NULL; }
    *len = out.n;
    return out.p;
}
