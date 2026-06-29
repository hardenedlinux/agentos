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
/**
 * agentos/forge_coordinator.h
 *
 * ForgeCoordinator — internal sub-component of Orchestrator (ADR-022).
 *
 * Owns the Forge pipeline state machine (ADR-019):
 *   drafting → reviewing → promoted
 *                       └→ rejected (retry with feedback, or human_review)
 *
 * Runs in its own thread. Orchestrator posts ForgeRequests via
 * post(); results are delivered through the on_complete callback
 * supplied at construction, called on the ForgeCoordinator thread.
 *
 * Dependencies injected at construction; ForgeCoordinator owns none of them.
 */

#include "agentos/database.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/llm_proxy.h"
#include "agentos/registry.h"
#include "agentos/types.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace agentos::forge
{

// ---------------------------------------------------------------------------
// ForgeRequest — what Orchestrator hands to ForgeCoordinator
// ---------------------------------------------------------------------------
struct ForgeRequest
{
  std::string forge_job_id; // pre-created forge_pipeline_jobs row
  std::string task_id;      // job_id — used to resume the job on complete
  std::string step_id;      // pipeline step id — used to record token usage
  std::string requirement_json; // serialised capability requirement (ADR-019)
  std::string feedback;         // empty on first attempt; set on retry
  int         attempt     = 0;
  int         max_attempts = 3;
};

// ---------------------------------------------------------------------------
// ForgeResult — what ForgeCoordinator delivers back to Orchestrator
// ---------------------------------------------------------------------------
struct ForgeResult
{
  std::string forge_job_id;
  std::string task_id;

  enum class Outcome
  {
    promoted,     // worker registered; resume original step
    human_review, // escalated; original step stays pending
    failed        // unrecoverable error (e.g. DB failure)
  };
  Outcome outcome;

  std::string worker_id;   // populated when outcome == promoted
  std::string review_id;   // populated when outcome == human_review
  std::string error;       // populated when outcome == failed
};

// ---------------------------------------------------------------------------
// ForgeCoordinator
// ---------------------------------------------------------------------------
class ForgeCoordinator
{
public:
  using CompletionCallback = std::function<void(ForgeResult)>;

  // All references must outlive ForgeCoordinator.
  // on_complete is called on the ForgeCoordinator thread after each job
  // finishes (promoted, human_review, or failed).
  ForgeCoordinator(Database&          db,
                   LlmProxy&          llm,
                   Registry&          registry,
                   CompletionCallback on_complete);

  ~ForgeCoordinator();

  // Non-copyable, non-movable (owns a thread).
  ForgeCoordinator(const ForgeCoordinator&)            = delete;
  ForgeCoordinator& operator=(const ForgeCoordinator&) = delete;

  // Start the background thread. Call once at startup.
  void start();

  // Signal the background thread to stop and join. Safe to call from any thread.
  void stop();

  // Enqueue a forge request. Returns immediately.
  // Thread-safe; callable from Orchestrator thread.
  void post(ForgeRequest req);

  // Register the callback invoked when a forge job completes.
  // Must be called before start(). Thread-safe.
  void set_completion_callback(CompletionCallback cb)
  {
    on_complete_ = std::move(cb);
  }

private:
  // ---------------------------------------------------------------------------
  // Thread entry point
  // ---------------------------------------------------------------------------
  void run();
  void process(ForgeRequest req);

  // ---------------------------------------------------------------------------
  // State machine steps — called sequentially on the ForgeCoordinator thread
  // ---------------------------------------------------------------------------

  // Run one full attempt: call Code Writer, then Code Reviewer.
  // Returns true if the Reviewer accepted, false otherwise.
  // Populates job.writer_output_json, job.reviewer_verdict_json, job.feedback.
  bool run_attempt(ForgePipelineJob& job);

  // Call Code Writer Adviser and store result in job.writer_output_json.
  // Returns false on hard error (LLM failure, malformed JSON).
  // Populates tokens_prompt/tokens_completion with LLM usage.
  bool call_code_writer(ForgePipelineJob& job,
                        int& tokens_prompt, int& tokens_completion);

  // Call Code Reviewer Adviser and store result in job.reviewer_verdict_json.
  // Returns the verdict: true = accept, false = reject.
  // Populates job.feedback with the Reviewer's reason on rejection.
  // Populates tokens_prompt/tokens_completion with LLM usage.
  bool call_code_reviewer(ForgePipelineJob& job,
                          int& tokens_prompt, int& tokens_completion);

  // Enforce Layer pre-check (ADR-009 Layer B):
  // Validate capability declaration against ADR-006 policy before Reviewer runs.
  // Returns false (and sets job.feedback) if network:true or exec:true declared.
  bool enforce_capability_policy(ForgePipelineJob& job);

  // Write code file to ~/.agentos/<forge_job_id>/attempt_N.<ext>
  // Updates job.last_code_path.
  bool write_code_file(ForgePipelineJob& job, const std::string& code,
                       const std::string& language);

  // Register promoted worker: write binary, insert agents+capabilities rows.
  // Returns the new worker_id on success.
  std::optional<std::string> promote_worker(const ForgePipelineJob& job);

  // Insert a human_reviews row and return its id.
  std::string escalate_to_human(const ForgePipelineJob& job,
                                const std::string&      reason);

  // ---------------------------------------------------------------------------
  // DB persistence helpers — persist-before-act (ADR-022)
  // ---------------------------------------------------------------------------
  void persist(const ForgePipelineJob& job);

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------
  Database&          db_;
  LlmProxy&          llm_;
  Registry&          registry_;
  CompletionCallback on_complete_;

  std::queue<ForgeRequest> queue_;
  std::mutex               queue_mutex_;
  std::condition_variable  queue_cv_;
  bool                     stopping_ = false;

  std::thread thread_;
};

} // namespace agentos::forge
