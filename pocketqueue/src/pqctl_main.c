/* pqctl_main.c - command-line client (spec §30).
 *
 * Tiny HTTP client built directly on sockets + cJSON so we don't have to
 * add another vendored dependency. Each subcommand takes its own args
 * and prints results to stdout. Exit codes follow spec §30.7:
 *   0 success
 *   1 general error
 *   2 usage error
 *   3 network error
 *   4 server returned a client error (4xx)
 *   5 server returned an internal error (5xx)
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"

#define PQCTL_EXIT_OK            0
#define PQCTL_EXIT_GENERAL       1
#define PQCTL_EXIT_USAGE         2
#define PQCTL_EXIT_NETWORK       3
#define PQCTL_EXIT_CLIENT_ERROR  4
#define PQCTL_EXIT_SERVER_ERROR  5

#define DEFAULT_SERVER_URL "http://127.0.0.1:8080"
#define MAX_BODY_BYTES     (1u << 20)

/* ---- URL parsing ----------------------------------------------------- */
typedef struct {
    char host[256];
    char port[8];
    bool use_tls;
    char path[1024];
} parsed_url;

static int parse_url(const char *url, parsed_url *out)
{
    memset(out, 0, sizeof(*out));
    if (strncmp(url, "http://", 7) == 0) url += 7;
    else if (strncmp(url, "https://", 8) == 0) {
        out->use_tls = true;
        url += 8;
    } else {
        return -1;
    }
    const char *slash = strchr(url, '/');
    const char *colon = strchr(url, ':');
    if (slash == NULL) {
        if (colon == NULL) {
            strncpy(out->host, url, sizeof(out->host) - 1);
            strcpy(out->port, out->use_tls ? "443" : "80");
            strcpy(out->path, "/");
        } else {
            size_t hl = (size_t)(colon - url);
            if (hl >= sizeof(out->host)) return -1;
            memcpy(out->host, url, hl); out->host[hl] = '\0';
            strncpy(out->port, colon + 1, sizeof(out->port) - 1);
            strcpy(out->path, "/");
        }
    } else {
        if (colon == NULL || colon > slash) {
            size_t hl = (size_t)(slash - url);
            if (hl >= sizeof(out->host)) return -1;
            memcpy(out->host, url, hl); out->host[hl] = '\0';
            strcpy(out->port, out->use_tls ? "443" : "80");
        } else {
            size_t hl = (size_t)(colon - url);
            if (hl >= sizeof(out->host)) return -1;
            memcpy(out->host, url, hl); out->host[hl] = '\0';
            size_t pl = (size_t)(slash - colon - 1);
            if (pl >= sizeof(out->port)) return -1;
            memcpy(out->port, colon + 1, pl); out->port[pl] = '\0';
        }
        strncpy(out->path, slash, sizeof(out->path) - 1);
    }
    return 0;
}

/* ---- Minimal HTTP client (POST/GET) --------------------------------- */
typedef struct {
    int status;
    char *body;
    size_t body_len;
} http_response;

