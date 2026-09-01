# Architecture

PocketQueue is a single binary (`pocketqueue-server`) built as a
deliberately small C17 codebase. It exposes the HTTP/1.1 + JSON API
over SQLite. This document describes the module layout, the request
lifecycle, and the key invariants.

## 1. Module layout

```
                ┌─────────────────────────────────────┐
   HTTP in  →   │  http_server.c  (hand-rolled 1.1)  │
                │   + http_routes.c  (dispatch)      │
                └──────────────┬──────────────────────┘
                               │
                               v
                ┌─────────────────────────────────────┐
                │  queue_service.c  (dispatch)        │
                │   ┌─────────────────────────────┐  │
                │   │ pq_notifier  (long-poll wakeup) │  │
                │   └─────────────────────────────┘  │
                └────────┬─────────────────────┬──────┘
                         │                     │
        ┌────────────────▼──────┐   ┌──────────▼──────────────┐
        │  queue_service_inmem.c │   │  queue_service_sqlite.c  │
        │  (in-memory backend)   │   │  (SQLite backend)        │
        └───────────────────────┘   └──────────┬──────────────┘
                                                │
                                  ┌─────────────▼──────────┐
                                  │  sqlite_repository.c    │
                                  │   + migrations.c        │
                                  │  (raw SQLite handle)    │
                                  └────────────────────────┘
```

Lower layers never depend on upper layers: `queue_service_*` knows about
the queue semantics but not about HTTP; `http_routes` knows about HTTP
but never touches the database; the SQLite layer knows about the schema
but not about queues or the notifier.

The exception: `http_routes` calls into `pq_http_routes_dispatch`, which
in turn dispatches to handlers. Handlers call into `pq_service_*`, the
dispatch layer. So the directionality is always:

```
http_routes → pq_service → [inmem | sqlite] → sqlite_repository
```

## 2. The `pq_service` interface (spec §43)

A single struct, opaque to callers, exposes one vtable per backend:

```c
struct pq_service {
    pq_service_config cfg;
    pq_clock *clock;
    pq_repository *repo;          /* NULL for in-memory */
    void *state;                  /* backend-specific state */
    pq_notifier notifier;         /* shared across backends */
    const pq_service_vtable *vtable;
};
```

The `pq_service_*` public functions (`publish`, `reserve`, `ack`,
`nack`, `stats`, plus the implicit `next_event_ms`) are thin dispatchers
that:

1. Validate inputs
2. Forward to the backend's vtable function
3. Run a recovery pass (in some backends) to make sure the database
   state is current
4. Map errors to `pq_status` codes

This design lets us add a new backend (e.g. an in-memory shim for
tests, an experimental Redis backend, etc.) without touching the HTTP
layer.

## 3. Request lifecycle

For a typical `POST /queues/{q}/messages` (publish):

```
client             http_server          pq_routes_dispatch     pq_service          SQLite repo
  │                   │                       │                    │                  │
  ├─ TCP SYN ───────>│                       │                    │                  │
  │                   │                       │                    │                  │
  ├─ HTTP req ──────>│                       │                    │                  │
  │                   ├── parse headers      │                    │                  │
  │                   ├── dispatch ─────────>│                    │                  │
  │                   │                       ├── publish() ──────>│                  │
  │                   │                       │                    ├── lock repo     │
  │                   │                       │                    ├── INSERT ───────>│
  │                   │                       │                    ├── commit       │
  │                   │                       │                    ├── unlock       │
  │                   │                       │                    ├── broadcast ────┤ (notifier)
  │                   │                       ├── 201 resp ────────>                  │
  │                   ├─ 201 Created <───────│                    │                  │
  │<─ 201 Created ────┤                       │                    │                  │
```

For `GET /queues/{q}/messages?wait_ms=N` (reserve, long-poll):

