#include <stdio.h>
#include <stdint.h>
#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

int main() {
    mbd_init();
    void *p = mbd_alloc(64);
    int valid = 0;
    for (int a = 0; a < arena_count; a++) {
        if ((uintptr_t)p >= (uintptr_t)arenas[a].memory_pool &&
            (uintptr_t)p < (uintptr_t)arenas[a].memory_pool + POOL_SIZE) {
            valid = 1;
            break;
        }
    }
    printf("Is valid? %d\n", valid);
    return 0;
}
