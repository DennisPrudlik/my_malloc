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

// sbrk tracking: set/updated under g_heap_lock; read lock-free in is_slab_ptr.
static void* g_sbrk_base = nullptr;
static void* g_sbrk_end  = nullptr;

// Global pool of non-full slabs returned by threads or not yet handed out.
// Accessed under g_heap_lock only.
static slab_meta* g_partial_slabs[NUM_SLAB_CLASSES] = {};

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

// ── LARGE_CLASS bitmap free list ───────────────────────────────────────────
// LARGE_CLASS blocks are subdivided into 64 size buckets (bucket b covers
// sizes in [b*2048, (b+1)*2048)).  A 64-bit bitmap tracks non-empty buckets,
// enabling O(64) worst-case search via __builtin_ctzll instead of O(n).
// For buckets strictly above the target, the first block is always big enough.
// All operations are called with g_heap_lock held.

#define LARGE_BUCKET_COUNT  64
#define LARGE_BUCKET_SHIFT  11     // 1 << 11 = 2048

static block_meta* large_buckets[LARGE_BUCKET_COUNT] = {};
static uint64_t    large_bitmap                       = 0;

static inline int large_bucket_of(size_t sz) {
    int b = (int)(sz >> LARGE_BUCKET_SHIFT);
    return b < LARGE_BUCKET_COUNT ? b : LARGE_BUCKET_COUNT - 1;
}

static void large_insert(block_meta* blk) {
    int b = large_bucket_of(blk_size(blk));
    blk->free_next = large_buckets[b];
    blk->free_prev = nullptr;
    if (large_buckets[b]) large_buckets[b]->free_prev = blk;
    large_buckets[b] = blk;
    large_bitmap |= (uint64_t)1 << b;
}

static void large_remove(block_meta* blk) {
    int b = large_bucket_of(blk_size(blk));
    if (blk->free_prev) blk->free_prev->free_next = blk->free_next;
    else                large_buckets[b] = blk->free_next;
    if (blk->free_next) blk->free_next->free_prev = blk->free_prev;
    blk->free_next = blk->free_prev = nullptr;
    if (!large_buckets[b]) large_bitmap &= ~((uint64_t)1 << b);
}

// ── doubly-linked free list operations ────────────────────────────────────

static void fl_insert(block_meta* blk) {
    int cls = get_size_class(blk_size(blk));
    blk->size |= 1;                          // mark free (bit 0)

    // LARGE_CLASS uses bitmap-indexed buckets; skip the random-walk path.
    if (cls == LARGE_CLASS) {
        large_insert(blk);
        return;
    }

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
    if (cls == LARGE_CLASS) {
        large_remove(blk);
        return;
    }
    if (blk->free_prev)
        blk->free_prev->free_next = blk->free_next;
    else
        free_lists[cls] = blk->free_next;
    if (blk->free_next)
        blk->free_next->free_prev = blk->free_prev;
    blk->free_next = blk->free_prev = nullptr;
}

// ── per-thread cache ───────────────────────────────────────────────────────
// Two-level per-thread cache for all classes below LARGE_CLASS.
//
//  hot bin   (bins[]):     fast LIFO, cap = TCACHE_BIN_MAX (64 blocks).
//  overflow  (overflow[]): second-level LIFO, larger cap (class-scaled).
//                          Absorbs hot-bin overflow without acquiring any lock.
//
// Free path:
//   1. Push to hot bin.
//   2. If hot bin full  → move half to overflow  (no lock).
//   3. If overflow full → flush half to global    (lock, infrequent).
//
// Alloc path:
//   1. Pop from hot bin (no lock).
//   2. If hot bin empty → drain overflow to hot bin (no lock).
//   3. If overflow empty → refill from global in one lock acquisition.
//
// Result: same-thread alloc/free cycles are fully lock-free once warm.
// Cross-thread frees land in the freeing thread's cache (correct: any thread
// may recycle any block for allocations of the right size class).

