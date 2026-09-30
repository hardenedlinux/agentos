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

#include <rapidjson/document.h>

#include <deque>
#include <functional>
#include <optional>
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
    // Set at dispatch: the Worker or Adviser that runs this step, and
    // whether it is a declared reviewer (ADR-031 §14.2, agents.is_reviewer).
    std::string agent_id;
    bool reviewer = false;
  };

  struct ActiveJob
  {
    std::string job_id;
    std::string type;                     // oneshot | scheduled | loop
    std::string goal;                     // original user goal, injected into every task
    std::string user_id;                  // ADR-038: job owner for continuation access
    std::deque<ActiveStep> pending_steps; // steps not yet dispatched
    std::string current_run_id;           // run_id of the step in flight
    std::string last_step_result;         // result_json of the last completed step;
                                          // injected as $prev_result into the next step
    int current_iteration = 0;            // loop jobs
    int current_repairs = 0;             // loop jobs
    // ADR-012 (amended) + ADR-031 §12: this job's Digest Pass
    // classification — "artifact" | "result". Loaded from
    // db_.load_job()->deliverable_kind at plan_ready time; defaults to
    // "result" so a job predating this field (or any load_job() miss)
    // behaves exactly as before this change.
    std::string deliverable_kind = "result";
    // ADR-033 Step 0S / ADR-039 §D2: submitted with strict_ability_name.
    // A Registry miss fails the job instead of reaching Forge (ADR-031 §5)
    // and deliverable_kind is always "result" (ADR-031 §12).
    bool strict_ability = false;
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
    // ADR-040: owning job's user_id (ActiveJob, else jobs row).
    std::string owning_user_id (const std::string &job_id);
    // ADR-029: true iff job_id exists and is owned by user_id (exact match;
    // "0" is a real user). Used so a foreign job reads as not found.
    bool job_owned_by (const std::string &job_id, const std::string &user_id);

    // Master has made a decision (e.g. TriggerForge, JobFailed).
    void handle_master_decision (const OrchestratorEvent &ev);

    // PeriodicExecutor timer fired (e.g. scheduled job).
    void handle_timer_fired (const OrchestratorEvent &ev);

    // ---------------------------------------------------------------------------
    // Authentication (ADR-022)
    // ---------------------------------------------------------------------------

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

    // Builds the same JSON object cmd_job_status returns as its
    // `result` -- job_id, phase, goal, created_at, updated_at, error,
    // adviser_id, deliverable_kind, steps[] (each with tokens_prompt/
    // tokens_completion, forge_status when applicable), result_json,
    // bridge_hint. Shared by cmd_job_status and every notify() call
    // site that fires job.phase_changed/job.step_changed, so a
    // notification and a job.status reply for the same job at the
    // same moment are byte-identical in shape. Returns "" if job_id
    // doesn't exist.
    std::string build_job_status_json (const std::string &job_id);

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

    void cmd_adviser_register (const std::string &params_json,
                               const std::string &identity,
                               const std::string &request_id);

    void cmd_adviser_revoke (const std::string &params_json,
                             const std::string &identity,
                             const std::string &request_id);

    void cmd_forge_list (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);

    void cmd_forge_status (const std::string &params_json,
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

    void cmd_worker_enable (const std::string &params_json,
                            const std::string &identity,
                            const std::string &request_id);
    void cmd_worker_disable (const std::string &params_json,
                             const std::string &identity,
                             const std::string &request_id);
    void cmd_worker_revoke (const std::string &params_json,
                            const std::string &identity,
                            const std::string &request_id);

    // --- ADR-030: suite.* methods (local package install, no Marketplace
    //     download this round — "install" registers a locally-unpacked
    //     Suite directory's bundled Workers/Advisers) ---
    void cmd_suite_list (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);
    void cmd_suite_show (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);
    void cmd_suite_install (const std::string &params_json,
                            const std::string &identity,
                            const std::string &request_id);
    void cmd_suite_remove (const std::string &params_json,
                           const std::string &identity,
                           const std::string &request_id);

    // --- asset.register: local-path content-addressed registration ---
    void cmd_asset_register (const std::string &params_json,
                             const std::string &identity,
                             const std::string &request_id);
    void cmd_asset_show (const std::string &params_json,
                         const std::string &identity,
                         const std::string &request_id);
    void cmd_asset_list (const std::string &params_json,
                        const std::string &identity,
                        const std::string &request_id);
    void cmd_asset_revoke (const std::string &params_json,
                           const std::string &identity,
                           const std::string &request_id);
    // --- asset.revoke_by_user: whole-account hard erasure (GDPR
    // right-to-be-forgotten / inactivity auto-cleanup) — never used for
    // ordinary single-asset revoke, see cmd_asset_revoke above ---
    void cmd_asset_revoke_by_user (const std::string &params_json,
                                   const std::string &identity,
                                   const std::string &request_id);
    void cmd_asset_extract (const std::string &params_json,
                            const std::string &identity,
                            const std::string &request_id);

    // --- ADR user.facts
    void cmd_user_facts_record (const std::string &params_json,
                                const std::string &identity,
                                const std::string &request_id);
    void cmd_user_facts_get (const std::string &params_json,
                             const std::string &identity,
                             const std::string &request_id);

    // --- ADR subject.* methods
    void cmd_subject_register (const std::string &params_json,
                               const std::string &identity,
                               const std::string &request_id);
    void cmd_subject_units_populate (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id);
    void cmd_subject_units_next (const std::string &params_json,
                                 const std::string &identity,
                                 const std::string &request_id);
    void cmd_subject_units_complete (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id);
    void cmd_subject_units_progress (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id);
    void cmd_subject_memory_upsert (const std::string &params_json,
                                    const std::string &identity,
                                    const std::string &request_id);
    void cmd_subject_memory_query (const std::string &params_json,
                                   const std::string &identity,
                                   const std::string &request_id);
    // ADR-035 write-provenance addition.
    void cmd_subject_memory_write_policy_upsert (const std::string &params_json,
                                                 const std::string &identity,
                                                 const std::string &request_id);

    // ---------------------------------------------------------------------------
    // Shared registration helpers (ADR-031 §1/§2, ADR-018 Skill Package
    // Format) — used by cmd_worker_register/cmd_adviser_register directly,
    // and by cmd_suite_install for each bundled component. Returns true and
    // sets out_id on success; returns false and sets out_error otherwise.
    // Neither replies to the client — callers own that.
    // ---------------------------------------------------------------------------
    // ADR-030 all-or-nothing Suite install: how to undo one package
    // directory swap. `previous` is where the replaced package was parked
    // (empty if dest did not exist before).
    struct PackageSwap
    {
      std::filesystem::path dest;
      std::filesystem::path previous;
    };
    bool register_worker_package (const std::filesystem::path &src_dir,
                                  std::string &out_worker_id,
                                  std::string &out_error,
                                  PackageSwap *swap = nullptr);
    bool register_adviser_package (const std::filesystem::path &src_dir,
                                   std::string &out_adviser_id,
                                   std::string &out_error,
                                   PackageSwap *swap = nullptr);
    // Undo package swaps (reverse order) after a failed Suite install.
    static void undo_package_swaps (const std::vector<PackageSwap> &swaps);

    // ---------------------------------------------------------------------------
    // Pipeline execution
    // ---------------------------------------------------------------------------

    // Dispatch the next pending step of a job.
    // Looks up the Worker for the step's command, calls
    // dispatcher_.fork_exec(), records worker_runs in DB.
    void dispatch_next_step (ActiveJob &job);

    // ADR-031 §12: complete a Worker-target step whose job has
    // deliverable_kind == "artifact" WITHOUT actually executing the
    // Worker — writes a {"forge_generated":true,"agent_id":...} marker
    // result plus the reserved generated_code bridge_hint pointing at
    // the Worker's stable source location, then advances the pipeline
    // exactly as a normal step completion would (dispatch_next_step
    // handles both "more steps remain" and "this was the last step").
    // Used from two call sites that must behave identically regardless
    // of which one fires: forge_complete (the Worker was JUST promoted)
    // and dispatch_next_step (an already-registered Worker was found for
    // this step) — deliverable_kind's effect must not depend on whether
    // the capability happened to be generated by this job or reused from
    // a prior one.
    //
    // Returns false (and does NOT touch job.pending_steps/DB/notify) if
    // agent_id has no worker_impl.py at the expected Forge-promoted
    // location — the only reliable signal available that this Worker
    // wasn't actually Forge-generated (Worker.provenance in
    // protocol_types.h is unpopulated dead data; the agents table has no
    // such column). A hand-authored/Suite-bundled Worker reused by an
    // "artifact" job (e.g. a translation pipeline's internal reassemble
    // step) has no sensible "here's the source" interpretation — the
    // caller must fall through to normal execution in that case, not
    // silently do nothing.
    bool complete_step_as_generated_code (ActiveJob &job,
                                          const std::string &agent_id);

    // ADR-031 §9: dispatch a target_type:"adviser" step — spawns a detached
    // single-shot LLM thread (same shape as the Planning/domain-Adviser
    // spawn, ADR-018) but treats the response as a step result, not a Plan.
    // Enqueues AdviserDone/AdviserFailed on completion, same as
    // dispatch_next_step's Worker path enqueues WorkerDone/WorkerFailed via
    // Dispatcher's reap callback.
    void dispatch_adviser_step (ActiveJob &job);

    // ADR-031 §10 (+ asset attachment): resolve $prev_result,
    // $step:<id>.<field>, and $asset:<id> reference tokens in a step's
    // params/input map against persisted step results (db_.load_step_result)
    // and this job's attached assets (db_.load_job_assets). Returns a new
    // map with references replaced by the referenced content; non-reference
    // values pass through unchanged.
    // ADR-031 §13.4: if a reference cannot be resolved, `unresolved` names
    // it and the returned map is empty; no placeholder is substituted.
    std::unordered_map<std::string, std::string>
    resolve_step_references (const std::unordered_map<std::string, std::string> &params,
                             const std::string &prev_result,
                             const std::string &job_id,
                             std::string &unresolved);
    void fail_step_unresolved_reference (ActiveJob &job,
                                         const std::string &unresolved);
    // ADR-031 §13.1–§13.3: Plan-ingestion validation of step ids and
    // reference tokens. Empty string when valid.
    std::string validate_plan_references (const rapidjson::Value &steps,
                                          const std::string &job_id,
                                          const std::string &user_id);

    // Called on WorkerDone: read result, store in DB, advance pipeline.
    void on_step_complete (const std::string &job_id, const std::string &run_id,
                           int exit_code, const std::string &job_dir);

    // Called on WorkerFailed, or on an Adviser-target step's AdviserFailed:
    // ADR-031 §11 — retry the same step up to max_step_retries (local to
    // Orchestrator, never escalates to Master/Forge — Forge is only ever
    // triggered from dispatch_next_step's genuine Registry-miss path).
    // Exhausting retries fails the whole job.
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

    // data_json, if non-empty, must be a serialized JSON value; it is
    // attached as the JSON-RPC error's "data" member.
    void reply_error (const std::string &identity,
                      const std::string &request_id, int code,
                      const std::string &message,
                      const std::string &data_json = {});

    // ADR-039 §H2a: durably write the job's full job.status shape to the
    // outbox (agentos_home()/events/), advance jobs.notified_seq, then
    // broadcast params_json (small: job_id + the changed field) to all
    // connected clients. Returns false, without broadcasting, if the job is
    // unknown or the write failed; reemit_pending_outbox() recovers.
    bool notify_job (const std::string &method, const std::string &job_id,
                     const std::string &params_json);
    // Atomic, fsync'd write of one outbox file (tmp + rename + dir fsync).
    bool write_outbox_file (const std::string &message);
    // Re-emit every job whose state_seq is ahead of notified_seq.
    // Triggered on each heartbeat and once at startup.
    void reemit_pending_outbox ();

    // ADR-031 §14.4: a declared reviewer rejected the step's input. Persists
    // the step result, fails the step and the job with failure_kind
    // "review_rejected" and the job-level review object. Never retried.
    void reject_step_by_review (ActiveJob &job, const std::string &result_json,
                                const std::string &findings_json);

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

    // The authenticated caller's identity (access‑key id) for the current
    // request; set during handle_gateway_inbound and used for per‑user scoping.
    std::string current_caller_key_id_;
    // ADR-042: set while serving a job-channel request (serial dispatch, as
    // for current_caller_key_id_). Identifies the requesting run; the
    // user a request acts for comes from here, never from its params.
    std::optional<ChannelContext> channel_ctx_;
    void handle_channel_request (const OrchestratorEvent &ev);
    // The user a user-scoped request acts for: the job's user on the job
    // channel (a differing params.user_id is rejected with -32011); the
    // explicit, required params.user_id on the Gateway (admin only).
    // Replies with the error and returns nullopt on failure.
    std::optional<std::string>
    acting_user_id (const std::string &params_json,
                    const std::string &identity,
                    const std::string &request_id);

    // ADR-038: pending continuation_ids provided via job.submit before
    // the job is activated (plan_ready).
    std::unordered_map<std::string, std::string> pending_continuation_ids_;
  };

} // namespace agentos
