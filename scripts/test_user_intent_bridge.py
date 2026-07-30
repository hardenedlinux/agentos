#!/usr/bin/env python3
"""
Suite-ADR-001 / test_user_intent_bridge.py
"""
import os
ADMIN_KEY = os.environ.get("AGENTOS_ACCESS_KEY", "")  # ADR-020: top-level "key" field on every JSON-RPC request, not inside params

"""
Standalone stand-in for NekosenseCMS's side of the two-hop clarification
flow (§A), since the CMS bridge isn't ready yet. Talks to a running AgentOS
daemon exactly the way test_mnemos_dual_track.py does: a raw ZMQ DEALER
socket against ~/.agentos/run/agentos.sock, hand-rolled JSON-RPC 2.0.

Flow exercised:
  1. job.submit a deliberately vague goal (no continuation_id).
  2. Poll job.status until phase == "done".
  3. Read result_json — assert needs_clarification is present, print the
     question (+ options, if any).
  4. If result_json also carries a bridge_hint, read the file it points to
     (relative to that job's own output dir) to get the continuation_id.
  5. job.submit again: goal = original goal + a scripted "answer", plus
     continuation_id from step 4.
  6. Poll job.status again, assert THIS time it's a real Plan (has
     "steps"), not another clarification — enforces the one-round rule
     from the outside, not just trusting skill.md's own discipline.

ASSUMPTIONS THAT NEED CONFIRMING AGAINST THE ACTUAL RPC HANDLERS
(cmd_job_submit / cmd_job_status in orchestrator.cpp) BEFORE THIS RUNS:
  - job.submit params: {"goal": "<str>", "continuation_id": "<str, optional>"}
    (continuation_id at the top level, per Suite-ADR-001 §A's own text:
    "...and continuation_id = the value from this job's bridge_hint" — but
    I have not read cmd_job_submit's actual param parsing to confirm the
    key name is literally "continuation_id" and not e.g. nested or
    differently cased.)
  - job.status params: {"job_id": "<str>"} and response has "phase" and
    "result_json" at the top level of the result object (matches the
    cmd_job_status code read earlier in this session).
  - Socket path: ~/.agentos/run/agentos.sock (matches the Mnemos test's
    own connection target, per project memory).
  - AgentOS host filesystem is directly readable from wherever this script
    runs (true for same-machine dev; this mirrors the "CMS co-located with
    AgentOS" deployment model already assumed elsewhere in this project —
    NOT valid for a genuinely remote bridge, but that's out of scope here).

If any of these turn out wrong, the fix is almost certainly just editing
JOB_SUBMIT_PARAMS / JOB_STATUS_PARAMS / the response field names below —
the polling/assertion structure itself shouldn't need to change.
"""

import json
import sys
import time
import uuid
from pathlib import Path

import zmq

SOCKET_PATH = os.path.expanduser("~/.agentos/run/agentos.sock")
SOCKET_ADDR = f"ipc://{SOCKET_PATH}"
AGENTOS_HOME = Path(os.environ.get("AGENTOS_HOME", os.path.expanduser("~/.agentos")))

POLL_INTERVAL_S = 0.5
POLL_TIMEOUT_S = 60

# --- scripted scenario ---------------------------------------------------
# A deliberately vague goal: no format, no target file, no clear domain —
# should land User Intent Adviser in the Step-1 candidate set (broad
# "help"/"goal"/"general" tokens) and should NOT be answerable as a Plan
# without asking something first (unless your skill.md decides otherwise —
# if it doesn't ask, that's useful signal too, just re-check the prompt).
VAGUE_GOAL = "I need some help getting this done, not sure where to start"

# The scripted "answer" appended on turn 2. Whatever your skill.md actually
# asks, this needs to plausibly answer it — edit this once you see what
# question comes back on the first run.
SCRIPTED_ANSWER = "I want to translate a product description into Spanish and French"


class RpcClient:
    def __init__(self, addr: str):
        self.ctx = zmq.Context()
        self.sock = self.ctx.socket(zmq.DEALER)
        self.sock.setsockopt(zmq.IDENTITY, f"bridge-test-{uuid.uuid4().hex[:8]}".encode())
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

        raw = self.sock.recv_string()
        resp = json.loads(raw)
        if resp.get("id") != req_id:
            raise RuntimeError(f"id mismatch: sent {req_id}, got {resp.get('id')}")
        if "error" in resp and resp["error"] is not None:
            raise RuntimeError(f"{method} RPC error: {resp['error']}")
        return resp.get("result", {})

    def close(self):
        self.sock.close()
        self.ctx.term()


