#pragma once
/**
 * agentos/scheduler.h
 *
 * Scheduler — executes a validated Plan against the registered workers.
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
 * It receives a validated Plan and a Dispatcher, and drives execution.
 */

#include "agentos/types.h"
#include "agentos/dispatcher.h"
#include "database/database.h"
#include <functional>
#include <unordered_map>

namespace agentos
{
  class Registry;

  // Results from each step, keyed by step id.
  // Values are raw JSON strings from the worker.
  using StepResultMap = std::unordered_map<std::string, std::string>;

  struct SchedulerConfig
  {
    int max_retries = 2;
    int retry_delay_ms = 500;
  };

  class Scheduler
  {
  public:
    explicit Scheduler (const Registry &registry, Dispatcher &dispatcher,
                        const SchedulerConfig &config = {});

    // Execute a validated plan. Blocks until all steps complete or fail.
    // Returns per-step results on success; TaskResult.success=false on any
    // failure.
    TaskResult run (const Plan &plan);

    // Validate capability declaration for a step (ADR-006 Layer 2)
    bool validate_step_capabilities(const PlanStep &step);

  private:
    const Registry &registry_;
    Dispatcher &dispatcher_;
    SchedulerConfig config_;

    std::string interpolate_args (const std::string &args_template_json,
                                  const StepResultMap &results) const;
  };

} // namespace agentos
