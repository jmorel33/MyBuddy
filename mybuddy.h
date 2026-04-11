/**
 * @file mybuddy.h
 * @brief High-Performance Thread-Caching Buddy Allocator
 *
 * @version 1.3
 * @date April 11, 2026
 * @author Jacques Morel
 *
 * @section overview Overview
 * MyBuddy (MBd) is a production-grade, highly concurrent memory allocator for high-performance C/C++ applications. It combines the anti-fragmentation
 * guarantees of a classic Buddy Allocator with the lock-free speed of per-thread caching.
 *
 * @section features Key Strengths
 * - **Crazy Fast**: Lock-free thread-local cache delivers allocations up to 8 KiB in just a few CPU cycles.
 * - **Fully Thread-Safe**: True per-thread caching with global locks grouped and acquired only on cache misses or large blocks.
 * - **Hardened & Safe**: Double-free protection, underflow-protected bounds checking, magic-value validation, and defused memalign exploits.
 * - **Memory Efficient**: Uses `MAP_NORESERVE` so virtual memory is only backed by physical RAM when used. High-order blocks (>2 MiB) are safely returned to the OS via `madvise` to prevent memory hoarding.
 * - **Advanced Alignment**: Mathematically guaranteed 32-byte minimum alignment, plus `mbd_memalign()` for strict AVX-512 (64-byte+) requirements.
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
 * - **Large objects** (8 KiB – 128 MiB): Handled by the global buddy path (fast O(1) doubly-linked list traversal).
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

/* ── Configuration Macros ─────────────────────────────────────────── */
#define POOL_SIZE          (1ULL << 27)   // 128 MiB per arena
#define MAX_ORDER          27
#define MIN_ORDER          5              // 32 bytes minimum block size
#define SMALL_ORDER_MAX    13             // Includes 4 KiB pages
#define THREAD_CACHE_SIZE  64

/* ── Public API ───────────────────────────────────────────────────── */

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
void  mbd_init(void);

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
 *        - If ptr is NULL → behaves like mbd_alloc()
 *        - If new_size is 0 → frees the block and returns NULL
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
 * @return size_t Usable payload size (≥ requested size).
 */
size_t mbd_malloc_usable_size(const void *ptr);

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

/* ── String Helpers ───────────────────────────────────────────────── */
typedef struct {
    const char* data;   // pointer to character data (may not be null-terminated)
    size_t      len;    // exact length in bytes
} mbd_string_view_t;

/**
 * @brief Create a string view from a classic null-terminated C string.
 *        The view does **not** allocate or copy data.
 *
 * @param s A null-terminated C string.
 * @return mbd_string_view_t A string view struct pointing to the data.
 */
mbd_string_view_t mbd_string_view_from_cstr(const char *s);

/**
 * @brief Create a string view from raw data + exact length.
 *        ie. binary data, network buffers, or when you know the length.
 *        The view does **not** allocate or copy data.
 *
 * @param data Raw byte data.
 * @param len  Exact length in bytes.
 * @return mbd_string_view_t A string view struct pointing to the data.
 */
mbd_string_view_t mbd_string_view_from_data(const char *data, size_t len);

/**
 * @brief Allocate a null-terminated C string copy from a string view
 *        (uses mbd_alloc internally). Useful for classic C string.
 *
 * @param view The string view to duplicate.
 * @return char* A newly allocated, null-terminated C string, or NULL on failure.
 */
char *mbd_string_view_dup(mbd_string_view_t view);

#ifdef __cplusplus
}
#endif

/* ================================================================== *
 *  IMPLEMENTATION — define MYBUDDY_IMPLEMENTATION in exactly ONE .c  *
 * ================================================================== */
#ifdef MYBUDDY_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <assert.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif

#if defined(__linux__) || defined(__APPLE__)
#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
#endif
#endif

/* ── Internal types (not needed by callers) ───────────────────────── */
//#define CACHELINE_ALIGN    __attribute__((aligned(64)))

struct mbd_arena;

