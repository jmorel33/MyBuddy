/**
 * @file mybuddy.h
 * @brief High-Performance Thread-Caching Buddy Allocator
 *
 * @version 1.5.0-RC1
 * @date April 19, 2026
 * @author Jacques Morel
 *
 * @note v1.4.9 extends the thread cache to 1 MiB (order 20) and populates
 *       intermediate sizes during splits to eliminate cross-order cache thrashing.
 *       v1.4.8 fixes a severe performance bottleneck by removing the madvise
 *       cascade during coalescing. Adds mbd_release_to_os() for explicit memory release.
 *       v1.4.6 introduces granular thread cache limit configurations
 *       via `mbd_config_t` and disables `MBD_FLAG_HARDENED` by default
 *       for significant performance gains.
 *       v1.4.5 adds official support for GCC / MinGW-w64 on Windows.
 *       Same API, same safety, same performance characteristics.
 *       Uses winpthreads + native mmap emulation. No MSVC support yet.
 *       madvise hugepage hints and OS releases are no-ops on Windows via MinGW.
 *
 * @section overview Overview
 * MyBuddy (MBd) is a production-grade, highly concurrent memory allocator for high-performance C/C++ applications. It combines the anti-fragmentation
 * guarantees of a classic Buddy Allocator with the lock-free speed of per-thread caching.
 *
 * @section features Key Strengths
 * - **Crazy Fast**: Lock-free thread-local cache delivers allocations up to 1 MiB in just a few CPU cycles.
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
 * - **Tiny/Medium/Large objects** (strings, ECS entities, up to 1 MiB assets): Stay in the lock-free cache thanks to `global_config.small_order_max` (defaults to 20).
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
 *     // Lock-free fast path (falls under global_config.small_order_max)
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

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stddef.h> 
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Configuration Macros ---------------------------------------------------- */
/** @brief Configuration flags (can be combined with |) */
#define MBD_FLAG_HARDENED          (1u << 0)  // 1. Randomized magic + full safety checks
#define MBD_FLAG_ATOMIC_STATS      (1u << 1)  // 2. Update cached_bytes / cache_pressure on every alloc/free
#define MBD_FLAG_BUDDY_LARGE       (1u << 2)  // 3. Use buddy system for blocks > global_config.small_order_max (OFF = direct mmap)
#define MBD_FLAG_REALLOC_LOCK      (1u << 3)  // 4. Take lock even on realloc shrinks (OFF = early return)
#define MBD_FLAG_NUMA_AWARE        (1u << 4)  // 5. Bind thread arenas based on CPU core/NUMA node

#define MBD_MAX_POSSIBLE_ORDER 31

#ifndef MAX_ORDER
#define MAX_ORDER          27
#endif
#ifndef MIN_ORDER
#define MIN_ORDER          6              // 64 bytes minimum block size
#endif
#ifndef SMALL_ORDER_MAX
#define SMALL_ORDER_MAX    20             // Includes 1 MiB blocks (was 16)
#endif

#ifndef LARGE_CUTOFF_ORDER
#define LARGE_CUTOFF_ORDER 14             // 16 KiB threshold
#endif

#ifndef MBD_CACHE_PRESSURE_THRESHOLD
#define MBD_CACHE_PRESSURE_THRESHOLD (4ULL * 1024 * 1024)
#endif

#define BLOCK_USED           (1u << 0)
#define BLOCK_IS_MMAP        (1u << 1)
#define BLOCK_IN_FREE_LIST   (1u << 2)
#define BLOCK_IN_CACHE       (1u << 4)

typedef struct {
    uint32_t flags;
    int arena_count;
    size_t pool_size;
    
    /* Order Limits */
    uint32_t min_order;           // Default: 6 (64 bytes)
    uint32_t max_order;           // Default: 27 (128 MiB)
    uint32_t small_order_max;     // Default: 20 (1 MiB)
    uint32_t large_cutoff_order;  // Default: 14 (16 KiB)
    
    /* Cache Sizing & Thresholds */
    uint32_t cache_limits[32];    // Max order is 31, so 32 slots is safe
    uint32_t mmap_cache_slots;    // Default: 8
    uint32_t mmap_max_waste_ratio;// Default: 4
    size_t   cache_pressure_threshold; // Default: 4 MiB
    
    /* Advanced Heuristics */
    uint8_t  flush_high_watermark_pct;  // Default: 100 (Flush when 100% full)
    uint8_t  flush_low_watermark_pct;   // Default: 50  (Flush down to 50%)
    uint32_t refill_batch_size;         // Default: 0 (Unlimited/Fill to max)
    uint32_t max_remote_frees_per_lock; // Default: 0 (Unlimited)
    uint32_t migration_return_freq;     // Default: 64
    size_t   hugepage_threshold;        // Default: 2097152 (2 MiB)
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
 * @warning Permanently disables the allocator for the process.
 *          Subsequent calls to mbd_alloc() will safely return NULL.
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
 * @note In-place growth via coalescing may fail if the adjacent buddy block
 *       is currently held in a thread-local cache, resulting in a memcpy.
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
 * @brief Explicitly returns unused high-order memory to the operating system.
 *
 * Scans all arenas and calls madvise(MADV_DONTNEED) on the payload pages
 * of free blocks >= 2 MiB (order >= 21). This immediately reduces the
 * process's RSS but will cause page faults when that memory is next allocated.
 *
 * Unlike mbd_trim() (which flushes thread-local caches), this function
 * operates on the global free lists and acquires each arena lock briefly.
 *
 * @note Call this after mbd_trim() for maximum memory return:
 *       mbd_trim()        — flushes caches, coalesces blocks
 *       mbd_release_to_os — returns the resulting large free blocks to the OS
 *       madvise hugepage hints and OS releases are no-ops on Windows via MinGW.
 *
 * @warning Causes hard page faults on re-allocation of released pages.
 *          Only use in low-memory situations where RSS reduction matters
 *          more than allocation latency.
 */
void mbd_release_to_os(void);

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
// User must pass -lbcrypt manually during linking for MinGW
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#if defined(__MINGW32__) || defined(__MINGW64__)
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 1
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 2
#endif

static inline long mbd_sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (name == _SC_PAGESIZE) {
        return si.dwPageSize;
    }
    if (name == _SC_NPROCESSORS_ONLN) {
        return si.dwNumberOfProcessors;
    }
    return 0;
}
#define sysconf mbd_sysconf

