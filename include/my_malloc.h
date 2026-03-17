#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <cstddef>
#include <cstdint>

#define ALIGNMENT    8
#define ALIGN(size)  (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define CANARY_VALUE     ((uint64_t)0xDEADBEEFDEADBEEFULL)
#define CANARY_SIZE      sizeof(uint64_t)

// Magic values stored in block_meta::magic for double-free detection.
#define MAGIC_ALLOC      ((uint64_t)0xA110CA7ED00DBEEFULL)  // block is live
#define MAGIC_FREE       ((uint64_t)0xF4EEF4EEF4EEF4EEULL)  // block is on a free list / TLS cache

// Requests at or above this threshold bypass sbrk and use mmap/munmap so
// that freed large blocks are returned to the OS immediately.
#define MMAP_THRESHOLD   (128 * 1024)   // 128 KB

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
//   [ block_meta (48 B) ][ user payload (size B) ][ canary (8 B) ]
//
// Packing: status flags are stored in the low bits of the 'size' field.
//   bit 0 : free flag  (1 = block is on a free list)
//   bit 1 : mmap flag  (1 = block was obtained via mmap, not sbrk)
// Both flags are safe because ALIGNMENT == 8, so all user sizes are
// multiples of 8 and bits 0–2 are otherwise always zero.
typedef struct block_meta {
    struct block_meta* heap_next;  // next block in physical heap order (sbrk only)
    struct block_meta* heap_prev;  // prev block in physical heap order (sbrk only)
    struct block_meta* free_next;  // next in segregated free list
    struct block_meta* free_prev;  // prev in segregated free list
    size_t    size;                // bits [3..]: aligned user payload; bit 0: free; bit 1: mmap
    uint64_t  magic;               // MAGIC_ALLOC when live, MAGIC_FREE when freed
} block_meta;

// Packed-field accessors — all callers must use these instead of blk->size.
static inline size_t blk_size(const block_meta* b) { return b->size & ~(size_t)3; }
static inline bool   blk_free(const block_meta* b) { return (b->size & 1) != 0; }
static inline bool   blk_mmap(const block_meta* b) { return (b->size & 2) != 0; }
// Set size + flags together (avoids partial writes).
static inline void   blk_set_size_used(block_meta* b, size_t s) { b->size = s;         }
static inline void   blk_set_size_free(block_meta* b, size_t s) { b->size = s | 1;     }
static inline void   blk_set_size_mmap(block_meta* b, size_t s) { b->size = s | 2;     }
// Shrink helpers that preserve the mmap flag.
static inline void   blk_set_size_used_keep_flags(block_meta* b, size_t s) {
    b->size = s | (b->size & 2);   // keep mmap flag, clear free flag
}
// Toggle free flag only, preserving size and mmap bits.
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
    size_t num_sbrk_calls;        // times the OS was asked for more memory (sbrk)
    size_t num_wilderness_saves;  // times the free tail block was extended/reused
    size_t num_mmap_calls;        // large allocations served via mmap
    size_t num_munmap_calls;      // large blocks returned to OS via munmap
} alloc_stats;

void*              my_malloc(size_t size);
void               my_free(void* ptr);
void*              my_realloc(void* ptr, size_t size);
void*              my_calloc(size_t num, size_t size);
void               my_heap_dump();
const alloc_stats* my_get_stats();
void               my_stats_dump();

#endif
