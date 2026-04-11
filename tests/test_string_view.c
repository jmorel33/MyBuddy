#define MYBUDDY_IMPLEMENTATION
#include "../mybuddy.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main() {
    mbd_init();

    const char *test_str = "hello world";
    mbd_string_view_t sv1 = mbd_string_view_from_cstr(test_str);
    assert(sv1.len == strlen(test_str));
    assert(strncmp(sv1.data, test_str, sv1.len) == 0);

    mbd_string_view_t sv2 = mbd_string_view_from_data(test_str, 5);
    assert(sv2.len == 5);
    assert(strncmp(sv2.data, "hello", 5) == 0);

    char *dup = mbd_string_view_dup(sv2);
    assert(dup != NULL);
    assert(strlen(dup) == 5);
    assert(strcmp(dup, "hello") == 0);

    mbd_free(dup);
    mbd_destroy();
    printf("String view tests passed!\n");
    return 0;
}
