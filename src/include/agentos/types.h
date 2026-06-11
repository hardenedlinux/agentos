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
  };

  struct RegisteredExecutor
  {
    ClientId id;
    std::string name;
    std::string version;
    std::string binary_path; // path to the worker binary (from static catalog)
    std::vector<CommandSchema> commands; // self-described at registration
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

  // Plan (produced by Adviser, validated by Verifier)

  struct PlanStep
  {
    std::string id;      // e.g. "step_1"
    std::string command; // e.g. "web.search"
    std::unordered_map<std::string, std::string>
    args;                              // key → value or "{{step_id.field}}"
    std::vector<std::string> depends_on; // other step ids
    std::optional<CapabilityDeclaration> capabilities; // ADR-006 Layer 2
  };

  struct Plan
  {
    TaskId task_id;
    std::vector<PlanStep> steps;
  };

  // Task

  struct Task
  {
    TaskId id;
    std::string goal;       // natural language goal from user
    std::string input_json; // structured input (may be empty)
  };

  struct TaskResult
  {
    TaskId task_id;
    bool success = false;
    std::string output_json;
    std::string error; // populated if success == false
  };

  // Verification result

  struct VerifyResult
  {
    bool ok = false;
    std::vector<std::string> errors; // one entry per violated constraint
  };

  // ADR-016: Worker run record
  enum class WorkerStatus : int
  {
    running   = 0,
    completed = 1,
    failed    = 2,
    crashed   = 3,
  };

  struct WorkerRun
  {
    std::string  run_id;    // UUID
    std::string  worker_id; // worker identifier
    int          pid        = 0;
    int64_t      started_at = 0;
    int64_t      ended_at   = 0;
    int          exit_code  = -1;
    WorkerStatus status     = WorkerStatus::running;
    std::string  layer_path; // ~/.agentos/layers/runs/<run-id>/
    std::string  log_path;   // ~/.agentos/logs/runs/<run-id>/output.log
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
    std::string key;            // plaintext for display
    std::string key_hash;       // SHA-256(key || salt)
    std::string key_salt;       // per-key random salt (16 bytes base64url)
    std::string description;
    std::string role;           // admin | operator | readonly
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
    std::string command;      // capability method name
    std::string description;  // natural language (Master‑generated)
    std::unordered_map<std::string, std::string> params;
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
        GatewayInbound,  // raw message from Gateway
        WorkerDone,      // Dispatcher reaper: Worker completed
        WorkerFailed,    // Dispatcher reaper: Worker failed
        AdviserDone,     // Adviser thread completed successfully
        AdviserFailed,   // Adviser thread exited with error
        MasterDecision,  // Master has reached a decision
        TimerFired,      // from PeriodicExecutor (scheduled task)
    };

    Kind        kind;
    std::string payload_json;
    std::string job_id;   // associated job (replaces TaskId where relevant)
  };

  // ADR-024 — Master event queue entries
  struct MasterEvent
  {
    enum class Kind
    {
        JobSubmit,        // Orchestrator: new job needs planning
        WorkerExhausted,  // Orchestrator: no Worker can handle a step
        AdviserFailed,    // Orchestrator: Adviser thread failed
        ScheduledTask,    // PeriodicExecutor: periodic review / follow-up
    };

    Kind        kind;
    std::string payload_json;
    std::string job_id;
  };

  // ADR-020 — Gateway outbound message (response or notification to client)
  struct GatewayOutbound
  {
    std::string identity;    // ZMQ identity frame of the target client
                             // empty = broadcast to all connected clients
    std::string message;     // raw JSON-RPC 2.0 payload
  };

  // ADR-020 — Gateway event queue entries
  struct GatewayEvent
  {
    enum class Kind
    {
        Outbound,   // push message to client(s)
    };

    Kind           kind;
    GatewayOutbound outbound;
  };

  // ADR-023 — PeriodicExecutor control messages (register / cancel tasks)
  struct PeriodicControl
  {
    enum class Kind { Register, Cancel };

    struct Task
    {
      std::string id;
      int64_t     interval_s  = 0;   // 0 = one-shot
      int64_t     next_fire   = 0;   // Unix seconds
      std::string target;            // "gateway" | "orchestrator" | "master"
      std::string payload_json;
    };

    Kind        kind;
    Task        task;          // used for Register
    std::string cancel_id;     // used for Cancel
  };

  enum class StepStatus : int
  {
    pending = 0,
    running = 1,
    done    = 2,
    failed  = 3,
  };

  // ADR-022 — In-memory execution state per step (used by DB layer)
  struct StepState
  {
    PipelinePlanStep step;
    StepStatus       status      = StepStatus::pending;
    std::string      result_json; // filled on completion
    int              worker_attempt = 0;
  };

} // namespace agentos
