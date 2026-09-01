/* logger.h - thread-safe leveled logger with text and JSON output.
 * Spec §25. The logger flushes line-by-line; each log call is atomic with
 * respect to other log calls.
 */
#ifndef PQ_LOGGER_H
#define PQ_LOGGER_H

#include "config.h"

#include <stdarg.h>

/* Initialize the global logger. Safe to call once at startup. */
void pq_logger_init(pq_log_level level, pq_log_format format);

/* Log functions honour the level set at init time. */
void pq_log(pq_log_level level, const char *fmt, ...);
void pq_logv(pq_log_level level, const char *fmt, va_list ap);

/* Convenience wrappers. */
#define PQ_LOG_ERROR(fmt, ...) pq_log(PQ_LOG_ERROR, fmt, ##__VA_ARGS__)
#define PQ_LOG_WARN(fmt, ...)  pq_log(PQ_LOG_WARN,  fmt, ##__VA_ARGS__)
#define PQ_LOG_INFO(fmt, ...)  pq_log(PQ_LOG_INFO,  fmt, ##__VA_ARGS__)
#define PQ_LOG_DEBUG(fmt, ...) pq_log(PQ_LOG_DEBUG, fmt, ##__VA_ARGS__)

#endif /* PQ_LOGGER_H */