// Encoder PNG: il flusso deflate si decomprime e ridà i dati; chunk e CRC del file sono corretti.
#include "src/widgets/png_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_fail;
#define CHECK(cond, ...) do { g_tests++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── decompressore minimo (solo blocchi con codici fissi, come quelli scritti) ──
typedef struct { const uint8_t* p; size_t n, pos; int bit; } In;
static int bit(In* s) { if (s->pos >= s->n) return -1; int b = (s->p[s->pos] >> s->bit) & 1; if (++s->bit == 8) { s->bit = 0; s->pos++; } return b; }
static int bits(In* s, int n) { int v = 0; for (int i = 0; i < n; i++) { int b = bit(s); if (b < 0) return -1; v |= b << i; } return v; }
static int code(In* s, int n) { int v = 0; for (int i = 0; i < n; i++) { int b = bit(s); if (b < 0) return -1; v = v << 1 | b; } return v; }
static int litlen(In* s) {
    int c = code(s, 7);
    if (c < 0) return -1;
    if (c <= 23) return 256 + c;
    c = c << 1 | bit(s);
    if (c >= 0x30 && c <= 0xBF) return c - 0x30;
    if (c >= 0xC0 && c <= 0xC7) return 280 + c - 0xC0;
    c = c << 1 | bit(s);
    return c >= 0x190 && c <= 0x1FF ? 144 + c - 0x190 : -1;
}
static const int LB[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const int LE[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const int DB[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const int DE[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static uint8_t* inflate_fixed(const uint8_t* z, size_t zn, size_t* out_n) {
    if (zn < 6 || z[0] != 0x78 || ((z[0] << 8 | z[1]) % 31)) return NULL;
    In s = { z + 2, zn - 6, 0, 0 };
    if (bits(&s, 1) != 1 || bits(&s, 2) != 1) return NULL;
    size_t cap = 1024, n = 0;
    uint8_t* o = malloc(cap);
    for (;;) {
        int sym = litlen(&s);
        if (sym < 0 || sym > 285) { free(o); return NULL; }
        if (sym == 256) break;
        if (n + 260 > cap) { cap *= 2; o = realloc(o, cap); }
        if (sym < 256) { o[n++] = (uint8_t)sym; continue; }
        int li = sym - 257, len = LB[li] + bits(&s, LE[li]);
        int dc = code(&s, 5);
        if (dc < 0 || dc > 29) { free(o); return NULL; }
        int dist = DB[dc] + bits(&s, DE[dc]);
        if ((size_t)dist > n) { free(o); return NULL; }
        for (int k = 0; k < len; k++) { o[n] = o[n - (size_t)dist]; n++; }
    }
    // Adler-32 in coda
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + o[i]) % 65521; b = (b + a) % 65521; }
    const uint8_t* t = z + zn - 4;
    if (((uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3]) != (b << 16 | a)) { free(o); return NULL; }
    *out_n = n;
    return o;
}

static bool roundtrip(const uint8_t* data, size_t n, size_t* zsize) {
    size_t zn = 0, on = 0;
    uint8_t* z = png_zlib_compress(data, n, &zn);
    uint8_t* o = z ? inflate_fixed(z, zn, &on) : NULL;
    bool ok = o && on == n && (n == 0 || !memcmp(o, data, n));
    if (zsize) *zsize = zn;
    free(z);
    free(o);
    return ok;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int main(void) {
    CHECK(png_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "crc32 check value");
    CHECK(roundtrip((const uint8_t*)"", 0, NULL), "empty");
    const char* txt = "abracadabra abracadabra abracadabra, the quick brown fox";
    CHECK(roundtrip((const uint8_t*)txt, strlen(txt), NULL), "text");
    // zona uniforme (come uno sfondo): si comprime molto
    size_t n = 300000, zn = 0;
    uint8_t* d = malloc(n);
    memset(d, 0x2A, n);
    CHECK(roundtrip(d, n, &zn) && zn < n / 100, "uniform: %llu -> %llu", (unsigned long long)n, (unsigned long long)zn);
    // dati pseudo-casuali: corretti anche se non si comprimono
    uint32_t x = 12345;
    for (size_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; d[i] = (uint8_t)(x >> 16); }
    CHECK(roundtrip(d, n, NULL), "random");
    // misto: righe ripetute a distanza lunga (oltre i 258 byte)
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)((i % 7000) < 3500 ? (i % 13) : 200);
    CHECK(roundtrip(d, n, &zn) && zn < n / 10, "repeated rows: %llu", (unsigned long long)zn);
    free(d);

    // file PNG: firma, IHDR, CRC di ogni chunk, dati decompressi = righe con filtro 0
    int w = 37, h = 11;
    uint32_t* px = malloc(sizeof(uint32_t) * (size_t)(w * h));
    for (int i = 0; i < w * h; i++) px[i] = (uint32_t)(i * 2654435761u) & 0xFFFFFF;
    size_t len = 0;
    uint8_t* png = png_encode(w, h, px, &len);
    CHECK(png && len > 57 && !memcmp(png, "\x89PNG\r\n\x1a\n", 8), "signature");
    bool crc_ok = true, saw_end = false;
    uint8_t* idat = NULL;
    size_t idat_n = 0;
    for (size_t p = 8; png && p + 12 <= len; ) {
        uint32_t cl = rd32(png + p);
        if (rd32(png + p + 8 + cl) != png_crc32(png + p + 4, cl + 4)) crc_ok = false;
        if (!memcmp(png + p + 4, "IHDR", 4)) CHECK(rd32(png + p + 8) == (uint32_t)w && rd32(png + p + 12) == (uint32_t)h && png[p + 16] == 8 && png[p + 17] == 2, "ihdr");
        if (!memcmp(png + p + 4, "IDAT", 4)) { idat = png + p + 8; idat_n = cl; }
        if (!memcmp(png + p + 4, "IEND", 4)) saw_end = true;
        p += 12 + cl;
    }
    CHECK(crc_ok && saw_end && idat, "chunks");
    size_t rn = 0;
    uint8_t* raw = idat ? inflate_fixed(idat, idat_n, &rn) : NULL;
    bool pix_ok = raw && rn == (size_t)h * (size_t)(w * 3 + 1);
    for (int y = 0; pix_ok && y < h; y++) {
        const uint8_t* r = raw + (size_t)y * (size_t)(w * 3 + 1);
        if (r[0] != 0) pix_ok = false;
        for (int xx = 0; xx < w && pix_ok; xx++) {
            uint32_t c = px[y * w + xx];
            pix_ok = r[1 + xx * 3] == (uint8_t)(c >> 16) && r[2 + xx * 3] == (uint8_t)(c >> 8) && r[3 + xx * 3] == (uint8_t)c;
        }
    }
    CHECK(pix_ok, "pixels");
    free(raw);
    free(png);
    CHECK(png_write_file("/tmp/sv_png_test.png", w, h, px), "write file");
    CHECK(!png_write_file("/nonexistent_dir/x.png", w, h, px), "unwritable path");
    remove("/tmp/sv_png_test.png");
    free(px);

    if (g_fail) { printf("png tests: %d of %d FAILED\n", g_fail, g_tests); return 1; }
    printf("OK: %d/%d png checks passed\n", g_tests, g_tests);
    return 0;
}
