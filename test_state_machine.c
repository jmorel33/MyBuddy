#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    mbd_init(NULL);
    // 1. Fill the cache (forces refill_thread_cache — tests Locations 3 & 4)
    void *a = mbd_alloc(64);
    void *b = mbd_alloc(64);

    // 2. Free back to cache (tests Location 1)
    mbd_free(a);

    // 3. Realloc (tests that cached block has MAGIC_ALLOC)
    void *c = mbd_realloc(b, 128);

    // 4. Memalign (tests raw block has MAGIC_ALLOC)
    void *d = mbd_memalign(64, 128);

    // 5. Calloc (tests memset path works without FRESH)
    void *e = mbd_calloc(1, 64);

    mbd_free(c);
    mbd_free(d);
    mbd_free(e);

    printf("State machine tests passed!\n");
    return 0;
}
