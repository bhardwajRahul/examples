/* queue_service.h - internal bits of pq_service. The struct is opaque to
 * public callers; both the in-memory and SQLite-backed implementations
 * live behind the same pq_service_vtable so the public pq_service_*
 * signatures stay stable across backends.
 */
#ifndef PQ_QUEUE_SERVICE_H
#define PQ_QUEUE_SERVICE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "notifier.h"
#include "pocketqueue/service.h"

#define PQ_MAX_QUEUES 1024
#define PQ_MAX_PAYLOAD_BYTES (1 << 20)  /* 1 MiB hard cap */

typedef struct pq_service_vtable {
    void (*destroy)(void *state);
    pq_status (*publish)(void *state, const pq_publish_request *req,
                         pq_message *out, pq_error *err);
    pq_status (*reserve)(void *state, const char *queue_name,
                         int64_t visibility_timeout_ms, int64_t wait_ms,
                         pq_message *out, pq_error *err);
    pq_status (*ack)(void *state, const char *queue_name,
                     const char *message_id, const char *receipt,
                     pq_error *err);
    pq_status (*nack)(void *state, const char *queue_name,
                      const char *message_id, const char *receipt,
                      const char *reason, pq_error *err);
    pq_status (*stats)(void *state, const char *queue_name,
                       pq_queue_stats *out, pq_error *err);
    /* Returns the earliest reservation-expiry ms in the named queue,
     * or INT64_MAX if there are no reservations. The reserve loop uses
     * this to compute the next-event deadline for long-poll waits. */
    int64_t (*next_event_ms)(void *state, const char *queue_name);
} pq_service_vtable;

struct pq_service {
    pq_service_config cfg;
    pq_clock *clock;            /* cached for fast access */
    pq_repository *repo;        /* NULL for in-memory backend */
    void *state;                /* backend-specific state */
    pq_notifier notifier;       /* shared across publish/nack/ack */
    const pq_service_vtable *vtable;
};

/* In-memory backend. */
bool pq_inmem_init(pq_service *svc, const pq_service_config *cfg, pq_error *err);
extern const pq_service_vtable pq_inmem_vtable;

/* SQLite-backed backend. Requires a non-NULL repository. */
bool pq_sqlite_init(pq_service *svc, const pq_service_config *cfg, pq_error *err);
extern const pq_service_vtable pq_sqlite_vtable;

#endif /* PQ_QUEUE_SERVICE_H */