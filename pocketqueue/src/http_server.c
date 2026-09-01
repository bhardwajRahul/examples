/* http_server.c - hand-rolled minimal HTTP/1.1 server.
 *
 * Decision D1: written in-tree because the MIT-licensed alternatives
 * (libmicrohttpd) require autoconf and the GPL-licensed alternatives
 * (Mongoose, CivetWeb) are incompatible with our MIT license.
 *
 * Design:
 *   - One accept thread loops on accept(); each connection is enqueued
 *     onto a bounded queue protected by a mutex + condvar.
 *   - N worker threads pull connections and process them serially.
 *   - pq_http_server_stop closes the listener and broadcasts; workers
 *     finish their current request and exit.
 *   - The handler runs without any DB lock held. Long-poll will release
 *     any DB lock before waiting (spec §15.4).
 *
 * Limits:
 *   - Bound body bytes via max_body_bytes; oversized requests get 413.
 *   - Header section is capped at 32 KiB.
 *   - Method must be GET or POST; others return 405.
 *   - HTTP/1.1 only; HTTP/0.9 / HTTP/2 are rejected.
 */
#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "http_routes.h"
#include "str_util.h"

#define PQ_HTTP_MAX_HEADERS_BYTES (32 * 1024)
#define PQ_HTTP_QUEUE_CAPACITY    64

typedef struct connection_job {
    int fd;
    char remote[64];
} connection_job;

struct pq_http_server {
    pthread_t accept_thread;
    pthread_t *worker_threads;
    int worker_count;
    int listen_fd;
    uint16_t port;
    int64_t max_body_bytes;
    pq_http_handler handler;
    void *handler_ctx;

    pthread_mutex_t queue_mu;
    pthread_cond_t queue_not_empty;
    connection_job queue[PQ_HTTP_QUEUE_CAPACITY];
    int queue_head;
    int queue_tail;
    int queue_size;

    atomic_bool stopping;
    atomic_bool accept_thread_done;
    atomic_int workers_done;
};

static void *accept_loop(void *arg);
static void *worker_loop(void *arg);

static void set_socket_options(int fd)
{
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

static bool parse_bind_address(const char *addr, int *family,
                               struct sockaddr_storage *out, socklen_t *out_len)
{
    memset(out, 0, sizeof(*out));
    if (addr == NULL || *addr == '\0' || strcmp(addr, "127.0.0.1") == 0 ||
        strcmp(addr, "0.0.0.0") == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)out;
        sin->sin_family = AF_INET;
        sin->sin_port = 0; /* assigned by caller via bind() */
        if (addr && strcmp(addr, "0.0.0.0") == 0) {
            sin->sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        *family = AF_INET;
        *out_len = sizeof(*sin);
        return true;
    }
    if (strcmp(addr, "::1") == 0 || strcmp(addr, "::") == 0) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)out;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = 0;
        if (strcmp(addr, "::") == 0) {
            sin6->sin6_addr = in6addr_any;
        } else {
            sin6->sin6_addr = in6addr_loopback;
        }
        *family = AF_INET6;
        *out_len = sizeof(*sin6);
        return true;
    }
    /* Fallback: try inet_pton for IPv4. */
    struct sockaddr_in *sin = (struct sockaddr_in *)out;
    if (inet_pton(AF_INET, addr, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        *family = AF_INET;
        *out_len = sizeof(*sin);
        return true;
    }
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)out;
    if (inet_pton(AF_INET6, addr, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        *family = AF_INET6;
        *out_len = sizeof(*sin6);
        return true;
    }
    return false;
}

