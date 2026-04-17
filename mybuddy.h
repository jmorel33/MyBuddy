/**
 * @file mybuddy.h
 * @brief High-Performance Thread-Caching Buddy Allocator
 *
 * @version 1.4.5
 * @date April 14, 2026
 * @author Jacques Morel
 *
 * @note v1.4.5 adds official support for GCC / MinGW-w64 on Windows.
 *       Same API, same safety, same performance characteristics.
 *       Uses winpthreads + native mmap emulation. No MSVC support yet.
 *
 * @section overview Overview
 * MyBuddy (MBd) is a production-grade, highly concurrent memory allocator for high-performance C/C++ applications. It combines the anti-fragmentation
 * guarantees of a classic Buddy Allocator with the lock-free speed of per-thread caching.
 *
 * @section features Key Strengths
 * - **Crazy Fast**: Lock-free thread-local cache delivers allocations up to 8 KiB in just a few CPU cycles.
 * - **Fully Thread-Safe**: True per-thread caching with global locks grouped and acquired only on cache misses or large blocks.
 * - **Hardened & Safe**: Double-free protection, underflow-protected bounds checking, check-summed magic-value validation, and defused memalign exploits.
 * - **Memory Efficient**: Uses `MAP_NORESERVE` so virtual memory is only backed by physical RAM when used. High-order blocks (>2 MiB) are safely returned to the OS via `madvise` to prevent memory hoarding.
 * - **Advanced Alignment**: Mathematically guaranteed 32-byte minimum alignment, plus `mbd_memalign()` for stricter requirements.
 * - **Huge Allocations**: Requests over 128 MiB seamlessly bypass the buddy pool and use tracked direct `mmap()`/`munmap()`.
 * - **Production Readiness**: LD_PRELOAD-safe, self-initializing, includes atomic stats tracking (`mbd_get_stats`), and custom OOM handler hooks.
 *
 * @section multithreading Multithreading & Concurrency
 * - **No False Sharing**: Thread cache data structures are explicitly padded to 64-byte cachelines.
 * - **Anti-Hoarding & Thrashing Guard**: Full caches bulk-flush 50% of blocks. Mutex locks are batched during flushes to drastically reduce context switching.
 * - **Starvation Immunity**: If a thread's native arena runs dry, it automatically migrates and binds to an arena with available memory.
 *
 * @section usage Usage Scenarios
 * - **Tiny/Medium objects** (strings, ECS entities, 4 KiB pages): Stay in the lock-free cache thanks to `SMALL_ORDER_MAX = 13`.
 * - **Large objects** (8 KiB - 128 MiB): Handled by the global buddy path (fast O(1) doubly-linked list traversal).
 * - **Massive objects** (> 128 MiB): Seamlessly routed to direct OS mmaps.
 *
 * @section init Initialization & Teardown
 * The allocator is **self-initializing**. The first call to `mbd_alloc()` will automatically map arenas.
 * 
 * @note `mbd_destroy()` is provided strictly for unit-testing and clean process teardown. It asserts that only one thread is active. Do **not** call it while background threads are running.
 * 
 * @example
 * @code
 * #include "mybuddy.h"
 * 
 * int main() {
 *     // Lock-free fast path (falls under SMALL_ORDER_MAX)
 *     char *string_buffer = mbd_alloc(128);
 * 
 *     // AVX-512 strict alignment
 *     float *simd_data = mbd_memalign(64, 1024 * sizeof(float));
 * 
 *     // Huge allocation (bypasses buddy pool, uses direct mmap)
 *     void *massive_asset = mbd_alloc(1024 * 1024 * 256);
 * 
 *     mbd_free(string_buffer);
 *     mbd_free(simd_data);
 *     mbd_free(massive_asset);
 * 
 *     // Fetch accurate diagnostics
 *     mbd_stats_t stats = mbd_get_stats();
 *     
 *     mbd_destroy(); // Safe here, no other threads running
 *     return 0;
 * }
 * @endcode
 */
#ifndef MYBUDDY_H
#define MYBUDDY_H

#include <stddef.h> 
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Configuration Macros ---------------------------------------------------- */
/** @brief Configuration flags (can be combined with |) */
#define MBD_FLAG_HARDENED          (1u << 0)  // 1. Randomized magic + full safety checks
#define MBD_FLAG_ATOMIC_STATS      (1u << 1)  // 2. Update cached_bytes / cache_pressure on every alloc/free
#define MBD_FLAG_BUDDY_LARGE       (1u << 2)  // 3. Use buddy system for blocks > SMALL_ORDER_MAX (OFF = direct mmap)
#define MBD_FLAG_REALLOC_LOCK      (1u << 3)  // 4. Take lock even on realloc shrinks (OFF = early return)

#ifndef MAX_ORDER
#define MAX_ORDER          27
#endif
#ifndef MIN_ORDER
#define MIN_ORDER          6              // 64 bytes minimum block size
#endif
#ifndef SMALL_ORDER_MAX
#define SMALL_ORDER_MAX    16             // Includes 4 KiB pages
#endif

typedef struct {
    uint32_t flags;
    int arena_count;
    size_t pool_size;
    uint32_t cache_limits[SMALL_ORDER_MAX + 1];
} mbd_config_t;
/* -- Public API -------------------------------------------------------------- */

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
void  mbd_init(const mbd_config_t *config);

/**
 * @brief Destroys the allocator and unmaps all arenas.
 * Strictly for unit-testing and clean process teardown.
 */
void  mbd_destroy(void);

/**
 * @brief Allocates a block of memory of the specified size.
 * Tries the lock-free, O(1) Thread-Local Cache fast-path first. If empty
 * or the requested size is large, acquires the global lock and splits
 * blocks via standard Buddy system rules.
 *
 * @param requested_size The size of memory requested in bytes.
 * @return void* Pointer to the 32-byte aligned payload, or NULL on OOM/error.
 */
void *mbd_alloc(size_t requested_size);

/**
 * @brief Frees a previously allocated block of memory.
 * Includes bounds-checking (with underflow protection) and double-free
 * protection. Small blocks are pushed into the lock-free Thread-Local cache.
 * If the cache is full, a bulk flush triggers aggressive global Buddy
 * coalescing.
 *
 * @param ptr Pointer to the memory to free (can be NULL).
 */
void  mbd_free(void *ptr);

/**
 * @brief Reallocates a memory block to a new size.
 *        - If ptr is NULL -> behaves like mbd_alloc()
 *        - If new_size is 0 -> frees the block and returns NULL
 *        - If the new size fits inside the existing block, returns the
 *          same pointer (no copy, no lock).
 *        - Otherwise allocates a fresh block, copies data, and frees the old one.
 *
 * @param ptr      Old pointer (may be NULL).
 * @param new_size New requested payload size in bytes.
 * @return void* New pointer, or NULL on failure / zero-size.
 */
void *mbd_realloc(void *ptr, size_t new_size);

