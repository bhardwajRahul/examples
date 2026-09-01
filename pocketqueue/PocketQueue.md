# PocketQueue Software Requirements and Technical Specification

**Version:** 1.0  
**Implementation language:** C  
**Primary target:** Linux x86-64  
**Secondary targets:** Windows x64 and macOS  
**License:** To be selected by the project owner

---

## 1. Purpose

PocketQueue is a lightweight, durable message queue server intended for local development, small internal systems, and coding-harness evaluation.

Clients publish JSON messages to named queues. Consumers reserve messages, process them, and then acknowledge or reject them. Messages are stored in SQLite so that they survive process termination and server restarts.

The project must be small enough to implement in several hours, while requiring multiple development stages involving:

- HTTP server design
- SQLite persistence
- Concurrent consumers
- Transactional state transitions
- Visibility timeouts
- Retries
- Dead-letter queues
- Long polling
- Command-line tooling
- Unit and end-to-end testing
- User and developer documentation

This document uses the terms **MUST**, **SHOULD**, and **MAY** as normative requirements.

---

# 2. Project Goals

PocketQueue MUST provide:

1. Named FIFO message queues.
2. JSON message publishing over HTTP.
3. Durable SQLite-backed storage.
4. Atomic message reservation.
5. Message acknowledgement and negative acknowledgement.
6. Visibility timeouts.
7. Automatic retry tracking.
8. Dead-letter queues.
9. Long-polling consumers.
10. Graceful shutdown.
11. A command-line client.
12. Unit tests.
13. End-to-end tests.
14. User documentation.
15. Developer documentation.

The implementation MUST favor correctness, testability, and maintainability over maximum performance.

---

# 3. Non-Goals

Version 1.0 does not need to provide:

- Distributed clustering
- Replication
- High availability
- Authentication or authorization
- TLS termination
- AMQP, MQTT, Kafka, or Redis protocol compatibility
- Message priorities
- Exactly-once processing guarantees
- Transactions spanning multiple queues
- Message payload streaming
- Message compression
- Browser-based administration
- Multi-node coordination
- Dynamic server-side plugins
- Guaranteed compatibility between different SQLite database files created by unofficial forks

PocketQueue provides **at-least-once delivery**. Consumers MUST therefore be prepared to receive the same message more than once.

---

# 4. Required Technology

## 4.1 Language

Production code MUST be written in C17.

The implementation SHOULD avoid compiler-specific extensions unless they are isolated behind portability abstractions.

## 4.2 Build system

The project MUST use CMake.

Minimum recommended CMake version:

```text
3.20
```

The following commands MUST build and test the project:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## 4.3 Required dependencies

The project MUST use SQLite 3 for durable storage.

The implementation MAY use an existing lightweight HTTP library, such as:

- libmicrohttpd
- CivetWeb
- Mongoose
- another actively maintained C HTTP library

The chosen HTTP library and version MUST be documented.

The implementation MAY use a JSON parsing library such as:

- cJSON
- yyjson
- Jansson

The selected JSON library MUST support validation of arbitrary JSON values.

The project MUST NOT implement its own general-purpose JSON parser unless JSON parsing is explicitly being used as part of the coding benchmark.

## 4.4 Test dependencies

Unit tests MUST be written in C.

The project MAY use:

- CMocka
- Unity
- Criterion
- another lightweight C unit-testing framework

End-to-end tests MAY be implemented in:

- Python 3 using only the standard library
- C using libcurl or an equivalent HTTP client
- portable shell scripts using `curl`

All tests MUST be executable through CTest.

---

# 5. Supported Platforms

The reference implementation MUST build and run on:

- Linux x86-64 using GCC or Clang

The code SHOULD build on:

- Windows 10 or Windows 11 using MSVC
- macOS using Apple Clang

Platform-specific functionality MUST be placed behind explicit portability interfaces.

Examples include:

- Signal handling
- Thread creation
- Condition variables
- Filesystem paths
- Process spawning in tests
- Monotonic clocks

---

# 6. Deliverables

The completed project MUST contain:

```text
pocketqueue/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── include/
│   └── pocketqueue/
├── src/
├── tests/
│   ├── unit/
│   └── e2e/
├── docs/
│   ├── user-guide.md
│   ├── api.md
│   ├── architecture.md
│   ├── development.md
│   └── testing.md
├── migrations/
└── examples/
```

The project MUST produce at least two executables:

```text
pocketqueue-server
pqctl
```

The project MAY also produce a reusable internal or public library:

```text
libpocketqueue
```

---

# 7. High-Level Architecture

The implementation SHOULD be divided into the following logical modules.

## 7.1 Server entry point

Responsibilities:

- Parse configuration
- Initialize logging
- Open the database
- Apply migrations
- Start the HTTP server
- Install shutdown handlers
- Coordinate graceful shutdown

Suggested files:

```text
src/server_main.c
src/config.c
src/config.h
```

## 7.2 HTTP layer

Responsibilities:

- Parse requests
- Validate paths and query parameters
- Validate content types
- Parse JSON request bodies
- Call the queue service
- Translate service results into HTTP responses
- Produce structured JSON errors

Suggested files:

```text
src/http_server.c
src/http_server.h
src/http_routes.c
src/http_routes.h
```

## 7.3 Queue service

Responsibilities:

- Enforce queue behavior
- Implement publishing
- Implement reservation
- Implement acknowledgements
- Implement negative acknowledgements
- Implement retry rules
- Move exhausted messages into dead-letter queues
- Implement long-poll wake-up behavior
- Avoid exposing SQLite details to the HTTP layer

Suggested files:

```text
src/queue_service.c
src/queue_service.h
```

## 7.4 Repository layer

Responsibilities:

- Open and configure SQLite
- Apply schema migrations
- Execute parameterized statements
- Implement transactions
- Convert database records into domain structures
- Recover expired reservations

Suggested files:

```text
src/sqlite_repository.c
src/sqlite_repository.h
src/migrations.c
src/migrations.h
```

## 7.5 Portability layer

Responsibilities:

- Wall-clock time
- Monotonic time
- UUID or secure random token generation
- Mutexes
- Condition variables
- Signal handling
- Sleep and wake operations

