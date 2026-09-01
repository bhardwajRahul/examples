/* queue_service_inmem.c - in-memory backend for pq_service.
 *
 * Per-queue linked list of pq_inmem_node, each wrapping a pq_message.
 * One mutex on the state struct serialises all operations. Used by
 * unit tests (no SQLite available) and as a fallback when the service
 * is created with cfg->repository == NULL.
 */
#include "queue_service.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "clock.h"

#include "pocketqueue/pocketqueue.h"
#include "pocketqueue/service.h"

#include "random_util.h"
#include "str_util.h"

/* ----------------------------------------------------------------- */
/* State                                                              */
/* ----------------------------------------------------------------- */

typedef struct pq_inmem_node {
    pq_message msg;
    struct pq_inmem_node *next;
} pq_inmem_node_impl;

typedef struct pq_inmem_queue_impl {
    char name[65];
    pq_inmem_node_impl *head;
    pq_inmem_node_impl *tail;
    size_t count;
    bool in_use;
} pq_inmem_queue_impl;

struct pq_inmem_state {
    pthread_mutex_t mu;
    pq_clock *clock;
    int default_max_attempts;
    int64_t min_visibility_ms;
    int64_t max_visibility_ms;
    int64_t max_wait_ms;
    pq_inmem_queue_impl queues[PQ_MAX_QUEUES];
};

/* ----------------------------------------------------------------- */
/* Helpers                                                            */
/* ----------------------------------------------------------------- */

static int64_t now_ms(struct pq_inmem_state *st)
{
    return st->clock->wall_time_ms(st->clock->ctx);
}

static pq_inmem_queue_impl *get_or_create_queue_locked(struct pq_inmem_state *st,
                                                      const char *name)
{
    for (int i = 0; i < PQ_MAX_QUEUES; i++) {
        if (st->queues[i].in_use && strcmp(st->queues[i].name, name) == 0) {
            return &st->queues[i];
        }
    }
    for (int i = 0; i < PQ_MAX_QUEUES; i++) {
        if (!st->queues[i].in_use) {
            pq_inmem_queue_impl *q = &st->queues[i];
            pq_str_copy(q->name, sizeof(q->name), name);
            q->head = q->tail = NULL;
            q->count = 0;
            q->in_use = true;
            return q;
        }
    }
    return NULL;
}

static pq_inmem_queue_impl *find_queue_locked(struct pq_inmem_state *st,
                                              const char *name)
{
    for (int i = 0; i < PQ_MAX_QUEUES; i++) {
        if (st->queues[i].in_use && strcmp(st->queues[i].name, name) == 0) {
            return &st->queues[i];
        }
    }
    return NULL;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t sufl = strlen(suffix);
    return sl >= sufl && strcmp(s + sl - sufl, suffix) == 0;
}

static bool is_dead_queue(const char *name)
{
    return ends_with(name, ".dead");
}

static void unlink_node_locked(pq_inmem_queue_impl *q,
                               pq_inmem_node_impl *prev,
                               pq_inmem_node_impl *n)
{
    if (prev == NULL) q->head = n->next;
    else              prev->next = n->next;
    if (q->tail == n) q->tail = prev;
    q->count--;
}

static void move_node_to_dead_locked(struct pq_inmem_state *st,
                                     pq_inmem_queue_impl *q_from,
                                     pq_inmem_node_impl *prev,
                                     pq_inmem_node_impl *n,
                                     int64_t now)
{
    char dest[65];
    if (is_dead_queue(n->msg.queue)) {
        pq_str_copy(dest, sizeof(dest), n->msg.queue);
    } else {
        pq_str_copy(dest, sizeof(dest), n->msg.queue);
        pq_str_append_dot_dead(dest, sizeof(dest));
    }
    pq_str_copy(n->msg.queue, sizeof(n->msg.queue), dest);

    pq_inmem_queue_impl *q_to = get_or_create_queue_locked(st, dest);
    if (q_to == NULL) {
        n->msg.state = PQ_MSG_READY;
        return;
    }
    if (q_to == q_from) {
        n->msg.state = PQ_MSG_DEAD_READY;
        n->msg.receipt[0] = '\0';
        n->msg.reserved_until_ms = 0;
        n->msg.updated_at_ms = now;
        return;
    }
    unlink_node_locked(q_from, prev, n);
    n->next = NULL;
    if (q_to->tail != NULL) q_to->tail->next = n;
    else                     q_to->head = n;
    q_to->tail = n;
    q_to->count++;
    n->msg.state = PQ_MSG_DEAD_READY;
    n->msg.receipt[0] = '\0';
    n->msg.reserved_until_ms = 0;
    n->msg.updated_at_ms = now;
}