/**
 * @brief Allocates memory for an array of nmemb elements of size bytes each
 *        and initializes all bytes to zero.
 *
 * @param nmemb Number of elements.
 * @param size  Size of each element.
 * @return void* Pointer to the allocated memory, or NULL on failure / zero-size.
 */
void *mbd_calloc(size_t nmemb, size_t size);

/**
 * @brief Allocates memory with a specific alignment.
 *
 * @param alignment The required alignment (must be a power of two).
 * @param size      The size of memory requested in bytes.
 * @return void* Pointer to the aligned payload, or NULL on failure.
 */
void *mbd_memalign(size_t alignment, size_t size);

/**
 * @brief Returns the number of bytes actually usable in an allocated block.
 *        (Useful for string buffers, growing vectors, etc.)
 *
 * @param ptr Allocated pointer (must be valid).
 * @return size_t Usable payload size (>= requested size).
 */
size_t mbd_malloc_usable_size(const void *ptr);

/**
 * @brief Allocator statistics.
 */
typedef struct {
    size_t total_mapped_bytes;
    size_t total_allocated_bytes;
    size_t total_free_bytes;
    size_t total_cached_bytes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t bulk_flushes;
    uint64_t splits;
    uint64_t coalesces;
} mbd_stats_t;

/**
 * @brief Returns accurate diagnostics of mapped, allocated, and free bytes.
 *
 * @return mbd_stats_t Diagnostics information.
 */
mbd_stats_t mbd_get_stats(void);

/**
 * @brief Profiler event types.
 */
typedef enum {
    MBD_EVENT_ALLOC,
    MBD_EVENT_FREE,
    MBD_EVENT_SPLIT,
    MBD_EVENT_COALESCE,
    MBD_EVENT_FLUSH
} mbd_event_type_t;

/**
 * @brief Sets a custom profiler hook.
 *
 * @param hook Function pointer to the hook.
 */
void mbd_set_profiler_hook(void (*hook)(mbd_event_type_t, void*, size_t));

/**
 * @brief Forces a trim of all thread caches, returning memory to the global arena.
 * @note **Heavy Operation**: This triggers a cooperative trim where every thread will
 *       completely flush its local cache on its next allocation or free. This causes a
 *       100% cache miss rate immediately following the trim. Use only for low memory
 *       emergencies, not for periodic lightweight usage.
 * @warning **Not async-signal-safe**: Do NOT call from signal handlers. The trim flag
 *          is atomic, but the flush operation acquires mutexes and may deadlock if
 *          a signal interrupts a lock.
 */
void mbd_trim(void);

/**
 * @brief Sets a custom Out-Of-Memory handler hook.
 *
 * @param handler Function pointer to the handler.
 */
void mbd_set_oom_handler(void (*handler)(void));

/**
 * @brief Diagnostics Utility.
 * Prints the current state of the global free lists to stdout.
 * Note: Does not print blocks currently held in thread-local caches.
 */
void mbd_dump(void);

#ifdef __cplusplus
}
#endif

/* ================================================================== *
 *  IMPLEMENTATION -- define MYBUDDY_IMPLEMENTATION in exactly ONE .c  *
 * ================================================================== */
#ifdef MYBUDDY_IMPLEMENTATION

#if defined(__MINGW32__) || defined(__MINGW64__)
#include <windows.h>
#include <bcrypt.h>        // BCryptGenRandom (available in MinGW-w64)
#pragma comment(lib, "bcrypt.lib")  // only needed for MinGW linking
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <stdatomic.h>
#include <assert.h>
#include <time.h>

/* Windows / MinGW compatibility */
#if defined(__MINGW32__) || defined(__MINGW64__)
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MADV_DONTNEED
#define MADV_DONTNEED 0
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 0
#endif
#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 0
#endif
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#ifndef MADV_DONTNEED
#ifdef MADV_FREE
#define MADV_DONTNEED MADV_FREE
#else
#define MADV_DONTNEED 0
#endif
#endif

/* -- Internal types (not needed by callers) -------------------------------- */
//#define CACHELINE_ALIGN    __attribute__((aligned(64)))

struct mbd_arena;

typedef struct block_header {
    _Atomic(uint8_t) order;      // 1
    uint8_t used;       // 1
    uint8_t is_mmap;    // 1 (Direct mmap flag)
    uint8_t _padding;   // 1
    _Atomic(uint32_t) magic;     // 4 (always 32-bit to fit in 32-byte header on 64-bit systems)
    _Atomic(struct mbd_arena *) arena; // 8 or 4
    _Atomic(struct block_header *) next;  // 8 or 4
    union {
        struct block_header *prev;  // 8 or 4
        size_t mmap_size;           // Used when is_mmap == 1
    };
} block_header_t __attribute__((aligned(32)));

// Prevent accidental header bloat
static_assert(sizeof(block_header_t) == 32,
              "block_header_t size changed; verify padding/alignment");

typedef struct mbd_arena {
    pthread_mutex_t lock;
    pthread_mutex_t remote_lock;
    block_header_t *free_lists[MAX_ORDER + 1];
    uint8_t *memory_pool;
    struct {
        block_header_t *head;
    } remote_free_queue;
    _Atomic size_t cached_bytes;
    _Atomic size_t cache_pressure;
    _Atomic uint64_t splits;
    _Atomic uint64_t coalesces;
    _Atomic int active;
} __attribute__((aligned(64))) mbd_arena_t;

#define HEADER_SIZE        sizeof(block_header_t)

/**
 * @brief Thread Local Cache Data
 * Holds small-block free lists local to a specific thread to avoid
 * mutex contention on the global memory pool.
 */
typedef struct thread_cache_data {
    block_header_t *cache[SMALL_ORDER_MAX + 1];
    uint32_t        count[SMALL_ORDER_MAX + 1];
    _Atomic(mbd_arena_t *) arena;
    mbd_arena_t *native_arena;
    _Atomic(uint64_t) cache_hits;
    _Atomic(uint64_t) cache_misses;
    _Atomic(uint64_t) bulk_flushes;
    int             last_trim_request;
    struct thread_cache_data *next;
} thread_cache_data_t;

static mbd_arena_t *arenas = NULL;

static mbd_config_t global_config = {
    .flags = 0,
    .arena_count = 0,
    .pool_size = (1ULL << 27) // 128 MiB default
};

static mbd_config_t pending_config = {0};
static _Atomic int config_set = 0;

static int arena_count = 1;

static _Atomic uint32_t thread_counter = 0;
static _Atomic uint32_t active_threads = 0;
static _Atomic size_t huge_mmap_tracked = 0;

static long os_page_size = 4096;
static pthread_mutex_t cache_list_lock = PTHREAD_MUTEX_INITIALIZER;
static thread_cache_data_t *global_cache_list = NULL;
static _Atomic int trim_requested = 0;
static _Atomic int fully_destroyed = 0;

static pthread_key_t thread_cache_key;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static _Atomic(void (*)(void)) global_oom_handler = NULL;
static _Atomic(void (*)(mbd_event_type_t, void*, size_t)) global_profiler_hook = NULL;

