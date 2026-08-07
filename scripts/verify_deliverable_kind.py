"""
verify_deliverable_kind.py
===========================

End-to-end verification for ADR-012 (Digest Pass / deliverable_kind) +
ADR-031 §12 (Forge post-promotion dispatch) + ADR-037/039 (generated_code
bridge_hint) — the actual "does 'write me a monad in Python' come back as
code, not a demo execution result" check.

This is NOT a unit test. It talks to your real, running AgentOS daemon
over the same JSON-RPC/ZMQ path the chat prototype used, submits one real
job, and inspects the real job.status response.

Setup (same as agentos_chat_prototype.py):
    pip install pyzmq
    export AGENTOS_ACCESS_KEY="<your key>"
    export AGENTOS_SOCKET="~/.agentos/run/agentos.sock"   # optional, this is the default

Usage:
    python verify_deliverable_kind.py
    python verify_deliverable_kind.py --goal "write a quicksort implementation in python"
    python verify_deliverable_kind.py --poll-timeout 180
"""

import argparse
import json
import os
import sys
import time
import uuid

import zmq

SOCKET_PATH = os.path.expanduser(
    os.environ.get("AGENTOS_SOCKET", "~/.agentos/run/agentos.sock")
)
ACCESS_KEY = os.environ.get("AGENTOS_ACCESS_KEY")

DEFAULT_GOAL = "write a monad implementation in Python"


def fail(msg):
    print(f"\n\033[31mFAIL\033[0m: {msg}")
    sys.exit(1)


def ok(msg):
    print(f"\033[32mOK\033[0m:   {msg}")


def warn(msg):
    print(f"\033[33mWARN\033[0m: {msg}")


def info(msg):
    print(f"      {msg}")


def make_socket(ctx):
    sock = ctx.socket(zmq.DEALER)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(f"ipc://{SOCKET_PATH}")
    return sock


def rpc_call(sock, method, params, timeout_ms=5000):
    req_id = str(uuid.uuid4())
    msg = {
        "jsonrpc": "2.0",
        "id": req_id,
        "key": ACCESS_KEY,
        "method": method,
        "params": params,
    }
    sock.send_string(json.dumps(msg))

    poller = zmq.Poller()
    poller.register(sock, zmq.POLLIN)
    deadline = time.time() + timeout_ms / 1000

    while time.time() < deadline:
        remaining_ms = max(0, int((deadline - time.time()) * 1000))
        events = dict(poller.poll(remaining_ms))
        if sock in events:
            raw = sock.recv_string()
            reply = json.loads(raw)
            if reply.get("id") == req_id:
                return reply
            continue  # unsolicited notification, not our reply — keep waiting

    raise TimeoutError(f"AgentOS did not reply to {method} within {timeout_ms}ms")


def parse_result_json(raw_result_json):
    """result_json is a JSON-encoded string on the wire (ADR-037's opacity
    design) — parse it once, same as the chat prototype does."""
    if raw_result_json is None:
        return {}
    if isinstance(raw_result_json, dict):
        return raw_result_json  # already parsed, tolerate either shape
    try:
        return json.loads(raw_result_json) if raw_result_json else {}
    except json.JSONDecodeError:
        warn(f"result_json did not parse as JSON: {raw_result_json[:200]!r}")
        return {}


