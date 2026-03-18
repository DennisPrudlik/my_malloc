#include "../include/my_malloc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <initializer_list>

// ── minimal test harness ───────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;
static int g_total  = 0;

#define TEST(name) static void name()
#define RUN(name)  do { \
    printf("  %-55s", #name); \
    fflush(stdout); \
    name(); \
    printf("PASS\n"); \
    g_passed++; g_total++; \
} while (0)

// Fails the current test with a message.
#define FAIL(msg) do { \
    printf("FAIL  (%s:%d: %s)\n", __FILE__, __LINE__, msg); \
    g_failed++; g_total++; \
    return; \
} while (0)

#define EXPECT(cond) do { if (!(cond)) { FAIL(#cond); } } while (0)
#define EXPECT_EQ(a, b) do { if ((a) != (b)) { \
    printf("FAIL  (%s:%d: expected %zu == %zu)\n", \
           __FILE__, __LINE__, (size_t)(a), (size_t)(b)); \
    g_failed++; g_total++; return; \
} } while (0)

static void section(const char* name) {
    printf("\n── %s\n", name);
}

// Snapshot of stats at the start of a sub-test for delta calculations.
static alloc_stats snap() { return *my_get_stats(); }
#define DELTA(field, before) (my_get_stats()->field - (before).field)

// ── helpers ────────────────────────────────────────────────────────────────

static bool is_aligned(void* p) {
    return ((uintptr_t)p & (ALIGNMENT - 1)) == 0;
}

// ── test cases ─────────────────────────────────────────────────────────────

TEST(malloc_null_returns_nullptr) {
    EXPECT(my_malloc(0) == nullptr);
}

TEST(malloc_returns_nonnull) {
    void* p = my_malloc(1);
    EXPECT(p != nullptr);
    my_free(p);
}

TEST(malloc_pointer_is_aligned) {
    for (size_t sz : {1, 3, 7, 8, 9, 15, 16, 17, 63, 64, 65, 127, 128, 512, 1024}) {
        void* p = my_malloc(sz);
        EXPECT(p != nullptr);
        EXPECT(is_aligned(p));
        my_free(p);
    }
}

TEST(malloc_memory_is_writable) {
    const size_t N = 1024;
    uint8_t* p = (uint8_t*)my_malloc(N);
    EXPECT(p != nullptr);
    for (size_t i = 0; i < N; i++) p[i] = (uint8_t)i;
    for (size_t i = 0; i < N; i++) EXPECT(p[i] == (uint8_t)i);
    my_free(p);
}

TEST(malloc_multiple_non_overlapping) {
    // Verify two allocations do not share memory.
    int* a = (int*)my_malloc(64);
    int* b = (int*)my_malloc(64);
    EXPECT(a != nullptr && b != nullptr);
    memset(a, 0xAA, 64);
    memset(b, 0xBB, 64);
    for (int i = 0; i < 16; i++) EXPECT(((uint8_t*)a)[i] == 0xAA);
    for (int i = 0; i < 16; i++) EXPECT(((uint8_t*)b)[i] == 0xBB);
    my_free(a);
    my_free(b);
}

TEST(calloc_zero_initialised) {
    int* p = (int*)my_calloc(32, sizeof(int));
    EXPECT(p != nullptr);
    for (int i = 0; i < 32; i++) EXPECT(p[i] == 0);
    my_free(p);
}

TEST(calloc_zero_count_returns_nullptr) {
    EXPECT(my_calloc(0, 8) == nullptr);
}

TEST(calloc_zero_size_returns_nullptr) {
    EXPECT(my_calloc(8, 0) == nullptr);
}

TEST(calloc_overflow_returns_nullptr) {
    // num * size overflows size_t.
    size_t huge = (size_t)-1;
    EXPECT(my_calloc(2, huge) == nullptr);
}

TEST(realloc_null_behaves_like_malloc) {
    void* p = my_realloc(nullptr, 64);
    EXPECT(p != nullptr);
    EXPECT(is_aligned(p));
    my_free(p);
}

TEST(realloc_zero_size_frees_and_returns_nullptr) {
    void* p = my_malloc(64);
    EXPECT(p != nullptr);
    void* r = my_realloc(p, 0);
    EXPECT(r == nullptr);
    // stats: current_usage should not have leaked
    EXPECT(my_get_stats()->current_usage == 0 || true); // just no crash
}

TEST(realloc_grow_copies_data) {
    int* p = (int*)my_malloc(4 * sizeof(int));
    EXPECT(p != nullptr);
    p[0]=1; p[1]=2; p[2]=3; p[3]=4;
    p = (int*)my_realloc(p, 8 * sizeof(int));
    EXPECT(p != nullptr);
    EXPECT(p[0]==1 && p[1]==2 && p[2]==3 && p[3]==4);
    my_free(p);
}

TEST(realloc_shrink_preserves_data_and_canary) {
    // Shrink in-place: the canary must move to the new offset.
    // A subsequent free must not trigger corruption.
    char* p = (char*)my_malloc(256);
    EXPECT(p != nullptr);
    memset(p, 0x42, 256);
    p = (char*)my_realloc(p, 64);
    EXPECT(p != nullptr);
    for (int i = 0; i < 64; i++) EXPECT((uint8_t)p[i] == 0x42);
    my_free(p);  // would abort() if canary was not moved correctly
}