static int http_request(const parsed_url *url, const char *method,
                        const char *path, const char *content_type,
                        const char *body, http_response *resp)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(url->host, url->port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "pqctl: cannot resolve %s:%s: %s\n",
                url->host, url->port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "pqctl: connect failed\n");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    char hdr[2048];
    size_t body_len = body ? strlen(body) : 0;
    int hlen = snprintf(hdr, sizeof(hdr),
                        "%s %s HTTP/1.1\r\n"
                        "Host: %s:%s\r\n"
                        "User-Agent: pqctl/1.0\r\n"
                        "Connection: close\r\n"
                        "Accept: application/json\r\n",
                        method, path, url->host, url->port);
    if (body != NULL) {
        hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen,
                         "Content-Type: %s\r\n"
                         "Content-Length: %zu\r\n",
                         content_type ? content_type : "application/json",
                         body_len);
    }
    hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen, "\r\n");

    /* Write headers + body. */
    {
        size_t sent = 0;
        while (sent < (size_t)hlen) {
            ssize_t n = send(fd, hdr + sent, hlen - sent, 0);
            if (n <= 0) { close(fd); return -1; }
            sent += (size_t)n;
        }
        if (body != NULL) {
            sent = 0;
            while (sent < body_len) {
                ssize_t n = send(fd, body + sent, body_len - sent, 0);
                if (n <= 0) { close(fd); return -1; }
                sent += (size_t)n;
            }
        }
    }

    /* Read response. */
    size_t cap = 8192, used = 0;
    char *buf = malloc(cap);
    if (buf == NULL) { close(fd); return -1; }
    while (1) {
        ssize_t n = recv(fd, buf + used, cap - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        if (cap - used < 1024) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) { free(buf); close(fd); return -1; }
            buf = nb;
        }
    }
    close(fd);

    /* Parse status line + body. */
    int status = 0;
    char *body_start = NULL;
    {
        char *p = strstr(buf, "\r\n");
        if (p == NULL) { free(buf); return -1; }
        *p = '\0';
        sscanf(buf, "HTTP/1.1 %d", &status);
        body_start = strstr(p + 2, "\r\n\r\n");
        if (body_start != NULL) body_start += 4;
        else body_start = p + 2;
    }
    resp->status = status;
    resp->body_len = used - (size_t)(body_start - buf);
    resp->body = malloc(resp->body_len + 1);
    if (resp->body == NULL) { free(buf); return -1; }
    memcpy(resp->body, body_start, resp->body_len);
    resp->body[resp->body_len] = '\0';
    free(buf);
    return 0;
}

static void http_response_free(http_response *resp)
{
    free(resp->body);
    resp->body = NULL;
}

/* ---- Pretty-printing JSON to stdout (best-effort) ------------------- */
static void print_compact(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        printf("%s\n", json);
        return;
    }
    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        printf("%s\n", out);
        free(out);
    }
    cJSON_Delete(root);
}

/* ---- Subcommands ----------------------------------------------------- */
static int cmd_health(const parsed_url *url, int argc, char **argv)
{
    (void)argc; (void)argv;
    http_response r = {0};
    if (http_request(url, "GET", "/healthz", NULL, NULL, &r) != 0)
        return PQCTL_EXIT_NETWORK;
    printf("HTTP %d %s\n", r.status, r.status == 200 ? "ok" : "down");
    http_response_free(&r);
    return r.status == 200 ? PQCTL_EXIT_OK : PQCTL_EXIT_SERVER_ERROR;
}

static int cmd_publish(const parsed_url *url, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: pqctl publish QUEUE JSON [--max-attempts N]\n");
        return PQCTL_EXIT_USAGE;
    }
    const char *queue = argv[0];
    const char *json_text = argv[1];
    int max_attempts = -1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--max-attempts") == 0 && i + 1 < argc) {
            max_attempts = atoi(argv[++i]);
        }
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *payload = cJSON_Parse(json_text);
    if (payload == NULL) {
        fprintf(stderr, "pqctl: payload is not valid JSON\n");
        cJSON_Delete(body);
        return PQCTL_EXIT_USAGE;
    }
    cJSON_AddItemToObject(body, "payload", payload);
    if (max_attempts > 0) {
        cJSON_AddNumberToObject(body, "max_attempts", max_attempts);
    }
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/messages", queue);
    http_response r = {0};
    int rc = http_request(url, "POST", path, "application/json",
                          body_text, &r);
    free(body_text);
    if (rc != 0) return PQCTL_EXIT_NETWORK;
    if (r.status >= 400 && r.status < 500) {
        print_compact(r.body);
        http_response_free(&r);
        return PQCTL_EXIT_CLIENT_ERROR;
    }
    if (r.status >= 500) {
        print_compact(r.body);
        http_response_free(&r);
        return PQCTL_EXIT_SERVER_ERROR;
    }
    if (r.status == 201) {
        cJSON *root = cJSON_Parse(r.body);
        if (root != NULL) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            if (id && cJSON_IsString(id)) printf("%s\n", id->valuestring);
            cJSON_Delete(root);
        }
    }
    http_response_free(&r);
    return PQCTL_EXIT_OK;
}