/* Walk a queue and reset any expired reservations in place (spec §28).
 * Returns the number of nodes touched; safe to call on empty queues. */
static int expire_reservations_in_queue_locked(struct pq_inmem_state *st,
                                              pq_inmem_queue_impl *q,
                                              int64_t now)
{
    int touched = 0;
    pq_inmem_node_impl *prev = NULL;
    for (pq_inmem_node_impl *n = q->head; n != NULL; ) {
        pq_inmem_node_impl *next = n->next;
        if (n->msg.state == PQ_MSG_RESERVED &&
            n->msg.reserved_until_ms <= now) {
            if (n->msg.attempts >= n->msg.max_attempts) {
                move_node_to_dead_locked(st, q, prev, n, now);
            } else {
                n->msg.state = PQ_MSG_READY;
                n->msg.receipt[0] = '\0';
                n->msg.reserved_until_ms = 0;
                n->msg.updated_at_ms = now;
                prev = n;
            }
            touched++;
        } else {
            prev = n;
        }
        n = next;
    }
    return touched;
}

/* Run expire_reservations_in_queue_locked on the named queue (if any). */
static void expire_in_queue_locked(struct pq_inmem_state *st,
                                  const char *queue_name, int64_t now)
{
    pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
    if (q != NULL) {
        (void)expire_reservations_in_queue_locked(st, q, now);
    }
}

static void sum_queue_counts_locked(pq_inmem_queue_impl *q, bool live_view,
                                   int *ready, int *reserved,
                                   int *dead_lettered)
{
    for (pq_inmem_node_impl *n = q->head; n != NULL; n = n->next) {
        switch (n->msg.state) {
            case PQ_MSG_READY:        (*ready)++; break;
            case PQ_MSG_RESERVED:     (*reserved)++; break;
            case PQ_MSG_DEAD_READY:
                if (live_view) (*dead_lettered)++;
                else           (*ready)++;
                break;
            case PQ_MSG_DEAD_RESERVED:
                (*reserved)++;
                if (live_view) (*dead_lettered)++;
                break;
        }
    }
}

/* ----------------------------------------------------------------- */
/* Validation                                                         */
/* ----------------------------------------------------------------- */

static pq_status validate_payload_json(const char *data, size_t len,
                                       pq_error *err)
{
    if (data == NULL || len == 0) {
        pq_error_set(err, "invalid_request", "payload is required");
        return PQ_INVALID_ARGUMENT;
    }
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        const char *p = cJSON_GetErrorPtr();
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "JSON parse error near: %.40s",
                 p ? p : "(unknown)");
        pq_error_set(err, "invalid_json", msg);
        return PQ_INVALID_ARGUMENT;
    }
    cJSON_Delete(root);
    return PQ_OK;
}

static pq_status validate_publish_inputs(const pq_publish_request *req,
                                         pq_error *err)
{
    if (req == NULL) {
        pq_error_set(err, "invalid_request", "missing publish request");
        return PQ_INVALID_ARGUMENT;
    }
    if (!pq_str_is_valid_queue_name(req->queue_name, false)) {
        pq_error_set(err, "invalid_queue_name",
                     "queue name must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}");
        return PQ_INVALID_ARGUMENT;
    }
    if (req->max_attempts > 100) {
        pq_error_set(err, "invalid_request",
                     "max_attempts must be 1..100");
        return PQ_INVALID_ARGUMENT;
    }
    if (req->payload_len == 0 || req->payload_len > PQ_MAX_PAYLOAD_BYTES) {
        pq_error_set(err, "payload_too_large",
                     "payload exceeds size limit");
        return PQ_INVALID_ARGUMENT;
    }
    return validate_payload_json(req->payload_json, req->payload_len, err);
}

