/**
 * @file mybuddy.h
 * @brief High-Performance Thread-Caching Buddy Allocator
 *
 * @version 17.0 (FINAL)
 * @date April 10, 2026
 * @author Jacques Morel
 *
 * @section overview Overview
 * MyBuddy (MBd) is a production-grade, highly concurrent memory allocator for high-performance C/C++ applications. It combines the anti-fragmentation
 * guarantees of a classic Buddy Allocator with the lock-free speed of per-thread caching.
 *
* @section features Key Strengths
 * - **Crazy Fast**: Lock-free thread-local cache delivers allocations up to 8 KiB in just a few CPU cycles.
 * - **Fully Thread-Safe**: True per-thread caching with almost zero contention (global lock only on misses and large blocks).
 * - **Hardened & Safe**: Double-free protection, underflow-protected bounds checking, magic-value validation, and immediate `abort()` on corruption.
 * - **Better than standard malloc**: Superior anti-fragmentation thanks to classic buddy coalescing + mathematically guaranteed **32-byte** SIMD-safe alignment + zero leaks on thread exit.
 * - **Special Production Design**: LD_PRELOAD-safe, self-initializing, zero libc dependency in the hot path, and designed to live for the entire lifetime of the process.
 *
 * @section multithreading Multithreading & Concurrency
 * - **No False Sharing**: Thread caches are allocated from the pool with proper alignment.
 * - **Anti-Hoarding**: Full caches automatically bulk-flush 50% of blocks back to the global pool.
 *
 * @section usage Usage Scenarios
 * - **Tiny objects** (strings, AST nodes, ECS entities): Extremely fast.
 * - **Medium objects** (network buffers, 4 KiB pages): Stay in the lock-free cache thanks to `SMALL_ORDER_MAX = 13`.
 * - **Large objects** (> 8 KiB): Go through the global buddy path (still fast thanks to O(1) doubly-linked lists).
 *
 * @section init Initialization
 * The allocator is **self-initializing**. The first call to `mbd_alloc()` or `mbd_free()` will automatically initialize the pool. For latency-sensitive
 * applications you may still call `mbd_init()` explicitly during startup.
 *
 * @note There is intentionally no `mbd_destroy()` function. Production allocators are designed to live for the entire lifetime of the process.
 * The OS reclaims the memory on exit.
 * 
 * @example
 * @code
 * #include "mybuddy.h"
 * 
 * int main() {
 *     // Optional: Pre-warm the allocator before spawning threads
 *     mbd_init(); 
 * 
 *     // Lock-free fast path (falls under SMALL_ORDER_MAX)
 *     char *string_buffer = mbd_alloc(128);
 * 
 *     // Global slow path (bypasses cache, carves 2MB block)
 *     void *texture_asset = mbd_alloc(1024 * 1024 * 2);
 * 
 *     mbd_free(string_buffer);
 *     mbd_free(texture_asset);
 * 
 *     return 0; // OS reclaims memory pool. No leaks.
 * }
 * @endcode
 */
#ifndef MYBUDDY_H
#define MYBUDDY_H

#include <stddef.h> /* Required for size_t */
#include <stdint.h>

/* Enable C++ code to safely include this C header */
#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration Macros ─────────────────────────────────────────── */
#define POOL_SIZE          (1ULL << 30)   // 1 GiB
#define MAX_ORDER          30
#define SMALL_ORDER_MAX    13             // includes 4 KiB pages
#define THREAD_CACHE_SIZE  64

/* ── Public API ───────────────────────────────────────────────────── */
void mbd_init(void);
void *mbd_alloc(size_t requested_size);
void  mbd_free(void *ptr);
void *mbd_realloc(void *ptr, size_t new_size);
void *mbd_calloc(size_t nmemb, size_t size);
size_t mbd_malloc_usable_size(const void *ptr);
void  mbd_dump(void);

/* ── String Helpers ───────────────────────────────────────────────── */
typedef struct {
    const char* data;   // pointer to character data (may not be null-terminated)
    size_t      len;    // exact length in bytes
} mbd_string_view_t;

mbd_string_view_t mbd_string_view_from_cstr(const char *s);                 // Create string view from null-terminated C string. The view does **not** allocate or copy data.
mbd_string_view_t mbd_string_view_from_data(const char *data, size_t len);  // Create string view from raw data + exact length. ie. binary data, network buffers, or when you know the length.
char *mbd_string_view_dup(mbd_string_view_t view);                          // Allocate null-terminated C string copy from string view (uses mbd_alloc internally). Useful for classic C string.

