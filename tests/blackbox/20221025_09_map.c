#include "tests/test.h"
#include <stdio.h>

static void test_basic() {
    char *raw = exec_output();
    assert_string_equal(raw, "a = 1\n"
                             "3 33\n"
                             "3\n"
                             "2\n"
                             "catch error: 'key 'b' not found in map' at cases/20221025_09_map.n:16:11\n");
}

int main(void) {
    TEST_BASIC
}