#define mmap(a,b,c,d,e,f) VirtualAlloc(a, b, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
#define munmap(a,b) VirtualFree(a, 0, MEM_RELEASE)
#define MAP_FAILED NULL
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0
#define MAP_NORESERVE 0

#else
#include <unistd.h>
#include <sys/mman.h>
#endif
#include <errno.h>
#include <signal.h>
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#endif
#include <stdatomic.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>



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

/* ── Choose release strategy ──
 * MADV_FREE:  Lazy release — pages stay mapped and valid until the kernel
 *             needs them elsewhere. Re-allocation is nearly free if the
 *             OS hasn't reclaimed them. Ideal for allocators.
 * MADV_DONTNEED: Eager release — pages are immediately discarded. Causes
 *                hard page faults on re-access but guarantees instant RSS
 *                reduction. Use when you need to free physical RAM *now*.
 */
#ifndef MBD_MADV_RELEASE
#if defined(__linux__) && defined(MADV_FREE)
#define MBD_MADV_RELEASE  MADV_FREE
#elif defined(MADV_DONTNEED)
#define MBD_MADV_RELEASE  MADV_DONTNEED
#else
#define MBD_MADV_RELEASE  0
#endif
#endif

/* -- Internal types (not needed by callers) -------------------------------- */
//#define CACHELINE_ALIGN    __attribute__((aligned(64)))

struct mbd_arena;

typedef struct block_header {
    _Atomic(uint8_t) order;      // 1
    _Atomic(uint8_t) flags;      // 1
    uint8_t _pad[2];    // 2
    _Atomic(uint32_t) magic;     // 4 (always 32-bit to fit in 32-byte header on 64-bit systems)
    struct mbd_arena * arena; // 8 or 4
    struct block_header * next;  // 8 or 4
    union {
        struct block_header *prev;  // 8 or 4
        size_t mmap_size;           // Used when is_mmap == 1
    };
} block_header_t __attribute__((aligned(32)));

// Prevent accidental header bloat
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(block_header_t) == 32,
              "block_header_t size changed; verify padding/alignment");
#else
static_assert(sizeof(block_header_t) <= 32,
              "block_header_t size changed; verify padding/alignment");
#endif

typedef struct mbd_arena {
    pthread_mutex_t lock;
    block_header_t *free_lists[MBD_MAX_POSSIBLE_ORDER + 1];
    uint8_t *memory_pool;
    struct {
        _Atomic(block_header_t *) head;
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
    block_header_t *cache[MBD_MAX_POSSIBLE_ORDER + 1];
    uint32_t        count[MBD_MAX_POSSIBLE_ORDER + 1];
    _Atomic(mbd_arena_t *) arena;
    mbd_arena_t *native_arena;
    _Atomic(uint64_t) cache_hits;
    _Atomic(uint64_t) cache_misses;
    _Atomic(uint64_t) bulk_flushes;
    int             last_trim_request;
    size_t          total_cached;
    
    uint32_t        mmap_cache_count;
    struct thread_cache_data *next;
    
    /* NEW: Thread-local cache for large mmap blocks */
    block_header_t *mmap_cache[];
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
static __thread thread_cache_data_t *local_thread_cache = NULL;

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
static const uint32_t MAGIC_CACHED_MMAP = 0xF00DCAFE;

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
#if UINTPTR_MAX == UINT64_MAX
    uint32_t h = (uint32_t)(addr ^ (addr >> 32));
#else
    uint32_t h = (uint32_t)addr;
#endif
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
    struct timeval tv;
    gettimeofday(&tv, NULL);
    mbd_secret_key = (uint32_t)(uintptr_t)&mbd_secret_key ^ (tv.tv_sec ^ (tv.tv_usec << 10)) ^ 0x55AA55AA;
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
    atomic_fetch_or_explicit(&block->flags, BLOCK_IN_FREE_LIST, memory_order_relaxed);
    block->prev = NULL;
    block->next = arena->free_lists[order];
    if (arena->free_lists[order]) arena->free_lists[order]->prev = block;
    arena->free_lists[order] = block;
    block->arena = arena;
}

static void arena_remove(mbd_arena_t *arena, block_header_t *block, uint32_t order) {
    atomic_fetch_and_explicit(&block->flags, ~BLOCK_IN_FREE_LIST, memory_order_relaxed);
    if (block->prev) block->prev->next = block->next;
    else             arena->free_lists[order] = block->next;
    if (block->next) block->next->prev = block->prev;
    block->prev = block->next = NULL;
}

/* ================================================================== *
 *                       HELPER FUNCTIONS                             *
 * ================================================================== */
static inline uint32_t get_cache_limit(uint32_t order) {
    if (order > global_config.small_order_max) return 0;
    return global_config.cache_limits[order];
}

static inline uint32_t next_power_of_two_order(size_t req) {
    if (req <= (1ULL << global_config.min_order)) {
        return global_config.min_order;
    }
#if defined(__GNUC__) || defined(__clang__)
#if UINTPTR_MAX == UINT64_MAX
    return 64 - __builtin_clzll(req - 1);
#else
    return 32 - __builtin_clzl(req - 1);
#endif
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
    uint32_t orig_order = atomic_load_explicit(&block->order, memory_order_relaxed);
    while (atomic_load_explicit(&block->order, memory_order_relaxed) > target_order) {
        uint32_t new_order = atomic_load_explicit(&block->order, memory_order_relaxed) - 1;
        block_header_t *buddy = get_buddy(arena, block, new_order);
        atomic_store_explicit(&buddy->order, new_order, memory_order_relaxed);
        atomic_store_explicit(&buddy->flags, 0, memory_order_relaxed);
        atomic_store_explicit(&buddy->magic, encode_magic(buddy, MAGIC_FREE), memory_order_release);

        buddy->arena = arena;
        buddy->next = buddy->prev = NULL;
        arena_insert(arena, new_order, buddy);
        atomic_store_explicit(&block->order, new_order, memory_order_relaxed);
    }
    atomic_fetch_add(&arena->splits, orig_order - target_order);
    MBD_FIRE_EVENT(MBD_EVENT_SPLIT, block, orig_order - target_order);
    return block;
}

static block_header_t* coalesce_up_and_update(mbd_arena_t *arena, block_header_t *block, uint32_t *order_out) {
    uint32_t order = *order_out;
    while (order < global_config.max_order) {
        block_header_t *buddy = get_buddy(arena, block, order);
        if (!buddy) break;

        // Reads safe under arena lock
        if (load_magic(buddy) != encode_magic(buddy, MAGIC_FREE) ||
            atomic_load_explicit(&buddy->order, memory_order_relaxed) != order ||
            buddy->arena != arena)
            break;

        if (!(atomic_load_explicit(&buddy->flags, memory_order_relaxed) & BLOCK_IN_FREE_LIST))
            break;
            
        arena_remove(arena, buddy, order);
        atomic_fetch_add(&arena->coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, order);

        if ((uintptr_t)block > (uintptr_t)buddy) {
            block = buddy;
        }

        atomic_store_explicit(&block->order, order + 1, memory_order_relaxed);
        order++;
    }
    /* ── REMOVED: madvise(MADV_DONTNEED) cascade ──
     * Physical pages are no longer discarded during coalescing.
     * This keeps the buddy pool "warm" — re-allocation hits
     * already-backed pages instead of triggering hard faults.
     * Use mbd_release_to_os() to explicitly return memory.
     */
    *order_out = order;
    return block;
}
static inline void remote_push(mbd_arena_t *arena, block_header_t *block) {
    block->next = atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire);
    while (!atomic_compare_exchange_weak(&arena->remote_free_queue.head, &block->next, block)) {
        // Spin until pushed
    }
}

