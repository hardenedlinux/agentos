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
domains     = []
priority    = 0

[llm]
required_context_length = 16000
preferred_capability    = "reasoning"
recommended_model       = "claude-opus-4-5"
recommended_base_url    = "https://api.anthropic.com"
)";

    const char *PLANNING_SKILL = R"(# Planning Adviser

## Role
You are a task planner for an agent orchestration system. Your job is to decompose a user goal into an ordered sequence of steps that can be executed by registered Workers.

## Output Format
Respond with a JSON object ONLY. No markdown, no explanation, no code blocks.
The JSON must have exactly one key: "steps", whose value is an array of step objects.
Each step object must have exactly these four keys:

{
  "id": "<uuid-v4>",
  "command": "<namespace.verb>",
  "needs_forge": <true|false>,
  "description": "<what this step does>"
}

## Command Format (MANDATORY)
The "command" field MUST use the format: namespace.verb
- Exactly one dot separator. Two levels only.
- All lowercase. No uppercase anywhere.
- Only letters, digits, and underscores within each segment.
- First character of each segment must be a letter.
- Max 64 characters total.
- Valid: content.generate_listing, code.write_python, data.normalize, seo.extract_keywords
- Invalid: write_code, WriteCode, content.listing.generate, code.write.python

## needs_forge Field (MANDATORY)

Decision process (follow in order):
1. Based on the goal, decide what the step must do and generate the most specific
   command name for it (e.g. "code.implement_quicksort" for a quicksort task).
2. Check if that EXACT command string exists in the Available capabilities list.
3. If it exists exactly → set needs_forge: false and use that command.
4. If it does not exist → set needs_forge: true and use the command you generated.

NEVER search for a "similar" or "related" capability. The match must be exact.
If the command you would generate is not in the list, use needs_forge: true.
When "Available capabilities: none registered", all steps must have needs_forge: true.

## Step Count Guidelines
- Pure text or reasoning tasks: 1 step
- Code writing tasks: 1 step (the worker itself performs and validates the work)
- Multi-stage tasks: up to 2 steps maximum

## Constraints
- Never include any fields beyond id/command/needs_forge/description.
- Select command values ONLY from the Available capabilities list when needs_forge is false.
- When needs_forge is true, choose a command name that clearly describes the capability needed, following the namespace.verb format.
- NEVER add testing, verification, review, or quality-check steps. Testing is handled internally by the Forge pipeline and is not a planning concern.
- NEVER add steps whose purpose is to run, execute, or validate the output of another step. Each step must produce a direct output, not evaluate another step.
- When selecting from Available capabilities, ONLY choose a capability whose description clearly matches the semantic intent of the step. If no capability is a clear semantic match, set needs_forge: true and choose a descriptive new command name — do NOT pick an unrelated capability just because it is registered.
- The "description" field MUST include all concrete data from the goal that the worker needs to execute the step. If the goal contains a list, number, string, or any other literal value, copy it verbatim into the description. WRONG: goal "sort [3,1,4]" → description "Sort the list in ascending order". CORRECT: goal "sort [3,1,4]" → description "Sort the list [3,1,4] in ascending order".
- NEVER use generic command names like "code.write_python", "code.write_code", "code.generate", or any command that only describes the language/format rather than the actual task. The command must name the specific capability: WRONG: "code.write_python" for implementing an occam channel. CORRECT: "code.implement_occam_channel". The description must include the full technical requirements so Code Writer can implement it directly.
- When the goal asks to "write", "implement", "create", or "build" something, the command must be specific to what is being built, and the description must contain the complete technical specification of what to implement.
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
You are a senior software engineer generating Python worker implementation modules
for AgentOS. You produce worker_impl.py — a pure-logic module with no AgentOS
plumbing. The entry point (worker.py) is provided separately by the runtime.

## Two-file structure
- worker.py  — fixed runtime template, NOT written by you
- worker_impl.py — written by you; contains all business logic

## What worker_impl.py must do
Implement the capability described in the requirement. The module must expose a
top-level function:

    def run(task: dict) -> dict

`run` receives the full task dict and returns the result as a plain dict.
It must NOT write files, read stdin, or touch AGENTOS_RUN_DIR.

