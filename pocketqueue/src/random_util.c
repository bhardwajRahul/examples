/* random_util.c - secure random (getrandom + /dev/urandom fallback). */
#include "random_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/random.h>
#endif

bool pq_random_bytes(void *buf, size_t n)
{
    if (buf == NULL || n == 0) {
        return n == 0;
    }
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;

#if defined(__linux__) && defined(SYS_getrandom)
    /* getrandom() with GRND_NONBLOCK avoids hangs on uninitialised pools
     * and accepts a non-NUL-terminated buffer. */
    while (got < n) {
        long r = syscall(SYS_getrandom, p + got, n - got, 0x0001 /* GRND_NONBLOCK */);
        if (r < 0) {
            if (errno == EINTR) continue;
            break; /* fall back to /dev/urandom */
        }
        got += (size_t)r;
        if (r == 0) break;
    }
#endif
    if (got < n) {
        /* /dev/urandom fallback. We open lazily on first need; the cost is
         * one open per process, but we accept a small startup hit to
         * keep the implementation simple across kernel versions. */
        static int urandom_fd = -1;
        if (urandom_fd < 0) {
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return false;
            }
            urandom_fd = fd;
        }
        while (got < n) {
            ssize_t r = read(urandom_fd, p + got, n - got);
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (r == 0) return false;
            got += (size_t)r;
        }
    }
    return true;
}

static void hex_encode(char *dst, const uint8_t *src, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        dst[2 * i]     = hex[src[i] >> 4];
        dst[2 * i + 1] = hex[src[i] & 0x0F];
    }
}

bool pq_random_hex(char out[33], size_t byte_count)
{
    if (byte_count > 16) {
        return false; /* overflows 32 hex chars + NUL */
    }
    uint8_t buf[16];
    if (!pq_random_bytes(buf, byte_count)) {
        return false;
    }
    hex_encode(out, buf, byte_count);
    out[2 * byte_count] = '\0';
    return true;
}

bool pq_random_uuid_v7(char out[37])
{
    /* RFC 9562 §5.7 layout (big-endian, network byte order on the wire):
     *   0                   1                   2                   3
     *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                           unix_ts_ms                          |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |          unix_ts_ms           |  ver  |       rand_a          |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |var|                        rand_b                             |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                            rand_b                             |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */
    uint8_t b[16];
    if (!pq_random_bytes(b, 16)) {
        return false;
    }
    int64_t ts_ms = 0;
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            ts_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        }
    }
    b[0]  = (uint8_t)((ts_ms >> 40) & 0xFF);
    b[1]  = (uint8_t)((ts_ms >> 32) & 0xFF);
    b[2]  = (uint8_t)((ts_ms >> 24) & 0xFF);
    b[3]  = (uint8_t)((ts_ms >> 16) & 0xFF);
    b[4]  = (uint8_t)((ts_ms >>  8) & 0xFF);
    b[5]  = (uint8_t)((ts_ms      ) & 0xFF);
    /* version (7) in high 4 bits of byte 6 */
    b[6]  = (uint8_t)((b[6] & 0x0F) | 0x70);
    /* variant (10xx) in high 2 bits of byte 8 */
    b[8]  = (uint8_t)((b[8] & 0x3F) | 0x80);
    /* Format: 8-4-4-4-12. */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return true;
}