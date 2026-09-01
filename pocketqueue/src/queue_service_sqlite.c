/* queue_service_sqlite.c - SQLite-backed implementation of pq_service
 * (spec §43, spec §10).
 *
 * All operations go through prepared statements with bound parameters
 * (spec §34). The reservation path uses a single UPDATE…RETURNING
 * statement so two concurrent consumers cannot reserve the same
 * delivery attempt (spec §10.2). The repository's mutex serialises
 * every statement; long-poll waits release it before sleeping (stage 7).
 *
 * Expired reservations are recovered before every reserve/stats call
 * (spec §28) and at server startup.
 */
#include "queue_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "cJSON.h"
#include "clock.h"

#include "pocketqueue/pocketqueue.h"
#include "pocketqueue/service.h"

#include "random_util.h"
#include "sqlite_repository.h"
#include "str_util.h"

/* ----------------------------------------------------------------- */
/* Statement ids                                                      */
/* ----------------------------------------------------------------- */
enum st {
    ST_PUBLISH = 0,
    ST_RESERVE_LIVE,
    ST_RESERVE_DEAD,
    ST_ACK,
    ST_NACK_REQUEUE_READY,
    ST_NACK_REQUEUE_DEAD_READY,
    ST_NACK_TO_DEAD,
    ST_RECOVER_TO_READY,
    ST_RECOVER_DEAD_RESERVED,
    ST_RECOVER_TO_DEAD,
    ST_STATS_LIVE,
    ST_STATS_DEAD,
    ST_OLDEST_READY,
    ST_COUNT_VISIBLE,
    ST__MAX
};

struct sqlite_state {
    pq_repository *repo;
    pq_clock *clock;
    int default_max_attempts;
    int64_t min_visibility_ms;
    int64_t max_visibility_ms;
    int64_t max_wait_ms;
    sqlite3_stmt *stmt[ST__MAX];
};

/* ----------------------------------------------------------------- */
/* Helpers                                                            */
/* ----------------------------------------------------------------- */

static int64_t now_ms(struct sqlite_state *st)
{
    return st->clock->wall_time_ms(st->clock->ctx);
}

static void finalize_all(struct sqlite_state *st)
{
    for (int i = 0; i < ST__MAX; i++) {
        if (st->stmt[i] != NULL) {
            sqlite3_finalize(st->stmt[i]);
            st->stmt[i] = NULL;
        }
    }
}

static int prepare(sqlite3 *db, sqlite3_stmt **out, const char *sql)
{
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite prepare failed: %s\n%s\n",
                sqlite3_errmsg(db), sql);
        return -1;
    }
    return 0;
}

/* Read a row from the prepared SELECT/RETURNING statement into *m. The
 * caller must have just stepped the statement once (SQLITE_ROW). */
static void row_to_message_locked(sqlite3_stmt *stmt, pq_message *m)
{
    memset(m, 0, sizeof(*m));
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *qname = (const char *)sqlite3_column_text(stmt, 1);
    const char *payload = (const char *)sqlite3_column_text(stmt, 2);
    int state = sqlite3_column_int(stmt, 3);
    int64_t created = sqlite3_column_int64(stmt, 4);
    int64_t available = sqlite3_column_int64(stmt, 5);
    int attempts = sqlite3_column_int(stmt, 6);
    int max_attempts = sqlite3_column_int(stmt, 7);
    const char *receipt = (const char *)sqlite3_column_text(stmt, 8);
    int64_t reserved_until = sqlite3_column_int64(stmt, 9);
    const char *last_error = (const char *)sqlite3_column_text(stmt, 10);

    if (id) pq_str_copy(m->id, sizeof(m->id), id);
    if (qname) pq_str_copy(m->queue, sizeof(m->queue), qname);
    if (payload != NULL) {
        size_t n = strlen(payload);
        m->payload_json = malloc(n + 1);
        if (m->payload_json != NULL) {
            memcpy(m->payload_json, payload, n + 1);
            m->payload_len = n;
        }
    }
    m->state = (pq_message_state)state;
    m->created_at_ms = created;
    m->available_at_ms = available;
    m->attempts = attempts;
    m->max_attempts = max_attempts;
    if (receipt) pq_str_copy(m->receipt, sizeof(m->receipt), receipt);
    m->reserved_until_ms = reserved_until;
    if (last_error) pq_str_copy(m->last_error, sizeof(m->last_error), last_error);
}

