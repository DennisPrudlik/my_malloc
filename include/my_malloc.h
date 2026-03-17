#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <cstddef>
#include <cstdint>

#define ALIGNMENT    8
#define ALIGN(size)  (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define CANARY_VALUE  ((uint64_t)0xDEADBEEFDEADBEEFULL)
#define CANARY_SIZE   sizeof(uint64_t)

#define NUM_CLASSES  12
#define LARGE_CLASS  (NUM_CLASSES - 1)

// Maximum user payload size for each size class (last = uncapped / large).
// Two new classes (4096, 8192) reduce requests that fall into the slow LARGE_CLASS
// first-fit path.
static const size_t CLASS_MAX[NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, (size_t)-1
};

// Classes strictly below this index are never coalesced on free.
// Keeping small free blocks in their exact-size lists means a same-size
// reallocation gets an O(1) pop instead of paying a split cost.
// Must be ≤ LARGE_CLASS.  Classes at or above this threshold still coalesce
// so that larger blocks can be reclaimed and split for varied requests.
#define COALESCE_THRESHOLD  8   // = first class whose CLASS_MAX ≥ 2048 B

// Block metadata sits immediately before every user allocation.
// Memory layout:
//   [ block_meta (40 B) ][ user payload (size B) ][ canary (8 B) ]
//
// Packing: the free flag is stored in bit 0 of the 'size' field.
// This is safe because ALIGNMENT == 8, so all user sizes are multiples of 8
// and bit 0 is otherwise always zero.  The size_class field is dropped and
// recomputed on demand via get_size_class(), which is a short linear scan.
// Net saving: 8 B per block (48 B → 40 B) improving cache density.
typedef struct block_meta {
    struct block_meta* heap_next;  // next block in physical heap order
    struct block_meta* heap_prev;  // prev block in physical heap order
    struct block_meta* free_next;  // next in segregated free list
    struct block_meta* free_prev;  // prev in segregated free list
    size_t size;   // bits [3..]: aligned user payload; bit 0: free flag
} block_meta;

// Packed-field accessors — all callers must use these instead of blk->size.
static inline size_t blk_size(const block_meta* b) { return b->size & ~(size_t)1; }
static inline bool   blk_free(const block_meta* b) { return (b->size & 1) != 0; }
// Set both size and flag together (avoids two writes).
static inline void   blk_set_size_used(block_meta* b, size_t s) { b->size = s;      }
static inline void   blk_set_size_free(block_meta* b, size_t s) { b->size = s | 1;  }
// Toggle flag only, preserving the size bits.
static inline void   blk_mark_used(block_meta* b) { b->size &= ~(size_t)1; }

// Global allocator statistics.
typedef struct alloc_stats {
    size_t total_allocated;       // cumulative bytes ever given to users
    size_t current_usage;         // live user bytes right now
    size_t peak_usage;            // high-water mark of current_usage
    size_t num_allocs;            // total my_malloc / my_calloc calls
    size_t num_frees;             // total my_free calls
    size_t num_splits;            // large block splits performed
    size_t num_coalesces;         // merge operations performed
    size_t num_sbrk_calls;        // times the OS was asked for more memory
    size_t num_wilderness_saves;  // times the free tail block was extended/reused
} alloc_stats;

void*              my_malloc(size_t size);
void               my_free(void* ptr);
void*              my_realloc(void* ptr, size_t size);
void*              my_calloc(size_t num, size_t size);
void               my_heap_dump();
const alloc_stats* my_get_stats();
void               my_stats_dump();

#endif
