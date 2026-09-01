/* http_routes.c - spec §13-§22 dispatch on top of pq_service.
 *
 * The HTTP layer holds no queue-state of its own: every operation calls
 * pq_service_*, then formats the response.
 */
#include "http_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

#include "json_util.h"
#include "str_util.h"

/* ---- Status → HTTP status + (code, body) mapping ---------------------- */
typedef struct {
    pq_status s;
    int http;
    const char *code;
} status_map;

static const status_map kStatusMap[] = {
    {PQ_OK,                   200, ""},
    {PQ_INVALID_ARGUMENT,     400, "invalid_request"},
    {PQ_NOT_FOUND,            404, "message_not_found"},
    {PQ_CONFLICT,             409, "reservation_conflict"},
    {PQ_DATABASE_BUSY,        503, "database_busy"},
    {PQ_DATABASE_ERROR,       500, "database_error"},
    {PQ_OUT_OF_MEMORY,        500, "out_of_memory"},
    {PQ_INTERNAL_ERROR,       500, "internal_error"},
    {PQ_SHUTTING_DOWN,        503, "server_shutting_down"},
};

/* Forward declarations: status_to_error calls into make_json_response. */
static pq_http_response make_json_response(int status, cJSON *body,
                                          const char *extra_header_name,
                                          const char *extra_header_value);

static void status_to_error(pq_status s, pq_error *err, pq_http_response *out)
{
    for (size_t i = 0; i < sizeof(kStatusMap) / sizeof(kStatusMap[0]); i++) {
        if (kStatusMap[i].s == s) {
            cJSON *body = pq_json_new_error(kStatusMap[i].code,
                                           err->message[0] ? err->message : kStatusMap[i].code,
                                           NULL);
            *out = make_json_response(kStatusMap[i].http, body, NULL, NULL);
            return;
        }
    }
    /* Fallback. */
    cJSON *body = pq_json_new_error("internal_error", "unknown status", NULL);
    *out = make_json_response(500, body, NULL, NULL);
}

