#ifndef BTREE_H
#define BTREE_H

#include "../../include/common.h"
#include "../storage/buffer_pool.h"
#include "../table/tuple.h"

#define BTREE_ORDER 64
#define BTREE_IS_LEAF(node) ((node)->hdr.header & 0x01)
#define BTREE_SET_LEAF(node) ((node)->hdr.header |= 0x01)
#define BTREE_CLEAR_LEAF(node) ((node)->hdr.header &= ~0x01)

typedef struct {
    uint32_t header;
    uint16_t num_keys;
    uint16_t pad;
} BTreeNodeHeader;

typedef struct {
    uint32_t page_id;
    uint32_t slot_id;
} BTreeValue;

typedef struct {
    BTreeNodeHeader hdr;
    uint64_t keys[BTREE_ORDER];
    union {
        uint32_t children[BTREE_ORDER + 1];
        BTreeValue values[BTREE_ORDER];
    } u;
    uint32_t next_leaf;
    uint8_t  _pad[SV_PAGE_SIZE - sizeof(BTreeNodeHeader) - (BTREE_ORDER * 8) - sizeof(uint32_t) - (BTREE_ORDER + 1) * 4];
} BTreeNode;

typedef struct {
    BufferPool* bp;
    uint32_t    root_id;
    uint32_t    num_pages;
} BTree;

BTree* btree_create(BufferPool* bp, uint32_t root_id);
BTree* btree_open(BufferPool* bp, uint32_t root_id);
int btree_insert(BTree* bt, uint64_t key, BTreeValue val);
bool btree_search(BTree* bt, uint64_t key, BTreeValue* out_val);
int btree_delete(BTree* bt, uint64_t key);

typedef struct {
    BTree* bt;
    uint32_t leaf_page;
    int      slot;
    bool     done;
} BTreeScan;

BTreeScan* btree_scan_open(BTree* bt);
bool btree_scan_next(BTreeScan* scan, uint64_t* key, BTreeValue* val);
void btree_scan_close(BTreeScan* scan);

#endif
