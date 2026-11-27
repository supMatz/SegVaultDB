// Buffer di testo comune (include/sv_strbuf.h, CR-B2): crescita, limite max, errore "appiccicoso", printf, take.
#include "../include/sv_strbuf.h"
#include <stdint.h>
#include <stdio.h>

static int g_tests, g_fail;
#define CHECK(c, ...) do { g_tests++; if (!(c)) { g_fail++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    SvStrBuf b = {0};
    char* e = svsb_take(&b);
    CHECK(e && !e[0], "empty buffer gives \"\"");
    free(e);

    for (int i = 0; i < 100000; i++) svsb_putc(&b, (char)('a' + i % 26));
    CHECK(!b.oom && b.n == 100000 && b.p[99999] == 'a' + 99999 % 26 && b.p[100000] == '\0', "growth to 100000 bytes");
    svsb_printf(&b, "|%d|%s|", 42, "x");
    CHECK(b.n == 100006 && !strcmp(b.p + 100000, "|42|x|"), "short printf appended");
    char big[1000];
    memset(big, 'z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    svsb_printf(&b, "[%s]", big);
    CHECK(b.n == 100006 + 1001 && b.p[b.n - 1] == ']' && b.p[100007] == 'z', "printf longer than the stack buffer");
    char* t = svsb_take(&b);
    CHECK(t && strlen(t) == 101007 && !b.p && !b.n && !b.cap, "take hands over the text and empties the buffer");
    free(t);

    SvStrBuf m = { .max = 10 };
    svsb_puts(&m, "0123456789");
    CHECK(!m.oom && m.n == 10, "exactly max fits");
    svsb_putc(&m, 'x');
    CHECK(m.oom && m.toolong && m.n == 10, "beyond max: toolong");
    svsb_puts(&m, "");
    svsb_putc(&m, 'y');
    CHECK(m.n == 10 && !strcmp(m.p, "0123456789"), "sticky: nothing is appended after a failure");
    CHECK(svsb_take(&m) == NULL && !m.p, "take of a failed buffer is NULL and frees it");

    SvStrBuf o = {0};
    svsb_puts(&o, "abc");
    volatile size_t huge = SIZE_MAX - 2;                   // volatile: il compilatore non la vede costante
    svsb_put(&o, "x", huge);                               // la somma traboccherebbe size_t
    CHECK(o.oom && !o.toolong && o.n == 3, "size_t overflow is an error, not a wrap-around");
    svsb_free(&o);

    char* p = NULL;
    size_t cap = 0;
    CHECK(svsb_grow(&p, &cap, 0, 5, 0) == 0 && cap >= 6, "grow for a foreign buffer");
    CHECK(svsb_grow(&p, &cap, 5, huge + 2, 0) == 1, "grow refuses an overflowing size");
    CHECK(svsb_grow(&p, &cap, 0, 100, 50) == 2, "grow honours max");
    free(p);

    if (g_fail) { printf("FAIL: %d/%d strbuf checks passed\n", g_tests - g_fail, g_tests); return 1; }
    printf("OK: %d/%d strbuf checks passed\n", g_tests, g_tests);
    return 0;
}
