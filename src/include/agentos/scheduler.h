#pragma once
/**
 * agentos/scheduler.h
 *
 * Scheduler — executes a validated Plan against the registered executors.
 *
 * Responsibilities:
 *   - Topological sort of plan steps by depends_on
 *   - Run independent steps in parallel (via libuv thread pool)
 *   - Resolve {{step_id.field}} variable references between steps
 *   - Enforce per-step timeouts from CommandSchema.limits
 *   - Retry transient failures (configurable policy)
 *   - Collect all step results into a StepResultMap
 *
 * The Scheduler does not know about agents or LLMs.
 * It receives a validated Plan and a send function, and drives execution.
 */

#include "agentos/types.h"
#include <functional>
#include <unordered_map>

namespace agentos
{
  class Registry;

  // Results from each step, keyed by step id.
  // Values are raw JSON strings from the executor.
  using StepResultMap = std::unordered_map<std::string, std::string>;

  // Function the Scheduler calls to actually send a command to an executor.
  // Provided by the Orchestrator (which holds the Dispatcher).
  using ExecuteFn = std::function<std::string (const ClientId &executor_id,
                                               const std::string &command,
                                               const std::string &args_json)>;

  struct SchedulerConfig
  {
    int max_retries = 2;
    int retry_delay_ms = 500;
  };

  class Scheduler
  {
  public:
    explicit Scheduler (const Registry &registry, ExecuteFn execute_fn,
                        const SchedulerConfig &config = {});

    // Execute a validated plan. Blocks until all steps complete or fail.
    // Returns per-step results on success; TaskResult.success=false on any
    // failure.
    TaskResult run (const Plan &plan);

  private:
    const Registry &registry_;
    ExecuteFn execute_fn_;
    SchedulerConfig config_;

    std::string interpolate_args (const std::string &args_template_json,
                                  const StepResultMap &results) const;
  };

} // namespace agentos
