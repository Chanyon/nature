#include <stdio.h>

#include "string.h"
#include "tests/test.h"
#include "utils/assertf.h"
#include "utils/exec.h"

static void test_basic() {
    return;
    char *raw = exec_output();
    printf("%s", raw);
    return;
    char *str = "";
    assert_string_equal(raw, str);
}

int main(void) {
    TEST_BASIC
}