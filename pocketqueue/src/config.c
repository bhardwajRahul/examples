/* config.c - server configuration parsing.
 *
 * Precedence: command-line flags > environment variables > built-in
 * defaults (spec §24). Unknown flags or invalid values cause startup to
 * fail with an actionable error message and exit code 2.
 */
#include "config.h"

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "str_util.h"

#define ENV(name) #name
#define ENV_BIND "PQ_BIND"
#define ENV_PORT "PQ_PORT"
#define ENV_DATABASE "PQ_DATABASE"
#define ENV_MIGRATIONS_DIR "PQ_MIGRATIONS_DIR"
#define ENV_DEFAULT_VISIBILITY_MS "PQ_DEFAULT_VISIBILITY_MS"
#define ENV_MIN_VISIBILITY_MS "PQ_MIN_VISIBILITY_MS"
#define ENV_MAX_VISIBILITY_MS "PQ_MAX_VISIBILITY_MS"
#define ENV_DEFAULT_MAX_ATTEMPTS "PQ_DEFAULT_MAX_ATTEMPTS"
#define ENV_MAX_WAIT_MS "PQ_MAX_WAIT_MS"
#define ENV_MAX_BODY_BYTES "PQ_MAX_BODY_BYTES"
#define ENV_WORKER_THREADS "PQ_WORKER_THREADS"
#define ENV_LOG_LEVEL "PQ_LOG_LEVEL"
#define ENV_LOG_FORMAT "PQ_LOG_FORMAT"

enum {
    OPT_HELP = 1000,
};

static const struct option kLongOptions[] = {
    {"bind",                 required_argument, NULL, 'b'},
    {"port",                 required_argument, NULL, 'p'},
    {"database",             required_argument, NULL, 'd'},
    {"migrations-dir",       required_argument, NULL, 10},
    {"default-visibility-ms", required_argument, NULL, 1},
    {"min-visibility-ms",    required_argument, NULL, 2},
    {"max-visibility-ms",    required_argument, NULL, 3},
    {"default-max-attempts", required_argument, NULL, 4},
    {"max-wait-ms",          required_argument, NULL, 5},
    {"max-body-bytes",       required_argument, NULL, 6},
    {"worker-threads",       required_argument, NULL, 7},
    {"log-level",            required_argument, NULL, 8},
    {"log-format",           required_argument, NULL, 9},
    {"help",                 no_argument,       NULL, OPT_HELP},
    {NULL, 0, NULL, 0}
};

static const char *kUsage =
    "Usage: pocketqueue-server [options]\n"
    "  --bind ADDRESS              Bind address (default 127.0.0.1)\n"
    "  --port PORT                 TCP port (default 8080)\n"
    "  --database PATH             SQLite file path (default pocketqueue.db)\n"
    "  --migrations-dir PATH       Migrations directory (default ./migrations)\n"
    "  --default-visibility-ms N   Default visibility timeout (default 30000)\n"
    "  --min-visibility-ms N       Minimum visibility timeout (default 1000)\n"
    "  --max-visibility-ms N       Maximum visibility timeout (default 600000)\n"
    "  --default-max-attempts N    Default max delivery attempts (default 3)\n"
    "  --max-wait-ms N             Maximum long-poll wait (default 30000)\n"
    "  --max-body-bytes N          Maximum HTTP request body (default 1048576)\n"
    "  --worker-threads N          HTTP worker pool size (default 8)\n"
    "  --log-level LEVEL           error|warn|info|debug (default info)\n"
    "  --log-format FORMAT         text|json (default text)\n"
    "  --help                      Print this help\n";

static bool env_string(const char *name, char *dst, size_t dst_size)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0') return false;
    return pq_str_copy(dst, dst_size, v) >= 0;
}

static bool env_int(const char *name, int64_t *out)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0') return false;
    return pq_str_parse_int64(v, out);
}

static bool parse_log_level(const char *s, pq_log_level *out)
{
    if (pq_str_iequal(s, "error") == 0) { *out = PQ_LOG_ERROR; return true; }
    if (pq_str_iequal(s, "warn")  == 0) { *out = PQ_LOG_WARN;  return true; }
    if (pq_str_iequal(s, "info")  == 0) { *out = PQ_LOG_INFO;  return true; }
    if (pq_str_iequal(s, "debug") == 0) { *out = PQ_LOG_DEBUG; return true; }
    return false;
}

static bool parse_log_format(const char *s, pq_log_format *out)
{
    if (pq_str_iequal(s, "text") == 0) { *out = PQ_LOG_FORMAT_TEXT; return true; }
    if (pq_str_iequal(s, "json") == 0) { *out = PQ_LOG_FORMAT_JSON; return true; }
    return false;
}

static int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void fail(pq_error *err, const char *msg)
{
    if (err == NULL) return;
    pq_error_set(err, "invalid_config", msg);
}

