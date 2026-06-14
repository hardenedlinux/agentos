#pragma once
#include <rapidjson/document.h>
#include <string>

namespace agentos::cli {

rapidjson::Document build_worker_register_params(const std::string& path);
rapidjson::Document build_worker_list_params(const std::string& enabled_str);
rapidjson::Document build_worker_toggle_params(const std::string& worker_id);

} // namespace agentos::cli
