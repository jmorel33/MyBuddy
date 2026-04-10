#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

int main() {
    mbd_init();
    void* p = mbd_alloc(128);
    mbd_free(p);
    printf("Tests passed.\n");
    return 0;
}
