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
    bp_unpin(bt->bp, out, false);   // pin lasciato da bp_new_page (prima restava: il pool si riempiva)
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

// ── inserimento con divisione dei nodi ───────────────────────────
// Le foglie contengono chiavi e valori ordinati e sono collegate da next_leaf. Nei nodi interni
// keys[i] è la chiave più piccola del sottoalbero children[i + 1]. Un nodo pieno viene diviso a
// metà e la chiave separatrice sale al padre; se si divide la radice, il suo contenuto passa in
// una pagina nuova e la radice diventa un nodo interno: il suo id (salvato nel catalogo) non cambia.

#define BT_MAX BTREE_ORDER

// inserisce nel sottoalbero di pid; ritorna 1 se il nodo si è diviso (up_key / up_pid = nuovo fratello
// destro), 0 se no, <0 in caso di errore
static int insert_rec(BTree* bt, uint32_t pid, uint64_t key, BTreeValue val, uint64_t* up_key, uint32_t* up_pid) {
    BTreeNode* node = btree_fetch(bt, pid);
    if (!node) return SV_ERR;

    if (BTREE_IS_LEAF(node)) {
        int idx;
        int nk = node->hdr.num_keys;
        if (btree_search_in_leaf(node, key, &idx)) {          // chiave già presente: aggiorna il valore
            node->u.values[idx] = val;
            btree_unpin(bt, pid, true);
            return 0;
        }
        if (nk < BT_MAX) {
            for (int i = nk; i > idx; i--) { node->keys[i] = node->keys[i - 1]; node->u.values[i] = node->u.values[i - 1]; }
            node->keys[idx] = key;
            node->u.values[idx] = val;
            node->hdr.num_keys++;
            btree_unpin(bt, pid, true);
            return 0;
        }
        // foglia piena: BT_MAX + 1 chiavi divise fra questa foglia e una nuova a destra
        uint64_t   k[BT_MAX + 1];
        BTreeValue v[BT_MAX + 1];
        for (int i = 0, j = 0; i <= nk; i++) {
            if (i == idx) { k[i] = key; v[i] = val; continue; }
            k[i] = node->keys[j]; v[i] = node->u.values[j]; j++;
        }
        uint32_t rpid = btree_new_page(bt);
        if (!rpid) { btree_unpin(bt, pid, false); return SV_ERR; }
        BTreeNode* right = btree_fetch(bt, rpid);
        if (!right) { btree_unpin(bt, pid, false); return SV_ERR; }
        int total = nk + 1, left_n = total / 2;
        BTREE_SET_LEAF(right);
        right->hdr.num_keys = (uint16_t)(total - left_n);
        for (int i = left_n; i < total; i++) { right->keys[i - left_n] = k[i]; right->u.values[i - left_n] = v[i]; }
        right->next_leaf = node->next_leaf;
        node->hdr.num_keys = (uint16_t)left_n;
        for (int i = 0; i < left_n; i++) { node->keys[i] = k[i]; node->u.values[i] = v[i]; }
        node->next_leaf = rpid;
        *up_key = right->keys[0];
        *up_pid = rpid;
        btree_unpin(bt, rpid, true);
        btree_unpin(bt, pid, true);
        return 1;
    }

    // nodo interno: scende nel figlio giusto
    int ci = btree_search_internal(node, key);
    uint32_t child = node->u.children[ci];
    btree_unpin(bt, pid, false);
    uint64_t ck;
    uint32_t cp;
    int rc = insert_rec(bt, child, key, val, &ck, &cp);
    if (rc <= 0) return rc;

    node = btree_fetch(bt, pid);                               // il figlio si è diviso: aggiunge il separatore
    if (!node) return SV_ERR;
    int nk = node->hdr.num_keys;
    if (nk < BT_MAX) {
        for (int i = nk; i > ci; i--) { node->keys[i] = node->keys[i - 1]; node->u.children[i + 1] = node->u.children[i]; }
        node->keys[ci] = ck;
        node->u.children[ci + 1] = cp;
        node->hdr.num_keys++;
        btree_unpin(bt, pid, true);
        return 0;
    }
    // nodo interno pieno: BT_MAX + 1 chiavi e BT_MAX + 2 figli, la chiave centrale sale
    uint64_t k[BT_MAX + 1];
    uint32_t c[BT_MAX + 2];
    for (int i = 0, j = 0; i <= nk; i++) {
        if (i == ci) { k[i] = ck; continue; }
        k[i] = node->keys[j++];
    }
    for (int i = 0, j = 0; i <= nk + 1; i++) {
        if (i == ci + 1) { c[i] = cp; continue; }
        c[i] = node->u.children[j++];
    }
    uint32_t rpid = btree_new_page(bt);
    if (!rpid) { btree_unpin(bt, pid, false); return SV_ERR; }
    BTreeNode* right = btree_fetch(bt, rpid);
    if (!right) { btree_unpin(bt, pid, false); return SV_ERR; }
    int total = nk + 1, mid = total / 2;                       // k[mid] sale al padre
    node->hdr.num_keys = (uint16_t)mid;
    for (int i = 0; i < mid; i++) node->keys[i] = k[i];
    for (int i = 0; i <= mid; i++) node->u.children[i] = c[i];
    BTREE_CLEAR_LEAF(right);
    right->hdr.num_keys = (uint16_t)(total - mid - 1);
    for (int i = mid + 1; i < total; i++) right->keys[i - mid - 1] = k[i];
    for (int i = mid + 1; i <= total; i++) right->u.children[i - mid - 1] = c[i];
    right->next_leaf = 0;
    *up_key = k[mid];
    *up_pid = rpid;
    btree_unpin(bt, rpid, true);
    btree_unpin(bt, pid, true);
    return 1;
}

int btree_insert(BTree* bt, uint64_t key, BTreeValue val) {
    if (!bt) return SV_ERR;
    uint64_t up_key;
    uint32_t up_pid;
    int rc = insert_rec(bt, bt->root_id, key, val, &up_key, &up_pid);
    if (rc <= 0) return rc < 0 ? rc : SV_OK;

    // la radice si è divisa: il suo contenuto (metà sinistra) va in una pagina nuova
    uint32_t lpid = btree_new_page(bt);
    if (!lpid) return SV_ERR;
    BTreeNode* root = btree_fetch(bt, bt->root_id);
    BTreeNode* left = btree_fetch(bt, lpid);
    if (!root || !left) {
        if (root) btree_unpin(bt, bt->root_id, false);
        if (left) btree_unpin(bt, lpid, false);
        return SV_ERR;
    }
    memcpy(left, root, sizeof(BTreeNode));
    memset(root, 0, sizeof(BTreeNode));
    BTREE_CLEAR_LEAF(root);
    root->hdr.num_keys = 1;
    root->keys[0] = up_key;
    root->u.children[0] = lpid;
    root->u.children[1] = up_pid;
    btree_unpin(bt, lpid, true);
    btree_unpin(bt, bt->root_id, true);
    return SV_OK;
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

    while (scan->slot >= node->hdr.num_keys) {               // foglia finita (o vuota): la successiva
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