```
client                  http_server               dispatch loop
  │                        │                          │
  ├─ GET ... ─────────────>│                          │
  │                        ├── dispatch                │
  │                        ├── reserve (wait=0)        │
  │                        │   → 204 NOT_FOUND         │
  │                        ├── if wait_ms > 0:         │
  │                        │     compute next_event    │
  │                        │     pq_notifier_wait_until│
  │                        │       (condvar)           │
  │                        │   ← wakes on broadcast   │
  │                        │     loop back to reserve  │
  │                        │   → 200 with message      │
  │<─ 200 OK ──────────────┤                          │
```

The condvar is `pq_notifier` (mutex + cond + generation counter).
- **Publish** / **ack** / **nack** (state changes that may free up
  availability) → `pq_notifier_broadcast(&svc->notifier)`
- **Reserve** → on `PQ_NOT_FOUND`, compute the next reservation
  deadline (smallest `reserved_until_ms` in the queue, or the user's
  wait deadline), then `pq_notifier_wait_until(notifier, deadline)`
- **Spurious wakes** (condvar without broadcast) — re-loop and re-check

The notifier never holds the database mutex. Long-poll sleeps are pure
in-process events.

## 4. Storage: SQLite

We use the official SQLite amalgamation (single-file build, fetched by
CMake) compiled in. PRAGMAs at startup:

```sql
PRAGMA journal_mode = WAL;       -- concurrent readers + single writer
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;     -- wait up to 5s for the writer
PRAGMA synchronous = NORMAL;     -- safe with WAL, faster than FULL
```

`WAL` is critical: it lets the HTTP server read messages while a
`publish` or `ack` is mid-transaction, with no read locks.

### Schema

The schema lives in `migrations/V001__init.sql` and
`V002__messages.sql`. Each migration runs in a single transaction
(`BEGIN; … COMMIT;`). `migrations.c` records the current version in
`schema_version` and refuses to start if the database has a higher
version than the binary supports.

```
messages
├── id                  TEXT PRIMARY KEY  -- UUID v7
├── queue_name          TEXT NOT NULL
├── original_queue_name TEXT             -- set on dead-letter
├── payload_json        TEXT NOT NULL
├── state               INTEGER NOT NULL -- 0/1/2/3 = ready/reserved/dead_ready/dead_reserved
├── created_at_ms       INTEGER NOT NULL
├── available_at_ms     INTEGER NOT NULL -- when this becomes eligible again
├── updated_at_ms       INTEGER NOT NULL
├── attempts            INTEGER NOT NULL
├── max_attempts        INTEGER NOT NULL
├── receipt_token       TEXT             -- hex
├── reserved_until_ms   INTEGER
├── last_error          TEXT
└── dead_lettered_at_ms INTEGER
```

Indexes:
- `idx_messages_available (queue_name, state, available_at_ms, created_at_ms)`
  — speeds up the FIFO reservation query
- `idx_messages_reserved (state, reserved_until_ms)` — speeds up the
  expiration pass
- `idx_messages_original_queue (original_queue_name, state)` — speeds up
  dead-letter stats

### Atomic reservation

A single statement does the work of "find oldest eligible + mark
reserved":

```sql
UPDATE messages
SET
    state = ?1,
    receipt_token = ?2,
    reserved_until_ms = ?3,
    attempts = attempts + 1,
    updated_at_ms = ?4
WHERE id = (
    SELECT id FROM messages
    WHERE queue_name = ?5 AND state = ?6 AND available_at_ms <= ?7
    ORDER BY available_at_ms, created_at_ms, id
    LIMIT 1
)
RETURNING id, queue_name, payload_json, state, ...;
```

`UPDATE ... RETURNING` is atomic — no transaction is needed for
single-statement atomicity in SQLite. The repo mutex serializes all
statements, so no other thread can race.

## 5. Threading model

```
                ┌──────────────────┐
                │  accept thread    │  1 thread, blocks on accept()
                │  (http_server)    │
                └────────┬─────────┘
                         │  enqueues connection_job
                         v
            ┌────────────────────────────┐
            │  bounded queue (capacity 64)│
            └────────────────────────────┘
                         ▲
                         │  dequeued by
                ┌────────┴─────────┐
                │  worker thread    │  N threads (config: --worker-threads)
                │  (http_server)    │  reads request, calls handler, writes response
                └──────────────────┘
```

