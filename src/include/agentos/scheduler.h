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

#include "agentos/database/database.h" // ADR-019: Database for forge worker persistence
#include "agentos/dispatcher.h"
#include "agentos/types.h"
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
                        const SchedulerConfig &config, Database &db);

    // Execute a validated plan. Blocks until all steps complete or fail.
    // Returns per-step results on success; TaskResult.success=false on any
    // failure.
    TaskResult run (const Plan &plan);

    // Validate capability declaration for a step (ADR-006 Layer 2)
    std::string validate_step_capabilities (const PlanStep &step,
                                            const std::string &worker_id);

    // ADR-019: provide a database reference for storing forge‑job results
    static void set_database (Database &db);

  private:
    const Registry &registry_;
    Dispatcher &dispatcher_;
    SchedulerConfig config_;
    Database &db_;

    std::string
    interpolate_args (const std::unordered_map<std::string, std::string> &args,
                      const StepResultMap &results) const;
  };

} // namespace agentos
