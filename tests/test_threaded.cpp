#include "../include/my_malloc.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <set>

// ── harness ────────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

#define FAIL(msg) do { \
    printf("FAIL  (%s:%d: %s)\n", __FILE__, __LINE__, msg); \
    g_failed++; \
    return; \
} while (0)

#define EXPECT(cond) do { if (!(cond)) { FAIL(#cond); } } while (0)

// ── shared live-pointer registry ───────────────────────────────────────────
// Tracks every pointer currently held by any thread.  Any duplicate signals a
// double-allocation (two threads received the same address while both are live).

static std::set<void*> g_live;
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;

static bool live_insert(void* p) {
    pthread_mutex_lock(&g_live_lock);
    bool ok = g_live.insert(p).second;   // false → duplicate
    pthread_mutex_unlock(&g_live_lock);
    return ok;
}

static void live_remove(void* p) {
    pthread_mutex_lock(&g_live_lock);
    g_live.erase(p);
    pthread_mutex_unlock(&g_live_lock);
}

// ── worker ─────────────────────────────────────────────────────────────────

static const size_t SIZES[] = { 8, 16, 32, 64, 128, 256, 512, 1024 };
static const int    NSIZES  = sizeof(SIZES) / sizeof(SIZES[0]);
static const int    ITERS   = 10000;

// Shared failure flag; written under g_live_lock.
static int g_worker_failed = 0;

static void* worker(void* arg) {
    long tid = (long)arg;

    for (int i = 0; i < ITERS; i++) {
        size_t sz = SIZES[(tid * 7 + i) % NSIZES];   // vary size per thread

        void* p = my_malloc(sz);
        if (!p) {
            pthread_mutex_lock(&g_live_lock);
            g_worker_failed = 1;
            pthread_mutex_unlock(&g_live_lock);
            return nullptr;
        }

        // Check alignment.
        if ((uintptr_t)p & (ALIGNMENT - 1)) {
            pthread_mutex_lock(&g_live_lock);
            g_worker_failed = 1;
            pthread_mutex_unlock(&g_live_lock);
            my_free(p);
            return nullptr;
        }

        // Register as live; abort on duplicate.
        if (!live_insert(p)) {
            pthread_mutex_lock(&g_live_lock);
            g_worker_failed = 1;
            pthread_mutex_unlock(&g_live_lock);
            my_free(p);
            return nullptr;
        }

        // Write a recognisable pattern so a corrupt heap shows up as a bad read.
        memset(p, (int)(tid & 0xFF), sz);

        // Verify own bytes before freeing (catches overlap with another thread).
        uint8_t* b = (uint8_t*)p;
        for (size_t j = 0; j < sz; j++) {
            if (b[j] != (uint8_t)(tid & 0xFF)) {
                pthread_mutex_lock(&g_live_lock);
                g_worker_failed = 1;
                pthread_mutex_unlock(&g_live_lock);
                live_remove(p);
                my_free(p);
                return nullptr;
            }
        }

        live_remove(p);
        my_free(p);
    }
    return nullptr;
}

// ── tests ──────────────────────────────────────────────────────────────────

static void threaded_alloc_free_stress() {
    const int NTHREADS = 8;
    pthread_t threads[NTHREADS];

    g_worker_failed = 0;
    g_live.clear();

    for (long t = 0; t < NTHREADS; t++)
        pthread_create(&threads[t], nullptr, worker, (void*)t);

    for (int t = 0; t < NTHREADS; t++)
        pthread_join(threads[t], nullptr);

    EXPECT(g_worker_failed == 0);
    EXPECT(g_live.empty());   // all allocations were freed
}

static void threaded_stats_consistent() {
    // After all threads complete, current_usage must be 0 (no leaks).
    EXPECT(my_get_stats()->current_usage == 0);
}

static void threaded_realloc_stress() {
    // One thread per size, each growing and shrinking via realloc.
    const int NTHREADS = 4;
    pthread_t threads[NTHREADS];

    struct Args { size_t start; size_t end; int failed; };
    Args args[NTHREADS] = {{64,512,0},{128,1024,0},{32,256,0},{16,128,0}};

    auto realloc_worker = [](void* arg) -> void* {
        Args* a = (Args*)arg;
        void* p = my_malloc(a->start);
        if (!p) { a->failed = 1; return nullptr; }
        memset(p, 0xAB, a->start);
        for (size_t sz = a->start; sz <= a->end; sz *= 2) {
            void* np = my_realloc(p, sz);
            if (!np) { a->failed = 1; my_free(p); return nullptr; }
            p = np;
        }
        my_free(p);
        return nullptr;
    };

    for (int t = 0; t < NTHREADS; t++)
        pthread_create(&threads[t], nullptr, realloc_worker, &args[t]);
    for (int t = 0; t < NTHREADS; t++)
        pthread_join(threads[t], nullptr);

    for (int t = 0; t < NTHREADS; t++)
        EXPECT(args[t].failed == 0);
}

// ── main ───────────────────────────────────────────────────────────────────

int main() {
    printf("my_malloc threaded tests\n");
    printf("================================================\n\n");
    printf("── Thread safety (%d threads × %d iterations)\n", 8, ITERS);

    auto run = [](const char* name, void(*fn)()) {
        printf("  %-55s", name);
        fflush(stdout);
        fn();
        if (g_failed == 0) { printf("PASS\n"); g_passed++; }
    };

    run("threaded_alloc_free_stress",  threaded_alloc_free_stress);
    run("threaded_stats_consistent",   threaded_stats_consistent);
    run("threaded_realloc_stress",     threaded_realloc_stress);

    printf("\n================================================\n");
    printf("Results: %d/%d passed", g_passed, g_passed + g_failed);
    if (g_failed) printf(", %d FAILED", g_failed);
    printf("\n");
    return g_failed ? 1 : 0;
}
