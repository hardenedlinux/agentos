#pragma once
/**
 * agentos/orchestrator.h
 *
 * Orchestrator — operational core of the daemon (ADR-022).
 *
 * Responsibilities:
 *   - Authenticate inbound Gateway commands (access key verification)
 *   - Execute Plans as serial pipelines (ADR-022)
 *   - Manage Worker and Adviser lifecycles via Dispatcher / ForgeCoordinator
 *   - Handle pipeline step results and advance to next step
 *   - Report Worker exhaustion and Adviser failure to Master
 *   - Persist all state transitions to Database before acting
 *   - Crash recovery on startup (ADR-005)
 *
 * Orchestrator is an Actor<OrchestratorEvent>: it owns a MessageQueue
 * and a dedicated thread. All internal state is accessed only from
 * that thread — no locks needed for job/step state.
 *
 * Dispatcher::reap() callback and ForgeCoordinator completion callback
 * both enqueue OrchestratorEvents from other threads — that is safe
 * because MessageQueue is thread-safe.
 */

#include "agentos/actor.h"
#include "agentos/config.h"
#include "agentos/cred_vault.h"
#include "agentos/database.h"
#include "agentos/dispatcher.h"
#include "agentos/forge_coordinator.h"
#include "agentos/llm_proxy.h"
#include "agentos/registry.h"
#include "agentos/types.h"
#include "agentos/user_manager.h"

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

namespace agentos
{

  // ---------------------------------------------------------------------------
  // In-memory execution state per active job
  // ---------------------------------------------------------------------------

  struct ActiveStep
  {
    PipelinePlanStep step;
    std::string run_id;  // set when Worker is forked
    std::string job_dir; // set when Worker is forked
    int attempts = 0;
  };

  struct ActiveJob
  {
    std::string job_id;
    std::string type;                     // oneshot | scheduled | loop
    std::deque<ActiveStep> pending_steps; // steps not yet dispatched
    std::string current_run_id;           // run_id of the step in flight
    int current_iteration = 0;            // loop jobs
    int current_repairs = 0;              // loop jobs
  };

  // ---------------------------------------------------------------------------
  // Orchestrator
  // ---------------------------------------------------------------------------

  class Orchestrator : public Actor<OrchestratorEvent>
  {
  public:
    using SendToMaster = std::function<void (MasterEvent)>;
    using SendToGateway = std::function<void (GatewayEvent)>;

    Orchestrator (Database &db, LlmProxy &llm, Registry &registry,
                  Dispatcher &dispatcher, forge::ForgeCoordinator &forge,
                  const Config &config, CredVault &cred_vault,
                  SendToMaster send_to_master, SendToGateway send_to_gateway);

    ~Orchestrator () = default;

    Orchestrator (const Orchestrator &) = delete;
    Orchestrator &operator= (const Orchestrator &) = delete;

    // Called once at startup before start() — loads active keys cache,
    // reconstructs in-memory job index from DB, registers reap callback
    // with Dispatcher (ADR-022 crash recovery).
    void init ();

  private:
    // ---------------------------------------------------------------------------
    // Actor interface
    // ---------------------------------------------------------------------------
    void on_message (OrchestratorEvent msg) override;

    // ---------------------------------------------------------------------------
    // Message handlers (all called on the Orchestrator thread)
    // ---------------------------------------------------------------------------

    // Inbound command from Gateway: authenticate, route to handler.
    void handle_gateway_inbound (const OrchestratorEvent &ev);

    // Worker reaped by Dispatcher::reap() — read result, advance pipeline.
    void handle_worker_done (const OrchestratorEvent &ev);
    void handle_worker_failed (const OrchestratorEvent &ev);

    // Adviser thread completed or failed.
    void handle_adviser_done (const OrchestratorEvent &ev);
    void handle_adviser_failed (const OrchestratorEvent &ev);

    // Master has made a decision (e.g. TriggerForge, JobFailed).
    void handle_master_decision (const OrchestratorEvent &ev);

    // PeriodicExecutor timer fired (e.g. scheduled job).
    void handle_timer_fired (const OrchestratorEvent &ev);

    // ---------------------------------------------------------------------------
    // Authentication (ADR-022)
    // ---------------------------------------------------------------------------

    // Load all valid access keys from DB into active_keys_ cache.
    void load_active_keys ();

    // Verify key from inbound message. Returns the AccessKey on success,
    // or nullopt on failure (missing, invalid, expired, revoked).
    std::optional<Database::AccessKey>
    authenticate (const std::string &key_value) const;

