# MyBuddy Update Log

---

## [v1.6.2] - Dedicated Version Header & `MbdGetVersionString()` API
**Release Date:** 2026-06-14

### Architecture
- **`mybuddy_version.h`** — new dedicated version header, the sole canonical source of truth for MyBuddy's version. Mirrors `situation_base_version.h` exactly: `MYBUDDY_VERSION_MAJOR`, `MYBUDDY_VERSION_MINOR`, `MYBUDDY_VERSION_PATCH`, `MYBUDDY_VERSION_REVISION` (empty string or tag), and `MYBUDDY_VERSION_DESCRIPTION` (human-readable release note). Bump only this file on every release.
- Version macros removed from `mybuddy_api.h` — replaced with `#include "mybuddy_version.h"`. The `mybuddy_api.h` doc comment still carries `@version` for tooling, but the macros live exclusively in `mybuddy_version.h`.
- `mybuddy.h` (orchestrator) now includes `mybuddy_version.h` first, before `mybuddy_api.h`, so version macros are available to any translation unit that includes `mybuddy.h` — regardless of whether `MYBUDDY_IMPLEMENTATION` is defined.

### API
- **`MbdGetVersionString()`** — new public function declared in `mybuddy_api.h`, implemented in `mybuddy_impl.h`. Returns a static null-terminated string in the format `"MAJOR.MINOR.PATCH[REVISION] (DESCRIPTION)"`. Uses the same stringify-macro pattern as Situation's `SituationGetVersionString()`:
  ```c
  const char *MbdGetVersionString(void);
  // Returns: "1.6.2 (Dedicated version header, MbdGetVersionString() API, full 3-OS audit)"
  ```
  Do not parse this string programmatically — use `MYBUDDY_VERSION_*` macros for compile-time checks.

### Validation
All 7 tests pass. Version string verified at runtime:
```
Version string: 1.6.2 (Dedicated version header, MbdGetVersionString() API, full 3-OS audit)
Macros:         1.6.2
Description:    Dedicated version header, MbdGetVersionString() API, full 3-OS audit
```

---

## [v1.6.1] - MinGW Build Fixes & Full Test Gate
**Release Date:** 2026-06-14

### Bug Fixes
- **`<ntstatus.h>` added to MinGW include block** (`mybuddy_impl.h`) — `BCryptGenRandom` returns an `NTSTATUS` value, but `STATUS_SUCCESS` is declared in `<ntstatus.h>`, not in `<bcrypt.h>` or `<windows.h>`. Under MSYS2/MinGW-w64 this caused a compile error: `'STATUS_SUCCESS' undeclared`. Fixed by adding `#include <ntstatus.h>` immediately after `<windows.h>` in the MinGW platform block.
- **`mybuddy_puts()` ported to MinGW** (`mybuddy_impl.h`) — the `mbd_dump()` diagnostic function used `write(STDERR_FILENO, ...)`, a raw POSIX syscall that is not reliably available in MinGW's headers (it lives in `<unistd.h>` which is excluded on the MinGW path). Fixed with a `#if defined(__MINGW32__) || defined(__MINGW64__)` branch that uses `fwrite(..., stderr)` instead. The Linux/macOS path is unchanged.
- **`-lbcrypt` added to Makefile** — MinGW requires explicit linkage against `bcrypt.dll` for `BCryptGenRandom`. Added `EXTRA_LIBS = -lbcrypt` under an `ifeq ($(OS),Windows_NT)` guard so the flag is injected automatically on Windows and is a no-op on Linux/macOS. All test targets and the benchmark pass `$(EXTRA_LIBS)` at link time.

### Validation
All 7 tests pass on Windows (MinGW-w64, MSYS2, GCC 15.1):
- `test_basic` ✅
- `test_threads` ✅
- `test_huge` ✅
- `test_string_view` ✅
- `test_usable_size` ✅
- `test_multithread_stress` ✅
- `test_brutal` ✅ — 4,592 threads spawned and destroyed under 5-second torture

