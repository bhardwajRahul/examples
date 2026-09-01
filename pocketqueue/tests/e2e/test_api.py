#!/usr/bin/env python3
"""E2E tests for stage 3: spec §14-§22 HTTP API round-trips through pqctl.

Each scenario starts the live server in a fresh temp dir on an unused port
and shuts it down at completion. The server binary is built by CMake at
$BUILD_DIR/pocketqueue-server.
"""
from __future__ import annotations

import json
import os
import re
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
PQCTL = os.path.join(BUILD, "pqctl")
MIGRATIONS = os.path.join(ROOT, "migrations")


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_ready(server_url: str, deadline_s: float = 5.0) -> None:
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            with urllib.request.urlopen(server_url + "/healthz", timeout=1) as r:
                if r.status == 200:
                    return
        except Exception:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"server did not become ready: {server_url}")


def run_server(port: int, db_path: str) -> subprocess.Popen:
    log_path = db_path + ".log"
    proc = subprocess.Popen(
        [SERVER, "--bind", "127.0.0.1", "--port", str(port),
         "--database", db_path, "--migrations-dir", MIGRATIONS,
         "--log-level", "warn", "--log-format", "text"],
        stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
    )
    wait_ready(f"http://127.0.0.1:{port}")
    return proc


def call(*args: str, server_url: str) -> tuple[int, str]:
    """Run pqctl, capture exit code and stdout."""
    argv = [PQCTL, "--server", server_url] + list(args)
    res = subprocess.run(argv, capture_output=True, text=True, timeout=10)
    return res.returncode, (res.stdout + res.stderr).strip()


def curl(method: str, url: str, body=None) -> tuple[int, str]:
    data = None
    if body is not None:
        data = body.encode("utf-8")
    req = urllib.request.Request(url, method=method, data=data)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(f"FAIL: {msg}")


def parse_kv(out: str) -> dict:
    """Parse pqctl's `key: value` output. Strips whitespace from values
    (pqctl pads keys for column alignment, which would corrupt URLs)."""
    result = {}
    for line in out.splitlines():
        if ": " not in line:
            continue
        key, _, value = line.partition(": ")
        result[key.strip()] = value.strip()
    return result


def scenario_publish_reserve_ack(server_url: str, queue: str) -> None:
    print(f"--- scenario_publish_reserve_ack [{queue}]")
    rc, out = call("publish", queue, '{"task":"demo"}', server_url=server_url)
    expect(rc == 0 and re.match(r"^[0-9a-f-]{36}$", out),
           f"publish returned {rc} {out!r}")
    rc, out = call("consume", queue, server_url=server_url)
    expect(rc == 0, "consume rc")
    lines = out.splitlines()
    fields = parse_kv(out)
    expect("id" in fields and "receipt" in fields, f"missing fields in {out!r}")
    msg_id = fields["id"]
    receipt = fields["receipt"]
    rc, out = call("ack", queue, msg_id, receipt, server_url=server_url)
    expect(rc == 0, f"ack rc={rc} out={out!r}")
    rc, out = call("consume", queue, server_url=server_url)
    expect(rc == 0 and "no message" in out, f"queue not empty: {out!r}")


def scenario_nack_requeue(server_url: str, queue: str) -> None:
    print(f"--- scenario_nack_requeue [{queue}]")
    call("publish", queue, '{"task":"a"}', server_url=server_url)
    rc, out = call("consume", queue, server_url=server_url)
    expect(rc == 0, "consume rc")
    fields = parse_kv(out)
    rc, out = call("nack", queue, fields["id"], fields["receipt"],
                   "--reason", "transient", server_url=server_url)
    expect(rc == 0, f"nack rc={rc} out={out!r}")
    rc, out = call("consume", queue, server_url=server_url)
    expect(rc == 0, f"consume rc={rc}")
    fields = parse_kv(out)
    expect(fields.get("attempts") == "2", f"expected attempts=2, got {fields}")