#ifdef __cplusplus
}
#endif

/* ================================================================== *
 *  IMPLEMENTATION — define MYBUDDY_IMPLEMENTATION in exactly ONE .c  *
 * ================================================================== */
#ifdef MYBUDDY_IMPLEMENTATION

/* ── Internal types (not needed by callers) ───────────────────────── */
//#define CACHELINE_ALIGN    __attribute__((aligned(64)))

typedef struct block_header {
    uint32_t order;      // 4
    uint32_t used;       // 4
    uint32_t magic;      // 4
    uint32_t _padding;   // 4
    struct block_header *next;  // 8 or 4
    struct block_header *prev;  // 8 or 4
#if UINTPTR_MAX == 0xFFFFFFFFULL
    uint32_t _align_pad[2];     // +8 bytes on 32-bit → exactly 32 bytes
#endif
} block_header_t __attribute__((aligned(32)));

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

#define HEADER_SIZE        sizeof(block_header_t)
#define MIN_BLOCK          32

/**
 * @brief Thread Local Cache Data
 * Holds small-block free lists local to a specific thread to avoid
 * mutex contention on the global memory pool.
 */
typedef struct thread_cache_data {
    block_header_t *cache[SMALL_ORDER_MAX + 1];
    uint32_t        count[SMALL_ORDER_MAX + 1];
} thread_cache_data_t;

static uint8_t memory_pool[POOL_SIZE] __attribute__((aligned(32)));

static block_header_t *global_free_lists[MAX_ORDER + 1] = {0};
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t thread_cache_key;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static const uint32_t MAGIC_ALLOC  = 0xCAFEBABE;
static const uint32_t MAGIC_FREE   = 0xDEADBEEF;
static const uint32_t MAGIC_CACHED = 0xBAADF00D;

/* ================================================================== *
 *                       GLOBAL LIST OPERATIONS                       *
 * ================================================================== */

/**
 * @brief Inserts a block into the global doubly-linked free list.
 * @warning The caller MUST hold `global_lock` before calling this function.
 * 
 * @param order The power-of-two order (index) of the block.
 * @param block The block to insert.
 */
static void global_insert(uint32_t order, block_header_t *block) {
    block->prev = NULL;
    block->next = global_free_lists[order];
    if (global_free_lists[order]) global_free_lists[order]->prev = block;
    global_free_lists[order] = block;
}

/**
 * @brief Removes a block from the global doubly-linked free list (O(1)).
 * @warning The caller MUST hold `global_lock` before calling this function.
 * 
 * @param block The block to remove.
 * @param order The power-of-two order (index) of the block.
 */
static void global_remove(block_header_t *block, uint32_t order) {
    if (block->prev) block->prev->next = block->next;
    else             global_free_lists[order] = block->next;
    if (block->next) block->next->prev = block->prev;
    block->prev = block->next = NULL;
}

/* ================================================================== *
 *                       HELPER FUNCTIONS                             *
 * ================================================================== */

/**
 * @brief Rounds up a requested size to the nearest power of two.
 * Uses a standard bit-twiddling algorithm. Includes a fallback for 64-bit 
 * integers if compiled on a 64-bit architecture.
 * 
 * @param req The requested memory size in bytes.
 * @return uint32_t The nearest power of two >= req (minimum MIN_BLOCK).
 */
static inline uint32_t next_power_of_two(size_t req) {
    if (req <= MIN_BLOCK) return MIN_BLOCK;
    req--;
    req |= req >> 1; req |= req >> 2; req |= req >> 4;
    req |= req >> 8; req |= req >> 16;
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL
    req |= req >> 32;
#endif
    return (uint32_t)(req + 1);
}

/**
 * @brief Calculates the exact memory address of a block's "buddy".
 * Uses relative offset XOR math. Added alignment check to prevent
 * future maintenance footguns if someone calls it with the wrong order.
 */