static pq_status validate_reserve_inputs(struct pq_inmem_state *st,
                                         const char *queue_name,
                                         int64_t visibility_ms,
                                         int64_t wait_ms,
                                         pq_error *err)
{
    if (!pq_str_is_valid_queue_name(queue_name, true)) {
        pq_error_set(err, "invalid_queue_name",
                     "queue name must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}");
        return PQ_INVALID_ARGUMENT;
    }
    if (visibility_ms < st->min_visibility_ms ||
        visibility_ms > st->max_visibility_ms) {
        pq_error_set(err, "invalid_request",
                     "visibility_timeout_ms out of range");
        return PQ_INVALID_ARGUMENT;
    }
    if (wait_ms < 0 || wait_ms > st->max_wait_ms) {
        pq_error_set(err, "invalid_request", "wait_ms out of range");
        return PQ_INVALID_ARGUMENT;
    }
    return PQ_OK;
}

/* ----------------------------------------------------------------- */
/* Backend operations                                                  */
/* ----------------------------------------------------------------- */

static void inmem_destroy_impl(void *state)
{
    if (state == NULL) return;
    struct pq_inmem_state *st = state;
    pthread_mutex_lock(&st->mu);
    for (int i = 0; i < PQ_MAX_QUEUES; i++) {
        if (!st->queues[i].in_use) continue;
        pq_inmem_node_impl *n = st->queues[i].head;
        while (n != NULL) {
            pq_inmem_node_impl *next = n->next;
            free(n->msg.payload_json);
            free(n);
            n = next;
        }
    }
    pthread_mutex_unlock(&st->mu);
    pthread_mutex_destroy(&st->mu);
    free(st);
}

static pq_status inmem_publish(void *state, const pq_publish_request *req,
                               pq_message *out, pq_error *err)
{
    struct pq_inmem_state *st = state;
    pq_status s = validate_publish_inputs(req, err);
    if (s != PQ_OK) return s;
    memset(out, 0, sizeof(*out));

    pq_inmem_node_impl *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        pq_error_set(err, "out_of_memory", "could not allocate node");
        return PQ_OUT_OF_MEMORY;
    }
    n->msg.payload_json = malloc(req->payload_len + 1);
    if (n->msg.payload_json == NULL) {
        free(n);
        pq_error_set(err, "out_of_memory", "could not allocate payload");
        return PQ_OUT_OF_MEMORY;
    }
    memcpy(n->msg.payload_json, req->payload_json, req->payload_len);
    n->msg.payload_json[req->payload_len] = '\0';
    n->msg.payload_len = req->payload_len;

    pq_random_uuid_v7(n->msg.id);
    pq_str_copy(n->msg.queue, sizeof(n->msg.queue), req->queue_name);
    n->msg.max_attempts = req->max_attempts > 0
                              ? req->max_attempts
                              : st->default_max_attempts;
    n->msg.attempts = 0;
    n->msg.state = PQ_MSG_READY;

    pthread_mutex_lock(&st->mu);
    int64_t now = now_ms(st);
    n->msg.created_at_ms = now;
    n->msg.available_at_ms = now;
    n->msg.updated_at_ms = now;

    pq_inmem_queue_impl *q = get_or_create_queue_locked(st, req->queue_name);
    if (q == NULL) {
        pthread_mutex_unlock(&st->mu);
        free(n->msg.payload_json);
        free(n);
        pq_error_set(err, "internal_error", "too many queues");
        return PQ_INTERNAL_ERROR;
    }
    n->next = NULL;
    if (q->tail != NULL) q->tail->next = n;
    else                 q->head = n;
    q->tail = n;
    q->count++;

    *out = n->msg;
    out->payload_json = malloc(n->msg.payload_len + 1);
    if (out->payload_json == NULL) {
        pthread_mutex_unlock(&st->mu);
        pq_error_set(err, "out_of_memory", "could not copy payload");
        return PQ_OUT_OF_MEMORY;
    }
    memcpy(out->payload_json, n->msg.payload_json, n->msg.payload_len + 1);
    pthread_mutex_unlock(&st->mu);
    return PQ_OK;
}

