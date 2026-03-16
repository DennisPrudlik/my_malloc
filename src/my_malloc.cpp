#include "../include/my_malloc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── globals ────────────────────────────────────────────────────────────────

static block_meta*  heap_head             = nullptr;
static block_meta*  heap_tail             = nullptr;
static block_meta*  free_lists[NUM_CLASSES] = {};
static alloc_stats  g_stats               = {};

// ── size class helpers ─────────────────────────────────────────────────────

static int get_size_class(size_t size) {
    for (int i = 0; i < LARGE_CLASS; i++)
        if (size <= CLASS_MAX[i]) return i;
    return LARGE_CLASS;
}

// ── canary helpers ─────────────────────────────────────────────────────────

static void write_canary(block_meta* blk) {
    *(uint64_t*)((uint8_t*)(blk + 1) + blk->size) = CANARY_VALUE;
}

static bool check_canary(const block_meta* blk) {
    return *(const uint64_t*)((const uint8_t*)(blk + 1) + blk->size) == CANARY_VALUE;
}

// ── doubly-linked free list operations ────────────────────────────────────

static void fl_insert(block_meta* blk) {
    int cls = blk->size_class;
    blk->free_next = free_lists[cls];
    blk->free_prev = nullptr;
    if (free_lists[cls])
        free_lists[cls]->free_prev = blk;
    free_lists[cls] = blk;
}

static void fl_remove(block_meta* blk) {
    int cls = blk->size_class;
    if (blk->free_prev)
        blk->free_prev->free_next = blk->free_next;
    else
        free_lists[cls] = blk->free_next;
    if (blk->free_next)
        blk->free_next->free_prev = blk->free_prev;
    blk->free_next = blk->free_prev = nullptr;
}

// ── physical heap list helpers ─────────────────────────────────────────────

static void heap_append(block_meta* blk) {
    blk->heap_prev = heap_tail;
    blk->heap_next = nullptr;
    if (heap_tail) heap_tail->heap_next = blk;
    else           heap_head = blk;
    heap_tail = blk;
}

// ── request a new block from the OS ───────────────────────────────────────

static block_meta* request_block(size_t size) {
    block_meta* blk = (block_meta*)sbrk(0);
    if (sbrk(sizeof(block_meta) + size + CANARY_SIZE) == (void*)-1)
        return nullptr;
    blk->size       = size;
    blk->size_class = get_size_class(size);
    blk->free       = false;
    blk->free_next  = blk->free_prev = nullptr;
    heap_append(blk);
    write_canary(blk);
    g_stats.num_sbrk_calls++;
    return blk;
}

// ── search free lists; split large blocks if possible ─────────────────────

static block_meta* split_or_take(block_meta* blk, size_t size) {
    fl_remove(blk);

    if (blk->size >= size + CANARY_SIZE + sizeof(block_meta) + ALIGNMENT) {
        size_t leftover = blk->size - size - CANARY_SIZE - sizeof(block_meta);
        block_meta* rest = (block_meta*)((uint8_t*)(blk + 1) + size + CANARY_SIZE);
        rest->size       = leftover;
        rest->size_class = get_size_class(leftover);
        rest->free       = true;
        rest->free_next  = rest->free_prev = nullptr;

        rest->heap_prev = blk;
        rest->heap_next = blk->heap_next;
        if (blk->heap_next) blk->heap_next->heap_prev = rest;
        else                heap_tail = rest;
        blk->heap_next = rest;

        blk->size       = size;
        blk->size_class = get_size_class(size);

        write_canary(blk);
        write_canary(rest);
        fl_insert(rest);
        g_stats.num_splits++;
    }
    // If no split: block may be larger than 'size' (internal fragmentation).
    // size_class is corrected by the caller; canary stays at blk->size.
    return blk;
}

static block_meta* fl_find(int cls, size_t size) {
    // 1. Exact small/medium class — O(1) pop (all blocks are CLASS_MAX[cls]).
    if (cls < LARGE_CLASS && free_lists[cls]) {
        block_meta* blk = free_lists[cls];
        fl_remove(blk);
        return blk;
    }

    // 2. Large class — first-fit with splitting.
    //    Also serves small-class requests whose exact list is empty (coalesced blocks
    //    land here and can be split back into the needed size).
    for (block_meta* blk = free_lists[LARGE_CLASS]; blk; blk = blk->free_next) {
        if (blk->size >= size)
            return split_or_take(blk, size);
    }
    return nullptr;
}

// ── wilderness (top-of-heap) optimization ─────────────────────────────────
// If the last heap block is free, extend it in-place instead of sbrk-ing a
// whole new chunk.  Returns the (still free) tail block on success.

static block_meta* try_wilderness(size_t size) {
    if (!heap_tail || !heap_tail->free) return nullptr;

    block_meta* tail = heap_tail;

    // Extend the tail block if it's too small.
    if (tail->size < size) {
        size_t deficit = size - tail->size;
        if (sbrk(deficit) == (void*)-1) return nullptr;
        tail->size = size;
        g_stats.num_sbrk_calls++;
    }

    // split_or_take handles both the split and the fl_remove.
    // It may update heap_tail if a remainder is created.
    block_meta* blk = split_or_take(tail, size);
    g_stats.num_wilderness_saves++;
    return blk;
}

// ── stat helpers ───────────────────────────────────────────────────────────

static void record_alloc(size_t size) {
    g_stats.num_allocs++;
    g_stats.total_allocated += size;
    g_stats.current_usage   += size;
    if (g_stats.current_usage > g_stats.peak_usage)
        g_stats.peak_usage = g_stats.current_usage;
}

// ── public API ─────────────────────────────────────────────────────────────