static const char *kSelectColumns =
    "id, queue_name, payload_json, state, created_at_ms, available_at_ms, "
    "attempts, max_attempts, receipt_token, reserved_until_ms, last_error";

/* ----------------------------------------------------------------- */
/* Recovery of expired reservations (spec §28)                       */
/* ----------------------------------------------------------------- */

/* Recover expired reservations for ALL queues. Caller holds the repo
 * lock. This is invoked once at startup and before every reserve/stats. */
static void recover_expired_locked(struct sqlite_state *st)
{
    int64_t now = now_ms(st);

    /* DEAD_RESERVED → DEAD_READY: never expires into a new dead queue. */
    sqlite3_reset(st->stmt[ST_RECOVER_DEAD_RESERVED]);
    sqlite3_bind_int64(st->stmt[ST_RECOVER_DEAD_RESERVED], 1,
                        PQ_MSG_DEAD_READY);
    sqlite3_bind_int64(st->stmt[ST_RECOVER_DEAD_RESERVED], 2, now);
    sqlite3_step(st->stmt[ST_RECOVER_DEAD_RESERVED]);

    /* RESERVED → READY (attempts < max_attempts). */
    sqlite3_reset(st->stmt[ST_RECOVER_TO_READY]);
    sqlite3_bind_int(st->stmt[ST_RECOVER_TO_READY], 1, PQ_MSG_READY);
    sqlite3_bind_int64(st->stmt[ST_RECOVER_TO_READY], 2, now); /* available_at */
    sqlite3_bind_int64(st->stmt[ST_RECOVER_TO_READY], 3, now); /* updated_at */
    sqlite3_bind_int64(st->stmt[ST_RECOVER_TO_READY], 4, now); /* cutoff */
    sqlite3_step(st->stmt[ST_RECOVER_TO_READY]);

    /* RESERVED → DEAD_READY (attempts >= max_attempts). We move each
     * individually so we can compute the dead queue name. */
    sqlite3_stmt *sel = NULL;
    sqlite3_prepare_v2(sqlite3_db_handle(st->stmt[ST_RECOVER_TO_READY]),
        "SELECT id, queue_name FROM messages "
        "WHERE state = 1 AND reserved_until_ms <= ?1 "
        "  AND attempts >= max_attempts",
        -1, &sel, NULL);
    if (sel != NULL) {
        sqlite3_bind_int64(sel, 1, now);
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(sel, 0);
            const char *qname = (const char *)sqlite3_column_text(sel, 1);
            char dead_name[65];
            pq_str_copy(dead_name, sizeof(dead_name), qname);
            pq_str_append_dot_dead(dead_name, sizeof(dead_name));
            sqlite3_reset(st->stmt[ST_RECOVER_TO_DEAD]);
            sqlite3_bind_text(st->stmt[ST_RECOVER_TO_DEAD], 1, dead_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st->stmt[ST_RECOVER_TO_DEAD], 2, PQ_MSG_DEAD_READY);
            sqlite3_bind_int64(st->stmt[ST_RECOVER_TO_DEAD], 3, now);
            sqlite3_bind_int64(st->stmt[ST_RECOVER_TO_DEAD], 4, now);
            sqlite3_bind_text(st->stmt[ST_RECOVER_TO_DEAD], 5, id, -1, SQLITE_TRANSIENT);
            sqlite3_step(st->stmt[ST_RECOVER_TO_DEAD]);
            sqlite3_reset(st->stmt[ST_RECOVER_TO_DEAD]);
        }
        sqlite3_finalize(sel);
    }
}

/* ----------------------------------------------------------------- */
/* Backend operations                                                  */
/* ----------------------------------------------------------------- */

