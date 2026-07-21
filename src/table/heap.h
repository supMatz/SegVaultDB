#ifndef HEAP_H
#define HEAP_H

#include "../../include/common.h"
#include "../storage/buffer_pool.h"
#include "../catalog/schema.h"
#include "tuple.h"

typedef struct {
    BufferPool* bp;
    uint32_t    root_page_id;
    uint32_t    num_pages;
} HeapFile;

HeapFile* heap_open(BufferPool* bp, uint32_t root_page_id, uint32_t num_pages);
int heap_insert(HeapFile* hf, Tuple* t, TableSchema* schema, RID* out_rid);
Tuple* heap_get(HeapFile* hf, RID rid, TableSchema* schema);
int heap_delete(HeapFile* hf, RID rid);
int heap_update(HeapFile* hf, RID rid, Tuple* t, TableSchema* schema);

typedef struct {
    HeapFile* hf;
    TableSchema* schema;
    uint32_t current_page;
    uint16_t current_slot;
    bool done;
} HeapScan;

HeapScan* heap_scan_open(HeapFile* hf, TableSchema* schema);
Tuple* heap_scan_next(HeapScan* scan, RID* out_rid);
void heap_scan_close(HeapScan* scan);

#endif
