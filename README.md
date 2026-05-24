# AgentOS

> Single-binary, language-agnostic agent runtime.  
> C++17 core · Unix sockets · JSON-RPC 2.0 · Plugins in any language.

---

## Quick Start

```bash
# 1. Clone
git clone https://github.com/yourorg/agentos && cd agentos

# 2. Install prerequisites (once)
# Ubuntu/Debian:
sudo apt install cmake ninja-build build-essential

# macOS:
brew install cmake ninja

# 3. Build
./scripts/build.sh

# 4. Run
./build/src/cli/agentos

# 5. Verify static linkage
./scripts/verify_static.sh
```

The first build downloads and compiles all dependencies (~2–5 min).  
Subsequent builds are incremental and fast.

---

## What is AgentOS?

AgentOS is a minimal agent runtime that ships as a single compiled binary.  
It orchestrates **plugins** — separate processes that can be written in  
any programming language — via a Unix domain socket using JSON-RPC 2.0.

```
┌─────────────────────────────────────────────────────────┐
│                   AgentOS Core Binary                   │
│  Plugin Host · Dispatcher · Capability Reg · Task DAG  │
└──────────────────────┬──────────────────────────────────┘
                       │  Unix socket + JSON-RPC 2.0
        ┌──────────────┼──────────────┐
        │              │              │
  ┌─────▼─────┐  ┌─────▼─────┐  ┌───▼───────┐
  │ Plugin    │  │ Plugin    │  │ Plugin    │
  │ (Python)  │  │ (Go)      │  │ (Rust)    │
  └───────────┘  └───────────┘  └───────────┘
```

---

## Project Structure

```
agentos/
├── CMakeLists.txt          # Root build file
├── cmake/
│   └── deps.cmake          # FetchContent dependency declarations
├── src/
│   ├── main.cpp            # Entry point + dependency smoke tests
│   ├── plugin_host/        # Process lifecycle (spawn, monitor, kill)
│   ├── dispatcher/         # JSON-RPC socket server and router
│   ├── capability_reg/     # Capability → plugin registry
│   ├── task_engine/        # Agent DAG scheduler
│   ├── obs_bus/            # Logs, metrics, traces aggregation
│   └── config/             # Manifest + env + CLI config loading
├── include/agentos/        # Public headers
├── tests/                  # Google Test unit tests
├── plugins/
│   └── hello-plugin/
│       ├── plugin.md       # Plugin manifest (Markdown + YAML frontmatter)
│       └── hello_plugin.py # Example plugin implementation (Python)
├── agents/
│   └── hello-agent.md      # Example agent definition
└── scripts/
    ├── build.sh            # Main build script
    └── verify_static.sh    # Static linkage verification
```

---

## Build Options

| Flag | Default | Description |
|---|---|---|
| `--debug` | off | Debug build with ASan/UBSan |
| `--musl` | off | Fully static via musl-gcc |
| `--no-tests` | off | Skip unit tests |
| `--clean` | off | Clean build directory first |

```bash
./scripts/build.sh --debug      # Debug + sanitisers
./scripts/build.sh --musl       # Fully static (requires musl-tools)
./scripts/build.sh --clean      # Clean rebuild
```

### CMake directly

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGENTOS_STATIC=ON \
  -DAGENTOS_STRIP=ON
cmake --build build --parallel
```

---

## Binary Linkage

### Standard build (recommended)

`libstdc++` and `libgcc` are statically linked. Only glibc remains dynamic.  
Runs on any Linux with glibc ≥ 2.17 (CentOS 7, Ubuntu 14.04+, and newer).

```
$ ldd ./build/src/cli/agentos
    linux-vdso.so.1
    libpthread.so.0   ← expected
    libdl.so.2        ← expected
    libc.so.6         ← expected (glibc)
    # libstdc++ and libgcc_s are absent ✓
```

### Fully static build (musl)

Zero dynamic dependencies. Runs on literally any Linux distribution.

```bash
sudo apt install musl-tools
./scripts/build.sh --musl

$ ldd ./build/src/cli/agentos
    not a dynamic executable   ✓
```

### Verify

```bash
./scripts/verify_static.sh
```

---

## Plugin Contract

A plugin is any executable that:

1. **Connects** to the Unix socket at `$AGENTOS_SOCKET`
2. **Sends** a `plugin.register` JSON-RPC notification on startup
3. **Handles** `task.invoke` requests with a `result` or `error`
4. **Handles** `plugin.shutdown` gracefully

### Message framing

All messages are length-prefixed:

```
[ 4 bytes: uint32 LE payload length ][ payload bytes: UTF-8 JSON ]
```

### Example: plugin.register

```json
{
  "jsonrpc": "2.0",
  "method":  "plugin.register",
  "params": {
    "name":         "hello-plugin",
    "version":      "0.1.0",
    "capabilities": ["hello.greet", "hello.farewell"]
  }
}
```

### Example: task.invoke

```json
{
  "jsonrpc": "2.0",
  "id":      "abc-123",
  "method":  "task.invoke",
  "params": {
    "capability": "hello.greet",
    "input": { "name": "Alice" }
  }
}
```

### Example: response

```json
{
  "jsonrpc": "2.0",
  "id":      "abc-123",
  "result":  { "message": "Hello, Alice!" }
}
```

See `plugins/hello-plugin/hello_plugin.py` for a complete working example.

---

## Plugin Manifest Format

Plugin manifests are Markdown files with YAML frontmatter.  
The frontmatter is the machine-readable contract. The body is documentation.

```markdown
---
name: my-plugin
version: 1.0.0
executable: ./my_plugin
capabilities:
  - my.capability
resources:
  max_memory_mb: 128
  timeout_ms: 10000
---

# my-plugin

Human-readable documentation goes here.
```

---

## Agent Definition Format

Agents are also Markdown files with YAML frontmatter.

```markdown
---
name: my-agent
steps:
  - id: step1
    capability: my.capability
    input:
      key: "{{variable}}"
  - id: step2
    capability: another.capability
    depends_on: [step1]
    input:
      data: "{{step1.output}}"
---

# my-agent

Documentation for the agent.
```

---

## Dependencies

All dependencies are fetched automatically by CMake at configure time.

| Library | Version | Role | License |
|---|---|---|---|
| [libuv](https://libuv.org) | 1.48.0 | Async I/O, process management | MIT |
| [RapidJSON](https://rapidjson.org) | 1.1.0 | JSON-RPC serialisation (header-only) | MIT |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | Manifest YAML parsing | MIT |
| [spdlog](https://github.com/gabime/spdlog) | 1.13.0 | Structured logging | MIT |
| [GoogleTest](https://github.com/google/googletest) | 1.14.0 | Unit testing (test builds only) | BSD-3 |

---

## Roadmap

| Phase | Status | Description |
|---|---|---|
| 0 — Foundation | 🔨 In progress | Core binary, socket server, plugin spawn, dispatcher |
| 1 — Agents | ⏳ Planned | TOML agent DAG, task engine, variable interpolation |
| 2 — SDKs | ⏳ Planned | Python, Go, TypeScript SDK wrappers + first-party plugins |
| 3 — Observability | ⏳ Planned | Structured logs, Prometheus, OpenTelemetry traces |
| 4 — Security | ⏳ Planned | cgroup limits, capability whitelisting, fuzz testing |
| 5 — Windows | ⏳ Planned | Named pipe transport, Windows CI |

---

## License

TBD
