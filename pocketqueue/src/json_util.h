/* json_util.h - thin wrappers over cJSON that match spec §14.
 *
 * The HTTP layer allocates and frees cJSON values; this header collects
 * repetitive error mapping into one place.
 */
#ifndef PQ_JSON_UTIL_H
#define PQ_JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "pocketqueue/pocketqueue.h"

/* Parse a JSON document, rejecting unknown top-level keys when strict.
 * The parser copies input if needed; safe with non-NUL-terminated buffers. */
cJSON *pq_json_parse(const char *data, size_t length, bool strict_top_level,
                     pq_error *err);

/* Serialize a cJSON value to a heap buffer the caller must free with
 * free(). Returns NULL on failure. */
char *pq_json_dump(const cJSON *value, size_t *out_length);

/* Convenience builders. */
cJSON *pq_json_new_object(void);
cJSON *pq_json_new_error(const char *code, const char *message,
                         const cJSON *details);

#endif /* PQ_JSON_UTIL_H */