#!/usr/bin/env python3
"""
Manual end-to-end test for ADR-035's inferred/attested write-provenance
path. Talks directly to the daemon's ZMQ ROUTER socket — no CLI wrapper
assumed, since subject.memory.* has no CLI command group yet.

Requires: pip install pyzmq --break-system-packages

Fill in ADMIN_KEY below with a real admin-role access key from your
running daemon (access_keys table / whatever you used with `agentos`
CLI to bootstrap). SOCKET_PATH defaults to the standard
~/.agentos/run/agentos.sock — override via AGENTOS_HOME env var if
your daemon uses a non-default home.
"""
import json
import os
import sys
import uuid
import zmq
import os

ADMIN_KEY = os.environ.get("AGENTOS_ACCESS_KEY", "")  # ADR-020: top-level "key" field on every JSON-RPC request, not inside params

home = os.environ.get("AGENTOS_HOME", os.path.expanduser("~/.agentos"))
SOCKET_PATH = f"ipc://{home}/run/agentos.sock"

ctx = zmq.Context()
sock = ctx.socket(zmq.DEALER)
sock.setsockopt(zmq.RCVTIMEO, 5000)  # matches the daemon's own 5000ms CLI-timeout convention
sock.setsockopt(zmq.LINGER, 0)
sock.connect(SOCKET_PATH)


def call(method: str, params: dict) -> dict:
    req_id = str(uuid.uuid4())
    msg = {"jsonrpc": "2.0", "id": req_id, "method": method, "key": ADMIN_KEY, "params": params}
    sock.send_string(json.dumps(msg))
    try:
        raw = sock.recv_string()
    except zmq.error.Again:
        print(f"  [TIMEOUT waiting for reply to {method}]")
        sys.exit(2)
    resp = json.loads(raw)
    print(f"  -> {json.dumps(resp)}")
    return resp


def expect(label: str, resp: dict, want_error_code: int | None):
    got_error = resp.get("error", {}).get("code") if "error" in resp else None
    if want_error_code is None:
        ok = "error" not in resp
    else:
        ok = got_error == want_error_code
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {label} (expected error={want_error_code}, got={got_error})\n")
    return ok


print("1. subject.register")
resp = call("subject.register", {"subject_type": "mnemos-test", "unit_type": "file"})
subject_id = resp.get("result", {}).get("subject_id")
if not subject_id:
    print("Could not create subject — check ADMIN_KEY and daemon connectivity. Aborting.")
    sys.exit(1)
print(f"  subject_id = {subject_id}\n")

print("2. subject.memory.write_policy.upsert (register the compliance prefix)")
resp = call("subject.memory.write_policy.upsert", {
    "entry_key_prefix": "certification:",
    "authorized_signers": ["compliance-adviser@v1"],
})
expect("policy registered", resp, None)

print("3. subject.memory.upsert — attested, correct signed_off_by (should SUCCEED)")
resp = call("subject.memory.upsert", {
    "subject_id": subject_id,
    "entry_key": "certification:CE",
    "entry_value": {"status": "verified", "note": "manual test"},
    "track": "attested",
    "signed_off_by": "compliance-adviser@v1",
    "source_job_id": "manual-test-job",
})
expect("attested write with authorized signed_off_by", resp, None)

print("4. subject.memory.upsert — attested, WRONG signed_off_by (should be REJECTED, -32032)")
resp = call("subject.memory.upsert", {
    "subject_id": subject_id,
    "entry_key": "certification:CE",
    "entry_value": {"status": "verified", "note": "should not land"},
    "track": "attested",
    "signed_off_by": "some-random-adviser",
    "source_job_id": "manual-test-job",
})
expect("attested write with unauthorized signed_off_by", resp, -32032)

print("5. subject.memory.upsert — inferred, no policy involved (should SUCCEED regardless)")
resp = call("subject.memory.upsert", {
    "subject_id": subject_id,
    "entry_key": "certification:CE:inferred",
    "entry_value": {"guess": "looks like it might have CE marking"},
    "source_job_id": "manual-test-job",
    # track omitted on purpose — must default to "inferred"
})
expect("ordinary inferred write (track omitted)", resp, None)

print("6. subject.memory.query — confirm both entries are visible with correct track")
resp = call("subject.memory.query", {"subject_id": subject_id, "key_prefix": "certification:"})
entries = resp.get("result", {}).get("entries", [])
tracks = {e["entry_key"]: e.get("track") for e in entries}
print(f"  entry_key -> track: {tracks}")
ok = tracks.get("certification:CE") == "attested" and tracks.get("certification:CE:inferred") == "inferred"
print(f"[{'PASS' if ok else 'FAIL'}] query returns correct track per entry\n")

print("Done.")
