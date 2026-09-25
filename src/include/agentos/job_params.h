#pragma once
#include <rapidjson/document.h>
#include <string>
#include <cstdint>

namespace agentos::cli {

rapidjson::Document build_job_submit_params(
    const std::string& goal,
    const std::string& type,
    const std::string& input_str,
    int64_t            interval_s,
    int64_t            starts_at,
    int                max_iterations,
    const std::string& reviewer_id,
    const std::string& acceptance_criteria);

rapidjson::Document build_job_status_params(const std::string& job_id);

rapidjson::Document build_job_list_params(
    const std::string& phase,
    const std::string& type_filter,
    int limit,
    int offset);

rapidjson::Document build_job_cancel_params(
    const std::string& job_id,
    bool               stop_schedule);

} // namespace agentos::cli
