/*
SCOPO: numeri decimali esatti (vedi decimal.h).
*/
#include "decimal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define BASE 1000000000u

static void norm(Dec* a) {
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
    if (a->n == 0) a->neg = false;
}

bool dec_is_zero(const Dec* a) { return a->n == 0; }

// mantissa *= 10^k (false se non ci sta)
static bool mul_pow10(Dec* a, int k) {
    static const uint32_t P[10] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
    while (k > 0) {
        int step = k > 9 ? 9 : k;
        uint64_t m = P[step], carry = 0;
        for (int i = 0; i < a->n; i++) {
            uint64_t v = (uint64_t)a->d[i] * m + carry;
            a->d[i] = (uint32_t)(v % BASE);
            carry = v / BASE;
        }
        if (carry) {
            if (a->n >= DEC_LIMBS) return false;
            a->d[a->n++] = (uint32_t)carry;
        }
        k -= step;
    }
    return true;
}

// mantissa /= 10^k; *first = la più significativa delle cifre tolte (decide l'arrotondamento)
static void div_pow10(Dec* a, int k, int* first) {
    *first = 0;
    for (int s = 0; s < k; s++) {
        uint64_t r = 0;
        for (int i = a->n - 1; i >= 0; i--) {
            uint64_t v = r * BASE + a->d[i];
            a->d[i] = (uint32_t)(v / 10);
            r = v % 10;
        }
        norm(a);
        *first = (int)r;                        // l'ultima tolta è la più significativa
    }
}

bool dec_parse(const char* s, Dec* out) {
    memset(out, 0, sizeof(*out));
    while (isspace((unsigned char)*s)) s++;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    char digits[DEC_LIMBS * 9 + 1];
    int nd = 0, scale = 0;
    bool dot = false, any = false;
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            any = true;
            if (nd == 0 && *s == '0' && !dot) continue;          // zeri iniziali
            if (nd >= DEC_LIMBS * 9) return false;
            digits[nd++] = *s;
            if (dot) scale++;
        } else if (*s == '.' && !dot) dot = true;
        else break;
    }
    while (isspace((unsigned char)*s)) s++;
    if (*s || !any || scale > DEC_MAX_SCALE + 30) return false;
    // dalle cifre (più significativa prima) ai limbi
    int li = 0;
    for (int end = nd; end > 0; end -= 9) {
        int start = end - 9 < 0 ? 0 : end - 9;
        uint32_t v = 0;
        for (int i = start; i < end; i++) v = v * 10 + (uint32_t)(digits[i] - '0');
        out->d[li++] = v;
    }
    out->n = li;
    out->neg = neg;
    out->scale = scale;
    norm(out);
    return true;
}

void dec_from_u64(uint64_t v, Dec* out) {
    memset(out, 0, sizeof(*out));
    while (v) { out->d[out->n++] = (uint32_t)(v % BASE); v /= BASE; }
}

void dec_from_i64(int64_t v, Dec* out) {
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    dec_from_u64(m, out);
    out->neg = v < 0 && out->n;
}

size_t dec_format(const Dec* a, char* out, size_t n) {
    char digits[DEC_LIMBS * 9 + 2];
    int nd = 0;
    if (a->n == 0) digits[nd++] = '0';
    else {
        nd = snprintf(digits, sizeof(digits), "%u", a->d[a->n - 1]);
        for (int i = a->n - 2; i >= 0; i--) nd += snprintf(digits + nd, sizeof(digits) - (size_t)nd, "%09u", a->d[i]);
    }
    char buf[DEC_LIMBS * 9 + 80];
    size_t o = 0;
    if (a->neg) buf[o++] = '-';
    int ip = nd - a->scale;                  // cifre intere
    if (ip <= 0) {
        buf[o++] = '0';
        if (a->scale > 0) {
            buf[o++] = '.';
            for (int i = 0; i < -ip; i++) buf[o++] = '0';
            memcpy(buf + o, digits, (size_t)nd); o += (size_t)nd;
        }
    } else {
        memcpy(buf + o, digits, (size_t)ip); o += (size_t)ip;
        if (a->scale > 0) { buf[o++] = '.'; memcpy(buf + o, digits + ip, (size_t)a->scale); o += (size_t)a->scale; }
    }
    buf[o] = '\0';
    snprintf(out, n, "%s", buf);
    return o;
}

size_t dec_key(const Dec* a, char* out, size_t n) {
    char t[DEC_LIMBS * 9 + 80];
    dec_format(a, t, sizeof(t));
    char* dot = strchr(t, '.');
    if (dot) {
        char* e = t + strlen(t) - 1;
        while (e > dot && *e == '0') *e-- = '\0';
        if (e == dot) *e = '\0';
    }
    if (!strcmp(t, "-0")) snprintf(t, sizeof(t), "0");
    snprintf(out, n, "%s", t);
    return strlen(out);
}

