/* http_fetch.c - small plain-socket HTTP/1.1 GET client.
 *
 * Handles enough of RFC 7230 to fetch a body: parses status line and
 * headers, reads Content-Length bytes. No keep-alive, no compressed
 * responses (we don't send Accept-Encoding).
 */
#include "http_fetch.h"

#include <ctype.h>
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

void fetch_response_free(fetch_response *r)
{
    if (r == NULL) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->content_type[0] = '\0';
}

static int parse_url(const char *url, char *host, size_t hl,
                     char *port, size_t pl, const char **path)
{
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        size_t hn = (size_t)(colon - p);
        if (hn >= hl) return -1;
        memcpy(host, p, hn); host[hn] = '\0';
        size_t pn = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (pn >= pl) return -1;
        memcpy(port, colon + 1, pn); port[pn] = '\0';
    } else {
        size_t hn = slash ? (size_t)(slash - p) : strlen(p);
        if (hn >= hl) return -1;
        memcpy(host, p, hn); host[hn] = '\0';
        snprintf(port, pl, "80");
    }
    *path = slash ? slash : "/";
    return 0;
}

/* Read until "\r\n\r\n" (headers terminator). Returns total bytes read
 * (headers + body), or -1 on error. The headers region is also returned
 * via *header_end (one past the last header byte). */
static int read_headers(int fd, char *buf, size_t cap, size_t *header_end)
{
    size_t got = 0;
    while (got < cap) {
        ssize_t n = recv(fd, buf + got, cap - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;  /* EOF before headers complete */
        got += n;
        char *p = (char *)memmem(buf, got, "\r\n\r\n", 4);
        if (p != NULL) {
            *header_end = (size_t)(p - buf) + 4;
            return (int)got;
        }
    }
    return -1;
}

bool http_fetch(const char *url, fetch_response *r)
{
    if (r == NULL || url == NULL) return false;
    memset(r, 0, sizeof(*r));

    char host[256];
    char port[8];
    const char *path;
    if (parse_url(url, host, sizeof(host), port, sizeof(port), &path) != 0)
        return false;

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) return false;

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

    struct timeval tv = {FETCH_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[1024];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s:%s\r\n"
                      "User-Agent: pq-spider/1.0\r\n"
                      "Accept: text/html,application/xhtml+xml,*/*\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, host, port);
    size_t sent = 0;
    while (sent < (size_t)rl) {
        ssize_t n = send(fd, req + sent, rl - sent, 0);
        if (n <= 0) { close(fd); return false; }
        sent += n;
    }

    /* Read headers + first chunk of body. */
    size_t cap = 8192, used = 0;
    char *buf = malloc(cap);
    if (buf == NULL) { close(fd); return false; }
    size_t header_end = 0;
    int got = read_headers(fd, buf, cap - 1, &header_end);
    if (got < 0) { free(buf); close(fd); return false; }
    used = (size_t)got;

    /* Parse status + headers. */
    long status = 0;
    {
        char *line_end = memchr(buf, '\r', header_end);
        if (line_end == NULL) { free(buf); close(fd); return false; }
        *line_end = '\0';
        const char *p = buf;
        if (sscanf(p, "HTTP/1.%*c %ld", &status) < 1) status = 0;
        *line_end = '\r';
        /* Walk headers, capture Content-Type. */
        char *headers = line_end + 2;
        for (char *h = headers; h < buf + header_end - 2; ) {
            char *eol = memmem(h, (buf + header_end) - h, "\r\n", 2);
            if (eol == NULL) break;
            *eol = '\0';
            static const char prefix[] = "content-type:";
            size_t pl = sizeof(prefix) - 1;
            if (strncasecmp(h, prefix, pl) == 0) {
                const char *v = h + pl;
                while (*v == ' ' || *v == '\t') v++;
                size_t vlen = strlen(v);
                while (vlen > 0 && (v[vlen-1] == ' ' || v[vlen-1] == '\t')) vlen--;
                if (vlen >= sizeof(r->content_type)) vlen = sizeof(r->content_type) - 1;
                memcpy(r->content_type, v, vlen);
                r->content_type[vlen] = '\0';
                char *semi = strchr(r->content_type, ';');
                if (semi != NULL) *semi = '\0';
            }
            h = eol + 2;
        }
    }
    r->http_status = status;

    /* Read body up to FETCH_MAX_BODY. */
    size_t header_consumed = used - header_end;
    size_t body_off = header_consumed;
    size_t body_cap = cap - header_end;
    char *body = malloc(body_cap + 1);
    if (body == NULL) { free(buf); close(fd); return false; }
    memcpy(body, buf + header_end, header_consumed);
    free(buf); buf = NULL;

    while (1) {
        if (body_off >= body_cap) {
            if (body_cap >= FETCH_MAX_BODY) {
                /* Cap hit. */
                break;
            }
            size_t new_cap = body_cap * 2;
            if (new_cap > FETCH_MAX_BODY) new_cap = FETCH_MAX_BODY;
            char *nb = realloc(body, new_cap + 1);
            if (nb == NULL) { free(body); close(fd); return false; }
            body = nb;
            body_cap = new_cap;
        }
        if (body_off >= body_cap) break;
        ssize_t n = recv(fd, body + body_off, body_cap - body_off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        body_off += (size_t)n;
    }
    close(fd);
    body[body_off] = '\0';

    r->body = body;
    r->body_len = body_off;
    return true;
}
