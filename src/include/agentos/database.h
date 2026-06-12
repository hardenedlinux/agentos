#pragma once

#include "agentos/forge/forge_pipeline_job.h"
#include "agentos/types.h"
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
  } // namespace db

  // ---------------------------------------------------------------------------
  // Database
  //
  // Owns the sqlite3* connection for the daemon lifetime.
  // open() once at startup; destructor closes.
  // All public methods preserve the original external interface exactly.
  // Internally, work is delegated to Repository objects.
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

    // Exposed for legacy code that needs the raw handle (prefer Repository
    // methods for all new code).
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

    // -- Job table ------------------------------------------------------------

    void store_job (const Task &task);
    void update_job_phase (const TaskId &id, const std::string &phase);
    void update_job_plan (const TaskId &id, const std::string &plan_json);
    std::string load_plan_json (const TaskId &job_id);
    std::vector<InFlightJob> resume_in_flight ();

    // -- Task table -----------------------------------------------------------

    void store_task (const TaskId &job_id, const PlanStep &step);

    // ADR-022 – Pipeline step persistence and retrieval
    void store_pipeline_task (const TaskId &job_id,
                              const PipelinePlanStep &step, int step_order);
    std::string load_step_result (const std::string &step_id);
    void update_step_result (const std::string &step_id,
                             const std::string &result_json);

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

    // -- HumanReview table ----------------------------------------------------

    void insert_human_review (const std::string &id, const std::string &reason,
                              const std::string &artifacts,
                              const std::string &forge_id);

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

  private:
    // -- Internal helpers -----------------------------------------------------

    // Executes a multi-statement DDL string. Logs and returns false on error.
    [[nodiscard]] bool exec_ddl (const char *sql);

    // Wraps a single write operation in BEGIN/COMMIT; rolls back on any
    // sqlite3_step failure. The callable receives the prepared stmt.
    // Usage: with_transaction([&]{ ... });
    // Returns false and logs if the transaction fails; never throws.
    template <typename Fn> bool with_transaction (Fn &&fn);

    // Prepares a statement and logs on failure.
    // Returns nullptr on failure — callers must check before use.
    sqlite3_stmt *prepare (const char *sql);

    // Binds an optional int64 — uses sqlite3_bind_null when empty.
    static void bind_optional_int64 (sqlite3_stmt *stmt, int col,
                                     const std::optional<int64_t> &val);

    // Binds an optional int — uses sqlite3_bind_null when empty.
    static void bind_optional_int (sqlite3_stmt *stmt, int col,
                                   const std::optional<int> &val);

    // Reads a nullable TEXT column — returns empty string if NULL.
    static std::string column_text_or_empty (sqlite3_stmt *stmt, int col);

    // Reads a nullable INTEGER column — returns nullopt if NULL.
    static std::optional<int64_t> column_int64_opt (sqlite3_stmt *stmt,
                                                    int col);
    static std::optional<int> column_int_opt (sqlite3_stmt *stmt, int col);

    // -- State ----------------------------------------------------------------

    sqlite3 *db_ = nullptr;
    std::string db_path_;
  };

} // namespace agentos