static void drain_remote_queue(mbd_arena_t *arena) {
    if (atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire) == NULL) return;

    block_header_t *head = atomic_exchange_explicit(&arena->remote_free_queue.head, (block_header_t*)NULL, memory_order_acquire);

    // Reverse the list
    block_header_t *prev = NULL;
    block_header_t *curr = head;
    while (curr) {
        block_header_t *next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }
    head = prev;

    uint32_t processed = 0;
    while (head) {
        if (global_config.max_remote_frees_per_lock > 0 && processed >= global_config.max_remote_frees_per_lock) {
            // Push remaining list back to the queue
            if (head) {
                // First, find the tail of the remaining list
                block_header_t *remainder_tail = head;
                while (remainder_tail->next) remainder_tail = remainder_tail->next;

                block_header_t *old_head = atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire);
                do {
                    remainder_tail->next = old_head;
                } while (!atomic_compare_exchange_weak_explicit(&arena->remote_free_queue.head, &old_head, head, memory_order_release, memory_order_relaxed));
            }
            break;
        }
        
        block_header_t *next = head->next;
        atomic_store_explicit(&head->magic, encode_magic(head, MAGIC_FREE), memory_order_release);

        uint32_t order = atomic_load_explicit(&head->order, memory_order_relaxed);
        head = coalesce_up_and_update(arena, head, &order);
        arena_insert(arena, order, head);
        head = next;
        processed++;
    }
}

/**
 * @brief Releases physical pages of high-order free blocks back to the OS.
 *
 * Scans each arena's free lists for blocks of order >= 21 (2 MiB) and
 * advises the kernel that the payload pages are no longer needed.
 * The header page is preserved so free-list traversal remains valid.
 *
 * MUST be called while holding the arena lock.
 *
 * @param arena The arena to scan.
 */
