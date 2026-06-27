/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "agentos/home_init.h"
#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace agentos
{

  namespace
  {

    // --- Default skill package files (ADR-018) ---

    // -------- Planning adviser --------

    const char *PLANNING_MANIFEST = R"([meta]
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

    const char *PLANNING_SKILL = R"(# Planning Adviser

## Role
You are a task planner for an agent orchestration system. Your job is to decompose a user goal into an ordered sequence of steps. Each step maps to a Worker capability that will be executed by the system.

## Critical Output Rules
- Respond with a JSON object ONLY. No markdown, no explanation, no code blocks.
- The JSON must have exactly one key: "steps", whose value is an array.
- Each step must have exactly three keys: "id" (string), "command" (string), "description" (string).
- "id" must be a UUID v4 string (e.g. "a1b2c3d4-e5f6-7890-abcd-ef1234567890").
- "command" must be a snake_case capability name (e.g. "write_python_code", "run_tests", "generate_text"). Never use dots, spaces, or CamelCase.
- Keep steps minimal — use the fewest steps necessary to achieve the goal.

## Output Format
{"steps":[{"id":"<uuid>","command":"<snake_case_name>","description":"<what this step does>"}]}

## Step Count Guidelines
- Pure text generation or reasoning tasks: 1 step (command: "generate_text")
- Code writing tasks: 1-2 steps (e.g. "write_code" then "verify_code")
- Multi-stage tasks: up to 3 steps maximum

## Constraints
- Never include "depends_on", "args", "capabilities", or any other fields beyond id/command/description.
- Never request network or exec capabilities unless the goal explicitly requires external access.
- command names must be lowercase snake_case only.
)";

    const char *PLANNING_CONFIG = R"(# Adviser runtime configuration
# All fields are optional; defaults are inherited from the global daemon config.

[llm]
# model       = ""
# base_url    = ""
# api_key_env = ""
# max_tokens  = 4096
# timeout_s   = 180
)";

    // -------- Code‑writer adviser --------

    const char *CODE_WRITER_MANIFEST = R"([meta]
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

    const char *CODE_WRITER_SKILL = R"AAA(# Code Writer Adviser

## Role

You are a senior software engineer writing Python worker scripts for AgentOS.
A worker script is a self-contained Python program that performs real work and
writes its output to result.json.

## What a worker does

A worker EXECUTES the task described by task["description"]. It does NOT generate
code strings as output.

Examples of correct behaviour:
- description: "implement haskell-style monads" -> worker contains the monad
  implementation; result is a demo of that implementation running
- description: "sort a list of numbers" -> worker sorts them; result is the sorted list
- description: "compute fibonacci(10)" -> worker computes it; result is {"value": 55}

Examples of FORBIDDEN behaviour:
- Returning {"code": "def hello(): ..."} - never return source code as the result
- Returning a placeholder, stub, or hardcoded dummy value
- Writing a worker whose only job is to emit a code string

## Output Format

You MUST respond with a JSON object only - no markdown fences, no explanation, no prose.
The JSON must have exactly these fields:
{
  "understanding": "<your interpretation of what this worker must do>",
  "language": "python",
  "entry_point": "<name of the main function>",
  "code": "<complete Python source code as a single escaped string>",
  "capability": {
    "network": false,
    "fs_read": [],
    "fs_write": [],
    "exec": false
  },
  "notes": "<optional remarks>"
}

## Worker Contract

The Python code you write MUST follow this pattern:

  import sys, os, json

  def main():
      task = json.loads(sys.stdin.read())
      description = task.get("description", "")
      prev_result = task.get("$prev_result", {})

      run_dir = os.environ.get("AGENTOS_RUN_DIR", ".")
      result_path = os.path.join(run_dir, "result.json")

      try:
          result = do_the_actual_work(description, prev_result)
          with open(result_path, "w") as f:
              json.dump({"status": "ok", "result": result}, f)
      except Exception as e:
          with open(result_path, "w") as f:
              json.dump({"status": "error", "error": str(e)}, f)
          sys.exit(1)

  if __name__ == "__main__":
      main()

## Critical Rules

- The worker "code" field IS the solution. It executes logic and produces a result,
  not a code string.
- "result" must be the actual output of running the logic. Never a source code string.
- Read task["$prev_result"] when this step builds on a previous step output.
- Never use print() for output - stdout is ignored; only result.json is read.
- Never write to hardcoded paths - always use AGENTOS_RUN_DIR.
- Use standard library only unless the requirement explicitly demands external packages.
- network and exec must be false unless the requirement explicitly demands them.
- The code must be complete and correct. No stubs, no TODOs, no hello-world placeholders.
)AAA";

    const char *CODE_WRITER_CONFIG = R"(# Adviser runtime configuration
# All fields are optional; defaults are inherited from the global daemon config.

[llm]
# model       = ""
# base_url    = ""
# api_key_env = ""
# max_tokens  = 4096
# timeout_s   = 180
)";

    // -------- Code‑reviewer adviser --------

    const char *CODE_REVIEWER_MANIFEST = R"([meta]
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

    const char *CODE_REVIEWER_SKILL = R"(# Code Reviewer Adviser

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

    const char *CODE_REVIEWER_CONFIG = R"(# Adviser runtime configuration
