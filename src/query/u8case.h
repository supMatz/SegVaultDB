/*
SCOPO: maiuscole / minuscole UTF-8 per UPPER, LOWER, LIKE e le collation _ci.
       Prima tutto passava da toupper / tolower (solo ASCII): UPPER('Città') = 'CITTà' e in una colonna _ci
       'CITTÀ' <> 'città'. Le mappature coperte (ASCII, Latino-1, Latino esteso A, greco, cirillico di base)
       non cambiano la lunghezza in byte della sequenza UTF-8, quindi la conversione si fa sul posto e le
       chiavi degli indici restano lunghe come il testo.
*/
#ifndef SV_U8CASE_H
#define SV_U8CASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define U8_RAW 0x110000u               // u8_get: byte grezzo b restituito come U8_RAW + b

static inline uint32_t u8_lower_cp(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x137 && c != 0x130 && !(c & 1)) return c + 1;
    if (c >= 0x139 && c <= 0x148 && (c & 1)) return c + 1;
    if (c >= 0x14A && c <= 0x177 && !(c & 1)) return c + 1;
    if (c >= 0x179 && c <= 0x17E && (c & 1)) return c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 0x25;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 0x3F;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

static inline uint32_t u8_upper_cp(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c >= 0x101 && c <= 0x137 && c != 0x131 && (c & 1)) return c - 1;
    if (c >= 0x13A && c <= 0x148 && !(c & 1)) return c - 1;
    if (c >= 0x14B && c <= 0x177 && (c & 1)) return c - 1;
    if (c >= 0x17A && c <= 0x17E && !(c & 1)) return c - 1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
    if (c == 0x3C2) return 0x3A3;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 0x25;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 0x3F;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

// carattere che inizia in s (ASCII o sequenza di 2 byte valida); *n = byte letti. Il resto (3-4 byte, byte non
// validi) torna byte per byte e non viene convertito
static inline uint32_t u8_get(const char* s, int* n) {
    unsigned char b = (unsigned char)s[0];
    if (b >= 0xC2 && b <= 0xDF && ((unsigned char)s[1] & 0xC0) == 0x80) {
        *n = 2;
        return (uint32_t)(b & 0x1F) << 6 | ((unsigned char)s[1] & 0x3F);
    }
    *n = b ? 1 : 0;
    // byte di una sequenza di 3-4 byte (€, emoji) o non valido: fuori da Unicode, così nessuna mappatura lo tocca
    // (prima UPPER('€') trattava il byte 0xE2 come 'â' e lo cambiava)
    return b < 0x80 ? b : U8_RAW + b;
}

// carattere completo (per LIKE '_'): byte di una sequenza UTF-8 qualsiasi, almeno 1 se non è la fine
static inline int u8_char_len(const char* s) {
    unsigned char b = (unsigned char)s[0];
    if (!b) return 0;
    int n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    for (int i = 1; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
    return n;
}

// conversione sul posto dei primi l byte
static inline void u8_case_inplace(char* s, size_t l, bool upper) {
    for (size_t i = 0; i < l; ) {
        int n;
        uint32_t c = u8_get(s + i, &n);
        if (n == 0) break;
        uint32_t m = upper ? u8_upper_cp(c) : u8_lower_cp(c);
        if (m != c && c < U8_RAW) {
            if (n == 1) s[i] = (char)m;
            else { s[i] = (char)(0xC0 | (m >> 6)); s[i + 1] = (char)(0x80 | (m & 0x3F)); }
        }
        i += (size_t)n;
    }
}

// piegatura delle collation _ci di MySQL (general / unicode / 0900_ai): senza maiuscole E senza accenti per le lettere
// latine ('Èra' = 'era', 'Ç' = 'c'). ß, æ, ø restano (in MySQL diventano due lettere). Il risultato può essere ASCII
static inline uint32_t u8_ci_cp(uint32_t c) {
    c = u8_lower_cp(c);
    if (c < 0xE0) return c;
    if (c <= 0xFF) {
        static const char L1[33] = "aaaaaa\0ceeeeiiii\0nooooo\0ouuuuy\0y";   // U+00E0 … U+00FF (\0 = invariato)
        char b = L1[c - 0xE0];
        return b ? (uint32_t)(unsigned char)b : c;
    }
    if (c < 0x100 || c > 0x17F) return c;
    // Latino esteso A (dopo u8_lower_cp le lettere sono minuscole): base di ogni coppia
    static const char LA[129] =
        "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii\0\0jjkk\0llllllllllnnnnnn\0\0\0oooooo\0\0rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzz\0";
    char b = LA[c - 0x100];
    return b ? (uint32_t)(unsigned char)b : c;
}

// copia piegata per le chiavi degli indici _ci (dst può coincidere con src: l'uscita non è mai più lunga)
static inline size_t u8_ci_fold(char* dst, const char* src) {
    size_t k = 0;
    for (;;) {
        int n;
        uint32_t c = u8_get(src, &n);
        if (!n) break;
        uint32_t m = u8_ci_cp(c);
        if (c >= U8_RAW) dst[k++] = (char)(c - U8_RAW);                     // byte grezzo: com'è
        else if (m < 0x80) dst[k++] = (char)m;
        else { dst[k++] = (char)(0xC0 | (m >> 6)); dst[k++] = (char)(0x80 | (m & 0x3F)); }
        src += n;
    }
    dst[k] = '\0';
    return k;
}

// confronto con la piegatura _ci, carattere per carattere (ordine dei punti di codice piegati)
static inline int u8_ci_cmp(const char* a, const char* b) {
    for (;;) {
        int na, nb;
        uint32_t x = u8_ci_cp(u8_get(a, &na)), y = u8_ci_cp(u8_get(b, &nb));
        if (x != y) return x < y ? -1 : 1;
        if (!na) return 0;
        a += na;
        b += nb;
    }
}

// hash FNV-1a del testo piegato (stessa piegatura di u8_ci_cmp)
static inline uint64_t u8_ci_hash(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (;;) {
        int n;
        uint32_t c = u8_ci_cp(u8_get(s, &n));
        if (!n) break;
        h ^= c;
        h *= 1099511628211ULL;
        s += n;
    }
    return h;
}

#endif
