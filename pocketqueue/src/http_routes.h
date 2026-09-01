/* http_routes.h - HTTP handler that dispatches the spec's queue API
 * (publish / reserve / ack / nack / stats) onto pq_service. The handler
 * is registered with pq_http_server_start() and runs on each request
 * worker thread.
 */
#ifndef PQ_HTTP_ROUTES_H
#define PQ_HTTP_ROUTES_H

#include "pocketqueue/service.h"

#include "config.h"
#include "http_server.h"

typedef struct pq_http_routes_ctx {
    pq_service *service;
    int64_t default_visibility_ms;
} pq_http_routes_ctx;

/* The actual request handler (compatible with pq_http_handler). */
pq_http_response pq_http_routes_dispatch(void *ctx, const pq_http_request *req);

/* Free any heap memory owned by the response (body, extra_header_*).
 * Safe to call on any response. */
void pq_http_routes_dispose(pq_http_response *resp);

/* Build a response with a heap-allocated body (the caller transfers
 * ownership to pq_http_routes_dispose). Use this instead of
 * PQ_HTTP_RESPONSE_TEXT for paths where the response must be disposed
 * by the generic dispatcher. */
pq_http_response pq_text_response(int status_code,
                                  const char *content_type,
                                  const char *body);

#endif /* PQ_HTTP_ROUTES_H */