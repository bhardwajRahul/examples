/* clock.h - injectable clock interface (spec §12).
 *
 * Tests use pq_clock_fake to advance time deterministically.
 */
#ifndef PQ_CLOCK_H
#define PQ_CLOCK_H

#include <stdint.h>

typedef struct pq_clock {
    int64_t (*wall_time_ms)(void *ctx);     /* Unix epoch milliseconds */
    int64_t (*monotonic_time_ms)(void *ctx); /* Monotonic ms, no epoch */
    void *ctx;
} pq_clock;

/* Wall + monotonic clock backed by system calls. */
const pq_clock *pq_clock_system(void);

/* Manual clock: set with pq_clock_fake_set(), advanced with
 * pq_clock_fake_advance_ms(). Wall time is the fake time; monotonic time is
 * also the fake time (tests don't care about the distinction). */
pq_clock *pq_clock_fake(void);
void pq_clock_fake_set(int64_t wall_ms);
void pq_clock_fake_advance_ms(int64_t delta_ms);
int64_t pq_clock_fake_now_ms(void);

#endif /* PQ_CLOCK_H */