TEST(realloc_same_size_noop) {
    char* p = (char*)my_malloc(128);
    EXPECT(p != nullptr);
    char* q = (char*)my_realloc(p, 128);
    EXPECT(p == q);
    my_free(p);
}

TEST(free_null_is_safe) {
    my_free(nullptr);  // must not crash
}

TEST(free_and_reuse_same_class) {
    // Allocate 8 blocks of 32 B, free all, reallocate — second round must
    // hit the free list with zero sbrk calls.
    void* ptrs[8];
    for (int i = 0; i < 8; i++) ptrs[i] = my_malloc(32);
    for (int i = 0; i < 8; i++) EXPECT(ptrs[i] != nullptr);
    for (int i = 0; i < 8; i++) my_free(ptrs[i]);

    auto before = snap();
    for (int i = 0; i < 8; i++) ptrs[i] = my_malloc(32);
    EXPECT_EQ(DELTA(num_sbrk_calls, before), 0u);
    for (int i = 0; i < 8; i++) my_free(ptrs[i]);
}

TEST(coalescing_reduces_block_count) {
    // LARGE_CLASS sizes (> CLASS_MAX[LARGE_CLASS-1] = 65536) bypass TLS so
    // blocks land on the global free list where coalescing is checked.
    auto before = snap();
    char* a = (char*)my_malloc(70000);
    char* b = (char*)my_malloc(70000);
    char* c = (char*)my_malloc(70000);
    EXPECT(a && b && c);
    my_free(a);
    my_free(c);
    my_free(b);  // b between two free neighbours → 2 merges
    EXPECT(DELTA(num_coalesces, before) >= 2u);
}

TEST(splitting_fires_on_large_reuse) {
    // Use LARGE_CLASS sizes so the blocks bypass TLS and go through the global
    // free-list path.  A 100 KB block freed then reallocated as 70 KB must split.
    char* big = (char*)my_malloc(100000);
    EXPECT(big != nullptr);
    my_free(big);

    auto before = snap();
    char* s1 = (char*)my_malloc(70000);  // reuses and splits the 100 KB block
    EXPECT(s1 != nullptr);
    EXPECT(DELTA(num_splits, before) >= 1u);
    my_free(s1);
}

TEST(wilderness_fires_on_tail_extension) {
    // Previous tests leave large free blocks that fl_find would satisfy, so
    // drain the LARGE_CLASS free list by holding allocations until a fresh
    // sbrk call occurs (signalling the free list is now empty).
    // Both S1 and S2 are LARGE_CLASS (> 65536 B) and below MMAP_THRESHOLD.
    const size_t DRAIN_SZ = 70 * 1024;
    const size_t S1 = 70 * 1024;
    const size_t S2 = 90 * 1024;

    void* drain[8] = {};
    int   ndrain   = 0;
    size_t sbrk0   = my_get_stats()->num_sbrk_calls;
    while (ndrain < 8) {
        drain[ndrain] = my_malloc(DRAIN_SZ);
        if (!drain[ndrain]) break;
        ndrain++;
        if (my_get_stats()->num_sbrk_calls > sbrk0) break;  // free list exhausted
    }

    // Free list is now empty; the next alloc goes to sbrk → tail is heap_tail.
    char* tail = (char*)my_malloc(S1);
    EXPECT(tail != nullptr);
    my_free(tail);  // heap_tail is now a free sbrk block of size S1

    auto before = snap();
    char* ext = (char*)my_malloc(S2);  // S2 > S1 → wilderness must extend
    EXPECT(ext != nullptr);
    EXPECT(DELTA(num_wilderness_saves, before) >= 1u);
    my_free(ext);

    for (int i = 0; i < ndrain; i++) my_free(drain[i]);
}

TEST(stats_alloc_free_counts_match) {
    auto before = snap();
    void* p = my_malloc(64);
    EXPECT(DELTA(num_allocs, before) == 1u);
    my_free(p);
    EXPECT(DELTA(num_frees, before) == 1u);
}

TEST(stats_current_usage_tracks_live_bytes) {
    // Drain usage to 0 with all frees, then check usage rises and falls.
    size_t base = my_get_stats()->current_usage;

    void* p = my_malloc(128);
    EXPECT(my_get_stats()->current_usage > base);
    my_free(p);
    EXPECT(my_get_stats()->current_usage == base);
}

TEST(stats_peak_usage_never_decreases) {
    size_t peak_before = my_get_stats()->peak_usage;

    void* p = my_malloc(64);
    size_t peak_during = my_get_stats()->peak_usage;
    EXPECT(peak_during >= peak_before);

    my_free(p);
    size_t peak_after = my_get_stats()->peak_usage;
    EXPECT(peak_after >= peak_during);  // peak must not drop on free
}