static void sqlite_destroy_impl(void *state)
{
    if (state == NULL) return;
    struct sqlite_state *st = state;
    finalize_all(st);
    free(st);
}

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
                                         struct sqlite_state *st,
                                         pq_error *err)
{
    (void)st;
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

static pq_status sqlite_publish(void *state, const pq_publish_request *req,
                                pq_message *out, pq_error *err)
{
    struct sqlite_state *st = state;
    pq_status s = validate_publish_inputs(req, st, err);
    if (s != PQ_OK) return s;

    char id[64];
    if (!pq_random_uuid_v7(id)) {
        pq_error_set(err, "internal_error", "uuid v7 generation failed");
        return PQ_INTERNAL_ERROR;
    }
    int max_attempts = req->max_attempts > 0 ? req->max_attempts
                                            : st->default_max_attempts;

    pq_repository_lock(st->repo);
    int64_t now = now_ms(st);

    sqlite3_reset(st->stmt[ST_PUBLISH]);
    sqlite3_bind_text(st->stmt[ST_PUBLISH], 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st->stmt[ST_PUBLISH], 2, req->queue_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st->stmt[ST_PUBLISH], 3, req->payload_json,
                      (int)req->payload_len, SQLITE_TRANSIENT);
    sqlite3_bind_int(st->stmt[ST_PUBLISH], 4, PQ_MSG_READY);
    sqlite3_bind_int64(st->stmt[ST_PUBLISH], 5, now);
    sqlite3_bind_int64(st->stmt[ST_PUBLISH], 6, now);
    sqlite3_bind_int64(st->stmt[ST_PUBLISH], 7, now);
    sqlite3_bind_int(st->stmt[ST_PUBLISH], 8, 0);     /* attempts */
    sqlite3_bind_int(st->stmt[ST_PUBLISH], 9, max_attempts);
    int rc = sqlite3_step(st->stmt[ST_PUBLISH]);
    sqlite3_reset(st->stmt[ST_PUBLISH]);
    if (rc != SQLITE_DONE) {
        pq_repository_unlock(st->repo);
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "publish failed: %s",
                 sqlite3_errmsg(sqlite3_db_handle(st->stmt[ST_PUBLISH])));
        pq_error_set(err, "database_error", msg);
        return PQ_DATABASE_ERROR;
    }

    /* Read the row back. */
    sqlite3_stmt *sel = NULL;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT %s FROM messages WHERE id = ?1", kSelectColumns);
    if (prepare(sqlite3_db_handle(st->stmt[ST_PUBLISH]), &sel, sql) != 0) {
        pq_repository_unlock(st->repo);
        pq_error_set(err, "internal_error", "could not re-select after publish");
        return PQ_INTERNAL_ERROR;
    }
    sqlite3_bind_text(sel, 1, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(sel);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(sel);
        pq_repository_unlock(st->repo);
        pq_error_set(err, "internal_error", "published message disappeared");
        return PQ_INTERNAL_ERROR;
    }
    row_to_message_locked(sel, out);
    sqlite3_finalize(sel);
    pq_repository_unlock(st->repo);
    return PQ_OK;
}

static pq_status validate_reserve_inputs(struct sqlite_state *st,
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

static pq_status sqlite_reserve(void *state, const char *queue_name,
                                int64_t visibility_ms, int64_t wait_ms,
                                pq_message *out, pq_error *err)
{
    struct sqlite_state *st = state;
    pq_status s = validate_reserve_inputs(st, queue_name, visibility_ms,
                                         wait_ms, err);
    if (s != PQ_OK) return s;

    pq_repository_lock(st->repo);
    recover_expired_locked(st);

    bool dead = (strstr(queue_name, ".dead") == queue_name + strlen(queue_name) - 5);
    int want = dead ? PQ_MSG_DEAD_READY : PQ_MSG_READY;
    int new_state = dead ? PQ_MSG_DEAD_RESERVED : PQ_MSG_RESERVED;

    char receipt[33];
    if (!pq_random_hex(receipt, 16)) {
        pq_repository_unlock(st->repo);
        pq_error_set(err, "internal_error", "receipt generation failed");
        return PQ_INTERNAL_ERROR;
    }

    int64_t now = now_ms(st);
    int64_t deadline = now + visibility_ms;

    int stmt_id = dead ? ST_RESERVE_DEAD : ST_RESERVE_LIVE;
    sqlite3_stmt *stmt = st->stmt[stmt_id];
    sqlite3_reset(stmt);
    /* bind order must match the SQL: state_set, receipt, deadline, updated_at,
     * state_want, queue, state_want_again, now */
    int p = 1;
    sqlite3_bind_int(stmt,    p++, new_state);
    sqlite3_bind_text(stmt,   p++, receipt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, p++, deadline);
    sqlite3_bind_int64(stmt, p++, now);
    sqlite3_bind_text(stmt,   p++, queue_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,    p++, want);
    sqlite3_bind_int64(stmt, p++, now);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(stmt);
        pq_repository_unlock(st->repo);
        return PQ_NOT_FOUND;
    }
    row_to_message_locked(stmt, out);
    sqlite3_reset(stmt);
    pq_repository_unlock(st->repo);
    (void)wait_ms; /* stage 7 */
    return PQ_OK;
}

