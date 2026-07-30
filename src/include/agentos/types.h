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
#pragma once
/**
 * agentos/types.h
 *
 * Core domain types shared across all subsystems.
 * No dependencies on other agentos headers.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view> // C++17
#include <unordered_map>
#include <vector>

#include "agentos/strong_id.h"

// Forward declarations of tag types for StrongId (ADR‑010)
struct ClientTag;
struct TaskTag;
struct JobTag;
struct ForgeJobTag;

namespace agentos
{

  // Identity – StrongId phantom types (ADR‑010)

  using ClientId = StrongId<struct ClientTag>;
  using TaskId = StrongId<struct TaskTag>;
  using JobId = StrongId<struct JobTag>;
  using ForgeJobId = StrongId<struct ForgeJobTag>;

  // Error handling (ADR‑010)

  using Error = std::string;

  // Tag to disambiguate error constructor when T == Error
  struct ErrorTag
  {
  };

  template <typename T> struct Result
  {
    T value;
    Error error;
    bool ok = false;

    Result () : value{}, ok (false) {}
    Result (T val) : value (std::move (val)), ok (true) {}
    Result (Error err, ErrorTag) : value{}, error (std::move (err)), ok (false)
    {
    }
  };

  // Executor command schema
  // Self-described by the worker at registration time.
  // The core uses this to validate agent plans before execution.

  struct ArgSchema
  {
    std::string type; // "string" | "integer" | "boolean" | "array" | "object"
    std::string description;
    bool required = true;
  };

  struct CommandLimits
  {
    int timeout_ms = 30000;
    int max_input_len = 4096;
  };

  struct CommandSchema
  {
    std::string name;
    std::string description; // natural language, also used by agent planning
    std::unordered_map<std::string, ArgSchema> input;
    std::unordered_map<std::string, ArgSchema> output;
    CommandLimits limits;
  };

  // Registered clients

  enum class ClientType
  {
    Adviser,
    Executor
  };

  struct RegisteredAdviser
  {
    ClientId id;
    std::string name;
    std::string version;
    // ~/.agentos/advisers/<name>/
    std::string skill_path;
    // Advisers declare which domains they can plan for (e.g. "research",
    // "coding")
    std::vector<std::string> domains;
    int priority = 0;
    // Natural-language description from manifest.toml [meta].description.
    // ADR-033 Step 2's LLM disambiguation prompt relies on this to tell
    // candidates apart — without it, the classifier only had `id` and
    // `domains` to go on, which duplicates Step 1's signal rather than
    // adding to it. Optional; empty if the manifest omits it.
    std::string description;
    bool supports_continuation = false; // ADR-038

    // Optional allow-list of adviser ids this adviser's own Plans may
    // target with a target_type:"adviser" step. std::nullopt (the
    // manifest didn't declare [capabilities] allowed_advisers at all) means
    // unrestricted — every existing product Suite (translation-pipeline,
    // user-intent, gap-mining) legitimately needs open access to whatever's
    // in "Available advisers" and gets no new restriction. A declared list
    // (even an empty one) is enforced at Plan-validation time in
    // orchestrator.cpp's plan_ready handler: any target_type:"adviser" step
    // whose command isn't in this list gets the whole Plan rejected
    // atomically, the same "no partial, fail outright" posture already used
    // for ADR-031 §1 capability-format validation. Exists because an
    // Adviser's own LLM completion can hallucinate a Plan step that invokes
    // some other real, already-registered adviser it was never meant to —
    // prompt-level instructions ("never reference X") are not a reliable
    // enough guard for this on their own.
    std::optional<std::vector<std::string>> allowed_advisers;

    // Inverse of allowed_advisers: some advisers must never be targeted as
    // an internal Plan step BY ANY OTHER adviser — only ever entered via
    // explicit top-level routing (job.submit's adviser_id, domain
    // selection, or continuation_id). allowed_advisers alone doesn't
    // protect against this — it only constrains what an adviser can
    // itself target, not who's allowed to target IT. Discovered in
    // practice: User Intent and Gap-Mining each spontaneously targeted the
    // OTHER as a target_type:"adviser" step in their own Plan, in both
    // directions — each is designed to be a front door / bridge-triggered
    // entry point, never an internal pipeline component another Adviser
    // delegates to. Defaults to false (every existing pipeline-component
    // Adviser — glossary-adviser, translate-adviser, edit-review-adviser —
    // legitimately needs to stay targetable by translation-pipeline's own
    // Plan; "planning" also stays targetable, since it's deliberately used
    // as a safe universal fallback/convergence target by other Advisers).
    // Enforced in orchestrator.cpp's plan_ready handler at the same point
    // allowed_advisers is checked: for every target_type:"adviser" step,
    // the TARGET's own entry_only flag is checked, regardless of which
    // adviser authored the Plan.
    bool entry_only = false;

    // Optional: can this adviser ever legitimately produce a Plan
    // (Shape 1, {"steps": [...]}) at all? Defaults to true (unrestricted —
    // every existing product Suite needs this). Some advisers (e.g. a
    // test-only one whose contract is "always respond with a fixed Shape 2,
    // never a Plan, under any circumstances") should never be allowed to
    // emit Shape 1 regardless of what target_type/command a hallucinated
    // step might use — allowed_advisers alone doesn't cover this, since it
    // only constrains target_type:"adviser" steps, not target_type:"worker"
    // ones (a hallucinated worker-target step sidesteps that check
    // entirely, as observed in practice). Enforced in orchestrator.cpp's
    // plan_ready handler, before allowed_advisers is even checked: if
    // false, ANY non-empty "steps" array in this adviser's response gets
    // the whole Plan rejected outright.
    bool can_produce_plan = true;
  };

  struct RegisteredExecutor
  {
    ClientId id;
    std::string name;
    std::string version;
    std::string binary_path; // path to the worker binary (from static catalog)
    std::vector<CommandSchema> commands; // self-described at registration
    // Sandbox grants declared in manifest.json's top-level fs_read/fs_write
    // (ADR-015/ADR-016). May contain the reserved placeholders
    // __JOB_INPUT_PATH__/__JOB_OUTPUT_DIR__, substituted per-job at
    // dispatch time (Orchestrator::dispatch_next_step) — never resolved
    // here, since Registry has no concept of "which job" is dispatching.
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_write;
    bool network = false;
  };

  // Sandbox tiers (ADR-006)

  enum class SandboxTier
  {
    Tier0, // pre‑approved catalog workers
    Tier1  // generated workers
  };

  enum class NetworkMode
  {
    Isolated,
    PortRestricted
  };

  // Capability declaration (ADR-006 Layer 2)

  struct CapabilityDeclaration
  {
    bool network = false;
    bool exec = false;
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_write;
    std::vector<int> tcp_connect_ports; // ADR-015
  };

  // Task

  struct Task
  {
    TaskId id;
    std::string goal;            // natural language goal from user
    std::string input_json;      // structured input (may be empty)
    std::string user_id = "0";   // ADR-029: submitting user
  };

  struct TaskResult
  {
    TaskId task_id;
    bool success = false;
    std::string output_json;
    std::string error; // populated if success == false
  };

  // ADR-016: Worker run record
  enum class WorkerStatus : int
  {
    running = 0,
    completed = 1,
    failed = 2,
    crashed = 3,
  };

  struct WorkerRun
  {
    std::string run_id;    // UUID
    std::string worker_id; // worker identifier
    int pid = 0;
    int64_t started_at = 0;
    int64_t ended_at = 0;
    int exit_code = -1;
    WorkerStatus status = WorkerStatus::running;
    std::string layer_path; // ~/.agentos/layers/runs/<run-id>/
    std::string log_path;   // ~/.agentos/logs/runs/<run-id>/output.log
  };

  // ADR-018: Adviser skill package manifest

  struct Manifest
  {
    std::string id;
    std::string version;
    std::string description;
    std::string author;
    std::string source_url;
    std::string sha256;

    struct Llm
    {
      int required_context_length = 0;
      std::string preferred_capability; // "code" | "reasoning" | "balanced"
      std::string recommended_model;
      std::string recommended_base_url;
    } llm;
  };

  // ADR-020: Gateway inbound message (identity + raw JSON‑RPC payload)
  struct GatewayInbound
  {
    std::string identity; // ZMQ identity frame
    std::string message;  // raw JSON-RPC 2.0 message
  };

  // ADR-020: Access key record (schema defined in ADR-020)
  struct AccessKey
  {
    std::string id;
    std::string key;      // plaintext for display
    std::string key_hash; // SHA-256(key || salt)
    std::string key_salt; // per-key random salt (16 bytes base64url)
    std::string description;
    std::string role; // admin | operator | readonly
    int64_t created_at = 0;
    std::optional<int64_t> expires_at;
    std::optional<int64_t> last_used_at;
    std::optional<int64_t> revoked_at;
    std::optional<std::string> revoked_reason;
  };

  // ADR-022 — Pipeline Plan (Master‑generated, serial steps)
  struct PipelinePlanStep
  {
    std::string id;
    std::string target_type; // ADR-031 §9: "worker" | "adviser" — required,
                             // no default (missing is logged as a warning
                             // and treated as "worker" for backward
                             // compatibility with pre-§9 plans, mirroring
                             // the existing needs_forge leniency below)
    std::string command;     // capability method name (worker) or adviser id
    std::string description; // natural language (Master‑generated)
    std::unordered_map<std::string, std::string> params;
    bool needs_forge = false; // ADR-031: true = Forge should be triggered if Registry misses
  };

  struct PipelinePlan
  {
    TaskId task_id;
    std::vector<PipelinePlanStep> steps; // ordered; steps[0] executes first
  };

  // ADR-022 — Orchestrator event queue entries
  struct OrchestratorEvent
  {
    enum class Kind
      {
        GatewayInbound, // raw message from Gateway
        WorkerDone,     // Dispatcher reaper: Worker completed
        WorkerFailed,   // Dispatcher reaper: Worker failed
        AdviserDone,    // Adviser thread completed successfully
        AdviserFailed,  // Adviser thread exited with error
        MasterDecision, // Master has reached a decision
        TimerFired,     // from PeriodicExecutor (scheduled task)
      };

    Kind kind;
    std::string payload_json;
    std::string identity;
    std::string job_id; // associated job (replaces TaskId where relevant)
  };

  // ADR-024 — Master event queue entries
  struct MasterEvent
  {
    enum class Kind
      {
        JobSubmit,       // Orchestrator: new job needs planning
      WorkerExhausted, // Orchestrator: no Worker can handle a step
      AdviserFailed,   // Orchestrator: Adviser thread failed
      ScheduledTask,   // PeriodicExecutor: periodic review / follow-up
    };

    Kind kind;
    std::string payload_json;
    std::string job_id;
  };

  // ADR-020 — Gateway outbound message (response or notification to client)
  struct GatewayOutbound
  {
    std::string identity; // ZMQ identity frame of the target client
                          // empty = broadcast to all connected clients
    std::string message;  // raw JSON-RPC 2.0 payload
  };

  // ADR-020 — Gateway event queue entries
  struct GatewayEvent
  {
    enum class Kind
    {
      Outbound, // push message to client(s)
    };

    Kind kind;
    GatewayOutbound outbound;
  };

  // ADR-023 — Task target classification
  enum class TaskTarget : uint8_t
  {
    Gateway,
    Orchestrator,
    Master
  };

  inline std::string to_string (TaskTarget t)
  {
    switch (t)
    {
    case TaskTarget::Gateway:
      return "gateway";
    case TaskTarget::Orchestrator:
      return "orchestrator";
    case TaskTarget::Master:
      return "master";
    }
    return "";
  }

  // ADR-023 — timer_tasks row representation (in-memory only)
  struct TimerTask
  {
    std::string id;
    int64_t interval_s = 0;
    int64_t next_fire = 0;
    TaskTarget target;
    std::string payload_json;
    bool enabled = true;
    int64_t created_at = 0;
  };

  // ADR-023 — PeriodicExecutor control messages (register / cancel tasks)
  struct PeriodicControl
  {
    enum class Kind
    {
      Register,
      Cancel
    };

    struct Task
    {
      std::string id;
      int64_t interval_s = 0; // 0 = one-shot
      int64_t next_fire = 0;  // Unix seconds
      TaskTarget target;      // where to dispatch
      std::string payload_json;
    };

    Kind kind;
    Task task;             // used for Register
    std::string cancel_id; // used for Cancel
  };

  enum class StepStatus : int
  {
    pending = 0,
    running = 1,
    done = 2,
    failed = 3,
  };

  // ADR-022 — In-memory execution state per step (used by DB layer)
  struct StepState
  {
    PipelinePlanStep step;
    StepStatus status = StepStatus::pending;
    std::string result_json; // filled on completion
    int worker_attempt = 0;
  };

  // ADR-028 Credential types ------------------------------------------------

  struct CredentialRow
  {
    std::string id;
    std::string user_id;
    std::string provider;
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> nonce;
    std::optional<std::vector<uint8_t>> refresh_ciphertext;
    std::optional<std::vector<uint8_t>> refresh_nonce;
    std::optional<int64_t> expires_at;
    std::optional<int64_t> refresh_expires_at;
    int64_t created_at = 0;
    int64_t updated_at = 0;
  };

  struct GrantRow
  {
    std::string id;
    std::string worker_id;
    std::string provider;
    std::optional<std::string> suite_id;
    int64_t granted_at = 0;
    std::string granted_by;
  };

  struct CredentialAuditRow
  {
    std::string id;
    std::string credential_id;
    std::string user_id;
    std::string worker_id;
    std::string job_id;
    std::string step_id;
    std::string run_id;
    std::string
      action; // injected|denied|refreshed|submitted|revoked|refresh_failed
    std::optional<std::string> reason;
    int64_t timestamp = 0;
  };

  // ADR-030 Suite types
  struct SuitePurchase
  {
    std::string suite_id;
    std::string version;
    std::string subscription_key;
    int64_t purchased_at = 0;
    std::optional<int64_t> expires_at;
  };

  struct SuiteStatus
  {
    std::string suite_id;
    std::string version;
    bool available = true;
    int64_t checked_at = 0;
  };

  namespace suite_error
  {
    inline constexpr int suite_unavailable = -32040;
    inline constexpr int capability_unavailable = -32041;
  } // namespace suite_error

} // namespace agentos
