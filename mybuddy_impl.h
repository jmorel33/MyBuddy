/**
 * @file mybuddy_impl.h
 * @brief MyBuddy implementation — internal types, globals, and all function bodies.
 *
 * Do not include this file directly. Include mybuddy.h instead, and define
 * MYBUDDY_IMPLEMENTATION in exactly one translation unit before that include.
 *
 * @version 1.6.2
 * @author Jacques Morel
 */
#ifndef MYBUDDY_IMPL_H
#define MYBUDDY_IMPL_H

/* ── Platform includes ───────────────────────────────────────────────────── */
#if defined(__MINGW32__) || defined(__MINGW64__)
#include <windows.h>
#include <ntstatus.h>  // STATUS_SUCCESS for BCryptGenRandom return check
#include <bcrypt.h>    // BCryptGenRandom (available in MinGW-w64)
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
#endif /* MINGW */

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

/* ── Portability fallbacks ───────────────────────────────────────────────── */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

// macOS uses MAP_ANON; Linux and MinGW use MAP_ANONYMOUS
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0
#endif
#endif

#ifndef MADV_DONTNEED
#ifdef MADV_FREE
#define MADV_DONTNEED MADV_FREE
#else
#define MADV_DONTNEED 0
#endif
#endif

/* ── Choose release strategy ──
 * MADV_FREE:     Lazy release — pages stay mapped and valid until the kernel
 *                needs them elsewhere. Re-allocation is nearly free if the OS
 *                hasn't reclaimed them. Ideal for allocators.
 * MADV_DONTNEED: Eager release — pages are immediately discarded. Causes hard
 *                page faults on re-access but guarantees instant RSS reduction.
 *                Use when you need to free physical RAM *now*.
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

/* ================================================================== *
 *  Internal types (not exposed to callers)                           *
 * ================================================================== */

struct mbd_arena;

typedef struct block_header {
    _Atomic(uint8_t) order;      // 1
    _Atomic(uint8_t) flags;      // 1
    uint8_t _pad[2];             // 2
    _Atomic(uint32_t) magic;     // 4 (always 32-bit to fit in 32-byte header on 64-bit systems)
    struct mbd_arena *arena;     // 8 or 4
    struct block_header *next;   // 8 or 4
    union {
        struct block_header *prev;  // 8 or 4
        size_t mmap_size;           // Used when BLOCK_IS_MMAP == 1
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
    _Atomic uint64_t splits;
    _Atomic uint64_t coalesces;
    _Atomic int active;
} __attribute__((aligned(64))) mbd_arena_t;

#define HEADER_SIZE  sizeof(block_header_t)

/**
 * @brief Thread-local cache data.
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
    /* Flexible array: thread-local cache for large mmap blocks */
    block_header_t *mmap_cache[];
} thread_cache_data_t;

/* ================================================================== *
 *  Globals                                                           *
 * ================================================================== */

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

static const uint32_t MAGIC_ALLOC       = 0xCAFEBABE;
static const uint32_t MAGIC_FREE        = 0xDEADBEEF;
static const uint32_t MAGIC_CACHED      = 0xBAADF00D;
static const uint32_t MAGIC_MEMALIGN    = 0x00000A11;
static const uint32_t MAGIC_MMAP        = 0x8BADF00D;
static const uint32_t MAGIC_CACHED_MMAP = 0xF00DCAFE;

static uint32_t mbd_secret_key = 0;

static inline int arena_index(const mbd_arena_t *a) {
    return (int)(a - arenas);
}

/* ================================================================== *
 *  Magic encoding / decoding                                         *
 * ================================================================== */

static inline uint32_t encode_magic(void *block, uint32_t magic) {
    if (!(global_config.flags & MBD_FLAG_HARDENED)) return magic;
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
#elif defined(__APPLE__)
    // getentropy(3) available since macOS 10.12 / iOS 10
    if (getentropy(&mbd_secret_key, sizeof(mbd_secret_key)) == 0)
        return;
#elif defined(__MINGW32__) || defined(__MINGW64__)
    if (BCryptGenRandom(NULL, (PUCHAR)&mbd_secret_key, sizeof(mbd_secret_key), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == STATUS_SUCCESS)
        return;
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    mbd_secret_key = (uint32_t)(uintptr_t)&mbd_secret_key ^ (tv.tv_sec ^ (tv.tv_usec << 10)) ^ 0x55AA55AA;
}

static inline uint32_t load_magic(const block_header_t *b) {
    return atomic_load_explicit(&b->magic, memory_order_acquire);
}

/* ================================================================== *
 *  Arena list operations                                             *
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
 *  Helper functions                                                  *
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
    while (req > 0) { req >>= 1; order++; }
    return order;
#endif
}

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
            atomic_load_explicit(&buddy->order, memory_order_acquire) != order ||
            buddy->arena != arena)
            break;
        if (!(atomic_load_explicit(&buddy->flags, memory_order_relaxed) & BLOCK_IN_FREE_LIST))
            break;
        arena_remove(arena, buddy, order);
        atomic_fetch_add(&arena->coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, order);
        if ((uintptr_t)block > (uintptr_t)buddy) block = buddy;
        atomic_store_explicit(&block->order, order + 1, memory_order_relaxed);
        order++;
    }
    /* Physical pages are no longer discarded during coalescing — keeps the pool warm.
     * Use mbd_release_to_os() to explicitly return memory. */
    *order_out = order;
    return block;
}

