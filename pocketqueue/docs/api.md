# API Reference

All endpoints are HTTP/1.1 + JSON, served at the address passed to
`pocketqueue-server --bind` / `--port`. The default is
`http://127.0.0.1:8080`. All requests are independent — no
keep-alive, no session.

The same API is exercised by `pqctl`. See [§ 6](#6-command-line-client-pqctl)
for the CLI mapping.

## 1. Conventions

### Path parameters

| Placeholder | Rule |
|---|---|
| `{queue}` | regex `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`; case-sensitive; `.dead` suffix is reserved |
| `{id}` | a UUIDv7 message identifier (36 chars) |

### JSON body

Requests with bodies must set `Content-Type: application/json` and a
`Content-Length` header. The body itself must be a JSON object.

### Response shape

Successful responses are `application/json`. Error responses (4xx/5xx)
follow:

```json
{ "error": { "code": "invalid_request", "message": "human-readable text" } }
```

The error object may include a `details` sub-object for structured
context (e.g. a field name). The server never returns SQL, internal
addresses, or stack traces.

### Status code summary

| Code | When |
|---|---|
| 200 | reserve / stats success |
| 201 | publish success (with `Location` header) |
| 204 | ack / nack success, **or** reserve saw no message in time |
| 400 | malformed JSON, missing field, out-of-range value |
| 404 | unknown queue / message id |
| 405 | wrong HTTP method (response includes `Allow` header) |
| 409 | reservation conflict (wrong receipt, expired, not reserved) |
| 413 | request body larger than `--max-body-bytes` |
| 415 | missing or wrong `Content-Type` |
| 500 | unexpected server error (see server log; not echoed to client) |
| 503 | server is shutting down, or SQLite is busy past `busy_timeout` |

---

## 2. Publish

`POST /queues/{queue}/messages`

### Request

```http
POST /queues/jobs/messages
Content-Type: application/json
Content-Length: ...

{
  "payload":      <any JSON value>,
  "max_attempts": 3
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `payload` | any | yes | Stored verbatim. May be `null`, `object`, `array`, `string`, `number`, `bool`. |
| `max_attempts` | int | no | 1–100. Omit to use `--default-max-attempts`. |

Publishing to `<queue>.dead` is rejected.

### Response 201

```http
HTTP/1.1 201 Created
Content-Type: application/json
Location: /queues/jobs/messages/019f657e-62c2-7885-9add-9619d78a80c5

{
  "id":           "019f657e-62c2-7885-9add-9619d78a80c5",
  "queue":        "jobs",
  "payload":      { ... same as request ... },
  "created_at":   "2026-07-15T12:30:45.123Z",
  "available_at": "2026-07-15T12:30:45.123Z",
  "attempts":     0,
  "max_attempts": 3
}
```

### Errors

| Code | When |
|---|---|
| 400 `invalid_request` | missing/extra top-level field, bad `max_attempts` |
| 400 `invalid_json` | body isn't valid JSON or isn't a top-level object |
| 400 `invalid_queue_name` | queue name doesn't match the regex |
| 413 `payload_too_large` | body or serialized payload larger than `--max-body-bytes` |
| 415 | missing `Content-Type: application/json` |

---

## 3. Reserve

`GET /queues/{queue}/messages`

### Query parameters

| Name | Type | Default | Notes |
|---|---|---|---|
| `wait_ms` | int (0 … `--max-wait-ms`) | 0 | Long-poll budget. The server holds the request open and responds early when a message becomes available (publish, nack, or reservation expiry). |
| `visibility_timeout_ms` | int (in `[--min-visibility-ms, --max-visibility-ms]`) | `--default-visibility-ms` | The reservation deadline. Set lower for fast-turnaround work, higher for slow jobs. |

### Response 200

Same shape as the publish response, with these additional fields:

```json
{
  "id":             "...",
  "queue":          "jobs",
  "payload":        { ... },
  "created_at":     "...",
  "available_at":   "...",
  "attempts":       1,
  "max_attempts":   3,
  "receipt":        "5546345a96a97cdadfc029c2a17e4f8b",
  "reserved_until": "2026-07-15T12:31:15.123Z"
}
```

`receipt` is a 32-character hex string. It MUST be supplied verbatim to
`ack` or `nack` — modifying it changes its meaning and produces a
`409 reservation_conflict`.

`attempts` is incremented on every successful reserve.

### Response 204

Returned when the wait elapses without a message becoming available.
Body is empty.

### Errors

| Code | When |
|---|---|
| 400 `invalid_request` | `wait_ms` or `visibility_timeout_ms` out of range |
| 400 `invalid_queue_name` | queue name doesn't match the regex |
| 404 `not_found` | (no `not_found` here — 204 is used for empty) |

---

## 4. Acknowledge

`POST /queues/{queue}/messages/{id}/ack`

### Request

```http
POST /queues/jobs/messages/019f657e-.../ack
Content-Type: application/json

{ "receipt": "5546345a96a97cdadfc029c2a17e4f8b" }
```

### Response 204

Empty body. The message is permanently removed.

### Errors

| Code | When |
|---|---|
| 400 `invalid_request` | missing `receipt` |
| 400 `invalid_queue_name` / `not_found` | queue or message id don't exist |
| 409 `reservation_conflict` | message is not currently reserved, **or** the receipt doesn't match, **or** the reservation expired |

The server runs its normal expiration pass *before* evaluating the
receipt match (spec §16.3). If your reservation expired and was
reclaimed, the next `ack` returns 409 with `reservation_conflict`.

---

## 5. Negative-acknowledge

`POST /queues/{queue}/messages/{id}/nack`

### Request

```http
POST /queues/jobs/messages/019f657e-.../nack
Content-Type: application/json

{ "receipt": "...", "reason": "image decoder failed" }
```

`reason` is optional, ≤1024 UTF-8 bytes. It's stored on the message
and visible in the `<queue>.dead` payload.

### Response 204

Empty body. The server's next action depends on `attempts` vs
`max_attempts`:

- If `attempts + 1 < max_attempts`, the message returns to `READY` (or
  `DEAD_READY` if it was on the dead-letter queue).
- Otherwise, the message is moved to `<queue>.dead`.

Nacking a `dead` message keeps it in the dead-letter queue — the
spec forbids recursive dead-lettering (§8.5).

### Errors

Same as `ack`.

---

## 6. Queue statistics

`GET /queues/{queue}/stats`

### Response 200

```json
{
  "queue":                "jobs",
  "ready":                12,
  "reserved":             3,
  "dead_lettered":        2,
  "total_active":         15,
  "oldest_ready_age_ms":  4821
}
```

| Field | Meaning |
|---|---|
| `ready` | messages eligible for immediate reservation |
| `reserved` | messages currently held by some consumer |
| `dead_lettered` | messages currently in `<queue>.dead` (always 0 for `<queue>.dead` itself) |
| `total_active` | `ready + reserved` |
| `oldest_ready_age_ms` | ms since the oldest READY message was published, or 0 if empty |

`stats` first runs the standard expiration pass, so the numbers always
reflect the most up-to-date state.

---

## 7. Health

`GET /healthz` — liveness. Always returns 200 with `{"status":"ok"}` if
the HTTP listener is up.

`GET /readyz` — readiness. Returns 200 with `{"status":"ready"}` if a
trivial `SELECT 1` against the database succeeds, else 503 with
`{"status":"not_ready"}`.

Use these to drive orchestrator health checks (k8s liveness / readiness).

---

## 8. Command-line client (`pqctl`)

`pqctl` is a thin wrapper around the HTTP API. Each subcommand prints
human-readable output and exits with a spec-defined code (§30.7):

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | general error |
| 2 | usage error (bad flags, etc.) |
| 3 | network error (couldn't reach the server) |
| 4 | server returned 4xx |
| 5 | server returned 5xx |

The server URL comes from `--server URL`, then `PQ_SERVER_URL`, then
`http://127.0.0.1:8080`.

```
pqctl [--server URL] <command> [args]

publish QUEUE JSON [--max-attempts N]
consume QUEUE [--wait-ms N] [--visibility-ms N] [--raw]
ack QUEUE MESSAGE_ID RECEIPT
nack QUEUE MESSAGE_ID RECEIPT [--reason TEXT]
stats QUEUE
health
```

### Example session

```bash
$ ./build/pqctl publish jobs '{"task":"render","frame":42}'
019f657d-45bb-7ab3-b0e8-2decd429d2e9

$ ./build/pqctl consume jobs --wait-ms 2000
id:        019f657d-45bb-7ab3-b0e8-2decd429d2e9
receipt:   16c2c90dd10771542d1cac92529d8aaf
attempts:  1
payload:   {"task":"render","frame":42}

$ ./build/pqctl ack jobs 019f657d-45bb-7ab3-b0e8-2decd429d2e9 16c2c90dd10771542d1cac92529d8aaf
# (no output, exit 0)

$ ./build/pqctl stats jobs
{"queue":"jobs","ready":0,"reserved":0,"dead_lettered":0,"total_active":0,"oldest_ready_age_ms":0}
```

`--raw` on `consume` skips the `key: value` formatting and prints the
JSON body verbatim — useful for piping into `jq`.
