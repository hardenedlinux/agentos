#pragma once
/**
 * agentos/orchestrator.h
 *
 * Orchestrator — internal job state machine within the Master daemon.
 *
 * Drives the full planning → execution → completion cycle:
 *   1. Find an adviser for the task domain
 *   2. Request a plan from the adviser
 *   3. Verify the plan (via Verifier)
 *   4. Execute the plan (via Scheduler)
 *   5. Return the result
 *
 * The Orchestrator is not a separate role; it is an internal subsystem
 * of the Master daemon (per ADR-002).
 */

#include "agentos/types.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"
#include "agentos/dispatcher.h"
#include <optional>

namespace agentos {

class Orchestrator {
public:
    Orchestrator(Registry&    registry,
                 Verifier&    verifier,
                 Scheduler&   scheduler,
                 Dispatcher&  dispatcher);

    // Submit a task for execution. Blocking — returns when complete.
    TaskResult submit(const Task& task);

private:
    Registry&   registry_;
    Verifier&   verifier_;
    Scheduler&  scheduler_;
    Dispatcher& dispatcher_;

    // Ask the chosen adviser to produce a Plan for this task.
    std::optional<Plan> request_plan(const RegisteredAdviser& adviser,
                                     const Task&            task);

    // Serialise all registered CommandSchemas into a JSON array string
    // to include in the planning request context.
    std::string build_command_context() const;

    // Parse the adviser's JSON response into a Plan struct
    std::optional<Plan> parse_plan(const TaskId&      task_id,
                                   const std::string& plan_json) const;
};

} // namespace agentos