double dec_to_double(const Dec* a) {
    char t[DEC_LIMBS * 9 + 80];
    dec_format(a, t, sizeof(t));
    double v = 0;
    sscanf(t, "%lf", &v);
    return v;
}

// confronto delle mantisse (stessa scala)
static int mag_cmp(const Dec* a, const Dec* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

// porta a e b alla stessa scala (la maggiore)
static bool align(Dec* a, Dec* b) {
    if (a->scale < b->scale) { if (!mul_pow10(a, b->scale - a->scale)) return false; a->scale = b->scale; }
    else if (b->scale < a->scale) { if (!mul_pow10(b, a->scale - b->scale)) return false; b->scale = a->scale; }
    return true;
}

int dec_cmp(const Dec* x, const Dec* y) {
    if (x->n == 0 && y->n == 0) return 0;
    if (x->neg != y->neg) return x->neg ? -1 : 1;
    Dec a = *x, b = *y;
    if (!align(&a, &b)) {                         // non allineabili: decide il double (casi estremi)
        double p = dec_to_double(x), q = dec_to_double(y);
        return p < q ? -1 : p > q;
    }
    int c = mag_cmp(&a, &b);
    return x->neg ? -c : c;
}

static void mag_add(const Dec* a, const Dec* b, Dec* o, bool* ovf) {
    uint64_t carry = 0;
    int n = a->n > b->n ? a->n : b->n;
    Dec r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < n; i++) {
        uint64_t v = carry + (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        r.d[i] = (uint32_t)(v % BASE);
        carry = v / BASE;
    }
    r.n = n;
    if (carry) { if (n >= DEC_LIMBS) { *ovf = true; return; } r.d[r.n++] = (uint32_t)carry; }
    r.scale = a->scale;
    *o = r;
}

static void mag_sub(const Dec* a, const Dec* b, Dec* o) {   // |a| >= |b|
    int64_t borrow = 0;
    Dec r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < a->n; i++) {
        int64_t v = (int64_t)a->d[i] - borrow - (i < b->n ? (int64_t)b->d[i] : 0);
        borrow = v < 0;
        if (v < 0) v += BASE;
        r.d[i] = (uint32_t)v;
    }
    r.n = a->n;
    r.scale = a->scale;
    norm(&r);
    *o = r;
}

static bool addsub(const Dec* x, const Dec* y, bool negate_y, Dec* out) {
    Dec a = *x, b = *y;
    if (negate_y && b.n) b.neg = !b.neg;
    if (!align(&a, &b)) return false;
    bool ovf = false;
    if (a.neg == b.neg) { mag_add(&a, &b, out, &ovf); if (ovf) return false; out->neg = a.neg; }
    else if (mag_cmp(&a, &b) >= 0) { mag_sub(&a, &b, out); out->neg = a.neg; }
    else { mag_sub(&b, &a, out); out->neg = b.neg; }
    out->scale = a.scale;
    norm(out);
    return true;
}

bool dec_add(const Dec* a, const Dec* b, Dec* out) { return addsub(a, b, false, out); }
bool dec_sub(const Dec* a, const Dec* b, Dec* out) { return addsub(a, b, true, out); }

bool dec_round(const Dec* a, int scale, Dec* out) {
    if (scale > DEC_MAX_SCALE) scale = DEC_MAX_SCALE;
    if (scale < -DEC_LIMBS * 9) scale = -DEC_LIMBS * 9;
    Dec r = *a;
    if (scale >= r.scale) {                    // più cifre: zeri in fondo
        if (scale > r.scale && !mul_pow10(&r, scale - r.scale)) return false;
        r.scale = scale;
        *out = r;
        return true;
    }
    // una sola volta, anche per una scala negativa (ROUND(14.5, -1) = 10, non 15 e poi 20)
    int drop = r.scale - scale, first;
    div_pow10(&r, drop, &first);
    r.scale = 0;
    if (first >= 5) {                          // a metà o oltre: lontano dallo zero
        Dec one;
        dec_from_u64(1, &one);
        bool ovf = false, neg = r.neg;
        r.neg = false;
        mag_add(&r, &one, &r, &ovf);
        if (ovf) return false;
        r.neg = neg;
    }
    if (scale < 0) { if (!mul_pow10(&r, -scale)) return false; r.scale = 0; }
    else r.scale = scale;
    norm(&r);
    *out = r;
    return true;
}

bool dec_trunc(const Dec* a, int scale, Dec* out) {
    if (scale >= a->scale) return dec_round(a, scale, out);
    Dec r = *a;
    int first;
    div_pow10(&r, r.scale - scale, &first);
    if (scale < 0) { if (!mul_pow10(&r, -scale)) return false; r.scale = 0; }
    else r.scale = scale;
    norm(&r);
    *out = r;
    return true;
}

