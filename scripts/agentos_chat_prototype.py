"""
AgentOS Chat Prototype
======================

A minimal local web UI to chat with AgentOS directly, for manual smoke-testing
before Bridge exists as a real thing. Talks JSON-RPC 2.0 over a ZMQ DEALER
socket against the daemon's ROUTER socket (ADR-020), following the request/
poll/continuation shape described in ADR-039.

This is NOT Bridge. It skips mailbox/heartbeat handling, error-code
translation, adviser_id routing, and multi-user concerns entirely. It exists
only so you can have a first conversation with your own running daemon.

Setup:
    pip install pyzmq gradio

    # Make sure `agentos run &` is already running, and you have an access key:
    #   agentos key generate
    # (admin role is fine for local-only testing)

    export AGENTOS_ACCESS_KEY="<the key you generated>"
    # optional, defaults to ~/.agentos/run/agentos.sock
    export AGENTOS_SOCKET="~/.agentos/run/agentos.sock"

    python agentos_chat_prototype.py

Then open the printed local URL in a browser.
"""

import json
import os
import time
import uuid

import gradio as gr
import zmq

SOCKET_PATH = os.path.expanduser(
    os.environ.get("AGENTOS_SOCKET", "~/.agentos/run/agentos.sock")
)
ACCESS_KEY = os.environ.get("AGENTOS_ACCESS_KEY")

if not ACCESS_KEY:
    raise SystemExit(
        "Set AGENTOS_ACCESS_KEY first (run `agentos key generate` if you don't have one yet)."
    )

_ctx = zmq.Context.instance()


def _make_socket():
    sock = _ctx.socket(zmq.DEALER)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(f"ipc://{SOCKET_PATH}")
    return sock


def _rpc_call(sock, method, params, timeout_ms=5000):
    """Send one JSON-RPC request and wait for the matching reply, ignoring
    unsolicited notifications (heartbeat, job.phase_changed, ...) that may
    arrive on the same socket in the meantime."""
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
            # notification not addressed to this request; keep waiting
            continue

    raise TimeoutError(f"AgentOS did not reply to {method} within {timeout_ms}ms")


def _read_continuation_id(bridge_hint):
    """ADR-039 §C: continuation_id lives in a file on disk, not in the RPC
    reply itself, because Bridge/daemon share a filesystem."""
    if not bridge_hint or not bridge_hint.get("path"):
        return None
    try:
        with open(bridge_hint["path"]) as f:
            return json.load(f).get("continuation_id")
    except OSError:
        return None


def submit_and_wait(goal, continuation_id=None, poll_interval=1.0, poll_timeout_s=120):
    sock = _make_socket()
    try:
        params = {"goal": goal}
        if continuation_id:
            params["continuation_id"] = continuation_id

        reply = _rpc_call(sock, "job.submit", params)
        if "error" in reply:
            return f"[AgentOS error] {reply['error'].get('message')}", None

        job_id = (reply.get("result") or {}).get("job_id")
        if not job_id:
            return f"[unexpected job.submit reply] {reply}", None

        deadline = time.time() + poll_timeout_s
        while time.time() < deadline:
            status_reply = _rpc_call(sock, "job.status", {"job_id": job_id})
            if "error" in status_reply:
                return f"[AgentOS error] {status_reply['error'].get('message')}", None

            result = status_reply.get("result") or {}
            phase = result.get("phase")
            result_json = result.get("result_json") or {}
            if isinstance(result_json, str):
                # daemon may hand this back as a raw JSON string (e.g. TEXT
                # column passed through as-is) rather than a parsed object
                try:
                    result_json = json.loads(result_json) if result_json else {}
                except json.JSONDecodeError:
                    result_json = {}

            if result_json.get("needs_clarification"):
                new_cid = _read_continuation_id(result.get("bridge_hint"))
                text = result_json.get("clarification_question", "(no question text)")
                options = result_json.get("clarification_options")
                if options:
                    text += "\n\nOptions: " + ", ".join(str(o) for o in options)
                return text, new_cid

            if phase == "done":
                return json.dumps(result_json, ensure_ascii=False, indent=2), None

            if phase == "failed":
                # Deliberately not showing result.get("error") verbatim to the
                # user — that's raw internal diagnostic text (ADR-039 §H1).
                return "[job failed — check daemon logs for details]", None

            time.sleep(poll_interval)

        return f"[timeout waiting for job {job_id} to finish]", None
    finally:
        sock.close()


def on_submit(user_message, chat_history, state):
    if not user_message.strip():
        return chat_history, "", state

    continuation_id = (state or {}).get("continuation_id")
    reply_text, new_cid = submit_and_wait(user_message, continuation_id=continuation_id)

    chat_history = chat_history + [
        {"role": "user", "content": user_message},
        {"role": "assistant", "content": reply_text},
    ]
    return chat_history, "", {"continuation_id": new_cid}


with gr.Blocks(title="AgentOS Chat (prototype)") as demo:
    gr.Markdown(
        "# AgentOS Chat — prototype\n"
        "Talks directly to your local `agentos` daemon over its RPC socket. "
        "Not Bridge — no mailbox, no multi-user, no error translation beyond "
        "the basics."
    )
    chatbot = gr.Chatbot(height=500)
    msg = gr.Textbox(label="Message", placeholder="Say something to AgentOS...")
    state = gr.State({})

    msg.submit(on_submit, [msg, chatbot, state], [chatbot, msg, state])

if __name__ == "__main__":
    demo.launch()
