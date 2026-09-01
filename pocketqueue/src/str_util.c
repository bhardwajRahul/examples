/* str_util.c - bounded string helpers. */
#include "str_util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pq_str_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return -1;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }
    size_t n = strlen(src);
    if (n >= dst_size) {
        memcpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return -1;
    }
    memcpy(dst, src, n + 1);
    return (int)n;
}

bool pq_str_is_valid_queue_name(const char *name, bool allow_dead_suffix)
{
    if (name == NULL) {
        return false;
    }
    size_t n = strlen(name);
    if (n == 0 || n > 64) {
        return false;
    }
    /* First character: alphanumeric. */
    if (!(isalnum((unsigned char)name[0]))) {
        return false;
    }
    /* Remaining: alphanumeric, '.', '_', '-'. */
    for (size_t i = 1; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    /* Spec §8.1: ".dead" is reserved for dead-letter queues. */
    if (!allow_dead_suffix && n >= 5 &&
        strcmp(name + n - 5, ".dead") == 0) {
        return false;
    }
    return true;
}

static int hex_nybble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int pq_str_url_decode(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (dst_size == 0 || src == NULL) {
        return -1;
    }
    size_t i = 0, j = 0;
    while (i < src_len) {
        if (src[i] == '%' && i + 2 < src_len && j + 1 < dst_size) {
            int hi = hex_nybble((unsigned char)src[i + 1]);
            int lo = hex_nybble((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        if (j + 1 >= dst_size) {
            break;
        }
        dst[j++] = (src[i] == '+') ? ' ' : src[i];
        i++;
    }
    dst[j] = '\0';
    return (int)j;
}

bool pq_str_parse_int64(const char *s, int64_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

bool pq_str_parse_uint16(const char *s, uint16_t *out)
{
    int64_t v;
    if (!pq_str_parse_int64(s, &v)) return false;
    if (v < 0 || v > 65535) return false;
    *out = (uint16_t)v;
    return true;
}

bool pq_str_parse_int(const char *s, int *out)
{
    int64_t v;
    if (!pq_str_parse_int64(s, &v)) return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

int pq_str_iequal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return (a == b) ? 0 : 1;
    }
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return 1;
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

void pq_str_append_dot_dead(char *s, size_t size)
{
    if (s == NULL || size == 0) return;
    size_t cur = strnlen(s, size);
    if (cur + 5 >= size) {
        /* Not enough room — truncate the base to make space. */
        if (size > 6) {
            s[size - 6] = '\0';
            cur = size - 6;
        } else if (size > 0) {
            s[0] = '\0';
            return;
        }
    }
    memcpy(s + cur, ".dead", 6); /* includes NUL */
}