#pragma once
/**
 * agentos/orchestrator.h
 *
 * Orchestrator — internal execution arm of the Master daemon (ADR-002,
 * ADR-009).
 *
 * Responsibilities (Enforce Layer):
 *   - Drive the job state machine (planning → executing → done/failed)
 *   - Persist job and task state to Database
 *   - Spawn Adviser processes and collect Plans (request_plan)
 *   - Verify and execute Plans via Verifier and Scheduler (execute)
 *   - Resume in-flight jobs on daemon restart (ADR-005)
 *
 * The Orchestrator does not call LLMs directly and does not make
 * policy decisions; those belong to the Master's Mind Layer.
 */
#include "agentos/database/database.h"
#include "agentos/dispatcher.h"
#include "agentos/registry.h"
#include "agentos/scheduler.h"
#include "agentos/types.h"
#include "agentos/verifier.h"
#include <optional>
#include <string>
#include <vector>

namespace agentos
{

  class Orchestrator
  {
  public:
    Orchestrator (Registry &registry, Verifier &verifier, Scheduler &scheduler,
                  Dispatcher &dispatcher, Database &db);

    ~Orchestrator () = default;

    // Spawn the named Adviser and collect a Plan for the task.
    // Called by Master after adviser selection (Mind Layer).
    std::optional<Plan> request_plan (const std::string &adviser_id,
                                      const Task &task);

    // Verify and execute a Plan that the Master has already approved.
    // Persists job state throughout; returns the final TaskResult.
    TaskResult execute (const Task &task, const Plan &plan);

    // Resume any in-flight jobs from a previous daemon run (ADR-005).
    // Called once at startup before the main loop begins.
    void resume_in_flight ();

  private:
    // Job persistence helpers
    void store_job (const Task &task);
    void update_job_phase (const TaskId &id, const std::string &phase);
    void update_job_plan (const TaskId &id, const std::string &plan_json);
    void store_task (const TaskId &job_id, const PlanStep &step);

    // Serialise a Plan to JSON for DB storage.
    std::string serialize_plan (const Plan &plan) const;

    // Parse an Adviser's JSON response into a Plan struct.
    std::optional<Plan> parse_plan (const TaskId &task_id,
                                    const std::string &plan_json) const;

    // Build the available-commands context sent to the Adviser.
    std::string build_command_context () const;

    Registry &registry_;
    Verifier &verifier_;
    Scheduler &scheduler_;
    Dispatcher &dispatcher_;
    Database &db_;
  };

} // namespace agentos
