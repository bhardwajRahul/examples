#!/usr/bin/env python3
"""E2E for pq-spider: start a tiny local web server with linked pages,
publish its seed URL to pocketqueue, run pq-spider for a few seconds,
verify that all the linked pages were consumed and acked, and that
a deliberately-bad URL ends up in pages.dead.
"""
from __future__ import annotations

import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
BUILD = os.environ.get("BUILD_DIR", os.path.join(ROOT, "build"))
# The spider's CMakeLists adds itself under examples/pq-spider/, so the
# binary ends up at build/examples/pq-spider/pq-spider. The server,
# built at top level, lives at build/pocketqueue-server.
SERVER = os.path.join(BUILD, "pocketqueue-server")
SPIDER = os.path.join(BUILD, "examples", "pq-spider", "pq-spider")
MIGRATIONS = os.path.join(ROOT, "migrations")


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_get(url: str, timeout: float = 5.0):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def main() -> int:
    if not os.path.exists(SERVER) or not os.path.exists(SPIDER):
        print(f"missing binaries: server={SERVER} spider={SPIDER}", file=sys.stderr)
        return 1
    if not os.path.isdir(MIGRATIONS):
        print(f"migrations dir {MIGRATIONS} missing", file=sys.stderr)
        return 1

    # --- Tiny local web server ----------------------------------------
    pages = {
        "/index.html": """<!doctype html>
<html><head><title>Index</title></head>
<body>
<h1>Index</h1>
<a href="/page1.html">page 1</a>
<a href="/page2.html">page 2</a>
<a href="/missing.html">missing</a>
</body></html>
""",
        "/page1.html": """<!doctype html>
<html><head><title>Page 1</title></head>
<body><h1>Page 1</h1>
<a href="/page2.html">page 2</a>
<a href="/index.html">index</a>
</body></html>
""",
        "/page2.html": """<!doctype html>
<html><head><title>Page 2</title></head>
<body><h1>Page 2</h1>
<a href="/index.html">back</a>
</body></html>
""",
        "/missing.html": None,  # sentinel for 404
    }

    class TestHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = pages.get(self.path)
            if body is None:
                self.send_response(404)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"missing")
            else:
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(body.encode())
        def log_message(self, *args, **kwargs):
            pass  # silence

    web_port = pick_free_port()
    web_server = http.server.HTTPServer(("127.0.0.1", web_port), TestHandler)
    web_thread = threading.Thread(target=web_server.serve_forever, daemon=True)
    web_thread.start()
    web_url = f"http://127.0.0.1:{web_port}"

    pq_tmpdir = tempfile.mkdtemp(prefix="pq-spider-e2e-")
    pq_db = os.path.join(pq_tmpdir, "spider.db")
    pq_log = os.path.join(pq_tmpdir, "spider-server.log")
    pq_port = pick_free_port()
    pq_proc = subprocess.Popen(
        [SERVER, "--bind", "127.0.0.1", "--port", str(pq_port),
         "--database", pq_db, "--migrations-dir", MIGRATIONS,
         "--log-level", "warn", "--log-format", "text"],
        stdout=open(pq_log, "w"), stderr=subprocess.STDOUT,
    )
    pq_url = f"http://127.0.0.1:{pq_port}"

    spider_log = os.path.join(pq_tmpdir, "spider.log")
    spider_seeds = os.path.join(pq_tmpdir, "seeds.txt")
    spider_dedup = os.path.join(pq_tmpdir, "dedup.tsv")
    with open(spider_seeds, "w") as f:
        f.write(f"{web_url}/index.html\n")

    spider_proc = None
    try:
        # Wait for pocketqueue to be ready
        for _ in range(50):
            st, _ = http_get(f"{pq_url}/healthz")
            if st == 200: break
            time.sleep(0.1)

        # Start spider with 4 threads, low max_attempts so /missing.html
        # dead-letters quickly.
        spider_proc = subprocess.Popen(
            [SPIDER,
             "--server", pq_url,
             "--seeds", spider_seeds,
             "--dedup", spider_dedup,
             "--threads", "4",
             "--visibility-ms", "2000",
             "--max-attempts", "2",
            ],
            stdout=open(spider_log, "w"), stderr=subprocess.STDOUT,
        )

        # Give it ~15s to crawl; we expect:
        # - /index.html, /page1.html, /page2.html discovered and acked
        # - /missing.html to dead-letter after 2 attempts
        deadline = time.time() + 25
        ok_pages = False
        ok_dead = False
        while time.time() < deadline:
            _, body = http_get(f"{pq_url}/queues/pages/stats")
            j = json.loads(body)
            if (j["ready"] == 0 and j["reserved"] == 0):
                # All live pages processed; check dead-letter.
                _, body = http_get(f"{pq_url}/queues/pages.dead/stats")
                jd = json.loads(body)
                if jd.get("ready", 0) >= 1:
                    ok_dead = True
                    ok_pages = True
                    break
            time.sleep(0.5)

        # Final stats for the report
        st_p, b_p = http_get(f"{pq_url}/queues/pages/stats")
        st_d, b_d = http_get(f"{pq_url}/queues/pages.dead/stats")
        st_a, b_a = http_get(f"{pq_url}/queues/assets/stats")
        print(f"pages stats: {b_p}")
        print(f"pages.dead stats: {b_d}")
        print(f"assets stats: {b_a}")

        jp = json.loads(b_p)
        jd = json.loads(b_d)
        ja = json.loads(b_a)

        if jp.get("ready", 0) != 0 or jp.get("reserved", 0) != 0:
            print(f"FAIL: pages queue not empty: {b_p}")
            return 1
        if jd.get("ready", 0) < 1:
            print(f"FAIL: pages.dead queue should have the missing URL")
            return 1
        # assets should be empty (no assets linked from our pages)
        if ja.get("ready", 0) != 0 or ja.get("reserved", 0) != 0:
            print(f"FAIL: assets queue not empty: {b_a}")
            return 1
        if not ok_dead:
            print(f"WARN: timeout reached before dead-letter observed")
        print("spider: PASS")
        return 0
    finally:
        if spider_proc is not None and spider_proc.poll() is None:
            spider_proc.terminate()
            try: spider_proc.wait(timeout=5)
            except subprocess.TimeoutExpired: spider_proc.kill()
        pq_proc.terminate()
        try: pq_proc.wait(timeout=5)
        except subprocess.TimeoutExpired: pq_proc.kill()
        web_server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