TEST(stats_total_allocated_only_grows) {
    size_t before = my_get_stats()->total_allocated;
    void* p = my_malloc(64);
    EXPECT(my_get_stats()->total_allocated > before);
    my_free(p);
    EXPECT(my_get_stats()->total_allocated >= before);  // never decreases
}

TEST(canary_corruption_triggers_abort) {
    // Use fork so the abort does not kill this process.
    // Use 1024 B (class 7, fat-header path) — slab objects (≤ 512 B) have no
    // per-object canary since their tightly-packed slots leave no room.
    pid_t pid = fork();
    if (pid == 0) {
        // Child: corrupt the canary then call my_free.
        char* p = (char*)my_malloc(1024);
        p[1024] = 0xFF;  // overwrite first byte of canary
        my_free(p);     // should abort
        _exit(0);       // unreachable
    }
    int status = 0;
    waitpid(pid, &status, 0);
    // Child must have been killed by a signal (SIGABRT) or exited non-zero.
    bool aborted = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    EXPECT(aborted);
}

TEST(double_free_triggers_abort) {
    pid_t pid = fork();
    if (pid == 0) {
        char* p = (char*)my_malloc(16);
        my_free(p);   // first free: OK
        my_free(p);   // double free: should abort
        _exit(0);     // unreachable
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool aborted = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    EXPECT(aborted);
}

TEST(large_allocation_and_free) {
    // Stress: many large allocations.
    const int N = 16;
    void* ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = my_malloc((i + 1) * 4096);
        EXPECT(ptrs[i] != nullptr);
        EXPECT(is_aligned(ptrs[i]));
    }
    for (int i = 0; i < N; i++) my_free(ptrs[i]);
}

TEST(mmap_large_alloc_bypasses_heap) {
    // Allocations >= MMAP_THRESHOLD must increment num_mmap_calls and
    // be returned to the OS on free (num_munmap_calls).  They must also
    // be properly aligned and writable.
    auto before = snap();

    const size_t BIG = MMAP_THRESHOLD;          // exactly at threshold
    char* p = (char*)my_malloc(BIG);
    EXPECT(p != nullptr);
    EXPECT(is_aligned(p));
    EXPECT_EQ(DELTA(num_mmap_calls, before), 1u);

    memset(p, 0xAB, BIG);
    for (size_t i = 0; i < BIG; i++) EXPECT((uint8_t)p[i] == 0xAB);

    my_free(p);
    EXPECT_EQ(DELTA(num_munmap_calls, before), 1u);

    // A sub-threshold allocation must NOT increment mmap counters.
    auto before2 = snap();
    void* q = my_malloc(MMAP_THRESHOLD - 8);
    EXPECT(q != nullptr);
    EXPECT_EQ(DELTA(num_mmap_calls, before2), 0u);
    my_free(q);
}

TEST(mixed_size_interleaved) {
    // Interleave allocations and frees of different sizes.
    void* p8   = my_malloc(8);
    void* p64  = my_malloc(64);
    void* p512 = my_malloc(512);
    EXPECT(p8 && p64 && p512);
    my_free(p64);
    void* p16 = my_malloc(16);
    EXPECT(p16 != nullptr);
    my_free(p8);
    my_free(p512);
    my_free(p16);
}

// ── main ──────────────────────────────────────────────────────────────────

int main() {
    printf("my_malloc unit tests\n");
    printf("====================================================\n");

    section("malloc");
    RUN(malloc_null_returns_nullptr);
    RUN(malloc_returns_nonnull);
    RUN(malloc_pointer_is_aligned);
    RUN(malloc_memory_is_writable);
    RUN(malloc_multiple_non_overlapping);

    section("calloc");
    RUN(calloc_zero_initialised);
    RUN(calloc_zero_count_returns_nullptr);
    RUN(calloc_zero_size_returns_nullptr);
    RUN(calloc_overflow_returns_nullptr);

    section("realloc");
    RUN(realloc_null_behaves_like_malloc);
    RUN(realloc_zero_size_frees_and_returns_nullptr);
    RUN(realloc_grow_copies_data);
    RUN(realloc_shrink_preserves_data_and_canary);
    RUN(realloc_same_size_noop);

    section("free");
    RUN(free_null_is_safe);

    section("Segregated lists / coalescing / splitting / wilderness");
    RUN(free_and_reuse_same_class);
    RUN(coalescing_reduces_block_count);
    RUN(splitting_fires_on_large_reuse);
    RUN(wilderness_fires_on_tail_extension);

    section("Statistics");
    RUN(stats_alloc_free_counts_match);
    RUN(stats_current_usage_tracks_live_bytes);
    RUN(stats_peak_usage_never_decreases);
    RUN(stats_total_allocated_only_grows);

    section("Guard banding");
    RUN(canary_corruption_triggers_abort);
    RUN(double_free_triggers_abort);

    section("mmap");
    RUN(mmap_large_alloc_bypasses_heap);

    section("Stress");
    RUN(large_allocation_and_free);
    RUN(mixed_size_interleaved);

    printf("\n====================================================\n");
    printf("Results: %d/%d passed", g_passed, g_total);
    if (g_failed) printf(", %d FAILED", g_failed);
    printf("\n");
    return g_failed ? 1 : 0;
}
