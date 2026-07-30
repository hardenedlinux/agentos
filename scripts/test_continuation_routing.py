#!/usr/bin/env python3
"""
test_continuation_routing.py
"""
import os
ADMIN_KEY = os.environ.get("AGENTOS_ACCESS_KEY", "")  # ADR-020: top-level "key" field on every JSON-RPC request, not inside params

"""
Verifies the known_adviser_id short-circuit added to cmd_job_submit /
Master::handle_job_submit: a job.submit carrying a valid, unconsumed
continuation_id should route straight to that continuation's owning
adviser — skipping domain-token matching and the Step-2 LLM disambiguation
call entirely — while a missing/stale/unknown continuation_id should fall
back to normal domain selection without erroring.

This does NOT re-test the clarification content itself (see
test_user_intent_bridge.py for that) — it only cares about routing:
did turn 2 actually skip selection, and did a bad continuation_id degrade
gracefully rather than blow up.

THREE SCENARIOS

  1. Real continuation (happy path): drive turn 1 through User Intent
     Adviser to get a genuine continuation_id, then submit turn 2 with it
     and confirm:
       a) the job completes normally
       b) it was materially faster than turn 1 (weak signal — skipping an
          LLM disambiguation round-trip should be a measurable delta, but
          this is a heuristic, not a proof; the log line in scenario 3
          below is the real evidence)
       c) (if AGENTOS_LOG_PATH is set) the daemon log actually contains
          "routed via known continuation owner" for this job_id

  2. Unknown continuation_id: submit a job with a made-up continuation_id
     that was never created. Expect: job still completes (falls back to
     normal domain selection), no RPC error, and — if log access is
     available — a "no unconsumed owner was found" warning for this
     job_id, NOT a hard failure.

  3. Log-based confirmation (best-effort, skipped if AGENTOS_LOG_PATH
     unset or unreadable): greps the daemon log for both the short-circuit
     line (scenario 1) and the fallback line (scenario 2), keyed by job_id
     so this doesn't get confused by other jobs' log lines if the daemon
     has been running a while.
"""

import json
import re
import sys
import time
import uuid
from pathlib import Path

import zmq

SOCKET_PATH = os.path.expanduser("~/.agentos/run/agentos.sock")
SOCKET_ADDR = f"ipc://{SOCKET_PATH}"
AGENTOS_HOME = Path(os.environ.get("AGENTOS_HOME", os.path.expanduser("~/.agentos")))
LOG_PATH = os.environ.get("AGENTOS_LOG_PATH")  # optional

POLL_INTERVAL_S = 0.5
POLL_TIMEOUT_S = 60

# Same vague goal as test_user_intent_bridge.py — reuse turn 1 to get a
# real continuation_id rather than depending on any DB-insertion shortcut.
VAGUE_GOAL = "I need some help getting this done, not sure where to start"
SCRIPTED_ANSWER = "I want to translate a product description into Spanish and French"

# Deliberately never-created — must not exist under any real user.
FAKE_CONTINUATION_ID = f"nonexistent-{uuid.uuid4()}"


class RpcClient:
    def __init__(self, addr: str):
        self.ctx = zmq.Context()
        self.sock = self.ctx.socket(zmq.DEALER)
        self.sock.setsockopt(zmq.IDENTITY, f"routing-test-{uuid.uuid4().hex[:8]}".encode())
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


def submit_job(client, goal, continuation_id=None):
    params = {"goal": goal}
    if continuation_id:
        params["continuation_id"] = continuation_id
    result = client.call("job.submit", params)
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