Suggested files:

```text
src/platform.c
src/platform.h
src/clock.c
src/clock.h
```

## 7.6 Command-line client

Responsibilities:

- Parse commands and options
- Send requests to the PocketQueue HTTP API
- Display results
- Return meaningful process exit codes

Suggested files:

```text
src/pqctl_main.c
src/http_client.c
src/http_client.h
```

---

# 8. Domain Concepts

## 8.1 Queue

A queue is identified by a case-sensitive name.

Valid queue names MUST match:

```regex
^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$
```

Examples of valid names:

```text
jobs
image-processing
email.outbound
queue_01
```

Examples of invalid names:

```text
/jobs
queue name
../jobs
jobs?
```

Queue names ending in `.dead` are reserved for dead-letter queues.

Clients MUST NOT be allowed to publish directly to queue names ending in `.dead`.

A queue does not need to be created explicitly. It comes into existence when its first message is published.

## 8.2 Message

A message contains:

- A server-generated identifier
- A queue name
- A JSON payload
- A creation timestamp
- A current availability timestamp
- A delivery-attempt count
- A maximum-attempt count
- A state
- Reservation metadata when reserved
- Dead-letter metadata when applicable

## 8.3 Reservation

Reserving a message temporarily assigns it to one consumer.

A reservation includes:

- Message ID
- Receipt token
- Reservation deadline
- Delivery-attempt number

The receipt token MUST be unpredictable.

An acknowledgement or negative acknowledgement MUST include the current receipt token.

This prevents a consumer holding an expired reservation from acknowledging a message that has since been assigned to another consumer.

## 8.4 Visibility timeout

The visibility timeout defines how long a reserved message remains hidden from other consumers.

If the consumer does not acknowledge or reject the message before the deadline, the reservation expires.

An expired message MUST either:

- Become available for retry, or
- Move into its dead-letter queue if its allowed delivery attempts have been exhausted

## 8.5 Dead-letter queue

Every normal queue has a corresponding virtual dead-letter queue:

```text
<queue-name>.dead
```

Examples:

```text
jobs.dead
image-processing.dead
```

A message is moved to the dead-letter queue when it cannot be successfully processed within its configured maximum number of attempts.

Dead-letter messages MUST retain:

- Original message ID
- Original payload
- Original queue name
- Original creation time
- Total attempt count
- Last failure reason, when available
- Time at which the message was dead-lettered

Dead-letter messages MAY be consumed using the normal reservation API.

A dead-letter message that is negatively acknowledged or expires MUST return to the dead-letter queue. It MUST NOT be recursively moved into another dead-letter queue.

---

# 9. Message States

The persistent implementation MUST support these logical states:

```text
READY
RESERVED
DEAD_READY
DEAD_RESERVED
```

Equivalent integer or textual representations MAY be used internally.

## 9.1 Normal message state transitions

```text
PUBLISH
   |
   v
 READY
   |
   | reserve
   v
RESERVED
   |
   +------ acknowledge ------> DELETED
   |
   +------ negative acknowledgement ------> READY or DEAD_READY
   |
   +------ visibility timeout ------------> READY or DEAD_READY
```

## 9.2 Dead-letter state transitions

```text
DEAD_READY
    |
    | reserve
    v
DEAD_RESERVED
    |
    +------ acknowledge ------> DELETED
    |
    +------ negative acknowledgement ------> DEAD_READY
    |
    +------ visibility timeout ------------> DEAD_READY
```

## 9.3 Delivery attempt semantics

A message's attempt counter MUST increment when the message is successfully reserved.

Publishing creates a message with:

```text
attempts = 0
```

The first successful reservation changes it to:

```text
attempts = 1
```

A message with:

```text
max_attempts = 3
```

may therefore be delivered up to three times.

If the third reservation is acknowledged, the message is successfully deleted.

If the third reservation expires or is negatively acknowledged, the message moves to the dead-letter queue.

---

# 10. SQLite Data Model

The exact schema MAY vary, but it MUST represent the following information.

A recommended schema is:

```sql
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);

CREATE TABLE messages (
    id                  TEXT PRIMARY KEY,
    queue_name          TEXT NOT NULL,
    original_queue_name TEXT,
    payload_json        TEXT NOT NULL,

    state               INTEGER NOT NULL,

    created_at_ms       INTEGER NOT NULL,
    available_at_ms     INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,

    attempts            INTEGER NOT NULL DEFAULT 0,
    max_attempts        INTEGER NOT NULL,

    receipt_token       TEXT,
    reserved_until_ms   INTEGER,

    last_error          TEXT,
    dead_lettered_at_ms INTEGER
);
```

Recommended indexes:

```sql
CREATE INDEX idx_messages_available
ON messages(queue_name, state, available_at_ms, created_at_ms);

CREATE INDEX idx_messages_reserved
ON messages(state, reserved_until_ms);

CREATE INDEX idx_messages_original_queue
ON messages(original_queue_name, state);
```

The database MUST store timestamps as signed 64-bit Unix epoch milliseconds.

The database MUST NOT store pointers, process-local identifiers, or monotonic clock values.

## 10.1 SQLite configuration

On startup, the server MUST configure SQLite appropriately.