typedef struct block_header {
    uint8_t order;      // 1
    uint8_t used;       // 1
    uint8_t is_mmap;    // 1 (Direct mmap flag)
    uint8_t _padding;   // 1
    uint32_t magic;     // 4
    struct mbd_arena *arena; // 8 or 4
    struct block_header *next;  // 8 or 4
    union {
        struct block_header *prev;  // 8 or 4
        size_t mmap_size;           // Used when is_mmap == 1
    };
#if UINTPTR_MAX == 0xFFFFFFFFULL
    uint32_t _align_pad[3];     // +12 bytes on 32-bit → exactly 32 bytes
#endif
} block_header_t __attribute__((aligned(32)));

typedef struct mbd_arena {
    pthread_mutex_t lock;
    block_header_t *free_lists[MAX_ORDER + 1];
    uint8_t *memory_pool;
    struct {
        _Atomic(block_header_t*) head;
    } remote_free_queue;
} mbd_arena_t;

#define HEADER_SIZE        sizeof(block_header_t)

/**
 * @brief Thread Local Cache Data
 * Holds small-block free lists local to a specific thread to avoid
 * mutex contention on the global memory pool.
 */
typedef struct thread_cache_data {
    block_header_t *cache[SMALL_ORDER_MAX + 1];
    uint32_t        count[SMALL_ORDER_MAX + 1];
    mbd_arena_t    *arena;
    uint64_t        cache_hits;
    uint64_t        cache_misses;
    uint64_t        bulk_flushes;
    struct thread_cache_data *next;
} __attribute__((aligned(64))) thread_cache_data_t;

static mbd_arena_t *arenas = NULL;

static int arena_count = 1;
static _Atomic uint32_t thread_counter = 0;
static _Atomic uint32_t active_threads = 0;
static _Atomic size_t huge_mmap_tracked = 0;

static long os_page_size = 4096;
static _Atomic size_t global_cached_bytes = 0;
static pthread_mutex_t cache_list_lock = PTHREAD_MUTEX_INITIALIZER;
static thread_cache_data_t *global_cache_list = NULL;

static pthread_key_t thread_cache_key;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static void (*global_oom_handler)(void) = NULL;
static void (*global_profiler_hook)(mbd_event_type_t, void*, size_t) = NULL;

#ifdef MYBUDDY_ENABLE_PROFILING
    #define MBD_FIRE_EVENT(type, ptr, sz) \
        do { \
            if (__builtin_expect(global_profiler_hook != NULL, 0)) \
                global_profiler_hook(type, ptr, sz); \
        } while(0)
#else
    #define MBD_FIRE_EVENT(type, ptr, sz) do {} while(0)
#endif

static _Atomic uint64_t global_splits = 0;
static _Atomic uint64_t global_coalesces = 0;
static _Atomic size_t global_cache_pressure = 0;

static const uint32_t MAGIC_ALLOC    = 0xCAFEBABE;
static const uint32_t MAGIC_FREE     = 0xDEADBEEF;
static const uint32_t MAGIC_CACHED   = 0xBAADF00D;
static const uint32_t MAGIC_MEMALIGN = 0x00000A11;
static const uint32_t MAGIC_MMAP     = 0x8BADF00D;

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

static inline uint32_t next_power_of_two_order(size_t req) {
    if (req <= (1ULL << MIN_ORDER)) {
        return MIN_ORDER;
    }
    return 64 - __builtin_clzll(req - 1);
}

static inline block_header_t *get_buddy(mbd_arena_t *arena, block_header_t *block, uint32_t order) {
    uintptr_t offset = (uintptr_t)block - (uintptr_t)arena->memory_pool;
    if (offset & ((1ULL << order) - 1)) return NULL;
    uintptr_t buddy_offset = offset ^ (1ULL << order);
    if (buddy_offset >= POOL_SIZE) return NULL;
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
        buddy->magic = MAGIC_FREE;

        buddy->arena = arena;
        buddy->next = buddy->prev = NULL;
        arena_insert(arena, new_order, buddy);
        block->order = new_order;
    }
    atomic_fetch_add(&global_splits, orig_order - target_order);
    MBD_FIRE_EVENT(MBD_EVENT_SPLIT, block, orig_order - target_order);
    return block;
}

