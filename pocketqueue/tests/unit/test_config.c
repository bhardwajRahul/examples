/* test_config.c - pq_config_parse: defaults, overrides, validation. */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <cmocka.h>

#include "config.h"

static void clear_env(void)
{
    unsetenv("PQ_BIND");
    unsetenv("PQ_PORT");
    unsetenv("PQ_DATABASE");
    unsetenv("PQ_MIGRATIONS_DIR");
    unsetenv("PQ_DEFAULT_VISIBILITY_MS");
    unsetenv("PQ_MIN_VISIBILITY_MS");
    unsetenv("PQ_MAX_VISIBILITY_MS");
    unsetenv("PQ_DEFAULT_MAX_ATTEMPTS");
    unsetenv("PQ_MAX_WAIT_MS");
    unsetenv("PQ_MAX_BODY_BYTES");
    unsetenv("PQ_WORKER_THREADS");
    unsetenv("PQ_LOG_LEVEL");
    unsetenv("PQ_LOG_FORMAT");
}

static void test_defaults(void **state)
{
    (void)state;
    clear_env();

    pq_config cfg;
    pq_error err = {0};
    char *argv[] = {(char *)"pocketqueue-server"};
    assert_int_equal(pq_config_parse(&cfg, &err, 1, argv), PQ_OK);
    assert_string_equal(cfg.bind_address, "127.0.0.1");
    assert_int_equal(cfg.port, 8080);
    assert_string_equal(cfg.database_path, "pocketqueue.db");
    assert_int_equal(cfg.default_max_attempts, 3);
    assert_int_equal(cfg.worker_threads, 8);
    assert_int_equal(cfg.log_level, PQ_LOG_INFO);
}

static void test_cli_override(void **state)
{
    (void)state;
    clear_env();
    pq_config cfg;
    pq_error err = {0};
    char *argv[] = {(char *)"pocketqueue-server",
                    (char *)"--port", (char *)"9090",
                    (char *)"--database", (char *)"/tmp/foo.db"};
    assert_int_equal(pq_config_parse(&cfg, &err, 5, argv), PQ_OK);
    assert_int_equal(cfg.port, 9090);
    assert_string_equal(cfg.database_path, "/tmp/foo.db");
}

static void test_env_override(void **state)
{
    (void)state;
    clear_env();
    setenv("PQ_PORT", "7000", 1);
    setenv("PQ_LOG_LEVEL", "debug", 1);
    pq_config cfg;
    pq_error err = {0};
    char *argv[] = {(char *)"pocketqueue-server"};
    assert_int_equal(pq_config_parse(&cfg, &err, 1, argv), PQ_OK);
    assert_int_equal(cfg.port, 7000);
    assert_int_equal(cfg.log_level, PQ_LOG_DEBUG);
}

static void test_cli_overrides_env(void **state)
{
    (void)state;
    clear_env();
    setenv("PQ_PORT", "7000", 1);
    pq_config cfg;
    pq_error err = {0};
    char *argv[] = {(char *)"pocketqueue-server",
                    (char *)"--port", (char *)"9999"};
    assert_int_equal(pq_config_parse(&cfg, &err, 3, argv), PQ_OK);
    assert_int_equal(cfg.port, 9999);
}

static void test_invalid_port(void **state)
{
    (void)state;
    clear_env();
    pq_config cfg;
    pq_error err = {0};
    char *argv[] = {(char *)"pocketqueue-server",
                    (char *)"--port", (char *)"99999"};
    assert_int_equal(pq_config_parse(&cfg, &err, 3, argv), PQ_INVALID_ARGUMENT);
}

static void test_unknown_option(void **state)
{
    (void)state;
    clear_env();
    pq_config cfg;
    pq_error err = {0};
    char *argv[] = {(char *)"pocketqueue-server",
                    (char *)"--not-a-flag"};
    assert_int_equal(pq_config_parse(&cfg, &err, 2, argv), PQ_INVALID_ARGUMENT);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_defaults),
        cmocka_unit_test(test_cli_override),
        cmocka_unit_test(test_env_override),
        cmocka_unit_test(test_cli_overrides_env),
        cmocka_unit_test(test_invalid_port),
        cmocka_unit_test(test_unknown_option),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}