bool dec_mul(const Dec* a, const Dec* b, Dec* out) {
    uint64_t t[2 * DEC_LIMBS + 1];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->n; j++) {
            uint64_t v = t[i + j] + (uint64_t)a->d[i] * b->d[j] + carry;
            t[i + j] = v % BASE;
            carry = v / BASE;
        }
        int k = i + b->n;
        while (carry) { uint64_t v = t[k] + carry; t[k] = v % BASE; carry = v / BASE; k++; }
    }
    int n = a->n + b->n;
    while (n > 0 && t[n - 1] == 0) n--;
    int scale = a->scale + b->scale;
    // troppe cifre decimali (oltre 30) o troppi limbi: si arrotonda togliendo cifre decimali
    Dec r;
    memset(&r, 0, sizeof(r));
    if (n > DEC_LIMBS) {
        // si tolgono limbi interi di cifre decimali quando possibile
        int drop = n - DEC_LIMBS;
        if (drop * 9 > scale) return false;
        for (int i = 0; i < DEC_LIMBS; i++) r.d[i] = (uint32_t)t[i + drop];
        r.n = DEC_LIMBS;
        scale -= drop * 9;
    } else {
        for (int i = 0; i < n; i++) r.d[i] = (uint32_t)t[i];
        r.n = n;
    }
    r.scale = scale;
    r.neg = (a->neg != b->neg);
    norm(&r);
    if (r.scale > DEC_MAX_SCALE) return dec_round(&r, DEC_MAX_SCALE, out);
    *out = r;
    return true;
}

// mantissa / piccolo intero (resto in *rem)
static void mag_divsmall(Dec* a, uint32_t m, uint32_t* rem) {
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t v = r * BASE + a->d[i];
        a->d[i] = (uint32_t)(v / m);
        r = v % m;
    }
    *rem = (uint32_t)r;
    norm(a);
}

// divisione lunga di mantisse (cifra per cifra decimale: semplice e abbastanza veloce per 81 cifre)
bool dec_div(const Dec* x, const Dec* y, int scale, Dec* out) {
    if (y->n == 0) return false;
    if (scale > DEC_MAX_SCALE) scale = DEC_MAX_SCALE;
    if (scale < 0) scale = 0;
    // q = x / y con scale + 1 cifre (poi si arrotonda): (x · 10^(scale + 1 + sy - sx)) / y
    Dec a = *x, b = *y;
    a.neg = b.neg = false;
    int k = scale + 1 + b.scale - a.scale;
    a.scale = b.scale = 0;
    if (k > 0) { if (!mul_pow10(&a, k)) return false; }
    else if (k < 0) { int f; div_pow10(&a, -k, &f); }
    Dec q;
    memset(&q, 0, sizeof(q));
    if (b.n == 1) {
        q = a;
        uint32_t rem;
        mag_divsmall(&q, b.d[0], &rem);
    } else {
        // divisione cifra per cifra: si costruisce il quoziente in base 10 sottraendo b · 10^i
        char ad[DEC_LIMBS * 9 + 2];
        Dec tmp = a;
        tmp.scale = 0;
        dec_format(&tmp, ad, sizeof(ad));
        Dec rem;
        memset(&rem, 0, sizeof(rem));
        char qd[DEC_LIMBS * 9 + 2];
        int nq = 0;
        for (const char* p = ad; *p; p++) {
            if (!mul_pow10(&rem, 1)) return false;
            Dec dig;
            dec_from_u64((uint64_t)(*p - '0'), &dig);
            bool ovf = false;
            mag_add(&rem, &dig, &rem, &ovf);
            if (ovf) return false;
            norm(&rem);
            int c = 0;
            while (mag_cmp(&rem, &b) >= 0) { mag_sub(&rem, &b, &rem); c++; }
            if (nq < (int)sizeof(qd) - 1) qd[nq++] = (char)('0' + c);
        }
        qd[nq] = '\0';
        if (!dec_parse(qd, &q)) return false;
    }
    q.scale = scale + 1;
    q.neg = (x->neg != y->neg) && q.n;
    return dec_round(&q, scale, out);
}

bool dec_mod(const Dec* a, const Dec* b, Dec* out) {
    if (b->n == 0) return false;
    // a - b · trunc(a / b), con la scala maggiore
    Dec q, p;
    int sc = a->scale > b->scale ? a->scale : b->scale;
    if (!dec_div(a, b, sc + 1, &q) || !dec_trunc(&q, 0, &q) || !dec_mul(&q, b, &p) || !dec_sub(a, &p, out)) return false;
    return dec_round(out, sc, out);
}

bool dec_to_i64(const Dec* a, int64_t* v) {
    Dec r;
    if (!dec_round(a, 0, &r) || r.n > 3) return false;
    uint64_t m = 0;
    for (int i = r.n - 1; i >= 0; i--) {
        if (m > (UINT64_MAX - r.d[i]) / BASE) return false;
        m = m * BASE + r.d[i];
    }
    if (r.neg) { if (m > (uint64_t)INT64_MAX + 1) return false; *v = m == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)m; }
    else { if (m > (uint64_t)INT64_MAX) return false; *v = (int64_t)m; }
    return true;
}