static inline block_header_t *get_buddy(block_header_t *block, uint32_t order) {
    uintptr_t offset = (uintptr_t)block - (uintptr_t)memory_pool;

    /* Defensive check: block must be properly aligned for this order */
    if (offset & ((1ULL << order) - 1)) return NULL;

    uintptr_t buddy_offset = offset ^ (1ULL << order);
    if (buddy_offset >= POOL_SIZE) return NULL;
    return (block_header_t *)(memory_pool + buddy_offset);
}

/**
 * @brief Splits a large block down to the exact target order, inserting
 *        all buddy blocks into the global free list as it descends.
 * @warning Caller MUST hold `global_lock`. The input block must already
 *          have been removed from any free list.
 *
 * @param block        Block with order >= target_order
 * @param target_order Desired final order
 * @return block_header_t* The final block of exactly `target_order`
 */
static block_header_t* split_block_down(block_header_t *block, uint32_t target_order) {
    while (block->order > target_order) {
        uint32_t new_order = block->order - 1;
        block_header_t *buddy = get_buddy(block, new_order);
        buddy->order = new_order;
        buddy->used  = 0;
        buddy->magic = MAGIC_FREE;
        buddy->next = buddy->prev = NULL;
        global_insert(new_order, buddy);
        block->order = new_order;
    }
    return block;
}

/**
 * @brief Coalesces a block upward with its buddies as far as possible.
 * @warning Caller MUST hold `global_lock`.
 *
 * @param block Pointer to the newly-freed block
 * @param order Its current order
 * @return uint32_t Final coalesced order after all possible merges
 */
static uint32_t coalesce_up(block_header_t *block, uint32_t order) {
    while (order < MAX_ORDER) {
        block_header_t *buddy = get_buddy(block, order);
        if (!buddy || buddy->magic != MAGIC_FREE || buddy->order != order) break;
        global_remove(buddy, order);
        if ((uintptr_t)block > (uintptr_t)buddy) block = buddy;
        block->order = order + 1;
        order++;
    }
    return order;
}

/* ================================================================== *
 *                       THREAD LIFECYCLE                             *
 * ================================================================== */

/**
 * @brief POSIX Thread Destructor.
 * Automatically invoked by the OS when a thread terminates. Flushes all 
 * remaining cached blocks to the global pool, then safely frees the cache 
 * struct itself to completely prevent memory leaks.
 * 
 * @param arg Pointer to the thread's `thread_cache_data_t` struct.
 */
static void thread_cache_destructor(void *arg) {
    thread_cache_data_t *data = arg;
    if (!data) return;

    pthread_mutex_lock(&global_lock);

    /* Flush all cached blocks */
    for (int o = 0; o <= SMALL_ORDER_MAX; o++) {
        while (data->cache[o]) {
            block_header_t *block = data->cache[o];
            data->cache[o] = block->next;
            data->count[o]--;

            block->magic = MAGIC_FREE;
            uint32_t order = block->order;

            /* Use the safe coalescing helper */
            order = coalesce_up(block, order);
            global_insert(order, block);
        }
    }

    /* Return the cache struct itself to our own pool */
    block_header_t *cache_block = (block_header_t *)((uint8_t*)data - HEADER_SIZE);
    cache_block->used = 0;
    cache_block->magic = MAGIC_FREE;
    uint32_t co = cache_block->order;

    /* Use the new safe helper */
    co = coalesce_up(cache_block, co);
    global_insert(co, cache_block);

    pthread_mutex_unlock(&global_lock);
}

/**
 * @brief Internal allocator initialization.
 * Executed exactly once during the lifetime of the program via `pthread_once`.
 */
static void internal_init(void) {
    //memset(memory_pool, 0, POOL_SIZE); // The pool is already zeroed by the OS via .bss.
    for (int i = 0; i <= MAX_ORDER; i++) global_free_lists[i] = NULL;

    block_header_t *initial = (block_header_t *)memory_pool;
    initial->order = MAX_ORDER;
    initial->used  = 0;
    initial->magic = MAGIC_FREE;
    initial->next = initial->prev = NULL;
    global_insert(MAX_ORDER, initial);

    pthread_key_create(&thread_cache_key, thread_cache_destructor);
}

/**
 * @brief Retrieves the Thread-Local cache data.
 * If the thread does not have a cache, one is created. To remain completely
 * independent of the libc standard allocator (for LD_PRELOAD support), 
 * the cache structure is allocated directly from our own memory pool.
 * 
 * @return thread_cache_data_t* Pointer to the thread's cache, or NULL on OOM.
 */
