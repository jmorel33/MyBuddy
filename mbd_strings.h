#ifndef MBD_STRINGS_H
#define MBD_STRINGS_H

#include <stddef.h>
#include <string.h>
#include "mybuddy.h"

#ifdef __cplusplus
extern "C" {
#endif

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
static inline mbd_string_view_t mbd_string_view_from_cstr(const char *s) {
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
static inline mbd_string_view_t mbd_string_view_from_data(const char *data, size_t len) {
    return (mbd_string_view_t){data, len};
}

/**
 * @brief Allocate a null-terminated C string copy from a string view
 *        (uses mbd_alloc internally). Useful for classic C string.
 *
 * @param view The string view to duplicate.
 * @return char* A newly allocated, null-terminated C string, or NULL on failure.
 */
static inline char *mbd_string_view_dup(mbd_string_view_t view) {
    if (!view.data || view.len == 0) return NULL;

    char *copy = (char *)mbd_alloc(view.len + 1);
    if (copy) {
        memcpy(copy, view.data, view.len);
        copy[view.len] = '\0';
    }
    return copy;
}

#ifdef __cplusplus
}
#endif

#endif /* MBD_STRINGS_H */
