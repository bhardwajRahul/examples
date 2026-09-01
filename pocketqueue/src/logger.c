/* logger.c - thread-safe leveled logger.
 *
 * Output goes to stderr. Each log call holds an internal mutex so that
 * interleaved writes from multiple threads produce one complete line each.
 * The level filter is checked first to avoid the cost of formatting.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "logger.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "str_util.h"

static struct {
    pthread_mutex_t mu;
    pq_log_level level;
    pq_log_format format;
    bool initialised;
} g_logger = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .level = PQ_LOG_INFO,
    .format = PQ_LOG_FORMAT_TEXT,
    .initialised = false,
};

const char *pq_log_level_name(pq_log_level level)
{
    switch (level) {
        case PQ_LOG_ERROR: return "error";
        case PQ_LOG_WARN:  return "warn";
        case PQ_LOG_INFO:  return "info";
        case PQ_LOG_DEBUG: return "debug";
    }
    return "info";
}

const char *pq_log_format_name(pq_log_format fmt)
{
    switch (fmt) {
        case PQ_LOG_FORMAT_TEXT: return "text";
        case PQ_LOG_FORMAT_JSON: return "json";
    }
    return "text";
}

void pq_logger_init(pq_log_level level, pq_log_format format)
{
    pthread_mutex_lock(&g_logger.mu);
    g_logger.level = level;
    g_logger.format = format;
    g_logger.initialised = true;
    pthread_mutex_unlock(&g_logger.mu);
}

static void format_iso8601_utc_ms(char out[40])
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    /* 2026-07-15T09:30:45.123Z */
    snprintf(out, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)((ts.tv_nsec + 500000) / 1000000));
}

static void write_text(pq_log_level level, const char *msg)
{
    char ts[40];
    format_iso8601_utc_ms(ts);
    fprintf(stderr, "%s %-5s %s\n", ts, pq_log_level_name(level), msg);
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 7 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
            case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '\b': dst[j++] = '\\'; dst[j++] = 'b';  break;
            case '\f': dst[j++] = '\\'; dst[j++] = 'f';  break;
            case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
            case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
            case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    j += (size_t)snprintf(dst + j, dst_size - j, "\\u%04x", c);
                } else {
                    dst[j++] = (char)c;
                }
        }
    }
    dst[j] = '\0';
}

static void write_json(pq_log_level level, const char *msg)
{
    char ts[40];
    format_iso8601_utc_ms(ts);
    char esc[2048];
    json_escape(msg, esc, sizeof(esc));
    fprintf(stderr,
            "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"}\n",
            ts, pq_log_level_name(level), esc);
}

void pq_logv(pq_log_level level, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&g_logger.mu);
    if (!g_logger.initialised || level > g_logger.level) {
        pthread_mutex_unlock(&g_logger.mu);
        return;
    }
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    if (g_logger.format == PQ_LOG_FORMAT_JSON) {
        write_json(level, msg);
    } else {
        write_text(level, msg);
    }
    pthread_mutex_unlock(&g_logger.mu);
}

void pq_log(pq_log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pq_logv(level, fmt, ap);
    va_end(ap);
}