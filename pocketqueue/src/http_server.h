/* http_server.h - hand-rolled minimal HTTP/1.1 server.
 *
 * Decision D1: written in-tree because no MIT-licensed lib was available
 * with a portable build (libmicrohttpd needs autoconf; Mongoose/CivetWeb
 * are GPL). The server is intentionally small and targets the spec's
 * needs: GET/POST, path params, query strings, bounded body sizes, and
 * graceful shutdown.
 *
 * Threading model: one accept thread + N worker threads pulling accepted
 * connections from a bounded queue. On pq_http_server_stop the listener
 * is closed and workers drain their current request before returning.
 */
#ifndef PQ_HTTP_SERVER_H
#define PQ_HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pq_http_server pq_http_server;

typedef struct pq_http_request {
    const char *method;          /* "GET" or "POST" */
    const char *path;            /* e.g. "/queues/jobs/messages" */
    const char *query;           /* NULL if absent; key=val&... */
    const char *content_type;    /* NULL if absent; lower-case */
    const char *body;            /* may be NULL or zero-length */
    size_t body_length;
    const char *remote_addr;     /* human-readable "x.x.x.x:port" or empty */
} pq_http_request;

typedef struct pq_http_response {
    int status_code;                       /* 200, 201, 204, 4xx, 5xx */
    const char *content_type;              /* may be NULL for empty body */
    const char *body;                      /* may be NULL; not freed */
    size_t body_length;                    /* bytes to send (0 allowed) */
    const char *extra_header_name;         /* optional; e.g. "Location" */
    const char *extra_header_value;        /* optional; ignored if name NULL */
} pq_http_response;

#define PQ_HTTP_RESPONSE_EMPTY ((pq_http_response){.status_code = 204})
#define PQ_HTTP_RESPONSE_TEXT(code, ctype, text)                                    \
    ((pq_http_response){                                                            \
        .status_code = (code), .content_type = (ctype), .body = (text),             \
        .body_length = sizeof(text) - 1,                                            \
        .extra_header_name = NULL, .extra_header_value = NULL})

/* Handler returns a response. The response struct is shallow-copied; the
 * server only reads it before returning from the handler. */
typedef pq_http_response (*pq_http_handler)(void *ctx, const pq_http_request *req);

/* bind_address may be "127.0.0.1", "0.0.0.0", or "::1". port is 1..65535.
 * worker_threads is the size of the worker pool; capped internally. */
pq_http_server *pq_http_server_start(const char *bind_address,
                                     uint16_t port,
                                     int worker_threads,
                                     int64_t max_body_bytes,
                                     pq_http_handler handler,
                                     void *ctx);

/* Returns the actual bound port (useful if port==0 picking a free port). */
uint16_t pq_http_server_port(const pq_http_server *srv);

/* Stop accepting new connections, drain in-flight, then close. Blocks
 * until all workers exit or until grace_period_ms elapses. */
void pq_http_server_stop(pq_http_server *srv, int64_t grace_period_ms);

/* Returns true if stop() has been called. */
bool pq_http_server_is_stopping(const pq_http_server *srv);

#endif /* PQ_HTTP_SERVER_H */