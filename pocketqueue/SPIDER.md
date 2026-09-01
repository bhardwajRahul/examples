# pq-spider Implementation Plan

---

## 1. Overview

`pq-spider` is a breadth-first web crawler built as a PocketQueue example. It
demonstrates using PocketQueue as the work-distribution backbone for a real
multi-phase I/O workload: discovering links in HTML pages, fetching assets, and
persisting a dedup store across runs.

The crawler runs as a single process with multiple worker threads. Each thread
publishes work to PocketQueue and consumes from it, allowing horizontal scaling
by simply launching more `pq-spider` instances against the same server.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────┐
│  pq-spider (single process)                     │
│                                                 │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐   │
│  │ Crawl     │  │ Fetch     │  │ Dedup      │   │
│  │ Worker    │  │ Worker    │  │ Store      │   │
│  │ Thread    │  │ Thread    │  │ (file)     │   │
│  └─────┬─────┘  └─────┬─────┘  └───────────┘   │
│        │               │                        │
└────────┼───────────────┼────────────────────────┘
         │               │
         v               v
   ┌─────────────────────────────┐
   │     PocketQueue Server      │
   │  ┌─────────┐  ┌─────────┐  │
   │  │ pages   │  │ assets  │  │
   │  │ (HTML)  │  │ (other) │  │
   │  └─────────┘  └─────────┘  │
   └─────────────────────────────┘
```

### 2.1 Two-queue topology

| Queue | Purpose | Content-Type filter | Max attempts |
|-------|---------|---------------------|--------------|
| `pages` | HTML documents to parse for links | `text/html`, `application/xhtml+xml` | 3 |
| `assets` | Images, zips, PDFs, and other non-HTML resources | Everything else | 3 |

Every fetched URL is published to exactly one of these queues. Dead-letter
queues (`pages.dead`, `assets.dead`) are used for poison URLs that fail
repeatedly.

### 2.2 Message payload

```json
{
  "url": "https://example.com/page",
  "depth": 0,
  "parent": "https://example.com/index"
}
```

Fields:

| Field | Type | Description |
|-------|------|-------------|
| `url` | string | Normalized absolute URL to fetch |
| `depth` | integer | Hop count from seed URL (0 = seed) |
| `parent` | string | URL that discovered this link (empty for seeds) |

---

