#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#define NUM_THREADS 32
#define ALLOCS_PER_THREAD 1000

void *stress_func(void *arg) {
    (void)arg;
    void *ptrs[ALLOCS_PER_THREAD];

    // Allocate random small sizes
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        ptrs[i] = mbd_alloc((rand() % 256) + 1);
        assert(ptrs[i] != NULL);
    }

    // Randomly realloc half of them
    for (int i = 0; i < ALLOCS_PER_THREAD; i+=2) {
        ptrs[i] = mbd_realloc(ptrs[i], (rand() % 512) + 1);
        assert(ptrs[i] != NULL);
    }

    // Free everything
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        mbd_free(ptrs[i]);
    }
    return NULL;
}

int main() {
    mbd_init(NULL);

    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, stress_func, NULL);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    mbd_destroy();
    printf("Multi-thread stress test passed!\n");
    return 0;
}