#ifdef MYBUDDY_ENABLE_PROFILING
    #define MBD_FIRE_EVENT(type, ptr, sz) \
        do { \
            void (*hook)(mbd_event_type_t, void*, size_t) = atomic_load(&global_profiler_hook); \
            if (__builtin_expect(hook != NULL, 0)) \
                hook(type, ptr, sz); \
        } while(0)
#else
    #define MBD_FIRE_EVENT(type, ptr, sz) do {} while(0)
#endif

static const uint32_t MAGIC_ALLOC    = 0xCAFEBABE;
static const uint32_t MAGIC_FREE     = 0xDEADBEEF;
static const uint32_t MAGIC_CACHED   = 0xBAADF00D;
static const uint32_t MAGIC_MEMALIGN = 0x00000A11;
static const uint32_t MAGIC_MMAP     = 0x8BADF00D;

static uint32_t mbd_secret_key = 0;

static inline int arena_index(const mbd_arena_t *a) {
    return (int)(a - arenas);
}

/**
 * @brief Encodes the magic value using the global secret key and the block's address.
 *
 * @param block The block header.
 * @param magic The base magic value.
 * @return uint32_t The encoded magic value.
 */
static inline uint32_t encode_magic(void *block, uint32_t magic) {
    if (!(global_config.flags & MBD_FLAG_HARDENED)) return magic;
    // Improved address hash with better diffusion (xxhash32-inspired)
    uintptr_t addr = (uintptr_t)block;
    uint32_t h = (uint32_t)(addr ^ (addr >> 32));
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return magic ^ h ^ mbd_secret_key;
}

