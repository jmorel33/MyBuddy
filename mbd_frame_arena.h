#ifndef MBD_FRAME_ARENA_H
#define MBD_FRAME_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include "mybuddy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *base;          // Single mbd_alloc for the whole frame
    size_t capacity;     // Total bytes allocated
    size_t offset;       // Current bump pointer
} mbd_frame_arena_t;

// Allocate the arena for this frame
static inline void mbd_frame_arena_init(mbd_frame_arena_t *fa, size_t capacity) {
    fa->capacity = capacity;
    fa->offset = 0;
    if (capacity > 0) {
        fa->base = mbd_alloc(capacity);
    } else {
        fa->base = NULL;
    }
}

// Bump-allocate within the frame (zero overhead, no bookkeeping)
static inline void *mbd_frame_alloc(mbd_frame_arena_t *fa, size_t size, size_t align) {
    if (!fa || !fa->base || size == 0 || align == 0) return NULL;

    uintptr_t current_ptr = (uintptr_t)fa->base + fa->offset;
    uintptr_t offset = current_ptr % align;
    uintptr_t adjustment = (offset == 0) ? 0 : (align - offset);

    // Check for overflow/capacity.
    // fa->offset + adjustment + size must be <= fa->capacity.
    // We rewrite to avoid integer overflow when size is huge:
    if (fa->capacity - fa->offset < adjustment) return NULL;
    if (fa->capacity - fa->offset - adjustment < size) return NULL;

    fa->offset += adjustment + size;
    return (void *)(current_ptr + adjustment);
}

// Reset for next frame (no per-block free, just reset the offset)
static inline void mbd_frame_arena_reset(mbd_frame_arena_t *fa) {
    if (fa) {
        fa->offset = 0;
    }
}

// Release the backing memory
static inline void mbd_frame_arena_destroy(mbd_frame_arena_t *fa) {
    if (fa && fa->base) {
        mbd_free(fa->base);
        fa->base = NULL;
        fa->capacity = 0;
        fa->offset = 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif // MBD_FRAME_ARENA_H
