# SCALING PLAN: MyBuddy Memory Allocator

This document outlines the feasibility and actionable steps for implementing advanced scaling and profiling features in the MyBuddy memory allocator.

## 1. Multi-Arena Scaling

**Feasibility:** High.
The current architecture uses a single global lock (`global_lock`) and a single static memory pool (`memory_pool`). Introducing multiple arenas will significantly reduce lock contention on the slow path (cache misses, bulk flushes, and large allocations) for highly concurrent workloads.

**Actionables:**
- [ ] **Abstract the Global State:** Encapsulate the global variables (`global_free_lists`, `memory_pool`) into an `mbd_arena_t` struct. Drop the single `global_lock` entirely in favor of per-arena locks (`pthread_mutex_t lock` inside `mbd_arena_t`). This alone yields near-linear scaling up to core count for most workloads.
- [ ] **Arena Array/Dynamic Allocation:** Replace the single static pool with an array of arenas or dynamically allocate arenas via `mmap()`/`VirtualAlloc()` instead of using a `.bss` static array.
- [ ] **Thread-to-Arena Mapping:** Don't overthink it initially. Start with a simple `arena = arenas[thread_id % arena_count];`. Explicitly define the strategy as `arena_count = 2 * number_of_cores` as a practical default (1x causes burst contention, 4x yields diminishing returns).
- [ ] **Arena-Local Caches:** The `thread_cache_data_t` TLS cache MUST be bound strictly to ONE arena with no mixing. Store the arena pointer explicitly inside the `thread_cache_data_t` struct upon initialization to prevent recomputation and drift bugs. This completely avoids reintroducing indirect cross-arena traffic.
- [ ] **Refactoring:** Update `global_insert`, `global_remove`, `coalesce_up`, and `split_block_down` to accept a pointer to the relevant `mbd_arena_t` instead of accessing global variables directly.
- [ ] **Cross-Arena Frees & Remote Queues (Crucial):** Do not rely on offset math or page headers. Add an explicit arena pointer to the header. Furthermore, to prevent cross-core lock contention during foreign frees (e.g., producer/consumer patterns), introduce a lock-free `remote_free_queue` per arena.
    ```c
    typedef struct block_header {
        uint8_t order;             // Shrunk to 1 byte
        uint32_t magic;            // Encode 'used' and 'cached' flags here
        struct mbd_arena *arena;   // ← KEY ADDITION (8 bytes)
        struct block_header *next;
        struct block_header *prev;
    } block_header_t;

    typedef struct mbd_arena {
        pthread_mutex_t lock;
        block_header_t *free_lists[MAX_ORDER + 1];
        _Atomic(block_header_t*) remote_free_queue; // Lock-free push, lock-drain
        size_t committed;
        size_t reserved;
    } mbd_arena_t;
    ```
    This guarantees O(1) arena lookup on free, eliminates range scanning, and avoids cross-thread locking on free by allowing the owning arena to drain the queue when it locks naturally.
- [ ] **Explicit Arena Growth Policy:** Arenas should grow independently (not share a global virtual space). Each arena must own its own `mmap` regions, grow in chunked sizes (e.g., 2MB-64MB), and independently track its `committed` and `reserved` capacities to avoid hidden NUMA coupling.

## 2. NUMA Awareness

**Feasibility:** Medium.
Requires OS-specific APIs (e.g., `libnuma` on Linux, or Windows NUMA APIs). It pairs well with Multi-Arena Scaling by ensuring that threads allocate memory physically close to the CPU they are running on. Importantly, NUMA awareness should only be layered *on top* of the multi-arena foundation.

**Actionables:**
- [ ] **NUMA-Aware Arenas:** Group the previously designed `mbd_arena_t` structures by NUMA node.
- [ ] **Memory Allocation:** Instead of a static BSS array, allocate arena memory using NUMA-aware APIs (e.g., `numa_alloc_onnode()` on Linux, or `mmap()` combined with `mbind()` and `MPOL_BIND`).
- [ ] **Thread Node Detection:** Upon thread initialization (in `get_thread_cache()`), detect the current thread's NUMA node. Wrap the detection in a portable macro like `mbd_get_current_cpu()`. On Linux, prefer `sched_getcpu()` over `getcpu()`, and fallback to simple round-robin if unavailable.
- [ ] **Permanent Pinning:** When evaluating `arena = arena_for_node(node)`, permanently pin the thread cache to that arena. Do NOT dynamically reassign arenas per allocation, as cache locality vastly outweighs theoretical balance and migration kills performance.
- [ ] **Fallback Policy:** Remote stealing should be the *last* resort. The policy sequence must be: 1) Try local arena, 2) Try allocate new memory locally (preferring chunked growth, e.g., 2MB-64MB chunks, over per-request mapping to avoid OS syscall overhead), 3) Fallback to remote steal.
- [ ] **OS Abstraction Layer:** Since NUMA APIs are highly OS-dependent, introduce macro guards (e.g., `#ifdef MBD_USE_NUMA`) and an abstraction layer to keep the library portable.

## 3. Long-Term Fragmentation Heuristics

**Feasibility:** Medium to High.
While the buddy system guarantees mathematical bounds on fragmentation, "long-lived" small allocations can pin larger blocks, preventing them from coalescing back into high-order blocks.