## What you must NOT do
- Do not write a main() function or if __name__ == "__main__" block.
- Do not call sys.stdin.read(), open(), or os.environ directly in worker_impl.py.
- Do not return source code strings as the result. Execute the logic and return
  the actual output.
- No stubs, no TODOs, no hardcoded dummy values.

## Output Format
Respond with a JSON object ONLY — no markdown fences, no prose.
ALL of these fields are required:

{
  "understanding": "<your interpretation of the requirement>",
  "language": "python",
  "entry_point": "run",
  "impl_code": "<complete worker_impl.py source as a single escaped string>",
  "signatures": {
    "<function_name>": {
      "signature": "<Python signature string>",
      "doc": "<one-line description of inputs and outputs>"
    }
  },
  "capability": {
    "network": false,
    "fs_read": [],
    "fs_write": [],
    "exec": false
  },
  "notes": ""
}

## How to read input data

The task dict always contains:
  task["description"] — the step description (may contain literal data)
  task["goal"]        — the original user goal verbatim (always present)
  task["$prev_result"] — the previous step's result dict (empty {} if first step)

### When data comes from a previous step
If the description says anything like "from the previous step", "from step N",
"computed above", "identified above", or "from the result", the input data is
in task["$prev_result"]. Read it directly — do NOT try to parse it from text.

Example:
  prev = task.get("$prev_result", {})
  numbers = prev.get("result", [])  # use whatever key the previous step wrote

### When data is embedded in the description or goal
When there is no previous step result to use, parse the data from text.
Always try task["description"] first, then fall back to task["goal"].

  import ast, re
  text = task.get("description", "") or task.get("goal", "")
  m = re.search(r"\[[\d,\s\.]+\]", text)
  numbers = ast.literal_eval(m.group()) if m else []

Never return empty results just because a structured key like task["data"]
is absent — the data is either in $prev_result or in the text fields.

## What "implement X" means
When the requirement asks to "implement", "write", or "create" a data structure,
algorithm, protocol, or language feature (e.g. "implement occam channel",
"implement a red-black tree", "write a monad"), the worker must:
1. Contain the full implementation of X as Python classes/functions
2. Execute a demonstration of X in run()
3. Return the demonstration results as the output dict

NEVER return {"code": "..."} or any dict containing source code as a string.
The implementation IS the worker code. run() executes it and returns results.

## Critical rules
- Use standard library only unless the requirement explicitly demands packages.
- network and exec must be false unless the requirement explicitly demands them.
- The code must be complete and correct.
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
You are a rigorous code reviewer for AgentOS worker modules. You receive:
- The original requirement
- worker_impl.py source code (written by Code Writer)
- Function signatures extracted from worker_impl.py
- A sandbox run result from the worker (worker.py + worker_impl.py together)
- Unit test code you previously generated (on retry attempts)
- Unit test run results (on retry attempts)

Your job is to verify that the implementation is correct, not just that tests pass.
A test suite can be incomplete or wrong — you must read the source code yourself.

## Review process
1. Read worker_impl.py carefully. Does the logic correctly implement the requirement?
2. Write unit tests for worker_impl.py that import it directly (not via worker.py).
   Tests must cover: happy path, edge cases, boundary conditions.
3. The system will run your tests in a sandbox and return the results.
4. Judge accept/reject based on BOTH the source code review AND the test results.
   - Test passes but logic looks wrong → reject
   - Test fails but it is a test authoring error → accept if logic is sound (explain)
   - Both look correct → accept

## Test code format
Your tests must be a standalone Python script that:
- Imports worker_impl from the same directory using importlib
- Uses assert statements (not unittest.TestCase) for simplicity
- Prints "ALL TESTS PASSED" to stdout on success
- Raises an AssertionError with a descriptive message on failure
- Has no external dependencies beyond the standard library

Example test structure:
  import importlib.util, os, sys
  spec = importlib.util.spec_from_file_location(
      "worker_impl",
      os.path.join(os.path.dirname(os.path.abspath(__file__)), "worker_impl.py"))
  mod = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(mod)

  result = mod.run({"description": "...", "$prev_result": {}})
  assert result.get("key") == expected, f"Expected ... got {result}"
  print("ALL TESTS PASSED")