/* NOTE: Classic lock-free LIFO push — no ABA protection.
 * In theory a block can be pushed, popped, reused, and pushed again at the same
 * address causing CAS to succeed incorrectly. In practice this requires extremely
 * tight cross-thread timing that current workloads don't exhibit.
 * If corruption appears in stress tests, tagged pointers are the fix.
 * See: https://en.wikipedia.org/wiki/ABA_problem */
static inline void remote_push(mbd_arena_t *arena, block_header_t *block) {
    block->next = atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire);
    while (!atomic_compare_exchange_weak(&arena->remote_free_queue.head, &block->next, block)) {}
}

static void drain_remote_queue(mbd_arena_t *arena) {
    if (atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire) == NULL) return;
    block_header_t *head = atomic_exchange_explicit(&arena->remote_free_queue.head, (block_header_t*)NULL, memory_order_acquire);

    // Reverse the list for FIFO processing order
    block_header_t *prev = NULL, *curr = head;
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
            // Push remainder back to queue
            block_header_t *remainder_tail = head;
            while (remainder_tail->next) remainder_tail = remainder_tail->next;
            block_header_t *old_head = atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire);
            do {
                remainder_tail->next = old_head;
            } while (!atomic_compare_exchange_weak_explicit(&arena->remote_free_queue.head, &old_head, head, memory_order_release, memory_order_relaxed));
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

static void mbd_madvise_free_blocks(mbd_arena_t *arena) {
#if MBD_MADV_RELEASE != 0
    for (uint32_t order = 21; order <= global_config.max_order; order++) {
        for (block_header_t *block = arena->free_lists[order]; block; block = block->next) {
            uintptr_t block_addr = (uintptr_t)block;
            uintptr_t adv_start  = (block_addr + HEADER_SIZE + os_page_size - 1) & ~(os_page_size - 1);
            uintptr_t adv_end    = block_addr + (1ULL << order);
            if (adv_end > adv_start)
                madvise((void *)adv_start, adv_end - adv_start, MBD_MADV_RELEASE);
        }
    }
#else
    (void)arena;
#endif
}

static void handle_oom(void) {
    void (*hook)(void) = atomic_load(&global_oom_handler);
    if (hook) hook();
}

/* ================================================================== *
 *  Thread lifecycle                                                  *
 * ================================================================== */

static void thread_cache_destructor(void *arg) {
    thread_cache_data_t *data = arg;
    if (!data) return;
    local_thread_cache = NULL;

    pthread_mutex_lock(&cache_list_lock);
    thread_cache_data_t **curr = &global_cache_list;
    while (*curr) {
        if (*curr == data) { *curr = data->next; break; }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&cache_list_lock);

    // Flush cached buddy blocks via remote free queues (avoids taking arena lock)
    for (uint32_t o = global_config.min_order; o <= global_config.small_order_max; o++) {
        while (data->cache[o]) {
            block_header_t *block = data->cache[o];
            data->cache[o] = block->next;
            data->count[o]--;
            data->total_cached -= (1ULL << o);
            mbd_arena_t *block_arena = block->arena;
            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
            if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
                atomic_fetch_sub(&block_arena->cached_bytes, 1ULL << o);
            if (atomic_load(&block_arena->active))
                remote_push(block_arena, block);
        }
    }

    // Free cached mmap blocks
    for (uint32_t i = 0; i < data->mmap_cache_count; i++) {
        block_header_t *mmap_block = data->mmap_cache[i];
        size_t mmap_size = mmap_block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(mmap_block, mmap_size);
    }

    // Return the cache struct itself
    block_header_t *cache_block = (block_header_t *)((uint8_t*)data - HEADER_SIZE);
    atomic_store_explicit(&cache_block->flags, 0, memory_order_relaxed);
    atomic_store_explicit(&cache_block->magic, encode_magic(cache_block, MAGIC_FREE), memory_order_release);
    mbd_arena_t *cache_arena = cache_block->arena;
    if (atomic_load(&cache_arena->active))
        remote_push(cache_arena, cache_block);

    atomic_fetch_sub(&active_threads, 1);
}

/* ================================================================== *
 *  Initialization                                                    *
 * ================================================================== */

#define MBD_MAX_ARENAS 16
static mbd_arena_t static_arenas[MBD_MAX_ARENAS];

