# PocketQueue User Guide

PocketQueue is a small, durable message queue for local development, small
internal systems, and coding-harness evaluation. It speaks HTTP/1.1 +
JSON, persists every message in a single SQLite file, and provides named
FIFO queues with at-least-once delivery, visibility timeouts, retries,
and dead-letter routing.

> **Delivery semantics.** PocketQueue provides *at-least-once* delivery. A
> consumer may receive the same message more than once if it does not
> acknowledge before its visibility timeout, or if the server crashes
> between delivering and persisting the acknowledgement. Always make your
> consumers idempotent.

---

## 1. Install & run

### Build from source

```bash
cmake -S . -B build
cmake --build build -j
```

This produces:

| Binary | Path |
|---|---|
| `pocketqueue-server` | `build/pocketqueue-server` |
| `pqctl` (CLI client) | `build/pqctl` |
| `pq-spider` (example crawler) | `build/examples/pq-spider/pq-spider` |

### Sanitizer builds (recommended for CI)

```bash
cmake -S . -B build-asan -DPQ_ENABLE_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Same recipe for `-DPQ_ENABLE_UBSAN=ON`.

### Start the server

```bash
./build/pocketqueue-server --database ./pocketqueue.db
```

Defaults: bind `127.0.0.1:8080`, log level `info`, log format `text`.
Full flag list:

```
--bind ADDRESS              # default 127.0.0.1
--port PORT                 # default 8080
--database PATH             # default pocketqueue.db
--migrations-dir PATH      # default ./migrations
--default-visibility-ms N   # default 30000
--min-visibility-ms N       # default 1000
--max-visibility-ms N       # default 600000
--default-max-attempts N    # default 3
--max-wait-ms N             # default 30000  (long-poll ceiling)
--max-body-bytes N          # default 1048576 (1 MiB)
--worker-threads N          # default 8
--log-level LEVEL           # error | warn | info | debug
--log-format FORMAT         # text | json
```

Every flag has an environment variable equivalent: `PQ_PORT`, `PQ_DATABASE`, etc. CLI > env > built-in defaults.

### Stop the server

`SIGINT` / `SIGTERM` triggers graceful shutdown — workers drain, long-poll waiters wake up, in-flight requests are allowed up to one visibility window to finish.

---

## 2. Quick start with `pqctl`

```bash
# Publish
./build/pqctl publish jobs '{"task":"resize","file":"photo.jpg"}'

# Reserve (5s long-poll — returns when a message is available)
./build/pqctl consume jobs --wait-ms 5000

# Ack
./build/pqctl ack jobs <message-id> <receipt>
```

Output of `consume`:

```
id:        019f657e-62c2-7885-9add-9619d78a80c5
receipt:   c7a40ccc013cbce96b319b350b9fd9f5
attempts:  1
payload:   {"task":"resize"}
```

`--raw` prints the response body as-is (no `key: value` formatting).

`--server URL` (or `PQ_SERVER_URL` env) lets you point at a non-default server. The full list of subcommands is in [`api.md`](./api.md#command-line-client-pqctl).

---

## 3. Queues, messages, and the lifecycle

### Queue names

- Regex: `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`
- Case-sensitive
- The `.dead` suffix is reserved for dead-letter queues

### Message states

```
              PUBLISH
                │
                ▼
              READY ──────────────────┐
                │                   │ reserve
                │  reserve           │
                ▼                   │
             RESERVED               │
           ┌────┼────┐               │
       ack  │    │    │ visibility   │
           │    │    │ timeout / nack│
           ▼    │    ▼               │
       DELETED   │  READY (requeue)  │
                │  or DEAD_READY    │
                │  (max attempts)   │
                └───────────────────┘