static int cmd_consume(const parsed_url *url, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: pqctl consume QUEUE [--wait-ms N] [--visibility-ms N] [--raw]\n");
        return PQCTL_EXIT_USAGE;
    }
    const char *queue = argv[0];
    int64_t wait_ms = 0, visibility_ms = 0;
    bool raw = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) wait_ms = atoll(argv[++i]);
        else if (strcmp(argv[i], "--visibility-ms") == 0 && i + 1 < argc) visibility_ms = atoll(argv[++i]);
        else if (strcmp(argv[i], "--raw") == 0) raw = true;
    }
    char path[1024];
    if (wait_ms || visibility_ms) {
        snprintf(path, sizeof(path), "/queues/%s/messages?wait_ms=%lld&visibility_timeout_ms=%lld",
                 queue, (long long)wait_ms, (long long)visibility_ms);
    } else {
        snprintf(path, sizeof(path), "/queues/%s/messages", queue);
    }
    http_response r = {0};
    if (http_request(url, "GET", path, NULL, NULL, &r) != 0)
        return PQCTL_EXIT_NETWORK;
    if (r.status == 204) {
        printf("no message available\n");
        http_response_free(&r);
        return PQCTL_EXIT_OK;
    }
    if (r.status >= 400) {
        print_compact(r.body);
        http_response_free(&r);
        return r.status < 500 ? PQCTL_EXIT_CLIENT_ERROR : PQCTL_EXIT_SERVER_ERROR;
    }
    if (raw) {
        printf("%s\n", r.body);
    } else {
        cJSON *root = cJSON_Parse(r.body);
        if (root == NULL) { printf("%s\n", r.body); http_response_free(&r); return PQCTL_EXIT_OK; }
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *receipt = cJSON_GetObjectItemCaseSensitive(root, "receipt");
        cJSON *attempts = cJSON_GetObjectItemCaseSensitive(root, "attempts");
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        char *payload_str = payload ? cJSON_PrintUnformatted(payload) : NULL;
        printf("id:        %s\n", (id && cJSON_IsString(id)) ? id->valuestring : "?");
        printf("receipt:   %s\n", (receipt && cJSON_IsString(receipt)) ? receipt->valuestring : "?");
        printf("attempts:  %d\n", (attempts && cJSON_IsNumber(attempts)) ? (int)attempts->valuedouble : 0);
        printf("payload:   %s\n", payload_str ? payload_str : "(none)");
        free(payload_str);
        cJSON_Delete(root);
    }
    http_response_free(&r);
    return PQCTL_EXIT_OK;
}

static int cmd_ack(const parsed_url *url, int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: pqctl ack QUEUE MESSAGE_ID RECEIPT\n");
        return PQCTL_EXIT_USAGE;
    }
    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/messages/%s/ack",
             argv[0], argv[1]);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receipt", argv[2]);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    http_response r = {0};
    int rc = http_request(url, "POST", path, "application/json",
                          body_text, &r);
    free(body_text);
    if (rc != 0) return PQCTL_EXIT_NETWORK;
    if (r.status >= 400) {
        print_compact(r.body);
        http_response_free(&r);
        return r.status < 500 ? PQCTL_EXIT_CLIENT_ERROR : PQCTL_EXIT_SERVER_ERROR;
    }
    http_response_free(&r);
    return PQCTL_EXIT_OK;
}

static int cmd_nack(const parsed_url *url, int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: pqctl nack QUEUE MESSAGE_ID RECEIPT [--reason TEXT]\n");
        return PQCTL_EXIT_USAGE;
    }
    const char *reason = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--reason") == 0 && i + 1 < argc) {
            reason = argv[++i];
        }
    }
    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/messages/%s/nack",
             argv[0], argv[1]);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receipt", argv[2]);
    if (reason != NULL) cJSON_AddStringToObject(body, "reason", reason);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    http_response r = {0};
    int rc = http_request(url, "POST", path, "application/json",
                          body_text, &r);
    free(body_text);
    if (rc != 0) return PQCTL_EXIT_NETWORK;
    if (r.status >= 400) {
        print_compact(r.body);
        http_response_free(&r);
        return r.status < 500 ? PQCTL_EXIT_CLIENT_ERROR : PQCTL_EXIT_SERVER_ERROR;
    }
    http_response_free(&r);
    return PQCTL_EXIT_OK;
}