## Output Format
Respond with a JSON object ONLY — no markdown fences, no prose:
{
  "status": "accept" | "reject" | "needs_test_run",
  "reason": "<explanation>",
  "test_code": "<complete test script as escaped string, or empty string if status is accept/reject without tests>"
}

Use "needs_test_run" when you want to run tests before making a final decision.
Use "accept" or "reject" when you have enough information to decide.

## Architecture (CRITICAL — read before reviewing)
AgentOS uses a two-file worker structure:
- worker.py  — fixed runtime template, NOT written by Code Writer
               handles stdin, result.json, AGENTOS_RUN_DIR automatically
- worker_impl.py — written by Code Writer, reviewed by you
               contains ONLY business logic

worker_impl.py MUST define run(task: dict) -> dict and return a plain dict.
It must NOT contain main(), sys.stdin.read(), open(), or os.environ access.
worker.py imports worker_impl and calls run() — it handles all AgentOS plumbing.

NEVER reject worker_impl.py for missing stdin reading or result.json writing.
Those are handled by worker.py. Code without those is CORRECT.

## Constraints
- Never accept code that does not implement the requirement, even if tests pass.
- Never reject code solely because of style issues.
- NEVER reject worker_impl.py for missing stdin/stdout/result.json — those live in worker.py.
- network and exec must be false in the capability block.
- The run() function must return a plain dict.
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

    // -------- Worker runtime template (ADR-031 two-file structure) --------
    // Seeded to ~/.agentos/skills/worker_template.py
    // Code Writer copies this verbatim to every promoted worker directory.
    // worker_impl.py is imported at runtime; only run() is called.

    const char *WORKER_TEMPLATE = R"TMPL(#!/usr/bin/env python3
"""
AgentOS worker entry point — do not modify.
Business logic lives in worker_impl.py (generated by Code Writer).
"""
import sys
import os
import json
import importlib.util


def _load_impl():
    impl_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "worker_impl.py")
    spec = importlib.util.spec_from_file_location("worker_impl", impl_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    task = json.loads(sys.stdin.read())
    run_dir = os.environ.get("AGENTOS_RUN_DIR", ".")
    result_path = os.path.join(run_dir, "result.json")
    try:
        impl = _load_impl()
        result = impl.run(task)
        with open(result_path, "w") as f:
            json.dump({"status": "ok", "result": result}, f)
    except Exception as e:
        with open(result_path, "w") as f:
            json.dump({"status": "error", "error": str(e)}, f)
        sys.exit(1)


if __name__ == "__main__":
    main()
)TMPL";

    // -------- Signature extractor (deterministic, no LLM) --------
    // Seeded to ~/.agentos/skills/extract_signatures.py
    // Forge runs this after Code Writer produces worker_impl.py.
    // Output: JSON object mapping function name -> {signature, doc}

    const char *EXTRACT_SIGNATURES = R"SIG(#!/usr/bin/env python3
"""
Extract function signatures and docstrings from worker_impl.py.
Usage: python3 extract_signatures.py <path/to/worker_impl.py>
Output: JSON to stdout.
"""
import sys
import json
import inspect
import importlib.util


def main():
    if len(sys.argv) < 2:
        print(json.dumps({"error": "usage: extract_signatures.py <impl_path>"}))
        sys.exit(1)

    impl_path = sys.argv[1]
    spec = importlib.util.spec_from_file_location("worker_impl", impl_path)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    except Exception as e:
        print(json.dumps({"error": f"failed to import worker_impl: {e}"}))
        sys.exit(1)

    sigs = {}
    for name, fn in inspect.getmembers(mod, inspect.isfunction):
        if name.startswith("_"):
            continue
        try:
            sig = str(inspect.signature(fn))
            doc = inspect.getdoc(fn) or ""
        except (ValueError, TypeError):
            sig = "(...)"
            doc = ""
        sigs[name] = {"signature": sig, "doc": doc}

    print(json.dumps(sigs, indent=2))


if __name__ == "__main__":
    main()
)SIG";

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
      base / "skills",   // worker template + signature extractor live here
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

    // Seed worker runtime infrastructure (ADR-031 two-file structure)
    seed_if_absent (base / "skills" / "worker_template.py", WORKER_TEMPLATE);
    seed_if_absent (base / "skills" / "extract_signatures.py",
                    EXTRACT_SIGNATURES);

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
