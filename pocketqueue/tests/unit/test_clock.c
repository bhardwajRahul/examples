/* test_clock.c - pq_clock vtable and fake clock. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include "clock.h"

static void test_system_clock_returns_recent_time(void **state)
{
    (void)state;
    const pq_clock *c = pq_clock_system();
    int64_t now = c->wall_time_ms(NULL);
    /* Should be after 2024-01-01. */
    assert_true(now > 1704067200000LL);
    int64_t mono = c->monotonic_time_ms(NULL);
    assert_true(mono >= 0);
}

static void test_fake_clock_advances(void **state)
{
    (void)state;
    pq_clock *c = pq_clock_fake();
    pq_clock_fake_set(1000000000000LL);
    assert_int_equal(c->wall_time_ms(NULL), 1000000000000LL);
    pq_clock_fake_advance_ms(500);
    assert_int_equal(c->wall_time_ms(NULL), 1000000000500LL);
    assert_int_equal(pq_clock_fake_now_ms(), 1000000000500LL);
    pq_clock_fake_advance_ms(-100);
    assert_int_equal(c->wall_time_ms(NULL), 1000000000400LL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_system_clock_returns_recent_time),
        cmocka_unit_test(test_fake_clock_advances),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}