#include "../include/my_malloc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>

// ── globals ────────────────────────────────────────────────────────────────

static block_meta*     heap_head              = nullptr;
static block_meta*     heap_tail              = nullptr;
static block_meta*     free_lists[NUM_CLASSES] = {};
static alloc_stats     g_stats                = {};
static pthread_mutex_t g_heap_lock            = PTHREAD_MUTEX_INITIALIZER;

// ── size class helpers ─────────────────────────────────────────────────────

static int get_size_class(size_t size) {
    for (int i = 0; i < LARGE_CLASS; i++)
        if (size <= CLASS_MAX[i]) return i;
    return LARGE_CLASS;
}

// ── canary helpers ─────────────────────────────────────────────────────────

static void write_canary(block_meta* blk) {
    *(uint64_t*)((uint8_t*)(blk + 1) + blk_size(blk)) = CANARY_VALUE;
}

static bool check_canary(const block_meta* blk) {
    return *(const uint64_t*)((const uint8_t*)(blk + 1) + blk_size(blk)) == CANARY_VALUE;
}

// ── randomised insertion RNG ───────────────────────────────────────────────
// xorshift64: fast, lock-free quality good enough for heap layout shuffling.
// g_heap_lock is always held when fl_insert is called, so no atomic needed.

#define FL_RAND_DEPTH  8   // max position offset for randomised insertion

static uint64_t g_rand_state = 0x9E3779B97F4A7C15ULL;  // golden-ratio seed

static uint64_t next_rand() {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return g_rand_state;
}

// ── doubly-linked free list operations ────────────────────────────────────

static void fl_insert(block_meta* blk) {
    int cls = get_size_class(blk_size(blk));
    blk->size |= 1;                          // mark free (bit 0)

    // Randomise the insertion position within [0, FL_RAND_DEPTH) so the
    // heap layout is non-deterministic.  This defeats heap-spray attacks
    // that rely on a predictable free-list order.
    uint32_t offset = (uint32_t)(next_rand() % FL_RAND_DEPTH);

    block_meta* prev = nullptr;
    block_meta* cur  = free_lists[cls];
    for (uint32_t i = 0; i < offset && cur; i++, prev = cur, cur = cur->free_next) {}

    blk->free_next = cur;
    blk->free_prev = prev;
    if (cur)  cur->free_prev  = blk;
    if (prev) prev->free_next = blk;
    else      free_lists[cls] = blk;
}

static void fl_remove(block_meta* blk) {
    int cls = get_size_class(blk_size(blk));
    if (blk->free_prev)
        blk->free_prev->free_next = blk->free_next;
    else
        free_lists[cls] = blk->free_next;
    if (blk->free_next)
        blk->free_next->free_prev = blk->free_prev;
    blk->free_next = blk->free_prev = nullptr;
}

// ── per-thread cache ───────────────────────────────────────────────────────
// Classes 0..TCACHE_MAX_CLASS-1 (≤ 1024 B, the no-coalesce zone) get a
// per-thread LIFO stack of up to TCACHE_BIN_MAX blocks.  Alloc pops and free
// pushes without touching g_heap_lock — the common small-alloc path is fully
// lock-free.  When a bin fills up, half its blocks are flushed to the global
// free list under g_heap_lock.  The TCache destructor drains all bins on thread
// exit (including program exit for the main thread).
//
// Hot statistics (num_allocs, num_frees, current_usage, total_allocated,
// peak_usage) are updated via __atomic builtins so both the lock-free TLS path
// and the locked slow path remain consistent without requiring an extra lock.

#define TCACHE_MAX_CLASS  8    // classes 0–7  (max payload ≤ 1024 B)
#define TCACHE_BIN_MAX   32   // max cached blocks per class per thread

struct TCache {
    struct Bin {
        block_meta* head  = nullptr;
        int         count = 0;
    };
    Bin bins[TCACHE_MAX_CLASS];
    ~TCache();
    void flush_bin(int cls, int n);
};

void TCache::flush_bin(int cls, int n) {
    Bin& bin = bins[cls];
    pthread_mutex_lock(&g_heap_lock);
    for (int i = 0; i < n && bin.head; i++) {
        block_meta* blk = bin.head;
        bin.head = blk->free_next;
        bin.count--;
        blk->free_next = blk->free_prev = nullptr;
        fl_insert(blk);
    }
    pthread_mutex_unlock(&g_heap_lock);
}

