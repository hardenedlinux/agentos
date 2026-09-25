#!/usr/bin/env python3
# Copyright (C) 2026  HardenedLinux community
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""
plugins/hello-plugin/hello_plugin.py

Example AgentOS plugin written in Python.
Demonstrates the minimal plugin contract:
  1. Connect to the core via ZMQ (REQ socket)
  2. Send plugin.register notification
  3. Handle task.invoke requests
  4. Handle plugin.shutdown notification

No SDK required — just the standard library + pyzmq.
"""

import os
import sys
import json
import logging
import signal
import zmq

logging.basicConfig(
    level=os.environ.get("LOG_LEVEL", "INFO").upper(),
    format="[%(asctime)s] [hello-plugin] [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

PLUGIN_NAME    = "hello-plugin"
PLUGIN_VERSION = "0.1.0"
CAPABILITIES   = ["hello.greet", "hello.farewell", "hello.prompt"]

# ─── Capability handlers ──────────────────────────────────────────────────────

def handle_greet(params: dict) -> dict:
    name = params.get("name", "world")
    return {"message": f"Hello, {name}! Greetings from AgentOS."}

def handle_farewell(params: dict) -> dict:
    name = params.get("name", "world")
    return {"message": f"Goodbye, {name}! Until next time."}

def handle_prompt(params: dict) -> dict:
    """Return a Markdown-formatted prompt."""
    name = params.get("name", "world")
    md = f"""# Hello, {name}!

This is a **Markdown** prompt returned by the hello-plugin.

- It supports **bold**, *italic*, and `code`.
- Lists are easy.
- [Links](https://example.com) work too.

```python
print("Hello, {name}!")
```

> A blockquote for emphasis.

Enjoy!
"""
    return {"message": md}

HANDLERS = {
    "hello.greet":    handle_greet,
    "hello.farewell": handle_farewell,
    "hello.prompt":   handle_prompt,
}

# ─── Main loop ────────────────────────────────────────────────────────────────

def main() -> None:
    endpoint = os.environ.get("AGENTOS_ZMQ_ENDPOINT", "tcp://127.0.0.1:5555")

    log.info("Connecting to AgentOS core at %s", endpoint)

    context = zmq.Context()
    sock = context.socket(zmq.REQ)
    sock.connect(endpoint)
    log.info("Connected.")

    # Step 1: Register with the core
    sock.send_json({
        "jsonrpc": "2.0",
        "method":  "plugin.register",
        "params": {
            "name":         PLUGIN_NAME,
            "version":      PLUGIN_VERSION,
            "capabilities": CAPABILITIES,
        }
    })
    log.info("Sent plugin.register")

    # Step 2: Wait for acknowledgement
    ack = sock.recv_json()
    if not ack or ack.get("result") != "ok":
        log.error("Registration failed: %s", ack)
        sys.exit(1)
    log.info("Registered successfully. Ready.")

    # Step 3: Main message loop
    def _shutdown(signum, frame):
        log.info("Received signal %s, shutting down", signum)
        sock.close()
        context.term()
        sys.exit(0)

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT,  _shutdown)

    while True:
        msg = sock.recv_json()
        if msg is None:
            log.info("Core closed connection, exiting.")
            break

        method = msg.get("method", "")
        msg_id = msg.get("id")

        # Ordered shutdown
        if method == "plugin.shutdown":
            log.info("Received plugin.shutdown, exiting cleanly.")
            break

        # Task invocation
        if method == "task.invoke":
            params     = msg.get("params", {})
            capability = params.get("capability", "")
            input_data = params.get("input", {})

            handler = HANDLERS.get(capability)
            if handler:
                try:
                    result = handler(input_data)
                    sock.send_json({"jsonrpc": "2.0", "id": msg_id, "result": result})
                except Exception as exc:
                    log.exception("Handler error for %s", capability)
                    sock.send_json({
                        "jsonrpc": "2.0", "id": msg_id,
                        "error": {"code": -32000, "message": str(exc)}
                    })
            else:
                sock.send_json({
                    "jsonrpc": "2.0", "id": msg_id,
                    "error": {"code": -32601, "message": f"Unknown capability: {capability}"}
                })
        else:
            log.warning("Unhandled method: %s", method)

    sock.close()
    context.term()
    log.info("Goodbye.")

if __name__ == "__main__":
    main()
