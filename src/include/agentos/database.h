/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "agentos/forge/forge_pipeline_job.h"
#include "agentos/types.h"
#include "agentos/protocol_types.h"
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace agentos
{

  namespace fs = std::filesystem;

  // String constants — avoids raw literals scattered across the codebase.
  // Use these wherever a status/phase string is written or compared.
  namespace db
  {
    namespace job_phase
    {
      inline constexpr std::string_view planning = "planning";
      inline constexpr std::string_view executing = "executing";
      inline constexpr std::string_view done = "done";
      inline constexpr std::string_view failed = "failed";
      inline constexpr std::string_view repairing = "repairing";
      inline constexpr std::string_view human_review = "human_review";
    } // namespace job_phase

    namespace worker_status
    {
      inline constexpr std::string_view running = "running";
      inline constexpr std::string_view completed = "completed";
      inline constexpr std::string_view failed = "failed";
      inline constexpr std::string_view crashed = "crashed";
    } // namespace worker_status

    namespace forge_status
    {
      inline constexpr std::string_view drafting = "drafting";
      inline constexpr std::string_view reviewing = "reviewing";
      inline constexpr std::string_view promoted = "promoted";
      inline constexpr std::string_view rejected = "rejected";
      inline constexpr std::string_view human_review = "human_review";
    } // namespace forge_status

    namespace job_type
    {
      inline constexpr std::string_view oneshot   = "oneshot";
      inline constexpr std::string_view scheduled = "scheduled";
      inline constexpr std::string_view loop      = "loop";
    } // namespace job_type

    namespace step_status
    {
      inline constexpr std::string_view pending = "pending";
      inline constexpr std::string_view running = "running";
      inline constexpr std::string_view done    = "done";
      inline constexpr std::string_view failed  = "failed";
    } // namespace step_status

    namespace review_status
    {
      inline constexpr std::string_view pending  = "pending";
      inline constexpr std::string_view approved = "approved";
      inline constexpr std::string_view rejected = "rejected";
    } // namespace review_status

    namespace review_type
    {
      inline constexpr std::string_view auto_  = "auto";
      inline constexpr std::string_view human  = "human";
    } // namespace review_type
  } // namespace db

  // ---------------------------------------------------------------------------
  // Database
  //
  // Owns the sqlite3* connection for the daemon lifetime.
  // open() once at startup; destructor closes.
  // All public methods preserve the original external interface exactly.
  //
  // Thread safety: sqlite3 is opened in serialized mode (SQLITE_THREADSAFE=1).
  // A 5-second busy timeout handles write contention under WAL.
  // Callers never see SQLITE_BUSY; they see a logged error and a no-op.
  // ---------------------------------------------------------------------------

  class Database
  {
  public:
    // -- Connection management ------------------------------------------------

    explicit Database (const std::string &db_path = "");
    ~Database ();

    Database (const Database &) = delete;
    Database &operator= (const Database &) = delete;
    Database (Database &&) = delete;
    Database &operator= (Database &&) = delete;

    // Opens the database and runs CREATE TABLE IF NOT EXISTS for all tables.
    // Returns false and logs on failure; daemon should abort if this fails.
    [[nodiscard]] bool open ();
    void close ();
    [[nodiscard]] bool is_open () const;

    // Exposed for test code that needs the raw handle.
    // Production code must not call this — use the typed methods below.
    sqlite3 *db_handle () const;

    // -- Nested types (preserved for ABI compatibility) -----------------------

    struct InFlightJob
    {
      TaskId job_id;
      std::string plan_json;
    };

    struct AgentRow
    {
      std::string id;
      std::string role;
      std::string binary_path;
      std::string manifest;
    };

    struct CapabilityRow
    {
      std::string agent_id;
      std::string method;
      std::string description;
      std::string input_schema; // raw JSON
    };

    // -- Job table (original, preserved for compatibility) -------------------

    void store_job (const Task &task);
    void update_job_phase (const TaskId &id, const std::string &phase);
    void update_job_plan (const TaskId &id, const std::string &plan_json);
    std::string load_plan_json (const TaskId &job_id);
    std::vector<InFlightJob> resume_in_flight ();

    // -- Job table (ADR‑025) --------------------------------------------------

    void              insert_job (const Job &job);
    void              update_job_phase (const std::string &id,
                                        std::string_view old_phase,
                                        std::string_view new_phase);
    void              update_job_error (const std::string &id,
                                        const std::string &error);
    std::optional<Job> load_job (const std::string &id);
    std::vector<Job>    load_jobs (std::optional<std::string_view> type_filter,
                                   std::optional<std::string_view> phase_filter,
                                   int limit, int offset);
    int                 count_jobs (std::optional<std::string_view> type_filter,
                                    std::optional<std::string_view> phase_filter);
    void              increment_job_iteration (const std::string &id);
    void              update_job_feedback (const std::string &id,
                                           const std::string &feedback);
    void              increment_job_repairs (const std::string &id);

    // -- Task table -----------------------------------------------------------
    // ADR‑022 – Pipeline step persistence and retrieval
    void store_pipeline_task (const TaskId &job_id,
                              const PipelinePlanStep &step, int step_order);
    std::string load_step_result (const std::string &step_id);
    void update_step_result (const std::string &step_id,
                             const std::string &result_json);

    // -- Step table (ADR‑025) -------------------------------------------------

    void                     insert_step (const Step &step);
    void                     update_step_status (const std::string &id,
                                                  std::string_view new_status,
                                                  std::optional<std::string> error = std::nullopt);
    void                     complete_step (const std::string &id,
                                            const std::string &result_json);
    std::optional<Step>      load_step (const std::string &id);
    std::vector<Step>        load_steps_for_job (const std::string &job_id);
    std::optional<std::string> load_step_result_opt (const std::string &step_id);

    // -- WorkerRun table ------------------------------------------------------

    void insert_worker_run (const WorkerRun &run);
    void update_worker_run (const WorkerRun &run);
    std::vector<WorkerRun> get_active_worker_runs ();
    std::vector<WorkerRun> get_all_worker_runs ();
    void mark_all_running_as_crashed ();

    // -- ForgePipelineJob table -----------------------------------------------

    void store_forge_pipeline_job (const ForgePipelineJob &job);
    void update_forge_pipeline_job (const ForgePipelineJob &job);
    void update_forge_pipeline_job_status (const std::string &forge_id,
                                           ForgeStatus status);
    std::optional<ForgePipelineJob>
    load_forge_pipeline_job (const std::string &forge_id);
    std::vector<ForgePipelineJob> load_in_flight_forge_pipeline_jobs ();

    // -- Agent / Capability tables --------------------------------------------

    void ensure_agent_tables ();
    std::vector<AgentRow> load_enabled_agents ();
    void insert_agent (const std::string &id, const std::string &role,
                       const std::string &binary_path,
                       const std::string &manifest);
    void insert_capability (const std::string &agent_id,
                            const std::string &method,
                            const std::string &description,
                            const std::string &input_schema);
    std::vector<CapabilityRow> load_capabilities ();

    // -- HumanReview table (original, preserved for ABI compatibility) --------

    void insert_human_review (const std::string &id, const std::string &reason,
                              const std::string &artifacts,
                              const std::string &forge_id);

    // -- HumanReview table (ADR‑025) ------------------------------------------

    void                     insert_human_review (const HumanReview &review);
    void                     update_review_status (const std::string &id,
                                                   std::string_view status,
                                                   const std::string &decision);
    std::optional<HumanReview> load_human_review (const std::string &id);
    std::vector<HumanReview>   load_human_reviews (
                                    std::optional<std::string_view> type_filter,
                                    std::optional<std::string_view> status_filter);

    // --- Crash recovery helpers (ADR‑025) -----------------------------------

    std::vector<Job>  load_active_jobs ();
    std::vector<Step> load_active_steps (const std::vector<std::string> &job_ids);

    // -- AccessKey table (ADR-020) --------------------------------------------

    struct AccessKey
    {
      std::string id;
      std::string key;      // plaintext
      std::string key_hash; // SHA-256(key || salt)
      std::string key_salt; // 16-byte random, base64url
      std::string description;
      std::string role; // "admin" | "operator" | "readonly"
      int64_t created_at = 0;
      std::optional<int64_t> expires_at;
      std::optional<int64_t> last_used_at;
      std::optional<int64_t> revoked_at;
      std::optional<std::string> revoked_reason;
    };

    void insert_access_key (const AccessKey &key);
    void revoke_access_key (const std::string &id, const std::string &reason);
    void touch_access_key (const std::string &id);
    std::vector<AccessKey> load_active_access_keys ();

    // -- timer_tasks table (ADR-023) ------------------------------------------

    void insert_timer_task (const TimerTask &t);       // INSERT OR IGNORE
    void persist_timer_task (const TimerTask &t);      // INSERT OR REPLACE
    void upsert_timer_task_next_fire (const std::string &id,
                                      int64_t next_fire);
    void disable_timer_task (const std::string &id);
    bool timer_task_exists (const std::string &id);
    std::vector<TimerTask> load_enabled_timer_tasks ();

  private:
    // -- Internal helpers -----------------------------------------------------

    [[nodiscard]] bool exec_ddl (const char *sql);

    template <typename Fn> bool with_transaction (Fn &&fn);

    sqlite3_stmt *prepare (const char *sql);

    static void bind_optional_int64 (sqlite3_stmt *stmt, int col,
                                     const std::optional<int64_t> &val);
    static void bind_optional_int (sqlite3_stmt *stmt, int col,
                                   const std::optional<int> &val);
    static void bind_optional_text (sqlite3_stmt *stmt, int col,
                                    const std::optional<std::string> &val);
    static std::string column_text_or_empty (sqlite3_stmt *stmt, int col);
    static std::optional<int64_t> column_int64_opt (sqlite3_stmt *stmt,
                                                    int col);
    static std::optional<int> column_int_opt (sqlite3_stmt *stmt, int col);

    // -- State ----------------------------------------------------------------

    sqlite3 *db_ = nullptr;
    std::string db_path_;
  };

} // namespace agentos
