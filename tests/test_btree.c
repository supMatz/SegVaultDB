// Test del B+tree su disco di sola lettura (src/index/bptree.c, P1.6): costruzione da 20 000 chiavi ordinate,
// ricerca di ogni chiave e di chiavi assenti con un buffer pool piccolo (le pagine vanno e tornano dal disco),
// elenco delle pagine, rifiuto di chiavi troppo lunghe senza pagine perse, pagina danneggiata = errore.
#include "src/index/bptree.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/page.h"
#include "src/platform_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 20000

static int failures = 0, checks = 0;
static void check(int cond, const char* what, long i) {
    checks++;
    if (!cond && failures++ < 10) printf("FAIL: %s (%ld)\n", what, i);
}

static int g_pages;
static void count_page(void* ctx, uint32_t pid) { (void)ctx; (void)pid; g_pages++; }

int main(void) {
    const char* path = "/tmp/sv_bptree_test.db";
    remove(path);
    int fd = sv_open(path, SV_O_RDWR | SV_O_CREAT | SV_O_TRUNC, 0600);
    if (fd < 0) { printf("cannot open %s\n", path); return 1; }
    BufferPool* bp = bp_create(16, fd);                 // 16 frame: quasi ogni pagina si rilegge dal disco
    static char keys[N][16];
    static RID rids[N][3];
    static BptEntry e[N];
    for (int i = 0; i < N; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k%08d", i * 2);   // chiavi pari: le dispari sono assenti
        int nr = i % 7 == 0 ? 3 : 1;                         // qualche chiave con più righe (indice non unico)
        for (int k = 0; k < nr; k++) rids[i][k] = (RID){ (uint32_t)(1000 + i), (uint16_t)k };
        e[i] = (BptEntry){ keys[i], rids[i], nr };
    }
    uint32_t root = 0;
    int pages = 0;
    check(bpt_build(bp, e, N, 1, &root, &pages) == SV_OK && root, "build 20000 keys", pages);
    check(bp_flush_all(bp) == SV_OK, "flush", 0);
    long reads_max = 0;
    for (int i = 0; i < N; i++) {
        RID* r = NULL;
        int n = 0, reads = 0;
        int f = bpt_lookup(bp, root, keys[i], &r, &n, &reads);
        int want = i % 7 == 0 ? 3 : 1;
        check(f == 1 && n == want && r[0].page_id == (uint32_t)(1000 + i) && r[n - 1].slot_id == (uint16_t)(want - 1), "lookup", i);
        if (reads > reads_max) reads_max = reads;
        free(r);
        char miss[16];
        snprintf(miss, sizeof(miss), "k%08d", i * 2 + 1);
        check(bpt_lookup(bp, root, miss, &r, &n, NULL) == 0 && !r, "absent key", i);
    }
    RID* r = NULL;
    int n = 0;
    check(bpt_lookup(bp, root, "a", &r, &n, NULL) == 0, "smaller than every key", 0);
    check(bpt_lookup(bp, root, "z", &r, &n, NULL) == 0, "larger than every key", 0);
    check(reads_max <= 3, "pages read per lookup", reads_max);
    g_pages = 0;
    check(bpt_pages(bp, root, count_page, NULL) && g_pages == pages, "every page listed once", g_pages);
    // chiave troppo lunga: SV_FULL e nessuna pagina persa (tornano tutte libere)
    static char longkey[BPT_MAX_KEY + 10];
    memset(longkey, 'x', sizeof(longkey) - 1);
    BptEntry bad[2] = { { "a", rids[0], 1 }, { longkey, rids[1], 1 } };
    int free0 = bp_free_count(bp);
    uint32_t r2 = 0;
    check(bpt_build(bp, bad, 2, 1, &r2, NULL) == SV_FULL && r2 == 0, "key too long refused", 0);
    check(bp_free_count(bp) > free0, "pages of the failed build returned to the free list", bp_free_count(bp) - free0);
    // albero vuoto
    uint32_t r3 = 0;
    check(bpt_build(bp, NULL, 0, 1, &r3, NULL) == SV_OK && r3 && bpt_lookup(bp, r3, "k", &r, &n, NULL) == 0, "empty tree", 0);
    // radice che non è una pagina di indice: errore, non "assente"
    uint32_t heap_id;
    bp_new_page(bp, &heap_id);
    bp_unpin(bp, heap_id, true);
    check(bpt_lookup(bp, heap_id, "k00000000", &r, &n, NULL) == -1, "not an index page: error", 0);
    // pagina danneggiata sul disco: errore
    bp_flush_all(bp);
    bp_remove_all(bp);
    char junk[64];
    memset(junk, 0x5a, sizeof(junk));
    if (sv_pwrite(fd, junk, sizeof(junk), (int64_t)root * SV_PAGE_SIZE + 100) != (ssize_t)sizeof(junk)) {}
    check(bpt_lookup(bp, root, "k00000000", &r, &n, NULL) == -1, "damaged root page: error", 0);
    bp_destroy(bp);
    sv_close(fd);
    remove(path);
    if (failures) { printf("bptree: %d of %d checks FAILED\n", failures, checks); return 1; }
    printf("OK: B+tree with %d keys in %d pages, at most %ld page reads per lookup (%d checks)\n", N, pages, reads_max, checks);
    return 0;
}