static pq_status inmem_reserve(void *state, const char *queue_name,
                               int64_t visibility_ms, int64_t wait_ms,
                               pq_message *out, pq_error *err)
{
    struct pq_inmem_state *st = state;
    pq_status s = validate_reserve_inputs(st, queue_name, visibility_ms,
                                         wait_ms, err);
    if (s != PQ_OK) return s;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&st->mu);
    int64_t now = now_ms(st);
    pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
    if (q == NULL) {
        pthread_mutex_unlock(&st->mu);
        return PQ_NOT_FOUND;
    }

    /* Pass 1: expire stale reservations (spec §28). */
    (void)expire_reservations_in_queue_locked(st, q, now);

    /* Pass 2: pick the oldest eligible. */
    bool dead = is_dead_queue(queue_name);
    pq_message_state want = dead ? PQ_MSG_DEAD_READY : PQ_MSG_READY;
    pq_inmem_node_impl *best = NULL;
    for (pq_inmem_node_impl *n = q->head; n != NULL; n = n->next) {
        if (n->msg.state != want) continue;
        if (n->msg.available_at_ms > now) continue;
        if (best == NULL) { best = n; continue; }
        if (n->msg.available_at_ms < best->msg.available_at_ms) { best = n; continue; }
        if (n->msg.available_at_ms > best->msg.available_at_ms) continue;
        if (n->msg.created_at_ms < best->msg.created_at_ms) { best = n; continue; }
        if (n->msg.created_at_ms > best->msg.created_at_ms) continue;
        /* Linked-list order is the stable tie-breaker. */
    }

    if (best == NULL) {
        pthread_mutex_unlock(&st->mu);
        return PQ_NOT_FOUND;
    }

    pq_random_hex(best->msg.receipt, 16);
    best->msg.state = dead ? PQ_MSG_DEAD_RESERVED : PQ_MSG_RESERVED;
    best->msg.reserved_until_ms = now + visibility_ms;
    best->msg.attempts++;
    best->msg.updated_at_ms = now;

    *out = best->msg;
    out->payload_json = malloc(best->msg.payload_len + 1);
    if (out->payload_json == NULL) {
        best->msg.state = want;
        best->msg.receipt[0] = '\0';
        best->msg.reserved_until_ms = 0;
        best->msg.attempts--;
        pthread_mutex_unlock(&st->mu);
        pq_error_set(err, "out_of_memory", "could not copy payload");
        return PQ_OUT_OF_MEMORY;
    }
    memcpy(out->payload_json, best->msg.payload_json, best->msg.payload_len + 1);
    pthread_mutex_unlock(&st->mu);
    (void)wait_ms; /* stage 7 */
    return PQ_OK;
}

static pq_status inmem_ack(void *state, const char *queue_name,
                           const char *message_id, const char *receipt,
                           pq_error *err)
{
    struct pq_inmem_state *st = state;
    if (!pq_str_is_valid_queue_name(queue_name, true) ||
        message_id == NULL || receipt == NULL) {
        pq_error_set(err, "invalid_request",
                     "queue, message_id, and receipt are required");
        return PQ_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&st->mu);
    /* Spec §16.3: apply normal expiration behavior before failing on
     * an expired reservation. If a reservation has expired but not yet
     * been recovered (because no reserve/stats call has run since), this
     * pass returns the message to READY (or to dead-letter if max
     * attempts were reached). The subsequent state check then fails with
     * "message is not currently reserved" — the spec's intended behavior. */
    int64_t now = now_ms(st);
    expire_in_queue_locked(st, queue_name, now);

    pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
    if (q == NULL) {
        pthread_mutex_unlock(&st->mu);
        pq_error_set(err, "message_not_found", "queue or message not found");
        return PQ_NOT_FOUND;
    }
    pq_inmem_node_impl *prev = NULL;
    for (pq_inmem_node_impl *n = q->head; n != NULL; prev = n, n = n->next) {
        if (strcmp(n->msg.id, message_id) != 0) continue;
        if (n->msg.state != PQ_MSG_RESERVED && n->msg.state != PQ_MSG_DEAD_RESERVED) {
            pthread_mutex_unlock(&st->mu);
            pq_error_set(err, "reservation_conflict",
                         "message is not currently reserved");
            return PQ_CONFLICT;
        }
        if (strcmp(n->msg.receipt, receipt) != 0) {
            pthread_mutex_unlock(&st->mu);
            pq_error_set(err, "reservation_conflict",
                         "receipt does not match");
            return PQ_CONFLICT;
        }
        unlink_node_locked(q, prev, n);
        free(n->msg.payload_json);
        free(n);
        pthread_mutex_unlock(&st->mu);
        return PQ_OK;
    }
    pthread_mutex_unlock(&st->mu);
    pq_error_set(err, "message_not_found", "no such message");
    return PQ_NOT_FOUND;
}

