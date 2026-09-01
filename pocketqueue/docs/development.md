# Development Guide

PocketQueue is C17, single binary, ~6,200 lines of project C (excluding
vendored cJSON and the SQLite amalgamation). This guide covers how to
build, what conventions to follow, and how to extend it.

## 1. Build commands

```bash
# Configure + build
cmake -S . -B build
cmake --build build -j

# Run all tests
ctest --test-dir build --output-on-failure

# Sanitizer builds
cmake -S . -B build-asan -DPQ_ENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan

cmake -S . -B build-ubsan -DPQ_ENABLE_UBSAN=ON
cmake --build build-ubsan
ctest --test-dir build-ubsan

# pq-spider example
build/examples/pq-spider/pq-spider --server http://127.0.0.1:8080 \
    --seeds examples/pq-spider/seeds.txt
```

Dependencies (auto-fetched by CMake):
- **SQLite** amalgamation from sqlite.org (one .c + one .h, no native dep)
- **cJSON** (single .c + .h in `third_party/`)
- **cmocka** for unit tests (FetchContent)
- **pthread** (system)
- (For `pq-spider` only) nothing — uses raw sockets.

## 2. Source layout

```
include/pocketqueue/   public C API headers
src/                   core library
  config.{c,h}         CLI/env parsing
  logger.{c,h}         leveled logger (text + JSON)
  clock.{c,h}          pq_clock vtable + system / fake
  http_server.{c,h}     hand-rolled HTTP/1.1 listener
  http_routes.{c,h}    HTTP → service dispatch
  json_util.{c,h}      cJSON wrappers
  str_util.{c,h}       queue-name regex, parsers
  random_util.{c,h}    getrandom() + UUIDv7
  notifier.{c,h}       condvar-based wakeup
  sqlite_repository.{c,h} SQLite handle + transactions
  migrations.{c,h}     Vnnn__name.sql loader
  queue_service.{c,h}   public dispatch + vtable
  queue_service_inmem.c in-memory backend
  queue_service_sqlite.c SQLite backend
  pqctl_main.c         CLI client
  server_main.c        server entry point

migrations/
  V001__init.sql       schema_version table
  V002__messages.sql   messages table + indexes

tests/
  unit/                CMocka unit tests
  e2e/                 Python 3 stdlib-only e2e tests

examples/pq-spider/    breadth-first web crawler

third_party/           vendored cJSON
```

The split between public (`include/pocketqueue/`) and internal
(`src/*.h`) is enforced by the `pq_service_vtable` indirection — the
HTTP layer never sees the in-memory linked-list type, and the in-memory
backend never sees HTTP.

## 3. Coding conventions

### Style

- C17 (`-std=c17`)
- 4-space indent, 100-column wrap (`.clang-format` is in the root)
- `snake_case` for functions / variables
- `pq_` prefix for **public** types and functions
- `PQ_` prefix for **public** macros and enum values
- `static` for anything not in a header
- One responsibility per function where practical
- No hidden mutable globals
- No `.c`-file includes — every header is its own consumer's first include

### Memory ownership

Every allocation has a single owner. Conventions:

- **`pq_message`** is filled by `pq_service_publish` / `pq_service_reserve`
  and contains a heap-allocated `payload_json`. The **caller** owns it
  after the call returns and must call `pq_message_dispose()` to free the
  payload. Other fields are shallow copies owned by the service.
- **`pq_http_response.body`** is a heap-allocated string owned by the
  HTTP handler. The dispatcher (server-side) calls
  `pq_http_routes_dispose(&resp)` after sending the response.
- **Repository statements** (`sqlite3_stmt*`) are prepared once at service
  init and reused for the service lifetime. The repo's mutex serializes
  access.
- **Errors**: a `pq_error` is a fixed-size struct; do not heap-allocate.

### Error model

Public functions return a `pq_status` enum and write a bounded
`pq_error` struct via out-parameter. Never use a global error variable.
Status codes are listed in `include/pocketqueue/pocketqueue.h`.

```c
pq_status s = pq_service_publish(svc, &req, &msg, &err);
if (s != PQ_OK) {
    log_warn("publish failed: %s (%s)", err.message, err.code);
    return translate_status(s);
}
```

### Concurrency

- All public service functions are thread-safe (they take the repo mutex).
- The HTTP server runs handlers in parallel on a thread pool.
- The `pq_notifier` is shared across all backends; it's the only
  cross-thread wake-up mechanism.
- Don't hold the repo mutex across a blocking syscall. Long-poll waits
  release the mutex *before* sleeping on the condvar.

## 4. Adding a database migration

PocketQueue uses sequential `Vnnn__name.sql` files (D5 in the plan).
Adding one is straightforward:

1. Create `migrations/V003__your_change.sql` (next number).
2. Bump `PQ_SUPPORTED_SCHEMA_VERSION` in `src/server_main.c`.
3. Add tests if the migration is non-trivial.
4. Build, run, and confirm the new schema is applied:

