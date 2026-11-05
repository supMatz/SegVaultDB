// Test di stress del B-tree contro un modello di riferimento (array ordinato).
// Inserimenti casuali, aggiornamenti, ricerche, cancellazioni e scansione ordinata;
// il buffer pool è piccolo, così le pagine vengono scritte e rilette dal disco.
#include "src/index/btree.h"
#include "src/storage/buffer_pool.h"
#include "src/platform_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 20000

static uint64_t keys[N];
static int      present[N];
static uint32_t vals[N];
static int failures = 0;

static void check(int cond, const char* what, long i) {
    if (!cond && failures++ < 10) printf("FAIL: %s (%ld)\n", what, i);
}

static uint64_t rnd64(void) { return ((uint64_t)rand() << 42) ^ ((uint64_t)rand() << 21) ^ (uint64_t)rand(); }

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    const char* path = "/tmp/sv_btree_test.db";
    remove(path);
    int fd = sv_open(path, SV_O_RDWR | SV_O_CREAT, 0644);
    if (fd < 0) { printf("FAIL: open\n"); return 1; }
    BufferPool* bp = bp_create(16, fd);   // piccolo: forza evizioni e riletture
    uint32_t root;
    if (bp_new_page(bp, &root) != SV_OK) { printf("FAIL: new page\n"); return 1; }
    BTree* bt = btree_create(bp, root);

    srand(12345);
    for (int i = 0; i < N; i++) {           // chiavi distinte, anche "negative" (bit alto)
        uint64_t k;
        int dup;
        do {
            k = rnd64();
            dup = 0;
            for (int j = 0; j < i && !dup; j++) dup = keys[j] == k;
        } while (dup);
        keys[i] = k;
    }
    for (int i = 0; i < N; i++) {
        vals[i] = (uint32_t)i;
        check(btree_insert(bt, keys[i], (BTreeValue){vals[i], 1}) == SV_OK, "insert", i);
        present[i] = 1;
    }
    for (int i = 0; i < N; i += 3) {        // aggiornamento del valore di una chiave esistente
        vals[i] = (uint32_t)(i + 1000000);
        check(btree_insert(bt, keys[i], (BTreeValue){vals[i], 2}) == SV_OK, "update", i);
    }
    for (int i = 0; i < N; i += 2) {        // cancella metà delle chiavi
        check(btree_delete(bt, keys[i]) == SV_OK, "delete", i);
        present[i] = 0;
    }
    check(btree_delete(bt, keys[0]) == SV_NOT_FOUND, "delete twice", 0);
    for (int i = 0; i < N; i++) {
        BTreeValue v;
        bool found = btree_search(bt, keys[i], &v);
        check(found == (bool)present[i], "search presence", i);
        if (found) check(v.page_id == vals[i], "search value", i);
    }
    // scansione: tutte e sole le chiavi presenti, in ordine crescente
    uint64_t* expect = malloc(sizeof(uint64_t) * N);
    int ne = 0;
    for (int i = 0; i < N; i++) if (present[i]) expect[ne++] = keys[i];
    qsort(expect, ne, sizeof(uint64_t), cmp_u64);
    BTreeScan* sc = btree_scan_open(bt);
    uint64_t k;
    int got = 0;
    while (btree_scan_next(sc, &k, NULL)) {
        if (got < ne) check(k == expect[got], "scan order", got);
        got++;
    }
    btree_scan_close(sc);
    check(got == ne, "scan count", got);
    // reinserimento delle chiavi cancellate
    for (int i = 0; i < N; i += 2) check(btree_insert(bt, keys[i], (BTreeValue){7, 7}) == SV_OK, "reinsert", i);
    for (int i = 0; i < N; i++) check(btree_search(bt, keys[i], NULL), "search after reinsert", i);

    free(expect);
    free(bt);
    bp_flush_all(bp);
    bp_destroy(bp);
    sv_close(fd);
    remove(path);
    printf("%s: B-tree with %d keys (%d failures)\n", failures ? "FAIL" : "OK", N, failures);
    return failures ? 1 : 0;
}