pq_http_server *pq_http_server_start(const char *bind_address,
                                     uint16_t port,
                                     int worker_threads,
                                     int64_t max_body_bytes,
                                     pq_http_handler handler,
                                     void *ctx)
{
    if (handler == NULL || port == 0 || worker_threads < 1) {
        return NULL;
    }
    if (worker_threads > 64) worker_threads = 64;

    pq_http_server *srv = calloc(1, sizeof(*srv));
    if (srv == NULL) return NULL;
    srv->port = port;
    srv->max_body_bytes = max_body_bytes;
    srv->handler = handler;
    srv->handler_ctx = ctx;
    srv->worker_count = worker_threads;
    atomic_init(&srv->stopping, false);
    atomic_init(&srv->accept_thread_done, false);
    atomic_init(&srv->workers_done, 0);
    pthread_mutex_init(&srv->queue_mu, NULL);
    pthread_cond_init(&srv->queue_not_empty, NULL);

    int family = AF_INET;
    struct sockaddr_storage sa;
    socklen_t sa_len = 0;
    if (!parse_bind_address(bind_address, &family, &sa, &sa_len)) {
        free(srv);
        return NULL;
    }
    /* Set port. */
    if (family == AF_INET) {
        ((struct sockaddr_in *)&sa)->sin_port = htons(port);
    } else {
        ((struct sockaddr_in6 *)&sa)->sin6_port = htons(port);
    }

    srv->listen_fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv->listen_fd < 0) {
        free(srv);
        return NULL;
    }
    set_socket_options(srv->listen_fd);
    if (bind(srv->listen_fd, (struct sockaddr *)&sa, sa_len) != 0) {
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }
    if (listen(srv->listen_fd, 64) != 0) {
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }
    /* Read back the actual bound port (useful for port==0 picking). */
    if (port == 0) {
        struct sockaddr_storage bound;
        socklen_t blen = sizeof(bound);
        if (getsockname(srv->listen_fd, (struct sockaddr *)&bound, &blen) == 0) {
            if (bound.ss_family == AF_INET) {
                srv->port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
            } else {
                srv->port = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
            }
        }
    }

    /* Spawn workers first so they're ready to drain the queue. */
    srv->worker_threads = calloc((size_t)worker_threads, sizeof(pthread_t));
    if (srv->worker_threads == NULL) {
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }
    for (int i = 0; i < worker_threads; i++) {
        if (pthread_create(&srv->worker_threads[i], NULL, worker_loop, srv) != 0) {
            /* Best effort: stop and fail. */
            atomic_store(&srv->stopping, true);
            pthread_cond_broadcast(&srv->queue_not_empty);
            for (int j = 0; j < i; j++) pthread_join(srv->worker_threads[j], NULL);
            free(srv->worker_threads);
            close(srv->listen_fd);
            free(srv);
            return NULL;
        }
    }
    if (pthread_create(&srv->accept_thread, NULL, accept_loop, srv) != 0) {
        atomic_store(&srv->stopping, true);
        pthread_cond_broadcast(&srv->queue_not_empty);
        for (int i = 0; i < worker_threads; i++)
            pthread_join(srv->worker_threads[i], NULL);
        free(srv->worker_threads);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    /* Ignore SIGPIPE so writes to closed sockets return EPIPE. */
    signal(SIGPIPE, SIG_IGN);
    return srv;
}

uint16_t pq_http_server_port(const pq_http_server *srv)
{
    return srv ? srv->port : 0;
}

bool pq_http_server_is_stopping(const pq_http_server *srv)
{
    return srv ? atomic_load((_Atomic bool *)&srv->stopping) : true;
}

/* ---------------------------------------------------------------------- */
/* Connection handling                                                    */
/* ---------------------------------------------------------------------- */

/* Read up to n bytes. Returns total bytes read, 0 on EOF, -1 on error. */
static ssize_t read_exact(int fd, char *buf, size_t n, int timeout_ms)
{
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) return -1;
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Read until "\r\n\r\n" or until PQ_HTTP_MAX_HEADERS_BYTES. The remainder
 * of the body length is returned in *out_body_remaining. Returns total
 * header bytes read on success, -1 on failure. */
static ssize_t read_headers(int fd, char *buf, size_t buf_size,
                            size_t *out_body_remaining)
{
    size_t got = 0;
    while (got + 4 <= buf_size) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 30000);
        if (pr <= 0) return -1;
        ssize_t r = recv(fd, buf + got, buf_size - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
        buf[got] = '\0';
        char *p = strstr(buf, "\r\n\r\n");
        if (p != NULL) {
            *out_body_remaining = got - (size_t)(p - buf) - 4;
            return (ssize_t)got;
        }
    }
    return -1; /* header section too large */
}

static const char *header_value(const char *headers, const char *name,
                                char *out, size_t out_size)
{
    /* Case-insensitive header lookup, returns pointer into headers or
     * copies a NUL-terminated trimmed value into out. */
    size_t namelen = strlen(name);
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, name, namelen) == 0 && p[namelen] == ':') {
            const char *v = p + namelen + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *end = strstr(v, "\r\n");
            if (end == NULL) end = v + strlen(v);
            size_t n = (size_t)(end - v);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return out;
        }
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL) break;
        p = eol + 2;
    }
    return NULL;
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void write_all(int fd, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return;
        }
        sent += (size_t)r;
    }
}

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

