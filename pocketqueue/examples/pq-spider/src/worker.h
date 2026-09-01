/* worker.h - the spider worker. Each thread runs worker_loop until
 * shutdown is observed or max-iterations is hit.
 */
#ifndef SPIDER_WORKER_H
#define SPIDER_WORKER_H

#include <stdbool.h>

#include "dedup.h"
#include "pqctl_client.h"
#include "url_norm.h"

typedef struct {
    pq_client *client;
    dedup_store *dedup;
    int thread_idx;
    int default_visibility_ms;
    int max_depth;
    bool assets;                 /* true = work from `assets` queue */
    volatile bool *shutdown;
} worker_ctx;

void *worker_loop(void *arg);

#endif /* SPIDER_WORKER_H */
