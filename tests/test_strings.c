#define MYBUDDY_IMPLEMENTATION
#include "../mbd_strings.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

void test_string_helpers() {
    printf("Testing string helpers...\n");

    const char *orig = "Hello, MyBuddy!";

    // Test mbd_strdup
    char *dup1 = mbd_strdup(orig);
    assert(dup1 != NULL);
    assert(strcmp(dup1, orig) == 0);
    mbd_free(dup1);

    // Test mbd_strndup
    char *dup2 = mbd_strndup(orig, 5);
    assert(dup2 != NULL);
    assert(strcmp(dup2, "Hello") == 0);
    mbd_free(dup2);

    // Test mbd_asprintf
    char *fmt_str = mbd_asprintf("Testing %d, %d, %d", 1, 2, 3);
    assert(fmt_str != NULL);
    assert(strcmp(fmt_str, "Testing 1, 2, 3") == 0);
    mbd_free(fmt_str);

    printf("String helpers passed!\n");
}

void test_string_builder() {
    printf("Testing string builder...\n");

    mbd_string_t sb = mbd_string_new(8);
    assert(sb.data != NULL);
    assert(sb.len == 0);
    assert(sb.capacity >= 8);

    mbd_string_append(&sb, "Hello");
    assert(sb.len == 5);
    assert(strcmp(sb.data, "Hello") == 0);

    mbd_string_append(&sb, ", ");
    assert(sb.len == 7);
    assert(strcmp(sb.data, "Hello, ") == 0);

    mbd_string_appendf(&sb, "World! %d", 2026);
    assert(sb.len == 18);
    assert(strcmp(sb.data, "Hello, World! 2026") == 0);
    assert(sb.capacity >= 19); // Make sure it grew

    mbd_string_free(&sb);
    assert(sb.data == NULL);
    assert(sb.len == 0);
    assert(sb.capacity == 0);

    printf("String builder passed!\n");
}

void test_table() {
    printf("Testing table...\n");

    mbd_table_t *t = mbd_table_new(4);
    assert(t != NULL);

    // Test hash part
    mbd_table_insert(t, "key1", "value1");
    mbd_table_insert(t, "key2", "value2");
    mbd_table_insert(t, "key3", "value3");
    mbd_table_insert(t, "key4", "value4"); // Should trigger resize
    mbd_table_insert(t, "key5", "value5");

    assert(strcmp((char *)mbd_table_get(t, "key1"), "value1") == 0);
    assert(strcmp((char *)mbd_table_get(t, "key5"), "value5") == 0);
    assert(mbd_table_get(t, "missing") == NULL);

    mbd_table_remove(t, "key3");
    assert(mbd_table_get(t, "key3") == NULL);
    assert(t->size == 4);

    // Test array part
    mbd_table_seti(t, 0, "array_val0");
    mbd_table_seti(t, 5, "array_val5");
    mbd_table_seti(t, 10, "array_val10");

    assert(strcmp((char *)mbd_table_geti(t, 0), "array_val0") == 0);
    assert(strcmp((char *)mbd_table_geti(t, 5), "array_val5") == 0);
    assert(strcmp((char *)mbd_table_geti(t, 10), "array_val10") == 0);
    assert(mbd_table_geti(t, 2) == NULL);

    mbd_table_free(t);

    printf("Table passed!\n");
}

int main() {
    mbd_init();

    test_string_helpers();
    test_string_builder();
    test_table();

    mbd_destroy();
    printf("All strings and table tests passed!\n");
    return 0;
}
