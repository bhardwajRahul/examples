/* test_str_util.c - string validation, parsing, url-decode. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include "str_util.h"

static void test_valid_queue_names(void **state)
{
    (void)state;
    assert_true(pq_str_is_valid_queue_name("a", false));
    assert_true(pq_str_is_valid_queue_name("jobs", false));
    assert_true(pq_str_is_valid_queue_name("image-processing", false));
    assert_true(pq_str_is_valid_queue_name("email.outbound", false));
    assert_true(pq_str_is_valid_queue_name("queue_01", false));
    char max[65];
    memset(max, 'a', 64);
    max[64] = '\0';
    assert_true(pq_str_is_valid_queue_name(max, false));
}

static void test_invalid_queue_names(void **state)
{
    (void)state;
    assert_false(pq_str_is_valid_queue_name("", false));
    assert_false(pq_str_is_valid_queue_name("/jobs", false));
    assert_false(pq_str_is_valid_queue_name("queue name", false));
    assert_false(pq_str_is_valid_queue_name("../jobs", false));
    assert_false(pq_str_is_valid_queue_name("jobs?", false));
    assert_false(pq_str_is_valid_queue_name("jobs.dead", false)); /* reserved */
    assert_false(pq_str_is_valid_queue_name("x.dead", false));
    char too_long[66];
    memset(too_long, 'a', 65);
    too_long[65] = '\0';
    assert_false(pq_str_is_valid_queue_name(too_long, false));
}

static void test_dead_suffix_allowed_with_flag(void **state)
{
    (void)state;
    assert_true(pq_str_is_valid_queue_name("jobs.dead", true));
    assert_false(pq_str_is_valid_queue_name("jobs.dead", false));
}

static void test_parse_int(void **state)
{
    (void)state;
    int64_t v;
    assert_true(pq_str_parse_int64("12345", &v));
    assert_int_equal(v, 12345);
    assert_false(pq_str_parse_int64("", &v));
    assert_false(pq_str_parse_int64("abc", &v));
    assert_false(pq_str_parse_int64("12abc", &v));
    uint16_t u;
    assert_true(pq_str_parse_uint16("8080", &u));
    assert_int_equal(u, 8080);
    assert_false(pq_str_parse_uint16("99999", &u));
}

static void test_url_decode(void **state)
{
    (void)state;
    char buf[64];
    int n = pq_str_url_decode(buf, sizeof(buf), "hello+world", 11);
    assert_int_equal(n, 11);
    assert_string_equal(buf, "hello world");

    n = pq_str_url_decode(buf, sizeof(buf), "a%20b%2Bc", 8);
    assert_int_equal(n, 4);
    assert_string_equal(buf, "a b+");

    /* Full URL-encoded string. */
    n = pq_str_url_decode(buf, sizeof(buf), "a%20b%2Bc", 9);
    assert_int_equal(n, 5);
    assert_string_equal(buf, "a b+c");

    /* Truncated percent escape — copied verbatim. */
    n = pq_str_url_decode(buf, sizeof(buf), "abc%2", 5);
    assert_int_equal(n, 5);
    assert_string_equal(buf, "abc%2");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_valid_queue_names),
        cmocka_unit_test(test_invalid_queue_names),
        cmocka_unit_test(test_dead_suffix_allowed_with_flag),
        cmocka_unit_test(test_parse_int),
        cmocka_unit_test(test_url_decode),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}