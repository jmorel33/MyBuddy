#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main() {
    mbd_init();

    // Huge allocation (bypass buddy pool)
    size_t huge_size = 1024 * 1024 * 128 + 1024; // > 128 MiB
    void *ptr = mbd_alloc(huge_size);
    assert(ptr != NULL);

    // Test write access
    memset(ptr, 0xBB, 100);

    // Check stats to ensure mmap happened
    mbd_stats_t stats = mbd_get_stats();
    assert(stats.total_allocated_bytes >= huge_size);

    mbd_free(ptr);

    mbd_destroy();
    printf("Huge allocation test passed!\n");
    return 0;
}
