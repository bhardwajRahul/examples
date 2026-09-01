#!/usr/bin/env python3
"""E2E smoke for stage 1: start the server, hit /healthz and /readyz, stop it."""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_get(url: str, timeout: float = 5.0) -> tuple[int, str]:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.status, resp.read().decode("utf-8")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: smoke.py <server-binary> <migrations-dir>", file=sys.stderr)
        return 1
    server_path = sys.argv[1]
    migrations_dir = sys.argv[2]
    if not os.path.exists(server_path):
        print(f"server binary not found: {server_path}", file=sys.stderr)
        return 1
    if not os.path.isdir(migrations_dir):
        print(f"migrations dir not found: {migrations_dir}", file=sys.stderr)
        return 1

    port = pick_free_port()
    tmpdir = tempfile.mkdtemp(prefix="pq-e2e-")
    db_path = os.path.join(tmpdir, "smoke.db")
    log_path = os.path.join(tmpdir, "smoke.log")

    env = os.environ.copy()
    proc = subprocess.Popen(
        [server_path,
         "--bind", "127.0.0.1",
         "--port", str(port),
         "--database", db_path,
         "--migrations-dir", migrations_dir,
         "--log-level", "info",
         "--log-format", "text"],
        stdout=open(log_path, "w"),
        stderr=subprocess.STDOUT,
        env=env,
    )
    try:
        # Wait for the server to become ready.
        deadline = time.time() + 10.0
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz",
                                            timeout=1.0) as r:
                    if r.status == 200:
                        break
            except Exception:
                time.sleep(0.1)
        else:
            print("server did not become ready in time", file=sys.stderr)
            with open(log_path) as f:
                print(f.read(), file=sys.stderr)
            return 1

        status, body = http_get(f"http://127.0.0.1:{port}/healthz")
        if status != 200 or '"ok"' not in body:
            print(f"unexpected /healthz: {status} {body!r}", file=sys.stderr)
            return 1

        status, body = http_get(f"http://127.0.0.1:{port}/readyz")
        if status != 200 or '"ready"' not in body:
            print(f"unexpected /readyz: {status} {body!r}", file=sys.stderr)
            return 1

        # Unknown route should 404.
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/nope", timeout=1.0)
            print("/nope unexpectedly returned", file=sys.stderr)
            return 1
        except urllib.error.HTTPError as e:
            if e.code != 404:
                print(f"/nope: expected 404, got {e.code}", file=sys.stderr)
                return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    print("e2e_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
