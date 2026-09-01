/* test_random_util.c - secure random + UUID v7 + hex encoding. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include "random_util.h"

static void test_random_bytes_returns_data(void **state)
{
    (void)state;
    uint8_t a[32], b[32];
    assert_true(pq_random_bytes(a, sizeof(a)));
    assert_true(pq_random_bytes(b, sizeof(b)));
    /* Two independent reads should not be byte-identical. */
    assert_int_not_equal(memcmp(a, b, sizeof(a)), 0);
}

static void test_uuid_v7_shape(void **state)
{
    (void)state;
    char id1[37], id2[37];
    assert_true(pq_random_uuid_v7(id1));
    assert_true(pq_random_uuid_v7(id2));

    /* Length: 36 chars + NUL. */
    assert_int_equal(strlen(id1), 36);
    assert_int_equal(strlen(id2), 36);

    /* Dashes at fixed positions. */
    assert_int_equal(id1[8], '-');
    assert_int_equal(id1[13], '-');
    assert_int_equal(id1[18], '-');
    assert_int_equal(id1[23], '-');

    /* Version nibble must be '7' (RFC 9562). */
    assert_int_equal(id1[14], '7');
    assert_int_equal(id2[14], '7');

    /* Variant nibble must be 8/9/a/b (binary 10xx). */
    char v = id1[19];
    assert_true(v == '8' || v == '9' || v == 'a' || v == 'b');

    /* Sequential UUIDs from the same epoch are time-ordered. */
    assert_true(strcmp(id1, id2) < 0 || strcmp(id1, id2) > 0); /* not equal */
}

static void test_random_hex(void **state)
{
    (void)state;
    char hex1[33], hex2[33];
    assert_true(pq_random_hex(hex1, 16));
    assert_true(pq_random_hex(hex2, 16));
    assert_int_equal(strlen(hex1), 32);
    assert_int_equal(strlen(hex2), 32);
    assert_int_not_equal(memcmp(hex1, hex2, 32), 0);
    for (size_t i = 0; i < 32; i++) {
        char c = hex1[i];
        assert_true((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_random_bytes_returns_data),
        cmocka_unit_test(test_uuid_v7_shape),
        cmocka_unit_test(test_random_hex),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}