<div align="center">
  <img src="MyBuddy.jpg" alt="K-Term Logo" width="933">
</div>

**High-performance lock-free thread-caching buddy allocator for C/C++.**

MyBuddy (MBd) is a production-grade, highly concurrent memory allocator for high-performance C/C++ applications. It combines the anti-fragmentation guarantees of a classic Buddy Allocator with the lock-free speed of per-thread caching. It is designed with 32-byte SIMD-safe alignment, zero thread-exit leaks, hardened safety, and is LD_PRELOAD-ready.

## Key Features

- **Crazy Fast**: Lock-free thread-local cache delivers allocations up to 8 KiB in just a few CPU cycles.
- **Fully Thread-Safe**: True per-thread caching with almost zero contention (global lock is only used on cache misses and large blocks).
- **Hardened & Safe**: Includes double-free protection, underflow-protected bounds checking, magic-value validation, and immediate `abort()` on corruption.
- **Superior to Standard `malloc`**: Offers superior anti-fragmentation thanks to classic buddy coalescing, mathematically guaranteed **32-byte** SIMD-safe alignment, and zero leaks on thread exit.
- **Production Design**: LD_PRELOAD-safe, self-initializing, zero libc dependency in the hot path, and designed to live for the entire lifetime of the process.

## Multithreading & Concurrency

- **No False Sharing**: Thread caches are allocated from the pool with proper alignment to prevent false sharing.
- **Anti-Hoarding**: Full caches automatically bulk-flush 50% of blocks back to the global pool, preventing excessive memory hoarding by inactive threads.

## Usage Scenarios

- **Tiny Objects** (strings, AST nodes, ECS entities): Extremely fast due to lock-free cache.
- **Medium Objects** (network buffers, 4 KiB pages): Stay in the lock-free cache thanks to `SMALL_ORDER_MAX = 13`.
- **Large Objects** (> 8 KiB): Go through the global buddy path, which is still very fast thanks to O(1) doubly-linked lists.

## Quick Start

MyBuddy is a single-header library. To use it, you just need to include the header. However, exactly **one** C or C++ file in your project must define `MYBUDDY_IMPLEMENTATION` before including the header to instantiate the implementation.

### Implementation File (`mybuddy.c` or similar):
```c
#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"
```

### Other Files:
```c
#include "mybuddy.h"

int main() {
    // Optional: Pre-warm the allocator before spawning threads
    mbd_init();

    // Lock-free fast path (falls under SMALL_ORDER_MAX)
    char *string_buffer = mbd_alloc(128);

    // Global slow path (bypasses cache, carves 2MB block)
    void *texture_asset = mbd_alloc(1024 * 1024 * 2);

    mbd_free(string_buffer);
    mbd_free(texture_asset);

    return 0; // OS reclaims memory pool. No leaks.
}
```

*Note: The allocator is self-initializing. The first call to `mbd_alloc()` or `mbd_free()` will automatically initialize the pool. For latency-sensitive applications, you may still call `mbd_init()` explicitly during startup.*

## API Reference

### Core Memory Operations
- `void mbd_init(void);`
  Explicitly initializes the allocator. Optional, as the library is self-initializing.
- `void *mbd_alloc(size_t requested_size);`
  Allocates a block of memory of the specified size. Returns a 32-byte aligned pointer.
- `void mbd_free(void *ptr);`
  Frees a previously allocated block of memory. Includes bounds checking and double-free protection.
- `void *mbd_realloc(void *ptr, size_t new_size);`
  Reallocates a memory block to a new size.
- `void *mbd_calloc(size_t nmemb, size_t size);`
  Allocates zero-initialized memory for an array.
- `size_t mbd_malloc_usable_size(const void *ptr);`
  Returns the number of usable bytes in an allocated block.
- `void mbd_dump(void);`
  Prints the current state of the global free lists for diagnostics.

### String View Helpers
MyBuddy includes a simple, allocation-free string view struct `mbd_string_view_t` to handle strings efficiently.

- `mbd_string_view_t mbd_string_view_from_cstr(const char *s);`
  Create a string view from a null-terminated C string (does not allocate).
- `mbd_string_view_t mbd_string_view_from_data(const char *data, size_t len);`
  Create a string view from raw data and exact length (does not allocate).
- `char *mbd_string_view_dup(mbd_string_view_t view);`
  Allocate a null-terminated C string copy from a string view (uses `mbd_alloc`).

## Compilation / Linking

When compiling your application, ensure you link against the pthread library:
```bash
gcc -o my_app main.c mybuddy.c -lpthread
```