static block_header_t* coalesce_up_and_update(mbd_arena_t *arena, block_header_t *block, uint32_t *order_out) {
    uint32_t order = *order_out;
    while (order < MAX_ORDER) {
        block_header_t *buddy = get_buddy(arena, block, order);
        if (buddy)
        if (!buddy || buddy->magic != MAGIC_FREE || buddy->order != order || buddy->arena != arena) break;
        arena_remove(arena, buddy, order);
        atomic_fetch_add(&global_coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, order);
        if ((uintptr_t)block > (uintptr_t)buddy) block = buddy;
        block->order = order + 1;
        order++;
    }

    /* Safe madvise: Only advise the payload pages, preserving the header page */
    if (order >= 21) {
#if defined(__linux__) || defined(__APPLE__)
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
    block_header_t *head = atomic_exchange(&arena->remote_free_queue.head, NULL);
    while (head) {
        block_header_t *next = head->next;
        head->magic = MAGIC_FREE;

        // IMPORTANT: The original block pointer must be passed and updated if it coalesces!
        // But coalesce_up in original mybuddy.h returned uint32_t and did not update the caller's pointer!
        // We will fix coalesce_up to take a block_header_t**
        uint32_t order = head->order;
        head = coalesce_up_and_update(arena, head, &order);
        arena_insert(arena, order, head);
        head = next;
    }
}


static void handle_oom(void) {
    if (global_oom_handler) global_oom_handler();
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
    size_t bytes_flushed = 0;
    thread_cache_data_t *data = arg;
    if (!data) return;

    mbd_arena_t *locked_arena = NULL;


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

    /* Flush all cached blocks. Group locks to prevent thrashing. */
    for (int o = MIN_ORDER; o <= SMALL_ORDER_MAX; o++) {
        while (data->cache[o]) {
            block_header_t *block = data->cache[o];
            data->cache[o] = block->next;
            
            block->magic = MAGIC_FREE;

            mbd_arena_t *block_arena = block->arena;

            if (locked_arena != block_arena) {
                if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
                locked_arena = block_arena;
                pthread_mutex_lock(&locked_arena->lock);
            }

            uint32_t order = block->order;
            block = coalesce_up_and_update(locked_arena, block, &order);
            arena_insert(locked_arena, order, block);
            bytes_flushed += (1ULL << o);
        }
    }

    if (locked_arena) {
        pthread_mutex_unlock(&locked_arena->lock);
        locked_arena = NULL;
    }

    if (bytes_flushed > 0) atomic_fetch_sub(&global_cached_bytes, bytes_flushed);
        atomic_fetch_sub(&global_cache_pressure, bytes_flushed);

    /* Return the cache struct itself */
    block_header_t *cache_block = (block_header_t *)((uint8_t*)data - HEADER_SIZE);
    cache_block->used = 0;
    cache_block->magic = MAGIC_FREE;
    mbd_arena_t *cache_arena = cache_block->arena;

    pthread_mutex_lock(&cache_arena->lock);
    drain_remote_queue(cache_arena);
    uint32_t co = cache_block->order;
    cache_block = coalesce_up_and_update(cache_arena, cache_block, &co);
    arena_insert(cache_arena, co, cache_block);
    pthread_mutex_unlock(&cache_arena->lock);
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
    long sc_page = sysconf(_SC_PAGESIZE);
    if (sc_page > 0) os_page_size = sc_page;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    arena_count = (cores > 0) ? (2 * cores) : 2;
    if (arena_count > MBD_MAX_ARENAS) arena_count = MBD_MAX_ARENAS;

    arenas = static_arenas;

    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_init(&arenas[a].lock, NULL);
        for (int i = 0; i <= MAX_ORDER; i++) arenas[a].free_lists[i] = NULL;
        
        arenas[a].memory_pool = (uint8_t *)mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (arenas[a].memory_pool == MAP_FAILED) abort();

        block_header_t *initial = (block_header_t *)arenas[a].memory_pool;
        initial->order = MAX_ORDER;
        initial->used  = 0;
        initial->is_mmap = 0;
        initial->magic = MAGIC_FREE;
        initial->arena = &arenas[a];
        initial->next = initial->prev = NULL;
        
        arenas[a].free_lists[MAX_ORDER] = initial;
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
static thread_cache_data_t *get_thread_cache(void) {
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
        
        if (cur > MAX_ORDER) {
            pthread_mutex_unlock(&arena->lock);
            
            /* Fallback to other arenas */
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
                    block->magic = MAGIC_ALLOC;
                    block->arena = other_arena;
                    block->next = block->prev = NULL;
                    
                    data = (thread_cache_data_t *)((uint8_t*)block + HEADER_SIZE);
                    memset(data, 0, sizeof(thread_cache_data_t));
                    data->arena = arena;

        data->next = NULL;
        pthread_mutex_lock(&cache_list_lock);
        data->next = global_cache_list;
        global_cache_list = data;
        pthread_mutex_unlock(&cache_list_lock);
 // Affined cache requests to the thread's native arena.
                    
                    if (pthread_setspecific(thread_cache_key, data) != 0) {
                        block->used = 0;
                        block->magic = MAGIC_FREE;

                        uint32_t o = block->order;
                        block = coalesce_up_and_update(other_arena, block, &o);
                        arena_insert(other_arena, o, block);
                        pthread_mutex_unlock(&other_arena->lock);
                        return NULL;
                    }
                    atomic_fetch_add(&active_threads, 1);
                    pthread_mutex_unlock(&other_arena->lock);
                    return data;
                }
                pthread_mutex_unlock(&other_arena->lock);
            }
            return NULL;
        }

        block_header_t *block = arena->free_lists[cur];
        arena_remove(arena, block, cur);
        block = split_block_down(arena, block, order);

        block->used  = 1;
        block->is_mmap = 0;
        block->magic = MAGIC_ALLOC;
        block->arena = arena;
        block->next = block->prev = NULL;

        data = (thread_cache_data_t *)((uint8_t*)block + HEADER_SIZE);
        memset(data, 0, sizeof(thread_cache_data_t));
        data->arena = arena;

        data->next = NULL;
        pthread_mutex_lock(&cache_list_lock);
        data->next = global_cache_list;
        global_cache_list = data;
        pthread_mutex_unlock(&cache_list_lock);


        if (pthread_setspecific(thread_cache_key, data) != 0) {
            block->used = 0;
            block->magic = MAGIC_FREE;

            uint32_t o = block->order;
            block = coalesce_up_and_update(arena, block, &o);
            arena_insert(arena, o, block);
            pthread_mutex_unlock(&arena->lock);
            return NULL;
        }
        atomic_fetch_add(&active_threads, 1);
        pthread_mutex_unlock(&arena->lock);
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
    size_t bytes_fetched = 0;
    if (order > SMALL_ORDER_MAX || order < MIN_ORDER) return;

    mbd_arena_t *arena = data->arena;
    while (data->count[order] < THREAD_CACHE_SIZE) {

        block_header_t *block = arena->free_lists[order];
        if (!block) {
            uint32_t cur = order + 1;
            while (cur <= MAX_ORDER && !arena->free_lists[cur]) cur++;
            if (cur > MAX_ORDER) {

            break;
        }

            block = arena->free_lists[cur];
            arena_remove(arena, block, cur);
            block = split_block_down(arena, block, order);
        } else {
            arena_remove(arena, block, order);
        }

        block->magic = MAGIC_CACHED;
        block->arena = arena;
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        bytes_fetched += (1ULL << order);
    }
    if (bytes_fetched > 0) atomic_fetch_add(&global_cached_bytes, bytes_fetched);
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

void mbd_destroy(void) {
    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (data) {
        thread_cache_destructor(data); /* Flush our own cache back to arenas */
        pthread_setspecific(thread_cache_key, NULL);
    }

    assert(atomic_load(&active_threads) == 0 && "mbd_destroy called while other threads are active!");
    
    for (int a = 0; a < arena_count; a++) {
        if (arenas[a].memory_pool && arenas[a].memory_pool != MAP_FAILED) {
            munmap(arenas[a].memory_pool, POOL_SIZE);
            arenas[a].memory_pool = NULL;
        }
        pthread_mutex_destroy(&arenas[a].lock);
    }
    pthread_key_delete(thread_cache_key);
}

void mbd_set_oom_handler(void (*handler)(void)) {
    global_oom_handler = handler;
}

void mbd_set_profiler_hook(void (*hook)(mbd_event_type_t, void*, size_t)) {
    global_profiler_hook = hook;
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
    size_t needed = requested_size + HEADER_SIZE;

    /* Fallback to direct mmap for gigantic allocations (> 128 MB) */
    if (needed > POOL_SIZE) {
        block_header_t *block = (block_header_t *)mmap(NULL, needed, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            handle_oom();
            return NULL;
        }
        block->is_mmap = 1;
        block->magic = MAGIC_MMAP;
        block->arena = NULL;
        block->mmap_size = needed; // Store allocation size in unused prev pointer
        atomic_fetch_add(&huge_mmap_tracked, needed);
        void *res = (void *)((uint8_t*)block + HEADER_SIZE);
        MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
        return res;
    }

    thread_cache_data_t *data = get_thread_cache();
    if (!data) { handle_oom(); return NULL; }
    mbd_arena_t *arena = data->arena;

    uint32_t order = next_power_of_two_order(needed);

    /* HOT PATH — lock-free */

    /* Opportunistic drain to prevent remote queue hoarding */
    if (atomic_load(&arena->remote_free_queue.head) && pthread_mutex_trylock(&arena->lock) == 0) {
        drain_remote_queue(arena);
        pthread_mutex_unlock(&arena->lock);
    }

    if (order <= SMALL_ORDER_MAX && data->cache[order]) {
        block_header_t *block = data->cache[order];
        data->cache[order] = block->next;
        data->count[order]--;
        atomic_fetch_sub(&global_cached_bytes, 1ULL << order);
        atomic_fetch_sub(&global_cache_pressure, 1ULL << order);
        block->used  = 1;
        block->is_mmap = 0;
        block->magic = MAGIC_ALLOC;
        data->cache_hits++;
        // Notice: We intentionally do NOT overwrite block->arena.
        block->next = block->prev = NULL;
        void *res = (void *)((uint8_t*)block + HEADER_SIZE);
        MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
        return res;
    }

    pthread_mutex_lock(&arena->lock);

    if (order <= SMALL_ORDER_MAX) {
        if (data) data->cache_misses++;
        refill_thread_cache(data, order);
        if (data->cache[order]) {

            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            atomic_fetch_sub(&global_cached_bytes, 1ULL << order);
        atomic_fetch_sub(&global_cache_pressure, 1ULL << order);
            block->used  = 1;
            block->is_mmap = 0;
            block->magic = MAGIC_ALLOC;
            block->next = block->prev = NULL;
            pthread_mutex_unlock(&arena->lock);
            return (void *)((uint8_t*)block + HEADER_SIZE);
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
            cur = order;
            while (cur <= MAX_ORDER && !other_arena->free_lists[cur]) cur++;
            
            if (cur <= MAX_ORDER) {
                block = other_arena->free_lists[cur];
                arena_remove(other_arena, block, cur);
                block = split_block_down(other_arena, block, order);
                block->used  = 1;
                block->is_mmap = 0;
                block->magic = MAGIC_ALLOC;
                block->arena = other_arena;
                block->next = block->prev = NULL;
                pthread_mutex_unlock(&other_arena->lock);

                /* NEW: Migrate thread to the arena that actually has memory */
                data->arena = other_arena; 

                return (void *)((uint8_t*)block + HEADER_SIZE);
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
    block->magic = MAGIC_ALLOC;
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

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (block->magic == MAGIC_MEMALIGN) {
        void *raw = block->prev;
        block->magic = MAGIC_FREE; /* Defuse double-free vulnerability, but keep it MAGIC_FREE to fail consistently */
        mbd_free(raw);
        return;
    }

    if (block->is_mmap && block->magic == MAGIC_MMAP) {
        size_t mmap_size = block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(block, mmap_size);
        return;
    }

    if (block->magic != MAGIC_ALLOC) {
        fprintf(stderr, "mbd_free: DOUBLE-FREE or corruption!\n");
        abort();
    }

    mbd_arena_t *arena = block->arena;
    if (!arena || arena < arenas || arena >= arenas + arena_count) abort();
    if ((uintptr_t)ptr < (uintptr_t)arena->memory_pool + HEADER_SIZE ||
        (uintptr_t)ptr >= (uintptr_t)arena->memory_pool + POOL_SIZE) {
        abort();
    }

    block->used = 0;
    uint32_t order = block->order;

    /* Large blocks bypass cache and go straight to global pool */
    if (order > SMALL_ORDER_MAX) {
        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);
        block->magic = MAGIC_FREE;

        block = coalesce_up_and_update(arena, block, &order);
        arena_insert(arena, order, block);
        pthread_mutex_unlock(&arena->lock);
        return;
    }


    /* Remote free queue push if we are on foreign thread cache */
    thread_cache_data_t *data = get_thread_cache();
    if (data && block->arena != data->arena) {
        block_header_t *old_head;
        do {
            old_head = atomic_load(&block->arena->remote_free_queue.head);
            block->next = old_head;
        } while (!atomic_compare_exchange_weak(&block->arena->remote_free_queue.head, &old_head, block));
        return;
    }

    /* Thread cache fast path */
    if (!data) {
        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);
        block->magic = MAGIC_FREE;

        block = coalesce_up_and_update(arena, block, &order);
        arena_insert(arena, order, block);
        pthread_mutex_unlock(&arena->lock);
        return;
    }


    /* Opportunistic drain to prevent remote queue hoarding */
    if (atomic_load(&arena->remote_free_queue.head) && pthread_mutex_trylock(&arena->lock) == 0) {
        drain_remote_queue(arena);
        pthread_mutex_unlock(&arena->lock);
    }

    if (data->count[order] < THREAD_CACHE_SIZE) {
        MBD_FIRE_EVENT(MBD_EVENT_FREE, ptr, 1ULL << block->order);
        block->magic = MAGIC_CACHED;

        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        atomic_fetch_add(&global_cached_bytes, 1ULL << order);
        atomic_fetch_add(&global_cache_pressure, 1ULL << order);
        return;
    }

    /* Bulk flush with grouped lock acquisition to prevent thrashing */
    data->bulk_flushes++;
    MBD_FIRE_EVENT(MBD_EVENT_FLUSH, data, THREAD_CACHE_SIZE / 2);
    mbd_arena_t *locked_arena = NULL;
    int flush_count = THREAD_CACHE_SIZE / 2;
    if (atomic_load(&global_cache_pressure) > ((size_t)arena_count * POOL_SIZE) / 4) { flush_count = (THREAD_CACHE_SIZE * 3) / 4; }
    size_t bytes_flushed = 0;
    while (flush_count-- > 0 && data->cache[order]) {
        block_header_t *to_global = data->cache[order];
        data->cache[order] = to_global->next;
        data->count[order]--;

        to_global->magic = MAGIC_FREE;
        mbd_arena_t *block_arena = to_global->arena;

        if (locked_arena != block_arena) {
            if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
            locked_arena = block_arena;
            pthread_mutex_lock(&locked_arena->lock);
            drain_remote_queue(locked_arena);
        }

        uint32_t o = to_global->order;
        to_global = coalesce_up_and_update(locked_arena, to_global, &o);
        arena_insert(locked_arena, o, to_global);
        bytes_flushed += (1ULL << to_global->order);
    }
    
    if (locked_arena) {
        pthread_mutex_unlock(&locked_arena->lock);
    }

    if (bytes_flushed > 0) {
        atomic_fetch_sub(&global_cached_bytes, bytes_flushed);
        atomic_fetch_sub(&global_cache_pressure, bytes_flushed);
    }
    
    /* Add the target block to the now-emptied cache */
    block->magic = MAGIC_CACHED;
    block->next = data->cache[order];
    data->cache[order] = block;
    data->count[order]++;
    atomic_fetch_add(&global_cached_bytes, 1ULL << order);
        atomic_fetch_add(&global_cache_pressure, 1ULL << order);
}

/**
 * @brief Reallocates a memory block to a new size.
 *        - If ptr is NULL → behaves like mbd_alloc()
 *        - If new_size is 0 → frees the block and returns NULL
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
    
    if (block->magic == MAGIC_MEMALIGN) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = raw_block->is_mmap ? (raw_block->mmap_size - HEADER_SIZE) : ((size_t)1 << raw_block->order) - HEADER_SIZE;
        size_t offset = (uint8_t*)ptr - (uint8_t*)raw;
        size_t actual_usable = old_usable - offset;

        if (new_size <= actual_usable) return ptr;
        
        void *new_ptr = mbd_alloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, actual_usable);
        mbd_free(ptr);
        return new_ptr;
    }

    if (block->is_mmap && block->magic == MAGIC_MMAP) {
        size_t old_usable = (size_t)block->prev - HEADER_SIZE;
        if (new_size <= old_usable) return ptr;

        void *new_ptr = mbd_alloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, old_usable);
        mbd_free(ptr);
        return new_ptr;
    }

    if (block->magic != MAGIC_ALLOC) abort();

    size_t old_usable = ((size_t)1 << block->order) - HEADER_SIZE;
    if (new_size <= old_usable) return ptr;

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
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *mbd_memalign(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= 32) return mbd_alloc(size); // Naturally 32-byte aligned

    /* Overallocate to ensure alignment and space for a fake header */
    if (SIZE_MAX - size < alignment + HEADER_SIZE) return NULL;
    size_t request = size + alignment + HEADER_SIZE;
    void *raw = mbd_alloc(request);
    if (!raw) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + HEADER_SIZE + alignment - 1) & ~(alignment - 1);

    /* Plant fake header pointing to the real allocated block for easy free-ing */
    block_header_t *fake = (block_header_t *)(aligned_addr - HEADER_SIZE);
    fake->magic = MAGIC_MEMALIGN;
    fake->prev = (block_header_t *)raw; 

    return (void *)aligned_addr;
}

