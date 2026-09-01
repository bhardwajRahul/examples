# PocketQueue + pq-spider Implementation Plan

**Status:** Confirmed direction — phases, decisions, and scope agreed.
**Source of truth:** `PocketQueue.md` (spec, sections 1–48), `SPIDER.md` (example app).
**Goal:** Build PocketQueue v1.0 to "Definition of Done" (`PocketQueue.md` §48) and ship `pq-spider` as a v1.0 example workload.

This plan sequences work, names decisions, and points at the spec section that governs each step. When the spec and this plan disagree, the spec wins.

---

## 0. Decisions

Lock-ins agreed before any code:

| # | Decision | Choice | Rationale | Spec ref |
|---|---|---|---|---|
| D1 | HTTP library | **Hand-rolled minimal HTTP/1.1 server** (`src/http_server.c`) | Deviation from "libmicrohttpd" decided at implementation time. Constraint: dev packages unavailable, `sudo` blocked, Mongoose/CivetWeb are GPL (incompatible with MIT, D3), libmicrohttpd uses autoconf (not available). Spec §4.3 permits "another actively maintained C HTTP library" or none. Hand-rolling gives full control over graceful shutdown (spec §26) and keeps the dep tree to SQLite + cJSON + cmocka. Trade-off: ~600 lines of HTTP code in-tree. | §4.3, §26 |
| D2 | JSON library | **cJSON** | Tiny, clean C API, permissively licensed, vendored via `FetchContent` | §4.3 |
| D3 | Project license | **MIT** | Permissive, short, compatible with all chosen deps | §1 |
| D4 | SQLite threading model | **Mutex-protected single connection** for v1.0 | Smallest surface area; spec explicitly allows it; long-poll waits must release the mutex anyway | §32 |
| D5 | SQLite schema location | `migrations/V001__init.sql`, sequential numeric versions | Simple, transactional, easy to roll forward | §10, §27 |
| D6 | Logger | Hand-rolled, thread-safe, two formats (text + JSON), levels `error/warn/info/debug` | Avoids a dep, keeps log policy under our control | §25 |
| D7 | `pq_error` representation | `struct { char code[32]; char message[256]; char details_json[512]; }` | Bounded, no heap, easy to copy through layers | §42 |
| D8 | Identifier format | UUID v7 string (lexicographic, time-ordered) | Better index locality than v4; spec allows v4 or v7 | §23 |
| D9 | Receipt token | 32 hex chars from `getrandom()` (with `/dev/urandom` fallback on older glibc) | Unpredictable, fixed width | §23 |
| D10 | `.clang-format` | 4-space indent, `ColumnLimit: 100`, `IndentWidth: 4`, `PointerAlignment: Right` | Matches the spec's "explicit, consistent" coding-standards section | §42 |
| D11 | Compiler flags baseline | `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror=implicit-function-declaration` | Spec lists `-Wall/-Wextra/-Wpedantic/-Wconversion/-Wshadow` | §34 |
| D12 | v1.0 scope | **PocketQueue core (stages 1–9) + `pq-spider`** | Spec §47 delayed-messages extension deferred to a post-v1.0 release | §47, §48 |

D1, D2, D3 are load-bearing — changing them later means rework across HTTP, JSON, and licensing layers. D4 can be revisited later behind the same queue-service interface. D12 means `pq-spider` is a v1.0 deliverable, not a follow-on. D1's deviation is documented; if libmicrohttpd becomes available, swapping in is a self-contained change to `src/http_server.c` plus a CMake adjustment.

---

## 1. Project Scaffolding (before stage 1)

- Create the deliverable tree (`include/pocketqueue/`, `src/`, `tests/{unit,e2e}/`, `docs/`, `migrations/`, `examples/`) per spec §6.
- Top-level `CMakeLists.txt` with:
  - `cmake_minimum_required(VERSION 3.20)`, `project(pocketqueue C)`.
  - `set(CMAKE_C_STANDARD 17)` + `C_STANDARD_REQUIRED ON`.
  - `find_package(SQLite3 REQUIRED)`, `pkg_check_modules(LIBMICROHTTPD REQUIRED libmicrohttpd)`.
  - Vendored cJSON via `FetchContent`.
  - Options `PQ_ENABLE_ASAN`, `PQ_ENABLE_UBSAN` (per spec §31, §39).
  - Two executables: `pocketqueue-server`, `pqctl`.
  - `enable_testing()` + `add_subdirectory(tests/unit)` + `add_subdirectory(tests/e2e)`.
- Add `LICENSE` with the MIT text (per D3).
- Add `.clang-format` (per D10).
- Add a developer `README` section linking to spec + this plan.
- Sanity check: an empty `pocketqueue-server` that prints its version and exits 0, plus a CTest no-op test, must build and pass.