static void internal_init(void) {
    if (atomic_load(&config_set)) global_config = pending_config;

    int limits_uninitialized = 1;
    for (uint32_t i = 0; i <= global_config.small_order_max; i++) {
        if (global_config.cache_limits[i] != 0) { limits_uninitialized = 0; break; }
    }

    if (global_config.min_order == 0)          global_config.min_order          = MIN_ORDER;
    if (global_config.max_order == 0)          global_config.max_order          = MAX_ORDER;
    if (global_config.small_order_max == 0)    global_config.small_order_max    = SMALL_ORDER_MAX;
    if (global_config.large_cutoff_order == 0) global_config.large_cutoff_order = LARGE_CUTOFF_ORDER;
    if (global_config.mmap_cache_slots == 0)   global_config.mmap_cache_slots   = 8;
    if (global_config.flush_high_watermark_pct == 0) global_config.flush_high_watermark_pct = 100;
    if (global_config.flush_low_watermark_pct == 0)  global_config.flush_low_watermark_pct  = 50;
    if (global_config.migration_return_freq == 0)    global_config.migration_return_freq    = 64;
    if (global_config.hugepage_threshold == 0)       global_config.hugepage_threshold       = 2 * 1024 * 1024;
    if (global_config.mmap_max_waste_ratio == 0)     global_config.mmap_max_waste_ratio     = 4;
    if (global_config.cache_pressure_threshold == 0) global_config.cache_pressure_threshold = MBD_CACHE_PRESSURE_THRESHOLD;

    if (limits_uninitialized) {
        for (uint32_t order = global_config.min_order; order <= global_config.small_order_max; order++) {
            if      (order <= 8)  global_config.cache_limits[order] = 512;
            else if (order <= 10) global_config.cache_limits[order] = 256;
            else if (order <= 12) global_config.cache_limits[order] = 128;
            else if (order <= 14) global_config.cache_limits[order] = 64;
            else if (order <= 16) global_config.cache_limits[order] = 32;
            else if (order <= 18) global_config.cache_limits[order] = 16;
            else if (order <= 19) global_config.cache_limits[order] = 8;
            else                  global_config.cache_limits[order] = 4;
        }
    }
    if (!(global_config.flags & MBD_FLAG_BUDDY_LARGE)) {
        for (uint32_t order = global_config.large_cutoff_order + 1; order <= global_config.small_order_max; order++)
            global_config.cache_limits[order] = 0;
    }
    if (global_config.pool_size == 0) {
        global_config.pool_size = (1ULL << 27);
    } else {
        if (global_config.pool_size > (1ULL << global_config.max_order))
            global_config.pool_size = (1ULL << global_config.max_order);
        size_t p = 1;
        while (p < global_config.pool_size) p <<= 1;
        global_config.pool_size = p;
    }

    long sc_page = sysconf(_SC_PAGESIZE);
    if (sc_page > 0) os_page_size = sc_page;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    mbd_init_secret_key();

    if (global_config.arena_count > 0) arena_count = global_config.arena_count;
    else arena_count = (cores > 0) ? (2 * cores) : 2;
    if (arena_count > MBD_MAX_ARENAS) arena_count = MBD_MAX_ARENAS;

    arenas = static_arenas;

    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_init(&arenas[a].lock, NULL);
        atomic_init(&arenas[a].remote_free_queue.head, NULL);
        atomic_init(&arenas[a].cached_bytes, 0);
        atomic_init(&arenas[a].splits, 0);
        atomic_init(&arenas[a].coalesces, 0);
        atomic_init(&arenas[a].active, 1);
        for (uint32_t i = 0; i <= global_config.max_order; i++) arenas[a].free_lists[i] = NULL;

        arenas[a].memory_pool = (uint8_t *)mmap(NULL, global_config.pool_size,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (arenas[a].memory_pool == MAP_FAILED) abort();

#if defined(__linux__) || defined(__APPLE__) || defined(__MINGW32__) || defined(__MINGW64__)
#if defined(MADV_HUGEPAGE)
        madvise(arenas[a].memory_pool, global_config.pool_size, MADV_HUGEPAGE);
#endif
#if defined(MADV_DONTDUMP)
        madvise(arenas[a].memory_pool, global_config.pool_size, MADV_DONTDUMP);
#endif
#endif

        uint32_t max_order = 0;
        size_t temp_size = global_config.pool_size;
        while (temp_size > 1) { temp_size >>= 1; max_order++; }
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

/* ================================================================== *
 *  Thread cache management                                           *
 * ================================================================== */

static void flush_my_cache(thread_cache_data_t *curr);

__attribute__((noinline, cold))
static thread_cache_data_t *get_thread_cache_slow(void) {
    if (atomic_load_explicit(&fully_destroyed, memory_order_relaxed)) return NULL;
    pthread_once(&init_once, internal_init);

    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (!data) {
        uint32_t t_id = atomic_fetch_add(&thread_counter, 1);

        uint32_t arena_idx;
        if (global_config.flags & MBD_FLAG_CPU_LOCAL) {
#if defined(__linux__)
            int cpu = sched_getcpu();
            arena_idx = (cpu >= 0) ? (uint32_t)(cpu % arena_count) : (t_id % arena_count);
#elif defined(_WIN32)
            uint32_t cpu = GetCurrentProcessorNumber();
            arena_idx = cpu % arena_count;
#else
            // macOS / other POSIX: no sched_getcpu equivalent without Mach thread policy;
            // fall through to round-robin. CPU pinning is Phase 3 work.
            arena_idx = t_id % arena_count;
#endif
        } else {
            arena_idx = t_id % arena_count;
        }
        mbd_arena_t *arena = &arenas[arena_idx];

        pthread_mutex_lock(&arena->lock);
        drain_remote_queue(arena);

        uint32_t needed = (uint32_t)sizeof(thread_cache_data_t)
                        + (global_config.mmap_cache_slots * sizeof(block_header_t*))
                        + HEADER_SIZE;
        uint32_t order = next_power_of_two_order(needed);
        uint32_t cur   = order;

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
            for (int i = 0; i < arena_count; i++) {
                mbd_arena_t *other = &arenas[i];
                if (other == arena) continue;
                pthread_mutex_lock(&other->lock);
                drain_remote_queue(other);
                cur = order;
                while (cur <= global_config.max_order && !other->free_lists[cur]) cur++;
                if (cur <= global_config.max_order) {
                    block = other->free_lists[cur];
                    arena_remove(other, block, cur);
                    block = split_block_down(other, block, order);
                    atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
                    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
                    block->arena = other;
                    target_arena = other;
                    pthread_mutex_unlock(&other->lock);
                    break;
                }
                pthread_mutex_unlock(&other->lock);
            }
        }

        if (!block) return NULL;

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

static void refill_thread_cache(thread_cache_data_t *data, mbd_arena_t *locked_arena, uint32_t order) {
    if (order > global_config.small_order_max || order < global_config.min_order) return;

    uint32_t limit    = get_cache_limit(order);
    uint32_t refilled = 0;
    while (data->count[order] < limit) {
        if (global_config.refill_batch_size > 0 && refilled >= global_config.refill_batch_size) break;

        block_header_t *block = locked_arena->free_lists[order];
        if (!block) {
            uint32_t cur = order + 1;
            while (cur <= global_config.max_order && !locked_arena->free_lists[cur]) cur++;
            if (cur > global_config.max_order) break;

            block = locked_arena->free_lists[cur];
            arena_remove(locked_arena, block, cur);

            uint32_t split_count = 0;
            while (atomic_load_explicit(&block->order, memory_order_relaxed) > order) {
                uint32_t new_order = atomic_load_explicit(&block->order, memory_order_relaxed) - 1;
                block_header_t *buddy = get_buddy(locked_arena, block, new_order);
                /* GCC 15 LTO false positive: interprocedural analysis incorrectly infers
                 * buddy may be NULL (from the get_buddy bounds check) and flags a zero-address
                 * write. At runtime buddy is always valid here — we just verified cur <= max_order
                 * and split downward, so the offset is always within the pool. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
                atomic_store_explicit(&buddy->order, new_order, memory_order_relaxed);
                atomic_store_explicit(&buddy->flags, 0, memory_order_relaxed);
                buddy->arena = locked_arena;
                buddy->next = buddy->prev = NULL;

                if (new_order <= global_config.small_order_max && data->count[new_order] < get_cache_limit(new_order)) {
                    atomic_store_explicit(&buddy->flags, BLOCK_IN_CACHE, memory_order_relaxed);
                    atomic_store_explicit(&buddy->magic, encode_magic(buddy, MAGIC_CACHED), memory_order_release);
                    buddy->next = data->cache[new_order];
                    data->cache[new_order] = buddy;
                    data->count[new_order]++;
                    data->total_cached += (1ULL << new_order);
                    if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
                        atomic_fetch_add(&locked_arena->cached_bytes, 1ULL << new_order);
                } else {
                    atomic_store_explicit(&buddy->magic, encode_magic(buddy, MAGIC_FREE), memory_order_release);
                    arena_insert(locked_arena, new_order, buddy);
                }
                atomic_store_explicit(&block->order, new_order, memory_order_relaxed);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
                split_count++;
            }
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
        if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
            atomic_fetch_add(&locked_arena->cached_bytes, 1ULL << order);
        refilled++;
    }
}

/* ================================================================== *
 *  Public API implementations                                        *
 * ================================================================== */

/* ── Version query ───────────────────────────────────────────────────────── */

/* Stringify helpers — same pattern as Situation's SituationGetVersionString */
#define _MBD_STR_HELPER(x) #x
#define _MBD_STR(x) _MBD_STR_HELPER(x)

const char *MbdGetVersionString(void) {
    return _MBD_STR(MYBUDDY_VERSION_MAJOR) "."
           _MBD_STR(MYBUDDY_VERSION_MINOR) "."
           _MBD_STR(MYBUDDY_VERSION_PATCH)
           MYBUDDY_VERSION_REVISION
           " (" MYBUDDY_VERSION_DESCRIPTION ")";
}

/* ── Initialization ──────────────────────────────────────────────────────── */

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

void mbd_destroy(void) {
    // No-op if never initialized (pthread_once not yet run, arenas is NULL)
    if (!atomic_load_explicit(&fully_destroyed, memory_order_relaxed) && arenas == NULL) {
        return;
    }
    // No-op if already destroyed
    if (atomic_load_explicit(&fully_destroyed, memory_order_relaxed)) {
        return;
    }

    thread_cache_data_t *data = pthread_getspecific(thread_cache_key);
    if (data) {
        thread_cache_destructor(data);
        pthread_setspecific(thread_cache_key, NULL);
    }

    if (atomic_load_explicit(&active_threads, memory_order_acquire) != 0) {
        fprintf(stderr, "FATAL: mbd_destroy() called with %u active threads. "
                "All threads must exit before destroy.\n",
                atomic_load(&active_threads));
        abort();
    }

    for (int a = 0; a < arena_count; a++) atomic_store(&arenas[a].active, 0);

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

void mbd_set_oom_handler(void (*handler)(void)) {
    atomic_store(&global_oom_handler, handler);
}

void mbd_set_profiler_hook(void (*hook)(mbd_event_type_t, void*, size_t)) {
    atomic_store(&global_profiler_hook, hook);
}

void *mbd_alloc(size_t requested_size) {
    if (requested_size == 0) requested_size = 1;
    if (requested_size > SIZE_MAX - HEADER_SIZE) { handle_oom(); return NULL; }
    size_t needed = requested_size + HEADER_SIZE;
    uint32_t order = next_power_of_two_order(needed);
    thread_cache_data_t *data = local_thread_cache;

    /* HOT PATH */
    if (__builtin_expect(data && order <= global_config.small_order_max && data->cache[order], 1)) {
        if (__builtin_expect(atomic_load(&trim_requested) == data->last_trim_request, 1)) {
            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            data->total_cached -= (1ULL << order);
            atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            return (void *)((uint8_t*)block + HEADER_SIZE);
        }
    }

    /* SLOW PATH: large / mmap / OOM */
    if (needed > global_config.pool_size ||
        (!(global_config.flags & MBD_FLAG_BUDDY_LARGE) && order > global_config.large_cutoff_order)) {
        data = pthread_getspecific(thread_cache_key);
        if (data) {
            int best_idx = -1;
            size_t best_size = SIZE_MAX;
            for (uint32_t i = 0; i < data->mmap_cache_count; i++) {
                size_t bsize = data->mmap_cache[i]->mmap_size;
                if (bsize >= needed && bsize <= needed * global_config.mmap_max_waste_ratio && bsize < best_size) {
                    best_idx = (int)i;
                    best_size = bsize;
                    if (bsize == needed) break;
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
        if (block == MAP_FAILED) { handle_oom(); return NULL; }
#if defined(__linux__) || defined(__APPLE__) || defined(__MINGW32__) || defined(__MINGW64__)
#if defined(MADV_HUGEPAGE)
        if (needed >= global_config.hugepage_threshold)
            madvise(block, needed, MADV_HUGEPAGE);
#endif
#endif
        atomic_store_explicit(&block->flags, BLOCK_IS_MMAP, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_MMAP), memory_order_release);
        block->arena = NULL;
        block->mmap_size = needed;
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
        if (data && (global_config.flags & MBD_FLAG_ATOMIC_STATS))
            atomic_fetch_add_explicit(&data->cache_misses, 1, memory_order_relaxed);
        refill_thread_cache(data, arena, order);
        if (data->cache[order]) {
            block_header_t *block = data->cache[order];
            data->cache[order] = block->next;
            data->count[order]--;
            data->total_cached -= (1ULL << order);
            if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
                atomic_fetch_sub(&arena->cached_bytes, 1ULL << order);
            atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
            pthread_mutex_unlock(&arena->lock);
            void *res = (void *)((uint8_t*)block + HEADER_SIZE);
            MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
            return res;
        }
    }

    uint32_t cur = order;
    while (cur <= global_config.max_order && !arena->free_lists[cur]) cur++;

    if (cur > global_config.max_order) {
        pthread_mutex_unlock(&arena->lock);
        block_header_t *block = NULL;
        for (int i = 0; i < arena_count; i++) {
            mbd_arena_t *other = &arenas[i];
            if (other == arena) continue;
            pthread_mutex_lock(&other->lock);
            drain_remote_queue(other);
            cur = order;
            while (cur <= global_config.max_order && !other->free_lists[cur]) cur++;
            if (cur <= global_config.max_order) {
                block = other->free_lists[cur];
                arena_remove(other, block, cur);
                block = split_block_down(other, block, order);
                atomic_store_explicit(&block->flags, BLOCK_USED, memory_order_relaxed);
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
                block->arena = other;
                pthread_mutex_unlock(&other->lock);
                atomic_store(&data->arena, other);
                MBD_FIRE_EVENT(MBD_EVENT_MIGRATION, data, 0);

                static __thread uint32_t migration_counter = 0;
                migration_counter++;
                if (global_config.migration_return_freq > 0 &&
                   (migration_counter % global_config.migration_return_freq) == 0) {
                    atomic_store(&data->arena, data->native_arena);
                    MBD_FIRE_EVENT(MBD_EVENT_MIGRATION, data, 0);
                }

                void *res = (void *)((uint8_t*)block + HEADER_SIZE);
                MBD_FIRE_EVENT(MBD_EVENT_ALLOC, res, requested_size);
                return res;
            }
            pthread_mutex_unlock(&other->lock);
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
                atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);
                block->next = data->cache[order];
                data->cache[order] = block;
                data->count[order]++;
                data->total_cached += (1ULL << order);
                return;
            }
        }
    }

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
            if (raw_magic != MAGIC_ALLOC && raw_magic != MAGIC_MMAP) {
                fprintf(stderr, "mbd_free: DOUBLE-FREE or corruption of mmap block! ptr=%p\n", ptr);
                abort();
            }
        } else {
            if (raw_magic != encode_magic(block, MAGIC_ALLOC) && raw_magic != encode_magic(block, MAGIC_MMAP)) {
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
            if (raw_magic != MAGIC_CACHED) abort();
            fprintf(stderr, "mbd_free: DOUBLE-FREE! Block already in thread cache. ptr=%p\n", ptr);
            abort();
        }
        if (raw_magic != MAGIC_ALLOC) abort();
    } else {
        if (raw_flags & BLOCK_IN_CACHE) {
            if (raw_magic != encode_magic(block, MAGIC_CACHED)) abort();
            fprintf(stderr, "mbd_free: DOUBLE-FREE! Block already in thread cache. ptr=%p\n", ptr);
            abort();
        }
        if (raw_magic != encode_magic(block, MAGIC_ALLOC)) abort();
    }

    mbd_arena_t *arena = block->arena;
    if (global_config.flags & MBD_FLAG_HARDENED) {
        if (!arena || arena < arenas || arena >= arenas + arena_count) abort();
        if ((uintptr_t)block < (uintptr_t)arena->memory_pool ||
            (uintptr_t)block >= (uintptr_t)arena->memory_pool + global_config.pool_size) abort();
        if ((uintptr_t)ptr < (uintptr_t)arena->memory_pool + HEADER_SIZE ||
            (uintptr_t)ptr >= (uintptr_t)arena->memory_pool + global_config.pool_size) abort();
    }

    uint32_t order = atomic_load_explicit(&block->order, memory_order_relaxed);

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

    if (data && block->arena != atomic_load_explicit(&data->arena, memory_order_relaxed)) {
        mbd_arena_t *block_arena = block->arena;
        if (atomic_load(&block_arena->active)) {
            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
            remote_push(block_arena, block);
        }
        return;
    }

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
        if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
            atomic_fetch_add(&block->arena->cached_bytes, 1ULL << order);
        return;
    }

    if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
        atomic_fetch_add_explicit(&data->bulk_flushes, 1, memory_order_relaxed);

    mbd_arena_t *locked_arena = NULL;

    for (uint32_t o = global_config.min_order; o <= global_config.small_order_max; o++) {
        if (data->count[o] == 0) continue;
        int flush_count = 0;
        uint32_t current_count = data->count[o];
        uint32_t o_limit        = get_cache_limit(o);
        uint32_t high_watermark = (o_limit * global_config.flush_high_watermark_pct) / 100;
        uint32_t low_watermark  = (o_limit * global_config.flush_low_watermark_pct)  / 100;

        int deterministic = global_config.flags & MBD_FLAG_DETERMINISTIC;
        if (current_count >= high_watermark) {
            flush_count = (current_count > low_watermark) ? (int)(current_count - low_watermark) : 0;
            if (flush_count > 0) MBD_FIRE_EVENT(MBD_EVENT_WATERMARK_FLUSH, data, flush_count);
        } else if (!deterministic && data->total_cached >= global_config.cache_pressure_threshold) {
            flush_count = (current_count > low_watermark) ? (int)(current_count - low_watermark) : 0;
            if (flush_count > 0) MBD_FIRE_EVENT(MBD_EVENT_PRESSURE_FLUSH, data, flush_count);
        }

        while (flush_count > 0 && data->cache[o]) {
            flush_count--;
            block_header_t *to_global = data->cache[o];
            data->cache[o] = to_global->next;
            data->count[o]--;
            data->total_cached -= (1ULL << o);
            mbd_arena_t *block_arena = to_global->arena;
            if (locked_arena != block_arena) {
                if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
                locked_arena = block_arena;
                pthread_mutex_lock(&locked_arena->lock);
                drain_remote_queue(locked_arena);
            }
            atomic_store_explicit(&to_global->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&to_global->magic, encode_magic(to_global, MAGIC_FREE), memory_order_release);
            uint32_t original_order  = atomic_load_explicit(&to_global->order, memory_order_relaxed);
            uint32_t coalesced_order = original_order;
            to_global = coalesce_up_and_update(locked_arena, to_global, &coalesced_order);
            arena_insert(locked_arena, coalesced_order, to_global);
            if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
                atomic_fetch_sub(&locked_arena->cached_bytes, 1ULL << original_order);
        }
    }

    if (data->count[order] < limit) {
        if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
        atomic_store_explicit(&block->flags, BLOCK_IN_CACHE, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_CACHED), memory_order_release);
        block->next = data->cache[order];
        data->cache[order] = block;
        data->count[order]++;
        data->total_cached += (1ULL << order);
        if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
            atomic_fetch_add(&block->arena->cached_bytes, 1ULL << order);
    } else {
        mbd_arena_t *block_arena = block->arena;
        if (locked_arena != block_arena) {
            if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
            locked_arena = block_arena;
            pthread_mutex_lock(&locked_arena->lock);
            drain_remote_queue(locked_arena);
        }
        atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
        atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
        uint32_t coalesced_order = order;
        block = coalesce_up_and_update(locked_arena, block, &coalesced_order);
        arena_insert(locked_arena, coalesced_order, block);
        if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
    }
}

void *mbd_realloc(void *ptr, size_t new_size) {
    if (!ptr) return mbd_alloc(new_size);
    if (new_size == 0) { mbd_free(ptr); return NULL; }
    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);

    if (load_magic(block) == encode_magic(block, MAGIC_MEMALIGN)) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = (atomic_load_explicit(&raw_block->flags, memory_order_relaxed) & BLOCK_IS_MMAP)
                          ? (raw_block->mmap_size - HEADER_SIZE)
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
        if (m != encode_magic(block, MAGIC_ALLOC) && m != encode_magic(block, MAGIC_MMAP)) abort();
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
    if (new_size <= old_usable) {
        if (global_config.flags & MBD_FLAG_REALLOC_LOCK) {
            mbd_arena_t *arena = block->arena;
            pthread_mutex_lock(&arena->lock);
            pthread_mutex_unlock(&arena->lock);
        }
        return ptr;
    }

    mbd_arena_t *arena = block->arena;
    pthread_mutex_lock(&arena->lock);
    drain_remote_queue(arena);

    while (atomic_load_explicit(&block->order, memory_order_relaxed) < global_config.max_order) {
        size_t current_usable = ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
        if (new_size <= current_usable) break;
        uint32_t cur_order = atomic_load_explicit(&block->order, memory_order_relaxed);
        block_header_t *buddy = get_buddy(arena, block, cur_order);
        if (!buddy ||
            load_magic(buddy) != encode_magic(buddy, MAGIC_FREE) ||
            atomic_load_explicit(&buddy->order, memory_order_relaxed) != cur_order ||
            buddy->arena != arena ||
            !(atomic_load_explicit(&buddy->flags, memory_order_relaxed) & BLOCK_IN_FREE_LIST) ||
            (uintptr_t)buddy < (uintptr_t)block)
            break;
        arena_remove(arena, buddy, cur_order);
        atomic_fetch_add(&arena->coalesces, 1);
        MBD_FIRE_EVENT(MBD_EVENT_COALESCE, buddy, cur_order);
        atomic_fetch_add_explicit(&block->order, 1, memory_order_relaxed);
    }

    atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_ALLOC), memory_order_release);
    pthread_mutex_unlock(&arena->lock);

    old_usable = ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
    if (new_size <= old_usable) return ptr;

    void *new_ptr = mbd_alloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_usable);
    mbd_free(ptr);
    return new_ptr;
}

void *mbd_calloc(size_t nmemb, size_t size) {
    if (size != 0 && nmemb > SIZE_MAX / size) return NULL;
    size_t total = nmemb * size;
    if (total == 0) {
        void *p = mbd_alloc(1);
        if (p) memset(p, 0, mbd_malloc_usable_size(p));
        return p;
    }
    void *ptr = mbd_alloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *mbd_memalign(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= 32) return mbd_alloc(size);
    if (SIZE_MAX - size < alignment + 2 * HEADER_SIZE) return NULL;
    size_t request = size + alignment + 2 * HEADER_SIZE;
    void *raw = mbd_alloc(request);
    if (!raw) return NULL;
    uintptr_t raw_addr     = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + 2 * HEADER_SIZE + alignment - 1) & ~(alignment - 1);
    block_header_t *fake = (block_header_t *)(aligned_addr - HEADER_SIZE);
    memset(fake, 0, sizeof(*fake));
    atomic_store_explicit(&fake->magic, encode_magic(fake, MAGIC_MEMALIGN), memory_order_release);
    fake->prev = (block_header_t *)raw;
    return (void *)aligned_addr;
}