/**
 * @brief Returns the number of bytes actually usable in an allocated block.
 *        (Useful for string buffers, growing vectors, etc.)
 *
 * @param ptr Allocated pointer (must be valid).
 * @return size_t Usable payload size (≥ requested size).
 */
size_t mbd_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;

    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (block->magic == MAGIC_MEMALIGN) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = raw_block->is_mmap ? (raw_block->mmap_size - HEADER_SIZE) : ((size_t)1 << raw_block->order) - HEADER_SIZE;
        size_t offset = (const uint8_t*)ptr - (uint8_t*)raw;
        return old_usable > offset ? old_usable - offset : 0;
    }

    if (block->is_mmap && block->magic == MAGIC_MMAP) {
        return block->mmap_size - HEADER_SIZE;
    }

    if (block->magic != MAGIC_ALLOC) return 0;
    return ((size_t)1 << block->order) - HEADER_SIZE;
}


void mbd_trim(void) {
    pthread_mutex_lock(&cache_list_lock);
    thread_cache_data_t *curr = global_cache_list;
    while (curr) {
        mbd_arena_t *locked_arena = NULL;
        for (int o = MIN_ORDER; o <= SMALL_ORDER_MAX; o++) {
            block_header_t *p = curr->cache[o];
            while (p) {

                p = p->next;
            }
            while (curr->cache[o]) {
                block_header_t *block = curr->cache[o];
                curr->cache[o] = block->next;
                curr->count[o]--;

                block->magic = MAGIC_FREE;

                mbd_arena_t *block_arena = block->arena;

                if (locked_arena != block_arena) {
                    if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
                    locked_arena = block_arena;
                    pthread_mutex_lock(&locked_arena->lock);
                    drain_remote_queue(locked_arena);
                }


                uint32_t original_order = block->order;
                uint32_t coalesced_order = original_order;
                block = coalesce_up_and_update(locked_arena, block, &coalesced_order);
                arena_insert(locked_arena, coalesced_order, block);

                atomic_fetch_sub(&global_cached_bytes, 1ULL << original_order);
                atomic_fetch_sub(&global_cache_pressure, 1ULL << original_order);
            }
        }
        if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
        curr = curr->next;
    }
    pthread_mutex_unlock(&cache_list_lock);
    atomic_store(&global_cache_pressure, 0);
}