**Actionables:**
- [ ] **Fragmentation Metrics:** Track the distribution of free blocks. Compare the total free memory to the largest contiguous free block available. Crucially, track a per-order histogram, as buddy systems behave in orders, making a lack of specific mid-tier orders a strong signal for fragmentation.
- [ ] **Segregation via Extended API:** Do not force separate arenas immediately. Instead, introduce an extended API like `mbd_alloc_ex(size, flags)`. Internally route flagged allocations to dedicated arenas. This preserves the clean standard API while providing a gradual evolution path.
- [ ] **Aggressive Coalescing/Trim:** While `thread_cache_destructor` currently flushes all blocks, long-running threads might accumulate unused cache capacity. Introduce a periodic heuristic (or a user-callable `mbd_trim()`) that forcefully flushes thread caches to global lists, maximizing the chances of `coalesce_up` succeeding.
- [ ] **Cache Decay / Aging:** Add a time-based or allocation-count based decay mechanism (e.g., `if (unlikely(op_counter % DECAY_INTERVAL == 0)) flush_some_cache(data)`) to proactively flush unused capacity from long-lived threads, significantly improving long-term memory quality.
- [ ] **Constrained OS Page Release:** For dynamic pools (via `mmap`), use `madvise(..., MADV_DONTNEED)` to return physical RAM to the OS while keeping the virtual address space intact. However, strictly constrain this release to `order >= PAGE_ORDER + 1` to avoid constant page churn.

## 4. Profiling & Telemetry Hooks

**Feasibility:** High.
Adding telemetry is straightforward but must be done carefully to avoid introducing overhead in the lock-free fast path.

**Actionables:**
- [ ] **Metrics Structure:** Define a public `mbd_stats_t` struct containing counters for:
    - [ ] Total allocated bytes / Total free bytes
    - [ ] Thread cache hits vs. misses
    - [ ] Arena lock contention (e.g., via `pthread_mutex_trylock` loops, or measuring wait time)
    - [ ] Number of bulk flushes and block splits
- [ ] **Atomic Counters:** Use `_Atomic` or compiler intrinsics (e.g., `__atomic_fetch_add`) for global metrics. For fast-path metrics (cache hits), store counters directly in `thread_cache_data_t` to avoid cache-line bouncing, and aggregate them only when requested.
- [ ] **API Exposure:** Implement `void mbd_get_stats(mbd_stats_t *out_stats)` to aggregate thread-local and global statistics.
- [ ] **Event Hooks:** Provide an optional callback mechanism, e.g., `void mbd_set_profiler_hook(void (*hook)(mbd_event_type_t, void* ptr, size_t size))`, enabled via a compile-time macro (`#define MYBUDDY_ENABLE_PROFILING`). Explicitly define the event types early to prevent API churn:
    ```c
    typedef enum {
        MBD_EVENT_ALLOC,
        MBD_EVENT_FREE,
        MBD_EVENT_SPLIT,
        MBD_EVENT_COALESCE,
        MBD_EVENT_FLUSH
    } mbd_event_type_t;
    ```
- [ ] **Zero-Overhead Wrapper:** When firing hooks, ensure zero branch misprediction penalty by wrapping the call using `__builtin_expect`:
    ```c
    #ifdef MYBUDDY_ENABLE_PROFILING
        if (__builtin_expect(profiler_hook != NULL, 0))
            profiler_hook(...);
    #endif
    ```

## 5. Micro-Optimizations (Optional)
- [ ] **Shrink Header:** Store `order` in a single `uint8_t` (since MAX_ORDER is typically 30).
- [ ] **Encode Flags:** Encode `used` or `cached` states into the `magic` value (e.g., `MAGIC_ALLOC | FLAG_CACHED`) to shrink the header further and improve cache density.
- [ ] **O(1) Order Lookup:** Replace the `while ((1U << order) < size)` loop with compiler intrinsics like `__builtin_clz()` or a precomputed lookup table to eliminate looping branches on the fast path.

## 6. Implementation Roadmap

To successfully implement these architectural shifts without destabilizing the current deterministic buddy core, follow this phased approach:

- [ ] **Phase 1 (Core Scalability):**
    - [ ] Introduce `mbd_arena_t` struct to encapsulate global state.
    - [ ] Replace `global_lock` with per-arena locks (`pthread_mutex_t lock`).
    - [ ] Add the `arena` pointer into `block_header_t` to allow O(1) arena lookup.
    - [ ] Bind TLS caches (`thread_cache_data_t`) strictly to a single arena.
- [ ] **Phase 2 (Correctness + Structure):**
    - [ ] Refactor internal helpers to be arena-aware instead of using global variables.
    - [ ] Implement O(1) cross-arena frees via the `arena` header pointer.
    - [ ] Introduce a lock-free `remote_free_queue` per arena for safe, cross-core foreign frees.
- [ ] **Phase 3 (Observability):**
    - [ ] Define `mbd_stats_t` for observability metrics (e.g., total bytes, cache hits/misses, lock contention).
    - [ ] Use atomic/TLS counters for tracking metrics without lock overhead.
    - [ ] Create an optional event hook system with the `mbd_event_type_t` enum (ALLOC, FREE, SPLIT, COALESCE, FLUSH).
    - [ ] Wrap event hooks in zero-overhead `__builtin_expect` statements.
- [ ] **Phase 4 (Memory Behavior):**
    - [ ] Build out fragmentation metrics by tracking per-order histograms.
    - [ ] Implement `mbd_trim()` to forcefully flush unused cache capacity from long-lived threads.
    - [ ] Constrain OS page release logic (`madvise` with `MADV_DONTNEED`) strictly to `order >= PAGE_ORDER + 1` to prevent constant page churn.
- [ ] **Phase 5 (Advanced Scaling):**
    - [ ] Group `mbd_arena_t` structures by NUMA node.
    - [ ] Allocate arena memory using NUMA APIs (e.g., `numa_alloc_onnode` or `mmap` combined with `mbind`).
    - [ ] Add portable thread node detection via a macro like `mbd_get_current_cpu()`.
    - [ ] Enforce a remote fallback stealing policy: local arena -> local allocate (chunked growth) -> remote steal.
