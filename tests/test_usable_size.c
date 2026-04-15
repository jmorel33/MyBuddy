#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <assert.h>

int main() {
    mbd_init(NULL);

    void *ptr = mbd_alloc(10);
    size_t usable = mbd_malloc_usable_size(ptr);
    assert(usable >= 10);

    mbd_free(ptr);
    mbd_destroy();
    printf("Usable size tests passed!\n");
    return 0;
}
