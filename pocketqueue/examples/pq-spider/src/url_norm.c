/* url_norm.c - tiny URL normalizer for the dedup store.
 *
 * Not RFC-3986-compliant. Just the rules pq-spider actually relies on:
 *   - scheme and host are lowercased
 *   - default ports (:80 for http, :443 for https) are stripped
 *   - query parameters are sorted lexicographically by key
 *   - fragment is dropped
 *   - trailing slashes on the path are preserved (server may distinguish
 *     /dir vs /dir/)
 *   - relative URLs are resolved against a base URL
 *
 * The implementation is allocation-free: it normalizes in place inside
 * a caller-supplied buffer of URL_MAX bytes.
 */
#include "url_norm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Component extraction                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *scheme_start;
    size_t scheme_len;
    char *host_start;            /* non-const: we lower-case in place */
    size_t host_len;
    const char *port_start;     /* may be NULL */
    size_t port_len;
    char *path_start;
    size_t path_len;
    char *query_start;          /* non-const: we may rewrite query */
    size_t query_len;
    const char *frag_start;     /* may be NULL */
} parsed_url;

static bool parse_url(const char *url, parsed_url *p)
{
    memset(p, 0, sizeof(*p));
    const char *p1 = strstr(url, "://");
    if (p1 == NULL || p1 == url) return false;
    p->scheme_start = url;
    p->scheme_len = (size_t)(p1 - url);
    const char *p2 = p1 + 3;
    p->host_start = p2;
    const char *slash = strchr(p2, '/');
    const char *q = strchr(p2, '?');
    const char *f = strchr(p2, '#');
    const char *path_end = slash ? slash : (q ? q : (f ? f : url + strlen(url)));
    p->host_len = (size_t)(path_end - p2);
    if (f != NULL && f < path_end) path_end = f;
    if (q != NULL && q < path_end) path_end = q;
    if (slash != NULL && slash < path_end) path_end = slash;

    /* Re-derive host len to stop at first /, ?, #. */
    const char *host_end = p2;
    while (host_end < path_end && *host_end != '/' &&
           *host_end != '?' && *host_end != '#') {
        host_end++;
    }
    p->host_len = (size_t)(host_end - p2);

    /* Optional ":port" at end of host. */
    const char *colon = (const char *)memchr(p2, ':', p->host_len);
    if (colon != NULL) {
        p->host_len = (size_t)(colon - p2);
        p->port_start = colon + 1;
        p->port_len = (size_t)(host_end - (colon + 1));
    }

    p->path_start = host_end;
    if (*host_end == '/') {
        p->path_start = host_end;
        const char *end = host_end + 1;
        while (end < path_end || (path_end > host_end && *end)) {
            /* walk until we hit ?, #, or string end */
            if (end >= url + strlen(url)) break;
            if (*end == '?' || *end == '#') break;
            end++;
        }
        /* Actually simpler: the slice up to next ? or #. */
        const char *qe = host_end;
        while (*qe && *qe != '?' && *qe != '#') qe++;
        p->path_len = (size_t)(qe - host_end);
        if (*qe == '?') {
            p->query_start = qe + 1;
            const char *fe = qe + 1;
            while (*fe && *fe != '#') fe++;
            p->query_len = (size_t)(fe - p->query_start);
            if (*fe == '#') p->frag_start = fe + 1;
        } else if (*qe == '#') {
            p->frag_start = qe + 1;
        }
    }
    return true;
}

static void lower_ascii(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
    }
}

