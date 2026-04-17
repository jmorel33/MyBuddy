#define MYBUDDY_IMPLEMENTATION
#include "mybuddy.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main() {
    mbd_init(NULL);
    int iters = 1000000 / 100;
    void **ptrs = malloc(iters * sizeof(void *));
    size_t *sizes = malloc(iters * sizeof(size_t));
    unsigned int seed = 42;
    for (int i = 0; i < iters; i++) {
        sizes[i] = (rand_r(&seed) % (128 * 1024)) + 4096;
    }

    // Warm up phase 1
    void **warm = malloc(1000000 * sizeof(void *));
    for (int i = 0; i < 1000000; i++) warm[i] = mbd_alloc(64);
    for (int i = 0; i < 1000000; i++) mbd_free(warm[i]);
    free(warm);

    double start = get_time_ms();
    for (int i = 0; i < iters; i++) ptrs[i] = mbd_alloc(sizes[i]);
    for (int i = 0; i < iters; i++) mbd_free(ptrs[i]);
    double time_ms = get_time_ms() - start;

    printf("Phase 2 after Phase 1: %.2f ms\n", time_ms);
    free(ptrs);
    free(sizes);
    return 0;
}
