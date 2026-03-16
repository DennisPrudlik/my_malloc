# my_malloc

A custom heap allocator implemented in C++ that replaces the standard `malloc`, `free`, `realloc`, and `calloc`. Built from scratch using `sbrk` for OS-level memory requests.

## Features

- **Segregated free lists** — 10 size classes (8 B → 2 KB + large) for O(1) allocation of small/medium blocks
- **Block splitting** — large free blocks are split to satisfy smaller requests, reducing waste
- **Coalescing** — adjacent free blocks are merged on `my_free` to fight fragmentation
- **Wilderness optimization** — the top-of-heap free block is extended in-place before calling `sbrk` again
- **Canary guard bands** — every allocation is bracketed by a `0xDEADBEEFDEADBEEF` sentinel; heap corruption is detected immediately on `my_free`
- **8-byte alignment** — all returned pointers are aligned to 8 bytes
- **Allocator statistics** — counters for allocs, frees, splits, coalesces, `sbrk` calls, and more

## Memory Layout

Each allocation consists of three contiguous regions on the heap:

```
[ block_meta (48 B) ][ user payload (N B) ][ canary (8 B) ]
```

`block_meta` holds two doubly-linked lists:
- **heap list** — physical order of every block on the heap (used for coalescing)
- **free list** — per-size-class list of available blocks (used for allocation)

## API

```cpp
void*  my_malloc (size_t size);
void   my_free   (void* ptr);
void*  my_realloc(void* ptr, size_t size);
void*  my_calloc (size_t num, size_t size);

// Diagnostics
const alloc_stats* my_get_stats();
void               my_stats_dump();
void               my_heap_dump();
```

## Project Structure

```
my_malloc/
├── include/
│   └── my_malloc.h        # Public API, block_meta, size-class constants
├── src/
│   ├── my_malloc.cpp      # Allocator implementation
│   └── main.cpp           # Manual demo / smoke tests
├── tests/
│   └── test_malloc.cpp    # Full unit test suite
├── Makefile
└── README.md
```

## Build & Run

**Requirements:** `g++` with C++17 support (Xcode CLT on macOS or `build-essential` on Linux).

```bash
# Build everything
make

# Run the unit tests
make test

# Run the manual demo
make run

# Clean build artifacts
make clean
```

## Running the Tests

```
$ make test
── Basic malloc / free
  malloc_null_returns_nullptr                             PASS
  malloc_returns_nonnull                                  PASS
  malloc_pointer_is_aligned                               PASS
  malloc_memory_is_writable                               PASS
  malloc_multiple_non_overlapping                         PASS
...
```

## Size Classes

| Class | Max payload |
|-------|-------------|
| 0     | 8 B         |
| 1     | 16 B        |
| 2     | 32 B        |
| 3     | 64 B        |
| 4     | 128 B       |
| 5     | 256 B       |
| 6     | 512 B       |
| 7     | 1 024 B     |
| 8     | 2 048 B     |
| 9     | > 2 048 B (large) |

Small/medium requests (classes 0–8) are rounded up to the class ceiling so every block in a class is identical in size, enabling O(1) pop from the free list. Large requests (class 9) use first-fit search with splitting.

## Platform Notes

- Uses `sbrk(2)`, which is available on macOS and Linux. Not supported on Windows.
- Tested with `g++ -std=c++17` on macOS (Apple Clang) and Linux (GCC 12+).
