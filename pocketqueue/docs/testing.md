# Testing Guide

PocketQueue has three layers of tests: **unit** (CMocka, in-tree,
<0.1 s), **end-to-end** (Python 3 stdlib, against the live server), and
**example** (the `pq-spider` test which exercises the API from a real
workload). All are run via CTest.

## 1. Quick reference

```bash
# All tests, regular build
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure

# Just one test
ctest --test-dir build -R e2e_api           # by name pattern
ctest --test-dir build -R "stress"           # partial match

# Per-test verbose
ctest --test-dir build -V -R e2e_api

# Sanitizer builds (CI)
cmake -S . -B build-asan -DPQ_ENABLE_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan

cmake -S . -B build-ubsan -DPQ_ENABLE_UBSAN=ON
cmake --build build-ubsan -j
ctest --test-dir build-ubsan
```

## 2. Test layout

```
tests/
├── unit/
│   ├── test_smoke.c        — startup / /healthz / /readyz
│   ├── test_clock.c        — system + fake clock
│   ├── test_config.c       — CLI / env / default config parsing
│   ├── test_str_util.c     — queue-name regex, URL decode, parsers
│   ├── test_random_util.c  — getrandom, UUID v7, hex
│   └── test_queue_service.c — service-layer behaviour
└── e2e/
    ├── smoke.py            — healthz + readyz + 404
    ├── test_api.py         — 13 scenarios from spec §37
    └── stress.py          — 1000 msgs × 8 consumers (spec §36)
```

The unit tests are individual executables (`test_*`) so a single
failure points to a single file. Each e2e Python file is a complete
script that spawns its own server, runs, and tears down — no shared
state between tests.

## 3. Unit tests

### Fake clock

`pq_clock_fake` advances with `pq_clock_fake_advance_ms(N)`. Use this for
any time-dependent test — the alternative is `nanosleep`, which makes
tests slow and flaky.

```c
#include "clock.h"

static void test_reservation_expires(void **state) {
    (void)state;
    pq_clock_fake_set(1000000);
    pq_service_reserve(svc, "q", 1000 /* vis ms */, 0, &m, &err);
    assert_int_equal(m.state, PQ_MSG_RESERVED);
    assert_int_equal(m.attempts, 1);

    /* Step past the deadline. The next reserve should pick up the
     * previously-reserved message with attempts=2. */
    pq_clock_fake_advance_ms(2000);
    pq_service_reserve(svc, "q", 1000, 0, &m2, &err);
    assert_string_equal(m2.id, m.id);
    assert_int_equal(m2.attempts, 2);
}
```

### Memory discipline

Two rules:

1. Always `pq_message_dispose(&msg)` after a reserve. The struct is
   filled in by the service, but `payload_json` is heap-allocated
   and the caller's job to free.
2. Always zero-initialise the message before the call. `pq_message m;`
   is uninitialised stack memory — calling `pq_message_dispose` on it
   would `free()` garbage under UBSan.

```c
pq_message m;
memset(&m, 0, sizeof(m));          /* zero before use */
pq_service_reserve(svc, "q", 0, &m, &err);
/* ...use m... */
pq_message_dispose(&m);
```

### CMocka basics

`pq_unit_test_setup_teardown(test_fn, setup_fn, teardown_fn)` runs
`setup_fn` before and `teardown_fn` after each `test_fn`. We use this
to spin up a fresh `pq_service` per test.

```c
static int setup(void **state) {
    (void)state;
    pq_clock_fake_set(1000000);
    pq_service_config cfg = {
        .clock = pq_clock_fake(),
        .default_visibility_ms = 1000,
        .min_visibility_ms = 100,
        .max_visibility_ms = 60000,
        .default_max_attempts = 3,
        .max_wait_ms = 30000,
    };
    svc = pq_service_create(&cfg, &err);
    return svc ? 0 : -1;
}
```

## 4. End-to-end tests

### Conventions

- All e2e tests are in `tests/e2e/`, written in Python 3 (no third-party
  dependencies — stdlib only).
- They each spawn their own `pocketqueue-server` against a temp DB on
  a free port. The server is killed in a `finally` block.
- Use `pqctl` for normal-path operations and `urllib.request` for
  edge cases (wrong method, bad content-type, etc.).

### Adding a scenario

