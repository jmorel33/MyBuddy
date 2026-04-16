#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main() {
    mbd_init(NULL);

    // Basic allocation and free
    void *ptr1 = mbd_alloc(100);
    assert(ptr1 != NULL);

    // Write to memory to ensure it's accessible
    memset(ptr1, 0xAA, 100);

    // Realloc
    void *ptr2 = mbd_realloc(ptr1, 200);
    assert(ptr2 != NULL);
    // Check if original data was preserved
    unsigned char *cptr = (unsigned char *)ptr2;
    for(int i=0; i<100; i++) {
        assert(cptr[i] == 0xAA);
    }

    // Calloc
    void *ptr3 = mbd_calloc(10, 10);
    assert(ptr3 != NULL);
    cptr = (unsigned char *)ptr3;
    for(int i=0; i<100; i++) {
        assert(cptr[i] == 0);
    }

    // Memalign
    void *ptr4 = mbd_memalign(64, 1024);
    assert(ptr4 != NULL);
    assert(((size_t)ptr4 % 64) == 0);

    mbd_free(ptr2);
    mbd_free(ptr3);
    mbd_free(ptr4);

    mbd_destroy();
    printf("Basic tests passed!\n");
    return 0;
}
