/* random_util.h - secure random for message IDs and receipts (spec §23). */
#ifndef PQ_RANDOM_UTIL_H
#define PQ_RANDOM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fill a buffer with cryptographically-secure random bytes. Falls back to
 * /dev/urandom when getrandom() is unavailable. Returns false on fatal
 * failure (e.g. both unavailable). */
bool pq_random_bytes(void *buf, size_t n);

/* Generate a UUID v7 (time-ordered) as a 36-char string including NUL. */
bool pq_random_uuid_v7(char out[37]);

/* Generate a 32-char lowercase hex string for receipts. */
bool pq_random_hex(char out[33], size_t byte_count);

#endif /* PQ_RANDOM_UTIL_H */