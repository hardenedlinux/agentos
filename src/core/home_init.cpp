#include "agentos/home_init.h"
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

namespace agentos {

namespace {

// --- Default skill package files (ADR-018) ---

// -------- Planning adviser --------

const char* PLANNING_MANIFEST = R"([meta]
id          = "planning"
version     = "1.0.0"
description = "General-purpose task planning adviser"
author      = "agentos-core"
source_url  = ""
sha256      = ""

[llm]
required_context_length = 16000
preferred_capability    = "reasoning"
recommended_model       = "claude-opus-4-5"
recommended_base_url    = "https://api.anthropic.com"
)";

const char* PLANNING_SKILL = R"(# Planning Adviser

## Role
You are an expert task planner that constructs agentic workflows.

## Capabilities
You can decompose a high-level goal into a sequence of executable steps using registered worker commands.
You validate capability requirements and propose sandbox tiers.

## Output Format
Respond with a JSON object:
{
  "steps": [
    {
      "id": "step_N",
      "command": "worker.command",
      "args": { "key": "value" },
      "depends_on": [],
      "capabilities": { "network": false }
    }
  ]
}

## Constraints
- Never request capabilities beyond what the task requires.
- Use existing registered commands only.
)";

const char* PLANNING_CONFIG = R"(# Adviser runtime configuration
# All fields are optional; defaults are inherited from the global daemon config.

[llm]
# model       = ""
# base_url    = ""
# api_key_env = ""
# max_tokens  = 4096
# timeout_s   = 180
)";

// -------- Code‑writer adviser --------

const char* CODE_WRITER_MANIFEST = R"([meta]
id          = "code-writer"
version     = "1.0.0"
description = "Generates Python worker code from a capability specification"
author      = "agentos-core"
source_url  = ""
sha256      = ""

[llm]
required_context_length = 32000
preferred_capability    = "code"
recommended_model       = "claude-opus-4-5"
recommended_base_url    = "https://api.anthropic.com"
)";

const char* CODE_WRITER_SKILL = R"(# Code Writer Adviser

## Role
You are a senior software engineer specialised in writing robust, secure Python worker
code for the AgentOS ecosystem.

## Capabilities
You receive a natural-language or structured capability specification and produce a
complete, runnable Python module that fulfils the spec.

## Output Format
Return the entire source code wrapped between ```python and ``` fences.
After the code block you may optionally include a brief explanation of design decisions.

## Constraints
- The code must be self-contained and import nothing outside the standard library unless
  explicitly permitted.
- Never access the filesystem unless the capability declaration allows it.
- Use defensive programming; handle all foreseeable edge cases.
)";

const char* CODE_WRITER_CONFIG = R"(# Adviser runtime configuration
# All fields are optional; defaults are inherited from the global daemon config.

[llm]
# model       = ""
# base_url    = ""
# api_key_env = ""
# max_tokens  = 4096
# timeout_s   = 180
)";

// -------- Code‑reviewer adviser --------

const char* CODE_REVIEWER_MANIFEST = R"([meta]
id          = "code-reviewer"
version     = "1.0.0"
description = "Reviews generated worker code for safety, correctness, and style"
author      = "agentos-core"
source_url  = ""
sha256      = ""

[llm]
required_context_length = 32000
preferred_capability    = "code"
recommended_model       = "claude-opus-4-5"
recommended_base_url    = "https://api.anthropic.com"
)";

const char* CODE_REVIEWER_SKILL = R"(# Code Reviewer Adviser

## Role
You are an experienced code reviewer specialised in Python worker modules for AgentOS.

## Capabilities
You examine a piece of Python code together with its capability specification and assess:
- Conformance to the specification
- Security risks (e.g. arbitrary code execution, unauthorised file access)
- Coding style and maintainability

## Output Format
Respond with a JSON object:
{
  "passed": true|false,
  "issues": [
    {
      "severity": "error|warning|info",
      "line": 123,
      "message": "..."
    }
  ]
}

## Constraints
- Report every issue, but keep messages concise.
- If no issues are found, return "passed": true with an empty issues array.
)";

const char* CODE_REVIEWER_CONFIG = R"(# Adviser runtime configuration
# All fields are optional; defaults are inherited from the global daemon config.

[llm]
# model       = ""
# base_url    = ""
# api_key_env = ""
# max_tokens  = 4096
# timeout_s   = 180
)";

// -------- Adviser stub scripts (backward‑compatibility with earlier tests) --------