static pq_status inmem_nack(void *state, const char *queue_name,
                            const char *message_id, const char *receipt,
                            const char *reason, pq_error *err)
{
    struct pq_inmem_state *st = state;
    if (!pq_str_is_valid_queue_name(queue_name, true) ||
        message_id == NULL || receipt == NULL) {
        pq_error_set(err, "invalid_request",
                     "queue, message_id, and receipt are required");
        return PQ_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&st->mu);
    /* Spec §17.3 + §28: same expiration pass as ack. */
    int64_t now = now_ms(st);
    expire_in_queue_locked(st, queue_name, now);

    pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
    if (q == NULL) {
        pthread_mutex_unlock(&st->mu);
        pq_error_set(err, "message_not_found", "queue or message not found");
        return PQ_NOT_FOUND;
    }
    pq_inmem_node_impl *prev = NULL;
    for (pq_inmem_node_impl *n = q->head; n != NULL; prev = n, n = n->next) {
        if (strcmp(n->msg.id, message_id) != 0) continue;
        if (n->msg.state != PQ_MSG_RESERVED && n->msg.state != PQ_MSG_DEAD_RESERVED) {
            pthread_mutex_unlock(&st->mu);
            pq_error_set(err, "reservation_conflict",
                         "message is not currently reserved");
            return PQ_CONFLICT;
        }
        if (strcmp(n->msg.receipt, receipt) != 0) {
            pthread_mutex_unlock(&st->mu);
            pq_error_set(err, "reservation_conflict",
                         "receipt does not match");
            return PQ_CONFLICT;
        }

        if (n->msg.state == PQ_MSG_DEAD_RESERVED) {
            n->msg.state = PQ_MSG_DEAD_READY;
            n->msg.receipt[0] = '\0';
            n->msg.reserved_until_ms = 0;
            n->msg.updated_at_ms = now;
            if (reason != NULL) {
                pq_str_copy(n->msg.last_error, sizeof(n->msg.last_error), reason);
            }
            pthread_mutex_unlock(&st->mu);
            return PQ_OK;
        }

        if (reason != NULL) {
            pq_str_copy(n->msg.last_error, sizeof(n->msg.last_error), reason);
        }
        if (n->msg.attempts >= n->msg.max_attempts) {
            move_node_to_dead_locked(st, q, prev, n, now);
            pthread_mutex_unlock(&st->mu);
            return PQ_OK;
        }
        n->msg.state = PQ_MSG_READY;
        n->msg.receipt[0] = '\0';
        n->msg.reserved_until_ms = 0;
        n->msg.available_at_ms = now;
        n->msg.updated_at_ms = now;
        pthread_mutex_unlock(&st->mu);
        return PQ_OK;
    }
    pthread_mutex_unlock(&st->mu);
    pq_error_set(err, "message_not_found", "no such message");
    return PQ_NOT_FOUND;
}