static thread_cache_data_t *get_thread_cache(void) {
    pthread_once(&init_once, internal_init);

    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (!data) {
        /* Allocate cache struct from our own pool */
        pthread_mutex_lock(&global_lock);

        uint32_t needed = (uint32_t)sizeof(thread_cache_data_t) + HEADER_SIZE;
        uint32_t order  = 0;
        uint32_t size   = next_power_of_two(needed);
        while ((1U << order) < size) order++;

        uint32_t cur = order;
        while (cur <= MAX_ORDER && !global_free_lists[cur]) cur++;
        if (cur > MAX_ORDER) {
            pthread_mutex_unlock(&global_lock);
            return NULL;
        }

        block_header_t *block = global_free_lists[cur];
        global_remove(block, cur);

        /* Use the safe, non-duplicated helper */
        block = split_block_down(block, order);

        block->used  = 1;
        block->magic = MAGIC_ALLOC;
        block->next = block->prev = NULL;

        data = (thread_cache_data_t *)((uint8_t*)block + HEADER_SIZE);
        memset(data, 0, sizeof(thread_cache_data_t));

        if (pthread_setspecific(thread_cache_key, data) != 0) {
            /* setspecific failed → return block to pool */
            block->used = 0;
            block->magic = MAGIC_FREE;
            uint32_t o = block->order;

            /* FIXED: Use the safe coalescing helper */
            o = coalesce_up(block, o);
            global_insert(o, block);

            pthread_mutex_unlock(&global_lock);
            return NULL;
        }

        pthread_mutex_unlock(&global_lock);
    }
    return data;
}

/**
 * @brief Refills a thread's local cache from the global free list.
 * Will split larger blocks if exact-sized blocks are unavailable.
 * @warning The caller MUST hold `global_lock` before calling this function.
 * 
 * @param data Pointer to the thread's cache struct.
 * @param order The block order (size) to refill.
 */
static void refill_thread_cache(thread_cache_data_t *data, uint32_t order) {
    if (order > SMALL_ORDER_MAX) return;

    while (data->count[order] < THREAD_CACHE_SIZE) {
        block_header_t *block = global_free_lists[order];
        if (!block) {
            /* No exact match — find the next larger block and split it down */
            uint32_t cur = order + 1;
            while (cur <= MAX_ORDER && !global_free_lists[cur]) cur++;
            if (cur > MAX_ORDER) break;

            block = global_free_lists[cur];
            global_remove(block, cur);

            /* FIXED: Use the safe non-duplicated helper */
            block = split_block_down(block, order);
        } else {
            global_remove(block, order);
        }

        /* Add the (now exact-order) block to the thread-local cache */
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
    }
}

/* ================================================================== *
 *                       PUBLIC API                                   *
 * ================================================================== */

/**
 * @brief Explicitly initializes the allocator (Optional).
 * 
 * The allocator is fully self-initializing; it will automatically set itself
 * up on the first call to mbd_alloc(). However, if you want to pre-warm the
 * memory pool and prevent initialization latency on the first allocation, 
 * you can call this function during your application's startup phase.
 * 
 * This function is thread-safe and idempotent.
 */
void mbd_init(void) {
    pthread_once(&init_once, internal_init);
}

/**
 * @brief Allocates a block of memory of the specified size.
 * Tries the lock-free, O(1) Thread-Local Cache fast-path first. If empty
 * or the requested size is large, acquires the global lock and splits
 * blocks via standard Buddy system rules (now using the safe helper).
 * 
 * @param requested_size The size of memory requested in bytes.
 * @return void* Pointer to the 32-byte aligned payload, or NULL on OOM/error.
 */