static void send_response(int fd, pq_http_response resp)
{
    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n",
                           resp.status_code, status_text(resp.status_code),
                           resp.content_type ? resp.content_type : "application/octet-stream",
                           resp.body_length);
    write_all(fd, hdr, (size_t)hdr_len);
    if (resp.extra_header_name != NULL && resp.extra_header_value != NULL) {
        int l = snprintf(hdr, sizeof(hdr), "%s: %s\r\n",
                         resp.extra_header_name, resp.extra_header_value);
        write_all(fd, hdr, (size_t)l);
    }
    write_all(fd, "\r\n", 2);
    if (resp.body_length > 0 && resp.body != NULL) {
        write_all(fd, resp.body, resp.body_length);
    }
}

static void handle_connection(pq_http_server *srv, int fd, const char *remote)
{
    char header_buf[PQ_HTTP_MAX_HEADERS_BYTES + 1];
    size_t body_in_header = 0;
    ssize_t total = read_headers(fd, header_buf, sizeof(header_buf) - 1,
                                 &body_in_header);
    if (total < 0) {
        close(fd);
        return;
    }
    header_buf[total] = '\0';

    /* Parse request line. */
    char *line_end = strstr(header_buf, "\r\n");
    if (line_end == NULL) {
        send_response(fd, (pq_http_response){.status_code = 400,
                                             .content_type = "text/plain",
                                             .body = "bad request",
                                             .body_length = 11});
        close(fd);
        return;
    }
    *line_end = '\0';
    char method[16] = {0};
    char target[1024] = {0};
    char version[16] = {0};
    if (sscanf(header_buf, "%15s %1023s %15s", method, target, version) < 2) {
        send_response(fd, (pq_http_response){.status_code = 400,
                                             .content_type = "text/plain",
                                             .body = "bad request",
                                             .body_length = 11});
        close(fd);
        return;
    }
    if (!starts_with(version, "HTTP/1.")) {
        send_response(fd, (pq_http_response){.status_code = 505,
                                             .content_type = "text/plain",
                                             .body = "HTTP Version Not Supported",
                                             .body_length = 26});
        close(fd);
        return;
    }

    /* Split path / query. */
    char *path = target;
    char *query = NULL;
    char *q = strchr(target, '?');
    if (q != NULL) {
        *q = '\0';
        query = q + 1;
    }

    /* Locate header section body and find Content-Length. */
    char *headers_start = line_end + 2;
    char cl_buf[32] = {0};
    const char *cl = header_value(headers_start, "Content-Length",
                                  cl_buf, sizeof(cl_buf));
    int64_t content_length = 0;
    if (cl != NULL) {
        if (!pq_str_parse_int64(cl, &content_length) || content_length < 0) {
            send_response(fd, (pq_http_response){.status_code = 400,
                                                 .content_type = "text/plain",
                                                 .body = "bad Content-Length",
                                                 .body_length = 17});
            close(fd);
            return;
        }
    }
    if (content_length > srv->max_body_bytes) {
        send_response(fd, (pq_http_response){.status_code = 413,
                                             .content_type = "text/plain",
                                             .body = "payload too large",
                                             .body_length = 18});
        close(fd);
        return;
    }
    char ct_buf[128] = {0};
    const char *ct = header_value(headers_start, "Content-Type",
                                  ct_buf, sizeof(ct_buf));

    /* Read remaining body. */
    char *body = NULL;
    if (content_length > 0) {
        body = malloc((size_t)content_length + 1);
        if (body == NULL) {
            send_response(fd, (pq_http_response){.status_code = 500,
                                                 .content_type = "text/plain",
                                                 .body = "out of memory",
                                                 .body_length = 13});
            close(fd);
            return;
        }
        size_t have = body_in_header;
        if (have > (size_t)content_length) have = (size_t)content_length;
        if (have > 0) {
            memcpy(body, header_buf + (total - (ssize_t)body_in_header), have);
        }
        size_t need = (size_t)content_length - have;
        if (need > 0) {
            ssize_t r = read_exact(fd, body + have, need, 30000);
            if (r < 0 || (size_t)r != need) {
                free(body);
                close(fd);
                return;
            }
        }
        body[content_length] = '\0';
    }

    pq_http_request req = {
        .method = method,
        .path = path,
        .query = query,
        .content_type = ct,
        .body = body,
        .body_length = (size_t)content_length,
        .remote_addr = remote,
    };
    pq_http_response resp = srv->handler(srv->handler_ctx, &req);
    send_response(fd, resp);
    /* Some handlers (e.g. pq_http_routes_dispatch) allocate heap
     * memory for the body and any extra header values. Release it now
     * so the response struct doesn't leak. */
    pq_http_routes_dispose(&resp);
    free(body);
    close(fd);
}

/* ---------------------------------------------------------------------- */
/* Threads                                                                */
/* ---------------------------------------------------------------------- */