```

A message in `RESERVED` is held by exactly one consumer. If the consumer doesn't acknowledge (`ack` / `nack`) before its visibility timeout, the server treats the message as if it had been nacked — returning it to `READY` (or to the dead-letter queue if its attempts hit `max_attempts`).

### Dead-letter routing

When a message has been nacked or expired enough times to hit its
`max_attempts`, the server moves it from `<queue>` to `<queue>.dead`. The
original payload, id, attempts count, and last error are preserved.
You consume from `<queue>.dead` exactly the same way as a live queue,
and nacking a `dead` message just requeues it to `dead` again — the server
never dead-letters a dead-letter (spec §8.5).

### Visibility timeout

Each `consume` request specifies a `visibility_timeout_ms` (default
`--default-visibility-ms`, typically 30 s). The clock starts when the
message is handed to a consumer. If the consumer hasn't acknowledged by
then, the message is reclaimed.

If the request specifies `wait_ms` (long-poll), the next reclaim
deadline is also used as an early wake-up — the long-poll sleep
interrupts as soon as a reservation is about to expire.

---

## 4. Long polling

`consume` accepts `wait_ms` (0–30000 ms, capped by `--max-wait-ms`).
The server responds as soon as either a message becomes available or the
deadline expires, whichever comes first.

- **`wait_ms=0`** — return immediately (204 if empty).
- **`wait_ms=2000`** — wait up to 2 s for a message; if a `publish` (or
  `nack` / reservation-expiry) wakes the waiter, return early.

Use long polling in production to avoid the "request → 204 → retry"
hot loop.

---

## 5. HTTP API at a glance

The full reference is in [`api.md`](./api.md). The five endpoints you need:

| Method & path | Purpose |
|---|---|
| `POST /queues/{queue}/messages` | publish a message (returns 201 + `Location` header) |
| `GET /queues/{queue}/messages?wait_ms=N&visibility_timeout_ms=N` | reserve a message (returns 200 or 204) |
| `POST /queues/{queue}/messages/{id}/ack` | acknowledge (returns 204) |
| `POST /queues/{queue}/messages/{id}/nack` | negative-acknowledge (returns 204) |
| `GET /queues/{queue}/stats` | `{ready, reserved, dead_lettered, total_active, oldest_ready_age_ms}` |

### Error format

Every 4xx/5xx response carries:

```json
{ "error": { "code": "invalid_request", "message": "..." } }
```

`code` is one of: `invalid_request`, `invalid_json`, `invalid_queue_name`, `unsupported_media_type`, `payload_too_large`, `message_not_found`, `reservation_conflict`, `database_busy`, `database_error`, `method_not_allowed`, `internal_error`, `server_shutting_down`.

### `curl` examples

```bash
# Publish
curl -X POST http://127.0.0.1:8080/queues/jobs/messages \
     -H 'Content-Type: application/json' \
     -d '{"payload": {"task": "render"}, "max_attempts": 3}'

# Reserve with long-poll
curl 'http://127.0.0.1:8080/queues/jobs/messages?wait_ms=2000&visibility_timeout_ms=30000'

# Ack
curl -X POST http://127.0.0.1:8080/queues/jobs/messages/<id>/ack \
     -H 'Content-Type: application/json' \
     -d '{"receipt": "<receipt>"}'
```

---

## 6. Backup & restore

PocketQueue's state is the single SQLite file at `--database PATH`. To
back up: stop the server (graceful), copy the file, restart. To
restore: same procedure, but with the old file restored.

WAL mode (`journal_mode=WAL`) is enabled automatically, so the `-wal` and
`-shm` sidecar files are part of the durable state too. Copy all three
together.

---

## 7. What PocketQueue is NOT

PocketQueue is deliberately small. It does **not** provide:

- Multiple consumers in a consumer group (one consumer per message, but
  no group semantics)
- Message priorities, ordering beyond per-queue FIFO
- Topic exchange, fanout, or routing keys
- Authentication or transport encryption (bind to `127.0.0.1`, put a
  reverse proxy in front if you need TLS or auth)
- Cross-region or cross-host replication

If you need any of those, look at RabbitMQ / NATS / Kafka.
