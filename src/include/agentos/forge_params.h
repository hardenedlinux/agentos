#pragma once
#include <rapidjson/document.h>
#include <string>

namespace agentos::cli {

rapidjson::Document build_forge_list_params(const std::string& phase);
rapidjson::Document build_forge_status_params(const std::string& forge_id);

} // namespace agentos::cli