Linux and macOS are unaffected — the two fixes are strictly inside MinGW-guarded `#if` blocks, and the `write()`/`STDERR_FILENO` path on POSIX is preserved in the `#else` branch.

### Platform Audit Summary
| Platform | mmap/munmap | Entropy | CPU-local | madvise release | Build | Tests |
|----------|-------------|---------|-----------|-----------------|-------|-------|
| Linux | `mmap`/`munmap` | `SYS_getrandom` | `sched_getcpu()` | `MADV_FREE` | ✅ | ✅ |
| macOS 10.12+ | `mmap`/`munmap` | `getentropy(3)` | round-robin | `MADV_FREE` | ✅ (compile-verified) | pending |
| Windows (MinGW-w64) | `VirtualAlloc`/`VirtualFree` | `BCryptGenRandom` | `GetCurrentProcessorNumber()` | no-op | ✅ | ✅ |

---

## [v1.6.0] - Three-File Split, Versioning, macOS Support & Docs Accuracy
**Release Date:** 2026-06-14

### Library
- **Three-file split** — The monolithic single-header is now organized across three files:
  - `mybuddy.h` — Orchestrator. Includes `mybuddy_api.h` unconditionally, then includes `mybuddy_impl.h` only when `MYBUDDY_IMPLEMENTATION` is defined. This is the only file users include — existing `#include "mybuddy.h"` code requires no changes.
  - `mybuddy_api.h` — Public API only: version macros, configuration flags and `mbd_config_t`, all `mbd_*` function declarations, `mbd_stats_t`, `mbd_event_type_t`. Zero implementation details, zero platform includes. Safe to include anywhere.
  - `mybuddy_impl.h` — Complete implementation: platform includes and shims, all internal types (`block_header_t`, `mbd_arena_t`, `thread_cache_data_t`), all static globals, and every function body. Only compiled in the single TU that defines `MYBUDDY_IMPLEMENTATION`.
- **Version macros** — Added `MYBUDDY_VERSION_MAJOR`, `MYBUDDY_VERSION_MINOR`, `MYBUDDY_VERSION_PATCH`, and `MYBUDDY_VERSION_STRING` preprocessor macros (in `mybuddy_api.h`). The `@version` doc comment remains the human-readable canonical label.
- **macOS: entropy** — `mbd_init_secret_key()` now uses `getentropy(3)` (available since macOS 10.12) on Apple platforms. Previously fell through to the weak `gettimeofday`-based seed on macOS; hardened magic-value scheme is now fully seeded on all three platforms.
- **macOS: `MAP_ANONYMOUS`** — Added `#ifndef MAP_ANONYMOUS / #ifdef MAP_ANON` fallback so `mmap()` calls compile correctly on macOS, which only defines `MAP_ANON`.
- **macOS: CPU-local binding** — `MBD_FLAG_CPU_LOCAL` falls through to the `#else` round-robin arena selection on macOS (no `sched_getcpu()` equivalent without Mach thread policy). This is correct and silent — no crash, no degraded correctness, just no CPU affinity. Full macOS CPU pinning is Phase 3 work.

### Docs
- **SCALING_PLAN.md** — Corrected two stale "mutex-protected remote_free_queue" descriptions to accurately reflect the lock-free CAS implementation (`_Atomic(block_header_t*)` head, `atomic_compare_exchange_weak` push). The plan was written before the final implementation choice was made; the code was always correct, the doc was not.
- **UPDATELOG.md** — This file. First entry. All future patches to MyBuddy will be logged here before integration into Situation or any other embedder.

