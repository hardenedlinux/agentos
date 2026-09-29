#pragma once

#include <cstdint>
#include <string>

namespace agentos
{

  enum class ForgeStatus : int
  {
    drafting = 0,
    reviewing = 1,
    promoted = 2,
    rejected = 3,
    human_review = 4,
  };

  constexpr bool is_terminal (ForgeStatus s) noexcept
  {
    return s == ForgeStatus::promoted || s == ForgeStatus::rejected
           || s == ForgeStatus::human_review;
  }

  struct ForgePipelineJob
  {
    std::string id;
    std::string task_id;
    // ADR-040: user whose job triggered this Forge run. Used for every LLM
    // request in the run (incl. after restart); records who caused the
    // work, not who owns the resulting Worker (Workers are shared).
    std::string user_id;
    ForgeStatus status = ForgeStatus::drafting;
    std::string requirement_json;
    std::string writer_output_json;
    std::string reviewer_verdict_json;
    std::string feedback;
    int attempt = 0;
    int max_attempts = 3;
    std::string last_code_path;
    int64_t created_at = 0;
    int64_t updated_at = 0;
  };

} // namespace agentos
