# MyBuddy v1.5 — Hardening Plan

Actionable fixes for the sharp edges identified in the code review, plus architectural prep for Quest NRC integration.

> **Context:** MyBuddy is a lock-free buddy allocator that works well in controlled conditions but can tilt unpredictably under parameter changes and bursty workloads. This plan addresses correctness issues first, then stability/predictability, then NRC-specific optimizations.

## Table of Contents
1. [Memory Ordering Fix (order load)](#1-memory-ordering-fix)
2. [ABA Risk Documentation + Mitigation](#2-aba-risk-on-remote-free-queue)
3. [Thread Destructor Contract Enforcement](#3-thread-destructor-contract)
4. [Cache Pressure Accounting Unification](#4-cache-pressure-accounting)
5. [Hardened Mode Assertions](#5-hardened-mode-assertions)
6. [NUMA Flag Honesty](#6-numa-flag-honesty)
7. [Skipped Bit Documentation](#7-skipped-bit-documentation)
8. [Frame Arena Layer for NRC](#8-frame-arena-layer-for-nrc)
9. [Predictability Improvements](#9-predictability-improvements)

---

## 1. Memory Ordering Fix

**Priority:** P0 — Correctness
**Effort:** 15 minutes
**Risk:** Zero — strictly tighter ordering, no behavioral change

### Problem
In `coalesce_up()`, the buddy validation reads `magic` with `acquire` but `order` with `relaxed`:
```c
if (load_magic(buddy) != encode_magic(buddy, MAGIC_FREE) ||
    atomic_load_explicit(&buddy->order, memory_order_relaxed) != order ||
    buddy->arena != arena)
```
If another thread writes `order` then `magic` (with release), this is fine. But if the write order is ever reversed (magic first, order later), we can observe new magic + stale order — a silent coalescing bug.

### Fix
Change the `order` load to `acquire`:
```c
atomic_load_explicit(&buddy->order, memory_order_acquire) != order
```
This is not a hot loop — it's the coalescing validation path under the arena lock. One instruction difference, correctness guaranteed.

### Files
- `mybuddy.h` — `coalesce_up()` function (~line 684)

---

## 2. ABA Risk on Remote Free Queue

**Priority:** P2 — Acknowledge + Document
**Effort:** 30 minutes (comment + optional tagged pointer)
**Risk:** Low in practice, theoretical in current workloads

### Problem
The remote free queue uses a classic lock-free LIFO push:
```c
block->next = atomic_load_explicit(&arena->remote_free_queue.head, memory_order_acquire);
while (!atomic_compare_exchange_weak(&arena->remote_free_queue.head, &block->next, block)) {}
```
No ABA protection. A block can be pushed, popped, reused, and pushed again at the same address. The CAS can succeed incorrectly.

### Why It Hasn't Bitten
- Low contention window in current workloads
- Allocator timing hides the race
- Quest's frame-coherent allocation pattern doesn't trigger rapid cross-thread push/pop cycles

### Action
1. **Document the risk** in a comment above `remote_push()`:
   ```c
   /* NOTE: This is a classic lock-free LIFO push with no ABA protection.
    * In theory, a block can be pushed, popped, reused, and pushed again at the
    * same address, causing CAS to succeed incorrectly. In practice, this requires
    * extremely tight cross-thread timing that our workloads don't exhibit.
    * If corruption is ever observed in stress tests, tagged pointers are the fix.
    * See: https://en.wikipedia.org/wiki/ABA_problem */
   ```
2. **Optional future fix:** Tagged pointers (pack a generation counter into the upper bits of the pointer). Only worth doing if stress tests ever reproduce corruption.

### Files
- `mybuddy.h` — `remote_push()` function (~line 712)

---

## 3. Thread Destructor Contract

**Priority:** P1 — Safety
**Effort:** 30 minutes
**Risk:** Zero — additive diagnostics only

### Problem
The `active` flag teardown in `mbd_destroy()` is a hard contract:
```c
assert(atomic_load(&active_threads) == 0 && "mbd_destroy called while other threads are active!");
```
But `assert` is compiled out in release builds. If someone violates the contract, silent corruption follows.

### Fix
1. **Add a runtime check in release builds:**
   ```c
   if (atomic_load_explicit(&active_threads, memory_order_acquire) != 0) {
       fprintf(stderr, "FATAL: mbd_destroy() called with %u active threads. "
               "All threads must exit before destroy.\n",
               atomic_load(&active_threads));
       abort();
   }
   ```
2. **Document in README** under a "Thread Safety Contract" section:
   > `mbd_destroy()` must only be called after all threads using the allocator have exited. Violating this contract causes undefined behavior. The allocator will abort in debug and release builds if active threads are detected.

### Files
- `mybuddy.h` — `mbd_destroy()` function (~line 1258)
- `README.md` — add Thread Safety Contract section

---

## 4. Cache Pressure Accounting

**Priority:** P1 — Stability under bursty workloads
**Effort:** 2–3 hours
**Risk:** Medium — behavioral change in heuristics, needs stress testing

### Problem
`cached_bytes` and `cache_pressure` are updated inconsistently:
- Refill path updates both
- Some free paths update both
- Some paths update only one
- The `cache_pressure_threshold` check uses `cache_pressure` but the stats report `cached_bytes`

Under bursty NRC workloads (allocate a batch, train, free), this causes oscillation: premature flushes when pressure is over-counted, or hoarding when it's under-counted.

### Fix — Option A: Unify into one counter
Remove `cache_pressure` entirely. Use `cached_bytes` as the single source of truth for both stats and heuristic decisions. Every path that adds to cache increments it, every path that removes decrements it. The `cache_pressure_threshold` check becomes:
```c
if (atomic_load_explicit(&arena->cached_bytes, memory_order_relaxed) > 
    global_config.cache_pressure_threshold) {
    // trigger flush
}
```

### Fix — Option B: Separate semantics clearly
If you want `cache_pressure` to mean something different from `cached_bytes` (e.g., a weighted/decayed metric), document the difference explicitly and ensure every code path updates the correct one. Add a comment block at the struct definition explaining the distinction.

### Recommendation
Option A. Simpler, fewer bugs, easier to reason about. The "pressure" concept was likely added for future decay heuristics that never materialized.

### Validation
Run `test_multithread_stress` before and after. Compare `mbd_get_stats()` output — `total_cached_bytes` should be consistent across runs with the same workload.

### Files
- `mybuddy.h` — arena struct definition, all paths that touch `cached_bytes` or `cache_pressure` (~15 sites)

---

## 5. Hardened Mode Assertions

**Priority:** P2 — Defense in depth
**Effort:** 30 minutes
**Risk:** Zero — only fires in hardened mode

### Problem
`get_buddy()` trusts the `order` field for alignment math:
```c
uintptr_t offset = (uintptr_t)block - (uintptr_t)arena->memory_pool;
if (offset & ((1ULL << order) - 1)) return NULL;
```
If `order` is corrupted, this silently returns NULL (or worse, a valid-looking pointer to the wrong block).

### Fix
In hardened mode, add an explicit assertion:
```c
if (global_config.flags & MBD_FLAG_HARDENED) {
    uint32_t loaded_order = atomic_load_explicit(&block->order, memory_order_acquire);
    if (loaded_order != order) {
        fprintf(stderr, "CORRUPTION: get_buddy() order mismatch: expected %u, got %u at %p\n",
                order, loaded_order, (void*)block);
        abort();
    }
}
```

### Files
- `mybuddy.h` — `get_buddy()` function (~line 650)

---

## 6. NUMA Flag Honesty

**Priority:** P2 — Naming accuracy
**Effort:** 30 minutes
**Risk:** Zero — documentation/naming only

### Problem
`MBD_FLAG_NUMA_AWARE` currently does CPU-local arena binding via `sched_getcpu()`, but no actual NUMA memory policy (`mbind`, `numa_alloc_onnode`). The flag name implies more than it delivers.

### Fix
Either:
1. **Rename** to `MBD_FLAG_CPU_LOCAL` (honest about what it does)
2. **Or implement** actual NUMA binding (Phase 5 of SCALING_PLAN.md)

### Recommendation
Rename now, implement later. A flag that lies is worse than a missing feature.

### Files
- `mybuddy.h` — flag definition (~line 95), `get_thread_cache()` (~line 1025)
- `README.md` — update flag documentation

---

## 7. Skipped Bit Documentation

**Priority:** P3 — Maintenance hygiene
**Effort:** 5 minutes
**Risk:** Zero

### Problem
```c
#define BLOCK_IS_MMAP        (1u << 1)
#define BLOCK_IN_FREE_LIST   (1u << 2)
// bit 3 skipped
#define BLOCK_IN_CACHE       (1u << 4)
```
Bit 3 is unused with no comment explaining why.

### Fix
```c
#define BLOCK_IS_MMAP        (1u << 1)
#define BLOCK_IN_FREE_LIST   (1u << 2)
// bit 3: reserved (was BLOCK_IS_SPLIT, removed in v1.4)
#define BLOCK_IN_CACHE       (1u << 4)
```
Or whatever the actual history is. Just document it.

### Files
- `mybuddy.h` — flag definitions (~line 119)

---

## 8. Frame Arena Layer for NRC

**Priority:** P1 — Architecture for Quest integration
**Effort:** 4–6 hours
**Risk:** Low — additive API, doesn't touch existing paths

### Problem
Quest's NRC workload is phase-based: allocate a batch of path data, train the network, free everything. This is a frame arena pattern, not a general-purpose allocation pattern. Using `mbd_alloc`/`mbd_free` per sample wastes cycles on per-block bookkeeping when the entire batch has the same lifetime.

### Design
A thin frame arena layer on top of MyBuddy:

```c
typedef struct {
    void *base;          // Single mbd_alloc for the whole frame
    size_t capacity;     // Total bytes allocated
    size_t offset;       // Current bump pointer
} mbd_frame_arena_t;

// Allocate the arena for this frame
void mbd_frame_arena_init(mbd_frame_arena_t *fa, size_t capacity);

// Bump-allocate within the frame (zero overhead, no bookkeeping)
void *mbd_frame_alloc(mbd_frame_arena_t *fa, size_t size, size_t align);

// Reset for next frame (no per-block free, just reset the offset)
void mbd_frame_arena_reset(mbd_frame_arena_t *fa);

// Release the backing memory
void mbd_frame_arena_destroy(mbd_frame_arena_t *fa);
```

### Usage in Quest NRC
```c
// Per frame:
mbd_frame_arena_reset(&nrc_arena);
PathData *batch = mbd_frame_alloc(&nrc_arena, batch_size * sizeof(PathData), 32);
// ... fill batch from GPU readback ...
// ... train network ...
// batch is implicitly "freed" on next reset
```

Zero per-sample allocation. One `mbd_alloc` per frame for the backing memory (reused across frames). The buddy allocator handles the big block; the frame arena handles the subdivision.

### Files
- New: `mbd_frame_arena.h` (or inline in `mybuddy.h` under `#ifdef MBD_FRAME_ARENA`)

---

## 9. Predictability Improvements

**Priority:** P2 — Reduce "tilting" behavior
**Effort:** 2–3 hours
**Risk:** Medium — behavioral changes, needs profiling

### Problem
MyBuddy can behave unpredictably under parameter changes because multiple interacting heuristics (cache limits, pressure thresholds, flush watermarks, refill batch sizes) create emergent behavior that's hard to reason about.

### Actionables

1. **Add a deterministic mode flag** (`MBD_FLAG_DETERMINISTIC`):
   - Disables adaptive heuristics (fixed cache sizes, no pressure-based flushing)
   - Makes behavior reproducible for debugging
   - Useful for A/B testing parameter changes

2. **Log heuristic decisions** when profiler hook is enabled:
   ```c
   MBD_FIRE_EVENT(MBD_EVENT_FLUSH, NULL, flushed_bytes);
   // Add: MBD_EVENT_PRESSURE_FLUSH, MBD_EVENT_WATERMARK_FLUSH, MBD_EVENT_MIGRATION
   ```
   This lets you trace *why* the allocator made a decision, not just *what* it did.

3. **Bound the interaction space**: Document which parameters interact and how:
   | Parameter | Interacts With | Effect |
   |-----------|---------------|--------|
   | `cache_pressure_threshold` | `flush_high_watermark_pct` | Both can trigger flushes — which fires first? |
   | `refill_batch_size` | `cache_limits[]` | Batch > limit = wasted work |
   | `migration_return_freq` | `arena_count` | More arenas = more migration opportunities |

### Files
- `mybuddy.h` — new flag, event types, parameter interaction documentation
- `README.md` — parameter interaction table

---

## Summary

| # | Item | Priority | Effort | Type |
|---|------|----------|--------|------|
| 1 | Memory ordering fix | P0 | 15 min | Correctness |
| 2 | ABA documentation | P2 | 30 min | Documentation |
| 3 | Thread destructor enforcement | P1 | 30 min | Safety |
| 4 | Cache pressure unification | P1 | 2–3 hr | Stability |
| 5 | Hardened mode assertions | P2 | 30 min | Defense |
| 6 | NUMA flag rename | P2 | 30 min | Naming |
| 7 | Skipped bit comment | P3 | 5 min | Hygiene |
| 8 | Frame arena for NRC | P1 | 4–6 hr | Architecture |
| 9 | Predictability improvements | P2 | 2–3 hr | Stability |

**Total estimated effort:** 10–14 hours

**Recommended execution order:** 1 → 3 → 4 → 8 → 5 → 2 → 9 → 6 → 7

Fix the correctness issue first (1), then the safety contract (3), then stabilize the heuristics (4), then build the NRC integration layer (8). Everything else is polish.
