/* worker.c - one spider worker thread.
 *
 * Loops: reserve → fetch → extract links → publish new candidates →
 * ack. Bad URLs are nacked so that pocketqueue-server retries them, and
 * eventually dead-letters if max_attempts is hit.
 */
#include "worker.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

#include "extract.h"
#include "http_fetch.h"

static const char *queue_for_worker(const worker_ctx *w)
{
    return w->assets ? "assets" : "pages";
}

static bool content_type_is_html(const char *ct)
{
    if (ct == NULL || ct[0] == '\0') return false;
    if (strncasecmp(ct, "text/html", 9) == 0) return true;
    if (strncasecmp(ct, "application/xhtml", 17) == 0) return true;
    return false;
}

/* Read the URL out of the message payload. Returns true on success. */
static bool message_url(const pq_message *m, char *out, size_t out_size)
{
    if (m == NULL || m->payload_json == NULL) return false;
    cJSON *root = cJSON_Parse(m->payload_json);
    if (root == NULL) return false;
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    bool ok = false;
    if (url != NULL && cJSON_IsString(url)) {
        size_t n = strlen(url->valuestring);
        if (n + 1 <= out_size) {
            memcpy(out, url->valuestring, n + 1);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

void *worker_loop(void *arg)
{
    worker_ctx *w = arg;
    const char *queue = queue_for_worker(w);

    while (!*w->shutdown) {
        pq_message msg;
        memset(&msg, 0, sizeof(msg));
        pq_error err = {0};
        bool ok = pq_client_reserve(w->client, queue,
                                   w->default_visibility_ms,
                                   5000, &msg, &err);
        if (!ok) {
            /* Either transport error or 204 (no message yet). Back off. */
            struct timespec ts = {0, 100 * 1000 * 1000};  /* 100 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        /* Pull the URL out of the message payload. */
        char url[URL_MAX];
        if (!message_url(&msg, url, sizeof(url))) {
            /* Malformed payload — nack + ack-via-nack, drop. */
            if (msg.id[0] != '\0') {
                pq_error nerr = {0};
                pq_client_nack(w->client, queue, msg.id, msg.receipt,
                               "missing url", &nerr);
                pq_client_ack(w->client, queue, msg.id, msg.receipt, &nerr);
            }
            pq_message_dispose(&msg);
            continue;
        }

        /* Fetch the URL. */
        fetch_response r;
        memset(&r, 0, sizeof(r));
        bool fetched = http_fetch(url, &r);
        (void)fetched;

        if (!fetched || r.http_status < 200 || r.http_status >= 400) {
            /* Nack with transient failure — let pocketqueue-server retry
             * or eventually dead-letter this URL. */
            if (msg.id[0] != '\0') {
                pq_error nerr = {0};
                pq_client_nack(w->client, queue, msg.id, msg.receipt,
                               "fetch failed", &nerr);
            }
            fetch_response_free(&r);
            pq_message_dispose(&msg);
            continue;
        }

        /* Only parse HTML on the pages queue; assets queue just fetches +
         * acks. */
        if (!w->assets && content_type_is_html(r.content_type) && r.body) {
            extracted ex;
            extract_links(r.body, r.body_len, url, &ex);
            for (size_t i = 0; i < ex.count; i++) {
                pq_error perr = {0};
                const char *link = ex.urls[i];
                if (dedup_contains(w->dedup, link)) continue;
                /* Heuristic asset routing. Real spiders would
                 * GET-with-HEAD to know. */
                const char *target = "pages";
                if (strstr(link, ".png") || strstr(link, ".jpg") ||
                    strstr(link, ".gif") || strstr(link, ".zip") ||
                    strstr(link, ".pdf") || strstr(link, ".css") ||
                    strstr(link, ".js")) {
                    target = "assets";
                }
                if (dedup_add(w->dedup, link)) {
                    int mxd = 5;
                    pq_client_publish(w->client, target, link, mxd, &perr);
                }
            }
            extracted_free(&ex);
        }

        /* Ack the consumed message. */
        if (msg.id[0] != '\0') {
            pq_error aerr = {0};
            pq_client_ack(w->client, queue, msg.id, msg.receipt, &aerr);
        }
        fetch_response_free(&r);
        dedup_mark(w->dedup, url, 'v');
        pq_message_dispose(&msg);
    }
    return NULL;
}
