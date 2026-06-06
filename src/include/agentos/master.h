#pragma once
/**
 * agentos/master.h
 *
 * Master — the sole decision-maker and resource arbiter (ADR-002, ADR-009).
 *
 * Responsibilities (Mind Layer):
 *   - Receive user tasks
 *   - Use LLM to understand the task and select an appropriate Adviser
 *   - Spawn the Adviser and obtain a Plan
 *   - Use LLM to review the Plan for soundness
 *   - Delegate execution to Orchestrator
 *
 * The Master does not drive the job state machine or touch the database
 * directly; those are Orchestrator's responsibilities.
 */
#include "agentos/config.h"
#include "agentos/dispatcher.h"
#include "agentos/llm_client.h"
#include "agentos/orchestrator.h"
#include "agentos/registry.h"
#include "agentos/types.h"
#include <optional>
#include <string>

namespace agentos
{

  class Master
  {
  public:
    Master (LlmClient &llm, Orchestrator &orchestrator, Registry &registry);

    // Submit a task for execution. Blocking — returns when complete.
    TaskResult submit (const Task &task);

  private:
    // Mind Layer: use LLM to select the best Adviser for this task.
    // Returns the adviser id, or empty string if none found.
    std::string select_adviser (const Task &task);

    // Mind Layer: use LLM to review the Plan produced by the Adviser.
    // Returns empty string on approval, or a rejection reason.
    std::string review_plan (const Task &task, const Plan &plan);

    // Build the system prompt for adviser selection.
    std::string build_selection_prompt (const Task &task) const;

    // Build the system prompt for plan review.
    std::string build_review_prompt (const Task &task, const Plan &plan) const;

    LlmClient &llm_;
    Orchestrator &orchestrator_;
    Registry &registry_;
  };

} // namespace agentos