**Acceptance:** `cmake -S . -B build && cmake --build build && ctest --test-dir build` succeeds.

---

## 2. PocketQueue Stages (mirror spec §46, with notes)

Each stage must leave the tree buildable and `ctest` green. Stages are sequential; within a stage the order of subtasks is loose.

### Stage 1 — Foundation
Spec §46.1 / §19 / §24 / §25.

- `src/server_main.c`: parse config (CLI > env > defaults; per D10 flags), init logger, open SQLite, run migrations table stub, install `SIGINT`/`SIGTERM` handlers, register `atexit` cleanup, start libmicrohttpd daemon, block on signal, drain, exit.
- `src/config.{c,h}`: parse every option from spec §24, validate ranges (visibility min/max/default, max-attempts 1–100, max-wait, max-body, port 1–65535), reject invalid configs with exit code 2.
- `src/logger.{c,h}`: levels, formats (text + JSON), thread-safe via internal mutex; honor `--log-level` and `--log-format`.
- `src/http_server.{c,h}`: libmicrohttpd access handler that routes by method + path; placeholder responses for `/healthz` (`{"status":"ok"}`, always 200) and `/readyz` (lightweight `SELECT 1`, 200/503).
- `src/sqlite_repository.{c,h}`: open with `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE`, apply pragmas from spec §10.1, store handle behind the global mutex (D4).
- `src/migrations.{c,h}`: read `schema_version`, apply pending migrations transactionally, refuse to start if `version > supported_version`.

**Acceptance:** Server starts, `/healthz` returns 200, `/readyz` returns 200, `Ctrl-C` exits 0, `ctest` (smoke) passes.

### Stage 2 — In-memory queue behavior
Spec §46.2 / §8 / §35.

- Domain structs (`pq_message`, `pq_reservation`, `pq_queue_stats`) in `include/pocketqueue/`.
- `src/queue_name.{c,h}`: validate against `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`, reject `.dead` suffix for publish operations.
- `src/queue_service.{c,h}`: the in-memory variant that satisfies the suggested interface in spec §43 (`pq_service_publish/reserve/ack/nack`). State lives in a `struct pq_service`; the SQLite-backed implementation in stage 4 will replace the storage.
- `src/clock.{c,h}`: `pq_clock` vtable (wall + monotonic) with `pq_clock_system()` and `pq_clock_fake(now_ms)` (spec §12).
- Unit tests in `tests/unit/`:
  - Queue-name validation matrix (spec §35.1).
  - Config parsing (defaults, overrides, env precedence, invalid).
  - Publish/reserve/ack/nack happy paths using the fake clock.

**Acceptance:** All stage-2 unit tests pass; HTTP layer still not wired to queue service.

### Stage 3 — HTTP API
Spec §13–§22 / §46.3 / §20.

- `src/http_routes.{c,h}`: parse URL path params, query params (`wait_ms`, `visibility_timeout_ms`), content-type, JSON body (cJSON); call queue service; map `pq_status` → HTTP status + structured `{"error":{"code","message","details"}}` body.
- Implement endpoints: `POST /queues/{q}/messages`, `GET /queues/{q}/messages`, `POST .../ack`, `POST .../nack`, `GET /queues/{q}/stats`.
- Payload size guard: read into bounded buffer, `413` before reading more than `max_body_bytes`.
- Unknown top-level properties in publish body → `400 invalid_request`.
- Wire `pqctl` to call these endpoints; ship `publish|consume|ack|nack|stats|health` with spec §30 exit codes.
- E2E test harness (`tests/e2e/harness.py`): spawn `pocketqueue-server` with `--database <tmp>` and an unused port, wait for `/readyz`, run a scenario, terminate. Enforce per-scenario deadlines.
- E2E scenarios 1, 2, 3, 13 from spec §37.

**Acceptance:** `pqctl` smoke (`publish → consume → ack → stats` shows empty) works against the live server. Spec §48 manual smoke test green.

### Stage 4 — SQLite persistence
Spec §10 / §46.4 / §27.

- Implement the schema from spec §10 in `migrations/V001__init.sql`. Apply on startup.
- `src/sqlite_repository.{c,h}`: bind-parameter statements for insert / select-oldest-ready / atomic-reserve / ack / nack / move-to-dead / stats / recover-expired. Use `BEGIN IMMEDIATE` for reservation (spec §10.2).
- All client-supplied strings bound, never concatenated (spec §34).
- Replace the in-memory storage in `queue_service` with the SQLite-backed repo. Service interface stays the same.
- E2E scenarios 7, 8 (restart persistence) from spec §37.

