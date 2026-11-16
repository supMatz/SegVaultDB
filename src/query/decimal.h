/*
SCOPO: numeri decimali esatti (P1.9) per DECIMAL, i letterali con la virgola (0.10) e gli interi oltre 64 bit.
       Valore = (-1)^neg · mantissa / 10^scale, mantissa intera in base 10^9 (fino a DEC_LIMBS limbi = 81 cifre).
       Regole della scala come MySQL: + e - la maggiore, * la somma, / quella del dividendo + 4 (al massimo 30),
       arrotondamento a metà lontano dallo zero. Niente esponenti: un DECIMAL non si stampa mai come 1e+16.
*/
#ifndef SV_DECIMAL_H
#define SV_DECIMAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEC_LIMBS     9
#define DEC_MAX_SCALE 30
#define DEC_DIV_INC   4         // div_precision_increment di MySQL

typedef struct {
    bool     neg;
    int      scale;             // cifre dopo la virgola
    int      n;                 // limbi usati (0 = zero)
    uint32_t d[DEC_LIMBS];      // mantissa, la cifra meno significativa per prima
} Dec;

bool   dec_parse(const char* s, Dec* out);            // "[-+]123[.45]" (spazi intorno ammessi; niente esponente)
void   dec_from_i64(int64_t v, Dec* out);
void   dec_from_u64(uint64_t v, Dec* out);
size_t dec_format(const Dec* a, char* out, size_t n);  // con esattamente a->scale cifre decimali
double dec_to_double(const Dec* a);
bool   dec_to_i64(const Dec* a, int64_t* v);           // solo se intero (scala 0 dopo l'arrotondamento) e nel campo
int    dec_cmp(const Dec* a, const Dec* b);
bool   dec_add(const Dec* a, const Dec* b, Dec* out);  // false = oltre 81 cifre
bool   dec_sub(const Dec* a, const Dec* b, Dec* out);
bool   dec_mul(const Dec* a, const Dec* b, Dec* out);
bool   dec_div(const Dec* a, const Dec* b, int scale, Dec* out);   // false anche per b = 0
bool   dec_mod(const Dec* a, const Dec* b, Dec* out);
bool   dec_round(const Dec* a, int scale, Dec* out);   // a metà lontano dallo zero; scala negativa: decine, ...
bool   dec_trunc(const Dec* a, int scale, Dec* out);
bool   dec_is_zero(const Dec* a);
// testo canonico (senza zeri finali dopo la virgola): chiave per GROUP BY / DISTINCT / join (1.50 = 1.5)
size_t dec_key(const Dec* a, char* out, size_t n);

#endif