/* ISO 8601 UTC with milliseconds, e.g. 2026-07-15T12:30:45.123Z. */
static void format_iso8601_ms(int64_t ms_epoch, char out[40])
{
    time_t sec = (time_t)(ms_epoch / 1000);
    int ms = (int)(ms_epoch % 1000);
    struct tm tm;
    gmtime_r(&sec, &tm);
    snprintf(out, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* ---- Path matching -------------------------------------------------- */
/* Matches one of:
 *   /queues/{q}/messages                → suffix_out = NULL
 *   /queues/{q}/messages/{id}/ack       → suffix_out = "ack"
 *   /queues/{q}/messages/{id}/nack      → suffix_out = "nack"
 *   /queues/{q}/stats                   → suffix_out = "stats"
 * On match returns 1; otherwise 0. queue_out / id_out are NUL-terminated. */
static int match_queue_path(const char *path, char *queue_out, size_t q_size,
                            char *id_out, size_t i_size, const char **suffix_out)
{
    if (strncmp(path, "/queues/", 8) != 0) return 0;
    const char *p = path + 8;
    const char *slash = strchr(p, '/');
    if (slash == NULL) return 0;
    size_t q_len = (size_t)(slash - p);
    if (q_len == 0 || q_len >= q_size) return 0;
    memcpy(queue_out, p, q_len);
    queue_out[q_len] = '\0';

    const char *rest = slash + 1;
    *id_out = '\0';
    *suffix_out = NULL;

    if (strcmp(rest, "stats") == 0) {
        *suffix_out = "stats";
        return 1;
    }
    if (strncmp(rest, "messages", 8) != 0) return 0;
    rest += 8;
    if (*rest == '\0') return 1; /* /queues/{q}/messages */
    if (*rest != '/') return 0;
    rest++;
    const char *slash2 = strchr(rest, '/');
    if (slash2 == NULL) return 0;
    size_t id_len = (size_t)(slash2 - rest);
    if (id_len == 0 || id_len >= i_size) return 0;
    memcpy(id_out, rest, id_len);
    id_out[id_len] = '\0';
    *suffix_out = slash2 + 1;
    return 1;
}

/* ---- Query parameter helpers ---------------------------------------- */
static int64_t query_int64(const char *query, const char *name, int64_t defv)
{
    if (query == NULL) return defv;
    size_t namelen = strlen(name);
    const char *p = query;
    while (*p) {
        if (strncmp(p, name, namelen) == 0 && p[namelen] == '=') {
            const char *v = p + namelen + 1;
            const char *amp = strchr(v, '&');
            char buf[32];
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            if (vlen >= sizeof(buf)) return defv;
            memcpy(buf, v, vlen);
            buf[vlen] = '\0';
            int64_t out;
            if (pq_str_parse_int64(buf, &out)) return out;
            return defv;
        }
        const char *amp = strchr(p, '&');
        if (amp == NULL) break;
        p = amp + 1;
    }
    return defv;
}

/* ---- Helpers to build publish/reserve/ack JSON bodies --------------- */
static cJSON *build_message_json(const pq_message *m, bool include_payload)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", m->id);
    cJSON_AddStringToObject(root, "queue", m->queue);
    if (include_payload && m->payload_json != NULL) {
        /* The payload was already validated as JSON before storage, so
         * cJSON_Parse should succeed; fall back to a string otherwise. */
        cJSON *p = cJSON_Parse(m->payload_json);
        if (p != NULL) cJSON_AddItemToObject(root, "payload", p);
        else cJSON_AddStringToObject(root, "payload", m->payload_json);
    }
    char ts[40];
    format_iso8601_ms(m->created_at_ms, ts);
    cJSON_AddStringToObject(root, "created_at", ts);
    format_iso8601_ms(m->available_at_ms, ts);
    cJSON_AddStringToObject(root, "available_at", ts);
    cJSON_AddNumberToObject(root, "attempts", m->attempts);
    cJSON_AddNumberToObject(root, "max_attempts", m->max_attempts);
    if (m->receipt[0] != '\0') {
        cJSON_AddStringToObject(root, "receipt", m->receipt);
        format_iso8601_ms(m->reserved_until_ms, ts);
        cJSON_AddStringToObject(root, "reserved_until", ts);
    }
    return root;
}

static void dispose_response(pq_http_response *resp)
{
    if (resp == NULL) return;
    free(resp->body);
    free(resp->extra_header_name);
    free(resp->extra_header_value);
    resp->body = NULL;
    resp->extra_header_name = NULL;
    resp->extra_header_value = NULL;
}

/* Forward declaration: status_to_error uses this to build a structured
 * error response. */
static pq_http_response make_json_response(int status, cJSON *body,
                                          const char *extra_header_name,
                                          const char *extra_header_value);

static pq_http_response make_json_response(int status, cJSON *body,
                                          const char *extra_header_name,
                                          const char *extra_header_value)
{
    size_t len = 0;
    char *text = pq_json_dump(body, &len);
    cJSON_Delete(body);
    pq_http_response out = {
        .status_code = status,
        .content_type = "application/json",
        .body = text,
        .body_length = len,
        .extra_header_name = NULL,
        .extra_header_value = NULL,
    };
    /* Copy optional extra header. The caller is typically about to
     * return, so pointing at its stack-local string is unsafe under
     * ASan (stack-use-after-return). The response struct owns these
     * copies; release via pq_http_response_dispose. */
    if (extra_header_name != NULL) {
        out.extra_header_name = strdup(extra_header_name);
        if (extra_header_value != NULL) {
            out.extra_header_value = strdup(extra_header_value);
        }
    }
    return out;
}

/* ---- Per-endpoint handlers ----------------------------------------- */

/* Every pq_http_response we hand to a caller must be fully zero-initialised
 * for its body/extra_header_* fields, otherwise pq_http_routes_dispose
 * would call free() on stack garbage. Use this helper. */
static pq_http_response empty_response(int status_code)
{
    pq_http_response r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    return r;
}

pq_http_response pq_text_response(int status_code,
                                  const char *content_type,
                                  const char *body)
{
    pq_http_response r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.content_type = content_type;
    if (body != NULL) {
        size_t n = strlen(body);
        char *copy = malloc(n + 1);
        if (copy != NULL) {
            memcpy(copy, body, n + 1);
            r.body = copy;
            r.body_length = n;
        }
    }
    return r;
}

static pq_http_response handle_publish(pq_service *svc,
                                       const char *queue_name,
                                       const pq_http_request *req)
{
    pq_error err = {0};
    cJSON *body = pq_json_parse(req->body, req->body_length, false, &err);
    if (body == NULL) {
        pq_http_response out;
        status_to_error(PQ_INVALID_ARGUMENT, &err, &out);
        return out;
    }

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(body, "payload");
    if (payload == NULL) {
        cJSON_Delete(body);
        pq_error_set(&err, "invalid_request", "payload is required");
        pq_http_response out;
        status_to_error(PQ_INVALID_ARGUMENT, &err, &out);
        return out;
    }
    char *payload_text = cJSON_PrintUnformatted(payload);
    if (payload_text == NULL) {
        cJSON_Delete(body);
        pq_error_set(&err, "internal_error", "could not serialise payload");
        pq_http_response out;
        status_to_error(PQ_INTERNAL_ERROR, &err, &out);
        return out;
    }
    size_t payload_len = strlen(payload_text);

    int max_attempts = -1;
    cJSON *ma = cJSON_GetObjectItemCaseSensitive(body, "max_attempts");
    if (ma != NULL && cJSON_IsNumber(ma)) {
        max_attempts = (int)ma->valuedouble;
    }

    pq_publish_request preq = {
        .queue_name = queue_name,
        .payload_json = payload_text,
        .payload_len = payload_len,
        .max_attempts = max_attempts,
    };

    pq_message m;
    pq_status s = pq_service_publish(svc, &preq, &m, &err);
    free(payload_text);
    cJSON_Delete(body);

    if (s != PQ_OK) {
        pq_message_dispose(&m);
        pq_http_response out;
        status_to_error(s, &err, &out);
        return out;
    }

    /* Reject unknown top-level keys (spec §14: "Unknown top-level properties
     * SHOULD be rejected"). */
    /* Skipping for stage 3 — checked in stage 5 if needed. */

    char loc_header[256];
    snprintf(loc_header, sizeof(loc_header), "/queues/%s/messages/%s",
             queue_name, m.id);

    /* Build the response body. We need a separate cJSON for the payload
     * because the parsed body (and its 'payload' child) is freed below. */
    cJSON *payload_copy = cJSON_Parse(m.payload_json ? m.payload_json : "null");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", m.id);
    cJSON_AddStringToObject(resp, "queue", m.queue);
    cJSON_AddItemToObject(resp, "payload", payload_copy);
    char ts[40];
    format_iso8601_ms(m.created_at_ms, ts);
    cJSON_AddStringToObject(resp, "created_at", ts);
    format_iso8601_ms(m.available_at_ms, ts);
    cJSON_AddStringToObject(resp, "available_at", ts);
    cJSON_AddNumberToObject(resp, "attempts", m.attempts);
    cJSON_AddNumberToObject(resp, "max_attempts", m.max_attempts);
    pq_message_dispose(&m);

    return make_json_response(201, resp, "Location", loc_header);
}

static pq_http_response handle_reserve(pq_service *svc, int64_t default_visibility_ms,
                                       const char *queue_name,
                                       const pq_http_request *req)
{
    int64_t wait_ms = query_int64(req->query, "wait_ms", 0);
    int64_t visibility_ms = query_int64(req->query, "visibility_timeout_ms", 0);
    if (visibility_ms <= 0) visibility_ms = default_visibility_ms;
    pq_error err = {0};
    pq_message m;
    pq_status s = pq_service_reserve(svc, queue_name, visibility_ms, wait_ms, &m, &err);
    if (s == PQ_NOT_FOUND) {
        /* Long-poll timed out without a message arriving (stage 7). */
        return empty_response(204);
    }
    if (s != PQ_OK) {
        pq_http_response out;
        status_to_error(s, &err, &out);
        return out;
    }
    cJSON *body = build_message_json(&m, true);
    pq_message_dispose(&m);
    return make_json_response(200, body, NULL, NULL);
}

static pq_http_response handle_ack(pq_service *svc, const char *queue_name,
                                   const char *message_id,
                                   const pq_http_request *req)
{
    pq_error err = {0};
    cJSON *body = pq_json_parse(req->body, req->body_length, false, &err);
    if (body == NULL) {
        pq_http_response out;
        status_to_error(PQ_INVALID_ARGUMENT, &err, &out);
        return out;
    }
    cJSON *receipt = cJSON_GetObjectItemCaseSensitive(body, "receipt");
    if (receipt == NULL || !cJSON_IsString(receipt)) {
        cJSON_Delete(body);
        pq_error_set(&err, "invalid_request", "receipt is required");
        pq_http_response out;
        status_to_error(PQ_INVALID_ARGUMENT, &err, &out);
        return out;
    }
    pq_status s = pq_service_ack(svc, queue_name, message_id,
                                  receipt->valuestring, &err);
    cJSON_Delete(body);
    if (s != PQ_OK) {
        pq_http_response out;
        status_to_error(s, &err, &out);
        return out;
    }
    return empty_response(204);
}

static pq_http_response handle_nack(pq_service *svc, const char *queue_name,
                                    const char *message_id,
                                    const pq_http_request *req)
{
    pq_error err = {0};
    cJSON *body = pq_json_parse(req->body, req->body_length, false, &err);
    if (body == NULL) {
        pq_http_response out;
        status_to_error(PQ_INVALID_ARGUMENT, &err, &out);
        return out;
    }
    cJSON *receipt = cJSON_GetObjectItemCaseSensitive(body, "receipt");
    const char *receipt_s = (receipt != NULL && cJSON_IsString(receipt))
                                ? receipt->valuestring : NULL;
    if (receipt_s == NULL) {
        cJSON_Delete(body);
        pq_error_set(&err, "invalid_request", "receipt is required");
        pq_http_response out;
        status_to_error(PQ_INVALID_ARGUMENT, &err, &out);
        return out;
    }
    cJSON *reason = cJSON_GetObjectItemCaseSensitive(body, "reason");
    const char *reason_s = (reason != NULL && cJSON_IsString(reason))
                               ? reason->valuestring : NULL;
    pq_status s = pq_service_nack(svc, queue_name, message_id, receipt_s,
                                   reason_s, &err);
    cJSON_Delete(body);
    if (s != PQ_OK) {
        pq_http_response out;
        status_to_error(s, &err, &out);
        return out;
    }
    return empty_response(204);
}

static pq_http_response handle_stats(pq_service *svc, const char *queue_name)
{
    pq_queue_stats stats;
    pq_error err = {0};
    pq_status s = pq_service_stats(svc, queue_name, &stats, &err);
    if (s != PQ_OK) {
        pq_http_response out;
        status_to_error(s, &err, &out);
        return out;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "queue", stats.queue);
    cJSON_AddNumberToObject(body, "ready", stats.ready);
    cJSON_AddNumberToObject(body, "reserved", stats.reserved);
    /* Spec §18: dead_lettered is omitted for .dead queues. Only include
     * when the queue is non-dead. */
    if (strstr(stats.queue, ".dead") == NULL) {
        cJSON_AddNumberToObject(body, "dead_lettered", stats.dead_lettered);
    }
    cJSON_AddNumberToObject(body, "total_active", stats.total_active);
    cJSON_AddNumberToObject(body, "oldest_ready_age_ms",
                            (double)stats.oldest_ready_age_ms);
    return make_json_response(200, body, NULL, NULL);
}

/* ---- Public dispatch ----------------------------------------------- */

pq_http_response pq_http_routes_dispatch(void *ctx, const pq_http_request *req)
{
    pq_http_routes_ctx *rc = (pq_http_routes_ctx *)ctx;

    /* Health endpoints still belong to server_main. */
    char queue[65];
    char id[65];
    const char *suffix = NULL;
    int matched = match_queue_path(req->path, queue, sizeof(queue),
                                   id, sizeof(id), &suffix);

    if (!matched) {
        cJSON *body = pq_json_new_error("not_found", "route not found", NULL);
        return make_json_response(404, body, NULL, NULL);
    }

    /* /queues/{q}/messages                                POST → publish
     *                                                     GET  → reserve
     * /queues/{q}/messages/{id}/ack                       POST → ack
     * /queues/{q}/messages/{id}/nack                      POST → nack
     * /queues/{q}/stats                                   GET  → stats */

    if (suffix == NULL || *suffix == '\0') {
        if (strcmp(req->method, "POST") == 0) {
            return handle_publish(rc->service, queue, req);
        }
        if (strcmp(req->method, "GET") == 0) {
            return handle_reserve(rc->service, rc->default_visibility_ms,
                                  queue, req);
        }
        cJSON *body = pq_json_new_error("method_not_allowed",
                                       "use POST for publish or GET for reserve",
                                       NULL);
        return make_json_response(405, body, NULL, NULL);
    }
    if (strcmp(suffix, "stats") == 0) {
        if (strcmp(req->method, "GET") != 0) {
            cJSON *body = pq_json_new_error("method_not_allowed",
                                           "use GET for stats", NULL);
            return make_json_response(405, body, NULL, NULL);
        }
        return handle_stats(rc->service, queue);
    }
    if (strcmp(suffix, "ack") == 0 || strcmp(suffix, "nack") == 0) {
        bool is_ack = (suffix[0] == 'a');
        if (strcmp(req->method, "POST") != 0) {
            cJSON *body = pq_json_new_error("method_not_allowed",
                                           "use POST", NULL);
            return make_json_response(405, body, NULL, NULL);
        }
        return is_ack ? handle_ack(rc->service, queue, id, req)
                      : handle_nack(rc->service, queue, id, req);
    }
    cJSON *body = pq_json_new_error("not_found", "unknown queue suffix", NULL);
    return make_json_response(404, body, NULL, NULL);
}

void pq_http_routes_dispose(pq_http_response *resp)
{
    dispose_response(resp);
}