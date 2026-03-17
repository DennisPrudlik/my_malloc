# my_malloc

A custom heap allocator implemented in C++ that replaces `malloc`, `free`, `realloc`, and `calloc` with a hybrid design:
- `sbrk` heap for small/medium allocations,
- `mmap`/`munmap` for large allocations.

## What Works Right Now

The following features are implemented and exercised by the current test suite:

- **API compatibility:** `my_malloc`, `my_free`, `my_realloc`, `my_calloc`
- **Alignment:** all returned pointers are 8-byte aligned
- **Segregated free lists:** 12 size classes with fast-path reuse for small blocks
- **Split + coalesce logic:** larger free blocks split on demand; coalescing for larger classes
- **Wilderness growth:** extends top free heap block before requesting a fresh block
- **Canary corruption detection:** guard value is checked on free
- **Double-free/state validation:** block magic state tracking (`MAGIC_ALLOC` / `MAGIC_FREE`)
- **Large alloc path:** requests ≥ `MMAP_THRESHOLD` are served via `mmap` and released with `munmap`
- **Threaded operation:** allocator guarded for concurrency and validated with multithreaded stress tests
- **Per-thread cache:** lock-reduced fast path for small class alloc/free operations
- **Stats reporting:** allocation counters, peak/current usage, split/coalesce, sbrk/mmap metrics

## Memory Layout

Each allocation uses this layout:

```
[ block_meta (48 B) ][ user payload (N B) ][ canary (8 B) ]
```

`block_meta::size` stores packed flags in low bits:
- bit 0 = free flag
- bit 1 = mmap flag

## API

```cpp
void*              my_malloc(size_t size);
void               my_free(void* ptr);
void*              my_realloc(void* ptr, size_t size);
void*              my_calloc(size_t num, size_t size);
void               my_heap_dump();
const alloc_stats* my_get_stats();
void               my_stats_dump();
```

## Size Classes

| Class | Max payload |
|------:|-------------|
| 0     | 8 B         |
| 1     | 16 B        |
| 2     | 32 B        |
| 3     | 64 B        |
| 4     | 128 B       |
| 5     | 256 B       |
| 6     | 512 B       |
| 7     | 1,024 B     |
| 8     | 2,048 B     |
| 9     | 4,096 B     |
| 10    | 8,192 B     |
| 11    | large / uncapped |

## Build, Test, Run

Requirements: `g++` with C++17 and pthread support.

```bash
make
make test
make bench
make run
make clean
```

`make test` runs:
- `build/test_malloc` (single-thread functional tests)
- `build/test_threaded` (multithread stress tests)

## Project Structure

```
my_malloc/
├── include/my_malloc.h
├── src/my_malloc.cpp
├── src/main.cpp
├── tests/test_malloc.cpp
├── tests/test_threaded.cpp
├── tests/bench_malloc.cpp
├── Makefile
└── README.md
```

## Platform Notes

- Uses POSIX APIs (`sbrk`, `mmap`, `pthread`), so it targets macOS/Linux.
- Not intended for Windows without compatibility layer changes.