static pq_status sqlite_ack(void *state, const char *queue_name,
                            const char *message_id, const char *receipt,
                            pq_error *err)
{
    struct sqlite_state *st = state;
    if (!pq_str_is_valid_queue_name(queue_name, true) ||
        message_id == NULL || receipt == NULL) {
        pq_error_set(err, "invalid_request",
                     "queue, message_id, and receipt are required");
        return PQ_INVALID_ARGUMENT;
    }
    pq_repository_lock(st->repo);
    /* Spec §16.3: apply normal expiration behavior before failing on
     * an expired reservation. If the user's reservation has already
     * timed out but no recent reserve/stats call recovered it, this
     * pass handles the transition; the receipt check below then fails
     * with 409 because the message is no longer reserved. */
    recover_expired_locked(st);

    sqlite3_reset(st->stmt[ST_ACK]);
    sqlite3_bind_text(st->stmt[ST_ACK], 1, message_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st->stmt[ST_ACK], 2, queue_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st->stmt[ST_ACK], 3, receipt, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st->stmt[ST_ACK]);
    int changes = sqlite3_changes(sqlite3_db_handle(st->stmt[ST_ACK]));
    sqlite3_reset(st->stmt[ST_ACK]);
    pq_repository_unlock(st->repo);
    if (rc != SQLITE_DONE) {
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "ack failed: %s",
                 sqlite3_errmsg(sqlite3_db_handle(st->stmt[ST_ACK])));
        pq_error_set(err, "database_error", msg);
        return PQ_DATABASE_ERROR;
    }
    if (changes == 0) {
        /* Could be: not found, not reserved, or receipt mismatch. Distinguish
         * "no such row" (NOT_FOUND) from "wrong state/receipt" (CONFLICT). */
        pq_repository_lock(st->repo);
        sqlite3_stmt *sel = NULL;
        pq_status result = PQ_NOT_FOUND;
        if (prepare(sqlite3_db_handle(st->stmt[ST_ACK]),
                    &sel, "SELECT state, receipt_token FROM messages "
                          "WHERE id = ?1") == 0) {
            sqlite3_bind_text(sel, 1, message_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(sel) == SQLITE_ROW) {
                int state = sqlite3_column_int(sel, 0);
                const char *cur_receipt =
                    (const char *)sqlite3_column_text(sel, 1);
                bool receipt_match = (cur_receipt != NULL &&
                                      strcmp(cur_receipt, receipt) == 0);
                bool state_ok = (state == PQ_MSG_RESERVED ||
                                 state == PQ_MSG_DEAD_RESERVED);
                if (!state_ok) {
                    pq_error_set(err, "reservation_conflict",
                                 "message is not currently reserved");
                    result = PQ_CONFLICT;
                } else if (!receipt_match) {
                    pq_error_set(err, "reservation_conflict",
                                 "receipt does not match");
                    result = PQ_CONFLICT;
                } else {
                    /* DELETE returned 0 even though state+receipt match —
                     * treat as conflict (shouldn't normally happen). */
                    result = PQ_CONFLICT;
                }
            } else {
                pq_error_set(err, "message_not_found", "no such message");
                result = PQ_NOT_FOUND;
            }
            sqlite3_finalize(sel);
        } else {
            pq_error_set(err, "internal_error", "ack lookup failed");
            result = PQ_INTERNAL_ERROR;
        }
        pq_repository_unlock(st->repo);
        return result;
    }
    return PQ_OK;
}

