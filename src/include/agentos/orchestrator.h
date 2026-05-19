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
#include <string>
#include <vector>
#include <memory>

namespace agentos {

class Database; // forward declaration

class Orchestrator {
public:
    Orchestrator(Registry&    registry,
                 Verifier&    verifier,
                 Scheduler&   scheduler,
                 Dispatcher&  dispatcher,
                 const std::string& db_path = "");

    ~Orchestrator();

    // Submit a task for execution. Blocking — returns when complete.
    TaskResult submit(const Task& task);

private:
    Registry&   registry_;
    Verifier&   verifier_;
    Scheduler&  scheduler_;
    Dispatcher& dispatcher_;
    std::unique_ptr<Database> db_;
    std::string db_path_;

    // Database helpers
    void store_job(const Task& task);
    void update_job_phase(const TaskId& id, const std::string& phase);
    void update_job_plan(const TaskId& id, const std::string& plan_json);
    void store_task(const TaskId& job_id, const PlanStep& step);
    void update_task_result(const TaskId& task_id, const std::string& result, const std::string& status);
    std::vector<Task> load_in_flight_jobs();
    void resume_in_flight();
    std::string load_plan_json(const TaskId& job_id);
    std::string serialize_plan(const Plan& plan) const;

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