**Acceptance:** Restart preserves messages in flight and at rest; queue-service unit tests still pass (now exercised against a temp SQLite file).

### Stage 5 — Visibility timeout & retries
Spec §9.3 / §14–§17 / §28 / §46.5.

- Reservation sets `reserved_until_ms` from `visibility_timeout_ms`.
- `recover_expired_for_queue()`: invoked at startup, before every reserve, before stats, and when a long-poll wait reaches a known deadline (spec §28). Mutates expired normal reservations: return to `READY` if `attempts < max_attempts`, else move to `<queue>.dead`.
- Receipt verification on ack/nack: compare against stored `receipt_token`; on mismatch apply expiration recovery first, then return `409 Conflict` if the reservation has moved on (spec §16.3, §17.4).
- Fake-clock unit tests for all visibility-timeout cases (spec §35.1).
- E2E scenarios 4, 5 from spec §37.

**Acceptance:** No real-time sleeps in unit tests; expiry works deterministically.

### Stage 6 — Dead-letter queues
Spec §8.5 / §9.2 / §18 / §46.6.

- `<queue>.dead` is virtual — not a separate SQLite table, just a `queue_name` value with the suffix and `state ∈ {DEAD_READY, DEAD_RESERVED}`. Reserve on `.dead` works the same way; nack from `.dead` returns to `DEAD_READY` (no recursive dead-lettering, spec §8.5).
- Stats for `.dead` omit `dead_lettered` (spec §18).
- Unit + e2e scenarios 6 from spec §37.

**Acceptance:** Poison URLs flow to `<queue>.dead` after `max_attempts` rejections.

### Stage 7 — Long polling
Spec §15.4 / §29 / §46.7.

- `src/notifier.{c,h}`: process-local `pq_notifier` (mutex + condvar + `uint64_t generation` + `bool shutting_down`). Operations that may make a message available (publish, nack, reservation expiration) increment `generation` and broadcast.
- Reserve loop:
  1. Recover expired reservations for the queue.
  2. Try to reserve; if found, return.
  3. Compute remaining wait = `wait_ms - elapsed` and `next_event_ms = min(next_available, next_reservation_expiry)`.
  4. Wait on condvar with timeout; on wake (signal, timeout, shutdown) recheck.
- Must NOT hold the DB mutex or any open SQLite transaction while waiting (spec §15.4, §29).
- During shutdown: set `shutting_down`, broadcast, drain in-flight requests, then exit.
- E2E scenarios 9, 10, 12 from spec §37.

**Acceptance:** Empty-queue long-poll returns within ~100 ms after a publish; no busy-loop under load.

### Stage 8 — `pqctl` polish and documentation
Spec §30 / §40 / §41 / §46.8.

- `pqctl` flags: `--server URL` (`PQ_SERVER_URL` fallback), `--max-attempts`, `--wait-ms`, `--visibility-ms`, `--reason`, `--raw`.
- Write `docs/user-guide.md`, `docs/api.md`, `docs/architecture.md`, `docs/development.md`, `docs/testing.md`.
- Expand `README.md` with quick-start, dependencies, links to docs.

**Acceptance:** Every `pqctl` subcommand documented with an example in `user-guide.md`; `README.md` quick-start works on a clean machine.

### Stage 9 — Hardening
Spec §39 / §44 / §46.9 / §48.

- AddressSanitizer + UBSan builds clean (`build-asan`, `build-ubsan`).
- 1,000-message × 8-consumer stress test (spec §36) — add as `tests/e2e/stress.py`, gating CI.
- Static analysis via `clang-tidy` on the `src/` tree (warnings config in D10).
- Reduce log noise, tighten error paths, finalize exit codes.
- Definition-of-Done checklist (spec §48) signed off.

**Acceptance:** Full suite passes under ASan, UBSan, and the reference compiler at `-Werror`.

---

## 3. Deferred — Delayed Messages Extension

Spec §47. **Excluded from v1.0** by D12. Reopen only if explicitly requested.

Sketch (for when it lands):

- No new schema column needed — store delayed messages as normal `READY` rows with `available_at_ms` in the future. Add `deliver_after_ms`/`delay_ms` input on publish.
- Reservation query: `WHERE available_at_ms <= now`. Long-poll sleeper chooses the earliest of `wait_deadline | next_delayed_available | next_reservation_expiry` (spec §29, §47.3).
- Stats gain `delayed: N`.
- Tests from spec §47.4.

---

## 4. `pq-spider` (v1.0 deliverable, per D12)

Reference design: `SPIDER.md`. Implementation starts once PocketQueue stage 8 lands; spider-ASan pass runs after PocketQueue stage 9.

