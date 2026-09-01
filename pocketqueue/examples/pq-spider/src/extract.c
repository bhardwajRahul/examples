/* extract.c - light link extraction. Scans HTML for href / src attributes.
 *
 * Not a real parser; doesn't understand CDATA / comments / <script>.
 * Good enough for a small static test site.
 */
#include "extract.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "url_norm.h"

#define MAX_LINKS_PER_PAGE 1024

static bool already_contains(const extracted *e, const char *url)
{
    for (size_t i = 0; i < e->count; i++) {
        if (strcmp(e->urls[i], url) == 0) return true;
    }
    return false;
}

static bool push_url(extracted *e, const char *url)
{
    if (e->count == MAX_LINKS_PER_PAGE) return false;
    if (already_contains(e, url)) return false;
    if (e->count + 1 > e->cap) {
        size_t newcap = e->cap ? e->cap * 2 : 16;
        char **nb = realloc(e->urls, newcap * sizeof(*nb));
        if (nb == NULL) return false;
        e->urls = nb;
        e->cap = newcap;
    }
    e->urls[e->count] = strdup(url);
    if (e->urls[e->count] == NULL) return false;
    e->count++;
    return true;
}

void extracted_free(extracted *e)
{
    if (e == NULL || e->urls == NULL) return;
    for (size_t i = 0; i < e->count; i++) free(e->urls[i]);
    free(e->urls);
    e->urls = NULL;
    e->count = e->cap = 0;
}

/* Scan `body` for the next href=… or src=… attribute and return one URL.
 *
 * On entry, *cursor points into the body. On exit, *cursor is past the
 * closing quote (or past the unquoted value). Returns false when no more
 * attribute occurrences remain. */
static bool extract_once(const char *body, size_t body_len, size_t *cursor,
                        const char *base_url, extracted *e)
{
    static const char *const names[] = {
        "href", "src", "poster", "cite", "background",
    };
    size_t nnames = sizeof(names) / sizeof(names[0]);

    while (*cursor + 5 < body_len) {
        /* Find the next occurrence of any of the attribute names. */
        size_t best_pos = (size_t)-1;
        size_t best_name_len = 0;
        for (size_t i = 0; i < nnames; i++) {
            size_t nlen = strlen(names[i]);
            for (size_t p = *cursor; p + nlen < body_len; p++) {
                if (body[p] != '<' && body[p] != names[i][0]) continue;
                size_t scan_start = (body[p] == '<') ? p + 1 : p;
                if (scan_start + nlen > body_len) continue;
                if (strncasecmp(body + scan_start, names[i], nlen) != 0) continue;
                char c = body[scan_start + nlen];
                if (c != '=' && c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    continue;
                if (scan_start < best_pos) {
                    best_pos = scan_start;
                    best_name_len = nlen;
                }
            }
        }
        if (best_pos == (size_t)-1) {
            *cursor = body_len;
            return false;
        }

        /* Skip past "name=" then whitespace then the value. */
        size_t p = best_pos + best_name_len;
        while (p < body_len && body[p] != '=') p++;
        if (p >= body_len || body[p] != '=') {
            *cursor = best_pos + best_name_len;
            continue;
        }
        p++;
        while (p < body_len && (body[p] == ' ' || body[p] == '\t')) p++;
        if (p >= body_len) return false;
        char quote = body[p];
        size_t start, end;
        if (quote == '"' || quote == '\'') {
            start = p + 1;
            end = start;
            while (end < body_len && body[end] != quote) end++;
        } else {
            start = p;
            end = p;
            while (end < body_len && body[end] != '>' &&
                   body[end] != ' ' && body[end] != '\t' &&
                   body[end] != '\n' && body[end] != '\r') end++;
        }

        if (end > start) {
            char raw[URL_MAX];
            size_t vlen = end - start;
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, body + start, vlen);
            raw[vlen] = '\0';

            /* Skip JavaScript / data / mailto / etc. */
            if (strncmp(raw, "javascript:", 11) == 0 ||
                strncmp(raw, "data:", 5) == 0 ||
                strncmp(raw, "mailto:", 7) == 0 ||
                raw[0] == '#') {
                *cursor = (quote == '"' || quote == '\'') ? end + 1 : end;
                return true;
            }

            char resolved[URL_MAX];
            if (!url_resolve(base_url, raw, resolved, sizeof(resolved))) {
                *cursor = (quote == '"' || quote == '\'') ? end + 1 : end;
                return true;
            }
            if (!url_is_fetchable(resolved)) {
                *cursor = (quote == '"' || quote == '\'') ? end + 1 : end;
                return true;
            }
            char norm[URL_MAX];
            if (url_normalize(resolved, norm, sizeof(norm))) {
                push_url(e, norm);
            }
        }
        *cursor = (quote == '"' || quote == '\'') ? end + 1 : end;
        return true;
    }
    return false;
}

bool extract_links(const char *body, size_t body_len,
                   const char *base_url, extracted *e)
{
    if (body == NULL || base_url == NULL) return false;
    memset(e, 0, sizeof(*e));
    size_t cursor = 0;
    while (extract_once(body, body_len, &cursor, base_url, e)) {}
    return true;
}
