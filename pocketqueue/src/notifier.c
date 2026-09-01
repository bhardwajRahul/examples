/* notifier.c - see notifier.h for design. */
#include "notifier.h"

#include <errno.h>
#include <time.h>

/* Convert an absolute wall-time deadline in milliseconds (relative to
 * CLOCK_REALTIME) to an absolute timespec suitable for
 * pthread_cond_timedwait. Assumes the deadline is in the future; if it
 * isn't, the condvar wait returns immediately with ETIMEDOUT. */
static void deadline_to_abstime(int64_t deadline_ms, struct timespec *out)
{
    clock_gettime(CLOCK_REALTIME, out);
    int64_t cur_ms = (int64_t)out->tv_sec * 1000 + out->tv_nsec / 1000000;
    int64_t delta_ms = deadline_ms - cur_ms;
    if (delta_ms <= 0) {
        /* Already past — set to the current moment. */
        return;
    }
    out->tv_sec += delta_ms / 1000;
    out->tv_nsec += (delta_ms % 1000) * 1000000;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

void pq_notifier_init(pq_notifier *n)
{
    pthread_mutex_init(&n->mu, NULL);
    pthread_cond_init(&n->cond, NULL);
    n->generation = 0;
    n->shutting_down = false;
}

void pq_notifier_destroy(pq_notifier *n)
{
    pthread_cond_destroy(&n->cond);
    pthread_mutex_destroy(&n->mu);
}

void pq_notifier_broadcast(pq_notifier *n)
{
    pthread_mutex_lock(&n->mu);
    n->generation++;
    pthread_cond_broadcast(&n->cond);
    pthread_mutex_unlock(&n->mu);
}

void pq_notifier_shutdown(pq_notifier *n)
{
    pthread_mutex_lock(&n->mu);
    n->shutting_down = true;
    pthread_cond_broadcast(&n->cond);
    pthread_mutex_unlock(&n->mu);
}

bool pq_notifier_wait_until(pq_notifier *n, const pq_clock *clock,
                           int64_t deadline_ms)
{
    pthread_mutex_lock(&n->mu);
    uint64_t start_gen = n->generation;
    while (n->generation == start_gen && !n->shutting_down) {
        int64_t now = clock->wall_time_ms(clock->ctx);
        if (now >= deadline_ms) break;
        int64_t remaining = deadline_ms - now;

        /* Refuse to wait if the deadline has already passed. Without
         * this guard the condvar could wake immediately with
         * ETIMEDOUT. */
        if (remaining < 1) remaining = 1;

        struct timespec abs_deadline;
        deadline_to_abstime(deadline_ms, &abs_deadline);

        int rc = pthread_cond_timedwait(&n->cond, &n->mu, &abs_deadline);
        if (rc == ETIMEDOUT) {
            /* Recheck; loop will exit when generation advances or deadline
             * is reached. */
        }
    }
    bool shutdown = n->shutting_down;
    pthread_mutex_unlock(&n->mu);
    return !shutdown;
}