Recommended settings:

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
PRAGMA synchronous = NORMAL;
```

The selected settings MUST be documented.

## 10.2 Transactions

All message state transitions MUST be transactional.

In particular, reservation MUST atomically:

1. Find the next eligible message.
2. Verify that it is still available.
3. Increment its attempt count.
4. Generate and store a receipt token.
5. Set its reservation deadline.
6. Change its state to reserved.
7. Commit the transaction.

Two concurrent consumers MUST NOT successfully reserve the same delivery attempt.

An implementation MAY use:

- `BEGIN IMMEDIATE` with a select-and-update transaction
- An atomic `UPDATE ... RETURNING` operation where supported
- Another SQLite-safe transactional strategy

All SQL values originating from clients MUST use bound parameters.

Client strings MUST NOT be inserted into SQL through string concatenation.

---

# 11. FIFO Semantics

Messages MUST be selected in this order:

1. Earliest `available_at_ms`
2. Earliest `created_at_ms`
3. Stable tie-breaker, such as message ID or SQLite row ID

PocketQueue provides FIFO ordering among messages that are currently eligible for delivery.

Strict global ordering is not guaranteed when:

- A message is negatively acknowledged
- A visibility timeout expires
- Different messages have different availability times
- Multiple consumers reserve concurrently
- A message is returned for retry

---

# 12. Time Handling

Persistent timestamps MUST use Unix epoch milliseconds.

In-process waiting SHOULD use a monotonic clock.

The implementation SHOULD expose time through an injectable clock interface so tests can advance time without sleeping.

Suggested interface:

```c
typedef struct pq_clock {
    int64_t (*wall_time_ms)(void *context);
    int64_t (*monotonic_time_ms)(void *context);
    void *context;
} pq_clock;
```

Tests MUST NOT rely on long real-time sleeps to test visibility expiration.

Short synchronization waits in end-to-end tests are acceptable, but deterministic polling with an overall deadline is preferred.

---

# 13. HTTP API

The default server base URL is:

```text
http://127.0.0.1:8080
```

All JSON requests MUST use:

```text
Content-Type: application/json
```

All JSON responses MUST use:

```text
Content-Type: application/json
```

Timestamps returned by the API MUST use UTC ISO 8601 format, for example:

```text
2026-07-13T12:30:45.123Z
```

Durations in request parameters SHOULD use milliseconds unless otherwise stated.

---

# 14. Publish Message

## 14.1 Request

```http
POST /queues/{queue}/messages
```

Request body:

```json
{
  "payload": {
    "task": "resize-image",
    "filename": "photo.jpg"
  },
  "max_attempts": 3
}
```

`payload`:

- MUST be present
- MAY be any valid JSON value
- MAY be an object, array, string, number, boolean, or null

`max_attempts`:

- MAY be omitted
- MUST be an integer
- MUST be between 1 and 100
- Defaults to the server's configured value

Unknown top-level properties SHOULD be rejected.

## 14.2 Success response

Status:

```text
201 Created
```

Example:

```json
{
  "id": "0197f39f-9d12-7a40-9c58-0db7cab7ed77",
  "queue": "images",
  "payload": {
    "task": "resize-image",
    "filename": "photo.jpg"
  },
  "created_at": "2026-07-13T12:30:45.123Z",
  "available_at": "2026-07-13T12:30:45.123Z",
  "attempts": 0,
  "max_attempts": 3
}
```

The `Location` header SHOULD contain:

```text
/queues/images/messages/{message-id}
```

## 14.3 Error responses

Possible errors include:

- `400 Bad Request`: invalid JSON or invalid field
- `404 Not Found`: invalid route
- `413 Content Too Large`: payload exceeds configured limit
- `415 Unsupported Media Type`: incorrect content type
- `500 Internal Server Error`: unexpected failure
- `503 Service Unavailable`: database temporarily unavailable

---

# 15. Reserve Message

## 15.1 Request

```http
GET /queues/{queue}/messages
```

Supported query parameters:

```text
wait_ms
visibility_timeout_ms
```

Example:

```http
GET /queues/images/messages?wait_ms=20000&visibility_timeout_ms=30000
```

`wait_ms`:

- Defaults to `0`
- Minimum is `0`
- Maximum is configured by the server
- Causes the request to wait until a message is available or the wait expires

`visibility_timeout_ms`:

- Defaults to the server default
- MUST be within configured minimum and maximum values

## 15.2 Success response

Status:

```text
200 OK
```

Example:

```json
{
  "id": "0197f39f-9d12-7a40-9c58-0db7cab7ed77",
  "queue": "images",
  "payload": {
    "task": "resize-image",
    "filename": "photo.jpg"
  },
  "created_at": "2026-07-13T12:30:45.123Z",
  "attempts": 1,
  "max_attempts": 3,
  "receipt": "ee7b41d5cc0b457e925b5ad67c118cb8",
  "reserved_until": "2026-07-13T12:31:15.123Z"
}
```

## 15.3 No message response

If no message is available before the wait expires:

```text
204 No Content
```

The response MUST have no JSON body.

## 15.4 Long-poll behavior

The implementation MUST NOT continuously query SQLite in a tight polling loop.

Long-poll requests SHOULD wait using a condition variable or equivalent event mechanism.

Waiting consumers MUST be woken when:

- A new message is published
- A message is negatively acknowledged
- A message's availability time is reached
- A reservation expires
- The server begins shutting down

A long-poll request MUST NOT hold an active SQLite transaction while waiting.

A long-poll request MUST NOT hold a global database mutex while waiting.

Spurious wake-ups MUST be handled by rechecking the database.

---

# 16. Acknowledge Message

## 16.1 Request

```http
POST /queues/{queue}/messages/{id}/ack
```

Request body:

```json
{
  "receipt": "ee7b41d5cc0b457e925b5ad67c118cb8"
}
```

## 16.2 Success response

```text
204 No Content
```

The acknowledged message MUST be deleted transactionally.

## 16.3 Validation rules

The acknowledgement MUST fail if:

- The message does not exist
- The message is not currently reserved
- The receipt does not match
- The reservation has expired
- The queue in the URL does not match the message's current queue

If the reservation has expired, the server MUST first apply the normal expiration behavior before returning the failure.

## 16.4 Error responses

- `400 Bad Request`: missing or invalid receipt
- `404 Not Found`: message does not exist
- `409 Conflict`: message is not reserved, receipt does not match, or reservation expired

Acknowledgements are not required to be idempotent. Repeating an acknowledgement after successful deletion MAY return `404 Not Found`.

---

# 17. Negative Acknowledgement

## 17.1 Request

```http
POST /queues/{queue}/messages/{id}/nack
```

Request body:

```json
{
  "receipt": "ee7b41d5cc0b457e925b5ad67c118cb8",
  "reason": "image decoder failed"
}
```

`reason`:

- MAY be omitted
- MUST be a string if present
- MUST be limited to a documented maximum length
- Recommended maximum: 1024 UTF-8 bytes

## 17.2 Success response

```text
204 No Content
```

## 17.3 Behavior

For a normal message:

- If `attempts < max_attempts`, return the message to `READY`
- If `attempts >= max_attempts`, move the message to the corresponding dead-letter queue

For a dead-letter message:

- Return it to `DEAD_READY`
- Do not increment or reset its normal attempt count
- Do not create another dead-letter queue

## 17.4 Errors

The same reservation and receipt validation rules used by acknowledgement MUST apply.

---

# 18. Queue Statistics

## 18.1 Request

```http
GET /queues/{queue}/stats
```

## 18.2 Response

```text
200 OK
```

Example:

```json
{
  "queue": "images",
  "ready": 12,
  "reserved": 3,
  "dead_lettered": 2,
  "total_active": 15,
  "oldest_ready_age_ms": 4821
}
```

Before calculating statistics, the server MUST process expired reservations relevant to the queue.

For a normal queue:

- `ready` counts immediately available messages
- `reserved` counts messages with active reservations
- `dead_lettered` counts messages in the corresponding dead-letter queue
- `total_active` equals `ready + reserved`

For a `.dead` queue:

- `ready` counts available dead-letter messages
- `reserved` counts reserved dead-letter messages
- `dead_lettered` SHOULD be omitted or set to zero

Empty queues MUST return zero-valued statistics rather than `404 Not Found`.

---

# 19. Health Endpoints

## 19.1 Liveness

```http
GET /healthz
```

Response:

```text
200 OK
```

```json
{
  "status": "ok"
}
```

This endpoint only indicates that the server process and HTTP listener are functioning.

## 19.2 Readiness

```http
GET /readyz
```

The server MUST perform a lightweight database operation.

Success:

```text
200 OK
```

```json
{
  "status": "ready"
}
```

Failure:

```text
503 Service Unavailable
```

```json
{
  "status": "not_ready"
}
```

---

# 20. Error Response Format

All API errors with response bodies MUST use this structure:

```json
{
  "error": {
    "code": "invalid_request",
    "message": "max_attempts must be between 1 and 100"
  }
}
```

An optional `details` object MAY be included:

```json
{
  "error": {
    "code": "invalid_request",
    "message": "The request contains invalid fields",
    "details": {
      "field": "max_attempts"
    }
  }
}
```

Recommended error codes:

```text
invalid_request
invalid_json
invalid_queue_name
unsupported_media_type
payload_too_large
message_not_found
reservation_conflict
database_busy
database_error
method_not_allowed
internal_error
server_shutting_down
```

Internal database paths, SQL statements, memory addresses, and stack traces MUST NOT be returned to clients.

---

# 21. HTTP Status Codes

The server MUST use status codes consistently.

| Status | Meaning |
|---|---|
| 200 | Successful request with a response body |
| 201 | Message successfully published |
| 204 | Successful request without a body, or long-poll timeout |
| 400 | Invalid path parameter, query parameter, or request body |
| 404 | Route or message not found |
| 405 | HTTP method not supported for the route |
| 409 | Invalid message state or stale receipt |
| 413 | Request body exceeds the configured limit |
| 415 | Unsupported content type |
| 500 | Unexpected internal error |
| 503 | Database unavailable or server shutting down |

A `405` response SHOULD include an `Allow` header.

---

# 22. Payload Limits

The default maximum HTTP request body size SHOULD be:

```text
1 MiB
```

The value MUST be configurable.

The limit MUST be checked before allocating an unbounded buffer.

The server MUST reject oversized requests with:

```text
413 Content Too Large
```

The server SHOULD also impose reasonable limits on:

- Queue-name length
- URL length
- Header size
- Failure-reason length
- Number of concurrent requests

---

# 23. Identifier and Receipt Generation

Message IDs MUST be globally unique enough for practical use.

Acceptable formats include:

- UUID version 4
- UUID version 7
- 128-bit random hexadecimal identifiers

Receipt tokens MUST be generated using a cryptographically secure operating-system random source where available.

Receipt tokens MUST NOT be generated using:

```c
rand()
```

or another predictable pseudo-random generator.

If secure randomness is unavailable, the server MUST fail startup or fail the reservation operation rather than generate predictable receipt tokens.

---

# 24. Configuration

The server MUST support command-line configuration.

Recommended command:

```bash
pocketqueue-server [options]
```

Required options:

```text
--bind ADDRESS
--port PORT
--database PATH
--default-visibility-ms N
--min-visibility-ms N
--max-visibility-ms N
--default-max-attempts N
--max-wait-ms N
--max-body-bytes N
--worker-threads N
--log-level LEVEL
--log-format FORMAT
```

Recommended defaults:

```text
bind address:             127.0.0.1
port:                     8080
database:                 pocketqueue.db
default visibility:       30000 ms
minimum visibility:       1000 ms
maximum visibility:       600000 ms
default max attempts:     3
maximum long-poll wait:   30000 ms
maximum request body:     1048576 bytes
worker threads:           8
log level:                info
log format:               text
```

The server SHOULD support equivalent environment variables:

```text
PQ_BIND
PQ_PORT
PQ_DATABASE
PQ_DEFAULT_VISIBILITY_MS
PQ_MIN_VISIBILITY_MS
PQ_MAX_VISIBILITY_MS
PQ_DEFAULT_MAX_ATTEMPTS
PQ_MAX_WAIT_MS
PQ_MAX_BODY_BYTES
PQ_WORKER_THREADS
PQ_LOG_LEVEL
PQ_LOG_FORMAT
```

Configuration precedence SHOULD be:

1. Command-line options
2. Environment variables
3. Built-in defaults

Invalid configuration MUST cause startup to fail with a nonzero exit code and an actionable error message.

---

# 25. Logging

The server MUST support these log levels:

```text
error
warn
info
debug
```

The server MUST support:

```text
text
json
```

log formats.

Each request SHOULD be logged with:

- Timestamp
- Method
- Path
- Status code
- Duration
- Request identifier
- Remote address where available

Logs MUST NOT include message payloads by default.

Receipt tokens MUST NOT be logged at `info` level.

Database errors SHOULD include enough information for diagnosis without exposing client payloads or secrets.

---

# 26. Graceful Shutdown

The server MUST respond to normal operating-system termination signals.

On POSIX platforms, this includes:

```text
SIGINT
SIGTERM
```

During shutdown, the server MUST:

1. Stop accepting new connections.
2. Wake all waiting long-poll requests.
3. Cause new requests to receive `503 Service Unavailable` where practical.
4. Allow active short-running requests to finish within a bounded grace period.
5. Roll back incomplete database transactions.
6. Close HTTP resources.
7. Close SQLite connections.
8. Flush logs.
9. Exit with an appropriate process code.

Reserved messages MUST remain reserved in SQLite when the server stops.

After restart, expired reservations MUST be recovered according to their stored deadlines.

The server MUST NOT automatically return all reserved messages to the ready state merely because it restarted.

---

# 27. Startup and Recovery

At startup, the server MUST:

1. Parse and validate configuration.
2. Open or create the SQLite database.
3. Apply required migrations.
4. Verify the schema version.
5. Configure SQLite pragmas.
6. Recover any reservations that have already expired.
7. Start the HTTP listener.
8. Report that it is ready.

If the database schema is newer than the server supports, startup MUST fail without modifying the database.

Database migrations MUST execute transactionally.

---

# 28. Expired Reservation Recovery

Expired reservations MUST be processed:

- At server startup
- Before attempting to reserve a message
- Before returning queue statistics
- When a long-poll wait reaches a known reservation deadline

A background maintenance mechanism MAY also be used.

The implementation MUST NOT depend solely on a periodic background scan for correctness.

For every expired normal reservation:

```text
if attempts >= max_attempts:
    move to dead-letter queue
