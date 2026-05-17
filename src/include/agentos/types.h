#pragma once
/**
 * agentos/types.h
 *
 * Core domain types shared across all subsystems.
 * No dependencies on other agentos headers.
 */

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos
{

  // Identity

  using ClientId = std::string; // assigned by core on registration
  using TaskId = std::string;   // UUID assigned per task.submit

  // Executor command schema
  // Self-described by the executor at registration time.
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
    // Advisers declare which domains they can plan for (e.g. "research",
    // "coding")
    std::vector<std::string> domains;
  };

  struct RegisteredExecutor
  {
    ClientId id;
    std::string name;
    std::string version;
    std::vector<CommandSchema> commands; // self-described at registration
  };

  // Plan (produced by Adviser, validated by Verifier)

  struct PlanStep
  {
    std::string id;      // e.g. "step_1"
    std::string command; // e.g. "web.search"
    std::unordered_map<std::string, std::string>
      args;                              // key → value or "{{step_id.field}}"
    std::vector<std::string> depends_on; // other step ids
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

} // namespace agentos