```bash
./build/pocketqueue-server --database /tmp/pq.db
# Look for: "schema version: 3"
sqlite3 /tmp/pq.db "SELECT version, applied_at FROM schema_version;"
```

If a fresh start from the new state is needed, delete the DB file
**and** bump the version. The migrations loader refuses to start on a
newer database than the binary supports — it treats that as a hard
error rather than silently downgrading.

The migration script is run inside a single `BEGIN; ... COMMIT;`
transaction. If your migration is non-idempotent (e.g. `CREATE TABLE
messages`), make sure its name (V003) makes the operation implicit:
running V003 twice is the second application's problem, not yours.

## 5. Adding a new HTTP endpoint

The HTTP layer is structured around a single `pq_http_routes_dispatch`
function that matches path templates to handlers. To add an endpoint:

1. Pick a path under the existing `match_queue_path` (which recognizes
   `/queues/{q}/messages[/{id}[/ack|nack]]`). For a new top-level
   shape, extend the matcher.
2. Add a `handle_*` function in `src/http_routes.c`.
3. Use the vtable to forward to the service — never reach into a
   backend directly.
4. Return a fully-initialized `pq_http_response` via:
   - `make_json_response(status, body, NULL, NULL)` for JSON bodies
   - `empty_response(status)` for empty (204 etc.) bodies
   - `pq_text_response(status, ctype, body)` for static-text bodies
5. The dispatcher will route to the right verb automatically.
6. Add at least one e2e scenario in `tests/e2e/test_api.py`.

Example: adding a hypothetical `GET /queues/{q}/peek`:

```c
static pq_http_response handle_peek(pq_service *svc, const char *queue,
                                   const pq_http_request *req)
{
    (void)req;
    /* Build response from a transient in-memory peek. */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "queue", queue);
    cJSON_AddNumberToObject(body, "ok", 1);
    return make_json_response(200, body, NULL, NULL);
}
```

Then in `pq_http_routes_dispatch`, add a `peek` suffix case.

## 6. Adding a unit test

CMocka tests live in `tests/unit/`. Each test is a small C file with
its own `main()`. To add a new module test:

1. Create `tests/unit/test_<module>.c`.
2. Add the file to `tests/unit/CMakeLists.txt`:

   ```cmake
   add_executable(test_<module> test_<module>.c)
   target_include_directories(test_<module> PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
   target_link_libraries(test_<module> PRIVATE pocketqueue_core cmocka)
   add_test(NAME test_<module> COMMAND test_<module>)
   ```

3. Use `pq_clock_fake` when you need to drive time. Real
   `clock_gettime`-based code makes tests slow and flaky.

CMocka boilerplate:

```c
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include "queue_name.h"

static void test_valid(void **state) {
    (void)state;
    assert_true(pq_str_is_valid_queue_name("jobs", false));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_valid),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
```

## 7. Adding an e2e test

`tests/e2e/` is a flat directory of Python 3 scripts. Each:

1. Picks a free port (`tests/e2e/test_api.py: pick_free_port`)
2. Spawns `pocketqueue-server` against a temp DB
3. Hits the API with `urllib.request`
4. Asserts on the responses
5. Cleans up the server

Add a new file in `tests/e2e/`, register it in
`tests/e2e/CMakeLists.txt`. Stdlib-only — no `requests` / `pytest`.

## 8. Debugging tips

- **Server logs** go to stderr at `--log-level LEVEL`. Use
  `--log-level debug` to see request flow.
- **DB inspection** is one `sqlite3 db.db ".schema"` away. WAL files
  (`-wal`, `-shm`) are part of the durable state.
- **Stuck long-poll** in the test? `pqctl stats <queue>` — if
  `reserved > 0` and never goes down, your consumer is leaking
  reservations (visibility timeout isn't being honored).
- **Dead-letter growth** is normal for poison URLs. Drain
  `<queue>.dead` periodically if your job mix has any.
- **ASan trace** is detailed — when a test fails under ASan, the
  log shows the allocator that owned the freed block.

## 9. Releasing

The repository doesn't have a release process yet. When you cut a
release:

1. Update `CMakeLists.txt` `project(pocketqueue VERSION 1.0.0 ...)`.
2. Update `src/pocketqueue-server` log line that prints the version
   string (currently hard-coded to "1.0.0" — change the literal).
3. Tag the commit: `git tag v1.0.0`.
4. Build and test once more on a clean machine:
   ```bash
   cmake -S . -B build
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```
5. Manual smoke (spec §48):
   ```bash
   ./build/pocketqueue-server --database /tmp/pq.db &
   ./build/pqctl publish jobs '{"task":"demo"}'
   ./build/pqctl consume jobs --wait-ms 1000
   ./build/pqctl ack jobs <id> <receipt>
   ./build/pqctl stats jobs
   ```
6. Publish a tarball. The repo builds offline (vendored cJSON +
   amalgamated SQLite + FetchContent for cmocka), so this works
   in a CI artifact without network.