    // Check role permission for a method (ADR-025 role matrix).
    bool is_permitted (const std::string &role,
                       const std::string &method) const;

    // ---------------------------------------------------------------------------
    // JSON-RPC command handlers (post-authentication)
    // ---------------------------------------------------------------------------

    void cmd_job_submit (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);

    void cmd_job_status (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);

    void cmd_job_list (const std::string &params_json,
                       const std::string &identity,
                       const std::string &request_id);

    void cmd_job_cancel (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);

    void cmd_review_approve (const std::string &params_json,
                             const std::string &identity,
                             const std::string &request_id);

    void cmd_review_reject (const std::string &params_json,
                            const std::string &identity,
                            const std::string &request_id);

    void cmd_worker_register (const std::string &params_json,
                              const std::string &identity,
                              const std::string &request_id);

    void cmd_worker_list (const std::string &params_json,
                          const std::string &identity,
                          const std::string &request_id);

    void cmd_adviser_list (const std::string &params_json,
                           const std::string &identity,
                           const std::string &request_id);

    // --- ADR-028: cred.* methods ---
    void cmd_cred_submit (const std::string &params_json,
                          const std::string &identity,
                          const std::string &request_id);
    void cmd_cred_revoke (const std::string &params_json,
                          const std::string &identity,
                          const std::string &request_id);
    void cmd_cred_grant (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);
    void cmd_cred_revoke_grant (const std::string &params_json,
                                const std::string &identity,
                                const std::string &request_id);
    void cmd_cred_list (const std::string &params_json,
                        const std::string &identity,
                        const std::string &request_id);
    void cmd_cred_audit (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);
    void cmd_vault_rekey (const std::string &params_json,
                          const std::string &identity,
                          const std::string &request_id);

    // --- ADR-029: user.* methods ---
    void cmd_user_register (const std::string &params_json,
                            const std::string &identity,
                            const std::string &request_id);
    void cmd_user_list (const std::string &params_json,
                        const std::string &identity,
                        const std::string &request_id);
    void cmd_user_enable (const std::string &params_json,
                          const std::string &identity,
                          const std::string &request_id);
    void cmd_user_disable (const std::string &params_json,
                           const std::string &identity,
                           const std::string &request_id);
    void cmd_user_profile (const std::string &params_json,
                           const std::string &identity,
                           const std::string &request_id);

    // ---------------------------------------------------------------------------
    // Pipeline execution
    // ---------------------------------------------------------------------------

    // Dispatch the next pending step of a job.
    // Looks up the Worker for the step's command, calls
    // dispatcher_.fork_exec(), records worker_runs in DB.
    void dispatch_next_step (ActiveJob &job);

    // Called on WorkerDone: read result, store in DB, advance pipeline.
    void on_step_complete (const std::string &job_id, const std::string &run_id,
                           int exit_code, const std::string &job_dir);

    // Called on WorkerFailed: try next Worker or escalate to Master.
    void on_step_failed (const std::string &job_id, const std::string &run_id,
                         int exit_code);

    // Mark a job done/failed, remove from active_jobs_, notify Gateway.
    void finish_job (const std::string &job_id, bool success,
                     const std::string &error = {});

    // ---------------------------------------------------------------------------
    // JSON-RPC response helpers
    // ---------------------------------------------------------------------------

    void reply_ok (const std::string &identity, const std::string &request_id,
                   const std::string &result_json);

    void reply_error (const std::string &identity,
                      const std::string &request_id, int code,
                      const std::string &message);

    // Broadcast a notification to all connected clients (no identity).
    void notify (const std::string &method, const std::string &params_json);

    // Generate a UUID for run_id / job_id.
    static std::string new_uuid ();

    // ---------------------------------------------------------------------------
    // Dependencies
    // ---------------------------------------------------------------------------
    Database &db_;
    LlmProxy &llm_;
    Registry &registry_;
    Dispatcher &dispatcher_;
    forge::ForgeCoordinator &forge_;
    const Config &config_;
    CredVault &cred_vault_;
    SendToMaster send_to_master_;
    SendToGateway send_to_gateway_;

    // ADR-029: user management (shared service, daemon lifetime)
    UserManager user_manager_;

    // ---------------------------------------------------------------------------
    // In-memory state (accessed only from the Orchestrator thread)
    // ---------------------------------------------------------------------------

    // Active jobs: job_id → ActiveJob
    std::unordered_map<std::string, ActiveJob> active_jobs_;

    // Access key cache: key_value → AccessKey (ADR-022)
    std::unordered_map<std::string, Database::AccessKey> active_keys_;
  };

} // namespace agentos