1. **Phase A — Scaffold + HTTP client**
   - `examples/pq-spider/CMakeLists.txt` (separate, depends on installed PocketQueue + libcurl).
   - `src/http_fetch.{c,h}`: libcurl wrapper with timeout, redirect limit, response capture, content-type detection.
   - `src/url_norm.{c,h}`: normalize (scheme lower-case, host lower-case, drop default port, sort query params, drop fragments).
   - `src/dedup.{c,h}`: persistent hash set (file-backed; one line per seen URL, with periodic compaction).

2. **Phase B — Worker loop**
   - `src/worker.{c,h}`: threads that reserve from `pocketqueue-server` over HTTP (a thin client around the same logic we use for `pqctl`). Publishes discovered links to `pages` or `assets` based on `Content-Type` filter (SPIDER.md §2.1). `max_attempts=3`.
   - Seed loading from `--seed-file` (one URL per line).
   - Graceful shutdown on `SIGINT`/`SIGTERM`: stop reserving, let in-flight finishes, exit.

3. **Phase C — Link extraction**
   - HTML parser: **Gumbo** (Google's HTML5 parser, vendored). Extract `<a href>`, `<link href>`, `<img src>`, `<script src>`, etc. Resolve relative URLs against the parent URL.

4. **Phase D — Testing**
   - E2E: start `pocketqueue-server`, run `pq-spider` against a tiny local HTTP server (`tests/e2e/spider_site.py`), assert discovered pages/assets match expectations, assert dead-letter queue only contains deliberate 5xx URLs.
   - Run under ASan; verify no leaks.

**Acceptance:** `pq-spider` crawls a 10-page test site, dedups correctly across restarts, and routes one bad URL to `pages.dead` after three attempts. v1.0 ships only after M10 is green.

---

## 5. Testing & Validation Strategy

- **Unit (CMocka)** — fast, hermetic, fake clock. Run in <5 s. Triggered on every build.
- **E2E (Python 3 stdlib)** — spawns the real server, ~30 s total. Triggered in CI and pre-merge.
- **Sanitizers** — ASan + UBSan runs gate merges.
- **Stress** — 1,000 msgs × 8 consumers, ~60 s, gated.
- **Manual smoke** — `pocketqueue-server` + `pqctl` loop from spec §48.

---

## 6. Milestones / Checkpoints

| Milestone | Stage gate | Demonstrable |
|---|---|---|
| M1 — Skeleton | Stage 1 | Server starts, `/healthz` returns 200, clean shutdown on `Ctrl-C` |
| M2 — Logic | Stage 2 | Queue-service unit tests green in isolation |
| M3 — API | Stage 3 | `pqctl` publish/consume/ack/stats round-trip works |
| M4 — Durable | Stage 4 | Messages survive restart, including in-flight reservations |
| M5 — Time | Stage 5 | Visibility timeouts work deterministically under fake clock |
| M6 — Dead letter | Stage 6 | Rejected URLs flow to `<queue>.dead` after `max_attempts` |
| M7 — Long poll | Stage 7 | Empty-queue wait returns within ~100 ms of publish |
| M8 — Documented | Stage 8 | Quick-start works on a clean machine; `docs/` complete |
| M9 — Hardened | Stage 9 | ASan/UBSan clean; 1k×8 stress passes; spec §48 sign-off |
| M10 — Spider | Phase A–D | `pq-spider` crawls test site; dedup persists across runs |

---

## 7. Remaining Risks

- **Long-poll under shutdown** is the trickiest correctness story (spec §15.4, §26). The notifier must be the only wake-up mechanism; SQLite stays source of truth. Review this path early in stage 7.
- **Receipt race** between expiration recovery and ack: stage 5 must serialize them on the same mutex.
- **`max_body_bytes` enforcement** must happen before reading the body — libmicrohttpd's POST handler lets us set a size cap on the daemon; confirm the exact flag during stage 3.
- **Windows/macOS portability**: spec lists them as SHOULD, not MUST. Defer beyond v1.0; keep `src/platform.{c,h}` honest so a later port is mechanical.
- **pq-spider HTML parsing**: Gumbo is the proposal because it has a clean C API and is permissively licensed. lexbor/libxml2 are alternatives if Gumbo proves problematic.
- **LICENSE banner placement**: add the standard MIT header to every new source file as it's created (don't rely on a single LICENSE file at the root).

---

## 8. What This Plan Is Not

- Not a copy of `PocketQueue.md` — the spec stays the authority on requirements.
- Not a backlog of every task — it's the sequence and the decisions. Granular tasks live in `docs/development.md` once stage 8 lands.
- Not a guarantee of dates — milestones above are ordering checkpoints, not schedules.
