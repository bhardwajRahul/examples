/* str_util.h - bounded string helpers. All functions NUL-terminate. */
#ifndef PQ_STR_UTIL_H
#define PQ_STR_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Like snprintf but always NUL-terminates. Returns length excluding NUL,
 * or -1 if truncated. */
int pq_str_copy(char *dst, size_t dst_size, const char *src);

/* Validate a queue name against spec §8.1. Returns true if valid.
 * If allow_dead_suffix is false, names ending in ".dead" are rejected. */
bool pq_str_is_valid_queue_name(const char *name, bool allow_dead_suffix);

/* URL-decode in place into a bounded buffer. Returns new length or -1. */
int pq_str_url_decode(char *dst, size_t dst_size, const char *src, size_t src_len);

/* Parse a positive integer from a string. Returns true on success. */
bool pq_str_parse_int64(const char *s, int64_t *out);
bool pq_str_parse_uint16(const char *s, uint16_t *out);
bool pq_str_parse_int(const char *s, int *out);

/* Case-insensitive equality. */
int pq_str_iequal(const char *a, const char *b);

/* Append ".dead" to a base queue name. *dst* must already hold the base
 * name; the suffix is concatenated in place, truncated if necessary. */
void pq_str_append_dot_dead(char *s, size_t size);

#endif /* PQ_STR_UTIL_H */