size_t mbd_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;
    block_header_t *block = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);
    if (atomic_load_explicit(&block->flags, memory_order_relaxed) & BLOCK_IS_MMAP) {
        uint32_t m = load_magic(block);
        if (m == encode_magic(block, MAGIC_ALLOC) || m == encode_magic(block, MAGIC_MMAP))
            return block->mmap_size - HEADER_SIZE;
    }
    mbd_arena_t *arena = block->arena;
    if (!arena || arena < arenas || arena >= arenas + arena_count) return 0;
    if ((uintptr_t)block < (uintptr_t)arena->memory_pool ||
        (uintptr_t)block >= (uintptr_t)arena->memory_pool + global_config.pool_size) return 0;
    if (load_magic(block) == encode_magic(block, MAGIC_MEMALIGN)) {
        void *raw = block->prev;
        block_header_t *raw_block = (block_header_t *)((uint8_t*)raw - HEADER_SIZE);
        size_t old_usable = (atomic_load_explicit(&raw_block->flags, memory_order_relaxed) & BLOCK_IS_MMAP)
                          ? (raw_block->mmap_size - HEADER_SIZE)
                          : ((size_t)1 << atomic_load_explicit(&raw_block->order, memory_order_relaxed)) - HEADER_SIZE;
        size_t offset = (const uint8_t*)ptr - (uint8_t*)raw;
        return old_usable > offset ? old_usable - offset : 0;
    }
    if (load_magic(block) != encode_magic(block, MAGIC_ALLOC)) return 0;
    return ((size_t)1 << atomic_load_explicit(&block->order, memory_order_relaxed)) - HEADER_SIZE;
}

