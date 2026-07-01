/**
 * @file mybuddy.h
 * @brief High-Performance Thread-Caching Buddy Allocator — build orchestrator.
 *
 * @version 1.6.2
 * @date June 14, 2026
 * @author Jacques Morel
 *
 * @note v1.6.0 splits the library into three files:
 *         mybuddy.h      — this file; controls the build and pulls in the other two.
 *         mybuddy_api.h  — public API: types, configuration macros, function declarations.
 *         mybuddy_impl.h — full implementation: internal types, globals, all function bodies.
 *
 *       Include only this file in your project. Define MYBUDDY_IMPLEMENTATION in
 *       exactly one translation unit before the include to instantiate the implementation:
 *
 *       @code
 *       // In one .c file:
 *       #define MYBUDDY_IMPLEMENTATION
 *       #include "mybuddy.h"
 *
 *       // In all other files:
 *       #include "mybuddy.h"
 *       @endcode
 *
 *       See UPDATELOG.md for the full version history.
 *
 * @section overview Overview
 * MyBuddy (MBd) is a production-grade, highly concurrent memory allocator combining
 * the anti-fragmentation guarantees of a classic Buddy Allocator with the lock-free
 * speed of per-thread caching.
 *
 * @section features Key Strengths
 * - **Crazy Fast**: Lock-free thread-local cache delivers allocations up to 1 MiB in just a few CPU cycles.
 * - **Fully Thread-Safe**: True per-thread caching; global locks acquired only on cache misses or large blocks.
 * - **Hardened & Safe**: Double-free protection, bounds checking, checksummed magic-value validation.
 * - **Memory Efficient**: MAP_NORESERVE — virtual memory only backed by physical RAM when used.
 * - **Advanced Alignment**: Guaranteed 32-byte minimum; mbd_memalign() for stricter requirements.
 * - **Huge Allocations**: Requests > pool_size bypass the buddy pool via direct mmap()/munmap().
 * - **Production Readiness**: LD_PRELOAD-safe, self-initializing, atomic stats, OOM hooks.
 *
 * @section platform Platform Support
 * | Platform            | Memory        | Entropy          | CPU-local     | madvise release |
 * |---------------------|---------------|------------------|---------------|-----------------|
 * | Linux               | mmap/munmap   | SYS_getrandom    | sched_getcpu  | MADV_FREE       |
 * | macOS 10.12+        | mmap/munmap   | getentropy(3)    | round-robin   | MADV_FREE       |
 * | Windows (MinGW-w64) | VirtualAlloc  | BCryptGenRandom  | GetCurrentProcessorNumber | no-op |
 *
 * @note MSVC is not supported. MinGW-w64 (-lpthread -lbcrypt) required on Windows.
 */
#ifndef MYBUDDY_H
#define MYBUDDY_H

// Version macros available first — even before the full API or implementation
#include "mybuddy_version.h"
#include "mybuddy_api.h"

#ifdef MYBUDDY_IMPLEMENTATION
#include "mybuddy_impl.h"
#endif /* MYBUDDY_IMPLEMENTATION */

#endif /* MYBUDDY_H */