static void mbd_init_secret_key(void) {
#if defined(__linux__)
    if (syscall(SYS_getrandom, &mbd_secret_key, sizeof(mbd_secret_key), 0) == sizeof(mbd_secret_key))
        return;
#elif defined(__MINGW32__) || defined(__MINGW64__)
    if (BCryptGenRandom(NULL, (PUCHAR)&mbd_secret_key, sizeof(mbd_secret_key), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == STATUS_SUCCESS)
        return;
#endif
    mbd_secret_key = (uint32_t)(uintptr_t)&mbd_secret_key ^ (uint32_t)time(NULL) ^ 0x55AA55AA;
}


/**
 * @brief Loads the magic value from a block header safely using relaxed atomic operations.
 *
 * @param b The block header.
 * @return uint32_t The raw, loaded magic value.
 */
static inline uint32_t load_magic(const block_header_t *b) {
    return atomic_load_explicit(&b->magic, memory_order_acquire);
}

/* ================================================================== *
 *                       ARENA LIST OPERATIONS                        *
 * ================================================================== */

static void arena_insert(mbd_arena_t *arena, uint32_t order, block_header_t *block) {
    block->prev = NULL;
    block->next = arena->free_lists[order];
    if (arena->free_lists[order]) arena->free_lists[order]->prev = block;
    arena->free_lists[order] = block;
    block->arena = arena;
}

static void arena_remove(mbd_arena_t *arena, block_header_t *block, uint32_t order) {
    if (block->prev) block->prev->next = block->next;
    else             arena->free_lists[order] = block->next;
    if (block->next) block->next->prev = block->prev;
    block->prev = block->next = NULL;
}

/* ================================================================== *
 *                       HELPER FUNCTIONS                             *
 * ================================================================== */

static inline uint32_t get_cache_limit(uint32_t order) {
    if (order > SMALL_ORDER_MAX) return 0;
    return global_config.cache_limits[order];
}

static inline uint32_t next_power_of_two_order(size_t req) {
    if (req <= (1ULL << MIN_ORDER)) {
        return MIN_ORDER;
    }
#if defined(__GNUC__) || defined(__clang__)
    return 64 - __builtin_clzll(req - 1);
#else
    req--;
    uint32_t order = 0;
    while (req > 0) {
        req >>= 1;
        order++;
    }
    return order;
#endif
}

/**
 * @brief Returns the buddy block of a given block within an arena.
 *
 * @param arena The arena containing the block.
 * @param block The block to find the buddy for.
 * @param order The current order (size class) of the block.
 * @return block_header_t* Pointer to the buddy block.
 */
static inline block_header_t *get_buddy(mbd_arena_t *arena, block_header_t *block, uint32_t order) {
    uintptr_t offset = (uintptr_t)block - (uintptr_t)arena->memory_pool;
    if (offset & ((1ULL << order) - 1)) return NULL;
    uintptr_t buddy_offset = offset ^ (1ULL << order);
    if (buddy_offset >= global_config.pool_size) return NULL;
    return (block_header_t *)(arena->memory_pool + buddy_offset);
}

static block_header_t* split_block_down(mbd_arena_t *arena, block_header_t *block, uint32_t target_order) {
    uint32_t orig_order = block->order;
    while (block->order > target_order) {
        uint32_t new_order = block->order - 1;
        block_header_t *buddy = get_buddy(arena, block, new_order);
        buddy->order = new_order;
        buddy->used  = 0;
        buddy->is_mmap = 0;
        buddy->magic = encode_magic(buddy, MAGIC_FREE);

        buddy->arena = arena;
        buddy->next = buddy->prev = NULL;
        arena_insert(arena, new_order, buddy);
        block->order = new_order;
    }
    atomic_fetch_add(&arena->splits, orig_order - target_order);
    MBD_FIRE_EVENT(MBD_EVENT_SPLIT, block, orig_order - target_order);
    return block;
}

static block_header_t* coalesce_up_and_update(mbd_arena_t *arena, block_header_t *block, uint32_t *order_out) {
    uint32_t order = *order_out;
    while (order < MAX_ORDER) {
        block_header_t *buddy = get_buddy(arena, block, order);
        if (!buddy) break;

        // TSAN-safe reads
        if (load_magic(buddy) != encode_magic(buddy, MAGIC_FREE) ||
            atomic_load_explicit(&buddy->order, memory_order_acquire) != order ||
            atomic_load_explicit(&buddy->arena, memory_order_acquire) != arena)
            break;

        if (buddy->prev == NULL && arena->free_lists[order] != buddy)
            break;
            
        arena_remove(arena, buddy, order);
        atomic_fetch_add(&arena->coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, order);

        if ((uintptr_t)block > (uintptr_t)buddy) block = buddy;
        block->order = order + 1;
        order++;
    }
    /* Safe madvise: Only advise the payload pages, preserving the header page */
    if (order >= 21) {
#if defined(__linux__) || defined(__APPLE__) || (defined(__MINGW32__) || defined(__MINGW64__))
        uintptr_t block_addr = (uintptr_t)block;
        uintptr_t adv_start = (block_addr + HEADER_SIZE + os_page_size - 1) & ~(os_page_size - 1);
        uintptr_t adv_end = block_addr + (1ULL << order);
        
        if (adv_end > adv_start) {
            madvise((void *)adv_start, adv_end - adv_start, MADV_DONTNEED);
        }
#endif
    }
    *order_out = order;
    return block;
}
static void drain_remote_queue(mbd_arena_t *arena) {
    pthread_mutex_lock(&arena->remote_lock);
    block_header_t *head = arena->remote_free_queue.head;
    arena->remote_free_queue.head = NULL;
    pthread_mutex_unlock(&arena->remote_lock);
    while (head) {
        block_header_t *next = atomic_load_explicit(&head->next, memory_order_acquire);
        atomic_store_explicit(&head->magic, encode_magic(head, MAGIC_FREE), memory_order_release);

        uint32_t order = atomic_load_explicit(&head->order, memory_order_acquire);
        head = coalesce_up_and_update(arena, head, &order);
        arena_insert(arena, order, head);
        head = next;
    }
}


/**
 * @brief Handles Out-Of-Memory (OOM) conditions.
 * Calls the user-registered OOM hook if present.
 */
static void handle_oom(void) {
    void (*hook)(void) = atomic_load(&global_oom_handler);
    if (hook) hook();
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

    pthread_mutex_lock(&cache_list_lock);
    thread_cache_data_t **curr = &global_cache_list;
    while (*curr) {
        if (*curr == data) {
            *curr = data->next;
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&cache_list_lock);

    /* Flush all cached blocks by pushing them to their owning arena's remote free queue. */
    /* This avoids taking the primary arena lock during thread destruction. */
    for (int o = MIN_ORDER; o <= SMALL_ORDER_MAX; o++) {
        while (data->cache[o]) {
            block_header_t *block = data->cache[o];
            data->cache[o] = block->next;
            
            mbd_arena_t *block_arena = block->arena;


            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

            atomic_fetch_sub(&block_arena->cached_bytes, 1ULL << o);
            atomic_fetch_sub(&block_arena->cache_pressure, 1ULL << o);

            if (atomic_load(&block_arena->active)) {
                pthread_mutex_lock(&block_arena->remote_lock);
                block->next = block_arena->remote_free_queue.head;
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
                block_arena->remote_free_queue.head = block;
                pthread_mutex_unlock(&block_arena->remote_lock);
            }
        }
    }

    /* Return the cache struct itself via the remote free queue */
    block_header_t *cache_block = (block_header_t *)((uint8_t*)data - HEADER_SIZE);
    atomic_store_explicit(&cache_block->magic, encode_magic(cache_block, MAGIC_FREE), memory_order_release);
    mbd_arena_t *cache_arena = cache_block->arena;

    if (atomic_load(&cache_arena->active)) {
        pthread_mutex_lock(&cache_arena->remote_lock);
        cache_block->next = cache_arena->remote_free_queue.head;
        atomic_store_explicit(&cache_block->magic, encode_magic(cache_block, MAGIC_FREE), memory_order_release);
        cache_arena->remote_free_queue.head = cache_block;
        pthread_mutex_unlock(&cache_arena->remote_lock);
    }

    atomic_fetch_sub(&active_threads, 1);
}

/**
 * @brief Internal allocator initialization.
 * Executed exactly once during the lifetime of the program via `pthread_once`.
 */
// Keep a small hard limit on memory so we don't blow up 32-bit systems
#define MBD_MAX_ARENAS 16
static mbd_arena_t static_arenas[MBD_MAX_ARENAS];

static void internal_init(void) {
    if (atomic_load(&config_set)) {
        global_config = pending_config;
    }
    int limits_uninitialized = 1;
    for (uint32_t i = 0; i <= SMALL_ORDER_MAX; i++) {
        if (global_config.cache_limits[i] != 0) {
            limits_uninitialized = 0;
            break;
        }
    }

    // We assume limits are uninitialized only if all limits are 0.
    // If a user genuinely wants to disable the cache by setting everything to 0,
    // they can just set SMALL_ORDER_MAX to 0. Otherwise this defaults behavior.
    if (limits_uninitialized) {
        for (uint32_t order = 0; order <= SMALL_ORDER_MAX; order++) {
            if (order <= 8)  global_config.cache_limits[order] = 256;
            else if (order <= 10) global_config.cache_limits[order] = 64;
            else if (order <= 12) global_config.cache_limits[order] = 32;
            else global_config.cache_limits[order] = 16;
        }
    }
    if (global_config.pool_size == 0) {
        global_config.pool_size = (1ULL << 27);
    } else {
        if (global_config.pool_size > (1ULL << MAX_ORDER)) {
            global_config.pool_size = (1ULL << MAX_ORDER);
        }
        // Ensure pool_size is a power of 2
        size_t p = 1;
        while (p < global_config.pool_size) p <<= 1;
        global_config.pool_size = p;
    }
    long sc_page = sysconf(_SC_PAGESIZE);
    if (sc_page > 0) os_page_size = sc_page;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    mbd_init_secret_key();
    if (global_config.arena_count > 0) {
        arena_count = global_config.arena_count;
    } else {
        arena_count = (cores > 0) ? (2 * cores) : 2;
    }
    if (arena_count > MBD_MAX_ARENAS) arena_count = MBD_MAX_ARENAS;

    arenas = static_arenas;

    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_init(&arenas[a].lock, NULL);
        pthread_mutex_init(&arenas[a].remote_lock, NULL);
        arenas[a].remote_free_queue.head = NULL;
        atomic_init(&arenas[a].cached_bytes, 0);
        atomic_init(&arenas[a].cache_pressure, 0);
        atomic_init(&arenas[a].splits, 0);
        atomic_init(&arenas[a].coalesces, 0);
        atomic_init(&arenas[a].active, 1);
        for (int i = 0; i <= MAX_ORDER; i++) arenas[a].free_lists[i] = NULL;
        
        arenas[a].memory_pool = (uint8_t *)mmap(NULL, global_config.pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (arenas[a].memory_pool == MAP_FAILED) abort();
#if defined(__linux__) || defined(__APPLE__) || (defined(__MINGW32__) || defined(__MINGW64__))
#if defined(MADV_HUGEPAGE)
        madvise(arenas[a].memory_pool, global_config.pool_size, MADV_HUGEPAGE);
#endif
#if defined(MADV_DONTDUMP)
        madvise(arenas[a].memory_pool, global_config.pool_size, MADV_DONTDUMP);
#endif
#endif

        // Find max order that fits in pool_size
        uint32_t max_order = 0;
        size_t temp_size = global_config.pool_size;
        while (temp_size > 1) {
            temp_size >>= 1;
            max_order++;
        }
        if (max_order > MAX_ORDER) max_order = MAX_ORDER;

        block_header_t *initial = (block_header_t *)arenas[a].memory_pool;
        initial->order = max_order;
        initial->used  = 0;
        initial->is_mmap = 0;
        atomic_store_explicit(&initial->magic, encode_magic(initial, MAGIC_FREE), memory_order_release);
        initial->arena = &arenas[a];
        initial->next = initial->prev = NULL;
        
        arenas[a].free_lists[max_order] = initial;
    }

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
static void flush_my_cache(thread_cache_data_t *curr);
static thread_cache_data_t *get_thread_cache(void) {
    if (atomic_load(&fully_destroyed)) return NULL;

    pthread_once(&init_once, internal_init);

    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (!data) {
        uint32_t t_id = atomic_fetch_add(&thread_counter, 1);
        mbd_arena_t *arena = &arenas[t_id % arena_count];

        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);

        uint32_t needed = (uint32_t)sizeof(thread_cache_data_t) + HEADER_SIZE;
        uint32_t order  = next_power_of_two_order(needed);
        uint32_t cur = order;

        while (cur <= MAX_ORDER && !arena->free_lists[cur]) cur++;
        
        block_header_t *block = NULL;
        mbd_arena_t *target_arena = arena;

        if (cur <= MAX_ORDER) {
            block = arena->free_lists[cur];
            arena_remove(arena, block, cur);
            block = split_block_down(arena, block, order);
            
            block->used  = 1;
            block->is_mmap = 0;
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            block->arena = arena;
            block->next = block->prev = NULL;

            pthread_mutex_unlock(&arena->lock);
        } else {
            pthread_mutex_unlock(&arena->lock);
            
            /* Fallback to other arenas */
            for (int i = 0; i < arena_count; i++) {
                mbd_arena_t *other_arena = &arenas[i];
                if (other_arena == arena) continue;
                
                pthread_mutex_lock(&other_arena->lock);
                drain_remote_queue(other_arena);
                cur = order;
                while (cur <= MAX_ORDER && !other_arena->free_lists[cur]) cur++;
                
                if (cur <= MAX_ORDER) {
                    block = other_arena->free_lists[cur];
                    arena_remove(other_arena, block, cur);
                    block = split_block_down(other_arena, block, order);
                    
                    block->used  = 1;
                    block->is_mmap = 0;
                    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
                    block->arena = other_arena;
                    block->next = block->prev = NULL;

                    target_arena = other_arena;
                    pthread_mutex_unlock(&other_arena->lock);
                    break;
                }
                pthread_mutex_unlock(&other_arena->lock);
            }
        }
        
        if (!block) {
            return NULL;
        }

        data = (thread_cache_data_t *)((uint8_t*)block + HEADER_SIZE);
        memset(data, 0, sizeof(thread_cache_data_t));
        data->native_arena = target_arena;
        atomic_store(&data->arena, target_arena);

        if (pthread_setspecific(thread_cache_key, data) != 0) {
            pthread_mutex_lock(&target_arena->lock);
            drain_remote_queue(target_arena);

            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

            uint32_t o = block->order;
            block = coalesce_up_and_update(target_arena, block, &o);
            arena_insert(target_arena, o, block);
            pthread_mutex_unlock(&target_arena->lock);
            return NULL;
        }

        data->next = NULL;
        pthread_mutex_lock(&cache_list_lock);
        data->next = global_cache_list;
        global_cache_list = data;
        pthread_mutex_unlock(&cache_list_lock);

        atomic_fetch_add(&active_threads, 1);
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
static void refill_thread_cache(thread_cache_data_t *data, mbd_arena_t *locked_arena, uint32_t order) {
    if (order > SMALL_ORDER_MAX || order < MIN_ORDER) return;

    uint32_t limit = get_cache_limit(order);
    while (data->count[order] < limit) {

        block_header_t *block = locked_arena->free_lists[order];
        if (!block) {
            uint32_t cur = order + 1;
            while (cur <= MAX_ORDER && !locked_arena->free_lists[cur]) cur++;
            if (cur > MAX_ORDER) {
                break;
            }

            block = locked_arena->free_lists[cur];
            arena_remove(locked_arena, block, cur);
            block = split_block_down(locked_arena, block, order);
        } else {
            arena_remove(locked_arena, block, order);
        }

        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);
        block->arena = locked_arena;
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        atomic_fetch_add(&locked_arena->cached_bytes, 1ULL << order);
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
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

void mbd_init(const mbd_config_t *config) {
    if (config && !atomic_load(&config_set)) {
        pthread_mutex_lock(&init_mutex);
        if (!atomic_load(&config_set)) {
            pending_config = *config;
            atomic_store(&config_set, 1);
        }
        pthread_mutex_unlock(&init_mutex);
    }
    pthread_once(&init_once, internal_init);
}

/**
 * @brief Destroys the allocator and unmaps all arenas.
 * Strictly for unit-testing and clean process teardown.
 */
void mbd_destroy(void) {
    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (data) {
        thread_cache_destructor(data); /* Flush our own cache back to arenas */
        pthread_setspecific(thread_cache_key, NULL);
    }

    assert(atomic_load(&active_threads) == 0 && "mbd_destroy called while other threads are active!");
    
    for (int a = 0; a < arena_count; a++) {
        atomic_store(&arenas[a].active, 0);
    }

    for (int a = 0; a < arena_count; a++) {
        if (arenas[a].memory_pool && arenas[a].memory_pool != MAP_FAILED) {
            munmap(arenas[a].memory_pool, global_config.pool_size);
            arenas[a].memory_pool = NULL;
        }
        pthread_mutex_destroy(&arenas[a].lock);
        pthread_mutex_destroy(&arenas[a].remote_lock);
    }
    pthread_key_delete(thread_cache_key);

    arenas = NULL;
    arena_count = 1;
    atomic_store(&thread_counter, 0);
    atomic_store(&huge_mmap_tracked, 0);
    atomic_store(&trim_requested, 0);
    atomic_store(&active_threads, 0);
    global_cache_list = NULL;

    atomic_store(&fully_destroyed, 1);
}

/**
 * @brief Sets a custom Out-Of-Memory handler hook.
 *
 * @param handler Function pointer to the handler.
 */
void mbd_set_oom_handler(void (*handler)(void)) {
    atomic_store(&global_oom_handler, handler);
}

/**
 * @brief Sets a custom profiler hook.
 *
 * @param hook Function pointer to the hook.
 */
void mbd_set_profiler_hook(void (*hook)(mbd_event_type_t, void*, size_t)) {
    atomic_store(&global_profiler_hook, hook);
}


/**
 * @brief Allocates a block of memory of the specified size.
 * Tries the lock-free, O(1) Thread-Local Cache fast-path first. If empty
 * or the requested size is large, acquires the global lock and splits
 * blocks via standard Buddy system rules.
 *
 * @param requested_size The size of memory requested in bytes.
 * @return void* Pointer to the 32-byte aligned payload, or NULL on OOM/error.
 */
void *mbd_alloc(size_t requested_size) {

    if (requested_size == 0) requested_size = 1;

    if (requested_size > SIZE_MAX - HEADER_SIZE) {
        handle_oom();
        return NULL;
    }
    size_t needed = requested_size + HEADER_SIZE;

    /* Fallback to direct mmap for gigantic allocations (> 128 MB) */
    if (needed > global_config.pool_size) {
        block_header_t *block = (block_header_t *)mmap(NULL, needed, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            handle_oom();
            return NULL;
        }
#if defined(__linux__) || defined(__APPLE__) || (defined(__MINGW32__) || defined(__MINGW64__))
#if defined(MADV_HUGEPAGE)
        if (needed >= (2 * 1024 * 1024)) {
            madvise(block, needed, MADV_HUGEPAGE);
        }
#endif
#endif
        block->is_mmap = 1;
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_MMAP), memory_order_release);
        block->arena = NULL;
        block->mmap_size = needed; // Store allocation size in unused prev pointer
        atomic_fetch_add(&huge_mmap_tracked, needed);
        void *res = (void *)((uint8_t*)block + HEADER_SIZE);
        MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
        return res;
    }

    thread_cache_data_t *data = get_thread_cache();
    if (!data) { handle_oom(); return NULL; }
    if (__builtin_expect(atomic_load(&trim_requested) != data->last_trim_request, 0)) {
        flush_my_cache(data);
        data->last_trim_request = atomic_load(&trim_requested);
    }
    mbd_arena_t *arena = atomic_load(&data->arena);

    uint32_t order = next_power_of_two_order(needed);

    /* HOT PATH -- lock-free */


    if (order <= SMALL_ORDER_MAX && data->cache[order]) {
        block_header_t *block = data->cache[order];
        data->cache[order] = block->next;
        data->count[order]--;
        atomic_fetch_sub(&arena->cached_bytes, 1ULL << order);
        atomic_fetch_sub(&arena->cache_pressure, 1ULL << order);
        block->used  = 1;
        block->is_mmap = 0;
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
        atomic_fetch_add_explicit(&data->cache_hits, 1, memory_order_relaxed);
        // Notice: We intentionally do NOT overwrite block->arena.
        block->next = block->prev = NULL;
        void *res = (void *)((uint8_t*)block + HEADER_SIZE);
        MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
        return res;
    }

    pthread_mutex_lock(&arena->lock);
    drain_remote_queue(arena);

    if (order <= SMALL_ORDER_MAX) {
        if (data) {
            atomic_fetch_add_explicit(&data->cache_misses, 1, memory_order_relaxed);
        }
        refill_thread_cache(data, arena, order);
        if (data->cache[order]) {

            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            atomic_fetch_sub(&arena->cached_bytes, 1ULL << order);
            atomic_fetch_sub(&arena->cache_pressure, 1ULL << order);
            block->used  = 1;
            block->is_mmap = 0;
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            block->next = block->prev = NULL;
            pthread_mutex_unlock(&arena->lock);
            void *res = (void *)((uint8_t*)block + HEADER_SIZE);
            MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
            return res;
        }
    }

    /* Global slow path */
    uint32_t cur = order;
    while (cur <= MAX_ORDER && !arena->free_lists[cur]) cur++;
    
    if (cur > MAX_ORDER) {
        pthread_mutex_unlock(&arena->lock);
        
        block_header_t *block = NULL;
        for (int i = 0; i < arena_count; i++) {
            mbd_arena_t *other_arena = &arenas[i];
            if (other_arena == arena) continue;
            
            pthread_mutex_lock(&other_arena->lock);
            drain_remote_queue(other_arena);
            cur = order;
            while (cur <= MAX_ORDER && !other_arena->free_lists[cur]) cur++;
            
            if (cur <= MAX_ORDER) {
                block = other_arena->free_lists[cur];
                arena_remove(other_arena, block, cur);
                block = split_block_down(other_arena, block, order);
                block->used  = 1;
                block->is_mmap = 0;
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
                block->arena = other_arena;
                block->next = block->prev = NULL;
                pthread_mutex_unlock(&other_arena->lock);

                /* NEW: Migrate thread to the arena that actually has memory */
                atomic_store(&data->arena, other_arena); 

                /* Probabilistic return to native arena to prevent long-term crowding */
                if ((atomic_load_explicit(&data->cache_misses, memory_order_relaxed) & 0x3F) == 0) {          // 1 in 64 chance
                    atomic_store(&data->arena, data->native_arena);
                }

                void *res = (void *)((uint8_t*)block + HEADER_SIZE);
                MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
                return res;
            }
            pthread_mutex_unlock(&other_arena->lock);
        }
        
        handle_oom();
        return NULL;
    }

    block_header_t *block = arena->free_lists[cur];
    arena_remove(arena, block, cur);
    block = split_block_down(arena, block, order);

    block->used  = 1;
    block->is_mmap = 0;
    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
    block->arena = arena;
    block->next = block->prev = NULL;

    pthread_mutex_unlock(&arena->lock);
    void *res = (void *)((uint8_t*)block + HEADER_SIZE);
        MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
        return res;
}

/**
 * @brief Frees a previously allocated block of memory.
 * Includes bounds-checking (with underflow protection) and double-free
 * protection. Small blocks are pushed into the lock-free Thread-Local cache.
 * If the cache is full, a bulk flush triggers aggressive global Buddy
 * coalescing.
 *
 * @param ptr Pointer to the memory to free (can be NULL).
 */
void mbd_free(void *ptr) {

    if (!ptr) return;

    if (atomic_load(&fully_destroyed)) return;   // prevent use-after-destroy

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (load_magic(block) == encode_magic(block, MAGIC_MEMALIGN)) {
        void *raw = block->prev;
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release); /* Defuse double-free vulnerability, but keep it MAGIC_FREE to fail consistently */
        mbd_free(raw);
        return;
    }

    if (block->is_mmap && load_magic(block) == encode_magic(block, MAGIC_MMAP)) {
        size_t mmap_size = block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(block, mmap_size);
        return;
    }

    if (load_magic(block) != encode_magic(block, MAGIC_ALLOC)) {
        fprintf(stderr, "mbd_free: DOUBLE-FREE or corruption! ptr=%p block=%p magic=%x alloc=%x cached=%x free=%x\n", ptr, block, load_magic(block), encode_magic(block, MAGIC_ALLOC), encode_magic(block, MAGIC_CACHED), encode_magic(block, MAGIC_FREE));
        abort();
    }

    mbd_arena_t *arena = block->arena;
    if (!arena || arena < arenas || arena >= arenas + arena_count) abort();
    if ((uintptr_t)ptr < (uintptr_t)arena->memory_pool + HEADER_SIZE ||
        (uintptr_t)ptr >= (uintptr_t)arena->memory_pool + global_config.pool_size) {
        abort();
    }


    uint32_t order = block->order;

    /* Large blocks bypass cache and go straight to global pool */
    if (order > SMALL_ORDER_MAX) {
        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

        block = coalesce_up_and_update(arena, block, &order);
        arena_insert(arena, order, block);
        pthread_mutex_unlock(&arena->lock);
        return;
    }


    thread_cache_data_t *data = get_thread_cache();
    if (data && __builtin_expect(atomic_load(&trim_requested) != data->last_trim_request, 0)) {
        flush_my_cache(data);
        data->last_trim_request = atomic_load(&trim_requested);
    }

    /* Remote free queue push if we are on foreign thread cache */
    if (data && block->arena != atomic_load(&data->arena)) {
        pthread_mutex_lock(&block->arena->remote_lock);
        block->next = block->arena->remote_free_queue.head;
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
        block->arena->remote_free_queue.head = block;
        pthread_mutex_unlock(&block->arena->remote_lock);
        return;
    }

    /* Thread cache fast path */
    if (!data) {
        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

        block = coalesce_up_and_update(arena, block, &order);
        arena_insert(arena, order, block);
        pthread_mutex_unlock(&arena->lock);
        return;
    }



    // GCC warns about out-of-bounds access for `data->count[order]`.
    // We already check `if (order > SMALL_ORDER_MAX)` and early-return above.
    // So order is strictly <= SMALL_ORDER_MAX.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

    uint32_t limit = get_cache_limit(order);
    if (data->count[order] < limit) {
        MBD_FIRE_EVENT(MBD_EVENT_FREE, ptr, 1ULL << block->order);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);

        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        atomic_fetch_add(&block->arena->cached_bytes, 1ULL << order);
        atomic_fetch_add(&block->arena->cache_pressure, 1ULL << order);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        return;
    }

    /* Bulk flush with grouped lock acquisition to prevent thrashing */
    atomic_fetch_add_explicit(&data->bulk_flushes, 1, memory_order_relaxed);
    MBD_FIRE_EVENT(MBD_EVENT_FLUSH, data, limit / 2);
    mbd_arena_t *locked_arena = NULL;
    int flush_count = limit / 2;
    if (atomic_load(&data->arena->cache_pressure) > global_config.pool_size / 4) { flush_count = (limit * 3) / 4; }
    while (flush_count-- > 0 && data->cache[order]) {
        block_header_t *to_global = data->cache[order];
        data->cache[order] = to_global->next;
        data->count[order]--;


        mbd_arena_t *block_arena = to_global->arena;

        if (locked_arena != block_arena) {
            if (locked_arena) {
                pthread_mutex_unlock(&locked_arena->lock);
            }
            locked_arena = block_arena;
            pthread_mutex_lock(&locked_arena->lock);
            drain_remote_queue(locked_arena);
        }

        atomic_store_explicit(&to_global->magic, encode_magic(to_global, MAGIC_FREE), memory_order_release);

        uint32_t original_order = to_global->order;
        uint32_t o = to_global->order;
        to_global = coalesce_up_and_update(locked_arena, to_global, &o);
        arena_insert(locked_arena, o, to_global);
        atomic_fetch_sub(&locked_arena->cached_bytes, 1ULL << original_order);
        atomic_fetch_sub(&locked_arena->cache_pressure, 1ULL << original_order);
    }
    
    if (locked_arena) {
        pthread_mutex_unlock(&locked_arena->lock);
    }

    /* Add the target block to the now-emptied cache */
    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);
    block->next = data->cache[order];
    data->cache[order] = block;
    data->count[order]++;
    atomic_fetch_add(&block->arena->cached_bytes, 1ULL << order);
    atomic_fetch_add(&block->arena->cache_pressure, 1ULL << order);
}

