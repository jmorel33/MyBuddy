#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define ITERATIONS 1000000
#define NUM_THREADS 4
#define MIX_ITERATIONS 500000
#define MAX_MIX_SIZE 16384 // Max size to randomize up to in the mix test

// Forward declarations of benchmark functions
// To be implemented in next steps

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

// Single-thread small allocations (glibc vs MyBuddy)
void st_small_allocs_glibc(double *time_ms) {
    void **ptrs = malloc(ITERATIONS * sizeof(void *));
    double start = get_time_ms();
    for (int i = 0; i < ITERATIONS; i++) ptrs[i] = malloc(64);
    for (int i = 0; i < ITERATIONS; i++) free(ptrs[i]);
    *time_ms = get_time_ms() - start;
    free(ptrs);
}

void st_small_allocs_mbd(double *time_ms) {
    void **ptrs = malloc(ITERATIONS * sizeof(void *));
    double start = get_time_ms();
    for (int i = 0; i < ITERATIONS; i++) ptrs[i] = mbd_alloc(64);
    for (int i = 0; i < ITERATIONS; i++) mbd_free(ptrs[i]);
    *time_ms = get_time_ms() - start;
    free(ptrs);
}

// Single-thread large allocations
void st_large_allocs_glibc(double *time_ms) {
    int iters = ITERATIONS / 100;
    void **ptrs = malloc(iters * sizeof(void *));
    size_t *sizes = malloc(iters * sizeof(size_t));
    unsigned int seed = 42;
    for (int i = 0; i < iters; i++) {
        sizes[i] = (rand_r(&seed) % (128 * 1024)) + 4096; // Random sizes between 4KB and ~132KB
    }
    double start = get_time_ms();
    for (int i = 0; i < iters; i++) ptrs[i] = malloc(sizes[i]);
    for (int i = 0; i < iters; i++) free(ptrs[i]);
    *time_ms = get_time_ms() - start;
    free(ptrs);
    free(sizes);
}

void st_large_allocs_mbd(double *time_ms) {
    int iters = ITERATIONS / 100;
    void **ptrs = malloc(iters * sizeof(void *));
    size_t *sizes = malloc(iters * sizeof(size_t));
    unsigned int seed = 42;
    for (int i = 0; i < iters; i++) {
        sizes[i] = (rand_r(&seed) % (128 * 1024)) + 4096; // Random sizes between 4KB and ~132KB
    }
    double start = get_time_ms();
    for (int i = 0; i < iters; i++) ptrs[i] = mbd_alloc(sizes[i]);
    for (int i = 0; i < iters; i++) mbd_free(ptrs[i]);
    *time_ms = get_time_ms() - start;
    free(ptrs);
    free(sizes);
}

// Multi-thread small allocations
typedef struct {
    int is_mbd;
} ThreadData;

void *mt_small_worker(void *arg) {
    ThreadData *td = (ThreadData *)arg;
    int iters = ITERATIONS / NUM_THREADS;
    void **ptrs = malloc(iters * sizeof(void *));

    if (td->is_mbd) {
        for (int i = 0; i < iters; i++) ptrs[i] = mbd_alloc(64);
        for (int i = 0; i < iters; i++) mbd_free(ptrs[i]);
    } else {
        for (int i = 0; i < iters; i++) ptrs[i] = malloc(64);
        for (int i = 0; i < iters; i++) free(ptrs[i]);
    }

    free(ptrs);
    return NULL;
}

void mt_small_allocs_glibc(double *time_ms) {
    pthread_t threads[NUM_THREADS];
    ThreadData td = {0};
    double start = get_time_ms();
    for (int i = 0; i < NUM_THREADS; i++) pthread_create(&threads[i], NULL, mt_small_worker, &td);
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
    *time_ms = get_time_ms() - start;
}

void mt_small_allocs_mbd(double *time_ms) {
    pthread_t threads[NUM_THREADS];
    ThreadData td = {1};
    double start = get_time_ms();
    for (int i = 0; i < NUM_THREADS; i++) pthread_create(&threads[i], NULL, mt_small_worker, &td);
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
    *time_ms = get_time_ms() - start;
}

