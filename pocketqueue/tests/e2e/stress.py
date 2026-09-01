#!/usr/bin/env python3
"""Spec §36 stress test:
- ≥1,000 messages published
- ≥8 consumer threads reserve + ack each
- Every published ID is acked exactly once
- Queue is empty at end
"""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.environ.get("BUILD_DIR", os.path.join(ROOT, "build"))
SERVER = os.path.join(BUILD, "pocketqueue-server")


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_ready(url: str, deadline_s: float = 10.0) -> None:
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            with urllib.request.urlopen(url + "/healthz", timeout=1) as r:
                if r.status == 200:
                    return
        except Exception:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"server did not become ready: {url}")


def http_post(url: str, body: dict | None = None,
              headers: dict | None = None) -> tuple[int, str]:
    data = None
    if body is not None:
        import json
        data = json.dumps(body).encode()
    req = urllib.request.Request(url, method="POST",
                                 data=data,
                                 headers={"Content-Type": "application/json",
                                          **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def http_get_json(url: str, timeout: float = 10.0) -> tuple[int, dict | None]:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read().decode()
            import json
            return r.status, json.loads(body)
    except urllib.error.HTTPError as e:
        return e.code, None
    except Exception:
        return 0, None


def main() -> int:
    N_MESSAGES = 1000
    N_CONSUMERS = 8

    if not os.path.exists(SERVER):
        print(f"server not found: {SERVER}", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="pq-stress-")
    port = pick_free_port()
    db_path = os.path.join(tmp, "stress.db")
    log_path = db_path + ".log"

    proc = subprocess.Popen(
        [SERVER, "--bind", "127.0.0.1", "--port", str(port),
         "--database", db_path, "--migrations-dir",
         os.path.join(ROOT, "migrations"),
         "--log-level", "warn", "--log-format", "text"],
        stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
    )
    base_url = f"http://127.0.0.1:{port}"
    queue = "stress_q"
    try:
        wait_ready(base_url)

        # Publish all N_MESSAGES
        print(f"=== publishing {N_MESSAGES} messages to {queue} ===")
        t0 = time.time()
        ids: list[str] = []
        for i in range(N_MESSAGES):
            status, body = http_post(
                f"{base_url}/queues/{queue}/messages",
                {"payload": {"i": i}, "max_attempts": 5},
            )
            if status != 201:
                print(f"FAIL: publish #{i} status={status} body={body}",
                      file=sys.stderr)
                return 1
            # Body is JSON: extract id
            import json
            ids.append(json.loads(body)["id"])
        print(f"  published in {time.time() - t0:.2f}s")

        # Stats before consumers
        status, stats = http_get_json(f"{base_url}/queues/{queue}/stats")
        print(f"  initial stats: ready={stats['ready']} reserved={stats['reserved']}")

        # N_CONSUMERS threads, each reserves + acks until queue empty
        print(f"=== starting {N_CONSUMERS} consumer threads ===")
        consumed: list[str] = [None] * N_MESSAGES  # type: ignore
        consumed_lock = threading.Lock()
        stop_flag = {"stop": False}
        id_index = {}  # id → index (for finding consumed)

        def consumer(thread_idx: int):
            local_consumed = []
            local_index = {}
            while not stop_flag["stop"]:
                rc, body = http_get_json(
                    f"{base_url}/queues/{queue}/messages?visibility_timeout_ms=10000",
                    timeout=5.0,
                )
                if rc != 200 or body is None:
                    time.sleep(0.05)
                    continue
                msg_id = body["id"]
                receipt = body["receipt"]
                # Ack the message
                status, _ = http_post(
                    f"{base_url}/queues/{queue}/messages/{msg_id}/ack",
                    {"receipt": receipt},
                )
                if status == 204:
                    local_consumed.append(msg_id)
                    # Record index for validation
                    # The id encodes creation time. We just collect all.
                else:
                    print(f"  [{thread_idx}] ack failed rc={status}")
            # Merge into shared
            with consumed_lock:
                consumed.extend(local_consumed)

        threads = []
        for i in range(N_CONSUMERS):
            t = threading.Thread(target=consumer, args=(i,))
            t.start()
            threads.append(t)

        # Wait for all messages to be consumed
        deadline = time.time() + 60
        while time.time() < deadline:
            status, stats = http_get_json(f"{base_url}/queues/{queue}/stats")
            n_total = stats["ready"] + stats["reserved"]
            # Count actual consumed
            with consumed_lock:
                n_consumed = sum(1 for x in consumed if x is not None)
            if n_total == 0 and n_consumed >= N_MESSAGES:
                break
            time.sleep(0.2)

        stop_flag["stop"] = True
        for t in threads:
            t.join(timeout=5)

        status, stats = http_get_json(f"{base_url}/queues/{queue}/stats")
        with consumed_lock:
            consumed_now = [x for x in consumed if x is not None]

        print(f"=== results ===")
        print(f"  published: {len(ids)}")
        print(f"  consumed:  {len(consumed_now)}")
        print(f"  stats: ready={stats['ready']} reserved={stats['reserved']} "
              f"dead_lettered={stats['dead_lettered']}")

        ok = True
        if len(consumed_now) != N_MESSAGES:
            print(f"FAIL: expected {N_MESSAGES} consumed, got {len(consumed_now)}")
            ok = False
        if stats["ready"] != 0 or stats["reserved"] != 0:
            print(f"FAIL: queue is not empty")
            ok = False

        # Verify every published id was acked exactly once
        published = set(ids)
        acked = set(consumed_now)
        missing = published - acked
        extra = acked - published
        if missing:
            print(f"FAIL: {len(missing)} ids were never acked (first 5: {list(missing)[:5]})")
            ok = False
        if extra:
            print(f"FAIL: {len(extra)} acked ids never published (first 5: {list(extra)[:5]})")
            ok = False
        if len(consumed_now) != len(set(consumed_now)):
            dups = len(consumed_now) - len(set(consumed_now))
            print(f"FAIL: {dups} duplicate acks!")
            ok = False

        if ok:
            print("stress: PASS")
            ret = 0
        else:
            print("stress: FAIL")
            ret = 1

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    return ret


if __name__ == "__main__":
    sys.exit(main())