/**
 * @brief Reallocates a memory block to a new size.
 *        - If ptr is NULL -> behaves like mbd_alloc()
 *        - If new_size is 0 -> frees the block and returns NULL
 *        - If the new size fits inside the existing block, returns the
 *          same pointer (no copy, no lock).
 *        - Otherwise allocates a fresh block, copies data, and frees the old one.
 *
 * @param ptr      Old pointer (may be NULL).
 * @param new_size New requested payload size in bytes.
 * @return void* New pointer, or NULL on failure / zero-size.
 */
void *mbd_realloc(void *ptr, size_t new_size) {
    if (!ptr) return mbd_alloc(new_size);
    if (new_size == 0) { mbd_free(ptr); return NULL; }

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (load_magic(block) == encode_magic(block, MAGIC_MEMALIGN)) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = raw_block->is_mmap ? (raw_block->mmap_size - HEADER_SIZE)
                                               : ((size_t)1 << raw_block->order) - HEADER_SIZE;
        size_t offset = (uint8_t*)ptr - (uint8_t*)raw;
        size_t actual_usable = old_usable - offset;
        if (new_size <= actual_usable) return ptr;

        void *new_ptr = mbd_alloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, actual_usable);
        mbd_free(ptr);
        return new_ptr;
    }

    if (block->is_mmap && load_magic(block) == encode_magic(block, MAGIC_MMAP)) {
        size_t old_usable = block->mmap_size - HEADER_SIZE;
        if (new_size <= old_usable) return ptr;

        void *new_ptr = mbd_alloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, old_usable);
        mbd_free(ptr);
        return new_ptr;
    }

    if (load_magic(block) != encode_magic(block, MAGIC_ALLOC)) abort();

    size_t old_usable = ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
    if (new_size <= old_usable) return ptr;

    /* Try in-place coalescing first */
    mbd_arena_t *arena = block->arena;
    pthread_mutex_lock(&arena->lock);
    drain_remote_queue(arena);

    while (block->order < MAX_ORDER) {
        size_t current_usable = ((size_t)1 << block->order) - HEADER_SIZE;
        if (new_size <= current_usable) break;

        block_header_t *buddy = get_buddy(arena, block, block->order);
        if (!buddy ||
            load_magic(buddy) != encode_magic(buddy, MAGIC_FREE) ||
            atomic_load_explicit(&buddy->order, memory_order_acquire) != block->order ||
            atomic_load_explicit(&buddy->arena, memory_order_acquire) != arena)
            break;

        if (buddy->prev == NULL && arena->free_lists[block->order] != buddy)
            break;

        /* Only coalesce if buddy is to our right (we are left buddy) */
        if ((uintptr_t)buddy < (uintptr_t)block) break;

        arena_remove(arena, buddy, block->order);
        atomic_fetch_add(&arena->coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, block->order);

        block->order += 1;
    }
    
    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
    pthread_mutex_unlock(&arena->lock);

    old_usable = ((size_t)1 << block->order) - HEADER_SIZE;
    if (new_size <= old_usable) return ptr;

    /* Fall back to allocate + copy */
    void *new_ptr = mbd_alloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_usable);
    mbd_free(ptr);
    return new_ptr;
}

