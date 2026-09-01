/* clock.c - system and fake clocks. */
#include "clock.h"

#include <stdatomic.h>
#include <time.h>

static int64_t system_wall_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t system_mono_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const pq_clock g_system_clock = {
    .wall_time_ms = system_wall_ms,
    .monotonic_time_ms = system_mono_ms,
    .ctx = NULL,
};

const pq_clock *pq_clock_system(void)
{
    return &g_system_clock;
}

/* ---- Fake clock --------------------------------------------------------- */
static _Atomic int64_t g_fake_wall_ms = 0;
static _Atomic int64_t g_fake_mono_offset_ms = 0;
static int64_t g_fake_mono_origin_ms = 0;

static int64_t fake_wall_ms(void *ctx)
{
    (void)ctx;
    return atomic_load(&g_fake_wall_ms);
}

static int64_t fake_mono_ms(void *ctx)
{
    (void)ctx;
    /* The fake monotonic clock starts at 0 and only advances. */
    return atomic_load(&g_fake_mono_offset_ms);
}

static pq_clock g_fake_clock = {
    .wall_time_ms = fake_wall_ms,
    .monotonic_time_ms = fake_mono_ms,
    .ctx = NULL,
};

pq_clock *pq_clock_fake(void)
{
    return &g_fake_clock;
}

void pq_clock_fake_set(int64_t wall_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_fake_mono_origin_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    atomic_store(&g_fake_mono_offset_ms, 0);
    atomic_store(&g_fake_wall_ms, wall_ms);
}

void pq_clock_fake_advance_ms(int64_t delta_ms)
{
    atomic_fetch_add(&g_fake_wall_ms, delta_ms);
    atomic_fetch_add(&g_fake_mono_offset_ms, delta_ms);
}

int64_t pq_clock_fake_now_ms(void)
{
    return atomic_load(&g_fake_wall_ms);
}