pq_status pq_config_parse(pq_config *out, pq_error *err,
                          int argc, char **argv)
{
    if (out == NULL) {
        fail(err, "internal: NULL config pointer");
        return PQ_INVALID_ARGUMENT;
    }
    *out = PQ_CONFIG_DEFAULTS;

    /* Apply env first so CLI overrides take precedence. */
    env_string(ENV_BIND, out->bind_address, sizeof(out->bind_address));
    {
        int64_t port;
        if (env_int(ENV_PORT, &port)) out->port = (uint16_t)port;
    }
    env_string(ENV_DATABASE, out->database_path, sizeof(out->database_path));
    env_string(ENV_MIGRATIONS_DIR, out->migrations_dir, sizeof(out->migrations_dir));
    {
        int64_t v;
        if (env_int(ENV_DEFAULT_VISIBILITY_MS, &v)) out->default_visibility_ms = v;
        if (env_int(ENV_MIN_VISIBILITY_MS,     &v)) out->min_visibility_ms     = v;
        if (env_int(ENV_MAX_VISIBILITY_MS,     &v)) out->max_visibility_ms     = v;
        if (env_int(ENV_DEFAULT_MAX_ATTEMPTS,  &v)) out->default_max_attempts  = (int)v;
        if (env_int(ENV_MAX_WAIT_MS,           &v)) out->max_wait_ms           = v;
        if (env_int(ENV_MAX_BODY_BYTES,        &v)) out->max_body_bytes        = v;
        if (env_int(ENV_WORKER_THREADS,        &v)) out->worker_threads        = (int)v;
    }
    {
        const char *lvl = getenv(ENV_LOG_LEVEL);
        if (lvl && *lvl) {
            if (!parse_log_level(lvl, &out->log_level)) {
                fail(err, "PQ_LOG_LEVEL must be one of error|warn|info|debug");
                return PQ_INVALID_ARGUMENT;
            }
        }
        const char *fmt = getenv(ENV_LOG_FORMAT);
        if (fmt && *fmt) {
            if (!parse_log_format(fmt, &out->log_format)) {
                fail(err, "PQ_LOG_FORMAT must be text or json");
                return PQ_INVALID_ARGUMENT;
            }
        }
    }

    /* CLI overrides. */
    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "b:p:d:",
                              kLongOptions, NULL)) != -1) {
        switch (opt) {
            case 'b':
                pq_str_copy(out->bind_address, sizeof(out->bind_address), optarg);
                break;
            case 'p': {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1 || v > 65535) {
                    fail(err, "--port must be 1..65535");
                    return PQ_INVALID_ARGUMENT;
                }
                out->port = (uint16_t)v;
                break;
            }
            case 'd':
                pq_str_copy(out->database_path, sizeof(out->database_path), optarg);
                break;
            case 10:
                pq_str_copy(out->migrations_dir, sizeof(out->migrations_dir), optarg);
                break;
            case 1: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1) {
                    fail(err, "--default-visibility-ms must be > 0");
                    return PQ_INVALID_ARGUMENT;
                }
                out->default_visibility_ms = v;
                break;
            }
            case 2: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1) {
                    fail(err, "--min-visibility-ms must be > 0");
                    return PQ_INVALID_ARGUMENT;
                }
                out->min_visibility_ms = v;
                break;
            }
            case 3: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1) {
                    fail(err, "--max-visibility-ms must be > 0");
                    return PQ_INVALID_ARGUMENT;
                }
                out->max_visibility_ms = v;
                break;
            }
            case 4: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1 || v > 100) {
                    fail(err, "--default-max-attempts must be 1..100");
                    return PQ_INVALID_ARGUMENT;
                }
                out->default_max_attempts = (int)v;
                break;
            }
            case 5: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 0) {
                    fail(err, "--max-wait-ms must be >= 0");
                    return PQ_INVALID_ARGUMENT;
                }
                out->max_wait_ms = v;
                break;
            }
            case 6: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1) {
                    fail(err, "--max-body-bytes must be > 0");
                    return PQ_INVALID_ARGUMENT;
                }
                out->max_body_bytes = v;
                break;
            }
            case 7: {
                int64_t v;
                if (!pq_str_parse_int64(optarg, &v) || v < 1) {
                    fail(err, "--worker-threads must be > 0");
                    return PQ_INVALID_ARGUMENT;
                }
                out->worker_threads = (int)v;
                break;
            }
            case 8: {
                if (!parse_log_level(optarg, &out->log_level)) {
                    fail(err, "--log-level must be one of error|warn|info|debug");
                    return PQ_INVALID_ARGUMENT;
                }
                break;
            }
            case 9: {
                if (!parse_log_format(optarg, &out->log_format)) {
                    fail(err, "--log-format must be text or json");
                    return PQ_INVALID_ARGUMENT;
                }
                break;
            }
            case OPT_HELP:
                fputs(kUsage, stdout);
                exit(0);
            case '?':
            default:
                fputs(kUsage, stderr);
                fail(err, "unknown or malformed option");
                return PQ_INVALID_ARGUMENT;
        }
    }

    /* Cross-field validation. */
    if (out->min_visibility_ms > out->max_visibility_ms) {
        fail(err, "--min-visibility-ms must be <= --max-visibility-ms");
        return PQ_INVALID_ARGUMENT;
    }
    if (out->default_visibility_ms < out->min_visibility_ms ||
        out->default_visibility_ms > out->max_visibility_ms) {
        fail(err, "--default-visibility-ms must be within min and max");
        return PQ_INVALID_ARGUMENT;
    }
    if (out->default_max_attempts < 1 || out->default_max_attempts > 100) {
        fail(err, "--default-max-attempts must be 1..100");
        return PQ_INVALID_ARGUMENT;
    }
    if (out->max_wait_ms > 600000) {
        fail(err, "--max-wait-ms must be <= 600000");
        return PQ_INVALID_ARGUMENT;
    }
    if (out->worker_threads > 256) {
        fail(err, "--worker_threads must be <= 256");
        return PQ_INVALID_ARGUMENT;
    }
    if (out->port == 0) {
        fail(err, "--port must be > 0");
        return PQ_INVALID_ARGUMENT;
    }
    (void)clamp_i64; /* reserved for future use */
    return PQ_OK;
}