else:
    return to READY
```

For every expired dead-letter reservation:

```text
return to DEAD_READY
```

Recovery MUST clear:

- Receipt token
- Reservation deadline

Recovery MUST update the message modification timestamp.

---

# 29. Long-Poll Coordination

The implementation SHOULD maintain a process-local notification generation counter.

A possible design is:

```c
typedef struct pq_notifier {
    pq_mutex mutex;
    pq_condition condition;
    uint64_t generation;
    bool shutting_down;
} pq_notifier;
```

Operations that may make a message available SHOULD increment `generation` and signal or broadcast the condition.

A waiting request SHOULD:

1. Query SQLite for an available message.
2. If found, reserve and return it.
3. Calculate its remaining long-poll deadline.
4. Determine the next relevant availability or reservation-expiry time.
5. Record the notification generation.
6. Wait on the condition variable with a timeout.
7. Wake when signaled, timed out, or shutting down.
8. Recheck SQLite.

Condition variables are allowed to wake spuriously.

The database MUST remain the source of truth.

The notifier MUST NOT be treated as a durable queue.

---

# 30. Command-Line Client

The `pqctl` executable MUST support a configurable server URL.

Default:

```text
http://127.0.0.1:8080
```

The URL MAY be supplied through:

```text
--server URL
```

or:

```text
PQ_SERVER_URL
```

## 30.1 Publish

```bash
pqctl publish QUEUE JSON
```

Example:

```bash
pqctl publish images '{"filename":"photo.jpg"}'
```

Options:

```text
--max-attempts N
```

The command MUST print the created message ID on success.

## 30.2 Consume

```bash
pqctl consume QUEUE
```

Options:

```text
--wait-ms N
--visibility-ms N
--raw
```

Default output SHOULD include:

- Message ID
- Receipt
- Attempts
- Payload

With `--raw`, the client SHOULD print the API response body without formatting.

## 30.3 Acknowledge

```bash
pqctl ack QUEUE MESSAGE_ID RECEIPT
```

## 30.4 Negative acknowledgement

```bash
pqctl nack QUEUE MESSAGE_ID RECEIPT
```

Options:

```text
--reason TEXT
```

## 30.5 Statistics

```bash
pqctl stats QUEUE
```

## 30.6 Health

```bash
pqctl health
```

## 30.7 Exit codes

Recommended exit codes:

| Code | Meaning |
|---:|---|
| 0 | Success |
| 1 | General error |
| 2 | Command-line usage error |
| 3 | Network error |
| 4 | Server returned a client error |
| 5 | Server returned an internal error |

---

# 31. Memory Management Requirements

All ownership rules MUST be clear in public and internal interfaces.

Every allocation MUST have a defined owner.

Functions returning allocated memory SHOULD follow a consistent naming or documentation convention.

The server MUST:

- Check allocation failures
- Avoid unbounded allocation based on client-supplied lengths
- Release request resources on every error path
- Finalize SQLite statements
- Roll back or close active transactions on failure
- Avoid use-after-free during shutdown
- Avoid retaining HTTP request pointers after a request ends

The test suite SHOULD be runnable under AddressSanitizer and UndefinedBehaviorSanitizer.

Recommended CMake options:

```text
PQ_ENABLE_ASAN
PQ_ENABLE_UBSAN
```

---

# 32. Thread Safety

The HTTP server MAY process multiple requests concurrently.

Shared components MUST document their thread-safety rules.

The implementation MUST ensure that:

- Two consumers do not receive the same reservation
- Receipt generation is thread-safe
- Shutdown state is synchronized
- Condition-variable notification is synchronized
- SQLite connections are used according to SQLite threading rules
- Statements are not shared concurrently unless explicitly safe
- Logging is thread-safe

The implementation MAY use:

- One SQLite connection protected by a mutex
- One connection per worker thread
- A small connection pool

Long-poll waiting MUST occur outside any database lock.

A global database mutex is acceptable for version 1.0 if it does not remain locked while requests wait or perform unrelated network operations.

---

# 33. Database Busy Handling

The server MUST configure a SQLite busy timeout.

Transient `SQLITE_BUSY` and `SQLITE_LOCKED` errors SHOULD be retried by SQLite through its configured busy handler.

If the operation cannot complete within the configured timeout, the server SHOULD return:

```text
503 Service Unavailable
```

with:

```json
{
  "error": {
    "code": "database_busy",
    "message": "The queue database is temporarily busy"
  }
}
```

The server MUST NOT spin indefinitely.

---

# 34. Security Requirements

Although PocketQueue 1.0 does not implement authentication, it MUST follow basic secure coding practices.

The implementation MUST:

- Bind to `127.0.0.1` by default
- Validate all queue names
- Validate all integer ranges
- Limit request sizes
- Use prepared SQL statements
- Use secure random receipt tokens
- Avoid format-string vulnerabilities
- Avoid shell execution for request processing
- Avoid path construction from queue names
- Avoid returning internal details to clients
- Compile cleanly with common warning flags

Recommended GCC and Clang flags:

```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
```

The continuous-integration build SHOULD treat warnings as errors.

---

# 35. Unit-Test Requirements

Unit tests MUST be written in C and run through CTest.

Tests SHOULD use temporary databases or in-memory SQLite databases where appropriate.

The queue service SHOULD be testable without starting an HTTP listener.

A fake clock SHOULD be used for time-dependent behavior.

## 35.1 Required unit-test groups

### Queue-name validation

Test:

- Minimum valid name
- Maximum valid length
- Invalid leading character
- Spaces
- Slashes
- Empty name
- Names longer than 64 characters
- Reserved `.dead` suffix
- Case sensitivity

### Configuration parsing

Test:

- Defaults
- Valid command-line overrides
- Environment-variable overrides
- Command-line precedence
- Invalid ports
- Invalid duration ranges
- Invalid maximum-attempt values
- Missing option values
- Unknown options

### Publishing

Test:

- Publish object payload
- Publish array payload
- Publish scalar payload
- Publish null payload
- Default `max_attempts`
- Custom `max_attempts`
- Invalid JSON
- Missing payload
- Oversized payload
- Invalid maximum attempts

### FIFO reservation

Test:

- One message
- Multiple messages
- Stable FIFO ordering
- No message available
- Reservation increments attempts
- Reservation creates a receipt
- Reservation sets the correct deadline

### Acknowledgement

Test:

- Successful acknowledgement
- Unknown message
- Incorrect receipt
- Missing receipt
- Acknowledgement after expiration
- Acknowledgement of an unreserved message
- Acknowledgement from the wrong queue
- Acknowledgement of a dead-letter message

### Negative acknowledgement

Test:

- Successful requeue
- Failure reason storage
- Incorrect receipt
- Negative acknowledgement after expiration
- Requeue before maximum attempts
- Dead-lettering at maximum attempts
- Negative acknowledgement of dead-letter message

### Visibility timeout

Test:

- Message unavailable before deadline
- Message available after deadline
- Attempt count preserved
- Receipt cleared after expiration
- Expired final attempt moves to dead-letter queue
- Expired dead-letter reservation returns to dead-letter queue

### Dead-letter queue

Test:

- Correct dead-letter queue name
- Payload preserved
- Message ID preserved
- Original queue preserved
- Attempt count preserved
- Failure reason preserved
- Dead-letter timestamp recorded
- Direct publishing to `.dead` rejected

### Statistics

Test:

- Empty queue
- Ready count
- Reserved count
- Dead-letter count
- Expired reservations recovered before counting
- Dead-letter queue statistics

### Database migration

Test:

- New empty database
- Existing current schema
- Upgrade from every supported old schema
- Unsupported newer schema
- Failed migration rollback

### Error paths

Test selected failures such as:

- Allocation failure where practical
- SQLite open failure
- SQLite transaction failure
- Secure-random generation failure
- Invalid persistent record

---

# 36. Concurrency Tests

Concurrency tests MAY be unit, integration, or end-to-end tests.

The test suite MUST verify:

1. Multiple consumers can reserve concurrently.
2. No delivery attempt is assigned to more than one consumer.
3. All published messages can eventually be consumed.
4. Acknowledgements do not interfere with other messages.
5. Long-poll waiters wake when messages are published.
6. Shutdown wakes all waiting consumers.
7. Concurrent publishing and consuming does not corrupt the database.

A required stress test MUST:

1. Publish at least 1,000 messages.
2. Start at least 8 consumer threads or processes.
3. Reserve and acknowledge all messages.
4. Record every message ID.
5. Assert that every published ID was acknowledged exactly once by the test consumers.
6. Assert that the queue is empty at the end.

The server may technically redeliver messages under failure conditions, but the controlled stress test without simulated failures SHOULD not observe duplicate delivery.

---

# 37. End-to-End Test Requirements

End-to-end tests MUST start the real server executable using:

- A temporary database
- A temporary directory
- An unused local TCP port

Tests MUST terminate the server at completion, including after failure.

The end-to-end harness MUST enforce deadlines so a broken server cannot hang the test suite indefinitely.

## 37.1 Required end-to-end scenarios

### Scenario 1: Server health

1. Start the server.
2. Wait for readiness.
3. Request `/healthz`.
4. Request `/readyz`.
5. Verify successful responses.

### Scenario 2: Publish, reserve, acknowledge

1. Publish a message.
2. Reserve it.
3. Validate its payload.
4. Acknowledge it.
5. Verify that the queue is empty.

### Scenario 3: Negative acknowledgement

1. Publish a message.
2. Reserve it.
3. Negative-acknowledge it.
4. Reserve it again.
5. Verify that the attempt count increased.
6. Acknowledge it.

### Scenario 4: Visibility timeout

1. Publish a message.
2. Reserve it with a short visibility timeout.
3. Do not acknowledge it.
4. Verify that it cannot immediately be reserved again.
5. Wait until the timeout expires.
6. Reserve it again.
7. Verify that it has a new receipt.

### Scenario 5: Stale receipt

1. Reserve a message.
2. Allow the reservation to expire.
3. Reserve it again.
4. Attempt acknowledgement using the old receipt.
5. Verify `409 Conflict`.
6. Acknowledge using the current receipt.

### Scenario 6: Dead-letter queue

1. Publish with `max_attempts` set to 2.
2. Reserve and reject it.
3. Reserve and reject it again.
4. Verify that the normal queue is empty.
5. Reserve from `<queue>.dead`.
6. Validate the original payload and metadata.
7. Acknowledge the dead-letter message.

### Scenario 7: Persistence across restart

1. Start the server.
2. Publish several messages.
3. Stop the server.
4. Restart using the same database.
5. Reserve and acknowledge the messages.
6. Verify original ordering.

### Scenario 8: Reserved message across restart

1. Publish a message.
2. Reserve it with a visibility timeout.
3. Stop the server before expiration.
4. Restart using the same database.
5. Verify that the message remains unavailable before the original deadline.
6. Verify that it becomes available after the deadline.

### Scenario 9: Long-poll wake-up

1. Start a long-poll request on an empty queue.
2. Publish a message from another connection.
3. Verify that the waiting request returns promptly with the message.

### Scenario 10: Long-poll timeout

1. Request a message from an empty queue with a short wait.
2. Publish nothing.
3. Verify `204 No Content`.
4. Verify the elapsed time is within a reasonable tolerance.

### Scenario 11: Concurrent consumers

1. Publish at least 1,000 messages.
2. Run multiple consumers.
3. Acknowledge every received message.
4. Verify no missing IDs.
5. Verify no unexpected duplicate IDs.
6. Verify zero remaining messages.

### Scenario 12: Graceful shutdown

1. Start several long-poll requests.
2. Send the normal termination signal.
3. Verify that the process exits within the configured grace period.
4. Verify that waiting requests do not remain hung.
5. Restart and verify database integrity.

### Scenario 13: Invalid requests

Verify behavior for:

- Invalid JSON
- Missing payload
- Invalid queue name
- Oversized request
- Invalid query parameter
- Unsupported content type
- Wrong HTTP method
- Incorrect receipt
- Unknown message ID

---

# 38. Test Isolation

Each test MUST:

- Use a unique temporary database or reset state explicitly
- Use unique queue names where appropriate
- Avoid depending on execution order
- Avoid depending on another test's server process
- Clean up temporary files
- Use bounded timeouts

Parallel tests MUST not share the same database unless sharing is the behavior under test.

---

# 39. Sanitizer and Analysis Builds

The project SHOULD provide documented commands for:

## AddressSanitizer

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPQ_ENABLE_ASAN=ON

cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## UndefinedBehaviorSanitizer

```bash
cmake -S . -B build-ubsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPQ_ENABLE_UBSAN=ON

cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure
```

The project SHOULD also document use of:

- Valgrind on Linux
- Static analysis through Clang-Tidy
- Compiler warnings
- SQLite integrity checks

---

# 40. User Documentation

## 40.1 README.md

The README MUST include:

- Project summary
- Feature list
- Supported platforms
- Dependencies
- Build instructions
- Test instructions
- Quick-start example
- Links to detailed documentation
- License information

The quick start MUST show:

1. Building the project
2. Starting the server
3. Publishing a message
4. Consuming a message
5. Acknowledging it
6. Viewing statistics

## 40.2 User guide

`docs/user-guide.md` MUST include:

- Installation
- Server startup
- Configuration options
- Environment variables
- Queue-name rules
- Publishing messages
- Consuming messages
- Acknowledging messages
- Rejecting messages
- Visibility timeout explanation
- Retry behavior
- Dead-letter queue behavior
- Persistence and database location
- Graceful shutdown
- Backup recommendations
- Common errors
- Troubleshooting

The guide MUST clearly explain that PocketQueue provides at-least-once delivery.

## 40.3 API documentation

`docs/api.md` MUST document:

- Every endpoint
- Every query parameter
- Every request body
- Every response body
- Status codes
- Error format
- Queue-name restrictions
- Payload-size limits
- Example `curl` commands

## 40.4 Command-line client documentation

The user documentation MUST provide examples for every `pqctl` command.

---

# 41. Developer Documentation

## 41.1 Architecture document

`docs/architecture.md` MUST describe:

- Major modules
- Dependency direction
- Request lifecycle
- Publish lifecycle
- Reservation lifecycle
- Acknowledgement lifecycle
- Retry lifecycle
- Dead-letter lifecycle
- Long-poll notification strategy
- Database schema
- Transaction boundaries
- Thread-safety model
- Shutdown sequence

It SHOULD include at least one state diagram.

## 41.2 Development guide

`docs/development.md` MUST include:

- Required build tools
- Dependency setup
- Debug build instructions
- Release build instructions
- Coding conventions
- Error-handling conventions
- Memory ownership conventions
- Adding a database migration
- Adding an API endpoint
- Platform abstraction rules
- Logging conventions

## 41.3 Testing guide

`docs/testing.md` MUST include:

- Running all tests
- Running unit tests only
- Running end-to-end tests only
- Running a single test
- Running sanitizer builds
- Test directory structure
- Fake-clock usage
- Temporary-database strategy
- End-to-end server lifecycle
- How to add a regression test

## 41.4 Source-level documentation

Public functions and structures MUST be documented in headers.

Comments SHOULD explain:

- Ownership
- Thread safety
- Valid input ranges
- Error returns
- Lifetime requirements

Comments SHOULD explain why a non-obvious decision exists rather than merely restating the code.

---

# 42. Coding Standards

The implementation MUST follow a consistent style.

The project SHOULD provide a `.clang-format` file.

Recommended conventions:

- `snake_case` for functions and variables
- `PQ_` prefix for public macros
- `pq_` prefix for public functions and types
- Explicit fixed-width integer types for stored values
- One responsibility per function where practical
- No hidden mutable global state
- Structured cleanup paths
- Consistent result or error types

A function that can fail SHOULD return an explicit status.

Example:

```c
typedef enum pq_status {
    PQ_OK = 0,
    PQ_INVALID_ARGUMENT,
    PQ_NOT_FOUND,
    PQ_CONFLICT,
    PQ_DATABASE_BUSY,
    PQ_DATABASE_ERROR,
    PQ_OUT_OF_MEMORY,
    PQ_INTERNAL_ERROR
} pq_status;
```

Error messages SHOULD be returned through a bounded error object rather than global buffers.

---

# 43. Recommended Public Service Interface

The internal queue service MAY expose an interface similar to:

```c
typedef struct pq_service pq_service;

