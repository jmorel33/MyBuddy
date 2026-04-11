#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>

#define NUM_THREADS 4
#define ALLOCS_PER_THREAD 1000

void *thread_func(void *arg) {
    (void)arg;
    void *ptrs[ALLOCS_PER_THREAD];

    // Allocate
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        ptrs[i] = mbd_alloc((rand() % 1024) + 1);
        assert(ptrs[i] != NULL);
    }

    // Free
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

    mbd_destroy();
    printf("Thread tests passed!\n");
    return 0;
}
