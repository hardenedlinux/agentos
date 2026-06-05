#include "agentos/forge/code_reviewer.h"
#include "agentos/error_utils.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/types.h"
#include <cstdlib>
#include <future>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

namespace agentos::forge
{

  std::string code_reviewer (const std::string &input_json)
  {
    // TODO: Parse input JSON using rapidjson
    // Expected structure per ADR-019:
    // {
    //   "task_id": "...",
    //   "forge_job_id": "...",
    //   "requirement": { "description": "...", "input_schema": {...},
    //   "output_schema": {...} }, "writer_output": {
    //     "code": "...",
    //     "language": "python" | "guile",
    //     "entry_point": "...",
    //     "capability": { "network": bool, "fs_read": [...], "fs_write": [...],
    //     "exec": bool }, "understanding": "..."
    //   }
    // }
    // Extract: task_id, forge_job_id, requirement (as object), writer_output
    // (as object) Extract from writer_output: code, language, capability block
    // Return make_error(...) on any parse failure or missing mandatory field

    // TODO: ENFORCE LAYER PRE-CHECK (ADR-009 Layer B, ADR-019)
    // Read capability block from writer_output
    // If capability.network == true: return make_error("policy violation:
    // network access declared") If capability.exec == true:    return
    // make_error("policy violation: exec access declared") This check is
    // deterministic and must run before sandbox and before LLM A policy
    // violation here causes immediate terminal rejection (state = rejected, no
    // retry)

    // TODO: Write code to a temporary file under a scratch directory
    // Use forge_job_id to construct a deterministic path, e.g.:
    //   ~/.agentos/forge/<forge_job_id>/sandbox_probe.<ext>
    // Extension: .py for python, .scm for guile
    // Return make_error(...) on write failure

    // TODO: Construct a synthetic mock input conforming to
    // requirement.input_schema For each field in input_schema, generate a
    // minimal valid value by type:
    //   "string" -> ""
    //   "int"    -> 0
    //   "path"   -> a temp path under the scratch directory
    // Write mock input to a temp JSON file in the scratch directory
    // This is the input fed to the worker during sandbox execution

    // TODO: SANDBOX EXECUTION (ADR-006, ADR-011, ADR-015, ADR-016)
    // fork()
    // In child process, apply sandbox stack in this order:
    //   1. cgroup v2 limits (CPU / memory / PID) via libcgroup
    //      Use Tier-1 defaults from config: memory_mb, cpu_weight, pid_limit
    //   2. CLONE_NEWNS + CLONE_NEWUSER for mount namespace
    //   3. Mount overlayfs: lower=host, upper=scratch dir; pivot_root into
    //   merged view
    //   4. CLONE_NEWNET (capability.network == false, which we already verified
    //   above)
    //   5. Landlock ruleset (ADR-015):
    //      - allow read+write on scratch dir
    //      - allow read on each path in capability.fs_read
    //      - allow read+write on each path in capability.fs_write
    //      - no TCP rules (network is false)
    //   6. seccomp whitelist: read, write, open, close, mmap, exit, futex, brk,
    //   stat
    //      deny: execve (except the interpreter itself), socket, connect, bind,
    //      fork, ptrace, mount, setuid Use SECCOMP_RET_ERRNO for denials so
    //      behaviour is observable, not SECCOMP_RET_KILL (SECCOMP_RET_ERRNO
    //      lets us detect what was attempted; KILL would lose that information)
    //   7. libcap: drop all capabilities
    //   8. exec the worker binary with mock input path as argument
    //      stdout/stderr redirect to scratch dir output file
    // In parent process:
    //   waitpid() with timeout
    //   If timeout exceeded: kill child, record status = "sandbox_timeout"
    //   Capture exit code
    // Return make_error(...) if sandbox setup itself fails (not if the worker
    // fails)

    // TODO: CAPABILITY HONESTY CHECK (ADR-019)
    // Parse the seccomp audit log / ptrace record from sandbox execution
    // Check for any of the following that were attempted but NOT declared:
    //   - socket() / connect() / bind() syscalls -> undeclared network access
    //   - open() / openat() paths outside declared fs_read + fs_write ->
    //   undeclared fs access
    //   - execve() beyond the initial interpreter -> undeclared exec
    // If any undeclared access detected:
    //   return reject verdict immediately with specific reason string
    //   do NOT proceed to LLM review
    // If exit_code != 0:
    //   return reject verdict with "worker exited with code <N>"
    //   do NOT proceed to LLM review

    // TODO: Validate sandbox output conforms to requirement.output_schema
    // Read the output file written by the worker during sandbox execution
    // Parse as JSON
    // For each field declared in output_schema, verify the field exists and
    // type matches If validation fails: return reject verdict with specific
    // field mismatch reason This is still a deterministic check; LLM is not
    // involved

    // TODO: LLM REVIEW (functional correctness only — ADR-019)
    // Only reached if all sandbox checks above passed
    // Read system prompt from skill.md:
    //   path = std::getenv("AGENTOS_ADVISER_SKILL_PATH")
    //   read entire file as string
    // Read LLM config from environment (ADR-018 AGENTOS_ADVISER_* prefix):
    //   AGENTOS_ADVISER_BASE_URL
    //   AGENTOS_ADVISER_API_KEY
    //   AGENTOS_ADVISER_MODEL
    //   AGENTOS_ADVISER_MAX_TOKENS
    //   AGENTOS_ADVISER_TIMEOUT_S
    // Return make_error(...) if API key is empty
    // Build user prompt containing:
    //   - requirement.description
    //   - writer_output.understanding
    //   - code
    //   - sandbox execution summary (exit code, output produced, capability
    //   check result)
    // Instruct LLM to return JSON: { "status": "accept"|"reject", "reason":
    // "..." } Use LlmProxy passed in as parameter (do NOT construct a static
    // local LlmProxy) Call proxy.enqueue(req), block on future If LLM call
    // fails: return make_error(...) Parse LLM response JSON; if malformed or
    // missing "status": return make_error(...)

    // TODO: Build and return final verdict JSON per ADR-019 output contract:
    // {
    //   "task_id": "...",
    //   "status": "accept" | "reject",   // from LLM response
    //   "reason": "..."                  // from LLM response
    // }
    // Use rapidjson to construct; return as serialised string
  }
} // namespace agentos::forge