typedef struct pq_publish_request {
    const char *queue_name;
    const char *payload_json;
    int max_attempts;
} pq_publish_request;

typedef struct pq_reserve_request {
    const char *queue_name;
    int64_t visibility_timeout_ms;
} pq_reserve_request;

pq_status pq_service_publish(
    pq_service *service,
    const pq_publish_request *request,
    pq_message *out_message,
    pq_error *error);

pq_status pq_service_reserve(
    pq_service *service,
    const pq_reserve_request *request,
    pq_message *out_message,
    pq_error *error);

pq_status pq_service_ack(
    pq_service *service,
    const char *queue_name,
    const char *message_id,
    const char *receipt,
    pq_error *error);

pq_status pq_service_nack(
    pq_service *service,
    const char *queue_name,
    const char *message_id,
    const char *receipt,
    const char *reason,
    pq_error *error);
```

The exact API MAY differ, but the HTTP layer MUST not contain raw SQL or duplicate queue-state logic.

---

# 44. Performance Expectations

PocketQueue is not intended to compete with high-throughput distributed brokers.

On a normal development machine, the implementation SHOULD be able to:

- Store at least 100,000 small messages in one database
- Process the required 1,000-message concurrency test
- Support at least 8 simultaneous consumers
- Handle at least 100 simultaneous long-poll requests without busy looping
- Complete ordinary API requests without memory growth over time

There is no strict messages-per-second target for version 1.0.

Correctness takes priority over throughput.

---

# 45. Required Behavioral Guarantees

The finished implementation MUST satisfy all of the following:

1. Published messages survive a normal server restart.
2. A successful acknowledgement permanently removes a message.
3. A negative acknowledgement makes a message available again unless attempts are exhausted.
4. An expired reservation makes a message available again unless attempts are exhausted.
5. Exhausted messages move to the corresponding dead-letter queue.
6. No two successful reservations return the same receipt for the same delivery attempt.
7. A stale receipt cannot acknowledge a newer reservation.
8. Long polling does not hold a database transaction while waiting.
9. Long polling wakes when a message becomes available.
10. Shutdown does not corrupt the SQLite database.
11. Queue names and request sizes are validated.
12. All state transitions are protected by SQLite transactions.
13. Unit tests cover queue-service behavior.
14. End-to-end tests exercise the actual server executable.
15. User and developer documentation describe the implemented behavior.

---

# 46. Recommended Implementation Stages

The work SHOULD be divided into independently testable stages.

## Stage 1: Project foundation

Deliver:

- CMake build
- Server executable
- Unit-test executable
- Logging
- Configuration parsing
- SQLite opening
- Health endpoints

Acceptance:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

must succeed.

## Stage 2: In-memory queue behavior

Deliver:

- Message structures
- Queue-name validation
- Publish
- Reserve
- Acknowledge
- Negative acknowledgement
- Unit tests
- Fake clock

Persistence is not required at this stage.

## Stage 3: HTTP API

Deliver:

- Publish endpoint
- Reserve endpoint
- Ack endpoint
- Nack endpoint
- Statistics endpoint
- Error responses
- Basic end-to-end tests

## Stage 4: SQLite persistence

Deliver:

- Schema
- Migrations
- Repository layer
- Transactional reservation
- Restart persistence tests
- SQLite configuration documentation

The in-memory state MUST no longer be authoritative.

## Stage 5: Visibility timeout and retries

Deliver:

- Reservation deadlines
- Expiration recovery
- Retry counting
- Stale-receipt protection
- Fake-clock unit tests
- End-to-end expiration tests

## Stage 6: Dead-letter queues

Deliver:

- Dead-letter transitions
- Dead-letter consumption
- Statistics
- Unit tests
- End-to-end tests

## Stage 7: Long polling

Deliver:

- Condition-variable notification
- Timeout behavior
- Publish wake-up
- Expiration wake-up
- Shutdown wake-up
- Concurrency tests

## Stage 8: Command-line client and documentation

Deliver:

- `pqctl`
- User guide
- API guide
- Architecture guide
- Development guide
- Testing guide
- Complete README

## Stage 9: Hardening

Deliver:

- Sanitizer-clean test run
- Improved error paths
- Concurrent-consumer stress test
- Graceful shutdown test
- Compiler-warning cleanup

Each stage MUST leave the project buildable and testable.

---

# 47. Optional Benchmark Extension: Delayed Messages

Delayed publishing is an optional extension intended as a final coding-harness challenge.

## 47.1 Publish request

The publish endpoint gains:

```json
{
  "payload": {
    "task": "send-reminder"
  },
  "deliver_after": "2026-07-13T15:00:00.000Z"
}
```

or alternatively:

```json
{
  "payload": {
    "task": "send-reminder"
  },
  "delay_ms": 60000
}
```

The implementation SHOULD support one form, not necessarily both.

A delayed message MUST not be reservable before its availability time.

## 47.2 Delayed statistics

Queue statistics gain:

```json
{
  "delayed": 4
}
```

## 47.3 Scheduling requirement

The implementation MUST NOT continuously scan the database in a tight polling loop.

A waiting consumer SHOULD sleep until the earliest of:

- Its long-poll deadline
- The next delayed message availability time
- The next reservation-expiry time
- A notifier signal
- Server shutdown

## 47.4 Delayed-message tests

Required tests:

- Delayed message unavailable before its deadline
- Delayed message available at its deadline
- Earlier normal message delivered first
- Restart preserves delay
- Long poll wakes when delayed message becomes eligible
- Statistics report delayed messages separately

---

# 48. Definition of Done

PocketQueue version 1.0 is complete when:

- The required server and client executables build.
- All required HTTP endpoints are implemented.
- SQLite persistence is operational.
- Message reservation is concurrency-safe.
- Visibility timeouts work.
- Retry limits work.
- Dead-letter queues work.
- Long polling works without busy looping.
- Graceful shutdown works.
- Required unit tests pass.
- Required end-to-end tests pass.
- The concurrency stress test passes.
- The test suite passes under AddressSanitizer on Linux.
- The project builds without compiler warnings under the supported reference compiler.
- User documentation is complete.
- Developer documentation is complete.
- No known critical or high-severity defects remain.

The following command sequence MUST complete successfully on the reference platform:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

A manual smoke test MUST also succeed:

```bash
./build/pocketqueue-server --database /tmp/pocketqueue.db
```

In another terminal:

```bash
./build/pqctl publish jobs '{"task":"demo"}'
./build/pqctl consume jobs --wait-ms 1000
./build/pqctl ack jobs MESSAGE_ID RECEIPT
./build/pqctl stats jobs
```

The final statistics output must show that the queue contains no active messages.
