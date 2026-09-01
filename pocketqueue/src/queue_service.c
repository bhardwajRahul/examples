/* queue_service.c - dispatch layer (spec §43).
 *
 * Public pq_service_* functions route to whichever backend the service
 * was created with. Two backends:
 *   - in-memory: stage 2 / unit tests
 *   - sqlite:    stage 4+ / production
 *
 * Both live in their own .c file and expose a `const pq_service_vtable`.
 * No queue-state lives here.
 */
#include "queue_service.h"

#include <stdlib.h>
#include <string.h>

#include "pocketqueue/pocketqueue.h"
#include "pocketqueue/service.h"

static int64_t now_ms_dispatch(pq_service *svc)
{
    return svc->clock->wall_time_ms(svc->clock->ctx);
}

/* ---- Public dispatch ------------------------------------------------ */

/* Forward declarations of backend init functions. Each takes a
 * pre-allocated pq_service and fills in state + vtable. Returns true
 * on success, false on failure (with err set). */
struct pq_inmem_state;
struct pq_sqlite_state;
bool pq_inmem_init(pq_service *svc, const pq_service_config *cfg, pq_error *err);
bool pq_sqlite_init(pq_service *svc, const pq_service_config *cfg, pq_error *err);

pq_service *pq_service_create(const pq_service_config *cfg, pq_error *err)
{
    if (cfg == NULL || cfg->clock == NULL) {
        pq_error_set(err, "invalid_argument",
                     "service: clock is required");
        return NULL;
    }
    pq_service *svc = calloc(1, sizeof(*svc));
    if (svc == NULL) {
        pq_error_set(err, "out_of_memory", "service: calloc failed");
        return NULL;
    }
    svc->cfg = *cfg;
    svc->clock = cfg->clock;
    svc->repo = cfg->repository;

    bool ok;
    if (cfg->repository != NULL) {
        ok = pq_sqlite_init(svc, cfg, err);
    } else {
        ok = pq_inmem_init(svc, cfg, err);
    }
    if (!ok) {
        free(svc);
        return NULL;
    }
    pq_notifier_init(&svc->notifier);
    return svc;
}

void pq_service_destroy(pq_service *svc)
{
    if (svc == NULL) return;
    /* Wake every parked long-poll waiter so they can observe shutdown. */
    pq_notifier_shutdown(&svc->notifier);
    if (svc->vtable && svc->vtable->destroy) svc->vtable->destroy(svc->state);
    pq_notifier_destroy(&svc->notifier);
    free(svc);
}

pq_status pq_service_publish(pq_service *svc, const pq_publish_request *req,
                             pq_message *out, pq_error *err)
{
    if (svc == NULL || svc->vtable == NULL) {
        pq_error_set(err, "internal_error", "service not initialised");
        return PQ_INTERNAL_ERROR;
    }
    pq_status s = svc->vtable->publish(svc->state, req, out, err);
    if (s == PQ_OK) pq_notifier_broadcast(&svc->notifier);
    return s;
}

pq_status pq_service_reserve(pq_service *svc,
                             const char *queue_name,
                             int64_t visibility_timeout_ms, int64_t wait_ms,
                             pq_message *out, pq_error *err)
{
    if (svc == NULL || svc->vtable == NULL) {
        pq_error_set(err, "internal_error", "service not initialised");
        return PQ_INTERNAL_ERROR;
    }
    /* Long-poll loop (spec §15.4 + §29). The backends implement an
     * immediate "best-effort" reserve; waiting is the dispatcher's job
     * so both backends share the same notifier without duplicating
     * the condvar dance. */
    int64_t start_ms = now_ms_dispatch(svc);
    int64_t deadline_ms = (wait_ms > 0) ? start_ms + wait_ms : start_ms;
    for (;;) {
        /* Always poll with wait_ms=0 — the dispatcher drives waiting. */
        pq_status s = svc->vtable->reserve(svc->state, queue_name,
                                            visibility_timeout_ms, 0,
                                            out, err);
        if (s != PQ_NOT_FOUND) return s;
        if (wait_ms <= 0) return PQ_NOT_FOUND;

        int64_t now = now_ms_dispatch(svc);
        if (now >= deadline_ms) return PQ_NOT_FOUND;

        int64_t next_event = deadline_ms;
        if (svc->vtable->next_event_ms) {
            int64_t q_next = svc->vtable->next_event_ms(svc->state,
                                                         queue_name);
            if (q_next > 0 && q_next < next_event) next_event = q_next;
        }
        /* If the queue's next reservation expiry already passed, retry
         * the loop immediately. */
        if (next_event <= now) continue;

        bool keep_going = pq_notifier_wait_until(&svc->notifier, svc->clock,
                                                  next_event);
        if (!keep_going) {
            pq_error_set(err, "server_shutting_down", "server is shutting down");
            return PQ_SHUTTING_DOWN;
        }
    }
}

pq_status pq_service_ack(pq_service *svc, const char *queue_name,
                         const char *message_id, const char *receipt,
                         pq_error *err)
{
    if (svc == NULL || svc->vtable == NULL) {
        pq_error_set(err, "internal_error", "service not initialised");
        return PQ_INTERNAL_ERROR;
    }
    pq_status s = svc->vtable->ack(svc->state, queue_name, message_id,
                                  receipt, err);
    /* Ack leaves a slot open — broadcast so a parked waiter can pick up
     * another message right away if one is in the queue. */
    if (s == PQ_OK) pq_notifier_broadcast(&svc->notifier);
    return s;
}

pq_status pq_service_nack(pq_service *svc, const char *queue_name,
                          const char *message_id, const char *receipt,
                          const char *reason, pq_error *err)
{
    if (svc == NULL || svc->vtable == NULL) {
        pq_error_set(err, "internal_error", "service not initialised");
        return PQ_INTERNAL_ERROR;
    }
    pq_status s = svc->vtable->nack(svc->state, queue_name, message_id,
                                   receipt, reason, err);
    /* Nack either requeues a message back to READY (which makes it
     * available again) or moves it to the dead-letter queue. Either way,
     * the set of available messages on some queue has changed; the
     * broadcast is cheap and noisy broadcasts are harmless. */
    if (s == PQ_OK) pq_notifier_broadcast(&svc->notifier);
    return s;
}

pq_status pq_service_stats(pq_service *svc, const char *queue_name,
                           pq_queue_stats *out, pq_error *err)
{
    if (svc == NULL || svc->vtable == NULL) {
        pq_error_set(err, "internal_error", "service not initialised");
        return PQ_INTERNAL_ERROR;
    }
    return svc->vtable->stats(svc->state, queue_name, out, err);
}

/* These are also exported via the public header, so keep them in sync. */
void pq_message_init(pq_message *m)
{
    if (m) memset(m, 0, sizeof(*m));
}
void pq_message_dispose(pq_message *m)
{
    if (m == NULL) return;
    free(m->payload_json);
    m->payload_json = NULL;
}

const char *pq_message_state_name(pq_message_state s)
{
    switch (s) {
        case PQ_MSG_READY:        return "ready";
        case PQ_MSG_RESERVED:     return "reserved";
        case PQ_MSG_DEAD_READY:   return "dead_ready";
        case PQ_MSG_DEAD_RESERVED:return "dead_reserved";
    }
    return "ready";
}