static void mbd_madvise_free_blocks(mbd_arena_t *arena) {
#if MBD_MADV_RELEASE != 0
    for (uint32_t order = 21; order <= global_config.max_order; order++) {
        for (block_header_t *block = arena->free_lists[order]; block; block = block->next) {
            uintptr_t block_addr = (uintptr_t)block;
            uintptr_t adv_start  = (block_addr + HEADER_SIZE + os_page_size - 1) & ~(os_page_size - 1);
            uintptr_t adv_end    = block_addr + (1ULL << order);
            if (adv_end > adv_start) {
                madvise((void *)adv_start, adv_end - adv_start, MBD_MADV_RELEASE);
            }
        }
    }
#else
    (void)arena;
#endif
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
    local_thread_cache = NULL;

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
    for (uint32_t o = global_config.min_order; o <= global_config.small_order_max; o++) {
        while (data->cache[o]) {
            block_header_t *block = data->cache[o];
            data->cache[o] = block->next;
            data->count[o]--;
            data->total_cached -= (1ULL << o);
            
            mbd_arena_t *block_arena = block->arena;


            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

            if (global_config.flags & MBD_FLAG_ATOMIC_STATS) {
                atomic_fetch_sub(&block_arena->cached_bytes, 1ULL << o);
                atomic_fetch_sub(&block_arena->cache_pressure, 1ULL << o);
            }

            if (atomic_load(&block_arena->active)) {
                remote_push(block_arena, block);
            }
        }
    }

    // Free any cached mmap blocks
    for (uint32_t i = 0; i < data->mmap_cache_count; i++) {
        block_header_t *mmap_block = data->mmap_cache[i];
        size_t mmap_size = mmap_block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(mmap_block, mmap_size);
    }
    
    /* Return the cache struct itself via the remote free queue */
    block_header_t *cache_block = (block_header_t *)((uint8_t*)data - HEADER_SIZE);
    atomic_store_explicit(&cache_block->flags, 0, memory_order_relaxed);
    atomic_store_explicit(&cache_block->magic, encode_magic(cache_block, MAGIC_FREE), memory_order_release);
    mbd_arena_t *cache_arena = cache_block->arena;

    if (atomic_load(&cache_arena->active)) {
        remote_push(cache_arena, cache_block);
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
    for (uint32_t i = 0; i <= global_config.small_order_max; i++) {
        if (global_config.cache_limits[i] != 0) {
            limits_uninitialized = 0;
            break;
        }
    }
    
    if (global_config.min_order == 0) global_config.min_order = MIN_ORDER;
    if (global_config.max_order == 0) global_config.max_order = MAX_ORDER;
    if (global_config.small_order_max == 0) global_config.small_order_max = SMALL_ORDER_MAX;
    if (global_config.large_cutoff_order == 0) global_config.large_cutoff_order = LARGE_CUTOFF_ORDER;
    if (global_config.mmap_cache_slots == 0) global_config.mmap_cache_slots = 8;
    if (global_config.flush_high_watermark_pct == 0) global_config.flush_high_watermark_pct = 100;
    if (global_config.flush_low_watermark_pct == 0) global_config.flush_low_watermark_pct = 50;
    if (global_config.migration_return_freq == 0) global_config.migration_return_freq = 64;
    if (global_config.hugepage_threshold == 0) global_config.hugepage_threshold = 2 * 1024 * 1024;

    if (global_config.mmap_max_waste_ratio == 0) {
        global_config.mmap_max_waste_ratio = 4; // Default to 4x waste tolerance
    }

    if (global_config.cache_pressure_threshold == 0) {
        global_config.cache_pressure_threshold = MBD_CACHE_PRESSURE_THRESHOLD;
    }

    // We assume limits are uninitialized only if all limits are 0.
    // If a user genuinely wants to disable the cache by setting everything to 0,
    // they can just set global_config.small_order_max to 0. Otherwise this defaults behavior.
    /* ── Default cache limit table (performance-oriented, user-overridable) ── */
    if (limits_uninitialized) {
        for (uint32_t order = global_config.min_order; order <= global_config.small_order_max; order++) {
            if (order <= 8)       global_config.cache_limits[order] = 512;  /* <= 256 B     */
            else if (order <= 10) global_config.cache_limits[order] = 256;  /* <= 1 KiB     */
            else if (order <= 12) global_config.cache_limits[order] = 128;  /* <= 4 KiB     */
            else if (order <= 14) global_config.cache_limits[order] = 64;   /* 8-16 KiB     */
            else if (order <= 16) global_config.cache_limits[order] = 32;   /* 32-64 KiB    */
            else if (order <= 18) global_config.cache_limits[order] = 16;   /* 128-256 KiB  */
            else if (order <= 19) global_config.cache_limits[order] = 8;    /* 512 KiB      */
            else                  global_config.cache_limits[order] = 4;    /* 1 MiB        */
        }
    }
    if (!(global_config.flags & MBD_FLAG_BUDDY_LARGE)) {
        // Cap the effective cache limits to the mmap cutoff to avoid confusion
        for (uint32_t order = global_config.large_cutoff_order + 1; order <= global_config.small_order_max; order++) {
            global_config.cache_limits[order] = 0;
        }
    }
    if (global_config.pool_size == 0) {
        global_config.pool_size = (1ULL << 27);
    } else {
        if (global_config.pool_size > (1ULL << global_config.max_order)) {
            global_config.pool_size = (1ULL << global_config.max_order);
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
        atomic_init(&arenas[a].remote_free_queue.head, NULL);
        atomic_init(&arenas[a].cached_bytes, 0);
        atomic_init(&arenas[a].cache_pressure, 0);
        atomic_init(&arenas[a].splits, 0);
        atomic_init(&arenas[a].coalesces, 0);
        atomic_init(&arenas[a].active, 1);
        for (uint32_t i = 0; i <= global_config.max_order; i++) arenas[a].free_lists[i] = NULL;
        
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
        if (max_order > global_config.max_order) max_order = global_config.max_order;

        block_header_t *initial = (block_header_t *)arenas[a].memory_pool;
        atomic_store_explicit(&initial->order, max_order, memory_order_relaxed);
        atomic_store_explicit(&initial->flags, BLOCK_IN_FREE_LIST, memory_order_relaxed);
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

__attribute__((noinline, cold))
static thread_cache_data_t *get_thread_cache_slow(void) {
    if (atomic_load_explicit(&fully_destroyed, memory_order_relaxed)) {
        return NULL;
    }
    pthread_once(&init_once, internal_init);

    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (!data) {
        uint32_t t_id = atomic_fetch_add(&thread_counter, 1);

        uint32_t arena_idx;
        if (global_config.flags & MBD_FLAG_NUMA_AWARE) {
            #if defined(__linux__)
                int cpu = sched_getcpu();
                arena_idx = (cpu >= 0) ? (uint32_t)(cpu % arena_count) : (t_id % arena_count);
            #elif defined(_WIN32)
                uint32_t cpu = GetCurrentProcessorNumber();
                arena_idx = cpu % arena_count;
            #else
                arena_idx = t_id % arena_count;
            #endif
        } else {
            arena_idx = t_id % arena_count;
        }
        mbd_arena_t *arena = &arenas[arena_idx];

        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);

        uint32_t needed = (uint32_t)sizeof(thread_cache_data_t) + (global_config.mmap_cache_slots * sizeof(block_header_t*)) + HEADER_SIZE;
        uint32_t order  = next_power_of_two_order(needed);
        uint32_t cur = order;

        while (cur <= global_config.max_order && !arena->free_lists[cur]) cur++;
        
        block_header_t *block = NULL;
        mbd_arena_t *target_arena = arena;

        if (cur <= global_config.max_order) {
            block = arena->free_lists[cur];
            arena_remove(arena, block, cur);
            block = split_block_down(arena, block, order);
            
            atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            block->arena = arena;

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
                while (cur <= global_config.max_order && !other_arena->free_lists[cur]) cur++;
                
                if (cur <= global_config.max_order) {
                    block = other_arena->free_lists[cur];
                    arena_remove(other_arena, block, cur);
                    block = split_block_down(other_arena, block, order);
                    
                    atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
                    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
                    block->arena = other_arena;

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

            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

            uint32_t o = atomic_load_explicit(&block->order, memory_order_relaxed);
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
    local_thread_cache = data;
    return data;
}

static inline thread_cache_data_t *get_thread_cache_fast(void) {
    thread_cache_data_t *tc = local_thread_cache;
    if (__builtin_expect(tc != NULL, 1)) return tc;
    return get_thread_cache_slow();
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
    if (order > global_config.small_order_max || order < global_config.min_order) return;

    uint32_t limit = get_cache_limit(order);
    uint32_t refilled = 0;
    while (data->count[order] < limit) {
        if (global_config.refill_batch_size > 0 && refilled >= global_config.refill_batch_size) {
            break; // Yield the lock, let other threads work
        }

        block_header_t *block = locked_arena->free_lists[order];
        if (!block) {
            uint32_t cur = order + 1;
            while (cur <= global_config.max_order && !locked_arena->free_lists[cur]) cur++;
            if (cur > global_config.max_order) break;

            block = locked_arena->free_lists[cur];
            arena_remove(locked_arena, block, cur);

            /* Custom split that populates the thread cache for intermediate sizes */
            uint32_t split_count = 0;
            while (atomic_load_explicit(&block->order, memory_order_relaxed) > order) {
                uint32_t new_order = atomic_load_explicit(&block->order, memory_order_relaxed) - 1;
                block_header_t *buddy = get_buddy(locked_arena, block, new_order);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
                atomic_store_explicit(&buddy->order, new_order, memory_order_relaxed);
                atomic_store_explicit(&buddy->flags, 0, memory_order_relaxed);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
                buddy->arena = locked_arena;
                buddy->next = buddy->prev = NULL;

                /* If the buddy is cacheable, put it directly in the thread cache! */
                if (new_order <= global_config.small_order_max && data->count[new_order] < get_cache_limit(new_order)) {
                    atomic_store_explicit(&buddy->flags, BLOCK_IN_CACHE, memory_order_relaxed);
                    atomic_store_explicit(&buddy->magic, encode_magic(buddy, MAGIC_CACHED), memory_order_release);
                    buddy->next = data->cache[new_order];
                    data->cache[new_order] = buddy;
                    data->count[new_order]++;
                    data->total_cached += (1ULL << new_order);
                    if (global_config.flags & MBD_FLAG_ATOMIC_STATS) {
                        atomic_fetch_add(&locked_arena->cached_bytes, 1ULL << new_order);
                        atomic_fetch_add(&locked_arena->cache_pressure, 1ULL << new_order);
                    }
                } else {
                    atomic_store_explicit(&buddy->magic, encode_magic(buddy, MAGIC_FREE), memory_order_release);
                    arena_insert(locked_arena, new_order, buddy);
                }
                atomic_store_explicit(&block->order, new_order, memory_order_relaxed);
                split_count++;
            }

            /* Update split stats */
            if (split_count > 0) {
                atomic_fetch_add(&locked_arena->splits, split_count);
                MBD_FIRE_EVENT(MBD_EVENT_SPLIT, block, split_count);
            }
        } else {
            arena_remove(locked_arena, block, order);
        }

        atomic_store_explicit(&block->flags, BLOCK_IN_CACHE, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);
        block->arena = locked_arena;
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        data->total_cached += (1ULL << order);
        if (global_config.flags & MBD_FLAG_ATOMIC_STATS) {
            atomic_fetch_add(&locked_arena->cached_bytes, 1ULL << order);
        }
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
    pthread_mutex_lock(&init_mutex);
    if (config && !atomic_load(&config_set)) {
        pending_config = *config;
        atomic_store(&config_set, 1);
    }
    pthread_once(&init_once, internal_init);
    pthread_mutex_unlock(&init_mutex);
}

/**
 * @brief Destroys the allocator and unmaps all arenas.
 * Strictly for unit-testing and clean process teardown.
 * @warning Permanently disables the allocator for the process.
 *          Subsequent calls to mbd_alloc() will safely return NULL.
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

    uint32_t order = next_power_of_two_order(needed);

    thread_cache_data_t *data = local_thread_cache;

    /* HOT PATH -- Zero branches, zero atomic loads (besides the block flags) */
    if (__builtin_expect(data && order <= global_config.small_order_max && data->cache[order], 1)) {
        if (__builtin_expect(atomic_load(&trim_requested) != data->last_trim_request, 0)) {
            // Fall through to slow path which handles the trim
        } else {
            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            data->total_cached -= (1ULL << order);

            atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
            if (global_config.flags & MBD_FLAG_HARDENED) {
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            }
            return (void *)((uint8_t*)block + HEADER_SIZE);
        }
    }

    /* SLOW PATH STARTS HERE */
    if (needed > global_config.pool_size ||
        (!(global_config.flags & MBD_FLAG_BUDDY_LARGE) && order > global_config.large_cutoff_order)) {
        
        // Re-read data if necessary, or just use what we have (slow path)
        data = pthread_getspecific(thread_cache_key);
        
        // Check thread-local mmap cache first to avoid syscalls!
        if (data) {
            int best_idx = -1;
            size_t best_size = SIZE_MAX;
            for (uint32_t i = 0; i < data->mmap_cache_count; i++) {
                size_t bsize = data->mmap_cache[i]->mmap_size;
                if (bsize >= needed && bsize <= needed * global_config.mmap_max_waste_ratio && bsize < best_size) {
                    best_idx = (int)i;
                    best_size = bsize;
                    if (bsize == needed) break; // Exact match!
                }
            }

            if (best_idx >= 0) {
                block_header_t *block = data->mmap_cache[best_idx];
                data->mmap_cache[best_idx] = data->mmap_cache[--data->mmap_cache_count];

                atomic_store_explicit(&block->flags, BLOCK_IS_MMAP, memory_order_relaxed);
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);

                void *res = (void *)((uint8_t*)block + HEADER_SIZE);
                MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
                return res;
            }
        }
        
        block_header_t *block = (block_header_t *)mmap(NULL, needed, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            handle_oom();
            return NULL;
        }
#if defined(__linux__) || defined(__APPLE__) || (defined(__MINGW32__) || defined(__MINGW64__))
#if defined(MADV_HUGEPAGE)
        if (needed >= global_config.hugepage_threshold) {
            madvise(block, needed, MADV_HUGEPAGE);
        }
#endif
#endif
        atomic_store_explicit(&block->flags, BLOCK_IS_MMAP, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_MMAP), memory_order_release);
        block->arena = NULL;
        block->mmap_size = needed; // Store allocation size in unused prev pointer
        atomic_fetch_add(&huge_mmap_tracked, needed);
        void *res = (void *)((uint8_t*)block + HEADER_SIZE);
        MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
        return res;
    }

    data = get_thread_cache_fast();
    if (!data) { handle_oom(); return NULL; }
    if (__builtin_expect(atomic_load(&trim_requested) != data->last_trim_request, 0)) {
        flush_my_cache(data);
        data->last_trim_request = atomic_load(&trim_requested);
    }
    mbd_arena_t *arena = atomic_load_explicit(&data->arena, memory_order_relaxed);

    pthread_mutex_lock(&arena->lock);
    drain_remote_queue(arena);

    if (order <= global_config.small_order_max) {
        if (data && (global_config.flags & MBD_FLAG_ATOMIC_STATS)) {
            atomic_fetch_add_explicit(&data->cache_misses, 1, memory_order_relaxed);
        }
        refill_thread_cache(data, arena, order);
        if (data->cache[order]) {

            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            data->total_cached -= (1ULL << order);
            if (global_config.flags & MBD_FLAG_ATOMIC_STATS) {
                atomic_fetch_sub(&arena->cached_bytes, 1ULL << order);
                atomic_fetch_sub(&arena->cache_pressure, 1ULL << order);
            }
            atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            pthread_mutex_unlock(&arena->lock);
            void *res = (void *)((uint8_t*)block + HEADER_SIZE);
            MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
            return res;
        }
    }

    /* Global slow path */
    uint32_t cur = order;
    while (cur <= global_config.max_order && !arena->free_lists[cur]) cur++;
    
    if (cur > global_config.max_order) {
        pthread_mutex_unlock(&arena->lock);
        
        block_header_t *block = NULL;
        for (int i = 0; i < arena_count; i++) {
            mbd_arena_t *other_arena = &arenas[i];
            if (other_arena == arena) continue;
            
            pthread_mutex_lock(&other_arena->lock);
            drain_remote_queue(other_arena);
            cur = order;
            while (cur <= global_config.max_order && !other_arena->free_lists[cur]) cur++;
            
            if (cur <= global_config.max_order) {
                block = other_arena->free_lists[cur];
                arena_remove(other_arena, block, cur);
                block = split_block_down(other_arena, block, order);
                atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
                block->arena = other_arena;
                pthread_mutex_unlock(&other_arena->lock);

                /* NEW: Migrate thread to the arena that actually has memory */
                atomic_store(&data->arena, other_arena); 

                /* Probabilistic return to native arena to prevent long-term crowding */
                static __thread uint32_t migration_counter = 0;
                migration_counter++;
                if (global_config.migration_return_freq > 0 && 
                   (migration_counter % global_config.migration_return_freq) == 0) {
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

    atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
    block->arena = arena;

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
    if (__builtin_expect(!ptr, 0)) return;
    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    uint8_t raw_flags = atomic_load_explicit(&block->flags, memory_order_relaxed);

    /* FAST PATH: Standard block, Hardening OFF */
    if (__builtin_expect(raw_flags == BLOCK_USED && !(global_config.flags & MBD_FLAG_HARDENED), 1)) {
        uint32_t order = atomic_load_explicit(&block->order, memory_order_relaxed);
        thread_cache_data_t *data = local_thread_cache;

        if (__builtin_expect(data && order <= global_config.small_order_max, 1)) {
            uint32_t limit = get_cache_limit(order);
            if (__builtin_expect(data->count[order] < limit, 1)) {
                atomic_store_explicit(&block->flags, BLOCK_IN_CACHE, memory_order_relaxed);
                // No magic write needed if HARDENED is OFF
                block->next = data->cache[order];
                data->cache[order] = block;
                data->count[order]++;
                data->total_cached += (1ULL << order);
                return;
            }
        }
    }

    /* SLOW PATH / HARDENED PATH STARTS HERE */
    if (atomic_load_explicit(&fully_destroyed, memory_order_relaxed)) return;
    uint32_t raw_magic = load_magic(block);

    if (!(global_config.flags & MBD_FLAG_HARDENED)) {
        if (raw_magic == MAGIC_MEMALIGN) {
            void *raw = block->prev;
            atomic_store_explicit(&block->magic, MAGIC_FREE, memory_order_release);
            mbd_free(raw);
            return;
        }
    } else {
        if (raw_magic == encode_magic(block, MAGIC_MEMALIGN)) {
            void *raw = block->prev;
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
            mbd_free(raw);
            return;
        }
    }

    if (raw_flags & BLOCK_IS_MMAP) {
        if (!(global_config.flags & MBD_FLAG_HARDENED)) {
            if (raw_magic != MAGIC_ALLOC && raw_magic != MAGIC_MMAP && raw_magic != MAGIC_CACHED_MMAP) {
                fprintf(stderr, "mbd_free: DOUBLE-FREE or corruption of mmap block! ptr=%p\n", ptr);
                abort();
            }
        } else {
            if (raw_magic != encode_magic(block, MAGIC_ALLOC) && raw_magic != encode_magic(block, MAGIC_MMAP) && raw_magic != encode_magic(block, MAGIC_CACHED_MMAP)) {
                fprintf(stderr, "mbd_free: DOUBLE-FREE or corruption of mmap block! ptr=%p\n", ptr);
                abort();
            }
        }

        thread_cache_data_t *data = atomic_load_explicit(&fully_destroyed, memory_order_relaxed) ? NULL : pthread_getspecific(thread_cache_key);
        if (data && data->mmap_cache_count < global_config.mmap_cache_slots) {
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED_MMAP), memory_order_release);
            data->mmap_cache[data->mmap_cache_count++] = block;
            return;
        }
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
        size_t mmap_size = block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(block, mmap_size);
        return;
    }

    if (!(global_config.flags & MBD_FLAG_HARDENED)) {
        if (raw_flags & BLOCK_IN_CACHE) {
            fprintf(stderr, "mbd_free: DOUBLE-FREE! Block already in thread cache. ptr=%p\n", ptr);
            abort();
        }
        if (raw_magic != MAGIC_ALLOC && raw_magic != MAGIC_CACHED) abort();
    } else {
        if (raw_flags & BLOCK_IN_CACHE) {
            if (raw_magic != encode_magic(block, MAGIC_CACHED)) abort();
            fprintf(stderr, "mbd_free: DOUBLE-FREE! Block already in thread cache. ptr=%p\n", ptr);
            abort();
        }
        if (raw_magic != encode_magic(block, MAGIC_ALLOC) && raw_magic != encode_magic(block, MAGIC_CACHED)) abort();
    }

    mbd_arena_t *arena = block->arena;
    if (global_config.flags & MBD_FLAG_HARDENED) {
        if (!arena || arena < arenas || arena >= arenas + arena_count) abort();
        if ((uintptr_t)block < (uintptr_t)arena->memory_pool ||
            (uintptr_t)block >= (uintptr_t)arena->memory_pool + global_config.pool_size) {
            abort();
        }
        if ((uintptr_t)ptr < (uintptr_t)arena->memory_pool + HEADER_SIZE ||
            (uintptr_t)ptr >= (uintptr_t)arena->memory_pool + global_config.pool_size) {
            abort();
        }
    }

    uint32_t order = atomic_load_explicit(&block->order, memory_order_relaxed);

    /* Large blocks bypass cache and go straight to global pool */
    if (order > global_config.small_order_max) {
        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);
        atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

        block = coalesce_up_and_update(arena, block, &order);
        arena_insert(arena, order, block);
        pthread_mutex_unlock(&arena->lock);
        return;
    }

    thread_cache_data_t *data = get_thread_cache_fast();
    if (data && __builtin_expect(atomic_load(&trim_requested) != data->last_trim_request, 0)) {
        flush_my_cache(data);
        data->last_trim_request = atomic_load(&trim_requested);
    }

    /* Remote free queue push if we are on foreign thread cache */
    if (data && block->arena != atomic_load_explicit(&data->arena, memory_order_relaxed)) {
        mbd_arena_t *block_arena = block->arena;
        if (atomic_load(&block_arena->active)) {
            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
            remote_push(block_arena, block);
        }
        return;
    }

    /* Thread cache fast path */
    if (!data) {
        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);
        atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

        block = coalesce_up_and_update(arena, block, &order);
        arena_insert(arena, order, block);
        pthread_mutex_unlock(&arena->lock);
        return;
    }

    uint32_t limit = get_cache_limit(order);
    
    if (order <= global_config.small_order_max && data->count[order] < limit) {
        MBD_FIRE_EVENT(MBD_EVENT_FREE, ptr, 1ULL << atomic_load_explicit(&block->order, memory_order_relaxed));
        atomic_store_explicit(&block->flags, BLOCK_IN_CACHE, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        data->total_cached += (1ULL << order);
        if (global_config.flags & MBD_FLAG_ATOMIC_STATS) {
            atomic_fetch_add(&block->arena->cached_bytes, 1ULL << order);
            atomic_fetch_add(&block->arena->cache_pressure, 1ULL << order);
        }
        return;
    }

    /* Cache full: push to remote free queue (Lock-free!) */
    mbd_arena_t *block_arena = block->arena;
    if (atomic_load(&block_arena->active)) {
        atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
        remote_push(block_arena, block);
    }
}

/**
 * @brief Reallocates a memory block to a new size.
 *        - If ptr is NULL -> behaves like mbd_alloc()
 *        - If new_size is 0 -> frees the block and returns NULL
 *        - If the new size fits inside the existing block, returns the
 *          same pointer (no copy, no lock).
 *        - Otherwise allocates a fresh block, copies data, and frees the old one.
 *
 * @note In-place growth via coalescing may fail if the adjacent buddy block
 *       is currently held in a thread-local cache, resulting in a memcpy.
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
        size_t old_usable = (atomic_load_explicit(&raw_block->flags, memory_order_relaxed) & BLOCK_IS_MMAP) ? (raw_block->mmap_size - HEADER_SIZE)
                                               : ((size_t)1 << atomic_load_explicit(&raw_block->order, memory_order_relaxed)) - HEADER_SIZE;
        size_t offset = (uint8_t*)ptr - (uint8_t*)raw;
        size_t actual_usable = old_usable - offset;
        if (new_size <= actual_usable) return ptr;

        void *new_ptr = mbd_alloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, actual_usable);
        mbd_free(ptr);
        return new_ptr;
    }

    if (atomic_load_explicit(&block->flags, memory_order_relaxed) & BLOCK_IS_MMAP) {
        uint32_t m = load_magic(block);
        if (!(global_config.flags & MBD_FLAG_HARDENED)) {
            if (m != MAGIC_ALLOC && m != MAGIC_MMAP && m != MAGIC_CACHED_MMAP) abort();
        } else {
            if (m != encode_magic(block, MAGIC_ALLOC) && m != encode_magic(block, MAGIC_MMAP) && m != encode_magic(block, MAGIC_CACHED_MMAP)) abort();
        }

        size_t old_usable = block->mmap_size - HEADER_SIZE;
        if (new_size <= old_usable) return ptr;

        void *new_ptr = mbd_alloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, old_usable);
        mbd_free(ptr);
        return new_ptr;
    }

    if (!(global_config.flags & MBD_FLAG_HARDENED)) {
        if (load_magic(block) != MAGIC_ALLOC && load_magic(block) != MAGIC_CACHED) abort();
    } else {
        if (load_magic(block) != encode_magic(block, MAGIC_ALLOC) && load_magic(block) != encode_magic(block, MAGIC_CACHED)) abort();
    }

    size_t old_usable = ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
    if (new_size <= old_usable) {
        if (global_config.flags & MBD_FLAG_REALLOC_LOCK) {
            mbd_arena_t *arena = block->arena;
            pthread_mutex_lock(&arena->lock);
            pthread_mutex_unlock(&arena->lock);
        }
        return ptr;
    }

    /* Try in-place coalescing first */
    mbd_arena_t *arena = block->arena;
    pthread_mutex_lock(&arena->lock);
    drain_remote_queue(arena);

    while (atomic_load_explicit(&block->order, memory_order_relaxed) < global_config.max_order) {
        size_t current_usable = ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
        if (new_size <= current_usable) break;
        block_header_t *buddy = get_buddy(arena, block, atomic_load_explicit(&block->order, memory_order_relaxed));
        if (!buddy ||
            load_magic(buddy) != encode_magic(buddy, MAGIC_FREE) ||
            atomic_load_explicit(&buddy->order, memory_order_relaxed) != atomic_load_explicit(&block->order, memory_order_relaxed) ||
            buddy->arena != arena)
            break;

        if (!(atomic_load_explicit(&buddy->flags, memory_order_relaxed) & BLOCK_IN_FREE_LIST))
            break;

        /* Only coalesce if buddy is to our right (we are left buddy) */
        if ((uintptr_t)buddy < (uintptr_t)block) break;

        arena_remove(arena, buddy, atomic_load_explicit(&block->order, memory_order_relaxed));
        atomic_fetch_add(&arena->coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, atomic_load_explicit(&block->order, memory_order_relaxed));

        atomic_fetch_add_explicit(&block->order, 1, memory_order_relaxed);
    }
    
    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
    pthread_mutex_unlock(&arena->lock);

    old_usable = ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
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
    if (total == 0) {
        void *p = mbd_alloc(1);
        if (p) memset(p, 0, mbd_malloc_usable_size(p));
        return p;
    }

    void *ptr = mbd_alloc(total);
    if (ptr) {
        memset(ptr, 0, total);
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
    memset(fake, 0, sizeof(*fake));
    atomic_store_explicit(&fake->magic, encode_magic(fake, MAGIC_MEMALIGN), memory_order_release);
    fake->prev = (block_header_t *)raw;

    return (void *)aligned_addr;
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
    if (atomic_load_explicit(&block->flags, memory_order_relaxed) & BLOCK_IS_MMAP) {
        uint32_t m = load_magic(block);
        if (!(global_config.flags & MBD_FLAG_HARDENED)) {
            if (m == MAGIC_ALLOC || m == MAGIC_MMAP || m == MAGIC_CACHED_MMAP)
                return block->mmap_size - HEADER_SIZE;
        } else {
            if (m == encode_magic(block, MAGIC_ALLOC) || m == encode_magic(block, MAGIC_MMAP) || m == encode_magic(block, MAGIC_CACHED_MMAP))
                return block->mmap_size - HEADER_SIZE;
        }
    }

    mbd_arena_t *arena = block->arena;
    if (!arena || arena < arenas || arena >= arenas + arena_count) return 0;
    if ((uintptr_t)block < (uintptr_t)arena->memory_pool ||
        (uintptr_t)block >= (uintptr_t)arena->memory_pool + global_config.pool_size) {
        return 0;
    }

    if (load_magic(block) == encode_magic(block, MAGIC_MEMALIGN)) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = (atomic_load_explicit(&raw_block->flags, memory_order_relaxed) & BLOCK_IS_MMAP) ? (raw_block->mmap_size - HEADER_SIZE) : ((size_t)1 << atomic_load_explicit(&raw_block->order, memory_order_relaxed)) - HEADER_SIZE;
        size_t offset = (const uint8_t*)ptr - (uint8_t*)raw;
        return old_usable > offset ? old_usable - offset : 0;
    }

    if (!(global_config.flags & MBD_FLAG_HARDENED)) {
        if (load_magic(block) != MAGIC_ALLOC && load_magic(block) != MAGIC_CACHED) return 0;
    } else {
        if (load_magic(block) != encode_magic(block, MAGIC_ALLOC) && load_magic(block) != encode_magic(block, MAGIC_CACHED)) return 0;
    }
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
    for (uint32_t o = global_config.min_order; o <= global_config.small_order_max; o++) {
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

            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);

            uint32_t original_order = atomic_load_explicit(&block->order, memory_order_relaxed);
            uint32_t coalesced_order = original_order;
            block = coalesce_up_and_update(locked_arena, block, &coalesced_order);
            arena_insert(locked_arena, coalesced_order, block);

            if (global_config.flags & MBD_FLAG_ATOMIC_STATS) {
                atomic_fetch_sub(&locked_arena->cached_bytes, 1ULL << original_order);
                atomic_fetch_sub(&locked_arena->cache_pressure, 1ULL << original_order);
            }
            curr->total_cached -= (1ULL << original_order);
        }
    }
    if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);

    // Flush mmap cache back to OS on trim
    for (uint32_t i = 0; i < curr->mmap_cache_count; i++) {
        block_header_t *mmap_block = curr->mmap_cache[i];
        size_t mmap_size = mmap_block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(mmap_block, mmap_size);
    }
    curr->mmap_cache_count = 0;
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
void mbd_release_to_os(void) {
    pthread_once(&init_once, internal_init);
    if (!arenas) return;

    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_lock(&arenas[a].lock);
        drain_remote_queue(&arenas[a]);
        mbd_madvise_free_blocks(&arenas[a]);
        pthread_mutex_unlock(&arenas[a].lock);
    }
}

void mbd_trim(void) {
    atomic_fetch_add(&trim_requested, 1);
    /* Also release any already-coalesced high-order blocks to the OS.
     * Note: thread caches are flushed lazily, so newly coalesced blocks
     * from the flush won't be released until the next mbd_release_to_os()
     * or mbd_trim() call. Call mbd_release_to_os() again after a brief
     * delay if maximum RSS reduction is needed. */
    mbd_release_to_os();
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
            for (uint32_t i = global_config.min_order; i <= global_config.max_order; i++) {
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
static void mybuddy_itoa(size_t val, char *buf, int *len) {
    char temp[32];
    int i = 0;
    if (val == 0) {
        temp[i++] = '0';
    } else {
        while (val > 0) {
            temp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }
    for (int j = 0; j < i; j++) {
        buf[*len + j] = temp[i - 1 - j];
    }
    *len += i;
}

static void mybuddy_puts(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    if (len > 0) {
        ssize_t ret = write(STDERR_FILENO, str, len);
        (void)ret;
    }
}

void mbd_dump(void) {
    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_lock(&arenas[a].lock);
        drain_remote_queue(&arenas[a]);

        char buf[128];
        int len = 0;
        mybuddy_puts("\n=== Arena ");
        mybuddy_itoa(a, buf, &len);
        buf[len] = '\0';
        mybuddy_puts(buf);
        mybuddy_puts(" Free Lists ===\n");

        for (uint32_t i = global_config.min_order; i <= global_config.max_order; i++) {
            int count = 0;
            for (block_header_t *b = arenas[a].free_lists[i]; b; b = b->next) count++;
            if (count) {
                len = 0;
                mybuddy_puts("Order ");
                mybuddy_itoa(i, buf, &len);
                buf[len] = '\0';
                mybuddy_puts(buf);
                mybuddy_puts(" (");

                len = 0;
                mybuddy_itoa((size_t)1<<i, buf, &len);
                buf[len] = '\0';
                mybuddy_puts(buf);
                mybuddy_puts(" B) : ");

                len = 0;
                mybuddy_itoa(count, buf, &len);
                buf[len] = '\0';
                mybuddy_puts(buf);
                mybuddy_puts(" free\n");
            }
        }
        mybuddy_puts("==================================\n\n");
        pthread_mutex_unlock(&arenas[a].lock);
    }
}

#endif /* MYBUDDY_IMPLEMENTATION */
#endif /* MYBUDDY_H */
