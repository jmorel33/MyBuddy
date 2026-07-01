/**
 * @file mybuddy_api.h
 * @brief MyBuddy public API — types, configuration, and function declarations.
 *
 * Do not include this file directly. Include mybuddy.h instead.
 *
 * @version 1.6.2
 * @author Jacques Morel
 */
#ifndef MYBUDDY_API_H
#define MYBUDDY_API_H

/* -- Version (canonical source: mybuddy_version.h — bump only there) -------- */
#include "mybuddy_version.h"

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
#define MBD_FLAG_ATOMIC_STATS      (1u << 1)  // 2. Update cached_bytes on every alloc/free
#define MBD_FLAG_BUDDY_LARGE       (1u << 2)  // 3. Use buddy system for blocks > global_config.small_order_max (OFF = direct mmap)
#define MBD_FLAG_REALLOC_LOCK      (1u << 3)  // 4. Take lock even on realloc shrinks (OFF = early return)
#define MBD_FLAG_CPU_LOCAL         (1u << 4)  // 5. Bind thread arenas based on CPU core/NUMA node
#define MBD_FLAG_DETERMINISTIC     (1u << 5)  // 6. Disable adaptive heuristics (e.g., pressure flushing)

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
// bit 3: reserved (was BLOCK_IS_SPLIT, removed in v1.4)
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
 * @brief Returns the MyBuddy version as a human-readable string.
 *
 * Format: "MAJOR.MINOR.PATCH[REVISION] (DESCRIPTION)"
 * Example: "1.6.2 (Dedicated version header, MbdGetVersionString() API, full 3-OS audit)"
 *
 * Do not parse this string programmatically — use the MYBUDDY_VERSION_*
 * macros from mybuddy_version.h for compile-time version checks instead.
 *
 * @return const char* Pointer to a static null-terminated version string.
 */
const char *MbdGetVersionString(void);

/**
 * @brief Explicitly initializes the allocator (Optional).
 *
 * The allocator is fully self-initializing; it will automatically set itself
 * up on the first call to mbd_alloc(). However, if you want to pre-warm the
 * memory pool and prevent initialization latency on the first allocation,
 * you can call this function during your application's startup phase.
 *
 * Pass NULL to use all defaults. This function is thread-safe and idempotent.
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
    MBD_EVENT_FLUSH,
    MBD_EVENT_PRESSURE_FLUSH,
    MBD_EVENT_WATERMARK_FLUSH,
    MBD_EVENT_MIGRATION
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
 *       mbd_trim()         — flushes caches, coalesces blocks
 *       mbd_release_to_os  — returns the resulting large free blocks to the OS
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

#endif /* MYBUDDY_API_H */