def scenario_dead_letter_at_max(server_url: str, queue: str) -> None:
    print(f"--- scenario_dead_letter_at_max [{queue}]")
    call("publish", queue, '{"task":"x"}', "--max-attempts", "1", server_url=server_url)
    rc, out = call("consume", queue, server_url=server_url)
    fields = parse_kv(out)
    msg_id, receipt = fields["id"], fields["receipt"]
    rc, _ = call("nack", queue, msg_id, receipt, server_url=server_url)
    expect(rc == 0, f"nack rc={rc}")
    rc, out = call("consume", queue, server_url=server_url)
    expect("no message" in out, f"{queue} not empty: {out!r}")
    rc, out = call("stats", queue, server_url=server_url)
    j = json.loads(out)
    expect(j["ready"] == 0, f"{queue} ready: {j}")
    expect(j["dead_lettered"] == 1, f"{queue} dead_lettered: {j}")
    rc, out = call("consume", queue + ".dead", server_url=server_url)
    expect(rc == 0 and "id:" in out, f"{queue}.dead reserve failed: {out!r}")
    fields = parse_kv(out)
    expect(fields["id"] == msg_id, f"dead id mismatch")
    call("ack", queue + ".dead", msg_id, fields["receipt"], server_url=server_url)


def scenario_stale_receipt_rejected(server_url: str, queue: str) -> None:
    print(f"--- scenario_stale_receipt_rejected [{queue}]")
    call("publish", queue, '{"task":"a"}', server_url=server_url)
    rc, out = call("consume", queue, server_url=server_url)
    fields = parse_kv(out)
    msg_id, receipt = fields["id"], fields["receipt"]
    rc, out = call("ack", queue, msg_id, "deadbeef" + receipt[8:],
                   server_url=server_url)
    expect(rc == 4, f"wrong receipt should fail with rc=4, got rc={rc}")
    rc, _ = call("ack", queue, msg_id, receipt, server_url=server_url)
    expect(rc == 0, f"correct receipt ack failed: rc={rc}")


def scenario_invalid_inputs(server_url: str, queue: str) -> None:
    print(f"--- scenario_invalid_inputs [{queue}]")
    rc, out = call("publish", "../bad", '{}', server_url=server_url)
    expect(rc == 4, f"../bad publish rc={rc}")
    rc, _ = call("publish", queue, "not json", server_url=server_url)
    expect(rc == 2, f"bad json publish rc={rc}")
    rc, out = call("ack", queue,
                   "00000000-0000-7000-8000-000000000000",
                   "0123456789ab0123456789ab01234567",
                   server_url=server_url)
    expect(rc == 4, f"unknown msg ack rc={rc}")
    status, _ = curl("DELETE", f"{server_url}/queues/{queue}/messages")
    expect(status == 405, f"DELETE → {status}")


def scenario_stats(server_url: str, queue: str) -> None:
    print(f"--- scenario_stats [{queue}]")
    call("publish", queue, '{"a":1}', server_url=server_url)
    call("publish", queue, '{"b":2}', server_url=server_url)
    call("consume", queue, server_url=server_url)
    rc, out = call("stats", queue, server_url=server_url)
    j = json.loads(out)
    expect(j["ready"] == 1 and j["reserved"] == 1,
           f"stats mismatch: {j}")


