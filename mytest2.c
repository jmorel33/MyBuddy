#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

void* thread_func(void* arg) {
    (void)arg;
    for (int i = 0; i < 1000; ++i) {
        void* p = mbd_alloc((rand() % 256) + 1);
        p = mbd_realloc(p, (rand() % 512) + 1);
        mbd_free(p);
    }
    return NULL;
}

int main() {
    mbd_init();
    pthread_t threads[32];
    for (int i=0; i<32; ++i) pthread_create(&threads[i], NULL, thread_func, NULL);
    for (int i=0; i<32; ++i) pthread_join(threads[i], NULL);
    mbd_destroy();
    return 0;
}