static void flush_my_cache(thread_cache_data_t *curr) {
    mbd_arena_t *locked_arena = NULL;
    for (uint32_t o = global_config.min_order; o <= global_config.small_order_max; o++) {
        while (curr->cache[o]) {
            block_header_t *block = curr->cache[o];
            curr->cache[o] = block->next;
            curr->count[o]--;
            mbd_arena_t *block_arena = block->arena;
            if (locked_arena != block_arena) {
                if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);
                locked_arena = block_arena;
                pthread_mutex_lock(&locked_arena->lock);
                drain_remote_queue(locked_arena);
            }
            atomic_store_explicit(&block->flags, 0, memory_order_relaxed);
            atomic_store_explicit(&block->magic, encode_magic(block, MAGIC_FREE), memory_order_release);
            uint32_t original_order  = atomic_load_explicit(&block->order, memory_order_relaxed);
            uint32_t coalesced_order = original_order;
            block = coalesce_up_and_update(locked_arena, block, &coalesced_order);
            arena_insert(locked_arena, coalesced_order, block);
            if (global_config.flags & MBD_FLAG_ATOMIC_STATS)
                atomic_fetch_sub(&locked_arena->cached_bytes, 1ULL << original_order);
            curr->total_cached -= (1ULL << original_order);
        }
    }
    if (locked_arena) pthread_mutex_unlock(&locked_arena->lock);

    for (uint32_t i = 0; i < curr->mmap_cache_count; i++) {
        block_header_t *mmap_block = curr->mmap_cache[i];
        size_t mmap_size = mmap_block->mmap_size;
        atomic_fetch_sub(&huge_mmap_tracked, mmap_size);
        munmap(mmap_block, mmap_size);
    }
    curr->mmap_cache_count = 0;
}

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
    mbd_release_to_os();
}

