<!--
Copyright (C) 2026  HardenedLinux community

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
-->

# hello-plugin

**name:** hello-plugin  
**version:** 0.1.0  
**executable:** `./hello_plugin`  
**capabilities:** `hello.greet`, `hello.farewell`, `hello.prompt`  
**resources:** max_memory_mb: 64, timeout_ms: 5000  
**env:** `LOG_LEVEL` (default `info`)

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

### `hello.prompt`

Returns a Markdown-formatted prompt for a given name.

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
AGENTOS_ZMQ_ENDPOINT=tcp://127.0.0.1:5555 ./hello_plugin
```
