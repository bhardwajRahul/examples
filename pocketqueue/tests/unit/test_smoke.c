/* test_smoke.c - trivial sanity test, ensures CTest finds the binary. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

static void test_smoke(void **state)
{
    (void)state;
    assert_true(1 + 1 == 2);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_smoke),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}