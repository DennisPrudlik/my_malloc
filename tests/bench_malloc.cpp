// bench_malloc.cpp — throughput/latency comparison: my_malloc vs system malloc
//
// Compile via:  make bench
// Run via:      ./build/bench_malloc
//
// Each scenario runs ITERS iterations; wall-clock time is measured with
// CLOCK_MONOTONIC.  Throughput is reported as Mops/s (million ops per second).

#include "../include/my_malloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

// ── config ─────────────────────────────────────────────────────────────────

static const int    WARMUP = 2;         // warm-up rounds (results discarded)
static const int    ROUNDS = 3;         // measurement rounds (best-of taken)
static const size_t ITERS  = 200'000;   // ops per round

// ── timing ─────────────────────────────────────────────────────────────────

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ── allocator abstraction ──────────────────────────────────────────────────

struct Alloc {
    const char* name;
    void* (*alloc)(size_t);
    void  (*dealloc)(void*);
};

static Alloc MINE  = { "my_malloc", my_malloc, my_free  };
static Alloc LIBC  = { "sys malloc", malloc,   free     };

// ── benchmark kernels ──────────────────────────────────────────────────────

// Returns -1.0 on OOM.
static double bench_pingpong(const Alloc& a, size_t sz) {
    double best = 1e18;
    for (int r = 0; r < WARMUP + ROUNDS; r++) {
        double t0 = now_sec();
        for (size_t i = 0; i < ITERS; i++) {
            void* p = a.alloc(sz);
            if (!p) return -1.0;
            *(volatile char*)p = (char)i;
            a.dealloc(p);
        }
        double elapsed = now_sec() - t0;
        if (r >= WARMUP && elapsed < best) best = elapsed;
    }
    return ITERS / best / 1e6; // Mops/s
}

// 2. Batch: alloc N, then free N.
static const size_t BATCH = 512;
static void*        g_ptrs[BATCH]; // static to avoid stack pressure

static double bench_batch(const Alloc& a, size_t sz) {
    double best = 1e18;
    size_t full_batches = ITERS / BATCH;

    for (int r = 0; r < WARMUP + ROUNDS; r++) {
        double t0 = now_sec();
        bool oom = false;
        for (size_t b = 0; b < full_batches && !oom; b++) {
            for (size_t i = 0; i < BATCH; i++) {
                g_ptrs[i] = a.alloc(sz);
                if (!g_ptrs[i]) { oom = true; break; }
                *(volatile char*)g_ptrs[i] = (char)i;
            }
            for (size_t i = 0; i < BATCH; i++)
                if (g_ptrs[i]) a.dealloc(g_ptrs[i]);
        }
        if (oom) return -1.0;
        double elapsed = now_sec() - t0;
        if (r >= WARMUP && elapsed < best) best = elapsed;
    }
    return (full_batches * BATCH) / best / 1e6;
}

// 3. Interleaved: keep a live working set of LIVE pointers, each step replace
//    one random slot (free old, alloc new).  Simulates steady-state heap.
static const size_t LIVE = 128;

static double bench_interleaved(const Alloc& a, size_t sz) {
    void* live[LIVE] = {};
    for (size_t i = 0; i < LIVE; i++) {
        live[i] = a.alloc(sz);
        if (!live[i]) {
            for (size_t j = 0; j < i; j++) a.dealloc(live[j]);
            return -1.0;
        }
        *(volatile char*)live[i] = 0;
    }

    double best = 1e18;
    uint64_t rng = 0xdeadbeef12345678ULL;

    for (int r = 0; r < WARMUP + ROUNDS; r++) {
        double t0 = now_sec();
        bool oom = false;
        for (size_t i = 0; i < ITERS && !oom; i++) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            size_t slot = rng % LIVE;
            a.dealloc(live[slot]);
            live[slot] = a.alloc(sz);
            if (!live[slot]) { oom = true; break; }
            *(volatile char*)live[slot] = (char)i;
        }
        if (oom) { for (size_t i = 0; i < LIVE; i++) if (live[i]) a.dealloc(live[i]); return -1.0; }
        double elapsed = now_sec() - t0;
        if (r >= WARMUP && elapsed < best) best = elapsed;
    }

    for (size_t i = 0; i < LIVE; i++) a.dealloc(live[i]);
    return ITERS / best / 1e6;
}

// 4. Mixed sizes: cycle through a spread of sizes within one class band.
static double bench_mixed(const Alloc& a, const size_t* sizes, int nsizes) {
    double best = 1e18;
    for (int r = 0; r < WARMUP + ROUNDS; r++) {
        double t0 = now_sec();
        bool oom = false;
        for (size_t i = 0; i < ITERS && !oom; i++) {
            size_t sz = sizes[i % nsizes];
            void* p = a.alloc(sz);
            if (!p) { oom = true; break; }
            *(volatile char*)p = (char)i;
            a.dealloc(p);
        }
        if (oom) return -1.0;
        double elapsed = now_sec() - t0;
        if (r >= WARMUP && elapsed < best) best = elapsed;
    }
    return ITERS / best / 1e6;
}