- **Accept thread** is single — fine for our scale; multi-accept
  is more complex than it's worth at <10k qps.
- **Worker thread pool** defaults to 8 — set with `--worker-threads`.
- **Repository mutex** serializes all SQLite access. Long-poll workers
  release the mutex before sleeping, so SQLite isn't held while parked.
- **`pq_notifier`** is a single shared structure with one mutex + one
  condvar. Waiters and broadcasters serialize on the mutex; broadcast is
  O(1); wait_until uses `pthread_cond_timedwait` with an absolute
  deadline.

### Graceful shutdown

On `SIGINT` / `SIGTERM`:

1. The accept thread closes the listener socket.
2. The accept thread broadcasts the notifier so parked long-poll
   waiters wake up and return `503 server_shutting_down`.
3. Workers finish their current request, send the response, exit.
4. The server waits up to one visibility window (capped at 30s) for
   workers to drain, then closes the SQLite connection and exits.

## 6. State machine (spec §9)

```
                 ┌──────────┐
PUBLISH ──────> │  READY   │ <─────────────────────┐
                 └────┬─────┘                       │
                      │ reserve                     │ nack (attempts < max_attempts)
                      v                             │
                 ┌──────────┐                       │
                 │ RESERVED │ ────────────> ────────┘
                 └────┬─────┘
                      │ ack
                      v
                  (deleted)

   Additional transitions (the spec calls these out explicitly):
   - visibility timeout on RESERVED  →  back to READY
   - nack (attempts >= max_attempts) →  DEAD_READY on `<queue>.dead`
   - reserve on DEAD_READY          →  DEAD_RESERVED
   - nack on DEAD_RESERVED          →  DEAD_READY (no recursion)
```

## 7. Recovery semantics (spec §28)

Expired reservations are reclaimed at the **first** opportunity of:

- Server startup
- Beginning of a reserve
- Beginning of a stats request
- Beginning of an ack / nack
- (Long-poll only) when the next-event deadline fires

The "expired" predicate: `state IN (RESERVED, DEAD_RESERVED) AND
reserved_until_ms <= now`. For each match:

| state | attempts vs max | action |
|---|---|---|
| RESERVED | `< max_attempts` | back to READY, available_at_ms = now |
| RESERVED | `>= max_attempts` | move to `<queue>.dead`, DEAD_READY |
| DEAD_RESERVED | (always) | DEAD_READY (no recursion) |

After recovery, the operation (reserve / stats / ack) proceeds.

## 8. Test surface

- **Unit tests** (CMocka, in `tests/unit/`): queue-name validation,
  config parsing, clock, queue service against the fake clock, random
  utilities, string utilities. ~1 s total.
- **E2E tests** (Python, in `tests/e2e/`):
  - `smoke.py` — server starts, /healthz + /readyz return 200
  - `test_api.py` — 13 scenarios from spec §37.1–§37.10 (publish-reserve-ack,
    nack, dead-letter, stale receipt, persistence across restart, reserved
    survives restart, visibility timeout, long-poll wakeup, long-poll
    timeout, long-poll expiry wakeup, etc.) ~6 s total
  - `stress.py` — 1,000 publishes, 8 concurrent consumers, every ID
    acked exactly once, queue empty. ~60 s
- **Example tests** (`examples/pq-spider/tests/e2e/spider_test.py`) —
  the breadth-first crawler end-to-end against a local test site. ~5 s

## 9. Build pipeline

```
CMakeLists.txt
├── FetchContent:           cJSON, SQLite (amalgamation)
├── Optional sub-project:    examples/pq-spider/  (default ON)
└── Tests via CTest:        unit + e2e + spider + (per build) sanitizers
```

Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, with `-Werror`
on implicit-function-declaration. The build treats warning-free as
buildable, but doesn't currently turn all warnings into errors. The CI
loop above catches them by re-running under ASan + UBSan.
