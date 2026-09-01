/* url_norm.h - URL canonicalization.
 *
 * Two URLs that differ only in ways this module normalizes (case, default
 * port, query-param order, fragment, trailing slash) are treated as the
 * same URL for dedup purposes.
 */
#ifndef SPIDER_URL_NORM_H
#define SPIDER_URL_NORM_H

#include <stdbool.h>
#include <stddef.h>

#define URL_MAX 4096

/* Normalize `in` into `out`. The output is always NUL-terminated.
 *
 * Returns true on success, false if the URL is malformed or too long.
 * On false, `out` is set to an empty string. */
bool url_normalize(const char *in, char *out, size_t out_size);

/* Resolve a (possibly relative) URL against a base URL.
 *
 * `base` is the page URL where the reference was found; `ref` is the
 * raw href/src attribute value. The resolved absolute URL is written to
 * `out`. Returns true on success, false if the result is invalid or
 * `out_size` is too small. */
bool url_resolve(const char *base, const char *ref,
                 char *out, size_t out_size);

/* Return true if the URL scheme is one the spider can fetch — http or
 * https. Other schemes (mailto, file, javascript, data, ...) are
 * filtered out. */
bool url_is_fetchable(const char *url);

#endif /* SPIDER_URL_NORM_H */