def clean_events_dir():
    """Delete leftover event-mailbox files under agentos_home()/events/.

    This mailbox (job.phase_changed/job.step_changed, persisted alongside
    the live broadcast) is meant to be drained and deleted by a real
    Bridge. This script has no Bridge running behind it, so nothing ever
    consumes these files when testing AgentOS standalone — they just pile
    up and can make it confusing to reason about "did THIS run actually
    fire a notification" once several runs' leftovers are mixed together.
    Not an AgentOS concern; purely test hygiene for this script.
    """
    home = os.path.expanduser(os.environ.get("AGENTOS_HOME", "~/.agentos"))
    events_dir = os.path.join(home, "events")
    if not os.path.isdir(events_dir):
        return
    removed = 0
    for name in os.listdir(events_dir):
        path = os.path.join(events_dir, name)
        try:
            if os.path.isfile(path):
                os.remove(path)
                removed += 1
        except OSError:
            pass
    if removed:
        info(f"cleaned {removed} leftover event file(s) from {events_dir}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goal", default=DEFAULT_GOAL,
                         help=f"job goal to submit (default: {DEFAULT_GOAL!r})")
    parser.add_argument("--poll-interval", type=float, default=2.0)
    parser.add_argument("--poll-timeout", type=float, default=180.0,
                         help="max seconds to wait for the job to finish "
                              "(Forge generation can be slow — default 180s)")
    parser.add_argument("--no-clean-events", action="store_true",
                         help="skip clearing agentos_home()/events/ before "
                              "running (default: clear it, since nothing "
                              "else consumes it in this standalone script)")
    args = parser.parse_args()

    if not ACCESS_KEY:
        fail("AGENTOS_ACCESS_KEY is not set. Run `agentos key generate` "
             "and export it first.")

    if not args.no_clean_events:
        clean_events_dir()

    print(f"Submitting goal: {args.goal!r}")
    print(f"Socket: {SOCKET_PATH}\n")

    ctx = zmq.Context.instance()
    sock = make_socket(ctx)

    try:
        submit_reply = rpc_call(sock, "job.submit", {"goal": args.goal})
    except TimeoutError as e:
        fail(str(e))

    if "error" in submit_reply:
        fail(f"job.submit returned an error: {submit_reply['error']}")

    job_id = (submit_reply.get("result") or {}).get("job_id")
    if not job_id:
        fail(f"job.submit reply had no job_id — raw reply: {submit_reply}")
    ok(f"job submitted: {job_id}")

    # ------------------------------------------------------------------
    # Poll job.status until phase is done/failed
    # ------------------------------------------------------------------
    deadline = time.time() + args.poll_timeout
    status = None
    last_phase = None
    while time.time() < deadline:
        try:
            reply = rpc_call(sock, "job.status", {"job_id": job_id})
        except TimeoutError as e:
            fail(str(e))
        if "error" in reply:
            fail(f"job.status returned an error: {reply['error']}")
        status = reply.get("result") or {}
        phase = status.get("phase")
        if phase != last_phase:
            info(f"phase: {phase}")
            last_phase = phase
        if phase in ("done", "failed"):
            break
        time.sleep(args.poll_interval)
    else:
        fail(f"job {job_id} did not reach done/failed within "
             f"{args.poll_timeout}s (last phase: {last_phase})")

    if status.get("phase") == "failed":
        fail(f"job failed: {status.get('error', '(no error field)')}")

    print()  # blank line before the actual checks

    # ------------------------------------------------------------------
    # Check 1: deliverable_kind
    # ------------------------------------------------------------------
    deliverable_kind = status.get("deliverable_kind")
    if deliverable_kind is None:
        fail("job.status response has no 'deliverable_kind' field at all — "
             "check that cmd_job_status was actually rebuilt with the "
             "ADR-012/039 changes (this field should always be present, "
             "defaulting to 'result').")
    info(f"deliverable_kind = {deliverable_kind!r}")

    if deliverable_kind == "result":
        warn("Digest Pass classified this as 'result', not 'artifact'. "
             "This means the LLM classification itself didn't recognize "
             "a code-writing request as wanting the artifact — this is a "
             "prompt-quality issue in run_digest_pass's system prompt, not "
             "a wiring bug. The rest of this script's checks (marker "
             "result_json, generated_code hint) will legitimately fail "
             "below, because none of that machinery should fire when "
             "deliverable_kind is 'result'.")
    elif deliverable_kind != "artifact":
        warn(f"deliverable_kind is an unexpected value: {deliverable_kind!r} "
             "(expected 'artifact' or 'result')")
    else:
        ok("deliverable_kind == 'artifact'")

    # ------------------------------------------------------------------
    # Check 2: result_json shape
    # ------------------------------------------------------------------
    raw_result_json = status.get("result_json")
    result = parse_result_json(raw_result_json)
    info(f"result_json (parsed) = {json.dumps(result, ensure_ascii=False)}")

    if deliverable_kind == "artifact":
        if result.get("forge_generated") is True and "agent_id" in result:
            ok(f"result_json is the expected marker "
               f"(agent_id={result['agent_id']!r}), not an execution demo")
        else:
            fail("deliverable_kind is 'artifact' but result_json is NOT the "
                 "expected {'forge_generated': true, 'agent_id': ...} marker "
                 "— check Orchestrator's forge_complete handler (ADR-031 §12) "
                 "actually took the artifact branch instead of falling "
                 "through to dispatch_next_step.")

    # ------------------------------------------------------------------
    # Check 3: generated_code bridge_hint
    # ------------------------------------------------------------------
    bridge_hint = status.get("bridge_hint")
    if deliverable_kind == "artifact":
        if not bridge_hint:
            fail("deliverable_kind is 'artifact' but no bridge_hint was "
                 "returned at all. Check cmd_job_status's bridge_hint "
                 "construction picked up the marker result_json's "
                 "bridge_hint field, and that the generated_code carve-out "
                 "(skipping the job-output-dir containment check) is in "
                 "place.")
        elif bridge_hint.get("key") != "generated_code":
            fail(f"bridge_hint present but key is {bridge_hint.get('key')!r}, "
                 "expected 'generated_code'")
        else:
            path = bridge_hint.get("path")
            info(f"bridge_hint.path = {path}")
            if not path or not os.path.isfile(path):
                fail(f"bridge_hint.path does not point to an existing file: "
                     f"{path!r} — check the promoted worker's actual on-disk "
                     f"location matches agentos_home()/workers/<agent_id>/"
                     f"worker_impl.py")
            else:
                ok("bridge_hint.path exists on disk")
                with open(path, "r", errors="replace") as f:
                    content = f.read()
                print("\n----- generated_code content -----")
                print(content[:2000] + ("... (truncated)" if len(content) > 2000 else ""))
                print("----- end -----\n")
                if content.strip():
                    ok("generated_code file is non-empty — this is the "
                       "actual fix for the original 'monad demo output' bug")
                else:
                    fail("generated_code file exists but is EMPTY")

    print("\n" + "=" * 60)
    if deliverable_kind == "artifact" and bridge_hint and bridge_hint.get("key") == "generated_code":
        print("RESULT: the deliverable_kind → generated_code pipeline worked end-to-end.")
    elif deliverable_kind == "result":
        print("RESULT: pipeline is wired correctly, but Digest Pass's classification "
              "did not mark this request as 'artifact'. Re-run with a more explicit "
              "goal (e.g. 'write me the source code for X, do not run it') and/or "
              "revisit run_digest_pass's system prompt in master.cpp.")
    else:
        print("RESULT: something is broken — see FAIL/WARN lines above.")
    print("=" * 60)


if __name__ == "__main__":
    main()