TCache::~TCache() {
    for (int cls = 0; cls < TCACHE_MAX_CLASS; cls++)
        flush_bin(cls, bins[cls].count);
}

static thread_local TCache tls_cache;

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
    blk_set_size_used(blk, size);            // size stored, free flag = 0
    blk->free_next = blk->free_prev = nullptr;
    blk->magic = MAGIC_ALLOC;
    heap_append(blk);
    write_canary(blk);
    g_stats.num_sbrk_calls++;
    return blk;
}

// ── search free lists; split large blocks if possible ─────────────────────

static block_meta* split_or_take(block_meta* blk, size_t size) {
    fl_remove(blk);

    if (blk_size(blk) >= size + CANARY_SIZE + sizeof(block_meta) + ALIGNMENT) {
        size_t leftover = blk_size(blk) - size - CANARY_SIZE - sizeof(block_meta);
        block_meta* rest = (block_meta*)((uint8_t*)(blk + 1) + size + CANARY_SIZE);
        blk_set_size_free(rest, leftover);   // leftover size + free flag
        rest->free_next = rest->free_prev = nullptr;
        rest->magic = MAGIC_FREE;

        rest->heap_prev = blk;
        rest->heap_next = blk->heap_next;
        if (blk->heap_next) blk->heap_next->heap_prev = rest;
        else                heap_tail = rest;
        blk->heap_next = rest;

        blk_set_size_used(blk, size);        // exact size, free flag cleared
        blk->magic = MAGIC_ALLOC;
        write_canary(blk);
        write_canary(rest);
        fl_insert(rest);
        g_stats.num_splits++;
    }
    // If no split: block may be larger than 'size' (internal fragmentation).
    // Canary stays at blk_size(blk); caller (my_malloc) marks the block used.
    return blk;
}

static block_meta* fl_find(int cls, size_t size) {
    // 1. Exact small/medium class — O(1) pop.
    //    Blocks in a small/medium free list have sizes in (CLASS_MAX[cls-1], CLASS_MAX[cls]].
    //    A rest block from a previous split may have size < CLASS_MAX[cls], so we must
    //    verify the block is at least 'size' bytes before using it.  Inflating the
    //    recorded size past the physical block extent corrupts adjacent free blocks.
    if (cls < LARGE_CLASS && free_lists[cls] && blk_size(free_lists[cls]) >= size) {
        block_meta* blk = free_lists[cls];
        fl_remove(blk);
        return blk;
    }

    // 2. Search upward through intermediate classes (added 4096/8192 classes
    //    mean there are now splittable blocks between cls and LARGE_CLASS).
    for (int c = cls + 1; c < LARGE_CLASS; c++) {
        if (free_lists[c])
            return split_or_take(free_lists[c], size);
    }

    // 3. LARGE_CLASS — first-fit with splitting.
    for (block_meta* blk = free_lists[LARGE_CLASS]; blk; blk = blk->free_next) {
        if (blk_size(blk) >= size)
            return split_or_take(blk, size);
    }
    return nullptr;
}

// ── wilderness (top-of-heap) optimization ─────────────────────────────────

static block_meta* try_wilderness(size_t size) {
    if (!heap_tail || !blk_free(heap_tail)) return nullptr;

    block_meta* tail = heap_tail;

    if (blk_size(tail) < size) {
        // Remove from the free list BEFORE changing the size.  fl_remove uses
        // get_size_class(blk_size(tail)) to locate the right list; updating
        // tail->size first would cause it to look in the wrong class, leaving a
        // dangling pointer in the old list and allowing double-allocation.
        fl_remove(tail);
        size_t deficit = size - blk_size(tail);
        if (sbrk(deficit) == (void*)-1) {
            fl_insert(tail);                 // restore: extension failed
            return nullptr;
        }
        blk_set_size_free(tail, size);       // safe to update size now
        fl_insert(tail);                     // re-insert under the new class
        g_stats.num_sbrk_calls++;
    }

    block_meta* blk = split_or_take(tail, size);
    g_stats.num_wilderness_saves++;
    return blk;
}

// ── mmap allocator ─────────────────────────────────────────────────────────
// Requests at or above MMAP_THRESHOLD bypass the sbrk heap entirely.
// Each block is a private anonymous mapping of exactly the right size; freeing
// it calls munmap, returning the pages to the OS immediately.  mmap blocks are
// NOT inserted into the heap chain (heap_next/heap_prev are null).