static pq_status inmem_stats(void *state, const char *queue_name,
                             pq_queue_stats *out, pq_error *err)
{
    struct pq_inmem_state *st = state;
    if (!pq_str_is_valid_queue_name(queue_name, true)) {
        pq_error_set(err, "invalid_queue_name",
                     "queue name must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}");
        return PQ_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    pq_str_copy(out->queue, sizeof(out->queue), queue_name);

    pthread_mutex_lock(&st->mu);
    int64_t now = now_ms(st);
    int64_t oldest = -1;
    int ready = 0, reserved = 0, dead_lettered = 0;

    /* Spec §28: stats must reflect recovery first. */
    expire_in_queue_locked(st, queue_name, now);

    bool live = !is_dead_queue(queue_name);
    if (live) {
        pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
        if (q != NULL) {
            sum_queue_counts_locked(q, true, &ready, &reserved, &dead_lettered);
            for (pq_inmem_node_impl *n = q->head; n != NULL; n = n->next) {
                if (n->msg.state == PQ_MSG_READY &&
                    (oldest < 0 || n->msg.created_at_ms < oldest)) {
                    oldest = n->msg.created_at_ms;
                }
            }
        }
        char dead_name[65];
        pq_str_copy(dead_name, sizeof(dead_name), queue_name);
        pq_str_append_dot_dead(dead_name, sizeof(dead_name));
        pq_inmem_queue_impl *qd = find_queue_locked(st, dead_name);
        if (qd != NULL) {
            int d_ready = 0, d_reserved = 0, d_dead = 0;
            sum_queue_counts_locked(qd, true, &d_ready, &d_reserved, &d_dead);
            dead_lettered += d_dead;
            (void)d_ready; (void)d_reserved;
        }
    } else {
        pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
        if (q != NULL) {
            int d_ready = 0, d_reserved = 0, d_dead = 0;
            sum_queue_counts_locked(q, false, &d_ready, &d_reserved, &d_dead);
            ready = d_ready;
            reserved = d_reserved;
            dead_lettered = 0;
            for (pq_inmem_node_impl *n = q->head; n != NULL; n = n->next) {
                if (n->msg.state == PQ_MSG_DEAD_READY &&
                    (oldest < 0 || n->msg.created_at_ms < oldest)) {
                    oldest = n->msg.created_at_ms;
                }
            }
        }
    }
    pthread_mutex_unlock(&st->mu);

    out->ready = ready;
    out->reserved = reserved;
    out->dead_lettered = dead_lettered;
    out->total_active = ready + reserved;
    if (oldest >= 0) out->oldest_ready_age_ms = now - oldest;
    return PQ_OK;
}

static int64_t inmem_next_event_ms(void *state, const char *queue_name)
{
    struct pq_inmem_state *st = state;
    pthread_mutex_lock(&st->mu);
    int64_t soonest = 0;
    pq_inmem_queue_impl *q = find_queue_locked(st, queue_name);
    if (q != NULL) {
        for (pq_inmem_node_impl *n = q->head; n != NULL; n = n->next) {
            if (n->msg.state == PQ_MSG_RESERVED ||
                n->msg.state == PQ_MSG_DEAD_RESERVED) {
                if (soonest == 0 || n->msg.reserved_until_ms < soonest) {
                    soonest = n->msg.reserved_until_ms;
                }
            }
        }
    }
    pthread_mutex_unlock(&st->mu);
    return soonest;
}

const pq_service_vtable pq_inmem_vtable = {
    .destroy = inmem_destroy_impl,
    .publish = inmem_publish,
    .reserve = inmem_reserve,
    .ack     = inmem_ack,
    .nack    = inmem_nack,
    .stats   = inmem_stats,
    .next_event_ms = inmem_next_event_ms,
};

bool pq_inmem_init(pq_service *svc, const pq_service_config *cfg, pq_error *err)
{
    struct pq_inmem_state *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        pq_error_set(err, "out_of_memory", "inmem: calloc failed");
        return false;
    }
    pthread_mutex_init(&st->mu, NULL);
    st->clock = cfg->clock;
    st->default_max_attempts = cfg->default_max_attempts > 0
                                  ? cfg->default_max_attempts : 3;
    st->min_visibility_ms = cfg->min_visibility_ms > 0
                                ? cfg->min_visibility_ms : 100;
    st->max_visibility_ms = cfg->max_visibility_ms > 0
                                ? cfg->max_visibility_ms : 600000;
    st->max_wait_ms = cfg->max_wait_ms > 0 ? cfg->max_wait_ms : 300000;

    svc->state = st;
    svc->vtable = &pq_inmem_vtable;
    return true;
}