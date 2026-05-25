#include "agentos/config.h"
#include "agentos/home_init.h"
#include <toml.hpp>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>

namespace agentos {

std::optional<Config> load_config(std::string_view path, std::string& error) {
    Config cfg;

    // Determine config file path
    std::filesystem::path config_path;
    if (!path.empty()) {
        config_path = path;
    } else {
        config_path = agentos_home() / "config.toml";
    }

    // Open file
    std::ifstream file(config_path);
    if (!file.is_open()) {
        // File not found is not an error; use defaults
        return cfg;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    try {
        toml::table tbl = toml::parse(content);

        // [llm]
        if (auto* llm_tbl = tbl["llm"].as_table()) {
            if (auto* val = llm_tbl->at_path("base_url").as_string()) {
                cfg.llm.base_url = val->get();
            }
            if (auto* val = llm_tbl->at_path("model").as_string()) {
                cfg.llm.model = val->get();
            }
            if (auto* val = llm_tbl->at_path("max_tokens").as_integer()) {
                cfg.llm.max_tokens = static_cast<int>(val->get());
            }
            if (auto* val = llm_tbl->at_path("timeout_s").as_integer()) {
                cfg.llm.timeout_s = static_cast<int>(val->get());
            }
        }

        // [forge]
        if (auto* forge_tbl = tbl["forge"].as_table()) {
            if (auto* val = forge_tbl->at_path("max_attempts").as_integer()) {
                cfg.forge.max_attempts = static_cast<int>(val->get());
            }
            if (auto* val = forge_tbl->at_path("promotion_threshold").as_integer()) {
                cfg.forge.promotion_threshold = static_cast<int>(val->get());
            }
        }

        // [sandbox]
        if (auto* sandbox_tbl = tbl["sandbox"].as_table()) {
            if (auto* val = sandbox_tbl->at_path("tier1_memory_mb").as_integer()) {
                cfg.sandbox.memory_mb = static_cast<int>(val->get());
            }
            if (auto* val = sandbox_tbl->at_path("tier1_cpu_weight").as_integer()) {
                cfg.sandbox.cpu_weight = static_cast<int>(val->get());
            }
            if (auto* val = sandbox_tbl->at_path("tier1_pid_limit").as_integer()) {
                cfg.sandbox.pid_limit = static_cast<int>(val->get());
            }
        }

        // [database]
        if (auto* db_tbl = tbl["database"].as_table()) {
            if (auto* val = db_tbl->at_path("path").as_string()) {
                cfg.database.path = val->get();
            }
        }

        // [logging]
        if (auto* log_tbl = tbl["logging"].as_table()) {
            if (auto* val = log_tbl->at_path("level").as_string()) {
                cfg.logging.level = val->get();
            }
        }

    } catch (const toml::parse_error& e) {
        error = e.what();
        return std::nullopt;
    }

    return cfg;
}

bool read_env_api_key(Config& cfg) {
    const char* val = std::getenv("AGENTOS_LLM_API_KEY");
    if (val) {
        cfg.llm.api_key = val;
        // Clear env var
        unsetenv("AGENTOS_LLM_API_KEY");
        return true;
    }
    return false;
}

} // namespace agentos
