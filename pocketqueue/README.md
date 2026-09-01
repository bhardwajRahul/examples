# PocketQueue

A lightweight, durable message queue for local development, small internal
systems, and coding-harness evaluation. PocketQueue provides named FIFO queues
with at-least-once delivery, visibility timeouts, retries, dead-letter queues,
and long-polling consumers. Everything is persisted in a single SQLite file.

## Features

- Named FIFO queues with optional message-ordering tie-breakers
- JSON payloads over HTTP/1.1
- Durable SQLite-backed storage (no external broker required)
- Atomic reservation using `BEGIN IMMEDIATE` transactions
- Message acknowledgement (`ack`) and negative acknowledgement (`nack`)
- Visibility timeouts with automatic recovery
- Configurable retry limit with automatic dead-letter routing (`<queue>.dead`)
- Long-polling consumers (HTTP `wait_ms` up to `--max-wait-ms`)
- Graceful shutdown on `SIGINT` / `SIGTERM`
- Command-line client (`pqctl`)
- Concurrency-safe with global database mutex
- CMocka unit tests and Python 3 end-to-end tests, all runnable through CTest
- Optional AddressSanitizer / UndefinedBehaviorSanitizer builds

## Build & Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional sanitizer builds:

```bash
cmake -S . -B build-asan -DPQ_ENABLE_ASAN=ON && cmake --build build-asan && ctest --test-dir build-asan
cmake -S . -B build-ubsan -DPQ_ENABLE_UBSAN=ON && cmake --build build-ubsan && ctest --test-dir build-ubsan
```

Vendored dependencies: SQLite 3 (amalgamation, downloaded by CMake), cJSON
(single-file, vendored in `third_party/`), cmocka (unit tests, fetched by
CMake). The HTTP server is hand-rolled — see `PLAN.md` D1 for rationale.

## Run

```bash
./build/pocketqueue-server --database ./pocketqueue.db
```

By default the server binds to `127.0.0.1:8080`, applies migrations in
`./migrations/`, logs to stderr, and answers:

```bash
curl http://127.0.0.1:8080/healthz   # {"status":"ok"}
curl http://127.0.0.1:8080/readyz    # {"status":"ready"}
```

`pqctl` is fully implemented; the HTTP API is documented in [`docs/api.md`](docs/api.md).
For installation, configuration, and a quick start, see [`docs/user-guide.md`](docs/user-guide.md).
For internal design (modules, request lifecycle, recovery, threading),
see [`docs/architecture.md`](docs/architecture.md).
To add a feature or migration, read [`docs/development.md`](docs/development.md).
To write or run tests, read [`docs/testing.md`](docs/testing.md).
The full specification is in [`PocketQueue.md`](PocketQueue.md) and the
implementation roadmap in [`PLAN.md`](PLAN.md).

## Project layout

```
CMakeLists.txt           top-level build (CMake ≥ 3.20, C17)
include/pocketqueue/    public C headers
src/                    core library, server, pqctl
migrations/             Vnnn__name.sql files (V001__init, V002__messages)
tests/{unit,e2e}/       CMocka tests + Python e2e harness
docs/                   user-guide, api, architecture, development, testing
examples/pq-spider/     breadth-first web-crawler example
third_party/            cJSON.{c,h} (single-file vendored copy)
```

## License

MIT — see `LICENSE`.
