#pragma once

#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

#include "agentos/forge_pipeline_job.h" // ADR-019 pipeline job
#include "agentos/types.h"              // for Task, PlanStep, TaskId, etc.

namespace agentos
{

  class Database
  {
  public:
    explicit Database (const std::string &db_path);
    ~Database ();

    // No copy or move
    Database (const Database &) = delete;
    Database &operator= (const Database &) = delete;

    bool open ();
    void close ();
    bool is_open () const;

    // Expose the underlying sqlite3 handle for use by Registry (static catalog)
    sqlite3 *db_handle () const;

    // Store/update jobs
    void store_job (const Task &task);
    void update_job_phase (const TaskId &id, const std::string &phase);
    void update_job_plan (const TaskId &id, const std::string &plan_json);

    // Store a task (plan step)
    void store_task (const TaskId &job_id, const PlanStep &step);

    // Load plan JSON for a job
    std::string load_plan_json (const TaskId &job_id);

    // Resume in-flight jobs: returns list of (job_id, plan_json)
    struct InFlightJob
    {
      TaskId job_id;
      std::string plan_json;
    };
    std::vector<InFlightJob> resume_in_flight ();

    // ADR-016: Worker run management
    void insert_worker_run (const WorkerRun &run);
    void update_worker_run (const WorkerRun &run);
    std::vector<WorkerRun> get_active_worker_runs ();
    std::vector<WorkerRun> get_all_worker_runs ();
    virtual void mark_all_running_as_crashed ();

    // ADR-019: Forge pipeline job CRUD
    void store_forge_pipeline_job (const ForgePipelineJob &job);
    void update_forge_pipeline_job (const ForgePipelineJob &job);
    std::optional<ForgePipelineJob>
    load_forge_pipeline_job (const std::string &forge_id);
    std::vector<ForgePipelineJob> load_in_flight_forge_pipeline_jobs ();

    void ensure_agent_tables ();
    std::vector<AgentRow> load_enabled_agents ();
    void insert_agent (const std::string &id, const std::string &role,
                       const std::string &binary_path,
                       const std::string &manifest);
    void insert_capability (const std::string &agent_id,
                            const std::string &method,
                            const std::string &description,
                            const std::string &input_schema);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

  // Registry catalog support
  struct AgentRow
  {
    std::string id;
    std::string role;
    std::string binary_path;
    std::string manifest;
  };

} // namespace agentos
