# pq-spider

`pq-spider` is a small breadth-first web crawler built as a PocketQueue
example. It demonstrates using the queue as the work-distribution
backbone for a multi-threaded I/O workload: many threads fetch and
parse pages in parallel, push newly-discovered URLs back to the server,
and PocketQueue's FIFO + visibility-timeout machinery handles
back-pressure for free.

## Two-queue topology

| Queue | Content | Routing |
|-------|---------|---------|
| `pages`   | `text/html` / `application/xhtml+xml` | extracted and parsed for links |
| `assets`  | everything else | fetched but not parsed |

URLs with extensions like `.png`, `.js`, `.css`, `.zip`, `.pdf` are
heuristically routed to `assets`; the rest go to `pages`. Failed
deliveries eventually dead-letter after `max-attempts` retries.

## Build

```bash
cmake -S . -B build -DPQ_ENABLE_ASAN=ON  # or UBSAN
cmake --build build --target pq-spider
```

`pq-spider` links against libcurl (system-installed) and pthread.

## Run

```bash
./build/pq-spider \
    --server http://127.0.0.1:8080 \
    --seeds seeds.txt \
    --threads 8 \
    --dedup dedup.tsv
```

`seeds.txt` has one URL per line. Lines starting with `#` are ignored.

Graceful shutdown on `SIGINT` / `SIGTERM`.

## Files

| File | Purpose |
|------|---------|
| `src/main.c` | CLI + workers setup |
| `src/worker.{c,h}` | One thread: reserve → fetch → extract → ack/nack |
| `src/http_fetch.{c,h}` | libcurl wrapper |
| `src/extract.{c,h}` | Regex-based `<a href>`, `<img src>` etc. extraction |
| `src/url_norm.{c,h}` | URL canonicalization + relative resolution |
| `src/dedup.{c,h}` | Persistent hash-table dedup store |
| `src/pqctl_client.{c,h}` | Talk to pocketqueue-server over HTTP |

## Tested with

`tests/e2e/spider_test.py` runs pq-spider against a tiny local web
server and verifies that:
- all linked pages are eventually consumed and acked
- a deliberately-bad URL ends up in `pages.dead`