static pq_status sqlite_nack(void *state, const char *queue_name,
                             const char *message_id, const char *receipt,
                             const char *reason, pq_error *err)
{
    struct sqlite_state *st = state;
    if (!pq_str_is_valid_queue_name(queue_name, true) ||
        message_id == NULL || receipt == NULL) {
        pq_error_set(err, "invalid_request",
                     "queue, message_id, and receipt are required");
        return PQ_INVALID_ARGUMENT;
    }
    pq_repository_lock(st->repo);
    /* Spec §17.3 / §28: same expiration pass as ack. */
    recover_expired_locked(st);

    /* First, look up current state + attempts + max_attempts + queue. */
    sqlite3_stmt *sel = NULL;
    if (prepare(sqlite3_db_handle(st->stmt[ST_ACK]),
                &sel, "SELECT state, attempts, max_attempts, queue_name "
                      "FROM messages WHERE id = ?1") != 0) {
        pq_repository_unlock(st->repo);
        pq_error_set(err, "internal_error", "nack lookup failed");
        return PQ_INTERNAL_ERROR;
    }
    sqlite3_bind_text(sel, 1, message_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(sel);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(sel);
        pq_repository_unlock(st->repo);
        pq_error_set(err, "message_not_found", "no such message");
        return PQ_NOT_FOUND;
    }
    int msg_state = sqlite3_column_int(sel, 0);
    int attempts = sqlite3_column_int(sel, 1);
    int max_attempts = sqlite3_column_int(sel, 2);
    const char *qname = (const char *)sqlite3_column_text(sel, 3);
    (void)qname;
    sqlite3_finalize(sel);

    if (msg_state != PQ_MSG_RESERVED && msg_state != PQ_MSG_DEAD_RESERVED) {
        pq_repository_unlock(st->repo);
        pq_error_set(err, "reservation_conflict",
                     "message is not currently reserved");
        return PQ_CONFLICT;
    }

    int64_t now = now_ms(st);
    pq_status s = PQ_OK;

    if (msg_state == PQ_MSG_DEAD_RESERVED) {
        /* Nack from DEAD_RESERVED → DEAD_READY. */
        sqlite3_reset(st->stmt[ST_NACK_REQUEUE_DEAD_READY]);
        sqlite3_bind_int(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 1, PQ_MSG_DEAD_READY);
        sqlite3_bind_int64(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 2, now);
        if (reason != NULL) {
            sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 3,
                              reason, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 3);
        }
        sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 4,
                          message_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 5,
                          queue_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_DEAD_READY], 6,
                          receipt, -1, SQLITE_TRANSIENT);
        int rc2 = sqlite3_step(st->stmt[ST_NACK_REQUEUE_DEAD_READY]);
        sqlite3_reset(st->stmt[ST_NACK_REQUEUE_DEAD_READY]);
        if (rc2 != SQLITE_DONE || sqlite3_changes(sqlite3_db_handle(
                st->stmt[ST_NACK_REQUEUE_DEAD_READY])) == 0) {
            s = PQ_CONFLICT;
        }
    } else if (attempts >= max_attempts) {
        /* Move to dead-letter queue. */
        char dead_name[65];
        pq_str_copy(dead_name, sizeof(dead_name), queue_name);
        pq_str_append_dot_dead(dead_name, sizeof(dead_name));
        sqlite3_reset(st->stmt[ST_NACK_TO_DEAD]);
        sqlite3_bind_text(st->stmt[ST_NACK_TO_DEAD], 1, dead_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st->stmt[ST_NACK_TO_DEAD], 2, PQ_MSG_DEAD_READY);
        sqlite3_bind_int64(st->stmt[ST_NACK_TO_DEAD], 3, now);
        if (reason != NULL) {
            sqlite3_bind_text(st->stmt[ST_NACK_TO_DEAD], 4,
                              reason, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(st->stmt[ST_NACK_TO_DEAD], 4);
        }
        sqlite3_bind_int64(st->stmt[ST_NACK_TO_DEAD], 5, now);
        sqlite3_bind_text(st->stmt[ST_NACK_TO_DEAD], 6,
                          message_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st->stmt[ST_NACK_TO_DEAD], 7,
                          queue_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st->stmt[ST_NACK_TO_DEAD], 8,
                          receipt, -1, SQLITE_TRANSIENT);
        int rc2 = sqlite3_step(st->stmt[ST_NACK_TO_DEAD]);
        sqlite3_reset(st->stmt[ST_NACK_TO_DEAD]);
        if (rc2 != SQLITE_DONE || sqlite3_changes(sqlite3_db_handle(
                st->stmt[ST_NACK_TO_DEAD])) == 0) {
            s = PQ_CONFLICT;
        }
    } else {
        /* Requeue as READY. */
        sqlite3_reset(st->stmt[ST_NACK_REQUEUE_READY]);
        sqlite3_bind_int(st->stmt[ST_NACK_REQUEUE_READY], 1, PQ_MSG_READY);
        sqlite3_bind_int64(st->stmt[ST_NACK_REQUEUE_READY], 2, now);
        sqlite3_bind_int64(st->stmt[ST_NACK_REQUEUE_READY], 3, now);
        if (reason != NULL) {
            sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_READY], 4,
                              reason, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(st->stmt[ST_NACK_REQUEUE_READY], 4);
        }
        sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_READY], 5,
                          message_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_READY], 6,
                          queue_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st->stmt[ST_NACK_REQUEUE_READY], 7,
                          receipt, -1, SQLITE_TRANSIENT);
        int rc2 = sqlite3_step(st->stmt[ST_NACK_REQUEUE_READY]);
        sqlite3_reset(st->stmt[ST_NACK_REQUEUE_READY]);
        if (rc2 != SQLITE_DONE || sqlite3_changes(sqlite3_db_handle(
                st->stmt[ST_NACK_REQUEUE_READY])) == 0) {
            s = PQ_CONFLICT;
        }
    }
    pq_repository_unlock(st->repo);
    if (s != PQ_OK) {
        pq_error_set(err, "reservation_conflict",
                     "nack failed (state changed or receipt stale)");
        return s;
    }
    return PQ_OK;
}

