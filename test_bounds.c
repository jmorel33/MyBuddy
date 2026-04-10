#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

int main() {
    mbd_init();
    void* p = mbd_alloc(128);
    printf("Usable size: %zu\n", mbd_malloc_usable_size(p));
    p = mbd_realloc(p, 256);
    printf("Usable size: %zu\n", mbd_malloc_usable_size(p));
    mbd_free(p);
    printf("Tests bounds passed.\n");
    return 0;
}
