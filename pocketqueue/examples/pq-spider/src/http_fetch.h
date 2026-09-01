/* http_fetch.h - plain-socket HTTP client (no libcurl dependency).
 *
 * Supports a single GET; no redirect-following, no keep-alive. Returns
 * body up to FETCH_MAX_BODY bytes. Caller frees the fetch_response.
 */
#ifndef SPIDER_HTTP_FETCH_H
#define SPIDER_HTTP_FETCH_H

#include <stdbool.h>
#include <stddef.h>

#define FETCH_MAX_BODY (8u << 20)   /* 8 MiB hard cap */
#define FETCH_TIMEOUT_S 20

typedef struct fetch_response {
    char *body;
    size_t body_len;
    char content_type[128];        /* empty string if unset */
    long http_status;              /* 0 on transport error */
} fetch_response;

void fetch_response_free(fetch_response *r);

/* Synchronously GET `url`. Returns true on success (even for HTTP errors
 * 4xx/5xx — in that case r->http_status is set and r->body may be empty).
 * Returns false on transport / DNS / connection errors. */
bool http_fetch(const char *url, fetch_response *r);

#endif /* SPIDER_HTTP_FETCH_H */