# All fields are optional; defaults are inherited from the global daemon config.

[llm]
# model       = ""
# base_url    = ""
# api_key_env = ""
# max_tokens  = 4096
# timeout_s   = 180
)";

    // -------- Adviser stub scripts (backward‑compatibility with earlier tests)
    // --------

    const char *PLANNING_SCRIPT = "#!/usr/bin/env python3\n";
    const char *CODE_WRITER_SCRIPT = "#!/usr/bin/env python3\n";
    const char *CODE_REVIEWER_SCRIPT = "#!/usr/bin/env python3\n";

    // --- Generic helpers (from ADR-014) ---

    void seed_if_absent (const std::filesystem::path &path, const char *content)
    {
      if (std::filesystem::exists (path))
      {
        return;
      }
      std::ofstream out (path);
      if (!out)
      {
        spdlog::error ("[home_init] cannot seed file {}", path.string ());
        return;
      }
      out << content;
      out.close ();
      spdlog::info ("[home_init] seeded default {}", path.string ());
    }

  } // anonymous namespace

  std::filesystem::path agentos_home ()
  {
    if (const char *env = std::getenv ("AGENTOS_HOME"))
    {
      return std::filesystem::path (env);
    }
    const char *home = std::getenv ("HOME");
    if (!home)
    {
      spdlog::error (
        "[home_init] HOME environment variable not set, using /tmp/.agentos");
      return std::filesystem::path ("/tmp/.agentos");
    }
    return std::filesystem::path (home) / ".agentos";
  }

  void initialise_home (const std::filesystem::path &base)
  {
    const std::vector<std::filesystem::path> dirs = {
      base,
      base / "run",
      base / "advisers" / "planning",
      base / "advisers" / "code-writer",
      base / "advisers" / "code-reviewer",
      base / "workers",
      base / "skills",
      base / "forge",
      base / "vault",
      base / "logs",
    };

    for (const auto &dir : dirs)
    {
      std::error_code ec;
      std::filesystem::create_directories (dir, ec);
      if (ec)
      {
        spdlog::error ("[home_init] cannot create directory {}: {}",
                       dir.string (), ec.message ());
      }
    }

    // Seed default global config only if absent
    const auto global_cfg = base / "config.toml";
    if (!std::filesystem::exists (global_cfg))
    {
      std::ofstream out (global_cfg);
      if (out)
      {
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
        out.close ();
      }
      else
      {
        spdlog::error ("[home_init] cannot create default config at {}",
                       global_cfg.string ());
      }
    }

    const std::filesystem::path advisers = base / "advisers";

    // --- Seed ADR-018 skill packages for the three built‑in advisers ---

    // Planning adviser
    seed_if_absent (advisers / "planning" / "manifest.toml", PLANNING_MANIFEST);
    seed_if_absent (advisers / "planning" / "skill.md", PLANNING_SKILL);
    seed_if_absent (advisers / "planning" / "config.toml", PLANNING_CONFIG);
    seed_if_absent (advisers / "planning" / "planning.py", PLANNING_SCRIPT);
    seed_if_absent (advisers / "planning" / "adviser.py", PLANNING_SCRIPT);

    // Code‑writer adviser
    seed_if_absent (advisers / "code-writer" / "manifest.toml",
                    CODE_WRITER_MANIFEST);
    seed_if_absent (advisers / "code-writer" / "skill.md", CODE_WRITER_SKILL);
    seed_if_absent (advisers / "code-writer" / "config.toml",
                    CODE_WRITER_CONFIG);
    seed_if_absent (advisers / "code-writer" / "code_writer.py",
                    CODE_WRITER_SCRIPT);
    seed_if_absent (advisers / "code-writer" / "adviser.py",
                    CODE_WRITER_SCRIPT);

    // Code‑reviewer adviser
    seed_if_absent (advisers / "code-reviewer" / "manifest.toml",
                    CODE_REVIEWER_MANIFEST);
    seed_if_absent (advisers / "code-reviewer" / "skill.md",
                    CODE_REVIEWER_SKILL);
    seed_if_absent (advisers / "code-reviewer" / "config.toml",
                    CODE_REVIEWER_CONFIG);
    seed_if_absent (advisers / "code-reviewer" / "code_reviewer.py",
                    CODE_REVIEWER_SCRIPT);
    seed_if_absent (advisers / "code-reviewer" / "adviser.py",
                    CODE_REVIEWER_SCRIPT);

    // TPM state self-healing
    const auto nvchip = base / "vault" / "NVChip";
    if (std::filesystem::exists (nvchip))
      {
        // simple check: if file size is 0 then repair
        std::error_code ec;
        auto size = std::filesystem::file_size (nvchip, ec);
        if (ec || size == 0)
          {
            spdlog::warn (
                          "[home_init] NVChip appears corrupt, removing for reinitialisation");
            std::filesystem::remove (nvchip, ec);
          }
      }

    // Clean up stale socket from previous unclean shutdown
    const auto sock = base / "run" / "agentos.sock";
    if (std::filesystem::exists (sock))
      {
        std::error_code ec;
        std::filesystem::remove (sock, ec);
        spdlog::warn ("[home_init] removed stale socket {}", sock.string ());
      }
  }
} // namespace agentos
