/* pqctl_client.c - tiny HTTP client for the pocketqueue-server. */
#include "pqctl_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"

struct pq_client {
    char host[128];
    char port[8];
};

static bool parse_url(const char *url, char *host, size_t hl,
                      char *port, size_t pl)
{
    if (strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7;
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        size_t hn = (size_t)(colon - p);
        if (hn >= hl) return false;
        memcpy(host, p, hn); host[hn] = '\0';
        size_t pn = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (pn >= pl) return false;
        memcpy(port, colon + 1, pn); port[pn] = '\0';
    } else {
        size_t hn = slash ? (size_t)(slash - p) : strlen(p);
        if (hn >= hl) return false;
        memcpy(host, p, hn); host[hn] = '\0';
        snprintf(port, pl, "80");
    }
    return true;
}

/* Tiny HTTP request: builds the request, sends it, reads the response
 * header, then reads (Content-Length) bytes of body. Returns false on
 * any I/O error or non-2xx status. */
static bool http_request(const pq_client *c, const char *method,
                        const char *path, const char *body,
                        int *out_status, char **out_body,
                        size_t *out_body_len)
{
    if (out_body) *out_body = NULL;
    if (out_body_len) *out_body_len = 0;
    if (out_status) *out_status = 0;

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(c->host, c->port, &hints, &res) != 0) return false;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    size_t body_len = body ? strlen(body) : 0;
    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "%s %s HTTP/1.1\r\n"
                        "Host: %s:%s\r\n"
                        "User-Agent: pq-spider/1.0\r\n"
                        "Accept: application/json\r\n"
                        "Connection: close\r\n",
                        method, path, c->host, c->port);
    if (body != NULL) {
        hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen,
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n", body_len);
    }
    hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen, "\r\n");

    size_t sent = 0;
    while (sent < (size_t)hlen) {
        ssize_t n = send(fd, hdr + sent, hlen - sent, 0);
        if (n <= 0) { close(fd); return false; }
        sent += n;
    }
    if (body != NULL) {
        sent = 0;
        while (sent < body_len) {
            ssize_t n = send(fd, body + sent, body_len - sent, 0);
            if (n <= 0) { close(fd); return false; }
            sent += n;
        }
    }

    size_t cap = 8192, used = 0;
    char *buf = malloc(cap);
    if (buf == NULL) { close(fd); return false; }
    while (1) {
        ssize_t n = recv(fd, buf + used, cap - used, 0);
        if (n <= 0) break;
        used += n;
        if (cap - used < 1024) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) { free(buf); close(fd); return false; }
            buf = nb;
        }
    }
    close(fd);

    /* Parse status. */
    int status = 0;
    {
        char *p = strstr(buf, "\r\n");
        if (p == NULL) { free(buf); return false; }
        *p = '\0';
        sscanf(buf, "HTTP/1.1 %d", &status);
        *p = '\r';
    }
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start != NULL) body_start += 4;
    else body_start = buf + strlen(buf);

    size_t bl = used - (size_t)(body_start - buf);
    if (out_body) {
        *out_body = malloc(bl + 1);
        if (*out_body == NULL) { free(buf); return false; }
        memcpy(*out_body, body_start, bl);
        (*out_body)[bl] = '\0';
    }
    if (out_body_len) *out_body_len = bl;
    free(buf);
    if (out_status) *out_status = status;
    return (status >= 200 && status < 300);
}

pq_client *pq_client_open(const char *base_url)
{
    pq_client *c = calloc(1, sizeof(*c));
    if (c == NULL) return NULL;
    if (!parse_url(base_url, c->host, sizeof(c->host),
                   c->port, sizeof(c->port))) {
        free(c);
        return NULL;
    }
    return c;
}

void pq_client_close(pq_client *c)
{
    free(c);
}

static bool publish_path(pq_client *c, const char *queue,
                         const char *payload_json, int max_attempts,
                         pq_error *err)
{
    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/messages", queue);
    cJSON *root = cJSON_CreateObject();
    cJSON *p = cJSON_Parse(payload_json);
    cJSON_AddItemToObject(root, "payload", p ? p : cJSON_CreateNull());
    if (max_attempts > 0) cJSON_AddNumberToObject(root, "max_attempts", max_attempts);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    int status = 0;
    char *resp = NULL;
    bool ok = http_request(c, "POST", path, body, &status, &resp, NULL);
    free(body);
    if (!ok && resp != NULL) free(resp);
    if (!ok) {
        pq_error_set(err, "network_error", "publish failed (network)");
        return false;
    }
    if (status != 201) {
        pq_error_set(err, "server_error", resp ? resp : "publish failed");
        free(resp);
        return false;
    }
    free(resp);
    return true;
}