static void enqueue(pq_http_server *srv, connection_job job)
{
    pthread_mutex_lock(&srv->queue_mu);
    while (!atomic_load(&srv->stopping) &&
           srv->queue_size == PQ_HTTP_QUEUE_CAPACITY) {
        pthread_cond_wait(&srv->queue_not_empty, &srv->queue_mu);
    }
    if (atomic_load(&srv->stopping)) {
        pthread_mutex_unlock(&srv->queue_mu);
        close(job.fd);
        return;
    }
    srv->queue[srv->queue_tail] = job;
    srv->queue_tail = (srv->queue_tail + 1) % PQ_HTTP_QUEUE_CAPACITY;
    srv->queue_size++;
    pthread_cond_signal(&srv->queue_not_empty);
    pthread_mutex_unlock(&srv->queue_mu);
}

static bool dequeue(pq_http_server *srv, connection_job *out)
{
    pthread_mutex_lock(&srv->queue_mu);
    while (srv->queue_size == 0 &&
           !atomic_load(&srv->stopping)) {
        pthread_cond_wait(&srv->queue_not_empty, &srv->queue_mu);
    }
    if (srv->queue_size == 0 && atomic_load(&srv->stopping)) {
        pthread_mutex_unlock(&srv->queue_mu);
        return false;
    }
    *out = srv->queue[srv->queue_head];
    srv->queue_head = (srv->queue_head + 1) % PQ_HTTP_QUEUE_CAPACITY;
    srv->queue_size--;
    /* If the queue was full and we just removed an item, signal the
     * accept thread so it can stop blocking. */
    if (srv->queue_size == PQ_HTTP_QUEUE_CAPACITY - 1) {
        pthread_cond_broadcast(&srv->queue_not_empty);
    }
    pthread_mutex_unlock(&srv->queue_mu);
    return true;
}

static void *accept_loop(void *arg)
{
    pq_http_server *srv = arg;
    while (!atomic_load(&srv->stopping)) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(srv->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            continue;
        }
        set_socket_options(fd);

        char remote[64] = "";
        if (peer.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&peer;
            inet_ntop(AF_INET, &sin->sin_addr, remote, sizeof(remote) - 8);
            size_t l = strlen(remote);
            snprintf(remote + l, sizeof(remote) - l, ":%u",
                     (unsigned)ntohs(sin->sin_port));
        } else if (peer.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&peer;
            inet_ntop(AF_INET6, &sin6->sin6_addr, remote, sizeof(remote) - 8);
            size_t l = strlen(remote);
            snprintf(remote + l, sizeof(remote) - l, ":%u",
                     (unsigned)ntohs(sin6->sin6_port));
        }
        connection_job job = {.fd = fd};
        memcpy(job.remote, remote, sizeof(remote));
        enqueue(srv, job);
    }
    /* Wake all workers so they can exit. */
    pthread_mutex_lock(&srv->queue_mu);
    atomic_store(&srv->stopping, true);
    pthread_cond_broadcast(&srv->queue_not_empty);
    pthread_mutex_unlock(&srv->queue_mu);
    atomic_store(&srv->accept_thread_done, true);
    return NULL;
}

static void *worker_loop(void *arg)
{
    pq_http_server *srv = arg;
    connection_job job;
    while (dequeue(srv, &job)) {
        handle_connection(srv, job.fd, job.remote);
    }
    atomic_fetch_add(&srv->workers_done, 1);
    return NULL;
}

void pq_http_server_stop(pq_http_server *srv, int64_t grace_period_ms)
{
    if (srv == NULL) return;
    atomic_store(&srv->stopping, true);
    /* Closing the listener unblocks accept() with EBADF on Linux. */
    shutdown(srv->listen_fd, SHUT_RDWR);
    close(srv->listen_fd);
    srv->listen_fd = -1;

    pthread_mutex_lock(&srv->queue_mu);
    pthread_cond_broadcast(&srv->queue_not_empty);
    pthread_mutex_unlock(&srv->queue_mu);

    pthread_join(srv->accept_thread, NULL);
    for (int i = 0; i < srv->worker_count; i++) {
        pthread_join(srv->worker_threads[i], NULL);
    }
    /* Drain any leftover connections. */
    for (int i = 0; i < srv->queue_size; i++) {
        int idx = (srv->queue_head + i) % PQ_HTTP_QUEUE_CAPACITY;
        close(srv->queue[idx].fd);
    }
    free(srv->worker_threads);
    pthread_mutex_destroy(&srv->queue_mu);
    pthread_cond_destroy(&srv->queue_not_empty);
    free(srv);
}