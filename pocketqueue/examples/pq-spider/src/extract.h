/* extract.h - lightweight link extraction from HTML.
 *
 * Scans the body for href="…" and src="…" attributes and emits each
 * unique URL as a separate NUL-terminated string into a growable array.
 * The caller owns the returned array and must free it with extract_free.
 */
#ifndef SPIDER_EXTRACT_H
#define SPIDER_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct extracted {
    char **urls;
    size_t count;
    size_t cap;
} extracted;

void extracted_free(extracted *e);

/* Returns true on success (e->count may be 0). Emits up to MAX_LINK_PER_PAGE
 * unique URLs; further URLs are dropped. */
bool extract_links(const char *body, size_t body_len,
                   const char *base_url, extracted *e);

#endif /* SPIDER_EXTRACT_H */