#define TCACHE_MAX_CLASS  LARGE_CLASS  // all classes below LARGE_CLASS (≤ 64 KB)
#define TCACHE_BIN_MAX   64            // hot-bin cap (blocks per class)

// Overflow cap scales with class size to bound per-thread memory.
//   cls 0–7  (≤  1 KB): 512 blocks  → max   512 KB
//   cls 8–10 (≤  8 KB):  64 blocks  → max   512 KB
//   cls 11–13(≤ 64 KB):   8 blocks  → max   512 KB
static inline int tcache_overflow_max(int cls) {
    if (cls < 8)  return 512;
    if (cls < 11) return 64;
    return 8;
}

struct TCache {
    struct Bin {
        block_meta* head  = nullptr;
        int         count = 0;
    };
    Bin bins[TCACHE_MAX_CLASS];      // hot bins
    Bin overflow[TCACHE_MAX_CLASS];  // warm overflow (per-thread, no lock)
    ~TCache();
    void flush_to_overflow(int cls, int n);   // hot  → overflow (no lock)
    void drain_overflow   (int cls, int n);   // overflow → hot  (no lock)
    void flush_bin        (int cls, int n);   // overflow → global (lock, last resort)
    void refill_bin       (int cls, int n);   // global → hot (lock, cold start)
};

// Move n blocks from hot bin to overflow (no lock).
void TCache::flush_to_overflow(int cls, int n) {
    Bin& src = bins[cls];
    Bin& dst = overflow[cls];
    for (int i = 0; i < n && src.head; i++) {
        block_meta* blk = src.head;
        src.head = blk->free_next;
        src.count--;
        blk->free_next = dst.head;
        blk->free_prev = nullptr;
        dst.head = blk;
        dst.count++;
    }
}

// Move n blocks from overflow to hot bin (no lock).
void TCache::drain_overflow(int cls, int n) {
    Bin& src = overflow[cls];
    Bin& dst = bins[cls];
    for (int i = 0; i < n && src.head; i++) {
        block_meta* blk = src.head;
        src.head = blk->free_next;
        src.count--;
        blk->free_next = dst.head;
        blk->free_prev = nullptr;
        dst.head = blk;
        dst.count++;
    }
}

// Spill n blocks from overflow to the global free list (lock held here).
void TCache::flush_bin(int cls, int n) {
    Bin& bin = overflow[cls];   // spill from overflow, not hot bin
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
    for (int cls = 0; cls < TCACHE_MAX_CLASS; cls++) {
        // Consolidate: move hot into overflow, then flush everything to global.
        flush_to_overflow(cls, bins[cls].count);
        flush_bin(cls, overflow[cls].count);
    }
}

static thread_local TCache tls_cache;

// ── slab allocator ─────────────────────────────────────────────────────────
// Classes 0–NUM_SLAB_CLASSES-1 (≤ 256 B) are served from page-aligned slabs.
// Each slab is a 4 KB mmap page.  slab_meta occupies the first 64 bytes;
// fixed-size object slots immediately follow (first slot at slab_first_offset).
//
// Per-thread active slab (tl_active_slab[]):
//   Alloc: pop from slab->freelist (no lock, no atomics).
//   Same-thread free: push to slab->freelist (no lock, no atomics).
//   Cross-thread free: CAS-push to slab->remote_free (lock-free).
//   On alloc miss: drain remote_free into freelist, then get a new slab
//   from g_partial_slabs[] under g_heap_lock (or mmap a fresh page).
//
// Dispatch in my_free():
//   is_slab_ptr() reads the magic word at ptr's page start to distinguish
//   slab objects (SLAB_PAGE_MAGIC) from fat-header sbrk/mmap blocks.

// Byte offset from the start of the slab page to the first object slot.
// Must be a multiple of obj_size for natural alignment.
static inline size_t slab_first_offset(int cls) {
    size_t obj = CLASS_MAX[cls];
    return ((sizeof(slab_meta) + obj - 1) / obj) * obj;
}