### Platform Matrix After This Release
| Platform | mmap/munmap | Entropy | CPU-local | madvise release | Status |
|----------|-------------|---------|-----------|-----------------|--------|
| Linux    | `mmap` / `munmap` | `SYS_getrandom` | `sched_getcpu()` | `MADV_FREE` | ✅ Full |
| macOS    | `mmap` / `munmap` | `getentropy(3)` | round-robin (no pinning) | `MADV_FREE` | ✅ Functional |
| Windows (MinGW-w64) | `VirtualAlloc` / `VirtualFree` | `BCryptGenRandom` | `GetCurrentProcessorNumber()` | no-op | ✅ Full |

### Notes
- All correctness issues originally flagged for this release (memory ordering in `coalesce_up_and_update`, `mbd_destroy` safety contract, cache pressure counter unification, MinGW compatibility) were **already resolved in v1.5.x** prior to this entry. They are documented below for historical completeness.

---

## [v1.4.9] - Thread Cache Extended to 1 MiB
*Historical entry — reconstructed from header @note.*

- **`SMALL_ORDER_MAX` raised to 20** — thread cache now covers blocks up to order 20 (1 MiB), up from the previous limit. During `refill_thread_cache`, intermediate-sized buddies produced by splits are placed directly into their respective cache slots rather than returned to the free list. This eliminates cross-order cache thrashing where a refill at one order would evict useful blocks at adjacent orders.

---

## [v1.4.8] - Removed madvise Cascade; Added `mbd_release_to_os()`
*Historical entry — reconstructed from header @note.*

- **Removed automatic `madvise(MADV_DONTNEED)` from `coalesce_up_and_update()`** — previously, every coalesce of a block at order ≥ 21 immediately discarded its physical pages. This caused hard page faults on every subsequent re-allocation of that memory, creating a severe performance bottleneck in workloads that repeatedly allocate and free large blocks. The pool now stays warm — pages remain backed until explicitly released.
- **`mbd_release_to_os()`** — new public API to explicitly return physical pages of free blocks ≥ 2 MiB to the OS via `madvise`. Intended usage: call `mbd_trim()` first to flush caches and coalesce, then call `mbd_release_to_os()` to minimize RSS. Only use when memory pressure reduction matters more than allocation latency.

---

## [v1.4.6] - Granular Cache Configuration via `mbd_config_t`
*Historical entry — reconstructed from header @note.*

- **`mbd_config_t` struct** — introduced a full runtime configuration struct passed to `mbd_init(const mbd_config_t *config)`. Configurable fields: pool size, order limits (`min_order`, `max_order`, `small_order_max`, `large_cutoff_order`), per-order cache limits (`cache_limits[32]`), mmap cache slots, flush watermarks, refill batch size, remote free queue limit, arena migration frequency, hugepage threshold.
- **`MBD_FLAG_HARDENED` disabled by default** — the randomized magic-value scheme (per-block XOR with address hash and secret key) is now opt-in via `MBD_FLAG_HARDENED`. Disabling it removes the encode/decode overhead from every alloc/free hot path, yielding a significant performance improvement in non-adversarial workloads.
- **`MBD_FLAG_DETERMINISTIC`** — new flag that disables adaptive heuristics (pressure-based flushing, migration return frequency) for reproducible benchmarking and debugging.
- **`LARGE_CUTOFF_ORDER`** — new configurable threshold (default: 14, i.e. 16 KiB) separating cache-eligible "small" blocks from "large" blocks that go directly to the global pool. Allows tuning the cache/pool boundary without recompiling.

---

## [v1.4.5] - Official MinGW-w64 / Windows Support
*Historical entry — reconstructed from header @note.*

- **Windows compatibility layer** — `mybuddy_impl.h` (formerly the `MYBUDDY_IMPLEMENTATION` block) now includes a full MinGW-w64 shim:
  - `mmap`/`munmap` mapped to `VirtualAlloc(MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)` / `VirtualFree(MEM_RELEASE)`
  - `sysconf(_SC_PAGESIZE)` and `sysconf(_SC_NPROCESSORS_ONLN)` replaced by `mbd_sysconf()` using `GetSystemInfo()`
  - `MAP_FAILED` defined as `NULL`, POSIX `MAP_*` / `PROT_*` constants stubbed to 0
  - `BCryptGenRandom` (`-lbcrypt`) used for CSPRNG entropy instead of `SYS_getrandom`
  - `madvise` calls (hugepage hints, OS release) are no-ops on Windows — `MBD_MADV_RELEASE` resolves to 0
