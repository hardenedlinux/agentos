#pragma once
/**
 * agentos/master.h
 *
 * Master — the sole decision-maker and resource arbiter per ADR-002.
 *
 * Responsibilities:
 *   - Receive a task from the user (via Orchestrator)
 *   - Ask an Adviser for a plan
 *   - Verify the plan (via Verifier)
 *   - Execute the plan (via Scheduler)
 *   - Return the result
 *
 * The Master does not own I/O; it delegates to Dispatcher, Registry,
 * Verifier, and Scheduler.
 */

#include "agentos/types.h"
#include "agentos/dispatcher.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"

namespace agentos {

class Master {
public:
    Master(Dispatcher&  dispatcher,
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
