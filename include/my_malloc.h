#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <cstddef>
#include <cstdint>

#define ALIGNMENT    8
#define ALIGN(size)  (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define CANARY_VALUE  ((uint64_t)0xDEADBEEFDEADBEEFULL)
#define CANARY_SIZE   sizeof(uint64_t)

#define NUM_CLASSES  10
#define LARGE_CLASS  (NUM_CLASSES - 1)

// Maximum user payload size for each size class (last = uncapped / large).
static const size_t CLASS_MAX[NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, (size_t)-1
};

// Block metadata sits immediately before every user allocation.
// Memory layout of one block:
//   [ block_meta (48 B) ][ user payload (size B) ][ canary (8 B) ]
typedef struct block_meta {
    struct block_meta* heap_next;  // next block in physical heap order
    struct block_meta* heap_prev;  // prev block in physical heap order
    struct block_meta* free_next;  // next in segregated free list
    struct block_meta* free_prev;  // prev in segregated free list
    size_t size;                   // aligned user payload (NOT including canary)
    int    size_class;             // index into free_lists[]
    bool   free;
} block_meta;

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
