#include <stdio.h>
#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

int main() {
    mbd_init();
    void *p = mbd_alloc(10);
    size_t sz = mbd_malloc_usable_size(p);
    printf("Usable size: %zu\n", sz);
    return 0;
}
