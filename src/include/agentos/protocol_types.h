#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agentos
{

  // --- Schedule (embedded in Job when type=scheduled) ---

  struct Schedule
  {
    std::string timer_id;             // associated timer id
    int64_t interval_s = 0;           // execution interval in seconds
    std::optional<int64_t> starts_at; // first fire time; null means immediately
    std::optional<int64_t> last_run_at; // last execution start time
    std::optional<int64_t> next_run_at; // next scheduled execution time
  };

  // --- Loop (embedded in Job when type=loop) ---

  struct Loop
  {
    int max_iterations = 0;
    int current_iteration = 0;
    int max_repairs = 0;
    int current_repairs = 0;
    std::string reviewer_id;
    std::string acceptance_criteria;
    std::optional<std::string>
    last_feedback; // Reviewer feedback from last rejection
  };

  // --- Job (top-level entity) ---

  struct Job
  {
    std::string id;
    std::string type; // oneshot | scheduled | loop
    std::string goal; // original natural language goal
    std::vector<std::string> tags;

    std::string user_id = "0"; // ADR-029

    std::string
    phase; // planning | executing | repairing | done | failed | human_review
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::optional<std::string> error; // failure reason

    // Embedded entity for type=scheduled (std::nullopt otherwise)
    std::optional<Schedule> schedule;
    // Embedded entity for type=loop (std::nullopt otherwise)
    std::optional<Loop> loop;
  };

  // --- Step (pipeline execution unit) ---

  struct Step
  {
    std::string id;
    std::string job_id;
    int step_order = 0;
    std::string description;
    std::string status; // pending | running | done | failed
    std::optional<int64_t> started_at;
    std::optional<int64_t> completed_at;
    std::optional<std::string> error; // failure summary, if failed
  };

  // --- Worker (Executor) ---

  struct Worker
  {
    std::string id;
    std::vector<std::string> capabilities; // method names this Worker provides
    std::string tier;                      // tier0 | tier1
    std::string provenance;                // forge | manual
    bool enabled = true;
    int64_t registered_at = 0;
  };

  // --- Adviser ---

  struct Adviser
  {
    std::string id;
    std::string description;
    std::string skill_path; // path to skill.md
    std::string model;      // effective LLM model
    bool active = false;
  };

  // --- ForgeJob ---

  struct ForgeJob
  {
    std::string id;
    std::string requirement;
    std::string
      phase; // drafting | reviewing | promoted | rejected | human_review
    int attempt = 0;
    int max_attempts = 0;
    std::optional<std::string> last_feedback;
    int64_t created_at = 0;
    int64_t updated_at = 0;
  };

  // --- HumanReview ---

  struct HumanReview
  {
    std::string id;
    std::string type; // auto | human
    std::optional<std::string> forge_id;
    std::optional<std::string> job_id;
    std::string reason;
    std::string artifacts; // JSON summary of attempt records
    std::string status;    // pending | approved | rejected
    std::optional<std::string> decision;
    int64_t created_at = 0;
    std::optional<int64_t> reviewed_at;
  };

} // namespace agentos