Open `tests/e2e/test_api.py`. Add a function `scenario_xxx(server_url, queue)` and append it to the `SCENARIOS` list. The test harness spawns one server, then runs every scenario against it in order, using unique queue names per scenario (`q_basic`, `q_nack`, ...) to avoid bleeding state between scenarios.

Each scenario should:

1. Print `--- scenario_xxx [q_xxx]`.
2. Use the `call()` helper to invoke `pqctl`.
3. Use `parse_kv()` to extract fields from `pqctl`'s `key: value` output.
4. `expect()` a condition; on failure an `AssertionError` is raised,
   the harness records it, and continues to the next scenario.

### Stress test

`tests/e2e/stress.py` implements spec §36:

1. Publishes 1,000 messages to a `stress_q` queue.
2. Spawns 8 threads, each calling `consume` → `ack` in a tight loop.
3. After all 1,000 messages are consumed, verifies:
   - Every published ID is in the consumed set.
   - No ID is consumed more than once.
   - The queue is empty (ready=0, reserved=0).

The test takes ~60 s — long enough to surface lock-ordering bugs but
short enough to run on every commit.

## 5. Sanitizer builds

| Flag | What it adds |
|---|---|
| `-DPQ_ENABLE_ASAN=ON` | AddressSanitizer: detects use-after-free, leaks, out-of-bounds, etc. Adds ~2× runtime overhead and 3-5× memory. |
| `-DPQ_ENABLE_UBSAN=ON` | UndefinedBehaviorSanitizer: detects signed overflow, null deref, invalid shifts, etc. Adds ~2× runtime. |

ASan is the most useful for finding memory bugs. UBSan catches
things that "work" on x86 but are technically undefined. Run both
before any non-trivial change.

ASan has been known to flag false positives in third-party code (cJSON,
SQLite). Our build rules silence those warnings for vendored sources,
so any ASan output you see comes from our code.

## 6. Debugging a failing test

A failing test gives a one-line summary in CTest output:

```
8: --- scenario_publish_reserve_ack [q_basic]
8:   FAIL: scenario_publish_reserve_ack: FAIL: consume rc
```

For a deeper view:

```bash
ctest --test-dir build -V -R e2e_api
```

The `-V` flag shows the full stdout/stderr of the test, including any
output the test script wrote.

If the failure is intermittent (timing, race), run it several times:

```bash
for i in {1..10}; do ctest --test-dir build -R e2e_api || break; done
```

For ASan-specific failures, the log shows the exact allocation / free
trace:

```
==PID==ERROR: AddressSanitizer: heap-use-after-free on address 0x...
READ of size 1 at 0x... thread T2
    #0 0x... in worker_loop src/worker.c:107
    #1 0x... in pthread_start ...
0x... is located 8 bytes inside of 48-byte region [0x..., 0x...)
freed by thread T2 here:
    #0 0x... in free ...
    #1 0x... in some_other_function ...
```

The fix is almost always in the **freed by** trace — that's where the
double-free or premature free happened. The **READ of size N at**
trace is the read that used the now-freed memory.

## 7. Test isolation rules

- Each test gets a **fresh temp DB** (different `pick_free_port()` and
  `tempfile.mkdtemp()`).
- Each test has its own `pq_service` in setup, destroyed in teardown.
- The fake clock is reset at the start of each test (`pq_clock_fake_set`).
- **Don't** share state between tests. In particular, don't rely on a
  specific message ID, count, or timestamp.

## 8. When to add a test

| Change | Required test |
|---|---|
| New HTTP endpoint | e2e scenario covering happy path + 1 failure mode |
| Schema migration | unit + e2e + restart-survival scenario |
| State transition | unit test using fake clock |
| New CLI subcommand | e2e scenario + assert exit code matches spec |
| Performance regression | stress test should still pass; the 1,000×8 contract is in CI |
| Library upgrade (cJSON, SQLite) | full re-run under ASan + UBSan |
| New example (pq-spider-style) | e2e test that exercises the real workload |

## 9. Continuous integration

A reasonable CI matrix:

| Build | Tests run |
|---|---|
| regular | all unit + all e2e (including stress) |
| ASan | all unit + all e2e (excluding stress — slow under ASan) |
| UBSan | all unit + all e2e (excluding stress) |
| Clang (if available) | regular tests, with `-Werror` for warnings |

`stress.py` takes ~60 s; budget 120 s for the test timeout.

The repo's `.github/` directory doesn't yet have CI workflows — the
recipes above are the minimum that should be encoded.