void *mbd_alloc(size_t requested_size) {
    if (requested_size == 0) return NULL;
    if (requested_size > POOL_SIZE - HEADER_SIZE) {
        fprintf(stderr, "mybuddy: request too large\n");
        return NULL;
    }

    thread_cache_data_t *data = get_thread_cache();
    if (!data) return NULL;

    size_t needed = requested_size + HEADER_SIZE;
    uint32_t order = 0;
    uint32_t size  = next_power_of_two(needed);
    while ((1U << order) < size) order++;

    /* HOT PATH — lock-free */
    if (order <= SMALL_ORDER_MAX && data->cache[order]) {
        block_header_t *block = data->cache[order];
        data->cache[order] = block->next;
        data->count[order]--;
        block->used  = 1;
        block->magic = MAGIC_ALLOC;
        block->next = block->prev = NULL;
        return (void *)((uint8_t*)block + HEADER_SIZE);
    }

    pthread_mutex_lock(&global_lock);

    if (order <= SMALL_ORDER_MAX) {
        refill_thread_cache(data, order);
        if (data->cache[order]) {
            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            block->used  = 1;
            block->magic = MAGIC_ALLOC;
            block->next = block->prev = NULL;
            pthread_mutex_unlock(&global_lock);
            return (void *)((uint8_t*)block + HEADER_SIZE);
        }
    }

    /* Global slow path */
    uint32_t cur = order;
    while (cur <= MAX_ORDER && !global_free_lists[cur]) cur++;
    if (cur > MAX_ORDER) {
        pthread_mutex_unlock(&global_lock);
        return NULL;
    }

    block_header_t *block = global_free_lists[cur];
    global_remove(block, cur);

    /* FIXED: Use the safe, non-duplicated helper (eliminates the get_buddy bug) */
    block = split_block_down(block, order);

    block->used  = 1;
    block->magic = MAGIC_ALLOC;
    block->next = block->prev = NULL;

    pthread_mutex_unlock(&global_lock);
    return (void *)((uint8_t*)block + HEADER_SIZE);
}

/**
 * @brief Frees a previously allocated block of memory.
 * Includes bounds-checking (with underflow protection) and double-free 
 * protection. Small blocks are pushed into the lock-free Thread-Local cache. 
 * If the cache is full, a bulk flush triggers aggressive global Buddy 
 * coalescing via the safe helper.
 * 
 * @param ptr Pointer to the memory to free (can be NULL).
 */
void mbd_free(void *ptr) {
    if (!ptr) return;

    /* Underflow-protected pool bounds check (prevents reading before memory_pool) */
    if ((uintptr_t)ptr < (uintptr_t)memory_pool + HEADER_SIZE ||
        (uintptr_t)ptr >= (uintptr_t)memory_pool + POOL_SIZE) {
        fprintf(stderr, "mbd_free: pointer outside managed pool!\n");
        abort();
    }

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (block->magic != MAGIC_ALLOC) {
        fprintf(stderr, "mbd_free: DOUBLE-FREE or corruption!\n");
        abort();
    }

    block->used = 0;
    uint32_t order = block->order;

    /* Large blocks go straight to global */
    if (order > SMALL_ORDER_MAX) {
        pthread_mutex_lock(&global_lock);
        block->magic = MAGIC_FREE;

        /* Use the safe coalescing helper */
        order = coalesce_up(block, order);
        global_insert(order, block);

        pthread_mutex_unlock(&global_lock);
        return;
    }

    /* Small blocks — thread cache */
    thread_cache_data_t *data = get_thread_cache();
    if (!data) {
        fprintf(stderr, "mbd_free: thread cache alloc failed — freeing directly\n");
        pthread_mutex_lock(&global_lock);
        block->magic = MAGIC_FREE;

        /* Use the safe coalescing helper */
        order = coalesce_up(block, order);
        global_insert(order, block);

        pthread_mutex_unlock(&global_lock);
        return;
    }

    if (data->count[order] < THREAD_CACHE_SIZE) {
        block->magic = MAGIC_CACHED;
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        return;
    }

    /* Bulk flush (50% of cache) */
    pthread_mutex_lock(&global_lock);
    int flush_count = THREAD_CACHE_SIZE / 2;
    while (flush_count-- > 0 && data->cache[order]) {
        block_header_t *to_global = data->cache[order];
        data->cache[order] = to_global->next;
        data->count[order]--;

        to_global->magic = MAGIC_FREE;

        /* Use the safe coalescing helper */
        uint32_t o = to_global->order;
        o = coalesce_up(to_global, o);
        global_insert(o, to_global);
    }
    pthread_mutex_unlock(&global_lock);

    /* Now cache the current block */
    block->magic = MAGIC_CACHED;
    block->next = data->cache[order];
    data->cache[order] = block;
    data->count[order]++;
}

