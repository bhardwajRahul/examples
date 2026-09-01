/* config.h - server configuration (spec §24).
 *
 * CLI > env > built-in defaults. Parsed once at startup into pq_config_t.
 */
#ifndef PQ_CONFIG_H
#define PQ_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "pocketqueue/pocketqueue.h"

typedef enum pq_log_level {
    PQ_LOG_ERROR = 0,
    PQ_LOG_WARN,
    PQ_LOG_INFO,
    PQ_LOG_DEBUG
} pq_log_level;

typedef enum pq_log_format {
    PQ_LOG_FORMAT_TEXT = 0,
    PQ_LOG_FORMAT_JSON
} pq_log_format;

typedef struct pq_config {
    char bind_address[64];
    uint16_t port;
    char database_path[1024];
    char migrations_dir[1024];

    int64_t default_visibility_ms;
    int64_t min_visibility_ms;
    int64_t max_visibility_ms;

    int default_max_attempts;     /* valid range: 1..100 */
    int64_t max_wait_ms;          /* long-poll upper bound */

    int64_t max_body_bytes;       /* HTTP request body cap */

    int worker_threads;           /* HTTP worker pool size */

    pq_log_level log_level;
    pq_log_format log_format;
} pq_config;

#define PQ_CONFIG_DEFAULTS                                            \
    ((pq_config){                                                     \
        .bind_address = "127.0.0.1",                                  \
        .port = 8080,                                                 \
        .database_path = "pocketqueue.db",                            \
        .migrations_dir = "migrations",                               \
        .default_visibility_ms = 30000,                               \
        .min_visibility_ms = 1000,                                    \
        .max_visibility_ms = 600000,                                  \
        .default_max_attempts = 3,                                    \
        .max_wait_ms = 30000,                                         \
        .max_body_bytes = 1048576,                                    \
        .worker_threads = 8,                                          \
        .log_level = PQ_LOG_INFO,                                     \
        .log_format = PQ_LOG_FORMAT_TEXT,                             \
    })

/* Parse argv + env. On success returns PQ_OK and fills *out. On failure
 * returns a non-OK status and writes a human-readable message into *err
 * (caller-supplied buffer). */
pq_status pq_config_parse(pq_config *out, pq_error *err,
                          int argc, char **argv);

const char *pq_log_level_name(pq_log_level level);
const char *pq_log_format_name(pq_log_format fmt);

#endif /* PQ_CONFIG_H */