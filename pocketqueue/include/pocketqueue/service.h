/* service.h - public queue-service interface (spec §43).
 *
 * The HTTP layer and the pqctl client both go through this interface.
 * Stage 2 implements it with an in-memory backend; stage 4 swaps in the
 * SQLite-backed implementation behind the same signatures.
 */
#ifndef POCKETQUEUE_SERVICE_H
#define POCKETQUEUE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pocketqueue/pocketqueue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declared so public headers don't pull in internal headers. The
 * clock and repository are owned by the caller; the service does not free
 * them. */
struct pq_clock;
typedef struct pq_clock pq_clock;
struct pq_repository;
typedef struct pq_repository pq_repository;

/* ---- Logical message states (spec §9) ---------------------------------- */
typedef enum pq_message_state {
    PQ_MSG_READY = 0,
    PQ_MSG_RESERVED = 1,
    PQ_MSG_DEAD_READY = 2,
    PQ_MSG_DEAD_RESERVED = 3
} pq_message_state;

const char *pq_message_state_name(pq_message_state s);

/* ---- Message -----------------------------------------------------------
 * payload_json is heap-allocated by the service on the way out; the caller
 * MUST call pq_message_dispose() to free it. Other fields are shallow
 * copies owned by the service.
 */
typedef struct pq_message {
    char id[64];                 /* UUID v7, ASCII */
    char queue[65];              /* queue name, NUL-terminated */
    char *payload_json;          /* NULL-terminated, owned by the message */
    size_t payload_len;          /* bytes excluding the trailing NUL */

    int64_t created_at_ms;
    int64_t available_at_ms;
    int64_t updated_at_ms;

    int attempts;
    int max_attempts;

    pq_message_state state;

    char receipt[33];            /* empty unless reserved */
    int64_t reserved_until_ms;   /* 0 unless reserved */

    char last_error[1024];       /* optional, may be empty */
} pq_message;

void pq_message_init(pq_message *m);
void pq_message_dispose(pq_message *m);

/* ---- Queue stats (spec §18) ------------------------------------------- */
typedef struct pq_queue_stats {
    char queue[65];
    int ready;
    int reserved;
    int dead_lettered;
    int total_active;
    int64_t oldest_ready_age_ms;
} pq_queue_stats;

/* ---- Service configuration --------------------------------------------- */
typedef struct pq_service_config {
    int64_t default_visibility_ms;
    int64_t min_visibility_ms;
    int64_t max_visibility_ms;
    int default_max_attempts;
    int64_t max_wait_ms;
    pq_clock *clock;             /* non-NULL */
    /* Optional: when non-NULL, the service uses the SQLite-backed
     * implementation; otherwise it falls back to the in-memory backend. */
    struct pq_repository *repository;
} pq_service_config;

/* ---- Service handle ---------------------------------------------------- */
typedef struct pq_service pq_service;

pq_service *pq_service_create(const pq_service_config *cfg, pq_error *err);
void pq_service_destroy(pq_service *svc);

/* ---- Publish (spec §14) ----------------------------------------------- */
typedef struct pq_publish_request {
    const char *queue_name;
    const char *payload_json;
    size_t payload_len;
    int max_attempts;            /* <= 0 → use service default */
} pq_publish_request;

pq_status pq_service_publish(pq_service *svc, const pq_publish_request *req,
                             pq_message *out, pq_error *err);

/* ---- Reserve (spec §15) -----------------------------------------------
 * wait_ms=0 returns immediately. Stage 2 implements wait_ms as "best
 * effort"; long-poll semantics land in stage 7. */
pq_status pq_service_reserve(pq_service *svc,
                             const char *queue_name,
                             int64_t visibility_timeout_ms,
                             int64_t wait_ms,
                             pq_message *out, pq_error *err);

/* ---- Ack (spec §16) --------------------------------------------------- */
pq_status pq_service_ack(pq_service *svc, const char *queue_name,
                         const char *message_id, const char *receipt,
                         pq_error *err);

/* ---- Nack (spec §17). reason may be NULL or empty. -------------------- */
pq_status pq_service_nack(pq_service *svc, const char *queue_name,
                          const char *message_id, const char *receipt,
                          const char *reason, pq_error *err);

/* ---- Stats (spec §18) ------------------------------------------------- */
pq_status pq_service_stats(pq_service *svc, const char *queue_name,
                           pq_queue_stats *out, pq_error *err);

#ifdef __cplusplus
}
#endif

#endif /* POCKETQUEUE_SERVICE_H */