mbd_stats_t mbd_get_stats(void) {
    pthread_once(&init_once, internal_init);
    mbd_stats_t s = {0};
    s.total_mapped_bytes = ((size_t)arena_count * POOL_SIZE) + atomic_load(&huge_mmap_tracked);
    
    if (arenas) {
        for (int a = 0; a < arena_count; a++) {
            pthread_mutex_lock(&arenas[a].lock);
            drain_remote_queue(&arenas[a]);
            for (int i = MIN_ORDER; i <= MAX_ORDER; i++) {
                for (block_header_t *b = arenas[a].free_lists[i]; b; b = b->next) {
                    s.total_free_bytes += (1ULL << i);
                }
            }
            pthread_mutex_unlock(&arenas[a].lock);
        }
    }
    
    s.total_allocated_bytes = s.total_mapped_bytes - s.total_free_bytes - atomic_load(&global_cached_bytes);
    s.total_cached_bytes = atomic_load(&global_cached_bytes);
    s.splits = atomic_load(&global_splits);
    s.coalesces = atomic_load(&global_coalesces);

    pthread_mutex_lock(&cache_list_lock);
    for (thread_cache_data_t *curr = global_cache_list; curr; curr = curr->next) {
        s.cache_hits += curr->cache_hits;
        s.cache_misses += curr->cache_misses;
        s.bulk_flushes += curr->bulk_flushes;
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
}

/* ================================================================== *
 *                       STRING HELPERS                               *
 * ================================================================== */
 
/**
 * @brief Create a string view from a classic null-terminated C string.
 *        The view does **not** allocate or copy data.
 *
 * @param s A null-terminated C string.
 * @return mbd_string_view_t A string view struct pointing to the data.
 */
mbd_string_view_t mbd_string_view_from_cstr(const char *s) {
    if (!s) return (mbd_string_view_t){NULL, 0};
    return (mbd_string_view_t){s, strlen(s)};
}

/**
 * @brief Create a string view from raw data + exact length.
 *        ie. binary data, network buffers, or when you know the length.
 *        The view does **not** allocate or copy data.
 *
 * @param data Raw byte data.
 * @param len  Exact length in bytes.
 * @return mbd_string_view_t A string view struct pointing to the data.
 */
mbd_string_view_t mbd_string_view_from_data(const char *data, size_t len) {
    return (mbd_string_view_t){data, len};
}

/**
 * @brief Allocate a null-terminated C string copy from a string view
 *        (uses mbd_alloc internally). Useful for classic C string.
 *
 * @param view The string view to duplicate.
 * @return char* A newly allocated, null-terminated C string, or NULL on failure.
 */
char *mbd_string_view_dup(mbd_string_view_t view) {
    if (!view.data || view.len == 0) return NULL;

    char *copy = (char *)mbd_alloc(view.len + 1);
    if (copy) {
        memcpy(copy, view.data, view.len);
        copy[view.len] = '\0';
    }
    return copy;
}

#endif /* MYBUDDY_IMPLEMENTATION */
#endif /* MYBUDDY_H */