// ── overhead: bytes per allocation ────────────────────────────────────────
// my_malloc: sizeof(block_meta) + CANARY_SIZE overhead per block.
// system malloc: implementation-dependent (typically 16 B on glibc/macOS).

static void print_overhead() {
    printf("\n── Per-allocation overhead\n");
    printf("  my_malloc  header: %zu B + canary: %zu B = %zu B total\n",
           sizeof(block_meta), CANARY_SIZE, sizeof(block_meta) + CANARY_SIZE);
    printf("  sys malloc header: ~16 B (platform estimate)\n");
}

// ── table helpers ──────────────────────────────────────────────────────────

static void header(const char* title) {
    printf("\n── %s\n", title);
    printf("  %-12s  %12s  %12s  %8s\n",
           "size", "my_malloc", "sys malloc", "ratio");
    printf("  %-12s  %12s  %12s  %8s\n",
           "----", "---------", "----------", "-----");
}

static void row(const char* label, double mine, double sys) {
    if (mine < 0 || sys < 0) {
        printf("  %-12s  %12s  %12s  %8s\n", label,
               mine < 0 ? "OOM" : "",
               sys  < 0 ? "OOM" : "",
               "n/a");
        return;
    }
    printf("  %-12s  %10.2f Mop  %10.2f Mop  %7.2fx\n",
           label, mine, sys, mine / sys);
}

// ── main ──────────────────────────────────────────────────────────────────

int main() {
    printf("==========================================================\n");
    printf("  my_malloc vs system malloc — throughput benchmark\n");
    printf("  %zu iterations per scenario, best of %d rounds\n", ITERS, ROUNDS);
    printf("==========================================================\n");

    // ── 1. Ping-pong by size class ──────────────────────────────────────
    header("Ping-pong (alloc + free, single pointer)");

    struct { const char* label; size_t sz; } pp[] = {
        { "8 B",    8    },
        { "16 B",   16   },
        { "32 B",   32   },
        { "64 B",   64   },
        { "128 B",  128  },
        { "256 B",  256  },
        { "512 B",  512  },
        { "1 KB",   1024 },
        { "2 KB",   2048 },
        { "8 KB",   8192 },
    };
    for (auto& p : pp)
        row(p.label, bench_pingpong(MINE, p.sz), bench_pingpong(LIBC, p.sz));

    // ── 2. Batch alloc/free ─────────────────────────────────────────────
    header("Batch (alloc 512, then free 512)");

    struct { const char* label; size_t sz; } bt[] = {
        { "16 B",  16   },
        { "64 B",  64   },
        { "256 B", 256  },
        { "1 KB",  1024 },
        { "4 KB",  4096 },
    };
    for (auto& b : bt)
        row(b.label, bench_batch(MINE, b.sz), bench_batch(LIBC, b.sz));

    // ── 3. Interleaved steady-state ─────────────────────────────────────
    header("Interleaved (128-entry live set, random replace)");

    struct { const char* label; size_t sz; } il[] = {
        { "16 B",  16   },
        { "64 B",  64   },
        { "256 B", 256  },
        { "1 KB",  1024 },
    };
    for (auto& i : il)
        row(i.label, bench_interleaved(MINE, i.sz), bench_interleaved(LIBC, i.sz));

    // ── 4. Mixed sizes ──────────────────────────────────────────────────
    header("Mixed sizes (cycling through a spread)");

    {
        static const size_t small_sizes[] = { 8, 12, 16, 24, 32, 48, 64 };
        row("small (8-64 B)",
            bench_mixed(MINE, small_sizes, 7),
            bench_mixed(LIBC, small_sizes, 7));
    }
    {
        static const size_t med_sizes[] = { 128, 192, 256, 384, 512, 768, 1024 };
        row("medium (128-1KB)",
            bench_mixed(MINE, med_sizes, 7),
            bench_mixed(LIBC, med_sizes, 7));
    }
    {
        static const size_t large_sizes[] = { 2048, 4096, 8192, 16384 };
        row("large (2-16 KB)",
            bench_mixed(MINE, large_sizes, 4),
            bench_mixed(LIBC, large_sizes, 4));
    }

    // ── 5. Overhead breakdown ───────────────────────────────────────────
    print_overhead();

    // ── 6. my_malloc internal stats ─────────────────────────────────────
    printf("\n── my_malloc internal stats (cumulative across all scenarios)\n");
    my_stats_dump();

    printf("\n==========================================================\n");
    printf("  ratio > 1.0x → my_malloc faster, < 1.0x → sys malloc faster\n");
    printf("==========================================================\n");
    return 0;
}