static int cmp_qpair(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    while (*pa && *pa != '=' && *pb && *pb != '=') {
        if (*pa != *pb) return (unsigned char)*pa - (unsigned char)*pb;
        pa++; pb++;
    }
    /* If one ends before '=', it sorts first. */
    int ea = (*pa == '=' || *pa == '\0');
    int eb = (*pb == '=' || *pb == '\0');
    if (ea != eb) return ea - eb;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

bool url_normalize(const char *in, char *out, size_t out_size)
{
    if (in == NULL || out == NULL || out_size < 2) {
        if (out && out_size > 0) out[0] = '\0';
        return false;
    }

    /* Operate on a copy of the URL (URL_MAX bytes). */
    char buf[URL_MAX];
    size_t n = strlen(in);
    if (n >= URL_MAX) {
        out[0] = '\0';
        return false;
    }
    memcpy(buf, in, n + 1);

    parsed_url p;
    if (!parse_url(buf, &p)) {
        /* Not a fully-qualified URL — pass through after stripping fragment. */
        char *frag = strchr(buf, '#');
        if (frag) *frag = '\0';
        if (strlen(buf) >= out_size) {
            out[0] = '\0';
            return false;
        }
        strcpy(out, buf);
        return true;
    }

    /* Lower-case the scheme and host in place. */
    lower_ascii(buf, p.scheme_len);
    lower_ascii((char *)p.host_start, p.host_len);

    /* Strip default ports. */
    if (p.port_start != NULL) {
        bool is_default = false;
        size_t sl = p.scheme_len;
        if (sl == 4 && memcmp(buf, "http", 4) == 0 &&
            p.port_len == 2 && memcmp(p.port_start, "80", 2) == 0) is_default = true;
        if (sl == 5 && memcmp(buf, "https", 5) == 0 &&
            p.port_len == 2 && memcmp(p.port_start, "443", 2) == 0) is_default = true;
        if (is_default) {
            memmove((char *)p.port_start - 1, p.port_start + p.port_len,
                    strlen(p.port_start + p.port_len) + 1);
            /* Re-parse after mutation. */
            parse_url(buf, &p);
        }
    }

    /* Sort query parameters. */
    if (p.query_start != NULL && p.query_len > 0) {
        /* Split into k=v pairs, sort, rejoin. */
        char qbuf[URL_MAX];
        const char *src = p.query_start;
        const char *end = p.query_start + p.query_len;
        size_t pair_count = 0;
        const char *pairs[256];
        for (const char *s = src; s < end; ) {
            pairs[pair_count++] = s;
            while (s < end && *s != '&') s++;
            if (s < end) s++;  /* skip & */
        }
        qsort(pairs, pair_count, sizeof(pairs[0]), cmp_qpair);
        size_t off = 0;
        for (size_t i = 0; i < pair_count; i++) {
            const char *e = pairs[i];
            while (e < end && *e != '&') e++;
            size_t len = (size_t)(e - pairs[i]);
            if (i > 0 && off < sizeof(qbuf)) qbuf[off++] = '&';
            if (off + len < sizeof(qbuf)) {
                memcpy(qbuf + off, pairs[i], len);
                off += len;
            }
        }
        qbuf[off] = '\0';
        /* Splice qbuf back into buf at p.query_start. */
        size_t trailing = 0;
        if (p.frag_start != NULL) trailing = strlen(p.frag_start) + 1;
        memmove(p.query_start + off + 1, p.query_start + p.query_len,
                trailing);
        memcpy(p.query_start, qbuf, off);
        p.query_start[off] = '\0';
    }

    /* Drop fragment. */
    char *frag = strchr(buf, '#');
    if (frag) *frag = '\0';

    /* Copy normalized URL to output. */
    if (strlen(buf) >= out_size) {
        out[0] = '\0';
        return false;
    }
    strcpy(out, buf);
    return true;
}

bool url_is_fetchable(const char *url)
{
    if (url == NULL) return false;
    return (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0);
}

bool url_resolve(const char *base, const char *ref,
                 char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return false;
    out[0] = '\0';
    if (ref == NULL || ref[0] == '\0') return false;

    /* Absolute URL — normalize and copy. */
    if (url_is_fetchable(ref)) {
        return url_normalize(ref, out, out_size);
    }

    /* Fragment-only reference — strip and resolve against base. */
    if (ref[0] == '#') {
        if (base == NULL) return false;
        size_t n = strlen(base);
        const char *frag = strchr(base, '#');
        if (frag != NULL) n = (size_t)(frag - base);
        if (n + 1 > out_size) return false;
        memcpy(out, base, n);
        out[n] = '\0';
        return true;
    }

    if (base == NULL) return false;

    /* Resolve relative path against base. */
    /* Strip everything from '?' and '#' in ref for the path component. */
    char refbuf[URL_MAX];
    size_t rn = strlen(ref);
    if (rn >= URL_MAX) return false;
    memcpy(refbuf, ref, rn + 1);
    char *refq = strchr(refbuf, '?');
    char *reffrag = strchr(refbuf, '#');
    char *refend = refq ? refq : (reffrag ? reffrag : refbuf + rn);
    size_t ref_path_len = (size_t)(refend - refbuf);

    /* If ref starts with '/' and isn't '//' (scheme-relative), it's
     * path-absolute: use scheme + host + ref's path. */
    if (refbuf[0] == '/' && refbuf[1] != '/') {
        parsed_url bp;
        if (!parse_url(base, &bp)) return false;
        size_t need = bp.scheme_len + 3 + bp.host_len;
        if (bp.port_start) need += 1 + bp.port_len;
        need += ref_path_len + (refq ? (size_t)(reffrag ? reffrag - refq : strlen(refq)) : 0);
        if (need + 1 > out_size) return false;
        size_t off = 0;
        memcpy(out + off, base, bp.scheme_len); off += bp.scheme_len;
        memcpy(out + off, "://", 3); off += 3;
        memcpy(out + off, bp.host_start, bp.host_len); off += bp.host_len;
        if (bp.port_start) {
            out[off++] = ':';
            memcpy(out + off, bp.port_start, bp.port_len);
            off += bp.port_len;
        }
        memcpy(out + off, refbuf, ref_path_len); off += ref_path_len;
        if (refq) {
            size_t ql = reffrag ? (size_t)(reffrag - refq) : strlen(refq);
            memcpy(out + off, refq, ql); off += ql;
        }
        out[off] = '\0';
        return url_normalize(out, out, out_size);
    }

    /* Otherwise, resolve against base's directory. */
    parsed_url bp;
    if (!parse_url(base, &bp)) return false;

    /* Trim base path to the last '/' (the directory). */
    char dir[URL_MAX];
    size_t dir_len = 0;
    if (base[0] != '\0') {
        const char *slash2 = NULL;
        for (const char *p = base; *p; p++) {
            if (*p == '/') slash2 = p;
        }
        if (slash2 != NULL && slash2 > base + bp.scheme_len + 3 + bp.host_len) {
            dir_len = (size_t)(slash2 - base) + 1;
            memcpy(dir, base, dir_len);
        } else {
            dir_len = bp.scheme_len + 3 + bp.host_len;
            memcpy(dir, base, dir_len);
            dir[dir_len++] = '/';
        }
    }

    if (ref_path_len == 0) {
        /* Reference is just a fragment/query — drop the path. */
        if (dir_len >= out_size) return false;
        memcpy(out, dir, dir_len);
        out[dir_len] = '\0';
        if (refq) {
            size_t ql = reffrag ? (size_t)(reffrag - refq) : strlen(refq);
            if (dir_len + ql + 1 > out_size) return false;
            memcpy(out + dir_len, refq, ql);
            out[dir_len + ql] = '\0';
        }
        return url_normalize(out, out, out_size);
    }

    if (ref_path_len >= sizeof(dir)) return false;
    memcpy(dir + dir_len, refbuf, ref_path_len);
    size_t assembled = dir_len + ref_path_len;
    if (refq) {
        size_t ql = reffrag ? (size_t)(reffrag - refq) : strlen(refq);
        if (assembled + ql + 1 > sizeof(dir)) return false;
        memcpy(dir + assembled, refq, ql);
        assembled += ql;
    }
    dir[assembled] = '\0';

    /* Collapse "./" and "../" segments. */
    char *outp = dir;
    const char *read = dir;
    const char *base_end = dir + dir_len;  /* the directory portion */
    const char *scheme_host_end = strstr(dir, "://");
    scheme_host_end = (scheme_host_end ? scheme_host_end + 3 : dir);
    const char *host_end = strchr(scheme_host_end, '/');
    if (host_end == NULL) host_end = dir + strlen(dir);
    const char *path_start = host_end;

    while (*read) {
        if (read[0] == '.' && read[1] == '/' &&
            (read == path_start - 0 || read[-1] == '/' || read == dir + dir_len)) {
            read += 2;
        } else if (read[0] == '.' && read[1] == '.' && read[2] == '/' &&
                   (read == path_start || read[-1] == '/')) {
            /* Pop the last directory component (but not before path_start). */
            if (outp > path_start) {
                outp--;
                while (outp > path_start && outp[-1] != '/') outp--;
            }
            read += 3;
        } else {
            *outp++ = *read++;
        }
        (void)base_end;  /* future: detect when we exit the base dir */
    }
    *outp = '\0';

    if ((size_t)(outp - dir) + 1 > out_size) return false;
    memcpy(out, dir, (size_t)(outp - dir) + 1);
    return url_normalize(out, out, out_size);
}