/**
 * @brief Allocates memory for an array of nmemb elements of size bytes each
 *        and initializes all bytes to zero.
 *
 * @param nmemb Number of elements.
 * @param size  Size of each element.
 * @return void* Pointer to the allocated memory, or NULL on failure / zero-size.
 */
void *mbd_calloc(size_t nmemb, size_t size) {
    if (size != 0 && nmemb > SIZE_MAX / size) return NULL;
    size_t total = nmemb * size;
    if (total == 0) return mbd_alloc(1); // POSIX-consistent

    void *ptr = mbd_alloc(total);
    if (ptr) {
        block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);
        if (!block->is_mmap) {
            memset(ptr, 0, total);
        }
    }
    return ptr;
}

/**
 * @brief Allocates memory with a specific alignment.
 *
 * @param alignment The required alignment (must be a power of two).
 * @param size      The size of memory requested in bytes.
 * @return void* Pointer to the aligned payload, or NULL on failure.
 */
void *mbd_memalign(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= 32) return mbd_alloc(size); // Naturally 32-byte aligned

    /* Overallocate to ensure alignment and space for a fake header */
    if (SIZE_MAX - size < alignment + 2 * HEADER_SIZE) return NULL;
    size_t request = size + alignment + 2 * HEADER_SIZE;
    void *raw = mbd_alloc(request);
    if (!raw) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + 2 * HEADER_SIZE + alignment - 1) & ~(alignment - 1);

    /* Plant fake header pointing to the real allocated block for easy free-ing */
    block_header_t *fake = (block_header_t *)(aligned_addr - HEADER_SIZE);
    fake->magic = encode_magic(fake, MAGIC_MEMALIGN);
    fake->prev = (block_header_t *)raw;

    return (void *)aligned_addr;
}