def submit_job(client: RpcClient, goal: str, continuation_id: str | None = None) -> str:
    params = {"goal": goal}
    if continuation_id:
        params["continuation_id"] = continuation_id
    result = client.call("job.submit", params)
    job_id = result.get("job_id")
    if not job_id:
        raise RuntimeError(f"job.submit did not return job_id: {result}")
    return job_id


def poll_until_done(client: RpcClient, job_id: str) -> dict:
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


def read_continuation_id_from_bridge_hint(job_id: str, bridge_hint: dict) -> str:
    """Mirrors what NekosenseCMS will eventually do: bridge_hint.path is
    relative to this job's own output dir, never an absolute path (the
    orchestrator validates this server-side and drops anything that
    resolves outside that directory)."""
    output_dir = AGENTOS_HOME / "jobs" / job_id / "output"
    hint_path = output_dir / bridge_hint["path"]
    if not hint_path.is_file():
        raise FileNotFoundError(f"bridge_hint points to {hint_path}, which doesn't exist")
    with open(hint_path) as f:
        hint_content = json.load(f)
    cid = hint_content.get("continuation_id")
    if not cid:
        raise RuntimeError(f"{hint_path} exists but has no continuation_id: {hint_content}")
    return cid


def main():
    client = RpcClient(SOCKET_ADDR)
    try:
        # --- Turn 1: vague goal, expect a clarification ------------------
        print(f"Turn 1: submitting vague goal: {VAGUE_GOAL!r}")
        job_id_1 = submit_job(client, VAGUE_GOAL)
        status_1 = poll_until_done(client, job_id_1)

        if status_1.get("phase") == "failed":
            print(f"FAIL: turn 1 job failed: {status_1}")
            sys.exit(1)

        result_json_raw = status_1.get("result_json")
        if not result_json_raw:
            print(f"FAIL: turn 1 job is done but has no result_json at all: {status_1}")
            sys.exit(1)

        result_1 = json.loads(result_json_raw)
        print(f"  result_json: {json.dumps(result_1, indent=2)}")

        if not result_1.get("needs_clarification"):
            print(
                "NOTE: turn 1 did not ask for clarification — either your "
                "skill.md decided it had enough to plan directly (check "
                "'steps' below), or the vague goal wasn't vague enough to "
                "trigger Shape 2. This isn't necessarily a failure, but the "
                "two-hop flow below won't run."
            )
            print(f"  steps (if any): {result_1.get('steps')}")
            return

        question = result_1.get("clarification_question")
        options = result_1.get("clarification_options")
        print(f"  Adviser asked: {question!r}")
        if options:
            print(f"  Options: {options}")

        # --- Extract continuation_id via bridge_hint, if present ---------
        continuation_id = None
        bridge_hint = status_1.get("bridge_hint")
        if bridge_hint:
            print(f"  bridge_hint: {bridge_hint}")
            continuation_id = read_continuation_id_from_bridge_hint(job_id_1, bridge_hint)
            print(f"  continuation_id: {continuation_id}")
        else:
            print(
                "  WARNING: no bridge_hint on turn 1's job.status response — "
                "did the adviser's manifest.toml set [continuation] supports "
                "= true, and did the response include updated_context? "
                "Continuing without a continuation_id; turn 2 will rely on "
                "goal text alone."
            )

        # --- Turn 2: answer the question ---------------------------------
        combined_goal = f"{VAGUE_GOAL}\n\nAnswer: {SCRIPTED_ANSWER}"
        print(f"\nTurn 2: submitting answer (continuation_id={continuation_id})")
        job_id_2 = submit_job(client, combined_goal, continuation_id=continuation_id)
        status_2 = poll_until_done(client, job_id_2)

        if status_2.get("phase") == "failed":
            print(f"FAIL: turn 2 job failed: {status_2}")
            sys.exit(1)

        result_json_raw_2 = status_2.get("result_json")
        result_2 = json.loads(result_json_raw_2) if result_json_raw_2 else {}
        print(f"  result_json: {json.dumps(result_2, indent=2)}")

        # --- Enforce the one-round rule from the outside ------------------
        if result_2.get("needs_clarification"):
            print(
                "FAIL: turn 2 asked for clarification AGAIN — the "
                "one-clarification-round rule (skill.md discipline, not "
                "protocol-enforced) was violated."
            )
            sys.exit(1)

        if "steps" not in result_2 and "steps" not in status_2:
            print(
                "FAIL: turn 2 produced neither a clarification nor anything "
                "resembling a Plan. Check the raw job.status response above."
            )
            sys.exit(1)

        print("\nPASS: two-hop clarification flow completed — turn 2 produced a Plan.")

    finally:
        client.close()


if __name__ == "__main__":
    main()
