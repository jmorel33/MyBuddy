# SCALING PLAN: MyBuddy Memory Allocator

This document outlines the feasibility and actionable steps for implementing advanced scaling and profiling features in the MyBuddy memory allocator.

## 1. Multi-Arena Scaling

**Feasibility:** High.
The current architecture uses a single global lock (`global_lock`) and a single static memory pool (`memory_pool`). Introducing multiple arenas will significantly reduce lock contention on the slow path (cache misses, bulk flushes, and large allocations) for highly concurrent workloads.

**Actionables:**
*   **Abstract the Global State:** Encapsulate the global variables (`global_free_lists`, `memory_pool`, `global_lock`) into an `mbd_arena_t` struct.
*   **Arena Array/Dynamic Allocation:** Replace the single static pool with an array of arenas or dynamically allocate arenas via `mmap()`/`VirtualAlloc()` instead of using a `.bss` static array.
*   **Thread-to-Arena Mapping:** Assign threads to specific arenas. This could be done via a simple hash of the thread ID or round-robin assignment stored in a thread-local variable when the thread's cache is initialized.
*   **Refactoring:** Update `global_insert`, `global_remove`, `coalesce_up`, and `split_block_down` to accept a pointer to the relevant `mbd_arena_t` instead of accessing global variables directly.
*   **Cross-Arena Frees:** When freeing a block, determine which arena it belongs to (via pointer arithmetic or page headers) to ensure it is returned to the correct arena and lock the appropriate mutex.

## 2. NUMA Awareness

**Feasibility:** Medium.
Requires OS-specific APIs (e.g., `libnuma` on Linux, or Windows NUMA APIs). It pairs well with Multi-Arena Scaling by ensuring that threads allocate memory physically close to the CPU they are running on.

**Actionables:**
*   **NUMA-Aware Arenas:** Group the previously designed `mbd_arena_t` structures by NUMA node.
*   **Memory Allocation:** Instead of a static BSS array, allocate arena memory using NUMA-aware APIs (e.g., `numa_alloc_onnode()` on Linux, or `mmap()` combined with `mbind()` and `MPOL_BIND`).
*   **Thread Node Detection:** Upon thread initialization (in `get_thread_cache()`), detect the current thread's NUMA node (e.g., using `getcpu()`). Assign the thread to an arena on its local node.
*   **Fallback Mechanism:** If a thread's local NUMA arena runs out of memory, implement a slow-path fallback to steal memory from an arena on a remote NUMA node, or map more memory locally.
*   **OS Abstraction Layer:** Since NUMA APIs are highly OS-dependent, introduce macro guards (e.g., `#ifdef MBD_USE_NUMA`) and an abstraction layer to keep the library portable.

## 3. Long-Term Fragmentation Heuristics

**Feasibility:** Medium to High.
While the buddy system guarantees mathematical bounds on fragmentation, "long-lived" small allocations can pin larger blocks, preventing them from coalescing back into high-order blocks.

**Actionables:**
*   **Fragmentation Metrics:** Track the distribution of free blocks. Compare the total free memory to the largest contiguous free block available.
*   **Segregation of Short and Long-Lived Objects:** (If API changes are allowed) Introduce arena hints or separate arenas for known long-lived objects to prevent them from fragmenting the general-purpose, high-turnover pool.
*   **Aggressive Coalescing/Trim:** While `thread_cache_destructor` currently flushes all blocks, long-running threads might accumulate unused cache capacity. Introduce a periodic heuristic (or a user-callable `mbd_trim()`) that forcefully flushes thread caches to global lists, maximizing the chances of `coalesce_up` succeeding.
*   **OS Page Release:** For dynamic pools (via `mmap`), identify buddy blocks that span entire OS pages (e.g., order 12+ for 4 KiB pages) and use `madvise(..., MADV_DONTNEED)` to return physical RAM to the OS while keeping the virtual address space intact.

## 4. Profiling & Telemetry Hooks

**Feasibility:** High.
Adding telemetry is straightforward but must be done carefully to avoid introducing overhead in the lock-free fast path.

**Actionables:**
*   **Metrics Structure:** Define a public `mbd_stats_t` struct containing counters for:
    *   Total allocated bytes / Total free bytes
    *   Thread cache hits vs. misses
    *   Arena lock contention (e.g., via `pthread_mutex_trylock` loops, or measuring wait time)
    *   Number of bulk flushes and block splits
*   **Atomic Counters:** Use `_Atomic` or compiler intrinsics (e.g., `__atomic_fetch_add`) for global metrics. For fast-path metrics (cache hits), store counters directly in `thread_cache_data_t` to avoid cache-line bouncing, and aggregate them only when requested.
*   **API Exposure:** Implement `void mbd_get_stats(mbd_stats_t *out_stats)` to aggregate thread-local and global statistics.
*   **Event Hooks:** Provide an optional callback mechanism, e.g., `void mbd_set_profiler_hook(void (*hook)(mbd_event_type_t, void* ptr, size_t size))`, enabled via a compile-time macro (`#define MYBUDDY_ENABLE_PROFILING`) to ensure zero overhead in production when disabled.
