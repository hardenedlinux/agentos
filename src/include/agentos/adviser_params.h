#pragma once
#include <rapidjson/document.h>
#include <string>

namespace agentos::cli {

rapidjson::Document build_adviser_register_params(const std::string& path);
rapidjson::Document build_adviser_list_params();

} // namespace agentos::cli