mbd_stats_t mbd_get_stats(void) {
    pthread_once(&init_once, internal_init);
    mbd_stats_t s = {0};
    s.total_mapped_bytes = ((size_t)arena_count * global_config.pool_size) + atomic_load(&huge_mmap_tracked);
    if (arenas) {
        for (int a = 0; a < arena_count; a++) {
            pthread_mutex_lock(&arenas[a].lock);
            drain_remote_queue(&arenas[a]);
            for (uint32_t i = global_config.min_order; i <= global_config.max_order; i++)
                for (block_header_t *b = arenas[a].free_lists[i]; b; b = b->next)
                    s.total_free_bytes += (1ULL << i);
            s.total_cached_bytes += atomic_load(&arenas[a].cached_bytes);
            s.splits   += atomic_load(&arenas[a].splits);
            s.coalesces += atomic_load(&arenas[a].coalesces);
            pthread_mutex_unlock(&arenas[a].lock);
        }
    }
    s.total_allocated_bytes = s.total_mapped_bytes - s.total_free_bytes - s.total_cached_bytes;
    pthread_mutex_lock(&cache_list_lock);
    for (thread_cache_data_t *curr = global_cache_list; curr; curr = curr->next) {
        s.cache_hits   += atomic_load_explicit(&curr->cache_hits,   memory_order_relaxed);
        s.cache_misses += atomic_load_explicit(&curr->cache_misses, memory_order_relaxed);
        s.bulk_flushes += atomic_load_explicit(&curr->bulk_flushes, memory_order_relaxed);
    }
    pthread_mutex_unlock(&cache_list_lock);
    return s;
}