// Number of object slots in one slab page.
static inline size_t slab_obj_count(int cls) {
    return (PAGE_SIZE - slab_first_offset(cls)) / CLASS_MAX[cls];
}

// Build (or rebuild) the embedded LIFO freelist for a slab page.
// Safe to call on a fully-empty slab to reinitialise it for reuse.
static void slab_build_freelist(slab_meta* slab, int cls) {
    size_t obj_size  = CLASS_MAX[cls];
    size_t first_off = slab_first_offset(cls);
    size_t count     = slab_obj_count(cls);
    slab->total      = (uint32_t)count;
    slab->free_count = (uint32_t)count;
    slab->freelist   = nullptr;
    uint8_t* base = (uint8_t*)slab + first_off;
    void* head = nullptr;
    for (size_t i = count; i > 0; i--) {
        void* obj    = base + (i - 1) * obj_size;
        *(void**)obj = head;
        head = obj;
    }
    slab->freelist = head;
}

// Allocate and initialise a fresh slab page.  Called under g_heap_lock.
static slab_meta* new_slab(int cls) {
    void* page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return nullptr;

    slab_meta* slab    = (slab_meta*)page;
    slab->magic        = SLAB_PAGE_MAGIC;
    slab->remote_free  = nullptr;
    slab->next_partial = nullptr;
    slab->cls          = (uint8_t)cls;
    slab->in_partial   = 0;
    slab_build_freelist(slab, cls);

    g_stats.num_slab_pages++;
    return slab;
}

// Atomically steal the entire remote_free list and splice it into the local
// freelist.  Called by the owning thread when its local freelist is empty.
static void slab_drain_remote(slab_meta* slab) {
    void* remote = __atomic_exchange_n(
        (void**)&slab->remote_free, nullptr, __ATOMIC_ACQUIRE);
    while (remote) {
        void* next      = *(void**)remote;
        *(void**)remote = slab->freelist;
        slab->freelist  = remote;
        slab->free_count++;
        remote = next;
    }
}

// Per-thread active slabs (one per slab class).
static thread_local slab_meta* tl_active_slab  [NUM_SLAB_CLASSES] = {};
// Per-thread partial slabs: non-active slabs that have free objects.
// Populated lock-free on same-thread frees; drained before hitting g_heap_lock.
static thread_local slab_meta* tl_partial_slabs[NUM_SLAB_CLASSES] = {};

// Destructor: on thread exit, return active and partial slabs with free
// slots to the global partial pool so other threads can reuse them.
struct SlabTLS {
    ~SlabTLS() {
        pthread_mutex_lock(&g_heap_lock);
        for (int cls = 0; cls < NUM_SLAB_CLASSES; cls++) {
            // Return active slab.
            slab_meta* slab = tl_active_slab[cls];
            if (slab) {
                tl_active_slab[cls] = nullptr;
                void* remote = __atomic_exchange_n(
                    (void**)&slab->remote_free, nullptr, __ATOMIC_ACQUIRE);
                while (remote) {
                    void* next = *(void**)remote;
                    *(void**)remote = slab->freelist;
                    slab->freelist  = remote;
                    slab->free_count++;
                    remote = next;
                }
                if (slab->free_count > 0) {
                    slab->in_partial      = 1;
                    slab->next_partial    = g_partial_slabs[cls];
                    g_partial_slabs[cls]  = slab;
                }
            }
            // Return all thread-local partial slabs.
            slab_meta* p = tl_partial_slabs[cls];
            tl_partial_slabs[cls] = nullptr;
            while (p) {
                slab_meta* nxt = p->next_partial;
                p->next_partial   = g_partial_slabs[cls];
                g_partial_slabs[cls] = p;
                p->in_partial = 1;
                p = nxt;
            }
        }
        pthread_mutex_unlock(&g_heap_lock);
    }
};
static thread_local SlabTLS slab_tls;

