#ifndef MBD_STRINGS_H
#define MBD_STRINGS_H

#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
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

/**
 * @brief Duplicate a null-terminated C string using mbd_alloc.
 *
 * @param s The string to duplicate.
 * @return char* A newly allocated string, or NULL on failure.
 */
static inline char *mbd_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)mbd_alloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

/**
 * @brief Duplicate up to n characters of a C string using mbd_alloc.
 *
 * @param s The string to duplicate.
 * @param n Maximum number of characters to copy.
 * @return char* A newly allocated, null-terminated string, or NULL on failure.
 */
static inline char *mbd_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len] != '\0') {
        len++;
    }
    char *copy = (char *)mbd_alloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

/**
 * @brief Allocate a string formatted according to the format string.
 *
 * @param fmt The format string.
 * @param ... The arguments for the format string.
 * @return char* A newly allocated, formatted string, or NULL on failure.
 */
static inline char *mbd_asprintf(const char *fmt, ...) {
    if (!fmt) return NULL;
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);

    // Determine required length
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) {
        va_end(args);
        return NULL;
    }

    char *str = (char *)mbd_alloc((size_t)len + 1);
    if (str) {
        vsnprintf(str, (size_t)len + 1, fmt, args);
    }
    va_end(args);
    return str;
}

/* ── Arena-backed string builder ─────────────────────────────────────────── */
typedef struct mbd_string {
    char *data;
    size_t len;
    size_t capacity;
    struct mbd_arena *arena;   // for future migration awareness
} mbd_string_t;

/**
 * @brief Initialize a new arena-backed string builder.
 *
 * @param initial_capacity The starting capacity in bytes.
 * @return mbd_string_t The initialized string builder.
 */
static inline mbd_string_t mbd_string_new(size_t initial_capacity) {
    mbd_string_t s;
    s.len = 0;
    s.capacity = initial_capacity > 0 ? initial_capacity : 16;
    s.data = (char *)mbd_alloc(s.capacity);
    if (s.data) {
        s.data[0] = '\0';
    } else {
        s.capacity = 0;
    }
    s.arena = NULL; // Can be linked later if needed
    return s;
}

/**
 * @brief Append a null-terminated C string to the string builder.
 *
 * @param s The string builder.
 * @param append The string to append.
 */
static inline void mbd_string_append(mbd_string_t *s, const char *append) {
    if (!s || !s->data || !append) return;

    size_t append_len = strlen(append);
    if (append_len == 0) return;

    size_t new_len = s->len + append_len;
    if (new_len + 1 > s->capacity) {
        size_t new_capacity = s->capacity;
        while (new_capacity < new_len + 1) {
            new_capacity *= 2;
        }
        char *new_data = (char *)mbd_realloc(s->data, new_capacity);
        if (!new_data) return; // Allocation failed
        s->data = new_data;
        s->capacity = new_capacity;
    }

    memcpy(s->data + s->len, append, append_len);
    s->len = new_len;
    s->data[s->len] = '\0';
}

/**
 * @brief Append a formatted string to the string builder.
 *
 * @param s The string builder.
 * @param fmt The format string.
 * @param ... The arguments for the format string.
 */
static inline void mbd_string_appendf(mbd_string_t *s, const char *fmt, ...) {
    if (!s || !s->data || !fmt) return;

    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);

    int append_len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (append_len <= 0) {
        va_end(args);
        return;
    }

    size_t new_len = s->len + (size_t)append_len;
    if (new_len + 1 > s->capacity) {
        size_t new_capacity = s->capacity;
        while (new_capacity < new_len + 1) {
            new_capacity *= 2;
        }
        char *new_data = (char *)mbd_realloc(s->data, new_capacity);
        if (!new_data) {
            va_end(args);
            return;
        }
        s->data = new_data;
        s->capacity = new_capacity;
    }

    vsnprintf(s->data + s->len, (size_t)append_len + 1, fmt, args);
    s->len = new_len;
    va_end(args);
}

/**
 * @brief Free the string builder data.
 *
 * @param s The string builder to free.
 */
static inline void mbd_string_free(mbd_string_t *s) {
    if (s && s->data) {
        mbd_free(s->data);
        s->data = NULL;
        s->len = 0;
        s->capacity = 0;
    }
}

/* ── Table Utilities ──────────────────────────────────────────────── */
typedef struct mbd_table_entry {
    char *key;
    void *value;
    struct mbd_table_entry *next;
} mbd_table_entry_t;

typedef struct mbd_table {
    mbd_table_entry_t **buckets;
    size_t capacity;
    size_t size;

    void **array;
    size_t array_capacity;
} mbd_table_t;

/**
 * @brief Simple hash function for string keys.
 */