/**
 * @brief Reallocates a memory block to a new size.
 *        - If ptr is NULL → behaves like mbd_alloc()
 *        - If new_size is 0 → frees the block and returns NULL
 *        - If the new size fits inside the existing block, returns the
 *          same pointer (no copy, no lock).
 *        - Otherwise allocates a fresh block, copies data, and frees the old one.
 *
 * @param ptr      Old pointer (may be NULL)
 * @param new_size New requested payload size in bytes
 * @return void* New pointer, or NULL on failure / zero-size
 */
void *mbd_realloc(void *ptr, size_t new_size) {
    if (!ptr) return mbd_alloc(new_size);
    if (new_size == 0) {
        mbd_free(ptr);
        return NULL;
    }

    /* Underflow-protected bounds check */
    if ((uintptr_t)ptr < (uintptr_t)memory_pool + HEADER_SIZE ||
        (uintptr_t)ptr >= (uintptr_t)memory_pool + POOL_SIZE) {
        fprintf(stderr, "mbd_realloc: pointer outside managed pool!\n");
        abort();
    }

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);
    if (block->magic != MAGIC_ALLOC) {
        fprintf(stderr, "mbd_realloc: invalid pointer or double-free!\n");
        abort();
    }

    uint32_t old_order = block->order;
    size_t old_usable = ((size_t)1 << old_order) - HEADER_SIZE;

    if (new_size <= old_usable) {
        return ptr;
    }

    void *new_ptr = mbd_alloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_usable);
    mbd_free(ptr);
    return new_ptr;
}

/**
 * @brief Allocates memory for an array of nmemb elements of size bytes each
 *        and initializes all bytes to zero.
 */
void *mbd_calloc(size_t nmemb, size_t size) {
    if (size != 0 && nmemb > SIZE_MAX / size) return NULL;  // overflow guard
    size_t total = nmemb * size;
    if (total == 0) return NULL;

    void *ptr = mbd_alloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/**
 * @brief Returns the number of bytes actually usable in an allocated block.
 *        (Useful for string buffers, growing vectors, etc.)
 *
 * @param ptr Allocated pointer (must be valid)
 * @return size_t Usable payload size (≥ requested size)
 */
size_t mbd_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;

    /* Underflow-protected bounds check */
    if ((uintptr_t)ptr < (uintptr_t)memory_pool + HEADER_SIZE ||
        (uintptr_t)ptr >= (uintptr_t)memory_pool + POOL_SIZE) {
        return 0;
    }

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);
    if (block->magic != MAGIC_ALLOC) return 0; /* safety */

    return ((size_t)1 << block->order) - HEADER_SIZE;
}

/**
 * @brief Diagnostics Utility.
 * Prints the current state of the global free lists to stdout.
 * Note: Does not print blocks currently held in thread-local caches.
 */
void mbd_dump(void) {
    pthread_mutex_lock(&global_lock);
    printf("\n=== v16-FINAL Free Lists (1 GiB) ===\n");
    for (int i = 0; i <= MAX_ORDER; i++) {
        int count = 0;
        for (block_header_t *b = global_free_lists[i]; b; b = b->next) count++;
        if (count) printf("Global Order %2d (%8zu B) : %3d free\n", i, (size_t)1<<i, count);
    }
    printf("==================================\n\n");
    pthread_mutex_unlock(&global_lock);
}

/**
 * @brief Create a string view from a classic null-terminated C string.
 *        The view does **not** allocate or copy data.
 */
mbd_string_view_t mbd_string_view_from_cstr(const char *s) {
    if (!s) return (mbd_string_view_t){NULL, 0};
    return (mbd_string_view_t){s, strlen(s)};
}

/**
 * @brief Create a string view from raw data + exact length.
 *        The view does **not** allocate or copy data.
 */
mbd_string_view_t mbd_string_view_from_data(const char *data, size_t len) {
    return (mbd_string_view_t){data, len};
}

/**
 * @brief Allocate a null-terminated C string copy from a string view
 *        (uses mbd_alloc internally).
 */
char *mbd_string_view_dup(mbd_string_view_t view) {
    if (!view.data || view.len == 0) return NULL;

    char *copy = mbd_alloc(view.len + 1);
    if (copy) {
        memcpy(copy, view.data, view.len);
        copy[view.len] = '\0';
    }
    return copy;
}

#endif /* MYBUDDY_IMPLEMENTATION */
#endif /* MYBUDDY_H */
