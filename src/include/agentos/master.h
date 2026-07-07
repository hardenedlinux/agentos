#pragma once
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

/**
 * agentos/master.h
 *
 * Master — Mind Layer Actor (ADR-002, ADR-009, ADR-024).
 *
 * Responsibilities:
 *   - Select Adviser for a job (LLM call, detached thread)
 *   - Review plan produced by Adviser (LLM call, detached thread)
 *   - Decide response to WorkerExhausted (trigger Forge or fail job)
 *   - Decide response to AdviserFailed (retry or fail job)
 *
 * All on_message() handlers return immediately.
 * LLM calls run in detached threads and enqueue results back to Master.
 * Master never touches Database or drives state machines.
 */

#include "agentos/actor.h"
#include "agentos/llm_client.h"
#include "agentos/registry.h"
#include "agentos/types.h"

#include <functional>
#include <string>

namespace agentos
{

// ---------------------------------------------------------------------------
// Extended MasterEvent kinds for internal LLM result flow
// ---------------------------------------------------------------------------
// NOTE: MasterEvent::Kind in types.h defines the external-facing kinds.
// Internal kinds are handled by casting payload_json — we reuse the same
// struct to avoid a separate internal event type.
//
// Internal payload convention (kind == MasterDecision repurposed):
//   {"_internal":"adviser_selected","job_id":"...","adviser_id":"..."}
//   {"_internal":"plan_reviewed","job_id":"...","approved":true,"reason":"..."}
//   {"_internal":"forge_decision","job_id":"...","trigger_forge":true,"command":"..."}

class Master : public Actor<MasterEvent>
{
public:
  using SendToOrchestrator = std::function<void(OrchestratorEvent)>;

  Master (LlmClient          &llm,
          Registry           &registry,
          SendToOrchestrator  send_to_orchestrator);

  ~Master () = default;

  Master (const Master &)            = delete;
  Master &operator= (const Master &) = delete;

  // Allow unit tests to invoke private select_adviser() and to inject a
  // fake LLM response function (llm_fn_, ADR-033 Step 2 disambiguation)
  // before any detached LLM thread is started. No public setter is
  // exposed for llm_fn_; this friend struct is the sole access point.
  friend struct MasterSelectInvoker;

private:
  // ---------------------------------------------------------------------------
  // Actor interface — all return immediately
  // ---------------------------------------------------------------------------
  void on_message (MasterEvent msg) override;

  // ---------------------------------------------------------------------------
  // External event handlers
  // ---------------------------------------------------------------------------

  // Received from Orchestrator: new job needs an Adviser selected.
  // Spawns detached thread to call LLM, enqueues AdviserSelected result.
  void handle_job_submit (MasterEvent msg);

  // Received from Orchestrator: no Worker for a step.
  // Spawns detached thread to decide Forge vs fail.
  void handle_worker_exhausted (MasterEvent msg);

  // Received from Orchestrator: Adviser thread failed.
  // Decides retry or fail.
  void handle_adviser_failed (MasterEvent msg);

  // Received from PeriodicExecutor: scheduled review or follow-up.
  void handle_scheduled_task (MasterEvent msg);

  // ---------------------------------------------------------------------------
  // Internal result handlers (enqueued by detached LLM threads)
  // ---------------------------------------------------------------------------

  // LLM has selected an Adviser — tell Orchestrator to spawn it.
  void handle_adviser_selected (const std::string &payload_json);

  // LLM has reviewed a plan — tell Orchestrator to proceed or fail.
  void handle_plan_reviewed (const std::string &payload_json);

  // LLM has decided on Forge — tell Orchestrator to trigger or fail.
  void handle_forge_decision (const std::string &payload_json);

  // ---------------------------------------------------------------------------
  // LLM helpers (called inside detached threads)
  // ---------------------------------------------------------------------------

  // Use LLM to select the best Adviser for a job goal.
  // Returns adviser_id or empty string on failure.
  std::string select_adviser (const std::string &job_id,
                              const std::string &goal);

  // Use LLM to review a plan.
  // Returns empty string on approval, rejection reason otherwise.
  std::string review_plan (const std::string &job_id,
                           const std::string &plan_json);

  // Use LLM to decide whether to trigger Forge for a missing Worker.
  // Returns true if Forge should be triggered.
  bool decide_forge (const std::string &job_id,
                     const std::string &command);

  // Build adviser list context for LLM prompt.
  std::string build_adviser_context () const;

  // ADR-033: disambiguate among multiple domain candidates (Step 2)
  std::string llm_disambiguate_adviser (
      const std::string &goal,
      const std::vector<RegisteredAdviser> &candidates) const;

  // Test seam – allow injection of a fake LLM response for disambiguation.
  // If set, llm_disambiguate_adviser uses this instead of llm_.complete().
  // Private, and reachable only through MasterSelectInvoker (see friend
  // declaration above) — matches the existing access pattern used for
  // invoking select_adviser() from tests. No public setter is exposed:
  // a friend struct can assign llm_fn_ directly, so a setter would only
  // widen access unnecessarily to any caller holding a Master&, including
  // production code, and would be an unsynchronized write reachable
  // while a detached LLM thread (see class comment above) may be
  // reading it.
  std::function<Result<LlmResponse>(const LlmRequest&)> llm_fn_;

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------
  LlmClient          &llm_;
  Registry           &registry_;
  SendToOrchestrator  send_to_orchestrator_;
};

} // namespace agentos