static int count_state(sqlite3_stmt *stmt, const char *queue, int state)
{
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, queue, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, state);
    int n = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_reset(stmt);
    return n;
}

static int64_t oldest_ready_ms(sqlite3_stmt *stmt, const char *queue, int state)
{
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, queue, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, state);
    int64_t v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            v = sqlite3_column_int64(stmt, 0);
        }
    }
    sqlite3_reset(stmt);
    return v;
}

static pq_status sqlite_stats(void *state, const char *queue_name,
                              pq_queue_stats *out, pq_error *err)
{
    struct sqlite_state *st = state;
    if (!pq_str_is_valid_queue_name(queue_name, true)) {
        pq_error_set(err, "invalid_queue_name",
                     "queue name must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}");
        return PQ_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    pq_str_copy(out->queue, sizeof(out->queue), queue_name);

    pq_repository_lock(st->repo);
    recover_expired_locked(st);

    bool live = (strstr(queue_name, ".dead") != queue_name + strlen(queue_name) - 5);
    int64_t now = now_ms(st);

    if (live) {
        out->ready = count_state(st->stmt[ST_STATS_LIVE], queue_name, PQ_MSG_READY);
        out->reserved = count_state(st->stmt[ST_STATS_LIVE], queue_name, PQ_MSG_RESERVED);
        int64_t oldest = oldest_ready_ms(st->stmt[ST_OLDEST_READY], queue_name, PQ_MSG_READY);
        out->oldest_ready_age_ms = (oldest >= 0) ? (now - oldest) : 0;
        /* dead_lettered from .dead queue */
        char dead_name[65];
        pq_str_copy(dead_name, sizeof(dead_name), queue_name);
        pq_str_append_dot_dead(dead_name, sizeof(dead_name));
        out->dead_lettered =
            count_state(st->stmt[ST_STATS_DEAD], dead_name, PQ_MSG_DEAD_READY) +
            count_state(st->stmt[ST_STATS_DEAD], dead_name, PQ_MSG_DEAD_RESERVED);
    } else {
        /* .dead queue: DEAD_READY → ready, DEAD_RESERVED → reserved. */
        out->ready = count_state(st->stmt[ST_STATS_DEAD], queue_name, PQ_MSG_DEAD_READY);
        out->reserved = count_state(st->stmt[ST_STATS_DEAD], queue_name, PQ_MSG_DEAD_RESERVED);
        out->dead_lettered = 0;
        int64_t oldest = oldest_ready_ms(st->stmt[ST_OLDEST_READY],
                                         queue_name, PQ_MSG_DEAD_READY);
        out->oldest_ready_age_ms = (oldest >= 0) ? (now - oldest) : 0;
    }
    out->total_active = out->ready + out->reserved;
    pq_repository_unlock(st->repo);
    return PQ_OK;
}

static int64_t sqlite_next_event_ms(void *state, const char *queue_name);

const pq_service_vtable pq_sqlite_vtable = {
    .destroy = sqlite_destroy_impl,
    .publish = sqlite_publish,
    .reserve = sqlite_reserve,
    .ack     = sqlite_ack,
    .nack    = sqlite_nack,
    .stats   = sqlite_stats,
    .next_event_ms = sqlite_next_event_ms,
};

static int64_t sqlite_next_event_ms(void *state, const char *queue_name)
{
    struct sqlite_state *st = state;
    pq_repository_lock(st->repo);
    sqlite3_stmt *stmt = NULL;
    if (prepare(sqlite3_db_handle(st->stmt[ST_PUBLISH]), &stmt,
                "SELECT MIN(reserved_until_ms) FROM messages "
                "WHERE queue_name = ?1 AND state IN (1, 3)") != 0) {
        pq_repository_unlock(st->repo);
        return 0;
    }
    sqlite3_bind_text(stmt, 1, queue_name, -1, SQLITE_TRANSIENT);
    int64_t soonest = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            soonest = sqlite3_column_int64(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    pq_repository_unlock(st->repo);
    return soonest;
}

bool pq_sqlite_init(pq_service *svc, const pq_service_config *cfg, pq_error *err)
{
    if (cfg->repository == NULL) {
        pq_error_set(err, "invalid_argument",
                     "sqlite backend requires a repository");
        return false;
    }
    struct sqlite_state *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        pq_error_set(err, "out_of_memory", "sqlite_state: calloc failed");
        return false;
    }
    st->repo = cfg->repository;
    st->clock = cfg->clock;
    st->default_max_attempts = cfg->default_max_attempts > 0
                                  ? cfg->default_max_attempts : 3;
    st->min_visibility_ms = cfg->min_visibility_ms > 0
                                ? cfg->min_visibility_ms : 100;
    st->max_visibility_ms = cfg->max_visibility_ms > 0
                                ? cfg->max_visibility_ms : 600000;
    st->max_wait_ms = cfg->max_wait_ms > 0 ? cfg->max_wait_ms : 300000;

    sqlite3 *db = (sqlite3 *)pq_repository_handle_locked(st->repo);

    /* Publish. */
    const char *sql_publish =
        "INSERT INTO messages"
        " (id, queue_name, payload_json, state, created_at_ms, "
        "  available_at_ms, updated_at_ms, attempts, max_attempts) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";
    if (prepare(db, &st->stmt[ST_PUBLISH], sql_publish) != 0) goto fail;

    /* Reserve: UPDATE…RETURNING — atomic per spec §10.2. */
    char sql_reserve_live[1024];
    snprintf(sql_reserve_live, sizeof(sql_reserve_live),
        "UPDATE messages SET "
        "  state = ?1, receipt_token = ?2, reserved_until_ms = ?3, "
        "  attempts = attempts + 1, updated_at_ms = ?4 "
        "WHERE id = ("
        "  SELECT id FROM messages "
        "  WHERE queue_name = ?5 AND state = ?6 AND available_at_ms <= ?7 "
        "  ORDER BY available_at_ms, created_at_ms, id "
        "  LIMIT 1) "
        "RETURNING %s",
        kSelectColumns);
    if (prepare(db, &st->stmt[ST_RESERVE_LIVE], sql_reserve_live) != 0) goto fail;

    char sql_reserve_dead[1024];
    snprintf(sql_reserve_dead, sizeof(sql_reserve_dead),
        "UPDATE messages SET "
        "  state = ?1, receipt_token = ?2, reserved_until_ms = ?3, "
        "  attempts = attempts + 1, updated_at_ms = ?4 "
        "WHERE id = ("
        "  SELECT id FROM messages "
        "  WHERE queue_name = ?5 AND state = ?6 AND available_at_ms <= ?7 "
        "  ORDER BY available_at_ms, created_at_ms, id "
        "  LIMIT 1) "
        "RETURNING %s",
        kSelectColumns);
    if (prepare(db, &st->stmt[ST_RESERVE_DEAD], sql_reserve_dead) != 0) goto fail;

    /* Ack: deletes the row when state matches and receipt matches. */
    const char *sql_ack =
        "DELETE FROM messages "
        "WHERE id = ?1 AND queue_name = ?2 "
        "  AND receipt_token = ?3 "
        "  AND state IN (1, 3)";
    if (prepare(db, &st->stmt[ST_ACK], sql_ack) != 0) goto fail;

    /* Nack requeue (live → READY). */
    const char *sql_nack_ready =
        "UPDATE messages SET state = ?1, receipt_token = NULL, "
        "  reserved_until_ms = NULL, available_at_ms = ?2, "
        "  updated_at_ms = ?3, last_error = ?4 "
        "WHERE id = ?5 AND queue_name = ?6 AND receipt_token = ?7 "
        "  AND state = 1";
    if (prepare(db, &st->stmt[ST_NACK_REQUEUE_READY], sql_nack_ready) != 0)
        goto fail;

    /* Nack requeue (.dead → DEAD_READY). */
    const char *sql_nack_dead =
        "UPDATE messages SET state = ?1, receipt_token = NULL, "
        "  reserved_until_ms = NULL, updated_at_ms = ?2, last_error = ?3 "
        "WHERE id = ?4 AND queue_name = ?5 AND receipt_token = ?6 "
        "  AND state = 3";
    if (prepare(db, &st->stmt[ST_NACK_REQUEUE_DEAD_READY], sql_nack_dead) != 0)
        goto fail;

    /* Nack to dead-letter. */
    const char *sql_nack_to_dead =
        "UPDATE messages SET queue_name = ?1, state = ?2, "
        "  receipt_token = NULL, reserved_until_ms = NULL, "
        "  updated_at_ms = ?3, last_error = ?4, dead_lettered_at_ms = ?5 "
        "WHERE id = ?6 AND queue_name = ?7 AND receipt_token = ?8 "
        "  AND state = 1 AND attempts >= max_attempts";
    if (prepare(db, &st->stmt[ST_NACK_TO_DEAD], sql_nack_to_dead) != 0)
        goto fail;

    /* Recover expired → READY (attempts < max). */
    const char *sql_recover_ready =
        "UPDATE messages SET state = ?1, receipt_token = NULL, "
        "  reserved_until_ms = NULL, available_at_ms = ?2, "
        "  updated_at_ms = ?3 "
        "WHERE state = 1 AND reserved_until_ms <= ?4 "
        "  AND attempts < max_attempts";
    if (prepare(db, &st->stmt[ST_RECOVER_TO_READY], sql_recover_ready) != 0)
        goto fail;

    /* Recover DEAD_RESERVED → DEAD_READY. */
    const char *sql_recover_dead =
        "UPDATE messages SET state = ?1, receipt_token = NULL, "
        "  reserved_until_ms = NULL, updated_at_ms = ?2 "
        "WHERE state = 3 AND reserved_until_ms <= ?3";
    if (prepare(db, &st->stmt[ST_RECOVER_DEAD_RESERVED], sql_recover_dead) != 0)
        goto fail;

    /* Recover expired → DEAD_READY (attempts >= max). Bound by queue_name
     * would be ideal but recover_expired_locked walks all queues in one
     * SELECT then issues per-row UPDATEs that compute the destination. */
    const char *sql_recover_to_dead =
        "UPDATE messages SET queue_name = ?1, state = ?2, "
        "  receipt_token = NULL, reserved_until_ms = NULL, "
        "  updated_at_ms = ?3, dead_lettered_at_ms = ?4 "
        "WHERE id = ?5";
    if (prepare(db, &st->stmt[ST_RECOVER_TO_DEAD], sql_recover_to_dead) != 0)
        goto fail;

    /* Stats counters. */
    const char *sql_stats_live =
        "SELECT COUNT(*) FROM messages WHERE queue_name = ?1 AND state = ?2";
    if (prepare(db, &st->stmt[ST_STATS_LIVE], sql_stats_live) != 0) goto fail;

    const char *sql_stats_dead =
        "SELECT COUNT(*) FROM messages WHERE queue_name = ?1 AND state = ?2";
    if (prepare(db, &st->stmt[ST_STATS_DEAD], sql_stats_dead) != 0) goto fail;

    const char *sql_oldest =
        "SELECT MIN(created_at_ms) FROM messages "
        "WHERE queue_name = ?1 AND state = ?2";
    if (prepare(db, &st->stmt[ST_OLDEST_READY], sql_oldest) != 0) goto fail;

    (void)ST_COUNT_VISIBLE;

    svc->state = st;
    svc->vtable = &pq_sqlite_vtable;

    /* Recover any expired reservations left over from the previous run. */
    pq_repository_lock(svc->repo);
    recover_expired_locked(st);
    pq_repository_unlock(svc->repo);

    return true;

fail:
    finalize_all(st);
    free(st);
    return false;
}