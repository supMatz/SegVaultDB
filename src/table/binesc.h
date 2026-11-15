/*
SCOPO: byte arbitrari (anche 0x00) nei valori del motore, che sono stringhe C.
       Dentro il motore il byte 0x00 si scrive come la coppia C0 80 e il byte 0xC0 come C0 81 (0xC0 non
       compare mai in un testo UTF-8 valido, quindi il testo normale non cambia). Così un BLOB con zeri,
       l'output di serialize() di PHP o un UUID binario passano interi per righe, indici, log e risultati.
       Si codifica dove i byte entrano (letterali X'..' / 0x.. / '\0', parametri dei protocolli, UNHEX,
       CHAR, FROM_BASE64) e si decodifica dove escono (protocollo MySQL, JSON) e nelle funzioni che
       contano o trasformano i byte (LENGTH, HEX, TO_BASE64, MD5, ...).
*/
#ifndef SV_BINESC_H
#define SV_BINESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SV_BIN_ESC  0xC0

// scrive la forma interna del byte b in out (1 o 2 byte); ritorna quanti
static inline size_t sv_bin_put(uint8_t b, char* out) {
    if (b == 0x00) { out[0] = (char)SV_BIN_ESC; out[1] = (char)0x80; return 2; }
    if (b == SV_BIN_ESC) { out[0] = (char)SV_BIN_ESC; out[1] = (char)0x81; return 2; }
    out[0] = (char)b;
    return 1;
}

// la stringa contiene byte codificati? (altrimenti è già la forma esterna)
static inline bool sv_bin_has(const char* s, size_t n) {
    return memchr(s, SV_BIN_ESC, n) != NULL;
}

// forma esterna (byte veri) di s[0..n): in out (almeno n byte; può coincidere con s); ritorna la lunghezza
static inline size_t sv_bin_decode(const char* s, size_t n, char* out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == SV_BIN_ESC && i + 1 < n && ((unsigned char)s[i + 1] == 0x80 || (unsigned char)s[i + 1] == 0x81)) {
            out[k++] = (unsigned char)s[i + 1] == 0x80 ? '\0' : (char)SV_BIN_ESC;
            i++;
        } else out[k++] = (char)c;
    }
    return k;
}

// numero di byte veri della stringa (terminata da NUL) in forma interna
static inline size_t sv_bin_len(const char* s) {
    size_t k = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++, k++)
        if (*p == SV_BIN_ESC && (p[1] == 0x80 || p[1] == 0x81)) p++;
    return k;
}

#endif
