---
name: hello-plugin
version: 0.1.0
executable: ./hello_plugin
capabilities:
  - hello.greet
  - hello.farewell
resources:
  max_memory_mb: 64
  timeout_ms: 5000
env:
  LOG_LEVEL: info
---

# hello-plugin

A minimal example plugin that demonstrates the AgentOS plugin contract.
Can be written in **any language** — this example ships a Python implementation.

## Capabilities

### `hello.greet`

Returns a greeting message for a given name.

**Input:**
```json
{ "name": "string" }
```

**Output:**
```json
{ "message": "string" }
```

### `hello.farewell`

Returns a farewell message for a given name.

**Input:**
```json
{ "name": "string" }
```

**Output:**
```json
{ "message": "string" }
```

## Environment Variables

| Variable    | Default | Description          |
|-------------|---------|----------------------|
| `LOG_LEVEL` | `info`  | Plugin log verbosity |

## Running manually

```bash
AGENTOS_SOCKET=/tmp/agentos.sock ./hello_plugin
```
