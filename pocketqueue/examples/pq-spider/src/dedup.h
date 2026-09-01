/* dedup.h - persistent URL dedup store.
 *
 * One URL per line, optionally followed by a tab and a single-char status
 * ('v' = visited, 'f' = failed, 'p' = pending). The store is append-only on
 * first encounter and atomically rewritten on close (rename(2) replace).
 */
#ifndef SPIDER_DEDUP_H
#define SPIDER_DEDUP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct dedup_store dedup_store;

dedup_store *dedup_open(const char *path);
void dedup_close(dedup_store *s);

/* Returns true if `url` was already seen (added in a prior run OR earlier
 * in this run). */
bool dedup_contains(const dedup_store *s, const char *url);

/* Mark `url` as seen. If it wasn't already present, append it to the
 * backing file. Returns true if newly added, false if already present. */
bool dedup_add(dedup_store *s, const char *url);

/* Mark a URL's status ('v' / 'f' / 'p'). No-op if the URL isn't tracked. */
void dedup_mark(dedup_store *s, const char *url, char status);

/* Total URLs tracked (for stats / log output). */
size_t dedup_size(const dedup_store *s);

#endif /* SPIDER_DEDUP_H */