static int cmd_stats(const parsed_url *url, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: pqctl stats QUEUE\n");
        return PQCTL_EXIT_USAGE;
    }
    char path[1024];
    snprintf(path, sizeof(path), "/queues/%s/stats", argv[0]);
    http_response r = {0};
    if (http_request(url, "GET", path, NULL, NULL, &r) != 0)
        return PQCTL_EXIT_NETWORK;
    if (r.status >= 400) {
        print_compact(r.body);
        http_response_free(&r);
        return r.status < 500 ? PQCTL_EXIT_CLIENT_ERROR : PQCTL_EXIT_SERVER_ERROR;
    }
    print_compact(r.body);
    http_response_free(&r);
    return PQCTL_EXIT_OK;
}

/* ---- Main ------------------------------------------------------------ */
static const char *kUsage =
    "Usage: pqctl [--server URL] <command> [args]\n"
    "Commands:\n"
    "  health\n"
    "  publish QUEUE JSON [--max-attempts N]\n"
    "  consume QUEUE [--wait-ms N] [--visibility-ms N] [--raw]\n"
    "  ack QUEUE MESSAGE_ID RECEIPT\n"
    "  nack QUEUE MESSAGE_ID RECEIPT [--reason TEXT]\n"
    "  stats QUEUE\n";

int main(int argc, char **argv)
{
    /* Ignore SIGPIPE so writes to closed sockets return EPIPE. */
    signal(SIGPIPE, SIG_IGN);

    const char *server = getenv("PQ_SERVER_URL");
    if (server == NULL || *server == '\0') server = DEFAULT_SERVER_URL;

    static struct option longopts[] = {
        {"server", required_argument, NULL, 's'},
        {"help",   no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };
    /* Prefix optstring with '+' to enforce POSIX option-parsing order:
     * option parsing stops at the first non-option argument (the subcommand),
     * so `--max-attempts` etc. after the subcommand are not interpreted as
     * pqctl flags. */
    int opt;
    while ((opt = getopt_long(argc, argv, "+s:h", longopts, NULL)) != -1) {
        if (opt == 's') server = optarg;
        else if (opt == 'h') { fputs(kUsage, stdout); return 0; }
    }
    int cmd_start = optind;
    if (cmd_start >= argc) {
        fputs(kUsage, stderr);
        return PQCTL_EXIT_USAGE;
    }
    parsed_url url;
    if (parse_url(server, &url) != 0) {
        fprintf(stderr, "pqctl: invalid server URL '%s'\n", server);
        return PQCTL_EXIT_USAGE;
    }

    const char *cmd = argv[cmd_start];
    int cmd_argc = argc - cmd_start - 1;
    char **cmd_argv = argv + cmd_start + 1;

    if (strcmp(cmd, "health") == 0)
        return cmd_health(&url, cmd_argc, cmd_argv);
    if (strcmp(cmd, "publish") == 0)
        return cmd_publish(&url, cmd_argc, cmd_argv);
    if (strcmp(cmd, "consume") == 0)
        return cmd_consume(&url, cmd_argc, cmd_argv);
    if (strcmp(cmd, "ack") == 0)
        return cmd_ack(&url, cmd_argc, cmd_argv);
    if (strcmp(cmd, "nack") == 0)
        return cmd_nack(&url, cmd_argc, cmd_argv);
    if (strcmp(cmd, "stats") == 0)
        return cmd_stats(&url, cmd_argc, cmd_argv);

    fprintf(stderr, "pqctl: unknown command '%s'\n%s", cmd, kUsage);
    return PQCTL_EXIT_USAGE;
}