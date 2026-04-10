#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"

#define NUM_THREADS 8
#define ALLOCS_PER_THREAD 1000

void* thread_func(void* arg) {
    void* ptrs[ALLOCS_PER_THREAD];
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        ptrs[i] = mbd_alloc(128 + (i % 512));
        assert(ptrs[i] != NULL);
    }
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        mbd_free(ptrs[i]);
    }
    return NULL;
}

int main() {
    mbd_init();
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, thread_func, NULL);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("Tests passed.\n");
    return 0;
}
