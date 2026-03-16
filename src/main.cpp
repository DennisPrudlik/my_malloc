#include "../include/my_malloc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

// ── helpers ────────────────────────────────────────────────────────────────

static void section(const char* name) {
    printf("\n══════════════════════════════════════\n");
    printf("  %s\n", name);
    printf("══════════════════════════════════════\n");
}

// ── 1. Basic correctness ───────────────────────────────────────────────────

static void test_basic() {
    section("Basic malloc / calloc / realloc / free");

    int* a = (int*)my_malloc(sizeof(int) * 4);
    assert(a);
    a[0]=1; a[1]=2; a[2]=3; a[3]=4;
    printf("malloc:  %d %d %d %d\n", a[0], a[1], a[2], a[3]);

    int* b = (int*)my_calloc(4, sizeof(int));
    assert(b);
    for (int i = 0; i < 4; i++) assert(b[i] == 0);
    printf("calloc:  all zeros OK\n");

    int* c = (int*)my_malloc(sizeof(int) * 2);
    assert(c); c[0]=10; c[1]=20;
    c = (int*)my_realloc(c, sizeof(int) * 6);
    assert(c);
    assert(c[0]==10 && c[1]==20);
    c[2]=30; c[3]=40; c[4]=50; c[5]=60;
    printf("realloc: %d %d %d %d %d %d\n",
           c[0], c[1], c[2], c[3], c[4], c[5]);

    my_free(a); my_free(b); my_free(c);
    printf("free: OK\n");
}

// ── 2. Guard banding ──────────────────────────────────────────────────────

static void test_canary() {
    section("Guard Banding (Canary)");

    // Allocate a 16-byte block and verify canary is intact on normal free.
    char* buf = (char*)my_malloc(16);
    assert(buf);
    memset(buf, 0xAB, 16);
    printf("canary: clean free (no error) ... ");
    my_free(buf);
    printf("OK\n");

    // Intentional overflow — will trigger abort(); skip in normal test run.
    // Uncomment to manually verify the corruption message:
    //
    //   char* bad = (char*)my_malloc(8);
    //   bad[8] = 0xFF;   // overwrite first byte of canary
    //   my_free(bad);    // should print corruption message and abort
}

// ── 3. Segregated free lists ───────────────────────────────────────────────

static void test_segregated() {
    section("Segregated Free Lists");

    const alloc_stats* s0 = my_get_stats();
    size_t sbrk_before = s0->num_sbrk_calls;

    // Allocate many 32-byte blocks (class 2), free them, then reallocate.
    // Second round should hit the free list, not sbrk.
    void* ptrs[8];
    for (int i = 0; i < 8; i++) ptrs[i] = my_malloc(32);
    for (int i = 0; i < 8; i++) my_free(ptrs[i]);

    size_t sbrk_mid = my_get_stats()->num_sbrk_calls;
    for (int i = 0; i < 8; i++) ptrs[i] = my_malloc(32);

    size_t sbrk_after = my_get_stats()->num_sbrk_calls;
    printf("sbrk for first  8 allocs : %zu\n", sbrk_mid   - sbrk_before);
    printf("sbrk for second 8 allocs : %zu  (should be 0 — free list hit)\n",
           sbrk_after - sbrk_mid);
    assert(sbrk_after == sbrk_mid);

    for (int i = 0; i < 8; i++) my_free(ptrs[i]);
    printf("segregated lists: OK\n");
}

// ── 4. Large block splitting ───────────────────────────────────────────────

static void test_splitting() {
    section("Large Block Splitting");

    size_t splits_before = my_get_stats()->num_splits;

    // Allocate a large block, free it, then request a smaller piece.
    // The allocator should split the large free block.
    char* big = (char*)my_malloc(4096);
    assert(big);
    my_free(big);

    char* small1 = (char*)my_malloc(512);
    char* small2 = (char*)my_malloc(512);
    assert(small1 && small2);

    size_t splits_after = my_get_stats()->num_splits;
    printf("splits performed: %zu\n", splits_after - splits_before);
    assert(splits_after > splits_before);

    my_free(small1);
    my_free(small2);
    printf("splitting: OK\n");
}

// ── 5. Wilderness optimization ────────────────────────────────────────────

static void test_wilderness() {
    section("Wilderness Optimization");

    size_t saves_before = my_get_stats()->num_wilderness_saves;
    size_t sbrk_before  = my_get_stats()->num_sbrk_calls;

    // Use sizes far larger than anything left in the free lists from previous
    // tests.  1 MB forces a fresh sbrk; 2 MB is too large for the 1 MB tail
    // block so fl_find finds nothing and try_wilderness fires.
    const size_t S1 = 1024 * 1024;       // 1 MB
    const size_t S2 = 2 * 1024 * 1024;   // 2 MB

    char* tail = (char*)my_malloc(S1);
    assert(tail);
    my_free(tail);   // heap_tail is now a free 1 MB block

    // S2 > S1: fl_find cannot satisfy the request → wilderness extends the tail
    // block by the deficit (1 MB sbrk) instead of requesting a fresh 2 MB chunk.
    char* extended = (char*)my_malloc(S2);
    assert(extended);

    size_t saves_after = my_get_stats()->num_wilderness_saves;
    size_t sbrk_after  = my_get_stats()->num_sbrk_calls;

    printf("wilderness saves : %zu\n", saves_after - saves_before);
    printf("sbrk calls       : %zu  (1 for 1MB alloc + 1 for 1MB deficit)\n",
           sbrk_after - sbrk_before);
    assert(saves_after > saves_before);

    my_free(extended);
    printf("wilderness: OK\n");
}

// ── 6. Coalescing ─────────────────────────────────────────────────────────

static void test_coalescing() {
    section("Coalescing");

    size_t coal_before = my_get_stats()->num_coalesces;

    char* a = (char*)my_malloc(4096);
    char* b = (char*)my_malloc(4096);
    char* c = (char*)my_malloc(4096);
    assert(a && b && c);

    my_free(a);
    my_free(c);
    my_free(b);  // b is between a and c — both neighbours free → 2 coalesces

    size_t coal_after = my_get_stats()->num_coalesces;
    printf("coalesces performed: %zu  (expected >= 2)\n", coal_after - coal_before);
    assert(coal_after - coal_before >= 2);
    printf("coalescing: OK\n");
}

// ── 7. Heap dump & stats ──────────────────────────────────────────────────

static void test_dump() {
    section("Heap Dump");
    char* x = (char*)my_malloc(100);
    char* y = (char*)my_malloc(200);
    my_free(x);
    my_heap_dump();
    my_free(y);
}

// ── main ──────────────────────────────────────────────────────────────────

int main() {
    test_basic();
    test_canary();
    test_segregated();
    test_splitting();
    test_wilderness();
    test_coalescing();
    test_dump();

    section("Final Statistics");
    my_stats_dump();

    printf("\nAll tests passed.\n");
    return 0;
}
