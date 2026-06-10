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
  struct WorkerRun
  {
    std::string run_id;    // UUID
    std::string worker_id; // worker identifier
    int pid = 0;
    int64_t started_at = 0;
    int64_t ended_at = 0;
    int exit_code = -1;
    std::string status;     // running | completed | failed | crashed
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

} // namespace agentos
