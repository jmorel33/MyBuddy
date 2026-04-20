#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define NUM_CONCURRENT_THREADS 16
#define TEST_DURATION_SECONDS 5

// A shared pool to test cross-thread frees.
#define SHARED_POOL_SIZE 1024
void *shared_pool[SHARED_POOL_SIZE];
pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global flag to stop the threads
volatile int keep_running = 1;

// Returns a random size representing thrashing behavior
size_t rand_size() {
    int r = rand() % 100;
    if (r < 50) return (rand() % 256) + 1; // Small
    if (r < 80) return (rand() % 4096) + 1; // Medium
    if (r < 95) return (rand() % (1024 * 1024)) + 1; // Large
    return (rand() % (4 * 1024 * 1024)) + 1; // Huge
}

void *churn_worker(void *arg) {
    (void)arg;
    void *local_ptrs[64] = {0};

    while (keep_running) {
        // Randomly pick an index to operate on
        int idx = rand() % 64;

        // Free if it exists
        if (local_ptrs[idx]) {
            mbd_free(local_ptrs[idx]);
            local_ptrs[idx] = NULL;
        }

        // Allocate a new block
        size_t sz = rand_size();
        local_ptrs[idx] = mbd_alloc(sz);
        assert(local_ptrs[idx] != NULL);
        // Write to memory to ensure it's mapped
        memset(local_ptrs[idx], 0xAB, sz > 128 ? 128 : sz);

        // Periodically Realloc Torture
        if (rand() % 10 == 0) {
            size_t new_sz = rand_size();
            local_ptrs[idx] = mbd_realloc(local_ptrs[idx], new_sz);
            assert(local_ptrs[idx] != NULL);
            memset(local_ptrs[idx], 0xCD, new_sz > 128 ? 128 : new_sz);
        }

        // Periodically try to push to or pop from shared pool (Cross-thread chaos)
        if (rand() % 20 == 0) {
            pthread_mutex_lock(&pool_mutex);
            int pool_idx = rand() % SHARED_POOL_SIZE;
            if (shared_pool[pool_idx] == NULL && local_ptrs[idx] != NULL) {
                // Push our pointer to shared pool for someone else to free
                shared_pool[pool_idx] = local_ptrs[idx];
                local_ptrs[idx] = NULL;
            } else if (shared_pool[pool_idx] != NULL) {
                // Take a pointer from shared pool and free it locally
                void *p = shared_pool[pool_idx];
                shared_pool[pool_idx] = NULL;
                pthread_mutex_unlock(&pool_mutex);
                mbd_free(p);
                continue;
            }
            pthread_mutex_unlock(&pool_mutex);
        }

        // Forced trim abuse
        if (rand() % 1000 == 0) {
            mbd_trim();
        }
    }

    // Clean up local pointers
    for (int i = 0; i < 64; i++) {
        if (local_ptrs[i]) {
            mbd_free(local_ptrs[i]);
        }
    }

    return NULL;
}

int main() {
    mbd_init(NULL);
    srand(time(NULL));

    memset(shared_pool, 0, sizeof(shared_pool));

    printf("Starting brutal test for %d seconds...\n", TEST_DURATION_SECONDS);
    time_t start_time = time(NULL);

    long long threads_spawned = 0;
    while (time(NULL) - start_time < TEST_DURATION_SECONDS) {
        pthread_t threads[NUM_CONCURRENT_THREADS];

        // Spawn a batch
        for (int i = 0; i < NUM_CONCURRENT_THREADS; i++) {
            pthread_create(&threads[i], NULL, churn_worker, NULL);
            threads_spawned++;
        }

        // Let them churn for a fraction of a second to stress thread teardown vs running
        usleep(10000); // 10ms

        // Signal stop for this batch to test thread churn/teardown (Thread churn torture test)
        keep_running = 0;

        for (int i = 0; i < NUM_CONCURRENT_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        keep_running = 1; // Reset for next batch
    }

    printf("Spawned and destroyed %lld threads during torture test.\n", threads_spawned);

    // Clean up any remaining pointers in the shared pool
    for (int i = 0; i < SHARED_POOL_SIZE; i++) {
        if (shared_pool[i]) {
            mbd_free(shared_pool[i]);
            shared_pool[i] = NULL;
        }
    }

    mbd_destroy();
    printf("Brutal torture test passed!\n");
    return 0;
}
