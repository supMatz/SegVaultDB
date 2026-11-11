// Export XLSX e PDF: struttura dei file (zip con CRC giusti, XML, PDF con xref corretta).
// Con SV_EXPORT_KEEP=cartella i file restano lì per controllarli con altri programmi.
#include "src/widgets/export_bin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned rd16(const uint8_t* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static bool has(const uint8_t* b, size_t n, const char* s) {
    size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; i++) if (!memcmp(b + i, s, l)) return true;
    return false;
}

int main(void) {
    // risultato di prova: numeri, testo con caratteri speciali, NULL, molte righe (più pagine PDF)
    QueryResult r;
    memset(&r, 0, sizeof(r));
    r.num_cols = 3;
    char* names[3] = { "id", "nome", "prezzo" };
    r.col_names = names;
    int n = 120;
    r.num_rows = n;
    r.rows = calloc((size_t)n, sizeof(Row));
    for (int i = 0; i < n; i++) {
        char a[16], c[32], b[64];
        snprintf(a, sizeof(a), "%d", i + 1);
        snprintf(b, sizeof(b), i == 0 ? "Caff\xc3\xa8 & <latte> (\"bio\") \xe2\x82\xac" : "prodotto %d", i);
        snprintf(c, sizeof(c), "%d.5", i);
        const char* v[3] = { a, b, i == 2 ? NULL : c };
        CellType t[3] = { CELL_TYPE_INT, CELL_TYPE_STRING, CELL_TYPE_FLOAT };
        db_row_set(&r.rows[i], 3, v, t);
    }
    int order[3] = { 2, 0, 1 };

    size_t xl = 0;
    uint8_t* x = export_xlsx(&r, NULL, n, &xl);
    CHECK(x && xl > 100 && rd32(x) == 0x04034b50, "xlsx is a zip");
    // fine dell'archivio: 6 file, e ogni file locale ha il CRC giusto
    size_t eo = xl - 22;
    CHECK(x && rd32(x + eo) == 0x06054b50 && rd16(x + eo + 10) == 6, "6 files in the zip");
    size_t pos = 0;
    int files = 0;
    bool crc_ok = true, sheet = false;
    while (x && pos + 30 < xl && rd32(x + pos) == 0x04034b50) {
        uint32_t crc = rd32(x + pos + 14), sz = rd32(x + pos + 18);
        unsigned nl = rd16(x + pos + 26);
        const uint8_t* data = x + pos + 30 + nl;
        if (export_crc32(data, sz) != crc) crc_ok = false;
        static const char* const ROOTS[] = { "</Types>", "</Relationships>", "</workbook>", "</styleSheet>", "</worksheet>" };
        bool closed = false;
        for (int k = 0; k < 5; k++) { size_t rl = strlen(ROOTS[k]); if (sz >= rl && !memcmp(data + sz - rl, ROOTS[k], rl)) closed = true; }
        if (!closed) { crc_ok = false; printf("  part not complete: %.*s\n", (int)nl, (const char*)x + pos + 30); }
        if (nl == 24 && !memcmp(x + pos + 30, "xl/worksheets/sheet1.xml", 24)) {
            sheet = has(data, sz, "<c r=\"A2\"><v>1</v></c>") && has(data, sz, "Caff\xc3\xa8 &amp; &lt;latte&gt; (&quot;bio&quot;)") &&
                    has(data, sz, "<c r=\"C2\"><v>0.5</v></c>") && !has(data, sz, "r=\"C4\"") && has(data, sz, "s=\"1\"");
        }
        pos += 30 + nl + sz;
        files++;
    }
    CHECK(files == 6 && crc_ok, "local files and CRC: %d files", files);
    CHECK(sheet, "sheet content: numbers, escaped text, NULL as empty cell, bold header");

    size_t pl = 0;
    uint8_t* p = export_pdf(&r, NULL, n, "SELECT * FROM prodotto", &pl);
    CHECK(p && pl > 200 && !memcmp(p, "%PDF-1.4", 8) && has(p, pl, "%%EOF"), "pdf header and trailer");
    // ogni voce della xref punta a "N 0 obj"
    const char* sx = p ? strstr((char*)p + pl - 40, "startxref") : NULL;
    long xref = sx ? atol(sx + 10) : -1;
    bool xref_ok = xref > 0 && !memcmp(p + xref, "xref", 4);
    int nobj = 0;
    if (xref_ok) {
        sscanf((char*)p + xref, "xref\n0 %d", &nobj);
        const char* e = strchr((char*)p + xref + 5, '\n') + 1 + 20;    // dopo la voce 0
        for (int i = 1; i < nobj && xref_ok; i++, e += 20) {
            long o = atol(e);
            char want[32];
            snprintf(want, sizeof(want), "%d 0 obj", i);
            if (o <= 0 || (size_t)o >= pl || strncmp((char*)p + o, want, strlen(want))) xref_ok = false;
        }
    }
    CHECK(xref_ok && nobj > 6, "pdf xref offsets (%d objects)", nobj);
    CHECK(p && has(p, pl, "/Count 4"), "120 rows: 4 pages");
    CHECK(p && has(p, pl, "Caff\xe8 & <latte> \\(\"bio\"\\) \x80"), "WinAnsi text with escaped parentheses and euro");

    uint8_t* p2 = export_pdf(&r, order, 3, "", &pl);
    CHECK(p2 && has(p2, pl, "/Count 1") && has(p2, pl, "(NULL)"), "row order and NULL");

    const char* keep = getenv("SV_EXPORT_KEEP");
    if (keep && x && p) {
        char f[600];
        snprintf(f, sizeof(f), "%s/t.xlsx", keep); FILE* o = fopen(f, "wb"); if (o) { fwrite(x, 1, xl, o); fclose(o); }
        snprintf(f, sizeof(f), "%s/t.pdf", keep); o = fopen(f, "wb"); if (o) { size_t l2; uint8_t* q = export_pdf(&r, NULL, n, "SELECT * FROM prodotto", &l2); fwrite(q, 1, l2, o); fclose(o); free(q); }
    }
    free(x); free(p); free(p2);
    for (int i = 0; i < n; i++) free(r.rows[i].cells);
    free(r.rows);
    printf("%s: %d/%d export checks passed\n", g_fail ? "FAILED" : "OK", g_tests - g_fail, g_tests);
    return g_fail ? 1 : 0;
}
