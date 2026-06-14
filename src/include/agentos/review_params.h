#pragma once
#include <rapidjson/document.h>
#include <string>

namespace agentos::cli {

rapidjson::Document build_review_list_params(
    const std::string& status,
    const std::string& type);

rapidjson::Document build_review_id_params(const std::string& review_id);

rapidjson::Document build_review_reject_params(
    const std::string& review_id,
    const std::string& message);

} // namespace agentos::cli