def get_continuation_id_from_turn1(client) -> tuple[str, str, float]:
    """Runs turn 1 (vague goal -> clarification) and returns
    (job_id, continuation_id, elapsed_seconds)."""
    print(f"[setup] Turn 1: submitting vague goal to get a real continuation_id")
    t0 = time.time()
    job_id = submit_job(client, VAGUE_GOAL)
    status = poll_until_done(client, job_id)
    elapsed = time.time() - t0

    if status.get("phase") == "failed":
        raise RuntimeError(f"turn 1 job failed: {status}")

    result_json_raw = status.get("result_json")
    if not result_json_raw:
        raise RuntimeError(f"turn 1 job done but no result_json: {status}")
    result = json.loads(result_json_raw)

    if not result.get("needs_clarification"):
        raise RuntimeError(
            "turn 1 didn't ask for clarification — can't get a continuation_id "
            "this way. Either adjust VAGUE_GOAL or run test_user_intent_bridge.py "
            "first to confirm Shape 2 triggers at all."
        )

    bridge_hint = status.get("bridge_hint")
    if not bridge_hint:
        raise RuntimeError(
            "turn 1 asked for clarification but produced no bridge_hint — "
            "check [continuation] supports = true in the adviser's manifest.toml "
            "and that its response included updated_context."
        )

    output_dir = AGENTOS_HOME / "jobs" / job_id / "output"
    hint_path = output_dir / bridge_hint["path"]
    with open(hint_path) as f:
        continuation_id = json.load(f)["continuation_id"]

    print(f"  turn 1 took {elapsed:.2f}s, continuation_id={continuation_id}")
    return job_id, continuation_id, elapsed


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def grep_log(job_id: str, needle: str) -> bool:
    if not LOG_PATH or not os.path.isfile(LOG_PATH):
        return False
    pattern = re.compile(re.escape(job_id))
    with open(LOG_PATH, errors="replace") as f:
        for line in f:
            line = _ANSI_RE.sub("", line)
            if pattern.search(line) and needle in line:
                return True
    return False


def scenario_1_real_continuation(client):
    print("\n=== Scenario 1: real continuation should short-circuit routing ===")
    turn1_job_id, continuation_id, turn1_elapsed = get_continuation_id_from_turn1(client)

    combined_goal = f"{VAGUE_GOAL}\n\nAnswer: {SCRIPTED_ANSWER}"
    print(f"Turn 2: submitting with continuation_id={continuation_id}")
    t0 = time.time()
    job_id_2 = submit_job(client, combined_goal, continuation_id=continuation_id)
    status_2 = poll_until_done(client, job_id_2)
    turn2_elapsed = time.time() - t0

    if status_2.get("phase") == "failed":
        print(f"FAIL: turn 2 job failed: {status_2}")
        return False

    print(f"  turn 1: {turn1_elapsed:.2f}s   turn 2: {turn2_elapsed:.2f}s")
    if turn2_elapsed >= turn1_elapsed:
        print(
            "  NOTE: turn 2 wasn't faster than turn 1 — this is a weak "
            "heuristic (network/LLM latency varies), not a failure by "
            "itself. Check the log-based confirmation below if available."
        )

    if grep_log(job_id_2, "routed via known continuation owner"):
        print("  PASS: log confirms short-circuit routing for this job_id")
    elif LOG_PATH:
        print(
            "  FAIL: AGENTOS_LOG_PATH is set but no short-circuit log line "
            f"found for job_id={job_id_2} — routing may have fallen back to "
            "normal domain selection instead of using known_adviser_id."
        )
        return False
    else:
        print("  (AGENTOS_LOG_PATH not set — skipping log confirmation)")

    print("PASS: scenario 1")
    return True


def scenario_2_unknown_continuation(client):
    print("\n=== Scenario 2: unknown continuation_id should fall back gracefully ===")
    print(f"Submitting a fresh goal with a fabricated continuation_id={FAKE_CONTINUATION_ID}")
    job_id = submit_job(client, VAGUE_GOAL, continuation_id=FAKE_CONTINUATION_ID)
    status = poll_until_done(client, job_id)

    if status.get("phase") == "failed":
        print(
            f"FAIL: job failed outright instead of falling back to normal "
            f"domain selection: {status}"
        )
        return False

    print(f"  job completed normally (phase={status.get('phase')}) — no hard failure")

    if grep_log(job_id, "no unconsumed owner was found"):
        print("  PASS: log confirms graceful fallback (peek found nothing) for this job_id")
    elif LOG_PATH:
        print(
            "  NOTE: AGENTOS_LOG_PATH is set but the expected fallback "
            f"warning wasn't found for job_id={job_id} — worth checking "
            "manually, but the job did still complete without erroring."
        )
    else:
        print("  (AGENTOS_LOG_PATH not set — skipping log confirmation)")

    print("PASS: scenario 2")
    return True


def main():
    client = RpcClient(SOCKET_ADDR)
    try:
        results = [
            scenario_1_real_continuation(client),
            scenario_2_unknown_continuation(client),
        ]
        if all(results):
            print("\nALL SCENARIOS PASSED")
        else:
            print("\nSOME SCENARIOS FAILED — see above")
            sys.exit(1)
    finally:
        client.close()


if __name__ == "__main__":
    main()