/**
 * @brief Checks if a given pointer falls within the memory range of any active arena.
 *
 * @param ptr The pointer to verify.
 * @return int 1 if the pointer is within an arena's memory pool, 0 otherwise.
 */
static int is_pointer_in_any_arena(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    for (int a = 0; a < arena_count; a++) {
        if (arenas[a].memory_pool && addr >= (uintptr_t)arenas[a].memory_pool && addr < (uintptr_t)arenas[a].memory_pool + global_config.pool_size) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Returns the number of bytes actually usable in an allocated block.
 *        (Useful for string buffers, growing vectors, etc.)
 *
 * @param ptr Allocated pointer (must be valid).
 * @return size_t Usable payload size (>= requested size).
 */
size_t mbd_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (block->is_mmap && load_magic(block) == encode_magic(block, MAGIC_MMAP)) {
        return block->mmap_size - HEADER_SIZE;
    }

    if (!is_pointer_in_any_arena(block)) return 0;

    if (load_magic(block) == encode_magic(block, MAGIC_MEMALIGN)) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = raw_block->is_mmap ? (raw_block->mmap_size - HEADER_SIZE) : ((size_t)1 << raw_block->order) - HEADER_SIZE;
        size_t offset = (const uint8_t*)ptr - (uint8_t*)raw;
        return old_usable > offset ? old_usable - offset : 0;
    }

    if (load_magic(block) != encode_magic(block, MAGIC_ALLOC)) return 0;
    return ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
}


