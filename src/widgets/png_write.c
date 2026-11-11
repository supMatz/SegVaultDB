#include "png_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── CRC-32 (PNG) e Adler-32 (zlib) ──────────────────────────────

uint32_t png_crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return b << 16 | a;
}

// ── flusso di bit (deflate: bit meno significativi per primi) ──

typedef struct { uint8_t* p; size_t n, cap; uint32_t acc; int nbits; bool oom; } Bits;

static void put_byte(Bits* b, uint8_t v) {
    if (b->oom) return;
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        uint8_t* p = realloc(b->p, cap);
        if (!p) { b->oom = true; return; }
        b->p = p;
        b->cap = cap;
    }
    b->p[b->n++] = v;
}

static void put_bits(Bits* b, uint32_t v, int n) {     // n <= 16
    b->acc |= v << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) { put_byte(b, (uint8_t)b->acc); b->acc >>= 8; b->nbits -= 8; }
}

// i codici di Huffman si scrivono dal bit più significativo
static void put_code(Bits* b, uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
    put_bits(b, r, len);
}

// codice fisso di un simbolo letterale / lunghezza (0..287)
static void put_litlen(Bits* b, int sym) {
    if (sym < 144) put_code(b, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256) put_code(b, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280) put_code(b, (uint32_t)(sym - 256), 7);
    else put_code(b, 0xC0 + (uint32_t)(sym - 280), 8);
}

static const uint16_t LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(Bits* b, int len, int dist) {
    int i = 28;
    while (LEN_BASE[i] > len) i--;
    put_litlen(b, 257 + i);
    if (LEN_EXTRA[i]) put_bits(b, (uint32_t)(len - LEN_BASE[i]), LEN_EXTRA[i]);
    int d = 29;
    while (DIST_BASE[d] > dist) d--;
    put_code(b, (uint32_t)d, 5);
    if (DIST_EXTRA[d]) put_bits(b, (uint32_t)(dist - DIST_BASE[d]), DIST_EXTRA[d]);
}

#define WIN      32768
#define HBITS    15
#define HSIZE    (1 << HBITS)
#define MAXCHAIN 24

uint8_t* png_zlib_compress(const uint8_t* data, size_t n, size_t* out_len) {
    Bits b = {0};
    put_byte(&b, 0x78);
    put_byte(&b, 0x01);
    int32_t* head = malloc(sizeof(int32_t) * HSIZE);
    int32_t* prev = malloc(sizeof(int32_t) * WIN);
    if (!head || !prev) { free(head); free(prev); free(b.p); return NULL; }
    for (int i = 0; i < HSIZE; i++) head[i] = -1;
    put_bits(&b, 1, 1);         // ultimo blocco
    put_bits(&b, 1, 2);         // codici fissi
    size_t i = 0;
    while (i < n && !b.oom) {
        int best = 0, bdist = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)data[i] << 10 ^ (uint32_t)data[i + 1] << 5 ^ data[i + 2]) & (HSIZE - 1);
            int32_t cand = head[h];
            size_t maxlen = n - i < 258 ? n - i : 258;
            for (int chain = 0; cand >= 0 && chain < MAXCHAIN && i - (size_t)cand <= WIN - 1; chain++) {
                size_t l = 0;
                const uint8_t *a = data + cand, *c = data + i;
                while (l < maxlen && a[l] == c[l]) l++;
                if ((int)l > best) { best = (int)l; bdist = (int)(i - (size_t)cand); if (l == maxlen) break; }
                int32_t nx = prev[cand & (WIN - 1)];
                if (nx >= cand) break;
                cand = nx;
            }
            prev[i & (WIN - 1)] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= 3) {
            put_match(&b, best, bdist);
            // le posizioni coperte dalla corrispondenza entrano comunque nella tabella
            for (size_t k = i + 1; k < i + (size_t)best && k + 3 <= n; k++) {
                uint32_t h = ((uint32_t)data[k] << 10 ^ (uint32_t)data[k + 1] << 5 ^ data[k + 2]) & (HSIZE - 1);
                prev[k & (WIN - 1)] = head[h];
                head[h] = (int32_t)k;
            }
            i += (size_t)best;
        } else {
            put_litlen(&b, data[i]);
            i++;
        }
    }
    put_litlen(&b, 256);                       // fine del blocco
    if (b.nbits) put_bits(&b, 0, 8 - b.nbits);
    uint32_t ad = adler32(data, n);
    put_byte(&b, (uint8_t)(ad >> 24)); put_byte(&b, (uint8_t)(ad >> 16));
    put_byte(&b, (uint8_t)(ad >> 8));  put_byte(&b, (uint8_t)ad);
    free(head);
    free(prev);
    if (b.oom) { free(b.p); return NULL; }
    *out_len = b.n;
    return b.p;
}

// ── PNG ─────────────────────────────────────────────────────────

static void be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// chunk: lunghezza, tipo, dati, CRC di tipo + dati
static uint8_t* put_chunk(uint8_t* o, const char* type, const uint8_t* data, size_t n) {
    be32(o, (uint32_t)n);
    memcpy(o + 4, type, 4);
    if (n) memcpy(o + 8, data, n);
    be32(o + 8 + n, png_crc32(o + 4, n + 4));
    return o + 12 + n;
}

uint8_t* png_encode(int w, int h, const uint32_t* px, size_t* len) {
    if (w <= 0 || h <= 0 || !px) return NULL;
    size_t stride = (size_t)w * 3 + 1, rawn = stride * (size_t)h;
    uint8_t* raw = malloc(rawn);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        uint8_t* r = raw + stride * (size_t)y;
        r[0] = 0;                                   // filtro: nessuno
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * (size_t)w + (size_t)x];
            r[1 + x * 3] = (uint8_t)(c >> 16);
            r[2 + x * 3] = (uint8_t)(c >> 8);
            r[3 + x * 3] = (uint8_t)c;
        }
    }
    size_t zn = 0;
    uint8_t* z = png_zlib_compress(raw, rawn, &zn);
    free(raw);
    if (!z) return NULL;
    size_t total = 8 + (12 + 13) + (12 + zn) + 12;
    uint8_t* out = malloc(total);
    if (!out) { free(z); return NULL; }
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(out, SIG, 8);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;      // bit per canale
    ihdr[9] = 2;      // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    uint8_t* o = put_chunk(out + 8, "IHDR", ihdr, 13);
    o = put_chunk(o, "IDAT", z, zn);
    put_chunk(o, "IEND", NULL, 0);
    free(z);
    *len = total;
    return out;
}

bool png_write_file(const char* path, int w, int h, const uint32_t* pixels) {
    size_t n = 0;
    uint8_t* png = png_encode(w, h, pixels, &n);
    if (!png) return false;
    FILE* f = fopen(path, "wb");
    bool ok = f && fwrite(png, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = false;
    free(png);
    return ok;
}
