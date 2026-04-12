#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

int main() {
    mbd_init();
    void* p = mbd_alloc(10);
    mbd_free(p);
    mbd_destroy();
    return 0;
}