// Allocate one object from the per-thread active slab for class cls.
// Lock-free fast path; one lock acquisition when a new slab is needed.
// Empty slab pages (free_count == total after remote drain) are munmap-ed
// and skipped so they are returned to the OS promptly.
static void* slab_alloc(int cls) {
    slab_meta* slab = tl_active_slab[cls];

    // Try to recover cross-thread frees before giving up on the current slab.
    if (slab && !slab->freelist)
        slab_drain_remote(slab);

    if (!slab || !slab->freelist) {
        slab_meta* next = nullptr;

        // 1. Check the thread-local partial list first (no lock).
        //    Drain remote frees.  Fully-empty pages are reinitialised in-place
        //    rather than munmapped — this avoids mmap/munmap churn in batch
        //    patterns where many objects are freed then immediately reallocated.
        while ((next = tl_partial_slabs[cls]) != nullptr) {
            tl_partial_slabs[cls] = next->next_partial;
            next->next_partial    = nullptr;
            next->in_partial      = 0;
            slab_drain_remote(next);
            if (next->freelist) break;  // has live objects
            // Fully empty: reinitialise the freelist and use immediately.
            slab_build_freelist(next, cls);
            break;
        }

        if (!next) {
            // 2. Fall back to global partial pool or a fresh mmap page.
            //    Empty pages from the global pool are returned to the OS
            //    (they were contributed by other threads and may not be reused
            //    soon, so giving them back avoids long-term memory bloat).
            pthread_mutex_lock(&g_heap_lock);
            while ((next = g_partial_slabs[cls]) != nullptr) {
                g_partial_slabs[cls] = next->next_partial;
                next->next_partial   = nullptr;
                next->in_partial     = 0;
                slab_drain_remote(next);
                if (next->free_count < next->total) break;  // has usable objects
                munmap(next, PAGE_SIZE);
                next = nullptr;
            }
            if (!next) next = new_slab(cls);
            pthread_mutex_unlock(&g_heap_lock);
        }

        tl_active_slab[cls] = next;
        slab = next;
        if (!slab) return nullptr;
    }

    // Pop from local freelist — no atomics needed.
    void* obj      = slab->freelist;
    slab->freelist = *(void**)obj;
    slab->free_count--;
    // Clear the double-free poison marker so the next free is detectable.
    if (CLASS_MAX[cls] >= 16) ((uint64_t*)obj)[1] = 0;
    return obj;
}