/**
 * @brief Flushes all blocks from a thread's local cache back into the global arenas.
 *
 * This internal helper is called when a thread cache needs to be forcibly trimmed,
 * returning all cached blocks to their respective arenas and attempting to coalesce them.
 *
 * @param curr Pointer to the thread cache data to flush.
 */
static void flush_my_cache(thread_cache_data_t *curr) {
    mbd_arena_t *locked_arena = NULL;
    for (int o = MIN_ORDER; o <= SMALL_ORDER_MAX; o++) {
        while (curr->cache[o]) {
            block_header_t *block = curr->cache[o];
            curr->cache[o] = block->next;
            curr->count[o]--;

            mbd_arena_t *block_arena = block->arena;

            if (locked_arena != block_arena) {
                if (locked_arena) {
                    pthread_mutex_unlock(&locked_arena->lock);
                }
                locked_arena = block_arena;
                pthread_mutex_lock(&locked_arena->lock);
                drain_remote_queue(locked_arena);
            }

            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

            uint32_t original_order = block->order;
            uint32_t coalesced_order = original_order;
            block = coalesce_up_and_update(locked_arena, block, &coalesced_order);
            arena_insert(locked_arena, coalesced_order, block);

            atomic_fetch_sub(&locked_arena->cached_bytes, 1ULL << original_order);
            atomic_fetch_sub(&locked_arena->cache_pressure, 1ULL << original_order);
        }
    }
    if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
}

/**
 * @brief Forces a trim of all thread caches, returning memory to the global arena.
 * @note **Heavy Operation**: This triggers a cooperative trim where every thread will
 *       completely flush its local cache on its next allocation or free. This causes a
 *       100% cache miss rate immediately following the trim. Use only for low memory
 *       emergencies, not for periodic lightweight usage.
 * @warning **Not async-signal-safe**: Do NOT call from signal handlers. The trim flag
 *          is atomic, but the flush operation acquires mutexes and may deadlock if
 *          a signal interrupts a lock.
 */
void mbd_trim(void) {
    atomic_fetch_add(&trim_requested, 1);
}

/**
 * @brief Returns accurate diagnostics of mapped, allocated, and free bytes.
 *
 * @return mbd_stats_t Diagnostics information.
 */
mbd_stats_t mbd_get_stats(void) {
    pthread_once(&init_once, internal_init);
    mbd_stats_t s = {0};
    s.total_mapped_bytes = ((size_t)arena_count * global_config.pool_size) + atomic_load(&huge_mmap_tracked);
    
    if (arenas) {
        for (int a = 0; a < arena_count; a++) {
            pthread_mutex_lock(&arenas[a].lock);
            drain_remote_queue(&arenas[a]);
            for (int i = MIN_ORDER; i <= MAX_ORDER; i++) {
                for (block_header_t *b = arenas[a].free_lists[i]; b; b = b->next) {
                    s.total_free_bytes += (1ULL << i);
                }
            }
            s.total_cached_bytes += atomic_load(&arenas[a].cached_bytes);
            s.splits += atomic_load(&arenas[a].splits);
            s.coalesces += atomic_load(&arenas[a].coalesces);
            pthread_mutex_unlock(&arenas[a].lock);
        }
    }
    
    s.total_allocated_bytes = s.total_mapped_bytes - s.total_free_bytes - s.total_cached_bytes;

    pthread_mutex_lock(&cache_list_lock);
    for (thread_cache_data_t *curr = global_cache_list; curr; curr = curr->next) {
        s.cache_hits += atomic_load_explicit(&curr->cache_hits, memory_order_relaxed);
        s.cache_misses += atomic_load_explicit(&curr->cache_misses, memory_order_relaxed);
        s.bulk_flushes += atomic_load_explicit(&curr->bulk_flushes, memory_order_relaxed);
    }
    pthread_mutex_unlock(&cache_list_lock);
    return s;
}

/**
 * @brief Diagnostics Utility.
 * Prints the current state of the global free lists to stdout.
 * Note: Does not print blocks currently held in thread-local caches.
 */
void mbd_dump(void) {
    pthread_mutex_lock(&cache_list_lock);
    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_lock(&arenas[a].lock);
        printf("\n=== Arena %d Free Lists ===\n", a);
        for (int i = MIN_ORDER; i <= MAX_ORDER; i++) {
            int count = 0;
            for (block_header_t *b = arenas[a].free_lists[i]; b; b = b->next) count++;
            if (count) printf("Order %2d (%8zu B) : %3d free\n", i, (size_t)1<<i, count);
        }
        printf("==================================\n\n");
        pthread_mutex_unlock(&arenas[a].lock);
    }
    pthread_mutex_unlock(&cache_list_lock);
}

#endif /* MYBUDDY_IMPLEMENTATION */
#endif /* MYBUDDY_H */