- **`MBD_FLAG_CPU_LOCAL` on Windows** — uses `GetCurrentProcessorNumber()` for CPU-local arena binding instead of `sched_getcpu()`
- **No MSVC support** — requires MinGW-w64 with winpthreads (`-lpthread`). MSVC lacks C11 `_Atomic`, `__thread`, and POSIX thread primitives.
*These changes were made between v1.3.1 and v1.5.1 before this updatelog existed. Reconstructed from code review.*

- **`block_header_t::order` and `::flags`** converted to `_Atomic(uint8_t)`. All write sites use `atomic_store_explicit` with appropriate ordering; `coalesce_up_and_update()` reads `buddy->order` with `memory_order_acquire`.
- **`mbd_destroy()` safety contract** — replaced `assert(active_threads == 0)` with an unconditional `fprintf(stderr, ...) + abort()` that fires in both debug and release builds.
- **Cache pressure unified** — the dual-counter scheme (`global_cached_bytes` + `global_cache_pressure`) was consolidated into per-arena `cached_bytes` (`_Atomic size_t` inside `mbd_arena_t`). No separate global pressure counter exists.
- **Remote free queue** — implemented as a lock-free LIFO CAS stack (`_Atomic(block_header_t*)` with `atomic_compare_exchange_weak`), not the mutex-protected queue described in SCALING_PLAN's original design notes. ABA risk documented in a comment above `remote_push()`.
- **MinGW/Windows compatibility** — full `VirtualAlloc`/`VirtualFree` shim for `mmap`/`munmap`, `mbd_sysconf()` wrapper for `sysconf`, and `bcrypt.h` include for `BCryptGenRandom`. No MSVC support (requires MinGW-w64).
- **Configuration API** — `mbd_init()` now accepts `const mbd_config_t *config` (pass `NULL` for defaults). Added `MBD_FLAG_*` compile-time option flags and a full `mbd_config_t` struct for runtime tuning (pool size, order limits, cache sizing, flush thresholds).
- **`SMALL_ORDER_MAX` raised to 20** (1 MiB) — thread cache now covers up to 1 MiB blocks, eliminating cross-order cache thrashing for medium-large objects.
- **`LARGE_CUTOFF_ORDER` added** — 16 KiB threshold separating "small" (cache-eligible) from "large" (global pool direct) paths.
- **Bit 3 reserved comment** — `// bit 3: reserved (was BLOCK_IS_SPLIT, removed in v1.4)` added to flag definitions.
- **`mbd_release_to_os()`** — new API to explicitly return physical pages to the OS (replaces the old automatic `madvise(MADV_DONTNEED)` cascade in `coalesce_up_and_update`, which was removed to keep the pool warm).
- **`MBD_FLAG_DETERMINISTIC`** — disables adaptive heuristics for reproducible benchmarking and debugging.
- **`MBD_FLAG_CPU_LOCAL`** — CPU-local arena binding via `sched_getcpu()` (replaces the previously misleading `MBD_FLAG_NUMA_AWARE` name).
- **Per-arena stats** — `splits`, `coalesces`, `cached_bytes`, `active` moved into `mbd_arena_t` to allow per-arena observability.
- **`mbd_destroy()` full reset** — after unmapping all arenas, resets `arenas`, `thread_counter`, `huge_mmap_tracked`, `trim_requested`, `active_threads`, `global_cache_list`, and sets `fully_destroyed = 1`, making the allocator safe to re-initialize in the same process (unit test support).
