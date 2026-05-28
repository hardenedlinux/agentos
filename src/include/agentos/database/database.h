#pragma once

#include <sqlite3.h>
#include <string>
#include <memory>
#include <vector>
#include <optional>

#include "agentos/types.h" // for Task, PlanStep, TaskId, etc.

namespace agentos {

class Database {
public:
    explicit Database(const std::string& db_path);
    ~Database();

    // No copy or move
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open();
    void close();
    bool is_open() const;

    // Expose the underlying sqlite3 handle for use by Registry (static catalog)
    sqlite3* db_handle() const;

    // Store/update jobs
    void store_job(const Task& task);
    void update_job_phase(const TaskId& id, const std::string& phase);
    void update_job_plan(const TaskId& id, const std::string& plan_json);

    // Store a task (plan step)
    void store_task(const TaskId& job_id, const PlanStep& step);

    // Load plan JSON for a job
    std::string load_plan_json(const TaskId& job_id);

    // Resume in-flight jobs: returns list of (job_id, plan_json)
    struct InFlightJob {
        TaskId      job_id;
        std::string plan_json;
    };
    std::vector<InFlightJob> resume_in_flight();

    // ADR-016: Worker run management
    void insert_worker_run(const WorkerRun& run);
    void update_worker_run(const WorkerRun& run);
    std::vector<WorkerRun> get_active_worker_runs();
    std::vector<WorkerRun> get_all_worker_runs();
    virtual void mark_all_running_as_crashed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agentos