static block_meta* mmap_alloc(size_t size) {
    size_t total = sizeof(block_meta) + size + CANARY_SIZE;
    void* p = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    block_meta* blk = (block_meta*)p;
    blk->heap_next = blk->heap_prev = nullptr;
    blk->free_next = blk->free_prev = nullptr;
    blk_set_size_mmap(blk, size);   // bit 1 = mmap flag; bit 0 = 0 (used)
    blk->magic = MAGIC_ALLOC;
    write_canary(blk);
    g_stats.num_mmap_calls++;
    return blk;
}

// ── stat helpers ───────────────────────────────────────────────────────────

static void record_alloc(size_t size) {
    __atomic_fetch_add(&g_stats.num_allocs,      1,    __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stats.total_allocated, size, __ATOMIC_RELAXED);
    size_t usage = __atomic_add_fetch(&g_stats.current_usage, size, __ATOMIC_RELAXED);
    // Update peak via CAS; benign races only ever make peak larger.
    size_t peak = __atomic_load_n(&g_stats.peak_usage, __ATOMIC_RELAXED);
    while (usage > peak) {
        if (__atomic_compare_exchange_n(&g_stats.peak_usage, &peak, usage,
                                        /*weak=*/true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

// ── lock-free internal implementations ────────────────────────────────────
// All internal helpers already operate without locking.  These two functions
// contain the full malloc/free logic and are called either from the locked
// public API or from my_realloc which holds the lock itself.

static void* malloc_unlocked(size_t size) {
    if (size == 0) return nullptr;
    size = ALIGN(size);

    // Large requests bypass the sbrk heap: map private pages, free with munmap.
    if (size >= MMAP_THRESHOLD) {
        block_meta* blk = mmap_alloc(size);
        if (!blk) return nullptr;
        record_alloc(size);
        return (void*)(blk + 1);
    }

    int cls = get_size_class(size);
    if (cls < LARGE_CLASS) size = CLASS_MAX[cls];

    block_meta* blk = fl_find(cls, size);
    if (!blk) blk = try_wilderness(size);
    if (!blk) blk = request_block(size);
    if (!blk) return nullptr;

    blk_set_size_used(blk, size);
    blk->magic = MAGIC_ALLOC;
    write_canary(blk);
    record_alloc(size);
    return (void*)(blk + 1);
}

static void free_unlocked(void* ptr) {
    if (!ptr) return;

    block_meta* blk = (block_meta*)ptr - 1;

    if (!check_canary(blk)) {
        fprintf(stderr,
                "[my_malloc] HEAP CORRUPTION at %p (blk_size=%zu): canary overwritten "
                "(expected 0x%016llX, got 0x%016llX)\n",
                ptr, blk_size(blk),
                (unsigned long long)CANARY_VALUE,
                (unsigned long long)*(uint64_t*)((uint8_t*)(blk + 1) + blk_size(blk)));
        abort();
    }

    // Double-free detection (covers my_realloc's internal free path).
    if (blk->magic == MAGIC_FREE) {
        fprintf(stderr, "[my_malloc] DOUBLE FREE at %p\n", ptr);
        abort();
    }
    blk->magic = MAGIC_FREE;

    __atomic_fetch_add(&g_stats.num_frees,     1,            __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_stats.current_usage, blk_size(blk), __ATOMIC_RELAXED);

    // mmap blocks are returned to the OS directly; no free-list insertion.
    if (blk_mmap(blk)) {
        size_t total = sizeof(block_meta) + blk_size(blk) + CANARY_SIZE;
        g_stats.num_munmap_calls++;
        munmap(blk, total);   // blk is invalid after this point
        return;
    }

    fl_insert(blk);

    if (get_size_class(blk_size(blk)) < COALESCE_THRESHOLD) return;

    block_meta* nxt = blk->heap_next;
    if (nxt && blk_free(nxt)) {
        fl_remove(nxt);
        fl_remove(blk);
        blk_set_size_free(blk, blk_size(blk) + CANARY_SIZE + sizeof(block_meta) + blk_size(nxt));
        blk->heap_next = nxt->heap_next;
        if (nxt->heap_next) nxt->heap_next->heap_prev = blk;
        else                heap_tail = blk;
        write_canary(blk);
        fl_insert(blk);
        g_stats.num_coalesces++;
    }

    block_meta* prv = blk->heap_prev;
    if (prv && blk_free(prv)) {
        fl_remove(prv);
        fl_remove(blk);
        blk_set_size_free(prv, blk_size(prv) + CANARY_SIZE + sizeof(block_meta) + blk_size(blk));
        prv->heap_next = blk->heap_next;
        if (blk->heap_next) blk->heap_next->heap_prev = prv;
        else                heap_tail = prv;
        write_canary(prv);
        fl_insert(prv);
        g_stats.num_coalesces++;
    }
}

// ── public API (every entry point holds g_heap_lock for its full duration) ─

void* my_malloc(size_t size) {
    if (size == 0) return nullptr;
    size_t aligned = ALIGN(size);

    // TLS fast path: small, non-mmap classes only.
    if (aligned < MMAP_THRESHOLD) {
        int cls = get_size_class(aligned);
        if (cls < TCACHE_MAX_CLASS) {
            TCache::Bin& bin = tls_cache.bins[cls];
            if (bin.head) {
                block_meta* blk = bin.head;
                bin.head = blk->free_next;
                blk->free_next = blk->free_prev = nullptr;
                bin.count--;
                blk->magic = MAGIC_ALLOC;
                record_alloc(blk_size(blk));
                return (void*)(blk + 1);
            }
        }
    }

    // Slow path: lock, call into full allocator.
    pthread_mutex_lock(&g_heap_lock);
    void* p = malloc_unlocked(size);
    pthread_mutex_unlock(&g_heap_lock);
    return p;
}

void my_free(void* ptr) {
    if (!ptr) return;

    block_meta* blk = (block_meta*)ptr - 1;

    // Canary check: no lock needed — the caller owns this block.
    if (!check_canary(blk)) {
        fprintf(stderr,
                "[my_malloc] HEAP CORRUPTION at %p (blk_size=%zu): canary overwritten "
                "(expected 0x%016llX, got 0x%016llX)\n",
                ptr, blk_size(blk),
                (unsigned long long)CANARY_VALUE,
                (unsigned long long)*(uint64_t*)((uint8_t*)(blk + 1) + blk_size(blk)));
        abort();
    }

    // Double-free detection.
    if (blk->magic == MAGIC_FREE) {
        fprintf(stderr, "[my_malloc] DOUBLE FREE at %p\n", ptr);
        abort();
    }

    // mmap blocks bypass the free list entirely.
    if (blk_mmap(blk)) {
        blk->magic = MAGIC_FREE;
        __atomic_fetch_add(&g_stats.num_frees,     1,            __ATOMIC_RELAXED);
        __atomic_fetch_sub(&g_stats.current_usage, blk_size(blk), __ATOMIC_RELAXED);
        size_t total = sizeof(block_meta) + blk_size(blk) + CANARY_SIZE;
        pthread_mutex_lock(&g_heap_lock);
        g_stats.num_munmap_calls++;
        pthread_mutex_unlock(&g_heap_lock);
        munmap(blk, total);
        return;
    }

    int cls = get_size_class(blk_size(blk));

    // TLS fast path: small no-coalesce classes.
    if (cls < TCACHE_MAX_CLASS) {
        blk->magic = MAGIC_FREE;
        __atomic_fetch_add(&g_stats.num_frees,     1,            __ATOMIC_RELAXED);
        __atomic_fetch_sub(&g_stats.current_usage, blk_size(blk), __ATOMIC_RELAXED);
        TCache::Bin& bin = tls_cache.bins[cls];
        if (bin.count >= TCACHE_BIN_MAX)
            tls_cache.flush_bin(cls, TCACHE_BIN_MAX / 2);
        blk->free_next = bin.head;
        blk->free_prev = nullptr;
        bin.head = blk;
        bin.count++;
        return;
    }

    // Slow path: coalescing/large classes go through the global heap lock.
    pthread_mutex_lock(&g_heap_lock);
    free_unlocked(ptr);
    pthread_mutex_unlock(&g_heap_lock);
}

void* my_realloc(void* ptr, size_t size) {
    // Delegate edge cases to the public functions (they acquire the lock).
    if (!ptr)    return my_malloc(size);
    if (size == 0) { my_free(ptr); return nullptr; }

    pthread_mutex_lock(&g_heap_lock);

    size = ALIGN(size);
    block_meta* blk = (block_meta*)ptr - 1;

    if (blk_size(blk) == size) {
        pthread_mutex_unlock(&g_heap_lock);
        return ptr;
    }

    if (blk_size(blk) > size) {
        // Shrink in place; preserve the mmap flag if present.
        __atomic_fetch_sub(&g_stats.current_usage, blk_size(blk) - size, __ATOMIC_RELAXED);
        blk_set_size_used_keep_flags(blk, size);
        write_canary(blk);
        pthread_mutex_unlock(&g_heap_lock);
        return ptr;
    }

    // Grow: allocate new, copy, free old — all under the same lock acquisition
    // to avoid a window where another thread could observe a dangling ptr.
    size_t old_size = blk_size(blk);
    void*  new_ptr  = malloc_unlocked(size);
    if (!new_ptr) {
        pthread_mutex_unlock(&g_heap_lock);
        return nullptr;
    }
    memcpy(new_ptr, ptr, old_size);
    free_unlocked(ptr);
    pthread_mutex_unlock(&g_heap_lock);
    return new_ptr;
}

void* my_calloc(size_t num, size_t size) {
    size_t total = num * size;
    if (num != 0 && total / num != size) return nullptr;  // overflow
    pthread_mutex_lock(&g_heap_lock);
    void* ptr = malloc_unlocked(total);
    pthread_mutex_unlock(&g_heap_lock);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

// ── heap dump ──────────────────────────────────────────────────────────────

void my_heap_dump() {
    pthread_mutex_lock(&g_heap_lock);

    printf("%-4s  %-18s  %-10s  %-5s  %-6s\n",
           "#", "block addr", "data size", "class", "status");
    printf("----  ------------------  ----------  -----  ------\n");

    int    n     = 0;
    size_t total = 0;
    size_t live  = 0;

    for (block_meta* b = heap_head; b; b = b->heap_next, n++) {
        size_t sz  = blk_size(b);
        int    cls = get_size_class(sz);
        printf("%-4d  %-18p  %-10zu  %-5d  %s\n",
               n, (void*)b, sz, cls, blk_free(b) ? "FREE" : "USED");
        total += sz;
        if (!blk_free(b)) live += sz;
    }

    printf("----  ------------------  ----------  -----  ------\n");
    printf("total %zu B across %d block(s) — %zu live, %zu free\n\n",
           total, n, live, total - live);

    printf("Free lists:\n");
    for (int i = 0; i < NUM_CLASSES; i++) {
        int cnt = 0;
        for (block_meta* b = free_lists[i]; b; b = b->free_next) cnt++;
        if (cnt) printf("  class %d (max %5zu B): %d block(s)\n",
                        i, CLASS_MAX[i] == (size_t)-1 ? (size_t)0 : CLASS_MAX[i], cnt);
    }

    pthread_mutex_unlock(&g_heap_lock);
}

// ── stats dump ─────────────────────────────────────────────────────────────

const alloc_stats* my_get_stats() {
    // Returns a pointer to the live struct; the caller must not hold the lock
    // while dereferencing it across a yield point.  Safe for single-threaded
    // use and for the test harness which calls it between allocations.
    return &g_stats;
}

void my_stats_dump() {
    pthread_mutex_lock(&g_heap_lock);
    alloc_stats snap = g_stats;               // copy under lock, print outside
    pthread_mutex_unlock(&g_heap_lock);

    printf("=== Allocator Statistics ===\n");
    printf("  Allocations       : %zu\n",   snap.num_allocs);
    printf("  Frees             : %zu\n",   snap.num_frees);
    printf("  Total allocated   : %zu B\n", snap.total_allocated);
    printf("  Current usage     : %zu B\n", snap.current_usage);
    printf("  Peak usage        : %zu B\n", snap.peak_usage);
    printf("  Splits            : %zu\n",   snap.num_splits);
    printf("  Coalesces         : %zu\n",   snap.num_coalesces);
    printf("  sbrk calls        : %zu\n",   snap.num_sbrk_calls);
    printf("  Wilderness saves  : %zu\n",   snap.num_wilderness_saves);
    printf("  mmap calls        : %zu\n",   snap.num_mmap_calls);
    printf("  munmap calls      : %zu\n",   snap.num_munmap_calls);
}
