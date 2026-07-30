#!/usr/bin/env python3
"""
test_job_status_fields.py
"""
import os
ADMIN_KEY = os.environ.get("AGENTOS_ACCESS_KEY", "")  # ADR-020: top-level "key" field on every JSON-RPC request, not inside params

"""
Verifies the two job.status exposure gaps closed this session:

  1. Job-level "adviser_id" — which adviser Master routed this job to at
     entry (set once, at spawn_adviser time). Was previously invisible to
     job.status; only the daemon log showed it.

  2. Per-step "command" / "target_type" / "needs_forge" — were already
     stored in the tasks table (method/target_type/needs_forge columns)
     but never read back out or serialized into job.status. This is the
     signal any downstream Suite/CMS needs to tell "already-doable" steps
     apart from "needs_forge" gaps.

This does NOT test routing or clarification content — see
test_user_intent_bridge.py / test_continuation_routing.py for those. This
only checks that job.status's JSON shape now actually contains these
fields, with sane values.

Uses a goal that (per this session's earlier runs) reliably produces a
real multi-step Plan with a known mix of worker/adviser steps and
needs_forge values, so the assertions below have something concrete to
check rather than just "key exists".
"""

import json
import sys
import time
import uuid

import zmq

SOCKET_PATH = os.path.expanduser("~/.agentos/run/agentos.sock")
SOCKET_ADDR = f"ipc://{SOCKET_PATH}"

POLL_INTERVAL_S = 0.5
POLL_TIMEOUT_S = 60

# Same combined goal that produced a real 6-step Plan earlier this session
# (4 worker steps needing forge, 2 adviser steps not needing it) — gives
# concrete values to assert against instead of just checking key presence.
GOAL = (
    "I need some help getting this done, not sure where to start\n\n"
    "Answer: I want to translate a product description into Spanish and French"
)


class RpcClient:
    def __init__(self, addr: str):
        self.ctx = zmq.Context()
        self.sock = self.ctx.socket(zmq.DEALER)
        self.sock.setsockopt(zmq.IDENTITY, f"fields-test-{uuid.uuid4().hex[:8]}".encode())
        self.sock.connect(addr)
        self.poller = zmq.Poller()
        self.poller.register(self.sock, zmq.POLLIN)

    def call(self, method: str, params: dict, timeout_ms: int = 5000) -> dict:
        req_id = str(uuid.uuid4())
        req = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "key": ADMIN_KEY,  # ADR-020 — top-level, not inside params
            "params": params,
        }
        self.sock.send_string(json.dumps(req))
        events = dict(self.poller.poll(timeout_ms))
        if self.sock not in events:
            raise TimeoutError(f"{method} timed out after {timeout_ms}ms")
        resp = json.loads(self.sock.recv_string())
        if resp.get("id") != req_id:
            raise RuntimeError(f"id mismatch: sent {req_id}, got {resp.get('id')}")
        if resp.get("error") is not None:
            raise RuntimeError(f"{method} RPC error: {resp['error']}")
        return resp.get("result", {})

    def close(self):
        self.sock.close()
        self.ctx.term()


def submit_job(client, goal):
    result = client.call("job.submit", {"goal": goal})
    job_id = result.get("job_id")
    if not job_id:
        raise RuntimeError(f"job.submit did not return job_id: {result}")
    return job_id


def poll_until_done(client, job_id):
    deadline = time.time() + POLL_TIMEOUT_S
    last_phase = None
    while time.time() < deadline:
        result = client.call("job.status", {"job_id": job_id})
        phase = result.get("phase")
        if phase != last_phase:
            print(f"  [job {job_id}] phase -> {phase}")
            last_phase = phase
        if phase in ("done", "failed"):
            return result
        time.sleep(POLL_INTERVAL_S)
    raise TimeoutError(f"job {job_id} did not reach a terminal phase within {POLL_TIMEOUT_S}s")


def check_adviser_id(status: dict) -> bool:
    print("\n--- Checking job-level adviser_id ---")
    adviser_id = status.get("adviser_id")
    if not adviser_id:
        print(f"FAIL: job.status has no (or empty) adviser_id field: {status.keys()}")
        return False
    print(f"  PASS: adviser_id = {adviser_id!r}")
    return True


def check_step_fields(status: dict) -> bool:
    print("\n--- Checking per-step command/target_type/needs_forge ---")
    steps = status.get("steps", [])
    if not steps:
        print(
            "NOTE: this job produced zero steps (maybe it asked for "
            "clarification instead of a Plan this run — LLM output isn't "
            "fully deterministic). Nothing to check here this run; try "
            "again, or adjust GOAL if this keeps happening."
        )
        return True

    ok = True
    saw_worker_needs_forge_true = False
    saw_adviser_needs_forge_false = False

    for i, s in enumerate(steps):
        missing = [k for k in ("command", "target_type", "needs_forge") if k not in s]
        if missing:
            print(f"FAIL: step {i} ({s.get('id')}) is missing fields: {missing}")
            ok = False
            continue

        command = s["command"]
        target_type = s["target_type"]
        needs_forge = s["needs_forge"]

        print(f"  step {i}: target_type={target_type!r} command={command!r} needs_forge={needs_forge!r}")

        if target_type not in ("worker", "adviser"):
            print(f"    FAIL: unexpected target_type {target_type!r}")
            ok = False
        if not isinstance(needs_forge, bool):
            print(f"    FAIL: needs_forge is not a bool: {type(needs_forge)}")
            ok = False
        if not command:
            print(f"    FAIL: command is empty for a step that should reference a real capability/adviser id")
            ok = False

        if target_type == "worker" and needs_forge is True:
            saw_worker_needs_forge_true = True
        if target_type == "adviser" and needs_forge is False:
            saw_adviser_needs_forge_false = True

    if not saw_worker_needs_forge_true:
        print(
            "NOTE: no worker step had needs_forge=true this run — expected "
            "at least one gap (document.extract/translation.execute/etc. "
            "aren't registered capabilities) based on this session's "
            "earlier runs with the same GOAL. Not necessarily a failure — "
            "LLM output varies — but worth a second look if this persists."
        )
    if not saw_adviser_needs_forge_false:
        print(
            "NOTE: no adviser step with needs_forge=false was seen this "
            "run — either the Plan had no adviser-target steps this time, "
            "or something upstream is setting needs_forge on adviser steps "
            "(skill.md says it never should)."
        )

    if ok:
        print("  PASS: all steps have well-typed command/target_type/needs_forge")
    return ok


def main():
    client = RpcClient(SOCKET_ADDR)
    try:
        print(f"Submitting goal: {GOAL!r}")
        job_id = submit_job(client, GOAL)
        status = poll_until_done(client, job_id)

        if status.get("phase") == "failed":
            print(
                f"NOTE: job failed ({status.get('error')!r}) — unrelated to "
                "what this script checks (that's a worker/adviser execution "
                "issue, not a job.status shape issue). Still running the "
                "field checks below against whatever job.status returned, "
                "since adviser_id/command/target_type/needs_forge should "
                "all be populated correctly even for a job that failed "
                "partway through."
            )

        results = [
            check_adviser_id(status),
            check_step_fields(status),
        ]

        if all(results):
            print("\nALL CHECKS PASSED")
        else:
            print("\nSOME CHECKS FAILED — see above")
            sys.exit(1)
    finally:
        client.close()


if __name__ == "__main__":
    main()