// Multi-thread Mix test
void *mt_mix_worker(void *arg) {
    ThreadData *td = (ThreadData *)arg;
    int iters = MIX_ITERATIONS / NUM_THREADS;
    void **ptrs = malloc(iters * sizeof(void *));
    size_t *sizes = malloc(iters * sizeof(size_t));
    int *actions = malloc(iters * sizeof(int));

    unsigned int seed = (unsigned int)(uintptr_t)pthread_self();
    for (int i = 0; i < iters; i++) {
        sizes[i] = (rand_r(&seed) % MAX_MIX_SIZE) + 1;
        actions[i] = rand_r(&seed) % 100;
        ptrs[i] = NULL;
    }

    if (td->is_mbd) {
        for (int i = 0; i < iters; i++) {
            if (actions[i] < 45) { // Alloc
                ptrs[i] = mbd_alloc(sizes[i]);
            } else if (actions[i] < 90) { // Free
                int target = rand_r(&seed) % (i + 1);
                if (ptrs[target]) {
                    mbd_free(ptrs[target]);
                    ptrs[target] = NULL;
                }
            } else if (actions[i] < 95) { // Realloc
                int target = rand_r(&seed) % (i + 1);
                size_t new_size = (rand_r(&seed) % MAX_MIX_SIZE) + 1;
                ptrs[target] = mbd_realloc(ptrs[target], new_size);
            } else { // Calloc
                ptrs[i] = mbd_calloc(1, sizes[i]);
            }
        }
        for (int i = 0; i < iters; i++) {
            if (ptrs[i]) mbd_free(ptrs[i]);
        }
    } else {
        for (int i = 0; i < iters; i++) {
            if (actions[i] < 45) { // Alloc
                ptrs[i] = malloc(sizes[i]);
            } else if (actions[i] < 90) { // Free
                int target = rand_r(&seed) % (i + 1);
                if (ptrs[target]) {
                    free(ptrs[target]);
                    ptrs[target] = NULL;
                }
            } else if (actions[i] < 95) { // Realloc
                int target = rand_r(&seed) % (i + 1);
                size_t new_size = (rand_r(&seed) % MAX_MIX_SIZE) + 1;
                ptrs[target] = realloc(ptrs[target], new_size);
            } else { // Calloc
                ptrs[i] = calloc(1, sizes[i]);
            }
        }
        for (int i = 0; i < iters; i++) {
            if (ptrs[i]) free(ptrs[i]);
        }
    }

    free(ptrs);
    free(sizes);
    free(actions);
    return NULL;
}

void mt_mix_glibc(double *time_ms) {
    pthread_t threads[NUM_THREADS];
    ThreadData td = {0};
    double start = get_time_ms();
    for (int i = 0; i < NUM_THREADS; i++) pthread_create(&threads[i], NULL, mt_mix_worker, &td);
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
    *time_ms = get_time_ms() - start;
}

void mt_mix_mbd(double *time_ms) {
    pthread_t threads[NUM_THREADS];
    ThreadData td = {1};
    double start = get_time_ms();
    for (int i = 0; i < NUM_THREADS; i++) pthread_create(&threads[i], NULL, mt_mix_worker, &td);
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
    *time_ms = get_time_ms() - start;
}

int main() {
    printf("=========================================\n");
    printf("         MyBuddy Benchmark Tool          \n");
    printf("=========================================\n");
    printf("Iterations: %d\n", ITERATIONS);
    printf("Threads: %d\n", NUM_THREADS);
    printf("Mix Iterations: %d\n", MIX_ITERATIONS);
    printf("Max Mix Size: %d\n", MAX_MIX_SIZE);
    printf("=========================================\n");

    mbd_config_t cfg = {0};
    cfg.flags = 0; // Ensure HARDENED and ATOMIC_STATS are OFF
    cfg.min_order = 6;
    cfg.small_order_max = 24; // Cache up to 16 MiB blocks!
    cfg.large_cutoff_order = 24;
    cfg.refill_batch_size = 32; // Grab 32 blocks at a time to avoid locks
    cfg.flush_low_watermark_pct = 20; // Keep caches very warm
    cfg.flush_high_watermark_pct = 95;
    cfg.pool_size = 1ULL * 1024 * 1024 * 1024; // 1 GB pool size
    mbd_init(&cfg);

    double glibc_time, mbd_time;

    // Phase 1: Single-thread Small Allocations
    printf("\n[1] Single-Thread Small Allocs (64B)\n");
    st_small_allocs_glibc(&glibc_time);
    printf("    glibc:   %8.2f ms\n", glibc_time);
    st_small_allocs_mbd(&mbd_time);
    printf("    MyBuddy: %8.2f ms\n", mbd_time);
    printf("    Speedup: %8.2fx\n", glibc_time / mbd_time);

    // Phase 2: Single-thread Large Allocations
    printf("\n[2] Single-Thread Large Allocs (Random 4KB-132KB)\n");
    st_large_allocs_glibc(&glibc_time);
    printf("    glibc:   %8.2f ms\n", glibc_time);
    st_large_allocs_mbd(&mbd_time);
    printf("    MyBuddy: %8.2f ms\n", mbd_time);
    printf("    Speedup: %8.2fx\n", glibc_time / mbd_time);

    // Phase 3: Multi-thread Small Allocations
    printf("\n[3] Multi-Thread Small Allocs (64B, %d Threads)\n", NUM_THREADS);
    mt_small_allocs_glibc(&glibc_time);
    printf("    glibc:   %8.2f ms\n", glibc_time);
    mt_small_allocs_mbd(&mbd_time);
    printf("    MyBuddy: %8.2f ms\n", mbd_time);
    printf("    Speedup: %8.2fx\n", glibc_time / mbd_time);

    // Phase 4: Multi-thread Mix Test
    printf("\n[4] Multi-Thread Mix Test (Alloc/Free/Realloc/Calloc, %d Threads)\n", NUM_THREADS);
    mt_mix_glibc(&glibc_time);
    printf("    glibc:   %8.2f ms\n", glibc_time);
    mt_mix_mbd(&mbd_time);
    printf("    MyBuddy: %8.2f ms\n", mbd_time);
    double mix_speedup = glibc_time / mbd_time;
    printf("    Speedup: %8.2fx\n", mix_speedup);

    printf("\n=========================================\n");
    printf("FINAL SCORE (Mix Test Speedup): %.2fx\n", mix_speedup);
    printf("=========================================\n");

    mbd_destroy();
    return 0;
}
