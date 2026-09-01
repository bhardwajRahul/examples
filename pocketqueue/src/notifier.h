/* notifier.h - process-local notifier for long-poll waiters (spec §15.4,
 * §29).
 *
 * The notifier is never durable: SQLite is the source of truth. Its job
 * is purely to wake a parked consumer quickly when something may have
 * made a message available (publish, nack, recovery). If the wait times
 * out or the server is shutting down, the consumer still falls through
 * and rechecks the DB.
 *
 * One notifier lives inside pq_service and is broadcast by publish / nack /
 * ack / reservation-recovery paths. wait_until is called by the reserve
 * path when no message is currently eligible.
 */
#ifndef PQ_NOTIFIER_H
#define PQ_NOTIFIER_H

#include <stdbool.h>
#include <stdint.h>

#include "clock.h"

#include <pthread.h>

typedef struct pq_notifier {
    pthread_mutex_t mu;
    pthread_cond_t cond;
    uint64_t generation;
    bool shutting_down;
} pq_notifier;

void pq_notifier_init(pq_notifier *n);
void pq_notifier_destroy(pq_notifier *n);

/* Increment the generation and wake every waiter. Safe to call from
 * any thread at any time — including from within the service's mutex. */
void pq_notifier_broadcast(pq_notifier *n);

/* Wake every waiter and set the shutting_down flag so waiters can
 * observe it and exit promptly. Idempotent. */
void pq_notifier_shutdown(pq_notifier *n);

/* Wait until one of:
 *   - the deadline (absolute, in clock->wall_time_ms units) expires
 *   - someone calls pq_notifier_broadcast (the generation counter moves)
 *   - someone calls pq_notifier_shutdown
 *
 * Returns true if the caller should keep processing (waiting timed out or
 * a generation change happened), false if shutdown was signalled.
 *
 * pthread_cond_timedwait uses the system real-time clock; the supplied
 * pq_clock is consulted for the current time so unit tests with the fake
 * clock can drive time forward with pq_clock_fake_advance_ms.
 */
bool pq_notifier_wait_until(pq_notifier *n, const pq_clock *clock,
                           int64_t deadline_ms);

#endif /* PQ_NOTIFIER_H */