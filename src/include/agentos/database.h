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

#include "agentos/forge_pipeline_job.h"
#include "agentos/protocol_types.h"
#include "agentos/types.h"
#include <expected>
#include <filesystem>
#include <optional>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace agentos
{

  namespace fs = std::filesystem;

  // forward declaration for ADR‑028
  struct CipherBlob;

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
      inline constexpr std::string_view oneshot = "oneshot";
      inline constexpr std::string_view scheduled = "scheduled";
      inline constexpr std::string_view loop = "loop";
    } // namespace job_type

    namespace step_status
    {
      inline constexpr std::string_view pending = "pending";
      inline constexpr std::string_view running = "running";
      inline constexpr std::string_view done = "done";
      inline constexpr std::string_view failed = "failed";
    } // namespace step_status

    namespace review_status
    {
      inline constexpr std::string_view pending = "pending";
      inline constexpr std::string_view approved = "approved";
      inline constexpr std::string_view rejected = "rejected";
    } // namespace review_status

    namespace review_type
    {
      inline constexpr std::string_view auto_ = "auto";
      inline constexpr std::string_view human = "human";
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
      int64_t approved_at = 0;
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
    void update_job_type (const std::string &job_id, const std::string &type);

    void update_job_plan (const TaskId &id, const std::string &plan_json);
    std::string load_plan_json (const TaskId &job_id);
    std::vector<InFlightJob> resume_in_flight ();

    // -- Job table (ADR‑025) --------------------------------------------------

    void insert_job (const Job &job);
    void update_job_phase (const std::string &id, std::string_view old_phase,
                           std::string_view new_phase);
    void update_job_error (const std::string &id, const std::string &error);
    std::optional<Job> load_job (const std::string &id);
    std::vector<Job> load_jobs (std::optional<std::string_view> type_filter,
                                std::optional<std::string_view> phase_filter,
                                int limit, int offset);
    int count_jobs (std::optional<std::string_view> type_filter,
                    std::optional<std::string_view> phase_filter);
    void increment_job_iteration (const std::string &id);
    void update_job_feedback (const std::string &id,
                              const std::string &feedback);
    void increment_job_repairs (const std::string &id);

    // -- Task table -----------------------------------------------------------
    // ADR‑022 – Pipeline step persistence and retrieval
    void store_pipeline_task (const TaskId &job_id,
                              const PipelinePlanStep &step, int step_order);
    std::string load_step_result (const std::string &step_id);
    void update_step_result (const std::string &step_id,
                             const std::string &result_json);

    // -- Step table (ADR‑025) -------------------------------------------------

    void insert_step (const Step &step);
    void update_step_status (const std::string &id, std::string_view new_status,
                             std::optional<std::string> error = std::nullopt);
    void complete_step (const std::string &id, const std::string &result_json);
    std::optional<Step> load_step (const std::string &id);
    std::vector<Step> load_steps_for_job (const std::string &job_id);
    std::optional<std::string>
    load_step_result_opt (const std::string &step_id);

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
    std::vector<ForgePipelineJob>
    load_forge_pipeline_jobs (std::optional<ForgeStatus> status_filter
                              = std::nullopt,
                              int limit = 100);

    // -- Agent / Capability tables --------------------------------------------

    void ensure_agent_tables ();
    std::vector<AgentRow> load_enabled_agents ();
    void insert_agent (const std::string &id, const std::string &role,
                       const std::string &binary_path,
                       const std::string &manifest);
    // Accumulate LLM token usage for a step (additive — safe to call
    // multiple times for Planning + Forge contributions).
    void update_step_tokens (const std::string &step_id,
                             int prompt_tokens, int completion_tokens);

    void insert_capability (const std::string &agent_id,
                            const std::string &method,
                            const std::string &description,
                            const std::string &input_schema);
    std::vector<CapabilityRow> load_capabilities ();

    // Toggle a worker's enabled flag (1=enabled, 0=disabled).
    // Silently no-ops if worker_id does not exist.
    void set_worker_enabled (const std::string &worker_id, bool enabled);

    // Soft-delete a worker: sets enabled = -1 (revoked).
    // Revoked workers are excluded from load_enabled_agents() and are never
    // dispatched. The row is retained for audit purposes.
    void revoke_worker (const std::string &worker_id);

    // Force-revoke a worker: marks all running worker_runs as failed (status=2)
    // then sets enabled=-1. Callers must have already sent SIGTERM/SIGKILL to
    // the active PIDs before calling this. Used when a worker is stuck and
    // cannot exit cleanly.
    void force_revoke_worker (const std::string &worker_id);

    // -- HumanReview table (original, preserved for ABI compatibility) --------

    void insert_human_review (const std::string &id, const std::string &reason,
                              const std::string &artifacts,
                              const std::string &forge_id);

    // -- HumanReview table (ADR‑025) ------------------------------------------

    void insert_human_review (const HumanReview &review);
    void update_review_status (const std::string &id, std::string_view status,
                               const std::string &decision);
    std::optional<HumanReview> load_human_review (const std::string &id);
    std::vector<HumanReview>
    load_human_reviews (std::optional<std::string_view> type_filter,
                        std::optional<std::string_view> status_filter);

    // --- Crash recovery helpers (ADR‑025) -----------------------------------

    std::vector<Job> load_active_jobs ();
    std::vector<Step>
    load_active_steps (const std::vector<std::string> &job_ids);

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

    void insert_timer_task (const TimerTask &t);  // INSERT OR IGNORE
    void persist_timer_task (const TimerTask &t); // INSERT OR REPLACE
    void upsert_timer_task_next_fire (const std::string &id, int64_t next_fire);
    void disable_timer_task (const std::string &id);
    bool timer_task_exists (const std::string &id);
    std::vector<TimerTask> load_enabled_timer_tasks ();

    // -- Credential storage (ADR-028) -----------------------------------------

    /// Store a credential. Returns the credential id on success.
    /// On insert, caller_id is used as the row id so that the HKDF info string
    /// used for encryption always matches the id stored in the DB.
    /// On update (row already exists), the existing id is preserved and
    /// returned.
    std::expected<std::string, Error>
    insert_credential (const std::string &caller_id, const std::string &user_id,
                       const std::string &provider,
                       const CipherBlob &token_blob,
                       const std::optional<CipherBlob> &refresh_blob,
                       std::optional<int64_t> expires_at);

    /// Load a credential by (user_id, provider).
    std::optional<CredentialRow> load_credential (const std::string &user_id,
                                                  const std::string &provider);

    /// Load all credentials for a given user.
    std::vector<CredentialRow>
    load_credentials_by_user (const std::string &user_id);

    /// Load all credentials (used for rekey).
    std::vector<CredentialRow> load_all_credentials ();

    /// Update the token ciphertext and (optional) expiry for a credential.
    bool update_credential_token (const std::string &id, const CipherBlob &blob,
                                  std::optional<int64_t> expires_at);

    /// Update both token and refresh ciphertexts (used for rekey).
    bool update_credential_full (const std::string &id,
                                 const CipherBlob &token_blob,
                                 const std::optional<CipherBlob> &refresh_blob,
                                 std::optional<int64_t> expires_at);

    /// Permanently remove a credential.
    bool revoke_credential (const std::string &user_id,
                            const std::string &provider);

    /// Return credentials whose expires_at < threshold_unix.
    std::vector<CredentialRow>
    load_expiring_credentials (int64_t threshold_unix);

    // -- Credential grants (ADR-028) -----------------------------------------

    /// Create a grant for a worker to use a provider. Returns grant id.
    std::expected<std::string, Error>
    insert_credential_grant (const std::string &worker_id,
                             const std::string &provider,
                             const std::string &granted_by);

    /// Load a grant by worker and provider.
    std::optional<GrantRow> load_credential_grant (const std::string &worker_id,
                                                   const std::string &provider);

    /// Revoke a grant (delete from DB).
    bool revoke_credential_grant (const std::string &grant_id);

    // -- Credential audit (ADR-028) -----------------------------------------

    /// Insert an audit row.
    void insert_credential_audit (const CredentialAuditRow &row);

    /// Load audit rows, optionally filtered by user/job/provider, limited.
    std::vector<CredentialAuditRow>
    load_credential_audit (const std::optional<std::string> &user_id,
                           const std::optional<std::string> &job_id,
                           const std::optional<std::string> &provider,
                           int limit);

    // ADR-030 Suite purchase / status / agent ref lookup

    void insert_suite_purchase (const SuitePurchase &p);
    std::optional<SuitePurchase>
    load_suite_purchase (const std::string &suite_id,
                         const std::string &version);
    void remove_suite_purchase (const std::string &suite_id,
                                const std::string &version);
    void remove_suite_purchase (const std::string &suite_id);

    void upsert_suite_status (const SuiteStatus &s);
    std::optional<SuiteStatus> load_suite_status (const std::string &suite_id);
    void update_suite_availability (const std::string &suite_id, bool available,
                                    int64_t checked_at);
    void remove_suite_status (const std::string &suite_id);
    std::vector<SuiteStatus> load_all_suite_status ();

    std::optional<std::string> resolve_agent_binary (const std::string &ref,
                                                     const std::string &version
                                                     = "");
    std::vector<Job>
    load_jobs_since (std::optional<int64_t> since_unix, int limit,
                     std::optional<std::string> user_id_filter = std::nullopt);

    /// Failure modes for with_transaction.
    enum class DbTxError
    {
      BeginFailed,       ///< BEGIN failed; no state changed.
      TransactionFailed, ///< fn() returned false; ROLLBACK succeeded.
      Exception,         ///< fn() threw; ROLLBACK succeeded.
      CommitFailed,      ///< COMMIT failed; data state unknown — do not retry
                         ///< blindly.
      RollbackFailed,    ///< ROLLBACK failed; connection state suspect.
    };

    /// Execute fn inside a BEGIN/COMMIT (or ROLLBACK on failure).
    /// Returns std::unexpected(DbTxError) on any failure so callers can
    /// distinguish COMMIT failures (unknown state) from ordinary fn failures.
    template <typename Fn>
    std::expected<void, DbTxError> with_transaction (Fn &&fn)
    {
      if (!exec_ddl ("BEGIN"))
        return std::unexpected (DbTxError::BeginFailed);

      bool ok = false;
      try
      {
        ok = fn ();
      }
      catch (...)
      {
        if (!exec_ddl ("ROLLBACK"))
        {
          spdlog::error ("[database] ROLLBACK failed after exception — "
                         "connection untrusted");
          return std::unexpected (DbTxError::RollbackFailed);
        }
        return std::unexpected (DbTxError::Exception);
      }

      if (!ok)
      {
        if (!exec_ddl ("ROLLBACK"))
        {
          spdlog::error ("[database] ROLLBACK failed after fn failure — "
                         "connection untrusted");
          return std::unexpected (DbTxError::RollbackFailed);
        }
        return std::unexpected (DbTxError::TransactionFailed);
      }

      if (!exec_ddl ("COMMIT"))
      {
        spdlog::error ("[database] COMMIT failed — data state unknown, do not "
                       "retry blindly");
        return std::unexpected (DbTxError::CommitFailed);
      }
      return {};
    }

  private:
    // -- Internal helpers -----------------------------------------------------

    [[nodiscard]] bool exec_ddl (const char *sql);

    // Seed built-in adviser rows into the agents table (INSERT OR IGNORE).
    // Called at the end of open() so advisers are always available without
    // manual DB intervention.
    void seed_builtin_advisers ();

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