static inline size_t mbd_table_hash(const char *key) {
    size_t hash = 5381;
    int c;
    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

/**
 * @brief Initialize a new hybrid array+hash table.
 *
 * @param initial_capacity The starting capacity for the hash part.
 * @return mbd_table_t* The initialized table, or NULL on failure.
 */
static inline mbd_table_t *mbd_table_new(size_t initial_capacity) {
    mbd_table_t *t = (mbd_table_t *)mbd_alloc(sizeof(mbd_table_t));
    if (!t) return NULL;

    t->capacity = initial_capacity > 0 ? initial_capacity : 16;
    t->size = 0;
    t->buckets = (mbd_table_entry_t **)mbd_calloc(t->capacity, sizeof(mbd_table_entry_t *));
    if (!t->buckets) {
        mbd_free(t);
        return NULL;
    }

    t->array_capacity = 0;
    t->array = NULL;

    return t;
}

/**
 * @brief Free the table and all its string keys. Note: values are NOT freed.
 *
 * @param t The table to free.
 */
static inline void mbd_table_free(mbd_table_t *t) {
    if (!t) return;

    if (t->buckets) {
        for (size_t i = 0; i < t->capacity; i++) {
            mbd_table_entry_t *entry = t->buckets[i];
            while (entry) {
                mbd_table_entry_t *next = entry->next;
                if (entry->key) {
                    mbd_free(entry->key);
                }
                mbd_free(entry);
                entry = next;
            }
        }
        mbd_free(t->buckets);
    }

    if (t->array) {
        mbd_free(t->array);
    }

    mbd_free(t);
}

/**
 * @brief Resize the hash table when load factor is high.
 */
static inline void mbd_table_resize(mbd_table_t *t) {
    size_t new_capacity = t->capacity * 2;
    mbd_table_entry_t **new_buckets = (mbd_table_entry_t **)mbd_calloc(new_capacity, sizeof(mbd_table_entry_t *));
    if (!new_buckets) return;

    for (size_t i = 0; i < t->capacity; i++) {
        mbd_table_entry_t *entry = t->buckets[i];
        while (entry) {
            mbd_table_entry_t *next = entry->next;
            size_t new_index = mbd_table_hash(entry->key) % new_capacity;
            entry->next = new_buckets[new_index];
            new_buckets[new_index] = entry;
            entry = next;
        }
    }

    mbd_free(t->buckets);
    t->buckets = new_buckets;
    t->capacity = new_capacity;
}

/**
 * @brief Insert or update a string key-value pair.
 *
 * @param t The table.
 * @param key The string key (it will be copied).
 * @param value The value.
 */
static inline void mbd_table_insert(mbd_table_t *t, const char *key, void *value) {
    if (!t || !key) return;

    if (t->size >= t->capacity * 0.75) {
        mbd_table_resize(t);
    }

    size_t index = mbd_table_hash(key) % t->capacity;
    mbd_table_entry_t *entry = t->buckets[index];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            entry->value = value;
            return;
        }
        entry = entry->next;
    }

    entry = (mbd_table_entry_t *)mbd_alloc(sizeof(mbd_table_entry_t));
    if (!entry) return;

    entry->key = mbd_strdup(key);
    if (!entry->key) {
        mbd_free(entry);
        return;
    }
    entry->value = value;
    entry->next = t->buckets[index];
    t->buckets[index] = entry;
    t->size++;
}

/**
 * @brief Get a value by string key.
 *
 * @param t The table.
 * @param key The string key.
 * @return void* The value, or NULL if not found.
 */
static inline void *mbd_table_get(mbd_table_t *t, const char *key) {
    if (!t || !key) return NULL;

    size_t index = mbd_table_hash(key) % t->capacity;
    mbd_table_entry_t *entry = t->buckets[index];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return entry->value;
        }
        entry = entry->next;
    }

    return NULL;
}

/**
 * @brief Remove a value by string key.
 *
 * @param t The table.
 * @param key The string key.
 */
static inline void mbd_table_remove(mbd_table_t *t, const char *key) {
    if (!t || !key) return;

    size_t index = mbd_table_hash(key) % t->capacity;
    mbd_table_entry_t *entry = t->buckets[index];
    mbd_table_entry_t *prev = NULL;

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            if (prev) {
                prev->next = entry->next;
            } else {
                t->buckets[index] = entry->next;
            }
            mbd_free(entry->key);
            mbd_free(entry);
            t->size--;
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

/**
 * @brief Set an array element at a specific index.
 *
 * @param t The table.
 * @param index The integer index.
 * @param value The value.
 */
static inline void mbd_table_seti(mbd_table_t *t, uint32_t index, void *value) {
    if (!t) return;

    if (index >= t->array_capacity) {
        size_t new_capacity = index >= (t->array_capacity * 2) ? (index + 1) : (t->array_capacity * 2);
        if (new_capacity < 8) new_capacity = 8;

        void **new_array = (void **)mbd_realloc(t->array, new_capacity * sizeof(void *));
        if (!new_array) return;

        // Zero the new part
        memset(new_array + t->array_capacity, 0, (new_capacity - t->array_capacity) * sizeof(void *));

        t->array = new_array;
        t->array_capacity = new_capacity;
    }

    t->array[index] = value;
}

/**
 * @brief Get an array element at a specific index.
 *
 * @param t The table.
 * @param index The integer index.
 * @return void* The value, or NULL if out of bounds or not set.
 */
static inline void *mbd_table_geti(mbd_table_t *t, uint32_t index) {
    if (!t || index >= t->array_capacity) return NULL;
    return t->array[index];
}

#ifdef __cplusplus
}
#endif

#endif /* MBD_STRINGS_H */