/* ── Diagnostics ─────────────────────────────────────────────────────────── */

static void mybuddy_itoa(size_t val, char *buf, int *len) {
    char temp[32];
    int i = 0;
    if (val == 0) { temp[i++] = '0'; }
    else { while (val > 0) { temp[i++] = '0' + (val % 10); val /= 10; } }
    for (int j = 0; j < i; j++) buf[*len + j] = temp[i - 1 - j];
    *len += i;
}

static void mybuddy_puts(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    if (len > 0) {
#if defined(__MINGW32__) || defined(__MINGW64__)
        // MinGW: use fwrite to stderr — write()/STDERR_FILENO are not always available
        fwrite(str, 1, len, stderr);
#else
        ssize_t ret = write(STDERR_FILENO, str, len);
        (void)ret;
#endif
    }
}

void mbd_dump(void) {
    for (int a = 0; a < arena_count; a++) {
        pthread_mutex_lock(&arenas[a].lock);
        drain_remote_queue(&arenas[a]);
        char buf[128];
        int len = 0;
        mybuddy_puts("\n=== Arena ");
        mybuddy_itoa(a, buf, &len); buf[len] = '\0';
        mybuddy_puts(buf);
        mybuddy_puts(" Free Lists ===\n");
        for (uint32_t i = global_config.min_order; i <= global_config.max_order; i++) {
            int count = 0;
            for (block_header_t *b = arenas[a].free_lists[i]; b; b = b->next) count++;
            if (count) {
                len = 0;
                mybuddy_puts("Order ");
                mybuddy_itoa(i, buf, &len); buf[len] = '\0'; mybuddy_puts(buf);
                mybuddy_puts(" (");
                len = 0;
                mybuddy_itoa((size_t)1<<i, buf, &len); buf[len] = '\0'; mybuddy_puts(buf);
                mybuddy_puts(" B) : ");
                len = 0;
                mybuddy_itoa(count, buf, &len); buf[len] = '\0'; mybuddy_puts(buf);
                mybuddy_puts(" free\n");
            }
        }
        mybuddy_puts("==================================\n\n");
        pthread_mutex_unlock(&arenas[a].lock);
    }
}

#endif /* MYBUDDY_IMPL_H */
