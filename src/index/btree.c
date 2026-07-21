#include "btree.h"
#include <string.h>

static uint32_t btree_new_page(BTree* bt) {
    uint32_t out;
    if (bp_new_page(bt->bp, &out) != SV_OK) return 0;
    bt->num_pages++;
    Page* p = bp_fetch(bt->bp, out);
    if (p) {
        memset(p->data, 0, SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE);
        bp_unpin(bt->bp, out, true);
    }
    return out;
}

static BTreeNode* btree_fetch(BTree* bt, uint32_t pid) {
    Page* p = bp_fetch(bt->bp, pid);
    if (!p) return NULL;
    return (BTreeNode*)p->data;
}

static void btree_unpin(BTree* bt, uint32_t pid, bool dirty) {
    bp_unpin(bt->bp, pid, dirty);
}

BTree* btree_create(BufferPool* bp, uint32_t root_id) {
    BTree* bt = SV_ALLOC(BTree);
    if (!bt) return NULL;
    bt->bp        = bp;
    bt->root_id   = root_id;
    bt->num_pages = 1;

    Page* p = bp_fetch(bp, root_id);
    if (!p) { free(bt); return NULL; }
    BTreeNode* node = (BTreeNode*)p->data;
    memset(p->data, 0, SV_PAGE_SIZE - SV_PAGE_HEADER_SIZE);
    BTREE_SET_LEAF(node);
    node->hdr.num_keys = 0;
    node->next_leaf    = 0;
    bp_unpin(bp, root_id, true);
    return bt;
}

BTree* btree_open(BufferPool* bp, uint32_t root_id) {
    BTree* bt = SV_ALLOC(BTree);
    if (!bt) return NULL;
    bt->bp        = bp;
    bt->root_id   = root_id;
    bt->num_pages = 1;
    return bt;
}

static int btree_search_in_leaf(BTreeNode* node, uint64_t key, int* out_idx) {
    int lo = 0, hi = node->hdr.num_keys - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (node->keys[mid] == key) { *out_idx = mid; return 1; }
        if (node->keys[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    *out_idx = lo;
    return 0;
}

static int btree_search_internal(BTreeNode* node, uint64_t key) {
    int lo = 0, hi = node->hdr.num_keys - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (key < node->keys[mid]) hi = mid - 1;
        else lo = mid + 1;
    }
    return lo;
}

bool btree_search(BTree* bt, uint64_t key, BTreeValue* out_val) {
    if (!bt) return false;
    uint32_t pid = bt->root_id;

    while (pid) {
        BTreeNode* node = btree_fetch(bt, pid);
        if (!node) return false;

        if (BTREE_IS_LEAF(node)) {
            int idx;
            int found = btree_search_in_leaf(node, key, &idx);
            if (found && out_val) *out_val = node->u.values[idx];
            btree_unpin(bt, pid, false);
            return found;
        }

        int child_idx = btree_search_internal(node, key);
        uint32_t next = node->u.children[child_idx];
        btree_unpin(bt, pid, false);
        pid = next;
    }
    return false;
}

int btree_insert(BTree* bt, uint64_t key, BTreeValue val) {
    if (!bt) return SV_ERR;

    // Walk to leaf, tracking path
    uint32_t pid = bt->root_id;

    while (1) {
        BTreeNode* node = btree_fetch(bt, pid);
        if (!node) return SV_ERR;

        if (BTREE_IS_LEAF(node)) {
            // Find insertion position
            int idx;
            btree_search_in_leaf(node, key, &idx);

            // Shift keys and values
            int nk = node->hdr.num_keys;
            if (idx < nk && node->keys[idx] == key) {
                // Duplicate key: update value
                node->u.values[idx] = val;
                btree_unpin(bt, pid, true);
                return SV_OK;
            }

            if (nk >= BTREE_ORDER) {
                btree_unpin(bt, pid, false);
                return SV_FULL;
            }

            for (int i = nk; i > idx; i--) {
                node->keys[i] = node->keys[i - 1];
                node->u.values[i] = node->u.values[i - 1];
            }
            node->keys[idx] = key;
            node->u.values[idx] = val;
            node->hdr.num_keys++;
            btree_unpin(bt, pid, true);
            return SV_OK;
        }

        int ci = btree_search_internal(node, key);
        uint32_t next = node->u.children[ci];
        btree_unpin(bt, pid, false);
        pid = next;
    }
}

int btree_delete(BTree* bt, uint64_t key) {
    if (!bt) return SV_ERR;
    uint32_t pid = bt->root_id;

    while (pid) {
        BTreeNode* node = btree_fetch(bt, pid);
        if (!node) return SV_ERR;

        if (BTREE_IS_LEAF(node)) {
            int idx;
            if (!btree_search_in_leaf(node, key, &idx)) {
                btree_unpin(bt, pid, false);
                return SV_NOT_FOUND;
            }
            int nk = node->hdr.num_keys;
            for (int i = idx; i < nk - 1; i++) {
                node->keys[i] = node->keys[i + 1];
                node->u.values[i] = node->u.values[i + 1];
            }
            node->hdr.num_keys--;
            btree_unpin(bt, pid, true);
            return SV_OK;
        }

        int ci = btree_search_internal(node, key);
        uint32_t next = node->u.children[ci];
        btree_unpin(bt, pid, false);
        pid = next;
    }
    return SV_NOT_FOUND;
}

BTreeScan* btree_scan_open(BTree* bt) {
    if (!bt) return NULL;
    BTreeScan* scan = SV_ALLOC(BTreeScan);
    if (!scan) return NULL;

    // Find leftmost leaf
    uint32_t pid = bt->root_id;
    while (pid) {
        BTreeNode* node = btree_fetch(bt, pid);
        if (!node) { free(scan); return NULL; }

        if (BTREE_IS_LEAF(node)) {
            scan->leaf_page = pid;
            scan->slot      = 0;
            scan->done      = false;
            scan->bt        = bt;
            btree_unpin(bt, pid, false);
            return scan;
        }

        uint32_t next = node->u.children[0];
        btree_unpin(bt, pid, false);
        pid = next;
    }
    free(scan);
    return NULL;
}

bool btree_scan_next(BTreeScan* scan, uint64_t* key, BTreeValue* val) {
    if (!scan || scan->done) return false;

    BTreeNode* node = btree_fetch(scan->bt, scan->leaf_page);
    if (!node) { scan->done = true; return false; }

    if (scan->slot >= node->hdr.num_keys) {
        uint32_t next = node->next_leaf;
        btree_unpin(scan->bt, scan->leaf_page, false);
        if (!next) { scan->done = true; return false; }
        scan->leaf_page = next;
        scan->slot = 0;
        node = btree_fetch(scan->bt, scan->leaf_page);
        if (!node) { scan->done = true; return false; }
    }

    if (key) *key = node->keys[scan->slot];
    if (val) *val = node->u.values[scan->slot];
    scan->slot++;
    btree_unpin(scan->bt, scan->leaf_page, false);
    return true;
}

void btree_scan_close(BTreeScan* scan) { free(scan); }