bool pq_client_publish(pq_client *c, const char *queue,
                        const char *payload_json,
                        int max_attempts, pq_error *err)
{
    return publish_path(c, queue, payload_json, max_attempts, err);
}

bool pq_client_reserve(pq_client *c, const char *queue,
                       int64_t visibility_timeout_ms,
                       int64_t wait_ms, pq_message *out, pq_error *err)
{
    char path[1024];
    if (visibility_timeout_ms > 0 || wait_ms > 0) {
        snprintf(path, sizeof(path),
                 "/queues/%s/messages?wait_ms=%lld&visibility_timeout_ms=%lld",
                 queue, (long long)wait_ms,
                 (long long)visibility_timeout_ms);
    } else {
        snprintf(path, sizeof(path), "/queues/%s/messages", queue);
    }
    int status = 0;
    char *resp = NULL;
    size_t body_len = 0;
    bool ok = http_request(c, "GET", path, NULL, &status, &resp, &body_len);
    if (!ok || status == 204) {
        if (resp) free(resp);
        /* Either transport failure or no message (204). */
        if (!ok) {
            pq_error_set(err, "network_error",
                         "reserve failed (network)");
            return false;
        }
        pq_error_set(err, "not_found", "no message");
        return false;
    }
    if (status < 200 || status >= 300) {
        pq_error_set(err, "server_error", resp ? resp : "reserve failed");
        free(resp);
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        pq_error_set(err, "internal_error", "could not parse reserve response");
        return false;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *q  = cJSON_GetObjectItemCaseSensitive(root, "queue");
    cJSON *rx = cJSON_GetObjectItemCaseSensitive(root, "receipt");
    cJSON *pay= cJSON_GetObjectItemCaseSensitive(root, "payload");
    cJSON *att = cJSON_GetObjectItemCaseSensitive(root, "attempts");

    memset(out, 0, sizeof(*out));
#define COPY_FIELD(field, src) do { \
    if ((src) && cJSON_IsString(src)) { \
        size_t _n = strlen((src)->valuestring); \
        if (_n >= sizeof(out->field)) _n = sizeof(out->field) - 1; \
        memcpy(out->field, (src)->valuestring, _n); \
        out->field[_n] = '\0'; \
    } \
} while (0)
    COPY_FIELD(id, id);
    COPY_FIELD(queue, q);
    COPY_FIELD(receipt, rx);
#undef COPY_FIELD
    if (att && cJSON_IsNumber(att)) out->attempts = (int)att->valuedouble;
    if (pay) {
        char *txt = cJSON_PrintUnformatted(pay);
        if (txt != NULL) {
            size_t n = strlen(txt);
            out->payload_json = malloc(n + 1);
            if (out->payload_json != NULL) {
                memcpy(out->payload_json, txt, n + 1);
                out->payload_len = n;
            }
            free(txt);
        }
    }
    cJSON_Delete(root);
    return true;
}

bool pq_client_ack(pq_client *c, const char *queue,
                   const char *message_id, const char *receipt,
                   pq_error *err)
{
    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/messages/%s/ack", queue, message_id);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "receipt", receipt);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    int status = 0;
    char *resp = NULL;
    bool ok = http_request(c, "POST", path, body, &status, &resp, NULL);
    free(body);
    if (resp) free(resp);
    if (!ok) {
        pq_error_set(err, "network_error", "ack failed (network)");
        return false;
    }
    if (status != 204) {
        pq_error_set(err, "ack_failed", "ack returned non-204");
        return false;
    }
    return true;
}

bool pq_client_nack(pq_client *c, const char *queue,
                    const char *message_id, const char *receipt,
                    const char *reason, pq_error *err)
{
    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/messages/%s/nack", queue, message_id);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "receipt", receipt);
    if (reason) cJSON_AddStringToObject(root, "reason", reason);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    int status = 0;
    char *resp = NULL;
    bool ok = http_request(c, "POST", path, body, &status, &resp, NULL);
    free(body);
    if (resp) free(resp);
    if (!ok) {
        pq_error_set(err, "network_error", "nack failed (network)");
        return false;
    }
    if (status != 204) {
        pq_error_set(err, "nack_failed", "nack returned non-204");
        return false;
    }
    return true;
}
