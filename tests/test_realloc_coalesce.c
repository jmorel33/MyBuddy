#include <stdio.h>
#include <assert.h>
#include <string.h>
#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"

int main() {
    mbd_init();

    // Allocate 3 consecutive blocks of same size
    // Wait, buddy allocator might return them differently.
    // Let's allocate an array of blocks to fill up and then free some to create known buddies.
    void* p1 = mbd_alloc(64);
    void* p2 = mbd_alloc(64);
    void* p3 = mbd_alloc(64);
    void* p4 = mbd_alloc(64);

    printf("p1=%p p2=%p p3=%p p4=%p\n", p1, p2, p3, p4);

    // We want p1 and p2 to be buddies.
    mbd_free(p2); // p2 is now free

    // If we realloc p1 to 128 (payload + header means it might need order 8 depending on MIN_ORDER)
    // Wait, with MIN_ORDER 6 (64), block size is 64. Payload is 64 - 32 = 32.
    // So mbd_alloc(64) actually requests 64 + 32 = 96 -> order 7 (128 bytes).

    void* p1_new = mbd_realloc(p1, 100);
    printf("p1_new=%p\n", p1_new);
    if (p1_new == p1) {
        printf("In-place coalesce worked!\n");
    } else {
        printf("Did not coalesce in place\n");
    }

    return 0;
}