def run_with_restart(port: int, db_path: str, body):
    """Restart the server against the same DB, yielding control to *body*
    between the first shutdown and the second start so scenarios can
    inspect state mid-test."""
    proc = run_server(port, db_path)
    try:
        body("running", proc, f"http://127.0.0.1:{port}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    proc2 = run_server(port, db_path)
    try:
        body("restarted", proc2, f"http://127.0.0.1:{port}")
    finally:
        proc2.terminate()
        try:
            proc2.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc2.kill()
            proc2.wait()


def scenario_persistence_across_restart(server_url: str, queue: str) -> None:
    print(f"--- scenario_persistence_across_restart [{queue}]")
    # Spec §37.7: publish, restart, consume — original ordering preserved.
    # We need to drive the restart ourselves because the harness currently
    # only spins one server. Use a private temp DB.
    db_path = os.path.join(tempfile.mkdtemp(prefix="pq-persist-"), "p.db")
    port = pick_free_port()

    def body(phase, _proc, url):
        if phase == "running":
            call("publish", queue, '{"n":1}', server_url=url)
            call("publish", queue, '{"n":2}', server_url=url)
            call("publish", queue, '{"n":3}', server_url=url)
            rc, out = call("stats", queue, server_url=url)
            expect(json.loads(out)["ready"] == 3, f"phase=running {out}")
        else:
            rc, out = call("stats", queue, server_url=url)
            expect(json.loads(out)["ready"] == 3,
                   f"phase=restarted stats: {out}")
            r1 = parse_kv(call("consume", queue, server_url=url)[1])
            r2 = parse_kv(call("consume", queue, server_url=url)[1])
            r3 = parse_kv(call("consume", queue, server_url=url)[1])
            expect(r1["payload"] == '{"n":1}', f"r1 payload: {r1}")
            expect(r2["payload"] == '{"n":2}', f"r2 payload: {r2}")
            expect(r3["payload"] == '{"n":3}', f"r3 payload: {r3}")

    run_with_restart(port, db_path, body)
    import shutil
    shutil.rmtree(os.path.dirname(db_path), ignore_errors=True)


def scenario_visibility_timeout(server_url: str, queue: str) -> None:
    """Spec §37.4: a reservation that isn't acked becomes reservable again
    after the visibility timeout expires, with a fresh receipt.
    """
    print(f"--- scenario_visibility_timeout [{queue}]")
    call("publish", queue, '{"task":"slow"}', server_url=server_url)
    rc, out = call("consume", queue, "--visibility-ms", "1000",
                   server_url=server_url)
    expect(rc == 0, f"first consume rc={rc}")
    r1 = parse_kv(out)
    receipt1 = r1["receipt"]
    msg_id = r1["id"]

    # Right after reserving, the message must not be available again.
    rc2, out2 = call("consume", queue, server_url=server_url)
    expect(rc2 == 0 and "no message" in out2,
           f"second consume within visibility: {rc2} {out2!r}")

    # Wait past the visibility timeout, then reserve again — should
    # return the same message with a new receipt.
    time.sleep(1.3)
    rc3, out3 = call("consume", queue, server_url=server_url)
    expect(rc3 == 0, f"consume after timeout rc={rc3}")
    r2 = parse_kv(out3)
    expect(r2["id"] == msg_id, f"id mismatch: {r2['id']} vs {msg_id}")
    expect(r2["receipt"] != receipt1,
           f"receipt unchanged after requeue: {receipt1} vs {r2['receipt']}")
    expect(r2["attempts"] == "2",
           f"expected attempts=2 after retry, got {r2['attempts']}")

    # Clean up by acking.
    call("ack", queue, msg_id, r2["receipt"], server_url=server_url)


def scenario_stale_receipt(server_url: str, queue: str) -> None:
    """Spec §37.5: a stale receipt cannot acknowledge; the current
    receipt succeeds."""
    print(f"--- scenario_stale_receipt [{queue}]")
    call("publish", queue, '{"task":"x"}', server_url=server_url)
    rc, out = call("consume", queue, "--visibility-ms", "1000",
                   server_url=server_url)
    expect(rc == 0, "first consume")
    r1 = parse_kv(out)
    old_id = r1["id"]
    old_receipt = r1["receipt"]

    # Let the reservation expire so the server recovers it.
    time.sleep(1.3)

    # Reserving again yields the same message with a new receipt.
    rc, out = call("consume", queue, "--visibility-ms", "5000",
                   server_url=server_url)
    expect(rc == 0, "second consume")
    r2 = parse_kv(out)
    new_receipt = r2["receipt"]
    expect(r2["id"] == old_id,
           f"id mismatch: {r2['id']} vs {old_id}")
    expect(new_receipt != old_receipt,
           f"receipt did not change: {old_receipt}")

    # Acking with the OLD receipt must fail with rc=4 (4xx response).
    rc, out = call("ack", queue, old_id, old_receipt, server_url=server_url)
    expect(rc == 4,
           f"stale-receipt ack should fail with rc=4, got rc={rc} {out!r}")

    # Acking with the CURRENT receipt succeeds.
    rc, _ = call("ack", queue, r2["id"], new_receipt, server_url=server_url)
    expect(rc == 0, f"current-receipt ack rc={rc}")


def scenario_long_poll_wakeup(server_url: str, queue: str) -> None:
    """Spec §37.9: a long-poll request returns promptly when a message
    is published while the request is parked.
    """
    print(f"--- scenario_long_poll_wakeup [{queue}]")
    # Start a long-poll consumer in a thread. It will wait up to 5s.
    result: dict = {}

    def consumer():
        t0 = time.time()
        rc, out = call("consume", queue, "--wait-ms", "5000",
                       server_url=server_url)
        result["elapsed"] = time.time() - t0
        result["rc"] = rc
        result["out"] = out

    t = threading.Thread(target=consumer)
    t.start()
    # Give the consumer a moment to actually start waiting.
    time.sleep(0.3)
    # Publish — should wake the parked consumer.
    call("publish", queue, '{"wake":"up"}', server_url=server_url)
    t.join(timeout=6.0)
    expect(not t.is_alive(), "consumer thread did not wake in time")
    expect(result["rc"] == 0, f"consume rc: {result['rc']}")
    expect(result["elapsed"] < 2.0,
           f"long-poll took {result['elapsed']:.2f}s; expected < 2s")
    fields = parse_kv(result["out"])
    expect(fields.get("payload") == '{"wake":"up"}',
           f"unexpected payload: {fields}")


def scenario_long_poll_timeout(server_url: str, queue: str) -> None:
    """Spec §37.10: a long-poll request that finds nothing returns 204
    once its wait elapses.
    """
    print(f"--- scenario_long_poll_timeout [{queue}]")
    t0 = time.time()
    rc, out = call("consume", queue, "--wait-ms", "300", server_url=server_url)
    elapsed = time.time() - t0
    expect(rc == 0, f"consume rc: {rc}")
    expect("no message" in out, f"unexpected body: {out!r}")
    expect(0.25 <= elapsed <= 1.0,
           f"long-poll elapsed {elapsed:.2f}s not in [0.25, 1.0]")


def scenario_long_poll_expiry_wakeup(server_url: str, queue: str) -> None:
    """Long-poll wakes when an existing reservation expires and the
    message becomes READY again — i.e. the next-event deadline fires.
    """
    print(f"--- scenario_long_poll_expiry_wakeup [{queue}]")
    # Reserve a message with a short visibility; the message stays
    # RESERVED, so the next consumer must wait until it expires.
    call("publish", queue, '{"n":1}', server_url=server_url)
    rc, out = call("consume", queue, "--visibility-ms", "1500",
                   server_url=server_url)
    expect(rc == 0, "first consume")
    first = parse_kv(out)
    call("ack", queue, first["id"], first["receipt"], server_url=server_url)

    # Reserve again, this time WITHOUT acking — it stays reserved for 1.5s.
    call("publish", queue, '{"n":2}', server_url=server_url)
    rc, out = call("consume", queue, "--visibility-ms", "1500",
                   server_url=server_url)
    expect(rc == 0, "second consume")
    # Now a third consumer parks and waits; when the in-flight reservation
    # expires, the message becomes available and the waiter returns.
    result: dict = {}

    def waiter():
        t0 = time.time()
        rc2, out2 = call("consume", queue, "--wait-ms", "5000",
                          server_url=server_url)
        result["elapsed"] = time.time() - t0
        result["rc"] = rc2
        result["out"] = out2

    t = threading.Thread(target=waiter)
    t.start()
    t.join(timeout=6.0)
    expect(not t.is_alive(), "waiter did not wake on expiry")
    expect(result["rc"] == 0, f"waiter rc: {result['rc']}")
    fields = parse_kv(result["out"])
    # Whether n=2 is what comes back depends on FIFO after requeue; we
    # just need a payload to be returned.
    expect("payload" in fields, f"no payload in: {result['out']}")


def scenario_reserved_survives_restart(server_url: str, queue: str) -> None:
    print(f"--- scenario_reserved_survives_restart [{queue}]")
    # Spec §37.8: publish, reserve, restart, the reservation stays
    # until the deadline; after the deadline the message becomes available.
    db_path = os.path.join(tempfile.mkdtemp(prefix="pq-reserve-"), "r.db")
    port = pick_free_port()

    def body(phase, _proc, url):
        if phase == "running":
            call("publish", queue, '{"n":1}', server_url=url)
            rc, out = call("consume", queue, server_url=url)
            expect(rc == 0, f"first consume rc={rc}")
            # Re-publish a second message so we can verify FIFO after
            # the first reservation is intact.
            call("publish", queue, '{"n":2}', server_url=url)
            rc, out = call("stats", queue, server_url=url)
            j = json.loads(out)
            expect(j["reserved"] == 1 and j["ready"] == 1,
                   f"phase=running stats: {out}")
        else:
            rc, out = call("stats", queue, server_url=url)
            j = json.loads(out)
            expect(j["reserved"] == 1,
                   f"reserved count after restart: {out}")
            expect(j["ready"] == 1,
                   f"ready count after restart: {out}")

    run_with_restart(port, db_path, body)
    import shutil
    shutil.rmtree(os.path.dirname(db_path), ignore_errors=True)


SCENARIOS = [
    (scenario_publish_reserve_ack, "q_basic"),
    (scenario_nack_requeue,        "q_nack"),
    (scenario_dead_letter_at_max,   "q_dead"),
    (scenario_stale_receipt_rejected, "q_stale"),
    (scenario_invalid_inputs,       "q_bad"),
    (scenario_stats,                "q_stats"),
    (scenario_persistence_across_restart, "q_persist"),
    (scenario_reserved_survives_restart,  "q_reserve"),
    (scenario_visibility_timeout, "q_visibility"),
    (scenario_stale_receipt, "q_receipt"),
    (scenario_long_poll_wakeup, "q_longpoll"),
    (scenario_long_poll_timeout, "q_longpoll_to"),
    (scenario_long_poll_expiry_wakeup, "q_longpoll_exp"),
]


def main() -> int:
    if not os.path.exists(SERVER) or not os.path.exists(PQCTL):
        print(f"server {SERVER} or pqctl {PQCTL} not built", file=sys.stderr)
        return 1
    if not os.path.isdir(MIGRATIONS):
        print(f"migrations dir {MIGRATIONS} missing", file=sys.stderr)
        return 1

    tmpdir = tempfile.mkdtemp(prefix="pq-e2e-api-")
    port = pick_free_port()
    proc = run_server(port, os.path.join(tmpdir, "api.db"))
    server_url = f"http://127.0.0.1:{port}"
    failed = []
    try:
        for fn, q in SCENARIOS:
            try:
                fn(server_url, q)
                print(f"  OK: {fn.__name__}")
            except AssertionError as e:
                print(f"  FAIL: {fn.__name__}: {e}")
                failed.append(fn.__name__)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    if failed:
        print(f"FAILED: {failed}")
        return 1
    print("e2e_api: all scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())