void* my_malloc(size_t size) {
    if (size == 0) return nullptr;
    size = ALIGN(size);

    int cls = get_size_class(size);
    // Round small requests up to the canonical class size so every block in a
    // small class has identical capacity (enabling O(1) free-list pop).
    if (cls < LARGE_CLASS) size = CLASS_MAX[cls];

    block_meta* blk = fl_find(cls, size);

    if (!blk) blk = try_wilderness(size);

    if (!blk) blk = request_block(size);

    if (!blk) return nullptr;

    blk->free       = false;
    blk->size_class = cls;
    write_canary(blk);
    record_alloc(blk->size);
    return (void*)(blk + 1);
}

void my_free(void* ptr) {
    if (!ptr) return;

    block_meta* blk = (block_meta*)ptr - 1;

    // ── guard band check ──────────────────────────────────────────────────
    if (!check_canary(blk)) {
        fprintf(stderr,
                "[my_malloc] HEAP CORRUPTION at %p: canary overwritten "
                "(expected 0x%016llX, got 0x%016llX)\n",
                ptr,
                (unsigned long long)CANARY_VALUE,
                (unsigned long long)*(uint64_t*)((uint8_t*)(blk + 1) + blk->size));
        abort();
    }

    g_stats.num_frees++;
    g_stats.current_usage -= blk->size;
    blk->free = true;
    fl_insert(blk);

    // ── coalesce with physical successor ─────────────────────────────────
    block_meta* nxt = blk->heap_next;
    if (nxt && nxt->free) {
        fl_remove(nxt);
        fl_remove(blk);
        blk->size      += CANARY_SIZE + sizeof(block_meta) + nxt->size;
        blk->heap_next  = nxt->heap_next;
        if (nxt->heap_next) nxt->heap_next->heap_prev = blk;
        else                heap_tail = blk;
        blk->size_class = get_size_class(blk->size);
        write_canary(blk);
        fl_insert(blk);
        g_stats.num_coalesces++;
    }

    // ── coalesce with physical predecessor ───────────────────────────────
    block_meta* prv = blk->heap_prev;
    if (prv && prv->free) {
        fl_remove(prv);
        fl_remove(blk);
        prv->size      += CANARY_SIZE + sizeof(block_meta) + blk->size;
        prv->heap_next  = blk->heap_next;
        if (blk->heap_next) blk->heap_next->heap_prev = prv;
        else                heap_tail = prv;
        prv->size_class = get_size_class(prv->size);
        write_canary(prv);
        fl_insert(prv);
        g_stats.num_coalesces++;
    }
}

void* my_realloc(void* ptr, size_t size) {
    if (!ptr)    return my_malloc(size);
    if (size == 0) { my_free(ptr); return nullptr; }

    size = ALIGN(size);
    block_meta* blk = (block_meta*)ptr - 1;

    if (blk->size == size) return ptr;

    if (blk->size > size) {
        // Shrink in place — move canary to new offset.
        g_stats.current_usage -= (blk->size - size);
        blk->size       = size;
        blk->size_class = get_size_class(size);
        write_canary(blk);
        return ptr;
    }

    // Grow: allocate, copy, free.
    void* new_ptr = my_malloc(size);
    if (!new_ptr) return nullptr;
    memcpy(new_ptr, ptr, blk->size);
    my_free(ptr);
    return new_ptr;
}

void* my_calloc(size_t num, size_t size) {
    size_t total = num * size;
    if (num != 0 && total / num != size) return nullptr;  // overflow
    void* ptr = my_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

// ── heap dump ──────────────────────────────────────────────────────────────

void my_heap_dump() {
    printf("%-4s  %-18s  %-10s  %-5s  %-6s\n",
           "#", "block addr", "data size", "class", "status");
    printf("----  ------------------  ----------  -----  ------\n");

    int    n          = 0;
    size_t total      = 0;
    size_t live       = 0;

    for (block_meta* b = heap_head; b; b = b->heap_next, n++) {
        printf("%-4d  %-18p  %-10zu  %-5d  %s\n",
               n, (void*)b, b->size, b->size_class, b->free ? "FREE" : "USED");
        total += b->size;
        if (!b->free) live += b->size;
    }

    printf("----  ------------------  ----------  -----  ------\n");
    printf("total %zu B across %d block(s) — %zu live, %zu free\n\n",
           total, n, live, total - live);

    // Per-class free list summary.
    printf("Free lists:\n");
    for (int i = 0; i < NUM_CLASSES; i++) {
        int cnt = 0;
        for (block_meta* b = free_lists[i]; b; b = b->free_next) cnt++;
        if (cnt) printf("  class %d (max %5zu B): %d block(s)\n",
                        i, CLASS_MAX[i] == (size_t)-1 ? (size_t)0 : CLASS_MAX[i], cnt);
    }
}

// ── stats dump ─────────────────────────────────────────────────────────────

const alloc_stats* my_get_stats() { return &g_stats; }

void my_stats_dump() {
    printf("=== Allocator Statistics ===\n");
    printf("  Allocations       : %zu\n",   g_stats.num_allocs);
    printf("  Frees             : %zu\n",   g_stats.num_frees);
    printf("  Total allocated   : %zu B\n", g_stats.total_allocated);
    printf("  Current usage     : %zu B\n", g_stats.current_usage);
    printf("  Peak usage        : %zu B\n", g_stats.peak_usage);
    printf("  Splits            : %zu\n",   g_stats.num_splits);
    printf("  Coalesces         : %zu\n",   g_stats.num_coalesces);
    printf("  sbrk calls        : %zu\n",   g_stats.num_sbrk_calls);
    printf("  Wilderness saves  : %zu\n",   g_stats.num_wilderness_saves);
}