const char* PLANNING_SCRIPT    = "#!/usr/bin/env python3\n";
const char* CODE_WRITER_SCRIPT = "#!/usr/bin/env python3\n";
const char* CODE_REVIEWER_SCRIPT = "#!/usr/bin/env python3\n";

// --- Generic helpers (from ADR-014) ---

void seed_if_absent(const std::filesystem::path& path, const char* content) {
    if (std::filesystem::exists(path)) {
        return;
    }
    std::ofstream out(path);
    if (!out) {
        spdlog::error("[home_init] cannot seed file {}", path.string());
        return;
    }
    out << content;
    out.close();
    spdlog::info("[home_init] seeded default {}", path.string());
}

} // anonymous namespace

std::filesystem::path agentos_home() {
    if (const char* env = std::getenv("AGENTOS_HOME")) {
        return std::filesystem::path(env);
    }
    const char* home = std::getenv("HOME");
    if (!home) {
        spdlog::error("[home_init] HOME environment variable not set, using /tmp/.agentos");
        return std::filesystem::path("/tmp/.agentos");
    }
    return std::filesystem::path(home) / ".agentos";
}

void initialise_home(const std::filesystem::path& base) {
    const std::vector<std::filesystem::path> dirs = {
        base,
        base / "run",
        base / "advisers" / "planning",
        base / "advisers" / "code-writer",
        base / "advisers" / "code-reviewer",
        base / "workers",
        base / "skills",
        base / "forge",
        base / "logs",
    };

    for (const auto& dir : dirs) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            spdlog::error("[home_init] cannot create directory {}: {}", dir.string(), ec.message());
        }
    }

    // Seed default global config only if absent
    const auto global_cfg = base / "config.toml";
    if (!std::filesystem::exists(global_cfg)) {
        std::ofstream out(global_cfg);
        if (out) {
            out << "# AgentOS user configuration\n";
            out << "# See ADR-013 for format details.\n\n";
            out << "[llm]\n";
            out << "base_url = \"https://api.anthropic.com\"\n";
            out << "api_key = \"\"\n";
            out << "model = \"claude-opus-4-5\"\n";
            out << "max_tokens = 1024\n";
            out << "timeout_s = 120\n";
            out << "max_concurrent = 0\n\n";
            out << "[daemon]\n";
            out << "log_level = \"info\"\n";
            out << "max_concurrent_jobs = 4\n";
            out.close();
        } else {
            spdlog::error("[home_init] cannot create default config at {}", global_cfg.string());
        }
    }

    const std::filesystem::path advisers = base / "advisers";

    // --- Seed ADR-018 skill packages for the three built‑in advisers ---

    // Planning adviser
    seed_if_absent(advisers / "planning" / "manifest.toml", PLANNING_MANIFEST);
    seed_if_absent(advisers / "planning" / "skill.md",      PLANNING_SKILL);
    seed_if_absent(advisers / "planning" / "config.toml",   PLANNING_CONFIG);
    seed_if_absent(advisers / "planning" / "planning.py",   PLANNING_SCRIPT);
    seed_if_absent(advisers / "planning" / "adviser.py",    PLANNING_SCRIPT);

    // Code‑writer adviser
    seed_if_absent(advisers / "code-writer" / "manifest.toml", CODE_WRITER_MANIFEST);
    seed_if_absent(advisers / "code-writer" / "skill.md",      CODE_WRITER_SKILL);
    seed_if_absent(advisers / "code-writer" / "config.toml",   CODE_WRITER_CONFIG);
    seed_if_absent(advisers / "code-writer" / "code_writer.py", CODE_WRITER_SCRIPT);
    seed_if_absent(advisers / "code-writer" / "adviser.py",     CODE_WRITER_SCRIPT);

    // Code‑reviewer adviser
    seed_if_absent(advisers / "code-reviewer" / "manifest.toml", CODE_REVIEWER_MANIFEST);
    seed_if_absent(advisers / "code-reviewer" / "skill.md",      CODE_REVIEWER_SKILL);
    seed_if_absent(advisers / "code-reviewer" / "config.toml",   CODE_REVIEWER_CONFIG);
    seed_if_absent(advisers / "code-reviewer" / "code_reviewer.py", CODE_REVIEWER_SCRIPT);
    seed_if_absent(advisers / "code-reviewer" / "adviser.py",       CODE_REVIEWER_SCRIPT);
}

} // namespace agentos