// Free one slab object.
// Same-thread path: push onto local freelist (no atomics).
// Cross-thread path: CAS-push onto remote_free (lock-free).
//
// Double-free detection: for slots ≥ 16 B, bytes 8–15 are unused by the
// freelist linkage (which only uses the first 8 bytes).  We write
// SLAB_FREE_MAGIC there on free and check it on the next free of the
// same pointer.  slab_alloc() clears the marker when handing the slot out.
static void slab_free_obj(void* ptr) {
    slab_meta* slab    = (slab_meta*)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
    size_t     obj_sz  = CLASS_MAX[slab->cls];

    // Double-free detection (slots ≥ 16 B only; 8 B slots have no spare word).
    if (obj_sz >= 16) {
        if (((uint64_t*)ptr)[1] == SLAB_FREE_MAGIC) {
            fprintf(stderr, "[my_malloc] DOUBLE FREE at %p\n", ptr);
            abort();
        }
        ((uint64_t*)ptr)[1] = SLAB_FREE_MAGIC;
    }

    if (slab == tl_active_slab[slab->cls]) {
        // Same-thread, active slab: simple push, no atomics.
        *(void**)ptr   = slab->freelist;
        slab->freelist = ptr;
        slab->free_count++;
    } else if (slab->cls < NUM_SLAB_CLASSES &&
               tl_active_slab[slab->cls] != nullptr) {
        // Same-thread, non-active slab: push to local freelist and register
        // in the thread-local partial list so slab_alloc can find it.
        *(void**)ptr   = slab->freelist;
        slab->freelist = ptr;
        slab->free_count++;
        if (!slab->in_partial) {
            slab->in_partial      = 1;
            slab->next_partial    = tl_partial_slabs[slab->cls];
            tl_partial_slabs[slab->cls] = slab;
        }
    } else {
        // Cross-thread free: CAS-push onto remote_free (lock-free).
        void* old_head;
        do {
            old_head     = __atomic_load_n((void**)&slab->remote_free, __ATOMIC_RELAXED);
            *(void**)ptr = old_head;
        } while (!__atomic_compare_exchange_n(
                     (void**)&slab->remote_free, &old_head, ptr,
                     /*weak=*/true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
        __atomic_fetch_add(&slab->free_count, 1u, __ATOMIC_RELAXED);
    }
}

// Return true if ptr is an object within a slab page.
// Fast rejection: sbrk blocks are within [g_sbrk_base, g_sbrk_end).
// Fallback: read magic word at the page start.
static inline bool is_slab_ptr(const void* ptr) {
    uintptr_t p = (uintptr_t)ptr;
    // Most frees will be fat-header sbrk blocks — reject quickly.
    uintptr_t base = (uintptr_t)__atomic_load_n(&g_sbrk_base, __ATOMIC_RELAXED);
    uintptr_t end  = (uintptr_t)__atomic_load_n(&g_sbrk_end,  __ATOMIC_RELAXED);
    if (base && p >= base && p < end) return false;
    // Check magic at the containing page start.
    const slab_meta* sm = (const slab_meta*)(p & ~(uintptr_t)(PAGE_SIZE - 1));
    return sm->magic == SLAB_PAGE_MAGIC;
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
    if (!g_sbrk_base) {
        g_sbrk_base = blk;  // record heap start on first sbrk call
        __atomic_store_n(&g_sbrk_base, blk, __ATOMIC_RELAXED);
    }
    if (sbrk(sizeof(block_meta) + size + CANARY_SIZE) == (void*)-1)
        return nullptr;
    __atomic_store_n(&g_sbrk_end, sbrk(0), __ATOMIC_RELAXED);
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

    // 3. LARGE_CLASS — bitmap-guided first-fit with splitting.
    //    Bucket b covers sizes [b*2048, (b+1)*2048).  For buckets strictly
    //    above need_bucket all blocks are guaranteed ≥ size; for need_bucket
    //    itself we must scan because some blocks there may be too small.
    {
        int need = large_bucket_of(size);
        // Mask out buckets below need (their blocks are too small).
        uint64_t mask = (need < LARGE_BUCKET_COUNT)
                        ? (large_bitmap & ~(((uint64_t)1 << need) - 1))
                        : 0;
        while (mask) {
            int b = __builtin_ctzll(mask);
            if (b > need) {
                // Every block in this bucket is big enough — take the first.
                if (large_buckets[b])
                    return split_or_take(large_buckets[b], size);
            } else {
                // b == need: some blocks here may be too small; scan.
                for (block_meta* blk = large_buckets[b]; blk; blk = blk->free_next) {
                    if (blk_size(blk) >= size)
                        return split_or_take(blk, size);
                }
            }
            mask &= ~((uint64_t)1 << b);  // clear this bucket, try next
        }
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

// ── TCache batch refill ────────────────────────────────────────────────────
// Pull up to n blocks for size class cls from the global free lists (or sbrk)
// into the TLS bin under a single lock acquisition.  Blocks sit in the bin
// as MAGIC_FREE; stats are recorded when they are popped by the caller.
// Must be called with g_heap_lock held.

void TCache::refill_bin(int cls, int n) {
    Bin& bin = bins[cls];
    size_t size = CLASS_MAX[cls];
    for (int i = 0; i < n; i++) {
        block_meta* blk = fl_find(cls, size);
        if (!blk) blk = try_wilderness(size);
        if (!blk) blk = request_block(size);
        if (!blk) break;
        blk_set_size_used(blk, size);
        blk->magic = MAGIC_FREE;   // TLS-cached, not yet live
        write_canary(blk);
        blk->free_next = bin.head;
        blk->free_prev = nullptr;
        bin.head = blk;
        bin.count++;
    }
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

    // Slab fast path: small objects (≤ 256 B) with zero per-object overhead.
    if (aligned < MMAP_THRESHOLD) {
        int cls = get_size_class(aligned);
        if (cls < NUM_SLAB_CLASSES) {
            void* obj = slab_alloc(cls);
            if (obj) {
                record_alloc(CLASS_MAX[cls]);
                return obj;
            }
            // OOM: fall through to slow path.
        }
        // TLS fast path: medium classes (NUM_SLAB_CLASSES ≤ cls < TCACHE_MAX_CLASS).
        else if (cls < TCACHE_MAX_CLASS) {
            TCache::Bin& bin = tls_cache.bins[cls];
            if (!bin.head) {
                if (tls_cache.overflow[cls].head)
                    tls_cache.drain_overflow(cls, TCACHE_BIN_MAX / 2);
                else {
                    pthread_mutex_lock(&g_heap_lock);
                    tls_cache.refill_bin(cls, TCACHE_BIN_MAX / 2);
                    pthread_mutex_unlock(&g_heap_lock);
                }
            }
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

    // Slow path: LARGE_CLASS and OOM fallback.
    pthread_mutex_lock(&g_heap_lock);
    void* p = malloc_unlocked(size);
    pthread_mutex_unlock(&g_heap_lock);
    return p;
}

void my_free(void* ptr) {
    if (!ptr) return;

    // Slab objects: detected by SLAB_PAGE_MAGIC at the containing page start.
    // Must be checked BEFORE dereferencing a block_meta header (slab objects
    // have no per-object header).
    if (is_slab_ptr(ptr)) {
        slab_meta* slab = (slab_meta*)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
        __atomic_fetch_add(&g_stats.num_frees,      1,                   __ATOMIC_RELAXED);
        __atomic_fetch_sub(&g_stats.current_usage,  CLASS_MAX[slab->cls], __ATOMIC_RELAXED);
        slab_free_obj(ptr);
        return;
    }

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

    // TLS fast path: all non-large classes.
    if (cls < TCACHE_MAX_CLASS) {
        blk->magic = MAGIC_FREE;
        __atomic_fetch_add(&g_stats.num_frees,     1,            __ATOMIC_RELAXED);
        __atomic_fetch_sub(&g_stats.current_usage, blk_size(blk), __ATOMIC_RELAXED);
        TCache::Bin& bin = tls_cache.bins[cls];
        if (bin.count >= TCACHE_BIN_MAX) {
            // Hot bin full: spill to overflow (no lock).
            // If overflow is also full, spill overflow to global (lock, rare).
            TCache::Bin& ovf = tls_cache.overflow[cls];
            if (ovf.count >= tcache_overflow_max(cls))
                tls_cache.flush_bin(cls, tcache_overflow_max(cls) / 2);
            tls_cache.flush_to_overflow(cls, TCACHE_BIN_MAX / 2);
        }
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
    if (!ptr)    return my_malloc(size);
    if (size == 0) { my_free(ptr); return nullptr; }

    // Slab objects have no in-place resize; always alloc-copy-free.
    if (is_slab_ptr(ptr)) {
        slab_meta* slab    = (slab_meta*)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
        size_t     old_sz  = CLASS_MAX[slab->cls];
        size_t     new_aln = ALIGN(size);
        if (new_aln <= old_sz) return ptr;   // fits in current slab slot
        void* np = my_malloc(size);
        if (!np) return nullptr;
        memcpy(np, ptr, old_sz);
        my_free(ptr);
        return np;
    }

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
    printf("  slab pages        : %zu\n",   snap.num_slab_pages);
}
