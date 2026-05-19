#pragma once
/**
 * agentos/orchestrator.h
 *
 * Orchestrator — owns the full task lifecycle. The only subsystem that
 * coordinates all others. Entry point for every user-submitted task.
 *
 * Lifecycle for a task.submit:
 *
 *   1. Find an Adviser from Registry that handles the task's domain
 *   2. Build a planning context: task goal + all available command schemas
 *   3. Send task.plan to the Adviser via Dispatcher
 *   4. Receive the Plan (JSON) from the Adviser
 *   5. Parse the Plan
 *   6. Hand to Verifier — reject if invalid, surface errors to caller
 *   7. Hand to Scheduler — executes steps against executors
 *   8. Return TaskResult to caller
 *
 * The Orchestrator owns no I/O itself. It delegates:
 *   - Network I/O  → Dispatcher
 *   - State lookup → Registry
 *   - Validation   → Verifier
 *   - Execution    → Scheduler
 */

#include "agentos/types.h"
#include "agentos/dispatcher.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"
#include "agentos/master.h"

namespace agentos {

class Orchestrator {
public:
    Orchestrator(Dispatcher&  dispatcher,
                 Registry&    registry,
                 Verifier&    verifier,
                 Scheduler&   scheduler);

    // Submit a task for execution. Blocking — returns when complete.
    TaskResult submit(const Task& task);

private:
    Dispatcher&  dispatcher_;
    Registry&    registry_;
    Verifier&    verifier_;
    Scheduler&   scheduler_;

    // Ask the chosen adviser to produce a Plan for this task.
    // The planning context includes all registered command schemas so the
    // adviser knows exactly what executors are available and what they can do.
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
